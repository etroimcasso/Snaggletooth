#pragma once

// The interpreter — runs a node's effects, and nothing else.
//
// It holds the CPU's registers and reads its memory through a bus its host
// answers. It never sees an instruction's bytes, its listing or its decoder: what
// it knows about an instruction is the effect layer, and what it produces is the
// registers after, every load and store in order, and the cycles the node cost.
// That is the whole point of it. An interpreter that could reach the bytes could
// copy them through and prove nothing; this one has to compute every value, so a
// run beside the core compares what the intermediate representation says against
// what the chip does.
//
// A host drives it one node at a time: it looks the node up for the live program
// counter and flags, supplies a hardware interrupt between two nodes when one is
// due, and answers every read.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "ir/ir.h"

namespace snaggletooth::ir {

// Memory as the interpreter reaches it: a byte read at an address, a byte written
// to one. The kind says what the access is for.
class Bus {
 public:
  virtual ~Bus() = default;
  virtual std::uint8_t read(Address address, Access access) = 0;
  virtual void write(Address address, std::uint8_t value, Access access) = 0;
};

enum class Run : std::uint8_t { Running, Waiting, Stopped };

// The CPU's state as the effects name it.
struct Registers {
  std::uint16_t pc = 0;
  std::uint16_t s = 0;
  std::uint16_t a = 0;
  std::uint16_t x = 0;
  std::uint16_t y = 0;
  std::uint16_t d = 0;
  std::uint8_t p = 0;
  std::uint8_t dbr = 0;
  std::uint8_t pbr = 0;
  bool e = false;
  Run run = Run::Running;

  friend bool operator==(const Registers&, const Registers&) = default;

  // The live widths: eight bits under emulation or the flag, sixteen otherwise.
  [[nodiscard]] bool accumulator8() const noexcept { return e || (p & 0x20u) != 0; }
  [[nodiscard]] bool index8() const noexcept { return e || (p & 0x10u) != 0; }
};

class Interpreter {
 public:
  Registers registers;

  // The index, in the sequence being run, of the effect whose accesses the bus
  // is answering — so a bus that reports where an access came from can name it.
  std::size_t effectIndex = 0;

  // Runs one node: re-establishes the invariants the chip holds between
  // instructions — the index high bytes zero while the index registers are eight
  // bits wide, the stack in page one under emulation — then every effect whose
  // condition holds, in order. Returns the cycles the node cost: its measured
  // base under the live widths plus every `Cycles` effect that fired. A block
  // move leaves the program counter on itself while bytes remain; the host runs
  // the node again for each.
  std::uint32_t execute(const Node& node, Bus& bus);

  // Runs a hardware interrupt sequence — a program's `nmi` or `irq` — the same
  // way, and returns its cycles. A waiting interpreter is released first.
  std::uint32_t interrupt(const std::vector<Effect>& sequence, Bus& bus);

  // Releases a wait without an interrupt sequence: what a maskable request does
  // while the interrupt-disable flag is set.
  void release() noexcept { if (registers.run == Run::Waiting) registers.run = Run::Running; }
};

}  // namespace snaggletooth::ir
