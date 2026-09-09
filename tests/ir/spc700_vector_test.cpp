// The SingleStepTests SPC700 vectors replayed through the sound CPU's
// interpreter: every case the core is proven by, run over the intermediate
// representation instead.
//
// Each case's instruction is decoded from the bytes at its program counter,
// lifted, and run by the interpreter from the case's initial registers over its
// sparse memory. The final registers, every write (address, value and order),
// every data read's address in order, the final memory and the cycle count are
// then held to what the vectors record.
//
// The recording does not say which of its reads are the instruction's own
// fetches, so the core tells: the case is run on the core a cycle at a time
// beside the recording, and a read at the program counter that steps the
// counter past the byte — or that reads the instruction's last byte as the
// counter moves to a jump's destination — is a fetch. The interpreter never
// sees the bytes or the core: the decoder and the lift run in the test, and
// the node is all the interpreter gets.

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../spc700/vector_harness.h"
#include "ir/ir_interpret.h"
#include "ir/spc700_lift.h"
#include "snaggletooth/apu/spc700.h"
#include "spc700_disasm.h"

#ifndef SNAGGLETOOTH_SPC700_VECTORS
#define SNAGGLETOOTH_SPC700_VECTORS ""
#endif

namespace {

using snaggletooth::Spc700;
using snaggletooth::Spc700State;
using snaggletooth::ir::Access;
using snaggletooth::ir::Address;
using snaggletooth::ir::Node;
using snaggletooth::ir::Spc700Interpreter;
using snaggletooth::ir::Spc700Registers;
using snaggletooth::test::CycleEvent;
using snaggletooth::test::RegState;
using snaggletooth::test::VectorCase;

std::string vectorsDir() { return SNAGGLETOOTH_SPC700_VECTORS; }

std::string opcodeFile(std::uint8_t opcode) {
  char name[16];
  std::snprintf(name, sizeof name, "/%02x.json", opcode);
  return vectorsDir() + name;
}

std::size_t caseCap() {
  const char* raw = std::getenv("SNAGGLETOOTH_SPC700_CASE_CAP");
  if (raw == nullptr) return 0;
  long v = std::strtol(raw, nullptr, 10);
  return v > 0 ? static_cast<std::size_t>(v) : 0;
}

struct AccessRecord {
  std::uint16_t address;
  std::uint8_t value;
  bool write;
};

// Sparse memory the interpreter reads through, recording every access in order.
struct SparseBus final : snaggletooth::ir::Bus {
  std::unordered_map<std::uint16_t, std::uint8_t> mem;
  std::vector<AccessRecord> log;

  std::uint8_t read(Address address, Access) override {
    const auto at = static_cast<std::uint16_t>(address & 0xFFFFu);
    const auto it = mem.find(at);
    const std::uint8_t value = it == mem.end() ? std::uint8_t{0} : it->second;
    log.push_back({at, value, false});
    return value;
  }
  void write(Address address, std::uint8_t value, Access) override {
    const auto at = static_cast<std::uint16_t>(address & 0xFFFFu);
    mem[at] = value;
    log.push_back({at, value, true});
  }
};

// The core's side of the same case: a flat memory that keeps the address and
// direction of each access, so the run can tell a fetch — a read at the
// program counter that stepped the counter past the byte — from a data read.
struct ClassifyingBus {
  std::array<std::uint8_t, 65536> ram{};
  std::size_t accesses = 0;
  std::uint16_t lastAddress = 0;
  bool lastWrite = false;

  std::uint8_t read(std::uint16_t address) {
    ++accesses;
    lastAddress = address;
    lastWrite = false;
    return ram[address];
  }
  void write(std::uint16_t address, std::uint8_t value) {
    ++accesses;
    lastAddress = address;
    lastWrite = true;
    ram[address] = value;
  }
};

Spc700Registers registersOf(const RegState& r) {
  Spc700Registers out;
  out.pc = r.pc;
  out.a = r.a;
  out.x = r.x;
  out.y = r.y;
  out.sp = r.sp;
  out.psw = r.psw;
  return out;
}

// The data accesses the recording holds, in order: every write, and every read
// that is not an opcode or operand fetch. Which reads are fetches is what the
// core says when it runs the same case. A read the chip makes at the counter
// and throws away is of the byte after the instruction's `length` bytes, and
// stays.
std::vector<AccessRecord> recordedAccesses(const VectorCase& c, std::uint8_t length) {
  ClassifyingBus bus;
  for (const auto& [address, value] : c.initial.ram) bus.ram[address] = value;
  Spc700 cpu(Spc700State{.pc = c.initial.pc,
                         .a = c.initial.a,
                         .x = c.initial.x,
                         .y = c.initial.y,
                         .sp = c.initial.sp,
                         .psw = c.initial.psw});
  std::vector<bool> fetchByAccess;
  for (std::size_t i = 0; i < c.cycles.size(); ++i) {
    const std::uint16_t before = cpu.state().pc;
    const std::size_t narrated = bus.accesses;
    cpu.stepCycle(bus);
    if (bus.accesses == narrated) continue;
    // A fetch reads at the counter and steps it — or, on an instruction's last
    // cycle, reads its last byte at the counter and then moves the counter to a
    // destination, as a jump does.
    const bool stepped = cpu.state().pc == static_cast<std::uint16_t>(before + 1u);
    const bool lastByte = cpu.atInstructionBoundary() &&
                          static_cast<std::uint16_t>(before - c.initial.pc) < length;
    const bool fetch = !bus.lastWrite && bus.lastAddress == before && (stepped || lastByte);
    fetchByAccess.push_back(fetch);
  }

  std::vector<AccessRecord> out;
  std::size_t access = 0;
  for (const CycleEvent& cycle : c.cycles) {
    if (cycle.kind == CycleEvent::Kind::Wait) continue;
    const bool fetch = access < fetchByAccess.size() && fetchByAccess[access];
    ++access;
    if (fetch) continue;
    if (!cycle.address.has_value()) continue;
    out.push_back({*cycle.address, cycle.value.value_or(0), cycle.kind == CycleEvent::Kind::Write});
  }
  return out;
}

class Spc700IrVectors : public ::testing::TestWithParam<std::uint8_t> {};

void runCase(const VectorCase& c, std::uint8_t opcode) {
  const std::string& name = c.name;
  SparseBus bus;
  for (const auto& [address, value] : c.initial.ram) bus.mem[address] = value;

  // The instruction's bytes, from the case's memory at the program counter.
  std::vector<std::uint8_t> bytes;
  for (std::uint32_t i = 0; i < 3; ++i) {
    const auto at = static_cast<std::uint16_t>((c.initial.pc + i) & 0xFFFFu);
    const auto it = bus.mem.find(at);
    bytes.push_back(it == bus.mem.end() ? std::uint8_t{0} : it->second);
  }
  ASSERT_EQ(int{bytes[0]}, int{opcode}) << name << " (the opcode at the program counter)";
  const std::optional<snaggletooth::disasm::Instruction> decoded =
      snaggletooth::disasm::decodeAt(bytes, c.initial.pc, c.initial.pc);
  ASSERT_TRUE(decoded.has_value()) << name << " (the instruction did not decode)";
  const Node node = snaggletooth::ir::liftSpc700Instruction(*decoded);

  Spc700Interpreter interpreter;
  interpreter.registers = registersOf(c.initial);
  const std::uint32_t cycles = interpreter.execute(node, bus);

  const Spc700Registers& r = interpreter.registers;
  EXPECT_EQ(int{r.pc}, int{c.final_.pc}) << name << " (pc)";
  EXPECT_EQ(int{r.a}, int{c.final_.a}) << name << " (a)";
  EXPECT_EQ(int{r.x}, int{c.final_.x}) << name << " (x)";
  EXPECT_EQ(int{r.y}, int{c.final_.y}) << name << " (y)";
  EXPECT_EQ(int{r.sp}, int{c.final_.sp}) << name << " (sp)";
  EXPECT_EQ(int{r.psw}, int{c.final_.psw}) << name << " (psw)";

  // The cycle count: the measured base plus the increments that fired, against
  // the cycles the chip took.
  EXPECT_EQ(std::size_t{cycles}, c.cycles.size()) << name << " (cycles)";

  // Every data access in order: address, direction, and a write's value.
  const std::vector<AccessRecord> want = recordedAccesses(c, decoded->length);
  ASSERT_EQ(bus.log.size(), want.size()) << name << " (data accesses)";
  for (std::size_t i = 0; i < want.size(); ++i) {
    EXPECT_EQ(int{bus.log[i].address}, int{want[i].address}) << name << " (access " << i << " address)";
    EXPECT_EQ(bus.log[i].write, want[i].write) << name << " (access " << i << " direction)";
    if (want[i].write) {
      EXPECT_EQ(int{bus.log[i].value}, int{want[i].value}) << name << " (access " << i << " value)";
    }
  }

  // The final memory: every cell the case accounts for, and no stray write.
  std::unordered_set<std::uint16_t> accounted;
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

TEST_P(Spc700IrVectors, MatchRecordedEffects) {
  const std::uint8_t opcode = GetParam();
  if (vectorsDir().empty()) {
    if (std::getenv("SNAGGLETOOTH_REQUIRE_VECTORS") != nullptr) {
      FAIL() << "SNAGGLETOOTH_SPC700_VECTORS is empty but SNAGGLETOOTH_REQUIRE_VECTORS "
                "demands the oracle — configure with -DSNAGGLETOOTH_SPC700_VECTORS "
                "pointing at the SingleStepTests SPC700 'v1' directory.";
    }
    GTEST_SKIP() << "SNAGGLETOOTH_SPC700_VECTORS is unset — point it at the "
                    "SingleStepTests SPC700 'v1' directory to run the vectors.";
  }

  const std::string path = opcodeFile(opcode);
  const auto text = snaggletooth::test::readFile(path);
  ASSERT_TRUE(text.has_value()) << "cannot open vector file: " << path;
  const auto cases = snaggletooth::test::parseVectorFile(*text);
  ASSERT_FALSE(cases.empty()) << "no cases in " << path;

  const std::size_t cap = caseCap();
  std::size_t ran = 0;
  for (const VectorCase& c : cases) {
    if (cap != 0 && ran >= cap) {
      std::printf("[case cap] %s: ran %zu of %zu cases\n", path.c_str(), ran, cases.size());
      break;
    }
    ++ran;
    runCase(c, opcode);
  }
}

// The four families of the core's own vector suite, so a failure names the
// family it came from. Together they name every opcode once.
constexpr std::uint8_t kMovOpcodes[] = {
    0xE8, 0xE6, 0xBF, 0xE4, 0xF4, 0xE5, 0xF5, 0xF6, 0xE7, 0xF7,
    0xCD, 0xF8, 0xF9, 0xE9, 0x8D, 0xEB, 0xFB, 0xEC,
    0xC6, 0xAF, 0xC4, 0xD4, 0xC5, 0xD5, 0xD6, 0xC7, 0xD7, 0xD8, 0xD9, 0xC9,
    0xCB, 0xDB, 0xCC,
    0x7D, 0xDD, 0x5D, 0xFD, 0x9D, 0xBD, 0xFA, 0x8F,
};

constexpr std::uint8_t kAluOpcodes[] = {
    0x88, 0x86, 0x84, 0x94, 0x85, 0x95, 0x96, 0x87, 0x97, 0x99, 0x89, 0x98,
    0xA8, 0xA6, 0xA4, 0xB4, 0xA5, 0xB5, 0xB6, 0xA7, 0xB7, 0xB9, 0xA9, 0xB8,
    0x68, 0x66, 0x64, 0x74, 0x65, 0x75, 0x76, 0x67, 0x77, 0x79, 0x69, 0x78,
    0xC8, 0x3E, 0x1E, 0xAD, 0x7E, 0x5E,
    0x28, 0x26, 0x24, 0x34, 0x25, 0x35, 0x36, 0x27, 0x37, 0x39, 0x29, 0x38,
    0x08, 0x06, 0x04, 0x14, 0x05, 0x15, 0x16, 0x07, 0x17, 0x19, 0x09, 0x18,
    0x48, 0x46, 0x44, 0x54, 0x45, 0x55, 0x56, 0x47, 0x57, 0x59, 0x49, 0x58,
    0xBC, 0x3D, 0xFC, 0xAB, 0xBB, 0xAC, 0x9C, 0x1D, 0xDC, 0x8B, 0x9B, 0x8C,
    0x1C, 0x0B, 0x1B, 0x0C, 0x5C, 0x4B, 0x5B, 0x4C,
    0x3C, 0x2B, 0x3B, 0x2C, 0x7C, 0x6B, 0x7B, 0x6C,
    0x9F,
};

constexpr std::uint8_t kWordBitOpcodes[] = {
    0xBA, 0xDA, 0x3A, 0x1A, 0x7A, 0x9A, 0x5A,
    0xCF, 0x9E,
    0xDF, 0xBE,
    0x02, 0x22, 0x42, 0x62, 0x82, 0xA2, 0xC2, 0xE2,
    0x12, 0x32, 0x52, 0x72, 0x92, 0xB2, 0xD2, 0xF2,
    0x0E, 0x4E,
    0x4A, 0x6A, 0x0A, 0x2A, 0x8A, 0xEA, 0xAA, 0xCA,
};

constexpr std::uint8_t kControlOpcodes[] = {
    0x2F, 0xF0, 0xD0, 0xB0, 0x90, 0x70, 0x50, 0x30, 0x10,
    0x03, 0x23, 0x43, 0x63, 0x83, 0xA3, 0xC3, 0xE3,
    0x13, 0x33, 0x53, 0x73, 0x93, 0xB3, 0xD3, 0xF3,
    0x2E, 0xDE, 0x6E, 0xFE,
    0x5F, 0x1F,
    0x3F, 0x4F,
    0x01, 0x11, 0x21, 0x31, 0x41, 0x51, 0x61, 0x71,
    0x81, 0x91, 0xA1, 0xB1, 0xC1, 0xD1, 0xE1, 0xF1,
    0x6F, 0x7F, 0x0F,
    0x2D, 0x4D, 0x6D, 0x0D, 0xAE, 0xCE, 0xEE, 0x8E,
    0x60, 0x80, 0xED, 0xE0, 0x20, 0x40, 0xA0, 0xC0,
    0x00, 0xEF, 0xFF,
};

std::string label(const ::testing::TestParamInfo<std::uint8_t>& info) {
  char text[8];
  std::snprintf(text, sizeof text, "op%02X", info.param);
  return std::string(text);
}

INSTANTIATE_TEST_SUITE_P(Mov, Spc700IrVectors, ::testing::ValuesIn(kMovOpcodes), label);
INSTANTIATE_TEST_SUITE_P(Alu, Spc700IrVectors, ::testing::ValuesIn(kAluOpcodes), label);
INSTANTIATE_TEST_SUITE_P(WordBit, Spc700IrVectors, ::testing::ValuesIn(kWordBitOpcodes), label);
INSTANTIATE_TEST_SUITE_P(Control, Spc700IrVectors, ::testing::ValuesIn(kControlOpcodes), label);

}  // namespace
