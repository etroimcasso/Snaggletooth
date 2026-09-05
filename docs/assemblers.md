# The assemblers

`spc700_asm` and `cpu65816_asm` turn assembly source into bytes. Each reads its
chip's dialect — [SPC700](spc700-assembly.md), [65816](65816-assembly.md) —
over the [common layer](assembly-lexicon.md), which is the language the two
disassemblers emit. A listing a disassembler wrote assembles back to the bytes it
came from, so a program can be disassembled, edited and rebuilt without its
original source ever existing.

Both tools are thin over one library. The common layer — lines, numbers,
symbols, expressions, `ORG` and the data directives, the two passes, the
diagnostics — is `snaggletooth_assembler`; each chip's dialect is a class beside
that chip's disassembler, built from the same instruction table the
disassembler decodes with, so the two cannot disagree about what an
instruction's bytes are.

---

## Contents

- [Command line](#command-line)
- [What is written](#what-is-written)
- [Diagnostics](#diagnostics)
- [The 65816 and its widths](#the-65816-and-its-widths)
- [Library](#library)
- [Writing a dialect](#writing-a-dialect)
- [Status](#status)
- [See also](#see-also)

## Command line

```
spc700_asm   <source.asm> -o <out.bin> [--base ADDR] [--size N] [--fill BYTE]
cpu65816_asm <source.asm> -o <out.bin> [--base ADDR] [--size N] [--fill BYTE]
```

A sound program's dump, disassembled from its entry and assembled back into the
full 64 KB it came from:

```
spc700_disasm ram.bin --base 0 --entry 0x054C -o driver.asm
spc700_asm driver.asm -o ram-rebuilt.bin --base 0 --size 65536
cmp ram.bin ram-rebuilt.bin
```

One bank of a cartridge, from the source tree the
[cartridge disassembler](snes-disassembler.md) wrote:

```
cpu65816_asm bank_00.asm -o bank_00.bin
```

A whole tree at once, every file placed where its manifest says and compared
with the image, is [`snes_verify`](snes-disassembler.md#verifying-the-tree),
which runs these assemblers as a library.

Numbers on the command line are decimal, or hexadecimal behind `0x` or `$`. A
file an `INCBIN` names is read beside the source, by the path the directive
gives relative to the source's own directory.

## What is written

The output is a flat image. Without `--base` and `--size` it runs from the first
byte the source emitted to the last; with them it is exactly the window asked
for, and a byte the source placed outside the window is an error. Gaps — an
`ORG` that moved forward, or the space between the first byte and `--base` —
hold `--fill`, `$00` by default.

The tool prints the ranges it wrote, one per line, so a file with several
forward `ORG`s says where each landed:

```
  $008000-$00CB03  19204 bytes
  $00CB13-$00FFFF  13549 bytes
wrote 32768 bytes to bank_00.bin
```

A bank file the cartridge disassembler wrote begins at its bank's first address
— `ORG $00:8000` — so its image starts there, and the ranges it reports are the
pieces the file holds around the sound program's bytes.

## Diagnostics

Every error names the file, the line and what was expected, and one run reports
every error in the file:

```
driver.asm:2: $012C does not fit in a byte
driver.asm:4: `MOV A,` is not a form of MOV
driver.asm:5: `nowhere` is not defined
3 errors; nothing written
```

A file with any error writes nothing; the exit status is 1. Wrong arguments
exit 2.

## The 65816 and its widths

An immediate is as wide as the register it loads, so `cpu65816_asm` follows the
register widths through the file the way the disassembler does — through `REP`,
`SEP`, `XCE`, `PLP` and `RTI` — and the source says what the instructions
cannot with `A8`, `A16`, `X8`, `X16`, `EMULATION` and `NATIVE`. A region begins
at the file's start, at every `ORG` and after every data directive, in native
mode with both widths unknown; an immediate under an unknown width is an error
naming the directive that settles it, never a guess. The rules are in the
[65816 dialect](65816-assembly.md#22-immediates-and-the-register-widths), and
the disassembler emits exactly the directives they need.

## Library

The command lines are thin over `assembler/assembler.h`, target
`snaggletooth_assembler`, and the two dialects, `spc700_asm.h` in
`snaggletooth_spc700` and `cpu65816_asm.h` in `snaggletooth_cpu65816`. `tools/`
is on the assembler library's public include path; each backend directory is on
its own.

```cpp
#include "spc700_asm.h"

const snaggletooth::assembler::Assembly assembly =
    snaggletooth::assembler::assembleSpc700(sourceText, "driver.asm");
if (!assembly.ok()) {
  for (const auto& error : assembly.errors) {
    // error.file, error.line, error.message
  }
}
for (const auto& range : assembly.ranges) {
  // range.start, range.bytes — in address order, never overlapping
}
assembly.symbols;  // every label and EQU with its value
```

`assembleCpu65816` is the same call for the other dialect. `image(assembly,
base, size, fill)` lays the ranges into one buffer, or returns nothing when a
range lies outside it. `assemble(dialect, source, file, reader)` takes any
`Dialect`.

A file an `INCBIN` names is read through the `Reader` the caller passes — a
function from a path to the file's bytes, or nothing when it cannot be read.
The path it is asked for is the directive's, joined with the directory of the
file the directive is in and normalised, so a reader keyed by paths relative to
a tree's root is asked for exactly those: `bank_00.asm` including
`vram/tiles.bin` asks for `vram/tiles.bin`; `code/bank_00.asm` including
`../vram/tiles.bin` asks for the same. Without a reader an `INCBIN` is an error
saying the assembly reads no files; the assembler never opens the filesystem
itself, so a front end that holds a tree in memory assembles it whole.

```cpp
const snaggletooth::assembler::Reader reader =
    [&](const std::string& path) -> std::optional<std::string> {
      return tree.contains(path) ? std::optional(tree.at(path)) : std::nullopt;
    };
const snaggletooth::assembler::Assembly assembly =
    snaggletooth::assembler::assembleCpu65816(sourceText, "bank_00.asm", reader);
```

| Symbol | Purpose |
|---|---|
| `Assembly` | The result: `ranges`, `symbols`, `errors`, and `ok()`. |
| `Range` | A run of bytes and the address of its first. |
| `Diagnostic` | A file, a line counted from 1, and a message. |
| `Dialect` | The interface a chip's assembler implements. |
| `Evaluator` | What a dialect evaluates its operands' expressions with. |
| `Spc700Dialect`, `Cpu65816Dialect` | The two dialects; `Cpu65816Dialect::mode()` is the mode the next instruction assembles under. |
| `assembleSpc700`, `assembleCpu65816` | The two calls that name their dialect. |
| `Reader` | How an assembly reads a file an `INCBIN` names. |
| `coreDirective` | Whether a name is a directive of the common layer, and so may not be a label. |
| `image` | The ranges laid into one buffer. |

## Writing a dialect

A dialect owns its mnemonics, the operand syntax of its addressing modes, and
any directive its encoding needs. It implements `Dialect`:

- `name()` and `addressBits()` — the chip, and 16 or 24.
- `reserved(upperName)` — whether a name is a mnemonic, a register or a
  directive of the dialect, and so cannot be a label.
- `beginRegion()` — called at the start of assembly, at every `ORG` and after
  every data directive; whatever the dialect carries along a region starts over.
- `directive(upperName, operands, evaluator, error)` — a directive of the
  dialect's own, or `false` when the name is not one.
- `encode(mnemonic, operands, at, evaluator)` — one instruction's bytes, or an
  error. The mnemonic arrives upper-cased; the operands as written, with the
  comment removed and the ends trimmed.

The assembler runs two passes. On the first, a symbol the file defines later
evaluates unresolved — value zero, `resolved` false — and the dialect emits bytes
of the right length without checking the value; on the second everything is
known. A dialect whose instruction lengths depend on a value, as the 65816's do
on a `REP` or `SEP` mask, reports a mask that is unresolved on the first pass
rather than guess a length.

`compact(text)` removes the spaces outside quotes so syntax can be matched
character by character; `expressionEnd(text, from)` finds where an expression
ends inside the dialect's own syntax, stopping before an index suffix written
`+X` or `+Y`; `fits(value, bits)` and `hex(value, digits)` are for the
diagnostics.

## Status

Both dialects are complete: every opcode of each chip, decoded by its
disassembler and assembled back, gives the bytes decoded — under every setting
of the 65816's widths. A real sound program's dump, disassembled from its entry
and reassembled, is byte-identical, and the 90 s render of the reassembled dump
is byte-identical to the original's. Thirty-one cartridges' source trees, one
file per bank plus the sound program, reassemble to their images byte for byte
under `snes_verify`, with branches, jumps and calls written as the labels the
listings define.

## See also

- [Assembly language: the common layer](assembly-lexicon.md) — what both
  dialects share.
- [SPC700 assembly language](spc700-assembly.md) and
  [65816 assembly language](65816-assembly.md) — the two dialects.
- [SPC700 disassembler](spc700-disassembler.md) and
  [65816 disassembler](65816-disassembler.md) — the inverses, over the same
  tables.
- [Cartridge disassembler](snes-disassembler.md) — the source tree a whole
  cartridge becomes, which these assemblers rebuild.
