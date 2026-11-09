# chip-sequencer design and scope

What the library is, the scope boundary it keeps with pcm-mixer, and the source layout, moved here from the README.

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
to a malloc'd PCM buffer or stream it to a canonical WAV. A separate, non-shipped
translation unit imports a Standard MIDI File and emits compilable C song
literals.

chip-sequencer **does not**: open a device, fork a sink, or manage a pipe; run
a thread (it publishes a lock-free command queue that the *foreign* mixer
thread drains, it never creates one); mix caller-supplied PCM clips against
each other or resample foreign-rate WAVs; crossfade or sequence tracks the way
pcm-mixer's two-slot music layer does; or soft-clip the master bus. pcm-mixer
already does all of that.

## Layout

```
include/chip_sequencer.h   the complete public interface (authoritative)
src/chip_sequencer.c       core synth + sequencer (POSIX-free, no libm)
src/chipseq_tools.c        MIDI import + C emission (a shipping game omits this)
tests/test_chipseq.c       headless unit + determinism-golden suite
tests/test_tools.c         MIDI parse + byte-stable C emission tests
tests/test_thread.c        concurrent snapshot regression under ThreadSanitizer
examples/demo.c            build a song, bounce it, drive live blocks
benchmarks/benchmark_chipseq.c maintained render/control microbenchmark
CHANGELOG.md               user-visible changes by release
Makefile                   all / test / sanitize / tsanitize / benchmark / install / clean
```

A consuming game vendors chip-sequencer as a submodule and compiles
`src/chip_sequencer.c` directly with the game's own flags — the tools
translation unit is never linked into a shipped binary, so the game carries no
MIDI parser. Only the game's `sound.c` includes `chip_sequencer.h`.

The API is pre-1.0 and may change between minor releases.
