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

The SPC700 CPU core and the APU machine that gives it real memory are complete: all 256 opcodes with
cycle counts matched to the documented per-instruction totals and validated per opcode against the
SingleStepTests vectors, and the machine's 64KB RAM, register overlay, three timers, and
communication ports. The S-DSP is complete: BRR decode, pitch and Gaussian interpolation, the volume
envelope, key-on/key-off, the eight-voice stereo mix, the shared noise generator, pitch modulation,
the master volume, and the echo delay line, surfaced as 32 kHz frames through the machine. The main
CPU's instruction set is complete too: all 256 of the 65816's opcodes run a cycle at a time, and every
cycle is checked against the recorded hardware traces — each address driven, each byte moved, and each
signal pin. The rest of the 5A22 around it has not been built. This table is kept honest as components
land.

| Component | Status |
|---|---|
| SPC700 CPU core (the audio CPU's instruction set) | complete — 256 opcodes, per-opcode vector-validated |
| APU machine (64KB RAM, timers, communication ports) | complete — register overlay, 3 timers, comm ports |
| S-DSP voice pipeline (BRR, pitch, envelopes, 8-voice mix) | complete — decode, interpolation, keying, stereo frames |
| S-DSP completion (echo, noise, pitch modulation, master volume) | complete — noise, PMON, master volume, echo delay line |
| SPC dump loader + WAV renderer | in progress — loads a dump into the machine and renders it to a 32 kHz WAV; output validation against reference renders not yet done |
| Public embedding API | not started |
| 5A22 CPU core (the 65816) | in progress — the instruction set is complete: all 256 opcodes execute one cycle at a time, with the 8/16-bit width and emulation-mode machinery, and every cycle is validated against the vectors in both modes. The rest of the 5A22 — DMA and HDMA, the hardware multiply and divide registers, and the memory-speed model — is not started |
| PPU, full system | future — see the roadmap |

See [docs/spc700-cpu.md](docs/spc700-cpu.md) for the CPU core's surface, usage, and how to run the
vector suite, [docs/apu-machine.md](docs/apu-machine.md) for the machine's memory map, timers, ports,
and stepping, and [docs/dsp.md](docs/dsp.md) for the full DSP — voices, the output mixer, and the
echo unit — and its stereo output. [docs/spc-rendering.md](docs/spc-rendering.md)
covers loading an SPC dump and rendering it to a WAV, and what a dump does and does
not carry. [docs/65816-cpu.md](docs/65816-cpu.md) covers the main CPU core — its bus,
state, the 8/16-bit width and emulation-mode machinery, cycle-stepped execution and
what each cycle drives, and how to run the vector suite.

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
