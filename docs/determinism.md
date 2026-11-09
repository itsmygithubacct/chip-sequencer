# chip-sequencer determinism and offline formats

The determinism guarantee, the WAV bounce and the MIDI importer, moved here from the README. [PROVENANCE.md](PROVENANCE.md) states the same contract alongside the provenance of the code.

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

Inactive engines take a bounded true-idle path, while active renders scan only
the voice range owned by live tracks. The halfband decimator shifts each delay
line once per input pair. `make benchmark` records idle, music-only, saturated
music-plus-SFX, command, and validation costs so these paths remain measurable.

## Offline formats

`chipseq_bounce_wav` renders and writes in 4,096-frame chunks, so its memory use
does not scale with song length. The WAV header and samples are explicitly
little-endian, and a partial output is removed if a write fails.

The offline MIDI importer accepts strict format-0/1 Standard MIDI Files up to
64 MiB. Each declared track must end with a final, zero-length end-of-track
event; status bytes in channel data, zero tempo, chunk overrun, and bytes after
the declared tracks are rejected (apart from one inert legacy NUL pad). Import
preserves the declared end-of-track tick as an exclusive endpoint, including
silent tails, without appending a blank pattern when that endpoint is exactly
on a pattern boundary. Import is intentionally a build-time tool: shipping
games can link only `src/chip_sequencer.c`.
