#include "chip_sequencer.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

enum { BLOCK_FRAMES = 512, TOTAL_FRAMES = 1048576 };

static const uint8_t wavetable[CHIPSEQ_WAVETABLE_LEN] = {
    8, 10, 12, 14, 15, 14, 12, 10, 8, 6, 4, 2, 0, 2, 4, 6,
    8, 9, 11, 13, 15, 13, 11, 9, 8, 7, 5, 3, 1, 3, 5, 7,
};

static const int16_t pcm_frames[] = {
    0, 6393, 12539, 18204, 23170, 27245, 30273, 32137,
    32767, 32137, 30273, 27245, 23170, 18204, 12539, 6393,
    0, -6393, -12539, -18204, -23170, -27245, -30273, -32137,
    -32768, -32137, -30273, -27245, -23170, -18204, -12539, -6393,
};

static const chipseq_pcm pcm = {
    pcm_frames, (uint32_t)(sizeof pcm_frames / sizeof pcm_frames[0]),
    0u, (uint32_t)(sizeof pcm_frames / sizeof pcm_frames[0]), 69u,
};

static const chipseq_instrument instruments[] = {
    { .wave = CHIPSEQ_WAVE_PULSE, .duty = CHIPSEQ_DUTY_12 },
    { .wave = CHIPSEQ_WAVE_PULSE, .duty = CHIPSEQ_DUTY_50 },
    { .wave = CHIPSEQ_WAVE_TRIANGLE, .tri_steps = 32u },
    { .wave = CHIPSEQ_WAVE_SAW },
    { .wave = CHIPSEQ_WAVE_NOISE, .noise_mode = CHIPSEQ_NOISE_LONG },
    { .wave = CHIPSEQ_WAVE_NOISE, .noise_mode = CHIPSEQ_NOISE_SHORT },
    { .wave = CHIPSEQ_WAVE_WAVETABLE, .wavetable = wavetable },
    { .wave = CHIPSEQ_WAVE_PCM, .pcm = &pcm },
};

static const chipseq_cell music_cells[] = {
    { 48u, 0u, 48u, CHIPSEQ_FX_NONE, 0u },
    { 52u, 1u, 44u, CHIPSEQ_FX_NONE, 0u },
    { 36u, 2u, 52u, CHIPSEQ_FX_NONE, 0u },
    { 55u, 3u, 40u, CHIPSEQ_FX_NONE, 0u },
    { 40u, 4u, 38u, CHIPSEQ_FX_NONE, 0u },
    { 64u, 5u, 32u, CHIPSEQ_FX_NONE, 0u },
    { 59u, 6u, 36u, CHIPSEQ_FX_NONE, 0u },
    { 69u, 7u, 40u, CHIPSEQ_FX_NONE, 0u },
};
static const chipseq_pattern music_patterns[] = {
    { music_cells, 1u },
};
static const uint8_t music_order[] = { 0u };
static const chipseq_song music_song = {
    .name = "benchmark-music",
    .instruments = instruments,
    .instrument_count =
        (uint16_t)(sizeof instruments / sizeof instruments[0]),
    .patterns = music_patterns,
    .pattern_count = 1u,
    .order = music_order,
    .order_length = 1u,
    .loop_order = 0u,
    .channels = 8u,
    .rows_per_beat = 4u,
    .ticks_per_row = 6u,
    .bpm_q8 = CHIPSEQ_BPM(140),
};

static const chipseq_cell sfx_cells[] = {
    { 76u, 0u, 48u, CHIPSEQ_FX_NONE, 0u },
    { 43u, 4u, 44u, CHIPSEQ_FX_NONE, 0u },
};
static const chipseq_pattern sfx_patterns[] = {
    { sfx_cells, 1u },
};
static const uint8_t sfx_order[] = { 0u };
static const chipseq_song sfx_song = {
    .name = "benchmark-sfx",
    .instruments = instruments,
    .instrument_count =
        (uint16_t)(sizeof instruments / sizeof instruments[0]),
    .patterns = sfx_patterns,
    .pattern_count = 1u,
    .order = sfx_order,
    .order_length = 1u,
    .loop_order = 0u,
    .channels = 2u,
    .rows_per_beat = 4u,
    .ticks_per_row = 6u,
    .bpm_q8 = CHIPSEQ_BPM(180),
};

static uint64_t nanoseconds(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0u;
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) +
           (uint64_t)now.tv_nsec;
}

static double benchmark_idle(uint64_t *checksum)
{
    chipseq_options options;
    chipseq seq;
    float block[BLOCK_FRAMES] = {0};

    chipseq_options_init(&options);
    if (!chipseq_init(&seq, &options)) return -1.0;
    for (unsigned warm = 0u; warm < 16u; ++warm)
        chipseq_render_f32(&seq, block, BLOCK_FRAMES);
    const uint64_t start = nanoseconds();
    for (size_t done = 0u; done < TOTAL_FRAMES; done += BLOCK_FRAMES)
        chipseq_render_f32(&seq, block, BLOCK_FRAMES);
    const uint64_t elapsed = nanoseconds() - start;
    *checksum += (uint64_t)(block[0] == 0.0f);
    chipseq_shutdown(&seq);
    return (double)elapsed / (double)TOTAL_FRAMES;
}

static double benchmark_render(uint8_t oversample, bool stereo, bool full,
                               uint64_t *checksum)
{
    chipseq_options options;
    chipseq seq;
    int16_t block[BLOCK_FRAMES * 2u];
    char error[128];
    bool okay = true;

    chipseq_options_init(&options);
    options.oversample = oversample;
    if (!chipseq_init(&seq, &options)) return -1.0;
    if (!chipseq_music_play(&seq, &music_song, true, error, sizeof error))
        okay = false;
    if (okay && full) {
        for (unsigned slot = 0u; slot < CHIPSEQ_SFX_SLOTS; ++slot) {
            if (chipseq_sfx_play(&seq, &sfx_song, 1.0f,
                                 (int)slot * 2, true) <= 0) {
                okay = false;
                break;
            }
        }
    }
    if (!okay) {
        chipseq_shutdown(&seq);
        return -1.0;
    }
    if (stereo)
        chipseq_render_s16_stereo(&seq, block, BLOCK_FRAMES);
    else
        chipseq_render_s16(&seq, block, BLOCK_FRAMES);
    for (unsigned warm = 0u; warm < 16u; ++warm) {
        if (stereo)
            chipseq_render_s16_stereo(&seq, block, BLOCK_FRAMES);
        else
            chipseq_render_s16(&seq, block, BLOCK_FRAMES);
    }
    const uint64_t start = nanoseconds();
    for (size_t done = 0u; done < TOTAL_FRAMES; done += BLOCK_FRAMES) {
        if (stereo)
            chipseq_render_s16_stereo(&seq, block, BLOCK_FRAMES);
        else
            chipseq_render_s16(&seq, block, BLOCK_FRAMES);
        *checksum += (uint16_t)block[(done / BLOCK_FRAMES) %
                                    (stereo ? BLOCK_FRAMES * 2u : BLOCK_FRAMES)];
    }
    const uint64_t elapsed = nanoseconds() - start;
    chipseq_shutdown(&seq);
    return (double)elapsed / (double)TOTAL_FRAMES;
}

static double benchmark_commands(uint64_t *checksum)
{
    enum { BATCHES = 16384, COMMANDS = 63 };
    chipseq_options options;
    chipseq seq;

    chipseq_options_init(&options);
    if (!chipseq_init(&seq, &options)) return -1.0;
    const uint64_t start = nanoseconds();
    for (unsigned batch = 0u; batch < BATCHES; ++batch) {
        for (unsigned index = 0u; index < COMMANDS; ++index) {
            if (!chipseq_music_set_volume(
                    &seq, (float)(index & 255u) * (1.0f / 255.0f))) {
                chipseq_shutdown(&seq);
                return -1.0;
            }
        }
        chipseq_flush_commands(&seq);
    }
    const uint64_t elapsed = nanoseconds() - start;
    *checksum += seq.volume_q8;
    chipseq_shutdown(&seq);
    return (double)elapsed / (double)(BATCHES * COMMANDS);
}

static double benchmark_validation(uint64_t *checksum)
{
    enum { CALLS = 262144 };
    char error[64];
    unsigned valid = 0u;

    const uint64_t start = nanoseconds();
    for (unsigned call = 0u; call < CALLS; ++call)
        valid += chipseq_song_validate(&music_song, error, sizeof error) ? 1u : 0u;
    const uint64_t elapsed = nanoseconds() - start;
    *checksum += valid;
    return (double)elapsed / (double)CALLS;
}

int main(void)
{
    uint64_t checksum = 0u;

    (void)printf("idle_f32_ov2_ns_per_frame=%.3f\n",
                 benchmark_idle(&checksum));
    (void)printf("music8_mono_ov1_ns_per_frame=%.3f\n",
                 benchmark_render(1u, false, false, &checksum));
    (void)printf("music8_mono_ov2_ns_per_frame=%.3f\n",
                 benchmark_render(2u, false, false, &checksum));
    (void)printf("music8_mono_ov4_ns_per_frame=%.3f\n",
                 benchmark_render(4u, false, false, &checksum));
    (void)printf("music8_stereo_ov2_ns_per_frame=%.3f\n",
                 benchmark_render(2u, true, false, &checksum));
    (void)printf("full16_mono_ov2_ns_per_frame=%.3f\n",
                 benchmark_render(2u, false, true, &checksum));
    (void)printf("full16_stereo_ov2_ns_per_frame=%.3f\n",
                 benchmark_render(2u, true, true, &checksum));
    (void)printf("command_ns_per_call=%.3f\n",
                 benchmark_commands(&checksum));
    (void)printf("validation_ns_per_call=%.3f\n",
                 benchmark_validation(&checksum));
    (void)printf("checksum=%" PRIu64 "\n", checksum);
    return EXIT_SUCCESS;
}
