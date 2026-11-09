# Writing and driving chip-sequencer songs

Song literals, live playback, the 0.4 live-editing controls and the effect column reference, moved here from the README. The authoritative interface is [`include/chip_sequencer.h`](../include/chip_sequencer.h).

## Quick start

A song is plain C literals a game writes inline — no binary format, no loader,
no allocation at play time. These author-side `CS_*` conveniences make cells
read like a tracker screen; they are ordinary macros, not part of the installed
API:

```c
#include "chip_sequencer.h"

#define CS__                   { CHIPSEQ_NOTE_NONE, 0, CHIPSEQ_VOL_NONE, CHIPSEQ_FX_NONE, 0 }
#define CS_OFF                 { CHIPSEQ_NOTE_OFF,  0, CHIPSEQ_VOL_NONE, CHIPSEQ_FX_NONE, 0 }
#define CS_CUT                 { CHIPSEQ_NOTE_CUT,  0, CHIPSEQ_VOL_NONE, CHIPSEQ_FX_NONE, 0 }
#define CS_N(pc,oct,i,v)       { CHIPSEQ_NOTE(CHIPSEQ_PC_##pc,oct), (i), (v), CHIPSEQ_FX_NONE, 0 }
#define CS_NF(pc,oct,i,v,fx,p) { CHIPSEQ_NOTE(CHIPSEQ_PC_##pc,oct), (i), (v), CHIPSEQ_FX_##fx, (p) }
#define CS_V(v)                { CHIPSEQ_NOTE_NONE, 0, (v), CHIPSEQ_FX_NONE, 0 }
#define CS_FX(fx,p)            { CHIPSEQ_NOTE_NONE, 0, CHIPSEQ_VOL_NONE, CHIPSEQ_FX_##fx, (p) }

/* per-tick int8 step tables drive volume / arpeggio / pitch / duty --
 * the exponential decays that expf() used to compute are just data here. */
static const int8_t env_pluck[] = { 64,60,52,44,38,32,28,24,20,16,12,8,4,0 };
static const chipseq_seq pluck_vol = {
    env_pluck, 14, CHIPSEQ_SEQ_NO_LOOP, CHIPSEQ_SEQ_NO_RELEASE
};
static const int8_t arp_maj[] = { 0, 4, 7 };            /* major triad */
static const chipseq_seq lead_arp = { arp_maj, 3, 0, CHIPSEQ_SEQ_NO_RELEASE };

static const chipseq_instrument insts[] = {
    { .name = "lead", .wave = CHIPSEQ_WAVE_PULSE, .duty = CHIPSEQ_DUTY_25,
      .vol_seq = &pluck_vol, .arp_seq = &lead_arp },
    { .name = "bass", .wave = CHIPSEQ_WAVE_TRIANGLE, .tri_steps = 32,
      .transpose = -12 },
};

/* a pattern is a row-major grid: cells[row * channels + channel] */
static const chipseq_cell p0[] = {
/*  lead                bass            */
    CS_N(C,5, 0,48),    CS_N(C,3, 1,56),
    CS__,               CS__,
    CS_N(E,5, 0,48),    CS__,
    CS_NF(G,5, 0,48, VIBRATO, 0x38), CS_N(G,3, 1,56),
};
static const chipseq_pattern pats[] = { { p0, 4 } };
static const uint8_t order[] = { 0 };

static const chipseq_song song = {
    .name = "demo",
    .instruments = insts, .instrument_count = 2,
    .patterns = pats,     .pattern_count = 1,
    .order = order,       .order_length = 1,
    .loop_order = 0,      .channels = 2,
    .rows_per_beat = 4,   .ticks_per_row = 6,
    .bpm_q8 = CHIPSEQ_BPM(140),
};
```

Driving it live is the generator seam plus control calls from the game thread.
Here `mixer` and `laser_song` are caller-owned integration objects:

```c
chipseq seq;
chipseq_options opt;
chipseq_options_init(&opt);
opt.sample_rate = 44100;                 /* MUST match the mixer */
chipseq_init(&seq, &opt);

char err[128];
chipseq_music_play(&seq, &song, true, err, sizeof err);  /* validates first */

/* the one line of coupling: the mixer thread now pulls audio from the synth */
pcmmix_set_generator(&mixer, chipseq_generator, &seq);

/* later, from the game thread -- these push onto a lock-free queue and return */
int laser = chipseq_sfx_play(&seq, &laser_song, 0.8f, 0, false);
chipseq_music_set_volume(&seq, 0.6f);

/* shutdown order matters: stop the mixer first (joins its thread, so the
 * generator can no longer run), THEN shut down the synth. */
pcmmix_stop(&mixer);
chipseq_shutdown(&seq);
```

## Live editing controls

Version 0.4 adds the controls needed by terminal instruments and trackers that
edit a running arrangement. `chipseq_music_set_channel_mix` submits gain, pan,
and mute for all eight music channels in one render-boundary command.
`chipseq_music_set_swing` accepts 50 (straight) through 75 percent and changes
timing on the next row; each even/odd row pair keeps the same total duration as
straight timing.

`chipseq_music_replace` swaps a validated, immutable song at the next row or
beat. It can preserve the monotonic row position or restart from row zero. Each
replacement carries a nonzero, monotonically increasing generation. The
control thread keeps the old and candidate song storage alive until
`chipseq_music_snapshot` publishes that generation (or a later one); at that
point the render thread no longer references the retired data. The extended
snapshot also reports the coherent order, row, tick, active flag, and monotonic
completed-row count.

These calls use the same single-producer/single-consumer command ring as the
original transport API. Submit controls from one control thread, render from
one audio thread, and never mutate a `chipseq_song` while either side may still
reference it.

An SFX is not a separate type: it *is* a `chipseq_song` of one or two channels,
played on an SFX slot. A jingle and a laser share the entire pipeline.

## Effect parameters

Effects occupy one cell row and use an unsigned byte `fxp`. Pitch units are
1/16 semitone and volume units are 0..64. Unless noted otherwise, running
effects act on ticks after tick 0.

| Effect | `fxp` encoding and behavior |
| --- | --- |
| `NONE` | Ignored. |
| `ARPEGGIO` | Cycles base note, high-nibble semitone offset, low-nibble semitone offset. |
| `PORTA_UP`, `PORTA_DOWN` | Adds or subtracts `fxp` pitch units per tick. |
| `TONE_PORTA` | A real note selects the target without resetting phase or sequences; moves `fxp * 4` pitch units per tick. A silent voice triggers normally. |
| `VIBRATO` | High nibble is speed and low nibble is depth for the fixed 64-step pitch LFO. |
| `TREMOLO` | High nibble is speed and low nibble is depth for the fixed 64-step volume LFO. |
| `VOL_SLIDE` | High nibble adds and low nibble subtracts volume units per tick. |
| `PAN` | Applied on tick 0: `0` is left, `128` center, and `255` right. Mono rendering ignores pan. |
| `DUTY` | Applied on tick 0; `1..63` selects pulse/saw duty and `0` leaves it unchanged. |
| `RETRIGGER` | A nonzero interval in ticks; matching ticks reset phase, PCM position, noise, and instrument sequences. |
| `NOTE_DELAY` | Triggers the row's real note at tick `fxp`; `0` is immediate. A value outside the active row length never triggers. |
| `NOTE_CUT` | Sets voice volume to zero at tick `fxp`. A value outside the active row length never triggers. |
| `SPEED` | `1..255` becomes ticks per row; `0` is ignored. |
| `TEMPO` | Integer BPM, clamped to a minimum of 20; `0` is ignored. |
| `ORDER_JUMP` | After the row, starts order index `fxp` at row 0. Use an index in the song's order list. |
| `PATTERN_BREAK` | After the row, starts the next order at row `fxp`; a row beyond that pattern starts at row 0. |
