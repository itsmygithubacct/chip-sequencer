# Provenance and determinism contract

`chip-sequencer` is original MIT-licensed work. It contains no external content:
no imported tracker modules, no sample banks, no recordings, no transcribed
melodies, and no code copied from any other project. Songs are plain C literals a
consuming program writes inline; the library ships one small demo song
(`examples/demo.c`) and a reference song inside the test suite, both authored for
this repository. There is no binary song format and no loader — nothing is read
from disk at play time.

The synthesis *techniques* the library implements (duty-modulated pulse voices,
a stair-stepped triangle, LFSR noise, a 32-nibble wavetable, and tracker effect
columns) are long-standing, non-copyrightable ideas re-expressed here from
scratch. Any pitch, envelope, or effect table used by the demo or the tests is
this repository's own.

## The determinism contract

Sample reproducibility is the library's load-bearing guarantee and is enforced
as a build-breaking test. For a fixed `(song, options, sample_rate)`,
`chipseq_render_song` produces the same signed int16 sample sequence on every
supported target and for every caller block partition. In-memory int16 byte
order is native; serialized WAV and golden-hash bytes are little-endian.

It holds because supported targets must provide 32-bit always-lock-free
`atomic_uint` and binary floating point with sufficient precision (enforced at
compile time), and because the render path contains **zero** libm or
transcendental functions (`sin`, `exp`, `pow`, `tanh`):

- Integer fixed point end to end: a `uint32_t` Q32 phase accumulator, 0..64
  integer volume, and 1/16-semitone integer detune. Nothing in the per-sample
  loop is a float.
- A precomputed `note_inc[128]` table built at init from a 12-entry Q16
  semitone-ratio table plus octave shifts — no `pow`/`exp` at runtime.
- Table-driven waveforms, a 15-bit LFSR with pinned feedback taps, and a static
  64-entry integer triangle LFO for vibrato and tremolo — no `sin`.
- Envelopes, arpeggios, pitch, and duty modulation advance one step per tick with
  a 64-bit remainder accumulator, so the musical tick sequence is identical
  across sample rates and frame positions scale to within one output sample.
- Operations that need a signed power-of-two scale use explicit, defined floor
  division rather than implementation-defined right shifts of negative values.
- Float output is defined as exactly `int16 * (1.0f / 32768.0f)` — a power of two
  exactly representable on supported targets — so `render_f32` and `render_s16`
  cannot diverge. The library builds with `-ffp-contract=off`.
- Oversampling (default 2x) with a fixed integer halfband-FIR decimation stage is
  part of the byte contract, as is every table and the oversample factor.

## Threading and scope

Playback and gain controls push onto a lock-free single-producer/single-consumer
command ring that the *foreign* mixer thread drains at block start. Master
enable is an always-lock-free atomic flag, and playhead/SFX-status queries read
atomic publication state. The render callback is fast, lock-free, non-blocking,
and never calls back into the API. The library owns no thread, no device, and no
pipe; the scope boundary against a transport mixer is described in full in
[../README.md](../README.md).

## Verification

- `make test` — the headless unit suite plus the golden-hash determinism gate:
  `chipseq_song_validate` rejects every out-of-bounds construction with a message
  naming the pattern, row, and channel; `render_f32` equals `render_s16 / 32768`
  sample-for-sample; queued commands apply exactly at block boundaries; renders
  are invariant to caller block partitioning; and the FNV-1a hash of the
  reference song render is reproduced byte-for-byte.
- `make sanitize` — the same suite under AddressSanitizer and
  UndefinedBehaviorSanitizer.
- `make tsanitize` — concurrent render/playhead publication under
  ThreadSanitizer.

No audio hardware is touched by any test.
