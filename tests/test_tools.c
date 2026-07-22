/* test_tools.c -- offline regression tests for the tools TU:
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

static void bput(buf *b, uint8_t v) {
    if (b->n == b->cap) {
        b->cap = b->cap ? b->cap * 2 : 128;
        b->d = realloc(b->d, b->cap);
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

    /* -------- write_c error path -------- */
    err[0] = 0;
    CHECK(!chipseq_song_write_c(NULL, c1_path, "song_test", err, sizeof err),
          "write_c rejects a NULL song");
    CHECK(err[0] != 0, "write_c writes a reason for a NULL song");

    /* -------- free (ASAN/LSAN watches this) -------- */
    chipseq_song_free(song);
    chipseq_song_free(NULL);   /* NULL-safe */

    remove(mid_path);
    remove(c1_path);
    remove(c2_path);

    if (g_fails) {
        printf("test_tools: %d/%d checks FAILED\n", g_fails, g_checks);
        return 1;
    }
    printf("test_tools: OK (%d checks passed)\n", g_checks);
    return 0;
}
