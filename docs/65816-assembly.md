# 65816 assembly language

The dialect [`cpu65816_disasm`](65816-disassembler.md) emits. Everything that does
not depend on the instruction set — source format, numbers, symbols, expressions,
the placement and data directives, the round-trip guarantee, diagnostics and
stability — is the [common layer](assembly-lexicon.md); this page describes what
the 65816 adds to it.

> **Status.** `cpu65816_asm` assembles this dialect and `cpu65816_disasm` emits
> it — [assemblers.md](assemblers.md). Every opcode round-trips through its own
> text under every setting of the register widths, and thirty-one cartridges'
> source trees reassemble to their images byte for byte.

---

## Contents

- [1. Scope](#1-scope)
- [2. Addressing modes](#2-addressing-modes)
  - [2.1 Absolute, direct and long](#21-absolute-direct-and-long)
  - [2.2 Immediates and the register widths](#22-immediates-and-the-register-widths)
  - [2.3 Branch targets are addresses, not displacements](#23-branch-targets-are-addresses-not-displacements)
  - [2.4 The jumps and calls](#24-the-jumps-and-calls)
  - [2.5 Block moves](#25-block-moves)
  - [2.6 The stack forms and the signatures](#26-the-stack-forms-and-the-signatures)
- [3. Directives](#3-directives)
  - [3.1 `A8`, `A16`, `X8`, `X16`](#31-a8-a16-x8-x16)
  - [3.2 `EMULATION`, `NATIVE`](#32-emulation-native)
- [See also](#see-also)

## 1. Scope

The dialect covers the 65816's 256 opcodes over the common layer. Addresses are
24 bits wide: a bank in the top byte and an offset below it. A label, an `ORG` and
a long operand are 24-bit values, and expression arithmetic wraps at 24 bits.

The mnemonics are the datasheet's. `JML` and `JSL` name the long jump and the long
call; `INC A` and `DEC A` name the accumulator forms.

---

## 2. Addressing modes

The operand syntax, one row per mode. `dp` is a direct-page offset, `abs` a
16-bit address, `long` a 24-bit address, `sr` a stack offset, `imm` an
immediate, `bank` a bank byte.

| Syntax | Mode | Bytes | Example |
|---|---|---|---|
| — | implied | 1 | `NOP`, `TXA`, `XCE` |
| `A` | accumulator | 1 | `ASL A`, `INC A` |
| `#imm` | immediate | 2 or 3 | `LDA #$12`, `LDA #$1234` |
| `dp` | direct page | 2 | `LDA $12` |
| `dp,X` | direct page indexed by X | 2 | `LDA $12,X` |
| `dp,Y` | direct page indexed by Y | 2 | `LDX $12,Y` |
| `(dp)` | direct page indirect | 2 | `LDA ($12)` |
| `(dp,X)` | direct page indexed indirect | 2 | `LDA ($12,X)` |
| `(dp),Y` | direct page indirect indexed | 2 | `LDA ($12),Y` |
| `[dp]` | direct page indirect long | 2 | `LDA [$12]` |
| `[dp],Y` | direct page indirect long indexed | 2 | `LDA [$12],Y` |
| `sr,S` | stack relative | 2 | `LDA $12,S` |
| `(sr,S),Y` | stack relative indirect indexed | 2 | `LDA ($12,S),Y` |
| `!abs` | absolute | 3 | `LDA !$1234` |
| `!abs,X` | absolute indexed by X | 3 | `LDA !$1234,X` |
| `!abs,Y` | absolute indexed by Y | 3 | `LDA !$1234,Y` |
| `long` | absolute long | 4 | `LDA $7E:1234` |
| `long,X` | absolute long indexed | 4 | `LDA $7E:1234,X` |
| `(!abs)` | absolute indirect | 3 | `JMP (!$1234)` |
| `[!abs]` | absolute indirect long | 3 | `JML [!$1234]` |
| `(!abs,X)` | absolute indexed indirect | 3 | `JMP (!$1234,X)`, `JSR (!$1234,X)` |
| `rel` | relative branch | 2 | `BNE loop` |
| `rel` | relative branch long | 3 | `BRL far` |
| `bank,bank` | block move | 3 | `MVN $00,$7E` |

### 2.1 Absolute, direct and long

Three modes can name the same byte and differ in length, in cycle cost, and in
which bank they reach, so each is marked in the operand and none is chosen by the
size of a number.

- A **direct-page** operand is unmarked: `LDA $12`. It is one byte, an offset from
  the direct register.
- An **absolute** operand carries `!`: `LDA !$0012`. It is two bytes, an offset in
  the data bank. The `!` is required.
- A **long** operand names its bank. A literal does so with the bank separator,
  `LDA $7E:1234`; a symbol or an expression does so behind `>`, `LDA >table`.
  It is three bytes.

`!$7E:1234` is an error: the bank separator already says the operand is long.

An absolute operand takes a 16-bit value, or a 24-bit value in the instruction's
own bank, which is taken as its offset — the value a label in the same file
has, so `JMP !loop` and `JSR !sub_0180E8` write the offset of a line the file
defines. A 24-bit value in any other bank does not fit and is reported, never
truncated to its low half.

### 2.2 Immediates and the register widths

An immediate operand is as wide as the register it loads. The accumulator's
width sizes `ADC`, `AND`, `BIT`, `CMP`, `EOR`, `LDA`, `ORA` and `SBC`; the index
registers' width sizes `CPX`, `CPY`, `LDX` and `LDY`. `REP`, `SEP`, `WDM` and the
signature byte of `BRK` and `COP` are always one byte.

The width comes from the mode the code runs under, never from the number's
digits: `LDA #$12` is two bytes under an 8-bit accumulator and three under a
16-bit one, and `LDA #$0012` is the same instruction either way.

The assembler follows the widths the way the hardware does, and the way the
disassembler does. `REP` and `SEP` move them by the bits in their mask; `XCE`
enters emulation mode, which forces both widths to eight, when the instruction
before it is `SEC`, and leaves it when that instruction is `CLC`; an `XCE` with
any other instruction before it keeps the mode. `PLP` and `RTI` load the status
byte from the stack, after which the widths are unknown until something says them.
An immediate assembled under an unknown width is an error naming the width it
needs.

The mask of a `REP` or `SEP` decides the width of everything after it, so it
must be resolvable when the line is read: a name defined later in the file is
an error there.

A **region** begins at the start of a file, at every `ORG`, and after every
data directive (`DB`, `DW`, `DL`, `DS`). A region begins in native mode with
both widths unknown and nothing remembered about the carry, until a directive
(§3) or a `REP`/`SEP` settles a width. Emulation mode has to be said, because
the instructions cannot say it and it decides whether `REP` and `SEP` move a
width at all: in emulation mode they move nothing.

### 2.3 Branch targets are addresses, not displacements

A relative branch takes the address it goes to. The assembler computes the
displacement from the end of the instruction and reports a target out of reach —
further than −128 or +127 for the eight conditional branches and `BRA`, further
than −32768 or +32767 for `BRL` — rather than silently truncating it. A branch
stays within its bank: the target is the same bank as the branch, and the
displacement wraps the offset within it.

```
loop:   LDA !$2140
        BNE loop        ; not BNE -5
```

### 2.4 The jumps and calls

| Instruction | Operand | Bytes | Lands in |
|---|---|---|---|
| `JMP !abs` | absolute | 3 | the program bank |
| `JMP (!abs)` | a pointer in bank zero | 3 | the program bank |
| `JMP (!abs,X)` | a pointer in the program bank | 3 | the program bank |
| `JML long` | absolute long | 4 | the bank named |
| `JML [!abs]` | a three-byte pointer in bank zero | 3 | the bank the pointer names |
| `JSR !abs` | absolute | 3 | the program bank |
| `JSR (!abs,X)` | a pointer in the program bank | 3 | the program bank |
| `JSL long` | absolute long | 4 | the bank named |
| `BRL rel` | a 16-bit displacement | 3 | the program bank |

### 2.5 Block moves

`MVN $00,$7E` moves from bank `$00` to bank `$7E` — source first, then
destination. The encoding stores the **destination** first: the bytes are
`54 7E 00`. `MVP` is the same shape. Text order and byte order differ, and the
assembler and disassembler agree on both.

### 2.6 The stack forms and the signatures

```
        PEA $1234       ; a 16-bit value, pushed as it is
        PEI ($12)       ; the word the direct page holds at $12
        PER label       ; the address, pushed; encoded as a displacement
        BRK #$12        ; the signature byte after the opcode
        COP #$12
        WDM #$12        ; the reserved opcode's operand byte
```

`PEA` takes a 16-bit value with no marker, since it has one form. `PER` takes the
address it names, like a branch, and the assembler computes the displacement.
`BRK` and `COP` are two-byte instructions; the byte after the opcode is written as
an immediate so that it is never lost.

---

## 3. Directives

### 3.1 `A8`, `A16`, `X8`, `X16`

```
        A8              ; the accumulator is eight bits wide from here
        X16             ; the index registers are sixteen
```

Each sets what the assembler knows about a register width from that line on, and
is what settles a width the instructions cannot: at the start of a region, and
after a `PLP` or `RTI`. A `REP` or `SEP` after it moves the width as the hardware
would, so a directive is not repeated after them. `A16` or `X16` in emulation
mode is an error, since both widths are eight there until `XCE` leaves it. A
directive takes no operand.

The disassembler emits these at the start of every region and wherever the trace
read an instruction under a width the instruction above did not leave — nowhere
else, because a `REP` or `SEP` already says its own change.

### 3.2 `EMULATION`, `NATIVE`

```
        EMULATION       ; the chip is in emulation mode from here
        NATIVE          ; native mode from here, the widths as they were
```

`EMULATION` says the code from that line on runs in emulation mode: both widths
are eight and known, `REP` and `SEP` move neither, and an `XCE` after `CLC`
leaves it. `NATIVE` returns to native mode and keeps the widths where emulation
held them. A region begins native, so `NATIVE` is only ever needed after an
`EMULATION` in the same region.

The disassembler emits `EMULATION` at the start of every region the trace read
in emulation mode, with no width directive under it, and wherever the
instruction above left the chip native and this one reads in emulation mode;
`NATIVE` where the instruction above left emulation mode and this one reads
native. A reset handler's file therefore begins `EMULATION`, and its `CLC` /
`XCE` says the rest.

---

## See also

- [Assembly language: the common layer](assembly-lexicon.md) — source format,
  numbers, symbols, directives, round-trip, diagnostics and stability.
- [The assemblers](assemblers.md) — `cpu65816_asm`, the tool that reads this
  language.
- [65816 disassembler](65816-disassembler.md) — the tool that emits this language.
- [65816 CPU core](65816-cpu.md) — the instruction set itself: the widths, the
  modes, and the cycles.
- [SPC700 assembly language](spc700-assembly.md) — the audio CPU's dialect over
  the same layer.
