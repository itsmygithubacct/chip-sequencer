# chip-sequencer

`chip-sequencer` is a small C11 library that turns compact in-source song
data — patterns of note cells plus table-driven instruments — into 16-bit PCM
using a deterministic fixed-point chip synth (pulse / triangle / saw / LFSR
noise / wavetable / caller-owned PCM voices). It is a **source of audio, not a
transport**: it owns no thread, no audio device, and no pipe. Its float render
entry point has the exact signature of pcm-mixer's generator callback, so the
whole integration is one line:

```c
pcmmix_set_generator(&mixer, chipseq_generator, &seq);
```

It replaces the *baking* half of a game's `sound.c` — the hand-written
`sinf`/`expf`/`tanhf` signal loops that synthesize tones and effects — while
pcm-mixer keeps the voice / device / thread half. The two libraries share
exactly one shape, the generator function pointer; nothing here includes
`pcm_mixer.h`, so chip-sequencer is equally usable from an SDL callback, a JACK
client, or a bare test harness.

## Scope boundary

This boundary is the most important property of the design.

chip-sequencer **does**: synthesize chip voices from instrument definitions;
sequence patterns and an order list with tempo, rows, ticks, and tracker
effects; play one music song plus a small fixed pool of song-based SFX with
per-channel SFX-preempts-music ducking; render to mono `int16`, interleaved
stereo `int16`, or *add* to a `float` buffer; and bounce a whole song offline
to a malloc'd PCM buffer or a canonical WAV. A separate, non-shipped
translation unit imports a Standard MIDI File and emits compilable C song
literals.

chip-sequencer **does not**: open a device, fork a sink, or manage a pipe; run
a thread (it publishes a lock-free command queue that the *foreign* mixer
thread drains, it never creates one); mix caller-supplied PCM clips against
each other or resample foreign-rate WAVs; crossfade or sequence tracks the way
pcm-mixer's two-slot music layer does; or soft-clip the master bus. pcm-mixer
already does all of that.

## Build and test

```sh
make
make test
make sanitize
make tsanitize
```

`make` builds `build/libchip-sequencer.a`, `build/libchip-sequencer.so`, and
the `build/demo` example. `make test` runs the headless unit + golden suite
(no audio hardware is touched). `make sanitize` reruns the suite under
`-fsanitize=address,undefined`; `make tsanitize` exercises the documented
game-thread/render-thread handoff under ThreadSanitizer. The core links no libm
and needs no POSIX. `make install PREFIX=/usr/local` stages the header and
libraries.

The public header is C11-only because the caller-owned `chipseq` object contains
C11 atomics. It intentionally rejects inclusion from a C++ translation unit;
C++ applications should compile their chip-sequencer integration as C and
expose a small application-specific `extern "C"` facade.

Running `./build/demo` validates a tiny song, prints its exact frame count,
renders it offline, bounces it to `demo.wav`, and drives a few live generator
blocks — all without opening an audio device.

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

## Determinism

Determinism is a hard, build-breaking invariant. The supported-target contract
requires 32-bit always-lock-free `atomic_uint` and binary floating point with
enough precision for exact int16 scaling; unsupported targets fail at compile
time. The render path contains **zero** libm or transcendental calls:

- Integer fixed point end to end — a `uint32_t` Q32 phase accumulator, 0..64
  integer volume, 1/16-semitone integer detune. Nothing in the per-sample loop
  is a float.
- A precomputed `note_inc[128]` table built at init from a 12-entry Q16
  semitone-ratio table and octave shifts — no `pow`/`exp` at runtime.
- Table-driven waveforms, a 15-bit LFSR with pinned feedback taps, and a static
  64-entry integer triangle LFO for vibrato/tremolo — no `sin`.
- Envelopes, arpeggios, pitch and duty modulation are per-tick step tables
  advanced with a 64-bit remainder accumulator, so the musical tick sequence
  is identical across sample rates and frame positions scale to within one
  output sample.
- Operations that need a signed power-of-two scale use explicit, defined floor
  division rather than implementation-defined right shifts of negative values.
- Float output is defined as *exactly* `int16 * (1.0f / 32768.0f)` — a power of
  two exactly representable on supported targets — so `render_f32` and
  `render_s16` cannot diverge. The library builds with `-ffp-contract=off`.
- Oversampling (default 2×) with a fixed integer halfband-FIR decimation stage
  removes aliasing; the oversample factor and every table are part of the byte
  contract.

The consequence: for a fixed `(song, options, sample_rate)`,
`chipseq_render_song` produces the same signed int16 sample sequence on every
supported target and for every caller block partition. In-memory int16 byte
order is native; WAV and golden-hash serialization is explicitly little-endian.
The test suite pins a reference-song golden hash as the regression gate.

## Layout

```
include/chip_sequencer.h   the complete public interface (authoritative)
src/chip_sequencer.c       core synth + sequencer (POSIX-free, no libm)
src/chipseq_tools.c        MIDI import + C emission (a shipping game omits this)
tests/test_chipseq.c       headless unit + determinism-golden suite
tests/test_tools.c         MIDI parse + byte-stable C emission tests
tests/test_thread.c        concurrent snapshot regression under ThreadSanitizer
examples/demo.c            build a song, bounce it, drive live blocks
CHANGELOG.md               user-visible changes by release
Makefile                   all / test / sanitize / tsanitize / install / clean
```

A consuming game vendors chip-sequencer as a submodule and compiles
`src/chip_sequencer.c` directly with the game's own flags — the tools
translation unit is never linked into a shipped binary, so the game carries no
MIDI parser. Only the game's `sound.c` includes `chip_sequencer.h`.

The API is pre-1.0 and may change between minor releases.

## License

MIT. See [LICENSE](LICENSE) for the complete notices.
