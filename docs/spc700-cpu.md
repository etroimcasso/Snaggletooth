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

  static bool cycleStepped(std::uint8_t opcode) noexcept;  // runs a cycle at a time?
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
to resume it.

## The cycle bar

The move family runs a cycle at a time. Every one of its cycles is placed — which cycle reads,
which writes, which reaches memory not at all, and what address each drives — and every one is
checked against a per-cycle recording of the real chip.

The rest of the instruction set runs whole. Its cycle counts match the documented per-instruction
totals and it issues the documented accesses, but which cycle each access falls on is not modelled
yet. `cycleStepped()` reports which of the two paths an opcode is on:

```cpp
snaggletooth::Spc700::cycleStepped(0xE4);  // true  — MOV A,dp
snaggletooth::Spc700::cycleStepped(0x84);  // false — ADC A,dp
```

An opcode on the whole-instruction path cannot be driven with `stepCycle()`. The call that would
run its second cycle reaches memory not at all and the instruction makes no progress, so a
per-cycle run over one fails loudly instead of passing quietly. Run it with `stepInstruction()`.

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

## Testing against the vectors

The core is checked per opcode against the SingleStepTests SPC700 vectors — one file per opcode,
each case a before state, an after state, and a recording of every cycle the instruction took.

For an opcode the cycle engine carries, the suite runs the case one cycle at a time and compares
each cycle against the recording: what the cycle did (read, write, or nothing at all), the address
it drove, and the byte that moved. A field the recording leaves null is not asserted — the byte a
discarded read moved was never captured. It then demands that the core landed on an instruction
boundary, the exact final registers, and the exact final RAM (a full 64KB compare, so a stray write
cannot hide).

For an opcode on the whole-instruction path, the suite runs it whole and demands the same final
state plus the recorded cycle count.

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
- **A halted core still costs time.** Stepping it returns 2 cycles rather than 0, because the clock
  belongs to the machine around the CPU, not to the CPU.

## Where to look

- `include/snaggletooth/apu/spc700.h` — the whole core: the `ApuBus` concept, `Spc700State`, the
  flag masks, and the `Spc700` class with its cycle engine.
- `tests/spc700/cycle_engine_test.cpp` — the engine's own contract and the move shapes, pinned on
  hand-written programs.
- `tests/spc700/` — the vector harness and runner, and the hand-derived flag/algorithm cross-checks.
