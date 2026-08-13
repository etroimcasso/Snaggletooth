#include "snaggletooth/cpu/cpu65816.h"
#include "vector_harness.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <initializer_list>
#include <utility>

// The indirect and stack-relative instructions, cycle by cycle. The recorded
// vectors prove every opcode's whole trace; what is proven here is the documented
// shape behind them, with each expectation traced to the line that states it:
// W65C816S datasheet Table 5-7 rows 11 (direct indexed indirect), 12 (direct
// indirect), 13 (direct indirect indexed), 14 (direct indirect long indexed),
// 15 (direct indirect long), 23 (stack relative) and 24 (stack relative indirect
// indexed), their notes 1, 2 and 4, sections 7.1 on stack addressing and
// 7.2.1-7.2.3 on the direct addressing range, and — for how a pointer's own bytes
// step — Eyes & Lichty's account of the emulation mode in Programming the 65816.

namespace {

using snaggletooth::Cpu65816;
using snaggletooth::Cpu65816State;
using snaggletooth::kCpuFlagM;
using snaggletooth::kCpuFlagX;
using snaggletooth::cpu_vectors::kSignalRw;
using snaggletooth::cpu_vectors::kSignalVda;
using snaggletooth::cpu_vectors::kSignalVpa;
using snaggletooth::cpu_vectors::RecordingBus;

RecordingBus busWith(
    std::initializer_list<std::pair<std::uint32_t, std::uint8_t>> ram) {
  RecordingBus bus;
  for (const auto& [address, value] : ram) bus.mem[address] = value;
  return bus;
}

// Runs one instruction from $001000 and returns how many cycles it took, with the
// bus recording pin states from the processor as it goes.
std::uint32_t run(RecordingBus& bus, Cpu65816& cpu) {
  bus.cpu = &cpu;
  return cpu.stepInstruction(bus);
}

// The eight-bit widths as a processor status byte.
constexpr std::uint8_t kEightBit = kCpuFlagM | kCpuFlagX;

// ---- where the pointer comes from (Table 5-7 rows 11, 12, 15, 23) ----

TEST(Cpu65816Indirect, DirectIndirectReadsATwoBytePointerThenTheData) {
  // Row 12: the opcode, DO, then AAL and AAH out of the direct page, then the data
  // at DBR,AA. The direct register is page-aligned, so note 2 adds nothing.
  auto bus = busWith({{0x001000, 0xB2},
                      {0x001001, 0x10},
                      {0x000010, 0x34},
                      {0x000011, 0x12},
                      {0x011234, 0x7F}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .p = kEightBit, .dbr = 0x01});
  EXPECT_EQ(run(bus, cpu), 5u);
  EXPECT_EQ(cpu.state().a & 0xFF, 0x7F);
  ASSERT_EQ(bus.trace.size(), 5u);
  EXPECT_EQ(bus.trace[1].address, 0x001001u);
  EXPECT_EQ(bus.trace[2].address, 0x000010u);
  EXPECT_EQ(bus.trace[3].address, 0x000011u);
  EXPECT_EQ(bus.trace[4].address, 0x011234u);
}

TEST(Cpu65816Indirect, ADirectRegisterWithALowByteCostsACycle) {
  // Note 2, "add 1 cycle for direct register low (DL) not equal 0", and row 12's
  // cycle 2a: the extra cycle is internal and parks on the address the offset was
  // fetched from.
  auto bus = busWith({{0x001000, 0xB2},
                      {0x001001, 0x10},
                      {0x000047, 0x34},
                      {0x000048, 0x12},
                      {0x001234, 0x7F}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .d = 0x0037, .p = kEightBit});
  EXPECT_EQ(run(bus, cpu), 6u);
  ASSERT_EQ(bus.trace.size(), 6u);
  EXPECT_EQ(bus.trace[2].address, 0x001001u);
  EXPECT_EQ(bus.trace[2].signals[kSignalVda], '-');
  EXPECT_EQ(bus.trace[2].signals[kSignalVpa], '-');
  EXPECT_EQ(bus.trace[3].address, 0x000047u);
}

TEST(Cpu65816Indirect, DirectIndexedIndirectAlwaysSpendsACycleAddingX) {
  // Row 11 cycle 3: an internal cycle parked on the offset's address, present
  // whatever the index holds — X is added before the pointer can be read, so there
  // is nothing conditional about it.
  auto bus = busWith({{0x001000, 0xA1},
                      {0x001001, 0x10},
                      {0x000014, 0x34},
                      {0x000015, 0x12},
                      {0x001234, 0x7F}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .x = 0x04, .p = kEightBit});
  EXPECT_EQ(run(bus, cpu), 6u);
  EXPECT_EQ(cpu.state().a & 0xFF, 0x7F);
  ASSERT_EQ(bus.trace.size(), 6u);
  EXPECT_EQ(bus.trace[2].address, 0x001001u);
  EXPECT_EQ(bus.trace[3].address, 0x000014u);
}

TEST(Cpu65816Indirect, AnIndirectLongPointerCarriesItsOwnBank) {
  // Row 15: three pointer bytes — AAL, AAH and AAB — and the data comes from the
  // bank the third one names rather than from the data bank.
  auto bus = busWith({{0x001000, 0xA7},
                      {0x001001, 0x10},
                      {0x000010, 0x34},
                      {0x000011, 0x12},
                      {0x000012, 0x7E},
                      {0x7E1234, 0x5A}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .p = kEightBit, .dbr = 0x01});
  EXPECT_EQ(run(bus, cpu), 6u);
  EXPECT_EQ(cpu.state().a & 0xFF, 0x5A);
  ASSERT_EQ(bus.trace.size(), 6u);
  EXPECT_EQ(bus.trace[4].address, 0x000012u);
  EXPECT_EQ(bus.trace[5].address, 0x7E1234u);
}

TEST(Cpu65816Indirect, StackRelativeReadsItsOperandAtAnOffsetFromTheStack) {
  // Row 23: the opcode, SO, an internal cycle for the addition, then the operand at
  // 0,S+SO. Section 3.5.23: "the high-order 8 bits of the effective address are
  // always zero", so the operand is in bank zero whatever the data bank holds.
  auto bus = busWith({{0x001000, 0xA3}, {0x001001, 0x04}, {0x0001F8, 0x7F}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .s = 0x01F4, .p = kEightBit, .dbr = 0x7E});
  EXPECT_EQ(run(bus, cpu), 4u);
  EXPECT_EQ(cpu.state().a & 0xFF, 0x7F);
  ASSERT_EQ(bus.trace.size(), 4u);
  EXPECT_EQ(bus.trace[2].address, 0x001001u);
  EXPECT_EQ(bus.trace[2].signals[kSignalVda], '-');
  EXPECT_EQ(bus.trace[3].address, 0x0001F8u);
}

TEST(Cpu65816Indirect, AStackRelativeOperandKeepsItsSecondByteInBankZero) {
  // Section 3.5.23 again: both bytes of a sixteen-bit operand are addressed with a
  // zero bank, so an operand based at the top of bank zero reads its high byte from
  // $000000 rather than running on into bank one.
  auto bus = busWith({{0x001000, 0xA3},
                      {0x001001, 0x05},
                      {0x00FFFF, 0x34},
                      {0x000000, 0x12}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .s = 0xFFFA, .p = kCpuFlagX});
  EXPECT_EQ(run(bus, cpu), 5u);
  EXPECT_EQ(cpu.state().a, 0x1234);
  ASSERT_EQ(bus.trace.size(), 5u);
  EXPECT_EQ(bus.trace[3].address, 0x00FFFFu);
  EXPECT_EQ(bus.trace[4].address, 0x000000u);
}

TEST(Cpu65816Indirect, StackRelativeIndirectIndexedReadsItsPointerFromTheStack) {
  // Row 24: opcode, SO, the stack-addition cycle, AAL and AAH at 0,S+SO, the
  // indexing cycle, then the data at DBR,AA+Y — seven cycles for an eight-bit read.
  auto bus = busWith({{0x001000, 0xB3},
                      {0x001001, 0x04},
                      {0x0001F8, 0x34},
                      {0x0001F9, 0x12},
                      {0x011236, 0x7F}});
  Cpu65816 cpu(
      Cpu65816State{.pc = 0x1000, .s = 0x01F4, .y = 0x02, .p = kEightBit, .dbr = 0x01});
  EXPECT_EQ(run(bus, cpu), 7u);
  EXPECT_EQ(cpu.state().a & 0xFF, 0x7F);
  ASSERT_EQ(bus.trace.size(), 7u);
  EXPECT_EQ(bus.trace[3].address, 0x0001F8u);
  EXPECT_EQ(bus.trace[4].address, 0x0001F9u);
  EXPECT_EQ(bus.trace[6].address, 0x011236u);
}

// ---- the indexing cycle (Table 5-7 note 4, and row 24's unconditional one) ----

TEST(Cpu65816Indirect, AnEightBitIndexedReadPaysNothingWhenItStaysInThePage) {
  // Row 13 cycle 4a carries note 4 — "add 1 cycle for indexing across page
  // boundaries, or write, or X=0" — and a read with eight-bit index registers that
  // does not cross pays for none of the three.
  auto bus = busWith({{0x001000, 0xB1},
                      {0x001001, 0x10},
                      {0x000010, 0x00},
                      {0x000011, 0x12},
                      {0x011205, 0x7F}});
  Cpu65816 cpu(
      Cpu65816State{.pc = 0x1000, .y = 0x05, .p = kEightBit, .dbr = 0x01});
  EXPECT_EQ(run(bus, cpu), 5u);
  EXPECT_EQ(cpu.state().a & 0xFF, 0x7F);
  for (const auto& cycle : bus.trace) {
    EXPECT_NE(cycle.address, 0x011200u) << "no cycle drives the un-carried address";
  }
}

TEST(Cpu65816Indirect, IndexingAcrossAPageBoundaryCostsACycle) {
  // Note 4's first clause: the same instruction whose addition carries out of the
  // low byte spends a cycle waiting for the carry.
  auto bus = busWith({{0x001000, 0xB1},
                      {0x001001, 0x10},
                      {0x000010, 0xF8},
                      {0x000011, 0x12},
                      {0x011308, 0x7F}});
  Cpu65816 cpu(
      Cpu65816State{.pc = 0x1000, .y = 0x10, .p = kEightBit, .dbr = 0x01});
  EXPECT_EQ(run(bus, cpu), 6u);
  EXPECT_EQ(cpu.state().a & 0xFF, 0x7F);
}

TEST(Cpu65816Indirect, TheIndexingCycleDrivesTheAddressWithoutItsCarry) {
  // Row 13 cycle 4a gives that cycle's address as DBR,AAH,AAL+YL: the low bytes are
  // added but the carry has not reached the high byte, so the address driven is one
  // the instruction never means to read, and it drives no valid access.
  auto bus = busWith({{0x001000, 0xB1},
                      {0x001001, 0x10},
                      {0x000010, 0xF8},
                      {0x000011, 0x12},
                      {0x011308, 0x7F}});
  Cpu65816 cpu(
      Cpu65816State{.pc = 0x1000, .y = 0x10, .p = kEightBit, .dbr = 0x01});
  ASSERT_EQ(run(bus, cpu), 6u);
  ASSERT_EQ(bus.trace.size(), 6u);
  EXPECT_EQ(bus.trace[4].address, 0x011208u);
  EXPECT_EQ(bus.trace[4].signals[kSignalVda], '-');
  EXPECT_EQ(bus.trace[4].signals[kSignalVpa], '-');
  EXPECT_EQ(bus.trace[5].address, 0x011308u);
}

TEST(Cpu65816Indirect, SixteenBitIndexRegistersAlwaysPayTheIndexingCycle) {
  // Note 4's third clause, "or X=0": with sixteen-bit index registers the cycle is
  // paid whether or not the low-byte addition carries.
  auto bus = busWith({{0x001000, 0xB1},
                      {0x001001, 0x10},
                      {0x000010, 0x00},
                      {0x000011, 0x12},
                      {0x011305, 0x7F}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .y = 0x0105, .p = kCpuFlagM, .dbr = 0x01});
  EXPECT_EQ(run(bus, cpu), 6u);
  EXPECT_EQ(cpu.state().a & 0xFF, 0x7F);
}

TEST(Cpu65816Indirect, AnIndexedIndirectStoreAlwaysPaysTheIndexingCycle) {
  // Note 4's second clause, "or write": a store pays even where the matching read
  // would not, because it cannot begin writing before the address is certain.
  auto bus = busWith(
      {{0x001000, 0x91}, {0x001001, 0x10}, {0x000010, 0x00}, {0x000011, 0x12}});
  Cpu65816 cpu(
      Cpu65816State{.pc = 0x1000, .a = 0x7F, .y = 0x05, .p = kEightBit, .dbr = 0x01});
  EXPECT_EQ(run(bus, cpu), 6u);
  EXPECT_EQ(bus.mem[0x011205], 0x7F);
}

TEST(Cpu65816Indirect, TheLongIndexedModeNeverPaysAnIndexingCycle) {
  // Row 14 lists no such cycle and carries no note 4: Y is added to a 24-bit address
  // that has no page carry to settle, so the read costs the same as the unindexed
  // long mode.
  auto bus = busWith({{0x001000, 0xB7},
                      {0x001001, 0x10},
                      {0x000010, 0xF8},
                      {0x000011, 0x12},
                      {0x000012, 0x7E},
                      {0x7E1308, 0x5A}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .y = 0x10, .p = kEightBit});
  EXPECT_EQ(run(bus, cpu), 6u);
  EXPECT_EQ(cpu.state().a & 0xFF, 0x5A);
}

TEST(Cpu65816Indirect, StackRelativeIndirectAlwaysPaysItsIndexingCycle) {
  // Row 24 cycle 6 carries no note, so it is paid on every execution — here by an
  // eight-bit indexed read whose addition does not cross a page, which is exactly
  // the case row 13 would charge nothing for.
  auto bus = busWith({{0x001000, 0xB3},
                      {0x001001, 0x04},
                      {0x0001F8, 0x00},
                      {0x0001F9, 0x12},
                      {0x011205, 0x7F}});
  Cpu65816 cpu(
      Cpu65816State{.pc = 0x1000, .s = 0x01F4, .y = 0x05, .p = kEightBit, .dbr = 0x01});
  EXPECT_EQ(run(bus, cpu), 7u);
  EXPECT_EQ(cpu.state().a & 0xFF, 0x7F);
}

TEST(Cpu65816Indirect, StackRelativeIndirectParksOnTheLastPointerByte) {
  // Row 24 gives that cycle's address as 0,S+SO+1 — the pointer's own high byte,
  // rather than the partly-added data address row 13 drives.
  auto bus = busWith({{0x001000, 0xB3},
                      {0x001001, 0x04},
                      {0x0001F8, 0x00},
                      {0x0001F9, 0x12},
                      {0x011205, 0x7F}});
  Cpu65816 cpu(
      Cpu65816State{.pc = 0x1000, .s = 0x01F4, .y = 0x05, .p = kEightBit, .dbr = 0x01});
  ASSERT_EQ(run(bus, cpu), 7u);
  ASSERT_EQ(bus.trace.size(), 7u);
  EXPECT_EQ(bus.trace[5].address, 0x0001F9u);
  EXPECT_EQ(bus.trace[5].signals[kSignalVda], '-');
  EXPECT_EQ(bus.trace[5].signals[kSignalVpa], '-');
}

// ---- how the address is formed (sections 7.1, 7.2.1-7.2.3, 3.5.13) ----

TEST(Cpu65816Indirect, TheIndexCarriesIntoTheNextBank) {
  // Section 3.5.13: Y is added to the 24-bit base address the pointer and the data
  // bank form, so an addition past the end of a bank runs on into the next one.
  auto bus = busWith({{0x001000, 0xB1},
                      {0x001001, 0x10},
                      {0x000010, 0xFF},
                      {0x000011, 0xFF},
                      {0x020004, 0x7F}});
  Cpu65816 cpu(
      Cpu65816State{.pc = 0x1000, .y = 0x05, .p = kEightBit, .dbr = 0x01});
  EXPECT_EQ(run(bus, cpu), 6u);
  EXPECT_EQ(cpu.state().a & 0xFF, 0x7F);
}

TEST(Cpu65816Indirect, TheEmulatedDirectPageAddsOnlyTheLowByte) {
  // Section 7.2.2: in emulation mode with DH set and DL zero "the direct addressing
  // range is 00DH00 to 00DHFF", so the offset and X are added to the low byte alone
  // and the pointer stays inside page DH.
  auto bus = busWith({{0x001000, 0xA1},
                      {0x001001, 0xB6},
                      {0x00B2AF, 0x34},
                      {0x00B2B0, 0x12},
                      {0x011234, 0x7F}});
  Cpu65816 cpu(Cpu65816State{
      .pc = 0x1000, .x = 0x00F9, .d = 0xB200, .p = kEightBit, .dbr = 0x01, .e = true});
  EXPECT_EQ(run(bus, cpu), 6u);
  EXPECT_EQ(cpu.state().a & 0xFF, 0x7F);
  ASSERT_EQ(bus.trace.size(), 6u);
  EXPECT_EQ(bus.trace[3].address, 0x00B2AFu);
}

TEST(Cpu65816Indirect, APointerBasedOnAPageWithADirectRegisterRunsOnIntoTheNext) {
  // The addition is confined to the page; stepping to the pointer's second byte is
  // not. Eyes & Lichty: page-zero wrapping "is only enforced for 6502 and 65C02
  // instructions, and only when DP = 0" — with a direct register of its own, the
  // pointer based at $B2FF reads its high byte from $B300.
  auto bus = busWith({{0x001000, 0xA1},
                      {0x001001, 0xB6},
                      {0x00B2FF, 0x34},
                      {0x00B300, 0x12},
                      {0x011234, 0x7F}});
  Cpu65816 cpu(Cpu65816State{
      .pc = 0x1000, .x = 0x0049, .d = 0xB200, .p = kEightBit, .dbr = 0x01, .e = true});
  EXPECT_EQ(run(bus, cpu), 6u);
  EXPECT_EQ(cpu.state().a & 0xFF, 0x7F);
  ASSERT_EQ(bus.trace.size(), 6u);
  EXPECT_EQ(bus.trace[3].address, 0x00B2FFu);
  EXPECT_EQ(bus.trace[4].address, 0x00B300u);
}

TEST(Cpu65816Indirect, AnOldPointerInPageZeroWrapsInsideIt) {
  // Section 7.2.1: with the direct register at zero the emulated direct addressing
  // range is $000000 to $0000FF, which the modes the 6502 and 65C02 already had stay
  // inside — the pointer at $0000FF reads its high byte from $000000.
  auto bus = busWith({{0x001000, 0xB2},
                      {0x001001, 0xFF},
                      {0x0000FF, 0x34},
                      {0x000000, 0x12},
                      {0x011234, 0x7F}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .p = kEightBit, .dbr = 0x01, .e = true});
  EXPECT_EQ(run(bus, cpu), 5u);
  EXPECT_EQ(cpu.state().a & 0xFF, 0x7F);
  ASSERT_EQ(bus.trace.size(), 5u);
  EXPECT_EQ(bus.trace[2].address, 0x0000FFu);
  EXPECT_EQ(bus.trace[3].address, 0x000000u);
}

TEST(Cpu65816Indirect, ALongPointerInPageZeroRunsOnIntoTheStackPage) {
  // Section 7.2.1 names the exceptions: "[Direct] and [Direct],Y addressing modes
  // and the PEI instruction which will increment from 0000FE or 0000FF into the
  // Stack area". A three-byte pointer based at $0000FE reads on through $000100.
  auto bus = busWith({{0x001000, 0xA7},
                      {0x001001, 0xFE},
                      {0x0000FE, 0x34},
                      {0x0000FF, 0x12},
                      {0x000100, 0x7E},
                      {0x7E1234, 0x5A}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .p = kEightBit, .e = true});
  EXPECT_EQ(run(bus, cpu), 6u);
  EXPECT_EQ(cpu.state().a & 0xFF, 0x5A);
  ASSERT_EQ(bus.trace.size(), 6u);
  EXPECT_EQ(bus.trace[2].address, 0x0000FEu);
  EXPECT_EQ(bus.trace[3].address, 0x0000FFu);
  EXPECT_EQ(bus.trace[4].address, 0x000100u);
}

TEST(Cpu65816Indirect, ALongIndexedPointerInPageZeroAlsoRunsOnIntoTheStackPage) {
  // Section 7.2.1 names [Direct],Y beside [Direct], so its pointer leaves page zero
  // on the same terms — and the index it adds afterwards changes nothing about how
  // the pointer itself was read.
  auto bus = busWith({{0x001000, 0xB7},
                      {0x001001, 0xFE},
                      {0x0000FE, 0x34},
                      {0x0000FF, 0x12},
                      {0x000100, 0x7E},
                      {0x7E1236, 0x5A}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .y = 0x02, .p = kEightBit, .e = true});
  EXPECT_EQ(run(bus, cpu), 6u);
  EXPECT_EQ(cpu.state().a & 0xFF, 0x5A);
  ASSERT_EQ(bus.trace.size(), 6u);
  EXPECT_EQ(bus.trace[2].address, 0x0000FEu);
  EXPECT_EQ(bus.trace[3].address, 0x0000FFu);
  EXPECT_EQ(bus.trace[4].address, 0x000100u);
}

TEST(Cpu65816Indirect, AStackPointerReadNeverWrapsInsideAPage) {
  // Stack-relative indirect is one of the modes the 65816 added, so its pointer is
  // read the way Eyes & Lichty describe the new opcodes — page wrapping "is only
  // enforced for 6502 and 65C02 instructions" — and a pointer based at $0001FF
  // reads its high byte from $000200 even with the direct register at zero.
  auto bus = busWith({{0x001000, 0xB3},
                      {0x001001, 0x0F},
                      {0x0001FF, 0x34},
                      {0x000200, 0x12},
                      {0x011236, 0x7F}});
  Cpu65816 cpu(Cpu65816State{
      .pc = 0x1000, .s = 0x01F0, .y = 0x02, .p = kEightBit, .dbr = 0x01, .e = true});
  EXPECT_EQ(run(bus, cpu), 7u);
  EXPECT_EQ(cpu.state().a & 0xFF, 0x7F);
  ASSERT_EQ(bus.trace.size(), 7u);
  EXPECT_EQ(bus.trace[3].address, 0x0001FFu);
  EXPECT_EQ(bus.trace[4].address, 0x000200u);
}

TEST(Cpu65816Indirect, ANativePointerWrapsAtTheEndOfBankZero) {
  // Section 7.2.1: the effective address of the direct modes "will always be in the
  // Native mode range 000000 to 00FFFF", so a pointer based at the last byte of bank
  // zero reads its high byte from the first — never from bank one.
  auto bus = busWith({{0x001000, 0xB2},
                      {0x001001, 0xFF},
                      {0x00FFFF, 0x34},
                      {0x000000, 0x12},
                      {0x011234, 0x7F}});
  Cpu65816 cpu(
      Cpu65816State{.pc = 0x1000, .d = 0xFF00, .p = kEightBit, .dbr = 0x01});
  EXPECT_EQ(run(bus, cpu), 5u);
  EXPECT_EQ(cpu.state().a & 0xFF, 0x7F);
  ASSERT_EQ(bus.trace.size(), 5u);
  EXPECT_EQ(bus.trace[3].address, 0x000000u);
}

TEST(Cpu65816Indirect, TheEmulatedStackPointerAddressesFromPageOne) {
  // Section 7.1: "In the Emulation mode, the Stack address range is 000100 to
  // 0001FF." The high byte of the stack pointer is $01 there whatever was written to
  // it, so a stack-relative operand is addressed from page one — and an offset that
  // carries out of the low byte reaches the page above.
  auto bus = busWith({{0x001000, 0xA3}, {0x001001, 0xE0}, {0x000207, 0x7F}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .s = 0x8F27, .p = kEightBit, .e = true});
  EXPECT_EQ(run(bus, cpu), 4u);
  EXPECT_EQ(cpu.state().a & 0xFF, 0x7F);
  ASSERT_EQ(bus.trace.size(), 4u);
  EXPECT_EQ(bus.trace[3].address, 0x000207u);
}

// ---- what the cycles carry ----

TEST(Cpu65816Indirect, ASixteenBitStoreWritesBothBytesThroughThePointer) {
  // Note 1 adds a cycle per extra byte: a sixteen-bit store writes the low byte at
  // the pointer's address and the high byte one on from it.
  auto bus = busWith(
      {{0x001000, 0x92}, {0x001001, 0x10}, {0x000010, 0x34}, {0x000011, 0x12}});
  Cpu65816 cpu(
      Cpu65816State{.pc = 0x1000, .a = 0x1234, .p = kCpuFlagX, .dbr = 0x01});
  EXPECT_EQ(run(bus, cpu), 6u);
  EXPECT_EQ(bus.mem[0x011234], 0x34);
  EXPECT_EQ(bus.mem[0x011235], 0x12);
  ASSERT_EQ(bus.trace.size(), 6u);
  EXPECT_EQ(bus.trace[4].signals[kSignalRw], 'w');
  EXPECT_EQ(bus.trace[5].signals[kSignalRw], 'w');
}

TEST(Cpu65816Indirect, ThePointerBytesAreValidDataAddresses) {
  // Rows 11 to 15 mark every pointer cycle VDA: reading a pointer is an ordinary
  // data access, unlike the internal cycles around it.
  auto bus = busWith({{0x001000, 0xA1},
                      {0x001001, 0x10},
                      {0x000014, 0x34},
                      {0x000015, 0x12},
                      {0x011234, 0x7F}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .x = 0x04, .p = kEightBit, .dbr = 0x01});
  ASSERT_EQ(run(bus, cpu), 6u);
  ASSERT_EQ(bus.trace.size(), 6u);
  EXPECT_EQ(bus.trace[3].signals[kSignalVda], 'd');
  EXPECT_EQ(bus.trace[3].signals[kSignalRw], 'r');
  EXPECT_EQ(bus.trace[4].signals[kSignalVda], 'd');
}

TEST(Cpu65816Indirect, AnInstructionCanBeSnapshotWhileItsPointerIsHalfRead) {
  // Instruction progress is value state, so a snapshot taken between the pointer's
  // two bytes restores into a processor that finishes the instruction identically.
  auto bus = busWith({{0x001000, 0xB1},
                      {0x001001, 0x10},
                      {0x000010, 0x34},
                      {0x000011, 0x12},
                      {0x011236, 0x7F}});
  Cpu65816 cpu(
      Cpu65816State{.pc = 0x1000, .y = 0x02, .p = kEightBit, .dbr = 0x01});
  bus.cpu = &cpu;
  cpu.stepCycle(bus);  // the opcode fetch
  cpu.stepCycle(bus);  // the offset
  cpu.stepCycle(bus);  // the pointer's low byte
  const Cpu65816State midway = cpu.state();
  EXPECT_FALSE(cpu.atInstructionBoundary());

  auto second = busWith({{0x001000, 0xB1},
                         {0x001001, 0x10},
                         {0x000010, 0x34},
                         {0x000011, 0x12},
                         {0x011236, 0x7F}});
  Cpu65816 resumed(midway);
  second.cpu = &resumed;
  while (!resumed.atInstructionBoundary()) resumed.stepCycle(second);
  while (!cpu.atInstructionBoundary()) cpu.stepCycle(bus);
  EXPECT_EQ(resumed.state().a, cpu.state().a);
  EXPECT_EQ(resumed.state().p, cpu.state().p);
}

TEST(Cpu65816Indirect, EveryIndirectOpcodeRunsOnTheCycleEngine) {
  // The family's coverage: seven addressing modes across the eight accumulator
  // instructions. Each runs a cycle at a time and narrates every one of them, which
  // is what lets the recorded traces be compared cycle for cycle.
  for (int column : {0x01, 0x11, 0x12, 0x07, 0x17, 0x03, 0x13}) {
    for (int i = 0; i < 8; ++i) {
      const auto opcode = static_cast<std::uint8_t>(column + i * 0x20);
      auto bus = busWith({{0x001000, opcode}});
      Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .s = 0x01FF});
      const std::uint32_t cycles = run(bus, cpu);
      EXPECT_GE(cycles, 2u) << "opcode " << std::hex << int{opcode};
      EXPECT_EQ(bus.trace.size(), cycles) << "opcode " << std::hex << int{opcode};
    }
  }
}

}  // namespace
