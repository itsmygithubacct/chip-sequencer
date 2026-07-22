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
```

`make` builds `build/libchip-sequencer.a`, `build/libchip-sequencer.so`, and
the `build/demo` example. `make test` runs the headless unit + golden suite
(no audio hardware is touched). `make sanitize` reruns the suite under
`-fsanitize=address,undefined`. The core links no libm and needs no POSIX;
`make install PREFIX=/usr/local` stages the header and libraries.

Running `./build/demo` validates a tiny song, prints its exact frame count,
renders it offline, bounces it to `demo.wav`, and drives a few live generator
blocks — all without opening an audio device.

## Quick start

A song is plain C literals a game writes inline — no binary format, no loader,
no allocation at play time. The `CS_*` cell macros read like a tracker screen:

```c
#include "chip_sequencer.h"

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

Driving it live is the generator seam plus control calls from the game thread:

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

## Determinism

Determinism is a hard, build-breaking invariant. IEEE `+ - * /` are correctly
rounded and reproducible; transcendentals (`sin`, `exp`, `pow`, `tanh`) are
not guaranteed identical across libm implementations, so the render path
contains **zero** of them:

- Integer fixed point end to end — a `uint32_t` Q32 phase accumulator, 0..64
  integer volume, 1/16-semitone integer detune. Nothing in the per-sample loop
  is a float.
- A precomputed `note_inc[128]` table built at init from a 12-entry Q16
  semitone-ratio table and octave shifts — no `pow`/`exp` at runtime.
- Table-driven waveforms, a 15-bit LFSR with pinned feedback taps, and a static
  64-entry integer triangle LFO for vibrato/tremolo — no `sin`.
- Envelopes, arpeggios, pitch and duty modulation are per-tick step tables
  advanced with a 64-bit remainder accumulator, so tick timing is exact and
  **identical across sample rates**.
- Float output is defined as *exactly* `int16 * (1.0f / 32768.0f)` — a power of
  two — so `render_f32` and `render_s16` can never diverge. The library builds
  with `-ffp-contract=off` so no FMA contraction perturbs that one multiply.
- Oversampling (default 2×) with a fixed integer halfband-FIR decimation stage
  removes aliasing; the oversample factor and every table are part of the byte
  contract.

The consequence: for a fixed `(song, options, sample_rate)`,
`chipseq_render_song` produces **byte-identical** output on any machine, any
build, forever. The test suite pins a golden hash of a reference song as the
gate that proves it.

## Layout

```
include/chip_sequencer.h   the complete public interface (authoritative)
src/chip_sequencer.c       core synth + sequencer (POSIX-free, no libm)
src/chipseq_tools.c        MIDI import + C emission (a shipping game omits this)
tests/test_chipseq.c       headless unit + determinism-golden suite
tests/test_tools.c         MIDI parse + byte-stable C emission tests
examples/demo.c            build a song, bounce it, drive live blocks
Makefile                   all / test / sanitize / install / clean
```

A consuming game vendors chip-sequencer as a submodule and compiles
`src/chip_sequencer.c` directly with the game's own flags — the tools
translation unit is never linked into a shipped binary, so the game carries no
MIDI parser. Only the game's `sound.c` includes `chip_sequencer.h`.

The API is pre-1.0 and may change between minor releases.

## License

MIT. See [LICENSE](LICENSE) for the complete notices.
