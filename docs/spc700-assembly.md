# SPC700 assembly language

The dialect [`spc700_disasm`](spc700-disassembler.md) emits. Everything that does
not depend on the instruction set — source format, numbers, symbols, expressions,
the placement and data directives, the round-trip guarantee, diagnostics and
stability — is the [common layer](assembly-lexicon.md); this page describes what
the SPC700 adds to it.

> **Status.** `spc700_asm` assembles this dialect and `spc700_disasm` emits it —
> [assemblers.md](assemblers.md). Every opcode round-trips through its own text,
> and a real sound program's dump, disassembled and reassembled, is
> byte-identical and renders byte-identical audio.

---

## Contents

- [1. Scope](#1-scope)
- [2. Addressing modes](#2-addressing-modes)
  - [2.1 Branch targets are addresses, not displacements](#21-branch-targets-are-addresses-not-displacements)
  - [2.2 Bit operands](#22-bit-operands)
  - [2.3 The call forms that carry their destination in the opcode](#23-the-call-forms-that-carry-their-destination-in-the-opcode)
  - [2.4 The two-operand direct-page order](#24-the-two-operand-direct-page-order)
- [See also](#see-also)

## 1. Scope

The dialect covers the SPC700's 256 opcodes over the common layer. Addresses are
16 bits wide: every label, `ORG` and absolute operand is a 16-bit value, and
expression arithmetic wraps at 16 bits.

---

## 2. Addressing modes

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

### 2.1 Branch targets are addresses, not displacements

A relative branch takes the address it goes to. The assembler computes the
displacement from the end of the instruction and reports a target further than
−128 or +127 away rather than silently truncating it.

```
loop:   MOV A,(X)+
        BNE loop        ; not BNE -3
```

### 2.2 Bit operands

An absolute bit operand packs a 13-bit address and a 3-bit index into one 16-bit
operand. Written `!addr.bit`, the address must fit in 13 bits (`$0000`–`$1FFF`)
and the index in 0–7. A leading `/` selects the negated form where the
instruction has one. The index is what follows the last `.` in the operand, so
a name with a `.` in it — `flags.io.3` — still reads as the name and its bit.

Direct-page bit instructions — `SET1`, `CLR1`, `BBS`, `BBC` — carry the bit index
in the opcode rather than the operand, so it is written the same way but is not
part of the operand byte.

### 2.3 The call forms that carry their destination in the opcode

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
reaches as a comment, but the operand written in source is the byte, and
`PCALL $FF12` is an error.

### 2.4 The two-operand direct-page order

`MOV $12,$34` moves the byte at `$34` into `$12` — destination first, as
everywhere else in the language. The encoding stores the **source** offset first:
the bytes are `FA 34 12`.

The whole `ADC`/`SBC`/`CMP`/`AND`/`OR`/`EOR` `dp,dp` family and the `dp,#imm`
family behave the same way. Text order and byte order differ, and the assembler
and disassembler agree on both.

---

## See also

- [Assembly language: the common layer](assembly-lexicon.md) — source format,
  numbers, symbols, directives, round-trip, diagnostics and stability.
- [The assemblers](assemblers.md) — `spc700_asm`, the tool that reads this
  language.
- [SPC700 disassembler](spc700-disassembler.md) — the tool that emits this language.
- [SPC700 CPU core](spc700-cpu.md) — the instruction set itself: opcodes, cycles,
  and the interpreter.
- [65816 assembly language](65816-assembly.md) — the main CPU's dialect over the
  same layer.
