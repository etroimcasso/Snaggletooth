#include "snaggletooth/cpu/cpu65816.h"
#include "vector_harness.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <initializer_list>
#include <utility>

// The control-flow instructions, cycle by cycle. The recorded vectors prove every
// opcode's whole trace; what is proven here is the documented shape behind them,
// with each expectation traced to the line that states it: W65C816S datasheet
// Table 5-7 rows 20 (relative), 21 (relative long), 1b and 4b (the jumps), 3a and
// 3b (the indirect jumps), 2a and 2b (the indexed indirect jump and call), 1c and
// 4c (the calls), and 22g, 22h and 22i (the returns); notes 5 and 6 on the branch
// cycles and note 7 on emulation mode; section 7.1 on the stack address range and
// section 7.9 on which bank an indirect jump takes its pointer from.

namespace {

using snaggletooth::Cpu65816;
using snaggletooth::Cpu65816State;
using snaggletooth::kCpuFlagC;
using snaggletooth::kCpuFlagM;
using snaggletooth::kCpuFlagX;
using snaggletooth::kCpuFlagZ;
using snaggletooth::cpu_vectors::kSignalM;
using snaggletooth::cpu_vectors::kSignalRw;
using snaggletooth::cpu_vectors::kSignalVda;
using snaggletooth::cpu_vectors::kSignalVpa;
using snaggletooth::cpu_vectors::kSignalX;
using snaggletooth::cpu_vectors::RecordingBus;

RecordingBus busWith(
    std::initializer_list<std::pair<std::uint32_t, std::uint8_t>> ram) {
  RecordingBus bus;
  for (const auto& [address, value] : ram) bus.mem[address] = value;
  return bus;
}

// Runs one instruction and returns how many cycles it took, with the bus recording
// pin states from the processor as it goes.
std::uint32_t run(RecordingBus& bus, Cpu65816& cpu) {
  bus.cpu = &cpu;
  return cpu.stepInstruction(bus);
}

// The eight-bit widths as a processor status byte.
constexpr std::uint8_t kEightBit = kCpuFlagM | kCpuFlagX;

// ---- the relative branches (Table 5-7 row 20, notes 5 and 6) ----

TEST(Cpu65816ControlFlow, AnUntakenBranchIsTwoCyclesAndReadsNothingElse) {
  // Row 20: the opcode and the displacement. Cycle 2a is note 5's, spent only when
  // the branch is taken — so a branch that falls through never drives a third cycle.
  auto bus = busWith({{0x001000, 0xB0}, {0x001001, 0x10}});  // BCS +$10, carry clear
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .p = kEightBit});
  EXPECT_EQ(run(bus, cpu), 2u);
  ASSERT_EQ(bus.trace.size(), 2u);
  EXPECT_EQ(cpu.state().pc, 0x1002);
}

TEST(Cpu65816ControlFlow, ATakenBranchSpendsOneCycleParkedOnItsDisplacement) {
  // Note 5, "add 1 cycle if branch is taken", and row 20's cycle 2a gives that
  // cycle's address as PBR,PC+1 — the displacement's own address, not the
  // destination. The cycle drives neither valid-address pin.
  auto bus = busWith({{0x001000, 0xB0}, {0x001001, 0x10}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .p = static_cast<std::uint8_t>(kEightBit | kCpuFlagC)});
  EXPECT_EQ(run(bus, cpu), 3u);
  ASSERT_EQ(bus.trace.size(), 3u);
  EXPECT_EQ(bus.trace[2].address, 0x001001u);
  EXPECT_EQ(bus.trace[2].signals[kSignalVda], '-');
  EXPECT_EQ(bus.trace[2].signals[kSignalVpa], '-');
  EXPECT_EQ(cpu.state().pc, 0x1012);
}

TEST(Cpu65816ControlFlow, ADisplacementIsSignedAndCountsFromTheNextInstruction) {
  // Section 3.5.21: the displacement is added to the program counter as it stands
  // after the instruction — so $FE, which is minus two, branches to the branch.
  auto bus = busWith({{0x001000, 0x80}, {0x001001, 0xFE}});  // BRA -2
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .p = kEightBit});
  EXPECT_EQ(run(bus, cpu), 3u);
  EXPECT_EQ(cpu.state().pc, 0x1000);
}

TEST(Cpu65816ControlFlow, EmulationPaysASecondCycleWhenTheBranchCrossesAPage) {
  // Note 6, "add 1 cycle if branch is taken across page boundaries in 6502
  // emulation mode (E=1)": row 20's cycle 2b, parked at the same address as 2a.
  auto bus = busWith({{0x0010F0, 0x80}, {0x0010F1, 0x20}});  // BRA to $1112
  Cpu65816 cpu(Cpu65816State{.pc = 0x10F0, .s = 0x01FF, .p = kEightBit, .e = true});
  EXPECT_EQ(run(bus, cpu), 4u);
  ASSERT_EQ(bus.trace.size(), 4u);
  EXPECT_EQ(bus.trace[2].address, 0x0010F1u);
  EXPECT_EQ(bus.trace[3].address, 0x0010F1u);
  EXPECT_EQ(bus.trace[3].signals[kSignalVda], '-');
  EXPECT_EQ(cpu.state().pc, 0x1112);
}

TEST(Cpu65816ControlFlow, NativeModePaysNothingForCrossingAPage) {
  // Note 6 is conditioned on emulation mode alone, so the same branch that costs
  // four cycles above costs three here. The destination is identical.
  auto bus = busWith({{0x0010F0, 0x80}, {0x0010F1, 0x20}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x10F0, .p = kEightBit});
  EXPECT_EQ(run(bus, cpu), 3u);
  EXPECT_EQ(cpu.state().pc, 0x1112);
}

TEST(Cpu65816ControlFlow, ABranchWrapsWithinItsBankAndLeavesTheBankAlone) {
  // Section 3.4: the program bank register is not affected by relative addressing,
  // so a branch off the end of a bank continues at its start rather than moving on
  // to the next one.
  auto bus = busWith({{0x7FFFFE, 0x80}, {0x7FFFFF, 0x10}});  // BRA +$10 at $7F:FFFE
  Cpu65816 cpu(Cpu65816State{.pc = 0xFFFE, .p = kEightBit, .pbr = 0x7F});
  EXPECT_EQ(run(bus, cpu), 3u);
  EXPECT_EQ(cpu.state().pc, 0x0010);
  EXPECT_EQ(cpu.state().pbr, 0x7F);
}

// ---- BRL (Table 5-7 row 21) ----

TEST(Cpu65816ControlFlow, BranchLongIsFourCyclesWithItsAdditionParkedOnTheOperand) {
  // Row 21: the opcode, both displacement bytes, then an internal cycle at
  // PBR,PC+2 — the displacement's high byte. Always taken, so there is no
  // condition to pay for and no note 5 cycle on top.
  auto bus = busWith({{0x001000, 0x82}, {0x001001, 0x00}, {0x001002, 0x01}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .p = kEightBit});
  EXPECT_EQ(run(bus, cpu), 4u);
  ASSERT_EQ(bus.trace.size(), 4u);
  EXPECT_EQ(bus.trace[3].address, 0x001002u);
  EXPECT_EQ(bus.trace[3].signals[kSignalVda], '-');
  EXPECT_EQ(cpu.state().pc, 0x1103);
}

TEST(Cpu65816ControlFlow, BranchLongCostsTheSameInEmulationMode) {
  // Row 21 carries neither note 5 nor note 6, so BRL is four cycles in both modes —
  // including across a page, where a short branch would pay for the crossing.
  auto bus = busWith({{0x0010F0, 0x82}, {0x0010F1, 0x20}, {0x0010F2, 0x00}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x10F0, .s = 0x01FF, .p = kEightBit, .e = true});
  EXPECT_EQ(run(bus, cpu), 4u);
  EXPECT_EQ(cpu.state().pc, 0x1113);
}

// ---- the direct jumps (Table 5-7 rows 1b and 4b) ----

TEST(Cpu65816ControlFlow, JumpAbsoluteIsThreeCyclesAndKeepsTheBank) {
  // Row 1b: the opcode and the two destination bytes, and nothing more. Section 3.4
  // lists absolute addressing among the modes that leave the program bank alone.
  auto bus = busWith({{0x7F1000, 0x4C}, {0x7F1001, 0x34}, {0x7F1002, 0x12}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .p = kEightBit, .pbr = 0x7F});
  EXPECT_EQ(run(bus, cpu), 3u);
  EXPECT_EQ(cpu.state().pc, 0x1234);
  EXPECT_EQ(cpu.state().pbr, 0x7F);
}

TEST(Cpu65816ControlFlow, JumpLongTakesItsBankFromAThirdOperandByte) {
  // Row 4b: a fourth cycle reads the destination bank at PBR,PC+3 — through the
  // bank the instruction started in, since the register only moves afterwards.
  // Section 3.4 names JMP absolute long among the instructions that affect it.
  auto bus = busWith({{0x001000, 0x5C},
                      {0x001001, 0x34},
                      {0x001002, 0x12},
                      {0x001003, 0x7E}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .p = kEightBit});
  EXPECT_EQ(run(bus, cpu), 4u);
  ASSERT_EQ(bus.trace.size(), 4u);
  EXPECT_EQ(bus.trace[3].address, 0x001003u);
  EXPECT_EQ(cpu.state().pc, 0x1234);
  EXPECT_EQ(cpu.state().pbr, 0x7E);
}

// ---- the indirect jumps (Table 5-7 rows 3a and 3b, section 7.9) ----

TEST(Cpu65816ControlFlow, AnIndirectJumpReadsItsPointerFromBankZero) {
  // Section 7.9: "The JMP (a) and JML (a) instructions use the direct Bank for
  // indirect addressing" — bank zero, whatever bank the instruction itself is in.
  // Row 3b gives the two pointer reads as 0,AA and 0,AA+1.
  auto bus = busWith({{0x7F1000, 0x6C},
                      {0x7F1001, 0x00},
                      {0x7F1002, 0x20},
                      {0x002000, 0x34},
                      {0x002001, 0x12}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .p = kEightBit, .pbr = 0x7F});
  EXPECT_EQ(run(bus, cpu), 5u);
  ASSERT_EQ(bus.trace.size(), 5u);
  EXPECT_EQ(bus.trace[3].address, 0x002000u);
  EXPECT_EQ(bus.trace[4].address, 0x002001u);
  EXPECT_EQ(cpu.state().pc, 0x1234);
  EXPECT_EQ(cpu.state().pbr, 0x7F);
}

TEST(Cpu65816ControlFlow, AnIndirectPointerWrapsWithinBankZero) {
  // The pointer lives in bank zero, so its second byte at $FFFF comes from $0000 of
  // the same bank rather than from bank one.
  auto bus = busWith({{0x001000, 0x6C},
                      {0x001001, 0xFF},
                      {0x001002, 0xFF},
                      {0x00FFFF, 0x34},
                      {0x000000, 0x12}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .p = kEightBit});
  EXPECT_EQ(run(bus, cpu), 5u);
  EXPECT_EQ(bus.trace[4].address, 0x000000u);
  EXPECT_EQ(cpu.state().pc, 0x1234);
}

TEST(Cpu65816ControlFlow, TheLongIndirectJumpTakesABankFromAThirdPointerByte) {
  // Row 3a: six cycles, the third pointer byte at 0,AA+2 being the new program
  // bank. Section 3.4 lists JML among the instructions that affect the register.
  auto bus = busWith({{0x001000, 0xDC},
                      {0x001001, 0x00},
                      {0x001002, 0x20},
                      {0x002000, 0x34},
                      {0x002001, 0x12},
                      {0x002002, 0x7E}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .p = kEightBit});
  EXPECT_EQ(run(bus, cpu), 6u);
  ASSERT_EQ(bus.trace.size(), 6u);
  EXPECT_EQ(bus.trace[5].address, 0x002002u);
  EXPECT_EQ(cpu.state().pc, 0x1234);
  EXPECT_EQ(cpu.state().pbr, 0x7E);
}

// ---- the indexed indirect jump (Table 5-7 row 2a, section 7.9) ----

TEST(Cpu65816ControlFlow, TheIndexedIndirectJumpReadsItsPointerFromTheProgramBank) {
  // Section 7.9: "JMP (a,x) and JSR (a,x) use the Program Bank for indirect address
  // tables." Row 2a puts the reads at PBR,AA+X and PBR,AA+X+1, after an internal
  // cycle at PBR,PC+2 — spent whether or not the addition carries, unlike the
  // indexing cycle of the data addressing modes.
  auto bus = busWith({{0x7F1000, 0x7C},
                      {0x7F1001, 0x00},
                      {0x7F1002, 0x20},
                      {0x7F2004, 0x34},
                      {0x7F2005, 0x12}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .x = 0x0004, .p = kEightBit, .pbr = 0x7F});
  EXPECT_EQ(run(bus, cpu), 6u);
  ASSERT_EQ(bus.trace.size(), 6u);
  EXPECT_EQ(bus.trace[3].address, 0x7F1002u);
  EXPECT_EQ(bus.trace[3].signals[kSignalVda], '-');
  EXPECT_EQ(bus.trace[4].address, 0x7F2004u);
  EXPECT_EQ(bus.trace[5].address, 0x7F2005u);
  EXPECT_EQ(cpu.state().pc, 0x1234);
}

TEST(Cpu65816ControlFlow, TheIndexedPointerWrapsWithinTheProgramBank) {
  // The addition stays inside the program bank: indexing past its end comes back at
  // its start rather than crossing into the next bank.
  auto bus = busWith({{0x7F1000, 0x7C},
                      {0x7F1001, 0xFF},
                      {0x7F1002, 0xFF},
                      {0x7F0001, 0x34},
                      {0x7F0002, 0x12}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .x = 0x0002, .p = kEightBit, .pbr = 0x7F});
  EXPECT_EQ(run(bus, cpu), 6u);
  EXPECT_EQ(bus.trace[4].address, 0x7F0001u);
  EXPECT_EQ(cpu.state().pc, 0x1234);
}

// ---- the subroutine calls (Table 5-7 rows 1c, 2b and 4c) ----

TEST(Cpu65816ControlFlow, JumpToSubroutinePushesItsOwnLastByteHighByteFirst) {
  // Row 1c: the opcode, both destination bytes, an internal cycle at PBR,PC+2, then
  // the return address at 0,S and 0,S-1 — high byte first. The address pushed is
  // the instruction's last byte, one short of the next instruction, which is what
  // RTS adds one to.
  auto bus = busWith({{0x001000, 0x20}, {0x001001, 0x34}, {0x001002, 0x12}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .s = 0x01FF, .p = kEightBit});
  EXPECT_EQ(run(bus, cpu), 6u);
  ASSERT_EQ(bus.trace.size(), 6u);
  EXPECT_EQ(bus.trace[3].address, 0x001002u);
  EXPECT_EQ(bus.trace[3].signals[kSignalVda], '-');
  EXPECT_EQ(bus.trace[4].address, 0x0001FFu);
  EXPECT_EQ(bus.trace[4].signals[kSignalRw], 'w');
  EXPECT_EQ(bus.mem[0x0001FF], 0x10);  // the return address's high byte
  EXPECT_EQ(bus.mem[0x0001FE], 0x02);  // and its low byte: $1002
  EXPECT_EQ(cpu.state().pc, 0x1234);
  EXPECT_EQ(cpu.state().s, 0x01FD);
}

TEST(Cpu65816ControlFlow, TheIndexedCallPushesBeforeItReadsItsSecondOperandByte) {
  // Row 2b's ordering is the family's oddity: cycles 3 and 4 write the return
  // address, and only then does cycle 5 read the destination's high byte at
  // PBR,PC+2. Eight cycles in all.
  auto bus = busWith({{0x001000, 0xFC},
                      {0x001001, 0x00},
                      {0x001002, 0x20},
                      {0x002004, 0x34},
                      {0x002005, 0x12}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .s = 0x01FF, .x = 0x0004, .p = kEightBit});
  EXPECT_EQ(run(bus, cpu), 8u);
  ASSERT_EQ(bus.trace.size(), 8u);
  EXPECT_EQ(bus.trace[2].signals[kSignalRw], 'w');
  EXPECT_EQ(bus.trace[3].signals[kSignalRw], 'w');
  EXPECT_EQ(bus.trace[4].address, 0x001002u);  // the high byte, read after the pushes
  EXPECT_EQ(bus.trace[4].signals[kSignalVpa], 'p');
  EXPECT_EQ(bus.mem[0x0001FF], 0x10);
  EXPECT_EQ(bus.mem[0x0001FE], 0x02);
  EXPECT_EQ(cpu.state().pc, 0x1234);
}

TEST(Cpu65816ControlFlow, TheLongCallPushesItsBankFirstAndParksOnTheByteItWrote) {
  // Row 4c: the program bank goes on the stack alone at cycle 4, cycle 5 is an
  // internal cycle at 0,S — the byte just written — cycle 6 reads the destination
  // bank at PBR,PC+3, and the return address follows at 0,S-1 and 0,S-2.
  auto bus = busWith({{0x7F1000, 0x22},
                      {0x7F1001, 0x34},
                      {0x7F1002, 0x12},
                      {0x7F1003, 0x7E}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .s = 0x01FF, .p = kEightBit, .pbr = 0x7F});
  EXPECT_EQ(run(bus, cpu), 8u);
  ASSERT_EQ(bus.trace.size(), 8u);
  EXPECT_EQ(bus.trace[3].address, 0x0001FFu);
  EXPECT_EQ(int{*bus.trace[3].value}, 0x7F);  // the old program bank
  EXPECT_EQ(bus.trace[4].address, 0x0001FFu);
  EXPECT_EQ(bus.trace[4].signals[kSignalVda], '-');
  EXPECT_EQ(bus.trace[5].address, 0x7F1003u);
  EXPECT_EQ(bus.mem[0x0001FE], 0x10);
  EXPECT_EQ(bus.mem[0x0001FD], 0x03);  // the return address is $1003
  EXPECT_EQ(cpu.state().pc, 0x1234);
  EXPECT_EQ(cpu.state().pbr, 0x7E);
  EXPECT_EQ(cpu.state().s, 0x01FC);
}

// ---- the returns (Table 5-7 rows 22g, 22h and 22i) ----

TEST(Cpu65816ControlFlow, ReturnFromSubroutineParksOnTheByteItLastPulled) {
  // Row 22h: two internal cycles at PBR,PC+1, the two address bytes at 0,S+1 and
  // 0,S+2, then a sixth cycle — internal, at 0,S+2, the address the last pull came
  // from — while the address is stepped past the call's last byte.
  auto bus = busWith({{0x001000, 0x60}, {0x0001FE, 0x02}, {0x0001FF, 0x10}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .s = 0x01FD, .p = kEightBit});
  EXPECT_EQ(run(bus, cpu), 6u);
  ASSERT_EQ(bus.trace.size(), 6u);
  EXPECT_EQ(bus.trace[1].address, 0x001001u);
  EXPECT_EQ(bus.trace[2].address, 0x001001u);
  EXPECT_EQ(bus.trace[5].address, 0x0001FFu);
  EXPECT_EQ(bus.trace[5].signals[kSignalVda], '-');
  EXPECT_EQ(cpu.state().pc, 0x1003);
  EXPECT_EQ(cpu.state().s, 0x01FF);
}

TEST(Cpu65816ControlFlow, ACallAndItsReturnLandOnTheNextInstruction) {
  // The pair is the point of the offset: JSR pushes its own last byte and RTS adds
  // one, so control resumes at the instruction after the call.
  auto bus = busWith({{0x001000, 0x20}, {0x001001, 0x00}, {0x001002, 0x20},
                      {0x002000, 0x60}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .s = 0x01FF, .p = kEightBit});
  bus.cpu = &cpu;
  cpu.stepInstruction(bus);
  EXPECT_EQ(cpu.state().pc, 0x2000);
  cpu.stepInstruction(bus);
  EXPECT_EQ(cpu.state().pc, 0x1003);
  EXPECT_EQ(cpu.state().s, 0x01FF);
}

TEST(Cpu65816ControlFlow, TheLongReturnTakesABankByteInsteadOfTheLastInternalCycle) {
  // Row 22i: six cycles like RTS, but the sixth reads the program bank at 0,S+3
  // rather than idling. The address is stepped the same way, so JSL and RTL pair as
  // JSR and RTS do.
  auto bus = busWith({{0x7F1000, 0x6B},
                      {0x0001FD, 0x03},
                      {0x0001FE, 0x10},
                      {0x0001FF, 0x7E}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .s = 0x01FC, .p = kEightBit, .pbr = 0x7F});
  EXPECT_EQ(run(bus, cpu), 6u);
  ASSERT_EQ(bus.trace.size(), 6u);
  EXPECT_EQ(bus.trace[5].address, 0x0001FFu);
  EXPECT_EQ(bus.trace[5].signals[kSignalRw], 'r');
  EXPECT_EQ(cpu.state().pc, 0x1004);
  EXPECT_EQ(cpu.state().pbr, 0x7E);
}

TEST(Cpu65816ControlFlow, ReturnFromInterruptUsesThePulledAddressAsItStands) {
  // Row 22g: the status byte comes back first, at 0,S+1, then the address. An
  // interrupt pushes the address it means to resume at, so — unlike RTS — nothing
  // is added to what was pulled.
  auto bus = busWith({{0x001000, 0x40},
                      {0x0001FD, kEightBit},
                      {0x0001FE, 0x00},
                      {0x0001FF, 0x20},
                      {0x000200, 0x7E}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .s = 0x01FC, .p = kEightBit});
  EXPECT_EQ(run(bus, cpu), 7u);
  ASSERT_EQ(bus.trace.size(), 7u);
  EXPECT_EQ(bus.trace[3].address, 0x0001FDu);  // the status byte
  EXPECT_EQ(cpu.state().pc, 0x2000);
  EXPECT_EQ(cpu.state().pbr, 0x7E);
}

TEST(Cpu65816ControlFlow, EmulationModeReturnFromInterruptTakesNoBankByte) {
  // Note 7, "subtract 1 cycle for 6502 emulation mode": row 22g's cycle 7, the
  // program bank pull, is the one that goes — six cycles, and the bank register is
  // left where it was.
  auto bus = busWith({{0x7F1000, 0x40},
                      {0x0001FD, kEightBit},
                      {0x0001FE, 0x00},
                      {0x0001FF, 0x20}});
  Cpu65816 cpu(
      Cpu65816State{.pc = 0x1000, .s = 0x01FC, .p = kEightBit, .pbr = 0x7F, .e = true});
  EXPECT_EQ(run(bus, cpu), 6u);
  EXPECT_EQ(cpu.state().pc, 0x2000);
  EXPECT_EQ(cpu.state().pbr, 0x7F);
}

TEST(Cpu65816ControlFlow, ThePulledStatusSettlesAtTheEndOfTheInstruction) {
  // The widths a cycle reports are the ones it ran under. RTI pulls a status byte
  // that clears both eight-bit widths, and every cycle of the instruction —
  // including the three that follow the pull — still reports them set. The new
  // widths are in effect once the instruction is over.
  auto bus = busWith({{0x001000, 0x40},
                      {0x0001FD, 0x00},  // the status pulled: M and X both clear
                      {0x0001FE, 0x00},
                      {0x0001FF, 0x20},
                      {0x000200, 0x00}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .s = 0x01FC, .p = kEightBit});
  EXPECT_EQ(run(bus, cpu), 7u);
  ASSERT_EQ(bus.trace.size(), 7u);
  for (const auto& cycle : bus.trace) {
    EXPECT_EQ(cycle.signals[kSignalM], 'm');
    EXPECT_EQ(cycle.signals[kSignalX], 'x');
  }
  EXPECT_EQ(cpu.state().p & kEightBit, 0);
}

TEST(Cpu65816ControlFlow, EmulationModeHoldsTheWidthsWhateverTheStatusPulled) {
  // Section 7.8.3.1: the m and x bits are always set in emulation mode. A status
  // byte pulled with them clear cannot clear them.
  auto bus = busWith({{0x001000, 0x40},
                      {0x0001FD, 0x00},
                      {0x0001FE, 0x00},
                      {0x0001FF, 0x20}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .s = 0x01FC, .p = kEightBit, .e = true});
  EXPECT_EQ(run(bus, cpu), 6u);
  EXPECT_EQ(cpu.state().p & kEightBit, kEightBit);
}

// ---- the stack address range in emulation mode (section 7.1) ----

TEST(Cpu65816ControlFlow, TheOlderCallsAndReturnsStayInPageOne) {
  // Section 7.1 lists the instructions that step outside $000100-$0001FF while
  // accessing two or three bytes; JSR absolute and RTS are not among them, so the
  // second byte of a call pushed from $000100 lands at $0001FF.
  auto bus = busWith({{0x001000, 0x20}, {0x001001, 0x34}, {0x001002, 0x12}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .s = 0x0100, .p = kEightBit, .e = true});
  EXPECT_EQ(run(bus, cpu), 6u);
  EXPECT_EQ(bus.trace[4].address, 0x000100u);
  EXPECT_EQ(bus.trace[5].address, 0x0001FFu);
  EXPECT_EQ(cpu.state().s, 0x01FE);
}

TEST(Cpu65816ControlFlow, TheLongCallStepsOutOfPageOneAndIsPutBackAfterwards) {
  // Section 7.1 names JSL among the instructions that increment or decrement beyond
  // the emulation stack range. From $000100 its third byte lands at $0000FE, and
  // the pointer is back inside page one by the end of the instruction.
  auto bus = busWith({{0x001000, 0x22},
                      {0x001001, 0x34},
                      {0x001002, 0x12},
                      {0x001003, 0x7E}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .s = 0x0100, .p = kEightBit, .e = true});
  EXPECT_EQ(run(bus, cpu), 8u);
  EXPECT_EQ(bus.trace[3].address, 0x000100u);
  EXPECT_EQ(bus.trace[6].address, 0x0000FFu);
  EXPECT_EQ(bus.trace[7].address, 0x0000FEu);
  EXPECT_EQ(cpu.state().s, 0x01FD);
}

TEST(Cpu65816ControlFlow, TheLongReturnStepsOutOfPageOneToo) {
  // Section 7.1 names RTL as well: pulling from $0001FF continues at $000200, and
  // the pointer is put back into page one afterwards.
  auto bus = busWith({{0x001000, 0x6B},
                      {0x0001FF, 0x03},
                      {0x000200, 0x10},
                      {0x000201, 0x7E}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .s = 0x01FE, .p = kEightBit, .e = true});
  EXPECT_EQ(run(bus, cpu), 6u);
  EXPECT_EQ(bus.trace[4].address, 0x000200u);
  EXPECT_EQ(bus.trace[5].address, 0x000201u);
  EXPECT_EQ(cpu.state().pc, 0x1004);
  EXPECT_EQ(cpu.state().s, 0x0101);
}

TEST(Cpu65816ControlFlow, TheIndexedCallStaysInPageOneDespiteTheDocumentedList) {
  // The one place the contract sources and the recorded hardware traces disagree.
  // Section 7.1 lists JSR (a,x) among the instructions that step outside the
  // emulation stack range, and both cross-check references classify it the same way
  // — but every recorded case that reaches the page-one edge wraps inside the page,
  // as JSR absolute does. The traces are what the core follows, so the behaviour is
  // pinned here rather than left to look like an oversight.
  auto bus = busWith({{0x001000, 0xFC},
                      {0x001001, 0x00},
                      {0x001002, 0x20},
                      {0x002000, 0x34},
                      {0x002001, 0x12}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .s = 0x0100, .p = kEightBit, .e = true});
  EXPECT_EQ(run(bus, cpu), 8u);
  EXPECT_EQ(bus.trace[2].address, 0x000100u);
  EXPECT_EQ(bus.trace[3].address, 0x0001FFu);
  EXPECT_EQ(cpu.state().s, 0x01FE);
}

// ---- the family's membership ----

TEST(Cpu65816ControlFlow, EveryControlFlowOpcodeRunsOnTheCycleEngine) {
  // The runner compares cycle by cycle only where the engine carries the opcode, so
  // the family's membership is the coverage. Twenty-one opcodes: nine relative
  // branches and BRL, five jumps, three calls, three returns.
  constexpr std::uint8_t kFamily[] = {0x10, 0x30, 0x50, 0x70, 0x90, 0xB0, 0xD0,
                                      0xF0, 0x80, 0x82, 0x4C, 0x5C, 0x6C, 0xDC,
                                      0x7C, 0x20, 0xFC, 0x22, 0x60, 0x6B, 0x40};
  for (std::uint8_t opcode : kFamily) {
    EXPECT_TRUE(Cpu65816::cycleStepped(opcode)) << "opcode " << int{opcode};
  }
  // The software interrupts, the halts and the block moves are not decoded.
  constexpr std::uint8_t kUndecoded[] = {0x00, 0x02, 0x44, 0x54, 0xCB, 0xDB};
  for (std::uint8_t opcode : kUndecoded) {
    EXPECT_FALSE(Cpu65816::cycleStepped(opcode)) << "opcode " << int{opcode};
  }
}

}  // namespace
