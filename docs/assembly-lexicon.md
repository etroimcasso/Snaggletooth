# Assembly language: the common layer

The part of Snaggletooth's assembly language that does not depend on which chip the
code is for. Two dialects share it — [SPC700 assembly](spc700-assembly.md) and
[65816 assembly](65816-assembly.md) — and each dialect page describes only what
its instruction set adds: the mnemonics, the operand syntax of its addressing
modes, and the directives its encoding needs. Everything here holds in both.

> **Status.** Both assemblers are built — [assemblers.md](assemblers.md) — and
> the disassemblers emit this language. Every opcode of each chip round-trips
> through its own text; a real sound program's dump, disassembled and
> reassembled, is byte-identical and renders byte-identical audio; and
> thirty-one cartridges' source trees reassemble to their images byte for byte.

A disassembler and its assembler are inverses. A listing assembles back to the
bytes it came from, so a binary can be disassembled, edited and rebuilt without its
original source ever existing.

---

## Contents

- [1. Scope](#1-scope)
- [2. Source format](#2-source-format)
  - [2.1 Comments](#21-comments)
  - [2.2 Case](#22-case)
  - [2.3 Whitespace](#23-whitespace)
- [3. Numbers](#3-numbers)
  - [3.1 Character literals](#31-character-literals)
- [4. Symbols](#4-symbols)
  - [4.1 Labels](#41-labels)
  - [4.2 Constants](#42-constants)
  - [4.3 Expressions](#43-expressions)
- [5. Directives](#5-directives)
  - [5.1 `ORG`](#51-org)
  - [5.2 `DB`, `DW`, `DL`](#52-db-dw-dl)
  - [5.3 `DS`](#53-ds)
  - [5.4 `INCBIN`](#54-incbin)
- [6. Round-trip](#6-round-trip)
- [7. Diagnostics](#7-diagnostics)
- [8. Stability](#8-stability)
- [See also](#see-also)

## 1. Scope

The language covers an instruction set, symbolic labels, and absolute placement. It
has no macros, no source include mechanism, no conditional assembly, no sections
and no relocation; `INCBIN` places a file's bytes, which is placement, not
assembly. Assembly is absolute: every byte's address is known while it is being
assembled.

A program needing more than this generates its source with a program.

Each dialect has an address width — 16 bits for the SPC700, 24 for the 65816 — and
it is the width of every address, label and `ORG` in that dialect.

---

## 2. Source format

Source is UTF-8 text. Only the ASCII range is meaningful outside comments and
string literals.

A line is:

```
[label:] [instruction | directive] [; comment]
```

Every element is optional; a blank line is legal. One instruction per line.

Line endings may be LF or CRLF. Trailing whitespace is insignificant.

### 2.1 Comments

A semicolon begins a comment that runs to the end of the line. A semicolon inside
a string literal is an ordinary character.

```
        MOV A,$10       ; the comment starts here
        DB "a;b"        ; the semicolon here is data
```

### 2.2 Case

Mnemonics, register names and directives are case-insensitive: `mov a,$10`,
`MOV A,$10` and `Mov A,$10` assemble identically. The disassemblers emit upper
case.

**Labels are case-sensitive.** `Loop` and `loop` are two different symbols.

### 2.3 Whitespace

Whitespace separates tokens and is otherwise insignificant. A label must begin in
column 1; anything indented is an instruction or a directive. This is the only
place layout carries meaning.

---

## 3. Numbers

| Form | Base | Example |
|---|---|---|
| `$` prefix | 16 | `$1F`, `$0A2B` |
| `%` prefix | 2 | `%10110001` |
| bare digits | 10 | `31`, `2603` |

`0x` is not accepted. There is one hexadecimal form, so two listings of the same
bytes differ only where the bytes differ.

A number's written width does not affect encoding: `$0A` and `$A` are the same
byte, and an operand's width comes from its addressing mode — marked in the
operand syntax the dialect defines — never from how many digits were typed.

### 3.1 Character literals

A single character in single quotes is its ASCII code: `'A'` is `$41`. The escapes
`\\`, `\'`, `\"`, `\n`, `\r`, `\t` and `\0` are recognised.

---

## 4. Symbols

### 4.1 Labels

A label is defined by a name in column 1 followed by a colon. The colon is
required on definition and absent on reference.

```
loop:
        DBNZ Y,loop
```

A name matches `[A-Za-z_.][A-Za-z0-9_.]*`. A name that spells a mnemonic,
register or directive, in any case, is rejected, so `mov:` is an error and so is
`x:`.

A label takes the address of the byte that follows it. Two labels on consecutive
lines with nothing between them are the same address.

Redefining a label is an error. Forward references are legal — the assembler
resolves symbols in a second pass, so a label may be used before the line that
defines it.

### 4.2 Constants

`EQU` binds a name to a value:

```
T0OUT   EQU $FD
        MOV X,T0OUT
```

An `EQU` name is defined in column 1 with no colon. Its value must be resolvable
when the directive is read, so an `EQU` may not refer forward.

### 4.3 Expressions

An expression is a sequence of terms joined by `+` or `-`, evaluated left to
right. A term is a number, a character literal, a symbol, or `*` for the address
of the current line.

```
        MOV !template+13,Y
        MOV A,#end-start
```

There is no multiplication, no division, no bitwise operator, no parenthesis and
no precedence.

Arithmetic is unsigned and wraps at the dialect's address width. A value used
where a byte is required must fit in 8 bits after evaluation, and one used where a
word is required in 16, or the assembler reports it.

---

## 5. Directives

### 5.1 `ORG`

```
        ORG $0400
```

Sets the assembly address. Every byte after it is placed from there. A source
file with no `ORG` starts at address zero. The address must be resolvable when
the directive is read, so an `ORG` may not refer forward.

`ORG` may move forward, leaving a gap; the gap's contents are not emitted, and
the output describes the ranges that were written. `ORG` may not move backward
over bytes already emitted — overlapping output is an error rather than a silent
overwrite.

An `ORG` begins a region: whatever a dialect carries from line to line — the
65816's register widths — starts over there. So does a data directive.

### 5.2 `DB`, `DW`, `DL`

```
        DB $01,$02,$03
        DB "text",0
```

`DB` emits bytes. Each item is an expression yielding a byte, or a double-quoted
string emitting one byte per character. Escapes are as in §3.1.

`DW` emits 16-bit values, low byte first:

```
        DW $0A2B, table+2
```

`DL` emits 24-bit values, low byte first, and exists only in a dialect whose
addresses are 24 bits wide:

```
        DL $7E:2000, handler
```

### 5.3 `DS`

```
        DS 16            ; sixteen bytes of $00
        DS 16,$FF        ; sixteen bytes of $FF
```

Emits a run of a repeated byte. The fill defaults to `$00`. The count must be
resolvable when the directive is read.

### 5.4 `INCBIN`

```
        INCBIN "vram/00_9000.bin"
        INCBIN "vram/00_9000.bin", 32, 16     ; sixteen bytes from offset thirty-two
```

Emits the bytes of a file. The path is a string literal, with the escapes of
§3.1, and is relative to the file the directive is in. With an offset and a
length, only that part of the file is emitted: both must be resolvable when the
directive is read, the offset must lie within the file, and the length must be
at least one and reach no further than the file's end. A file that cannot be
read is an error naming the path.

`INCBIN` is a data directive: a region begins after it, as after `DB`.

An assembly reads files only through the reader its caller gives it — see
[assemblers.md](assemblers.md#library). The command-line assemblers read
beside the source; `snes_verify` reads from the tree it verifies; an assembly
given no reader reports the directive as an error rather than open anything.

---

## 6. Round-trip

The guarantee: for any byte range a disassembler emitted as code, as `DB`, or as
an `INCBIN` of a file it wrote, assembling its output reproduces those bytes
exactly.

What round-trip preserves: every emitted byte, at its original address.

What it does not preserve: layout, comment text, generated label names, and the
trailing comments carrying addresses, raw bytes and cycle costs. Those are
commentary on the bytes, not the bytes.

Bytes in a gap the disassembler never described are not covered — it emits what
it read, so a gap only exists where the input had one.

Renaming labels, adding comments and re-indenting all round-trip: none of that is
data. Changing an instruction changes the bytes.

---

## 7. Diagnostics

Every diagnostic names the file, the line, and what was expected. The assembler
does not guess: an operand that could plausibly be two modes is an error naming
both rather than a choice made quietly.

Assembly stops emitting output on the first error but continues parsing, so one
run reports every error in the file rather than one per run. A value that has
to be known when its line is read — an `EQU`, an `ORG`, a `DS` count, and in
the 65816 dialect the mask of a `REP` or `SEP` — reports a name defined later
in the file as such.

---

## 8. Stability

This document and the two dialect pages define a published surface. Once a
release accepts a program, a later release assembles that program to the same
bytes.

Additions are permitted — new directives, new number forms, a wider expression
grammar — because they cannot change what an existing program means. Changes to
what an accepted program assembles to are not, and neither is removing anything
these pages describe.

---

## See also

- [SPC700 assembly language](spc700-assembly.md) — the audio CPU's dialect.
- [65816 assembly language](65816-assembly.md) — the main CPU's dialect.
- [The assemblers](assemblers.md) — the tools that read this language, and the
  library they are built on.
- [SPC700 disassembler](spc700-disassembler.md) and
  [65816 disassembler](65816-disassembler.md) — the tools that emit it.
