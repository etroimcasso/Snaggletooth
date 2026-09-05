# Snaggletooth documentation

Snaggletooth is a clean-room SNES implementation, built audio-first. These pages describe what is
built and how to use it. Every value in them is derived from public hardware documentation and
validated against it; where the published documentation disagrees with the hardware, the page that
settles the case is [s-dsp-behavior.md](s-dsp-behavior.md).

The [project README](../README.md) carries the status table — what is live, what is partial, and
what is unstarted.

## Contents

- [Where to start](#where-to-start)
- [The machine](#the-machine)
- [The audio unit](#the-audio-unit)
- [Tools](#tools)
- [Reading these pages](#reading-these-pages)

## Where to start

| If you want to | Read |
|---|---|
| Hear a cartridge or a dump | [spc-rendering.md](spc-rendering.md) |
| Run a whole SNES machine | [snes-machine.md](snes-machine.md) |
| Run the audio unit alone | [apu-machine.md](apu-machine.md) |
| Understand how sound is produced | [dsp.md](dsp.md) |
| Read the code a dump contains | [spc700-disassembler.md](spc700-disassembler.md) |
| Read the code a cartridge contains | [65816-disassembler.md](65816-disassembler.md) |
| Turn a whole cartridge into a source tree | [snes-disassembler.md](snes-disassembler.md) |
| Prove a source tree rebuilds its cartridge | [snes-disassembler.md §Verifying the tree](snes-disassembler.md#verifying-the-tree) |
| Trace a cartridge past the jumps the bytes cannot name | [snes-disassembler.md §Running the cartridge](snes-disassembler.md#running-the-cartridge) |
| Trace a cartridge past a return to an address the code put on the stack, and see what the registers were at every site the run executed | [snes-disassembler.md §Where a run landed, and what it saw](snes-disassembler.md#where-a-run-landed-and-what-it-saw), [project-manifest.md §2.13](project-manifest.md#213-where-a-run-landed) |
| Trace a cartridge past a jump table whose index the bytes bound, without running it | [snes-disassembler.md §What every path proves](snes-disassembler.md#what-every-path-proves) |
| Play a cartridge through the disassembler so the trace reaches what a player does | [input-script.md](input-script.md) |
| See where the bytes the hardware received came from — every range the transfer engines moved as the cartridge ran | [snes-disassembler.md §What a run moved](snes-disassembler.md#what-a-run-moved), [project-manifest.md §2.11](project-manifest.md#211-what-a-run-moved) |
| Get the bytes the hardware received as files of their own — tilesets, palettes, sprite tables, HDMA tables — with the bank file including each where it was | [snes-disassembler.md §The assets](snes-disassembler.md#the-assets), [project-manifest.md §2.12](project-manifest.md#212-assets) |
| See what hardware a cartridge's code drives, and which routine drives what | [snes-disassembler.md §What the code reaches](snes-disassembler.md#what-the-code-reaches) |
| See what the direct register, the data bank and the stack pointer are at a label, and what a store writes | [snes-disassembler.md §What every path proves](snes-disassembler.md#what-every-path-proves), [project-manifest.md §2.9](project-manifest.md#29-what-every-path-proves) |
| Assemble source back into bytes | [assemblers.md](assemblers.md) |
| Lift a cartridge's code into a form with no bytes in it, and run that form | [ir.md](ir.md) |
| Read what the lift wrote for a cartridge | [ir.md §Reading a program](ir.md#reading-a-program) |
| Write SNES assembly back from the lifted form, with registers, labels and routines named | [ir.md §Rendering source](ir.md#rendering-source), [snes-disassembler.md §The tree](snes-disassembler.md#the-tree) |
| Prove the lifted form runs as the machine does, over a whole recorded run | [ir.md §Running beside the machine](ir.md#running-beside-the-machine) |
| Watch every access a running machine makes | [snes-machine.md §The bus observer](snes-machine.md#the-bus-observer) |
| Run a directory of cartridges through every tool | [tools/README.md](../tools/README.md) |
| Get the example cartridges the pages' output comes from | [tools/examples/README.md](../tools/examples/README.md) |

## The machine

| Page | Covers |
|---|---|
| [snes-machine.md](snes-machine.md) | The machine — the cartridge under its map and its save RAM, work RAM and its data port, the APU ports, region-by-region cycle cost at both clock rates, the video counters and their interrupts, the controller ports, the multiply/divide unit, the PPU register file, DMA and HDMA, the boot handshake, the bus observer, and stepping, running and snapshotting |
| [snes-cartridge.md](snes-cartridge.md) | The cartridge as a value — the header and its vectors, the LoROM, HiROM and ExHiROM maps, where every bus address lands in the image, and the save windows |
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
| [disassembly-framework.md](disassembly-framework.md) | The tracing disassembler's chip-independent half — the backend interface, 24-bit addresses, the per-path context and conflict reporting, the listing format, and the entry points a cartridge header names |
| [spc700-disassembler.md](spc700-disassembler.md) | The SPC700 disassembler — tracing from entry points, the register and patched-byte annotations, and the library surface |
| [65816-disassembler.md](65816-disassembler.md) | The 65816 disassembler — the register widths carried along every path, what it reports rather than guesses, the costs measured under each mode, and the registers named by bank, each with the part of the machine it belongs to |
| [snes-disassembler.md](snes-disassembler.md) | The cartridge disassembler and verifier — a whole cartridge traced across its banks from the vectors into one source file per bank, written from the lifted form with the registers as operands, labels across files and a header per routine, the sound program captured from the machine as a file of its own, the stops a person answers with entries, what is placed, `snes_verify`, which assembles the tree back and reports the difference from the image, what the traced code reaches and which routine reaches it, the cartridge run on the machine — unattended or played from a script — so its indirect jumps' destinations become entries, every instruction it executes is lifted from its fetches and checked so that where it landed without an instruction naming it becomes an entry too and the registers seen at every site are recorded, and every range the transfer engines moved is recorded with where it came from, every such range that begins in the image lifted into a file of its own kind, and what every path proves: the registers at each label, the values stored, and the jump tables the bytes bound, traced past without running |
| [project-manifest.md](project-manifest.md) | The project manifest — the lines that name the image, the files and their ranges, the sound program's blocks, the entries, stops and warnings, the hardware the code reaches and the routines that reach it, the destinations a run reached and the places it landed, the bytes derived, the ranges a run saw the transfer engines move, the files lifted out of the banks, what every path proves at each label and what a run saw at each site; what each tool reads back; stability |
| [input-script.md](input-script.md) | The input script — the recorded run `snes_disasm --input` replays into the controller ports: its line form, frames, ports, buttons, refusals, library and stability |
| [ir.md](ir.md) | The intermediate representation — a 65816 program as its meaning: the instruction layer and the effect layer, the vocabulary, every wrap and flag rule the effects follow, the measured cost, the lift from a listing, the interpreter, the program written out for reading, SNES assembly rendered back from the instruction layer, the run replayed beside the machine with every access, register and cycle checked and the lockstep the cartridge disassembler's own run shares, the dataflow that says what every path proves, and how all of it is held to the core |
| [assemblers.md](assemblers.md) | The two assemblers — the command lines, what is written, the diagnostics, how the 65816's widths are followed, the library and how an included file is read, and writing a dialect |
| [assembly-lexicon.md](assembly-lexicon.md) | The assembly language's common layer — source format, numbers, symbols, directives, round-trip, diagnostics and stability |
| [spc700-assembly.md](spc700-assembly.md) | The SPC700 dialect — its addressing modes and what its encoding needs |
| [65816-assembly.md](65816-assembly.md) | The 65816 dialect — its addressing modes, the width and mode directives, regions, and the long, jump, block-move and stack forms |

The tools themselves, and the libraries behind them, are described beside their
sources in [tools/README.md](../tools/README.md).

## Reading these pages

Each page opens with what its subject is and what it is for, then the surface, then worked
examples, then the gotchas worth knowing before they cost an afternoon. Every page carries a
Contents list under its opening.

A page describes what the code does now. Where a subject is modelled but not yet complete, the page
says so in place rather than leaving the reader to infer it — the PPU register file, for instance,
is a storage stub with no rendering behind it, and [snes-machine.md](snes-machine.md) says as much
where it describes those registers.
