# Changelog

All notable user-visible changes are recorded here. This project follows
[Semantic Versioning](https://semver.org/) while its public API remains pre-1.0.

## 0.3.0 - 2026-08-08

### Added

- A maintained benchmark covering idle, music, saturated SFX, command, and
  validation paths.
- Concurrent command, SFX-claim, mute, and snapshot coverage in the
  ThreadSanitizer regression.

### Changed

- Idle rendering skips synthesis once streaming tails are empty; active
  rendering bounds voice scans and shifts each decimator delay line once per
  input pair.
- WAV bounce streams bounded 4,096-frame chunks instead of retaining the whole
  song and removes partial files after write failure.
- MIDI import enforces a 64 MiB limit and strict track, data-byte, tempo,
  end-of-track, and trailing-byte structure.
- Strict warnings are build-breaking, dependency files are generated, and test
  scratch output stays inside the selected build directory.

### Fixed

- Reject out-of-range duty effects and order jumps during song validation.
- Make one-pole lowpass tails converge to exact silence instead of retaining a
  permanent negative one-LSB offset.
- Publish command-drained playhead state even while the engine is muted.
- Publish the updated playhead when offline callers explicitly flush commands.
- Treat MIDI end-of-track as an exclusive endpoint, preserving declared tails
  without appending a blank pattern at an exact pattern boundary.

## 0.2.0 - 2026-07-22

### Added

- Stereo rendering with per-cell pan and nonlinear NES-style mixing.
- Caller-owned PCM instruments, offline WAV bounce, Standard MIDI import, and
  deterministic C song emission.
- Concurrent playhead/SFX snapshots and a ThreadSanitizer regression target.

### Changed

- `chipseq_sfx_stop_all` now reports queue failure with a `bool` result.
- SFX handles use non-repeating generations for an engine lifetime.
- The public header now explicitly requires a C11 translation unit.
- The pinned reference render hash is `0x7af68f37b171809e`.

### Fixed

- Saturating pitch arithmetic, negative fixed-point scaling, triangle phase,
  PCM looping/interpolation, delayed vibrato, note release/cut behavior, and
  immediate silence when a track ends.
- Offline length/loop bounds and canonical little-endian RIFF/WAV output.
- MIDI bounds, tempo, pitch-bend, and short-note quantization handling.
- Generated C identifiers, escaped strings, and empty instrument tables.
- Lock-free command/snapshot publication under concurrent rendering.

## 0.1.0 - 2026-07-21

- Initial deterministic fixed-point synth and pattern sequencer.
