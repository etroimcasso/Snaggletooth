// The SingleStepTests 65816 vectors replayed through the interpreter: every case
// the core is proven by, run over the intermediate representation instead.
//
// Each case's instruction is decoded from the bytes at its program counter,
// lifted under the case's own mode, and run by the interpreter from the case's
// initial registers over its sparse memory. The final registers, every write
// (address, value and order), every data read's address in order, the final
// memory and the cycle count are then held to what the vectors record. A native
// case whose instruction has no immediate is run a second time lifted with both
// widths unknown, so the width selected by the live flag is proven on the same
// cases as the typed one.
//
// The interpreter never sees the bytes: the decoder and the lift run in the test,
// and the node is all the interpreter gets.

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../cpu65816/vector_harness.h"
#include "cpu65816_disasm.h"
#include "ir/cpu65816_lift.h"
#include "ir/ir_interpret.h"

#ifndef SNAGGLETOOTH_65816_VECTORS
#define SNAGGLETOOTH_65816_VECTORS ""
#endif

namespace {

using snaggletooth::cpu_vectors::CycleTrace;
using snaggletooth::cpu_vectors::kSignalRw;
using snaggletooth::cpu_vectors::kSignalVda;
using snaggletooth::cpu_vectors::kSignalVpa;
using snaggletooth::cpu_vectors::RegState;
using snaggletooth::cpu_vectors::VectorCase;
using snaggletooth::disasm::Cpu65816Mode;
using snaggletooth::ir::Access;
using snaggletooth::ir::Address;
using snaggletooth::ir::Interpreter;
using snaggletooth::ir::Node;
using snaggletooth::ir::Registers;

std::string vectorsDir() { return SNAGGLETOOTH_65816_VECTORS; }

struct VectorParam {
  std::uint8_t opcode;
  char mode;  // 'n' native, 'e' emulated
};

std::string opcodeFile(const VectorParam& param) {
  char name[16];
  std::snprintf(name, sizeof name, "/%02x.%c.json", param.opcode, param.mode);
  return vectorsDir() + name;
}

std::size_t caseCap() {
  const char* raw = std::getenv("SNAGGLETOOTH_65816_CASE_CAP");
  if (raw == nullptr) return 0;
  long v = std::strtol(raw, nullptr, 10);
  return v > 0 ? static_cast<std::size_t>(v) : 0;
}

// The recorder stops a trace here; only a block move runs long enough to reach
// it, and such a case ends part-way through a byte.
constexpr std::size_t kRecordedCycleCap = 100;

// The two instructions that halt: the recording carries one halted cycle after
// the instruction's own.
bool halts(std::uint8_t opcode) { return opcode == 0xCB || opcode == 0xDB; }
bool blockMove(std::uint8_t opcode) { return opcode == 0x54 || opcode == 0x44; }
bool immediate(std::uint8_t opcode) {
  using snaggletooth::disasm::Cpu65816Addressing;
  const auto mode = snaggletooth::disasm::cpu65816Opcodes()[opcode].mode;
  return mode == Cpu65816Addressing::ImmediateM || mode == Cpu65816Addressing::ImmediateX;
}

struct AccessRecord {
  Address address;
  std::uint8_t value;
  bool write;
};

// One cycle, by what the chip drove: a program access reading the instruction's
// own bytes, a cycle with no access, or a data access with its address and
// direction.
enum class CycleKind { Program, Idle, Data };
struct CycleEntry {
  CycleKind kind;
  Address address = 0;  // for Data
  std::uint8_t value = 0;
  bool write = false;
};

// Sparse memory the interpreter reads through, recording every access — as an
// access, and as one cycle in the shared ordered log a Clock also writes to.
struct SparseBus final : snaggletooth::ir::Bus {
  std::unordered_map<std::uint32_t, std::uint8_t> mem;
  std::vector<AccessRecord> log;
  std::vector<CycleEntry>* order = nullptr;

  std::uint8_t read(Address address, Access) override {
    const auto it = mem.find(address & 0xFFFFFFu);
    const std::uint8_t value = it == mem.end() ? std::uint8_t{0} : it->second;
    log.push_back({address & 0xFFFFFFu, value, false});
    if (order != nullptr) order->push_back({CycleKind::Data, address & 0xFFFFFFu, value, false});
    return value;
  }
  void write(Address address, std::uint8_t value, Access) override {
    mem[address & 0xFFFFFFu] = value;
    log.push_back({address & 0xFFFFFFu, value, true});
    if (order != nullptr) order->push_back({CycleKind::Data, address & 0xFFFFFFu, value, true});
  }
};

// The Clock beside the bus: a fetch appends that many program cycles to the same
// ordered log, an idle that many no-access cycles.
struct RecordingClock final : snaggletooth::ir::Clock {
  std::vector<CycleEntry>* order = nullptr;
  void fetch(unsigned cycles) override {
    for (unsigned i = 0; i < cycles; ++i) order->push_back({CycleKind::Program});
  }
  void idle(unsigned cycles) override {
    for (unsigned i = 0; i < cycles; ++i) order->push_back({CycleKind::Idle});
  }
};

Registers registersOf(const RegState& r) {
  Registers out;
  out.pc = r.pc;
  out.s = r.s;
  out.a = r.a;
  out.x = r.x;
  out.y = r.y;
  out.d = r.d;
  out.p = r.p;
  out.dbr = r.dbr;
  out.pbr = r.pbr;
  out.e = r.e;
  return out;
}

Cpu65816Mode modeOf(const RegState& r) {
  if (r.e) return Cpu65816Mode::reset();
  return Cpu65816Mode::native((r.p & 0x20u) != 0, (r.p & 0x10u) != 0);
}

// The data accesses the recording holds, in order: every write, and every read
// with a valid data address and no valid program address — an opcode or operand
// fetch is the instruction's identity, not its effect.
std::vector<AccessRecord> recordedAccesses(const VectorCase& c) {
  std::vector<AccessRecord> out;
  for (const CycleTrace& cycle : c.cycles) {
    if (!cycle.address.has_value() || !cycle.value.has_value()) continue;
    const bool write = cycle.signals[kSignalRw] == 'w';
    const bool dataRead = cycle.signals[kSignalVda] == 'd' && cycle.signals[kSignalVpa] != 'p';
    if (write || dataRead) out.push_back({*cycle.address, *cycle.value, write});
  }
  return out;
}

// Every cycle the recording holds, in order, by kind: a valid program address is
// a program cycle; otherwise a write, or a valid data address with a value, is a
// data cycle; otherwise the chip drove no access and the cycle is idle.
std::vector<CycleEntry> recordedCycles(const VectorCase& c) {
  std::vector<CycleEntry> out;
  for (const CycleTrace& cycle : c.cycles) {
    if (cycle.signals[kSignalVpa] == 'p') {
      out.push_back({CycleKind::Program});
      continue;
    }
    const bool write = cycle.signals[kSignalRw] == 'w';
    const bool hasData = cycle.address.has_value() && cycle.value.has_value();
    const bool dataRead = cycle.signals[kSignalVda] == 'd';
    if (hasData && (write || dataRead)) {
      out.push_back({CycleKind::Data, *cycle.address, *cycle.value, write});
    } else {
      out.push_back({CycleKind::Idle});
    }
  }
  return out;
}

std::string_view kindName(CycleKind kind) {
  switch (kind) {
    case CycleKind::Program: return "program";
    case CycleKind::Idle: return "idle";
    case CycleKind::Data: return "data";
  }
  return "?";
}

class IrVectors : public ::testing::TestWithParam<VectorParam> {};

// Runs one case through the interpreter under one lift mode and holds it to the
// recording.
void runCase(const VectorCase& c, const Cpu65816Mode& liftMode, std::uint8_t opcode,
             const std::string& variant) {
  const std::string name = c.name + " " + variant;
  SparseBus bus;
  for (const auto& [address, value] : c.initial.ram) bus.mem[address] = value;

  // The instruction's bytes, from the case's memory at the program counter.
  const Address address =
      (static_cast<Address>(c.initial.pbr) << 16) | c.initial.pc;
  std::vector<std::uint8_t> bytes;
  for (std::uint32_t i = 0; i < 4; ++i) {
    const Address at = (address & 0xFF0000u) | ((c.initial.pc + i) & 0xFFFFu);
    const auto it = bus.mem.find(at);
    bytes.push_back(it == bus.mem.end() ? std::uint8_t{0} : it->second);
  }
  const std::optional<snaggletooth::disasm::Instruction> decoded =
      snaggletooth::disasm::decodeAt(bytes, address, address, liftMode);
  ASSERT_TRUE(decoded.has_value()) << name << " (the instruction did not decode)";
  const Node node = snaggletooth::ir::liftInstruction(*decoded, liftMode);

  std::vector<CycleEntry> order;
  bus.order = &order;
  RecordingClock clock;
  clock.order = &order;

  Interpreter interpreter;
  interpreter.registers = registersOf(c.initial);
  interpreter.clock = &clock;
  std::uint32_t cycles = interpreter.execute(node, bus);

  // A block move runs its node once per byte; a case the recorder stopped
  // part-way holds the registers after the last whole byte, with the program
  // counter caught mid-fetch, so that one register is left out there.
  const bool capped = c.cycles.size() >= kRecordedCycleCap;
  if (blockMove(opcode)) {
    const std::size_t bytesRecorded = c.cycles.size() / 7;
    std::size_t moved = 1;
    while (moved < bytesRecorded && interpreter.registers.pc == (address & 0xFFFFu)) {
      cycles += interpreter.execute(node, bus);
      ++moved;
    }
  }

  const Registers& r = interpreter.registers;
  if (!capped) {
    EXPECT_EQ(int{r.pc}, int{c.final_.pc}) << name << " (pc)";
  }
  EXPECT_EQ(int{r.s}, int{c.final_.s}) << name << " (s)";
  EXPECT_EQ(int{r.a}, int{c.final_.a}) << name << " (a)";
  EXPECT_EQ(int{r.x}, int{c.final_.x}) << name << " (x)";
  EXPECT_EQ(int{r.y}, int{c.final_.y}) << name << " (y)";
  EXPECT_EQ(int{r.d}, int{c.final_.d}) << name << " (d)";
  EXPECT_EQ(int{r.p}, int{c.final_.p}) << name << " (p)";
  EXPECT_EQ(int{r.dbr}, int{c.final_.dbr}) << name << " (dbr)";
  EXPECT_EQ(int{r.pbr}, int{c.final_.pbr}) << name << " (pbr)";
  EXPECT_EQ(r.e, c.final_.e) << name << " (e)";

  // The cycle count: the measured base plus the increments that fired, against
  // the cycles the chip took. A halt's recording carries one halted cycle more.
  if (!capped) {
    const std::size_t expected = c.cycles.size() - (halts(opcode) ? 1u : 0u);
    EXPECT_EQ(std::size_t{cycles}, expected) << name << " (cycles)";
  }

  // Every access in order: address, value and direction.
  const std::vector<AccessRecord> want = recordedAccesses(c);
  ASSERT_EQ(bus.log.size(), want.size()) << name << " (data accesses)";
  for (std::size_t i = 0; i < want.size(); ++i) {
    EXPECT_EQ(bus.log[i].address, want[i].address) << name << " (access " << i << " address)";
    EXPECT_EQ(bus.log[i].write, want[i].write) << name << " (access " << i << " direction)";
    if (want[i].write) {
      EXPECT_EQ(int{bus.log[i].value}, int{want[i].value})
          << name << " (access " << i << " value)";
    }
  }

  // Every cycle in order: its kind, and for a data cycle its address and
  // direction. A halt's recording carries one halted cycle past the run's own; a
  // capped block move is compared through its last whole byte.
  const std::vector<CycleEntry> wantCycles = recordedCycles(c);
  const std::size_t compareCount = (blockMove(opcode) && capped)
                                       ? (c.cycles.size() / 7) * 7
                                       : wantCycles.size() - (halts(opcode) ? 1u : 0u);
  ASSERT_EQ(order.size(), compareCount) << name << " (cycle order length)";
  for (std::size_t i = 0; i < compareCount; ++i) {
    EXPECT_EQ(kindName(order[i].kind), kindName(wantCycles[i].kind))
        << name << " (cycle " << i << " kind)";
    if (wantCycles[i].kind == CycleKind::Data && order[i].kind == CycleKind::Data) {
      EXPECT_EQ(order[i].address, wantCycles[i].address) << name << " (cycle " << i << " address)";
      EXPECT_EQ(order[i].write, wantCycles[i].write) << name << " (cycle " << i << " direction)";
    }
  }

  // The invariant: every cycle the node cost is placed — as a program, an idle or
  // a data cycle — and no more.
  EXPECT_EQ(order.size(), std::size_t{cycles}) << name << " (cycles placed)";

  // The final memory: every cell the case accounts for, and no stray write.
  std::unordered_set<std::uint32_t> accounted;
  for (const auto& [cell, value] : c.final_.ram) {
    const auto it = bus.mem.find(cell);
    EXPECT_EQ(int{it == bus.mem.end() ? std::uint8_t{0} : it->second}, int{value})
        << name << " (ram " << cell << ")";
    accounted.insert(cell);
  }
  for (const auto& [cell, value] : c.initial.ram) {
    if (accounted.count(cell) == 0) {
      const auto it = bus.mem.find(cell);
      EXPECT_EQ(int{it == bus.mem.end() ? std::uint8_t{0} : it->second}, int{value})
          << name << " (untouched ram " << cell << ")";
      accounted.insert(cell);
    }
  }
  for (const AccessRecord& access : bus.log) {
    if (access.write) {
      EXPECT_NE(accounted.count(access.address), 0u) << name << " (stray write " << access.address << ")";
    }
  }
}

TEST_P(IrVectors, MatchRecordedEffects) {
  const VectorParam param = GetParam();
  if (vectorsDir().empty()) {
    if (std::getenv("SNAGGLETOOTH_REQUIRE_65816_VECTORS") != nullptr) {
      FAIL() << "SNAGGLETOOTH_65816_VECTORS is empty but "
                "SNAGGLETOOTH_REQUIRE_65816_VECTORS demands the oracle — configure "
                "with -DSNAGGLETOOTH_65816_VECTORS pointing at the SingleStepTests "
                "65816 'v1' directory.";
    }
    GTEST_SKIP() << "SNAGGLETOOTH_65816_VECTORS is unset — point it at the "
                    "SingleStepTests 65816 'v1' directory to run the vectors.";
  }

  const std::string path = opcodeFile(param);
  const auto text = snaggletooth::cpu_vectors::readFile(path);
  ASSERT_TRUE(text.has_value()) << "cannot open vector file: " << path;
  const auto cases = snaggletooth::cpu_vectors::parseVectorFile(*text);
  ASSERT_FALSE(cases.empty()) << "no cases in " << path;

  const std::size_t cap = caseCap();
  std::size_t ran = 0;
  for (const VectorCase& c : cases) {
    if (cap != 0 && ran >= cap) {
      std::printf("[case cap] %s: ran %zu of %zu cases\n", path.c_str(), ran, cases.size());
      break;
    }
    ++ran;
    runCase(c, modeOf(c.initial), param.opcode, "typed");
    if (!c.initial.e && !immediate(param.opcode)) {
      runCase(c, Cpu65816Mode::nativeUnknown(), param.opcode, "by the live flag");
    }
  }
}

std::vector<VectorParam> everyOpcode() {
  std::vector<VectorParam> params;
  for (unsigned opcode = 0; opcode < 256; ++opcode) {
    params.push_back({static_cast<std::uint8_t>(opcode), 'n'});
    params.push_back({static_cast<std::uint8_t>(opcode), 'e'});
  }
  return params;
}

INSTANTIATE_TEST_SUITE_P(EveryOpcode, IrVectors, ::testing::ValuesIn(everyOpcode()),
                         [](const ::testing::TestParamInfo<VectorParam>& info) {
                           char label[16];
                           std::snprintf(label, sizeof label, "op%02X_%c", info.param.opcode,
                                         info.param.mode);
                           return std::string(label);
                         });

}  // namespace
