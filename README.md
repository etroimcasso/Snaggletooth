# Snaggletooth

A clean-room, MIT-licensed implementation of the SNES — deterministic, embeddable, and
audio-first — with a toolkit that turns a cartridge into a source tree and proves the tree
rebuilds it.

Three things live in this repository:

- **The library**, `snaggletooth::snaggletooth`: the audio unit (SPC700, APU machine, S-DSP)
  and the main machine (65816, cartridge, work RAM, DMA, timers, the audio handshake), each
  stepped by a cycle budget the host supplies. The rendering PPU is not built yet.
- **The tools**, under [`tools/`](tools/README.md): two tracing disassemblers and two
  assemblers, a whole-cartridge disassembler and its verifier, an intermediate representation
  with an interpreter and a renderer, and two audio renderers — every one a thin command over a
  library a front end can link instead.
- **The documentation**, under [`docs/`](docs/README.md): a page per component, every value
  in it derived from public hardware documentation and validated against it.

## Contents

- [Why this exists](#why-this-exists)
- [Design commitments](#design-commitments)
- [What is built](#what-is-built)
  - [The audio unit](#the-audio-unit)
  - [The main machine](#the-main-machine)
  - [The toolkit](#the-toolkit)
  - [Validation](#validation)
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

The audio core is feature-complete, the main machine runs everything but the picture, and the
cartridge toolkit is complete. The rendering PPU is the next piece of work; the public embedding
API has not been started. Each row links to the page that describes the component in full.

### The audio unit

| Component | Status |
|---|---|
| [SPC700 CPU core](docs/spc700-cpu.md) | **complete** — 256 opcodes, cycle-stepped, every cycle checked against the SingleStepTests vectors |
| [APU machine](docs/apu-machine.md) | **complete** — 64KB RAM, the register overlay, three timers on their documented slots, the communication ports, exact cycle budgets |
| [S-DSP](docs/dsp.md) | feature-complete — BRR decode, pitch and Gaussian interpolation, envelopes, keying, the eight-voice stereo mix, noise, pitch modulation, master volume, the echo delay line, and the intra-sample register schedule; three sub-tests of the DSP test ROM still report a wrong checksum ([below](#validation)) |
| [SPC dump loader and WAV renderer](docs/spc-rendering.md) | in progress — loads a dump into the machine and renders 32 kHz WAV; output not yet validated against reference renders |

### The main machine

| Component | Status |
|---|---|
| [65816 CPU core](docs/65816-cpu.md) | **complete** — 256 opcodes, cycle-stepped, both operand widths and emulation mode, every cycle checked against recorded hardware traces |
| [SNES machine](docs/snes-machine.md) | in progress — the three cartridge maps, work RAM and its data port, the APU ports, region-priced cycles at both clock rates, exact master-cycle budgeting, video counters with the NMI and the timer IRQ, the multiply/divide unit, the PPU register file, eight DMA/HDMA channels, the controller ports, the audio boot handshake, and an observer told every access; the rendering PPU remains |
| [Cartridge](docs/snes-cartridge.md) | built — the header's map, size, title, checksum and vectors; LoROM, HiROM and ExHiROM; where every bus address lands in the image; the save windows |
| Public embedding API | not started |
| PPU | future — see the [roadmap](#roadmap) |

### The toolkit

| Component | Status |
|---|---|
| [Disassembly framework](docs/disassembly-framework.md) | **complete** — traces control flow so data is never read as code, carries a per-path context so a chip whose instruction widths depend on state is read correctly, reports a conflict rather than guessing, and emits assemblable source; each chip's disassembler is a backend over it |
| [SPC700 disassembler](docs/spc700-disassembler.md) | **complete** — names hardware registers, marks run-time-patched bytes, cycle costs measured from the core |
| [65816 disassembler](docs/65816-disassembler.md) | **complete** — carries the register widths through `REP`, `SEP` and `XCE`, reports an address read two ways and an operand nothing settled, names the registers by bank with the part of the machine each belongs to, cycle costs measured under each mode |
| [Cartridge disassembler](docs/snes-disassembler.md) | **complete** — a whole cartridge into a source tree: one file per bank, the boot-uploaded sound program captured from the machine as SPC700 source of its own, a manifest naming the files, the entries, the stops, every register the code reaches, every DMA transfer, and every routine with its calls and its role; the cartridge is run on the machine so the destinations its indirect jumps take become entries and every range the transfer engines move is recorded with where it came from, unattended or played from an [input script](docs/input-script.md); the lifted program is read for what every path proves, so a jump table the bytes bound is traced past without running and the manifest says what the direct register, the data bank and the stack pointer are at every label; the bank files are written from the lifted form with registers as operands, labels across files and a header per routine. The coprocessors have no backend |
| [Cartridge verifier](docs/snes-disassembler.md#verifying-the-tree) | **complete** — assembles every file of a tree, places the bytes where the manifest says, and reports every difference from the image; thirty-one cartridges across all three maps rebuild byte for byte |
| [Assemblers](docs/assemblers.md) | **complete** — the SPC700 and 65816 dialects over one [common layer](docs/assembly-lexicon.md), each built from its disassembler's own table so every opcode round-trips; the 65816 dialect follows the widths the way the disassembler does |
| [Intermediate representation](docs/ir.md) | **complete** — every 65816 instruction lifted into a form with no bytes in it: what source says, and the typed effects the chip performs with every wrap, flag and cycle rule stated. An interpreter runs the effects and is held to the core by the vector suite and by a whole recorded run replayed on the machine; a renderer writes SNES assembly back, and the cartridge disassembler's bank files come through it; a dataflow runs the effects over every path and proves the registers, the stored values and the bounded jump tables the trace then follows |

### Validation

Beyond the per-cycle vector suites, the machine runs self-checking SPC test ROMs end-to-end. The
CPU, timer and memory-access-timing ROMs pass in full. The DSP ROM does not yet: three of its
sub-tests still report a wrong checksum, and every other sub-test in it passes.

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

The public headers are under [`include/snaggletooth/`](include/snaggletooth); the pages under
[`docs/`](docs/README.md) describe each component's surface and how to drive it.

### Running the tools

A cartridge into a source tree, the tree proved against the image, and its program written out
as the lifted form:

```
snes_disasm game.sfc -o game
snes_verify game game.sfc
snes_lift game game.sfc -o game.snagir
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
`SNAGGLETOOTH_BUILD_TESTS` turns the suite and the tools on or off explicitly.

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

The rendering PPU is the next piece of work. After it, the machine is taken through the range of
cartridge types until they boot, and the DSP returns to close out its three sub-tests with the
wider body of real software available to exercise it.

**1.0 means a fully-featured, accurate SNES emulator core.** No dates are promised; each
component ships when it meets the accuracy bar.

## License

MIT — see [LICENSE](LICENSE).
