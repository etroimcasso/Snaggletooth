# Cartridge disassembler

`snes_disasm` disassembles a whole cartridge into a source tree: one file per
bank, the sound program the cartridge uploads at boot as a file of its own, and a
manifest that says where every file's bytes land in the image, where the trace
began, and where it stopped. `snes_verify` does the reverse: it assembles every
file the manifest names, places the bytes where the manifest says, and reports
the difference from the image. The tree is written in the
[65816](65816-assembly.md) and [SPC700](spc700-assembly.md) dialects over the
[common layer](assembly-lexicon.md), and the manifest's grammar is
[project-manifest.md](project-manifest.md).

The trace starts at the handlers the cartridge header names and follows control
flow across banks, so a byte anywhere in the image is code only when execution
can reach it from a vector — or from an entry a person adds to the manifest, or
from a destination the cartridge, run on the machine, was seen to take. The
sound program is found by booting the cartridge on the machine and reading what
reached the audio unit before its program started, then matched back to the bytes
of the image it was read from.

> **Status.** The tree is complete — every image byte is in exactly one file at
> its offset — and `snes_verify` proves it assembles back: thirty-one real
> cartridges' trees, across all three maps, rebuild their images byte for byte.
> The cartridge is run on the machine and the destinations its indirect jumps
> took become entries, so a cartridge whose dispatch goes through pointers is
> traced past them. The manifest says what the code reaches: every register
> access, every transfer, and every routine with what it calls and what it
> drives.
> The coprocessors have no backend, so a cartridge carrying one is traced as far
> as the main CPU's own code goes, and its tree still verifies.

---

## Contents

- [Command line](#command-line)
- [The tree](#the-tree)
- [The trace across banks](#the-trace-across-banks)
- [The sound program](#the-sound-program)
- [Stops, and getting past them](#stops-and-getting-past-them)
- [Running the cartridge](#running-the-cartridge)
- [What is placed](#what-is-placed)
- [Verifying the tree](#verifying-the-tree)
- [What the code reaches](#what-the-code-reaches)
- [Library](#library)
- [Status](#status)
- [See also](#see-also)

## Command line

```
snes_disasm <image> -o <directory> [--no-sound] [--boot-seconds N] [--no-run] [--run-seconds N] [--input <script>]
snes_verify <directory> <image> [-o <rebuilt>]
```

Reads a cartridge image, writes the tree under the directory, creating it, and
reports what it found:

```
$ snes_disasm cartridge.sfc -o cartridge
2 files, 29 instructions, 2 entries, 0 stops
65536 of 65536 bytes placed -> cartridge
```

A cartridge that uploads a sound program at boot reports that too, on a line of
its own: the entry the audio CPU started it at, how many blocks were sent, and
how many of those were matched back to bytes of the image.

A 512-byte copier header is dropped when the file length says one is present.

`--no-sound` skips the boot, so no sound program is looked for and the banks keep
every byte. `--boot-seconds N` bounds the boot at N seconds of the master clock;
the default is fifteen, more than a cartridge that clears its memory, unpacks its
program and streams tens of kilobytes of samples takes to start it. A cartridge
that has not started its sound program in that time is reported as a `note` in
the manifest and its banks keep every byte.

`--no-run` skips [running the cartridge](#running-the-cartridge); `--run-seconds N`
bounds the run at N seconds of the master clock, sixty by default. `--input <script>`
replays an [input script](input-script.md) into the controller ports while the
cartridge runs, so the run plays the game rather than watching it; a script that
cannot be read is refused with its line named, and `--input` under `--no-run` is
refused as well, having nothing to replay into.

When the directory already holds a `project.manifest`, its `entry`, `reached` and
`file` lines are read first, and the manifest must name the image it was written for: a
manifest written for another image is refused rather than applied. See
[Stops, and getting past them](#stops-and-getting-past-them).

## The tree

```
cartridge/
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
`irq_native` — except `cop` and `brk`, which are mnemonics and cannot be
labels, so those handlers are `cop_handler` and `brk_handler`. A routine
reached from another bank carries `sub_` or `loc_` with its address, the way
the disassemblers name any target, and a branch, jump or call whose target is
in the same file names that label: `JSR !sub_0080E8`, `BNE loc_008034`,
`JSL >sub_008A4E`. A target in another file is written as its address, since
a symbol does not cross a file.

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
the entries that land in it: each vector's handler in the mode the CPU takes that
vector in — the emulation-mode set, reset among them, in emulation mode, and the
native set in native mode with the widths unknown, since the image cannot say
what the interrupted code had — and any entry a person added. A
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

## Running the cartridge

A trace names a target only when the instruction names it. Four forms do not —
`JMP (!abs)`, `JMP (!abs,X)`, `JML [!abs]` and `JSR (!abs,X)` take their
destination from a pointer in memory — and a cartridge whose dispatch goes through
one of them is a wall to the trace: every stop it reports is real, and the
destinations only exist when the cartridge runs. So the disassembler runs it. The
machine is booted and stepped one instruction at a time for sixty seconds of its
clock; every time it is about to execute one of those four forms, the pointer it
is about to read is read first, the way the CPU reads it, and the destination is
recorded with the mode the instruction carries in. Each is a
[`reached` line](project-manifest.md#27-what-a-run-reached) in the manifest, and
the trace starts from it as it starts from a vector.

A cartridge that dispatches through a table:

```
$ snes_disasm cartridge.sfc -o cartridge --no-sound --run-seconds 1
1 files, 12 instructions, 1 entries, 2 stops
```

```
reached  $00:8240 loc_008240 e=0 m=8 x=8 from $00:800B
reached  $00:8220 loc_008220 e=0 m=8 x=8 from $00:8245

stop     $00:800B `JMP (!$8100,X)`: the target is computed at run time; add an entry for each destination
stop     $00:8245 `JMP (!$8100)`: the target is computed at run time; add an entry for each destination
```

Both stops stand, since the bytes still do not name the targets; both targets are
now code in the tree, labelled by the form that took them:

```
        JMP (!$8100,X)                  ; $00:800B  7C 00 81  6

loc_008240:
        A8
        X8
        LDA #$0F                        ; $00:8240  A9 0F     2
```

Nothing is inferred from where the CPU landed. The pointer is read before the
step, and the landing only confirms it: a step that services an interrupt instead
lands in the handler and records nothing, a step on which a DMA transfer holds the
CPU off the bus runs no instruction and records nothing, and the jump is seen on
the step that runs it. A pointer the run cannot read — one in a register window or
the save window rather than the image or work RAM — is named in a `note` and
recorded nowhere. The run is deterministic: work RAM is cleared at power-on and
the machine has no other seed, so the same cartridge reaches the same set.

A run sees what it exercised. An unattended boot reaches what the cartridge does
on its own — its title, its attract mode — and no further. To reach the rest, the
run is played: `--input <script>` replays an [input script](input-script.md) into
the controller ports as the machine runs, holding the buttons the script names
from the frames it names, and the program reads them through the auto-read and
the serial ports exactly as it reads a pad. A cartridge whose interrupt handler
takes one jump when Start is down and another when A is, neither named by the
bytes:

```
$ snes_disasm cartridge.sfc -o cartridge --no-sound --run-seconds 1
1 files, 5 instructions, 2 entries, 0 stops
$ cat play.txt
frame 5 1 start
frame 9 1 a
$ snes_disasm cartridge.sfc -o cartridge --no-sound --run-seconds 1 --input play.txt
1 files, 11 instructions, 2 entries, 0 stops
```

```
reached  $00:8200 loc_008200 e=1 m=8 x=8 from $00:8311
reached  $00:8210 loc_008210 e=1 m=8 x=8 from $00:833B
```

Each run's script is a person's record of what was played, and different scripts
reach different code. The manifest keeps every `reached` line from one
disassembly to the next and merges a new run's with them, so an unattended run
and a played one over the same directory accumulate — and an `entry` a person
adds still names what no run has taken.

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

That count says the tree describes every byte. Whether the text assembles back
to them is what `snes_verify` answers.

## Verifying the tree

```
snes_verify <directory> <image> [-o <rebuilt>]
```

Reads the directory's `project.manifest`, assembles every bank file with the
65816 dialect and the sound file with the SPC700 dialect, places each range a
file emits at the image offset its address reads from under the manifest's map
and each placed block at the offset the manifest recorded, and compares the
whole with the image:

```
$ snes_verify cartridge cartridge.sfc
bank_00.asm: 1 range, 32768 bytes, identical
bank_01.asm: 1 range, 32768 bytes, identical
…
bank_0E.asm: 4 ranges, 20794 bytes, identical
bank_0F.asm: 1 range, 32768 bytes, identical
apu/driver.asm: 3 blocks, 11974 bytes, identical
524288 of 524288 bytes compared, 0 differ
the tree assembles to the image
```

The exit status is 0 only for that last line, which is said only when every
file assembled, every image byte was produced by exactly one file, and none
differ. Anything else is reported where it is:

```
bank_00.asm: 1 range, 32768 bytes, 1 differ
  bank_00.asm $00:8000-$00:FFFF at $000000: first difference at $000000
bank_01.asm: 1 error, not assembled
  bank_01.asm:412: `BOGUS` is not a 65816 instruction
32768 of 65536 bytes compared, 1 differ, 32768 produced by no file
the tree does not assemble to the image
```

A run that differs names the file, the range as the file placed it, where it
lands in the image, and the first differing byte. A file that does not assemble
is reported with the assembler's own diagnostics and produces nothing, so its
bytes are counted among those no file produced. Bytes two files produce are
counted too, since a tree that holds a byte twice is not a clean rebuild
whatever the bytes say.

`-o` writes the image the tree assembled to, whatever the verdict, with a byte
nobody produced as `$00`. A manifest written for another image is refused, as
the disassembler refuses it. A copier header on the image is dropped the same
way.

The sound file's blocks are compared only where the manifest placed them; an
`unplaced` block's bytes are in its bank, and are compared there. A placed
block the sound file does not emit whole is reported rather than compared
against the fill.

## What the code reaches

A listing says which registers an instruction names. The manifest says what that
adds up to: for every instruction the trace decoded that reaches a hardware
register, an `access` line with the register, the part of the machine it belongs
to, whether the instruction reads or writes it, and the value it wrote where the
bytes say what that was; and for every DMA channel a routine set up, a `dma` line
with the transfer those accesses describe. Both are
[manifest lines](project-manifest.md#26-what-the-code-reaches), written fresh on
every run.

The destination of a transfer is the register the channel's `BBAD` names — the
value written there, not `BBAD` itself — so its class is what the transfer is
for. A transfer to `OAMDATA` is a sprite table, one to `VMDATAL` a tileset or a
tilemap, one to `CGDATA` a palette, one to `APUIO0` a sound driver or its
samples — each named by where it is sent, rather than by anything a person has
labelled yet:

```
dma      $00:8017 channel 0 to-register $00:2104 OAMDATA Oam source $7F:0000 start $01
dma      $00:8045 channel 1 to-register $00:2118 VMDATAL Vram source none start $02
dma      $00:8082 channel 2 direction-unknown $00:2122 CGDATA Cgram source none start none
```

**A value is what the bytes say and no more.** It is recorded where the
instruction immediately before loaded it as an immediate, with no label between
them — the `LDA #$8F` / `STA !$2100` idiom every cartridge is written in — and
where the instruction is `STZ`, which carries its own zero. Anything else leaves
the field `none`: the second transfer above has no source because its address
registers were filled from a table, and the third's direction is unknown because
nothing wrote its `DMAP` with a value the bytes settle. A run of straight-line
code is as far as a value carries, so pieces of one channel written across a call
are not joined.

An instruction under a sixteen-bit register reaches two registers and produces a
line for each, which is how one `STA !$4301` sets both a channel's B-bus address
and the low byte of its source.

**The routines.** A [`routine` line](project-manifest.md#28-routines) per
routine says which lines belong together, which routines it calls, and its
role — the classes its own lines reach, and the classes its calls reach through
every routine they call in turn:

```
routine  $00:8000 reset lines 26 bytes 66 calls sub_018000 reaches Display,Vram,Oam,Interrupt,DmaControl,DmaChannel through Cgram,DmaChannel
routine  $01:8000 sub_018000 lines 3 bytes 6 calls none reaches Cgram,DmaChannel through none
```

A routine is what execution reaches from a label by falling through, branching
and jumping, without passing a return or a halt and without entering the routine
a call names; every entry, every target a run reached, and every label a call
names begins one. Nothing else draws a boundary: a routine with no return of its
own runs on into the next label's code and holds those lines too, and a line two
routines reach is in both. That is what the bytes say, and the labels stay the
trace's — `sub_` and `loc_` with the address — until a person names them; the
role is what to name them from.

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
snaggletooth::disasm::writeProject(tree, "cartridge", error);
```

`disassembleCartridge` returns the header, the entries traced from, one
`RegionListing` per region — its `SourceRegion` and its `Listing`, whole — the
`SoundProgram` when one was captured, the `TraceStop`s, and `notes` for what the
run could not do. `bankRegions(map, imageBytes)` is the default split.
`captureUpload(rom, masterCycles, reason)` is the boot alone: the entry and the
blocks, each with its `romOffset` when the image holds it. `placeBytes` builds the
image the tree describes and counts what is unplaced or placed twice.

`reached` carries [what the run reached](#running-the-cartridge): one
`ReachedTarget` per destination — `target`, the `mode` it arrived in, the `site`
that took it, whether it was a `call`, and the `name` the tree gives it.
`rom/rom_observe.h` is the run itself: `observeRun(rom, masterCycles, input, notes)`
boots the machine, replays `input` — an [`InputScript`](input-script.md#6-library),
empty for the boot alone — into the controller ports, and returns the sightings
in site order; `sameSighting` says whether two are one. `CartridgeRequest::observeRun`
asks for the run — off unless asked, since it costs about as long as it emulates;
`snes_disasm` asks unless told `--no-run` — `runMasterCycles` bounds it, and
`CartridgeRequest::input` is the script it replays.

`accesses` and `dmas` carry [what the code reaches](#what-the-code-reaches).
`rom/rom_facts.h` is the producer, over a finished `CartridgeDisassembly`:
`hardwareAccesses(disassembly)` gives one `HardwareAccess` per register an
instruction reaches — its `site`, `registerAddress`, `name`, `cls`, `kind`, the
`value` where the bytes say it, and the `run` of straight-line code it sits in —
and `dmaTransfers(accesses)` gives one `DmaTransfer` per channel a run set up.
`accessKindName` and `dmaDirectionName` are their names as text. `routines`
carries one `Routine` per routine — its `address` and `label`, the `lines` it
holds and their `bytes`, the routines it `calls`, and its role as `reaches` and
`through` — from `routines(disassembly)`, which reads the finished listings, the
accesses and the transfers.

The files are text from `renderRegion`, `renderSoundProgram` and
`renderManifest`; `parseManifest` reads a manifest's entries, file split, map,
sound program and image identity back, and `manifestMismatch` says whether that
manifest can direct a run over a given image. `writeProject` writes all of it
under a directory.

Verification is `rom/rom_verify.h`:

```cpp
#include "rom/rom_verify.h"

const snaggletooth::disasm::VerifyReport report =
    snaggletooth::disasm::verifyTree("cartridge", image);
if (!report.identical()) std::cout << snaggletooth::disasm::renderReport(report);
```

`verifyTree(directory, rom)` reads the manifest and the files from the
directory; `verifyProject(manifest, rom, read)` takes a parsed `ManifestInput`
and reads each file through a function, which is how a front end verifies a
tree it holds in memory. The `VerifyReport` carries one `VerifiedFile` per file
— its diagnostics when it did not assemble, the runs and bytes compared, the
bytes differing — every `VerifyMismatch` with its first differing offset, the
rebuilt `image`, the totals, and `identical()`. `renderReport` is the text the
command prints.

The library target is `snaggletooth_rom`, which links both chip backends and
through them both assemblers; `tools/` is on its public include path.

## Status

The tree covers the main CPU's banks and the sound program uploaded at boot. The
cartridge coprocessors — the SuperFX, the DSP series, the Cx4, the ST018 — have
no backend, so a cartridge carrying one is traced as far as the main CPU's own
code goes; a cartridge that cannot boot without its coprocessor yields no sound
program, and says so in a `note`. Its tree still verifies, since a coprocessor's
program is data to the main CPU's file and comes back byte for byte.

The run answers the stops it took and no others: a jump the run never reached
keeps its `stop` alone, and a person's `entry` is still the way past it. A run
without a script reaches what a cartridge does on its own; with one it reaches
what the script plays, and no script plays everything — coverage is what a person
exercises, run by run, and the manifest accumulates it.

What the code reaches is reported for the main CPU's regions. The sound program is
another chip's, with registers of its own, and has no `access` or `routine`
lines. A value carries one instruction and no further, and only within a run of
straight-line code, so a channel configured from a table or across a call leaves
the fields it did not settle `none` rather than guessing at them. A routine's
role counts what the bytes reach and what a run reached; a call through a
pointer the run did not take contributes nothing to `through`, since nothing
names its target.

Every tree in a corpus of thirty-one cartridges — LoROM, HiROM and ExHiROM,
from 512 KB to 6 MB — assembles back to its image byte for byte under
`snes_verify`. The trace's own limits stand: a jump through a table stops it,
and an entry in the manifest is how a person carries it past.

## See also

- [Project manifest](project-manifest.md) — the manifest's grammar, what is read
  back, and its stability.
- [Input script](input-script.md) — the recorded run `--input` replays, its
  grammar and its refusals.
- [The assemblers](assemblers.md) — the tools `snes_verify` rebuilds the
  tree's files with, and their diagnostics.
- [65816 disassembler](65816-disassembler.md) and
  [SPC700 disassembler](spc700-disassembler.md) — the two backends the tree's
  listings come from.
- [Disassembly framework](disassembly-framework.md) — the tracer and renderer,
  and the cartridge entry points.
- [SNES cartridge](snes-cartridge.md) — the header, the three maps, and where
  every bus address lands in the image.
- [SNES machine](snes-machine.md) — the machine the boot runs on, and the
  upload handshake.
