# Snaggletooth

A clean-room, MIT-licensed implementation of the SNES — deterministic, embeddable, and
audio-first — with a toolkit that turns a cartridge into a source tree and proves the tree
rebuilds it.

Three things live in this repository:

- **The library**, `snaggletooth::snaggletooth`: the audio unit (SPC700, APU machine, S-DSP)
  and the main machine (65816, cartridge, work RAM, DMA, timers, the audio handshake, and the
  PPU), each stepped by a cycle budget the host supplies. The PPU resolves each visible dot
  — one pixel, or two half-pixels on a line drawn in half-pixels — and hands each finished frame
  to a frame observer.
- **The tools**, under [`tools/`](tools/README.md): a player that shows a cartridge running in a
  window and records the same run, two tracing disassemblers and two assemblers, a
  whole-cartridge disassembler and its verifier, an intermediate representation with an
  interpreter and a renderer, and two audio renderers — every one a thin command over a library
  a front end can link instead.
- **The documentation**, under [`docs/`](docs/README.md): a page per component, every value
  in it derived from public hardware documentation and validated against it.

## Contents

- [Why this exists](#why-this-exists)
- [Design commitments](#design-commitments)
- [What is built](#what-is-built)
  - [The audio unit](#the-audio-unit)
  - [The main machine](#the-main-machine)
  - [The coprocessors](#the-coprocessors)
  - [The toolkit](#the-toolkit)
  - [Validation](#validation)
- [Media](#media)
  - [Screenshots](#screenshots)
  - [Videos](#videos)
- [Getting started](#getting-started)
  - [Building](#building)
  - [Embedding the library](#embedding-the-library)
  - [Running the tools](#running-the-tools)
  - [The test data](#the-test-data)
- [Documentation](#documentation)
- [Roadmap](#roadmap)
- [License](#license)

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
  handshake it maps an original boot program written to the published protocol, never Sony's
  boot code. Nothing here needs a console boot ROM; a host holding its own dump may supply one
  through `SnesConfig::bootRom`, which is the only way bytes like those ever reach the machine.
- **Deterministic and steppable.** Components advance by an externally supplied cycle budget —
  no wall clock, no threads inside the core. The same starting state and the same inputs produce
  the same bytes, every run; whole-machine state snapshots and restores as a value.
- **Embeddable.** A C++20 static library with a declarative API: no UI, no audio device, no file
  formats required at the boundary. The host owns time, I/O, and the output samples.
- **Accurate.** Behavior is held to public hardware documentation and validated against real
  program output. Accuracy is never traded away for API convenience.
- **Provable.** A tool that writes something also proves it: the disassembler's tree is
  assembled back and compared with the image byte for byte; the lifted form is run beside the
  machine and held to every access, register and cycle.

## What is built

One row per component. The status is one of three — **complete**, **in progress**, **not
started** — followed by what the component does today and, where it is in progress, what it does
not do. Each row links to the page that describes the component in full.

### The audio unit

| Component | Status |
|---|---|
| [SPC700 CPU core](docs/spc700-cpu.md) | **complete** — 256 opcodes, cycle-stepped, every cycle checked against the SingleStepTests vectors |
| [APU machine](docs/apu-machine.md) | **complete** — the RAM and its register overlay, three timers on their documented slots, the communication ports, exact cycle budgets |
| [S-DSP](docs/dsp.md) | **in progress** — the whole voice pipeline, the echo delay line and the intra-sample register schedule; three sub-tests of the DSP test ROM report a wrong checksum ([Validation](#validation)) |
| [SPC dump loader and WAV renderer](docs/spc-rendering.md) | **in progress** — loads a dump and renders 32 kHz WAV; the output is not compared against a reference render |

### The main machine

| Component | Status |
|---|---|
| [65816 CPU core](docs/65816-cpu.md) | **complete** — 256 opcodes, cycle-stepped, both operand widths and emulation mode, every cycle checked against recorded hardware traces |
| [SNES machine](docs/snes-machine.md) | **in progress** — the bus and its region pricing, the complete beam with every per-line event at its own master offset, eight DMA/HDMA channels, the controller and APU ports, the boot handshake; no [coprocessor](#the-coprocessors) |
| [Cartridge](docs/snes-cartridge.md) | **complete** — the header, LoROM, HiROM and ExHiROM, a copier's header read and dropped, where every bus address lands, the save windows |
| [PPU](docs/ppu.md) | **complete** — a pixel resolved at its own dot from the registers as they stand there, each frame handed to an observer; the backgrounds of all eight modes at their depths, the offset table modes 2, 4 and 6 read their backgrounds through, Mode 7's field through its matrix with EXTBG's second layer and direct colour, the 512-half-pixel line of modes 5 and 6 and of `$2133` bit 3 with colour math reaching both halves, the interlaced picture and sprites at half height, mosaic on every background they draw, the sprites under the counts the chip can afford, the two windows, the sub screen and colour math; frames 256 or 512 wide, one field a frame; the register file complete beneath them. Verified against hand-built cartridges and the staged test ROMs; verification against the library of commercial cartridges is ahead |
| Public embedding API | **in progress** — a machine is built from one value, run to a cycle budget or a single access at a time, and snapshotted and restored whole; it hands over stereo frames, each finished picture, every bus access and each change to the save window through observers of their own, takes a whole controller state at once, and opens the three video memories and the audio machine's RAM to a reader. The surface is not settled: a host cannot yet answer a guest's memory access rather than watch it, be told before a chosen instruction runs, or call into the guest in the guest's own context |

### The coprocessors

The cartridge header names which chip a cartridge carries beside the CPU, and the machine reports
it; none of them runs. A cartridge that needs one does not boot.

| Component | Status |
|---|---|
| DSP-1, DSP-2, DSP-3, DSP-4 | **not started** — the NEC µPD77C25 fixed-point DSPs, each with its own program |
| SuperFX (GSU-1, GSU-2) | **not started** — the RISC coprocessor with its own ROM and RAM windows |
| SA-1 | **not started** — a second 65816 at four times the clock, with its own memory map and DMA |
| S-DD1 | **not started** — a decompression chip fed through DMA |
| S-RTC | **not started** — the real-time clock |
| OBC1 | **not started** — the sprite-attribute controller |
| SPC7110 | **not started** — decompression and data-ROM banking, with an optional real-time clock |
| ST010, ST011 | **not started** — the NEC µPD96050 DSPs |
| ST018 | **not started** — the ARM coprocessor |
| CX4 | **not started** — the Hitachi HG51B DSP |

### The toolkit

| Component | Status |
|---|---|
| [Disassembly framework](docs/disassembly-framework.md) | **complete** — traces control flow so data is never read as code, carries a per-path context, and reports a conflict rather than guessing |
| [SPC700 disassembler](docs/spc700-disassembler.md) | **complete** — names hardware registers, marks run-time-patched bytes, cycle costs measured from the core |
| [65816 disassembler](docs/65816-disassembler.md) | **complete** — carries the register widths through `REP`, `SEP` and `XCE`, and reports an operand nothing settled |
| [Cartridge disassembler](docs/snes-disassembler.md) | **in progress** — a whole cartridge into a source tree: a file per bank, the sound program the boot uploads as a program file and source of its own, and a manifest of the entries, the stops, the registers, the transfers and the routines; the cartridge is run, unattended or under an [input script](docs/input-script.md), so the jumps only a run resolves become entries, every executed instruction is lifted from the bytes the CPU fetched and checked against the chip, and every range a transfer engine moved is written as a file of its kind — tile sheet, palette, tilemap, sprite table, HDMA table — that the assembler includes back. The coprocessors have no backend |
| [Cartridge verifier](docs/snes-disassembler.md#verifying-the-tree) | **complete** — reassembles a tree and reports every difference from the image; thirty-one cartridges across all three maps rebuild byte for byte |
| [Assemblers](docs/assemblers.md) | **complete** — both dialects over one [common layer](docs/assembly-lexicon.md), each built from its disassembler's own table so every opcode round-trips |
| [Intermediate representation](docs/ir.md) | **complete** — both instruction sets lifted into one form with no bytes in it, with an interpreter per chip, a renderer back to source, and a dataflow that proves the registers, the stored values and the bounded jump tables over every path |
| [Player](tools/player/README.md) | **complete** — a cartridge in a window, held to the display where the display is close enough to the console and to the console's own rate where it is not, with its sound at the rate the run delivers, driven by a keyboard, a controller or an input script, and recorded as it runs |

### Validation

Beyond the per-cycle vector suites, the machine runs self-checking SPC test ROMs end-to-end. The
CPU, timer and memory-access-timing ROMs pass in full. Every sub-test of the DSP ROM passes but
three, which report a wrong checksum.

| Sub-test still failing | What it exercises |
|---|---|
| `Random/envelope` | envelope rates and phase transitions under randomised writes |
| `Random/kon pitch` | eight voices keyed together and re-keyed at random, read back through the echo ring |
| `Random/brr while playing` | BRR sample content decoded while voices are already sounding |

All three drive long randomised sequences and compare a single checksum at the end, so each run
reports only whether the whole sequence matched. [s-dsp-behavior.md](docs/s-dsp-behavior.md)
records what the sub-tests constrain and what they leave open; the three above turn on a rare
coincidence — a voice re-keyed at the instant its own output crosses zero — which ordinary music
does not reach, so a rendered comparison cannot arbitrate them either.

The suite is built and run on macOS, Linux and Windows, on x64 and ARM64, before any change
reaches `main`.

## Media

The machine running, as the player shows it. Every capture is the emulator's own output — the
frames the PPU hands to its observer and the audio the S-DSP mixes — with nothing composited or
retouched.

### Screenshots

### Videos


https://github.com/user-attachments/assets/afff5e21-05fb-463f-a442-cc2e41fa95cb



https://github.com/user-attachments/assets/db34386b-d83d-4ed5-a2a3-bb37578f9cd1



https://github.com/user-attachments/assets/474434ae-519b-4e1f-b379-b4cfbcfffa22


## Getting started

### Building

Requires a C++20 toolchain and CMake 3.24 or later. A build is optimized by default.

```
cmake -B build
cmake --build build
ctest --test-dir build
```

The tools build whenever Snaggletooth is the top-level project and land in the build
directory's root; each is its own target, listed in [tools/README.md](tools/README.md#building).

### Embedding the library

Vendor the repository (a git submodule works) and link the library target. The test suite and
the tools are off when Snaggletooth is built inside a parent project.

```cmake
add_subdirectory(snaggletooth)
target_link_libraries(your_target PRIVATE snaggletooth::snaggletooth)
```

The tools are off in a parent build unless it sets `SNAGGLETOOTH_BUILD_TOOLS`, which builds the
tool libraries and command-line tools without the suite.

The public headers are under [`include/snaggletooth/`](include/snaggletooth); the pages under
[`docs/`](docs/README.md) describe each component's surface and how to drive it.

### Running the tools

A cartridge into its program files and manifest, the source tree rendered from the program files,
the tree proved against the image, and the program file printed back with a summary:

```
snes_disasm game.sfc -o game
snes_render game
snes_verify game game.sfc
snes_lift game
```

`spc_render` and `rom_render` write what the audio unit plays, from a dump or from a cartridge,
as a WAV. Every page's example output comes from the hand-built cartridges under
[`tools/examples/`](tools/examples/README.md), which `snes_examples` writes to disk so anyone
can produce the same output.

### The test data

Three bodies of third-party reference data are never vendored; the tests that use them register
and skip, with a visible reason, until they are pointed at a local copy:

| Option | Points at |
|---|---|
| `SNAGGLETOOTH_SPC700_VECTORS` | the SingleStepTests SPC700 `v1` directory |
| `SNAGGLETOOTH_65816_VECTORS` | the SingleStepTests 65816 `v1` directory |
| `SNAGGLETOOTH_BLARGG_ROMS` | a directory holding the four Blargg SPC test ROMs |

```
cmake -B build -DSNAGGLETOOTH_65816_VECTORS=/path/to/65816/v1 \
               -DSNAGGLETOOTH_SPC700_VECTORS=/path/to/spc700/v1 \
               -DSNAGGLETOOTH_BLARGG_ROMS=/path/to/blargg
```

`SNAGGLETOOTH_BOOT_ROM` names a 64-byte audio boot ROM a host holds its own dump of; without
it the acceptance tests boot on the built-in upload program instead.
`SNAGGLETOOTH_BUILD_TESTS` turns the suite on or off explicitly, and `SNAGGLETOOTH_BUILD_TOOLS`
the tools.

## Documentation

[docs/README.md](docs/README.md) indexes every page with what it covers and says where to
start for a given task. By subject:

- **The machine** — [snes-machine.md](docs/snes-machine.md),
  [snes-cartridge.md](docs/snes-cartridge.md), [65816-cpu.md](docs/65816-cpu.md).
- **The audio unit** — [apu-machine.md](docs/apu-machine.md),
  [spc700-cpu.md](docs/spc700-cpu.md), [dsp.md](docs/dsp.md),
  [s-dsp-behavior.md](docs/s-dsp-behavior.md), [spc-rendering.md](docs/spc-rendering.md).
- **Disassembly** — [disassembly-framework.md](docs/disassembly-framework.md),
  [spc700-disassembler.md](docs/spc700-disassembler.md),
  [65816-disassembler.md](docs/65816-disassembler.md),
  [snes-disassembler.md](docs/snes-disassembler.md),
  [project-manifest.md](docs/project-manifest.md), [input-script.md](docs/input-script.md).
- **Assembly** — [assemblers.md](docs/assemblers.md),
  [assembly-lexicon.md](docs/assembly-lexicon.md),
  [spc700-assembly.md](docs/spc700-assembly.md), [65816-assembly.md](docs/65816-assembly.md).
- **The intermediate representation** — [ir.md](docs/ir.md).
- **The tools** — [tools/README.md](tools/README.md), with a README beside each tool, and
  [tools/examples/README.md](tools/examples/README.md), the cartridges the pages' output comes
  from.

## Roadmap

Snaggletooth is built audio-first:

1. **The audio unit** — the SPC700 CPU, the APU machine state, and the S-DSP, composing into a
   complete SNES audio core that plays real sound-driver programs and SPC dumps. It ships as an
   embeddable component and as the SNES sound backend for the Polyrhythm engine.
2. **The full machine** — the 5A22 (the main CPU with its DMA and timing hardware), the PPU, and
   the system glue that binds them to the audio core.

The PPU is built. What it draws is in its row under [What is built](#the-main-machine); what
remains is verifying it across the library of cartridges, and every wrong picture that pass finds
is fixed in the chip. After that, the machine is taken through the range of cartridge
types until they boot, and the DSP's three failing sub-tests are closed with the wider body of real
software then available to exercise it.

**1.0 means a fully-featured, accurate SNES emulator core.** No dates are promised; each
component ships when it meets the accuracy bar.

3. **A desktop app that unifies the tooling** — after the 1.0 release, because the core being
   complete comes first. The command-line tools here are what it will drive; its release
   packages will carry the app, which owns the toolkit's file types and their icons on each
   platform, and the source it was built from.

## License

MIT — see [LICENSE](LICENSE).
