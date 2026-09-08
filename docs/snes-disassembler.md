# Cartridge disassembler

`snes_disasm` disassembles a whole cartridge into its program file —
`program.snagir`, the whole program in the [intermediate representation](snagir.md)
— and a manifest that says where every file's bytes land in the image, where the
trace began, and where it stopped, with the bytes the cartridge sends to the
hardware as files of their own kind and the sound program the cartridge uploads
at boot as a file of its own. `snes_render` writes the source tree from the
program file: one file per bank, rendered from the program in the file and the
facts in the manifest, by a tool that holds neither the image nor the trace.
`snes_verify` does the reverse: it assembles every file the manifest names,
places the bytes where the manifest says, and reports the difference from the
image. The tree is written in the
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
> traced past them; every instruction the run executes is lifted from the bytes
> the CPU fetched and checked against the machine, so where the CPU arrived
> without an instruction naming it — a return to an address the code put on the
> stack — becomes an entry too, and the direct register and the data bank the
> run saw at every site are recorded beside what the paths prove; every range
> of bytes the transfer engines moved is recorded with where it came from,
> where it went and which instruction sent it — and, for the video memories,
> which words, palette entries or sprite bytes it landed on and what the PPU
> used them as when the run saw them drawn — and every such range that begins
> in the image is lifted out of its bank into a file of its own under a
> directory named for the memory it went to and, for VRAM, for whether the
> picture used it as a map or as tiles, the bank file including it where
> it was; and the lifted program is read for what every path proves, so a jump
> through a table whose index the bytes bound is traced past without running.
> The manifest says what the code reaches: every register access, every
> transfer, every routine with what it calls and what it drives, and the direct
> register, data bank and stack pointer at every label where the paths settle
> them — and the bank files say it beside the code, written from the
> intermediate representation: registers as operands, labels across files, a
> header per routine.
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
- [Where the CPU arrived, and what it saw](#where-the-cpu-arrived-and-what-it-saw)
- [What a run moved](#what-a-run-moved)
- [The assets](#the-assets)
- [Where the bytes came from](#where-the-bytes-came-from)
- [What is placed](#what-is-placed)
- [Verifying the tree](#verifying-the-tree)
- [What the code reaches](#what-the-code-reaches)
- [What every path proves](#what-every-path-proves)
- [Library](#library)
- [Status](#status)
- [See also](#see-also)

## Command line

```
snes_disasm <image> -o <directory> [--no-sound] [--boot-seconds N] [--no-run] [--run-seconds N] [--input <script> | --input-dir <directory>]
snes_render <directory>
snes_verify <directory> <image> [-o <rebuilt>]
```

`snes_disasm` reads a cartridge image, writes `program.snagir`, the manifest, the
lifted files and the sound program's file under the directory, creating it, and
reports what it found; `snes_render` then writes the bank files from the program
file and the manifest:

```
$ snes_disasm cartridge.sfc -o cartridge
2 files, 29 instructions, 2 entries, 0 stops
65536 of 65536 bytes placed -> cartridge
```

A cartridge that uploads a sound program at boot reports that too, on a line of
its own: the entry the audio CPU started it at, how many blocks were sent, and
how many of those were matched back to bytes of the image.

A copier's header ahead of the image — the 512 bytes a Super Wild Card, a Pro Fighter, a Game
Doctor or a Super UFO wrote so it could load the file again — is dropped, and a line says which
copier wrote it and what it declares:

```
dropped a Super Wild Card header: 512 KB, HiROM, 8 KB of save RAM mapped HiROM, a program; the image is 3584 bytes longer than declared
```

[The cartridge](snes-cartridge.md#a-copiers-header) says how each copier's header is recognised
and read.

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
refused as well, having nothing to replay into. `--input-dir <directory>` names a
directory of recorded runs instead: the one named for the image — its file name
without the extension, spaces as underscores, `.txt` — is replayed when it is
there, and the ports stay empty when it is not. The two forms are not given
together.

When the directory already holds a `project.manifest`, its `entry`, `reached`,
`ran`, `derived`, `moved`, `asset` and `file` lines are read first, and the manifest must name the image
it was written for: a manifest written for another image is refused rather than
applied. See [Stops, and getting past them](#stops-and-getting-past-them).

## The tree

The disassembler writes `program.snagir`, `project.manifest`, the lifted files
and `apu/driver.asm`; the bank files are `snes_render`'s, written from the
program file. Together:

```
cartridge/
  program.snagir
  project.manifest
  bank_00.asm
  bank_01.asm
  …
  bank_0F.asm
  apu/driver.asm
  vram/00_9000.bin
  cgram/00_9200.bin
  oam/00_9300.bin
  hdma/00_9700.bin
```

**One file per bank.** Each `bank_XX.asm` covers the ROM window of one bank
whole, from its first byte to its last, under one `ORG`: instructions where the
trace reached, `DB` runs everywhere else, and an `INCBIN` where a range was
lifted into a file of its own. Every image byte is placed once, at the
address `romAddress` reports for its offset, so a mirror is never written twice —
a bank reached through `$80`–`$FF` is written as `$00`–`$7F` under LoROM, and
under HiROM the banks are `$C0`–`$FF`. The instructions are written from the
[intermediate representation](ir.md#rendering-source) the listing lifts to, with
the facts the manifest carries attached as names. This is the start of
`bank_00.asm` for the `mixed` cartridge from
[`tools/examples/`](../tools/examples/README.md):

```
; The hardware registers this file names, and the labels other files
; define that it refers to.
NMITIMEN  EQU $4200
RDNMI     EQU $4210

        ORG $00:8000

; routine reset: 21 lines, 46 bytes
;   reaches Interrupt; through none
;   calls sub_008040; called by none
reset:
        EMULATION
        CLC                             ; $00:8000  18        2
        XCE                             ; $00:8001  FB        2
        REP #$30                        ; $00:8002  C2 30     3
        LDX #$0002                      ; $00:8004  A2 02 00  3
        LDA #$1234                      ; $00:8007  A9 34 12  3
        STA !$0100                      ; $00:800A  8D 00 01  5
        INC !$0100                      ; $00:800D  EE 00 01  8
        SEP #$20                        ; $00:8010  E2 20     3
        LDA #$05                        ; $00:8012  A9 05     2
        STA $10                         ; $00:8014  85 10     3
        LDA !$FFFF,X                    ; $00:8016  BD FF FF  5
        PHA                             ; $00:8019  48        3
        PLA                             ; $00:801A  68        4
        JSR !sub_008040                 ; $00:801B  20 40 80  6
        LDA #$80                        ; $00:801E  A9 80     2
        STA !NMITIMEN                   ; $00:8020  8D 00 42  4

loc_008023:
        INC !$0102                      ; $00:8023  EE 02 01  6
        LDA !$0104                      ; $00:8026  AD 04 01  4
        CMP #$03                        ; $00:8029  C9 03     2
        BNE loc_008023                  ; $00:802B  D0 F6     2/3
        STP                             ; $00:802D  DB        3
```

Each line is the instruction, then a comment with its address, its bytes, its
cycle cost and any annotation, as the [65816 disassembler](65816-disassembler.md#output)
writes a listing. What the file adds is names.

**The prologue.** The file opens with an `EQU` for every hardware register its
absolute operands address and every label another file defines that it refers
to — and nothing else, so the prologue is a list of what the file touches. A
register's `EQU` carries the 16-bit offset, which the absolute forms take in
every bank; a label's carries its full 24-bit address, which `>` takes.

**Register names as operands.** An absolute data operand that the manifest's
`access` line names is written as the register: `STA !NMITIMEN`, `LDA !RDNMI`,
`STA !VMDATAL,X`. That is exactly the set the listing annotates — an absolute
operand taken in bank zero — so the operand says what the comment said. A long
operand keeps its address with the name in the comment, since a long symbol
would need the bank in the name. A register whose name cannot be a symbol —
`$4016` is two registers, `JOYSER0/JOYOUT` — keeps its address and its comment,
as does one whose name the file already uses as a label.

**Labels across files.** The vectors' handlers carry the vector's name as their
label — `reset`, `nmi`, `irq_native` — except `cop` and `brk`, which are
mnemonics and cannot be labels, so those handlers are `cop_handler` and
`brk_handler`. A routine reached from another bank carries `sub_` or `loc_` with
its address, the way the disassemblers name any target. A branch, jump or call
whose target carries a label anywhere in the tree names it — `JSR !sub_0080E8`,
`BNE loc_008034` in the same file; `JSL >sub_018000` into another, whose `EQU`
the prologue carries. A target the bytes name through a mirror bank keeps its
address, since a symbol would carry the bank the bytes are placed in rather than
the one they name.

**Routine headers.** Every routine that begins in the file carries a comment
above its label: how many lines and bytes it holds, the parts of the machine it
reaches itself and through what it calls, what it calls, and what calls it — the
manifest's [`routine` line](#what-the-code-reaches) read out beside the code it
describes.

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

**The assets as files of their own.** A range the cartridge, run on the machine,
was seen to send from the image to the hardware is written once, as the bytes
are, in a file under a directory named for the memory it went to, and the bank
file includes it where it was — from `bank_00.asm` of the `lifting` cartridge:

```
; ---- $00:9000-$00:904F: 80 bytes a transfer carried to VMDATAL, in vram/00_9000.bin
        INCBIN "vram/00_9000.bin"

; ---- 432 bytes execution did not reach
```

The `INCBIN` emits the file's bytes at that address, so the bank's range runs on
through it; the comment says what the run saw the bytes were for, and the file
they are in. [The assets](#the-assets) says which ranges are lifted and which are
not.

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

Some stops the bytes answer themselves. A jump or call through a table in the
image whose index every path bounds — `AND #$07`, `ASL`, `TAX`, `JMP (!table,X)`;
or `CMP #$03`, `BCS` past the jump, then the same — names every destination it
can take, and each is a [`derived` line](project-manifest.md#210-what-the-bytes-derive)
the trace starts from, as it starts from an entry. A jump every destination of
which is derived is not a stop. What is bounded and how is
[what every path proves](#what-every-path-proves), below.

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

## Where the CPU arrived, and what it saw

The four forms are not the only way a program leaves the bytes' flow. A
routine dispatches by pushing an address and returning to it; a program builds
a return frame by hand and enters a routine with `RTL`; an `RTI` returns into
flow the bytes never carried. None of those reads a pointer the run can read
ahead, so the run reads the landing instead — and to know what a landing is,
it has to know what every instruction names. It does: every instruction the
CPU executes is lifted from the bytes the CPU fetched, wherever they lay,
through the same lift the tree's program comes from, and run through the
[interpreter](ir.md#running-beside-the-machine) held to the machine exactly as
the differential holds it — every access, every register, every cycle — so a
lift the machine disagrees with is a `note`, never a silent wrong fact. After
each instruction the run compares where the CPU went with what the instruction
names: the address after it, its constant target, the pointer the four forms
read, the vector a `BRK` or `COP` takes, and for a return, an address the run's
own calls and interrupts said to expect one at. Anything else is a
[`ran` line](project-manifest.md#213-where-the-cpu-arrived), and the trace starts
from it as it starts from a `reached` line. A landing in work RAM is a `note`,
since the tree has nothing to trace there.

A cartridge that copies a routine into work RAM, builds two return frames by
hand and enters it with `RTL`, lands past a `PEA`/`RTS` pair on both passes of
a loop, and returns through `RTI` into a frame it built with both widths
sixteen bits:

```
$ snes_disasm cartridge.sfc -o cartridge --no-sound --run-seconds 1
1 files, 41 instructions, 3 entries, 1 stops
program.snagir: 41 nodes
note: run: the CPU arrived at $7E:2000 from $00:802B, which the tree does not hold; not recorded
```

The stop is the `JSL` into work RAM, which the bytes name and the tree cannot
hold; the note is the `RTL` that followed it there.

```
ran      $00:803A loc_00803A e=1 m=8 x=8 from $00:8039
ran      $00:804D loc_00804D e=0 m=16 x=16 from $00:804C
ran      $00:802C loc_00802C e=1 m=8 x=8 from $7E:2001
```

The `RTS` was taken on both passes of the loop and is written once; the `RTI`
ran with both widths eight and arrived with both sixteen, and the line carries
the mode the CPU arrived in. Without the run, everything after the `RTL` at
`$00:802B` is data; with it, all three landings are labels with the code under
them, and the routine's return from work RAM is one of them:

```
loc_00802C:
        STA !$0102                      ; $00:802C  8D 02 01     4
```

The same lockstep sees the registers at every site. At each instruction the
run executes in the image, the direct register and the data bank it held are
recorded as a [`seen` line](project-manifest.md#214-what-a-run-saw), every
value the run saw, beside the `state` line that says what
[every path proves](#what-every-path-proves) — the run's answer where the
paths say `?`, and a disagreement where the run saw a value the paths do not
prove. A plain direct-page operand the paths prove nothing about, at a site the
run saw one direct register, carries in its comment the register that value
lands it on, marked `(run)`; a site the run saw two values at, an indexed form,
and a site the paths already prove get no such name:

```
seen     $00:802C D=$4300|$4310 DBR=$00
seen     $00:803F D=$4320 DBR=$00
```

```
        STA $01                         ; $00:803F  85 01        3  BBAD2 (run)
        STA $03,X                       ; $00:8041  95 03        4
```

The manifest keeps every `ran` line as it keeps `reached`, and writes the
`seen` lines fresh on every run: they are what the run saw, and the next run
sees it again.

## What a run moved

The same run watches the two transfer engines. A `dma` line (see
[What the code reaches](#what-the-code-reaches)) says what a channel was set up
for, and only where the bytes say the values: a routine that uploads a level's
tiles takes its source from a pointer, so its `dma` line reads `source none`
and always will. The run knows. Every byte a general-purpose transfer or an
HDMA channel moves crosses the bus in the engine's name, and the run records
each range of them as a [`moved` line](project-manifest.md#211-what-a-run-moved):
the instruction that started it, the channel, which way the bytes went, the
register they reached with its class, the memory address they began at and how
it stepped, how many there were, whether they were a transfer, an HDMA table as
a frame walked it or the block an indirect entry pointed at, and how many times
the run saw that exact range.

A cartridge that fills a channel from a pointer, run for one second:

```
$ snes_disasm cartridge.sfc -o cartridge --no-sound --run-seconds 1
1 files, 143 instructions, 3 entries, 0 stops
```

```
dma      $00:8132 channel 7 to-register $00:2104 OAMDATA Oam source none increment bytes none start none from none

moved    $00:8326 channel 7 to-register $00:2104 OAMDATA Oam memory $7E:0200 increment bytes 544 as dma times 60
```

The `dma` line stands where the channel's `BBAD` was written, and its source,
its count and its start are `none` because the address registers are filled
and the channel started elsewhere — here in the vertical-blank handler, which
writes them and starts the channel every frame.
The `moved` line stands at the handler's write to `MDMAEN`, names the 544 bytes
from `$7E:0200`, and says the run saw them go sixty times. A range in work RAM
is a table the program builds; a range in the image is bytes the cartridge
ships, which is what an asset is.

What is recorded is what the engine did. A `fixed` step is a fill from one byte
and says so, since sixty-four bytes from one address are not a range of the
image. An HDMA table is recorded once per frame that walked it, from its start
to where the walk stopped — so a table the run's end cut part-way is its own
range with the bytes it had read — and each block an indirect entry points at
is a range of its own, named by the register the channel reaches. Two channels
started by one write are two lines at one site; a write of zero starts nothing.
The register is the one the channel's `BBAD` names; a pattern that reaches a
second register is implied by the byte count.

A `moved` line's site is an instruction the run executed, at the address the
CPU executed it — a cartridge that runs its code from the fast mirror banks
names them, and the same bytes are in the file named after their home bank.
Where the site lies in a `DB` run of that file, the run reached code the trace
did not — a routine behind a jump the run took and the bytes do not name — and
an `entry` for the routine is how the trace catches up. A range whose memory address is in
work RAM was built by the program: a table the cartridge decompresses or draws
before sending it, which the image holds in another form, or not at all.

The manifest keeps every `moved` line from one disassembly to the next: a range
this run saw again carries this run's count, one it did not see is kept as it
was, and a run skipped with `--no-run` keeps them all. So a tree carries where
the hardware's bytes came from whether or not the disassembly that wrote it ran
the cartridge.

**Where a range landed.** The `moved` line says what the engine did on the
A bus; what happened on the other side of the port is the
[`landed` line](project-manifest.md#217-where-a-transfer-landed) beside it.
Every write through a video data port says where the port put the byte —
the machine reports it with the access ([SNES machine](snes-machine.md#the-bus-observer)) — and
the run keeps, for each range, the lowest and the highest VRAM word, palette
word or OAM byte its bytes reached. Then, at the first frame the PPU draws
after the bytes landed, it reads the screen mode and the base registers as
they stand at that frame's start to say what the memory was: a
layer's screen (`tilemap1`–`tilemap4`), a layer's name base at the colour
depth the mode gives it (`tiles1`–`tiles4`), the sprite tiles (`sprites`),
the whole of VRAM under Mode 7 (`mode7`), several of those joined by `+`,
`none` for a stretch no base reaches, or `unshown` when no frame was drawn
before the run ended. A range the engine was still carrying at a frame's
start has its later bytes read at the next drawn frame, and its areas are
every area read. A palette landing is `palette` and an OAM landing `oam`,
whatever the frame. The first drawn frame is the moment the picture first
used the words, so an upload made in forced blank with the bases written
afterwards, or into the half of a double buffer the screen is not showing,
is read as what it became, not as what the registers said when the engine
ran. A range whose bytes reach no data port — a table to the brightness
register, a copy into work RAM through its port, a read back — has no line;
a range that landed in two places has two, each with its own count.

The `landing` cartridge from [`tools/examples/`](../tools/examples/README.md)
uploads to every video memory in forced blank and sets its bases only
afterwards; the vertical-blank handler then flips a base behind an upload,
switches to Mode 7 and blanks the screen before one last upload:

```
$ snes_disasm landing.smc -o landing --no-sound --run-seconds 1
1 files, 365 instructions, 3 entries, 0 stops
program.snagir: 365 nodes
32768 of 32768 bytes placed -> landing
```

```
landed   $00:8034 channel 0 memory $00:9000 bytes 64 as dma at $3000-$301F in tiles1+tiles2 times 1
landed   $00:8066 channel 0 memory $00:9100 bytes 64 as dma at $0000-$001F in tilemap1 times 1
landed   $00:80CA channel 0 memory $00:9300 bytes 32 as dma at $5000-$500F in none times 1
landed   $00:8129 channel 0 memory $00:9500 bytes 32 as dma at $10-$1F in palette times 1
landed   $00:8349 channel 0 memory $00:9700 bytes 64 as dma at $7800-$781F in tilemap2 times 1
landed   $00:837B channel 0 memory $00:9D00 bytes 64 as dma at $0200-$021F in tilemap1+tilemap2 times 1
landed   $00:8401 channel 0 memory $00:9A00 bytes 64 as dma at $0100-$011F in unshown times 1
landed   $00:8430 channel 0 memory $00:A000 bytes 544 as dma at $000-$21F in oam times 2
landed   $00:8430 channel 0 memory $00:A000 bytes 544 as dma at $010-$21F in oam times 1
```

The tileset from `$9000` was uploaded before any base was written and is
read as the name base BG1 and BG2 share; the map from `$9700` was uploaded
to `$7800` while BG2's screen was elsewhere, and the flip that followed is
what the first drawn frame saw — a screen of four from `$7400`, whose last
wraps round the end of VRAM to hold the map from `$9D00` at `$0200` beside
BG1's; the upload from `$9A00` was made after the
screen was blanked and no frame drew it. The sprite table sent from the
handler landed at byte zero on two frames — the PPU reloads the OAM address
at the start of every vertical blank — and at `$010` on the one the handler
had moved the address first. The whole set is the
[manifest page's example](project-manifest.md#217-where-a-transfer-landed).
The line is written fresh and read back by nothing; what follows from it —
the directory a VRAM file lives under — is the next section's, and is kept
by the file's `asset` line.

## The assets

Every range the run saw an engine carry begins somewhere, and where it begins in
the image, the bytes are the cartridge's own — a tileset, a palette, a sprite
table, an HDMA table — and are lifted out of their bank into a file of their
own. The bank file includes the file where the bytes were, with `INCBIN`, so the
tree still assembles to the image; the manifest records the file as an
[`asset` line](project-manifest.md#212-assets); and the `moved` lines are its
uses, joined on the address.

**What is lifted.** A `moved` range whose bytes went to a register, stepping up
or down through memory that is the image: a general-purpose transfer to VRAM, to
CGRAM, to OAM or to the audio port, or an HDMA table or a block an indirect entry
pointed at, to any register. The file lives under `vram/`, `cgram/`, `oam/` or
`apu/` for a transfer, `hdma/` for a table or a block — and `staged/` for a
source the run's shadow named whose bytes were built into data for two
classes, the next section — and is named `<bank>_<offset>.bin` after the
address of its first byte. A VRAM file goes further, by
[where its bytes landed](#what-a-run-moved): when every landing the run saw
drawn of every range the file's bytes went through — its own ranges, or the
ranges built from it, or the stream that carried it — lies in a layer's
screen, the file lives under `maps/`; when every one lies in a name base or
the sprite tiles, under `tiles/`; and when a landing was never drawn, lies in
no area, lies under Mode 7, mixes a screen with tiles, or when there is no
landing at all — a transfer the code proves, a tree disassembled without a
run — it stays under `vram/`, which is the honest answer. The
`landing` cartridge's thirteen files:

```
asset    tiles/00_9000.bin Vram as dma from $00:9000 bytes 64
asset    maps/00_9100.bin Vram as dma from $00:9100 bytes 64
asset    vram/00_9200.bin Vram as dma from $00:9200 bytes 64
asset    vram/00_9300.bin Vram as dma from $00:9300 bytes 32
asset    tiles/00_9400.bin Vram as dma from $00:9400 bytes 64
asset    cgram/00_9500.bin Cgram as dma from $00:9500 bytes 32
asset    maps/00_9700.bin Vram as dma from $00:9700 bytes 64
asset    vram/00_9800.bin Vram as dma from $00:9800 bytes 64
asset    tiles/00_9900.bin Vram as dma from $00:9900 bytes 32
asset    vram/00_9A00.bin Vram as dma from $00:9A00 bytes 64
asset    maps/00_9B00.bin Vram as staged from $00:9B00 bytes 32
asset    maps/00_9D00.bin Vram as dma from $00:9D00 bytes 64
asset    oam/00_A000.bin Oam as dma from $00:A000 bytes 544
```

The range from `$9200` began in BG3's screen and ended in the name bases,
`$9300` landed where no base reaches, `$9800` was uploaded under Mode 7 and
`$9A00` was never drawn: all four stay under `vram/`. The thirty-two bytes at
`$9B00` were copied into work RAM and sent from there into BG1's screen, so
their source is placed as a map; the map from `$9D00` lies in two screens at
once, and two screens are still maps. A range read downward is lifted in image order,
which is how its bytes lie. Ranges that share a byte are one file, the union of
them — a tileset sent whole and then sent piece by piece is one tileset; ranges
that only touch are two files, since the run cannot say whether a chunked
upload is one asset or two.

**What is not, without a word.** A fill from one byte, which is not a range of
anything; a read from a register back into memory, whose memory is the
destination; a range in work RAM, save RAM or a register window — the program
built it, and the image holds it in another form or not at all; and a
general-purpose transfer to any other register, such as a copy into work RAM
through its port, whose bytes could be anything. A transfer whose address runs
past the end of its bank and on from the bank's start is lifted as far as its
bytes were in the image, and a `note` says how many were not; under HiROM the
bytes after the wrap are the image too, the bank's own first bytes, and are a
file of their own — the `wrapping` cartridge lifts `vram/C1_FFF0.bin` and
`vram/C1_0000.bin` from one transfer of thirty-two.

**What is refused, with a note.** A range over an instruction the trace decoded
— the instruction is the trace's reading of those bytes, and a file cut through
it would change what the tree says the code is; a range over a block of the
sound program, which is already another file's; and a range whose overlapping
ranges the engines sent two places, or two ways, since a file is one thing. Each
is left as `DB` rows in its bank, and the manifest says why.

The `lifting` cartridge from [`tools/examples/`](../tools/examples/README.md),
which sends bytes every way the rules have a case for, run for one second:

```
$ snes_disasm lifting.smc -o lifting --no-sound --run-seconds 1
2 files, 271 instructions, 1 entries, 0 stops
program.snagir: 271 nodes
note: moved $00:8165 channel 0 memory $00:8000 bytes 16: $00:8000-$00:800F overlaps an instruction the trace decoded; not lifted
note: moved $00:8205 channel 0 memory $01:FFF0 bytes 32: 16 of its bytes are not the image and are not lifted
note: the bytes at $00:9800 were sent to VMDATAL and CGDATA; not lifted
note: the bytes at $00:9C00 were sent to VMDATAL and VMDATAH; not lifted
65536 of 65536 bytes placed -> lifting
```

```
asset    vram/00_9000.bin Vram as dma from $00:9000 bytes 80
asset    cgram/00_9200.bin Cgram as dma from $00:9200 bytes 16
asset    oam/00_9300.bin Oam as dma from $00:9300 bytes 544
asset    apu/00_9600.bin Apu as dma from $00:9600 bytes 8
asset    hdma/00_9700.bin Cgram as table from $00:9700 bytes 7
asset    hdma/00_9710.bin Cgram as indirect from $00:9710 bytes 2
asset    hdma/00_9712.bin Cgram as indirect from $00:9712 bytes 2
asset    vram/00_9900.bin Vram as staged from $00:9900 bytes 16
asset    vram/01_8000.bin Vram as dma from $01:8000 bytes 32
asset    vram/01_FFF0.bin Vram as dma from $01:FFF0 bytes 16
```

Three transfers from `$9000` — sixty-four bytes, sixteen inside them, thirty-two
across their end — are the one file of eighty; the two blocks the HDMA table
points at lie end to end and are two files; the transfer from `$01:FFF0` read
its last sixteen bytes from `$01:0000`, which is not the image, and its file
holds the sixteen that were. The three notes are the three refusals: the same
bytes sent two places, twice — once to two classes, once to two registers of
one — and the reset routine's own bytes sent to VRAM. The `staged` file is the
next section's: `$01:0000` is work RAM, and the sixteen bytes there were the
ones the port had copied in from `$9900`, so the transfer carried them on and
the run knows it.

**A name is a person's.** The disassembler writes every source file fresh, so a
path edited in a bank file is gone on the next run; the manifest is where a
name lives. Rename the file, change its `asset` line to match, and the next run
writes the file under that name and includes it by it, since a line whose
address and length match a file lifted again gives it its path. The old file is
not removed — the disassembler never deletes anything — and an `asset` line
that matches no range the run lifts is dropped, with a `note` saying so. A tree
disassembled with `--no-run` lifts what its manifest's `moved` lines say, so the
files are kept from one run to the next whether or not the cartridge ran.

**What the code proves is lifted too.** A `dma` line whose every field the
paths settle — a general-purpose start, a destination in VRAM, CGRAM, OAM or
the audio port, a source stepping up or down through the image, and a count
— names a range as exactly as a `moved` line does, and where no run started
it, the file is lifted from the code's word: the same directory, the same
name, an `asset` line of kind `proven`, and an `INCBIN` in the bank file. A
fill, a transfer whose start the code does not prove, an HDMA enable (whose
count is not a length) and a source outside the image lift nothing, and say
nothing. Where the run did start the transfer, the `moved` range is the file
and the proof is only checked against it. The `declaring` cartridge, whose
reset code sets five channels up behind a button the run never presses:

```
asset    vram/00_9000.bin Vram as dma from $00:9000 bytes 32
asset    vram/00_9040.bin Vram as dma from $00:9040 bytes 16
asset    vram/00_9100.bin Vram as dma from $00:9100 bytes 48
asset    cgram/00_9200.bin Cgram as dma from $00:9200 bytes 16
asset    oam/00_9300.bin Oam as proven from $00:9300 bytes 544
```

The run took five transfers and confirmed each; the sprite table at `$9300`
is lifted because the code proves it whole. The 48 bytes at `$9100` are
proven whole too, and the run sent sixteen from inside them: a proven range
and a range the run moved that share bytes are one file, the run's kind, as
two of the run's ranges would be. The fill from `$9600`, the HDMA table at
`$9700`, the transfer whose source came from a variable and the one set up
and never started are left where they are.

Most of what a cartridge sends is built in work RAM first — decompressed, drawn,
assembled from pieces — and those ranges are recorded as `moved` lines whose
memory is work RAM. The image holds them in another form, and the run's shadow
says which: see the next section.

## Where the bytes came from

Beside the interpreter the run keeps a shadow
([ir.md §The shadow](ir.md#the-shadow)): every value carries the image offsets
it was computed from, work RAM keeps the origin and the last writer of every
byte, and every byte the engines and the port move keeps it current. So when a
transfer carries a range out of work RAM, the run reads the shadow under it and
the manifest says where the bytes came from — an
[`origin` line](project-manifest.md#215-where-a-staged-range-came-from) per
writer and per source — and what was built from each lifted file, a `staged`
line.

**An origin is data.** A value's origin is the set of image bytes its value was
computed from: a load takes the origin of the bytes it read, never of the
address it read them at; an operation takes the union of its operands'; a
constant and a flag have none. A value the image does not hold carries a mark
instead — a hardware register, named, or the save — so `computed` is said only
of bytes built from constants alone. The origin of a decoder's output is the
literal bytes it copied; the counts and lengths that decided how many are not
in it, since they never reached a value.

**A source is what the writer read.** The shadow follows every call and return
and keeps, for each invocation, the maximal stretches of the image it read
as runs, by the rule
[project-manifest.md §2.15](project-manifest.md#215-where-a-staged-range-came-from)
states: a read inside a run changes nothing, any other read starts a run of
the one byte, runs that touch are one, and a helper's runs join its caller's
as if the caller had read them. So two files uploaded a chunk at a time, turn
and turn about across vertical blanks, stay two whole runs; a decoder's
stream stays one run however many tables it consults and however often it
re-reads a byte; a copy that walks its source from the end reads one run. The
source of a staged byte is the run holding the byte's origin among the
writer's invocation's runs, followed outward through each caller whose run
begins or ends where it does — that caller read on from it: the compressed
stream whole, counts included, and not a table the decoder
consulted whose values never reached the output; for a decoder called once
per chunk, the whole file its caller's run grew into; and for a helper that
reads two bytes inside a block its caller had read whole, the two bytes — the
caller's run holds them strictly inside, and the search ends there. The `origin` line names the source and says, with `using`, how many
of its bytes the origin covers — every byte for a copy, the literals for a
decoder. Where no run holds the origin — the
bytes were staged through work RAM twice, or an engine wrote them — the
origin's own intervals are the source. A source may span more than one asset a
person would cut; the `staged` lines say which parts were used, and a file is
easier to split than to complete.

**A staged range is lifted as its source.** Each source of an extent in work
RAM that went to a register a file can be named for — carried by an engine, as
a `moved` range, or by the CPU a store at a time, as a `streamed` run — is
lifted under the same rules as a range from the image, as an `asset` of kind
`staged`, and placed by where the range built from it landed, as a range from
the image is; the `staged` line says which extent was built from it, which class
it went to, and by which routine. A computed range is not lifted, and nothing
is said. The same bytes sent directly and sent after staging are one file. A
source whose bytes were built into data for two classes — a block that
decodes into tiles and a palette — is one file under `staged/`, its `asset`
line naming both classes, with a `staged` line per extent and class; the
later passes read the lines, not the directories, so nothing is missed. A
routine's name is the routine's label; `none` is an engine's write, and a site
written as an address is one the trace holds no routine at — code the run
executed that the trace did not reach, which an `entry` answers. Consistent
addresses are what a reader can follow; a real name is a person's to give.

**A stream is lifted as the run its carrier read.** Consecutive stores the CPU
made to a data register — `VMDATAL`/`VMDATAH`, `CGDATA`, `OAMDATA`, the audio
ports — of consecutive bytes are recorded as a
[`streamed` line](project-manifest.md#216-what-the-cpu-streamed). Bytes loaded
from work RAM are the buffer being carried out, and the buffer is an extent
exactly as if an engine had carried it — the `streamed` line names it, and its
source is the file. Bytes with consecutive image origins are the image being
carried, and the file is the run of image bytes the carrying invocation read
that holds the first, as an `asset` of kind `stream`: a tileset the CPU copies
a word at a time is a file exactly as one an engine carries, and a loop that
reads a count before its data or an end mark after it lifts them with it.

The `staging` cartridge from [`tools/examples/`](../tools/examples/README.md)
builds eight ranges in work RAM every way the shadow has a rule for, then sends
each, run for one second:

```
$ snes_disasm staging.smc -o staging --no-sound --run-seconds 1
1 files, 268 instructions, 1 entries, 0 stops
program.snagir: 268 nodes
32768 of 32768 bytes placed -> staging
```

```
origin   $7E:0400 bytes 32 from $00:9300 bytes 32 using 32 by none exact
origin   $7E:0500 bytes 2 from $00:9100 bytes 32 using 2 by sub_008300 exact
origin   $7F:0000 bytes 32 from $00:9000 bytes 11 using 5 by sub_008100 exact
origin   $7F:0100 bytes 32 from $00:9100 bytes 32 using 32 by sub_008140 exact
origin   $7F:0300 bytes 16 computed by sub_0081A0
origin   $7F:0600 bytes 3 from $00:9400 bytes 3 using 3 by sub_008380 exact
origin   $7F:0700 bytes 8 from $00:9500 bytes 8 using 8 by sub_0083C0 exact
origin   $7F:0800 bytes 16 from $00:9600 bytes 16 using 16 by sub_008480 exact

asset    vram/00_9000.bin Vram as staged from $00:9000 bytes 11
asset    vram/00_9100.bin Vram as staged from $00:9100 bytes 32
asset    cgram/00_9200.bin Cgram as stream from $00:9200 bytes 17
asset    vram/00_9300.bin Vram as staged from $00:9300 bytes 32
asset    hdma/00_9400.bin Display as staged from $00:9400 bytes 3
asset    staged/00_9500.bin Vram+Cgram as staged from $00:9500 bytes 8
asset    vram/00_9600.bin Vram as staged from $00:9600 bytes 16

staged   vram/00_9300.bin at $7E:0400 bytes 32 to Vram by none exact
staged   vram/00_9100.bin at $7E:0500 bytes 2 to Vram by sub_008300 exact
staged   vram/00_9000.bin at $7F:0000 bytes 32 to Vram by sub_008100 exact
staged   vram/00_9100.bin at $7F:0100 bytes 32 to Vram by sub_008140 exact
staged   hdma/00_9400.bin at $7F:0600 bytes 3 to Display by sub_008380 exact
staged   staged/00_9500.bin at $7F:0700 bytes 8 to Vram by sub_0083C0 exact
staged   staged/00_9500.bin at $7F:0700 bytes 8 to Cgram by sub_0083C0 exact
staged   vram/00_9600.bin at $7F:0800 bytes 16 to Vram by sub_008480 exact

streamed $00:818F $00:2122 CGDATA Cgram from $00:9200 bytes 16 times 1 at $00-$07 in palette
streamed $00:84C8 $00:2118 VMDATAL Vram from $7F:0800 bytes 16 times 1 at $0035-$003D in unshown
```

The decoder at `$8100` unpacked eleven bytes — five runs, each a count and a
value, then a zero — into thirty-two at `$7F:0000`: the origin is the five
values, the source is the eleven, and the file is the eleven. The copy at
`$8140` used every byte of its source. The fill at `$81A0` built sixteen bytes
from a constant, which have no source and no file. An engine carried `$9300`
in through the port and out again, so that extent is the engine's; two stores
by `sub_008300` put the first two bytes of `$9100` through the port, and their
source is the block the copy read, which the file the copy was lifted from
carries a `staged` line for. The loop at `$8180` carried sixteen bytes to
`CGDATA` one at a time and read the end mark after them, so its file holds
seventeen while the `streamed` line says sixteen. The table at `$7F:0600` is
walked by HDMA, so its source is placed as a table the engine reads from the
image is, under `hdma/`. The eight bytes copied to `$7F:0700` went to VRAM and
then to CGRAM, so their source is one file under `staged/` named for both. The
sixteen copied to `$7F:0800` were carried out by the loop at `$84C0` a word at
a time: the `streamed` line names the buffer, and the buffer's source is its
file. A stream lands as a transfer does, and its line says where: the palette
loop's sixteen bytes at entries zero to seven; the VRAM loop's words at
`$0035`, unshown because no frame was drawn between the loop and the run's
end.

In the bank file, a staged file's `INCBIN` says what it is:

```
; ---- $00:9000-$00:900A: 11 bytes a routine built Vram data from, in vram/00_9000.bin
        INCBIN "vram/00_9000.bin"
```

A `staged` or `stream` file's evidence is the run's, written fresh; its `asset`
line is read back whole, so the file is kept through a disassembly without a
run and until a run lifts a wider file over its bytes. A `proven` file's
evidence is the code's, which every trace finds again, so its line is read
back for its path alone, as a `dma` file's is.

**Cost.** The shadow runs beside the interpreter on every instruction: on a
commercial cartridge's sixty seconds it adds about five seconds of wall time
and some thirty megabytes, and an origin is widened to its hull only above
sixty-four intervals — which no range of the five cartridges the tool is
measured on reaches.

## What is placed

The tree is complete when every image byte is in exactly one file at its offset.
The tool reports it on every run:

```
524288 of 524288 bytes placed
```

The count is made from the listings' own byte records — an instruction's bytes or
a data run, placed at the offset its address reads from — from the sound
program's placed blocks, and from the lifted files' bytes. Bytes no file carries, and bytes two files carry, are
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
the disassembler refuses it. A copier's header ahead of the image is dropped and
reported the same way.

The sound file's blocks are compared only where the manifest placed them; an
`unplaced` block's bytes are in its bank, and are compared there. A placed
block the sound file does not emit whole is reported rather than compared
against the fill.

A lifted file needs no line of its own: a bank file's `INCBIN` emits the file's
bytes into the bank's own range, read from the tree's directory as the sources
are, so a tree with assets verifies exactly as one without. A lifted file that
cannot be read is that bank file's assembly error, reported with its line, and
the bank's bytes are then among those no file produced.

## What the code reaches

A listing says which registers an instruction names. The manifest says what that
adds up to: for every instruction the trace decoded that reaches a hardware
register, an `access` line with the register, the part of the machine it belongs
to, whether the instruction reads or writes it, and the value it wrote where the
bytes say what that was; and for every transfer a routine set a channel up for
and started, a `dma` line with the transfer those accesses describe — the
channel's registers as they stood when the start was written. Both are
[manifest lines](project-manifest.md#26-what-the-code-reaches), written fresh on
every run.

The destination of a transfer is the register the channel's `BBAD` names — the
value written there, not `BBAD` itself — so its class is what the transfer is
for. A transfer to `OAMDATA` is a sprite table, one to `VMDATAL` a tileset or a
tilemap, one to `CGDATA` a palette, one to `APUIO0` a sound driver or its
samples — each named by where it is sent, rather than by anything a person has
labelled yet. The source is where the bytes come from and how the address
steps, as `DMAP` says; the count is what `DAS` was written with, which for a
general-purpose transfer is how many bytes move, a zero standing for 65536;
and the start is the write to `MDMAEN` or `HDMAEN` that set the channel going.
A stretch of straight-line code that starts one channel four times has four
lines, each with the registers as they stood — a start that rewrote only the
source and the count sends to the destination the last one left — and a
channel written and never started has a line whose start is `none`:

```
dma      $00:8007 channel 0 to-register $00:2118 VMDATAL Vram source $00:9000 increment bytes 32 start $01 from $00:8025
dma      $00:802A channel 0 to-register $00:2118 VMDATAL Vram source $00:9040 increment bytes 16 start $01 from $00:803E
dma      $00:8048 channel 0 to-register $00:2122 CGDATA Cgram source $00:920F decrement bytes 16 start $01 from $00:8066
dma      $00:8070 channel 0 to-register $00:2118 VMDATAL Vram source $00:9600 fixed bytes 64 start $01 from $00:808E
dma      $00:8098 channel 6 to-register $00:2118 VMDATAL Vram source $00:9080 increment bytes 16 start none from none
dma      $00:81C7 channel 4 to-register $00:2118 VMDATAL Vram source none increment bytes 16 start $10 from $00:81EA
dma      $00:8207 channel 5 to-register $00:2122 CGDATA Cgram source $00:9700 increment bytes 3 start-hdma $20 from $00:8225
```

These are among the `declaring` cartridge's, whose reset code starts channel 0
four times from one stretch of code — a tileset, then sixteen more bytes to
the same place with only the source and the count rewritten, a palette read
downward, a fill — then sets channel 6 up whole and branches before starting
it.

**A value is what the bytes prove and no more.** It is recorded where the
instruction immediately before loaded it as an immediate, with no label between
them — the `LDA #$8F` / `STA !$2100` idiom every cartridge is written in — where
the instruction is `STZ`, which carries its own zero, and where every path into
the store proves the register's value, however far back the load was and
whatever was called in between, as long as the calls give the register back
(see [What every path proves](#what-every-path-proves)). Anything else leaves
the field `none`, and a register written with a value the bytes do not say is
`none` from then on, whatever an earlier write left: the sixth transfer above
has no source because the low byte of its address was rewritten from a
variable. The last is an HDMA enable, and its count is the three the code
wrote and not a length — an HDMA channel takes its counts from its table, and
the engine writes that register itself. Pieces of one channel are joined only
within a run of straight-line code, so a channel set up across a label is two
transfers' worth of lines.

A transfer the code proves whole and the run also started is checked against
the [`moved` line](#what-a-run-moved) from the same start: where the two agree
the run confirms the proof, and where they do not, a `note` says what each
said and neither is changed. A transfer proven whole that no run started is
lifted from the code's word alone — see [The assets](#the-assets).

An instruction under a sixteen-bit register reaches two registers and produces a
line for each, which is how one `STA !$4301` sets both a channel's B-bus address
and the low byte of its source. A direct-page operand reaches a register when
every path proves the direct register it is an offset from: `LDA #$4300`, `TCD`,
then `STA $01` is a write to `BBAD0`, and the line says so.

## What every path proves

The lifted program's effects, run over every path at once with every register a
set of the values it can hold, say what is true at an instruction however
execution reached it. The rules are the ones the
[manifest page](project-manifest.md#29-what-every-path-proves) states: a value is
proven by an immediate, by a transfer of a proven register, by a pull of a byte
pushed on the same straight path, or by a load from the image, and by nothing
else; two paths that prove different values are a disagreement the tree
reports, never a choice; a call carries the caller's values into the routine
and brings back what the routine's returns prove, except a register the routine
gives back as it took it; a hardware interrupt is not a path; and the program
bank is the bank the CPU runs in, carried along every path like any other
value, so a cartridge that runs through a mirror of the bank the tree places it
in proves the mirror's number from `PHK`, and a data bank proven from it names
what that bank holds.

Three things come of it. A [`state` line](project-manifest.md#29-what-every-path-proves)
per label where any of the direct register, the data bank or the stack pointer
is known:

```
state    $00:802A D=$0000 DBR=$7E S=$1FFF
state    $00:8100 D=$0000|$0100|$0200|$4300 DBR=$00 S=$1FFD
state    $00:8380 D=$0100|$0200 DBR=$00 S=$1FFF
```

— the second inside a routine two callers enter with different direct
registers, two bytes down the stack; the third at a label two paths reach with
the direct register set differently.
A [`derived` line](project-manifest.md#210-what-the-bytes-derive) per
destination of a jump through a table the bytes bound:

```
derived  $00:8300 loc_008300 e=0 m=8 x=16 from $00:8033 via $00:8200
derived  $00:8310 loc_008310 e=0 m=8 x=16 from $00:8033 via $00:8202
```

— the index masked to eight values and doubled, so eight pointers, each read
from the address after `via`. And `access` lines a direct-page operand or a
value carried across a call could not have had before.

What is not proven is silent: a register the paths do not settle has `?` in its
`state` line, a table whose index nothing bounds keeps its `stop`, a store whose
value the paths disagree on has `none`. The analysis follows the accumulator
and the index registers by the byte, so an eight-bit load is known while the
other byte is not; it does not follow the carry, so the result of `ADC`, `SBC`,
`ROL` and `ROR` is never known, and it does not follow the decimal flag. Memory
other than the image — work RAM, a register, the save window — is a value the
cartridge supplies when it runs, and a load from it is not known.

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
snaggletooth::disasm::writeProject(tree, "cartridge", error);  // program.snagir, the manifest, the lifted files

std::size_t rendered = 0;
snaggletooth::disasm::renderTree("cartridge", rendered, error);  // the bank files, from the two files on disk
```

`disassembleCartridge` returns the header, the entries traced from, one
`RegionListing` per region — its `SourceRegion` and its `Listing`, whole — the
`SoundProgram` when one was captured, the `assets` lifted out of the banks, the
`TraceStop`s, and `notes` for what the run could not do. Each `AssetFile` is its
path, the `classes` of the registers its bytes went to — one, or two for a
`staged/` file — and the `registerAddress` of the one that named it, its
`kind` (a `MovedKind`), the address of its `first` byte, its `romOffset` and its
`bytes`; `CartridgeRequest::assets` is the `ManifestAsset`s read back from the
manifest — a path with the first address, length and classes it names — which
give a file lifted again its path. `bankRegions(map, imageBytes)` is the default split.
`captureUpload(rom, masterCycles, reason)` is the boot alone: the entry and the
blocks, each with its `romOffset` when the image holds it. `placeBytes` builds the
image the tree describes and counts what is unplaced or placed twice.

`reached` carries [what the run reached](#running-the-cartridge): one
`ReachedTarget` per destination — `target`, the `mode` it arrived in, the `site`
that took it, whether it was a `call`, and the `name` the tree gives it. `ran`
carries [where the CPU arrived](#where-the-cpu-arrived-and-what-it-saw): one
`Landing` per place — `target`, `mode`, `site` and `name`, as a reached target's
— and `seen` one `SeenState` per site the run executed in the image, its
`address` and the values of `d` and `dbr` seen there; `sameLanding` says
whether two landings are one. `moved`
carries [what the run moved](#what-a-run-moved): one `MovedRange` per range —
`site`, `channel`, `toRegister`, `registerAddress` with `registerName` and
`registerClass` where the address has one, `memory`, `step` (a `MovedStep`),
`bytes`, `kind` (a `MovedKind`) and `times`; `movedStepName` and `movedKindName`
are their names as text, `sameRange` says whether two are one range, and
`rangeBefore` is the order they are written in. `landed` carries
[where those ranges landed](#what-a-run-moved): one `LandedRange` per
distinct landing — the `site`, `channel`, `memory`, `bytes` and `kind` of its
`moved` line, the `landing` and `times`. A `PortLanding` is the `memory` the
port reaches (a `PortMemory`: `Vram`, `Cgram`, `Oam`), the `lowest` and
`highest` address the port put a byte at, whether the landing was `shown` —
read at the first frame drawn after it — and the `areas` it lies in, one bit
each of `kAreaTilemap1`–`kAreaTilemap4`, `kAreaTiles1`–`kAreaTiles4`,
`kAreaSprites`, `kAreaMode7`, `kAreaPalette` and `kAreaOam`; `areaText` is
the areas as the manifest writes them, `none` and `unshown` included,
`portAddressText` an address in the memory's width, and `landedBefore` the
order the lines are written in. `rom/rom_observe.h` is the run
itself: `observeRun(rom, masterCycles, input, notes)` boots the machine, replays
`input` — an [`InputScript`](input-script.md#6-library), empty for the boot alone
— into the controller ports, and returns a `RunObservation` holding the
`reached` sightings in site order, the `moved` ranges and their `landed`
landings, the `ran` landings,
the `seen` values, the `staged` extents — one `StagedRange` per extent of work RAM
carried to a register, by an engine or by the CPU a store at a time: `memory`,
`bytes`, the `origin` of every byte together (an `ir::OriginSet`) and the
`writers`, each a `StagedWriter` with the `writer` site or engine, whether the
bytes were `unwritten`, how many `bytes` it wrote, their `origin` and their
`sources`; `sameExtent` says whether two are one — the `streamed` runs, one
`StreamedRange` per stream: `site`, `registerAddress` with `registerName` and
`registerClass`, `bytes` and `times`, either the `memory` of the buffer
carried or the `romOffset` carried with the `source` run it is lifted as, and
its `landing` where the register is a video data port,
`sameStream` likewise — and what the run beside the
interpreter checked — the `instructions` and `interrupts` it ran a node or a
sequence for, the distinct `nodes` it lifted from the fetches, the
`divergences` on which a node disagreed with the machine, each site once in
the notes, and the `originSets` the shadow interned under its `originCap`.
`extentStart` is the lowest address a `MovedRange` covered. `CartridgeRequest::observeRun`
asks for the run — off unless asked, since it costs about as long as it emulates;
`snes_disasm` asks unless told `--no-run` — `runMasterCycles` bounds it, and
`CartridgeRequest::input` is the script it replays. `CartridgeRequest::reached`,
`CartridgeRequest::ran` and `CartridgeRequest::moved` are what earlier runs
found, read back from the manifest, which the disassembly merges with this
run's.

`accesses` and `dmas` carry [what the code reaches](#what-the-code-reaches).
`rom/rom_facts.h` is the producer, over a finished `CartridgeDisassembly`:
`hardwareAccesses(disassembly, proven)` gives one `HardwareAccess` per register an
instruction reaches — its `site`, `registerAddress`, `name`, `cls`, `kind`, the
`value` where the bytes prove it, and the `run` of straight-line code it sits in —
and `dmaTransfers(accesses)` gives one `DmaTransfer` per channel a run set up.
`proven` is a `ProvenProgram` from `proveProgram(disassembly, rom)`: the
[dataflow](ir.md#what-every-path-proves) run over the disassembly's `program` —
every region lifted once, in address order, an address two paths read two ways
as two nodes — which the dataflow refers to; without one, a value is the instruction before's alone
and a direct-page operand produces nothing. `derivedTargets(disassembly, proven)`
gives one `DerivedTarget` per destination the analysis derived — `target`,
`mode`, `site`, `pointer`, `call`, `name` — which `disassembleCartridge` traces
from and carries as `derived`; `stateFacts(disassembly, proven)` gives one
`StateFact` per label at which any of `d`, `dbr` or `s` is known, each the
values the register can hold, carried as `states`.
`accessKindName` and `dmaDirectionName` are their names as text. `routines`
carries one `Routine` per routine — its `address` and `label`, the `lines` it
holds and their `bytes`, the routines it `calls`, and its role as `reaches` and
`through` — from `routines(disassembly)`, which reads the finished listings, the
accesses and the transfers.

The disassembler's files are text from `renderProgramFile`, `renderManifest` and
`renderSoundProgram`; `writeProject` writes them under a directory — the
program file first, then the manifest, the lifted files as bytes under theirs,
and the sound file — and no bank file. `parseManifest` reads a manifest's
entries, reached and derived targets, landings, moved ranges, assets, file
split, map, sound program and image identity back, and the accesses, routines,
seen registers and transfers the renderer reads, and `manifestMismatch` says
whether that manifest can direct a run over a given image.

The bank files are `rom/rom_render.h`'s, a library that links nothing able to
trace, run or lift: `readRenderInput(directory, program, error)` builds a
`RenderInput` from `program.snagir` and `project.manifest` — each region's
listing from the file's nodes, labels and data runs; the register an operand
reaches from the `access` lines; the direct registers a run saw from `seen`;
the routines from `routine`; the lifted files from `asset`, with the register
their bytes went to found in the `moved` and `dma` lines; the sound program's
blocks from `block` — and `renderRegion(region, input, program)` writes each
instruction from its node in that program, the first at the line's address,
through `ir/ir_render.h`, with the labels, the register names and the routine
comments attached; the data runs and the sound-program cut are the listing's.
`renderTree(directory, rendered, error)` does it for every region, and
`snes_render` is that call. `renderInputOf(disassembly)` and
`renderRegion(region, disassembly)` in `rom_disasm.h` render the same from the
disassembly in memory, which is how a tree rendered from disk is held equal to
the disassembly that wrote it.

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
exercises, run by run, and the manifest accumulates it. The same holds for what
a run moved: a transfer the run never started has its `dma` line and no `moved`
line, and a byte the CPU carries to a register itself, a store at a time, is
not a transfer and is not recorded here. And for where a range landed: what
the memory was used as is read once, at the first frame drawn after the
bytes landed, so a stretch of VRAM a game uses as tiles in one scene and as a
map in another is named by the first; a run that ends before a frame is drawn
says `unshown`; and a file whose landings mix the two, or lie under Mode 7,
stays under `vram/` — the picture's use of every word over a whole run, and
the editable forms that follow from it, are not here yet. And for where the CPU arrived: a return
the run never took has no `ran` line, and code the program copied into work RAM
and ran there is checked by the run and lifted from its fetches, but the tree
has no file to place it in, so a landing there is a `note` and its bytes stay
in the bank as the data they were copied from.

The assets are the ranges the run saw leave the image, and the sources of the
ranges it saw leave work RAM. A range the program built in work RAM before
sending it — which is most of what a real cartridge sends, whether an engine
carries it out or the CPU does — is lifted as the image bytes its writer read
to build it, where the shadow found them; a range built from constants, or
from a register's values, has no source and no file, and the `origin` line
says which. A source is the run holding the origin among what the writer's
invocation read, followed out through the callers whose run grew from it: a
decoder that reads its stream through a helper is followed into the helper,
and one called once per chunk is followed out to its caller, so a source can
be wider than the asset a person would cut and the `staged` lines say which
parts were used; a range
staged twice through work RAM keeps only its origin's own intervals, and a
routine the trace holds no label at is named by its site. A transfer the run
never started has no `moved` line, and is a file only where the code proves
it whole — a `dma` line with its source, its step, its count and a
general-purpose start every path settles; one the code sets up from a table,
or starts from a handler the paths do not reach, stays in its bank.

What the code reaches is reported for the main CPU's regions. The sound program is
another chip's, with registers of its own, and has no `access`, `routine` or
`state` lines. A value carries as far as every path proves it and no further, so
a channel configured from a table, or across a call that does not give the
register back, leaves the fields it did not settle `none` rather than guessing at
them. A routine's role counts what the bytes reach and what a run reached; a call
through a pointer the run did not take contributes nothing to `through`, since
nothing names its target. A table is derived only where its index is bounded by
a mask or by a compare and a branch on the carry and the table lies in the
image; an index that comes from memory, or a table in work RAM, keeps its stop
for the run or a person to answer.

Every tree in a corpus of thirty-one cartridges — LoROM, HiROM and ExHiROM,
from 512 KB to 6 MB — assembles back to its image byte for byte under
`snes_verify`. The bank files are written from the intermediate representation,
which holds no bytes: the bytes in every comment are re-encoded from the
instruction layer, and the tree still assembles to the image. The trace's own
limits stand: a jump through a table stops it, and an entry in the manifest is
how a person carries it past.

## See also

- [Project manifest](project-manifest.md) — the manifest's grammar, what is read
  back, and its stability.
- [Input script](input-script.md) — the recorded run `--input` replays, its
  grammar and its refusals.
- [The assemblers](assemblers.md) — the tools `snes_verify` rebuilds the
  tree's files with, and their diagnostics.
- [The intermediate representation](ir.md#rendering-source) — what the bank
  files are written from, and how the writing is held to the listing.
- [65816 disassembler](65816-disassembler.md) and
  [SPC700 disassembler](spc700-disassembler.md) — the two backends the tree's
  listings come from.
- [Disassembly framework](disassembly-framework.md) — the tracer and renderer,
  and the cartridge entry points.
- [SNES cartridge](snes-cartridge.md) — the header, the three maps, and where
  every bus address lands in the image.
- [SNES machine](snes-machine.md) — the machine the boot runs on, and the
  upload handshake.
