# Building and testing chip-sequencer

Build, test, sanitizer, benchmark and install instructions, moved here from the README.

## Build and test

```sh
make
make test
make sanitize
make tsanitize
make benchmark
```

`make` builds `build/libchip-sequencer.a`, `build/libchip-sequencer.so`, and
the `build/demo` example. `make test` runs the headless unit + golden suite
(no audio hardware is touched). `make sanitize` reruns the suite under
`-fsanitize=address,undefined`; `make tsanitize` exercises the documented
game-thread/render-thread handoff under ThreadSanitizer. `make benchmark`
records the principal render and control costs. The core links no libm and
needs no POSIX. `make install PREFIX=/usr/local` stages the header and libraries.

The public header is C11-only because the caller-owned `chipseq` object contains
C11 atomics. It intentionally rejects inclusion from a C++ translation unit;
C++ applications should compile their chip-sequencer integration as C and
expose a small application-specific `extern "C"` facade.

Running `./build/demo` validates a tiny song, prints its exact frame count,
renders it offline, bounces it to `demo.wav`, and drives a few live generator
blocks — all without opening an audio device.
