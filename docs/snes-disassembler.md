# Cartridge disassembler

`snes_disasm` disassembles a whole cartridge into a source tree: one file per
bank, the sound program the cartridge uploads at boot as a file of its own, and a
manifest that says where every file's bytes land in the image, where the trace
began, and where it stopped. The tree is written in the
[65816](65816-assembly.md) and [SPC700](spc700-assembly.md) dialects over the
[common layer](assembly-lexicon.md), and the manifest's grammar is
[project-manifest.md](project-manifest.md).

The trace starts at the handlers the cartridge header names and follows control
flow across banks, so a byte anywhere in the image is code only when execution
can reach it from a vector — or from an entry a person adds to the manifest. The
sound program is found by booting the cartridge on the machine and reading what
reached the audio unit before its program started, then matched back to the bytes
of the image it was read from.

> **Status.** The tree is complete — every image byte is in exactly one file at
> its offset — and its listings are the two disassemblers' output. The
> [assemblers](assemblers.md) rebuild it: every file of a real cartridge's tree,
> assembled and placed at the offset the manifest names, gives the image's bytes.
> The command that does that placing and reports the difference — `verify` — is
> not built; until it is, the proof is run file by file.

---

## Contents

- [Command line](#command-line)
- [The tree](#the-tree)
- [The trace across banks](#the-trace-across-banks)
- [The sound program](#the-sound-program)
- [Stops, and getting past them](#stops-and-getting-past-them)
- [What is placed](#what-is-placed)
- [Library](#library)
- [Status](#status)
- [See also](#see-also)

## Command line

```
snes_disasm <image> -o <directory> [--no-sound] [--boot-seconds N]
```

Reads a cartridge image, writes the tree under the directory, creating it, and
reports what it found:

```
$ snes_disasm "Super Mario World.smc" -o smw
16 files, 1371 instructions, 5 entries, 1 stops
sound program: entry $0500, 3 blocks, 3 matched to the image
524288 of 524288 bytes placed -> smw
```

A 512-byte copier header is dropped when the file length says one is present.

`--no-sound` skips the boot, so no sound program is looked for and the banks keep
every byte. `--boot-seconds N` bounds the boot at N seconds of the master clock;
the default is fifteen, more than a cartridge that clears its memory, unpacks its
program and streams tens of kilobytes of samples takes to start it. A cartridge
that has not started its sound program in that time is reported as a `note` in
the manifest and its banks keep every byte.

When the directory already holds a `project.manifest`, its `entry` and `file`
lines are read first, and the manifest must name the image it was written for: a
manifest written for another image is refused rather than applied. See
[Stops, and getting past them](#stops-and-getting-past-them).

## The tree

```
smw/
  project.manifest
  bank_00.asm
  bank_01.asm
  …
  bank_0F.asm
  apu/driver.asm
```

**One file per bank.** Each `bank_XX.asm` covers the ROM window of one bank
whole, from its first byte to its last, under one `ORG`: instructions where the
trace reached, `DB` runs everywhere else. Every image byte is placed once, at the
address `romAddress` reports for its offset, so a mirror is never written twice —
a bank reached through `$80`–`$FF` is written as `$00`–`$7F` under LoROM, and
under HiROM the banks are `$C0`–`$FF`. The file starts the way every listing
does:

```
        ORG $00:8000

reset:
        EMULATION
        SEI                             ; $00:8000  78           2
        STZ !$4200                      ; $00:8001  9C 00 42     4  NMITIMEN
        STZ !$420C                      ; $00:8004  9C 0C 42     4  HDMAEN
```

The vectors' handlers carry the vector's name as their label — `reset`, `nmi`,
`irq_native` — and a routine reached from another bank carries `sub_` or `loc_`
with its address, the way the disassemblers name any target.

**The sound program as its own file.** The bytes the cartridge sends to the audio
unit are written once, as SPC700 source in `apu/driver.asm`, and the bank that
carried them leaves that range to it:

```
; ---- 4 bytes execution did not reach
        DB $3E,$0E,$00,$05              ; $0E:8000  |>...|

; ---- $0E:8004-$0E:8E41: the sound program, see apu/driver.asm
        ORG $0E:8E42
```

The four bytes are the upload table's own header for that block — its length and
destination — which the main CPU reads and the audio unit never receives; the
block itself is in the sound program's file. A bank file therefore carries an
`ORG` wherever a range was left out, and the lexicon's `ORG` rule, forward only
and never over emitted bytes, is what makes the file one program.

**The manifest.** `project.manifest` names the image, every file and the range it
covers, the sound program and each of its blocks with the image offset it was read
from, every entry the trace started from with its mode, every stop, and every
warning the listings raised. Its grammar is [project-manifest.md](project-manifest.md).

## The trace across banks

Every region is traced by the [65816 disassembler](65816-disassembler.md) from
the entries that land in it: the reset handler in emulation mode, every interrupt
handler in native mode with the widths unknown, and any entry a person added. A
call or a jump whose target is in another region — `JSL`, `JML`, and an absolute
jump whose bank is not the current one — enters that region's trace under the mode
the instruction was made in, so `REP #$30` in bank `$00` is what makes an immediate
three bytes in the routine bank `$01` is called into. A target reached through a
mirror bank is brought home first, so `JML $82:8000` traces `bank_02.asm`.

A new entry in a bank already traced sends the trace round again, and it goes
round until no bank has an entry it has not seen. A bank nothing reaches is one
`DB` run.

Two paths reaching one address under different widths are reported, not chosen,
exactly as the single-bank tool reports them; the warning names the file in the
manifest and heads the file itself:

```
warning  bank_00.asm $00:98E1 is reached with e=0 m=8 x=8 and with e=0 m=16 x=8
```

## The sound program

The cartridge is booted on the machine, twice: once over cleared audio memory
and once over audio memory filled with `$FF`. The boot runs until the audio CPU
leaves the upload stub for the program it was sent, which is the moment the
program starts, and its program counter then is the entry. A byte the upload
wrote reads the same after both boots; a byte it never touched reads as each
boot's fill. The runs of written bytes are the blocks.

Each block is then looked for in the image. A block the image holds at exactly
one place is placed there; a block it holds in pieces — two tables uploaded end to
end from two places in the image — is reported as those pieces, each at its own
place; a block the image does not hold as it is, because the cartridge unpacked
or transformed it on the way, is reported `unplaced`, and the bank keeps its
bytes. Only a placed block is left out of its bank.

The listing is traced from the entry over the uploaded blocks with the
[SPC700 disassembler](spc700-disassembler.md), and the file opens with what was
sent:

```
; The sound program the cartridge uploads at boot, traced from $0500.
; $0500: 3646 bytes, read from image offset $070004
; $1360: 5661 bytes, read from image offset $0718B5
; $5570: 2667 bytes, read from image offset $070E46

        ORG $0500

entry:
        CLRP                            ; $0500  20        2
        MOV X,#$CF                      ; $0501  CD CF     2
        MOV SP,X                        ; $0503  BD        2
```

What the boot captures is the program the cartridge starts with. A cartridge
that sends another program later — a different driver per level, samples
streamed on demand — sends it after the capture ends, and those bytes stay in
their banks as data.

## Stops, and getting past them

Static tracing ends where the bytes do not name the next address. The manifest
lists each such place as a `stop`:

```
stop     $00:86F7 `JML [!$0000]`: the target is computed at run time; add an entry for each destination
```

A jump or call through a table or a pointer — `JMP (!abs,X)`, `JML [!abs]`,
`JSR (!abs,X)` — is a stop, and so is a call or jump whose target is not in the
image, work RAM for instance, where the program copied a routine before running
it. `BRK` and `COP` are not stops: they continue at their vectors' handlers, which
are entries already.

A stop is answered with an entry. Add a line to the manifest naming the address
the trace should continue from, a label for it, and the mode execution arrives in:

```
entry    $00:9A12 sprite_table_0 e=0 m=8 x=16
```

and run the tool again over the same directory. The entry is traced with the
vectors, and the tree grows. The `file` lines are read back the same way, so the
file split is a person's to change: a region may be any range within one bank, and
the default the tool writes is one region per bank.

## What is placed

The tree is complete when every image byte is in exactly one file at its offset.
The tool reports it on every run:

```
524288 of 524288 bytes placed
```

The count is made from the listings' own byte records — an instruction's bytes or
a data run, placed at the offset its address reads from — and from the sound
program's placed blocks. Bytes no file carries, and bytes two files carry, are
counted and reported when there are any; a region split that leaves a bank
without a file is the usual way to have bytes nobody carries.

## Library

```cpp
#include "rom/rom_disasm.h"

snaggletooth::disasm::CartridgeRequest request;
request.rom = image;                       // std::span<const std::uint8_t>
request.entries = {};                      // beyond the vectors
request.regions = {};                      // empty: one file per bank
request.captureSound = true;
const snaggletooth::disasm::CartridgeDisassembly tree =
    snaggletooth::disasm::disassembleCartridge(request);

std::string error;
snaggletooth::disasm::writeProject(tree, "smw", error);
```

`disassembleCartridge` returns the header, the entries traced from, one
`RegionListing` per region — its `SourceRegion` and its `Listing`, whole — the
`SoundProgram` when one was captured, the `TraceStop`s, and `notes` for what the
run could not do. `bankRegions(map, imageBytes)` is the default split.
`captureUpload(rom, masterCycles, reason)` is the boot alone: the entry and the
blocks, each with its `romOffset` when the image holds it. `placeBytes` builds the
image the tree describes and counts what is unplaced or placed twice.

The files are text from `renderRegion`, `renderSoundProgram` and
`renderManifest`; `parseManifest` reads a manifest's entries, file split and image
identity back, and `manifestMismatch` says whether that manifest can direct a run
over a given image. `writeProject` writes all of it under a directory.

The library target is `snaggletooth_rom`, which links both chip backends;
`tools/` is on its public include path.

## Status

The tree covers the main CPU's banks and the sound program uploaded at boot. The
cartridge coprocessors — the SuperFX, the DSP series, the Cx4, the ST018 — have
no backend, so a cartridge carrying one is traced as far as the main CPU's own
code goes; a cartridge that cannot boot without its coprocessor yields no sound
program, and says so in a `note`.

Operands are written as addresses, not labels, so a generated label is defined
and never referenced. The trees round-trip as they are — each bank file and the
sound program's file assemble, with [`cpu65816_asm` and `spc700_asm`](assemblers.md),
to the bytes the manifest places them at. The command that assembles a whole
tree from its manifest and reports the difference from the image, `verify`, is
not built, and neither are symbolic operands.

## See also

- [Project manifest](project-manifest.md) — the manifest's grammar, what is read
  back, and its stability.
- [The assemblers](assemblers.md) — the tools that rebuild the tree's files.
- [65816 disassembler](65816-disassembler.md) and
  [SPC700 disassembler](spc700-disassembler.md) — the two backends the tree's
  listings come from.
- [Disassembly framework](disassembly-framework.md) — the tracer and renderer,
  and the cartridge entry points.
- [SNES cartridge](snes-cartridge.md) — the header, the three maps, and where
  every bus address lands in the image.
- [SNES machine](snes-machine.md) — the machine the boot runs on, and the
  upload handshake.
