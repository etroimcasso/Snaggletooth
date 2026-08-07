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

// ── MUL YA (CF) — table row "N.....Z.", "NZ on Y only" ───────────────────────
// The product is 16 bits (Y=high, A=low), but N and Z read ONLY the high byte Y.
// Y=$02 * A=$03 = $0006, so A=$06 is non-zero yet Z is set — because Y=$00.
TEST(MulFlags, ZeroFlagFromHighByteEvenWhenLowNonzero) {
  FlatRamBus bus;
  const Spc700 cpu =
      run1(Spc700State{.pc = 0x0200, .a = 0x03, .y = 0x02, .psw = 0}, {0xCF}, bus);
  EXPECT_EQ(cpu.state().a, 0x06);
  EXPECT_EQ(cpu.state().y, 0x00);
  EXPECT_EQ(cpu.state().psw & kFlagZ, kFlagZ);
}

// The converse: Y=$10 * A=$10 = $0100, so A=$00 yet Z is clear — because Y=$01.
TEST(MulFlags, ZeroFlagClearWhenHighByteNonzeroEvenIfLowIsZero) {
  FlatRamBus bus;
  const Spc700 cpu =
      run1(Spc700State{.pc = 0x0200, .a = 0x10, .y = 0x10, .psw = 0}, {0xCF}, bus);
  EXPECT_EQ(cpu.state().a, 0x00);
  EXPECT_EQ(cpu.state().y, 0x01);
  EXPECT_EQ(cpu.state().psw & kFlagZ, 0u);
}

// N comes from Y's high bit: $FF * $FF = $FE01, so Y=$FE sets N.
TEST(MulFlags, NegativeFromHighByte) {
  FlatRamBus bus;
  const Spc700 cpu =
      run1(Spc700State{.pc = 0x0200, .a = 0xFF, .y = 0xFF, .psw = 0}, {0xCF}, bus);
  EXPECT_EQ(cpu.state().y, 0xFE);
  EXPECT_EQ(cpu.state().a, 0x01);
  EXPECT_EQ(cpu.state().psw & kFlagN, kFlagN);
  EXPECT_EQ(cpu.state().psw & kFlagZ, 0u);
}

// ── DIV YA,X (9E) — table row "NV..H.Z.", the documented quirks ──────────────
// The quotient lands in A, the remainder in Y. $0011 / $05 = 3 r 2, and with the
// quotient inside a byte V stays clear.
TEST(DivFlags, QuotientToAccumulatorRemainderToY) {
  FlatRamBus bus;
  const Spc700 cpu = run1(
      Spc700State{.pc = 0x0200, .a = 0x11, .x = 0x05, .y = 0x00, .psw = 0}, {0x9E},
      bus);
  EXPECT_EQ(cpu.state().a, 0x03);
  EXPECT_EQ(cpu.state().y, 0x02);
  EXPECT_EQ(cpu.state().psw & kFlagV, 0u);
}

// V holds bit 8 of the quotient: $0200 / $02 = $100, which does not fit in A, so
// V is set (A keeps the low 8 bits, $00).
TEST(DivFlags, OverflowBitEightSetsV) {
  FlatRamBus bus;
  const Spc700 cpu = run1(
      Spc700State{.pc = 0x0200, .a = 0x00, .x = 0x02, .y = 0x02, .psw = 0}, {0x9E},
      bus);
  EXPECT_EQ(cpu.state().a, 0x00);
  EXPECT_EQ(cpu.state().psw & kFlagV, kFlagV);
}

// H is the odd one: it is set from the entry nibbles when (X&$F) <= (Y&$F),
// independent of the division itself. X=$03, Y=$05: 3 <= 5, so H is set.
TEST(DivFlags, HalfCarrySetWhenDivisorNibbleAtMostDividendNibble) {
  FlatRamBus bus;
  const Spc700 cpu = run1(
      Spc700State{.pc = 0x0200, .a = 0x00, .x = 0x03, .y = 0x05, .psw = 0}, {0x9E},
      bus);
  EXPECT_EQ(cpu.state().psw & kFlagH, kFlagH);
}

// And clear when the divisor nibble is larger: X=$05, Y=$03: 5 <= 3 is false.
TEST(DivFlags, HalfCarryClearWhenDivisorNibbleLarger) {
  FlatRamBus bus;
  const Spc700 cpu = run1(
      Spc700State{.pc = 0x0200, .a = 0x00, .x = 0x05, .y = 0x03, .psw = 0}, {0x9E},
      bus);
  EXPECT_EQ(cpu.state().psw & kFlagH, 0u);
}

// ── DAA A (DF) — "if A>$99 or C, +$60 & set C; then if (A&$F)>9 or H, +$06" ───
// Low nibble above 9 adds $06: $0B -> $11, no carry.
TEST(DaaFlags, LowNibbleAboveNineAddsSix) {
  FlatRamBus bus;
  const Spc700 cpu =
      run1(Spc700State{.pc = 0x0200, .a = 0x0B, .psw = 0}, {0xDF}, bus);
  EXPECT_EQ(cpu.state().a, 0x11);
  EXPECT_EQ(cpu.state().psw & kFlagC, 0u);
}

// The half-carry forces the $06 even when the low nibble is <= 9: $05 with H set
// becomes $0B.
TEST(DaaFlags, HalfCarryForcesAddSix) {
  FlatRamBus bus;
  const Spc700 cpu =
      run1(Spc700State{.pc = 0x0200, .a = 0x05, .psw = kFlagH}, {0xDF}, bus);
  EXPECT_EQ(cpu.state().a, 0x0B);
}

// A above $99 adds $60 and sets carry: $A5 -> $05 with C set.
TEST(DaaFlags, HighByteAboveNinetyNineAddsSixtyAndSetsCarry) {
  FlatRamBus bus;
  const Spc700 cpu =
      run1(Spc700State{.pc = 0x0200, .a = 0xA5, .psw = 0}, {0xDF}, bus);
  EXPECT_EQ(cpu.state().a, 0x05);
  EXPECT_EQ(cpu.state().psw & kFlagC, kFlagC);
}

// ── DAS A (BE) — "if A>$99 or !C, -$60 & clear C; then if (A&$F)>9 or !H, -$06" ─
// A half-borrow (H clear) forces the $06 subtract: $40 -> $3A, carry stays set.
TEST(DasFlags, HalfBorrowForcesSubtractSix) {
  FlatRamBus bus;
  const Spc700 cpu =
      run1(Spc700State{.pc = 0x0200, .a = 0x40, .psw = kFlagC}, {0xBE}, bus);
  EXPECT_EQ(cpu.state().a, 0x3A);
  EXPECT_EQ(cpu.state().psw & kFlagC, kFlagC);
}

// A borrow (C clear) subtracts $60 and clears carry: $40 -> $E0.
TEST(DasFlags, BorrowSubtractsSixtyAndClearsCarry) {
  FlatRamBus bus;
  const Spc700 cpu =
      run1(Spc700State{.pc = 0x0200, .a = 0x40, .psw = kFlagH}, {0xBE}, bus);
  EXPECT_EQ(cpu.state().a, 0xE0);
  EXPECT_EQ(cpu.state().psw & kFlagC, 0u);
}

// ── ADDW YA,dp (7A) — table row "NV..H.ZC", "H on high byte" ─────────────────
// The half-carry is the nibble carry of the HIGH byte: $0800 + $0800 = $1000
// sets H ($8 + $8 crosses the nibble at bit 11), with no carry out.
TEST(AddwFlags, HalfCarrySetFromHighByte) {
  FlatRamBus bus;
  bus.ram[0x0010] = 0x00;
  bus.ram[0x0011] = 0x08;
  const Spc700 cpu = run1(
      Spc700State{.pc = 0x0200, .a = 0x00, .y = 0x08, .psw = 0}, {0x7A, 0x10}, bus);
  EXPECT_EQ(cpu.state().a, 0x00);
  EXPECT_EQ(cpu.state().y, 0x10);
  EXPECT_EQ(cpu.state().psw & kFlagH, kFlagH);
  EXPECT_EQ(cpu.state().psw & kFlagC, 0u);
}

// A nibble carry in the LOW byte does not set H: $0008 + $0008 = $0010 leaves H
// clear, proving H tracks the high byte only.
TEST(AddwFlags, HalfCarryIsFromHighByteNotLow) {
  FlatRamBus bus;
  bus.ram[0x0010] = 0x08;
  bus.ram[0x0011] = 0x00;
  const Spc700 cpu = run1(
      Spc700State{.pc = 0x0200, .a = 0x08, .y = 0x00, .psw = 0}, {0x7A, 0x10}, bus);
  EXPECT_EQ(cpu.state().a, 0x10);
  EXPECT_EQ(cpu.state().y, 0x00);
  EXPECT_EQ(cpu.state().psw & kFlagH, 0u);
}

// Signed overflow of the 16-bit result: $7FFF + $0001 = $8000 sets V and N.
TEST(AddwFlags, SignedOverflowSetsVAndNegative) {
  FlatRamBus bus;
  bus.ram[0x0010] = 0x01;
  bus.ram[0x0011] = 0x00;
  const Spc700 cpu = run1(
      Spc700State{.pc = 0x0200, .a = 0xFF, .y = 0x7F, .psw = 0}, {0x7A, 0x10}, bus);
  EXPECT_EQ(cpu.state().a, 0x00);
  EXPECT_EQ(cpu.state().y, 0x80);
  EXPECT_EQ(cpu.state().psw & kFlagV, kFlagV);
  EXPECT_EQ(cpu.state().psw & kFlagN, kFlagN);
}

// Carry out of bit 15: $FFFF + $0001 = $0000 sets C and Z.
TEST(AddwFlags, CarryOutOfBitFifteenSetsCarryAndZero) {
  FlatRamBus bus;
  bus.ram[0x0010] = 0x01;
  bus.ram[0x0011] = 0x00;
  const Spc700 cpu = run1(
      Spc700State{.pc = 0x0200, .a = 0xFF, .y = 0xFF, .psw = 0}, {0x7A, 0x10}, bus);
  EXPECT_EQ(cpu.state().a, 0x00);
  EXPECT_EQ(cpu.state().y, 0x00);
  EXPECT_EQ(cpu.state().psw & kFlagC, kFlagC);
  EXPECT_EQ(cpu.state().psw & kFlagZ, kFlagZ);
}

// ── SUBW YA,dp (9A) — table row "NV..H.ZC", "H on high byte" ─────────────────
// No borrow sets carry: $0500 - $0300 = $0200, C set, positive.
TEST(SubwFlags, NoBorrowSetsCarry) {
  FlatRamBus bus;
  bus.ram[0x0010] = 0x00;
  bus.ram[0x0011] = 0x03;
  const Spc700 cpu = run1(
      Spc700State{.pc = 0x0200, .a = 0x00, .y = 0x05, .psw = 0}, {0x9A, 0x10}, bus);
  EXPECT_EQ(cpu.state().a, 0x00);
  EXPECT_EQ(cpu.state().y, 0x02);
  EXPECT_EQ(cpu.state().psw & kFlagC, kFlagC);
}

// A borrow clears carry: $0300 - $0500 = $FE00, C clear, N set.
TEST(SubwFlags, BorrowClearsCarry) {
  FlatRamBus bus;
  bus.ram[0x0010] = 0x00;
  bus.ram[0x0011] = 0x05;
  const Spc700 cpu = run1(
      Spc700State{.pc = 0x0200, .a = 0x00, .y = 0x03, .psw = 0}, {0x9A, 0x10}, bus);
  EXPECT_EQ(cpu.state().y, 0xFE);
  EXPECT_EQ(cpu.state().psw & kFlagC, 0u);
  EXPECT_EQ(cpu.state().psw & kFlagN, kFlagN);
}

// H is the half-carry of the high-byte subtract: $2000 - $1000 = $1000 sets it.
TEST(SubwFlags, HalfCarryFromHighByte) {
  FlatRamBus bus;
  bus.ram[0x0010] = 0x00;
  bus.ram[0x0011] = 0x10;
  const Spc700 cpu = run1(
      Spc700State{.pc = 0x0200, .a = 0x00, .y = 0x20, .psw = 0}, {0x9A, 0x10}, bus);
  EXPECT_EQ(cpu.state().a, 0x00);
  EXPECT_EQ(cpu.state().y, 0x10);
  EXPECT_EQ(cpu.state().psw & kFlagH, kFlagH);
  EXPECT_EQ(cpu.state().psw & kFlagC, kFlagC);
}

// ── CMPW YA,dp (5A) — table row "N.....ZC" ──────────────────────────────────
// Equal words set Z and (no borrow) C; YA is left untouched.
TEST(CmpwFlags, EqualSetsZeroAndCarryWithoutWriting) {
  FlatRamBus bus;
  bus.ram[0x0010] = 0x34;
  bus.ram[0x0011] = 0x12;
  const Spc700 cpu = run1(
      Spc700State{.pc = 0x0200, .a = 0x34, .y = 0x12, .psw = 0}, {0x5A, 0x10}, bus);
  EXPECT_EQ(cpu.state().a, 0x34);
  EXPECT_EQ(cpu.state().y, 0x12);
  EXPECT_EQ(cpu.state().psw & kFlagZ, kFlagZ);
  EXPECT_EQ(cpu.state().psw & kFlagC, kFlagC);
}

// YA below the operand borrows: $0100 - $0200 clears C and sets N (diff $FF00).
TEST(CmpwFlags, LessThanClearsCarrySetsNegative) {
  FlatRamBus bus;
  bus.ram[0x0010] = 0x00;
  bus.ram[0x0011] = 0x02;
  const Spc700 cpu = run1(
      Spc700State{.pc = 0x0200, .a = 0x00, .y = 0x01, .psw = 0}, {0x5A, 0x10}, bus);
  EXPECT_EQ(cpu.state().psw & kFlagC, 0u);
  EXPECT_EQ(cpu.state().psw & kFlagN, kFlagN);
}

// ── MOVW (BA / DA) — the N/Z are word-wide; the store sets nothing ───────────
// MOVW YA,dp: a $8000 word loads A=$00, Y=$80 and sets N; Z stays clear because
// the whole word, not the low byte A, decides Z.
TEST(MovwFlags, LoadNegativeFromBitFifteenZeroIsWordWide) {
  FlatRamBus bus;
  bus.ram[0x0010] = 0x00;
  bus.ram[0x0011] = 0x80;
  const Spc700 cpu =
      run1(Spc700State{.pc = 0x0200, .psw = 0}, {0xBA, 0x10}, bus);
  EXPECT_EQ(cpu.state().a, 0x00);
  EXPECT_EQ(cpu.state().y, 0x80);
  EXPECT_EQ(cpu.state().psw & kFlagN, kFlagN);
  EXPECT_EQ(cpu.state().psw & kFlagZ, 0u);
}

// MOVW dp,YA writes both bytes and touches no flag — N survives a stored $0000.
TEST(MovwFlags, StoreWordSetsNoFlags) {
  FlatRamBus bus;
  const Spc700 cpu = run1(
      Spc700State{.pc = 0x0200, .a = 0x00, .y = 0x00, .psw = kFlagN}, {0xDA, 0x10},
      bus);
  EXPECT_EQ(bus.ram[0x0010], 0x00);
  EXPECT_EQ(bus.ram[0x0011], 0x00);
  EXPECT_EQ(cpu.state().psw, kFlagN);
}

// ── TSET1 / TCLR1 !abs (0E / 4E) — "N,Z as for A - old_value" ────────────────
// TSET1 sets in memory the bits set in A and takes N/Z from A - old: equal
// operands set Z, and memory becomes A|old.
TEST(BitFlags, Tset1SetsBitsAndFlagsFromEqualityTest) {
  FlatRamBus bus;
  bus.ram[0x0300] = 0x30;
  const Spc700 cpu = run1(Spc700State{.pc = 0x0200, .a = 0x30, .psw = 0},
                          {0x0E, 0x00, 0x03}, bus);
  EXPECT_EQ(bus.ram[0x0300], 0x30);
  EXPECT_EQ(cpu.state().psw & kFlagZ, kFlagZ);
}

// TCLR1 clears the bits set in A and takes the same equality flags.
TEST(BitFlags, Tclr1ClearsBitsAndFlagsFromEqualityTest) {
  FlatRamBus bus;
  bus.ram[0x0300] = 0x0F;
  const Spc700 cpu = run1(Spc700State{.pc = 0x0200, .a = 0x0F, .psw = 0},
                          {0x4E, 0x00, 0x03}, bus);
  EXPECT_EQ(bus.ram[0x0300], 0x00);
  EXPECT_EQ(cpu.state().psw & kFlagZ, kFlagZ);
}

// ── MOV1 (AA / CA) — the 13-bit address + 3-bit index encoding ───────────────
// MOV1 m.b,C writes carry into one memory bit. Operand $6300 = address $0300,
// bit 3 (top three bits): with C set, bit 3 of $0300 becomes 1 ($00 -> $08).
TEST(BitFlags, Mov1MemoryBitTakesCarry) {
  FlatRamBus bus;
  run1(Spc700State{.pc = 0x0200, .psw = kFlagC}, {0xCA, 0x00, 0x63}, bus);
  EXPECT_EQ(bus.ram[0x0300], 0x08);
}

// MOV1 C,m.b reads that same bit into carry: bit 3 of $0300 set -> C set.
TEST(BitFlags, CarryTakesMemoryBit) {
  FlatRamBus bus;
  bus.ram[0x0300] = 0x08;
  const Spc700 cpu = run1(Spc700State{.pc = 0x0200, .psw = 0},
                          {0xAA, 0x00, 0x63}, bus);
  EXPECT_EQ(cpu.state().psw & kFlagC, kFlagC);
}

}  // namespace
