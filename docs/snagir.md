# The program file

A `.snagir` file is a whole 65816 program in the
[intermediate representation](ir.md), written as text that stands alone: which
image it is a program of, every region of source with its labels and the runs
of bytes execution never reached, every node with its instruction and its
effects, and the two hardware interrupt sequences. `snes_disasm` writes one as
`program.snagir` at the root of every tree, `snes_render` writes the tree's
source files from it, `snes_lift` prints it back and `snes_differential`
replays the cartridge's run beside it, `tools/ir/ir_text.h` writes and reads
one, and anything that reads tokens can read it.

> **Status.** `renderProgram` writes the file and `parseProgram` reads it;
> reading what was written gives the program back equivalent on every field
> the file carries, and writing what was read gives the same bytes. The
> disassembler writes it before anything else in the tree; the renderer reads
> it and the manifest and nothing else, and the two readers over the
> representation read it and no manifest at all.

---

## Contents

- [1. Form](#1-form)
- [2. Records](#2-records)
  - [2.1 The version](#21-the-version)
  - [2.2 The image](#22-the-image)
  - [2.3 Regions and warnings](#23-regions-and-warnings)
  - [2.4 Labels](#24-labels)
  - [2.5 Data](#25-data)
  - [2.6 Nodes](#26-nodes)
  - [2.7 Effects](#27-effects)
  - [2.8 The interrupt sequences](#28-the-interrupt-sequences)
- [3. An example](#3-an-example)
- [4. Refusals](#4-refusals)
- [5. Library](#5-library)
- [6. Stability](#6-stability)
- [See also](#see-also)

## 1. Form

A program file is UTF-8 text made of words and three delimiters. Whitespace —
spaces, tabs, line breaks — separates words and means nothing else: a file on
one line and the same file spread over a thousand read to the same program.
`{` opens a group and `}` closes it; `;` ends a record and ends an effect. A
string is written between double quotes, with `\"` for a quote and `\\` for a
backslash inside it, and is one word. `//` opens a comment that runs to the
end of its line. The writer indents the groups two spaces per level for a
reader's sake; a reader gives the indentation no meaning.

Every number is hexadecimal and begins with `$`, except three that are
decimal: an instruction's `length`, the four costs after `base`, and the byte
count on the `image` record. An address is written `$BB:XXXX`, the bank then
the offset within it. A value is written with as many digits as it needs and
no leading zeros — `$0`, `$2C`, `$1234`, `$7E0200`.

The records come in one order: the version, the image, then each region with
what it holds in address order, then `nmi` and `irq`.

## 2. Records

### 2.1 The version

```
snagir 1;
```

The first record of every file. A reader that knows version 1 reads the rest;
one given another number stops there (§4).

### 2.2 The image

```
image <bytes> <map>;
```

What the program is a program of: the image's size in bytes and its map —
`LoROM`, `HiROM` or `ExHiROM` — as the
[manifest](project-manifest.md#21-the-image) names them. Written once, after
the version.

### 2.3 Regions and warnings

```
region <file> <first>-<last> {
  warning "<text>";
  …
}
```

`region` opens a region of source: the file it is written to and the address
range it covers, inclusive, within one bank, and then its group. Every record
inside the group belongs to it. Within a region the labels, data runs and
nodes come in address order; at one address a label comes first, then a data
run, then a node — and where two nodes share an address (§2.6) both follow the
label. A region's records account for every byte of its range: each byte is in
a node's instruction or in a data run.

A `warning` comes first in its region's group and carries what the trace could
not settle there, as the manifest's [`warning`](project-manifest.md#25-stops-warnings-and-notes)
line does, its text as a string. A region has as many as the trace gave it,
and most have none.

### 2.4 Labels

```
label <address> <name>;
```

A label the trace gave an address in the region: the address, then the name a
branch, jump or call to it is written with. Every label is at the address of a
node.

### 2.5 Data

```
data <address> <bytes>;
```

A run of bytes execution never reached: its first address, then every byte of
the run as two hexadecimal digits, with nothing between them, as one word. A
run is as long as the trace left it — a table, a graphic, a whole bank — so
the word may be long.

### 2.6 Nodes

A node is one instruction at one address under one mode: its header, then its
effects (§2.7) in a group.

```
<address> <mnemonic> [<addressing>] operand <value> [operand2 <value>] length <n> flow <flow> [target <address>] <mode> base <a>/<b>/<c>/<d> [<register>] [patched] {
  <effect>
  …
}
```

The fields, in order:

- `<address>` — where the instruction is.
- `<mnemonic> [<addressing>]` — the instruction as source names it, and how its
  operand is laid out, as `ir.md` writes the addressing modes: nothing for an
  implied form, `A`, `#imm(M)`, `#imm(X)`, `#byte`, `dp`, `dp,X`, `dp,Y`,
  `(dp)`, `(dp,X)`, `(dp),Y`, `[dp]`, `[dp],Y`, `sr,S`, `(sr,S),Y`, `abs`,
  `abs,X`, `abs,Y`, `long`, `long,X`, `(abs)`, `[abs]`, `(abs,X)`, `rel`,
  `rel16`, `src,dst`, `#abs`. The two forms that write `rel16` are told apart
  by the mnemonic, as every pair is: the mnemonic and the addressing together
  name one opcode.
- `operand <value>` — the operand's value as the dialect writes it: an
  immediate, a direct-page offset, an absolute or long address, the target
  address of a relative form, the value `PEA` pushes, the address `PER` names,
  or a block move's source bank. `operand2 <value>` follows for a block move
  alone, and is its destination bank.
- `length <n>` — the instruction's bytes, in decimal.
- `flow <flow>` — how execution leaves it: `continue`, `branch`, `jump`,
  `call`, `return` or `halt`. `target <address>` follows where the instruction
  names a constant successor — a branch, a jump or a call to an address the
  bytes give.
- `<mode>` — the mode the instruction reads under: `e=1`, or `e=0 m=<w> x=<w>`
  with each width `8`, `16`, or `?` where the trace did not know it and the
  node selects by the live flag.
- `base <a>/<b>/<c>/<d>` — the measured cost under each setting of the widths,
  in `costIndex` order: both eight, index sixteen, accumulator sixteen, both
  sixteen. Decimal.
- `<register>` — the hardware register a long operand names, when it names
  one, by the name the [65816 disassembler](65816-disassembler.md#hardware-registers)
  gives that address. Absent otherwise.
- `patched` — present on a node lifted from bytes that differ from the image
  the code started as.

An address two paths read two ways is two nodes, each with its own mode, the
first reading first — the one the listing carries, which is the one a bank
file is written from.

### 2.7 Effects

Each effect is one record inside its node's group, or inside an interrupt
sequence's:

```
<op> [<dst> <-] [<a>[, <b>]] [<width>[ <step>[ <access>]][ pinned|unpinned]] [<condition>];
```

`<op>` is the operation's name — `Set`, `SetNZ`, `Load`, `Store`, `Adc`,
`WriteP`, every name in [ir.md §Operations](ir.md#operations). `<dst> <-`
follows where the operation has a destination; `<a>` and `<b>` are the
operands it has, the first followed by a comma when there are two. A place is
written by name — `A`, `X`, `Y`, `S`, `D`, `PC`, `PBR`, `DBR`, `P`, `E`, `T0`
to `T3` — and a flag as `P.` and its letter: `P.N`, `P.V`, `P.M`, `P.X`,
`P.D`, `P.I`, `P.Z`, `P.C`. The register `X` and the flag `P.X`, the register
`D` and the flag `P.D`, are two words. A constant is a value, `$20`.

The bracket carries the width — `8`, `16`, `24`, `byM` or `byX` — and, for a
`Load`, `Store` or `StoreRmw`, the step the access's later bytes take (`flat`,
`bank0`, `bank`, `direct`, `pointer`) and, where the access is not plain data,
its kind (`rmw`, `rmw-unmodified`, `vector`); for a `Push` or `Pull`, `pinned`
or `unpinned`. No other operation carries a step, a kind or a pin.

A condition, where the effect has one, follows the bracket: `if e`, `if !e`,
`if set <flag>`, `if clear <flag>`, `if is <place> <value>`,
`if is not <place> <value>`, `if D.lo`, `if crossed`, any of them followed by
`and e` where the emulation flag must be set as well. An effect with no
condition always runs. The semicolon ends the effect.

```
Set PC <- $8007 [16];
Set P.C <- $0 [8];
BankAddress T0 <- $100 [24];
Store T0, A [16 flat];
Load T1 <- T0 [8 direct rmw];
Push PC [16 pinned];
Cycles $1 [8] if D.lo;
Set PC <- $8023 [16] if clear P.Z;
```

### 2.8 The interrupt sequences

```
nmi {
  <effect>
  …
}
irq {
  <effect>
  …
}
```

After the last region, once each: the effects of a non-maskable interrupt and
of a maskable one, taken between two instructions, exactly as
[ir.md §The rules](ir.md#the-rules) gives them. They are the chip's rather than
the program's, and every file carries them.

## 3. An example

The `mixed` cartridge from [`tools/examples/`](../tools/examples/README.md),
disassembled with `snes_disasm mixed.smc -o mixed --no-run --no-sound`, has
this as the start of `mixed/program.snagir`:

```
snagir 1;
image 32768 LoROM;

region bank_00.asm $00:8000-$00:FFFF {
  label $00:8000 reset;
  $00:8000 CLC operand $0 length 1 flow continue e=1 base 2/2/2/2 {
    Set PC <- $8001 [16];
    Set P.C <- $0 [8];
  }
  $00:8001 XCE operand $0 length 1 flow continue e=1 base 2/2/2/2 {
    Set PC <- $8002 [16];
    Xce [8];
  }
  $00:8002 REP #byte operand $30 length 2 flow continue e=0 m=8 x=8 base 3/3/3/3 {
    Set PC <- $8004 [16];
    Set T0 <- P [8];
    And T0 <- T0, $CF [8];
    WriteP T0 [8];
  }
  $00:8004 LDX #imm(X) operand $2 length 3 flow continue e=0 m=16 x=16 base 2/3/2/3 {
    Set PC <- $8007 [16];
    SetNZ X <- $2 [16];
  }
  $00:8007 LDA #imm(M) operand $1234 length 3 flow continue e=0 m=16 x=16 base 2/2/3/3 {
    Set PC <- $800A [16];
    SetNZ A <- $1234 [16];
  }
  $00:800A STA abs operand $100 length 3 flow continue e=0 m=16 x=16 base 4/4/5/5 {
    Set PC <- $800D [16];
    BankAddress T0 <- $100 [24];
    Store T0, A [16 flat];
  }
```

One bank, the reset handler's label before its first node, and no data before
the first instruction. The file goes on through every node of the region — the
block move at `$00:804B` with both its banks, the four labels the trace gave
the loop, the routine and the two interrupt handlers — with a `data` record for
each run of bytes between them, closes the region, and ends with the two
interrupt sequences:

```
  Set PBR <- $0 [8];
  Cycles $7 [8];
  Cycles $1 [8] if !e;
}
```

## 4. Refusals

A file is read whole before anything is made of it, and a record that cannot
be read refuses the whole file, naming the line the offending word is on:

- a file that does not open with `snagir 1;`, or a version this reader does
  not know;
- a record whose first word this page does not name, or a word where a
  delimiter belongs and a delimiter where a word belongs;
- a record with a field missing, a field it does not have, a number that is
  not one, or a name that is not a mnemonic, an addressing mode, a place, a
  width, a step, an access kind, a flow or a condition;
- a mnemonic and an addressing mode that name no opcode together, or a
  register name that is not the name of the register at the operand's address;
- an effect without its `;`, or a step, an access kind or a pin on an operation
  that does not carry one;
- a string that does not close on its line;
- a label, a data run or a node outside a region's group, a warning after a
  region's first label, data run or node, or a record after the `irq` sequence;
- an `nmi` or `irq` missing, or a region, an `image` or an `nmi` inside a
  region.

```
line 12: `Sett` is not an operation
```

## 5. Library

```cpp
#include "ir/ir_text.h"

using namespace snaggletooth::ir;

ProgramFile file;
file.imageBytes = 32768;
file.map = "LoROM";
file.regions.push_back({.file = "bank_00.asm", .first = 0x008000, .last = 0x00FFFF,
                        .warnings = {}, .labels = {{0x008000, "reset"}}, .data = {}});
const std::string text = renderProgram(program, file);   // the file

std::string error;
const std::optional<Parsed> parsed = parseProgram(text, error);
if (!parsed) std::cerr << error << "\n";                 // "line N: …"
equivalent(parsed->program, program);                    // true
parsed->file == file;                                    // true
renderProgram(parsed->program, parsed->file) == text;    // true
```

`renderProgram` takes the `Program` — its nodes in address order, its `nmi`
and `irq` — and a `ProgramFile`, which carries what the program does not: the
image record, and each `ProgramRegion` with its file, its range, its warnings,
its labels and its data runs, the labels and runs in address order. It writes
the file as this page describes it and throws `std::invalid_argument` for a
node no region's range holds. `parseProgram` reads text into a `Parsed` — the
`Program` and the `ProgramFile` back, the nodes in address order whatever
order the regions came in — or returns nothing with `error` naming the line;
the mnemonics and register names in the nodes it returns are the instruction
table's and the register table's own. `renderNode` writes one node as it
stands inside a region's group, and `renderEffect` one effect with its
semicolon; both are what `renderProgram` calls. `selectFile(parsed, file)`
cuts a `Parsed` to the regions written to one source file — those regions,
the nodes whose addresses they hold, and the image line and the interrupt
sequences as they were — or returns nothing when no region is written to it;
`countProgram(parsed)` is a `ProgramCounts`: the regions, the code lines (one
per address a node stands at), the nodes, the nodes that select a width by
the live flag, the nodes naming a hardware register, the nodes lifted from
patched bytes, and the effects — the summary `snes_lift` prints.

Every type in `ir/ir.h` compares with `==`, and `equivalent` compares two
programs as the file carries them: every field, except the bit behind a width
the mode does not know. The file writes `?` for that width, the node selects
by the live flag, and no reader of the mode looks at the bit, so a program
read back is equivalent to the one written and the file carries nothing that
has no meaning. The library target is `snaggletooth_ir`.

## 6. Stability

This document defines a published surface, held to the same rule as the
[assembly language](assembly-lexicon.md#8-stability): once a release writes a
file, a later release reads it to the same program. The fields of the records
above do not change, and a name — an operation, a place, a width, a step, an
access kind, a condition, a flow — is never renamed or given a different
meaning. A new record, a new field at the end of a record, or a new name may
be added, and is added to this page before a release writes it; a reader
treats a record it does not know as an error, never as one to pass over. The
version number rises only if a record's written form has to change, and a
reader given a number it does not know stops.

## See also

- [The intermediate representation](ir.md) — what a node and an effect mean,
  every rule the effects follow, and the vocabulary the names here are.
- [Cartridge disassembler](snes-disassembler.md) — the tree whose program the
  file is, and `snes_render`, which writes the tree from it.
- [Project manifest](project-manifest.md) — the other file at the root of a
  tree: the facts the trace and the run attach to the program.
