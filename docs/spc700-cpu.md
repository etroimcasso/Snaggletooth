# The SPC700 CPU core

The SPC700 is the SNES's audio CPU — the processor inside the sound unit that runs a game's
sound-driver program. This is its instruction-set core: an interpreter for all 256 opcodes, running
over an abstract bus so it executes the same way on plain RAM as it does on the full APU.

The core is CPU-only. It has no memory of its own, no timers, and no DSP — it reads and writes an
address space you supply. Give it flat RAM and every address is a plain byte; give it the APU's
register-overlaid memory and the same instructions drive real hardware.

## The bus

The core talks to memory through any type that satisfies the `ApuBus` concept — an 8-bit read and
an 8-bit write over the 16-bit address space:

```cpp
template <typename B>
concept ApuBus = requires(B bus, std::uint16_t address, std::uint8_t value) {
  { bus.read(address) } -> std::same_as<std::uint8_t>;
  bus.write(address, value);
};
```

A flat 64KB array is the simplest conforming bus:

```cpp
#include "snaggletooth/apu/spc700.h"

#include <array>
#include <cstdint>

struct FlatRam {
  std::array<std::uint8_t, 65536> bytes{};
  std::uint8_t read(std::uint16_t address) const { return bytes[address]; }
  void write(std::uint16_t address, std::uint8_t value) { bytes[address] = value; }
};
```

The core issues *every* documented memory access, including the dummy reads some instructions
perform and discard. On flat RAM a dummy read has no consequence; on a bus whose reads have side
effects — the APU's register overlay, where reading a timer output clears it — issuing them is
what makes the core correct.

Some cycles reach memory not at all. The bus hears nothing on those: there is no third call to
implement, because an internal cycle is a cycle the chip spends without driving an access.

## State

The whole CPU is one value struct. Snapshot it by copying, restore it by assignment:

```cpp
struct Spc700State {
  std::uint16_t pc = 0;
  std::uint8_t a = 0;
  std::uint8_t x = 0;
  std::uint8_t y = 0;
  std::uint8_t sp = 0;   // low byte of the stack pointer; the stack lives in page $01
  std::uint8_t psw = 0;  // packed program status word
  RunState run = RunState::Running;

  std::uint8_t ir = 0;    // the instruction being executed
  std::uint8_t tcu = 0;   // cycle index within it; 0 means the next cycle fetches
  std::uint16_t ea = 0;   // effective-address scratch
  std::uint16_t ptr = 0;  // pointer / second-address scratch
  std::uint16_t tmp = 0;  // data scratch
};
```

The five fields after `run` are how far into an instruction the core is. They carry no
architectural meaning — no program can read them — but they are part of the value, which is what
makes a snapshot legal *between two cycles of one instruction* rather than only between
instructions.

`psw` is the packed status byte. Named masks read and write its flags:

| Mask | Bit | Flag |
|---|---|---|
| `kFlagN` | `0x80` | negative (high bit of the result) |
| `kFlagV` | `0x40` | signed overflow |
| `kFlagP` | `0x20` | direct page — moves the direct page to `$0100` |
| `kFlagB` | `0x10` | break |
| `kFlagH` | `0x08` | half-carry (carry across the nibble boundary) |
| `kFlagI` | `0x04` | interrupt enable |
| `kFlagZ` | `0x02` | zero |
| `kFlagC` | `0x01` | carry |

`RunState` is `Running`, `Sleeping`, or `Stopped` — the running state that SLEEP and STOP set.

Two notes on flags specific to this CPU. The `P` flag relocates the direct page: with it clear,
`dp` addressing reaches `$0000+dp`; with it set, `$0100+dp`. And `I` is an *enable* (the opcode EI
sets it, DI clears it), the opposite sense of the 6502's disable — but the audio unit has no
interrupt source, so nothing is ever delivered; the flag, EI/DI, and RETI's flag restore are all
present without an interrupt to act on.

## Running instructions

Construct the CPU from a starting state and step it. `stepInstruction()` runs to the next
instruction boundary and returns how many cycles that took, in the SPC700's own 1.024 MHz cycles:

```cpp
FlatRam ram;
ram.bytes[0x0200] = 0xE8;  // MOV A,#$2A
ram.bytes[0x0201] = 0x2A;

snaggletooth::Spc700 cpu(snaggletooth::Spc700State{.pc = 0x0200});
std::uint32_t cycles = cpu.stepInstruction(ram);

// cpu.state().a  == 0x2A
// cpu.state().pc == 0x0202
// cycles         == 2
```

The full surface:

```cpp
class Spc700 {
 public:
  Spc700() = default;
  explicit Spc700(Spc700State state);

  const Spc700State& state() const noexcept;   // read the current state
  void restore(Spc700State state) noexcept;    // replace it wholesale

  template <ApuBus B>
  void stepCycle(B& bus);                      // execute one chip cycle

  template <ApuBus B>
  std::uint32_t stepInstruction(B& bus);       // run to the next boundary

  template <ApuBus B>
  std::uint32_t step(B& bus);                  // the same call

  bool atInstructionBoundary() const noexcept; // between instructions?
};
```

Time is external: the caller decides how much to run by accumulating cycles against whatever budget
it keeps. The core holds no wall clock and starts no threads.

`restore()` and the state copy are how you replay: capture a state, run, and restore to run again
from the same point — the interpreter is a pure function of `(state, bus)`.

### One cycle at a time

`stepCycle()` runs a single chip cycle. A machine that has to interleave with the CPU part-way
through an instruction — advancing timers in step with execution, landing a register write on the
exact cycle it happens — drives the core this way:

```cpp
FlatRam ram;
ram.bytes[0x0200] = 0xE4;  // MOV A,$10
ram.bytes[0x0201] = 0x10;
ram.bytes[0x0010] = 0x7E;

snaggletooth::Spc700 cpu(snaggletooth::Spc700State{.pc = 0x0200});
cpu.stepCycle(ram);   // the opcode fetch
cpu.stepCycle(ram);   // the offset byte
// cpu.atInstructionBoundary() == false — the instruction is half-run
// cpu.state().a               == 0x00  — the load has not settled yet
cpu.stepCycle(ram);   // the operand, and the load
// cpu.atInstructionBoundary() == true
// cpu.state().a               == 0x7E
```

`atInstructionBoundary()` reports whether the core sits between instructions. Calling
`stepInstruction()` part-way through one finishes that instruction rather than starting another, so
the two calls mix freely.

A register settles on the instruction's **last** cycle, never part-way through it. The vectors carry
no per-cycle register readings, so where inside an instruction a register changes is unobservable;
placing every write at the end is what makes a mid-instruction snapshot mean one definite thing.

### Halting

SLEEP and STOP set `run` to `Sleeping` or `Stopped` and stop advancing. A `stepCycle()` on a core
that is not `Running` touches neither the state nor the bus, and `stepInstruction()` returns 2
cycles having done nothing — the machine that owns the clock keeps time passing (timers still tick
on delivered cycles) while the CPU sits idle. Move the core back to `Running` through `restore()`
to resume it. The instructions themselves are seven cycles long; [The control flow](#the-control-flow)
has their shape.

## The cycle bar

All 256 opcodes run a cycle at a time. Every cycle is placed — which one reads, which writes, which
reaches memory not at all, and what address each drives — and every one is checked against a
per-cycle recording of the real chip. There is no second path: `stepInstruction()` is the cycles the
instruction is made of, run to the next boundary.

## The moves

Every move settles an address, then reaches it. Where the cycles go depends on the addressing mode,
and three laws cut across all of them.

**The byte after the opcode is always read.** A one-byte instruction has no operand there, but the
cycle still happens and the read is real — it is only the program counter that does not step over
it:

```cpp
FlatRam ram;
ram.bytes[0x0200] = 0x7D;  // MOV A,X

snaggletooth::Spc700 cpu(snaggletooth::Spc700State{.pc = 0x0200, .x = 0x6C});
cpu.stepInstruction(ram);
// two cycles: the opcode at $0200, then a read of $0201 whose byte is thrown away
// cpu.state().pc == 0x0201 — the discarded byte is not an operand
```

**An indexed mode spends its cycle before the access.** `MOV A,$30+X` fetches its offset, spends a
cycle adding X — reaching memory not at all — and only then reads. The same shape covers `dp+Y`,
`!abs+X` and `!abs+Y`.

**A store reads its destination before writing it.** `MOV $10,A` reads `$10`, discards the byte,
and writes. That read is why a store to a timer output register clears it.

### Where the cycles go

| Instruction | Cycles |
|---|---|
| `MOV A,#imm` | opcode · the immediate |
| `MOV A,X` | opcode · discarded read |
| `MOV A,dp` | opcode · offset · read |
| `MOV A,dp+X` | opcode · offset · index · read |
| `MOV A,!abs` | opcode · address low · address high · read |
| `MOV A,!abs+X` | opcode · address low · address high · index · read |
| `MOV A,(X)` | opcode · discarded read · read |
| `MOV A,(X)+` | opcode · discarded read · read · increment |
| `MOV A,[dp+X]` | opcode · offset · index · pointer low · pointer high · read |
| `MOV A,[dp]+Y` | opcode · offset · index · pointer low · pointer high · read |
| `MOV dp,A` | opcode · offset · destination read · write |
| `MOV dp+X,A` | opcode · offset · index · destination read · write |
| `MOV !abs,A` | opcode · address low · address high · destination read · write |
| `MOV (X),A` | opcode · discarded read · destination read · write |
| `MOV (X)+,A` | opcode · discarded read · increment · write |
| `MOV [dp+X],A` | opcode · offset · index · pointer low · pointer high · destination read · write |
| `MOV [dp]+Y,A` | opcode · offset · pointer low · pointer high · index · destination read · write |
| `MOV dp,dp` | opcode · source offset · source read · destination offset · write |
| `MOV dp,#imm` | opcode · immediate · destination offset · destination read · write |

The *index* and *increment* rows are the cycles that reach memory not at all.

### The store that reads nothing

`MOV (X)+,A` is the exception to the destination read. Where every other store reads its
destination first, the auto-incrementing form spends that cycle without touching the bus:

```cpp
FlatRam ram;
ram.bytes[0x0200] = 0xAF;  // MOV (X)+,A

snaggletooth::Spc700 cpu(
    snaggletooth::Spc700State{.pc = 0x0200, .a = 0x11, .x = 0x20});
cpu.stepInstruction(ram);
// four cycles: opcode, discarded read, a cycle reaching nothing, the write to $0020
// ram.bytes[0x0020] == 0x11
// cpu.state().x     == 0x21 — X steps with the write
```

It matters on a bus with read side effects: a store through `(X)+` to a register that clears when
read leaves that register alone, where the same store through `(X)` would clear it.

### Where an indirect mode spends its cycle

`[dp+X]` spends its cycle before reading the pointer, whichever direction it goes. `[dp]+Y` does
the same when it reads — but when it *writes*, it reads the pointer first and spends the cycle
after it:

| Instruction | Order |
|---|---|
| `MOV A,[dp+X]` | offset · **index** · pointer · data |
| `MOV [dp+X],A` | offset · **index** · pointer · destination read · write |
| `MOV A,[dp]+Y` | offset · **index** · pointer · data |
| `MOV [dp]+Y,A` | offset · pointer · **index** · destination read · write |

Both bytes of a pointer live in the direct page, and the second wraps inside it: a pointer at `$FF`
reads its high byte from the page base, not from the page above.

### The two-operand moves

`MOV dp,dp` and `MOV dp,#imm` both carry two bytes and both end in a write, but they read different
addresses. `MOV $FF,$00` reads its **source** and never its destination — so it does not clear a
timer output at `$FF`. `MOV $FF,#$00` has its byte already and reads the **destination** — so it
does.

## The arithmetic

Arithmetic (ADC, SBC, CMP), logic (AND, OR, EOR), increment and decrement, the shifts and
rotations, and the nibble exchange all run on the same cycle sequences the moves do. Three shapes
cover the family.

**A byte read into a register** runs exactly as the matching move does. `ADC A,$30+X` spends the
same cycles as `MOV A,$30+X` — offset, index, read — and differs only in what it does with the
byte. So every law in [The moves](#the-moves) carries over unchanged: the byte after the opcode is
always read, and an indexed mode spends its cycle *before* the access.

```cpp
FlatRam ram;
ram.bytes[0x0200] = 0x14;  // OR A,$10+X
ram.bytes[0x0201] = 0x10;
ram.bytes[0x0014] = 0x0F;

snaggletooth::Spc700 cpu(
    snaggletooth::Spc700State{.pc = 0x0200, .a = 0xF0, .x = 0x04});
cpu.stepInstruction(ram);
// four cycles: opcode, offset, a cycle reaching nothing, the read of $0014
// cpu.state().a == 0xFF
```

**A byte changed in place** is the read-modify-write seat: the byte is read, and the result goes
back to the same address on the next cycle. `INC`, `DEC`, `ASL`, `LSR`, `ROL` and `ROR` all run it
against `dp`, `dp+X` and `!abs`.

```cpp
FlatRam ram;
ram.bytes[0x0200] = 0xAB;  // INC $10
ram.bytes[0x0201] = 0x10;
ram.bytes[0x0010] = 0x41;

snaggletooth::Spc700 cpu(snaggletooth::Spc700State{.pc = 0x0200});
cpu.stepInstruction(ram);
// four cycles: opcode, offset, the read of $0010, the write back to $0010
// ram.bytes[0x0010] == 0x42
```

The result settles on the cycle that writes it, and so do the flags. Step that instruction three
cycles instead and the byte in memory is still the one that was read.

**Two bytes in memory** covers the three two-operand forms. Each takes its source from one place
and both its other operand and its target from another:

| Instruction | Source | Target |
|---|---|---|
| `ADC (X),(Y)` | the byte at `dp+Y` | the byte at `dp+X` |
| `ADC dp,dp` | the byte named by the **first** offset | the byte named by the second |
| `ADC dp,#imm` | the immediate byte | the byte named by the offset |

The source is always reached first. `ADC (X),(Y)` reads the Y side, then the X side, then writes
the X side; `ADC dp,dp` reads its source byte as soon as the offset naming it has arrived, before
it has even fetched the destination offset.

### The comparison that writes nothing

`CMP` discards its result. Where its arithmetic siblings write, it reaches memory not at all — but
the cycle is still spent, so the two forms take exactly the same time:

| Instruction | Cycles |
|---|---|
| `OR dp,dp` | opcode · source offset · source read · destination offset · destination read · **write** |
| `CMP dp,dp` | opcode · source offset · source read · destination offset · destination read · **nothing** |

The same pairing holds for `CMP dp,#imm` against `OR dp,#imm`, and for `CMP (X),(Y)` against
`OR (X),(Y)`. This is the only place in the family where a final cycle reaches memory not at all.

`CMP` against a register — `CMP A,dp`, `CMP X,#imm`, `CMP Y,!abs` — is a plain read, not one of
these forms.

### Where the cycles go

| Instruction | Cycles |
|---|---|
| `ADC A,#imm` | opcode · the immediate |
| `INC A` | opcode · discarded read |
| `XCN A` | opcode · discarded read · three cycles inside the chip |
| `ADC A,dp` | opcode · offset · read |
| `ADC A,dp+X` | opcode · offset · index · read |
| `ADC A,!abs` | opcode · address low · address high · read |
| `ADC A,!abs+X` | opcode · address low · address high · index · read |
| `ADC A,(X)` | opcode · discarded read · read |
| `ADC A,[dp+X]` | opcode · offset · index · pointer low · pointer high · read |
| `ADC A,[dp]+Y` | opcode · offset · index · pointer low · pointer high · read |
| `INC dp` | opcode · offset · read · write |
| `INC dp+X` | opcode · offset · index · read · write |
| `INC !abs` | opcode · address low · address high · read · write |
| `ADC (X),(Y)` | opcode · discarded read · source read · target read · write |
| `ADC dp,dp` | opcode · source offset · source read · destination offset · destination read · write |
| `ADC dp,#imm` | opcode · immediate · destination offset · destination read · write |
| `CMP (X),(Y)` | opcode · discarded read · source read · target read · nothing |
| `CMP dp,dp` | opcode · source offset · source read · destination offset · destination read · nothing |
| `CMP dp,#imm` | opcode · immediate · destination offset · destination read · nothing |

The *index* rows, the three cycles of `XCN`, and the final row of each comparison are the cycles
that reach memory not at all.

`XCN` is the one instruction here that spends more than one cycle inside the chip: it reads the
opcode, reads and discards the byte after it, then swaps the nibbles of A over three more cycles.

## The words and the bits

Four groups run on shapes of their own: the 16-bit instructions over a direct-page word, the
multiply and divide, the decimal adjusts, and the instructions that reach a single bit.

### A word is two direct-page bytes

One offset names both bytes: the low byte at `dp`, the high byte one past it — and that second
address **wraps inside the direct page**, so a word at `$FF` takes its high byte from the page base.

| Instruction | Cycles |
|---|---|
| `MOVW YA,dp` | opcode · offset · low byte · a cycle inside the chip · high byte |
| `ADDW YA,dp` | opcode · offset · low byte · a cycle inside the chip · high byte |
| `SUBW YA,dp` | opcode · offset · low byte · a cycle inside the chip · high byte |
| `CMPW YA,dp` | opcode · offset · low byte · high byte |
| `INCW dp` | opcode · offset · low byte · write low · high byte · write high |
| `DECW dp` | opcode · offset · low byte · write low · high byte · write high |
| `MOVW dp,YA` | opcode · offset · destination read · write low · write high |

`CMPW` is the one word instruction with no cycle inside the chip, and it is a cycle shorter than
`ADDW` and `SUBW` for it — it discards its result, so it has nothing to settle.

**`INCW` and `DECW` interleave.** Neither reads the whole word and then writes it back: each half is
written before the next is read.

```cpp
FlatRam ram;
ram.bytes[0x0200] = 0x3A;  // INCW $10
ram.bytes[0x0201] = 0x10;
ram.bytes[0x0010] = 0xFF;  // the word $01FF
ram.bytes[0x0011] = 0x01;

snaggletooth::Spc700 cpu(snaggletooth::Spc700State{.pc = 0x0200});
cpu.stepInstruction(ram);
// six cycles: opcode, offset, read $0010, write $0010, read $0011, write $0011
// ram.bytes[0x0010] == 0x00  — the low byte wrapped
// ram.bytes[0x0011] == 0x02  — so the high byte stepped with it
```

The high byte moves only when the low one wrapped past its own end, and `N` and `Z` come from the
whole 16-bit result, not from either half.

**`MOVW dp,YA` reads only its low byte.** It reads the byte it is about to overwrite — once, at the
low address — and then writes both halves. On a bus with read side effects that matters: the low
address is reached, the high one only written.

### One bit at a time

`SET1` and `CLR1` name a direct-page byte and a bit in the opcode itself, and run the
read-modify-write seat: `opcode · offset · read · write`, with no flag touched.

`TSET1` and `TCLR1` reach an absolute byte and **read it twice** — the fifth of their six cycles is a
second read of the same address, not a cycle inside the chip. Both reads are real, so a register
that clears when read is reached twice. They set `N` and `Z` from `A - memory` and write `A`'s bits
into the byte (`TSET1` sets them, `TCLR1` clears them); `A` itself is untouched.

The carry-bit instructions take a 16-bit operand that packs two things: the **address in its low 13
bits**, and the **bit index in the three above them**.

```cpp
FlatRam ram;
ram.bytes[0x0200] = 0xAA;  // MOV1 C,$0300.3
ram.bytes[0x0201] = 0x00;  // the operand is $6300:
ram.bytes[0x0202] = 0x63;  //   $0300 is the address, 3 the bit index
ram.bytes[0x0300] = 0x08;

snaggletooth::Spc700 cpu(snaggletooth::Spc700State{.pc = 0x0200});
cpu.stepInstruction(ram);
// four cycles: opcode, operand low, operand high, the read of $0300
// cpu.state().psw & snaggletooth::kFlagC — set, because bit 3 of $08 is set
```

| Instruction | Cycles |
|---|---|
| `AND1 C,m.b` / `AND1 C,/m.b` | opcode · operand low · operand high · read |
| `MOV1 C,m.b` | opcode · operand low · operand high · read |
| `OR1 C,m.b` / `OR1 C,/m.b` | opcode · operand low · operand high · read · a cycle inside the chip |
| `EOR1 C,m.b` | opcode · operand low · operand high · read · a cycle inside the chip |
| `NOT1 m.b` | opcode · operand low · operand high · read · write |
| `MOV1 m.b,C` | opcode · operand low · operand high · read · a cycle inside the chip · write |

`AND1` and `MOV1 C,m.b` settle as the byte arrives; `OR1` and `EOR1` pay one more cycle inside the
chip after it. Only `NOT1` and `MOV1 m.b,C` write, and only the write form of `MOV1` spends a cycle
between the read and the write.

### Multiply, divide, and the decimal adjusts

`MUL YA` and `DIV YA,X` reach memory exactly twice — the opcode, and the byte after it that every
one-byte instruction reads and discards — and spend every remaining cycle computing inside the
chip. They are the two longest instructions the CPU has:

| Instruction | Cycles |
|---|---|
| `MUL YA` | opcode · discarded read · seven cycles inside the chip |
| `DIV YA,X` | opcode · discarded read · ten cycles inside the chip |
| `DAA A` / `DAS A` | opcode · discarded read · one cycle inside the chip |

`DIV` past a quotient of 511 produces what the hardware produces: the documented nine-iteration
restoring division runs to the end, sets `V` from the overflow bit, and leaves the garbage that
falls out. `H` is the nibble comparison `X & $F <= Y & $F` on the values the instruction started
with.

## The control flow

The branches, the jumps, the calls and returns, the stack transfers, the flag operations and the
halts all move — or decline to move — the program counter. Two laws run through the whole family.

**A branch's condition is settled before the cycles it costs.** The cycle that reads what the branch
tests decides it; the cycles that follow are spent, or not, on that decision.

**The program counter moves on the last cycle**, like every other register. That is why a call
writes its return address to the stack *before* its destination reaches the core, and why every
address in a stack instruction is measured from where `sp` began rather than from where it ends up.

### The branches

A relative branch is two cycles when its condition fails: the opcode, and the displacement. When it
holds, two more cycles pass inside the chip and the program counter moves at the end of them. The
displacement counts from past the whole instruction.

```cpp
FlatRam ram;
ram.bytes[0x0200] = 0x2F;  // BRA +5
ram.bytes[0x0201] = 0x05;

snaggletooth::Spc700 cpu(snaggletooth::Spc700State{.pc = 0x0200});
std::uint32_t cycles = cpu.stepInstruction(ram);
// four cycles: opcode, displacement, and two reaching memory not at all
// cpu.state().pc == 0x0207 — $0202, past the instruction, plus 5
```

`BRA` is the one that always takes them. `BEQ`, `BNE`, `BCS`, `BCC`, `BVS`, `BVC`, `BMI` and `BPL`
test one status bit each.

The other four branches read something first, and their displacement is the byte *after* that
operand:

| Instruction | Cycles | Not taken / taken |
|---|---|---|
| `BBS dp.b,rel` / `BBC dp.b,rel` | opcode · offset · read · a cycle inside the chip · displacement | 5 / 7 |
| `CBNE dp,rel` | opcode · offset · read · a cycle inside the chip · displacement | 5 / 7 |
| `CBNE dp+X,rel` | opcode · offset · index · read · a cycle inside the chip · displacement | 6 / 8 |
| `DBNZ dp,rel` | opcode · offset · read · **write** · displacement | 5 / 7 |
| `DBNZ Y,rel` | opcode · displacement · a cycle inside the chip · displacement again | 4 / 6 |

Two of those rows are worth reading twice. **`DBNZ dp` writes the decremented byte back before it
reads the displacement**, so the store lands whether or not the branch is taken. And **`DBNZ Y` has
only one operand byte and reads it twice** — once as the displacement, and again after the cycle the
decrement takes. Neither `CBNE` nor `DBNZ` sets a flag: the comparison and the decrement both settle
inside the chip.

### The jumps

`JMP !abs` is three cycles and spends none of them inside the chip — the operand is the destination.

`JMP [!abs+X]` adds X to the operand and reads a pointer there, and **that pointer's two bytes are
one linear byte apart**. It is the only address in the core that does not wrap inside a page: a
pointer at `$02FF` takes its high byte from `$0300`, where a direct-page word or an indirect mode's
pointer would wrap to the page base.

| Instruction | Cycles |
|---|---|
| `JMP !abs` | opcode · address low · address high |
| `JMP [!abs+X]` | opcode · address low · address high · index · pointer low · pointer high |

### The calls and the returns

A call writes the return address high byte first, so the low byte lands at the lower address and is
the first one read back. `sp` names the byte the first write reaches, and settles two (or, for
`BRK`, three) entries down at the end of the instruction.

| Instruction | Cycles |
|---|---|
| `CALL !abs` | opcode · address low · address high · internal · push high · push low · internal · internal |
| `PCALL up` | opcode · offset · internal · push high · push low · internal |
| `TCALL n` | opcode · discarded read · internal · push high · push low · internal · vector low · vector high |
| `BRK` | opcode · discarded read · push high · push low · push status · internal · vector low · vector high |
| `RET` | opcode · discarded read · internal · pull low · pull high |
| `RETI` | opcode · discarded read · internal · pull status · pull low · pull high |

`PCALL`'s destination is `$FF00` plus its one operand byte. `TCALL n` takes its destination from the
table that ends at `$FFDE` and counts downwards two bytes per entry, so `TCALL 0` reads `$FFDE` and
`$FFDF` and `TCALL 15` reads `$FFC0` and `$FFC1`. `BRK` reads the same entry `TCALL 0` does — and
differs in three ways: it pushes the status byte under the return address, it begins its pushes
immediately where `TCALL` spends a cycle inside the chip first, and it sets `B` and clears `I` at
the end, after the byte it pushed was already the old one.

```cpp
FlatRam ram;
ram.bytes[0x0200] = 0x3F;  // CALL !$0300
ram.bytes[0x0201] = 0x00;
ram.bytes[0x0202] = 0x03;

snaggletooth::Spc700 cpu(snaggletooth::Spc700State{.pc = 0x0200, .sp = 0xEF});
cpu.stepInstruction(ram);
// ram.bytes[0x01EF] == 0x02  — the return address, high byte at the higher address
// ram.bytes[0x01EE] == 0x03
// cpu.state().pc == 0x0300
// cpu.state().sp == 0xED
```

### The stack transfers

`PUSH` and `POP` are mirror images of each other, and both are four cycles: the push writes on the
third and spends the fourth inside the chip, the pop spends the third inside the chip and reads on
the fourth.

| Instruction | Cycles |
|---|---|
| `PUSH A` / `PUSH X` / `PUSH Y` / `PUSH PSW` | opcode · discarded read · write at `$0100+sp` · internal |
| `POP A` / `POP X` / `POP Y` / `POP PSW` | opcode · discarded read · internal · read at `$0100+sp+1` |

Popping a register sets no flag. `POP PSW` replaces the whole status byte, the flags it carries
included.

### The flag operations and the halts

`CLRC`, `SETC`, `CLRP`, `SETP`, `CLRV` and `NOP` are two cycles — the opcode and the discarded read.
`NOTC`, `EI` and `DI` spend one further cycle inside the chip and take three. `CLRV` clears the
half-carry along with the overflow.

`SLEEP` and `STOP` are seven cycles with a shape of their own: the byte after the opcode is read
three times over, each read followed by a cycle inside the chip. The core halts as the last of them
ends — on an instruction boundary, with the program counter left on the byte it kept reading.

```cpp
FlatRam ram;
ram.bytes[0x0200] = 0xEF;  // SLEEP

snaggletooth::Spc700 cpu(snaggletooth::Spc700State{.pc = 0x0200});
std::uint32_t cycles = cpu.stepInstruction(ram);
// cycles == 7
// cpu.state().run == snaggletooth::RunState::Sleeping
// cpu.state().pc  == 0x0201 — the byte is read, never stepped over
```

## Testing against the vectors

The core is checked per opcode against the SingleStepTests SPC700 vectors — one file per opcode,
each case a before state, an after state, and a recording of every cycle the instruction took.

Every case runs one cycle at a time, and each cycle is compared against the recording: what the
cycle did (read, write, or nothing at all), the address it drove, and the byte that moved. A field
the recording leaves null is not asserted — the byte a discarded read moved was never captured. The
suite then demands that the core landed on an instruction boundary, the exact final registers, and
the exact final RAM (a full 64KB compare, so a stray write cannot hide).

The vectors are large, machine-generated reference data and are not vendored. Point the build at a
local checkout of the SingleStepTests SPC700 `v1` directory:

```
cmake -B build -DSNAGGLETOOTH_SPC700_VECTORS=/path/to/spc700/v1
cmake --build build
ctest --test-dir build
```

Without that path the vector cases register but skip, naming the variable in the skip reason — the
rest of the suite (the hand-derived flag, algorithm and cycle-shape cross-checks) still runs. Two
environment variables shape a run:

- `SNAGGLETOOTH_SPC700_CASE_CAP=N` — run at most `N` cases per opcode for a fast dev loop; the cap
  prints what it truncated. Leave it unset for a full run.
- `SNAGGLETOOTH_REQUIRE_VECTORS=1` — turn a missing vector set into a failure instead of a skip, so
  an environment that means to run the oracle can never report green while exercising none of it.

## Gotchas

- **Reads can matter.** The core issues dummy reads on purpose. Keep them when you implement a bus
  with read side effects — dropping a "pointless" read is a correctness bug there, not an
  optimization.
- **A cycle can reach memory not at all.** Do not count bus calls to count cycles; ask the core
  instead, through the count `stepInstruction()` returns.
- **The stack is page $01.** `sp` is only the low byte; pushes and pops address `$0100 + sp` and
  wrap within that page.
- **The direct page can move.** Address `dp` with `P` set and you are reaching `$0100+dp`, not
  `$0000+dp`.
- **A word wraps inside the direct page.** The high byte of a word at `$FF` is at the page base, not
  in the page above. The same holds for the second byte of an indirect mode's pointer — and not for
  `JMP [!abs+X]`, whose pointer is the one that steps linearly.
- **A branch's displacement counts from past the instruction.** `BRA` at `$0200` with a displacement
  of 5 lands at `$0207`, not `$0205`.
- **A halted core still costs time.** Stepping it returns 2 cycles rather than 0, because the clock
  belongs to the machine around the CPU, not to the CPU.

## Where to look

- `include/snaggletooth/apu/spc700.h` — the whole core: the `ApuBus` concept, `Spc700State`, the
  flag masks, and the `Spc700` class with its cycle engine.
- `tests/spc700/cycle_engine_test.cpp` — the engine's own contract and the move shapes, pinned on
  hand-written programs.
- `tests/spc700/alu_cycles_test.cpp` — the arithmetic family's cycle placement: the indexing law,
  the read-modify-write seat, the order of the two-operand forms, and the comparison that writes
  nothing.
- `tests/spc700/word_bit_cycles_test.cpp` — the 16-bit and bit families' cycle placement: the word
  page-wrap and its interleave, the second read of `TSET1`, the bit operand's packing, and the
  instructions that run inside the chip.
- `tests/spc700/control_flow_cycles_test.cpp` — the control-flow family's cycle placement: what a
  taken branch costs, the order a call reaches the stack and its destination in, the linear pointer
  of `JMP [!abs+X]`, and the shape of a halt.
- `tests/spc700/` — the vector harness and runner, and the hand-derived flag/algorithm cross-checks.
