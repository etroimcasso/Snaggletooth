#pragma once

// The interpreter held to one step of the machine.
//
// The machine is the oracle and the interpreter owns no memory. An observer
// collects what the CPU did in one step — the bytes it fetched, the data
// accesses it made, the cycles it spent — and the interpreter then runs the
// node for that instruction through a bus that answers each read with the
// value the machine read and checks its address, and checks every write's
// address, value and order against the machine's. After the instruction the
// registers and flags are checked, and the cycles the interpreter reports are
// checked against the cycles the observer counted. The interpreter cannot copy
// a write through, because it never sees one; it has to compute every value
// from the effects.
//
// Two hosts drive this: the differential, which runs a tree's lifted program
// beside a recorded run, and the observed run, which lifts every instruction
// the CPU executes from the bytes it fetched and holds that node to the same
// check. Both name a disagreement the same way.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "ir/ir.h"
#include "ir/ir_interpret.h"
#include "snaggletooth/snes/snes.h"

namespace snaggletooth::ir {

// One place the interpreter and the machine disagreed. `expected` is the
// machine's value, `actual` the interpreter's. `effect` names the effect whose
// access diverged, as an index into the node's effects or the interrupt
// sequence; a register or cycle divergence names none. `name` is the mnemonic,
// or `NMI` / `IRQ` for a hardware sequence. `site` is the address the tree
// places the instruction at.
struct Divergence {
  std::uint64_t instruction = 0;  // the ordinal of the step in the run, from zero
  Address site = 0;
  std::string name;
  Mode mode;
  std::optional<std::size_t> effect;
  std::string what;
  std::uint32_t expected = 0;
  std::uint32_t actual = 0;
};

// The observer that collects one step's report: the bytes the CPU fetched, in
// order — the instruction's own bytes, wherever they lay — its data accesses,
// and its cycles, counted. A transfer engine's accesses and the work-RAM port's
// are not the CPU's and are left out.
struct StepObserver final : BusObserver {
  std::vector<BusAccess> fetches;  // the opcode fetch and every operand fetch
  std::vector<BusAccess> data;     // the CPU's data accesses, fetches left out
  std::uint32_t cpuCycles = 0;
  bool cpuRan = false;

  void access(const BusAccess& access) override;
  void internal(std::uint32_t address, std::optional<CycleKind> kind) override;
  void clear();
};

// The interpreter's view of a core state.
[[nodiscard]] Registers registersOf(const Cpu65816State& state) noexcept;

// Runs `node` on the interpreter over the step's data accesses, then checks
// the registers after against `after` and the cycles against the observer's
// count. Every disagreement lands in `out` carrying `prototype`'s step and
// site, with the node's name and mode filled in. When anything diverged the
// interpreter is realigned to `after`, so the host goes on from the machine's
// truth rather than compounding one disagreement into every step after it.
// Returns the cycles the node cost the interpreter.
std::uint32_t checkNode(Interpreter& interpreter, const Node& node, const StepObserver& step,
                        const Cpu65816State& after, Divergence prototype,
                        std::vector<Divergence>& out);

// The same for a hardware interrupt sequence — a program's `nmi` or `irq` —
// which the machine took at the boundary instead of the instruction at the
// program counter. `prototype.name` says which.
std::uint32_t checkInterrupt(Interpreter& interpreter, const std::vector<Effect>& sequence,
                             const StepObserver& step, const Cpu65816State& after,
                             Divergence prototype, std::vector<Divergence>& out);

}  // namespace snaggletooth::ir
