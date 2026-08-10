#include "snaggletooth/cpu/cpu65816.h"
#include "vector_harness.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_set>
#include <vector>

#ifndef SNAGGLETOOTH_65816_VECTORS
#define SNAGGLETOOTH_65816_VECTORS ""
#endif

namespace {

using snaggletooth::Cpu65816;
using snaggletooth::Cpu65816State;
using snaggletooth::cpu_vectors::RecordingBus;
using snaggletooth::cpu_vectors::RegState;
using snaggletooth::cpu_vectors::VectorCase;

std::string vectorsDir() { return SNAGGLETOOTH_65816_VECTORS; }

// One vector file: an opcode in one processor mode. Native and emulated cases live
// in separate files (NN.n.json / NN.e.json), so the suite runs one instance per
// opcode per mode.
struct VectorParam {
  std::uint8_t opcode;
  char mode;  // 'n' native, 'e' emulated
};

std::string opcodeFile(const VectorParam& param) {
  char name[16];
  std::snprintf(name, sizeof name, "/%02x.%c.json", param.opcode, param.mode);
  return vectorsDir() + name;
}

// An optional per-file case cap for a fast dev loop. Zero (unset) runs every case;
// a positive value truncates and prints what it dropped.
std::size_t caseCap() {
  const char* raw = std::getenv("SNAGGLETOOTH_65816_CASE_CAP");
  if (raw == nullptr) return 0;
  long v = std::strtol(raw, nullptr, 10);
  return v > 0 ? static_cast<std::size_t>(v) : 0;
}

// The load/store and register-transfer family this sub-block lands: LDA/LDX/LDY,
// STA/STX/STY/STZ across every addressing mode, the register-to-register transfers
// (including the 16-bit direct/stack transfers), XBA and XCE. Later sub-blocks
// extend this list until it covers every opcode.
constexpr std::uint8_t kLoadStoreTransferOpcodes[] = {
    // LDA (all modes)
    0xA9, 0xA5, 0xB5, 0xAD, 0xBD, 0xB9, 0xAF, 0xBF, 0xA1, 0xB1, 0xB2, 0xA7, 0xB7, 0xA3, 0xB3,
    // LDX / LDY
    0xA2, 0xA6, 0xB6, 0xAE, 0xBE, 0xA0, 0xA4, 0xB4, 0xAC, 0xBC,
    // STA (all modes)
    0x85, 0x95, 0x8D, 0x9D, 0x99, 0x8F, 0x9F, 0x81, 0x91, 0x92, 0x87, 0x97, 0x83, 0x93,
    // STX / STY
    0x86, 0x96, 0x8E, 0x84, 0x94, 0x8C,
    // STZ
    0x64, 0x74, 0x9C, 0x9E,
    // register transfers
    0xAA, 0xA8, 0xBA, 0x8A, 0x98, 0x9B, 0xBB, 0x9A,
    // 16-bit transfers to/from the direct and stack registers
    0x5B, 0x7B, 0x3B, 0x1B,
    // byte exchange and mode exchange
    0xEB, 0xFB,
};

std::vector<VectorParam> loadStoreTransferParams() {
  std::vector<VectorParam> params;
  for (std::uint8_t opcode : kLoadStoreTransferOpcodes) {
    params.push_back({opcode, 'n'});
    params.push_back({opcode, 'e'});
  }
  return params;
}

// The arithmetic and logic family this sub-block lands: ADC/SBC and the three
// bitwise operators across every accumulator addressing mode, the CMP/CPX/CPY
// comparisons, and BIT (whose flags depend on its addressing mode).
constexpr std::uint8_t kArithmeticLogicOpcodes[] = {
    // ADC (all modes)
    0x69, 0x65, 0x75, 0x6D, 0x7D, 0x79, 0x6F, 0x7F, 0x61, 0x71, 0x72, 0x67, 0x77, 0x63, 0x73,
    // SBC (all modes)
    0xE9, 0xE5, 0xF5, 0xED, 0xFD, 0xF9, 0xEF, 0xFF, 0xE1, 0xF1, 0xF2, 0xE7, 0xF7, 0xE3, 0xF3,
    // CMP (all modes)
    0xC9, 0xC5, 0xD5, 0xCD, 0xDD, 0xD9, 0xCF, 0xDF, 0xC1, 0xD1, 0xD2, 0xC7, 0xD7, 0xC3, 0xD3,
    // CPX / CPY
    0xE0, 0xE4, 0xEC, 0xC0, 0xC4, 0xCC,
    // AND (all modes)
    0x29, 0x25, 0x35, 0x2D, 0x3D, 0x39, 0x2F, 0x3F, 0x21, 0x31, 0x32, 0x27, 0x37, 0x23, 0x33,
    // EOR (all modes)
    0x49, 0x45, 0x55, 0x4D, 0x5D, 0x59, 0x4F, 0x5F, 0x41, 0x51, 0x52, 0x47, 0x57, 0x43, 0x53,
    // ORA (all modes)
    0x09, 0x05, 0x15, 0x0D, 0x1D, 0x19, 0x0F, 0x1F, 0x01, 0x11, 0x12, 0x07, 0x17, 0x03, 0x13,
    // BIT
    0x89, 0x24, 0x2C, 0x34, 0x3C,
};

std::vector<VectorParam> arithmeticLogicParams() {
  std::vector<VectorParam> params;
  for (std::uint8_t opcode : kArithmeticLogicOpcodes) {
    params.push_back({opcode, 'n'});
    params.push_back({opcode, 'e'});
  }
  return params;
}

Cpu65816State stateOf(const RegState& r) {
  return Cpu65816State{.pc = r.pc,
                       .s = r.s,
                       .a = r.a,
                       .x = r.x,
                       .y = r.y,
                       .d = r.d,
                       .p = r.p,
                       .dbr = r.dbr,
                       .pbr = r.pbr,
                       .e = r.e};
}

class Cpu65816Vectors : public ::testing::TestWithParam<VectorParam> {};

// Every case for one opcode in one mode: seed the initial state on a recording
// bus, step once, and demand the exact final registers, the exact final RAM, the
// documented cycle count, and that no write landed outside the addresses the case
// accounts for (a stray write cannot hide even though the 16 MB space is sparse).
TEST_P(Cpu65816Vectors, MatchFinalStateAndCycleCount) {
  const VectorParam param = GetParam();
  if (vectorsDir().empty()) {
    // With SNAGGLETOOTH_REQUIRE_65816_VECTORS set, a missing vector set is a
    // failure rather than a skip — so an environment that means to run the oracle
    // (CI) can never report green while silently exercising none of it.
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
      std::printf("[case cap] %s: ran %zu of %zu cases\n", path.c_str(), ran,
                  cases.size());
      break;
    }
    ++ran;

    RecordingBus bus;
    for (const auto& [address, value] : c.initial.ram) bus.mem[address] = value;

    Cpu65816 cpu(stateOf(c.initial));
    const std::uint32_t cycles = cpu.step(bus);

    const Cpu65816State& s = cpu.state();
    EXPECT_EQ(int{s.pc}, int{c.final_.pc}) << c.name << " (pc)";
    EXPECT_EQ(int{s.s}, int{c.final_.s}) << c.name << " (s)";
    EXPECT_EQ(int{s.a}, int{c.final_.a}) << c.name << " (a)";
    EXPECT_EQ(int{s.x}, int{c.final_.x}) << c.name << " (x)";
    EXPECT_EQ(int{s.y}, int{c.final_.y}) << c.name << " (y)";
    EXPECT_EQ(int{s.d}, int{c.final_.d}) << c.name << " (d)";
    EXPECT_EQ(int{s.p}, int{c.final_.p}) << c.name << " (p)";
    EXPECT_EQ(int{s.dbr}, int{c.final_.dbr}) << c.name << " (dbr)";
    EXPECT_EQ(int{s.pbr}, int{c.final_.pbr}) << c.name << " (pbr)";
    EXPECT_EQ(s.e, c.final_.e) << c.name << " (e)";
    EXPECT_EQ(cycles, c.cycles) << c.name << " (cycle count)";

    // (a) every address the final state accounts for holds its listed value.
    std::unordered_set<std::uint32_t> accounted;
    for (const auto& [address, value] : c.final_.ram) {
      EXPECT_EQ(int{bus.read(address)}, int{value}) << c.name << " (ram " << address << ")";
      accounted.insert(address);
    }
    // (c) an initial cell the final state does not re-list keeps its initial value.
    for (const auto& [address, value] : c.initial.ram) {
      if (accounted.count(address) == 0) {
        EXPECT_EQ(int{bus.read(address)}, int{value})
            << c.name << " (untouched ram " << address << ")";
        accounted.insert(address);
      }
    }
    // (b) no write landed outside the addresses the case accounts for.
    for (std::uint32_t address : bus.writes) {
      EXPECT_NE(accounted.count(address), 0u) << c.name << " (stray write " << address << ")";
    }
  }
}

INSTANTIATE_TEST_SUITE_P(
    LoadStoreTransfer, Cpu65816Vectors,
    ::testing::ValuesIn(loadStoreTransferParams()),
    [](const ::testing::TestParamInfo<VectorParam>& info) {
      char label[16];
      std::snprintf(label, sizeof label, "op%02X_%c", info.param.opcode,
                    info.param.mode);
      return std::string(label);
    });

INSTANTIATE_TEST_SUITE_P(
    ArithmeticLogic, Cpu65816Vectors,
    ::testing::ValuesIn(arithmeticLogicParams()),
    [](const ::testing::TestParamInfo<VectorParam>& info) {
      char label[16];
      std::snprintf(label, sizeof label, "op%02X_%c", info.param.opcode,
                    info.param.mode);
      return std::string(label);
    });

}  // namespace
