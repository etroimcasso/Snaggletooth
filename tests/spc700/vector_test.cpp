#include "snaggletooth/apu/spc700.h"
#include "vector_harness.h"

#include <gtest/gtest.h>

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
using snaggletooth::test::FlatRamBus;
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
// SNESdev instruction-set table. Each sub-block extends this list to the families
// it lands; the suite is green at every commit.
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

class Spc700Vectors : public ::testing::TestWithParam<std::uint8_t> {};

// Every case for one opcode: seed the initial state on a zeroed 64KB bus, step
// once, and demand the exact final registers, the exact final RAM (a full-buffer
// compare, so stray writes cannot hide), and the documented cycle count.
TEST_P(Spc700Vectors, MatchFinalStateAndCycleCount) {
  const std::uint8_t opcode = GetParam();
  if (vectorsDir().empty()) {
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

    FlatRamBus bus;
    for (const auto& [address, value] : c.initial.ram) bus.ram[address] = value;

    Spc700 cpu(Spc700State{.pc = c.initial.pc,
                           .a = c.initial.a,
                           .x = c.initial.x,
                           .y = c.initial.y,
                           .sp = c.initial.sp,
                           .psw = c.initial.psw});
    const std::uint32_t cycles = cpu.step(bus);

    FlatRamBus expected;
    for (const auto& [address, value] : c.final_.ram) expected.ram[address] = value;

    const snaggletooth::Spc700State& s = cpu.state();
    EXPECT_EQ(s.pc, c.final_.pc) << c.name;
    EXPECT_EQ(int{s.a}, int{c.final_.a}) << c.name;
    EXPECT_EQ(int{s.x}, int{c.final_.x}) << c.name;
    EXPECT_EQ(int{s.y}, int{c.final_.y}) << c.name;
    EXPECT_EQ(int{s.sp}, int{c.final_.sp}) << c.name;
    EXPECT_EQ(int{s.psw}, int{c.final_.psw}) << c.name;
    EXPECT_EQ(cycles, c.cycles) << c.name << " (cycle count)";
    EXPECT_EQ(bus.ram, expected.ram) << c.name << " (ram)";
  }
}

INSTANTIATE_TEST_SUITE_P(
    Mov, Spc700Vectors, ::testing::ValuesIn(kMovOpcodes),
    [](const ::testing::TestParamInfo<std::uint8_t>& info) {
      char label[8];
      std::snprintf(label, sizeof label, "op%02X", info.param);
      return std::string(label);
    });

}  // namespace
