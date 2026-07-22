/* chip_sequencer.h — deterministic chiptune synth + pattern sequencer.
 *
 * A SOURCE of audio, not a transport. It converts compact in-source song data
 * (patterns of note cells + table-driven instruments) into 16-bit PCM using a
 * small fixed-point chip synth (pulse/triangle/saw/noise/wavetable/PCM). Its
 * float renderer has the exact signature of pcm-mixer's generator callback, so
 * the whole integration is pcmmix_set_generator(&mix, chipseq_generator, &seq).
 *
 * It owns no thread, no device, no pipe, and does NOT mix foreign clips,
 * resample foreign WAVs, pace a transport, or crossfade tracks -- pcm-mixer
 * does all of that. Queued control calls run on the game thread and are applied
 * at block boundaries through a lock-free single-producer/single-consumer
 * queue; the master enable flag is a direct atomic control. Both are safe while
 * a mixer thread renders.
 *
 * Determinism is a hard invariant: the entire signal path is integer fixed
 * point with zero libm calls; float output is defined as int16 * (1/32768).
 * Same song + same options + same sample rate = identical signed PCM sample
 * values on every supported target. Serialized PCM/WAV bytes are little-endian.
 *
 * Language: ISO C11 (C translation units only). Dependencies: the C11
 * standard library only. Supported targets provide 32-bit always-lock-free
 * atomic_uint and binary float.
 * MIT licensed.
 */
#ifndef CHIP_SEQUENCER_H
#define CHIP_SEQUENCER_H

#ifdef __cplusplus
#error "chip-sequencer is a C11-only header; compile it in a C translation unit"
#else

#include <float.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The render callback's lock-free guarantee is part of the API, not a hopeful
 * property of a particular compiler runtime. All cross-thread state therefore
 * uses atomic_uint, and unsupported targets fail at compile time instead of
 * silently lowering an atomic operation to a hidden lock/libatomic call. */
#if UINT_MAX != UINT32_MAX
#error "chip-sequencer requires a 32-bit unsigned int"
#endif
#if ATOMIC_INT_LOCK_FREE != 2
#error "chip-sequencer requires always-lock-free 32-bit atomics"
#endif
#if FLT_RADIX != 2 || FLT_MANT_DIG < 16
#error "chip-sequencer requires binary float with at least 16 mantissa bits"
#endif

#define CHIPSEQ_VERSION_MAJOR 0
#define CHIPSEQ_VERSION_MINOR 2
#define CHIPSEQ_VERSION_PATCH 0

/* --- hard capacity limits baked into the object (nothing allocates) ------ */
#define CHIPSEQ_CHANNELS_MAX      8u   /* music channels */
#define CHIPSEQ_SFX_SLOTS         4u   /* concurrent SFX songs */
#define CHIPSEQ_SFX_CHANNELS_MAX  2u   /* channels per SFX song */
#define CHIPSEQ_VOICES_MAX \
    (CHIPSEQ_CHANNELS_MAX + CHIPSEQ_SFX_SLOTS * CHIPSEQ_SFX_CHANNELS_MAX)
#define CHIPSEQ_CMD_QUEUE         64u  /* control-command ring depth */
#define CHIPSEQ_WAVETABLE_LEN     32u  /* nibbles per wavetable */
#define CHIPSEQ_SEQ_LEN_MAX       256u /* steps per sequence */

/* --- note encoding ------------------------------------------------------- */
/* Pitch classes 0..11; CHIPSEQ_NOTE(pc,oct) = pc + 12*(oct+1); A4 == 69. */
#define CHIPSEQ_PC_C  0u
#define CHIPSEQ_PC_Cs 1u
#define CHIPSEQ_PC_D  2u
#define CHIPSEQ_PC_Ds 3u
#define CHIPSEQ_PC_E  4u
#define CHIPSEQ_PC_F  5u
#define CHIPSEQ_PC_Fs 6u
#define CHIPSEQ_PC_G  7u
#define CHIPSEQ_PC_Gs 8u
#define CHIPSEQ_PC_A  9u
#define CHIPSEQ_PC_As 10u
#define CHIPSEQ_PC_B  11u
#define CHIPSEQ_NOTE(pc,oct) ((uint8_t)((pc) + 12 * ((oct) + 1)))

/* cell note sentinels (a real note is 0..127) */
#define CHIPSEQ_NOTE_NONE 0xFFu  /* no note this cell */
#define CHIPSEQ_NOTE_OFF  0xFEu  /* note-off: enter release phase */
#define CHIPSEQ_NOTE_CUT  0xFDu  /* note-cut: silence immediately */
#define CHIPSEQ_VOL_NONE  0xFFu  /* leave running volume unchanged */

/* duty presets in 1/64ths of a period (any 1..63 is legal) */
#define CHIPSEQ_DUTY_12   8u
#define CHIPSEQ_DUTY_25   16u
#define CHIPSEQ_DUTY_50   32u
#define CHIPSEQ_DUTY_75   48u

/* sequence sentinels */
#define CHIPSEQ_SEQ_NO_LOOP    ((int16_t)-1)
#define CHIPSEQ_SEQ_NO_RELEASE ((int16_t)-1)

/* order / tempo helpers */
#define CHIPSEQ_NO_LOOP        0xFFFFu
#define CHIPSEQ_BPM(b)         ((uint16_t)((b) << 8))
#define CHIPSEQ_BPM_Q8(b,frac) ((uint16_t)(((b) << 8) | ((frac) & 0xFF)))
#define CHIPSEQ_BPM_MIN        (CHIPSEQ_BPM(20))

/* ======================================================================== */
/* enums                                                                    */
/* ======================================================================== */

typedef enum chipseq_wave {
    CHIPSEQ_WAVE_PULSE = 0,
    CHIPSEQ_WAVE_TRIANGLE,
    CHIPSEQ_WAVE_SAW,
    CHIPSEQ_WAVE_NOISE,
    CHIPSEQ_WAVE_WAVETABLE,
    CHIPSEQ_WAVE_PCM,
    CHIPSEQ_WAVE_COUNT
} chipseq_wave;

typedef enum chipseq_noise_mode {
    CHIPSEQ_NOISE_LONG = 0,   /* bit0 ^ bit1, ~32767-step hiss */
    CHIPSEQ_NOISE_SHORT       /* bit0 ^ bit6, 93/31-step metallic */
} chipseq_noise_mode;

typedef enum chipseq_fx {
    CHIPSEQ_FX_NONE = 0,
    CHIPSEQ_FX_ARPEGGIO,
    CHIPSEQ_FX_PORTA_UP,
    CHIPSEQ_FX_PORTA_DOWN,
    CHIPSEQ_FX_TONE_PORTA,
    CHIPSEQ_FX_VIBRATO,
    CHIPSEQ_FX_TREMOLO,
    CHIPSEQ_FX_VOL_SLIDE,
    CHIPSEQ_FX_PAN,
    CHIPSEQ_FX_DUTY,
    CHIPSEQ_FX_RETRIGGER,
    CHIPSEQ_FX_NOTE_DELAY,
    CHIPSEQ_FX_NOTE_CUT,
    CHIPSEQ_FX_SPEED,
    CHIPSEQ_FX_TEMPO,
    CHIPSEQ_FX_ORDER_JUMP,
    CHIPSEQ_FX_PATTERN_BREAK,
    CHIPSEQ_FX_COUNT
} chipseq_fx;

typedef enum chipseq_mix_mode {
    CHIPSEQ_MIX_LINEAR = 0,   /* per-channel linear sum (default, game-friendly) */
    CHIPSEQ_MIX_NES           /* nonlinear pulse+TND lookup tables (console color) */
} chipseq_mix_mode;

/* ======================================================================== */
/* song data model (all caller-owned; the engine never copies or frees it)  */
/* ======================================================================== */

/* A per-tick int8 step table driving volume / arpeggio / pitch / duty.
 * Advanced one index per sequencer tick. `loop` is the index to wrap to at
 * the end (or CHIPSEQ_SEQ_NO_LOOP to hold the last value); `release` is the
 * index to jump to on a note-off (or CHIPSEQ_SEQ_NO_RELEASE). */
typedef struct chipseq_seq {
    const int8_t *values;
    uint16_t length;
    int16_t  loop;
    int16_t  release;
} chipseq_seq;

/* Caller-owned PCM instrument body: 16-bit signed mono frames, played back
 * resampled so that note == root_note plays at the natural rate. loop_start ==
 * loop_end disables looping. */
typedef struct chipseq_pcm {
    const int16_t *frames;
    uint32_t frame_count;
    uint32_t loop_start;
    uint32_t loop_end;
    uint8_t  root_note;
} chipseq_pcm;

/* An instrument. Only the fields relevant to `wave` are read. Sequence and
 * table pointers may be NULL (meaning "no modulation of that kind"). */
typedef struct chipseq_instrument {
    const char *name;              /* optional, for tools/debug only */
    uint8_t wave;                  /* chipseq_wave */
    uint8_t duty;                  /* pulse/saw control, 0 = default; else 1..63 */
    uint8_t noise_mode;            /* chipseq_noise_mode */
    uint8_t tri_steps;             /* triangle quantization; 0 = smooth, else 4..255 */
    const chipseq_seq *vol_seq;    /* 0..64 gain per tick */
    const chipseq_seq *arp_seq;    /* signed semitone offset per tick */
    const chipseq_seq *pitch_seq;  /* signed 1/16-semitone detune per tick */
    const chipseq_seq *duty_seq;   /* absolute duty per tick */
    const uint8_t *wavetable;      /* CHIPSEQ_WAVETABLE_LEN nibbles, 0..15 */
    const chipseq_pcm *pcm;        /* for CHIPSEQ_WAVE_PCM */
    int8_t   transpose;            /* semitones added to every note */
    int16_t  finetune;            /* 1/16-semitone constant detune */
    uint8_t  vib_speed;            /* per-instrument auto-vibrato (0 = off) */
    uint8_t  vib_depth;
    uint8_t  vib_delay;            /* ticks before auto-vibrato begins */
} chipseq_instrument;

typedef struct chipseq_cell {
    uint8_t note;  /* 0..127, or CHIPSEQ_NOTE_NONE/OFF/CUT */
    uint8_t inst;  /* instrument index (used only when note is a real note) */
    uint8_t vol;   /* 0..64, or CHIPSEQ_VOL_NONE */
    uint8_t fx;    /* chipseq_fx */
    uint8_t fxp;   /* effect-specific parameter; see README "Effect parameters" */
} chipseq_cell;

/* Row-major cell grid: cells[row * song->channels + channel]. */
typedef struct chipseq_pattern {
    const chipseq_cell *cells;
    uint16_t rows;
} chipseq_pattern;

typedef struct chipseq_song {
    const char *name;
    const chipseq_instrument *instruments;
    uint16_t instrument_count;
    const chipseq_pattern *patterns;
    uint16_t pattern_count;
    const uint8_t *order;          /* pattern indices to play in sequence */
    uint16_t order_length;
    uint16_t loop_order;           /* order index to loop to, or CHIPSEQ_NO_LOOP */
    uint8_t  channels;             /* 1..CHIPSEQ_CHANNELS_MAX (music) / SFX max */
    uint8_t  rows_per_beat;
    uint8_t  ticks_per_row;
    uint16_t bpm_q8;               /* Q8.8 BPM */
} chipseq_song;

/* ======================================================================== */
/* engine options + object                                                  */
/* ======================================================================== */

typedef struct chipseq_options {
    uint32_t sample_rate; /* output frames/sec, 8000..192000; default 44100.
                             MUST equal the pcm-mixer's sample_rate. */
    uint8_t  oversample;  /* 1, 2 (default) or 4; part of the byte contract */
    uint8_t  mix_mode;    /* chipseq_mix_mode; default CHIPSEQ_MIX_LINEAR */
    float    volume;      /* master gain 0..1, quantized to 1/256; default 1.0 */
    float    sfx_duck;    /* music gain while any SFX plays, 0..1; default 1.0 */
    uint16_t lowpass;     /* one-pole cutoff hint 0..65535 (0 = off); default 0 */
} chipseq_options;

/* --- internal state: treat every field below as private ------------------ */
/* Declared in the header only so the caller can stack-allocate a chipseq,
 * exactly like pcmmix. Never read or write these fields. */

typedef struct chipseq_voice {   /* private */
    uint32_t phase, phase_inc;
    uint32_t lfsr;
    const chipseq_instrument *inst;
    uint16_t seq_pos[4];
    uint32_t note_ticks;         /* ticks elapsed since the last trigger */
    int32_t  cur_pitch;          /* fixed-point running pitch */
    uint64_t pcm_pos;            /* Q16 position into PCM frames; bounds are
                                    checked in 64-bit before narrowing an index */
    uint8_t  note, vol, duty;
    uint8_t  active, released, pan;
} chipseq_voice;

typedef struct chipseq_track {   /* private: one music/sfx playhead */
    const chipseq_song *song;
    uint64_t tick_accum;         /* remainder accumulator */
    uint64_t samples_per_tick;   /* Q32 */
    uint16_t order_pos, row;
    uint8_t  tick, ticks_per_row;
    uint16_t bpm_q8;
    uint16_t fade_ticks, fade_left;
    uint32_t generation;
    uint8_t  channels, first_voice;
    uint8_t  active, looping, paused;
} chipseq_track;

typedef struct chipseq_cmd {     /* private: one queued control command */
    uint8_t  op;
    uint8_t  slot;
    uint16_t pad0;
    uint32_t generation;
    int32_t  ia, ib;
    float    fa;
    const chipseq_song *song;
    uint8_t  loop, pad[3];
} chipseq_cmd;

/* NB: SFX liveness is not included in the playhead snapshot. It is carried by
 * the per-slot atomic claim words (sfx_claim[], below), which are the
 * authoritative cross-thread SFX state; a snapshot mask would only ever be
 * one block stale. */

typedef struct chipseq {
    /* private: use the API below. */
    uint32_t sample_rate, oversample, out_rate;
    uint8_t  mix_mode;
    uint32_t volume_q8, duck_q8;   /* quantized to 1/256 */
    uint16_t lowpass;
    int32_t  note_inc[128];        /* precomputed phase increments */
    chipseq_voice voices[CHIPSEQ_VOICES_MAX];
    chipseq_track music;
    chipseq_track sfx[CHIPSEQ_SFX_SLOTS]; /* render-thread-private working state */
    uint16_t voice_generation;
    /* render-thread-private streaming filter state. These PERSIST across render
     * blocks so the halfband decimation FIR and the one-pole lowpass are true
     * streaming filters: the signed sample sequence for a
     * (song, options, sample_rate) is identical regardless of block size, and
     * no transient is injected at block boundaries. Zeroed once at
     * chipseq_init; never reset per block. */
    int32_t dec_z0[15], dec_z1[15];              /* mono decimation delay lines */
    int32_t dec_z0l[15], dec_z1l[15];            /* stereo L decimation lines   */
    int32_t dec_z0r[15], dec_z1r[15];            /* stereo R decimation lines   */
    int32_t lp_mono, lp_l, lp_r;                 /* one-pole lowpass accumulators */
    /* Per-SFX-slot atomic claim word: state in bits 31..30 and a non-wrapping
     * 29-bit generation in bits 28..0. Slots retire on generation exhaustion,
     * so a stale public handle cannot become valid again during one engine
     * lifetime. Claim ordinals are game-thread-private: the render thread never
     * reads them, so they need no atomic operation. */
    atomic_uint sfx_claim[CHIPSEQ_SFX_SLOTS];
    uint64_t sfx_claim_next;
    uint64_t sfx_claim_ordinal[CHIPSEQ_SFX_SLOTS];
    /* lock-free SPSC command ring (game thread -> render thread) */
    chipseq_cmd  queue[CHIPSEQ_CMD_QUEUE];
    atomic_uint queue_head;   /* producer (game thread) */
    atomic_uint queue_tail;   /* consumer (render thread) */
    /* Coherent render-thread-published playhead. Sequentially consistent atomic
     * payload words make this seqlock data-race-free; snap_seq is odd only
     * during the bounded publish. */
    atomic_uint snap_seq;
    atomic_uint snap_position; /* high 16: row, low 16: order_pos */
    atomic_uint snap_state;    /* bit 8: music_active, low 8: tick */
    /* Master mute: a runtime control, so an atomic flag written directly by the
     * game thread (NOT via the command queue) and loaded by both threads. */
    atomic_uint enabled;
    bool offline, initialized;     /* write-once at init; read-only afterward */
} chipseq;

/* ======================================================================== */
/* lifecycle                                                                */
/* ======================================================================== */

/* Fill *options with defaults documented above. */
void chipseq_options_init(chipseq_options *options);

/* Initialize a chipseq. The object may hold any prior contents; it is fully
 * (re)initialized. Pass NULL options for defaults. Precomputes note tables.
 * Returns false (object left in a safe, uninitialized state) when an option is
 * out of range. Allocates nothing; there is no matching free of memory, only
 * chipseq_shutdown() to reset state. */
bool chipseq_init(chipseq *seq, const chipseq_options *options);

/* Reset a chipseq to a stopped, empty state. After this returns, nothing
 * touches the object or any song/PCM memory again. Idempotent. */
void chipseq_shutdown(chipseq *seq);

/* Master mute. Writes the atomic `enabled` flag DIRECTLY -- it does not route
 * through the command queue -- so the store is visible at once: the very next
 * render block silences all voices, and new play/sfx calls are rejected until
 * re-enabled. Works even when the command ring is full. chipseq_is_enabled
 * loads the same atomic. Thread-safe against the render thread. */
void chipseq_set_enabled(chipseq *seq, bool on);
bool chipseq_is_enabled(const chipseq *seq);

/* ======================================================================== */
/* song inspection (pure; no engine state)                                  */
/* ======================================================================== */

/* Validate every order entry, instrument reference, sequence loop/release
 * index and value range, waveform parameter/table, PCM body/loop point, cell
 * value/effect, tempo field, and channel count. On the first problem writes a
 * message naming the offending pattern/row/channel/instrument into err (when
 * err_len > 0) and returns false. music/sfx play call this internally. */
bool chipseq_song_validate(const chipseq_song *song, char *err, size_t err_len);

/* Exact number of output frames the song renders at sample rate `sr`
 * (8000..192000),
 * including FX_SPEED/FX_TEMPO, up to (and not past) its loop point; returns
 * UINT64_MAX for an infinitely-looping song or an unrepresentable finite
 * length. Uses the same fixed-point tick math as the renderer, so it is exact,
 * not an estimate. */
uint64_t chipseq_song_frames(const chipseq_song *song, uint32_t sr);

/* ======================================================================== */
/* music transport (game thread; applied at the next block boundary)        */
/* ======================================================================== */

/* Start playing `song` as music. Validates first; returns false with a reason
 * in err on invalid song. `loop` overrides the song's loop_order intent for
 * this playback. Replaces any current music (with no crossfade -- fade the old
 * one out first if you want one). */
bool chipseq_music_play(chipseq *seq, const chipseq_song *song, bool loop,
                        char *err, size_t err_len);

/* Fade music out over fade_ticks sequencer ticks (0 = stop immediately). */
bool chipseq_music_stop(chipseq *seq, uint16_t fade_ticks);

/* Live music gain, 0..1. Non-finite/out-of-range values are rejected. */
bool chipseq_music_set_volume(chipseq *seq, float volume);

/* Pause/resume music (voices hold; the playhead freezes). */
bool chipseq_music_set_paused(chipseq *seq, bool paused);

/* Read the last-published playhead position (may be one block stale, never
 * torn). Any out pointer may be NULL. Returns false when no music is playing
 * or when a bounded read cannot beat a concurrent publication. */
bool chipseq_music_position(const chipseq *seq, uint16_t *order_pos,
                            uint16_t *row, uint8_t *tick);

/* ======================================================================== */
/* SFX (a small pool of song-based one-shots / loops)                       */
/* ======================================================================== */

/* Play `song` (<= CHIPSEQ_SFX_CHANNELS_MAX channels) on a free SFX slot.
 * `vol` 0..1, `transpose` semitones, `loop` for engine-style held sounds.
 * Returns a handle > 0, or -1 when disabled / invalid song / no free slot.
 * When all slots are busy the oldest NON-LOOPING slot is stolen; looping slots
 * are never stolen. Handles are generation-checked. */
int  chipseq_sfx_play(chipseq *seq, const chipseq_song *song, float vol,
                      int transpose, bool loop);

/* Live-adjust a playing SFX (rev an engine loop). Stale handle -> false. */
bool chipseq_sfx_set(chipseq *seq, int handle, float vol, int transpose);

/* Stop one SFX / all SFX. A false stop-all result means the command queue is
 * full (or the engine is not initialized), so the caller can retry. */
bool chipseq_sfx_stop(chipseq *seq, int handle);
bool chipseq_sfx_stop_all(chipseq *seq);

/* True while the handle still names a live SFX. */
bool chipseq_sfx_active(const chipseq *seq, int handle);

/* ======================================================================== */
/* rendering (render thread; the seam with pcm-mixer)                       */
/* ======================================================================== */

/* Render `frames` frames, OVERWRITING dst. Drains the command queue at entry.
 * Mono. */
void chipseq_render_s16(chipseq *seq, int16_t *dst, size_t frames);

/* As above, interleaved L,R stereo (respects FX_PAN). dst holds 2*frames. */
void chipseq_render_s16_stereo(chipseq *seq, int16_t *dst, size_t frames);

/* Render `frames` frames and ADD them to dst (dst is NOT cleared), exactly
 * s16 * (1.0f/32768.0f). This matches pcm-mixer's zero-filled generator
 * buffer contract. */
void chipseq_render_f32(chipseq *seq, float *dst, size_t frames);

/* pcm-mixer generator adaptor: `user` is a chipseq*. This is the ONLY symbol
 * whose signature is shared with pcm_mixer.h; nothing here includes it.
 * Pass to pcmmix_set_generator(&mixer, chipseq_generator, &seq). */
void chipseq_generator(float *dst, size_t frames, void *user);

/* Apply all queued commands NOW (for offline/test engines with no render
 * thread; a live renderer drains the queue itself each block). */
void chipseq_flush_commands(chipseq *seq);

/* ======================================================================== */
/* offline bounce                                                           */
/* ======================================================================== */

/* Render a whole song to a fresh malloc'd mono s16 buffer at
 * options->sample_rate. max_frames is an upper bound: a finite song may end
 * sooner, while a looping song renders through its declared loop until the
 * bound and therefore REQUIRES max_frames != 0. With max_frames == 0, a finite
 * song uses chipseq_song_frames(). *out_frames receives the frame count and is
 * set to zero on failure. Returns NULL with a reason in err on OOM, invalid
 * input, an unrepresentable allocation, or an unbounded looping render. Free
 * with chipseq_pcm_free. */
int16_t *chipseq_render_song(const chipseq_song *song,
                             const chipseq_options *options,
                             uint64_t max_frames, size_t *out_frames,
                             char *err, size_t err_len);

/* Free a buffer from chipseq_render_song. NULL allowed. */
void chipseq_pcm_free(int16_t *frames);

/* Bounce a song straight to a canonical little-endian PCM/mono/16-bit WAV at
 * options->sample_rate -- the inverse of a strict WAV loader. Files exceeding
 * the 32-bit RIFF size limit are rejected before rendering. Returns false with
 * a reason in err on failure. */
bool chipseq_bounce_wav(const chipseq_song *song, const chipseq_options *options,
                        const char *path, uint64_t max_frames,
                        char *err, size_t err_len);

/* ======================================================================== */
/* MIDI import + C emission (in chipseq_tools.c; a shipped game omits this)  */
/* ======================================================================== */

typedef struct chipseq_midi_map {
    const uint8_t *program_to_instrument; /* 128 GM programs -> instrument idx */
    const uint8_t *drum_to_instrument;    /* 128 GM ch-10 notes -> instrument idx */
    const chipseq_instrument *instruments;
    uint16_t instrument_count;
    uint8_t  channels;
    uint8_t  rows_per_beat;
    uint8_t  ticks_per_row;
    uint16_t rows_per_pattern;
    bool     voice_steal;
    bool     import_pitch_bend;
} chipseq_midi_map;

/* Parse a format-0/1 SMF and quantize it onto chip channels via `map`.
 * Returns a malloc'd song (free with chipseq_song_free) or NULL + err. */
chipseq_song *chipseq_midi_load(const char *path, const chipseq_midi_map *map,
                                char *err, size_t err_len);

/* Free a song returned by chipseq_midi_load. NEVER call on a static literal. */
void chipseq_song_free(chipseq_song *song);

/* Emit diff-stable, self-contained C song literals named `ident` to `path`.
 * ident must be a non-keyword C11 identifier. */
bool chipseq_song_write_c(const chipseq_song *song, const char *path,
                          const char *ident, char *err, size_t err_len);

/* ======================================================================== */
/* small helpers                                                            */
/* ======================================================================== */

/* Classic NES 16-entry noise-note mapping (index 0..15 -> note). */
uint8_t chipseq_nes_noise_note(unsigned index);

/* Human-readable effect name ("VIBRATO", ...) for tools/debug. */
const char *chipseq_fx_name(chipseq_fx fx);

#endif /* !__cplusplus */

#endif /* CHIP_SEQUENCER_H */
