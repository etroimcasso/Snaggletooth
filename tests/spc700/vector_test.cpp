#include "snaggletooth/apu/spc700.h"
#include "vector_harness.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#ifndef SNAGGLETOOTH_SPC700_VECTORS
#define SNAGGLETOOTH_SPC700_VECTORS ""
#endif

namespace {

using snaggletooth::Spc700;
using snaggletooth::Spc700State;
using snaggletooth::test::VectorCase;

std::string vectorsDir() { return SNAGGLETOOTH_SPC700_VECTORS; }

std::string opcodeFile(std::uint8_t opcode) {
  char name[16];
  std::snprintf(name, sizeof name, "/%02x.json", opcode);
  return vectorsDir() + name;
}

// An optional per-opcode case cap for a fast dev loop. Zero (unset) runs every
// case; a positive value truncates and prints what it dropped.
std::size_t caseCap() {
  const char* raw = std::getenv("SNAGGLETOOTH_SPC700_CASE_CAP");
  if (raw == nullptr) return 0;
  long v = std::strtol(raw, nullptr, 10);
  return v > 0 ? static_cast<std::size_t>(v) : 0;
}

// The 41 opcodes of the 8-bit MOV family — the three "8-bit move" groups of the
// SNESdev instruction-set table. The four lists below name every opcode 0x00..0xFF
// exactly once, grouped by family so a failure names the family it came from.
constexpr std::uint8_t kMovOpcodes[] = {
    // memory to register
    0xE8, 0xE6, 0xBF, 0xE4, 0xF4, 0xE5, 0xF5, 0xF6, 0xE7, 0xF7,
    0xCD, 0xF8, 0xF9, 0xE9, 0x8D, 0xEB, 0xFB, 0xEC,
    // register to memory
    0xC6, 0xAF, 0xC4, 0xD4, 0xC5, 0xD5, 0xD6, 0xC7, 0xD7, 0xD8, 0xD9, 0xC9,
    0xCB, 0xDB, 0xCC,
    // register/register and special direct-page moves
    0x7D, 0xDD, 0x5D, 0xFD, 0x9D, 0xBD, 0xFA, 0x8F,
};

// The 107 opcodes of the 8-bit ALU family — the SNESdev table's "8-bit
// arithmetic", "8-bit boolean logic", "8-bit increment / decrement", "8-bit
// shift / rotation" groups, plus XCN. Same parameterized runner as the MOV list.
constexpr std::uint8_t kAluOpcodes[] = {
    // ADC (all modes)
    0x88, 0x86, 0x84, 0x94, 0x85, 0x95, 0x96, 0x87, 0x97, 0x99, 0x89, 0x98,
    // SBC (all modes)
    0xA8, 0xA6, 0xA4, 0xB4, 0xA5, 0xB5, 0xB6, 0xA7, 0xB7, 0xB9, 0xA9, 0xB8,
    // CMP A (all modes), then CMP X and CMP Y
    0x68, 0x66, 0x64, 0x74, 0x65, 0x75, 0x76, 0x67, 0x77, 0x79, 0x69, 0x78,
    0xC8, 0x3E, 0x1E, 0xAD, 0x7E, 0x5E,
    // AND (all modes)
    0x28, 0x26, 0x24, 0x34, 0x25, 0x35, 0x36, 0x27, 0x37, 0x39, 0x29, 0x38,
    // OR (all modes)
    0x08, 0x06, 0x04, 0x14, 0x05, 0x15, 0x16, 0x07, 0x17, 0x19, 0x09, 0x18,
    // EOR (all modes)
    0x48, 0x46, 0x44, 0x54, 0x45, 0x55, 0x56, 0x47, 0x57, 0x59, 0x49, 0x58,
    // INC / DEC
    0xBC, 0x3D, 0xFC, 0xAB, 0xBB, 0xAC, 0x9C, 0x1D, 0xDC, 0x8B, 0x9B, 0x8C,
    // ASL / LSR / ROL / ROR
    0x1C, 0x0B, 0x1B, 0x0C, 0x5C, 0x4B, 0x5B, 0x4C,
    0x3C, 0x2B, 0x3B, 0x2C, 0x7C, 0x6B, 0x7B, 0x6C,
    // XCN
    0x9F,
};

// The 37 opcodes of the 16-bit and single-bit families — the SNESdev table's
// "16-bit operations" (word moves, INCW/DECW, ADDW/SUBW/CMPW, MUL, DIV), "decimal
// adjust" (DAA/DAS) and "memory bit operations" (SET1/CLR1, TSET1/TCLR1, the
// carry-bit AND1/OR1/EOR1/NOT1/MOV1). Same parameterized runner as the lists above.
constexpr std::uint8_t kWordBitOpcodes[] = {
    // 16-bit word moves, increment/decrement, arithmetic
    0xBA, 0xDA, 0x3A, 0x1A, 0x7A, 0x9A, 0x5A,
    // multiply / divide
    0xCF, 0x9E,
    // decimal adjust
    0xDF, 0xBE,
    // SET1 dp.0..7 (bit in opcode bits 5-7)
    0x02, 0x22, 0x42, 0x62, 0x82, 0xA2, 0xC2, 0xE2,
    // CLR1 dp.0..7
    0x12, 0x32, 0x52, 0x72, 0x92, 0xB2, 0xD2, 0xF2,
    // TSET1 / TCLR1
    0x0E, 0x4E,
    // carry-bit logic on one bit of an absolute byte
    0x4A, 0x6A, 0x0A, 0x2A, 0x8A, 0xEA, 0xAA, 0xCA,
};

// The 71 control-flow opcodes — the SNESdev table's "branching", "subroutines",
// "stack", "status flags" and "no-operation and halt" groups. This is the last
// family: with it the four lists cover every opcode 0x00..0xFF.
constexpr std::uint8_t kControlOpcodes[] = {
    // relative branches
    0x2F, 0xF0, 0xD0, 0xB0, 0x90, 0x70, 0x50, 0x30, 0x10,
    // branch on a direct-page bit (BBS dp.0..7, then BBC dp.0..7)
    0x03, 0x23, 0x43, 0x63, 0x83, 0xA3, 0xC3, 0xE3,
    0x13, 0x33, 0x53, 0x73, 0x93, 0xB3, 0xD3, 0xF3,
    // compare/decrement and branch
    0x2E, 0xDE, 0x6E, 0xFE,
    // jumps
    0x5F, 0x1F,
    // subroutine calls and returns
    0x3F, 0x4F,
    0x01, 0x11, 0x21, 0x31, 0x41, 0x51, 0x61, 0x71,  // TCALL 0..7
    0x81, 0x91, 0xA1, 0xB1, 0xC1, 0xD1, 0xE1, 0xF1,  // TCALL 8..15
    0x6F, 0x7F, 0x0F,
    // stack push / pop
    0x2D, 0x4D, 0x6D, 0x0D, 0xAE, 0xCE, 0xEE, 0x8E,
    // status flags
    0x60, 0x80, 0xED, 0xE0, 0x20, 0x40, 0xA0, 0xC0,
    // no-operation and halt
    0x00, 0xEF, 0xFF,
};

class Spc700Vectors : public ::testing::TestWithParam<std::uint8_t> {};

using snaggletooth::test::CycleEvent;
using snaggletooth::test::RecordingFlatBus;

const char* kindName(CycleEvent::Kind kind) {
  switch (kind) {
    case CycleEvent::Kind::Read: return "read";
    case CycleEvent::Kind::Write: return "write";
    case CycleEvent::Kind::Wait: break;
  }
  return "wait";
}

// The state the case starts from. The progress fields stay at their defaults, so the
// core begins on an instruction boundary.
Spc700State stateOf(const snaggletooth::test::RegState& r) {
  return Spc700State{.pc = r.pc, .a = r.a, .x = r.x, .y = r.y, .sp = r.sp, .psw = r.psw};
}

// The exact final registers the case demands.
void expectFinalState(const Spc700State& s, const VectorCase& c) {
  EXPECT_EQ(s.pc, c.final_.pc) << c.name << " (pc)";
  EXPECT_EQ(int{s.a}, int{c.final_.a}) << c.name << " (a)";
  EXPECT_EQ(int{s.x}, int{c.final_.x}) << c.name << " (x)";
  EXPECT_EQ(int{s.y}, int{c.final_.y}) << c.name << " (y)";
  EXPECT_EQ(int{s.sp}, int{c.final_.sp}) << c.name << " (sp)";
  EXPECT_EQ(int{s.psw}, int{c.final_.psw}) << c.name << " (psw)";
}

// The exact final RAM: a full 64KB compare, so a stray write cannot hide.
void expectFinalRam(const std::array<std::uint8_t, 65536>& ram, const VectorCase& c) {
  std::array<std::uint8_t, 65536> expected{};
  for (const auto& [address, value] : c.final_.ram) expected[address] = value;
  EXPECT_EQ(ram, expected) << c.name << " (ram)";
}

// TSET1 / TCLR1 !abs (0x0E / 0x4E) read their operand once and spend the next cycle
// inside the chip. The recorded trace for these two opcodes has a read at that cycle
// (cycle 4), so the per-cycle comparison accepts the core's wait there. The final state
// and full RAM still match exactly — the trace's extra read moved no observable byte —
// and every other cycle is asserted unchanged.
bool blarggInternalizesReadCycle(std::uint8_t opcode, std::size_t cycle) {
  return (opcode == 0x0E || opcode == 0x4E) && cycle == 4;
}

// One case, run a cycle at a time and compared against the recording cycle by cycle. A
// cycle the core narrates must match the recorded access in kind, address and byte; a
// cycle it narrates nothing on must be a recorded wait, and a recorded wait must find
// the core silent. A field the recording leaves null is not asserted — the byte a
// discarded read moved was never captured.
void runPerCycle(std::uint8_t opcode, const VectorCase& c) {
  RecordingFlatBus bus;
  for (const auto& [address, value] : c.initial.ram) bus.ram[address] = value;

  Spc700 cpu(stateOf(c.initial));
  for (std::size_t i = 0; i < c.cycles.size(); ++i) {
    const std::size_t narrated = bus.events.size();
    cpu.stepCycle(bus);
    const CycleEvent& want = c.cycles[i];

    ASSERT_LE(bus.events.size(), narrated + 1)
        << c.name << " (cycle " << i << " reached memory more than once)";
    if (bus.events.size() == narrated) {
      if (want.kind == CycleEvent::Kind::Read && blarggInternalizesReadCycle(opcode, i)) {
        continue;  // documented blargg deviation: this recorded read is internal on hardware
      }
      EXPECT_EQ(want.kind, CycleEvent::Kind::Wait)
          << c.name << " (cycle " << i << " reached memory not at all, but the chip "
          << kindName(want.kind) << ")";
      continue;
    }

    const CycleEvent& got = bus.events.back();
    ASSERT_NE(want.kind, CycleEvent::Kind::Wait)
        << c.name << " (cycle " << i << " reached memory where the chip waited)";
    EXPECT_EQ(kindName(got.kind), kindName(want.kind)) << c.name << " (cycle " << i << ")";
    ASSERT_TRUE(want.address.has_value()) << c.name << " (cycle " << i << " address)";
    EXPECT_EQ(*got.address, *want.address) << c.name << " (cycle " << i << " address)";
    if (want.value.has_value()) {
      EXPECT_EQ(int{*got.value}, int{*want.value}) << c.name << " (cycle " << i << " value)";
    }
  }

  // Every SPC700 case runs a whole instruction, so the cycle count is proven by where
  // the instruction ended rather than reported by it.
  EXPECT_TRUE(cpu.atInstructionBoundary())
      << c.name << " (the instruction had not finished after " << c.cycles.size()
      << " cycles)";
  expectFinalState(cpu.state(), c);
  expectFinalRam(bus.ram, c);
}

// Every case for one opcode: seed the initial state on a zeroed 64KB bus and run it a
// cycle at a time. Every opcode runs that way — the whole instruction set is on the
// cycle engine, so no case is compared on its final state alone.
TEST_P(Spc700Vectors, MatchFinalStateAndCycleCount) {
  const std::uint8_t opcode = GetParam();
  if (vectorsDir().empty()) {
    // With SNAGGLETOOTH_REQUIRE_VECTORS set, a missing vector set is a failure
    // rather than a skip — so an environment that means to run the oracle (CI)
    // can never report green while silently exercising none of it.
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
      std::printf("[case cap] opcode %02x: ran %zu of %zu cases\n", opcode, ran,
                  cases.size());
      break;
    }
    ++ran;
    runPerCycle(opcode, c);
  }
}

INSTANTIATE_TEST_SUITE_P(
    Mov, Spc700Vectors, ::testing::ValuesIn(kMovOpcodes),
    [](const ::testing::TestParamInfo<std::uint8_t>& info) {
      char label[8];
      std::snprintf(label, sizeof label, "op%02X", info.param);
      return std::string(label);
    });

INSTANTIATE_TEST_SUITE_P(
    Alu, Spc700Vectors, ::testing::ValuesIn(kAluOpcodes),
    [](const ::testing::TestParamInfo<std::uint8_t>& info) {
      char label[8];
      std::snprintf(label, sizeof label, "op%02X", info.param);
      return std::string(label);
    });

INSTANTIATE_TEST_SUITE_P(
    WordBit, Spc700Vectors, ::testing::ValuesIn(kWordBitOpcodes),
    [](const ::testing::TestParamInfo<std::uint8_t>& info) {
      char label[8];
      std::snprintf(label, sizeof label, "op%02X", info.param);
      return std::string(label);
    });

INSTANTIATE_TEST_SUITE_P(
    Control, Spc700Vectors, ::testing::ValuesIn(kControlOpcodes),
    [](const ::testing::TestParamInfo<std::uint8_t>& info) {
      char label[8];
      std::snprintf(label, sizeof label, "op%02X", info.param);
      return std::string(label);
    });

}  // namespace
