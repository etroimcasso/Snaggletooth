#include "snaggletooth/cpu/cpu65816.h"
#include "vector_harness.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <initializer_list>
#include <utility>

// The stack instructions, cycle by cycle. The recorded vectors prove every opcode's
// whole trace; what is proven here is the documented shape behind them, with each
// expectation traced to the line that states it: W65C816S datasheet Table 5-7 rows
// 22b (pull a register), 22c (push a register), 22d (PEA), 22e (PEI) and 22f (PER),
// their notes 1 and 2, section 7.1 on the stack address range, and section 7.2.1 on
// the direct addressing range PEI is named as leaving.

namespace {

using snaggletooth::Cpu65816;
using snaggletooth::Cpu65816State;
using snaggletooth::kCpuFlagC;
using snaggletooth::kCpuFlagM;
using snaggletooth::kCpuFlagN;
using snaggletooth::kCpuFlagX;
using snaggletooth::kCpuFlagZ;
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

// ---- where the cycles go (Table 5-7 rows 22b and 22c) ----

TEST(Cpu65816Stack, APushSpendsOneInternalCycleBeforeItWrites) {
  // Row 22c: the opcode, one internal cycle at PBR,PC+1, then the write at 0,S.
  // Three cycles for an eight-bit register.
  auto bus = busWith({{0x001000, 0x48}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .s = 0x01FF, .a = 0x7F, .p = kEightBit});
  EXPECT_EQ(run(bus, cpu), 3u);
  ASSERT_EQ(bus.trace.size(), 3u);
  EXPECT_EQ(bus.trace[1].address, 0x001001u);
  EXPECT_EQ(bus.trace[1].signals[kSignalVda], '-');
  EXPECT_EQ(bus.trace[1].signals[kSignalVpa], '-');
  EXPECT_EQ(bus.trace[2].address, 0x0001FFu);
  EXPECT_EQ(bus.trace[2].signals[kSignalRw], 'w');
  EXPECT_EQ(bus.mem[0x0001FF], 0x7F);
  EXPECT_EQ(cpu.state().s, 0x01FE);
}

TEST(Cpu65816Stack, APullSpendsTwoInternalCyclesBeforeItReads) {
  // Row 22b: the opcode, two internal cycles both at PBR,PC+1, then the read at
  // 0,S+1. Four cycles for an eight-bit register — one more than the matching push.
  auto bus = busWith({{0x001000, 0x68}, {0x000200, 0x7F}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .s = 0x01FF, .p = kEightBit});
  EXPECT_EQ(run(bus, cpu), 4u);
  ASSERT_EQ(bus.trace.size(), 4u);
  EXPECT_EQ(bus.trace[1].address, 0x001001u);
  EXPECT_EQ(bus.trace[2].address, 0x001001u);
  EXPECT_EQ(bus.trace[1].signals[kSignalVda], '-');
  EXPECT_EQ(bus.trace[2].signals[kSignalVda], '-');
  EXPECT_EQ(bus.trace[3].signals[kSignalRw], 'r');
}

TEST(Cpu65816Stack, AWiderRegisterCostsOneMoreCycle) {
  // Note 1, "add 1 cycle for M=0 or X=0": a sixteen-bit push moves a second byte,
  // and row 22c's cycles 3a and 3 put the high byte at 0,S and the low byte at
  // 0,S-1 — so a pull reads the word back low byte first.
  auto bus = busWith({{0x001000, 0x48}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .s = 0x01FF, .a = 0x1234, .p = kCpuFlagX});
  EXPECT_EQ(run(bus, cpu), 4u);
  ASSERT_EQ(bus.trace.size(), 4u);
  EXPECT_EQ(bus.trace[2].address, 0x0001FFu);
  EXPECT_EQ(int{*bus.trace[2].value}, 0x12);
  EXPECT_EQ(bus.trace[3].address, 0x0001FEu);
  EXPECT_EQ(int{*bus.trace[3].value}, 0x34);
  EXPECT_EQ(cpu.state().s, 0x01FD);
}

TEST(Cpu65816Stack, TheDirectRegisterIsAlwaysAWholeWord) {
  // Row 22c lists PHD among the pushes, and the direct register is sixteen bits
  // whatever the accumulator width is — so PHD moves two bytes and takes four
  // cycles even with M set.
  auto bus = busWith({{0x001000, 0x0B}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .s = 0x01FF, .d = 0x1234, .p = kEightBit});
  EXPECT_EQ(run(bus, cpu), 4u);
  EXPECT_EQ(bus.mem[0x0001FF], 0x12);
  EXPECT_EQ(bus.mem[0x0001FE], 0x34);
}

TEST(Cpu65816Stack, TheBankRegistersMoveOneByteWhateverTheWidths) {
  // PHB and PHK carry a bank register, which is eight bits regardless of M — three
  // cycles each, with no second byte to move.
  auto phb = busWith({{0x001000, 0x8B}});
  Cpu65816 first(Cpu65816State{.pc = 0x1000, .s = 0x01FF, .p = kCpuFlagX, .dbr = 0x7E});
  EXPECT_EQ(run(phb, first), 3u);
  EXPECT_EQ(phb.mem[0x0001FF], 0x7E);

  auto phk = busWith({{0x7F1000, 0x4B}});
  Cpu65816 second(
      Cpu65816State{.pc = 0x1000, .s = 0x01FF, .p = kCpuFlagX, .pbr = 0x7F});
  EXPECT_EQ(run(phk, second), 3u);
  EXPECT_EQ(phk.mem[0x0001FF], 0x7F);
}

// ---- what a pull lands (Table 5-7 row 22b) ----

TEST(Cpu65816Stack, PullingARegisterSetsItsFlagsAtItsOwnWidth) {
  // A sixteen-bit pull reads the low byte from 0,S+1 and the high from 0,S+2, and
  // sets N and Z on the whole word.
  auto bus = busWith({{0x001000, 0x68}, {0x000200, 0x34}, {0x000201, 0x82}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .s = 0x01FF, .p = kCpuFlagX});
  EXPECT_EQ(run(bus, cpu), 5u);
  EXPECT_EQ(cpu.state().a, 0x8234);
  EXPECT_TRUE(cpu.state().p & kCpuFlagN);
  EXPECT_FALSE(cpu.state().p & kCpuFlagZ);
  ASSERT_EQ(bus.trace.size(), 5u);
  EXPECT_EQ(bus.trace[3].address, 0x000200u);
  EXPECT_EQ(bus.trace[4].address, 0x000201u);
}

TEST(Cpu65816Stack, AnEightBitPullLeavesTheAccumulatorsHighByteAlone) {
  // With M set the pull moves one byte into A and B keeps what it held.
  auto bus = busWith({{0x001000, 0x68}, {0x000200, 0x7F}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .s = 0x01FF, .a = 0xAB00, .p = kEightBit});
  EXPECT_EQ(run(bus, cpu), 4u);
  EXPECT_EQ(cpu.state().a, 0xAB7F);
}

TEST(Cpu65816Stack, PullingTheStatusByteSettlesTheWidthsAtOnce) {
  // PLP replaces the whole status byte, and narrowing the index registers clears
  // their high bytes within the instruction rather than on the next one.
  auto bus = busWith({{0x001000, 0x28}, {0x000200, kCpuFlagX}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .s = 0x01FF, .x = 0x1234, .y = 0x5678});
  EXPECT_EQ(run(bus, cpu), 4u);
  EXPECT_EQ(cpu.state().p, kCpuFlagX);
  EXPECT_EQ(cpu.state().x, 0x0034);
  EXPECT_EQ(cpu.state().y, 0x0078);
}

TEST(Cpu65816Stack, TheStatusByteReportedIsTheOneEachCycleRanUnder) {
  // The widths a cycle reports are the ones it ran under, so a PLP that clears them
  // reports the old widths on every cycle including its last — the change settles at
  // the end of it.
  auto bus = busWith({{0x001000, 0x28}, {0x000200, 0x00}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .s = 0x01FF, .p = kEightBit});
  EXPECT_EQ(run(bus, cpu), 4u);
  ASSERT_EQ(bus.trace.size(), 4u);
  EXPECT_EQ(bus.trace[0].signals, "dp-r-mx-");
  EXPECT_EQ(bus.trace[3].signals, "d--r-mx-");
  EXPECT_EQ(cpu.state().p, 0x00);
}

// ---- the stack address range in emulation mode (section 7.1) ----

TEST(Cpu65816Stack, A6502OriginalPullWrapsInsidePageOne) {
  // Section 7.1: in emulation the stack range is 000100 to 0001FF, and PLA is not
  // among the instructions named as leaving it. At S = $01FF the increment comes
  // back round to $0100 rather than stepping into page two.
  auto bus = busWith({{0x001000, 0x68}, {0x000100, 0x7F}, {0x000200, 0x5A}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .s = 0x01FF, .p = kEightBit, .e = true});
  EXPECT_EQ(run(bus, cpu), 4u);
  EXPECT_EQ(cpu.state().a & 0xFF, 0x7F);
  EXPECT_EQ(cpu.state().s, 0x0100);
  ASSERT_EQ(bus.trace.size(), 4u);
  EXPECT_EQ(bus.trace[3].address, 0x000100u);
}

TEST(Cpu65816Stack, AnInstructionThe65816AddedStepsOutOfPageOne) {
  // Section 7.1 names PLD among the instructions that increment beyond the emulated
  // stack range: at S = $01FF it reads on into page two, and the pointer is put back
  // inside page one once the transfer is done.
  auto bus = busWith({{0x001000, 0x2B}, {0x000200, 0x34}, {0x000201, 0x12}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .s = 0x01FF, .p = kEightBit, .e = true});
  EXPECT_EQ(run(bus, cpu), 5u);
  EXPECT_EQ(cpu.state().d, 0x1234);
  EXPECT_EQ(cpu.state().s, 0x0101);
  ASSERT_EQ(bus.trace.size(), 5u);
  EXPECT_EQ(bus.trace[3].address, 0x000200u);
  EXPECT_EQ(bus.trace[4].address, 0x000201u);
}

TEST(Cpu65816Stack, PullingTheDataBankAlsoStepsOutOfPageOne) {
  // PLB moves a single byte, so the range rule is visible in the address it reads
  // rather than in where the pointer lands: at S = $01FF it reads $000200, and the
  // pointer settles back to $0100 either way.
  auto bus = busWith({{0x001000, 0xAB}, {0x000100, 0x5A}, {0x000200, 0x7E}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .s = 0x01FF, .p = kEightBit, .e = true});
  EXPECT_EQ(run(bus, cpu), 4u);
  EXPECT_EQ(int{cpu.state().dbr}, 0x7E);
  EXPECT_EQ(cpu.state().s, 0x0100);
  ASSERT_EQ(bus.trace.size(), 4u);
  EXPECT_EQ(bus.trace[3].address, 0x000200u);
}

TEST(Cpu65816Stack, AMultiByteEmulatedPushRunsBelowPageOne) {
  // The same rule on the way down: PHD at S = $0100 writes its high byte there and
  // its low byte at $0000FF, below the emulated stack page, before the pointer is
  // put back.
  auto bus = busWith({{0x001000, 0x0B}});
  Cpu65816 cpu(
      Cpu65816State{.pc = 0x1000, .s = 0x0100, .d = 0x1234, .p = kEightBit, .e = true});
  EXPECT_EQ(run(bus, cpu), 4u);
  EXPECT_EQ(bus.mem[0x000100], 0x12);
  EXPECT_EQ(bus.mem[0x0000FF], 0x34);
  EXPECT_EQ(cpu.state().s, 0x01FE);
}

TEST(Cpu65816Stack, ASingleByteEmulatedPushCannotTellTheTwoRulesApart) {
  // A one-byte push writes at the pointer and steps it once, so both rules put the
  // byte at the same address and leave the pointer in the same place. PHA and PHB
  // are on opposite sides of section 7.1's list and are indistinguishable here — the
  // range rule only becomes observable on a pull or on a longer transfer.
  auto pha = busWith({{0x001000, 0x48}});
  Cpu65816 first(
      Cpu65816State{.pc = 0x1000, .s = 0x0100, .a = 0x7F, .p = kEightBit, .e = true});
  EXPECT_EQ(run(pha, first), 3u);

  auto phb = busWith({{0x001000, 0x8B}});
  Cpu65816 second(Cpu65816State{
      .pc = 0x1000, .s = 0x0100, .p = kEightBit, .dbr = 0x7F, .e = true});
  EXPECT_EQ(run(phb, second), 3u);

  EXPECT_EQ(pha.trace[2].address, phb.trace[2].address);
  EXPECT_EQ(first.state().s, second.state().s);
  EXPECT_EQ(first.state().s, 0x01FF);
}

TEST(Cpu65816Stack, NativeModeTreatsBothKindsOfStackInstructionAlike) {
  // Section 7.1's exception list is about the emulated page. With E clear the stack
  // may use all of bank zero, so PHA and PHD step the pointer the same way — and a
  // push at S = $0000 wraps to the top of the bank rather than into page one.
  auto bus = busWith({{0x001000, 0x48}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .s = 0x0000, .a = 0x7F, .p = kEightBit});
  EXPECT_EQ(run(bus, cpu), 3u);
  EXPECT_EQ(bus.mem[0x000000], 0x7F);
  EXPECT_EQ(cpu.state().s, 0xFFFF);
}

// ---- pushing an address the instruction works out (rows 22d, 22e, 22f) ----

TEST(Cpu65816Stack, PushEffectiveAbsolutePushesItsOwnOperand) {
  // Row 22d: the opcode, AAL and AAH as operand fetches, then the two writes. Five
  // cycles, no internal one — and the operand is pushed as it was read, with no
  // addressing applied to it.
  auto bus = busWith({{0x001000, 0xF4}, {0x001001, 0x34}, {0x001002, 0x12}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .s = 0x01FF, .p = kEightBit});
  EXPECT_EQ(run(bus, cpu), 5u);
  ASSERT_EQ(bus.trace.size(), 5u);
  EXPECT_EQ(bus.trace[1].signals[kSignalVpa], 'p');
  EXPECT_EQ(bus.trace[2].signals[kSignalVpa], 'p');
  EXPECT_EQ(bus.mem[0x0001FF], 0x12);
  EXPECT_EQ(bus.mem[0x0001FE], 0x34);
  EXPECT_EQ(cpu.state().pc, 0x1003);
}

TEST(Cpu65816Stack, PushEffectiveIndirectPushesTheWordTheDirectPageHolds) {
  // Row 22e: the opcode, DO, then AAL at 0,D+DO and AAH at 0,D+DO+1, then the two
  // writes. Six cycles with a page-aligned direct register.
  auto bus = busWith({{0x001000, 0xD4},
                      {0x001001, 0x10},
                      {0x000010, 0x34},
                      {0x000011, 0x12}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .s = 0x01FF, .p = kEightBit});
  EXPECT_EQ(run(bus, cpu), 6u);
  ASSERT_EQ(bus.trace.size(), 6u);
  EXPECT_EQ(bus.trace[2].address, 0x000010u);
  EXPECT_EQ(bus.trace[3].address, 0x000011u);
  EXPECT_EQ(bus.mem[0x0001FF], 0x12);
  EXPECT_EQ(bus.mem[0x0001FE], 0x34);
}

TEST(Cpu65816Stack, ADirectRegisterWithALowByteCostsPushEffectiveIndirectACycle) {
  // Note 2, "add 1 cycle for direct register low (DL) not equal 0", and row 22e's
  // cycle 2a: the extra cycle is internal and parks on the address the offset was
  // fetched from.
  auto bus = busWith({{0x001000, 0xD4},
                      {0x001001, 0x10},
                      {0x000047, 0x34},
                      {0x000048, 0x12}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .s = 0x01FF, .d = 0x0037, .p = kEightBit});
  EXPECT_EQ(run(bus, cpu), 7u);
  ASSERT_EQ(bus.trace.size(), 7u);
  EXPECT_EQ(bus.trace[2].address, 0x001001u);
  EXPECT_EQ(bus.trace[2].signals[kSignalVda], '-');
  EXPECT_EQ(bus.trace[2].signals[kSignalVpa], '-');
  EXPECT_EQ(bus.trace[3].address, 0x000047u);
}

TEST(Cpu65816Stack, PushEffectiveIndirectReadsOnPastTheEmulatedDirectPage) {
  // Section 7.2.1 names PEI, with [Direct] and [Direct],Y, as leaving the emulated
  // direct addressing range: at D = 0 and DO = $FF the word's low byte comes from
  // $0000FF and its high byte from $000100 rather than wrapping back to $000000.
  auto bus = busWith({{0x001000, 0xD4},
                      {0x001001, 0xFF},
                      {0x0000FF, 0x34},
                      {0x000100, 0x12},
                      {0x000000, 0x99}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .s = 0x01F0, .p = kEightBit, .e = true});
  EXPECT_EQ(run(bus, cpu), 6u);
  ASSERT_EQ(bus.trace.size(), 6u);
  EXPECT_EQ(bus.trace[2].address, 0x0000FFu);
  EXPECT_EQ(bus.trace[3].address, 0x000100u);
  EXPECT_EQ(bus.mem[0x0001F0], 0x12);
  EXPECT_EQ(bus.mem[0x0001EF], 0x34);
}

TEST(Cpu65816Stack, PushEffectiveRelativeSpendsACycleOnItsAddition) {
  // Row 22f: the opcode, both offset bytes, an internal cycle parked on the second
  // of them, then the two writes. Six cycles, and the offset is measured from the
  // address after the instruction — $1003 here, so $1003 + $0010 is pushed.
  auto bus = busWith({{0x001000, 0x62}, {0x001001, 0x10}, {0x001002, 0x00}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .s = 0x01FF, .p = kEightBit});
  EXPECT_EQ(run(bus, cpu), 6u);
  ASSERT_EQ(bus.trace.size(), 6u);
  EXPECT_EQ(bus.trace[3].address, 0x001002u);
  EXPECT_EQ(bus.trace[3].signals[kSignalVda], '-');
  EXPECT_EQ(bus.trace[3].signals[kSignalVpa], '-');
  EXPECT_EQ(bus.mem[0x0001FF], 0x10);
  EXPECT_EQ(bus.mem[0x0001FE], 0x13);
}

TEST(Cpu65816Stack, PushEffectiveRelativeWrapsItsSumInsideTheBank) {
  // The sum is a sixteen-bit program address: a displacement that runs past the end
  // of the bank comes back round to its start rather than reaching the next one.
  auto bus = busWith({{0x7E1000, 0x62}, {0x7E1001, 0x00}, {0x7E1002, 0xF0}});
  Cpu65816 cpu(
      Cpu65816State{.pc = 0x1000, .s = 0x01FF, .p = kEightBit, .pbr = 0x7E});
  EXPECT_EQ(run(bus, cpu), 6u);
  EXPECT_EQ(bus.mem[0x0001FF], 0x00);
  EXPECT_EQ(bus.mem[0x0001FE], 0x03);
}

TEST(Cpu65816Stack, TheThreePushEffectiveInstructionsLeaveTheFlagsAlone) {
  // None of the three names a register to set N and Z from; the status byte comes
  // out exactly as it went in.
  for (const auto& [opcode, cycles] :
       {std::pair<std::uint8_t, std::uint32_t>{0xF4, 5},
        std::pair<std::uint8_t, std::uint32_t>{0xD4, 6},
        std::pair<std::uint8_t, std::uint32_t>{0x62, 6}}) {
    auto bus = busWith({{0x001000, opcode},
                        {0x001001, 0x10},
                        {0x001002, 0x00},
                        {0x000010, 0x34},
                        {0x000011, 0x12}});
    Cpu65816 cpu(Cpu65816State{
        .pc = 0x1000, .s = 0x01FF, .p = static_cast<std::uint8_t>(kEightBit | kCpuFlagC)});
    EXPECT_EQ(run(bus, cpu), cycles) << "opcode " << int{opcode};
    EXPECT_EQ(int{cpu.state().p}, int{kEightBit | kCpuFlagC}) << "opcode " << int{opcode};
  }
}

// ---- a mid-instruction snapshot is a plain value ----

TEST(Cpu65816Stack, AStackInstructionResumesFromAMidInstructionSnapshot) {
  // The cycle engine keeps its progress in the state struct, so a copy taken between
  // the two writes of a PEI finishes the instruction identically.
  auto bus = busWith({{0x001000, 0xD4},
                      {0x001001, 0x10},
                      {0x000010, 0x34},
                      {0x000011, 0x12}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .s = 0x01FF, .p = kEightBit});
  bus.cpu = &cpu;
  for (int i = 0; i < 5; ++i) cpu.stepCycle(bus);
  const Cpu65816State midway = cpu.state();
  EXPECT_FALSE(cpu.atInstructionBoundary());

  auto second = busWith({{0x001000, 0xD4},
                         {0x001001, 0x10},
                         {0x000010, 0x34},
                         {0x000011, 0x12}});
  Cpu65816 resumed(midway);
  second.cpu = &resumed;
  while (!resumed.atInstructionBoundary()) resumed.stepCycle(second);
  while (!cpu.atInstructionBoundary()) cpu.stepCycle(bus);
  EXPECT_EQ(second.mem[0x0001FE], bus.mem[0x0001FE]);
  EXPECT_EQ(resumed.state().s, cpu.state().s);
}

TEST(Cpu65816Stack, EveryStackOpcodeRunsOnTheCycleEngine) {
  // The family's coverage: seven pushes, six pulls, and the three push-effective
  // instructions. Each runs a cycle at a time and narrates every one of them, which
  // is what lets the recorded traces be compared cycle for cycle.
  for (int opcode : {0x48, 0xDA, 0x5A, 0x08, 0x8B, 0x4B, 0x0B,   // push
                     0x68, 0xFA, 0x7A, 0x28, 0xAB, 0x2B,         // pull
                     0xF4, 0xD4, 0x62}) {                        // PEA, PEI, PER
    auto bus = busWith({{0x001000, static_cast<std::uint8_t>(opcode)}});
    Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .s = 0x01FF});
    const std::uint32_t cycles = run(bus, cpu);
    EXPECT_GE(cycles, 2u) << "opcode " << std::hex << opcode;
    EXPECT_EQ(bus.trace.size(), cycles) << "opcode " << std::hex << opcode;
  }
}

}  // namespace
