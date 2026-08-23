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
  unit boots by seeding the documented post-boot state, and where it runs the console's upload
  handshake it maps an original boot program written to the published protocol, never Sony's boot code.
  Nothing here needs a console boot ROM; a host holding its own dump may supply one through
  `SnesConfig::bootRom`, which is the only way bytes like those ever reach the machine.
- **Deterministic and steppable.** Components advance by an externally supplied cycle budget —
  no wall clock, no threads inside the core. The same starting state and the same inputs produce
  the same bytes, every run; whole-machine state snapshots and restores as a value.
- **Embeddable.** A C++20 static library with a declarative API: no UI, no audio device, no file
  formats required at the boundary. The host owns time, I/O, and the output samples.
- **Accurate.** Behavior is held to public hardware documentation and validated against real
  program output. Accuracy is never traded away for API convenience.

## Status

The audio core is complete and the main machine runs everything but the picture. The rendering PPU is
the missing piece; the public embedding API and the SPC700 assembler have not been started.

**The audio core.** The SPC700 executes all 256 opcodes a cycle at a time, every cycle checked against
the SingleStepTests vectors — the address driven, the byte moved, and the cycles that reach memory not
at all. Around it the APU machine supplies 64KB of RAM, the register overlay, the communication ports,
and three timers that tick on their documented slots of one shared counter, ahead of the CPU's access
on that cycle. The S-DSP builds each 32 kHz sample across that sample's 32 clock slots, reading every
register on its own slot so a mid-sample write lands where the hardware takes it: BRR decode, pitch and
Gaussian interpolation, envelopes, key-on/key-off, the eight-voice stereo mix, the shared noise
generator, pitch modulation, master volume, and the echo delay line.

**The main machine.** All 256 of the 65816's opcodes run a cycle at a time, each cycle checked against
recorded hardware traces — every address driven, every byte moved, every signal pin. The machine maps a
LoROM cartridge and 128KB of work RAM, drives the audio unit across the communication ports, prices each
cycle by the region it reaches at either the NTSC or PAL clock rate, and spends a master-cycle budget
exactly. Video counters drive the vertical-blank NMI and the H/V-timer IRQ, the hardware multiply and
divide unit runs, eight DMA channels handle general-purpose transfers and per-scanline HDMA, and the
audio unit boots the upload handshake that streams a driver in from the main CPU. A PPU register file
accepts video memory without drawing it.

**Validation.** Beyond the per-cycle vector suites, the machine runs self-checking SPC test ROMs
end-to-end. The CPU, timer and memory-access-timing ROMs pass. The DSP ROM does not: it clears the
echo tests and reports a failure in the envelope tests.

The table below is kept honest as components land.

| Component | Status |
|---|---|
| SPC700 CPU core (the audio CPU's instruction set) | **complete** — 256 opcodes, cycle-stepped, every cycle vector-validated |
| APU machine (64KB RAM, timers, communication ports) | **complete** — register overlay, three timers, communication ports, exact cycle budgets |
| S-DSP (voices, mixer, echo) | **complete** — BRR decode, pitch and Gaussian interpolation, envelopes, keying, eight-voice stereo mix, noise, pitch modulation, master volume, echo delay line, intra-sample register schedule |
| 65816 CPU core (the main CPU's instruction set) | **complete** — 256 opcodes, cycle-stepped, both operand widths and emulation mode, every cycle vector-validated |
| SPC700 disassembler | **complete** — traces control flow from entry points so data is never decoded as code, names hardware registers, marks run-time-patched bytes, emits assembly source; cycle costs measured from the interpreter |
| SNES machine (bus, memory map, clock, timing) | in progress — LoROM map, work RAM and its data port, APU communication ports, the 6/8/12-master-cycle region model at both clock rates, exact master-cycle budgeting and the CPU-to-APU interleave, video counters, vertical-blank NMI and H/V-timer IRQ, multiply/divide unit, PPU register file, eight DMA/HDMA channels, and the audio boot handshake. The rendering PPU remains |
| SPC dump loader + WAV renderer | in progress — loads a dump into the machine and renders 32 kHz WAV; output not yet validated against reference renders |
| SPC700 assembler | not started — the dialect it accepts is specified |
| Public embedding API | not started |
| PPU, full system | future — see the roadmap |

## Documentation

| Document | Covers |
|---|---|
| [spc700-cpu.md](docs/spc700-cpu.md) | The audio CPU core — its surface, usage, and how to run the vector suite |
| [apu-machine.md](docs/apu-machine.md) | The audio machine — memory map, timers, communication ports, the boot-ROM window, and stepping |
| [dsp.md](docs/dsp.md) | The S-DSP — voices, the output mixer, the echo unit, and its stereo output |
| [s-dsp-behavior.md](docs/s-dsp-behavior.md) | Where the published S-DSP documentation is incomplete, ambiguous or wrong, what the hardware does instead, and the measurement that settles each case |
| [65816-cpu.md](docs/65816-cpu.md) | The main CPU core — its bus and state, the operand-width and emulation-mode machinery, what each cycle drives, and the vector suite |
| [snes-machine.md](docs/snes-machine.md) | The machine around it — the LoROM map, work RAM and its data port, the APU ports, region-by-region cycle cost at both clock rates, the video counters and their interrupts, the multiply/divide unit, the PPU register file, the boot handshake, and stepping, running and snapshotting |
| [spc-rendering.md](docs/spc-rendering.md) | Loading an SPC dump and rendering it to a WAV, and what a dump does and does not carry |
| [spc700-disassembler.md](docs/spc700-disassembler.md) | The disassembler — tracing from entry points, the register and patched-byte annotations, and the library surface |
| [spc700-assembly.md](docs/spc700-assembly.md) | The assembly language the disassembler emits |

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
