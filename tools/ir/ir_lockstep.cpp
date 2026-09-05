#include "ir/ir_lockstep.h"

#include <utility>

namespace snaggletooth::ir {
namespace {

// Whether an access's declared purpose is the kind the core drove.
bool kindMatches(Access access, CycleKind kind, bool write) {
  switch (access) {
    case Access::Data: return kind == (write ? CycleKind::DataWrite : CycleKind::DataRead);
    case Access::Rmw: return kind == (write ? CycleKind::RmwWrite : CycleKind::RmwRead);
    case Access::RmwUnmodified: return write && kind == CycleKind::RmwModifyWrite;
    case Access::Vector: return !write && kind == CycleKind::VectorRead;
  }
  return false;
}

// The bus the interpreter reads through: the machine's accesses for the step,
// answered in order. A read's value is the machine's; its address, and every
// write's address and value, are checked as they come.
struct CheckingBus final : Bus {
  const std::vector<BusAccess>& expected;
  const Interpreter& interpreter;
  std::size_t cursor = 0;
  std::vector<Divergence>& out;
  Divergence prototype;  // the step's identity, copied into every divergence

  CheckingBus(const std::vector<BusAccess>& expected, const Interpreter& interpreter,
              std::vector<Divergence>& out, Divergence prototype)
      : expected(expected), interpreter(interpreter), out(out), prototype(std::move(prototype)) {}

  void diverge(std::string what, std::uint32_t machine, std::uint32_t ir) {
    Divergence d = prototype;
    d.effect = interpreter.effectIndex;
    d.what = std::move(what);
    d.expected = machine;
    d.actual = ir;
    out.push_back(std::move(d));
  }

  std::uint8_t read(Address address, Access access) override {
    address &= 0xFFFFFFu;
    if (cursor >= expected.size()) {
      diverge("a read the machine did not make", 0, address);
      return 0;
    }
    const BusAccess& e = expected[cursor++];
    if (e.write) diverge("a read where the machine wrote", e.address, address);
    if (e.address != address) diverge("read address", e.address, address);
    if (!kindMatches(access, e.kind, false)) {
      diverge("read kind", static_cast<std::uint32_t>(e.kind), static_cast<std::uint32_t>(access));
    }
    return e.value;
  }

  void write(Address address, std::uint8_t value, Access access) override {
    address &= 0xFFFFFFu;
    if (cursor >= expected.size()) {
      diverge("a write the machine did not make", 0, address);
      return;
    }
    const BusAccess& e = expected[cursor++];
    if (!e.write) diverge("a write where the machine read", e.address, address);
    if (e.address != address) diverge("write address", e.address, address);
    if (e.value != value) diverge("write value", e.value, value);
    if (!kindMatches(access, e.kind, true)) {
      diverge("write kind", static_cast<std::uint32_t>(e.kind), static_cast<std::uint32_t>(access));
    }
  }
};

// After the effects: the accesses the machine made that the node did not, the
// registers, the run state and the cycles. Realigns the interpreter when
// anything disagreed.
void checkAfter(Interpreter& interpreter, CheckingBus& bus, const StepObserver& step,
                const Cpu65816State& after, std::uint32_t cycles, const char* unmatched,
                std::size_t divergencesBefore, std::vector<Divergence>& out) {
  if (bus.cursor < step.data.size()) {
    bus.diverge(unmatched, step.data[bus.cursor].address,
                static_cast<std::uint32_t>(step.data.size() - bus.cursor));
  }
  const Registers machineAfter = registersOf(after);
  const Registers& irAfter = interpreter.registers;
  auto check = [&](const char* what, std::uint32_t machine, std::uint32_t ir) {
    if (machine == ir) return;
    Divergence d = bus.prototype;
    d.what = what;
    d.expected = machine;
    d.actual = ir;
    out.push_back(std::move(d));
  };
  check("register pc", machineAfter.pc, irAfter.pc);
  check("register s", machineAfter.s, irAfter.s);
  check("register a", machineAfter.a, irAfter.a);
  check("register x", machineAfter.x, irAfter.x);
  check("register y", machineAfter.y, irAfter.y);
  check("register d", machineAfter.d, irAfter.d);
  check("register p", machineAfter.p, irAfter.p);
  check("register dbr", machineAfter.dbr, irAfter.dbr);
  check("register pbr", machineAfter.pbr, irAfter.pbr);
  check("register e", machineAfter.e ? 1u : 0u, irAfter.e ? 1u : 0u);
  check("run state", static_cast<std::uint32_t>(machineAfter.run),
        static_cast<std::uint32_t>(irAfter.run));
  check("cycles", step.cpuCycles, cycles);
  if (out.size() != divergencesBefore) interpreter.registers = machineAfter;
}

}  // namespace

void StepObserver::access(const BusAccess& a) {
  if (a.source != AccessSource::Cpu) return;
  cpuRan = true;
  ++cpuCycles;
  if (a.kind == CycleKind::OpcodeFetch || a.kind == CycleKind::OperandFetch) {
    fetches.push_back(a);
    return;
  }
  data.push_back(a);
}

void StepObserver::internal(std::uint32_t, std::optional<CycleKind>) {
  cpuRan = true;
  ++cpuCycles;
}

void StepObserver::clear() {
  fetches.clear();
  data.clear();
  cpuCycles = 0;
  cpuRan = false;
}

Registers registersOf(const Cpu65816State& s) noexcept {
  Registers r;
  r.pc = s.pc;
  r.s = s.s;
  r.a = s.a;
  r.x = s.x;
  r.y = s.y;
  r.d = s.d;
  r.p = s.p;
  r.dbr = s.dbr;
  r.pbr = s.pbr;
  r.e = s.e;
  r.run = s.run == CpuRunState::Running   ? Run::Running
          : s.run == CpuRunState::Waiting ? Run::Waiting
                                          : Run::Stopped;
  return r;
}

std::uint32_t checkNode(Interpreter& interpreter, const Node& node, const StepObserver& step,
                        const Cpu65816State& after, Divergence prototype,
                        std::vector<Divergence>& out) {
  prototype.name = std::string(node.instruction.mnemonic);
  prototype.mode = node.mode;
  const std::size_t before = out.size();
  CheckingBus bus(step.data, interpreter, out, std::move(prototype));
  const std::uint32_t cycles = interpreter.execute(node, bus);
  checkAfter(interpreter, bus, step, after, cycles, "accesses the machine made that the node did not",
             before, out);
  return cycles;
}

std::uint32_t checkInterrupt(Interpreter& interpreter, const std::vector<Effect>& sequence,
                             const StepObserver& step, const Cpu65816State& after,
                             Divergence prototype, std::vector<Divergence>& out) {
  const Registers& r = interpreter.registers;
  prototype.mode.emulation = r.e;
  prototype.mode.accumulator8 = r.accumulator8();
  prototype.mode.index8 = r.index8();
  const std::size_t before = out.size();
  CheckingBus bus(step.data, interpreter, out, std::move(prototype));
  const std::uint32_t cycles = interpreter.interrupt(sequence, bus);
  checkAfter(interpreter, bus, step, after, cycles,
             "accesses the machine made that the sequence did not", before, out);
  return cycles;
}

}  // namespace snaggletooth::ir
