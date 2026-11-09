# chip-sequencer

`chip-sequencer` is a small C11 library that turns song data written as C
literals into 16-bit PCM through a deterministic fixed-point chiptune synth and
pattern sequencer.

It is not installed on its own. Games and apps pin it as a Git submodule and
compile `src/chip_sequencer.c` into their binaries; those games and apps install
on demand from the Kilix content catalog.

## What it does

- Synthesises pulse, triangle, saw, LFSR noise, 32-nibble wavetable and
  caller-owned PCM voices from table-driven instruments.
- Sequences patterns and an order list with tempo, rows, ticks and tracker
  effect columns (arpeggio, portamento, vibrato, tremolo, volume slide, pan,
  retrigger, delays, cuts, speed, tempo, jumps).
- Plays one music song plus a small pool of song-based sound effects, with sound
  effects ducking music per channel.
- Renders mono or interleaved stereo `int16`, or adds into a `float` buffer whose
  signature is exactly a [pcm-mixer](https://github.com/itsmygithubacct/pcm-mixer#readme)
  generator callback.
- Bounces a whole song to a PCM buffer or a WAV file; a separate build-time unit
  imports Standard MIDI Files and writes compilable C song literals.
- Supports live editing of a running arrangement: per-channel gain, pan and
  mute, swing, and song replacement at the next row or beat.
- Produces the same samples on every supported target and for every block
  partition; the render path uses no libm and no POSIX.

It owns no thread, no audio device and no pipe: it is a source of audio, not a
transport.

## Quick start

```sh
git clone https://github.com/itsmygithubacct/chip-sequencer.git
cd chip-sequencer
make            # build/libchip-sequencer.a, .so and build/demo
make test       # headless unit and golden-hash suite
./build/demo    # validates a song, renders it offline and writes demo.wav
```

With pcm-mixer, the whole integration is one call:

```c
chipseq seq;
chipseq_options opt;
chipseq_options_init(&opt);
opt.sample_rate = 44100;                       /* must match the mixer */
chipseq_init(&seq, &opt);
chipseq_music_play(&seq, &song, true, err, sizeof err);
pcmmix_set_generator(&mixer, chipseq_generator, &seq);
/* ... */
pcmmix_stop(&mixer);                           /* stop the mixer first */
chipseq_shutdown(&seq);
```

How to write `song` (instruments, patterns, order list) is in
[docs/songs.md](docs/songs.md).

## Usage

| Area | Main calls |
|---|---|
| Setup | `chipseq_options_init`, `chipseq_init`, `chipseq_shutdown`, `chipseq_set_enabled` |
| Music | `chipseq_music_play`, `chipseq_music_stop`, `chipseq_music_set_volume`, `chipseq_music_set_paused`, `chipseq_music_position` |
| Live editing | `chipseq_music_set_channel_mix`, `chipseq_music_set_swing`, `chipseq_music_replace`, `chipseq_music_snapshot` |
| Sound effects | `chipseq_sfx_play`, `chipseq_sfx_set`, `chipseq_sfx_stop`, `chipseq_sfx_stop_all`, `chipseq_sfx_active` |
| Rendering | `chipseq_generator`, `chipseq_render_s16`, `chipseq_render_s16_stereo`, `chipseq_render_f32` |
| Offline | `chipseq_song_validate`, `chipseq_song_frames`, `chipseq_render_song`, `chipseq_bounce_wav` |
| Build-time tools (`src/chipseq_tools.c`) | `chipseq_midi_load`, `chipseq_song_write_c`, `chipseq_song_free` |

| Make target | What it does |
|---|---|
| `make` | static and shared libraries plus the demo |
| `make test` | unit, golden-hash and MIDI tool tests; no audio hardware |
| `make sanitize` | the suite under AddressSanitizer and UndefinedBehaviorSanitizer |
| `make tsanitize` | the game-thread to render-thread handoff under ThreadSanitizer |
| `make benchmark` | render and control cost measurements |
| `make install PREFIX=/usr/local` | installs the header and libraries |

## How it works

```
 game thread                              mixer / audio thread
 ───────────                              ────────────────────
 chipseq_music_play, chipseq_sfx_play ──▶ lock-free command ring ──▶ chipseq_generator
 (validate, then enqueue and return)                                 │ sequencer ticks
                                                                     │ Q32 fixed-point voices
                                                                     │ 2x oversample, halfband FIR
                                                                     ▼
                                                               float block for the mixer
```

Control calls validate the song on the caller's thread and push commands onto a
single-producer, single-consumer ring; the render side drains it at block
boundaries. The synth runs in integer fixed point end to end, with precomputed
note-increment tables, table-driven waveforms and per-tick step tables for
envelopes and modulation, so there is no `sin`, `exp`, `pow` or `tanh` anywhere
in the render path. Float output is defined as exactly `int16 / 32768`, so the
float and integer renders cannot diverge. A reference-song golden hash in the
test suite is the build-breaking regression gate.

A shipping game compiles only `src/chip_sequencer.c`; the MIDI importer in
`src/chipseq_tools.c` stays a build-time tool. Details:
[docs/design.md](docs/design.md) for the scope boundary and layout,
[docs/determinism.md](docs/determinism.md) for the determinism contract and
offline formats.

## Documentation

- [docs/design.md](docs/design.md): what the library is, its scope boundary
  with pcm-mixer, and the source layout.
- [docs/songs.md](docs/songs.md): writing song literals, driving playback live,
  the 0.4 live-editing controls and the effect parameter table.
- [docs/determinism.md](docs/determinism.md): the determinism guarantee, WAV
  bounce and MIDI import.
- [docs/building.md](docs/building.md): build, test, sanitizer, benchmark and
  install targets, and the C-only header rule.
- [docs/PROVENANCE.md](docs/PROVENANCE.md): provenance of the code and data, and
  the determinism contract.
- [CHANGELOG.md](CHANGELOG.md): release history.

## Related repositories

Projects that pin `chip-sequencer` at `third_party/chip-sequencer`:

- [super-kilix](https://github.com/itsmygithubacct/super-kilix#readme): its
  soundtrack and effect set are chip-sequencer songs played through pcm-mixer.
- [pleb-plant-grower](https://github.com/itsmygithubacct/pleb-plant-grower#readme),
  which compiles it into its library archive.
- [kilix-techno](https://github.com/itsmygithubacct/kilix-techno#readme).

Its output is designed to feed
[pcm-mixer](https://github.com/itsmygithubacct/pcm-mixer#readme), which those
projects reach through
[kilix-game-sdk](https://github.com/itsmygithubacct/kilix-game-sdk#readme) or
pin directly.

## Part of Plebian-OS

chip-sequencer is one of the repositories behind
[Plebian-OS](https://github.com/itsmygithubacct/plebian-os#readme), a Debian
install whose desktop is [Kilix](https://github.com/itsmygithubacct/kilix#readme)
running the [Kilix 95](https://github.com/itsmygithubacct/kilix-95#readme)
desktop through [Pleb](https://github.com/itsmygithubacct/pleb#readme).
It reaches a user compiled into the games and apps that pin it, such as Super
Kilix, which install on demand from the Kilix content catalog. The map of every
repository and how it ships is on the
[components page](https://github.com/itsmygithubacct/plebian-os/blob/main/docs/components.md).

## License

MIT. See [LICENSE](LICENSE) for the complete notices.
