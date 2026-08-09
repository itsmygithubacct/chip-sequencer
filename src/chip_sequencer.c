/* chip_sequencer.c -- deterministic chiptune synth + pattern sequencer (core).
 *
 * POSIX-free: this uses only the C11 standard library. The offline bounce
 * helpers use malloc/free and standard file I/O; the live render path does not
 * allocate or perform I/O. No <math.h>: the signal path is integer fixed point
 * with zero libm transcendentals, so supported targets produce the same signed
 * sample sequence (see the pinned-contract constants below).
 *
 * The header struct is the fixed public ABI; several private track fields are
 * deliberately reused (documented at each use) because the struct is minimal by
 * design and nothing here allocates per-voice scratch.
 */
#include "chip_sequencer.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ======================================================================== */
/* Pinned sample-contract constants. Changing any of these is a              */
/* deliberate golden rehash, never an accident.                              */
/* ======================================================================== */

/* 12-entry Q16 semitone ratio table: round(2^(n/12) * 65536), n = 0..11. */
static const uint32_t k_semitone_q16[12] = {
    65536u, 69433u, 73562u, 77936u, 82571u, 87480u,
    92682u, 98193u, 104032u, 110218u, 116772u, 123717u
};

/* Reference frequency numerator: A4 = 440 Hz lives at MIDI 69. */
#define K_A4_HZ 440u

/* 15-bit LFSR seed loaded at every note-on (must be nonzero). */
#define K_LFSR_SEED 0x0001u

/* Waveform peak amplitude (pre-volume). Headroom is provided by the mix shift
 * so a single full-volume voice reaches ~1/4 of full scale. */
#define K_WAVE_PEAK 32767

/* 64-entry signed triangle LFO (vibrato / tremolo). Peak +-64, no sin(). */
static const int8_t k_lfo[64] = {
      0,  4,  8, 12, 16, 20, 24, 28, 32, 36, 40, 44, 48, 52, 56, 60,
     64, 60, 56, 52, 48, 44, 40, 36, 32, 28, 24, 20, 16, 12,  8,  4,
      0, -4, -8,-12,-16,-20,-24,-28,-32,-36,-40,-44,-48,-52,-56,-60,
    -64,-60,-56,-52,-48,-44,-40,-36,-32,-28,-24,-20,-16,-12, -8, -4
};

/* 15-tap windowed halfband FIR, Q15, symmetric about index 7. Zero taps at odd
 * offsets from centre; nonzero pairs + centre sum to exactly 32768 (Q15 1.0). */
#define HB_C  16446   /* centre  (index 7)         */
#define HB_K1  9992   /* index 6 & 8  (offset +-1) */
#define HB_K3 (-2242) /* index 4 & 10 (offset +-3) */
#define HB_K5   531   /* index 2 & 12 (offset +-5) */
#define HB_K7 (-120)  /* index 0 & 14 (offset +-7) */

/* NES nonlinear mix tables (Q15), pulse (31-entry) and TND (203-entry).
 * pulse[i]  = round(32767 * 95.52 / (8128/i   + 100)),  i = 1..30
 * tnd[j]    = round(32767 * 163.67/ (24329/j  + 100)),  j = 1..202  */
static const int32_t k_nes_pulse[31] = {
    0, 380, 752, 1114, 1468, 1814, 2152, 2482, 2805, 3120, 3429, 3731, 4026,
    4316, 4599, 4876, 5148, 5414, 5675, 5930, 6181, 6426, 6667, 6903, 7135,
    7362, 7586, 7805, 8020, 8231, 8438
};
static const int32_t k_nes_tnd[203] = {
    0, 220, 437, 653, 867, 1080, 1291, 1500, 1707, 1913, 2117, 2320, 2521,
    2720, 2918, 3115, 3309, 3503, 3694, 3885, 4074, 4261, 4447, 4632, 4815,
    4997, 5178, 5357, 5535, 5712, 5887, 6061, 6234, 6406, 6576, 6745, 6913,
    7079, 7245, 7409, 7572, 7734, 7895, 8055, 8214, 8371, 8528, 8683, 8837,
    8991, 9143, 9294, 9444, 9593, 9741, 9888, 10035, 10180, 10324, 10467,
    10610, 10751, 10891, 11031, 11170, 11307, 11444, 11580, 11715, 11849,
    11983, 12115, 12247, 12378, 12508, 12637, 12765, 12893, 13020, 13146,
    13271, 13395, 13519, 13642, 13764, 13886, 14006, 14126, 14246, 14364,
    14482, 14599, 14715, 14831, 14946, 15061, 15174, 15287, 15400, 15511,
    15622, 15733, 15842, 15952, 16060, 16168, 16275, 16382, 16488, 16593,
    16698, 16802, 16906, 17009, 17112, 17213, 17315, 17416, 17516, 17616,
    17715, 17813, 17911, 18009, 18106, 18202, 18298, 18394, 18489, 18583,
    18677, 18770, 18863, 18955, 19047, 19139, 19230, 19320, 19410, 19500,
    19589, 19677, 19765, 19853, 19940, 20027, 20113, 20199, 20285, 20370,
    20454, 20538, 20622, 20705, 20788, 20871, 20953, 21034, 21116, 21196,
    21277, 21357, 21437, 21516, 21595, 21673, 21751, 21829, 21906, 21983,
    22060, 22136, 22212, 22287, 22362, 22437, 22511, 22586, 22659, 22733,
    22806, 22878, 22950, 23022, 23094, 23165, 23236, 23307, 23377, 23447,
    23517, 23586, 23655, 23724, 23792, 23860, 23928, 23996, 24063, 24130,
    24196, 24262, 24328
};

/* Classic NES 16-entry noise-note map (descending pitch, period 0..15). */
static const uint8_t k_noise_note[16] = {
    96, 84, 72, 60, 48, 41, 36, 32, 28, 24, 17, 12, 5, 0, 0, 0
};

/* Command op-codes (private). */
enum {
    OP_MUSIC_PLAY = 1, OP_MUSIC_STOP, OP_MUSIC_VOL, OP_MUSIC_PAUSE,
    OP_SFX_PLAY, OP_SFX_SET, OP_SFX_STOP, OP_SFX_STOP_ALL
};

/* SFX claim-word states (2 bits). */
enum { ST_FREE = 0, ST_CLAIMED = 1, ST_PLAYING = 2, ST_LOOPING = 3 };

#define CLAIM_STATE_SHIFT 30u
#define CLAIM_GEN_MASK    0x1FFFFFFFu

/* Sequence index mapping in voice->seq_pos[]. */
enum { SEQ_VOL = 0, SEQ_ARP = 1, SEQ_PITCH = 2, SEQ_DUTY = 3 };

/* ======================================================================== */
/* small integer helpers                                                     */
/* ======================================================================== */

static int32_t clampi(int32_t v, int32_t lo, int32_t hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int16_t clamp16(int32_t v) {
    if (v > 32767) return (int16_t)32767;
    if (v < -32768) return (int16_t)-32768;
    return (int16_t)v;
}

/* ISO C leaves right-shifting a negative signed value implementation-defined.
 * The sample contract uses the arithmetic-shift result (division by 2^n rounded
 * toward negative infinity), so express that operation with defined arithmetic. */
static int64_t floor_shift_i64(int64_t value, unsigned shift) {
    uint64_t divisor = (uint64_t)1u << shift;
    if (value >= 0) return value / (int64_t)divisor;
    uint64_t magnitude = (uint64_t)(-(value + 1)) + 1u;
    uint64_t quotient = (magnitude + divisor - 1u) / divisor;
    return -(int64_t)quotient;
}

static int32_t saturating_add_i32(int32_t value, int32_t delta) {
    int64_t sum = (int64_t)value + delta;
    if (sum > INT32_MAX) return INT32_MAX;
    if (sum < INT32_MIN) return INT32_MIN;
    return (int32_t)sum;
}

#if defined(__GNUC__) || defined(__clang__)
static void set_err(char *err, size_t err_len, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
#endif
static void set_err(char *err, size_t err_len, const char *fmt, ...) {
    if (!err || err_len == 0) return;
    va_list ap;
    va_start(ap, fmt);
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wformat-nonliteral"
#endif
    (void)vsnprintf(err, err_len, fmt, ap);
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
    va_end(ap);
}

/* Q32 oversampled-samples-per-tick, drift-free remainder-accumulator clock. */
static uint64_t calc_spt(uint32_t rate, uint16_t bpm_q8, uint8_t rpb, uint8_t tpr) {
    if (bpm_q8 < CHIPSEQ_BPM_MIN) bpm_q8 = CHIPSEQ_BPM_MIN;
    if (rpb == 0) rpb = 1;
    if (tpr == 0) tpr = 1;
    uint64_t n = (uint64_t)rate * 256u * 60u;
    uint64_t d = (uint64_t)bpm_q8 * rpb * tpr;
    if (d == 0) d = 1;
    uint64_t q = n / d;
    uint64_t r = n % d;
    uint64_t spt = (q << 32) + ((r << 32) / d);
    if (spt == 0) spt = 1;
    return spt;
}

/* Phase increment (Q32 per oversampled sample) for a fractional pitch given in
 * 1/16-semitone units; linear interpolation between adjacent note_inc entries. */
static uint32_t inc_from_pitch(const chipseq *seq, int32_t p16) {
    p16 = clampi(p16, 0, 127 * 16);
    int32_t n = p16 >> 4;
    int32_t f = p16 & 15;
    int32_t a = seq->note_inc[n];
    int32_t b = seq->note_inc[n < 127 ? n + 1 : 127];
    int32_t inc = a + (int32_t)(((int64_t)(b - a) * f) >> 4);
    if (inc < 0) inc = 0;
    return (uint32_t)inc;
}

/* PCM Q16 frames-per-oversampled-sample step for a given pitch vs. root. */
static uint32_t pcm_step(const chipseq *seq, int32_t p16, uint8_t root) {
    uint32_t inc = inc_from_pitch(seq, p16);
    int32_t rootinc = seq->note_inc[root];
    if (rootinc <= 0) return 0;
    uint64_t ratio_q16 = ((uint64_t)inc << 16) / (uint32_t)rootinc;
    return (uint32_t)(ratio_q16 / seq->oversample);
}

static bool track_is_music(const chipseq *seq, const chipseq_track *t) {
    return t == &seq->music;
}

/* Transpose applied at synth time (SFX only; live-adjustable). */
static int32_t track_transpose(const chipseq *seq, const chipseq_track *t) {
    if (track_is_music(seq, t)) return 0;
    return (int32_t)t->fade_left - 128;   /* fade_left reused as biased transpose */
}

static bool any_sfx_active(const chipseq *seq) {
    for (unsigned i = 0; i < CHIPSEQ_SFX_SLOTS; i++)
        if (seq->sfx[i].active) return true;
    return false;
}

/* Track output gain in 1/256 (0..256). */
static uint32_t track_gain_q8(const chipseq *seq, const chipseq_track *t) {
    if (track_is_music(seq, t)) {
        uint32_t g = t->generation;                 /* generation reused as music vol_q8 */
        uint32_t duck = any_sfx_active(seq) ? seq->duck_q8 : 256u;
        g = (g * duck) >> 8;
        if (t->fade_ticks > 0) {                    /* fading out */
            uint32_t fg = (uint32_t)t->fade_left * 256u / t->fade_ticks;
            g = (g * fg) >> 8;
        }
        return g;
    }
    return t->fade_ticks;                            /* fade_ticks reused as sfx vol_q8 */
}

/* ======================================================================== */
/* SPSC command ring                                                         */
/* ======================================================================== */

static bool queue_space(const chipseq *seq) {
    uint32_t head = atomic_load_explicit(&seq->queue_head, memory_order_relaxed);
    uint32_t tail = atomic_load_explicit(&seq->queue_tail, memory_order_acquire);
    return (head - tail) < CHIPSEQ_CMD_QUEUE;
}

static bool queue_push(chipseq *seq, const chipseq_cmd *cmd) {
    uint32_t head = atomic_load_explicit(&seq->queue_head, memory_order_relaxed);
    uint32_t tail = atomic_load_explicit(&seq->queue_tail, memory_order_acquire);
    if ((head - tail) >= CHIPSEQ_CMD_QUEUE) return false;
    seq->queue[head % CHIPSEQ_CMD_QUEUE] = *cmd;
    atomic_store_explicit(&seq->queue_head, head + 1u, memory_order_release);
    return true;
}

/* ======================================================================== */
/* SFX claim words                                                           */
/* ======================================================================== */

static unsigned claim_pack(unsigned state, uint32_t gen) {
    return ((state & 3u) << CLAIM_STATE_SHIFT) | (gen & CLAIM_GEN_MASK);
}
static unsigned claim_state(unsigned w) { return w >> CLAIM_STATE_SHIFT; }
static uint32_t claim_gen(unsigned w)    { return (uint32_t)(w & CLAIM_GEN_MASK); }

static int handle_encode(unsigned slot, uint32_t gen) {
    return (int)((gen << 2) | (slot & 3u));
}

/* Game thread: claim a FREE slot, else steal the oldest PLAYING one. Returns a
 * handle > 0 or -1. Single CAS per ownership change; never reads sfx[] fields. */
static int sfx_try_claim(chipseq *seq) {
    for (int attempt = 0; attempt < 128; attempt++) {
        int free_slot = -1;
        for (unsigned i = 0; i < CHIPSEQ_SFX_SLOTS; i++) {
            unsigned w = atomic_load_explicit(&seq->sfx_claim[i], memory_order_acquire);
            if (claim_state(w) == ST_FREE && claim_gen(w) < CLAIM_GEN_MASK) {
                free_slot = (int)i;
                break;
            }
        }
        if (free_slot >= 0) {
            unsigned i = (unsigned)free_slot;
            unsigned w = atomic_load_explicit(&seq->sfx_claim[i], memory_order_acquire);
            if (claim_state(w) != ST_FREE || claim_gen(w) >= CLAIM_GEN_MASK) continue;
            uint32_t gen = claim_gen(w) + 1u;
            uint64_t ord = seq->sfx_claim_next++;
            unsigned nw = claim_pack(ST_CLAIMED, gen);
            if (atomic_compare_exchange_strong_explicit(&seq->sfx_claim[i], &w, nw,
                    memory_order_acq_rel, memory_order_acquire)) {
                seq->sfx_claim_ordinal[i] = ord;
                return handle_encode(i, gen);
            }
            continue;   /* lost race, rescan */
        }
        /* no free slot: steal the oldest PLAYING (never LOOPING/CLAIMED) */
        int victim = -1;
        uint64_t best_ord = UINT64_MAX;
        unsigned victim_w = 0;
        for (unsigned i = 0; i < CHIPSEQ_SFX_SLOTS; i++) {
            unsigned w = atomic_load_explicit(&seq->sfx_claim[i], memory_order_acquire);
            if (claim_state(w) == ST_PLAYING && claim_gen(w) < CLAIM_GEN_MASK) {
                uint64_t o = seq->sfx_claim_ordinal[i];
                if (o < best_ord) { best_ord = o; victim = (int)i; victim_w = w; }
            }
        }
        if (victim < 0) return -1;   /* all looping/claimed, nothing stealable */
        unsigned vi = (unsigned)victim;
        uint32_t gen = claim_gen(victim_w) + 1u;
        uint64_t ord = seq->sfx_claim_next++;
        unsigned nw = claim_pack(ST_CLAIMED, gen);
        if (atomic_compare_exchange_strong_explicit(&seq->sfx_claim[vi], &victim_w, nw,
                memory_order_acq_rel, memory_order_acquire)) {
            seq->sfx_claim_ordinal[vi] = ord;
            return handle_encode(vi, gen);
        }
        /* lost race, retry */
    }
    return -1;
}

/* Render thread: publish a stopped slot back to FREE (no-op if stolen). */
static void sfx_release(chipseq *seq, unsigned slot) {
    unsigned w = atomic_load_explicit(&seq->sfx_claim[slot], memory_order_acquire);
    unsigned st = claim_state(w);
    if (st == ST_PLAYING || st == ST_LOOPING) {
        unsigned nw = claim_pack(ST_FREE, claim_gen(w));
        (void)atomic_compare_exchange_strong_explicit(&seq->sfx_claim[slot], &w, nw,
                memory_order_acq_rel, memory_order_acquire);
    }
}

/* ======================================================================== */
/* validation                                                                */
/* ======================================================================== */

static bool validate_seq(const chipseq_seq *s, const char *kind, uint16_t inst_i,
                         int value_min, int value_max, char *err, size_t err_len) {
    if (!s) return true;
    if (s->values == NULL) {
        set_err(err, err_len, "instrument %u: %s values NULL", inst_i, kind);
        return false;
    }
    if (s->length < 1 || s->length > CHIPSEQ_SEQ_LEN_MAX) {
        set_err(err, err_len, "instrument %u: %s length %u out of 1..%u",
                inst_i, kind, s->length, CHIPSEQ_SEQ_LEN_MAX);
        return false;
    }
    if (s->loop != CHIPSEQ_SEQ_NO_LOOP && (s->loop < 0 || s->loop >= (int16_t)s->length)) {
        set_err(err, err_len, "instrument %u: %s loop %d out of range", inst_i, kind, s->loop);
        return false;
    }
    if (s->release != CHIPSEQ_SEQ_NO_RELEASE &&
        (s->release < 0 || s->release >= (int16_t)s->length)) {
        set_err(err, err_len, "instrument %u: %s release %d out of range",
                inst_i, kind, s->release);
        return false;
    }
    if (value_min <= value_max) {
        for (uint16_t i = 0; i < s->length; i++) {
            int value = s->values[i];
            if (value < value_min || value > value_max) {
                set_err(err, err_len,
                        "instrument %u: %s value[%u]=%d out of %d..%d",
                        inst_i, kind, i, value, value_min, value_max);
                return false;
            }
        }
    }
    return true;
}

bool chipseq_song_validate(const chipseq_song *song, char *err, size_t err_len) {
    if (!song) { set_err(err, err_len, "song is NULL"); return false; }
    const char *nm = song->name ? song->name : "?";

    if (song->channels < 1 || song->channels > CHIPSEQ_CHANNELS_MAX) {
        set_err(err, err_len, "song '%s': channels %u out of 1..%u",
                nm, song->channels, CHIPSEQ_CHANNELS_MAX);
        return false;
    }
    if (song->rows_per_beat < 1) {
        set_err(err, err_len, "song '%s': rows_per_beat must be >= 1", nm);
        return false;
    }
    if (song->ticks_per_row < 1) {
        set_err(err, err_len, "song '%s': ticks_per_row must be >= 1", nm);
        return false;
    }
    if (song->bpm_q8 < CHIPSEQ_BPM_MIN) {
        set_err(err, err_len, "song '%s': bpm_q8 %u below minimum %u",
                nm, song->bpm_q8, (unsigned)CHIPSEQ_BPM_MIN);
        return false;
    }
    if (song->order_length < 1 || song->order == NULL) {
        set_err(err, err_len, "song '%s': order_length must be >= 1 with non-NULL order", nm);
        return false;
    }
    if (song->pattern_count < 1 || song->patterns == NULL) {
        set_err(err, err_len, "song '%s': pattern_count must be >= 1", nm);
        return false;
    }
    if (song->instrument_count > 0 && song->instruments == NULL) {
        set_err(err, err_len, "song '%s': instrument_count is %u but instruments is NULL",
                nm, song->instrument_count);
        return false;
    }
    if (song->loop_order != CHIPSEQ_NO_LOOP && song->loop_order >= song->order_length) {
        set_err(err, err_len, "song '%s': loop_order %u >= order_length %u",
                nm, song->loop_order, song->order_length);
        return false;
    }
    for (uint16_t i = 0; i < song->order_length; i++) {
        if (song->order[i] >= song->pattern_count) {
            set_err(err, err_len, "song '%s': order[%u]=%u exceeds pattern_count %u",
                    nm, i, song->order[i], song->pattern_count);
            return false;
        }
    }
    for (uint16_t p = 0; p < song->pattern_count; p++) {
        const chipseq_pattern *pat = &song->patterns[p];
        if (pat->rows < 1 || pat->cells == NULL) {
            set_err(err, err_len, "song '%s': pattern %u has zero rows or NULL cells", nm, p);
            return false;
        }
        for (uint16_t r = 0; r < pat->rows; r++) {
            for (uint8_t c = 0; c < song->channels; c++) {
                const chipseq_cell *cell = &pat->cells[(size_t)r * song->channels + c];
                bool real_note = cell->note <= 127;
                if (real_note && cell->inst >= song->instrument_count) {
                    set_err(err, err_len,
                            "song '%s': pattern %u row %u chan %u inst %u >= %u",
                            nm, p, r, c, cell->inst, song->instrument_count);
                    return false;
                }
                if (!real_note && cell->note != CHIPSEQ_NOTE_NONE &&
                    cell->note != CHIPSEQ_NOTE_OFF && cell->note != CHIPSEQ_NOTE_CUT) {
                    set_err(err, err_len,
                            "song '%s': pattern %u row %u chan %u bad note 0x%02X",
                            nm, p, r, c, cell->note);
                    return false;
                }
                if (cell->fx != CHIPSEQ_FX_NONE) {
                    if (cell->fx >= CHIPSEQ_FX_COUNT) {
                        set_err(err, err_len,
                                "song '%s': pattern %u row %u chan %u bad fx %u",
                                nm, p, r, c, cell->fx);
                        return false;
                    }
                    if (cell->fx == CHIPSEQ_FX_DUTY && cell->fxp > 63u) {
                        set_err(err, err_len,
                                "song '%s': pattern %u row %u chan %u duty %u > 63",
                                nm, p, r, c, cell->fxp);
                        return false;
                    }
                    if (cell->fx == CHIPSEQ_FX_ORDER_JUMP &&
                        cell->fxp >= song->order_length) {
                        set_err(err, err_len,
                                "song '%s': pattern %u row %u chan %u order jump %u >= %u",
                                nm, p, r, c, cell->fxp, song->order_length);
                        return false;
                    }
                }
                if (cell->vol != CHIPSEQ_VOL_NONE && cell->vol > 64) {
                    set_err(err, err_len,
                            "song '%s': pattern %u row %u chan %u volume %u > 64",
                            nm, p, r, c, cell->vol);
                    return false;
                }
            }
        }
    }
    for (uint16_t i = 0; i < song->instrument_count; i++) {
        const chipseq_instrument *in = &song->instruments[i];
        if (in->wave >= CHIPSEQ_WAVE_COUNT) {
            set_err(err, err_len, "song '%s': instrument %u wave %u invalid", nm, i, in->wave);
            return false;
        }
        if ((in->wave == CHIPSEQ_WAVE_PULSE || in->wave == CHIPSEQ_WAVE_SAW) &&
            in->duty > 63u) {
            set_err(err, err_len, "song '%s': instrument %u duty %u > 63",
                    nm, i, in->duty);
            return false;
        }
        if (in->wave == CHIPSEQ_WAVE_NOISE && in->noise_mode > CHIPSEQ_NOISE_SHORT) {
            set_err(err, err_len, "song '%s': instrument %u noise_mode %u invalid",
                    nm, i, in->noise_mode);
            return false;
        }
        if (in->wave == CHIPSEQ_WAVE_TRIANGLE && in->tri_steps > 0u && in->tri_steps < 4u) {
            set_err(err, err_len,
                    "song '%s': instrument %u tri_steps %u must be 0 or >= 4",
                    nm, i, in->tri_steps);
            return false;
        }
        if (!validate_seq(in->vol_seq, "vol_seq", i, 0, 64, err, err_len)) return false;
        if (!validate_seq(in->arp_seq, "arp_seq", i, 1, 0, err, err_len)) return false;
        if (!validate_seq(in->pitch_seq, "pitch_seq", i, 1, 0, err, err_len)) return false;
        if (!validate_seq(in->duty_seq, "duty_seq", i, 1, 63, err, err_len)) return false;
        if (in->wave == CHIPSEQ_WAVE_WAVETABLE && in->wavetable == NULL) {
            set_err(err, err_len, "song '%s': instrument %u wavetable NULL", nm, i);
            return false;
        }
        if (in->wave == CHIPSEQ_WAVE_WAVETABLE) {
            for (unsigned k = 0; k < CHIPSEQ_WAVETABLE_LEN; k++) {
                if (in->wavetable[k] > 15u) {
                    set_err(err, err_len,
                            "song '%s': instrument %u wavetable[%u]=%u > 15",
                            nm, i, k, in->wavetable[k]);
                    return false;
                }
            }
        }
        if (in->wave == CHIPSEQ_WAVE_PCM) {
            const chipseq_pcm *pc = in->pcm;
            if (!pc || pc->frames == NULL || pc->frame_count < 1) {
                set_err(err, err_len, "song '%s': instrument %u PCM missing frames", nm, i);
                return false;
            }
            if (pc->loop_start > pc->loop_end || pc->loop_end > pc->frame_count) {
                set_err(err, err_len,
                        "song '%s': instrument %u PCM loop %u..%u exceeds %u frames",
                        nm, i, pc->loop_start, pc->loop_end, pc->frame_count);
                return false;
            }
            if (pc->root_note > 127) {
                set_err(err, err_len, "song '%s': instrument %u PCM root_note %u > 127",
                        nm, i, pc->root_note);
                return false;
            }
        }
    }
    return true;
}

/* ======================================================================== */
/* pure transport (position + timing); shared by renderer and song_frames    */
/* ======================================================================== */

static void apply_speed_tempo(const chipseq_song *song, chipseq_track *t, uint32_t rate) {
    const chipseq_pattern *pat = &song->patterns[song->order[t->order_pos]];
    for (uint8_t c = 0; c < t->channels; c++) {
        const chipseq_cell *cell = &pat->cells[(size_t)t->row * t->channels + c];
        if (cell->fx == CHIPSEQ_FX_SPEED && cell->fxp >= 1) {
            t->ticks_per_row = cell->fxp;
        } else if (cell->fx == CHIPSEQ_FX_TEMPO && cell->fxp >= 1) {
            uint16_t b = (uint16_t)((uint16_t)cell->fxp << 8);
            if (b < CHIPSEQ_BPM_MIN) b = CHIPSEQ_BPM_MIN;
            t->bpm_q8 = b;
        }
    }
    t->samples_per_tick = calc_spt(rate, t->bpm_q8, song->rows_per_beat, t->ticks_per_row);
}

/* Advance one tick. Returns false when a non-looping song has ended. */
static bool transport_step_tick(const chipseq_song *song, chipseq_track *t, uint32_t rate) {
    if ((uint16_t)(t->tick + 1) < t->ticks_per_row) {
        t->tick = (uint8_t)(t->tick + 1);
        return true;
    }
    t->tick = 0;

    const chipseq_pattern *pat = &song->patterns[song->order[t->order_pos]];
    int32_t jump_order = -1, break_row = 0;
    for (uint8_t c = 0; c < t->channels; c++) {
        const chipseq_cell *cell = &pat->cells[(size_t)t->row * t->channels + c];
        if (cell->fx == CHIPSEQ_FX_ORDER_JUMP) {
            jump_order = cell->fxp; break_row = 0;
        } else if (cell->fx == CHIPSEQ_FX_PATTERN_BREAK) {
            jump_order = (int32_t)t->order_pos + 1; break_row = cell->fxp;
        }
    }
    if (jump_order >= 0) {
        t->order_pos = (uint16_t)jump_order;
        t->row = (uint16_t)break_row;
    } else {
        t->row = (uint16_t)(t->row + 1);
        if (t->row >= pat->rows) { t->row = 0; t->order_pos = (uint16_t)(t->order_pos + 1); }
    }

    if (t->order_pos >= song->order_length) {
        if (t->looping) {
            t->order_pos = (song->loop_order == CHIPSEQ_NO_LOOP) ? 0 : song->loop_order;
            t->row = 0;
        } else {
            t->active = 0;
            return false;
        }
    }
    if (t->order_pos >= song->order_length) { t->active = 0; return false; }

    const chipseq_pattern *np = &song->patterns[song->order[t->order_pos]];
    if (t->row >= np->rows) t->row = 0;

    apply_speed_tempo(song, t, rate);
    return true;
}

/* ======================================================================== */
/* song frame count                                                          */
/* ======================================================================== */

uint64_t chipseq_song_frames(const chipseq_song *song, uint32_t sr) {
    if (!chipseq_song_validate(song, NULL, 0)) return 0;
    if (sr < 8000u || sr > 192000u) return 0;
    if (song->loop_order != CHIPSEQ_NO_LOOP) return UINT64_MAX;

    /* Frame count is oversample-independent, so simulate at the output rate. */
    chipseq_track t;
    memset(&t, 0, sizeof t);
    t.song = song;
    t.order_pos = 0; t.row = 0; t.tick = 0;
    t.ticks_per_row = song->ticks_per_row;
    t.bpm_q8 = song->bpm_q8;
    t.channels = song->channels;
    t.active = 1; t.looping = 0;
    apply_speed_tempo(song, &t, sr);

    /* Floyd cycle detection covers effect-driven loops (for example an
     * FX_ORDER_JUMP back to the same row) even when loop_order says NO_LOOP. */
    chipseq_track slow = t, fast = t;
    bool slow_live = true, fast_live = true;

    /* Accumulate whole and fractional frames separately. A single Q32 uint64_t
     * would overflow after roughly 2^32 output frames even though the public
     * return type can represent much longer finite songs. */
    uint64_t whole_frames = 0;
    uint32_t frac_frames = 0;

    for (;;) {
        uint64_t add_whole = t.samples_per_tick >> 32;
        uint64_t frac_sum = (uint64_t)frac_frames
                          + (uint32_t)t.samples_per_tick;
        uint64_t carry = frac_sum >> 32;
        if (whole_frames > UINT64_MAX - add_whole ||
            whole_frames + add_whole > UINT64_MAX - carry)
            return UINT64_MAX;
        whole_frames += add_whole + carry;
        frac_frames = (uint32_t)frac_sum;

        if (!transport_step_tick(song, &t, sr)) break;

        if (slow_live) slow_live = transport_step_tick(song, &slow, sr);
        if (fast_live) {
            fast_live = transport_step_tick(song, &fast, sr);
            if (fast_live) fast_live = transport_step_tick(song, &fast, sr);
        }
        if (slow_live && fast_live &&
            slow.order_pos == fast.order_pos && slow.row == fast.row &&
            slow.tick == fast.tick && slow.ticks_per_row == fast.ticks_per_row &&
            slow.bpm_q8 == fast.bpm_q8 &&
            slow.samples_per_tick == fast.samples_per_tick)
            return UINT64_MAX;
    }

    if (frac_frames != 0) {
        if (whole_frames == UINT64_MAX) return UINT64_MAX;
        whole_frames++;
    }
    return whole_frames;
}

/* ======================================================================== */
/* voice trigger + per-tick engine (render side)                             */
/* ======================================================================== */

static unsigned voice_note_off(chipseq_voice *v) {
    const chipseq_instrument *in = v->inst;
    if (!in) return 0;
    v->released = 1;
    unsigned release_mask = 0;
    const chipseq_seq *seqs[4] = { in->vol_seq, in->arp_seq, in->pitch_seq, in->duty_seq };
    for (int i = 0; i < 4; i++) {
        if (seqs[i] && seqs[i]->release != CHIPSEQ_SEQ_NO_RELEASE) {
            v->seq_pos[i] = (uint16_t)seqs[i]->release;
            release_mask |= 1u << (unsigned)i;
        }
    }
    return release_mask;
}

static void voice_trigger(const chipseq_song *song, chipseq_voice *v,
                          const chipseq_cell *cell, bool tone_porta) {
    const chipseq_instrument *in = &song->instruments[cell->inst];
    v->inst = in;
    v->note = cell->note;
    if (cell->vol != CHIPSEQ_VOL_NONE) v->vol = cell->vol;
    if (tone_porta) return;   /* glide: keep phase / pitch / sequences */
    v->phase = 0;
    v->pcm_pos = 0;
    v->lfsr = K_LFSR_SEED;
    v->seq_pos[0] = v->seq_pos[1] = v->seq_pos[2] = v->seq_pos[3] = 0;
    v->note_ticks = 0;
    v->cur_pitch = ((int32_t)cell->note + in->transpose) * 16 + in->finetune;
    v->released = 0;
    v->active = 1;
    if (in->wave == CHIPSEQ_WAVE_PULSE)
        v->duty = (uint8_t)(in->duty ? in->duty : CHIPSEQ_DUTY_50);
    else
        v->duty = in->duty;
}

static void advance_sequence(const chipseq_seq *s, uint16_t *pos) {
    if (!s) return;
    uint32_t p = (uint32_t)(*pos) + 1u;
    if (p >= s->length) {
        p = (s->loop != CHIPSEQ_SEQ_NO_LOOP) ? (uint32_t)s->loop : (uint32_t)(s->length - 1u);
    }
    *pos = (uint16_t)p;
}

/* Recompute voice->phase_inc, voice->duty and the effective gain for one
 * channel from its current tick state (sequences + running effects). */
static void refresh_voice(chipseq *seq, chipseq_track *t, uint8_t ch, int32_t *vgain) {
    unsigned vi = (unsigned)t->first_voice + ch;
    chipseq_voice *v = &seq->voices[vi];
    if (!v->active || !v->inst) { vgain[vi] = 0; return; }
    const chipseq_instrument *in = v->inst;
    const chipseq_pattern *pat = &t->song->patterns[t->song->order[t->order_pos]];
    const chipseq_cell *cell = &pat->cells[(size_t)t->row * t->channels + ch];
    uint32_t abs_tick = (uint32_t)t->row * t->ticks_per_row + (uint32_t)t->tick;

    /* --- effective volume --- */
    int32_t vol = v->vol;
    if (in->vol_seq) {
        int32_t sv = clampi(in->vol_seq->values[v->seq_pos[SEQ_VOL]], 0, 64);
        vol = (vol * sv) >> 6;
    }
    if (cell->fx == CHIPSEQ_FX_TREMOLO) {
        uint32_t speed = (uint32_t)(cell->fxp >> 4);
        int32_t depth = cell->fxp & 15;
        uint32_t idx = (abs_tick * speed) & 63u;
        vol += (int32_t)floor_shift_i64((int64_t)k_lfo[idx] * depth, 5);
    }
    vol = clampi(vol, 0, 64);
    uint32_t tg = track_gain_q8(seq, t);
    vgain[vi] = (int32_t)((uint32_t)vol * tg);   /* 0..64*256 */

    /* --- effective pitch (1/16-semitone) --- */
    /* cur_pitch deliberately saturates at the int32 limits under repeated
     * portamento. Accumulate transient modulation in int64 so an arp, SFX
     * transpose, or pitch sequence cannot overflow before the audible clamp. */
    int64_t p = v->cur_pitch;
    p += (int64_t)track_transpose(seq, t) * 16;
    if (in->arp_seq)
        p += (int64_t)in->arp_seq->values[v->seq_pos[SEQ_ARP]] * 16;
    if (cell->fx == CHIPSEQ_FX_ARPEGGIO) {
        unsigned a = (unsigned)t->tick % 3u;
        int32_t off = (a == 0) ? 0 : (a == 1) ? (cell->fxp >> 4) : (cell->fxp & 15);
        p += off * 16;
    }
    if (in->pitch_seq)
        p += in->pitch_seq->values[v->seq_pos[SEQ_PITCH]];
    if (cell->fx == CHIPSEQ_FX_VIBRATO) {
        uint32_t speed = (uint32_t)(cell->fxp >> 4);
        int32_t depth = cell->fxp & 15;
        uint32_t idx = (abs_tick * speed) & 63u;
        p += (int32_t)k_lfo[idx] * depth;
    }
    if (in->vib_speed && in->vib_depth && v->note_ticks >= in->vib_delay) {
        uint32_t age = v->note_ticks - in->vib_delay;
        uint32_t idx = (age * (uint32_t)in->vib_speed) & 63u;
        p += (int32_t)floor_shift_i64(
                (int64_t)k_lfo[idx] * (int32_t)in->vib_depth, 2);
    }

    /* --- duty from duty_seq --- */
    if (in->duty_seq)
        v->duty = (uint8_t)clampi(in->duty_seq->values[v->seq_pos[SEQ_DUTY]], 1, 63);

    int32_t audible_pitch;
    if (p < 0)
        audible_pitch = 0;
    else if (p > 127 * 16)
        audible_pitch = 127 * 16;
    else
        audible_pitch = (int32_t)p;
    if (in->wave == CHIPSEQ_WAVE_PCM && in->pcm)
        v->phase_inc = pcm_step(seq, audible_pitch, in->pcm->root_note);
    else
        v->phase_inc = inc_from_pitch(seq, audible_pitch);
}

static void refresh_track(chipseq *seq, chipseq_track *t, int32_t *vgain) {
    if (!t->active) return;
    for (uint8_t c = 0; c < t->channels; c++) refresh_voice(seq, t, c, vgain);
}

static void refresh_all(chipseq *seq, int32_t *vgain) {
    refresh_track(seq, &seq->music, vgain);
    for (unsigned i = 0; i < CHIPSEQ_SFX_SLOTS; i++) refresh_track(seq, &seq->sfx[i], vgain);
}

/* Stop every oscillator owned by a track and invalidate its cached gain. This
 * must happen at the exact sample where the track ends: vgain lives for a whole
 * render call, so merely clearing t->active would otherwise leave the final
 * note audible until the caller's next block boundary. */
static void silence_track_voices(chipseq *seq, const chipseq_track *t,
                                 int32_t *vgain) {
    for (uint8_t c = 0; c < t->channels; c++) {
        unsigned vi = (unsigned)t->first_voice + c;
        seq->voices[vi].active = 0;
        seq->voices[vi].inst = NULL;
        vgain[vi] = 0;
    }
}

/* Process one sequencer tick for a track (note triggers, sequence advance,
 * running effects), then refresh the track's voices. */
static void render_process_tick(chipseq *seq, chipseq_track *t, int32_t *vgain) {
    const chipseq_song *song = t->song;
    uint8_t tick = t->tick;
    const chipseq_pattern *pat = &song->patterns[song->order[t->order_pos]];
    bool is_music = track_is_music(seq, t);

    if (is_music && t->fade_ticks > 0) {   /* fade-out clock */
        if (t->fade_left > 0) t->fade_left = (uint16_t)(t->fade_left - 1);
        if (t->fade_left == 0) {
            t->active = 0;
            silence_track_voices(seq, t, vgain);
            return;
        }
    }

    for (uint8_t c = 0; c < t->channels; c++) {
        unsigned vi = (unsigned)t->first_voice + c;
        chipseq_voice *v = &seq->voices[vi];
        const chipseq_cell *cell = &pat->cells[(size_t)t->row * t->channels + c];
        bool trig = false;
        unsigned release_mask = 0;

        if (tick == 0) {
            if (cell->note == CHIPSEQ_NOTE_NONE) {
                if (cell->vol != CHIPSEQ_VOL_NONE) v->vol = cell->vol;
            } else if (cell->note == CHIPSEQ_NOTE_OFF) {
                release_mask = voice_note_off(v);
            } else if (cell->note == CHIPSEQ_NOTE_CUT) {
                v->active = 0;
            } else {   /* real note 0..127 */
                if (cell->fx == CHIPSEQ_FX_NOTE_DELAY) {
                    /* deferred; triggered below at tick == fxp */
                } else if (cell->fx == CHIPSEQ_FX_TONE_PORTA && v->active) {
                    voice_trigger(song, v, cell, true);   /* glide, keep phase/seq */
                } else {
                    voice_trigger(song, v, cell, false);
                    trig = true;
                }
            }
            if (cell->fx == CHIPSEQ_FX_DUTY && cell->fxp >= 1)
                v->duty = (uint8_t)clampi(cell->fxp, 1, 63);
            if (cell->fx == CHIPSEQ_FX_PAN)
                v->pan = cell->fxp;
        }

        if (cell->fx == CHIPSEQ_FX_NOTE_DELAY && tick == cell->fxp && cell->note <= 127) {
            voice_trigger(song, v, cell, false);
            trig = true;
        }
        if (cell->fx == CHIPSEQ_FX_RETRIGGER && cell->fxp > 0 && tick > 0 &&
            (tick % cell->fxp) == 0 && v->inst) {
            v->phase = 0; v->pcm_pos = 0; v->lfsr = K_LFSR_SEED;
            v->seq_pos[0] = v->seq_pos[1] = v->seq_pos[2] = v->seq_pos[3] = 0;
            v->note_ticks = 0;
            v->released = 0; v->active = 1; trig = true;
        }

        if (v->active && !trig && v->inst) {
            const chipseq_instrument *in = v->inst;
            if ((release_mask & (1u << SEQ_VOL)) == 0)
                advance_sequence(in->vol_seq, &v->seq_pos[SEQ_VOL]);
            if ((release_mask & (1u << SEQ_ARP)) == 0)
                advance_sequence(in->arp_seq, &v->seq_pos[SEQ_ARP]);
            if ((release_mask & (1u << SEQ_PITCH)) == 0)
                advance_sequence(in->pitch_seq, &v->seq_pos[SEQ_PITCH]);
            if ((release_mask & (1u << SEQ_DUTY)) == 0)
                advance_sequence(in->duty_seq, &v->seq_pos[SEQ_DUTY]);
            v->note_ticks++;
        }

        if (tick > 0 && v->active) {
            switch (cell->fx) {
            case CHIPSEQ_FX_PORTA_UP:
                v->cur_pitch = saturating_add_i32(v->cur_pitch, (int32_t)cell->fxp);
                break;
            case CHIPSEQ_FX_PORTA_DOWN:
                v->cur_pitch = saturating_add_i32(v->cur_pitch, -(int32_t)cell->fxp);
                break;
            case CHIPSEQ_FX_TONE_PORTA: {
                const chipseq_instrument *in = v->inst;
                int32_t target = ((int32_t)v->note + (in ? in->transpose : 0)) * 16
                               + (in ? in->finetune : 0);
                int32_t step = (int32_t)cell->fxp * 4;
                if (v->cur_pitch < target)
                    v->cur_pitch = (v->cur_pitch + step > target) ? target : v->cur_pitch + step;
                else if (v->cur_pitch > target)
                    v->cur_pitch = (v->cur_pitch - step < target) ? target : v->cur_pitch - step;
                break;
            }
            case CHIPSEQ_FX_VOL_SLIDE: {
                int32_t up = cell->fxp >> 4, dn = cell->fxp & 15;
                v->vol = (uint8_t)clampi((int32_t)v->vol + up - dn, 0, 64);
                break;
            }
            default: break;
            }
        }

        if (cell->fx == CHIPSEQ_FX_NOTE_CUT && tick == cell->fxp)
            v->vol = 0;
    }

    refresh_track(seq, t, vgain);
}

static void advance_to_next_tick(chipseq *seq, chipseq_track *t, unsigned slot,
                                 int32_t *vgain) {
    if (!transport_step_tick(t->song, t, seq->out_rate)) {
        t->active = 0;
        silence_track_voices(seq, t, vgain);
        if (!track_is_music(seq, t)) {
            sfx_release(seq, slot);
            /* Ducking is derived from live SFX tracks, so restore music gain at
             * this same sample rather than at the next render block. */
            refresh_track(seq, &seq->music, vgain);
        }
        return;
    }
    render_process_tick(seq, t, vgain);
}

/* Advance every active track by one oversampled sample, firing due ticks. */
static void transport_advance(chipseq *seq, int32_t *vgain, bool process_sfx) {
    if (seq->music.active && !seq->music.paused) {
        chipseq_track *t = &seq->music;
        t->tick_accum += (uint64_t)1 << 32;
        int guard = 0;
        while (t->tick_accum >= t->samples_per_tick && guard++ < 256) {
            t->tick_accum -= t->samples_per_tick;
            advance_to_next_tick(seq, t, 0, vgain);
            if (!t->active) break;
        }
    }
    if (process_sfx) {
        for (unsigned i = 0; i < CHIPSEQ_SFX_SLOTS; i++) {
            chipseq_track *t = &seq->sfx[i];
            if (!t->active) continue;
            t->tick_accum += (uint64_t)1 << 32;
            int guard = 0;
            while (t->tick_accum >= t->samples_per_tick && guard++ < 256) {
                t->tick_accum -= t->samples_per_tick;
                advance_to_next_tick(seq, t, i, vgain);
                if (!t->active) break;
            }
        }
    }
}

/* ======================================================================== */
/* voice synthesis (one oversampled sample; advances oscillator state)       */
/* ======================================================================== */

static int32_t synth_voice(chipseq_voice *v) {
    const chipseq_instrument *in = v->inst;
    if (!in) return 0;
    int32_t w = 0;
    switch (in->wave) {
    case CHIPSEQ_WAVE_PULSE: {
        uint32_t thr = (uint32_t)v->duty << 26;
        w = (v->phase < thr) ? K_WAVE_PEAK : -K_WAVE_PEAK;
        v->phase += v->phase_inc;
        break;
    }
    case CHIPSEQ_WAVE_TRIANGLE: {
        uint32_t ph = (v->phase >> 16) & 0xFFFFu;
        uint32_t up = ph < 0x8000u ? ph : (0xFFFFu - ph);   /* 0..0x7FFF */
        if (in->tri_steps >= 4) {
            uint32_t steps = in->tri_steps;
            uint32_t q = (up * steps) >> 15;
            if (q >= steps) q = steps - 1u;
            up = q * 32767u / (steps - 1u);
        }
        w = (int32_t)up * 2 - 32767;
        v->phase += v->phase_inc;
        break;
    }
    case CHIPSEQ_WAVE_SAW: {
        uint32_t s = v->phase >> 16;
        if (v->duty != 0) s = (s >> 13) << 13;   /* 8-level quantization */
        w = (int32_t)s - 32768;
        v->phase += v->phase_inc;
        break;
    }
    case CHIPSEQ_WAVE_NOISE: {
        w = (v->lfsr & 1u) ? K_WAVE_PEAK : -K_WAVE_PEAK;
        uint32_t np = v->phase + v->phase_inc;
        if (np < v->phase) {   /* wrapped: clock LFSR once */
            uint32_t bit = (in->noise_mode == CHIPSEQ_NOISE_SHORT)
                         ? ((v->lfsr ^ (v->lfsr >> 6)) & 1u)
                         : ((v->lfsr ^ (v->lfsr >> 1)) & 1u);
            v->lfsr = (v->lfsr >> 1) | (bit << 14);
        }
        v->phase = np;
        break;
    }
    case CHIPSEQ_WAVE_WAVETABLE: {
        uint32_t idx = v->phase >> 27;                 /* 0..31 */
        int32_t nib = in->wavetable[idx] & 15;
        w = (nib * 2 - 15) * 2184;
        v->phase += v->phase_inc;
        break;
    }
    case CHIPSEQ_WAVE_PCM: {
        const chipseq_pcm *p = in->pcm;
        uint64_t idx64 = v->pcm_pos >> 16;
        if (idx64 >= p->frame_count) { v->active = 0; return 0; }
        uint32_t idx = (uint32_t)idx64;
        bool loop = (p->loop_start < p->loop_end);
        uint32_t nidx = idx + 1u;
        if (loop && nidx >= p->loop_end)
            nidx = p->loop_start;
        else if (nidx >= p->frame_count)
            nidx = idx;
        int32_t s0 = p->frames[idx], s1 = p->frames[nidx];
        uint32_t fr = (uint32_t)(v->pcm_pos & 0xFFFFu);
        w = s0 + (int32_t)floor_shift_i64((int64_t)(s1 - s0) * fr, 16);
        v->pcm_pos += v->phase_inc;
        if (loop) {
            uint64_t loop_start_q16 = (uint64_t)p->loop_start << 16;
            uint64_t loop_end_q16 = (uint64_t)p->loop_end << 16;
            uint64_t loop_len_q16 = loop_end_q16 - loop_start_q16;
            if (v->pcm_pos >= loop_end_q16)
                v->pcm_pos = loop_start_q16
                           + (v->pcm_pos - loop_start_q16) % loop_len_q16;
        } else if ((v->pcm_pos >> 16) >= p->frame_count) {
            v->active = 0;
        }
        break;
    }
    default: break;
    }
    return w;
}

/* Mix one oversampled sample. Returns post-master int32 L (and R for stereo),
 * unclamped (clamped after decimation). */
static int64_t nes_mix_value(int32_t pulse, int32_t tri, int32_t noise) {
    pulse = clampi(pulse, 0, 30);
    tri = clampi(tri, 0, 15);
    noise = clampi(noise, 0, 15);
    int32_t ti = 3 * tri + 2 * noise;
    if (ti > 202) ti = 202;
    return ((int64_t)(k_nes_pulse[pulse] + k_nes_tnd[ti])) << 8;
}

static void mix_oversample(chipseq *seq, const int32_t *vgain,
                           unsigned voice_limit, bool stereo,
                           int32_t *outL, int32_t *outR) {
    bool nes = (seq->mix_mode == CHIPSEQ_MIX_NES);
    int64_t lin = 0, linL = 0, linR = 0;
    int32_t pulse_sum = 0, tri_sum = 0, noise_sum = 0;
    int32_t pulse_l = 0, pulse_r = 0;
    int32_t tri_l = 0, tri_r = 0;
    int32_t noise_l = 0, noise_r = 0;

    for (unsigned vi = 0; vi < voice_limit; vi++) {
        chipseq_voice *v = &seq->voices[vi];
        if (!v->active || !v->inst) continue;
        int32_t w = synth_voice(v);
        int32_t g = vgain[vi];
        if (nes) {
            uint8_t wave = v->inst->wave;
            int32_t vl = clampi(g >> 10, 0, 15);
            int32_t left = vl;
            int32_t right = vl;
            if (stereo) {
                int32_t pan = v->pan;
                left = (vl * (255 - pan) + 127) / 255;
                right = (vl * pan + 127) / 255;
            }
            if (wave == CHIPSEQ_WAVE_PULSE) {
                if (w > 0) {
                    if (stereo) { pulse_l += left; pulse_r += right; }
                    else pulse_sum += vl;
                }
                continue;
            }
            if (wave == CHIPSEQ_WAVE_TRIANGLE) {
                uint32_t up = (uint32_t)((w + 32767) >> 1);
                if (stereo) {
                    tri_l += (int32_t)((up * (uint32_t)left) >> 15);
                    tri_r += (int32_t)((up * (uint32_t)right) >> 15);
                } else {
                    tri_sum += (int32_t)((up * (uint32_t)vl) >> 15);
                }
                continue;
            }
            if (wave == CHIPSEQ_WAVE_NOISE) {
                if (w > 0) {
                    if (stereo) { noise_l += left; noise_r += right; }
                    else noise_sum += vl;
                }
                continue;
            }
        }
        int64_t c = floor_shift_i64((int64_t)w * g, 8);
        if (stereo) {
            int32_t pan = v->pan;
            linL += floor_shift_i64(c * (255 - pan), 8);
            linR += floor_shift_i64(c * pan, 8);
        } else {
            lin += c;
        }
    }

    int64_t accL, accR;
    if (nes) {
        if (stereo) {
            accL = nes_mix_value(pulse_l, tri_l, noise_l) + linL;
            accR = nes_mix_value(pulse_r, tri_r, noise_r) + linR;
        } else {
            accL = accR = nes_mix_value(pulse_sum, tri_sum, noise_sum) + lin;
        }
    } else if (stereo) {
        accL = linL; accR = linR;
    } else {
        accL = accR = lin;
    }
    *outL = (int32_t)floor_shift_i64(accL * (int64_t)seq->volume_q8, 16);
    if (outR)
        *outR = (int32_t)floor_shift_i64(accR * (int64_t)seq->volume_q8, 16);
}

/* ======================================================================== */
/* halfband decimation                                                       */
/* ======================================================================== */

static void push15_pair(int32_t z[15], int32_t older, int32_t newer) {
    memmove(&z[2], &z[0], 13u * sizeof(int32_t));
    z[1] = older;
    z[0] = newer;
}
static int32_t hb15(const int32_t z[15]) {
    int64_t a = (int64_t)HB_C * z[7]
              + (int64_t)HB_K1 * ((int64_t)z[6] + z[8])
              + (int64_t)HB_K3 * ((int64_t)z[4] + z[10])
              + (int64_t)HB_K5 * ((int64_t)z[2] + z[12])
              + (int64_t)HB_K7 * ((int64_t)z[0] + z[14]);
    return (int32_t)floor_shift_i64(a, 15);
}

/* Preserve the pinned response while a track is live, then guarantee that its
 * final positive sub-LSB delta reaches exact silence. Without the one-step
 * correction a completed negative lowpass tail can remain at -1 forever. */
static int32_t lowpass_sample(int32_t current, int32_t target,
                              uint16_t coefficient, bool converge_tail) {
    int64_t delta = (int64_t)target - current;
    int64_t step = floor_shift_i64(delta * coefficient, 16);
    if (converge_tail && target == 0 && step == 0 && delta > 0) step = 1;
    return (int32_t)((int64_t)current + step);
}

/* ======================================================================== */
/* command application (render side)                                         */
/* ======================================================================== */

static uint32_t float_to_q8(float f) {
    /* The negated comparison also catches NaN without requiring libm. */
    if (!(f > 0.0f)) return 0;
    if (f >= 1.0f) return 256u;
    uint32_t q = (uint32_t)(f * 256.0f + 0.5f);
    return q > 256u ? 256u : q;
}

static bool gain_is_valid(float f) {
    return f == f && f >= 0.0f && f <= 1.0f;
}

static bool options_are_valid(const chipseq_options *options) {
    if (options->sample_rate < 8000u || options->sample_rate > 192000u) return false;
    if (options->oversample != 1u && options->oversample != 2u && options->oversample != 4u)
        return false;
    if (options->mix_mode > CHIPSEQ_MIX_NES) return false;
    return gain_is_valid(options->volume) && gain_is_valid(options->sfx_duck);
}

static void reset_track_voices(chipseq *seq, uint8_t first, uint8_t channels) {
    for (uint8_t c = 0; c < channels; c++) {
        chipseq_voice *v = &seq->voices[(unsigned)first + c];
        memset(v, 0, sizeof *v);
        v->vol = 64;
        v->pan = 128;
        v->duty = CHIPSEQ_DUTY_50;
        v->lfsr = K_LFSR_SEED;
    }
}

static void start_track(chipseq *seq, chipseq_track *t, const chipseq_song *song,
                        uint8_t first_voice, bool loop) {
    t->song = song;
    t->order_pos = 0; t->row = 0; t->tick = 0;
    t->ticks_per_row = song->ticks_per_row;
    t->bpm_q8 = song->bpm_q8;
    t->channels = song->channels;
    t->first_voice = first_voice;
    t->looping = loop ? 1u : 0u;
    t->paused = 0;
    t->active = 1;
    t->tick_accum = 0;
    reset_track_voices(seq, first_voice, song->channels);
    apply_speed_tempo(song, t, seq->out_rate);
}

static void apply_cmd(chipseq *seq, const chipseq_cmd *cmd, int32_t *vgain) {
    switch (cmd->op) {
    case OP_MUSIC_PLAY: {
        chipseq_track *t = &seq->music;
        silence_track_voices(seq, t, vgain);
        t->fade_ticks = 0; t->fade_left = 0;
        t->generation = 256;   /* music volume, 1/256 */
        start_track(seq, t, cmd->song, 0, cmd->loop != 0);
        render_process_tick(seq, t, vgain);
        break;
    }
    case OP_MUSIC_STOP:
        if (cmd->ia <= 0) {
            seq->music.active = 0;
            silence_track_voices(seq, &seq->music, vgain);
        } else {
            seq->music.fade_ticks = (uint16_t)cmd->ia;
            seq->music.fade_left = (uint16_t)cmd->ia;
        }
        break;
    case OP_MUSIC_VOL:
        seq->music.generation = float_to_q8(cmd->fa);
        break;
    case OP_MUSIC_PAUSE:
        seq->music.paused = cmd->loop ? 1u : 0u;
        break;
    case OP_SFX_PLAY: {
        unsigned slot = cmd->slot;
        uint32_t gen = cmd->generation;
        unsigned w = atomic_load_explicit(&seq->sfx_claim[slot], memory_order_acquire);
        if (claim_state(w) != ST_CLAIMED || claim_gen(w) != gen) break;
        unsigned newstate = (cmd->loop != 0) ? ST_LOOPING : ST_PLAYING;
        unsigned nw = claim_pack(newstate, gen);
        if (!atomic_compare_exchange_strong_explicit(&seq->sfx_claim[slot], &w, nw,
                memory_order_acq_rel, memory_order_acquire))
            break;
        chipseq_track *t = &seq->sfx[slot];
        silence_track_voices(seq, t, vgain);
        uint8_t fv = (uint8_t)(CHIPSEQ_CHANNELS_MAX + slot * CHIPSEQ_SFX_CHANNELS_MAX);
        t->generation = gen;
        start_track(seq, t, cmd->song, fv, cmd->loop != 0);
        t->fade_ticks = (uint16_t)float_to_q8(cmd->fa);            /* sfx volume */
        t->fade_left = (uint16_t)(clampi(cmd->ia, -128, 127) + 128); /* biased transpose */
        render_process_tick(seq, t, vgain);
        break;
    }
    case OP_SFX_SET: {
        unsigned slot = cmd->slot;
        chipseq_track *t = &seq->sfx[slot];
        if (t->active && t->generation == cmd->generation) {
            t->fade_ticks = (uint16_t)float_to_q8(cmd->fa);
            t->fade_left = (uint16_t)(clampi(cmd->ia, -128, 127) + 128);
        }
        break;
    }
    case OP_SFX_STOP: {
        unsigned slot = cmd->slot;
        chipseq_track *t = &seq->sfx[slot];
        if (t->active && t->generation == cmd->generation) {
            t->active = 0;
            silence_track_voices(seq, t, vgain);
            sfx_release(seq, slot);
            refresh_track(seq, &seq->music, vgain);
        }
        break;
    }
    case OP_SFX_STOP_ALL:
        for (unsigned i = 0; i < CHIPSEQ_SFX_SLOTS; i++) {
            if (seq->sfx[i].active) {
                seq->sfx[i].active = 0;
                silence_track_voices(seq, &seq->sfx[i], vgain);
                sfx_release(seq, i);
            }
        }
        refresh_track(seq, &seq->music, vgain);
        break;
    default: break;
    }
}

static bool queue_drain(chipseq *seq, int32_t *vgain) {
    uint32_t tail = atomic_load_explicit(&seq->queue_tail, memory_order_relaxed);
    uint32_t head = atomic_load_explicit(&seq->queue_head, memory_order_acquire);
    bool playhead_changed = false;
    while (tail != head) {
        const chipseq_cmd *cmd = &seq->queue[tail % CHIPSEQ_CMD_QUEUE];
        if (cmd->op == OP_MUSIC_PLAY || cmd->op == OP_MUSIC_STOP)
            playhead_changed = true;
        apply_cmd(seq, cmd, vgain);
        tail++;
    }
    atomic_store_explicit(&seq->queue_tail, tail, memory_order_release);
    return playhead_changed;
}

/* ======================================================================== */
/* snapshot                                                                  */
/* ======================================================================== */

static void write_snapshot(chipseq *seq) {
    unsigned position = (unsigned)seq->music.order_pos
                      | ((unsigned)seq->music.row << 16);
    unsigned state = (unsigned)seq->music.tick
                   | ((unsigned)(seq->music.active != 0) << 8);
    /* Keep every snapshot operation in one sequentially-consistent order. It
     * makes the seqlock proof apply across the three distinct atomic objects:
     * equal even versions cannot bracket payload from an intervening publish. */
    (void)atomic_fetch_add_explicit(&seq->snap_seq, 1u, memory_order_seq_cst);
    atomic_store_explicit(&seq->snap_position, position, memory_order_seq_cst);
    atomic_store_explicit(&seq->snap_state, state, memory_order_seq_cst);
    (void)atomic_fetch_add_explicit(&seq->snap_seq, 1u, memory_order_seq_cst);
}

/* ======================================================================== */
/* render blocks                                                             */
/* ======================================================================== */

static unsigned active_voice_limit(const chipseq *seq) {
    unsigned limit = seq->music.active ? seq->music.channels : 0u;
    for (unsigned i = 0; i < CHIPSEQ_SFX_SLOTS; i++) {
        if (!seq->sfx[i].active) continue;
        unsigned end = CHIPSEQ_CHANNELS_MAX +
                       i * CHIPSEQ_SFX_CHANNELS_MAX + seq->sfx[i].channels;
        if (end > limit) limit = end;
    }
    return limit;
}

static bool samples_are_zero(const int32_t *samples, size_t count) {
    for (size_t i = 0; i < count; i++)
        if (samples[i] != 0) return false;
    return true;
}

static bool mono_tail_is_empty(const chipseq *seq) {
    if (seq->lp_mono != 0) return false;
    if (seq->oversample >= 2u &&
        !samples_are_zero(seq->dec_z0, 15u)) return false;
    return seq->oversample != 4u || samples_are_zero(seq->dec_z1, 15u);
}

static bool stereo_tail_is_empty(const chipseq *seq) {
    if (seq->lp_l != 0 || seq->lp_r != 0) return false;
    if (seq->oversample >= 2u &&
        (!samples_are_zero(seq->dec_z0l, 15u) ||
         !samples_are_zero(seq->dec_z0r, 15u))) return false;
    return seq->oversample != 4u ||
           (samples_are_zero(seq->dec_z1l, 15u) &&
            samples_are_zero(seq->dec_z1r, 15u));
}

static void render_block(chipseq *seq, size_t frames, int16_t *o16, float *of32) {
    int32_t vgain[CHIPSEQ_VOICES_MAX];
    memset(vgain, 0, sizeof vgain);
    (void)queue_drain(seq, vgain);
    if (!atomic_load_explicit(&seq->enabled, memory_order_acquire)) {
        if (o16) memset(o16, 0, frames * sizeof(int16_t));
        write_snapshot(seq);
        return;   /* f32: add nothing */
    }
    refresh_all(seq, vgain);
    bool process_sfx = any_sfx_active(seq);
    unsigned voice_limit = active_voice_limit(seq);
    if (voice_limit == 0u && mono_tail_is_empty(seq)) {
        if (o16) memset(o16, 0, frames * sizeof(int16_t));
        write_snapshot(seq);
        return;   /* f32: add nothing */
    }

    /* Persistent streaming filter state (see chipseq struct): NOT reset here, so
     * the decimation FIR + lowpass are block-size-independent. */
    int32_t *z0 = seq->dec_z0, *z1 = seq->dec_z1;
    uint32_t ov = seq->oversample;

    for (size_t f = 0; f < frames; f++) {
        int32_t outv;
        if (ov == 1) {
            int32_t s;
            transport_advance(seq, vgain, process_sfx);
            mix_oversample(seq, vgain, voice_limit, false, &s, NULL);
            outv = s;
        } else if (ov == 2) {
            int32_t a, b;
            transport_advance(seq, vgain, process_sfx);
            mix_oversample(seq, vgain, voice_limit, false, &a, NULL);
            transport_advance(seq, vgain, process_sfx);
            mix_oversample(seq, vgain, voice_limit, false, &b, NULL);
            push15_pair(z0, a, b);
            outv = hb15(z0);
        } else {   /* ov == 4 */
            int32_t s[4];
            for (int k = 0; k < 4; k++) {
                transport_advance(seq, vgain, process_sfx);
                mix_oversample(seq, vgain, voice_limit, false, &s[k], NULL);
            }
            push15_pair(z0, s[0], s[1]); int32_t o0 = hb15(z0);
            push15_pair(z0, s[2], s[3]); int32_t o1 = hb15(z0);
            push15_pair(z1, o0, o1);
            outv = hb15(z1);
        }
        if (seq->lowpass) {
            bool converge_tail = !seq->music.active && !any_sfx_active(seq);
            seq->lp_mono = lowpass_sample(seq->lp_mono, outv, seq->lowpass,
                                          converge_tail);
            outv = seq->lp_mono;
        }
        int16_t sample = clamp16(outv);
        if (o16) o16[f] = sample;
        if (of32) of32[f] += (float)sample * (1.0f / 32768.0f);
    }
    write_snapshot(seq);
}

static void render_block_stereo(chipseq *seq, size_t frames, int16_t *dst) {
    int32_t vgain[CHIPSEQ_VOICES_MAX];
    memset(vgain, 0, sizeof vgain);
    (void)queue_drain(seq, vgain);
    if (!atomic_load_explicit(&seq->enabled, memory_order_acquire)) {
        memset(dst, 0, frames * 2u * sizeof(int16_t));
        write_snapshot(seq);
        return;
    }
    refresh_all(seq, vgain);
    bool process_sfx = any_sfx_active(seq);
    unsigned voice_limit = active_voice_limit(seq);
    if (voice_limit == 0u && stereo_tail_is_empty(seq)) {
        memset(dst, 0, frames * 2u * sizeof(int16_t));
        write_snapshot(seq);
        return;
    }

    /* Persistent streaming filter state (see chipseq struct): NOT reset here, so
     * the decimation FIR + lowpass are block-size-independent. */
    int32_t *z0l = seq->dec_z0l, *z1l = seq->dec_z1l;
    int32_t *z0r = seq->dec_z0r, *z1r = seq->dec_z1r;
    uint32_t ov = seq->oversample;

    for (size_t f = 0; f < frames; f++) {
        int32_t oL, oR;
        if (ov == 1) {
            transport_advance(seq, vgain, process_sfx);
            mix_oversample(seq, vgain, voice_limit, true, &oL, &oR);
        } else if (ov == 2) {
            int32_t aL, aR, bL, bR;
            transport_advance(seq, vgain, process_sfx);
            mix_oversample(seq, vgain, voice_limit, true, &aL, &aR);
            transport_advance(seq, vgain, process_sfx);
            mix_oversample(seq, vgain, voice_limit, true, &bL, &bR);
            push15_pair(z0l, aL, bL); push15_pair(z0r, aR, bR);
            oL = hb15(z0l); oR = hb15(z0r);
        } else {   /* ov == 4 */
            int32_t sL[4], sR[4];
            for (int k = 0; k < 4; k++) {
                transport_advance(seq, vgain, process_sfx);
                mix_oversample(seq, vgain, voice_limit, true, &sL[k], &sR[k]);
            }
            push15_pair(z0l, sL[0], sL[1]); int32_t o0l = hb15(z0l);
            push15_pair(z0l, sL[2], sL[3]); int32_t o1l = hb15(z0l);
            push15_pair(z1l, o0l, o1l); oL = hb15(z1l);
            push15_pair(z0r, sR[0], sR[1]); int32_t o0r = hb15(z0r);
            push15_pair(z0r, sR[2], sR[3]); int32_t o1r = hb15(z0r);
            push15_pair(z1r, o0r, o1r); oR = hb15(z1r);
        }
        if (seq->lowpass) {
            bool converge_tail = !seq->music.active && !any_sfx_active(seq);
            seq->lp_l = lowpass_sample(seq->lp_l, oL, seq->lowpass,
                                       converge_tail); oL = seq->lp_l;
            seq->lp_r = lowpass_sample(seq->lp_r, oR, seq->lowpass,
                                       converge_tail); oR = seq->lp_r;
        }
        dst[2 * f]     = clamp16(oL);
        dst[2 * f + 1] = clamp16(oR);
    }
    write_snapshot(seq);
}

/* ======================================================================== */
/* public lifecycle                                                          */
/* ======================================================================== */

void chipseq_options_init(chipseq_options *options) {
    if (!options) return;
    options->sample_rate = 44100;
    options->oversample = 2;
    options->mix_mode = CHIPSEQ_MIX_LINEAR;
    options->volume = 1.0f;
    options->sfx_duck = 1.0f;
    options->lowpass = 0;
}

bool chipseq_init(chipseq *seq, const chipseq_options *options) {
    if (!seq) return false;
    chipseq_options def;
    chipseq_options_init(&def);
    chipseq_options selected = options ? *options : def;

    /* Establish the documented safe, uninitialized state before validating.
     * Reinitialization is only allowed once the render thread has stopped. */
    memset(seq, 0, sizeof *seq);
    for (unsigned i = 0; i < CHIPSEQ_SFX_SLOTS; i++)
        atomic_init(&seq->sfx_claim[i], claim_pack(ST_FREE, 0));
    seq->sfx_claim_next = 1u;
    atomic_init(&seq->queue_head, 0u);
    atomic_init(&seq->queue_tail, 0u);
    atomic_init(&seq->snap_seq, 0u);
    atomic_init(&seq->snap_position, 0u);
    atomic_init(&seq->snap_state, 0u);
    atomic_init(&seq->enabled, 0u);

    if (!options_are_valid(&selected)) return false;
    uint8_t ov = selected.oversample;

    seq->sample_rate = selected.sample_rate;
    seq->oversample = ov;
    seq->out_rate = selected.sample_rate * ov;
    seq->mix_mode = selected.mix_mode;
    seq->volume_q8 = float_to_q8(selected.volume);
    seq->duck_q8 = float_to_q8(selected.sfx_duck);
    seq->lowpass = selected.lowpass;

    for (int n = 0; n < 128; n++) {
        int d = n - 69;
        int e = d / 12;
        int f = d - 12 * e;
        if (f < 0) { f += 12; e -= 1; }
        uint64_t base = (uint64_t)K_A4_HZ * (uint64_t)k_semitone_q16[f];
        int shift = 16 + e;                 /* e in [-6,4] -> shift in [10,20] */
        uint64_t num = base << shift;
        uint64_t inc = (num + seq->out_rate / 2u) / seq->out_rate;
        if (inc > (uint64_t)INT32_MAX) inc = (uint64_t)INT32_MAX;
        seq->note_inc[n] = (int32_t)inc;
    }

    atomic_store_explicit(&seq->enabled, 1u, memory_order_relaxed);
    seq->offline = false;
    seq->initialized = true;
    return true;
}

void chipseq_shutdown(chipseq *seq) {
    if (!seq) return;
    seq->music.active = 0;
    seq->music.paused = 0;
    for (unsigned i = 0; i < CHIPSEQ_SFX_SLOTS; i++) {
        seq->sfx[i].active = 0;
        atomic_store_explicit(&seq->sfx_claim[i], claim_pack(ST_FREE, 0),
                              memory_order_relaxed);
    }
    atomic_store_explicit(&seq->queue_head, 0u, memory_order_relaxed);
    atomic_store_explicit(&seq->queue_tail, 0u, memory_order_relaxed);
    atomic_store_explicit(&seq->snap_position, 0u, memory_order_relaxed);
    atomic_store_explicit(&seq->snap_state, 0u, memory_order_relaxed);
    atomic_store_explicit(&seq->snap_seq, 0u, memory_order_release);
    atomic_store_explicit(&seq->enabled, 0u, memory_order_relaxed);
    seq->initialized = false;
}

void chipseq_set_enabled(chipseq *seq, bool on) {
    if (!seq || !seq->initialized) return;
    atomic_store_explicit(&seq->enabled, on ? 1u : 0u, memory_order_release);
}

bool chipseq_is_enabled(const chipseq *seq) {
    if (!seq || !seq->initialized) return false;
    return atomic_load_explicit(&seq->enabled, memory_order_acquire) != 0u;
}

/* ======================================================================== */
/* public music transport                                                    */
/* ======================================================================== */

bool chipseq_music_play(chipseq *seq, const chipseq_song *song, bool loop,
                        char *err, size_t err_len) {
    if (!seq || !seq->initialized) { set_err(err, err_len, "engine not initialized"); return false; }
    if (!chipseq_song_validate(song, err, err_len)) return false;
    if (!atomic_load_explicit(&seq->enabled, memory_order_acquire)) {
        set_err(err, err_len, "engine disabled"); return false;
    }
    chipseq_cmd c;
    memset(&c, 0, sizeof c);
    c.op = OP_MUSIC_PLAY;
    c.song = song;
    c.loop = loop ? 1u : 0u;
    if (!queue_push(seq, &c)) {
        set_err(err, err_len, "command queue full");
        return false;
    }
    return true;
}

bool chipseq_music_stop(chipseq *seq, uint16_t fade_ticks) {
    if (!seq || !seq->initialized) return false;
    chipseq_cmd c;
    memset(&c, 0, sizeof c);
    c.op = OP_MUSIC_STOP;
    c.ia = (int32_t)fade_ticks;
    return queue_push(seq, &c);
}

bool chipseq_music_set_volume(chipseq *seq, float volume) {
    if (!seq || !seq->initialized) return false;
    if (!gain_is_valid(volume)) return false;
    chipseq_cmd c;
    memset(&c, 0, sizeof c);
    c.op = OP_MUSIC_VOL;
    c.fa = volume;
    return queue_push(seq, &c);
}

bool chipseq_music_set_paused(chipseq *seq, bool paused) {
    if (!seq || !seq->initialized) return false;
    chipseq_cmd c;
    memset(&c, 0, sizeof c);
    c.op = OP_MUSIC_PAUSE;
    c.loop = paused ? 1u : 0u;
    return queue_push(seq, &c);
}

bool chipseq_music_position(const chipseq *seq, uint16_t *order_pos,
                            uint16_t *row, uint8_t *tick) {
    if (!seq || !seq->initialized) return false;
    unsigned position, state;
    for (unsigned attempt = 0; attempt < 64u; attempt++) {
        unsigned before, after;
        before = atomic_load_explicit(&seq->snap_seq, memory_order_seq_cst);
        if ((before & 1u) != 0u) continue;
        position = atomic_load_explicit(&seq->snap_position, memory_order_seq_cst);
        state = atomic_load_explicit(&seq->snap_state, memory_order_seq_cst);
        after = atomic_load_explicit(&seq->snap_seq, memory_order_seq_cst);
        if (before != after || (after & 1u) != 0u) continue;
        if (((state >> 8) & 1u) == 0u) return false;
        if (order_pos) *order_pos = (uint16_t)(position & 0xFFFFu);
        if (row) *row = (uint16_t)((position >> 16) & 0xFFFFu);
        if (tick) *tick = (uint8_t)(state & 0xFFu);
        return true;
    }
    return false;
}

/* ======================================================================== */
/* public SFX                                                                */
/* ======================================================================== */

static void sfx_release_claimed(chipseq *seq, unsigned slot, uint32_t gen) {
    unsigned w = atomic_load_explicit(&seq->sfx_claim[slot], memory_order_acquire);
    if (claim_state(w) == ST_CLAIMED && claim_gen(w) == gen) {
        unsigned nw = claim_pack(ST_FREE, gen);
        (void)atomic_compare_exchange_strong_explicit(&seq->sfx_claim[slot], &w, nw,
                memory_order_acq_rel, memory_order_acquire);
    }
}

int chipseq_sfx_play(chipseq *seq, const chipseq_song *song, float vol,
                     int transpose, bool loop) {
    if (!seq || !seq->initialized) return -1;
    if (!atomic_load_explicit(&seq->enabled, memory_order_acquire)) return -1;
    if (!gain_is_valid(vol)) return -1;
    if (!chipseq_song_validate(song, NULL, 0)) return -1;
    if (song->channels > CHIPSEQ_SFX_CHANNELS_MAX) return -1;
    if (!queue_space(seq)) return -1;

    int handle = sfx_try_claim(seq);
    if (handle < 0) return -1;
    unsigned slot = (unsigned)(handle & 3);
    uint32_t gen = (uint32_t)handle >> 2;

    chipseq_cmd c;
    memset(&c, 0, sizeof c);
    c.op = OP_SFX_PLAY;
    c.song = song;
    c.slot = (uint8_t)slot;
    c.generation = gen;
    c.fa = vol;
    c.ia = transpose;
    c.loop = loop ? 1u : 0u;
    if (!queue_push(seq, &c)) {
        sfx_release_claimed(seq, slot, gen);
        return -1;
    }
    return handle;
}

bool chipseq_sfx_set(chipseq *seq, int handle, float vol, int transpose) {
    if (!seq || !seq->initialized || handle <= 0) return false;
    if (!gain_is_valid(vol)) return false;
    unsigned slot = (unsigned)(handle & 3);
    uint32_t gen = (uint32_t)handle >> 2;
    if (slot >= CHIPSEQ_SFX_SLOTS) return false;
    unsigned w = atomic_load_explicit(&seq->sfx_claim[slot], memory_order_acquire);
    if (claim_state(w) == ST_FREE || claim_gen(w) != gen) return false;
    chipseq_cmd c;
    memset(&c, 0, sizeof c);
    c.op = OP_SFX_SET;
    c.slot = (uint8_t)slot;
    c.generation = gen;
    c.fa = vol;
    c.ia = transpose;
    return queue_push(seq, &c);
}

bool chipseq_sfx_stop(chipseq *seq, int handle) {
    if (!seq || !seq->initialized || handle <= 0) return false;
    unsigned slot = (unsigned)(handle & 3);
    uint32_t gen = (uint32_t)handle >> 2;
    if (slot >= CHIPSEQ_SFX_SLOTS) return false;
    unsigned w = atomic_load_explicit(&seq->sfx_claim[slot], memory_order_acquire);
    if (claim_state(w) == ST_FREE || claim_gen(w) != gen) return false;
    chipseq_cmd c;
    memset(&c, 0, sizeof c);
    c.op = OP_SFX_STOP;
    c.slot = (uint8_t)slot;
    c.generation = gen;
    return queue_push(seq, &c);
}

bool chipseq_sfx_stop_all(chipseq *seq) {
    if (!seq || !seq->initialized) return false;
    chipseq_cmd c;
    memset(&c, 0, sizeof c);
    c.op = OP_SFX_STOP_ALL;
    return queue_push(seq, &c);
}

bool chipseq_sfx_active(const chipseq *seq, int handle) {
    if (!seq || !seq->initialized || handle <= 0) return false;
    unsigned slot = (unsigned)(handle & 3);
    uint32_t gen = (uint32_t)handle >> 2;
    if (slot >= CHIPSEQ_SFX_SLOTS) return false;
    unsigned w = atomic_load_explicit(&seq->sfx_claim[slot], memory_order_acquire);
    return claim_state(w) != ST_FREE && claim_gen(w) == gen;
}

/* ======================================================================== */
/* public rendering                                                          */
/* ======================================================================== */

void chipseq_render_s16(chipseq *seq, int16_t *dst, size_t frames) {
    if (!seq || !dst) return;
    if (frames > SIZE_MAX / sizeof *dst) return;
    render_block(seq, frames, dst, NULL);
}

void chipseq_render_s16_stereo(chipseq *seq, int16_t *dst, size_t frames) {
    if (!seq || !dst) return;
    if (frames > SIZE_MAX / (2u * sizeof *dst)) return;
    render_block_stereo(seq, frames, dst);
}

void chipseq_render_f32(chipseq *seq, float *dst, size_t frames) {
    if (!seq || !dst) return;
    render_block(seq, frames, NULL, dst);
}

void chipseq_generator(float *dst, size_t frames, void *user) {
    chipseq_render_f32((chipseq *)user, dst, frames);
}

void chipseq_flush_commands(chipseq *seq) {
    if (!seq) return;
    int32_t vgain[CHIPSEQ_VOICES_MAX];
    memset(vgain, 0, sizeof vgain);
    if (queue_drain(seq, vgain)) write_snapshot(seq);
}

/* ======================================================================== */
/* offline bounce                                                            */
/* ======================================================================== */

static bool resolve_render_length(const chipseq_song *song, uint32_t sample_rate,
                                  uint64_t max_frames, uint64_t *out,
                                  char *err, size_t err_len) {
    uint64_t natural = chipseq_song_frames(song, sample_rate);
    if (natural == 0) {
        set_err(err, err_len, "cannot determine song length");
        return false;
    }
    if (max_frames == 0) {
        if (natural == UINT64_MAX) {
            set_err(err, err_len, "infinite/looping song requires nonzero max_frames");
            return false;
        }
        *out = natural;
    } else {
        *out = (natural == UINT64_MAX || max_frames < natural) ? max_frames : natural;
    }
    return true;
}

int16_t *chipseq_render_song(const chipseq_song *song, const chipseq_options *options,
                             uint64_t max_frames, size_t *out_frames,
                             char *err, size_t err_len) {
    if (out_frames) *out_frames = 0;
    if (!chipseq_song_validate(song, err, err_len)) return NULL;
    chipseq_options def;
    chipseq_options_init(&def);
    chipseq_options selected = options ? *options : def;
    if (!options_are_valid(&selected)) {
        set_err(err, err_len, "invalid options");
        return NULL;
    }

    uint64_t n;
    if (!resolve_render_length(song, selected.sample_rate, max_frames, &n, err, err_len))
        return NULL;
    if (n > (SIZE_MAX / sizeof(int16_t))) {
        set_err(err, err_len, "requested length too large");
        return NULL;
    }

    chipseq *s = (chipseq *)malloc(sizeof *s);
    if (!s) { set_err(err, err_len, "out of memory"); return NULL; }
    if (!chipseq_init(s, &selected)) { free(s); set_err(err, err_len, "invalid options"); return NULL; }
    int16_t *buf = (int16_t *)malloc((size_t)n * sizeof(int16_t));
    if (!buf) { chipseq_shutdown(s); free(s); set_err(err, err_len, "out of memory"); return NULL; }
    s->offline = true;

    char e2[128];
    bool play_loop = song->loop_order != CHIPSEQ_NO_LOOP;
    if (!chipseq_music_play(s, song, play_loop, e2, sizeof e2)) {
        chipseq_shutdown(s); free(s); free(buf);
        set_err(err, err_len, "%s", e2);
        return NULL;
    }

    size_t done = 0;
    while (done < n) {
        size_t chunk = (n - done > 4096u) ? 4096u : (size_t)(n - done);
        chipseq_render_s16(s, buf + done, chunk);
        done += chunk;
    }
    chipseq_shutdown(s);
    free(s);
    if (out_frames) *out_frames = (size_t)n;
    return buf;
}

void chipseq_pcm_free(int16_t *frames) {
    free(frames);
}

static void wav_put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}
static void wav_put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

bool chipseq_bounce_wav(const chipseq_song *song, const chipseq_options *options,
                        const char *path, uint64_t max_frames,
                        char *err, size_t err_len) {
    if (!path) { set_err(err, err_len, "path is NULL"); return false; }
    if (!chipseq_song_validate(song, err, err_len)) return false;
    chipseq_options def;
    chipseq_options_init(&def);
    chipseq_options selected = options ? *options : def;
    if (!options_are_valid(&selected)) {
        set_err(err, err_len, "invalid options");
        return false;
    }

    uint64_t expected_frames;
    if (!resolve_render_length(song, selected.sample_rate, max_frames,
                               &expected_frames, err, err_len))
        return false;
    const uint64_t wav_max_frames = ((uint64_t)UINT32_MAX - 36u) / sizeof(int16_t);
    if (expected_frames > wav_max_frames) {
        set_err(err, err_len, "WAV length exceeds the RIFF 32-bit size limit");
        return false;
    }

    uint32_t sr = selected.sample_rate;
    uint32_t data_bytes = (uint32_t)(expected_frames * sizeof(int16_t));
    uint8_t hdr[44];
    memcpy(hdr, "RIFF", 4);
    wav_put32(hdr + 4, 36u + data_bytes);
    memcpy(hdr + 8, "WAVE", 4);
    memcpy(hdr + 12, "fmt ", 4);
    wav_put32(hdr + 16, 16u);              /* fmt chunk size */
    wav_put16(hdr + 20, 1u);               /* PCM */
    wav_put16(hdr + 22, 1u);               /* mono */
    wav_put32(hdr + 24, sr);
    wav_put32(hdr + 28, sr * 2u);          /* byte rate */
    wav_put16(hdr + 32, 2u);               /* block align */
    wav_put16(hdr + 34, 16u);              /* bits per sample */
    memcpy(hdr + 36, "data", 4);
    wav_put32(hdr + 40, data_bytes);

    chipseq *s = (chipseq *)malloc(sizeof *s);
    if (!s) { set_err(err, err_len, "out of memory"); return false; }
    if (!chipseq_init(s, &selected)) {
        free(s);
        set_err(err, err_len, "invalid options");
        return false;
    }
    s->offline = true;
    char e2[128];
    bool play_loop = song->loop_order != CHIPSEQ_NO_LOOP;
    if (!chipseq_music_play(s, song, play_loop, e2, sizeof e2)) {
        chipseq_shutdown(s);
        free(s);
        set_err(err, err_len, "%s", e2);
        return false;
    }

    FILE *fp = fopen(path, "wb");
    if (!fp) {
        chipseq_shutdown(s);
        free(s);
        set_err(err, err_len, "cannot open '%s'", path);
        return false;
    }

    bool ok = (fwrite(hdr, 1, sizeof hdr, fp) == sizeof hdr);
    uint64_t offset = 0;
    int16_t pcm[4096u];
    uint8_t bytes[4096u * sizeof(int16_t)];
    while (ok && offset < expected_frames) {
        size_t count = (expected_frames - offset > 4096u)
                     ? 4096u : (size_t)(expected_frames - offset);
        chipseq_render_s16(s, pcm, count);
        for (size_t i = 0; i < count; i++) {
            uint16_t value = (uint16_t)pcm[i];
            bytes[2u * i] = (uint8_t)(value & 0xFFu);
            bytes[2u * i + 1u] = (uint8_t)(value >> 8);
        }
        size_t byte_count = count * sizeof(int16_t);
        ok = fwrite(bytes, 1, byte_count, fp) == byte_count;
        offset += count;
    }
    if (fclose(fp) != 0) ok = false;
    chipseq_shutdown(s);
    free(s);
    if (!ok) {
        (void)remove(path);
        set_err(err, err_len, "write failed for '%s'", path);
        return false;
    }
    return true;
}

/* ======================================================================== */
/* small helpers                                                             */
/* ======================================================================== */

uint8_t chipseq_nes_noise_note(unsigned index) {
    return k_noise_note[index & 15u];
}

const char *chipseq_fx_name(chipseq_fx fx) {
    switch (fx) {
    case CHIPSEQ_FX_NONE:          return "NONE";
    case CHIPSEQ_FX_ARPEGGIO:      return "ARPEGGIO";
    case CHIPSEQ_FX_PORTA_UP:      return "PORTA_UP";
    case CHIPSEQ_FX_PORTA_DOWN:    return "PORTA_DOWN";
    case CHIPSEQ_FX_TONE_PORTA:    return "TONE_PORTA";
    case CHIPSEQ_FX_VIBRATO:       return "VIBRATO";
    case CHIPSEQ_FX_TREMOLO:       return "TREMOLO";
    case CHIPSEQ_FX_VOL_SLIDE:     return "VOL_SLIDE";
    case CHIPSEQ_FX_PAN:           return "PAN";
    case CHIPSEQ_FX_DUTY:          return "DUTY";
    case CHIPSEQ_FX_RETRIGGER:     return "RETRIGGER";
    case CHIPSEQ_FX_NOTE_DELAY:    return "NOTE_DELAY";
    case CHIPSEQ_FX_NOTE_CUT:      return "NOTE_CUT";
    case CHIPSEQ_FX_SPEED:         return "SPEED";
    case CHIPSEQ_FX_TEMPO:         return "TEMPO";
    case CHIPSEQ_FX_ORDER_JUMP:    return "ORDER_JUMP";
    case CHIPSEQ_FX_PATTERN_BREAK: return "PATTERN_BREAK";
    case CHIPSEQ_FX_COUNT:         return "?";
    default:                       return "?";
    }
}
