/* test_chipseq.c -- the core regression suite, a standalone assert-and-
 * print harness that exits nonzero on the first failing group and prints a
 * summary line. No external dependencies: the determinism hash is an inline
 * FNV-1a 64 (no md5), and the WAV round-trip uses a strict inline reader.
 *
 * Covers, in order:
 *   1. validation rejects every out-of-bounds construction with a naming msg
 *   2. determinism golden (FNV-1a 64 over chipseq_render_song == pinned const)
 *   3. f32 == s16 * (1/32768) sample-exact over the reference song
 *   4. command queue: block-boundary apply, issue order, full-queue reject,
 *      no partial apply
 *   5. SFX voice stealing: oldest non-looping first, never a looping slot,
 *      stale generation-checked handles rejected
 *   6. chipseq_song_frames == actual rendered length incl. FX_SPEED / FX_TEMPO
 *   7. chipseq_bounce_wav round-trips against a strict PCM/mono/16-bit reader
 *   8. tick timing scales across sample rates (44100 vs 48000)
 *   9. render output is invariant to caller block partitioning
 *  10. lifecycle/numeric edge cases and synth-control regressions
 */
#include "chip_sequencer.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- tracker-screen cell macros (author-side conveniences) ------------- */
#define CS__                   { CHIPSEQ_NOTE_NONE, 0, CHIPSEQ_VOL_NONE, CHIPSEQ_FX_NONE, 0 }
#define CS_OFF                 { CHIPSEQ_NOTE_OFF,  0, CHIPSEQ_VOL_NONE, CHIPSEQ_FX_NONE, 0 }
#define CS_CUT                 { CHIPSEQ_NOTE_CUT,  0, CHIPSEQ_VOL_NONE, CHIPSEQ_FX_NONE, 0 }
#define CS_N(pc,oct,i,v)       { CHIPSEQ_NOTE(CHIPSEQ_PC_##pc,oct), (i), (v), CHIPSEQ_FX_NONE, 0 }
#define CS_NF(pc,oct,i,v,fx,p) { CHIPSEQ_NOTE(CHIPSEQ_PC_##pc,oct), (i), (v), CHIPSEQ_FX_##fx, (p) }
#define CS_FX(fx,p)            { CHIPSEQ_NOTE_NONE, 0, CHIPSEQ_VOL_NONE, CHIPSEQ_FX_##fx, (p) }

/* ======================================================================== */
/* harness                                                                   */
/* ======================================================================== */

static int g_fails = 0;
static int g_checks = 0;
#define CHECK(cond, msg) do { \
    g_checks++; \
    if (!(cond)) { printf("FAIL: %s  (%s:%d)\n", (msg), __FILE__, __LINE__); g_fails++; } \
} while (0)

/* FNV-1a 64. Hash each s16 sample as two little-endian bytes so the golden is
 * endian-independent -- the byte contract is the sample values, not host byte
 * order. */
#define FNV64_OFFSET 14695981039346656037ULL
#define FNV64_PRIME  1099511628211ULL
static uint64_t fnv1a_s16(const int16_t *s, size_t n) {
    uint64_t h = FNV64_OFFSET;
    for (size_t i = 0; i < n; i++) {
        uint16_t u = (uint16_t)s[i];
        h ^= (uint8_t)(u & 0xFFu);       h *= FNV64_PRIME;
        h ^= (uint8_t)((u >> 8) & 0xFFu); h *= FNV64_PRIME;
    }
    return h;
}

/* ======================================================================== */
/* the reference song (ORIGINAL music -- no copyrighted melody). Used by the  */
/* golden, the f32/s16 identity, the WAV round-trip, and the tick-timing test.*/
/* Its render bytes are the byte contract (see test 2).                       */
/* ======================================================================== */

static const int8_t ref_env_pluck[] = { 64, 60, 52, 44, 38, 32, 26, 20, 15, 10, 6, 3, 1, 0 };
static const chipseq_seq ref_pluck_vol = {
    ref_env_pluck, 14, CHIPSEQ_SEQ_NO_LOOP, CHIPSEQ_SEQ_NO_RELEASE
};
static const int8_t ref_arp_min[] = { 0, 3, 7 };   /* minor-triad arpeggio */
static const chipseq_seq ref_lead_arp = { ref_arp_min, 3, 0, CHIPSEQ_SEQ_NO_RELEASE };

static const int8_t ref_env_kick[] = { 64, 56, 38, 18, 5, 0 };
static const chipseq_seq ref_kick_vol = {
    ref_env_kick, 6, CHIPSEQ_SEQ_NO_LOOP, CHIPSEQ_SEQ_NO_RELEASE
};
static const int8_t ref_kick_pitch[] = { 20, 6, 0, -6 };
static const chipseq_seq ref_kick_pitch_seq = {
    ref_kick_pitch, 4, CHIPSEQ_SEQ_NO_LOOP, CHIPSEQ_SEQ_NO_RELEASE
};

static const chipseq_instrument ref_insts[] = {
    { .name = "lead", .wave = CHIPSEQ_WAVE_PULSE, .duty = CHIPSEQ_DUTY_25,
      .vol_seq = &ref_pluck_vol, .arp_seq = &ref_lead_arp },
    { .name = "bass", .wave = CHIPSEQ_WAVE_TRIANGLE, .tri_steps = 32, .transpose = -12 },
    { .name = "kick", .wave = CHIPSEQ_WAVE_NOISE, .noise_mode = CHIPSEQ_NOISE_LONG,
      .vol_seq = &ref_kick_vol, .pitch_seq = &ref_kick_pitch_seq },
};

/* pattern 0: lead / bass / kick, 8 rows */
static const chipseq_cell ref_p0[] = {
    CS_N(A,4, 0,48),                 CS_N(A,2, 1,56),  CS_N(A,1, 2,64),
    CS__,                            CS__,             CS__,
    CS_N(C,5, 0,48),                 CS__,             CS_N(A,1, 2,44),
    CS__,                            CS_N(E,3, 1,56),  CS__,
    CS_N(E,5, 0,48),                 CS__,             CS_N(A,1, 2,64),
    CS_NF(D,5, 0,44, VIBRATO, 0x38), CS__,             CS__,
    CS_N(C,5, 0,40),                 CS_N(A,2, 1,56),  CS_N(A,1, 2,44),
    CS_OFF,                          CS__,             CS__,
};
/* pattern 1: a short answering phrase */
static const chipseq_cell ref_p1[] = {
    CS_N(G,4, 0,48),                 CS_N(G,2, 1,56),  CS_N(A,1, 2,64),
    CS__,                            CS__,             CS__,
    CS_N(B,4, 0,48),                 CS__,             CS_N(A,1, 2,44),
    CS_NF(A,4, 0,44, TONE_PORTA,4),  CS_N(D,3, 1,56),  CS__,
    CS_N(D,5, 0,48),                 CS__,             CS_N(A,1, 2,64),
    CS__,                            CS__,             CS__,
    CS_N(A,4, 0,40),                 CS_N(A,2, 1,56),  CS_N(A,1, 2,44),
    CS_CUT,                          CS__,             CS__,
};
static const chipseq_pattern ref_pats[] = { { ref_p0, 8 }, { ref_p1, 8 } };
static const uint8_t ref_order[] = { 0, 1, 0 };

static const chipseq_song ref_song = {
    .name = "ref",
    .instruments = ref_insts, .instrument_count = 3,
    .patterns = ref_pats,     .pattern_count = 2,
    .order = ref_order,       .order_length = 3,
    .loop_order = CHIPSEQ_NO_LOOP, .channels = 3,
    .rows_per_beat = 4,   .ticks_per_row = 6,
    .bpm_q8 = CHIPSEQ_BPM(132),
};

/* ---- a tiny valid song used as a mutation base for validation tests ----- */
static const chipseq_cell base_p0[] = {
    CS_N(C,5, 0,48),  CS_N(C,3, 1,56),
    CS__,             CS__,
};
static const chipseq_instrument base_insts[] = {
    { .name = "a", .wave = CHIPSEQ_WAVE_PULSE, .duty = CHIPSEQ_DUTY_50 },
    { .name = "b", .wave = CHIPSEQ_WAVE_TRIANGLE, .tri_steps = 32 },
};
static const chipseq_pattern base_pats[] = { { base_p0, 2 } };
static const uint8_t base_order[] = { 0 };
static const chipseq_song base_song = {
    .name = "base",
    .instruments = base_insts, .instrument_count = 2,
    .patterns = base_pats, .pattern_count = 1,
    .order = base_order, .order_length = 1,
    .loop_order = CHIPSEQ_NO_LOOP, .channels = 2,
    .rows_per_beat = 4, .ticks_per_row = 6, .bpm_q8 = CHIPSEQ_BPM(120),
};

/* ---- tiny SFX songs (<= 2 channels) for the voice-steal test ------------ */
static const chipseq_instrument sfx_insts[] = {
    { .name = "blip", .wave = CHIPSEQ_WAVE_PULSE, .duty = CHIPSEQ_DUTY_50 },
};
static const chipseq_cell sfx_p0[] = { CS_N(A,5, 0,48), CS__, CS__, CS__ };
static const chipseq_pattern sfx_pats[] = { { sfx_p0, 4 } };
static const uint8_t sfx_order_once[] = { 0 };
static const chipseq_song sfx_oneshot = {
    .name = "sfx1", .instruments = sfx_insts, .instrument_count = 1,
    .patterns = sfx_pats, .pattern_count = 1,
    .order = sfx_order_once, .order_length = 1,
    .loop_order = CHIPSEQ_NO_LOOP, .channels = 1,
    .rows_per_beat = 4, .ticks_per_row = 6, .bpm_q8 = CHIPSEQ_BPM(120),
};
static const chipseq_song sfx_loop = {
    .name = "sfxL", .instruments = sfx_insts, .instrument_count = 1,
    .patterns = sfx_pats, .pattern_count = 1,
    .order = sfx_order_once, .order_length = 1,
    .loop_order = 0, .channels = 1,
    .rows_per_beat = 4, .ticks_per_row = 6, .bpm_q8 = CHIPSEQ_BPM(120),
};

/* ---- a song using FX_SPEED and FX_TEMPO (for song_frames test) ---------- */
static const chipseq_cell fx_p0[] = {
    CS_N(C,5, 0,48),
    CS_FX(SPEED, 3),
    CS_N(E,5, 0,48),
    CS_FX(TEMPO, 90),
    CS_N(G,5, 0,48),
    CS__,
    CS_N(C,6, 0,48),
    CS__,
};
static const chipseq_pattern fx_pats[] = { { fx_p0, 8 } };
static const uint8_t fx_order[] = { 0, 0 };
static const chipseq_song fx_song = {
    .name = "fx",
    .instruments = base_insts, .instrument_count = 2,
    .patterns = fx_pats, .pattern_count = 1,
    .order = fx_order, .order_length = 2,
    .loop_order = CHIPSEQ_NO_LOOP, .channels = 1,
    .rows_per_beat = 4, .ticks_per_row = 6, .bpm_q8 = CHIPSEQ_BPM(120),
};

/* ======================================================================== */
/* 1. validation                                                             */
/* ======================================================================== */

static bool has(const char *hay, const char *needle) {
    return strstr(hay, needle) != NULL;
}

static void test_validation(void) {
    char err[160];

    CHECK(chipseq_song_validate(&base_song, err, sizeof err), "base song validates");
    CHECK(chipseq_song_validate(&ref_song, err, sizeof err), "reference song validates");

    /* order[i] >= pattern_count */
    {
        static const uint8_t bad_order[] = { 5 };
        chipseq_song s = base_song; s.order = bad_order;
        err[0] = 0;
        CHECK(!chipseq_song_validate(&s, err, sizeof err), "reject order[i] >= pattern_count");
        CHECK(has(err, "order[") && has(err, "pattern_count"), "order msg names order+pattern_count");
    }
    /* inst >= instrument_count */
    {
        static const chipseq_cell bp[] = {
            CS_N(C,5, 9,48), CS_N(C,3, 1,56),   /* inst 9 >= 2 */
            CS__,            CS__,
        };
        static const chipseq_pattern bpat[] = { { bp, 2 } };
        chipseq_song s = base_song; s.patterns = bpat;
        err[0] = 0;
        CHECK(!chipseq_song_validate(&s, err, sizeof err), "reject inst >= instrument_count");
        CHECK(has(err, "inst") && has(err, "row") && has(err, "chan"),
              "inst msg names inst/row/chan");
    }
    /* seq.loop >= length */
    {
        static const int8_t vals[] = { 64, 32, 0 };
        static const chipseq_seq badloop = { vals, 3, 9, CHIPSEQ_SEQ_NO_RELEASE };
        static const chipseq_instrument bi[] = {
            { .name = "x", .wave = CHIPSEQ_WAVE_PULSE, .duty = CHIPSEQ_DUTY_50,
              .vol_seq = &badloop },
            { .name = "b", .wave = CHIPSEQ_WAVE_TRIANGLE, .tri_steps = 32 },
        };
        chipseq_song s = base_song; s.instruments = bi;
        err[0] = 0;
        CHECK(!chipseq_song_validate(&s, err, sizeof err), "reject seq.loop >= length");
        CHECK(has(err, "instrument") && has(err, "loop"), "seq loop msg names instrument+loop");
    }
    /* seq.release >= length */
    {
        static const int8_t vals[] = { 64, 32, 0 };
        static const chipseq_seq badrel = { vals, 3, CHIPSEQ_SEQ_NO_LOOP, 7 };
        static const chipseq_instrument bi[] = {
            { .name = "x", .wave = CHIPSEQ_WAVE_PULSE, .duty = CHIPSEQ_DUTY_50,
              .vol_seq = &badrel },
            { .name = "b", .wave = CHIPSEQ_WAVE_TRIANGLE, .tri_steps = 32 },
        };
        chipseq_song s = base_song; s.instruments = bi;
        err[0] = 0;
        CHECK(!chipseq_song_validate(&s, err, sizeof err), "reject seq.release >= length");
        CHECK(has(err, "instrument") && has(err, "release"),
              "seq release msg names instrument+release");
    }
    /* PCM loop_end > frame_count */
    {
        static const int16_t frames[8] = { 0, 1000, 2000, 1000, 0, -1000, -2000, -1000 };
        static const chipseq_pcm badpcm = {
            frames, 8, 0, 99 /* loop_end 99 > 8 */, 60
        };
        static const chipseq_instrument bi[] = {
            { .name = "x", .wave = CHIPSEQ_WAVE_PCM, .pcm = &badpcm },
            { .name = "b", .wave = CHIPSEQ_WAVE_TRIANGLE, .tri_steps = 32 },
        };
        chipseq_song s = base_song; s.instruments = bi;
        err[0] = 0;
        CHECK(!chipseq_song_validate(&s, err, sizeof err), "reject PCM loop_end > frame_count");
        CHECK(has(err, "instrument") && has(err, "PCM"), "PCM msg names instrument+PCM");
    }
    /* channels > CHIPSEQ_CHANNELS_MAX */
    {
        chipseq_song s = base_song; s.channels = (uint8_t)(CHIPSEQ_CHANNELS_MAX + 1u);
        err[0] = 0;
        CHECK(!chipseq_song_validate(&s, err, sizeof err), "reject channels > max");
        CHECK(has(err, "channels"), "channels msg names channels");
    }
    /* nonzero instrument_count requires a non-NULL instrument table */
    {
        chipseq_song s = base_song; s.instruments = NULL;
        err[0] = 0;
        CHECK(!chipseq_song_validate(&s, err, sizeof err),
              "reject NULL instruments with nonzero instrument_count");
        CHECK(has(err, "instruments") && has(err, "NULL"),
              "NULL instrument-table error is descriptive");
    }
    /* cell volumes are either 0..64 or CHIPSEQ_VOL_NONE */
    {
        static const chipseq_cell bp[] = {
            { CHIPSEQ_NOTE_NONE, 0, 65, CHIPSEQ_FX_NONE, 0 }, CS__,
            CS__, CS__,
        };
        static const chipseq_pattern bpat[] = { { bp, 2 } };
        chipseq_song s = base_song; s.patterns = bpat;
        err[0] = 0;
        CHECK(!chipseq_song_validate(&s, err, sizeof err), "reject cell volume > 64");
        CHECK(has(err, "volume"), "bad-volume error names volume");
    }
    /* zero-row pattern */
    {
        static const chipseq_pattern zpat[] = { { base_p0, 0 } };
        chipseq_song s = base_song; s.patterns = zpat;
        err[0] = 0;
        CHECK(!chipseq_song_validate(&s, err, sizeof err), "reject zero-row pattern");
        CHECK(has(err, "pattern") && has(err, "rows"), "zero-row msg names pattern+rows");
    }
    /* bpm_q8 < CHIPSEQ_BPM_MIN */
    {
        chipseq_song s = base_song; s.bpm_q8 = (uint16_t)(CHIPSEQ_BPM_MIN - 1u);
        err[0] = 0;
        CHECK(!chipseq_song_validate(&s, err, sizeof err), "reject bpm_q8 < min");
        CHECK(has(err, "bpm_q8"), "bpm msg names bpm_q8");
    }
    /* documented instrument/sequence value ranges are part of validation */
    {
        chipseq_instrument bi[2] = { base_insts[0], base_insts[1] };
        chipseq_song s = base_song; s.instruments = bi;
        bi[0].duty = 64;
        err[0] = 0;
        CHECK(!chipseq_song_validate(&s, err, sizeof err), "reject pulse duty > 63");
        CHECK(has(err, "duty"), "bad-duty error names duty");
    }
    {
        chipseq_instrument bi[2] = { base_insts[0], base_insts[1] };
        chipseq_song s = base_song; s.instruments = bi;
        bi[0].wave = CHIPSEQ_WAVE_NOISE; bi[0].noise_mode = 2;
        err[0] = 0;
        CHECK(!chipseq_song_validate(&s, err, sizeof err), "reject invalid noise mode");
        CHECK(has(err, "noise_mode"), "bad-noise-mode error is descriptive");
    }
    {
        static const int8_t bad_values[] = { 64, 65 };
        static const chipseq_seq bad_vol = {
            bad_values, 2, CHIPSEQ_SEQ_NO_LOOP, CHIPSEQ_SEQ_NO_RELEASE
        };
        chipseq_instrument bi[2] = { base_insts[0], base_insts[1] };
        chipseq_song s = base_song; s.instruments = bi; bi[0].vol_seq = &bad_vol;
        err[0] = 0;
        CHECK(!chipseq_song_validate(&s, err, sizeof err), "reject volume-sequence value > 64");
        CHECK(has(err, "vol_seq") && has(err, "value"),
              "bad volume-sequence error identifies its value");
    }
    {
        static const int8_t bad_values[] = { 32, 0 };
        static const chipseq_seq bad_duty = {
            bad_values, 2, CHIPSEQ_SEQ_NO_LOOP, CHIPSEQ_SEQ_NO_RELEASE
        };
        chipseq_instrument bi[2] = { base_insts[0], base_insts[1] };
        chipseq_song s = base_song; s.instruments = bi; bi[0].duty_seq = &bad_duty;
        err[0] = 0;
        CHECK(!chipseq_song_validate(&s, err, sizeof err), "reject duty-sequence value below one");
        CHECK(has(err, "duty_seq") && has(err, "value"),
              "bad duty-sequence error identifies its value");
    }
    {
        static const uint8_t bad_wavetable[CHIPSEQ_WAVETABLE_LEN] = { 16 };
        chipseq_instrument bi[2] = { base_insts[0], base_insts[1] };
        chipseq_song s = base_song; s.instruments = bi;
        bi[0].wave = CHIPSEQ_WAVE_WAVETABLE; bi[0].wavetable = bad_wavetable;
        err[0] = 0;
        CHECK(!chipseq_song_validate(&s, err, sizeof err), "reject wavetable nibble > 15");
        CHECK(has(err, "wavetable"), "bad-wavetable error is descriptive");
    }
    {
        chipseq_instrument bi[2] = { base_insts[0], base_insts[1] };
        chipseq_song s = base_song; s.instruments = bi;
        bi[1].tri_steps = 2;
        err[0] = 0;
        CHECK(!chipseq_song_validate(&s, err, sizeof err), "reject triangle step count 1..3");
        CHECK(has(err, "tri_steps"), "bad-triangle-step error is descriptive");
    }
    /* err_len == 0 must not crash / write */
    CHECK(!chipseq_song_validate(NULL, NULL, 0), "NULL song rejected, no err write");
}

/* ======================================================================== */
/* 2. determinism golden                                                     */
/* ======================================================================== */

/* THE BYTE CONTRACT. This 64-bit FNV-1a hash is computed over the entire
 * int16 PCM produced by chipseq_render_song(ref_song, default options). Any
 * change to it is a deliberate, reviewed audio change (a golden rehash), never
 * an accident -- exactly the documented byte-contract guarantee. If a change alters
 * the render path, this test fails and the change must be justified. */
/* Rehashed golden: track-end gains now clear at the exact ending sample, and a
 * triangle advances with the same one-cycle Q32 phase convention as every other
 * pitched oscillator (rather than sounding one octave flat). Both are deliberate
 * correctness fixes to the byte contract. */
#define REF_GOLDEN_FNV64 0x7af68f37b171809eULL

static void test_determinism_golden(void) {
    char err[128];
    chipseq_options opt; chipseq_options_init(&opt);

    size_t n1 = 0, n2 = 0;
    int16_t *a = chipseq_render_song(&ref_song, &opt, 0, &n1, err, sizeof err);
    int16_t *b = chipseq_render_song(&ref_song, &opt, 0, &n2, err, sizeof err);
    CHECK(a && b, "render_song returns buffers");
    if (!a || !b) { free(a); free(b); return; }

    CHECK(n1 == n2, "two renders same length");
    CHECK(memcmp(a, b, n1 * sizeof(int16_t)) == 0, "two renders byte-identical");

    uint64_t h = fnv1a_s16(a, n1);
    printf("  [golden] frames=%zu fnv1a64=0x%016llx\n", n1, (unsigned long long)h);
    CHECK(h == REF_GOLDEN_FNV64, "render matches pinned golden hash (byte contract)");

    chipseq_pcm_free(a);
    chipseq_pcm_free(b);
}

/* ======================================================================== */
/* 3. f32 == s16 * (1/32768) sample-exact                                     */
/* ======================================================================== */

static void test_f32_identity(void) {
    char err[128];
    chipseq_options opt; chipseq_options_init(&opt);

    uint64_t total = chipseq_song_frames(&ref_song, opt.sample_rate);
    CHECK(total > 0 && total != UINT64_MAX, "ref song finite");
    if (total == 0 || total == UINT64_MAX) return;

    chipseq s16eng, f32eng;
    CHECK(chipseq_init(&s16eng, &opt), "init s16 engine");
    CHECK(chipseq_init(&f32eng, &opt), "init f32 engine");
    CHECK(chipseq_music_play(&s16eng, &ref_song, false, err, sizeof err), "play s16");
    CHECK(chipseq_music_play(&f32eng, &ref_song, false, err, sizeof err), "play f32");

    const size_t BLK = 4096;
    int16_t *s16 = malloc(BLK * sizeof(int16_t));
    float   *f32 = malloc(BLK * sizeof(float));
    int exact = 1;
    uint64_t done = 0;
    while (done < total) {
        size_t chunk = (total - done > BLK) ? BLK : (size_t)(total - done);
        for (size_t i = 0; i < chunk; i++) f32[i] = 0.0f;
        chipseq_render_s16(&s16eng, s16, chunk);
        chipseq_render_f32(&f32eng, f32, chunk);
        for (size_t i = 0; i < chunk; i++) {
            if (f32[i] != (float)s16[i] * (1.0f / 32768.0f)) { exact = 0; break; }
        }
        if (!exact) break;
        done += chunk;
    }
    CHECK(exact, "f32 == s16 * (1/32768) sample-exact over the whole song");
    free(s16); free(f32);
    chipseq_shutdown(&s16eng);
    chipseq_shutdown(&f32eng);
}

/* ======================================================================== */
/* 4. command queue                                                          */
/* ======================================================================== */

static void test_command_queue(void) {
    char err[128];
    chipseq_options opt; chipseq_options_init(&opt);

    /* (a) commands apply at the block boundary, not synchronously */
    {
        chipseq seq; CHECK(chipseq_init(&seq, &opt), "init(a)");
        uint16_t op, row; uint8_t tk;
        CHECK(!chipseq_music_position(&seq, &op, &row, &tk), "no music before play");
        CHECK(chipseq_music_play(&seq, &ref_song, false, err, sizeof err), "queue music_play");
        /* not drained yet: position still reports inactive */
        CHECK(!chipseq_music_position(&seq, &op, &row, &tk),
              "music_play deferred until a render block");
        int16_t buf[64];
        chipseq_render_s16(&seq, buf, 64);   /* drains the queue */
        CHECK(chipseq_music_position(&seq, &op, &row, &tk),
              "music active after one render block");
        chipseq_shutdown(&seq);
    }

    /* (b) commands apply in issue order: play THEN stop(0) => stopped */
    {
        chipseq seq; CHECK(chipseq_init(&seq, &opt), "init(b)");
        CHECK(chipseq_music_play(&seq, &ref_song, false, err, sizeof err), "queue play");
        CHECK(chipseq_music_stop(&seq, 0), "queue stop after play");
        int16_t buf[64];
        chipseq_render_s16(&seq, buf, 64);
        uint16_t op, row; uint8_t tk;
        CHECK(!chipseq_music_position(&seq, &op, &row, &tk),
              "play-then-stop leaves music stopped (issue order honored)");
        chipseq_shutdown(&seq);
    }
    /* ... and the reverse order leaves it playing */
    {
        chipseq seq; CHECK(chipseq_init(&seq, &opt), "init(b2)");
        CHECK(chipseq_music_stop(&seq, 0), "queue stop first (no-op)");
        CHECK(chipseq_music_play(&seq, &ref_song, false, err, sizeof err), "queue play second");
        int16_t buf[64];
        chipseq_render_s16(&seq, buf, 64);
        uint16_t op, row; uint8_t tk;
        CHECK(chipseq_music_position(&seq, &op, &row, &tk),
              "stop-then-play leaves music playing (issue order honored)");
        chipseq_shutdown(&seq);
    }

    /* (c) a full queue returns false (music) / -1 (sfx); nothing partially
     *     applies. Never render, so the ring never drains. */
    {
        chipseq seq; CHECK(chipseq_init(&seq, &opt), "init(c)");
        unsigned accepted = 0;
        for (unsigned i = 0; i < CHIPSEQ_CMD_QUEUE + 8u; i++) {
            if (chipseq_music_set_volume(&seq, 0.5f)) accepted++;
            else break;
        }
        CHECK(accepted == CHIPSEQ_CMD_QUEUE, "exactly CHIPSEQ_CMD_QUEUE commands accepted");
        CHECK(!chipseq_music_set_volume(&seq, 0.5f), "music command rejected when queue full");
        CHECK(chipseq_sfx_play(&seq, &sfx_oneshot, 1.0f, 0, false) == -1,
              "sfx_play returns -1 when queue full");
        /* a rejected sfx_play must not have consumed a slot */
        int nfree = 0;
        /* draining then re-checking proves the ring is consistent and reusable */
        chipseq_flush_commands(&seq);
        CHECK(chipseq_music_set_volume(&seq, 0.4f),
              "queue reusable after draining (no corruption from rejects)");
        /* after a real drain a slot is available again */
        chipseq_flush_commands(&seq);
        int h = chipseq_sfx_play(&seq, &sfx_oneshot, 1.0f, 0, false);
        CHECK(h > 0, "sfx_play succeeds once the queue has space (no slot leaked)");
        (void)nfree;
        chipseq_shutdown(&seq);
    }

    /* (d) stop-all reports queue pressure instead of silently losing a stop. */
    {
        chipseq seq; CHECK(chipseq_init(&seq, &opt), "init(d)");
        int h = chipseq_sfx_play(&seq, &sfx_loop, 1.0f, 0, true);
        CHECK(h > 0, "start looping SFX for stop-all queue test");
        chipseq_flush_commands(&seq);
        for (unsigned i = 0; i < CHIPSEQ_CMD_QUEUE; i++)
            CHECK(chipseq_music_set_volume(&seq, 0.5f), "fill queue before stop-all");
        CHECK(!chipseq_sfx_stop_all(&seq), "stop-all reports a full command queue");
        CHECK(chipseq_sfx_active(&seq, h), "failed stop-all does not claim success");
        chipseq_flush_commands(&seq);
        CHECK(chipseq_sfx_stop_all(&seq), "stop-all queues once space is available");
        chipseq_flush_commands(&seq);
        CHECK(!chipseq_sfx_active(&seq, h), "successful stop-all releases the SFX");
        chipseq_shutdown(&seq);
    }
}

/* ======================================================================== */
/* 5. SFX voice stealing                                                     */
/* ======================================================================== */

static void test_sfx_stealing(void) {
    chipseq_options opt; chipseq_options_init(&opt);

    /* (a) all-non-looping: 5th play steals the OLDEST slot; stale handle dies */
    {
        chipseq seq; CHECK(chipseq_init(&seq, &opt), "init steal(a)");
        int h[CHIPSEQ_SFX_SLOTS];
        for (unsigned i = 0; i < CHIPSEQ_SFX_SLOTS; i++) {
            h[i] = chipseq_sfx_play(&seq, &sfx_oneshot, 1.0f, 0, false);
            CHECK(h[i] > 0, "sfx one-shot claims a slot");
        }
        chipseq_flush_commands(&seq);   /* CLAIMED -> PLAYING for all four */
        for (unsigned i = 0; i < CHIPSEQ_SFX_SLOTS; i++)
            CHECK(chipseq_sfx_active(&seq, h[i]), "all four SFX active");

        int h5 = chipseq_sfx_play(&seq, &sfx_oneshot, 1.0f, 0, false);
        CHECK(h5 > 0, "5th SFX steals a slot rather than failing");
        /* it stole the oldest, which is slot 0 (first claimed, smallest ordinal) */
        CHECK((h5 & 3) == (h[0] & 3), "steal took the oldest slot");
        CHECK(!chipseq_sfx_active(&seq, h[0]), "stolen handle is now stale/inactive");
        CHECK(chipseq_sfx_active(&seq, h5), "the new handle is active");
        /* stale generation-checked handle is rejected by set/stop */
        CHECK(!chipseq_sfx_set(&seq, h[0], 0.5f, 0), "sfx_set rejects a stale handle");
        CHECK(!chipseq_sfx_stop(&seq, h[0]), "sfx_stop rejects a stale handle");
        /* the still-live slots 1..3 remain active */
        for (unsigned i = 1; i < CHIPSEQ_SFX_SLOTS; i++)
            CHECK(chipseq_sfx_active(&seq, h[i]), "untouched slots stay active");
        chipseq_shutdown(&seq);
    }

    /* (b) a LOOPING slot is never stolen */
    {
        chipseq seq; CHECK(chipseq_init(&seq, &opt), "init steal(b)");
        int hl = chipseq_sfx_play(&seq, &sfx_loop, 1.0f, 0, true);
        CHECK(hl > 0, "looping SFX claims a slot");
        chipseq_flush_commands(&seq);   /* -> LOOPING */
        unsigned loop_slot = (unsigned)(hl & 3);

        /* fill the remaining slots with one-shots */
        for (unsigned i = 1; i < CHIPSEQ_SFX_SLOTS; i++) {
            int h = chipseq_sfx_play(&seq, &sfx_oneshot, 1.0f, 0, false);
            CHECK(h > 0, "one-shot fills a non-looping slot");
            chipseq_flush_commands(&seq);
        }
        /* now force several steals; the looping slot must survive every time */
        for (int k = 0; k < 6; k++) {
            int h = chipseq_sfx_play(&seq, &sfx_oneshot, 1.0f, 0, false);
            CHECK(h > 0, "steal succeeds (a non-looping victim exists)");
            CHECK((unsigned)(h & 3) != loop_slot, "steal never targets the looping slot");
            CHECK(chipseq_sfx_active(&seq, hl), "looping SFX stays alive across steals");
            chipseq_flush_commands(&seq);
        }
        chipseq_shutdown(&seq);
    }


    /* (c) the former 14-bit generation boundary must not resurrect a stale
     * handle. Sequential one-shots deliberately reuse slot zero. */
    {
        chipseq seq; CHECK(chipseq_init(&seq, &opt), "init generation-wrap regression");
        int stale = -1;
        bool cycled = true;
        for (unsigned i = 0; i < 16383u; i++) {
            int h = chipseq_sfx_play(&seq, &sfx_oneshot, 1.0f, 0, false);
            if (i == 0) stale = h;
            if (h <= 0) { cycled = false; break; }
            chipseq_flush_commands(&seq);
            if (!chipseq_sfx_stop(&seq, h)) { cycled = false; break; }
            chipseq_flush_commands(&seq);
        }
        CHECK(cycled, "cycle one slot through the old 14-bit generation limit");
        int current = chipseq_sfx_play(&seq, &sfx_oneshot, 1.0f, 0, false);
        CHECK(current > 0 && current != stale, "generation does not alias the stale handle");
        CHECK(!chipseq_sfx_active(&seq, stale) && !chipseq_sfx_stop(&seq, stale),
              "old-boundary stale handle remains rejected");
        chipseq_shutdown(&seq);
    }
}

/* ======================================================================== */
/* 6. chipseq_song_frames == rendered length (incl. FX_SPEED / FX_TEMPO)      */
/* ======================================================================== */

/* Render one frame at a time, reading the published playhead after each frame,
 * and return the frame index at which the music first goes inactive. */
static uint64_t measure_rendered_frames(const chipseq_song *song, uint32_t sr) {
    chipseq_options opt; chipseq_options_init(&opt);
    opt.sample_rate = sr;
    chipseq seq;
    if (!chipseq_init(&seq, &opt)) return 0;
    char err[64];
    if (!chipseq_music_play(&seq, song, false, err, sizeof err)) { chipseq_shutdown(&seq); return 0; }

    uint64_t count = 0;
    uint16_t op, row; uint8_t tk;
    const uint64_t guard = 5000000ULL;
    for (;;) {
        int16_t s;
        chipseq_render_s16(&seq, &s, 1);
        count++;
        if (!chipseq_music_position(&seq, &op, &row, &tk)) break; /* went inactive */
        if (count >= guard) break;
    }
    chipseq_shutdown(&seq);
    return count;
}

static void test_song_frames(void) {
    static const chipseq_cell cycle_cells[] = { CS_FX(ORDER_JUMP, 0) };
    static const chipseq_pattern cycle_pats[] = { { cycle_cells, 1 } };
    static const uint8_t cycle_order[] = { 0 };
    static const chipseq_song cycle_song = {
        .name = "effect-cycle", .instruments = NULL, .instrument_count = 0,
        .patterns = cycle_pats, .pattern_count = 1,
        .order = cycle_order, .order_length = 1,
        .loop_order = CHIPSEQ_NO_LOOP, .channels = 1,
        .rows_per_beat = 4, .ticks_per_row = 1, .bpm_q8 = CHIPSEQ_BPM(120),
    };

    struct { const chipseq_song *song; const char *name; } cases[] = {
        { &ref_song, "ref" },
        { &fx_song,  "fx(SPEED+TEMPO)" },
    };
    for (unsigned i = 0; i < 2; i++) {
        uint32_t sr = 44100;
        uint64_t predicted = chipseq_song_frames(cases[i].song, sr);
        uint64_t measured  = measure_rendered_frames(cases[i].song, sr);
        long diff = (long)measured - (long)predicted;
        printf("  [frames] %-16s predicted=%llu measured=%llu diff=%ld\n",
               cases[i].name, (unsigned long long)predicted,
               (unsigned long long)measured, diff);
        /* measured is the frame at which the playhead first reports inactive;
         * song_frames is the exact fixed-point tick length. The renderer and
         * chipseq_song_frames use the SAME fixed-point tick math, so they agree
         * exactly, including across FX_SPEED and FX_TEMPO. */
        CHECK(diff == 0,
              "song_frames == rendered length exactly (incl. FX_SPEED/FX_TEMPO)");
    }
    CHECK(chipseq_song_validate(&cycle_song, NULL, 0),
          "effect-driven cycle is structurally valid");
    CHECK(chipseq_song_frames(&cycle_song, 44100) == UINT64_MAX,
          "song_frames detects an infinite FX_ORDER_JUMP cycle");
    CHECK(chipseq_song_frames(&base_song, 0) == 0,
          "song_frames rejects an invalid sample rate");
    chipseq_song declared_loop = base_song;
    declared_loop.loop_order = 0;
    CHECK(chipseq_song_frames(&declared_loop, 0) == 0,
          "song_frames rejects an invalid sample rate before loop classification");
}

static void test_offline_length_and_looping(void) {
    static const chipseq_cell cells[] = { CS_N(A,4, 0,64) };
    static const chipseq_pattern patterns[] = { { cells, 1 } };
    static const uint8_t order[] = { 0 };
    static const chipseq_song loop_song = {
        .name = "offline-loop", .instruments = sfx_insts, .instrument_count = 1,
        .patterns = patterns, .pattern_count = 1,
        .order = order, .order_length = 1,
        .loop_order = 0, .channels = 1,
        .rows_per_beat = 4, .ticks_per_row = 1, .bpm_q8 = CHIPSEQ_BPM(120),
    };
    chipseq_song finite_song = loop_song;
    finite_song.loop_order = CHIPSEQ_NO_LOOP;
    chipseq_options opt; chipseq_options_init(&opt);
    opt.sample_rate = 8000; opt.oversample = 1;
    char err[128];

    uint64_t natural = chipseq_song_frames(&finite_song, opt.sample_rate);
    CHECK(natural > 0 && natural < SIZE_MAX / 2u, "offline-loop fixture has finite pass length");
    if (natural == 0 || natural >= SIZE_MAX / 2u) return;

    size_t frames = 0;
    int16_t *finite = chipseq_render_song(&finite_song, &opt, natural * 2u,
                                          &frames, err, sizeof err);
    CHECK(finite != NULL && frames == (size_t)natural,
          "max_frames is a cap, not silence-padding for a finite song");
    chipseq_pcm_free(finite);

    int16_t *looped = chipseq_render_song(&loop_song, &opt, natural * 2u,
                                          &frames, err, sizeof err);
    CHECK(looped != NULL && frames == (size_t)(natural * 2u),
          "capped offline render returns the requested looping duration");
    if (looped && frames == (size_t)(natural * 2u)) {
        size_t nonzero = 0;
        for (size_t i = (size_t)natural; i < frames; i++)
            if (looped[i] != 0) nonzero++;
        CHECK(nonzero > (size_t)natural / 2u,
              "declared loop point produces audio after the first pass");
    }
    chipseq_pcm_free(looped);
}

/* ======================================================================== */
/* 7. bounce_wav round-trip                                                   */
/* ======================================================================== */

/* strict PCM/mono/16-bit WAV reader: rejects anything else. Returns malloc'd
 * frames and count via *out_n, or NULL. */
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static int16_t *wav_read_strict(const char *path, size_t *out_n, uint32_t *out_sr) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    uint8_t hdr[44];
    if (fread(hdr, 1, 44, fp) != 44) { fclose(fp); return NULL; }
    if (memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0 ||
        memcmp(hdr + 12, "fmt ", 4) != 0 || rd32(hdr + 16) != 16 ||
        rd16(hdr + 20) != 1 /*PCM*/ || rd16(hdr + 22) != 1 /*mono*/ ||
        rd16(hdr + 34) != 16 /*bits*/ || memcmp(hdr + 36, "data", 4) != 0) {
        fclose(fp); return NULL;
    }
    uint32_t sr = rd32(hdr + 24);
    uint32_t data_bytes = rd32(hdr + 40);
    size_t n = data_bytes / 2u;
    int16_t *frames = malloc(n ? n * sizeof(int16_t) : 1);
    if (!frames) { fclose(fp); return NULL; }
    for (size_t i = 0; i < n; i++) {
        uint8_t b[2];
        if (fread(b, 1, 2, fp) != 2) { free(frames); fclose(fp); return NULL; }
        frames[i] = (int16_t)rd16(b);
    }
    fclose(fp);
    if (out_n) *out_n = n;
    if (out_sr) *out_sr = sr;
    return frames;
}

static void test_bounce_roundtrip(void) {
    char err[128];
    chipseq_options opt; chipseq_options_init(&opt);

    /* a local scratch file next to the build; removed at the end of the test */
    const char *path = "chipseq_roundtrip_test.wav";

    CHECK(chipseq_bounce_wav(&ref_song, &opt, path, 0, err, sizeof err), "bounce_wav succeeds");

    size_t wn = 0; uint32_t wsr = 0;
    int16_t *w = wav_read_strict(path, &wn, &wsr);
    CHECK(w != NULL, "strict WAV reader accepts the bounced file");
    CHECK(wsr == opt.sample_rate, "WAV sample rate matches options");

    size_t rn = 0;
    int16_t *r = chipseq_render_song(&ref_song, &opt, 0, &rn, err, sizeof err);
    CHECK(r != NULL, "render_song for comparison");

    if (w && r) {
        CHECK(wn == rn, "WAV frame count == render_song frame count");
        if (wn == rn)
            CHECK(memcmp(w, r, rn * sizeof(int16_t)) == 0,
                  "WAV samples == render_song samples (round-trip exact)");
    }
    free(w);
    chipseq_pcm_free(r);
    remove(path);

    uint64_t too_many_frames = ((uint64_t)UINT32_MAX - 36u) / sizeof(int16_t) + 1u;
    err[0] = 0;
    CHECK(!chipseq_bounce_wav(&sfx_loop, &opt, path, too_many_frames, err, sizeof err),
          "bounce_wav rejects output beyond the RIFF 32-bit size limit");
    CHECK(has(err, "RIFF") || has(err, "WAV"),
          "oversized WAV rejection has a descriptive error");
}

/* ======================================================================== */
/* 8. tick timing scales across sample rates                                */
/* ======================================================================== */

/* Capture, for a song at rate sr, the sequence of ticks: each element is the
 * output-frame index at which the playhead first shows a new (order,row,tick),
 * plus the packed position key. Returns count via *out_n (bounded by cap). */
typedef struct { uint64_t frame; uint32_t key; } tickpos;

static size_t capture_ticks(const chipseq_song *song, uint32_t sr,
                            tickpos *out, size_t cap) {
    chipseq_options opt; chipseq_options_init(&opt);
    opt.sample_rate = sr;
    chipseq seq;
    if (!chipseq_init(&seq, &opt)) return 0;
    char err[64];
    if (!chipseq_music_play(&seq, song, false, err, sizeof err)) { chipseq_shutdown(&seq); return 0; }

    size_t n = 0;
    uint32_t last_key = 0xFFFFFFFFu;
    uint64_t frame = 0;
    uint16_t op, row; uint8_t tk;
    for (;;) {
        int16_t s;
        chipseq_render_s16(&seq, &s, 1);
        if (!chipseq_music_position(&seq, &op, &row, &tk)) break;
        uint32_t key = ((uint32_t)op << 16) | ((uint32_t)row << 8) | (uint32_t)tk;
        if (key != last_key) {
            if (n < cap) { out[n].frame = frame; out[n].key = key; }
            n++;
            last_key = key;
        }
        frame++;
        if (frame > 3000000ULL) break;
    }
    chipseq_shutdown(&seq);
    return n;
}

static void test_tick_rate_independence(void) {
    enum { CAP = 4096 };
    static tickpos t44[CAP], t48[CAP];
    size_t n44 = capture_ticks(&ref_song, 44100, t44, CAP);
    size_t n48 = capture_ticks(&ref_song, 48000, t48, CAP);

    CHECK(n44 > 8 && n44 <= CAP, "captured a plausible tick sequence at 44100");
    CHECK(n44 == n48, "same number of ticks at 44100 and 48000");

    if (n44 == n48 && n44 <= CAP) {
        int same_keys = 1, scaled_ok = 1;
        long worst = 0;
        for (size_t i = 0; i < n44; i++) {
            if (t44[i].key != t48[i].key) { same_keys = 0; break; }
            /* frame position must scale by the rate ratio to within a sample */
            double expect = (double)t44[i].frame * 48000.0 / 44100.0;
            long d = (long)t48[i].frame - (long)(expect + 0.5);
            long ad = d < 0 ? -d : d;
            if (ad > worst) worst = ad;
            if (ad > 1) scaled_ok = 0;   /* contract: within one sample */
        }
        printf("  [ticks] n=%zu worst-scale-error=%ld samples\n", n44, worst);
        CHECK(same_keys, "identical (order,row,tick) sequence across sample rates");
        CHECK(scaled_ok, "note-on/tick positions scale by rate within a sample");
    }
}

/* ======================================================================== */
/* 9. render-block partition invariance                                      */
/* ======================================================================== */

static void test_block_partition_invariance(void) {
    chipseq_options opt; chipseq_options_init(&opt);
    uint64_t natural64 = chipseq_song_frames(&sfx_oneshot, opt.sample_rate);
    CHECK(natural64 > 0 && natural64 < SIZE_MAX - 257u,
          "block-partition fixture has a finite length");
    if (natural64 == 0 || natural64 >= SIZE_MAX - 257u) return;

    size_t natural = (size_t)natural64;
    size_t total = natural + 257u; /* include filter tail and sustained silence */
    int16_t *whole = calloc(total, sizeof *whole);
    int16_t *split = calloc(total, sizeof *split);
    CHECK(whole && split, "allocate block-partition buffers");
    if (!whole || !split) { free(whole); free(split); return; }

    chipseq a, b;
    char err[96];
    CHECK(chipseq_init(&a, &opt) && chipseq_init(&b, &opt),
          "initialize block-partition engines");
    CHECK(chipseq_music_play(&a, &sfx_oneshot, false, err, sizeof err) &&
          chipseq_music_play(&b, &sfx_oneshot, false, err, sizeof err),
          "start block-partition fixture");

    chipseq_render_s16(&a, whole, total);
    size_t done = 0;
    while (done < total) {
        size_t chunk = total - done > 13u ? 13u : total - done;
        chipseq_render_s16(&b, split + done, chunk);
        done += chunk;
    }
    CHECK(memcmp(whole, split, total * sizeof *whole) == 0,
          "one large render equals the same render split into 13-frame blocks");

    bool silent_tail = true;
    for (size_t i = natural + 32u; i < total; i++)
        if (whole[i] != 0) { silent_tail = false; break; }
    CHECK(silent_tail, "voices stop after the natural end plus the FIR tail");

    uint16_t order_pos, row; uint8_t tick;
    CHECK(!chipseq_music_position(&a, &order_pos, &row, &tick) &&
          !chipseq_music_position(&b, &order_pos, &row, &tick),
          "both block partitions publish the stopped state");
    chipseq_shutdown(&a); chipseq_shutdown(&b);

    /* The same invariant must hold when an SFX ends mid-block and releases
     * music ducking at that exact sample. */
    memset(whole, 0, total * sizeof *whole);
    memset(split, 0, total * sizeof *split);
    opt.sfx_duck = 0.0f;
    CHECK(chipseq_init(&a, &opt) && chipseq_init(&b, &opt),
          "initialize SFX-duck partition engines");
    CHECK(chipseq_music_play(&a, &sfx_oneshot, true, err, sizeof err) &&
          chipseq_music_play(&b, &sfx_oneshot, true, err, sizeof err),
          "start looping music for SFX-duck partition test");
    CHECK(chipseq_sfx_play(&a, &sfx_oneshot, 1.0f, 0, false) > 0 &&
          chipseq_sfx_play(&b, &sfx_oneshot, 1.0f, 0, false) > 0,
          "start one-shot SFX for duck partition test");
    chipseq_render_s16(&a, whole, total);
    done = 0;
    while (done < total) {
        size_t chunk = total - done > 13u ? 13u : total - done;
        chipseq_render_s16(&b, split + done, chunk);
        done += chunk;
    }
    CHECK(memcmp(whole, split, total * sizeof *whole) == 0,
          "SFX end and duck release are block-partition invariant");
    chipseq_shutdown(&a); chipseq_shutdown(&b);
    free(whole); free(split);
}

/* ======================================================================== */
/* 10. lifecycle/numeric and synth-control regressions                       */
/* ======================================================================== */

static void test_lifecycle_and_numeric_inputs(void) {
    chipseq_options good, bad;
    chipseq_options_init(&good);
    chipseq seq;

    CHECK(chipseq_init(&seq, &good), "initial valid init succeeds");
    CHECK(atomic_is_lock_free(&seq.queue_head) && atomic_is_lock_free(&seq.queue_tail) &&
          atomic_is_lock_free(&seq.sfx_claim[0]) && atomic_is_lock_free(&seq.snap_seq) &&
          atomic_is_lock_free(&seq.enabled),
          "every render/game handshake is lock-free on this supported target");
    bad = good; bad.sample_rate = 1;
    CHECK(!chipseq_init(&seq, &bad), "invalid reinit fails");
    CHECK(!chipseq_is_enabled(&seq), "failed reinit leaves engine disabled");
    CHECK(!chipseq_music_set_volume(&seq, 0.5f),
          "failed reinit leaves engine uninitialized");
    chipseq_set_enabled(&seq, true);
    CHECK(!chipseq_is_enabled(&seq), "cannot enable an uninitialized engine");
    chipseq_shutdown(&seq);

    bad = good; bad.volume = NAN;
    CHECK(!chipseq_init(&seq, &bad), "init rejects NaN master volume");
    bad = good; bad.sfx_duck = NAN;
    CHECK(!chipseq_init(&seq, &bad), "init rejects NaN duck gain");
    bad = good; bad.volume = 1.5f;
    CHECK(!chipseq_init(&seq, &bad), "init rejects master volume above one");

    CHECK(chipseq_init(&seq, &good), "reinitialize after rejected options");
    CHECK(!chipseq_music_set_volume(&seq, NAN), "music volume rejects NaN");
    CHECK(chipseq_sfx_play(&seq, &sfx_oneshot, NAN, 0, false) == -1,
          "SFX play rejects NaN gain");
    chipseq_shutdown(&seq);
}

static void test_synth_regressions(void) {
    static const chipseq_cell note_cell[] = { CS_N(A,4, 0,64) };
    static const chipseq_pattern one_pat[] = { { note_cell, 1 } };
    static const uint8_t one_order[] = { 0 };
    char err[96];

    /* A note-off uses the declared release value on that tick, and a cut at
     * tick zero is immediate. */
    {
        static const int8_t release_values[] = { 64, 23, 0 };
        static const chipseq_seq release_env = {
            release_values, 3, CHIPSEQ_SEQ_NO_LOOP, 1
        };
        static const chipseq_instrument release_inst[] = {
            { .wave = CHIPSEQ_WAVE_PULSE, .duty = CHIPSEQ_DUTY_50,
              .vol_seq = &release_env },
        };
        static const chipseq_cell release_cells[] = {
            CS_N(A,4, 0,64), CS_OFF,
        };
        static const chipseq_pattern release_pat[] = { { release_cells, 2 } };
        static const chipseq_song release_song = {
            .name = "release-index", .instruments = release_inst, .instrument_count = 1,
            .patterns = release_pat, .pattern_count = 1,
            .order = one_order, .order_length = 1,
            .loop_order = CHIPSEQ_NO_LOOP, .channels = 1,
            .rows_per_beat = 4, .ticks_per_row = 1, .bpm_q8 = CHIPSEQ_BPM(120),
        };
        chipseq_options opt; chipseq_options_init(&opt);
        opt.sample_rate = 8000; opt.oversample = 1;
        chipseq seq; int16_t sample; unsigned guard = 0;
        CHECK(chipseq_init(&seq, &opt), "init release-index engine");
        CHECK(chipseq_music_play(&seq, &release_song, false, err, sizeof err),
              "play release-index song");
        chipseq_flush_commands(&seq);
        while (seq.music.active && seq.music.row == 0 && guard++ < 2000u)
            chipseq_render_s16(&seq, &sample, 1);
        CHECK(seq.voices[0].released && seq.voices[0].seq_pos[0] == 1u,
              "note-off holds the first release-envelope index for its tick");
        chipseq_shutdown(&seq);
    }
    {
        static const chipseq_instrument cut_inst[] = {
            { .wave = CHIPSEQ_WAVE_PULSE, .duty = CHIPSEQ_DUTY_50 },
        };
        static const chipseq_cell cut_cell[] = {
            { CHIPSEQ_NOTE(CHIPSEQ_PC_A,4), 0, 64, CHIPSEQ_FX_NOTE_CUT, 0 },
        };
        static const chipseq_pattern cut_pat[] = { { cut_cell, 1 } };
        static const chipseq_song cut_song = {
            .name = "cut-zero", .instruments = cut_inst, .instrument_count = 1,
            .patterns = cut_pat, .pattern_count = 1,
            .order = one_order, .order_length = 1,
            .loop_order = CHIPSEQ_NO_LOOP, .channels = 1,
            .rows_per_beat = 4, .ticks_per_row = 2, .bpm_q8 = CHIPSEQ_BPM(120),
        };
        chipseq_options opt; chipseq_options_init(&opt);
        chipseq seq;
        CHECK(chipseq_init(&seq, &opt), "init zero-cut engine");
        CHECK(chipseq_music_play(&seq, &cut_song, false, err, sizeof err),
              "play zero-cut song");
        chipseq_flush_commands(&seq);
        CHECK(seq.voices[0].vol == 0u, "FX_NOTE_CUT parameter zero cuts on tick zero");
        chipseq_shutdown(&seq);
    }

    /* Repeated pitch slides saturate rather than invoking signed-overflow UB. */
    {
        static const int8_t porta_pitch_values[] = { 127 };
        static const chipseq_seq porta_pitch_seq = {
            porta_pitch_values, 1, CHIPSEQ_SEQ_NO_LOOP, CHIPSEQ_SEQ_NO_RELEASE
        };
        static const chipseq_instrument porta_inst[] = {
            { .wave = CHIPSEQ_WAVE_PULSE, .duty = CHIPSEQ_DUTY_50,
              .pitch_seq = &porta_pitch_seq },
        };
        static const chipseq_cell porta_cell[] = {
            { CHIPSEQ_NOTE(CHIPSEQ_PC_A,4), 0, 64, CHIPSEQ_FX_PORTA_UP, 255 },
        };
        static const chipseq_pattern porta_pat[] = { { porta_cell, 1 } };
        static const chipseq_song porta_song = {
            .name = "porta-saturate", .instruments = porta_inst, .instrument_count = 1,
            .patterns = porta_pat, .pattern_count = 1,
            .order = one_order, .order_length = 1,
            .loop_order = 0, .channels = 1,
            .rows_per_beat = 4, .ticks_per_row = 2, .bpm_q8 = CHIPSEQ_BPM(120),
        };
        chipseq_options opt; chipseq_options_init(&opt);
        opt.sample_rate = 8000; opt.oversample = 1;
        chipseq seq; int16_t sample; unsigned guard = 0;
        CHECK(chipseq_init(&seq, &opt), "init portamento saturation engine");
        CHECK(chipseq_music_play(&seq, &porta_song, true, err, sizeof err),
              "play portamento saturation song");
        chipseq_flush_commands(&seq);
        seq.voices[0].cur_pitch = INT32_MAX - 10;
        while (seq.music.tick == 0 && guard++ < 2000u)
            chipseq_render_s16(&seq, &sample, 1);
        CHECK(seq.voices[0].cur_pitch == INT32_MAX,
              "portamento saturates at INT32_MAX without wrapping");
        chipseq_shutdown(&seq);
    }

    /* Triangle and pulse use the same one-cycle Q32 phase convention. */
    {
        static const chipseq_instrument tri_inst[] = {
            { .wave = CHIPSEQ_WAVE_TRIANGLE, .tri_steps = 0 },
        };
        static const chipseq_song tri_song = {
            .name = "triangle", .instruments = tri_inst, .instrument_count = 1,
            .patterns = one_pat, .pattern_count = 1,
            .order = one_order, .order_length = 1,
            .loop_order = CHIPSEQ_NO_LOOP, .channels = 1,
            .rows_per_beat = 4, .ticks_per_row = 6, .bpm_q8 = CHIPSEQ_BPM(120),
        };
        chipseq_options opt; chipseq_options_init(&opt);
        opt.sample_rate = 8000; opt.oversample = 1;
        chipseq seq; int16_t sample;
        CHECK(chipseq_init(&seq, &opt), "init triangle regression engine");
        CHECK(chipseq_music_play(&seq, &tri_song, false, err, sizeof err),
              "play triangle regression song");
        chipseq_flush_commands(&seq);
        uint32_t inc = seq.voices[0].phase_inc;
        chipseq_render_s16(&seq, &sample, 1);
        CHECK(inc > 0 && seq.voices[0].phase == inc,
              "triangle advances by the full note phase increment");
        chipseq_shutdown(&seq);
    }

    /* Auto-vibrato is note-relative and remains off through vib_delay. */
    {
        static const chipseq_instrument vib_inst[] = {
            { .wave = CHIPSEQ_WAVE_PULSE, .duty = CHIPSEQ_DUTY_50,
              .vib_speed = 4, .vib_depth = 8, .vib_delay = 3 },
        };
        static const chipseq_song vib_song = {
            .name = "vib-delay", .instruments = vib_inst, .instrument_count = 1,
            .patterns = one_pat, .pattern_count = 1,
            .order = one_order, .order_length = 1,
            .loop_order = 0, .channels = 1,
            .rows_per_beat = 4, .ticks_per_row = 6, .bpm_q8 = CHIPSEQ_BPM(120),
        };
        chipseq_options opt; chipseq_options_init(&opt);
        opt.sample_rate = 8000; opt.oversample = 1;
        chipseq seq; int16_t sample;
        CHECK(chipseq_init(&seq, &opt), "init vibrato-delay engine");
        CHECK(chipseq_music_play(&seq, &vib_song, true, err, sizeof err),
              "play vibrato-delay song");
        chipseq_flush_commands(&seq);
        uint32_t base_inc = seq.voices[0].phase_inc;
        int guard = 0;
        while (seq.voices[0].note_ticks < 3u && guard++ < 4096)
            chipseq_render_s16(&seq, &sample, 1);
        CHECK(seq.voices[0].note_ticks == 3u && seq.voices[0].phase_inc == base_inc,
              "auto-vibrato remains neutral through vib_delay ticks");
        while (seq.voices[0].note_ticks < 4u && guard++ < 4096)
            chipseq_render_s16(&seq, &sample, 1);
        CHECK(seq.voices[0].note_ticks == 4u && seq.voices[0].phase_inc != base_inc,
              "auto-vibrato starts after vib_delay");
        chipseq_shutdown(&seq);
    }

    /* NES nonlinear mixing still honors per-voice stereo pan. */
    {
        static const chipseq_cell left_cell[] = {
            { CHIPSEQ_NOTE(CHIPSEQ_PC_A,4), 0, 64, CHIPSEQ_FX_PAN, 0 },
        };
        static const chipseq_cell right_cell[] = {
            { CHIPSEQ_NOTE(CHIPSEQ_PC_A,4), 0, 64, CHIPSEQ_FX_PAN, 255 },
        };
        static const chipseq_pattern left_pat[] = { { left_cell, 1 } };
        static const chipseq_pattern right_pat[] = { { right_cell, 1 } };
        static const chipseq_instrument pulse_inst[] = {
            { .wave = CHIPSEQ_WAVE_PULSE, .duty = CHIPSEQ_DUTY_50 },
        };
        static const chipseq_song left_song = {
            .name = "pan-left", .instruments = pulse_inst, .instrument_count = 1,
            .patterns = left_pat, .pattern_count = 1, .order = one_order, .order_length = 1,
            .loop_order = CHIPSEQ_NO_LOOP, .channels = 1,
            .rows_per_beat = 4, .ticks_per_row = 6, .bpm_q8 = CHIPSEQ_BPM(120),
        };
        static const chipseq_song right_song = {
            .name = "pan-right", .instruments = pulse_inst, .instrument_count = 1,
            .patterns = right_pat, .pattern_count = 1, .order = one_order, .order_length = 1,
            .loop_order = CHIPSEQ_NO_LOOP, .channels = 1,
            .rows_per_beat = 4, .ticks_per_row = 6, .bpm_q8 = CHIPSEQ_BPM(120),
        };
        chipseq_options opt; chipseq_options_init(&opt);
        opt.sample_rate = 8000; opt.oversample = 1; opt.mix_mode = CHIPSEQ_MIX_NES;
        chipseq lseq, rseq; int16_t left[2], right[2];
        CHECK(chipseq_init(&lseq, &opt) && chipseq_init(&rseq, &opt),
              "init NES-pan engines");
        CHECK(chipseq_music_play(&lseq, &left_song, false, err, sizeof err) &&
              chipseq_music_play(&rseq, &right_song, false, err, sizeof err),
              "play NES-pan songs");
        chipseq_render_s16_stereo(&lseq, left, 1);
        chipseq_render_s16_stereo(&rseq, right, 1);
        CHECK(left[0] > 0 && left[1] == 0, "NES pan 0 routes pulse fully left");
        CHECK(right[0] == 0 && right[1] > 0, "NES pan 255 routes pulse fully right");
        chipseq_shutdown(&lseq); chipseq_shutdown(&rseq);
    }

    /* PCM interpolation wraps at loop_end, and large steps use modulo rather
     * than a bounded subtraction loop. */
    {
        static const int16_t frames[] = { 1000, 2000, 3000, 30000 };
        static const chipseq_pcm pcm = { frames, 4, 1, 3, 69 };
        static const chipseq_instrument pcm_inst[] = {
            { .wave = CHIPSEQ_WAVE_PCM, .pcm = &pcm },
        };
        static const chipseq_song pcm_song = {
            .name = "pcm-loop", .instruments = pcm_inst, .instrument_count = 1,
            .patterns = one_pat, .pattern_count = 1, .order = one_order, .order_length = 1,
            .loop_order = CHIPSEQ_NO_LOOP, .channels = 1,
            .rows_per_beat = 4, .ticks_per_row = 6, .bpm_q8 = CHIPSEQ_BPM(120),
        };
        chipseq_options opt; chipseq_options_init(&opt);
        opt.sample_rate = 8000; opt.oversample = 1;
        chipseq seq; int16_t sample;
        CHECK(chipseq_init(&seq, &opt), "init PCM interpolation engine");
        CHECK(chipseq_music_play(&seq, &pcm_song, false, err, sizeof err),
              "play PCM interpolation song");
        chipseq_flush_commands(&seq);
        seq.voices[0].pcm_pos = ((uint64_t)2u << 16) | 0x8000u;
        chipseq_render_s16(&seq, &sample, 1);
        CHECK(sample == 625, "PCM loop-end interpolation blends into loop_start");
        CHECK(seq.voices[0].pcm_pos == (((uint64_t)1u << 16) | 0x8000u),
              "PCM position wraps modulo the loop length");
        chipseq_shutdown(&seq);
    }
    {
        static const int16_t frames[] = { 1000, 2000, 3000, 4000 };
        static const chipseq_pcm pcm = { frames, 4, 0, 1, 0 };
        static const chipseq_instrument pcm_inst[] = {
            { .wave = CHIPSEQ_WAVE_PCM, .pcm = &pcm },
        };
        static const chipseq_cell high_cell[] = { CS_N(G,9, 0,64) };
        static const chipseq_pattern high_pat[] = { { high_cell, 1 } };
        static const chipseq_song high_song = {
            .name = "pcm-large-step", .instruments = pcm_inst, .instrument_count = 1,
            .patterns = high_pat, .pattern_count = 1, .order = one_order, .order_length = 1,
            .loop_order = CHIPSEQ_NO_LOOP, .channels = 1,
            .rows_per_beat = 4, .ticks_per_row = 6, .bpm_q8 = CHIPSEQ_BPM(120),
        };
        chipseq_options opt; chipseq_options_init(&opt);
        opt.sample_rate = 8000; opt.oversample = 1;
        chipseq seq; int16_t samples[2];
        CHECK(chipseq_init(&seq, &opt), "init large-step PCM engine");
        CHECK(chipseq_music_play(&seq, &high_song, false, err, sizeof err),
              "play large-step PCM song");
        chipseq_render_s16(&seq, samples, 2);
        CHECK(samples[0] == 250 && samples[1] == 250 && seq.voices[0].active,
              "large PCM steps remain inside a one-frame loop");
        chipseq_shutdown(&seq);
    }
}

/* ======================================================================== */
/* main                                                                      */
/* ======================================================================== */

int main(void) {
    printf("test_chipseq: running regression suite\n");
    test_validation();
    test_determinism_golden();
    test_f32_identity();
    test_command_queue();
    test_sfx_stealing();
    test_song_frames();
    test_offline_length_and_looping();
    test_bounce_roundtrip();
    test_tick_rate_independence();
    test_block_partition_invariance();
    test_lifecycle_and_numeric_inputs();
    test_synth_regressions();

    if (g_fails) {
        printf("test_chipseq: %d/%d checks FAILED\n", g_fails, g_checks);
        return 1;
    }
    printf("test_chipseq: OK (%d checks passed)\n", g_checks);
    return 0;
}
