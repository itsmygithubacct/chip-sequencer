/* ThreadSanitizer regression for the documented game-thread/render-thread
 * handoff: commands and claims flow to the renderer while playhead/claim
 * snapshots and the direct mute flag flow back. The library owns no threads;
 * this harness supplies the two foreign threads. */
#include "chip_sequencer.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

static const chipseq_instrument insts[] = {
    { .wave = CHIPSEQ_WAVE_PULSE, .duty = CHIPSEQ_DUTY_50 },
};
static const chipseq_cell cells[] = {
    { 69, 0, 64, CHIPSEQ_FX_NONE, 0 },
};
static const chipseq_pattern patterns[] = { { cells, 1 } };
static const uint8_t order[] = { 0 };
static const chipseq_song song = {
    .name = "thread-snapshot",
    .instruments = insts, .instrument_count = 1,
    .patterns = patterns, .pattern_count = 1,
    .order = order, .order_length = 1,
    .loop_order = 0, .channels = 1,
    .rows_per_beat = 4, .ticks_per_row = 6,
    .bpm_q8 = CHIPSEQ_BPM(120),
};

static chipseq seq;
static atomic_bool done;
static atomic_uint positions_read;
static atomic_uint operations;

static void *render_thread(void *unused) {
    (void)unused;
    int16_t block[32];
    for (unsigned i = 0; i < 20000u; i++)
        chipseq_render_s16(&seq, block, 32);
    atomic_store_explicit(&done, true, memory_order_release);
    return NULL;
}

static void *game_thread(void *unused) {
    (void)unused;
    unsigned iteration = 0u;
    int handle = -1;
    while (!atomic_load_explicit(&done, memory_order_acquire)) {
        uint16_t order_pos, row;
        uint8_t tick;
        if (chipseq_music_position(&seq, &order_pos, &row, &tick))
            atomic_fetch_add_explicit(&positions_read, 1u, memory_order_relaxed);

        switch (iteration & 255u) {
        case 0u:
            (void)chipseq_music_set_volume(
                &seq, (float)(iteration & 127u) * (1.0f / 127.0f));
            break;
        case 32u:
            (void)chipseq_music_set_paused(&seq, true);
            break;
        case 48u:
            (void)chipseq_music_set_paused(&seq, false);
            break;
        case 96u:
            (void)chipseq_music_stop(&seq, 0u);
            break;
        case 112u: {
            char error[96];
            (void)chipseq_music_play(&seq, &song, true, error, sizeof error);
            break;
        }
        case 160u:
            chipseq_set_enabled(&seq, false);
            break;
        case 161u:
            chipseq_set_enabled(&seq, true);
            break;
        default:
            break;
        }

        if ((iteration & 31u) == 0u) {
            if (handle > 0 && chipseq_sfx_active(&seq, handle)) {
                if ((iteration & 64u) != 0u)
                    (void)chipseq_sfx_stop(&seq, handle);
                else
                    (void)chipseq_sfx_set(&seq, handle, 0.5f, 3);
            } else {
                handle = chipseq_sfx_play(&seq, &song, 0.75f, -2, false);
            }
        }
        atomic_fetch_add_explicit(&operations, 1u, memory_order_relaxed);
        iteration++;
    }
    return NULL;
}

int main(void) {
    chipseq_options options;
    chipseq_options_init(&options);
    if (!chipseq_init(&seq, &options)) return 1;

    char err[96];
    if (!chipseq_music_play(&seq, &song, true, err, sizeof err)) {
        fprintf(stderr, "music_play: %s\n", err);
        return 1;
    }
    /* Drain and publish once before handing render ownership to the worker. */
    int16_t first;
    chipseq_render_s16(&seq, &first, 1);

    atomic_init(&done, false);
    atomic_init(&positions_read, 0u);
    atomic_init(&operations, 0u);
    pthread_t renderer, game;
    if (pthread_create(&renderer, NULL, render_thread, NULL) != 0 ||
        pthread_create(&game, NULL, game_thread, NULL) != 0)
        return 1;
    if (pthread_join(renderer, NULL) != 0 || pthread_join(game, NULL) != 0)
        return 1;

    unsigned reads = atomic_load_explicit(&positions_read, memory_order_relaxed);
    unsigned ops = atomic_load_explicit(&operations, memory_order_relaxed);
    chipseq_shutdown(&seq);
    if (reads == 0 || ops == 0) {
        fprintf(stderr, "thread game-side worker made no progress\n");
        return 1;
    }
    printf("test_thread: OK (%u coherent snapshots, %u control operations)\n",
           reads, ops);
    return 0;
}
