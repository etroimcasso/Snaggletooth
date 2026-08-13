#include "snaggletooth/cpu/cpu65816.h"
#include "vector_harness.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <initializer_list>
#include <utility>

// Doc-derived cross-checks for the 65816 width machinery — the corners the raw
// vectors also cover, re-derived independently from the instruction reference so a
// silent model error shows up here too. Each assertion cites the reference section
// it comes from. [65816] = the 6502.org 65C816 opcode reference.

namespace {

using snaggletooth::Cpu65816;
using snaggletooth::Cpu65816State;
using snaggletooth::kCpuFlagC;
using snaggletooth::kCpuFlagD;
using snaggletooth::kCpuFlagM;
using snaggletooth::kCpuFlagN;
using snaggletooth::kCpuFlagV;
using snaggletooth::kCpuFlagX;
using snaggletooth::kCpuFlagZ;
using snaggletooth::cpu_vectors::RecordingBus;

struct Result {
  Cpu65816State st;
  std::uint32_t cycles;
  RecordingBus bus;
};

// Runs one instruction from a starting state with the given memory seeded.
Result run(const Cpu65816State& init,
           std::initializer_list<std::pair<std::uint32_t, std::uint8_t>> ram) {
  RecordingBus bus;
  for (const auto& [address, value] : ram) bus.mem[address] = value;
  Cpu65816 cpu(init);
  const std::uint32_t cycles = cpu.stepInstruction(bus);
  return {cpu.state(), cycles, std::move(bus)};
}

// ---- load widths (§6.5) ----

TEST(Cpu65816Widths, EightBitLoadPreservesTheAccumulatorHighByte) {
  // m = 1: LDA #$FF changes only the low (A) byte, leaving B intact. [65816 §4/§6.5]
  const auto r = run({.pc = 0x1000, .a = 0x1234, .p = kCpuFlagM},
                     {{0x001000, 0xA9}, {0x001001, 0xFF}});
  EXPECT_EQ(r.st.a, 0x12FF);
  EXPECT_TRUE(r.st.p & kCpuFlagN);
  EXPECT_EQ(r.cycles, 2u);  // 3 - m
}

TEST(Cpu65816Widths, SixteenBitLoadReplacesTheWholeAccumulator) {
  // m = 0: LDA #$CDAB is a 16-bit load; the high bit sets N. [65816 §6.5 example]
  const auto r = run({.pc = 0x1000, .a = 0x1234, .p = 0},
                     {{0x001000, 0xA9}, {0x001001, 0xAB}, {0x001002, 0xCD}});
  EXPECT_EQ(r.st.a, 0xCDAB);
  EXPECT_TRUE(r.st.p & kCpuFlagN);
  EXPECT_FALSE(r.st.p & kCpuFlagZ);
  EXPECT_EQ(r.cycles, 3u);  // 3 - m
}

TEST(Cpu65816Widths, SixteenBitZeroLoadSetsZero) {
  const auto r = run({.pc = 0x1000, .a = 0x1234, .p = 0},
                     {{0x001000, 0xA9}, {0x001001, 0x00}, {0x001002, 0x00}});
  EXPECT_EQ(r.st.a, 0x0000);
  EXPECT_TRUE(r.st.p & kCpuFlagZ);
}

TEST(Cpu65816Widths, EightBitIndexLoadClearsTheIndexHighByte) {
  // x = 1: LDX #$FF loads eight bits and forces XH to zero. [65816 §4]
  const auto r = run({.pc = 0x1000, .x = 0x1234, .p = kCpuFlagX},
                     {{0x001000, 0xA2}, {0x001001, 0xFF}});
  EXPECT_EQ(r.st.x, 0x00FF);
}

// ---- direct-page addressing (§5.7) ----

TEST(Cpu65816Widths, EmulationDirectPageWrapsWithinThePage) {
  // e = 1, DL = 0, D = $FF00: LDA $FF reaches $00FFFF, not $00FFFF+carry. [65816 §5.7 ex.1]
  const auto r = run({.pc = 0x1000, .d = 0xFF00, .p = 0x30, .e = true},
                     {{0x001000, 0xA5}, {0x001001, 0xFF}, {0x00FFFF, 0x42}});
  EXPECT_EQ(r.st.a & 0xFF, 0x42u);
}

TEST(Cpu65816Widths, DirectPageDataWrapsWithinBankZero) {
  // Native, D = $FF00, m = 0: LDA $FF reads low from $00FFFF, high from $000000. [65816 §5.7 ex.2]
  const auto r = run({.pc = 0x1000, .d = 0xFF00, .p = 0},
                     {{0x001000, 0xA5}, {0x001001, 0xFF}, {0x00FFFF, 0xAB}, {0x000000, 0xCD}});
  EXPECT_EQ(r.st.a, 0xCDAB);
}

// ---- register transfers (§6.10.1) ----

TEST(Cpu65816Widths, TxaTakesTheAccumulatorWidthNotTheIndexWidth) {
  // m = 1, x = 0: TXA moves only XL into A; B is preserved. [65816 §6.10.1 example]
  const auto r = run({.pc = 0x1000, .a = 0x1234, .x = 0xABCD, .p = kCpuFlagM},
                     {{0x001000, 0x8A}});
  EXPECT_EQ(r.st.a, 0x12CD);
  EXPECT_TRUE(r.st.p & kCpuFlagN);
}

TEST(Cpu65816Widths, TaxTakesTheIndexWidth) {
  // m = 0, x = 1: TAX moves eight bits into X and clears XH. [65816 §6.10.1]
  const auto r = run({.pc = 0x1000, .a = 0x12CD, .p = kCpuFlagX},
                     {{0x001000, 0xAA}});
  EXPECT_EQ(r.st.x, 0x00CD);
  EXPECT_TRUE(r.st.p & kCpuFlagN);
}

TEST(Cpu65816Widths, TsxReadsTheStackAtTheIndexWidth) {
  const auto r = run({.pc = 0x1000, .s = 0x01FF, .p = kCpuFlagX},
                     {{0x001000, 0xBA}});
  EXPECT_EQ(r.st.x, 0x00FF);
}

TEST(Cpu65816Widths, TxsInEightBitNativeClearsTheStackHighByte) {
  // e = 0, x = 1: TXS transfers all 16 bits of X; XH is zero, so SH becomes $00.
  // No flags change. [65816 Appendix / §6.10.1]
  const auto r = run({.pc = 0x1000, .s = 0xFFFF, .x = 0x1234, .p = kCpuFlagX | kCpuFlagZ},
                     {{0x001000, 0x9A}});
  EXPECT_EQ(r.st.s, 0x0034);
  EXPECT_TRUE(r.st.p & kCpuFlagZ);  // unchanged — TXS sets no flags
}

TEST(Cpu65816Widths, TxsInEmulationKeepsTheStackOnPageOne) {
  const auto r = run({.pc = 0x1000, .s = 0x0100, .x = 0x0034, .p = 0x30, .e = true},
                     {{0x001000, 0x9A}});
  EXPECT_EQ(r.st.s, 0x0134);
}

// ---- 16-bit transfers (§6.10.2) ----

TEST(Cpu65816Widths, TcsTransfersSixteenBitsWithoutFlags) {
  // TCS moves the whole 16-bit accumulator to S regardless of m, and sets no flags.
  // [65816 §4 example / §6.10.2]
  const auto r = run({.pc = 0x1000, .s = 0x0000, .a = 0xABCD, .p = kCpuFlagM | kCpuFlagZ},
                     {{0x001000, 0x1B}});
  EXPECT_EQ(r.st.s, 0xABCD);
  EXPECT_TRUE(r.st.p & kCpuFlagZ);  // unchanged
}

TEST(Cpu65816Widths, TdcTransfersSixteenBitsAndSetsFlags) {
  const auto r = run({.pc = 0x1000, .d = 0x1234, .p = kCpuFlagM},
                     {{0x001000, 0x7B}});
  EXPECT_EQ(r.st.a, 0x1234);
  EXPECT_FALSE(r.st.p & kCpuFlagN);
  EXPECT_FALSE(r.st.p & kCpuFlagZ);
}

// ---- byte exchange (§6.10.3) ----

TEST(Cpu65816Widths, XbaSwapsTheAccumulatorHalvesAndSetsEightBitFlags) {
  // A = $6789 -> $8967; N and Z come from the new low byte ($67). [65816 §6.10.3 example]
  const auto r = run({.pc = 0x1000, .a = 0x6789, .p = kCpuFlagN}, {{0x001000, 0xEB}});
  EXPECT_EQ(r.st.a, 0x8967);
  EXPECT_FALSE(r.st.p & kCpuFlagN);
  EXPECT_FALSE(r.st.p & kCpuFlagZ);
  EXPECT_EQ(r.cycles, 3u);
}

// ---- mode exchange (§6.10.4) ----

TEST(Cpu65816Widths, XceEntersEmulationForcingWidthsStackAndIndexHighBytes) {
  // e 0 -> 1 (via C = 1): forces m = x = 1, clears the index high bytes, and pins
  // the stack to page one. C takes the old e (0). [65816 §6.10.4 / §4]
  const auto r = run({.pc = 0x1000, .s = 0xA8B9, .x = 0x1234, .y = 0x5678, .p = kCpuFlagC},
                     {{0x001000, 0xFB}});
  EXPECT_TRUE(r.st.e);
  EXPECT_FALSE(r.st.p & kCpuFlagC);
  EXPECT_TRUE(r.st.p & kCpuFlagM);
  EXPECT_TRUE(r.st.p & kCpuFlagX);
  EXPECT_EQ(r.st.x, 0x0034);
  EXPECT_EQ(r.st.y, 0x0078);
  EXPECT_EQ(r.st.s, 0x01B9);
}

TEST(Cpu65816Widths, XceLeavesEmulationExchangingCarry) {
  // e 1 -> 0 (via C = 0): C takes the old e (1). [65816 §6.10.4]
  const auto r = run({.pc = 0x1000, .s = 0x0100, .p = 0x30, .e = true},
                     {{0x001000, 0xFB}});
  EXPECT_FALSE(r.st.e);
  EXPECT_TRUE(r.st.p & kCpuFlagC);
}

// ---- the continuous emulation invariant ----

TEST(Cpu65816Widths, EmulationForcesStackHighByteEvenWhenTheInstructionIgnoresIt) {
  // A load never touches S, yet emulation pins SH to $01 every step. [65816 §4]
  const auto r = run({.pc = 0x1000, .s = 0xA8B9, .p = 0x30, .e = true},
                     {{0x001000, 0xA9}, {0x001001, 0x00}});
  EXPECT_EQ(r.st.s, 0x01B9);
}

// ---- indexed cycle costs (§6.5) ----

TEST(Cpu65816Widths, LdxAbsoluteYPaysTwoWidthCyclesAndAPageCross) {
  // LDX $12FE,Y with an 8-bit index and Y = $05 crosses a page: 6 - 2x + x*p = 5.
  // [65816 §6.5 LDX abs,Y]
  const auto r = run({.pc = 0x1000, .y = 0x05, .p = kCpuFlagX},
                     {{0x001000, 0xBE}, {0x001001, 0xFE}, {0x001002, 0x12}, {0x001303, 0x42}});
  EXPECT_EQ(r.st.x, 0x0042);
  EXPECT_EQ(r.cycles, 5u);
}

TEST(Cpu65816Widths, LdaAbsoluteXWithSixteenBitIndexTakesNoPageDiscount) {
  // x = 0: the indexed read always pays the extra cycle (6 - m - 0 + 0). [65816 §6.5]
  const auto r = run({.pc = 0x1000, .x = 0x0002, .p = 0},
                     {{0x001000, 0xBD}, {0x001001, 0x00}, {0x001002, 0x20}, {0x002002, 0xAB}, {0x002003, 0xCD}});
  EXPECT_EQ(r.st.a, 0xCDAB);
  EXPECT_EQ(r.cycles, 6u);
}

// ---- stores (§6.5) ----

TEST(Cpu65816Widths, SixteenBitStoreWritesLittleEndian) {
  // m = 0: STA $10 writes low then high into consecutive direct-page bytes. [65816 §2]
  const auto r = run({.pc = 0x1000, .a = 0xCDAB, .d = 0x0000, .p = 0},
                     {{0x001000, 0x85}, {0x001001, 0x10}});
  EXPECT_EQ(int{r.bus.read(0x0010)}, 0xAB);
  EXPECT_EQ(int{r.bus.read(0x0011)}, 0xCD);
  EXPECT_EQ(r.cycles, 4u);  // 4 - m + w, DL = 0
}

TEST(Cpu65816Widths, EightBitStoreZeroWritesOneByte) {
  // m = 1: STZ $10 writes a single zero byte and leaves the next byte alone.
  const auto r = run({.pc = 0x1000, .d = 0x0000, .p = kCpuFlagM},
                     {{0x001000, 0x64}, {0x001001, 0x10}, {0x000011, 0x77}});
  EXPECT_EQ(int{r.bus.read(0x0010)}, 0x00);
  EXPECT_EQ(int{r.bus.read(0x0011)}, 0x77);  // untouched
}

// ---- binary ADC / SBC (§6.1.1.1) ----

TEST(Cpu65816Arithmetic, AdcBinaryEightBitAddsWithCarryIn) {
  // m = 1, d = 0, C = 1: A = $10 + $20 + 1 = $31, no carry, no overflow. [65816 §6.1.1.1]
  const auto r = run({.pc = 0x1000, .a = 0xAA10, .p = kCpuFlagM | kCpuFlagC},
                     {{0x001000, 0x69}, {0x001001, 0x20}});
  EXPECT_EQ(r.st.a, 0xAA31);  // B (high byte) preserved
  EXPECT_FALSE(r.st.p & kCpuFlagC);
  EXPECT_FALSE(r.st.p & kCpuFlagV);
  EXPECT_FALSE(r.st.p & kCpuFlagN);
  EXPECT_EQ(r.cycles, 2u);  // 3 - m
}

TEST(Cpu65816Arithmetic, AdcBinaryOverflowAndCarry) {
  // m = 1, d = 0, C = 0: $7F + $01 = $80 — signed overflow, high bit set, no carry
  // out. [65816 §6.1.1.1 v/n/c meanings]
  const auto r = run({.pc = 0x1000, .a = 0x007F, .p = kCpuFlagM},
                     {{0x001000, 0x69}, {0x001001, 0x01}});
  EXPECT_EQ(r.st.a & 0xFF, 0x80u);
  EXPECT_TRUE(r.st.p & kCpuFlagV);
  EXPECT_TRUE(r.st.p & kCpuFlagN);
  EXPECT_FALSE(r.st.p & kCpuFlagC);
}

TEST(Cpu65816Arithmetic, SbcBinarySixteenBitWorkedExample) {
  // The reference's own example: A = $0001, m = 0, d = 0, C = 1, SBC #$2003 gives
  // A = $DFFE with n = 1, v = 0, z = 0, c = 0. [65816 §6.1.1.1 example 1]
  const auto r = run({.pc = 0x1000, .a = 0x0001, .p = kCpuFlagC},
                     {{0x001000, 0xE9}, {0x001001, 0x03}, {0x001002, 0x20}});
  EXPECT_EQ(r.st.a, 0xDFFE);
  EXPECT_TRUE(r.st.p & kCpuFlagN);
  EXPECT_FALSE(r.st.p & kCpuFlagV);
  EXPECT_FALSE(r.st.p & kCpuFlagZ);
  EXPECT_FALSE(r.st.p & kCpuFlagC);
  EXPECT_EQ(r.cycles, 3u);  // 3 - m
}

TEST(Cpu65816Arithmetic, SbcBinaryCarrySetMeansNoBorrow) {
  // m = 1, C = 1: $50 - $30 = $20, C stays set (register was at or above the
  // operand). [65816 §6.1.1.1 c meaning]
  const auto r = run({.pc = 0x1000, .a = 0x0050, .p = kCpuFlagM | kCpuFlagC},
                     {{0x001000, 0xE9}, {0x001001, 0x30}});
  EXPECT_EQ(r.st.a & 0xFF, 0x20u);
  EXPECT_TRUE(r.st.p & kCpuFlagC);
}

TEST(Cpu65816Arithmetic, SbcBinaryCarryClearSubtractsExtraOne) {
  // m = 1, C = 0: A = A - M - 1 = $50 - $30 - 1 = $1F. [65816 §6.1.1.1 formula]
  const auto r = run({.pc = 0x1000, .a = 0x0050, .p = kCpuFlagM},
                     {{0x001000, 0xE9}, {0x001001, 0x30}});
  EXPECT_EQ(r.st.a & 0xFF, 0x1Fu);
  EXPECT_TRUE(r.st.p & kCpuFlagC);  // still no borrow
}

// ---- decimal ADC / SBC (§6.1.1.1 — d = 1 uses BCD; n, z, c stay meaningful) ----

TEST(Cpu65816Arithmetic, AdcDecimalEightBitAddsBcd) {
  // m = 1, d = 1, C = 0: 25 + 48 = 73 in BCD. [65816 §6.1.1.1 BCD]
  const auto r = run({.pc = 0x1000, .a = 0x0025, .p = kCpuFlagM | kCpuFlagD},
                     {{0x001000, 0x69}, {0x001001, 0x48}});
  EXPECT_EQ(r.st.a & 0xFF, 0x73u);
  EXPECT_FALSE(r.st.p & kCpuFlagC);
  EXPECT_FALSE(r.st.p & kCpuFlagZ);
}

TEST(Cpu65816Arithmetic, AdcDecimalEightBitCarriesPastNinetyNine) {
  // 81 + 92 = 173 -> 73 with the carry set (result left the range 0..99). n from the
  // high bit of the result ($73 -> 0). [65816 §6.1.1.1 c/n meanings]
  const auto r = run({.pc = 0x1000, .a = 0x0081, .p = kCpuFlagM | kCpuFlagD},
                     {{0x001000, 0x69}, {0x001001, 0x92}});
  EXPECT_EQ(r.st.a & 0xFF, 0x73u);
  EXPECT_TRUE(r.st.p & kCpuFlagC);
  EXPECT_FALSE(r.st.p & kCpuFlagN);
}

TEST(Cpu65816Arithmetic, AdcDecimalHighBitSetsNegative) {
  // 50 + 40 = 90 in BCD; the result's high bit is 1, so n is set. [65816 §6.1.1.1]
  const auto r = run({.pc = 0x1000, .a = 0x0050, .p = kCpuFlagM | kCpuFlagD},
                     {{0x001000, 0x69}, {0x001001, 0x40}});
  EXPECT_EQ(r.st.a & 0xFF, 0x90u);
  EXPECT_TRUE(r.st.p & kCpuFlagN);
}

TEST(Cpu65816Arithmetic, AdcDecimalSixteenBitAddsBcd) {
  // m = 0, d = 1, C = 0: 1234 + 5678 = 6912 across four BCD digits. [65816 §6.1.1.1]
  const auto r = run({.pc = 0x1000, .a = 0x1234, .p = kCpuFlagD},
                     {{0x001000, 0x69}, {0x001001, 0x78}, {0x001002, 0x56}});
  EXPECT_EQ(r.st.a, 0x6912);
  EXPECT_FALSE(r.st.p & kCpuFlagC);
}

TEST(Cpu65816Arithmetic, AdcDecimalSixteenBitWrapsToZeroWithCarry) {
  // 9999 + 0001 = 0000 with carry; z reflects the zero result. [65816 §6.1.1.1]
  const auto r = run({.pc = 0x1000, .a = 0x9999, .p = kCpuFlagD},
                     {{0x001000, 0x69}, {0x001001, 0x01}, {0x001002, 0x00}});
  EXPECT_EQ(r.st.a, 0x0000);
  EXPECT_TRUE(r.st.p & kCpuFlagC);
  EXPECT_TRUE(r.st.p & kCpuFlagZ);
}

TEST(Cpu65816Arithmetic, SbcDecimalEightBitSubtractsBcd) {
  // m = 1, d = 1, C = 1: 46 - 12 = 34 in BCD, no borrow. [65816 §6.1.1.1]
  const auto r = run({.pc = 0x1000, .a = 0x0046, .p = kCpuFlagM | kCpuFlagD | kCpuFlagC},
                     {{0x001000, 0xE9}, {0x001001, 0x12}});
  EXPECT_EQ(r.st.a & 0xFF, 0x34u);
  EXPECT_TRUE(r.st.p & kCpuFlagC);
}

TEST(Cpu65816Arithmetic, SbcDecimalEightBitBorrowWrapsPastZero) {
  // 00 - 01 = 99 in BCD with the carry cleared (a borrow occurred). [65816 §6.1.1.1]
  const auto r = run({.pc = 0x1000, .a = 0x0000, .p = kCpuFlagM | kCpuFlagD | kCpuFlagC},
                     {{0x001000, 0xE9}, {0x001001, 0x01}});
  EXPECT_EQ(r.st.a & 0xFF, 0x99u);
  EXPECT_FALSE(r.st.p & kCpuFlagC);
}

TEST(Cpu65816Arithmetic, SbcDecimalSixteenBitBorrowWraps) {
  // m = 0, d = 1, C = 1: 0000 - 0001 = 9999 across four digits, carry cleared.
  // [65816 §6.1.1.1]
  const auto r = run({.pc = 0x1000, .a = 0x0000, .p = kCpuFlagD | kCpuFlagC},
                     {{0x001000, 0xE9}, {0x001001, 0x01}, {0x001002, 0x00}});
  EXPECT_EQ(r.st.a, 0x9999);
  EXPECT_FALSE(r.st.p & kCpuFlagC);
}

// ---- CMP / CPX / CPY (§6.1.1.2) ----

TEST(Cpu65816Compare, CmpEqualSetsZeroAndCarry) {
  // The reference example: A = $1234, m = 0, CMP #$1234 gives n = 0, z = 1, c = 1.
  // [65816 §6.1.1.2 example]
  const auto r = run({.pc = 0x1000, .a = 0x1234, .p = 0},
                     {{0x001000, 0xC9}, {0x001001, 0x34}, {0x001002, 0x12}});
  EXPECT_TRUE(r.st.p & kCpuFlagZ);
  EXPECT_TRUE(r.st.p & kCpuFlagC);
  EXPECT_FALSE(r.st.p & kCpuFlagN);
  EXPECT_EQ(r.st.a, 0x1234);  // the accumulator is not modified
}

TEST(Cpu65816Compare, CmpBelowClearsCarryAndDoesNotTouchOverflow) {
  // m = 1: A = $30, CMP #$50 -> $30 - $50 = $E0, so c = 0 (register below operand)
  // and n = 1. v is a compare — left untouched. [65816 §6.1.1.2]
  const auto r = run({.pc = 0x1000, .a = 0x0030, .p = kCpuFlagM | kCpuFlagV},
                     {{0x001000, 0xC9}, {0x001001, 0x50}});
  EXPECT_FALSE(r.st.p & kCpuFlagC);
  EXPECT_TRUE(r.st.p & kCpuFlagN);
  EXPECT_TRUE(r.st.p & kCpuFlagV);  // unchanged by a compare
}

TEST(Cpu65816Compare, CpxComparesAtTheIndexWidth) {
  // x = 1: X = $FF, CPX #$FF -> equal, so z and c set. [65816 §6.1.1.2]
  const auto r = run({.pc = 0x1000, .x = 0x00FF, .p = kCpuFlagX},
                     {{0x001000, 0xE0}, {0x001001, 0xFF}});
  EXPECT_TRUE(r.st.p & kCpuFlagZ);
  EXPECT_TRUE(r.st.p & kCpuFlagC);
}

// ---- AND / EOR / ORA (§6.1.2.1) ----

TEST(Cpu65816Logic, EorSixteenBitWorkedExample) {
  // The reference example: A = $0F06, m = 0, EOR #$F103 gives A = $FE05, n = 1,
  // z = 0. [65816 §6.1.2.1 example]
  const auto r = run({.pc = 0x1000, .a = 0x0F06, .p = 0},
                     {{0x001000, 0x49}, {0x001001, 0x03}, {0x001002, 0xF1}});
  EXPECT_EQ(r.st.a, 0xFE05);
  EXPECT_TRUE(r.st.p & kCpuFlagN);
  EXPECT_FALSE(r.st.p & kCpuFlagZ);
}

TEST(Cpu65816Logic, AndEightBitPreservesTheHighByte) {
  // m = 1: AND #$0F masks only the low byte; B is untouched. [65816 §6.1.2.1]
  const auto r = run({.pc = 0x1000, .a = 0x12F3, .p = kCpuFlagM},
                     {{0x001000, 0x29}, {0x001001, 0x0F}});
  EXPECT_EQ(r.st.a, 0x1203);
  EXPECT_FALSE(r.st.p & kCpuFlagN);
}

TEST(Cpu65816Logic, OraSixteenBitSetsNegative) {
  // m = 0: ORA #$8000 sets the high bit, so n is set. [65816 §6.1.2.1]
  const auto r = run({.pc = 0x1000, .a = 0x0001, .p = 0},
                     {{0x001000, 0x09}, {0x001001, 0x00}, {0x001002, 0x80}});
  EXPECT_EQ(r.st.a, 0x8001);
  EXPECT_TRUE(r.st.p & kCpuFlagN);
}

// ---- BIT (§6.1.2.2 — the flags depend on the addressing mode) ----

TEST(Cpu65816Bit, NonImmediateTakesNVFromTheOperand) {
  // m = 1: A = $43, operand $9C. The AND ($43 & $9C = $00) sets z; n and v take the
  // top two bits of the operand ($9C: bit 7 = 1, bit 6 = 0). [65816 §6.1.2.2 example]
  const auto r = run({.pc = 0x1000, .a = 0x0043, .d = 0x0000, .p = kCpuFlagM},
                     {{0x001000, 0x24}, {0x001001, 0x10}, {0x000010, 0x9C}});
  EXPECT_TRUE(r.st.p & kCpuFlagZ);
  EXPECT_TRUE(r.st.p & kCpuFlagN);
  EXPECT_FALSE(r.st.p & kCpuFlagV);
}

TEST(Cpu65816Bit, SixteenBitNonImmediateTakesNVFromTheHighBits) {
  // m = 0: operand $C000 -> n from bit 15 (1) and v from bit 14 (1); the AND with
  // $0001 is zero, so z is set. [65816 §6.1.2.2]
  const auto r = run({.pc = 0x1000, .a = 0x0001, .d = 0x0000, .p = 0},
                     {{0x001000, 0x24}, {0x001001, 0x10}, {0x000010, 0x00}, {0x000011, 0xC0}});
  EXPECT_TRUE(r.st.p & kCpuFlagN);
  EXPECT_TRUE(r.st.p & kCpuFlagV);
  EXPECT_TRUE(r.st.p & kCpuFlagZ);
}

TEST(Cpu65816Bit, ImmediateAffectsOnlyZero) {
  // m = 1: BIT #$F0 with A = $0F sets z (the AND is zero) but leaves n and v exactly
  // as they were — the immediate form is the exception. [65816 §6.1.2.2]
  const auto r = run({.pc = 0x1000, .a = 0x000F, .p = kCpuFlagM | kCpuFlagN | kCpuFlagV},
                     {{0x001000, 0x89}, {0x001001, 0xF0}});
  EXPECT_TRUE(r.st.p & kCpuFlagZ);
  EXPECT_TRUE(r.st.p & kCpuFlagN);  // unchanged
  EXPECT_TRUE(r.st.p & kCpuFlagV);  // unchanged
}

// ---- INC / DEC (§6.1.1.3 — n from the high bit, z from zero; no carry) ----

TEST(Cpu65816IncDec, IndexIncrementOverflowsToNegative) {
  // The reference example: X = $7FFF, x = 0, after INX X = $8000, n = 1, z = 0.
  // [65816 §6.1.1.3 example]
  const auto r = run({.pc = 0x1000, .x = 0x7FFF, .p = 0}, {{0x001000, 0xE8}});
  EXPECT_EQ(r.st.x, 0x8000);
  EXPECT_TRUE(r.st.p & kCpuFlagN);
  EXPECT_FALSE(r.st.p & kCpuFlagZ);
  EXPECT_EQ(r.cycles, 2u);
}

TEST(Cpu65816IncDec, EightBitAccumulatorIncrementWrapsAndPreservesHighByte) {
  // m = 1: INC A on $12FF rolls the low byte to $00 (z = 1) and leaves B intact.
  // [65816 §6.1.1.3]
  const auto r = run({.pc = 0x1000, .a = 0x12FF, .p = kCpuFlagM}, {{0x001000, 0x1A}});
  EXPECT_EQ(r.st.a, 0x1200);
  EXPECT_TRUE(r.st.p & kCpuFlagZ);
}

TEST(Cpu65816IncDec, EightBitMemoryDecrementWritesBackAndSetsNegative) {
  // m = 1: DEC $10 on $00 wraps to $FF (n = 1) and writes it back. 7 - 2m + w = 5.
  // [65816 §6.1.1.3]
  const auto r = run({.pc = 0x1000, .d = 0x0000, .p = kCpuFlagM},
                     {{0x001000, 0xC6}, {0x001001, 0x10}, {0x000010, 0x00}});
  EXPECT_EQ(int{r.bus.read(0x0010)}, 0xFF);
  EXPECT_TRUE(r.st.p & kCpuFlagN);
  EXPECT_EQ(r.cycles, 5u);
}

// ---- ASL / LSR / ROL / ROR (§6.1.3) ----

TEST(Cpu65816Shift, AslMemoryWorkedExample) {
  // The reference example: m = 1, $10 holds $8F, after ASL $10 it holds $1E with
  // n = 0, z = 0, c = 1 (the old high bit). 7 - 2m + w = 5. [65816 §6.1.3 example]
  const auto r = run({.pc = 0x1000, .d = 0x0000, .p = kCpuFlagM},
                     {{0x001000, 0x06}, {0x001001, 0x10}, {0x000010, 0x8F}});
  EXPECT_EQ(int{r.bus.read(0x0010)}, 0x1E);
  EXPECT_FALSE(r.st.p & kCpuFlagN);
  EXPECT_FALSE(r.st.p & kCpuFlagZ);
  EXPECT_TRUE(r.st.p & kCpuFlagC);
  EXPECT_EQ(r.cycles, 5u);
}

TEST(Cpu65816Shift, AslSixteenBitAccumulatorShiftsIntoBit15) {
  // m = 0: ASL A on $4000 gives $8000 (n = 1), no carry out. [65816 §6.1.3]
  const auto r = run({.pc = 0x1000, .a = 0x4000, .p = 0}, {{0x001000, 0x0A}});
  EXPECT_EQ(r.st.a, 0x8000);
  EXPECT_TRUE(r.st.p & kCpuFlagN);
  EXPECT_FALSE(r.st.p & kCpuFlagC);
  EXPECT_EQ(r.cycles, 2u);
}

TEST(Cpu65816Shift, LsrAlwaysClearsNegativeAndTakesCarryFromBitZero) {
  // m = 1: LSR A on $FF gives $7F — a zero shifts into the top bit, so n is cleared
  // however high the input was; the old bit 0 becomes carry. [65816 §6.1.3]
  const auto r = run({.pc = 0x1000, .a = 0x00FF, .p = kCpuFlagM | kCpuFlagN},
                     {{0x001000, 0x4A}});
  EXPECT_EQ(r.st.a, 0x007F);
  EXPECT_FALSE(r.st.p & kCpuFlagN);
  EXPECT_TRUE(r.st.p & kCpuFlagC);
}

TEST(Cpu65816Shift, RolFeedsCarryIntoBitZero) {
  // m = 1, c = 1: ROL A on $80 shifts the old carry into bit 0 and the old high bit
  // out to carry: $80 -> $01, c = 1. [65816 §6.1.3]
  const auto r = run({.pc = 0x1000, .a = 0x0080, .p = kCpuFlagM | kCpuFlagC},
                     {{0x001000, 0x2A}});
  EXPECT_EQ(r.st.a, 0x0001);
  EXPECT_TRUE(r.st.p & kCpuFlagC);
  EXPECT_FALSE(r.st.p & kCpuFlagN);
}

TEST(Cpu65816Shift, RorFeedsCarryIntoTheHighBit) {
  // m = 1, c = 1: ROR A on $01 shifts the old carry into the top bit and the old bit
  // 0 out to carry: $01 -> $80 (n = 1), c = 1. [65816 §6.1.3]
  const auto r = run({.pc = 0x1000, .a = 0x0001, .p = kCpuFlagM | kCpuFlagC},
                     {{0x001000, 0x6A}});
  EXPECT_EQ(r.st.a, 0x0080);
  EXPECT_TRUE(r.st.p & kCpuFlagN);
  EXPECT_TRUE(r.st.p & kCpuFlagC);
}

// ---- TSB / TRB (§6.1.2.3 — z from the AND, memory bits set / reset) ----

TEST(Cpu65816TestBits, TsbSetsTheAccumulatorsBitsAndZFromTheAnd) {
  // The reference example: A = $43, m = 1, $10 holds $9C; TSB sets z (the AND is
  // zero) and writes $9C | $43 = $DF back. [65816 §6.1.2.3 example]
  const auto r = run({.pc = 0x1000, .a = 0x0043, .d = 0x0000, .p = kCpuFlagM},
                     {{0x001000, 0x04}, {0x001001, 0x10}, {0x000010, 0x9C}});
  EXPECT_EQ(int{r.bus.read(0x0010)}, 0xDF);
  EXPECT_TRUE(r.st.p & kCpuFlagZ);
}

TEST(Cpu65816TestBits, TrbClearsTheAccumulatorsBitsAndZReflectsTheAnd) {
  // m = 1: A = $0F, $10 holds $FF; TRB clears the low nibble ($FF & ~$0F = $F0) and
  // z = 0 because the AND is non-zero. [65816 §6.1.2.3]
  const auto r = run({.pc = 0x1000, .a = 0x000F, .d = 0x0000, .p = kCpuFlagM},
                     {{0x001000, 0x14}, {0x001001, 0x10}, {0x000010, 0xFF}});
  EXPECT_EQ(int{r.bus.read(0x0010)}, 0xF0);
  EXPECT_FALSE(r.st.p & kCpuFlagZ);
}

// ---- push / pull byte order and widths (§6.8.1–6.8.3) ----

TEST(Cpu65816Stack, PushDirectRegisterStoresHighByteAtTheHigherAddress) {
  // Native: PHD pushes D = $1234 high byte first, so the stack is little-endian —
  // $34 at S-1, $12 at S — and S drops by two. No flags. [65816 §6.8.3 / §2]
  const auto r = run({.pc = 0x1000, .s = 0x01FF, .d = 0x1234, .p = 0},
                     {{0x001000, 0x0B}});
  EXPECT_EQ(int{r.bus.read(0x01FF)}, 0x12);
  EXPECT_EQ(int{r.bus.read(0x01FE)}, 0x34);
  EXPECT_EQ(r.st.s, 0x01FD);
  EXPECT_EQ(r.cycles, 4u);
}

TEST(Cpu65816Stack, PullDirectRegisterIsSixteenBitAndSetsFlags) {
  // Native: PLD reads low then high and sets N,Z on the whole 16-bit value regardless
  // of the m width. Stack $01FE/$01FF = $00/$80 -> D = $8000, n = 1. [65816 §6.8.3]
  const auto r = run({.pc = 0x1000, .s = 0x01FD, .p = kCpuFlagM},
                     {{0x001000, 0x2B}, {0x0001FE, 0x00}, {0x0001FF, 0x80}});
  EXPECT_EQ(r.st.d, 0x8000);
  EXPECT_TRUE(r.st.p & kCpuFlagN);
  EXPECT_EQ(r.st.s, 0x01FF);
  EXPECT_EQ(r.cycles, 5u);
}

TEST(Cpu65816Stack, PeaPushesTheImmediateWord) {
  // Native: PEA #$1234 pushes the immediate directly, no flags. [65816 §6.8.1]
  const auto r = run({.pc = 0x1000, .s = 0x01FF, .p = kCpuFlagZ},
                     {{0x001000, 0xF4}, {0x001001, 0x34}, {0x001002, 0x12}});
  EXPECT_EQ(int{r.bus.read(0x01FF)}, 0x12);
  EXPECT_EQ(int{r.bus.read(0x01FE)}, 0x34);
  EXPECT_EQ(r.st.s, 0x01FD);
  EXPECT_TRUE(r.st.p & kCpuFlagZ);  // unchanged
  EXPECT_EQ(r.cycles, 5u);
}

TEST(Cpu65816Stack, PerPushesTheAddressRelativeToTheNextInstruction) {
  // Native: PER adds its 16-bit displacement to the address of the next instruction
  // (PC = $1003 here) and pushes the result $100D. [65816 §6.8.1 / §5.18]
  const auto r = run({.pc = 0x1000, .s = 0x01FF, .p = 0},
                     {{0x001000, 0x62}, {0x001001, 0x0A}, {0x001002, 0x00}});
  EXPECT_EQ(int{r.bus.read(0x01FF)}, 0x10);
  EXPECT_EQ(int{r.bus.read(0x01FE)}, 0x0D);
  EXPECT_EQ(r.st.s, 0x01FD);
  EXPECT_EQ(r.cycles, 6u);
}

// ---- emulation-mode stack wrapping (§5.22) ----

TEST(Cpu65816Stack, EmulationOldPushWrapsWithinPageOne) {
  // e = 1: PHA is a 6502-original push, so at S = $0100 the pointer wraps back to
  // $01FF within page one after writing at $0100. [65816 §5.22 "old" instructions]
  const auto r = run({.pc = 0x1000, .s = 0x0100, .a = 0x00AB, .p = 0x30, .e = true},
                     {{0x001000, 0x48}});
  EXPECT_EQ(int{r.bus.read(0x0100)}, 0xAB);
  EXPECT_EQ(r.st.s, 0x01FF);
}

TEST(Cpu65816Stack, EmulationNewPullLeavesPageOneThenForcesTheHighByteBack) {
  // e = 1: PLB is a 65816-new pull, so at S = $01FF it reads from $0200 (crossing out
  // of page one), then the pointer's high byte is forced back to $01, leaving
  // S = $0100. [65816 §5.22 "otherwise" + SL incremented]
  const auto r = run({.pc = 0x1000, .s = 0x01FF, .p = 0x30, .e = true},
                     {{0x001000, 0xAB}, {0x000200, 0x42}});
  EXPECT_EQ(r.st.dbr, 0x42);
  EXPECT_EQ(r.st.s, 0x0100);
  EXPECT_FALSE(r.st.p & kCpuFlagZ);
}

// ---- status flags, mode, and no-ops (§6.4.1 / §6.4.2 / §6.7 / §6.8.3) ----

TEST(Cpu65816Flags, SetAndClearIndividualFlags) {
  // SEC sets carry; CLV clears overflow. [65816 §6.4.1]
  const auto sec = run({.pc = 0x1000, .p = 0}, {{0x001000, 0x38}});
  EXPECT_TRUE(sec.st.p & kCpuFlagC);
  EXPECT_EQ(sec.cycles, 2u);
  const auto clv = run({.pc = 0x1000, .p = kCpuFlagV}, {{0x001000, 0xB8}});
  EXPECT_FALSE(clv.st.p & kCpuFlagV);
}

TEST(Cpu65816Flags, RepClearsTheMemoryWidthFlag) {
  // REP #$20 clears m, widening the accumulator to 16-bit. [65816 §6.4.2]
  const auto r = run({.pc = 0x1000, .p = kCpuFlagM},
                     {{0x001000, 0xC2}, {0x001001, 0x20}});
  EXPECT_FALSE(r.st.p & kCpuFlagM);
  EXPECT_EQ(r.cycles, 3u);
}

TEST(Cpu65816Flags, SepNarrowingTheIndexWidthClearsTheIndexHighBytes) {
  // SEP #$10 sets x; narrowing the index registers to 8-bit zeros their high bytes at
  // once, so X = $1234 becomes $0034 and Y = $5678 becomes $0078. [65816 §6.4.2 / §4]
  const auto r = run({.pc = 0x1000, .x = 0x1234, .y = 0x5678, .p = 0},
                     {{0x001000, 0xE2}, {0x001001, 0x10}});
  EXPECT_TRUE(r.st.p & kCpuFlagX);
  EXPECT_EQ(r.st.x, 0x0034);
  EXPECT_EQ(r.st.y, 0x0078);
}

TEST(Cpu65816Flags, PlpReplacesTheWholeStatusByte) {
  // Native: PLP loads P from the stack wholesale. [65816 §6.8.3]
  const auto r = run({.pc = 0x1000, .s = 0x01FF, .p = 0x00},
                     {{0x001000, 0x28}, {0x000200, 0xCC}});
  EXPECT_EQ(int{r.st.p}, 0xCC);
  EXPECT_EQ(r.cycles, 4u);
}

TEST(Cpu65816Flags, NopAndWdmAdvancePastTheirBytes) {
  // NOP is one byte, WDM is a two-byte reserved no-op; both take two cycles and touch
  // nothing else. [65816 §6.7]
  const auto nop = run({.pc = 0x1000, .p = 0}, {{0x001000, 0xEA}});
  EXPECT_EQ(nop.st.pc, 0x1001);
  EXPECT_EQ(nop.cycles, 2u);
  const auto wdm = run({.pc = 0x1000, .p = 0}, {{0x001000, 0x42}, {0x001001, 0x00}});
  EXPECT_EQ(wdm.st.pc, 0x1002);
  EXPECT_EQ(wdm.cycles, 2u);
}

}  // namespace
