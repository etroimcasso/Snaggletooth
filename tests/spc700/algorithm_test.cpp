#include "snaggletooth/apu/spc700.h"
#include "vector_harness.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <initializer_list>

// Doc-derived cross-checks on the MOV family's flag behavior, hand-derived from
// the SNESdev instruction-set table's flag columns and independent of the
// emulator-generated vectors. The vectors prove the whole result; these pin the
// one algorithmic decision a move makes — whether, and how, it touches N and Z.

namespace {

using snaggletooth::kFlagN;
using snaggletooth::kFlagZ;
using snaggletooth::Spc700;
using snaggletooth::Spc700State;
using snaggletooth::test::FlatRamBus;

// Lays `program` at the state's PC on a fresh flat bus, steps once, returns the
// resulting CPU. The bus is provided by the caller so stores can be inspected.
Spc700 run1(Spc700State init, std::initializer_list<std::uint8_t> program,
            FlatRamBus& bus) {
  std::uint16_t at = init.pc;
  for (std::uint8_t byte : program) bus.ram[at++] = byte;
  Spc700 cpu(init);
  cpu.step(bus);
  return cpu;
}

// Table: "8-bit move memory to register" — flags "N.....Z.".
// MOV A,#$00 sets Z, clears N; the flags going in (here N) do not survive.
TEST(MovFlags, LoadImmediateZeroSetsZeroClearsNegative) {
  FlatRamBus bus;
  const Spc700 cpu = run1(Spc700State{.pc = 0x0200, .psw = kFlagN}, {0xE8, 0x00}, bus);
  EXPECT_EQ(cpu.state().a, 0x00);
  EXPECT_EQ(cpu.state().psw & kFlagZ, kFlagZ);
  EXPECT_EQ(cpu.state().psw & kFlagN, 0u);
}

// MOV A,#$80 sets N (high bit), clears Z.
TEST(MovFlags, LoadImmediateHighBitSetsNegativeClearsZero) {
  FlatRamBus bus;
  const Spc700 cpu = run1(Spc700State{.pc = 0x0200, .psw = kFlagZ}, {0xE8, 0x80}, bus);
  EXPECT_EQ(cpu.state().a, 0x80);
  EXPECT_EQ(cpu.state().psw & kFlagN, kFlagN);
  EXPECT_EQ(cpu.state().psw & kFlagZ, 0u);
}

// MOV A,#$01 clears both N and Z.
TEST(MovFlags, LoadImmediateMidValueClearsBoth) {
  FlatRamBus bus;
  const Spc700 cpu = run1(
      Spc700State{.pc = 0x0200, .psw = static_cast<std::uint8_t>(kFlagN | kFlagZ)},
      {0xE8, 0x01}, bus);
  EXPECT_EQ(cpu.state().psw & kFlagN, 0u);
  EXPECT_EQ(cpu.state().psw & kFlagZ, 0u);
}

// MOV Y,#$FF (8D) drives N,Z from Y the same way.
TEST(MovFlags, LoadYImmediateHighBitSetsNegative) {
  FlatRamBus bus;
  const Spc700 cpu = run1(Spc700State{.pc = 0x0200}, {0x8D, 0xFF}, bus);
  EXPECT_EQ(cpu.state().y, 0xFF);
  EXPECT_EQ(cpu.state().psw & kFlagN, kFlagN);
}

// Table: "8-bit move register to memory" — flags "........".
// MOV dp,A (C4) writes A and touches no flag, even when A is zero.
TEST(MovFlags, StoreLeavesFlagsUntouched) {
  FlatRamBus bus;
  const Spc700 cpu =
      run1(Spc700State{.pc = 0x0200, .a = 0x00, .psw = 0}, {0xC4, 0x10}, bus);
  EXPECT_EQ(bus.ram[0x0010], 0x00);
  EXPECT_EQ(cpu.state().psw, 0u);
}

// MOV SP,X (BD) is a register move that, uniquely, sets NO flags — table
// row "........". A zero moved into SP must not raise Z.
TEST(MovFlags, MoveXToStackPointerLeavesFlagsUntouched) {
  FlatRamBus bus;
  const Spc700 cpu =
      run1(Spc700State{.pc = 0x0200, .x = 0x00, .psw = 0}, {0xBD}, bus);
  EXPECT_EQ(cpu.state().sp, 0x00);
  EXPECT_EQ(cpu.state().psw, 0u);
}

// MOV X,SP (9D), by contrast, DOES set N,Z — table row "N.....Z.".
TEST(MovFlags, MoveStackPointerToXSetsZeroWhenZero) {
  FlatRamBus bus;
  const Spc700 cpu = run1(
      Spc700State{.pc = 0x0200, .x = 0x7F, .sp = 0x00, .psw = 0}, {0x9D}, bus);
  EXPECT_EQ(cpu.state().x, 0x00);
  EXPECT_EQ(cpu.state().psw & kFlagZ, kFlagZ);
}

}  // namespace
