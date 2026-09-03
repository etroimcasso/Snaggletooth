# Project manifest

`project.manifest` is the file at the root of a source tree the
[cartridge disassembler](snes-disassembler.md) writes. It says which image the
tree is of, which files make it up and which bytes of the image each produces,
where the trace began and where it stopped. Two of its line kinds are read back
on the next run, which is how a person directs the trace.

> **Status.** The disassembler writes the manifest and reads it back on the next
> run; `snes_verify` executes it — assembles every file it names, places the
> bytes where it says, and reports the difference from the image. Thirty-one
> real cartridges' trees verify byte-identical through it.

---

## Contents

- [1. Form](#1-form)
- [2. Lines](#2-lines)
  - [2.1 The image](#21-the-image)
  - [2.2 Files](#22-files)
  - [2.3 The sound program](#23-the-sound-program)
  - [2.4 Entries](#24-entries)
  - [2.5 Stops, warnings and notes](#25-stops-warnings-and-notes)
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

A manifest as the disassembler writes one:

```
image    524288
map      LoROM
title    "SUPER MARIOWORLD"
checksum $A0DA $5F25

file     bank_00.asm 65816 $00:8000 $00:FFFF
file     bank_01.asm 65816 $01:8000 $01:FFFF
…
file     bank_0F.asm 65816 $0F:8000 $0F:FFFF
sound    apu/driver.asm SPC700 entry $0500
block    apu/driver.asm $0500 3646 at $070004
block    apu/driver.asm $1360 5661 at $0718B5
block    apu/driver.asm $5570 2667 at $070E46

entry    $00:8000 reset e=1 m=8 x=8
entry    $00:82C3 nmi e=0 m=? x=?
entry    $00:816A nmi_native e=0 m=? x=?
entry    $00:8374 irq_native e=0 m=? x=?
entry    $00:FFFF brk_native e=0 m=? x=?

stop     $00:86F7 `JML [!$0000]`: the target is computed at run time; add an entry for each destination

warning  bank_00.asm $00:98E1 is reached with e=0 m=8 x=8 and with e=0 m=16 x=8
```

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

## 3. What is read back

Two tools read a manifest, and each takes the lines that direct it.

When the disassembler runs over a directory that holds a manifest, it reads:

- every `entry` line, and traces from each with the vectors — an entry the
  vectors already name, under the same mode, is one entry;
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

Everything else is what the last run found and is written fresh. A `stop` line
records; only an `entry` line directs. The disassembler writes the files fresh
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
