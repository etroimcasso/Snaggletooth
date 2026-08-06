# Snaggletooth

A clean-room, MIT-licensed implementation of the SNES — deterministic, embeddable, and
audio-first.

## Why this exists

There is no MIT-compatible, embeddable SNES core. The established emulators are exceptional
work, but their licenses (GPL, LGPL, non-commercial terms) keep them out of permissively-licensed
engines, tools, and commercial products. Snaggletooth fills that gap: a SNES implementation you
can vendor into anything, under MIT, with clean provenance.

The name: older sound hardware produced clean, repeating trigonometric waveforms. The SNES's
SPC700 is an 8-bit sampler — its waveforms are snaggletoothed in comparison.

## Design commitments

- **Clean-room.** Implemented from public hardware documentation only. No emulator source is
  consulted, and no copyrighted bytes (game ROMs, the boot ROM) are ever included — the audio
  unit boots by seeding the documented post-boot state rather than executing Sony's boot code.
- **Deterministic and steppable.** Components advance by an externally supplied cycle budget —
  no wall clock, no threads inside the core. The same starting state and the same inputs produce
  the same bytes, every run; whole-machine state snapshots and restores as a value.
- **Embeddable.** A C++20 static library with a declarative API: no UI, no audio device, no file
  formats required at the boundary. The host owns time, I/O, and the output samples.
- **Accurate.** Behavior is held to public hardware documentation and validated against real
  program output. Accuracy is never traded away for API convenience.

## Status

The project is at its beginning: the build, test, and CI skeleton is in place, and the first
component — the SPC700 CPU core — is next. This table is kept honest as components land.

| Component | Status |
|---|---|
| SPC700 CPU core (the audio CPU's instruction set) | not started |
| APU machine (64KB RAM, timers, communication ports) | not started |
| S-DSP (8-voice sample playback, envelopes, echo, noise) | not started |
| SPC song-playback harness | not started |
| Public embedding API | not started |
| 5A22 CPU, PPU, full system | future — see the roadmap |

## Roadmap

Snaggletooth is built audio-first:

1. **The audio unit** — the SPC700 CPU, the APU machine state, and the S-DSP, composing into a
   complete SNES audio core that plays real sound-driver programs and SPC dumps. It ships as an
   embeddable component and as the SNES sound backend for the Retro++ engine.
2. **The full machine** — the 5A22 (the main CPU with its DMA and timing hardware), the PPU, and
   the system glue that binds them to the audio core.

**1.0 means a fully-featured, accurate SNES emulator core.** No dates are promised; each
component ships when it meets the accuracy bar.

## Building

Requires a C++20 toolchain and CMake 3.24+.

```
cmake -B build
cmake --build build
ctest --test-dir build
```

To embed it, vendor the repository (a git submodule works) and:

```cmake
add_subdirectory(snaggletooth)
target_link_libraries(your_target PRIVATE snaggletooth::snaggletooth)
```

## License

MIT — see [LICENSE](LICENSE).
