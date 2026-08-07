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

using snaggletooth::kFlagC;
using snaggletooth::kFlagH;
using snaggletooth::kFlagN;
using snaggletooth::kFlagV;
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

// ── ADC A,#imm (88) — table row "NV..H.ZC" ──────────────────────────────────
// H is the carry between the low and high nibble: $08 + $08 = $10 sets it, and
// this pure add (carry in clear) sets neither V, N, Z nor C.
TEST(AdcFlags, HalfCarrySetOnNibbleBoundary) {
  FlatRamBus bus;
  const Spc700 cpu =
      run1(Spc700State{.pc = 0x0200, .a = 0x08, .psw = 0}, {0x88, 0x08}, bus);
  EXPECT_EQ(cpu.state().a, 0x10);
  EXPECT_EQ(cpu.state().psw & kFlagH, kFlagH);
  EXPECT_EQ(cpu.state().psw & kFlagC, 0u);
  EXPECT_EQ(cpu.state().psw & kFlagV, 0u);
}

// V is signed overflow: $7F + $01 crosses into negative, setting V and N; the
// low nibble also carries, so H is set too. No unsigned carry out.
TEST(AdcFlags, SignedOverflowSetsVAndNegative) {
  FlatRamBus bus;
  const Spc700 cpu =
      run1(Spc700State{.pc = 0x0200, .a = 0x7F, .psw = 0}, {0x88, 0x01}, bus);
  EXPECT_EQ(cpu.state().a, 0x80);
  EXPECT_EQ(cpu.state().psw & kFlagV, kFlagV);
  EXPECT_EQ(cpu.state().psw & kFlagN, kFlagN);
  EXPECT_EQ(cpu.state().psw & kFlagC, 0u);
}

// C is the carry out of bit 7: $FF + $01 wraps to $00, setting C and Z.
TEST(AdcFlags, CarryOutSetsCarryAndZero) {
  FlatRamBus bus;
  const Spc700 cpu =
      run1(Spc700State{.pc = 0x0200, .a = 0xFF, .psw = 0}, {0x88, 0x01}, bus);
  EXPECT_EQ(cpu.state().a, 0x00);
  EXPECT_EQ(cpu.state().psw & kFlagC, kFlagC);
  EXPECT_EQ(cpu.state().psw & kFlagZ, kFlagZ);
}

// ── SBC A,#imm (A8) — table row "NV..H.ZC" ──────────────────────────────────
// Carry is "no borrow": with carry set going in (no incoming borrow),
// $50 - $30 = $20 leaves carry set because $50 >= $30.
TEST(SbcFlags, NoBorrowKeepsCarrySet) {
  FlatRamBus bus;
  const Spc700 cpu = run1(Spc700State{.pc = 0x0200, .a = 0x50, .psw = kFlagC},
                          {0xA8, 0x30}, bus);
  EXPECT_EQ(cpu.state().a, 0x20);
  EXPECT_EQ(cpu.state().psw & kFlagC, kFlagC);
  EXPECT_EQ(cpu.state().psw & kFlagN, 0u);
}

// A borrow clears carry: $30 - $50 underflows to $E0, clearing C and setting N.
TEST(SbcFlags, BorrowClearsCarry) {
  FlatRamBus bus;
  const Spc700 cpu = run1(Spc700State{.pc = 0x0200, .a = 0x30, .psw = kFlagC},
                          {0xA8, 0x50}, bus);
  EXPECT_EQ(cpu.state().a, 0xE0);
  EXPECT_EQ(cpu.state().psw & kFlagC, 0u);
  EXPECT_EQ(cpu.state().psw & kFlagN, kFlagN);
}

// V on a subtract: $80 - $01 = $7F crosses from negative to positive, so the
// signed result overflows and V is set.
TEST(SbcFlags, SignedOverflowSetsV) {
  FlatRamBus bus;
  const Spc700 cpu = run1(Spc700State{.pc = 0x0200, .a = 0x80, .psw = kFlagC},
                          {0xA8, 0x01}, bus);
  EXPECT_EQ(cpu.state().a, 0x7F);
  EXPECT_EQ(cpu.state().psw & kFlagV, kFlagV);
  EXPECT_EQ(cpu.state().psw & kFlagN, 0u);
}

// ── CMP A,#imm (68) — table row "N.....ZC" ──────────────────────────────────
// Equal operands set Z and, since there is no borrow, C; the result A keeps its
// value (compare writes no operand).
TEST(CmpFlags, EqualSetsZeroAndCarry) {
  FlatRamBus bus;
  const Spc700 cpu =
      run1(Spc700State{.pc = 0x0200, .a = 0x50, .psw = 0}, {0x68, 0x50}, bus);
  EXPECT_EQ(cpu.state().a, 0x50);
  EXPECT_EQ(cpu.state().psw & kFlagZ, kFlagZ);
  EXPECT_EQ(cpu.state().psw & kFlagC, kFlagC);
}

// A < operand borrows, so carry clears; $40 - $50 = $F0 sets N.
TEST(CmpFlags, LessThanClearsCarry) {
  FlatRamBus bus;
  const Spc700 cpu =
      run1(Spc700State{.pc = 0x0200, .a = 0x40, .psw = 0}, {0x68, 0x50}, bus);
  EXPECT_EQ(cpu.state().psw & kFlagC, 0u);
  EXPECT_EQ(cpu.state().psw & kFlagN, kFlagN);
  EXPECT_EQ(cpu.state().psw & kFlagZ, 0u);
}

// A > operand: no borrow (C set), non-zero (Z clear), positive result (N clear).
TEST(CmpFlags, GreaterThanSetsCarryClearsZero) {
  FlatRamBus bus;
  const Spc700 cpu =
      run1(Spc700State{.pc = 0x0200, .a = 0x60, .psw = 0}, {0x68, 0x50}, bus);
  EXPECT_EQ(cpu.state().psw & kFlagC, kFlagC);
  EXPECT_EQ(cpu.state().psw & kFlagZ, 0u);
  EXPECT_EQ(cpu.state().psw & kFlagN, 0u);
}

// ── Shift / rotate — table rows "N.....ZC" ──────────────────────────────────
// ASL A (1C) shifts bit 7 into carry: $81 -> $02, carry set, result positive.
TEST(ShiftFlags, AslCarryFromBitSeven) {
  FlatRamBus bus;
  const Spc700 cpu =
      run1(Spc700State{.pc = 0x0200, .a = 0x81, .psw = 0}, {0x1C}, bus);
  EXPECT_EQ(cpu.state().a, 0x02);
  EXPECT_EQ(cpu.state().psw & kFlagC, kFlagC);
  EXPECT_EQ(cpu.state().psw & kFlagN, 0u);
}

// LSR A (5C) shifts bit 0 into carry and a zero into bit 7, so N always clears:
// $FF -> $7F, carry set, negative clear.
TEST(ShiftFlags, LsrCarryFromBitZeroAlwaysClearsNegative) {
  FlatRamBus bus;
  const Spc700 cpu =
      run1(Spc700State{.pc = 0x0200, .a = 0xFF, .psw = kFlagN}, {0x5C}, bus);
  EXPECT_EQ(cpu.state().a, 0x7F);
  EXPECT_EQ(cpu.state().psw & kFlagC, kFlagC);
  EXPECT_EQ(cpu.state().psw & kFlagN, 0u);
}

// ROL A (3C) feeds the old carry into bit 0 while bit 7 leaves into carry:
// $80 with carry in -> $01, carry set.
TEST(ShiftFlags, RolFeedsCarryThroughBitZero) {
  FlatRamBus bus;
  const Spc700 cpu =
      run1(Spc700State{.pc = 0x0200, .a = 0x80, .psw = kFlagC}, {0x3C}, bus);
  EXPECT_EQ(cpu.state().a, 0x01);
  EXPECT_EQ(cpu.state().psw & kFlagC, kFlagC);
  EXPECT_EQ(cpu.state().psw & kFlagN, 0u);
}

// ROR A (7C) feeds the old carry into bit 7 while bit 0 leaves into carry:
// $01 with carry in -> $80, carry set, result negative.
TEST(ShiftFlags, RorFeedsCarryThroughBitSeven) {
  FlatRamBus bus;
  const Spc700 cpu =
      run1(Spc700State{.pc = 0x0200, .a = 0x01, .psw = kFlagC}, {0x7C}, bus);
  EXPECT_EQ(cpu.state().a, 0x80);
  EXPECT_EQ(cpu.state().psw & kFlagC, kFlagC);
  EXPECT_EQ(cpu.state().psw & kFlagN, kFlagN);
}

// ── XCN A (9F) — table row "N.....Z." ───────────────────────────────────────
// Nibble exchange: $08 -> $80 raises N from the swapped-up high nibble and
// touches no carry.
TEST(XcnFlags, ExchangeNibblesSetsNegativeFromHighNibble) {
  FlatRamBus bus;
  const Spc700 cpu =
      run1(Spc700State{.pc = 0x0200, .a = 0x08, .psw = 0}, {0x9F}, bus);
  EXPECT_EQ(cpu.state().a, 0x80);
  EXPECT_EQ(cpu.state().psw & kFlagN, kFlagN);
  EXPECT_EQ(cpu.state().psw & kFlagC, 0u);
}

}  // namespace
