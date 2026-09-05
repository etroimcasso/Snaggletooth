# Cartridge tools

Four things live here: `snes_disasm`, which disassembles a whole cartridge into a
source tree; `snes_verify`, which assembles that tree back and compares it with
the image; `rom_render`, which boots a cartridge on the whole SNES machine and
writes what the audio unit plays; and `cartridge_entries.h`, where a disassembly of
a whole cartridge starts.

## Contents

- [`snes_disasm`](#snes_disasm)
- [`snes_verify`](#snes_verify)
- [`rom_render`](#rom_render)
- [`cartridge_entries.h`](#cartridge_entriesh)
- [See also](#see-also)

## `snes_disasm`

```
snes_disasm <image> -o <directory> [--no-sound] [--boot-seconds N] [--no-run] [--run-seconds N] [--input <script>]
```

Writes one source file per bank, the sound program the cartridge uploads at boot as
`apu/driver.asm`, the bytes the run saw the cartridge send from the image to the
hardware as files of their own under `vram/`, `cgram/`, `oam/`, `apu/` and
`hdma/`, and `project.manifest`, which names the files, where the trace began,
and where it stopped. The trace starts at the vectors and follows control
flow across banks; the sound program is captured by booting the cartridge on the
machine and matched back to the image bytes it was read from. A manifest already in
the directory supplies entries a person added and the file split, which is how the
trace is carried past a jump table. A bank file's instructions are written from
the [intermediate representation](../ir/README.md) the listing lifts to, with
what the tree knows attached as names: an `EQU` prologue for the hardware
registers the file addresses and the labels other files define, registers as
operands (`STA !INIDISP`), labels across files (`JSL >sub_018000`), and a
comment above each routine with its size, its role, what it calls and what
calls it.

```
snes_disasm game.sfc -o game
16 files, 1371 instructions, 5 entries, 1 stops
sound program: entry $0500, 3 blocks, 3 matched to the image
524288 of 524288 bytes placed -> game
```

It also runs the cartridge: `rom/rom_observe.h`'s `observeRun` boots the machine
and steps it, recording the destination of every indirect jump or call the run
took, and the trace starts from each — see the
[Running the cartridge](../../docs/snes-disassembler.md#running-the-cartridge)
section — lifting every instruction the CPU executes from the bytes it fetched
and holding it to the machine through the representation's lockstep, so that
every place the CPU arrived without an instruction naming it is a `ran` line
the trace starts from too, and the direct register and the data bank the run
saw at every site are `seen` lines beside what the paths prove — see
[Where a run landed, and what it saw](../../docs/snes-disassembler.md#where-a-run-landed-and-what-it-saw)
— and recording every range of bytes the two transfer engines moved,
with the instruction that started it, the channel, the register it reached, the
memory address it began at, its length and how many times the run saw it, as
`moved` lines the manifest keeps — see
[What a run moved](../../docs/snes-disassembler.md#what-a-run-moved) — and
lifting every such range that begins in the image out of its bank into a file
of its own, the bank file including it with `INCBIN` and the manifest recording
it as an `asset` line, read back so a name a person gives a file survives — see
[The assets](../../docs/snes-disassembler.md#the-assets). `--no-run`
skips the run; `--run-seconds N` bounds it; `--input <script>` plays it,
replaying an [input script](../../docs/input-script.md) — which buttons are held
on which port from which frame — into the controller ports, so the run reaches
what a player would. `rom/input_script.h` reads the script (`parseInputScript`)
and says what a port holds at a frame (`InputScript::padAt`).

The facts it attaches to addresses — the hardware each instruction reaches, the
DMA transfers those add up to, the routines the instructions belong to, each
with what it calls and what it drives, and what every path proves about the
direct register, the data bank and the stack pointer at each label — come from
`rom/rom_facts.h`: `proveProgram(disassembly, rom)` lifts every region and runs
the [dataflow](../ir/README.md) over it, then `hardwareAccesses(disassembly,
&proven)`, `dmaTransfers(accesses)`, `routines(disassembly)` and
`stateFacts(disassembly, proven)`, written into the manifest as `access`, `dma`,
`routine` and `state` lines. `derivedTargets(disassembly, proven)` is every
destination of a jump through a table whose index the bytes bound; the
disassembler traces from each and writes it as a `derived` line, so a bounded
table is traced past without running the cartridge.

The library behind it is `rom/rom_disasm.h`: `disassembleCartridge` for the whole
run, `captureUpload` for the boot alone, `placeBytes` for the count, and the
renderers and `writeProject` for the files, the lifted ones (`AssetFile`)
included. Full page:
[docs/snes-disassembler.md](../../docs/snes-disassembler.md); the manifest's
grammar: [docs/project-manifest.md](../../docs/project-manifest.md).

## `snes_verify`

```
snes_verify <directory> <image> [-o <rebuilt>]
```

Reads the directory's `project.manifest`, assembles every file it names — the
bank files with the 65816 dialect, the sound program with the SPC700 dialect —
places each range and each placed block where the manifest says, and compares the
whole with the image. One line per file, one per run that differs with its first
differing byte, then the totals and the verdict; the exit status is 0 only when
the tree assembles to the image. `-o` writes the rebuilt image.

```
snes_verify game game.sfc
bank_00.asm: 1 range, 32768 bytes, identical
…
apu/driver.asm: 3 blocks, 11974 bytes, identical
524288 of 524288 bytes compared, 0 differ
the tree assembles to the image
```

The library behind it is `rom/rom_verify.h`: `verifyTree` for a directory,
`verifyProject` for a parsed manifest and a file reader, `renderReport` for the
text. Full page:
[docs/snes-disassembler.md §Verifying the tree](../../docs/snes-disassembler.md#verifying-the-tree).

## `rom_render`

```
rom_render <in.smc> (--seconds N | --samples N) -o <out.wav> [--quiet]
```

Boots the cartridge, runs it for the requested length, and writes the S-DSP's
32 kHz stereo output as a canonical PCM WAV. Rendering starts at power-on and runs
forward verbatim — no warm-up, no skip, no fade — so the opening seconds are
whatever the game does before it starts its sound driver, silence included. A
512-byte copier header is dropped when the file length says one is present.

```
rom_render game.sfc --seconds 90 -o game.wav
```

The audio carries the command stream the game's own code sends its driver, which a
standalone `.spc` image does not have. The tool links `snaggletooth_spc` for the WAV
writer.

## `cartridge_entries.h`

A cartridge's header names the handlers the CPU jumps to on reset and on every
interrupt — the addresses execution reaches before the program has run an
instruction — and those are the entry points a trace of the cartridge begins from.

```cpp
#include "rom/cartridge_entries.h"
#include "snaggletooth/snes/cartridge.h"

const std::optional<snaggletooth::CartridgeHeader> header =
    snaggletooth::parseCartridgeHeader(image);
for (const snaggletooth::disasm::VectorEntry& entry :
     snaggletooth::disasm::vectorEntries(*header)) {
  entry.address;  // in bank $00
  entry.name;     // "reset", "nmi", "irq", "cop", "brk", "abort"; "_native" on the native set
}
```

`vectorEntries` returns the vectors that land in ROM under the header's map, in
vector-table order with reset first; a vector pointing at RAM or a register is left
out. `codeOwner(map, address)` says which disassembler owns the bytes at a bus
address — the main CPU's for cartridge ROM, none for RAM, registers, a save window
and open bus. Both read the cartridge through the library's own header functions,
so a tool and the machine never disagree about a byte.

The library target is `snaggletooth_rom`; `tools/` is on its public include path,
and it links both chip backends, the representation the bank files are
rendered from, and the lockstep the run holds its nodes to the machine with.

## See also

- [docs/snes-disassembler.md](../../docs/snes-disassembler.md) — the whole
  cartridge as a source tree and its verification, and
  [docs/project-manifest.md](../../docs/project-manifest.md), the manifest one
  writes and both read.
- [docs/input-script.md](../../docs/input-script.md) — the recorded run
  `snes_disasm --input` replays.
- [docs/spc-rendering.md](../../docs/spc-rendering.md) — `rom_render` beside
  `spc_render`, and what each source carries.
- [docs/disassembly-framework.md §Cartridges](../../docs/disassembly-framework.md#cartridges)
  — the entry points in the framework's terms.
- [docs/snes-cartridge.md](../../docs/snes-cartridge.md) — the header, the three
  maps, and where every bus address lands in the image.
- [`../ir/`](../ir/README.md) — the representation the bank files are written
  from, and the renderer that writes them.
