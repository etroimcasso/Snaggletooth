# Project manifest

`project.manifest` is the file at the root of a source tree the
[cartridge disassembler](snes-disassembler.md) writes. It says which image the
tree is of, which files make it up and which bytes of the image each produces,
where the trace began and where it stopped. Three of its line kinds are read back
on the next run — which is how a person directs the trace, and how a run's
findings outlive it.

> **Status.** The disassembler writes the manifest and reads it back on the next
> run; `snes_verify` executes it — assembles every file it names, places the
> bytes where it says, and reports the difference from the image. Thirty-one
> real cartridges' trees verify byte-identical through it. It also carries what
> the traced code reaches: a line per hardware register access, and a line per
> DMA transfer a channel was set up for. Run on the machine, it records the
> destinations the indirect jumps took, and traces from them.

---

## Contents

- [1. Form](#1-form)
- [2. Lines](#2-lines)
  - [2.1 The image](#21-the-image)
  - [2.2 Files](#22-files)
  - [2.3 The sound program](#23-the-sound-program)
  - [2.4 Entries](#24-entries)
  - [2.5 Stops, warnings and notes](#25-stops-warnings-and-notes)
  - [2.6 What the code reaches](#26-what-the-code-reaches)
  - [2.7 What a run reached](#27-what-a-run-reached)
- [3. What is read back](#3-what-is-read-back)
- [4. Stability](#4-stability)
- [See also](#see-also)

## 1. Form

A manifest is UTF-8 text, one line per fact:

```
<kind> <field> <field> …    ; comment
```

The first word names the kind of line; the fields follow, separated by spaces or
tabs. A field with spaces in it is written in double quotes. A semicolon begins a
comment that runs to the end of the line, and a blank line is nothing. The
disassembler pads kinds to one column, which is layout and carries no meaning.

Addresses are written in the 65816 dialect's long form, `$BB:XXXX`, and the sound
program's in the SPC700 dialect's, `$XXXX`. An image offset is `$` and six
hexadecimal digits. A count is decimal.

A manifest as the disassembler writes one, for a two-bank cartridge whose reset
code blanks the screen, sets up two transfers and calls a routine in the second
bank:

```
image    65536
map      LoROM
title    "FACTS DEMO CARTRIDGE"
checksum $EDCB $1234

file     bank_00.asm 65816 $00:8000 $00:FFFF
file     bank_01.asm 65816 $01:8000 $01:FFFF

entry    $00:8000 reset e=1 m=8 x=8
entry    $00:8300 nmi_native e=0 m=? x=?

warning  bank_00.asm $00:8300 cannot be read: LDA # under an accumulator width the trace does not know

access   $00:8006 INIDISP Display write $8F
access   $00:8009 RDNMI Interrupt read none
access   $00:8017 BBAD0 DmaChannel write $04
access   $00:8017 A1T0L DmaChannel write $00
access   $00:8024 MDMAEN DmaControl write $01
access   $01:8002 BBAD2 DmaChannel write $22

dma      $00:8017 channel 0 to-register $00:2104 OAMDATA Oam source $7F:0000 start $01
dma      $01:8002 channel 2 direction-unknown $00:2122 CGDATA Cgram source none start none
```

A cartridge that uploads a sound program at boot also carries the `sound` and
`block` lines of [2.3](#23-the-sound-program), and one whose trace meets a target
the bytes do not name carries the `stop` lines of
[2.5](#25-stops-warnings-and-notes).

## 2. Lines

### 2.1 The image

```
image    <bytes>
map      LoROM | HiROM | ExHiROM
title    "<text>"
checksum $XXXX $XXXX
```

`image` is the image's size in bytes, and `checksum` its header checksum and
complement, as the header carries them. Together they name the image the manifest
was written for. `map` is the map the header names and `title` the header's title,
with anything outside printable ASCII removed.

### 2.2 Files

```
file     <path> 65816 <first> <last>
```

One line per source file, in image order. The file holds the bytes from `first` to
`last`, both inclusive, under one `ORG` — save any range the sound program takes,
which the file marks with a comment and an `ORG` past it. Both addresses lie in
one bank, and the range reads consecutive image bytes under the map, so `first`'s
offset places the whole file. The path is relative to the manifest and has no
spaces.

### 2.3 The sound program

```
sound    <path> SPC700 entry <address>
block    <path> <address> <length> at <offset>
block    <path> <address> <length> unplaced
```

`sound` names the file the sound program is written to and the address the audio
CPU starts it at. Each `block` is a run of bytes the cartridge sent, at the audio
address it was sent to, `length` bytes long. A block the image holds at exactly one
place is `at` that image offset, and its bank's file leaves those bytes to the
sound program's file; a block the image does not hold as it is, or holds at more
than one place, is `unplaced`, and its bank keeps the bytes. A manifest has these
lines only when a sound program was captured.

### 2.4 Entries

```
entry    <address> <name> e=<0|1> m=<8|16|?> x=<8|16|?>
```

An address the trace started from, the label it carries, and the mode execution
arrives in: `e=1` is emulation mode, which fixes both widths at eight; `e=0` is
native mode, with the accumulator width `m` and the index width `x` each eight,
sixteen, or `?` for a width the trace does not know and must not guess. The
vectors are written first — the reset handler `e=1`, every interrupt handler
`e=0 m=? x=?` — then every entry a person added.

### 2.5 Stops, warnings and notes

```
stop     <address> <reason>
warning  <path> <text>
note     <text>
```

A `stop` is an address whose successors the bytes do not name — a jump or a call
through a table or a pointer, or one into memory that is not the image — with the
instruction and the reason; a person answers it with an `entry`. A `warning` is a
listing's own warning, prefixed with the file it heads: an address two paths read
two ways, an operand no width settles. A `note` is what the run could not do at
all: no sound program within the boot's time, an entry that lies in no file, a
`file` line that does not read consecutive bytes.

### 2.6 What the code reaches

```
access   <address> <register> <class> read | write | read-write   $XX | none
dma      <address> channel <n> <direction> <register address> <register> <class>
                   source <address> start | start-hdma <mask>
```

An `access` is one instruction reaching one hardware register: where the
instruction is, the register's name and [class](65816-disassembler.md#hardware-registers),
whether it reads the register, writes it or both, and the byte it wrote where the
bytes say what that was. An instruction whose register is sixteen bits wide
reaches two registers and has a line for each.

A `dma` is a transfer a channel was set up for: the channel, which way it moves
bytes (`to-register`, `from-register`, or `direction-unknown`), the B-bus register
it reaches — the address the channel's `BBAD` names, with that register's own name
and class — the A-bus address it moves from, and the mask that started it, from
`MDMAEN` as `start` or from `HDMAEN` as `start-hdma`.

Every field is present on every line. `none` is a field the bytes did not say,
which is a fact about the cartridge rather than a gap in the format: a value is
written only where the instruction immediately before loaded it as an immediate,
or where the instruction is `STZ` and carries its own zero, and a channel
configured from a table says `none` rather than a guess.

A routine that blanks the screen and sends a sprite table, and the lines it
produces:

```
access   $00:8006 INIDISP Display write $8F
access   $00:8009 RDNMI Interrupt read none
access   $00:800C DMAP0 DmaChannel write $00
access   $00:8011 OAMADDL Oam write $00
access   $00:8011 OAMADDH Oam write $00
access   $00:8017 BBAD0 DmaChannel write $04
access   $00:8017 A1T0L DmaChannel write $00
access   $00:801D A1T0H DmaChannel write $00
access   $00:801D A1B0 DmaChannel write $7F
access   $00:8024 MDMAEN DmaControl write $01

dma      $00:8017 channel 0 to-register $00:2104 OAMDATA Oam source $7F:0000 start $01
```

`$00:8011`, `$00:8017` and `$00:801D` are each one instruction under a
sixteen-bit accumulator, so each has two lines: `STZ !$2102` clears both halves
of the OAM address, and `STA !$4301` writes `$04` to the channel's B-bus address
and `$00` to the low byte of its source. `$04` in `BBAD0` is what makes the
destination `OAMDATA`, and the three source bytes together make `$7F:0000`.

### 2.7 What a run reached

```
reached  <address> <name> e=<0|1> m=<8|16|?> x=<8|16|?> from <address>
```

A destination the cartridge, run on the machine, was seen to take through a
jump or a call whose target the bytes do not name — `JMP (!abs)`,
`JMP (!abs,X)`, `JML [!abs]` and `JSR (!abs,X)` — with the mode it arrived in
and the instruction that took it. It is an entry the trace starts from, exactly
as an `entry` line is, and the vectors and entries come first: a person's
`entry` naming the same address under the same mode gives it its name, which the
`reached` line then carries. The name is otherwise `sub_` for a call's target and
`loc_` for a jump's, with the address.

A cartridge that dispatches through a table, run for one second of its clock:

```
entry    $00:8000 reset e=1 m=8 x=8

reached  $00:8240 loc_008240 e=0 m=8 x=8 from $00:800B
reached  $00:8220 loc_008220 e=0 m=8 x=8 from $00:8245

stop     $00:800B `JMP (!$8100,X)`: the target is computed at run time; add an entry for each destination
stop     $00:8245 `JMP (!$8100)`: the target is computed at run time; add an entry for each destination
```

The `stop` lines stay: they are what the bytes say, and the `reached` lines beside
them are what the run saw. A run sees only what it exercised, so a jump the run
never took has a `stop` and no `reached`, and a person's `entry` is still the way
to name a destination the run did not.

## 3. What is read back

Two tools read a manifest, and each takes the lines that direct it.

When the disassembler runs over a directory that holds a manifest, it reads:

- every `entry` line, and traces from each with the vectors — an entry the
  vectors already name, under the same mode, is one entry;
- every `reached` line, and traces from each after the entries — so a run's
  findings are kept from one disassembly to the next whether or not the next
  one runs, and a new run's are merged with them;
- every `file` line, and writes exactly those files — a file split with a gap
  leaves the gap's bytes unplaced, which the run reports;
- `image` and `checksum`, and refuses to run when the image it was given is not
  the one the manifest was written for.

When [`snes_verify`](snes-disassembler.md#verifying-the-tree) runs over the
directory, it reads:

- `map`, and every `file` line — each file is assembled and its bytes placed at
  the image offset their address reads from under the map;
- the `sound` line and every `block` line — the sound file is assembled, and
  each block `at` an offset is placed there; an `unplaced` block is the bank's
  and is not compared;
- `image` and `checksum`, refusing a manifest written for another image as the
  disassembler does.

Everything else is what the last run found and is written fresh — the `access`
and `dma` lines among them, which no tool reads back: they are what the trace saw,
and the next trace sees it again. A `stop` line records; only an `entry` line
directs. The disassembler writes the files fresh
too: an edit to a bank file is not read back by it, so a person's changes to the
trace belong in the manifest, and their changes to the code in the sources,
which `snes_verify` assembles as they are.

A line that does not parse stops either tool with the line number and what was
expected, before anything is written. An `entry` whose name is a mnemonic —
`cop`, `brk` — is labelled with `_handler` after it, since the name cannot be a
label, and the run says so in a `note`; the vectors of those names are labelled
the same way.

## 4. Stability

This document defines a published surface, held to the same rule as the
[assembly language](assembly-lexicon.md#8-stability): once a release reads a
manifest, a later release reads it to the same effect. The fields of the kinds
above do not change. New kinds may be added, and are added to this page before a
release writes them; a reader treats a kind it does not know as an error, never
as a line to pass over.

## See also

- [Cartridge disassembler](snes-disassembler.md) — the tool that writes the
  manifest and the tree it describes, and `snes_verify`, which executes it.
- [Assembly language: the common layer](assembly-lexicon.md) — `ORG`, `DB` and
  the rest of what the files are written in.
- [SNES cartridge](snes-cartridge.md) — the maps, and how an address places an
  image byte.
