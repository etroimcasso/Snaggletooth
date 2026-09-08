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
> `asset` line, where every range landed on the other side of the port and what
> the PPU used that memory as, and the direct register and the data bank the
> run saw at every site it executed. Read for what every path proves, it records the direct
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
  - [2.13 Where the CPU arrived](#213-where-the-cpu-arrived)
  - [2.14 What a run saw](#214-what-a-run-saw)
  - [2.15 Where a staged range came from](#215-where-a-staged-range-came-from)
  - [2.16 What the CPU streamed](#216-what-the-cpu-streamed)
  - [2.17 Where a transfer landed](#217-where-a-transfer-landed)
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

dma      $00:8017 channel 0 to-register $00:2104 OAMDATA Oam source $7F:0000 increment bytes none start $01 from $00:8024
dma      $01:8002 channel 2 direction-unknown $00:2122 CGDATA Cgram source none none bytes none start none from none

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
then every entry a person added. A vector's entry is written where the tree
places its handler; a person's is written as they gave it, in the bank they say
the CPU enters in, which is where [what every path proves](#29-what-every-path-proves)
begins, and the trace places it to start from it.

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
                   source <address> increment | decrement | fixed bytes <n>
                   start | start-hdma <mask> from <address>
```

An `access` is one instruction reaching one hardware register: where the
instruction is, the register's name and [class](65816-disassembler.md#hardware-registers),
whether it reads the register, writes it or both, and the byte it wrote where the
bytes say what that was. An instruction whose register is sixteen bits wide
reaches two registers and has a line for each.

A `dma` is a transfer a channel was set up for: the channel, which way it moves
bytes (`to-register`, `from-register`, or `direction-unknown`), the B-bus register
it reaches — the address the channel's `BBAD` names, with that register's own name
and class — the A-bus address it moves from and how that address steps from one
byte to the next as the channel's `DMAP` says, `increment`, `decrement`, or
`fixed`, a fill from one byte; after `bytes`, the count the channel's `DAS` was
written with, which for a general-purpose transfer is how many bytes it moves,
zero standing for 65536; and the write that started it — the mask, from
`MDMAEN` as `start` or from `HDMAEN` as `start-hdma`, and after `from` the
instruction that wrote it. The first address is where the channel's `BBAD` was
written, or its `DMAP` where `BBAD` was not, or the first of its registers
written for this transfer. An HDMA channel takes its counts from its table and
the engine writes `DAS` itself, so a `start-hdma` line's count is what the code
happened to write there, not a length.

One line per start. The registers a channel holds are what its last writes left,
so a channel set up and started three times in one stretch of straight-line
code is three lines, each with the registers as they stood when its start was
written — a second start that rewrote only the source and the count is a
transfer to the same destination, and one that rewrote nothing is the same
transfer again, under the same first address; a channel whose direction or
destination was written after its last start, or never started before the
code branches, is a line whose start is `none`. A channel started with neither
its direction nor its destination known in that stretch has no line, since the
line could say nothing but the start; one that had only its source or count
rewritten and was not started is not a transfer of that stretch either.

Every field is present on every line. `none` is a field the bytes did not say,
which is a fact about the cartridge rather than a gap in the format: a value is
written only where every path into the instruction proves it — the instruction
immediately before loaded it as an immediate, the instruction is `STZ` and
carries its own zero, or a load however far back settled it and nothing since
touched the register (see [2.9](#29-what-every-path-proves)) — and a register
written with a value the bytes do not say is unknown from then on, so a channel
configured from a table says `none` rather than the value an earlier set-up
left.

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

dma      $00:8017 channel 0 to-register $00:2104 OAMDATA Oam source $7F:0000 increment bytes none start $01 from $00:8024
```

`$00:8011`, `$00:8017` and `$00:801D` are each one instruction under a
sixteen-bit accumulator, so each has two lines: `STZ !$2102` clears both halves
of the OAM address, and `STA !$4301` writes `$04` to the channel's B-bus address
and `$00` to the low byte of its source. `$04` in `BBAD0` is what makes the
destination `OAMDATA`, the three source bytes together make `$7F:0000`, `$00` in
`DMAP0` steps the address up, nothing wrote the count, and the write at `$00:8024`
started it.

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
`loc_` for a jump's, with the address. Both addresses are written as the CPU
ran them, in the bank it ran in: a cartridge that dispatches through a mirror
bank has its `reached` lines in that bank, the trace places the destination to
start from it, so the label is the placed address's, and
[what every path proves](#29-what-every-path-proves) at the destination begins in
the bank the CPU arrived in.

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
vector and an entry a person added begin with nothing proven beyond the program
bank, below; a target a run [reached](#27-what-a-run-reached) or the bytes
[derived](#210-what-the-bytes-derive), and a place the CPU
[arrived](#213-where-the-cpu-arrived), begins with what the site that took the CPU
there proves.

A call carries the caller's values into the routine it names. What comes back
is what every return of that routine proves — except a register the routine and
everything it calls never write, which comes back as it went in. A call whose
target the bytes do not name, a `BRK` or `COP`, and a routine that reaches a
jump the bytes do not name bring nothing back but the program bank, since
whatever returns returns to the bank it was called from. A hardware interrupt is not a
path: its handler's own paths begin at its vector, and what the handler does to
the registers between two of the program's instructions is the handler's to
restore, as the program itself assumes.

The program bank is the bank the CPU runs the instruction in, which is the bank
the tree places it in only when the program does not run through a mirror: a
cartridge whose code runs in bank `$80` pushes `$80` with `PHK`, and the data
bank it proves from that is `$80`. Where a mirror and its home read different
memory below `$8000` — under HiROM `$00:2100` is a register and `$C0:2100` the
image — a proven data bank names what the CPU's bank names, and a bank that
mirrors the image reads the image where the map says and the system area where
it says. The program bank begins as the reset vector's, zero; an interrupt
vector's handler begins in zero too, since the chip clears the bank to take the
interrupt; an entry a person added begins in the bank of the address they wrote;
a long jump or call takes its operand's bank; a destination a run
[reached](#27-what-a-run-reached), a place the CPU [arrived](#213-where-the-cpu-arrived)
and a target the bytes [derived](#210-what-the-bytes-derive) begin in the bank
the CPU arrived in, which their lines carry; a fall-through, a branch and a
jump within the bank keep it; and a call brings the caller's bank back, since a
routine returns to the bank it was called from. Two paths that run one
instruction in two banks are two values, reported as any other disagreement is.

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
`stop`. The target and the pointer are written in the bank the CPU runs the jump
in — the program bank [every path proves](#29-what-every-path-proves) there — and
the trace places the target to start from it, so the label is the placed
address's.

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
and a `moved` line for every range the run saw it carry, and a transfer the
code proves whole that the run started has a `moved` line from the same start
that agrees with it — the same source, step and count — or, where the run saw
something else, a `note` saying what each said. The two are never merged.
Where the bytes went on the other side of the port — the words of VRAM, the
palette entries, the bytes of OAM — is the [`landed` line](#217-where-a-transfer-landed)
beside it.

### 2.12 Assets

```
asset    <path> <class> as dma | table | indirect | staged | stream | proven from <address> bytes <n>
```

A file lifted out of a bank file: a range the run saw an engine carry from the
image to the hardware, the image source of a range a routine built in work RAM
before it was carried out, the run of image bytes a routine read while
carrying a stream to a data register itself, or a range the code proves a
channel was set up to carry from the image and no run has moved — written once, as the bytes are,
under a directory named for the memory it went to — and, for VRAM, for what the
PPU used that memory as when the run saw the bytes drawn: `maps/` for a file
every landing of which lies in a tilemap, `tiles/` for one every landing of
which lies in a name base or the sprite tiles, `vram/` for the rest (§2.17) —
and included from the bank
file where it was with [`INCBIN`](assembly-lexicon.md#54-incbin). The path is
relative to the manifest; then the
[class](65816-disassembler.md#hardware-registers) of the register the bytes
reached — two classes joined by `+`, `Vram+Cgram`, for a source a routine built
into data for both, which lives under `staged/` — and, after `as`, what the
file is: `dma`, `table` or `indirect`, what the range was to the engine as the
`moved` line says it; `staged`, the source a routine built its range from
(§2.15); `stream`, the run the CPU carried a stream from (§2.16); `proven`, a
transfer the code proves whole — its destination, source, step, count and a
general-purpose start every path settles (§2.6) — that no run has started, the
one kind of file whose evidence is the code alone. After `from`,
the address the tree places the file's first byte at, and after `bytes`, how
many it holds. The `moved` lines whose memory lies within a file are its uses,
and say which instruction sent it where and how many times; a `staged` line
says what a routine built from it; a `proven` file's use is the `dma` line
whose source and count name its bytes. One line per file, in address order. Which
ranges are lifted, and which stay in their bank with a `note` saying why, is
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
asset    vram/00_9900.bin Vram as staged from $00:9900 bytes 16
asset    vram/01_8000.bin Vram as dma from $01:8000 bytes 32
asset    vram/01_FFF0.bin Vram as dma from $01:FFF0 bytes 16
```

Three transfers from `$9000` that share bytes are the one file of eighty; the
two blocks the HDMA table at `$9700` points at lie end to end and are two
files. The transfer from `$01:FFF0` ran off its bank's end into `$01:0000`,
which is work RAM, holding sixteen bytes the port had copied there from
`$9900`: those are the `staged` file, the source of what the transfer carried
on.

The line is read back for its path: a person renames the file, changes the path
here to match, and the next run lifts the same range — the same `from` and
`bytes` — under that name. For a `dma`, `table` or `indirect` file the class,
the kind and the range are the run's and are written fresh, the `moved` lines
being what keeps the file, and for a `proven` file they are the trace's, which
finds the same transfer every time; a `staged` or `stream` file's evidence is written
fresh (§2.15, §2.16), so its line is read back whole and keeps the file from
one disassembly to the next until a run lifts a wider file over its bytes.

### 2.13 Where the CPU arrived

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
since the CPU arrived and nothing says the place is a routine. The landing is
written where the CPU arrived, in the bank it arrived in — a landing through a
mirror bank names the mirror, and [what every path proves](#29-what-every-path-proves)
there is what the paths prove at the instruction that took the CPU there, in
that bank — and the site as the tree places it; the trace
places the landing to start from it, so its label is the placed address's. A
landing outside the image — a return into a routine the program copied to work
RAM — is a `note` and no line, since the tree has nothing to trace there.

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

### 2.15 Where a staged range came from

```
origin   <address> bytes <n> from <address> bytes <n> using <n> by <label> exact | approximate
origin   <address> bytes <n> from register <address> <register> by <label>
origin   <address> bytes <n> from save by <label>
origin   <address> bytes <n> computed by <label>
origin   <address> bytes <n> unwritten
staged   <path> at <address> bytes <n> to <class> by <label> exact | approximate
```

Where a range carried out of work RAM came from. The run's shadow carries,
beside every value the interpreter computes, the image offsets it was computed
from — a load takes the origin of the bytes it read, an operation the union of
its operands', a constant and a flag none — and rests it in work RAM byte by
byte with the site that last wrote each; the engines' and the port's moves keep
it current. At every `moved` range whose memory is work RAM, and at every
`streamed` run the CPU carried out of work RAM (§2.16), the shadow is read, and
the extent — the first address and the count, every range and stream that
covers exactly those bytes together — gets one `origin` line per writer and
per source.

The first address and count name the extent. The writer after `by` is the
routine whose instruction last wrote the bytes, `none` for an engine's write,
and the site itself where the trace holds no routine there. What follows is
what the shadow found of the bytes that writer wrote. After `from`, an image
address and a count: the source, a run of image bytes the writer's invocation
read that holds the bytes' origin — a decoder reads its whole stream, counts
and lengths included, and only the literal values reach the output, so the
source is the stream and `using` says how many of its bytes the values came
from; a copy uses every byte of its source.

A run is a maximal stretch of image bytes one invocation read: every image
read the invocation makes either lands inside a run, which changes nothing,
or starts a run of that one byte; and two runs that come to touch — one's
last byte followed by the other's first — are one run. No two runs of an
invocation overlap or touch. So two files read a chunk at a time turn and
turn about are two runs, whatever was read in between; a decoder's stream is
one run however many tables it consults and however often it re-reads the
byte it is at; a copy that walks its source from the end reads one run; and a
table read in whatever order its entries are asked for is one run where the
entries lie end to end. When a helper returns, each of its runs joins the
caller's exactly as if the caller had read those bytes itself.

The source of a byte is the run holding its origin among the writer's
invocation's runs, followed outward: at each caller, the caller's run holding
the origin replaces the run found so far if it begins or ends where that run
does — the caller read on from it, so a decoder called once per chunk has the
whole file for its source — and the search ends where a caller's run holds
it strictly inside, or no caller's run holds it: a caller that had the bytes
already leaves the helper's run as the source. `exact` says the origin is; an
`approximate`
origin was widened to its hull by the run because it had more intervals than
the run keeps, and its source is every run the hull overlaps. A writer with
several sources has one line each. `from register` names a hardware register
whose value entered the bytes — the joypads, VRAM read back, the multiplier —
and `from save` a byte of the save; both stand beside an image source when
there is one. `computed` says the bytes were built from constants alone;
`unwritten` that nothing wrote them since power-on.

A `staged` line says what was built from a lifted file: the file whose bytes
hold a source, the extent built from it, the class of the register the extent
went to after `to`, the routine, and the mark. One line per file, extent,
class and writer — an extent sent to two classes has a line for each.

The `staging` cartridge builds eight ranges in work RAM and sends each, run for
one second:

```
origin   $7E:0400 bytes 32 from $00:9300 bytes 32 using 32 by none exact
origin   $7E:0500 bytes 2 from $00:9100 bytes 32 using 2 by sub_008300 exact
origin   $7F:0000 bytes 32 from $00:9000 bytes 11 using 5 by sub_008100 exact
origin   $7F:0100 bytes 32 from $00:9100 bytes 32 using 32 by sub_008140 exact
origin   $7F:0300 bytes 16 computed by sub_0081A0
origin   $7F:0600 bytes 3 from $00:9400 bytes 3 using 3 by sub_008380 exact
origin   $7F:0700 bytes 8 from $00:9500 bytes 8 using 8 by sub_0083C0 exact
origin   $7F:0800 bytes 16 from $00:9600 bytes 16 using 16 by sub_008480 exact

staged   vram/00_9300.bin at $7E:0400 bytes 32 to Vram by none exact
staged   vram/00_9100.bin at $7E:0500 bytes 2 to Vram by sub_008300 exact
staged   vram/00_9000.bin at $7F:0000 bytes 32 to Vram by sub_008100 exact
staged   vram/00_9100.bin at $7F:0100 bytes 32 to Vram by sub_008140 exact
staged   hdma/00_9400.bin at $7F:0600 bytes 3 to Display by sub_008380 exact
staged   staged/00_9500.bin at $7F:0700 bytes 8 to Vram by sub_0083C0 exact
staged   staged/00_9500.bin at $7F:0700 bytes 8 to Cgram by sub_0083C0 exact
staged   vram/00_9600.bin at $7F:0800 bytes 16 to Vram by sub_008480 exact
```

The decoder at `$8100` unpacked eleven bytes at `$9000` — five runs, each a
count and a value, then a zero — into thirty-two: the source is the eleven,
the values are five of them. The copy at `$8140` used every byte of its
source. The sixteen bytes at `$7F:0300` were filled from a constant and have
no source, and no file. An engine carried `$9300` in through the port and out
again, so the extent at `$7E:0400` is the engine's, and the two bytes at
`$7E:0500` were stores through the port by `sub_008300` of the first two bytes
of `$9100`: their source is the whole block, since the caller's run over it
begins where theirs does and walked on, and `using 2` says how much of it they
are. Every
lifted file carries a `staged` line for each extent built from it. The three
bytes at `$7F:0600` are an HDMA table the run walked sixty times, copied there
from `$9400`, and its file lives under `hdma/` as a table the engine reads from
the image would. The eight bytes at `$7F:0700` were copied from `$9500` and
sent to VRAM and then to CGRAM, so their source is one file under `staged/`
with a `staged` line per class. The sixteen at `$7F:0800` were copied from
`$9600` and carried out by the CPU itself, a word at a time (§2.16): an extent
like any other, lifted as its source.

Every line here is written fresh from the run and read back by nothing; a
`staged` or `stream` file outlives the run through its `asset` line (§2.12).

### 2.16 What the CPU streamed

```
streamed <address> <register address> <register> <class> from <address> bytes <n> times <n>
         at <lowest>-<highest> | none in <area> | none
```

A run of bytes the CPU carried to a data register one store at a time: the
site of the first store, as the tree places it; the register — `VMDATAL` for
the pair with `VMDATAH`, `CGDATA`, `OAMDATA`, an audio port for its pair — with
its name and class; after `from`, where the first byte came from and after
`bytes` how many consecutive ones followed; after `times`, how many sightings
of exactly this stream the run made; after `at`, where the bytes landed on the
other side of the port, and after `in`, what the PPU used that memory as,
exactly as a [`landed` line](#217-where-a-transfer-landed) says them — `none`
and `none` for the audio ports, whose bytes land in no memory the run can
name. A store continues a stream when its value
is the next byte and it was made at the same site as the last store or on one
straight run from it — no jump, branch, call or return between the two. A
sequence of one byte is not a stream. A stream seen again landing somewhere
else is another line, with its own count.

The bytes came from one of two places. A value loaded from work RAM and stored
as it was is the buffer at the address after `from` being carried out, the next
byte the next address: the buffer is an extent with its `origin` and `staged`
lines (§2.15), exactly as if an engine had carried it, and its source is what
is lifted. Otherwise a value with exactly one image byte as its origin is the
image at the address after `from` being carried, the next byte the next
offset, and the file is the run of image bytes the carrying invocation read
that holds the first — an `asset` line of kind `stream` under the register's
directory, which may be wider than the bytes carried: a loop that reads a
count before its data, or an end mark after it, has read them all.

The same cartridge's loops at `$8180` and `$84C0`:

```
streamed $00:818F $00:2122 CGDATA Cgram from $00:9200 bytes 16 times 1 at $00-$07 in palette
streamed $00:84C8 $00:2118 VMDATAL Vram from $7F:0800 bytes 16 times 1 at $0035-$003D in unshown
```

The first carried sixteen bytes of `$9200` and read an end mark after them, so
its file, `cgram/00_9200.bin`, holds seventeen; the port put them in palette
entries zero to seven. The second carried the buffer
at `$7F:0800` a word at a time; the buffer's `origin` line names `$9600`, and
`vram/00_9600.bin` is its file. Its words landed at `$0035`, and no frame was
drawn between the loop's last store and the run's end, so what the memory was
used as is unshown.

### 2.17 Where a transfer landed

```
landed   <address> channel <n> memory <address> bytes <n> as dma | table | indirect
         at <lowest>-<highest> in <area> times <n>
```

Where the bytes of one [`moved`](#211-what-a-run-moved) range went on the
other side of the port: the range's site, channel, memory address, count and
kind, exactly as its `moved` line writes them, which is how the two are read
together; after `at`, the lowest and the highest address the port put a byte
at — a VRAM word address, `$0000`–`$7FFF`, for a range to `VMDATAL` or
`VMDATAH`; a palette word, `$00`–`$FF`, for one to `CGDATA`; an OAM byte,
`$000`–`$21F`, for one to `OAMDATA` — after `in`, what the PPU used that memory
as, and after `times`, how many sightings of exactly this landing the run
made. A range whose bytes reach no data port — a table to the brightness
register, a fill of a register, a read back into memory — has no line. A
range the run saw land in two places has two lines, each with its own count,
beside the one `moved` line. A fill from one byte lands, and has its line.

The port steps for every byte it takes, so the extent is what the port did —
a range to `VMDATAL` alone under an increment on the high byte lands every
byte on one word, and the line says one word — and under a VRAM address
translation ([SNES machine](snes-machine.md#the-video-registers-a-stub)) it is
the hull of the rotated words, which lies within one aligned block of 256, 512
or 1024 words.

What the memory was used as is read at the first frame the PPU drew after the
bytes landed: at the first start of a frame at which forced blank is off, the
screen mode and the base registers as they then stand say what each word of
the extent lies in — and a range the engine was still carrying at that
frame's start has the bytes it lands afterwards read at the next such frame,
its areas being every area read. The areas are `tilemap1`–`tilemap4`, a layer's screen base
and its one, two or four screens of a thousand words, for the layers the mode
has; `tiles1`–`tiles4`, a layer's name base and the eight, sixteen or
thirty-two thousand words its thousand tiles occupy at the layer's colour
depth in that mode; `sprites`, the four thousand words at the sprite base and
the four thousand after the gap; and `mode7`, the whole of VRAM when the mode
is 7, where the map lies in the even bytes and the tiles in the odd. The line
writes every area the extent intersects, joined by `+`; `none` when it lies in
no area; and `unshown` when no frame was drawn between the landing and the
run's end — a run that never leaves forced blank says `unshown` of every
VRAM landing. A palette landing is `palette` and an OAM landing `oam`,
whatever the frame, since neither depends on a base. Whether a layer is enabled on the screen is
not consulted: a layer the mode has keeps its bases whether or not it is shown
this frame.

A cartridge that uploads in forced blank, sets its bases, turns the screen on,
and in later frames flips a base after uploading behind it, switches to Mode
7, and blanks the screen again before one last upload, run for one second of
its clock:

```
landed   $00:8034 channel 0 memory $00:9000 bytes 64 as dma at $3000-$301F in tiles1+tiles2 times 1
landed   $00:8066 channel 0 memory $00:9100 bytes 64 as dma at $0000-$001F in tilemap1 times 1
landed   $00:8098 channel 0 memory $00:9200 bytes 64 as dma at $0FF0-$100F in tilemap3+tiles1+tiles2+tiles3 times 1
landed   $00:80CA channel 0 memory $00:9300 bytes 32 as dma at $5000-$500F in none times 1
landed   $00:80FC channel 0 memory $00:9400 bytes 64 as dma at $6000-$601F in sprites times 1
landed   $00:8129 channel 0 memory $00:9500 bytes 32 as dma at $10-$1F in palette times 1
landed   $00:815B channel 0 memory $00:A000 bytes 544 as dma at $000-$21F in oam times 1
landed   $00:8192 channel 0 memory $00:9900 bytes 32 as dma at $2100-$2178 in tiles1+tiles2+tiles3+sprites times 1
landed   $00:81C9 channel 0 memory $00:9C00 bytes 64 as dma at $5C00-$5C1F in none times 1
landed   $00:8232 channel 0 memory $7E:0400 bytes 32 as dma at $0020-$002F in tilemap1 times 1
landed   $00:8349 channel 0 memory $00:9700 bytes 64 as dma at $7800-$781F in tilemap2 times 1
landed   $00:837B channel 0 memory $00:9D00 bytes 64 as dma at $0200-$021F in tilemap1+tilemap2 times 1
landed   $00:83BE channel 0 memory $00:9800 bytes 64 as dma at $0000-$001F in mode7 times 1
landed   $00:8401 channel 0 memory $00:9A00 bytes 64 as dma at $0100-$011F in unshown times 1
landed   $00:8430 channel 0 memory $00:A000 bytes 544 as dma at $000-$21F in oam times 2
landed   $00:8430 channel 0 memory $00:A000 bytes 544 as dma at $010-$21F in oam times 1
```

The first ten are the reset code's uploads, made in forced blank before any
base was written and read at the first drawn frame against the bases the
reset code then set: a tileset in the name base BG1 and BG2 share; a map in
BG1's screen; sixty-four bytes that begin in BG3's screen and end where the
three name bases start; thirty-two words nothing addresses; a sprite sheet;
a palette at entry sixteen; a sprite table; thirty-two bytes written under
the 8-bit translation, which scattered sixteen words across a block of a
hundred and twenty-one; a fill; and thirty-two bytes the code copied into
work RAM first and sent from there into BG1's screen. The vertical-blank
handler uploads a map to `$7800` and another to `$0200`, and only then points
BG2 at four screens from `$7400`, the last of which wraps round the end of
VRAM: the first drawn frame reads the one as `tilemap2` and the other, which
BG1's screen held all along, as `tilemap1+tilemap2`; it switches to Mode 7
before its next upload; blanks the screen before its last, which no frame
drew; and sends
the sprite table on the three frames the screen is on — the port put it at
`$000` on two of them, because the PPU reloads the OAM address at the start
of every vertical blank, and at `$010` on the one the handler had moved the
address first, so the one range has two lines.

The line is written fresh and read back by nothing: the next run sees the
landings again, and the one thing that follows from them — the directory a
VRAM file lives under — is kept by the file's `asset` line.


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
  a file survives; a `dma`, `table`, `indirect` or `proven` line matching no file this
  run lifts is dropped, and a `note` says so — and a `staged` or `stream` line
  whole, the file lifted again as the line records it unless this run lifts a
  file over its bytes;
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
`dma`, `routine`, `state`, `seen`, `origin`, `staged`, `streamed` and `landed`
lines among them, which no tool reads back:
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
