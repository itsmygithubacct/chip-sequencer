/* chipseq_tools.c -- Standard MIDI File import + diff-stable C emission.
 *
 * This translation unit is OFFLINE tooling: a shipping game never links it. The
 * intended flow is "import once at build time, then emit compilable C song
 * literals the game compiles". It uses only stdio/stdlib/string -- no threads,
 * no audio device, and (like the core) no libm.
 *
 *   chipseq_midi_load   -- parse a format-0/1 SMF and quantize it onto chip
 *                          channels via a chipseq_midi_map, malloc'ing a song.
 *   chipseq_song_free   -- free a midi-loaded song (never a static literal).
 *   chipseq_song_write_c-- emit self-contained, byte-stable C literals.
 *
 */
#include "chip_sequencer.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* small utilities                                                           */
/* ------------------------------------------------------------------------- */

static void tools_set_err(char *err, size_t err_len, const char *fmt, ...) {
    if (!err || err_len == 0) return;
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(err, err_len, fmt, ap);
    va_end(ap);
}

/* Growable byte-sized generic array. `cap`/`len` count ELEMENTS. */
typedef struct {
    void  *data;
    size_t len;
    size_t cap;
    size_t elem;
} vec;

static bool vec_init(vec *v, size_t elem) {
    v->data = NULL;
    v->len = 0;
    v->cap = 0;
    v->elem = elem;
    return true;
}

static void vec_free(vec *v) {
    free(v->data);
    v->data = NULL;
    v->len = v->cap = 0;
}

static void *vec_push(vec *v) {
    if (v->len == v->cap) {
        size_t ncap = v->cap ? v->cap * 2 : 32;
        void *nd = realloc(v->data, ncap * v->elem);
        if (!nd) return NULL;
        v->data = nd;
        v->cap = ncap;
    }
    char *slot = (char *)v->data + v->len * v->elem;
    v->len++;
    memset(slot, 0, v->elem);
    return slot;
}

/* ------------------------------------------------------------------------- */
/* SMF parsing                                                               */
/* ------------------------------------------------------------------------- */

enum { MEV_NOTEON = 0, MEV_NOTEOFF, MEV_PROGRAM, MEV_TEMPO, MEV_BEND };

typedef struct {
    uint64_t tick;   /* absolute MIDI tick */
    uint32_t seq;    /* insertion order, for a stable total sort */
    uint32_t d3;     /* tempo usec/quarter, or 14-bit bend value */
    uint8_t  type;
    uint8_t  chan;   /* MIDI channel 0..15 */
    uint8_t  d1;     /* note or program */
    uint8_t  d2;     /* velocity */
} mev;

static uint32_t rd_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}
static uint16_t rd_be16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

/* Decode a variable-length quantity. Advances *pp; sets *ok. */
static uint32_t read_vlq(const uint8_t **pp, const uint8_t *end, bool *ok) {
    uint32_t v = 0;
    int i = 0;
    while (*pp < end) {
        uint8_t b = **pp;
        (*pp)++;
        v = (v << 7) | (uint32_t)(b & 0x7Fu);
        if ((b & 0x80u) == 0) { *ok = true; return v; }
        if (++i >= 4) break;
    }
    *ok = false;
    return 0;
}

/* Append every relevant event of one MTrk chunk [p,end) into `evs`, tagging
 * each with a monotonically increasing seq from *seqp for a stable sort. */
static bool parse_track(const uint8_t *p, const uint8_t *end, vec *evs,
                        uint32_t *seqp, bool import_bend,
                        char *err, size_t err_len) {
    uint64_t abstick = 0;
    uint8_t status = 0;

    while (p < end) {
        bool ok = false;
        uint32_t delta = read_vlq(&p, end, &ok);
        if (!ok) { tools_set_err(err, err_len, "malformed VLQ delta-time"); return false; }
        abstick += delta;

        if (p >= end) { tools_set_err(err, err_len, "truncated event"); return false; }

        uint8_t b = *p;
        if (b & 0x80u) {
            status = b;
            p++;
        } else if (status == 0) {
            tools_set_err(err, err_len, "running status with no prior status byte");
            return false;
        }

        uint8_t hi = status & 0xF0u;
        uint8_t chan = status & 0x0Fu;

        if (status == 0xFFu) {                       /* meta event */
            if (p >= end) { tools_set_err(err, err_len, "truncated meta"); return false; }
            uint8_t mtype = *p++;
            uint32_t mlen = read_vlq(&p, end, &ok);
            if (!ok || (size_t)(end - p) < mlen) {
                tools_set_err(err, err_len, "truncated meta payload");
                return false;
            }
            if (mtype == 0x51u && mlen == 3u) {      /* set tempo */
                uint32_t usec = ((uint32_t)p[0] << 16) |
                                ((uint32_t)p[1] << 8) | (uint32_t)p[2];
                mev *e = vec_push(evs);
                if (!e) return false;
                e->tick = abstick; e->seq = (*seqp)++;
                e->type = MEV_TEMPO; e->d3 = usec ? usec : 500000u;
            }
            /* 0x58 time-sig and 0x2F end-of-track are recognized-and-skipped;
             * every other meta is skipped by length. */
            p += mlen;
            status = 0;                              /* meta clears running status */
        } else if (status == 0xF0u || status == 0xF7u) { /* sysex */
            uint32_t slen = read_vlq(&p, end, &ok);
            if (!ok || (size_t)(end - p) < slen) {
                tools_set_err(err, err_len, "truncated sysex");
                return false;
            }
            p += slen;
            status = 0;
        } else {                                     /* channel voice message */
            switch (hi) {
            case 0x80: {                             /* note off */
                if ((size_t)(end - p) < 2) { tools_set_err(err, err_len, "truncated note-off"); return false; }
                uint8_t note = p[0]; p += 2;
                mev *e = vec_push(evs);
                if (!e) return false;
                e->tick = abstick; e->seq = (*seqp)++;
                e->type = MEV_NOTEOFF; e->chan = chan; e->d1 = (uint8_t)(note & 0x7Fu);
                break;
            }
            case 0x90: {                             /* note on (vel 0 == off) */
                if ((size_t)(end - p) < 2) { tools_set_err(err, err_len, "truncated note-on"); return false; }
                uint8_t note = p[0], vel = p[1]; p += 2;
                mev *e = vec_push(evs);
                if (!e) return false;
                e->tick = abstick; e->seq = (*seqp)++;
                e->chan = chan; e->d1 = (uint8_t)(note & 0x7Fu); e->d2 = (uint8_t)(vel & 0x7Fu);
                e->type = (vel == 0) ? MEV_NOTEOFF : MEV_NOTEON;
                break;
            }
            case 0xA0:                               /* poly aftertouch: 2 bytes */
            case 0xB0:                               /* control change:  2 bytes */
                if ((size_t)(end - p) < 2) { tools_set_err(err, err_len, "truncated 2-byte msg"); return false; }
                p += 2;
                break;
            case 0xC0: {                             /* program change: 1 byte */
                if ((size_t)(end - p) < 1) { tools_set_err(err, err_len, "truncated program"); return false; }
                uint8_t prog = *p++;
                mev *e = vec_push(evs);
                if (!e) return false;
                e->tick = abstick; e->seq = (*seqp)++;
                e->type = MEV_PROGRAM; e->chan = chan; e->d1 = (uint8_t)(prog & 0x7Fu);
                break;
            }
            case 0xD0:                               /* channel pressure: 1 byte */
                if ((size_t)(end - p) < 1) { tools_set_err(err, err_len, "truncated pressure"); return false; }
                p += 1;
                break;
            case 0xE0: {                             /* pitch bend: 2 bytes */
                if ((size_t)(end - p) < 2) { tools_set_err(err, err_len, "truncated bend"); return false; }
                uint8_t lo = p[0], mhi = p[1]; p += 2;
                if (import_bend) {
                    mev *e = vec_push(evs);
                    if (!e) return false;
                    e->tick = abstick; e->seq = (*seqp)++;
                    e->type = MEV_BEND; e->chan = chan;
                    e->d3 = (uint32_t)(lo & 0x7Fu) | ((uint32_t)(mhi & 0x7Fu) << 7);
                }
                break;
            }
            default:
                tools_set_err(err, err_len, "unknown status byte 0x%02X", (unsigned)status);
                return false;
            }
        }
    }
    return true;
}

static int mev_cmp(const void *a, const void *b) {
    const mev *x = a, *y = b;
    if (x->tick < y->tick) return -1;
    if (x->tick > y->tick) return 1;
    if (x->seq  < y->seq)  return -1;
    if (x->seq  > y->seq)  return 1;
    return 0;
}

/* ------------------------------------------------------------------------- */
/* quantization onto chip channels                                           */
/* ------------------------------------------------------------------------- */

#define TOOLS_MAX_ROWS (1u << 20)

typedef struct {
    uint64_t start_fine, end_fine;
    uint8_t  col;    /* chip channel */
    uint8_t  note, inst, vol;
} placement;

/* MIDI tick -> fine sequencer-tick position (rows*ticks_per_row resolution). */
static uint64_t to_fine(uint64_t t, unsigned rpb, unsigned tpr, unsigned div) {
    uint64_t num = t * (uint64_t)rpb * (uint64_t)tpr;
    return (num + (uint64_t)div / 2u) / (uint64_t)div;
}

static uint8_t choose_inst(const chipseq_midi_map *map, uint8_t chan,
                           uint8_t note, const uint8_t *program) {
    uint8_t idx;
    if (chan == 9u) {   /* GM channel 10 (0-indexed 9) is the drum kit */
        idx = map->drum_to_instrument ? map->drum_to_instrument[note] : 0u;
    } else {
        uint8_t prog = program[chan];
        idx = map->program_to_instrument ? map->program_to_instrument[prog] : 0u;
    }
    if (idx >= map->instrument_count) idx = 0u;
    return idx;
}

/* ------------------------------------------------------------------------- */
/* chipseq_midi_load                                                         */
/* ------------------------------------------------------------------------- */

chipseq_song *chipseq_midi_load(const char *path, const chipseq_midi_map *map,
                                char *err, size_t err_len) {
    if (err && err_len) err[0] = 0;

    if (!path) { tools_set_err(err, err_len, "midi_load: NULL path"); return NULL; }

    FILE *fp = fopen(path, "rb");
    if (!fp) { tools_set_err(err, err_len, "midi_load: cannot open '%s'", path); return NULL; }

    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); tools_set_err(err, err_len, "midi_load: seek failed"); return NULL; }
    long fsz = ftell(fp);
    if (fsz < 14) { fclose(fp); tools_set_err(err, err_len, "midi_load: file too small for an SMF"); return NULL; }
    rewind(fp);

    uint8_t *buf = malloc((size_t)fsz);
    if (!buf) { fclose(fp); tools_set_err(err, err_len, "midi_load: out of memory"); return NULL; }
    if (fread(buf, 1, (size_t)fsz, fp) != (size_t)fsz) {
        free(buf); fclose(fp);
        tools_set_err(err, err_len, "midi_load: short read");
        return NULL;
    }
    fclose(fp);

    chipseq_song *song = NULL;
    vec evs; vec_init(&evs, sizeof(mev));
    vec places; vec_init(&places, sizeof(placement));

    if (!map) { tools_set_err(err, err_len, "midi_load: NULL map"); goto fail; }
    if (map->instrument_count == 0 || !map->instruments) {
        tools_set_err(err, err_len, "midi_load: map has no instruments"); goto fail;
    }
    if (map->channels < 1u || map->channels > CHIPSEQ_CHANNELS_MAX) {
        tools_set_err(err, err_len, "midi_load: map->channels %u out of 1..%u",
                      (unsigned)map->channels, (unsigned)CHIPSEQ_CHANNELS_MAX);
        goto fail;
    }
    if (map->rows_per_beat < 1u || map->ticks_per_row < 1u || map->rows_per_pattern < 1u) {
        tools_set_err(err, err_len, "midi_load: map grid parameters must be >= 1");
        goto fail;
    }

    /* --- MThd --- */
    if (memcmp(buf, "MThd", 4) != 0) { tools_set_err(err, err_len, "midi_load: missing MThd"); goto fail; }
    uint32_t hlen = rd_be32(buf + 4);
    if (hlen < 6 || 8u + hlen > (uint32_t)fsz) { tools_set_err(err, err_len, "midi_load: bad MThd length"); goto fail; }
    uint16_t format   = rd_be16(buf + 8);
    uint16_t ntracks  = rd_be16(buf + 10);
    uint16_t division = rd_be16(buf + 12);
    if (format > 1u) { tools_set_err(err, err_len, "midi_load: format %u unsupported (need 0 or 1)", (unsigned)format); goto fail; }
    if (division & 0x8000u) { tools_set_err(err, err_len, "midi_load: SMPTE division unsupported"); goto fail; }
    if (division == 0u) { tools_set_err(err, err_len, "midi_load: zero division"); goto fail; }

    /* --- walk MTrk chunks --- */
    const uint8_t *p = buf + 8 + hlen;
    const uint8_t *fend = buf + fsz;
    uint32_t seq = 0;
    unsigned tracks_seen = 0;
    while (p + 8 <= fend && tracks_seen < ntracks) {
        if (memcmp(p, "MTrk", 4) != 0) { tools_set_err(err, err_len, "midi_load: expected MTrk"); goto fail; }
        uint32_t tlen = rd_be32(p + 4);
        const uint8_t *tstart = p + 8;
        if (tstart + tlen > fend) { tools_set_err(err, err_len, "midi_load: MTrk overruns file"); goto fail; }
        if (!parse_track(tstart, tstart + tlen, &evs, &seq, map->import_pitch_bend, err, err_len))
            goto fail;
        p = tstart + tlen;
        tracks_seen++;
    }

    /* --- stable time sort --- */
    if (evs.len > 1) qsort(evs.data, evs.len, sizeof(mev), mev_cmp);

    unsigned rpb = map->rows_per_beat, tpr = map->ticks_per_row;
    unsigned chans = map->channels;

    /* --- base tempo (first set-tempo, else 120 BPM) --- */
    uint32_t base_usec = 500000u;
    size_t first_tempo_idx = (size_t)-1;
    const mev *events = evs.data;
    for (size_t i = 0; i < evs.len; i++) {
        if (events[i].type == MEV_TEMPO) { base_usec = events[i].d3; first_tempo_idx = i; break; }
    }
    /* bpm_q8 = round(60e6 * 256 / usec) */
    uint32_t bpm_q8 = (uint32_t)(((uint64_t)60000000u * 256u + base_usec / 2u) / base_usec);
    if (bpm_q8 < CHIPSEQ_BPM_MIN) bpm_q8 = CHIPSEQ_BPM_MIN;
    if (bpm_q8 > 0xFFFFu) bpm_q8 = 0xFFFFu;

    /* --- allocate voices and walk events, building placements --- */
    uint8_t program[16];
    memset(program, 0, sizeof program);

    /* per-chip-channel current-note bookkeeping */
    bool     col_busy[CHIPSEQ_CHANNELS_MAX];
    uint64_t col_start[CHIPSEQ_CHANNELS_MAX];
    uint8_t  col_mchan[CHIPSEQ_CHANNELS_MAX];
    uint8_t  col_note[CHIPSEQ_CHANNELS_MAX];
    uint8_t  col_inst[CHIPSEQ_CHANNELS_MAX];
    uint8_t  col_vol[CHIPSEQ_CHANNELS_MAX];
    memset(col_busy, 0, sizeof col_busy);
    memset(col_start, 0, sizeof col_start);
    memset(col_mchan, 0, sizeof col_mchan);
    memset(col_note, 0, sizeof col_note);
    memset(col_inst, 0, sizeof col_inst);
    memset(col_vol, 0, sizeof col_vol);

    uint64_t max_fine = 0;
    unsigned dropped = 0;

    for (size_t i = 0; i < evs.len; i++) {
        const mev *e = &events[i];
        uint64_t fine = to_fine(e->tick, rpb, tpr, division);
        if (fine > max_fine) max_fine = fine;

        if (e->type == MEV_PROGRAM) {
            program[e->chan] = e->d1;
            continue;
        }
        if (e->type == MEV_TEMPO || e->type == MEV_BEND) {
            continue; /* handled after the grid exists */
        }
        if (e->type == MEV_NOTEON) {
            uint8_t inst = choose_inst(map, e->chan, e->d1, program);
            uint8_t vol = (uint8_t)(e->d2 >> 1);   /* 0..127 -> 0..63 */

            int col = -1;
            for (unsigned c = 0; c < chans; c++) {
                if (!col_busy[c]) { col = (int)c; break; }
            }
            if (col < 0) {
                if (!map->voice_steal) { dropped++; continue; }
                /* steal the oldest (smallest start_fine) voice */
                unsigned oldest = 0;
                for (unsigned c = 1; c < chans; c++)
                    if (col_start[c] < col_start[oldest]) oldest = c;
                placement *pl = vec_push(&places);
                if (!pl) goto oom;
                pl->col = (uint8_t)oldest;
                pl->note = col_note[oldest]; pl->inst = col_inst[oldest]; pl->vol = col_vol[oldest];
                pl->start_fine = col_start[oldest];
                pl->end_fine = fine;
                col = (int)oldest;
            }
            unsigned uc = (unsigned)col;
            col_busy[uc] = true;
            col_start[uc] = fine;
            col_mchan[uc] = e->chan;
            col_note[uc]  = e->d1;
            col_inst[uc]  = inst;
            col_vol[uc]   = vol;
        } else { /* MEV_NOTEOFF */
            /* find the chip channel holding this (midi chan, note) */
            for (unsigned c = 0; c < chans; c++) {
                if (col_busy[c] && col_mchan[c] == e->chan && col_note[c] == e->d1) {
                    placement *pl = vec_push(&places);
                    if (!pl) goto oom;
                    pl->col = (uint8_t)c;
                    pl->note = col_note[c]; pl->inst = col_inst[c]; pl->vol = col_vol[c];
                    pl->start_fine = col_start[c];
                    pl->end_fine = fine;
                    col_busy[c] = false;
                    break;
                }
            }
        }
    }
    /* finalize any notes still held at end-of-song */
    for (unsigned c = 0; c < chans; c++) {
        if (col_busy[c]) {
            placement *pl = vec_push(&places);
            if (!pl) goto oom;
            pl->col = (uint8_t)c;
            pl->note = col_note[c]; pl->inst = col_inst[c]; pl->vol = col_vol[c];
            pl->start_fine = col_start[c];
            pl->end_fine = max_fine + (uint64_t)tpr; /* ring to the next row */
            col_busy[c] = false;
        }
    }

    /* --- grid dimensions --- */
    uint64_t end_fine_max = max_fine;
    {
        const placement *pls = places.data;
        for (size_t i = 0; i < places.len; i++)
            if (pls[i].end_fine > end_fine_max) end_fine_max = pls[i].end_fine;
    }
    uint64_t total_rows64 = end_fine_max / (uint64_t)tpr + 1u;
    if (total_rows64 > TOOLS_MAX_ROWS) {
        tools_set_err(err, err_len, "midi_load: song too long (%llu rows)",
                      (unsigned long long)total_rows64);
        goto fail;
    }
    unsigned total_rows = (unsigned)total_rows64;
    unsigned rpp = map->rows_per_pattern;
    unsigned npat = (total_rows + rpp - 1u) / rpp;
    if (npat == 0) npat = 1;
    unsigned padded_rows = npat * rpp;
    size_t total_cells = (size_t)padded_rows * chans;

    /* --- build the cell grid --- */
    chipseq_cell *cells = malloc(total_cells * sizeof(chipseq_cell));
    if (!cells) goto oom;
    for (size_t i = 0; i < total_cells; i++) {
        cells[i].note = CHIPSEQ_NOTE_NONE; cells[i].inst = 0;
        cells[i].vol = CHIPSEQ_VOL_NONE; cells[i].fx = CHIPSEQ_FX_NONE; cells[i].fxp = 0;
    }

    {
        const placement *pls = places.data;
        for (size_t i = 0; i < places.len; i++) {
            const placement *pl = &pls[i];
            unsigned row_s = (unsigned)(pl->start_fine / tpr);
            unsigned sub_s = (unsigned)(pl->start_fine % tpr);
            if (row_s >= padded_rows) continue;
            chipseq_cell *sc = &cells[(size_t)row_s * chans + pl->col];
            sc->note = pl->note; sc->inst = pl->inst; sc->vol = pl->vol;
            if (sub_s > 0) { sc->fx = CHIPSEQ_FX_NOTE_DELAY; sc->fxp = (uint8_t)sub_s; }

            if (pl->end_fine > pl->start_fine) {
                unsigned row_e = (unsigned)(pl->end_fine / tpr);
                unsigned sub_e = (unsigned)(pl->end_fine % tpr);
                if (row_e < padded_rows) {
                    chipseq_cell *ec = &cells[(size_t)row_e * chans + pl->col];
                    if (ec->note == CHIPSEQ_NOTE_NONE && ec->fx == CHIPSEQ_FX_NONE) {
                        if (sub_e > 0) { ec->fx = CHIPSEQ_FX_NOTE_CUT; ec->fxp = (uint8_t)sub_e; }
                        else            { ec->note = CHIPSEQ_NOTE_CUT; }
                    }
                }
            }
        }
    }

    /* --- mid-song tempo changes -> FX_TEMPO, pitch bends -> FX_PORTA. The first
     *     set-tempo already defined the song's base bpm_q8, so it is skipped;
     *     every LATER tempo becomes an FX_TEMPO cell. --- */
    for (size_t i = 0; i < evs.len; i++) {
        const mev *e = &events[i];
        if (e->type != MEV_TEMPO && e->type != MEV_BEND) continue;
        if (e->type == MEV_TEMPO && i == first_tempo_idx) continue; /* the base */
        uint64_t fine = to_fine(e->tick, rpb, tpr, division);
        unsigned row = (unsigned)(fine / tpr);
        if (row >= padded_rows) continue;

        uint8_t fx; uint8_t fxp;
        if (e->type == MEV_TEMPO) {
            uint32_t bpm = (uint32_t)(((uint64_t)60000000u * 256u + e->d3 / 2u) / e->d3) >> 8;
            if (bpm < 1u) bpm = 1u;
            if (bpm > 255u) bpm = 255u;
            fx = (uint8_t)CHIPSEQ_FX_TEMPO; fxp = (uint8_t)bpm;
        } else { /* MEV_BEND -> FX_PORTA up/down (deterministic mapping) */
            int delta = (int)e->d3 - 8192;
            if (delta == 0) continue;
            unsigned mag = (unsigned)(delta < 0 ? -delta : delta) >> 9;
            if (mag < 1u) mag = 1u;
            if (mag > 255u) mag = 255u;
            fx = (uint8_t)(delta > 0 ? CHIPSEQ_FX_PORTA_UP : CHIPSEQ_FX_PORTA_DOWN);
            fxp = (uint8_t)mag;
        }
        /* attach to the first fully-empty cell in this row (never clobber a note
         * or an existing effect) */
        for (unsigned c = 0; c < chans; c++) {
            chipseq_cell *cc = &cells[(size_t)row * chans + c];
            if (cc->note == CHIPSEQ_NOTE_NONE && cc->fx == CHIPSEQ_FX_NONE &&
                cc->vol == CHIPSEQ_VOL_NONE) {
                cc->fx = fx; cc->fxp = fxp;
                break;
            }
        }
    }

    /* --- patterns + order --- */
    chipseq_pattern *patterns = malloc((size_t)npat * sizeof(chipseq_pattern));
    uint8_t *order = malloc((size_t)npat);
    if (!patterns || !order) { free(patterns); free(order); free(cells); goto oom; }
    for (unsigned pi = 0; pi < npat; pi++) {
        patterns[pi].cells = cells + (size_t)pi * rpp * chans;
        patterns[pi].rows = (uint16_t)rpp;
        order[pi] = (uint8_t)pi;
    }

    /* --- instruments (copied so the song is one self-owned allocation set; the
     *     inner seq/pcm/wavetable pointers stay caller-owned, exactly like the
     *     C-literal path) --- */
    chipseq_instrument *insts = malloc((size_t)map->instrument_count * sizeof(chipseq_instrument));
    if (!insts) { free(patterns); free(order); free(cells); goto oom; }
    memcpy(insts, map->instruments, (size_t)map->instrument_count * sizeof(chipseq_instrument));

    /* --- name --- */
    const char *nm = "imported";
    char *name = malloc(strlen(nm) + 1);
    if (!name) { free(insts); free(patterns); free(order); free(cells); goto oom; }
    memcpy(name, nm, strlen(nm) + 1);

    song = calloc(1, sizeof(chipseq_song));
    if (!song) { free(name); free(insts); free(patterns); free(order); free(cells); goto oom; }

    song->name = name;
    song->instruments = insts;
    song->instrument_count = map->instrument_count;
    song->patterns = patterns;
    song->pattern_count = (uint16_t)npat;
    song->order = order;
    song->order_length = (uint16_t)npat;
    song->loop_order = CHIPSEQ_NO_LOOP;
    song->channels = (uint8_t)chans;
    song->rows_per_beat = (uint8_t)rpb;
    song->ticks_per_row = (uint8_t)tpr;
    song->bpm_q8 = (uint16_t)bpm_q8;

    vec_free(&evs);
    vec_free(&places);
    free(buf);

    /* validate the freshly built song; report but do not discard on a warning */
    {
        char verr[160];
        if (!chipseq_song_validate(song, verr, sizeof verr)) {
            tools_set_err(err, err_len, "midi_load: produced invalid song: %s", verr);
            chipseq_song_free(song);
            return NULL;
        }
    }
    if (dropped > 0)
        tools_set_err(err, err_len, "midi_load: warning: %u note(s) dropped (voice budget %u)",
                      dropped, chans);
    return song;

oom:
    tools_set_err(err, err_len, "midi_load: out of memory");
fail:
    vec_free(&evs);
    vec_free(&places);
    free(buf);
    return NULL;
}

/* ------------------------------------------------------------------------- */
/* chipseq_song_free                                                         */
/* ------------------------------------------------------------------------- */

void chipseq_song_free(chipseq_song *song) {
    if (!song) return;
    free((void *)(uintptr_t)song->name);
    free((void *)(uintptr_t)song->instruments);
    if (song->patterns) {
        if (song->pattern_count > 0)
            free((void *)(uintptr_t)song->patterns[0].cells); /* one contiguous grid */
        free((void *)(uintptr_t)song->patterns);
    }
    free((void *)(uintptr_t)song->order);
    free(song);
}

/* ------------------------------------------------------------------------- */
/* chipseq_song_write_c -- diff-stable, self-contained C emission            */
/* ------------------------------------------------------------------------- */

static const char *WAVE_NAME[] = {
    "CHIPSEQ_WAVE_PULSE", "CHIPSEQ_WAVE_TRIANGLE", "CHIPSEQ_WAVE_SAW",
    "CHIPSEQ_WAVE_NOISE", "CHIPSEQ_WAVE_WAVETABLE", "CHIPSEQ_WAVE_PCM"
};
static const char *NOISE_NAME[] = { "CHIPSEQ_NOISE_LONG", "CHIPSEQ_NOISE_SHORT" };
static const char *PC_NAME[12] = {
    "C", "Cs", "D", "Ds", "E", "F", "Fs", "G", "Gs", "A", "As", "B"
};

static void emit_str_literal(FILE *f, const char *s) {
    fputc('"', f);
    if (s) {
        for (const char *q = s; *q; q++) {
            unsigned char c = (unsigned char)*q;
            if (c == '"' || c == '\\') { fputc('\\', f); fputc((int)c, f); }
            else if (c >= 0x20 && c < 0x7F) { fputc((int)c, f); }
            else { fprintf(f, "\\x%02X", (unsigned)c); }
        }
    }
    fputc('"', f);
}

static void emit_seq(FILE *f, const char *ident, unsigned inst,
                     const char *kind, const chipseq_seq *s) {
    fprintf(f, "static const int8_t %s_%s%u_v[] = {", ident, kind, inst);
    for (uint16_t i = 0; i < s->length; i++)
        fprintf(f, "%s%d", i ? "," : "", (int)s->values[i]);
    fprintf(f, "};\n");
    fprintf(f, "static const chipseq_seq %s_%s%u = { %s_%s%u_v, %u, ",
            ident, kind, inst, ident, kind, inst, (unsigned)s->length);
    if (s->loop < 0) fprintf(f, "CHIPSEQ_SEQ_NO_LOOP, ");
    else             fprintf(f, "%d, ", (int)s->loop);
    if (s->release < 0) fprintf(f, "CHIPSEQ_SEQ_NO_RELEASE };\n");
    else                fprintf(f, "%d };\n", (int)s->release);
}

/* Emit one cell using the most readable CS_* macro, falling back to a raw
 * brace-initializer for combinations no macro covers (keeps round-trip exact). */
static void emit_cell(FILE *f, const chipseq_cell *c) {
    bool vol_none = (c->vol == CHIPSEQ_VOL_NONE);
    bool fx_none  = (c->fx == CHIPSEQ_FX_NONE);

    if (c->note == CHIPSEQ_NOTE_NONE) {
        if (vol_none && fx_none) { fprintf(f, "CS__"); return; }
        if (vol_none && !fx_none) {
            fprintf(f, "CS_FX(%s,0x%02X)", chipseq_fx_name((chipseq_fx)c->fx), (unsigned)c->fxp);
            return;
        }
        if (!vol_none && fx_none) { fprintf(f, "CS_V(%u)", (unsigned)c->vol); return; }
        /* vol + fx, no note: no macro -> raw */
    } else if (c->note == CHIPSEQ_NOTE_OFF && vol_none && fx_none) {
        fprintf(f, "CS_OFF"); return;
    } else if (c->note == CHIPSEQ_NOTE_CUT && vol_none && fx_none) {
        fprintf(f, "CS_CUT"); return;
    } else if (c->note <= 127) {
        unsigned pc = c->note % 12u;
        int oct = (int)(c->note / 12u) - 1;
        if (fx_none) {
            fprintf(f, "CS_N(%s,%d, %u,%u)", PC_NAME[pc], oct, (unsigned)c->inst, (unsigned)c->vol);
            return;
        }
        fprintf(f, "CS_NF(%s,%d, %u,%u, %s,0x%02X)", PC_NAME[pc], oct,
                (unsigned)c->inst, (unsigned)c->vol,
                chipseq_fx_name((chipseq_fx)c->fx), (unsigned)c->fxp);
        return;
    }

    /* raw fallback: fully explicit five-byte initializer */
    fprintf(f, "{ ");
    if (c->note == CHIPSEQ_NOTE_NONE)     fprintf(f, "CHIPSEQ_NOTE_NONE, ");
    else if (c->note == CHIPSEQ_NOTE_OFF) fprintf(f, "CHIPSEQ_NOTE_OFF, ");
    else if (c->note == CHIPSEQ_NOTE_CUT) fprintf(f, "CHIPSEQ_NOTE_CUT, ");
    else                                  fprintf(f, "%u, ", (unsigned)c->note);
    fprintf(f, "%u, ", (unsigned)c->inst);
    if (vol_none) fprintf(f, "CHIPSEQ_VOL_NONE, ");
    else          fprintf(f, "%u, ", (unsigned)c->vol);
    fprintf(f, "CHIPSEQ_FX_%s, 0x%02X }", chipseq_fx_name((chipseq_fx)c->fx), (unsigned)c->fxp);
}

bool chipseq_song_write_c(const chipseq_song *song, const char *path,
                          const char *ident, char *err, size_t err_len) {
    if (err && err_len) err[0] = 0;
    if (!song)  { tools_set_err(err, err_len, "write_c: NULL song");  return false; }
    if (!path)  { tools_set_err(err, err_len, "write_c: NULL path");  return false; }
    if (!ident) { tools_set_err(err, err_len, "write_c: NULL ident"); return false; }

    char verr[160];
    if (!chipseq_song_validate(song, verr, sizeof verr)) {
        tools_set_err(err, err_len, "write_c: invalid song: %s", verr);
        return false;
    }

    FILE *f = fopen(path, "wb");
    if (!f) { tools_set_err(err, err_len, "write_c: cannot open '%s'", path); return false; }

    /* header -- NO timestamp, so regenerating from unchanged input is stable */
    fprintf(f, "/* Generated by chip-sequencer chipseq_song_write_c. Do not edit by hand. */\n");
    fprintf(f, "#include \"chip_sequencer.h\"\n\n");

    /* tracker-screen cell macros (guarded so several songs can coexist) */
    fprintf(f, "#ifndef CS__\n");
    fprintf(f, "#define CS__            { CHIPSEQ_NOTE_NONE, 0, CHIPSEQ_VOL_NONE, CHIPSEQ_FX_NONE, 0 }\n");
    fprintf(f, "#define CS_OFF          { CHIPSEQ_NOTE_OFF,  0, CHIPSEQ_VOL_NONE, CHIPSEQ_FX_NONE, 0 }\n");
    fprintf(f, "#define CS_CUT          { CHIPSEQ_NOTE_CUT,  0, CHIPSEQ_VOL_NONE, CHIPSEQ_FX_NONE, 0 }\n");
    fprintf(f, "#define CS_N(pc,oct,i,v)       { CHIPSEQ_NOTE(CHIPSEQ_PC_##pc,oct), (i), (v), CHIPSEQ_FX_NONE, 0 }\n");
    fprintf(f, "#define CS_NF(pc,oct,i,v,fx,p) { CHIPSEQ_NOTE(CHIPSEQ_PC_##pc,oct), (i), (v), CHIPSEQ_FX_##fx, (p) }\n");
    fprintf(f, "#define CS_V(v)         { CHIPSEQ_NOTE_NONE, 0, (v), CHIPSEQ_FX_NONE, 0 }\n");
    fprintf(f, "#define CS_FX(fx,p)     { CHIPSEQ_NOTE_NONE, 0, CHIPSEQ_VOL_NONE, CHIPSEQ_FX_##fx, (p) }\n");
    fprintf(f, "#endif\n\n");

    /* instruments (deterministic order 0..n-1; sub-tables in fixed kind order) */
    const chipseq_instrument *in = song->instruments;
    for (unsigned i = 0; i < song->instrument_count; i++) {
        const chipseq_instrument *it = &in[i];
        if (it->vol_seq)   emit_seq(f, ident, i, "vol",   it->vol_seq);
        if (it->arp_seq)   emit_seq(f, ident, i, "arp",   it->arp_seq);
        if (it->pitch_seq) emit_seq(f, ident, i, "pitch", it->pitch_seq);
        if (it->duty_seq)  emit_seq(f, ident, i, "duty",  it->duty_seq);
        if (it->wave == CHIPSEQ_WAVE_WAVETABLE && it->wavetable) {
            fprintf(f, "static const uint8_t %s_wt%u[] = {", ident, i);
            for (unsigned k = 0; k < CHIPSEQ_WAVETABLE_LEN; k++)
                fprintf(f, "%s%u", k ? "," : "", (unsigned)it->wavetable[k]);
            fprintf(f, "};\n");
        }
        if (it->wave == CHIPSEQ_WAVE_PCM && it->pcm) {
            const chipseq_pcm *pm = it->pcm;
            fprintf(f, "static const int16_t %s_pcm%u_f[] = {", ident, i);
            for (uint32_t k = 0; k < pm->frame_count; k++)
                fprintf(f, "%s%d", k ? "," : "", (int)pm->frames[k]);
            fprintf(f, "};\n");
            fprintf(f, "static const chipseq_pcm %s_pcm%u = { %s_pcm%u_f, %lu, %lu, %lu, %u };\n",
                    ident, i, ident, i,
                    (unsigned long)pm->frame_count, (unsigned long)pm->loop_start,
                    (unsigned long)pm->loop_end, (unsigned)pm->root_note);
        }
    }

    fprintf(f, "static const chipseq_instrument %s_insts[] = {\n", ident);
    for (unsigned i = 0; i < song->instrument_count; i++) {
        const chipseq_instrument *it = &in[i];
        fprintf(f, "    { ");
        fprintf(f, ".name = ");
        emit_str_literal(f, it->name);
        fprintf(f, ", .wave = %s",
                it->wave < 6u ? WAVE_NAME[it->wave] : "CHIPSEQ_WAVE_PULSE");
        if (it->duty)       fprintf(f, ", .duty = %u", (unsigned)it->duty);
        if (it->noise_mode) fprintf(f, ", .noise_mode = %s",
                                    it->noise_mode < 2u ? NOISE_NAME[it->noise_mode] : NOISE_NAME[0]);
        if (it->tri_steps)  fprintf(f, ", .tri_steps = %u", (unsigned)it->tri_steps);
        if (it->vol_seq)    fprintf(f, ", .vol_seq = &%s_vol%u", ident, i);
        if (it->arp_seq)    fprintf(f, ", .arp_seq = &%s_arp%u", ident, i);
        if (it->pitch_seq)  fprintf(f, ", .pitch_seq = &%s_pitch%u", ident, i);
        if (it->duty_seq)   fprintf(f, ", .duty_seq = &%s_duty%u", ident, i);
        if (it->wave == CHIPSEQ_WAVE_WAVETABLE && it->wavetable)
            fprintf(f, ", .wavetable = %s_wt%u", ident, i);
        if (it->wave == CHIPSEQ_WAVE_PCM && it->pcm)
            fprintf(f, ", .pcm = &%s_pcm%u", ident, i);
        if (it->transpose)  fprintf(f, ", .transpose = %d", (int)it->transpose);
        if (it->finetune)   fprintf(f, ", .finetune = %d", (int)it->finetune);
        if (it->vib_speed)  fprintf(f, ", .vib_speed = %u", (unsigned)it->vib_speed);
        if (it->vib_depth)  fprintf(f, ", .vib_depth = %u", (unsigned)it->vib_depth);
        if (it->vib_delay)  fprintf(f, ", .vib_delay = %u", (unsigned)it->vib_delay);
        fprintf(f, " },\n");
    }
    fprintf(f, "};\n\n");

    /* patterns */
    for (unsigned pi = 0; pi < song->pattern_count; pi++) {
        const chipseq_pattern *pt = &song->patterns[pi];
        fprintf(f, "static const chipseq_cell %s_pat%u[] = {\n", ident, pi);
        for (unsigned r = 0; r < pt->rows; r++) {
            fprintf(f, "    ");
            for (unsigned c = 0; c < song->channels; c++) {
                emit_cell(f, &pt->cells[(size_t)r * song->channels + c]);
                fprintf(f, ",");
                if (c + 1u < song->channels) fprintf(f, " ");
            }
            fprintf(f, "\n");
        }
        fprintf(f, "};\n");
    }
    fprintf(f, "\n");

    fprintf(f, "static const chipseq_pattern %s_patterns[] = {\n", ident);
    for (unsigned pi = 0; pi < song->pattern_count; pi++)
        fprintf(f, "    { %s_pat%u, %u },\n", ident, pi, (unsigned)song->patterns[pi].rows);
    fprintf(f, "};\n\n");

    fprintf(f, "static const uint8_t %s_order[] = {", ident);
    for (unsigned i = 0; i < song->order_length; i++)
        fprintf(f, "%s%u", i ? "," : "", (unsigned)song->order[i]);
    fprintf(f, "};\n\n");

    /* the song object -- `ident` is the exported symbol */
    fprintf(f, "const chipseq_song %s = {\n", ident);
    fprintf(f, "    .name = ");
    emit_str_literal(f, song->name);
    fprintf(f, ",\n");
    fprintf(f, "    .instruments = %s_insts, .instrument_count = %u,\n",
            ident, (unsigned)song->instrument_count);
    fprintf(f, "    .patterns = %s_patterns, .pattern_count = %u,\n",
            ident, (unsigned)song->pattern_count);
    fprintf(f, "    .order = %s_order, .order_length = %u,\n",
            ident, (unsigned)song->order_length);
    if (song->loop_order == CHIPSEQ_NO_LOOP)
        fprintf(f, "    .loop_order = CHIPSEQ_NO_LOOP, .channels = %u,\n", (unsigned)song->channels);
    else
        fprintf(f, "    .loop_order = %u, .channels = %u,\n",
                (unsigned)song->loop_order, (unsigned)song->channels);
    fprintf(f, "    .rows_per_beat = %u, .ticks_per_row = %u,\n",
            (unsigned)song->rows_per_beat, (unsigned)song->ticks_per_row);
    fprintf(f, "    .bpm_q8 = %u,\n", (unsigned)song->bpm_q8);
    fprintf(f, "};\n");

    if (fclose(f) != 0) { tools_set_err(err, err_len, "write_c: write/close failed"); return false; }
    return true;
}
