# Snaggletooth documentation

Snaggletooth is a clean-room SNES implementation, built audio-first. These pages describe what is
built and how to use it. Every value in them is derived from public hardware documentation and
validated against it; where the published documentation disagrees with the hardware, the page that
settles the case is [s-dsp-behavior.md](s-dsp-behavior.md).

The [project README](../README.md) carries the status table — what is live, what is partial, and
what is unstarted.

## Where to start

| If you want to | Read |
|---|---|
| Hear a cartridge or a dump | [spc-rendering.md](spc-rendering.md) |
| Run a whole SNES machine | [snes-machine.md](snes-machine.md) |
| Run the audio unit alone | [apu-machine.md](apu-machine.md) |
| Understand how sound is produced | [dsp.md](dsp.md) |
| Read the code a dump contains | [spc700-disassembler.md](spc700-disassembler.md) |

## The machine

| Page | Covers |
|---|---|
| [snes-machine.md](snes-machine.md) | The machine — the LoROM and HiROM maps and a cartridge's save RAM, work RAM and its data port, the APU ports, region-by-region cycle cost at both clock rates, the video counters and their interrupts, the multiply/divide unit, the PPU register file, the boot handshake, and stepping, running and snapshotting |
| [65816-cpu.md](65816-cpu.md) | The main CPU core — its bus and state, the operand-width and emulation-mode machinery, what each cycle drives, and the vector suite |

## The audio unit

| Page | Covers |
|---|---|
| [apu-machine.md](apu-machine.md) | The audio machine — memory map, timers, communication ports, the boot-ROM window, and stepping |
| [spc700-cpu.md](spc700-cpu.md) | The audio CPU core — its surface, usage, and how to run the vector suite |
| [dsp.md](dsp.md) | The S-DSP — voices, the output mixer, the echo unit, and its stereo output |
| [s-dsp-behavior.md](s-dsp-behavior.md) | Where the published S-DSP documentation is incomplete, ambiguous or wrong, what the hardware does instead, and the measurement that settles each case |

## Tools

| Page | Covers |
|---|---|
| [spc-rendering.md](spc-rendering.md) | Rendering audio to a WAV from an SPC dump or from a cartridge, and what a dump does and does not carry |
| [spc700-disassembler.md](spc700-disassembler.md) | The disassembler — tracing from entry points, the register and patched-byte annotations, and the library surface |
| [spc700-assembly.md](spc700-assembly.md) | The assembly language the disassembler emits |

## Reading these pages

Each page opens with what its subject is and what it is for, then the surface, then worked
examples, then the gotchas worth knowing before they cost an afternoon. Every page carries a
Contents list under its opening.

A page describes what the code does now. Where a subject is modelled but not yet complete, the page
says so in place rather than leaving the reader to infer it — the PPU register file, for instance,
is a storage stub with no rendering behind it, and [snes-machine.md](snes-machine.md) says as much
where it describes those registers.
