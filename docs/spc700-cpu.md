# The SPC700 CPU core

The SPC700 is the SNES's audio CPU — the processor inside the sound unit that runs a game's
sound-driver program. This is its instruction-set core: a cycle-counted interpreter for all 256
opcodes, running over an abstract bus so it executes the same way on plain RAM as it will on the
full APU once that lands.

The core is CPU-only. It has no memory of its own, no timers, and no DSP — it reads and writes an
address space you supply. Give it flat RAM and every address is a plain byte; give it the APU's
register-overlaid memory (a later component) and the same instructions drive real hardware.

## The bus

`step()` talks to memory through any type that satisfies the `ApuBus` concept — an 8-bit read and
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
};
```

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

Construct the CPU from a starting state and step it one instruction at a time. `step()` returns the
instruction's cycle count in the SPC700's own 1.024 MHz cycles:

```cpp
FlatRam ram;
ram.bytes[0x0200] = 0xE8;  // MOV A,#$2A
ram.bytes[0x0201] = 0x2A;

snaggletooth::Spc700 cpu(snaggletooth::Spc700State{.pc = 0x0200});
std::uint32_t cycles = cpu.step(ram);

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
  void restore(Spc700State state) noexcept;     // replace it wholesale

  template <ApuBus B>
  std::uint32_t step(B& bus);                    // execute one instruction
};
```

`step()` executes exactly one instruction and returns its cycle count. Time is external: the caller
decides how many instructions to run by accumulating the returned counts against whatever budget it
keeps. The core holds no wall clock and starts no threads.

`restore()` and the state copy are how you replay: capture a state, run, and restore to run again
from the same point — the interpreter is a pure function of `(state, bus)`.

### Halting

SLEEP and STOP set `run` to `Sleeping` or `Stopped` and stop advancing. A `step()` on a core that is
not `Running` returns 2 cycles and touches neither the state nor the bus — the machine that owns the
clock keeps time passing (timers still tick on delivered cycles) while the CPU sits idle. Move the
core back to `Running` through `restore()` to resume it.

## The cycle bar

Cycle counts match the documented per-instruction totals. What the core does *not* model is the
placement of individual bus accesses *within* an instruction — the sub-cycle order in which reads
and writes land. That ordering is contested even among hardware experts and does not affect a
sound driver's output, so the core commits to the total and leaves the intra-instruction sequence
unspecified.

## Testing against the vectors

The core is checked per opcode against the SingleStepTests SPC700 vectors — before/after
machine-state cases, one file per opcode. The suite asserts each case's final registers, its final
RAM (a full 64KB compare, so a stray write cannot hide), and the total cycle count.

The vectors are large, machine-generated reference data and are not vendored. Point the build at a
local checkout of the SingleStepTests SPC700 `v1` directory:

```
cmake -B build -DSNAGGLETOOTH_SPC700_VECTORS=/path/to/spc700/v1
cmake --build build
ctest --test-dir build
```

Without that path the vector cases register but skip, naming the variable in the skip reason — the
rest of the suite (the hand-derived flag and algorithm cross-checks) still runs. Two environment
variables shape a run:

- `SNAGGLETOOTH_SPC700_CASE_CAP=N` — run at most `N` cases per opcode for a fast dev loop; the cap
  prints what it truncated. Leave it unset for a full run.
- `SNAGGLETOOTH_REQUIRE_VECTORS=1` — turn a missing vector set into a failure instead of a skip, so
  an environment that means to run the oracle can never report green while exercising none of it.

## Gotchas

- **Reads can matter.** The core issues dummy reads on purpose. Keep them when you implement a bus
  with read side effects — dropping a "pointless" read is a correctness bug there, not an
  optimization.
- **The stack is page $01.** `sp` is only the low byte; pushes and pops address `$0100 + sp` and
  wrap within that page.
- **The direct page can move.** Address `dp` with `P` set and you are reaching `$0100+dp`, not
  `$0000+dp`.
- **A halted core still costs time.** Stepping it returns 2 cycles rather than 0, because the clock
  belongs to the machine around the CPU, not to the CPU.

## Where to look

- `include/snaggletooth/apu/spc700.h` — the whole core: the `ApuBus` concept, `Spc700State`, the
  flag masks, and the `Spc700` class with its `step()` dispatch.
- `tests/spc700/` — the vector harness and runner, and the hand-derived flag/algorithm cross-checks.
