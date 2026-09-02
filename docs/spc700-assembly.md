# SPC700 assembly language

The dialect [`spc700_disasm`](spc700-disassembler.md) emits.

> **Status.** The assembler is not built. The disassembler emits this language
> today; sections describing what the assembler accepts describe the target, not
> shipped behaviour.

The two tools are inverses. A listing assembles back to the bytes it came from, so
a binary can be disassembled, edited and rebuilt without its original source ever
existing.

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
- [5. Addressing modes](#5-addressing-modes)
  - [5.1 Branch targets are addresses, not displacements](#51-branch-targets-are-addresses-not-displacements)
  - [5.2 Bit operands](#52-bit-operands)
  - [5.3 The call forms that carry their destination in the opcode](#53-the-call-forms-that-carry-their-destination-in-the-opcode)
  - [5.4 The two-operand direct-page order](#54-the-two-operand-direct-page-order)
- [6. Directives](#6-directives)
  - [6.1 `ORG`](#61-org)
  - [6.2 `DB`](#62-db)
  - [6.3 `DS`](#63-ds)
- [7. Round-trip](#7-round-trip)
- [8. Diagnostics](#8-diagnostics)
- [9. Stability](#9-stability)
- [See also](#see-also)

## 1. Scope

The language covers the SPC700 instruction set, symbolic labels, and absolute
placement. It has no macros, no include mechanism, no conditional assembly, no
sections and no relocation. Assembly is absolute: every byte's address is known
while it is being assembled.

A program needing more than this generates its source with a program.

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
`MOV A,$10` and `Mov A,$10` assemble identically. The disassembler emits upper
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
byte, and an operand's width comes from its addressing mode (§5), never from how
many digits were typed.

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
register or directive is rejected, so `mov:` is an error.

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

Arithmetic is on unsigned 16-bit values and wraps. A value used where a byte is
required must fit in 8 bits after evaluation, or the assembler reports it.

---

## 5. Addressing modes

The operand syntax, one row per mode. `dp` is a direct-page offset, `abs` a
16-bit address, `imm` an 8-bit immediate.

| Syntax | Mode | Bytes | Example |
|---|---|---|---|
| — | implied | 1 | `NOP`, `MOV A,X`, `MUL YA` |
| `#imm` | immediate | 2 | `MOV A,#$01` |
| `dp` | direct page | 2 | `MOV A,$10` |
| `dp+X` | direct page indexed by X | 2 | `MOV A,$10+X` |
| `dp+Y` | direct page indexed by Y | 2 | `MOV X,$10+Y` |
| `!abs` | absolute | 3 | `MOV A,!$0A2B` |
| `!abs+X` | absolute indexed by X | 3 | `MOV A,!$0B0B+X` |
| `!abs+Y` | absolute indexed by Y | 3 | `MOV A,!$0B0B+Y` |
| `(X)` | indirect through X | 1 | `MOV A,(X)` |
| `(X)+` | indirect through X, X steps after | 1 | `MOV A,(X)+` |
| `[dp+X]` | indexed indirect | 2 | `MOV A,[$10+X]` |
| `[dp]+Y` | indirect indexed | 2 | `MOV A,[$10]+Y` |
| `(X),(Y)` | indirect to indirect | 1 | `ADC (X),(Y)` |
| `dp,dp` | direct page to direct page | 3 | `MOV $12,$34` |
| `dp,#imm` | immediate to direct page | 3 | `MOV $12,#$34` |
| `!abs.b` | absolute bit | 3 | `MOV1 C,!$1234.5` |
| `/!abs.b` | absolute bit, negated | 3 | `AND1 C,/!$1234.5` |
| `rel` | relative branch | 2 | `BNE loop` |
| `dp.b,rel` | direct-page bit branch | 3 | `BBS $10.3,loop` |

The `!` on absolute operands is required. It is what distinguishes `MOV A,$10`
(direct page, two bytes) from `MOV A,!$0010` (absolute, three bytes). Both reach
the same byte at run time and neither is a substitute for the other, because they
differ in length, in cycle cost, and in whether the P flag moves them.

### 5.1 Branch targets are addresses, not displacements

A relative branch takes the address it goes to. The assembler computes the
displacement from the end of the instruction and reports a target further than
−128 or +127 away rather than silently truncating it.

```
loop:   MOV A,(X)+
        BNE loop        ; not BNE -3
```

### 5.2 Bit operands

An absolute bit operand packs a 13-bit address and a 3-bit index into one 16-bit
operand. Written `!addr.bit`, the address must fit in 13 bits (`$0000`–`$1FFF`)
and the index in 0–7. A leading `/` selects the negated form where the
instruction has one.

Direct-page bit instructions — `SET1`, `CLR1`, `BBS`, `BBC` — carry the bit index
in the opcode rather than the operand, so it is written the same way but is not
part of the operand byte.

### 5.3 The call forms that carry their destination in the opcode

Three instructions do not take an ordinary address operand.

```
        TCALL 0         ; through the table below $FFDE; the entry is 0-15
        TCALL 15
        PCALL $12       ; a one-byte offset into page $FF, so $FF12
        BRK             ; no operand
```

`TCALL`'s operand is an entry number in decimal, 0 through 15, and rides in the
opcode's high nibble — it is not an address and not an emitted byte. `PCALL`
takes a **one-byte** offset; the disassembler prints the full destination it
reaches as a comment, but the operand written in source is the byte.

### 5.4 The two-operand direct-page order

`MOV $12,$34` moves the byte at `$34` into `$12` — destination first, as
everywhere else in the language. The encoding stores the **source** offset first:
the bytes are `FA 34 12`.

The whole `ADC`/`SBC`/`CMP`/`AND`/`OR`/`EOR` `dp,dp` family and the `dp,#imm`
family behave the same way. Text order and byte order differ, and the assembler
and disassembler agree on both.

---

## 6. Directives

### 6.1 `ORG`

```
        ORG $0400
```

Sets the assembly address. Every byte after it is placed from there. A source
file with no `ORG` starts at `$0000`.

`ORG` may move forward, leaving a gap; the gap's contents are not emitted, and
the output describes the ranges that were written. `ORG` may not move backward
over bytes already emitted — overlapping output is an error rather than a silent
overwrite.

### 6.2 `DB`

```
        DB $01,$02,$03
        DB "text",0
```

Emits bytes. Each item is an expression yielding a byte, or a double-quoted
string emitting one byte per character. Escapes are as in §3.1.

`DW` emits 16-bit values, low byte first:

```
        DW $0A2B, table+2
```

### 6.3 `DS`

```
        DS 16            ; sixteen bytes of $00
        DS 16,$FF        ; sixteen bytes of $FF
```

Emits a run of a repeated byte. The fill defaults to `$00`.

---

## 7. Round-trip

The guarantee: for any byte range the disassembler emitted as code or as `DB`,
assembling its output reproduces those bytes exactly.

What round-trip preserves: every emitted byte, at its original address.

What it does not preserve: layout, comment text, generated label names, and the
trailing comments carrying addresses, raw bytes and cycle costs. Those are
commentary on the bytes, not the bytes.

Bytes in a gap the disassembler never described are not covered — it emits what
it read, so a gap only exists where the input had one.

Renaming labels, adding comments and re-indenting all round-trip: none of that is
data. Changing an instruction changes the bytes.

---

## 8. Diagnostics

Every diagnostic names the file, the line, and what was expected. The assembler
does not guess: an operand that could plausibly be two modes is an error naming
both rather than a choice made quietly.

Assembly stops emitting output on the first error but continues parsing, so one
run reports every error in the file rather than one per run.

---

## 9. Stability

This document defines a published surface. Once a release accepts a program, a
later release assembles that program to the same bytes.

Additions are permitted — new directives, new number forms, a wider expression
grammar — because they cannot change what an existing program means. Changes to
what an accepted program assembles to are not, and neither is removing anything
this document describes.

---

## See also

- [SPC700 disassembler](spc700-disassembler.md) — the tool that emits this language.
- [SPC700 CPU core](spc700-cpu.md) — the instruction set itself: opcodes, cycles,
  and the interpreter.
