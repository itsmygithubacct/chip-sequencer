/* test_tools.c -- offline regression tests for the tools translation unit:
 *
 *   - build a small ORIGINAL Standard MIDI File in memory (format 1, three
 *     tracks: tempo, a monophonic melody with a program change + a pitch bend,
 *     and a drum line with off-grid hits), write it to a scratch file;
 *   - chipseq_midi_load it and assert the quantized song validates;
 *   - chipseq_song_write_c the result twice and assert the emitted C is
 *     byte-identical across the two runs (diff-stable / idempotent);
 *   - free with chipseq_song_free and exercise the error paths.
 *
 * This TU is NEVER linked into a game. It is stdio-only: it opens no audio
 * device, starts no thread, and calls no render path.
 */
#include "chip_sequencer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fails = 0;
static int g_checks = 0;
#define CHECK(cond, msg) do { \
    g_checks++; \
    if (!(cond)) { printf("FAIL: %s  (%s:%d)\n", (msg), __FILE__, __LINE__); g_fails++; } \
} while (0)

/* ------------------------------------------------------------------------- */
/* a tiny append-only byte buffer for assembling the SMF                     */
/* ------------------------------------------------------------------------- */

typedef struct { uint8_t *d; size_t n, cap; } buf;

_Noreturn static void test_oom(void) {
    fputs("test_tools: out of memory while building a fixture\n", stderr);
    exit(EXIT_FAILURE);
}

static void bput(buf *b, uint8_t v) {
    if (b->n == b->cap) {
        if (b->cap > SIZE_MAX / 2u) test_oom();
        size_t new_cap = b->cap ? b->cap * 2u : 128u;
        uint8_t *grown = realloc(b->d, new_cap);
        if (!grown) test_oom();
        b->d = grown;
        b->cap = new_cap;
    }
    b->d[b->n++] = v;
}
static void bput_be16(buf *b, uint16_t v) { bput(b, (uint8_t)(v >> 8)); bput(b, (uint8_t)v); }
static void bput_be32(buf *b, uint32_t v) {
    bput(b, (uint8_t)(v >> 24)); bput(b, (uint8_t)(v >> 16));
    bput(b, (uint8_t)(v >> 8));  bput(b, (uint8_t)v);
}
static void bput_bytes(buf *b, const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) bput(b, p[i]);
}
/* variable-length quantity */
static void bput_vlq(buf *b, uint32_t v) {
    uint8_t stack[5]; int n = 0;
    stack[n++] = (uint8_t)(v & 0x7Fu);
    v >>= 7;
    while (v) { stack[n++] = (uint8_t)((v & 0x7Fu) | 0x80u); v >>= 7; }
    for (int i = n - 1; i >= 0; i--) bput(b, stack[i]);
}

/* wrap a track body in an MTrk chunk appended to `out` */
static void append_mtrk(buf *out, const buf *body) {
    bput_bytes(out, (const uint8_t *)"MTrk", 4);
    bput_be32(out, (uint32_t)body->n);
    bput_bytes(out, body->d, body->n);
}

/* build the whole SMF into `out` (caller frees out->d) */
static void build_smf(buf *out) {
    /* MThd: format 1, 3 tracks, 48 ticks per quarter note */
    bput_bytes(out, (const uint8_t *)"MThd", 4);
    bput_be32(out, 6);
    bput_be16(out, 1);   /* format 1 */
    bput_be16(out, 3);   /* ntracks  */
    bput_be16(out, 48);  /* division (TPQN) */

    /* --- track 0: tempo map --- */
    {
        buf t = {0};
        bput_vlq(&t, 0); bput(&t, 0xFF); bput(&t, 0x51); bput(&t, 3);
        bput(&t, 0x07); bput(&t, 0xA1); bput(&t, 0x20);      /* 500000 us = 120 BPM */
        bput_vlq(&t, 96); bput(&t, 0xFF); bput(&t, 0x51); bput(&t, 3);
        bput(&t, 0x06); bput(&t, 0x1A); bput(&t, 0x80);      /* 400000 us = 150 BPM (mid-song) */
        bput_vlq(&t, 0); bput(&t, 0xFF); bput(&t, 0x2F); bput(&t, 0);
        append_mtrk(out, &t);
        free(t.d);
    }
    /* --- track 1: melody on channel 0, with a program change + a pitch bend --- */
    {
        buf t = {0};
        bput_vlq(&t, 0);  bput(&t, 0xC0); bput(&t, 0x00);            /* program 0 */
        bput_vlq(&t, 0);  bput(&t, 0x90); bput(&t, 60); bput(&t, 100); /* C4 on   */
        bput_vlq(&t, 48); bput(&t, 0x80); bput(&t, 60); bput(&t, 0);   /* C4 off  */
        bput_vlq(&t, 0);  bput(&t, 0x90); bput(&t, 64); bput(&t, 100); /* E4 on   */
        bput_vlq(&t, 12); bput(&t, 0xE0); bput(&t, 0x00); bput(&t, 0x60); /* bend +*/
        bput_vlq(&t, 36); bput(&t, 0x80); bput(&t, 64); bput(&t, 0);   /* E4 off  */
        bput_vlq(&t, 0);  bput(&t, 0x90); bput(&t, 67); bput(&t, 100); /* G4 on   */
        bput_vlq(&t, 48); bput(&t, 0x80); bput(&t, 67); bput(&t, 0);   /* G4 off  */
        bput_vlq(&t, 0);  bput(&t, 0x90); bput(&t, 72); bput(&t, 100); /* C5 on   */
        bput_vlq(&t, 48); bput(&t, 0x80); bput(&t, 72); bput(&t, 0);   /* C5 off  */
        bput_vlq(&t, 0);  bput(&t, 0xFF); bput(&t, 0x2F); bput(&t, 0);
        append_mtrk(out, &t);
        free(t.d);
    }
    /* --- track 2: drums on channel 9, deliberately off the row grid --- */
    {
        buf t = {0};
        bput_vlq(&t, 8);  bput(&t, 0x99); bput(&t, 38); bput(&t, 100); /* snare on  @ t=8  */
        bput_vlq(&t, 20); bput(&t, 0x89); bput(&t, 38); bput(&t, 0);   /* snare off @ t=28 */
        bput_vlq(&t, 20); bput(&t, 0x99); bput(&t, 38); bput(&t, 100); /* snare on  @ t=48 */
        bput_vlq(&t, 24); bput(&t, 0x89); bput(&t, 38); bput(&t, 0);   /* snare off @ t=72 */
        bput_vlq(&t, 0);  bput(&t, 0xFF); bput(&t, 0x2F); bput(&t, 0);
        append_mtrk(out, &t);
        free(t.d);
    }
}

static void put_smf_header_div(buf *out, uint16_t ntracks, uint16_t division) {
    bput_bytes(out, (const uint8_t *)"MThd", 4);
    bput_be32(out, 6);
    bput_be16(out, ntracks == 1 ? 0 : 1);
    bput_be16(out, ntracks);
    bput_be16(out, division);
}

static void put_smf_header(buf *out, uint16_t ntracks) {
    put_smf_header_div(out, ntracks, 48);
}

/* A format-0 song whose first tempo event occurs after playback has begun. */
static void build_delayed_tempo_smf(buf *out) {
    put_smf_header(out, 1);
    buf t = {0};
    bput_vlq(&t, 0);  bput(&t, 0x90); bput(&t, 60); bput(&t, 100);
    bput_vlq(&t, 48); bput(&t, 0xFF); bput(&t, 0x51); bput(&t, 3);
    bput(&t, 0x06); bput(&t, 0x1A); bput(&t, 0x80); /* 150 BPM */
    bput_vlq(&t, 48); bput(&t, 0x80); bput(&t, 60); bput(&t, 0);
    bput_vlq(&t, 0);  bput(&t, 0xFF); bput(&t, 0x2F); bput(&t, 0);
    append_mtrk(out, &t);
    free(t.d);
}

/* Channel 1 claims chip column 0 first; channel 0 therefore lives on column 1.
 * Its bend must follow it to column 1 rather than using the first empty cell. */
static void build_bend_routing_smf(buf *out) {
    put_smf_header(out, 1);
    buf t = {0};
    bput_vlq(&t, 0);  bput(&t, 0x91); bput(&t, 55); bput(&t, 100);
    bput_vlq(&t, 0);  bput(&t, 0x90); bput(&t, 60); bput(&t, 100);
    bput_vlq(&t, 24); bput(&t, 0xE0); bput(&t, 0x00); bput(&t, 0x60);
    bput_vlq(&t, 24); bput(&t, 0x81); bput(&t, 55); bput(&t, 0);
    bput_vlq(&t, 0);  bput(&t, 0x80); bput(&t, 60); bput(&t, 0);
    bput_vlq(&t, 0);  bput(&t, 0xFF); bput(&t, 0x2F); bput(&t, 0);
    append_mtrk(out, &t);
    free(t.d);
}

/* With one row per pattern this produces 301 patterns, which the uint8 order
 * representation cannot address without wrapping. */
static void build_too_many_patterns_smf(buf *out) {
    put_smf_header(out, 1);
    buf t = {0};
    bput_vlq(&t, 0);    bput(&t, 0x90); bput(&t, 60); bput(&t, 100);
    bput_vlq(&t, 3600); bput(&t, 0x80); bput(&t, 60); bput(&t, 0);
    bput_vlq(&t, 0);    bput(&t, 0xFF); bput(&t, 0x2F); bput(&t, 0);
    append_mtrk(out, &t);
    free(t.d);
}

/* A one-MIDI-tick note on a 480-TPQN timeline: both endpoints round to fine
 * tick zero on the 4-row/6-tick grid, but the importer must not let it sustain. */
static void build_short_note_smf(buf *out) {
    put_smf_header_div(out, 1, 480);
    buf t = {0};
    bput_vlq(&t, 0); bput(&t, 0x90); bput(&t, 60); bput(&t, 100);
    bput_vlq(&t, 1); bput(&t, 0x80); bput(&t, 60); bput(&t, 0);
    bput_vlq(&t, 0); bput(&t, 0xFF); bput(&t, 0x2F); bput(&t, 0);
    append_mtrk(out, &t);
    free(t.d);
}

/* Preserve a declared silent tail ending exactly at a pattern boundary. */
static void build_trailing_silence_smf(buf *out) {
    put_smf_header(out, 1);
    buf t = {0};
    bput_vlq(&t, 0);   bput(&t, 0x90); bput(&t, 60); bput(&t, 100);
    bput_vlq(&t, 48);  bput(&t, 0x80); bput(&t, 60); bput(&t, 0);
    bput_vlq(&t, 720); bput(&t, 0xFF); bput(&t, 0x2F); bput(&t, 0);
    append_mtrk(out, &t);
    free(t.d);
}

/* ------------------------------------------------------------------------- */
/* the import map (original instruments, with sequences to exercise emission) */
/* ------------------------------------------------------------------------- */

static const int8_t lead_env[] = { 64, 48, 34, 24, 16, 8, 0 };
static const chipseq_seq lead_vol = { lead_env, 7, CHIPSEQ_SEQ_NO_LOOP, CHIPSEQ_SEQ_NO_RELEASE };
static const int8_t drum_env[] = { 64, 40, 18, 4, 0 };
static const chipseq_seq drum_vol = { drum_env, 5, CHIPSEQ_SEQ_NO_LOOP, CHIPSEQ_SEQ_NO_RELEASE };

static const chipseq_instrument map_insts[] = {
    { .name = "lead", .wave = CHIPSEQ_WAVE_PULSE, .duty = CHIPSEQ_DUTY_25, .vol_seq = &lead_vol },
    { .name = "bass", .wave = CHIPSEQ_WAVE_TRIANGLE, .tri_steps = 32, .transpose = -12 },
    { .name = "drum", .wave = CHIPSEQ_WAVE_NOISE, .noise_mode = CHIPSEQ_NOISE_LONG, .vol_seq = &drum_vol },
};

/* ------------------------------------------------------------------------- */
/* helpers                                                                   */
/* ------------------------------------------------------------------------- */

static bool write_all(const char *path, const uint8_t *data, size_t n) {
    FILE *fp = fopen(path, "wb");
    if (!fp) return false;
    bool ok = fwrite(data, 1, n, fp) == n;
    if (fclose(fp) != 0) ok = false;
    return ok;
}

static uint8_t *read_all(const char *path, size_t *out_n) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    long sz = ftell(fp);
    if (sz < 0) { fclose(fp); return NULL; }
    rewind(fp);
    uint8_t *d = malloc((size_t)sz ? (size_t)sz : 1);
    if (!d) { fclose(fp); return NULL; }
    if (fread(d, 1, (size_t)sz, fp) != (size_t)sz) { free(d); fclose(fp); return NULL; }
    fclose(fp);
    if (out_n) *out_n = (size_t)sz;
    return d;
}

static bool file_contains(const char *path, const char *needle) {
    size_t n = 0;
    uint8_t *d = read_all(path, &n);
    if (!d) return false;
    /* NUL-terminate for strstr */
    uint8_t *z = realloc(d, n + 1);
    if (!z) { free(d); return false; }
    z[n] = 0;
    bool found = strstr((char *)z, needle) != NULL;
    free(z);
    return found;
}

/* ------------------------------------------------------------------------- */

int main(void) {
    const char *mid_path = "chipseq_tools_test.mid";
    const char *c1_path  = "chipseq_tools_out1.c";
    const char *c2_path  = "chipseq_tools_out2.c";
    const char *c3_path  = "chipseq_tools_out3.c";
    char err[192];

    /* -------- write the reference SMF -------- */
    buf smf = {0};
    build_smf(&smf);
    {
        FILE *fp = fopen(mid_path, "wb");
        CHECK(fp != NULL, "open scratch .mid for writing");
        if (fp) {
            CHECK(fwrite(smf.d, 1, smf.n, fp) == smf.n, "write SMF bytes");
            fclose(fp);
        }
    }
    free(smf.d);

    /* -------- error paths -------- */
    err[0] = 0;
    CHECK(chipseq_midi_load("no_such_file_xyz.mid", NULL, err, sizeof err) == NULL,
          "midi_load rejects a missing file");
    CHECK(err[0] != 0, "midi_load writes a reason for a missing file");

    /* build the map */
    static uint8_t prog2inst[128];   /* every GM program -> lead (inst 0) */
    static uint8_t drum2inst[128];   /* every drum note  -> drum (inst 2) */
    for (int i = 0; i < 128; i++) { prog2inst[i] = 0; drum2inst[i] = 2; }
    chipseq_midi_map map = {
        .program_to_instrument = prog2inst,
        .drum_to_instrument = drum2inst,
        .instruments = map_insts,
        .instrument_count = 3,
        .channels = 4,
        .rows_per_beat = 4,
        .ticks_per_row = 6,
        .rows_per_pattern = 16,
        .voice_steal = true,
        .import_pitch_bend = true,
    };

    err[0] = 0;
    CHECK(chipseq_midi_load(mid_path, NULL, err, sizeof err) == NULL, "midi_load rejects NULL map");
    CHECK(err[0] != 0, "midi_load writes a reason for a NULL map");

    /* -------- malformed-file bounds and track-count checks -------- */
    {
        const char *path = "chipseq_tools_bad.mid";
        static const uint8_t bad_hlen[] = {
            'M','T','h','d', 0xFF,0xFF,0xFF,0xFF,
            0,0, 0,1, 0,48,
        };
        CHECK(write_all(path, bad_hlen, sizeof bad_hlen), "write oversized-MThd fixture");
        err[0] = 0;
        CHECK(chipseq_midi_load(path, &map, err, sizeof err) == NULL,
              "midi_load rejects wrapped/oversized MThd length");
        CHECK(err[0] != 0, "oversized-MThd rejection has an error message");

        buf header_only = {0};
        put_smf_header(&header_only, 1);
        CHECK(write_all(path, header_only.d, header_only.n), "write missing-track fixture");
        free(header_only.d);
        err[0] = 0;
        CHECK(chipseq_midi_load(path, &map, err, sizeof err) == NULL,
              "midi_load rejects a declared but missing MTrk");
        CHECK(err[0] != 0, "missing-track rejection has an error message");

        static const uint8_t missing_eot[] = {
            'M','T','h','d', 0,0,0,6, 0,0, 0,1, 0,48,
            'M','T','r','k', 0,0,0,3, 0,0xC0,0,
        };
        static const uint8_t status_as_data[] = {
            'M','T','h','d', 0,0,0,6, 0,0, 0,1, 0,48,
            'M','T','r','k', 0,0,0,8,
            0,0x90,60,0x90, 0,0xFF,0x2F,0,
        };
        static const uint8_t after_eot[] = {
            'M','T','h','d', 0,0,0,6, 0,0, 0,1, 0,48,
            'M','T','r','k', 0,0,0,7,
            0,0xFF,0x2F,0, 0,0xC0,0,
        };
        static const uint8_t zero_tempo[] = {
            'M','T','h','d', 0,0,0,6, 0,0, 0,1, 0,48,
            'M','T','r','k', 0,0,0,11,
            0,0xFF,0x51,3,0,0,0, 0,0xFF,0x2F,0,
        };
        static const uint8_t wrong_tempo_length[] = {
            'M','T','h','d', 0,0,0,6, 0,0, 0,1, 0,48,
            'M','T','r','k', 0,0,0,10,
            0,0xFF,0x51,2,7,0xA1, 0,0xFF,0x2F,0,
        };
        static const uint8_t nonzero_eot_length[] = {
            'M','T','h','d', 0,0,0,6, 0,0, 0,1, 0,48,
            'M','T','r','k', 0,0,0,5,
            0,0xFF,0x2F,1,0,
        };
        static const uint8_t trailing_file_bytes[] = {
            'M','T','h','d', 0,0,0,6, 0,0, 0,1, 0,48,
            'M','T','r','k', 0,0,0,4, 0,0xFF,0x2F,0,
            1,2,3,4,
        };
        static const uint8_t legacy_zero_pad[] = {
            'M','T','h','d', 0,0,0,6, 0,0, 0,1, 0,48,
            'M','T','r','k', 0,0,0,4, 0,0xFF,0x2F,0,
            0,
        };
        struct malformed_case {
            const uint8_t *bytes;
            size_t size;
            const char *message;
        } cases[] = {
            { missing_eot, sizeof missing_eot, "midi_load requires end-of-track" },
            { status_as_data, sizeof status_as_data, "midi_load rejects a status byte as channel data" },
            { after_eot, sizeof after_eot, "midi_load rejects data after end-of-track" },
            { zero_tempo, sizeof zero_tempo, "midi_load rejects a zero tempo" },
            { wrong_tempo_length, sizeof wrong_tempo_length,
              "midi_load rejects a non-three-byte tempo" },
            { nonzero_eot_length, sizeof nonzero_eot_length,
              "midi_load rejects a nonzero end-of-track length" },
            { trailing_file_bytes, sizeof trailing_file_bytes,
              "midi_load rejects bytes after declared tracks" },
        };
        for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
            CHECK(write_all(path, cases[i].bytes, cases[i].size),
                  "write strict-MIDI malformed fixture");
            err[0] = 0;
            chipseq_song *bad = chipseq_midi_load(path, &map, err, sizeof err);
            CHECK(bad == NULL, cases[i].message);
            CHECK(err[0] != 0, "strict-MIDI rejection has an error message");
            chipseq_song_free(bad);
        }
        CHECK(write_all(path, legacy_zero_pad, sizeof legacy_zero_pad),
              "write legacy zero-pad MIDI fixture");
        err[0] = 0;
        chipseq_song *padded = chipseq_midi_load(path, &map, err, sizeof err);
        CHECK(padded != NULL, "midi_load accepts one inert legacy zero pad");
        chipseq_song_free(padded);
        remove(path);
    }

    /* -------- tempo, bend routing, and order-width regressions -------- */
    {
        const char *path = "chipseq_tools_edge.mid";
        buf edge = {0};
        build_delayed_tempo_smf(&edge);
        CHECK(write_all(path, edge.d, edge.n), "write delayed-tempo fixture");
        free(edge.d);
        err[0] = 0;
        chipseq_song *tempo_song = chipseq_midi_load(path, &map, err, sizeof err);
        CHECK(tempo_song != NULL, "load delayed-tempo fixture");
        if (tempo_song) {
            CHECK(tempo_song->bpm_q8 == CHIPSEQ_BPM(120),
                  "tempo before a nonzero-tick event remains MIDI-default 120 BPM");
            bool found_tempo = false;
            for (uint16_t p = 0; p < tempo_song->pattern_count; p++) {
                const chipseq_pattern *pat = &tempo_song->patterns[p];
                for (uint16_t r = 0; r < pat->rows; r++) {
                    for (uint8_t c = 0; c < tempo_song->channels; c++) {
                        const chipseq_cell *cell = &pat->cells[(size_t)r * tempo_song->channels + c];
                        if (cell->fx == CHIPSEQ_FX_TEMPO && cell->fxp == 150)
                            found_tempo = true;
                    }
                }
            }
            CHECK(found_tempo, "nonzero-tick first tempo is emitted as FX_TEMPO");
            chipseq_song_free(tempo_song);
        }

        edge = (buf){0};
        build_bend_routing_smf(&edge);
        CHECK(write_all(path, edge.d, edge.n), "write bend-routing fixture");
        free(edge.d);
        chipseq_midi_map bend_map = map;
        bend_map.channels = 2;
        err[0] = 0;
        chipseq_song *bend_song = chipseq_midi_load(path, &bend_map, err, sizeof err);
        CHECK(bend_song != NULL, "load bend-routing fixture");
        if (bend_song) {
            const chipseq_pattern *pat = &bend_song->patterns[0];
            const chipseq_cell *row2 = &pat->cells[(size_t)2u * bend_song->channels];
            CHECK(row2[0].fx != CHIPSEQ_FX_PORTA_UP && row2[1].fx == CHIPSEQ_FX_PORTA_UP,
                  "pitch bend follows its MIDI channel to chip column 1");
            chipseq_song_free(bend_song);
        }

        edge = (buf){0};
        build_short_note_smf(&edge);
        CHECK(write_all(path, edge.d, edge.n), "write sub-grid short-note fixture");
        free(edge.d);
        chipseq_midi_map short_map = map;
        short_map.channels = 1;
        err[0] = 0;
        chipseq_song *short_song = chipseq_midi_load(path, &short_map, err, sizeof err);
        CHECK(short_song != NULL, "load sub-grid short-note fixture");
        if (short_song) {
            const chipseq_cell *first = &short_song->patterns[0].cells[0];
            CHECK(first->note == 60 && first->fx == CHIPSEQ_FX_NOTE_CUT && first->fxp == 1,
                  "zero-duration quantization becomes a one-fine-tick note, not a sustain");
            chipseq_song_free(short_song);
        }

        edge = (buf){0};
        build_trailing_silence_smf(&edge);
        CHECK(write_all(path, edge.d, edge.n), "write trailing-silence fixture");
        free(edge.d);
        chipseq_midi_map tail_map = map;
        tail_map.channels = 1;
        err[0] = 0;
        chipseq_song *tail_song = chipseq_midi_load(path, &tail_map, err, sizeof err);
        CHECK(tail_song != NULL, "load trailing-silence fixture");
        if (tail_song) {
            CHECK(tail_song->pattern_count == 4u,
                  "end-of-track tail does not add a blank boundary pattern");
            chipseq_song_free(tail_song);
        }

        edge = (buf){0};
        build_too_many_patterns_smf(&edge);
        CHECK(write_all(path, edge.d, edge.n), "write too-many-patterns fixture");
        free(edge.d);
        chipseq_midi_map long_map = map;
        long_map.channels = 1;
        long_map.rows_per_pattern = 1;
        err[0] = 0;
        CHECK(chipseq_midi_load(path, &long_map, err, sizeof err) == NULL,
              "midi_load rejects songs exceeding the uint8 order range");
        CHECK(strstr(err, "patterns") != NULL || strstr(err, "order") != NULL,
              "order-width rejection has a descriptive error");
        remove(path);
    }

    /* -------- load the real file -------- */
    err[0] = 0;
    chipseq_song *song = chipseq_midi_load(mid_path, &map, err, sizeof err);
    CHECK(song != NULL, "midi_load parses + quantizes the reference SMF");
    if (!song) {
        printf("  midi_load err: %s\n", err);
        printf("test_tools: %d/%d checks FAILED\n", g_fails, g_checks);
        remove(mid_path);
        return 1;
    }

    /* the quantized song must validate against the same gate the engine uses */
    char verr[192];
    CHECK(chipseq_song_validate(song, verr, sizeof verr), "imported song validates");
    CHECK(song->channels == 4, "song has the mapped channel count");
    CHECK(song->pattern_count >= 1, "song has at least one pattern");
    CHECK(song->instrument_count == 3, "song carries the map instruments");
    /* base tempo 120 BPM in Q8.8 == 120<<8 */
    CHECK(song->bpm_q8 == (uint16_t)(120u << 8), "base tempo imported exactly (120 BPM, Q8.8)");
    CHECK(song->loop_order == CHIPSEQ_NO_LOOP, "imported song is one-shot");

    /* -------- byte-stable C emission across two runs -------- */
    err[0] = 0;
    CHECK(chipseq_song_write_c(song, c1_path, "song_test", err, sizeof err),
          "write_c run #1 succeeds");
    err[0] = 0;
    CHECK(chipseq_song_write_c(song, c2_path, "song_test", err, sizeof err),
          "write_c run #2 succeeds");

    size_t n1 = 0, n2 = 0;
    uint8_t *e1 = read_all(c1_path, &n1);
    uint8_t *e2 = read_all(c2_path, &n2);
    CHECK(e1 && e2, "read back both emitted C files");
    if (e1 && e2) {
        CHECK(n1 == n2, "two emissions have the same length");
        CHECK(n1 == n2 && memcmp(e1, e2, n1) == 0,
              "chipseq_song_write_c is byte-identical across runs (diff-stable)");
    }
    free(e1);
    free(e2);

    /* the emission is self-contained + names the requested identifier */
    CHECK(file_contains(c1_path, "const chipseq_song song_test ="),
          "emitted C defines the requested song symbol");
    CHECK(file_contains(c1_path, "#include \"chip_sequencer.h\""),
          "emitted C is self-contained (includes the header)");
    CHECK(file_contains(c1_path, "song_test_vol0_v"),
          "emitted C carries the lead volume sequence (stable naming)");

    /* Hex/octal escapes must terminate before a following hex digit. */
    {
        static const char odd_name[] = { 1, 'A', '?', '?', '/', 0 };
        chipseq_song odd = *song;
        odd.name = odd_name;
        err[0] = 0;
        CHECK(chipseq_song_write_c(&odd, c2_path, "odd_song", err, sizeof err),
              "write_c emits a song with non-printable name bytes");
        CHECK(file_contains(c2_path, ".name = \"\\001A\\?\\?/\","),
              "string escapes preserve a following hex digit and avoid C11 trigraphs");
    }

    /* Invalid/keyword identifiers are rejected before an uncompilable file is emitted. */
    err[0] = 0;
    CHECK(!chipseq_song_write_c(song, c2_path, "bad-name", err, sizeof err),
          "write_c rejects punctuation in an identifier");
    CHECK(err[0] != 0, "invalid identifier has an error message");
    err[0] = 0;
    CHECK(!chipseq_song_write_c(song, c2_path, "for", err, sizeof err),
          "write_c rejects a C keyword identifier");

    /* A valid silent song has no instrument table; emit portable ISO C11. */
    {
        static const chipseq_cell empty_cells[] = {
            { CHIPSEQ_NOTE_NONE, 0, CHIPSEQ_VOL_NONE, CHIPSEQ_FX_NONE, 0 },
        };
        static const chipseq_pattern empty_patterns[] = { { empty_cells, 1 } };
        static const uint8_t empty_order[] = { 0 };
        static const chipseq_song empty_song = {
            .name = NULL, .instruments = NULL, .instrument_count = 0,
            .patterns = empty_patterns, .pattern_count = 1,
            .order = empty_order, .order_length = 1,
            .loop_order = CHIPSEQ_NO_LOOP, .channels = 1,
            .rows_per_beat = 4, .ticks_per_row = 1, .bpm_q8 = CHIPSEQ_BPM(120),
        };
        err[0] = 0;
        CHECK(chipseq_song_write_c(&empty_song, c3_path, "empty_song", err, sizeof err),
              "write_c emits a zero-instrument song");
        CHECK(file_contains(c3_path, ".name = NULL,") &&
              file_contains(c3_path, ".instruments = NULL, .instrument_count = 0,"),
              "zero-instrument/NULL-name fields round-trip as portable C");
        CHECK(!file_contains(c3_path, "empty_song_insts[]"),
              "zero-instrument song emits no empty-array extension");
    }

    /* -------- write_c error path -------- */
    err[0] = 0;
    CHECK(!chipseq_song_write_c(NULL, c1_path, "song_test", err, sizeof err),
          "write_c rejects a NULL song");
    CHECK(err[0] != 0, "write_c writes a reason for a NULL song");

    /* -------- free (ASAN/LSAN watches this) -------- */
    chipseq_song_free(song);
    chipseq_song_free(NULL);   /* NULL-safe */

    remove(mid_path);
    if (getenv("CHIPSEQ_KEEP_TEST_OUTPUT") == NULL) {
        remove(c1_path);
        remove(c2_path);
        remove(c3_path);
    }

    if (g_fails) {
        printf("test_tools: %d/%d checks FAILED\n", g_fails, g_checks);
        return 1;
    }
    printf("test_tools: OK (%d checks passed)\n", g_checks);
    return 0;
}
