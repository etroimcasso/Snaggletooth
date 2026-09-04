#pragma once

// The run beside the core — a recorded run replayed on the machine, one
// instruction at a time, with the interpreter running the same program from the
// same registers and held to everything the machine did.
//
// The machine is the oracle and the interpreter owns no memory. The machine's
// observer reports every access the CPU makes; the interpreter reads through a
// bus that answers each read with the value the machine read — an input, like an
// interrupt — and checks its address, and checks every write's address, value
// and order against the machine's. After each instruction the registers and
// flags are checked, and the cycle count the interpreter reports is checked
// against the CPU cycles the observer counted. The interpreter cannot copy a
// write through, because it never sees one; it has to compute every value from
// the effects, and a run with no divergence says the effects carry the
// instruction's whole meaning.
//
// Three things are inputs rather than nodes: a hardware interrupt the machine
// takes between two instructions, which the interpreter runs as the program's
// own sequence and is checked the same way; a wait an interrupt line releases;
// and a step the CPU spent held off the bus while a transfer ran, which is
// skipped. An instruction at an address the program has no node for is counted
// and the interpreter realigned to the machine, since there is nothing to run.

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ir/ir.h"
#include "ir/ir_interpret.h"
#include "rom/input_script.h"
#include "snaggletooth/snes/snes.h"

namespace snaggletooth::ir {

// What to replay: the cartridge, how much of the master clock to run, and the
// recorded run to present at the controller ports frame by frame, exactly as
// the cartridge disassembler presents it. The run ends early when the CPU
// stops, or once `divergenceLimit` divergences have been recorded.
struct Replay {
  std::span<const std::uint8_t> rom;
  std::uint64_t masterCycles = 0;
  disasm::InputScript input;
  std::size_t divergenceLimit = 16;
};

// One place the interpreter and the machine disagreed. `expected` is the
// machine's value, `actual` the interpreter's. `effect` names the effect whose
// access diverged, as an index into the node's effects or the interrupt
// sequence; a register or cycle divergence names none. `name` is the mnemonic,
// or `NMI` / `IRQ` for a hardware sequence.
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

// What a replay found.
struct DifferentialReport {
  std::uint64_t instructions = 0;   // nodes run and checked
  std::uint64_t interrupts = 0;     // hardware sequences run and checked
  std::uint64_t heldSteps = 0;      // steps a transfer held the CPU off the bus for
  std::uint64_t haltedCycles = 0;   // cycles a waiting or stopped CPU sat through
  std::uint64_t releases = 0;       // waits an interrupt line ended
  std::uint64_t cpuCycles = 0;      // CPU cycles checked
  std::uint64_t masterCycles = 0;   // master cycles run
  std::uint64_t unlifted = 0;       // instructions at an address with no node
  std::vector<Address> unliftedSites;  // those addresses, each once, in address order
  bool stopped = false;             // the CPU stopped, which ended the run
  std::vector<Divergence> divergences;
  // How many times each instruction form ran, keyed by mnemonic, addressing
  // mode and the mode the node reads under — `LDA abs,X e=0 m=8 x=16`.
  std::map<std::string, std::uint64_t> forms;
  // How many times each named construct was exercised. Every name is present,
  // so a construct the run never reached reads zero.
  std::map<std::string, std::uint64_t> constructs;
};

// Replays the run on the machine beside the interpreter. `program` is the
// cartridge's nodes in address order, `Program::find` answering the node for
// the live flags at every step.
[[nodiscard]] DifferentialReport differential(const Program& program, const Replay& replay);

// The interpreter's view of a core state.
[[nodiscard]] Registers registersOf(const Cpu65816State& state) noexcept;

}  // namespace snaggletooth::ir
