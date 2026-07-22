/* demo.c -- minimal chip-sequencer example: build a tiny song in C literals,
 * bounce it offline, and drive a couple of live render blocks. Prints a short
 * summary. (No audio device is opened; chip-sequencer is a source, not a sink.)
 */
#include "chip_sequencer.h"

#include <stdio.h>
#include <stdlib.h>

/* tracker-screen cell macros (author-side conveniences) */
#define CS__                   { CHIPSEQ_NOTE_NONE, 0, CHIPSEQ_VOL_NONE, CHIPSEQ_FX_NONE, 0 }
#define CS_OFF                 { CHIPSEQ_NOTE_OFF,  0, CHIPSEQ_VOL_NONE, CHIPSEQ_FX_NONE, 0 }
#define CS_N(pc,oct,i,v)       { CHIPSEQ_NOTE(CHIPSEQ_PC_##pc,oct), (i), (v), CHIPSEQ_FX_NONE, 0 }
#define CS_NF(pc,oct,i,v,fx,p) { CHIPSEQ_NOTE(CHIPSEQ_PC_##pc,oct), (i), (v), CHIPSEQ_FX_##fx, (p) }

static const int8_t env_pluck[] = { 64, 60, 52, 44, 38, 32, 28, 24, 20, 16, 12, 8, 4, 0 };
static const chipseq_seq pluck_vol = {
    env_pluck, 14, CHIPSEQ_SEQ_NO_LOOP, CHIPSEQ_SEQ_NO_RELEASE
};
static const int8_t arp_maj[] = { 0, 4, 7 };
static const chipseq_seq lead_arp = { arp_maj, 3, 0, CHIPSEQ_SEQ_NO_RELEASE };

static const int8_t env_kick[] = { 64, 58, 40, 20, 6, 0 };
static const chipseq_seq kick_vol = {
    env_kick, 6, CHIPSEQ_SEQ_NO_LOOP, CHIPSEQ_SEQ_NO_RELEASE
};
static const int8_t kick_pitch[] = { 24, 8, 0, -8 };
static const chipseq_seq kick_pitch_seq = {
    kick_pitch, 4, CHIPSEQ_SEQ_NO_LOOP, CHIPSEQ_SEQ_NO_RELEASE
};

static const chipseq_instrument insts[] = {
    { .name = "lead", .wave = CHIPSEQ_WAVE_PULSE, .duty = CHIPSEQ_DUTY_25,
      .vol_seq = &pluck_vol, .arp_seq = &lead_arp },
    { .name = "bass", .wave = CHIPSEQ_WAVE_TRIANGLE, .tri_steps = 32, .transpose = -12 },
    { .name = "kick", .wave = CHIPSEQ_WAVE_NOISE, .noise_mode = CHIPSEQ_NOISE_LONG,
      .vol_seq = &kick_vol, .pitch_seq = &kick_pitch_seq },
};

static const chipseq_cell p0[] = {
    CS_N(C,5, 0,48),               CS_N(C,3, 1,56),  CS_N(C,2, 2,64),
    CS__,                          CS__,             CS__,
    CS_N(E,5, 0,48),               CS__,             CS_N(C,2, 2,48),
    CS__,                          CS_N(G,3, 1,56),  CS__,
    CS_N(G,5, 0,48),               CS__,             CS_N(C,2, 2,64),
    CS_NF(A,5, 0,48, VIBRATO, 0x38), CS__,           CS__,
    CS_N(E,5, 0,40),               CS_N(C,3, 1,56),  CS_N(C,2, 2,48),
    CS_OFF,                        CS__,             CS__,
};
static const chipseq_pattern pats[] = { { p0, 8 } };
static const uint8_t order[] = { 0, 0 };

static const chipseq_song song_demo = {
    .name = "demo",
    .instruments = insts, .instrument_count = 3,
    .patterns = pats,     .pattern_count = 1,
    .order = order,       .order_length = 2,
    .loop_order = CHIPSEQ_NO_LOOP, .channels = 3,
    .rows_per_beat = 4,   .ticks_per_row = 6,
    .bpm_q8 = CHIPSEQ_BPM(140),
};

int main(void) {
    char err[128];
    if (!chipseq_song_validate(&song_demo, err, sizeof err)) {
        fprintf(stderr, "validate: %s\n", err);
        return 1;
    }

    chipseq_options opt;
    chipseq_options_init(&opt);
    opt.sample_rate = 44100;

    uint64_t frames = chipseq_song_frames(&song_demo, opt.sample_rate);
    printf("song '%s': %llu frames at %u Hz\n",
           song_demo.name, (unsigned long long)frames, opt.sample_rate);

    size_t n = 0;
    int16_t *pcm = chipseq_render_song(&song_demo, &opt, 0, &n, err, sizeof err);
    if (!pcm) { fprintf(stderr, "render_song: %s\n", err); return 1; }
    int32_t peak = 0;
    for (size_t i = 0; i < n; i++) {
        int32_t a = pcm[i] < 0 ? -pcm[i] : pcm[i];
        if (a > peak) peak = a;
    }
    printf("rendered %zu frames, peak amplitude %d\n", n, peak);
    chipseq_pcm_free(pcm);

    /* bounce the whole song to a canonical PCM/mono/16-bit WAV -- the offline
     * deliverable a game can ship or a user can play in any audio tool. */
    const char *wav_path = "demo.wav";
    if (!chipseq_bounce_wav(&song_demo, &opt, wav_path, 0, err, sizeof err)) {
        fprintf(stderr, "bounce_wav: %s\n", err);
        return 1;
    }
    printf("bounced song to %s (%u Hz, mono, 16-bit)\n", wav_path, opt.sample_rate);

    /* live path: drive the generator seam a few blocks */
    chipseq seq;
    if (!chipseq_init(&seq, &opt)) { fprintf(stderr, "init failed\n"); return 1; }
    if (!chipseq_music_play(&seq, &song_demo, true, err, sizeof err)) {
        fprintf(stderr, "music_play: %s\n", err);
        chipseq_shutdown(&seq);
        return 1;
    }
    float block[512];
    for (int b = 0; b < 4; b++) {
        for (int i = 0; i < 512; i++) block[i] = 0.0f;
        chipseq_render_f32(&seq, block, 512);   /* == pcmmix generator contract */
    }
    uint16_t op = 0, row = 0;
    uint8_t tick = 0;
    if (chipseq_music_position(&seq, &op, &row, &tick))
        printf("live playhead: order %u row %u tick %u\n", op, row, tick);
    chipseq_shutdown(&seq);

    printf("demo OK\n");
    return 0;
}
