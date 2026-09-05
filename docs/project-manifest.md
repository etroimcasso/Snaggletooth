# Project manifest

`project.manifest` is the file at the root of a source tree the
[cartridge disassembler](snes-disassembler.md) writes. It says which image the
tree is of, which files make it up and which bytes of the image each produces,
where the trace began and where it stopped. Seven of its line kinds are read
back on the next run — which is how a person directs the trace, how a run's
findings outlive it, and how a name a person gives a file survives.

> **Status.** The disassembler writes the manifest and reads it back on the next
> run; `snes_verify` executes it — assembles every file it names, places the
> bytes where it says, and reports the difference from the image. Thirty-one
> real cartridges' trees verify byte-identical through it. It also carries what
> the traced code reaches: a line per hardware register access, a line per DMA
> transfer a channel was set up for, and a line per routine with what it calls
> and what it drives. Run on the machine, it records the destinations the
> indirect jumps took, and traces from them, every place the CPU arrived that no
> instruction named — a return to an address the code put on the stack — and
> traces from those too, every range of bytes the transfer engines moved —
> where from, where to, how many, and from which instruction — and lifts every
> such range that begins in the image into a file of its own, recorded as an
> `asset` line, and the direct register and the data bank the run saw at every
> site it executed. Read for what every path proves, it records the direct
> register, the data bank and the stack pointer at every label where something
> is proven, and the destinations of every jump through a table the bytes
> bound, and traces from those too.

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
  - [2.8 Routines](#28-routines)
  - [2.9 What every path proves](#29-what-every-path-proves)
  - [2.10 What the bytes derive](#210-what-the-bytes-derive)
  - [2.11 What a run moved](#211-what-a-run-moved)
  - [2.12 Assets](#212-assets)
  - [2.13 Where a run landed](#213-where-a-run-landed)
  - [2.14 What a run saw](#214-what-a-run-saw)
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

routine  $00:8000 reset lines 26 bytes 66 calls sub_018000 reaches Display,Vram,Oam,Interrupt,DmaControl,DmaChannel through Cgram,DmaChannel
routine  $01:8000 sub_018000 lines 3 bytes 6 calls none reaches Cgram,DmaChannel through none
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
vectors are written first, each in the mode the CPU takes it in — the
emulation-mode set, reset among them, `e=1 m=8 x=8`; the native set
`e=0 m=? x=?`, since the image cannot say what widths the interrupted code had —
then every entry a person added.

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

### 2.8 Routines

```
routine  <address> <label> lines <n> bytes <n> calls <label,…> | none
         reaches <class,…> | none  through <class,…> | none
```

A routine is the code execution reaches from a label by falling through,
branching and jumping, without passing a return or a halt and without entering
the routine a call names: the call is an edge, and execution resumes after it. A
jump or a call whose target the bytes do not name ends it, as it ends the trace.
Every entry, every target a run reached, and every label a call names is a
routine; a label only branches and jumps reach is inside the routines that reach
it, so a routine that falls through into the next label's code, or jumps into
it, holds those lines too, and a line two routines reach is in both.

`lines` and `bytes` count what the routine holds. `calls` names the routines its
call instructions name, each once, in address order. `reaches` is its role: the
[classes](65816-disassembler.md#hardware-registers) of the registers its own
lines reach and of the transfers its own lines set up, in the table's order.
`through` is what its calls reach, followed through every routine they call in
turn — so a routine that only calls others has `reaches none` and a `through`
that says what the calls were for. Each list is one field, its names joined by
commas, and an empty list is `none`.

The cartridge of [1](#1-form), whose reset code drives the screen and two
transfers itself and calls into the second bank for a third:

```
routine  $00:8000 reset lines 26 bytes 66 calls sub_018000 reaches Display,Vram,Oam,Interrupt,DmaControl,DmaChannel through Cgram,DmaChannel
routine  $01:8000 sub_018000 lines 3 bytes 6 calls none reaches Cgram,DmaChannel through none
```

`Oam` and `Vram` are in reset's own role through its transfers — the channel
registers it writes are `DmaChannel`, and where it points them is what the
transfers reach — and `Cgram` is in its role through the call alone.

### 2.9 What every path proves

```
state    <address> D=<value> DBR=<value> S=<value>
```

A `state` is what the code proves about three registers at a label, before the
instruction there runs: the direct register, the data bank register and the
stack pointer. Each field is the value — `$XXXX`, or `$XX` for the bank — where
every path into the label proves the same one; `?` where some path proves
nothing; and two or more values joined by `|` where different paths prove
different ones, which is a disagreement the code carries and the line reports
rather than settles. A label at which nothing is proven has no line.

A value is proven by an immediate, or by a transfer of a register whose value is
proven, and by nothing else. `LDA #$0000` then `TCD` proves the direct register;
`LDX #$1FFF` then `TXS` proves the stack pointer; `PHK` then `PLB` proves the data
bank to be the program bank, and `PEA $7E7E` then `PLB` twice proves it `$7E`. A
value pulled from the stack is proven only when the push that put it there is
on the same straight path, with no call and no store the stack could be under
between them. A value loaded from memory is proven only when the memory is the
image. The reset vector begins with the direct register and the data bank both
zero, which the chip clears on reset, and nothing else proven; an interrupt
vector and an entry a person added begin with nothing proven; a target a run
[reached](#27-what-a-run-reached) or the bytes [derived](#210-what-the-bytes-derive)
begins with what the site that jumped to it proves.

A call carries the caller's values into the routine it names. What comes back
is what every return of that routine proves — except a register the routine and
everything it calls never write, which comes back as it went in. A call whose
target the bytes do not name, a `BRK` or `COP`, and a routine that reaches a
jump the bytes do not name bring nothing back. A hardware interrupt is not a
path: its handler's own paths begin at its vector, and what the handler does to
the registers between two of the program's instructions is the handler's to
restore, as the program itself assumes.

The program bank is the bank the tree places the instruction in. A cartridge
that runs its code through a mirror of that bank reads the same bytes, and a
data bank proven from the program bank names the same registers.

The value proven at a site is also what the `access` lines of
[2.6](#26-what-the-code-reaches) carry: a register written from a value every
path proves has that value, whether the load was the instruction before or
twenty instructions and a call earlier, and a direct-page operand under a
proven direct register reaches the register it lands on.

A cartridge whose reset code sets the stack pointer, sets the direct register to
the DMA channels, calls a routine that keeps it, sets it back to zero, proves the
data bank twice over, and dispatches through a table two of whose targets set
the direct register apart and meet, and one of whose targets calls the same
routine again:

```
state    $00:8000 D=$0000 DBR=$00 S=?
state    $00:802A D=$0000 DBR=$7E S=$1FFF
state    $00:8100 D=$0000|$0100|$0200|$4300 DBR=$00 S=$1FFD
state    $00:8380 D=$0100|$0200 DBR=$00 S=$1FFF
state    $00:8390 D=$0000 DBR=$00 S=$1FFF
```

At `$00:8000` the reset vector's own facts and nothing else; at `$00:802A` the
data bank `PEA $7E7E`/`PLB`/`PLB` left; inside the routine at `$00:8100` the
direct registers its two callers bring — `$4300` from the reset code, three
values from the second call — and a stack pointer two bytes down; at `$00:8380`
two paths carrying different direct registers, reported as both; at `$00:8390`
five paths agreeing.

### 2.10 What the bytes derive

```
derived  <address> <name> e=<0|1> m=<8|16|?> x=<8|16|?> from <address> via <address>
```

A destination the bytes prove a jump or a call through a pointer takes: the
pointer lies in the image, and the index that selects it — for `JMP (!abs,X)`
and `JSR (!abs,X)` — is bounded on every path into the jump by a comparison
against an immediate followed by a branch on the carry (`CMP #n` and `BCS`, or
`CPX #n` and `BCC`), or by a mask (`AND #mask`), whose bound carries through
the arithmetic between it and the jump (`ASL`, `TAX`). Every value the index can
take selects one pointer, and each is a `derived` line: the target, its label,
the mode the jump carries in, the site, and after `via` the address the pointer
was read from. A pointer outside the image, or an index nothing bounds, derives
nothing. `JMP (!abs)` and `JML [!abs]` read a pointer that is a value in memory
rather than a slot of a table, and derive nothing either; the
[run](#27-what-a-run-reached) answers them.

A `derived` line is an entry the trace starts from, exactly as a `reached` line
is, and is named the same way: a person's `entry` for the same address under the
same mode gives it its name; otherwise `sub_` for a call's target and `loc_` for
a jump's, with the address. The two kinds are kept apart because they say
different things — `reached` that a run saw the destination taken, `derived`
that the bytes prove it can be — and a destination both saw has both lines. A
jump every one of whose destinations is derived is not a stop, and has no `stop`
line; a jump the bytes bound but whose pointers lie outside the image keeps its
`stop`.

The same cartridge's reset code masks a value nothing knows to three bits,
doubles it, and dispatches through an eight-entry table at `$00:8200`:

```
derived  $00:8300 loc_008300 e=0 m=8 x=16 from $00:8033 via $00:8200
derived  $00:8310 loc_008310 e=0 m=8 x=16 from $00:8033 via $00:8202
derived  $00:8320 loc_008320 e=0 m=8 x=16 from $00:8033 via $00:8204
derived  $00:8330 loc_008330 e=0 m=8 x=16 from $00:8033 via $00:8206
derived  $00:8340 loc_008340 e=0 m=8 x=16 from $00:8033 via $00:8208
derived  $00:8350 loc_008350 e=0 m=8 x=16 from $00:8033 via $00:820A
derived  $00:8360 loc_008360 e=0 m=8 x=16 from $00:8033 via $00:820C
derived  $00:8370 loc_008370 e=0 m=8 x=16 from $00:8033 via $00:820E
```

Eight slots, eight pointers, eight destinations, each traced from; the jump at
`$00:8033` has no `stop` line. The cartridge's third table, indexed by a value
nothing bounds, derives nothing and keeps its `stop`.

### 2.11 What a run moved

```
moved    <address> channel <n> to-register | from-register <register address> <register> <class>
                   memory <address> increment | decrement | fixed bytes <n>
                   as dma | table | indirect times <n>
```

One contiguous range of bytes one channel moved under one trigger, as the
transfer engine performed it while the cartridge ran on the machine. The first
address is the instruction that started it: the write to `MDMAEN` for a
general-purpose transfer, the write to `HDMAEN` that enabled the channel for a
table and the blocks it points at. Then the channel; which way the bytes went —
`to-register` from memory to the B bus, `from-register` back into memory; the
B-bus register the channel's `BBAD` named, with its name and
[class](65816-disassembler.md#hardware-registers), or `none none` where no
register has that address; after `memory`, the A-bus address the range began
at, and how the address stepped from one byte to the next — `increment`,
`decrement`, or `fixed`, a fill from one byte, which is not a range of anything;
after `bytes`, how many; after `as`, what the range was to the engine — `dma`, a
general-purpose transfer; `table`, an HDMA channel's table as one frame walked
it, its line counts, a direct table's inline values and an indirect table's
pointers, from the table's start to where the walk stopped; `indirect`, the
block one indirect entry pointed at — and after `times`, how many sightings of
exactly this range the run made.

A pattern that reaches a second register is implied by the count: a pair of
registers takes two bytes a unit, and the line names the first. A range is
recorded wherever its memory address lies — in the image, in work RAM, anywhere
the engine addressed — since the engine moved it and nothing here needs to read
it. Both addresses are the machine's: the site is where the CPU executed, and
the memory address is what the engine drove, so code and data reached through
a mirror bank are named by the mirror, while the tree's files are named by the
bank the same bytes are placed at (see [SNES cartridge](snes-cartridge.md)). A range closes at a new trigger for its channel, at the start of a frame for
the HDMA engine's, at a break in the step, and at the end of the run, so a walk
the run's end cut short is a range of its own with the bytes it had read.

A cartridge that moves bytes every way the engines can, run for one second of
its clock:

```
moved    $00:8025 channel 0 to-register $00:2118 VMDATAL Vram memory $00:9000 increment bytes 32 as dma times 1
moved    $00:8043 channel 0 to-register $00:2118 VMDATAL Vram memory $00:9100 fixed bytes 64 as dma times 1
moved    $00:806B channel 1 from-register $00:2139 VMDATALREAD Vram memory $7E:0300 increment bytes 16 as dma times 1
moved    $00:80B6 channel 2 to-register $00:2122 CGDATA Cgram memory $00:920F decrement bytes 16 as dma times 1
moved    $00:80B6 channel 3 to-register $00:2150 none none memory $00:9400 increment bytes 8 as dma times 1
moved    $00:80F7 channel 4 to-register $00:2100 INIDISP Display memory $00:9500 increment bytes 5 as table times 60
moved    $00:80F7 channel 5 to-register $00:2121 CGADD Cgram memory $00:9510 increment bytes 7 as table times 60
moved    $00:80F7 channel 5 to-register $00:2121 CGADD Cgram memory $00:9520 increment bytes 2 as indirect times 60
moved    $00:80F7 channel 5 to-register $00:2121 CGADD Cgram memory $00:9522 increment bytes 2 as indirect times 60
moved    $00:8128 channel 6 to-register $00:2100 INIDISP Display memory $7E:0400 increment bytes 129 as table times 59
moved    $00:8128 channel 6 to-register $00:2100 INIDISP Display memory $7E:0400 increment bytes 27 as table times 1
moved    $00:8183 channel 0 to-register $00:2118 VMDATAL Vram memory $00:9620 increment bytes 16 as dma times 1
moved    $00:8190 channel 0 to-register $00:2118 VMDATAL Vram memory $00:9600 increment bytes 16 as dma times 1
moved    $00:8190 channel 0 to-register $00:2118 VMDATAL Vram memory $00:9610 increment bytes 16 as dma times 1
moved    $00:8326 channel 7 to-register $00:2104 OAMDATA Oam memory $7E:0200 increment bytes 544 as dma times 60
```

Channel 0 carries a tileset from the image and then fills sixty-four bytes of
VRAM from the one byte at `$00:9100`; channel 1 reads sixteen bytes of VRAM back
into work RAM; channels 2 and 3 were started by one write at `$00:80B6`, and
the register at `$2150` has no name. Channel 5's indirect table at `$00:9510` is
one range and the two blocks its entries point at are two more, each named by
the register the channel reaches; the blocks are adjacent in memory, and stay
two because each frame's walk is its own. Channel 6's table lies in work RAM and
takes 127 lines to walk: fifty-nine frames walked it whole, and the frame the
run ended in had read twenty-seven bytes of it. Channel 0 then carries
forty-eight bytes in three chunks of sixteen: the write at `$00:8190` started
the first two, one after the other from where the last ended, and they are two
ranges because a new start is a new range; the third was started by a store to
`MDMAEN` through bank `$80`, which is the same register. Channel 7 sends the
sprite table from the vertical-blank handler, once a frame, and the line says so
once.

The `dma` line says what the code set up and only where the bytes say the
values; the `moved` line says what the run saw move. The two stand beside each
other: a channel filled from a pointer has a `dma` line whose source is `none`
and a `moved` line for every range the run saw it carry.

### 2.12 Assets

```
asset    <path> <class> as dma | table | indirect from <address> bytes <n>
```

A file lifted out of a bank file: a range the run saw an engine carry from the
image to the hardware, written once, as the bytes are, under a directory named
for the memory it went to, and included from the bank file where it was with
[`INCBIN`](assembly-lexicon.md#54-incbin). The path is relative to the
manifest; then the [class](65816-disassembler.md#hardware-registers) of the
register the bytes reached and, after `as`, what the range was to the engine,
as the `moved` line says them; after `from`, the address the tree places the
file's first byte at, and after `bytes`, how many it holds. The `moved` lines
whose memory lies within a file are its uses, and say which instruction sent it
where and how many times. One line per file, in address order. Which ranges are
lifted, and which stay in their bank with a `note` saying why, is
[snes-disassembler.md §The assets](snes-disassembler.md#the-assets).

A cartridge that sends bytes from the image every way the rules have a case
for, run for one second of its clock:

```
asset    vram/00_9000.bin Vram as dma from $00:9000 bytes 80
asset    cgram/00_9200.bin Cgram as dma from $00:9200 bytes 16
asset    oam/00_9300.bin Oam as dma from $00:9300 bytes 544
asset    apu/00_9600.bin Apu as dma from $00:9600 bytes 8
asset    hdma/00_9700.bin Cgram as table from $00:9700 bytes 7
asset    hdma/00_9710.bin Cgram as indirect from $00:9710 bytes 2
asset    hdma/00_9712.bin Cgram as indirect from $00:9712 bytes 2
asset    vram/01_8000.bin Vram as dma from $01:8000 bytes 32
asset    vram/01_FFF0.bin Vram as dma from $01:FFF0 bytes 16
```

Three transfers from `$9000` that share bytes are the one file of eighty; the
two blocks the HDMA table at `$9700` points at lie end to end and are two files.

The line is read back for its path: a person renames the file, changes the path
here to match, and the next run lifts the same range — the same `from` and
`bytes` — under that name. The class, the kind and the range are the run's and
are written fresh.

### 2.13 Where a run landed

```
ran      <address> <name> e=<0|1> m=<8|16|?> x=<8|16|?> from <address>
```

A place the CPU, run on the machine, arrived at that the instruction before it
did not name — a return to an address the code itself put on the stack (`PEA`
then `RTS`; a frame built by hand, then `RTL`), an `RTI` into flow the bytes do
not carry, any successor the instruction does not name — with the mode it
arrived in and the instruction that took it there. The run knows because it
runs every instruction the CPU executes through the
[interpreter](ir.md#running-beside-the-machine), lifted from the bytes the CPU
fetched, and reads where the CPU went against what the instruction names. A
fall-through, a taken branch, a jump or a call to a constant target, a call's
return to the address after it, an interrupt handler's `RTI` back to the
instruction it interrupted, and a `BRK` or `COP` to its vector are all named,
and produce no line. The four forms whose pointer the run reads ahead are
[`reached`](#27-what-a-run-reached) lines, never `ran`: `reached` says a pointer
was read and where it pointed; `ran` says only that the CPU arrived.

It is an entry the trace starts from, exactly as a `reached` line is, after
the `reached` lines; a person's `entry` naming the same address under the same
mode gives it its name, and the name is otherwise `loc_` with the address,
since the CPU arrived and nothing says the place is a routine. Both addresses
are as the tree places them: a landing through a mirror bank names the home
bank. A landing outside the image — a return into a routine the program copied
to work RAM — is a `note` and no line, since the tree has nothing to trace
there.

A cartridge that copies a routine into work RAM, builds two return frames by
hand and enters it with `RTL`, lands past a `PEA`/`RTS` pair on both passes of
a loop, and returns through `RTI` into a frame it built with both widths
sixteen bits, run for one second of its clock:

```
ran      $00:803A loc_00803A e=1 m=8 x=8 from $00:8039
ran      $00:804D loc_00804D e=0 m=16 x=16 from $00:804C
ran      $00:802C loc_00802C e=1 m=8 x=8 from $7E:2001

note     run: the CPU arrived at $7E:2000 from $00:802B, which the tree does not hold; not recorded
```

The first line is the `RTS` at `$00:8039` returning to the address the `PEA`
before it pushed, taken on both passes and written once. The second is the
`RTI`, which ran with both widths eight and arrived with both sixteen: the
mode is the CPU's on arrival. The third is the routine's own `RTL`, in work
RAM, returning to the frame the program built for it; the `RTL` that entered
the routine landed in work RAM, and is the note. Without the run, everything
after `$00:802B` is data to the trace; with it, all three landings are labels
with code under them.

### 2.14 What a run saw

```
seen     <address> D=<value|…> DBR=<value|…>
```

At a site in the image the run executed an instruction: every value the direct
register and the data bank held before it ran, each value once, ascending,
joined by `|`. One line per site, in address order, and every value the run
saw is written, however many. The line is written fresh on every run and read
back by nothing — the next run sees it again — and a disassembly without a run
writes none.

`seen` and [`state`](#29-what-every-path-proves) say different things about
the same registers: `state` is what every path into a label proves, `seen` is
what one run saw at a site. A site whose `state` line says `?` and whose `seen`
line holds one value is the analysis stopping short of what the run settled; a
`seen` value the `state` line's set does not hold is the analysis being wrong,
and the two lines say so beside each other rather than one hiding the other.
A plain direct-page operand at a site the paths prove nothing about and the
run saw one direct register at carries, in its comment in the bank file, the
register that value lands it on, marked `(run)` —
[snes-disassembler.md §The tree](snes-disassembler.md#the-tree).

The same cartridge, whose loop head at `$00:802C` runs under two direct
registers and whose store after the loop runs under one:

```
seen     $00:802C D=$4300|$4310 DBR=$00
seen     $00:803F D=$4320 DBR=$00
```

and in `bank_00.asm`, the store through the direct page after the loop, and
the indexed one beside it, which the run does not name since it does not carry
`X`:

```
        STA $01                         ; $00:803F  85 01        3  BBAD2 (run)
        STA $03,X                       ; $00:8041  95 03        4
```

## 3. What is read back

Two tools read a manifest, and each takes the lines that direct it.

When the disassembler runs over a directory that holds a manifest, it reads:

- every `entry` line, and traces from each with the vectors — an entry the
  vectors already name, under the same mode, is one entry;
- every `reached` line, and traces from each after the entries — so a run's
  findings are kept from one disassembly to the next whether or not the next
  one runs, and a new run's are merged with them;
- every `ran` line, and traces from each after the `reached` lines, kept and
  merged the same way;
- every `derived` line, and traces from each as it traces from a `reached`
  line — and derives them again from the bytes, so the set is kept and grows
  with the tree;
- every `moved` line, and keeps it — a range this run saw again carries this
  run's count, one it did not see is kept as it was — so what a run saw move
  outlives it, and a tree disassembled without a run still says where the
  hardware's bytes came from, and lifts the same files;
- every `asset` line, for its path — a file lifted again with the same first
  address and length takes the path the line gives it, so a name a person gave
  a file survives; a line matching no file this run lifts is dropped, and a
  `note` says so;
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

Everything else is what the last run found and is written fresh — the `access`,
`dma`, `routine`, `state` and `seen` lines among them, which no tool reads back:
they are what the trace and the run saw, and the next sees it again. A `stop` line records;
only an `entry` line directs. The disassembler writes the files fresh
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
