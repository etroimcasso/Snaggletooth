#include "snaggletooth/cpu/cpu65816.h"
#include "vector_harness.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <initializer_list>
#include <utility>

// The 65816 cycle engine itself, apart from any one instruction: that a cycle is a
// cycle, that an instruction in progress is an ordinary value a snapshot can carry,
// that the boundary is visible, and that the interrupt lines latch the way their
// pins do. The instructions' own cycle activity is proven against the recorded
// vectors; what is proven here is the machinery they run on.

namespace {

using snaggletooth::Cpu65816;
using snaggletooth::Cpu65816State;
using snaggletooth::CpuRunState;
using snaggletooth::kCpuFlagC;
using snaggletooth::kCpuFlagM;
using snaggletooth::kCpuFlagX;
using snaggletooth::cpu_vectors::RecordingBus;

RecordingBus busWith(
    std::initializer_list<std::pair<std::uint32_t, std::uint8_t>> ram) {
  RecordingBus bus;
  for (const auto& [address, value] : ram) bus.mem[address] = value;
  return bus;
}

// ---- one cycle at a time ----

TEST(Cpu65816CycleEngine, AFreshCoreSitsOnAnInstructionBoundary) {
  const Cpu65816 cpu;
  EXPECT_TRUE(cpu.atInstructionBoundary());
}

TEST(Cpu65816CycleEngine, TheFirstCycleFetchesTheOpcodeAndLeavesTheBoundary) {
  // NOP takes two cycles, so after one the core is part-way through it.
  auto bus = busWith({{0x001000, 0xEA}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000});
  cpu.stepCycle(bus);
  EXPECT_FALSE(cpu.atInstructionBoundary());
  EXPECT_EQ(cpu.state().ir, 0xEA);
  EXPECT_EQ(cpu.state().pc, 0x1001);
  EXPECT_EQ(bus.trace.size(), 1u);
}

TEST(Cpu65816CycleEngine, TheLastCycleReturnsToTheBoundary) {
  auto bus = busWith({{0x001000, 0xEA}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000});
  cpu.stepCycle(bus);
  cpu.stepCycle(bus);
  EXPECT_TRUE(cpu.atInstructionBoundary());
  EXPECT_EQ(bus.trace.size(), 2u);
}

TEST(Cpu65816CycleEngine, EveryCycleNarratesItselfThroughTheBus) {
  // LDA #$1234 with a 16-bit accumulator: an opcode fetch and two operand fetches,
  // three cycles and three bus events — a cycle never passes silently.
  auto bus = busWith({{0x001000, 0xA9}, {0x001001, 0x34}, {0x001002, 0x12}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000});
  const std::uint32_t cycles = cpu.stepInstruction(bus);
  EXPECT_EQ(cycles, 3u);
  EXPECT_EQ(bus.trace.size(), 3u);
  EXPECT_EQ(cpu.state().a, 0x1234);
}

TEST(Cpu65816CycleEngine, ACycleWithNoValidAccessStillDrivesAnAddress) {
  // NOP's second cycle reaches no memory, so it moves no byte — but the address
  // bus still drives, and the engine reports it.
  auto bus = busWith({{0x001000, 0xEA}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000});
  cpu.stepInstruction(bus);
  ASSERT_EQ(bus.trace.size(), 2u);
  ASSERT_TRUE(bus.trace[1].address.has_value());
  EXPECT_EQ(*bus.trace[1].address, 0x001001u);
  EXPECT_FALSE(bus.trace[1].value.has_value());
}

TEST(Cpu65816CycleEngine, TheCycleCountIsWhatExecuted) {
  // The same instruction at the two accumulator widths: the count follows the
  // cycles the core actually ran, one per operand byte.
  auto wide = busWith({{0x001000, 0xA9}, {0x001001, 0x34}, {0x001002, 0x12}});
  Cpu65816 wideCpu(Cpu65816State{.pc = 0x1000});
  EXPECT_EQ(wideCpu.stepInstruction(wide), 3u);

  auto narrow = busWith({{0x001000, 0xA9}, {0x001001, 0x34}});
  Cpu65816 narrowCpu(Cpu65816State{.pc = 0x1000, .p = kCpuFlagM});
  EXPECT_EQ(narrowCpu.stepInstruction(narrow), 2u);
}

// ---- an instruction in progress is a value ----

TEST(Cpu65816CycleEngine, AMidInstructionSnapshotRestoresToTheSameCycle) {
  // Stop half-way through a 16-bit immediate load, copy the state, finish the
  // instruction, then put the copy back and finish it again: the same instruction
  // completes the same way from the same cycle.
  auto bus = busWith({{0x001000, 0xA9}, {0x001001, 0x34}, {0x001002, 0x12}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000});
  cpu.stepCycle(bus);
  cpu.stepCycle(bus);
  ASSERT_FALSE(cpu.atInstructionBoundary());
  const Cpu65816State midway = cpu.state();

  cpu.stepCycle(bus);
  const Cpu65816State finished = cpu.state();
  ASSERT_TRUE(cpu.atInstructionBoundary());
  ASSERT_EQ(finished.a, 0x1234);

  cpu.restore(midway);
  EXPECT_FALSE(cpu.atInstructionBoundary());
  cpu.stepCycle(bus);
  EXPECT_TRUE(cpu.atInstructionBoundary());
  EXPECT_EQ(cpu.state().a, finished.a);
  EXPECT_EQ(cpu.state().pc, finished.pc);
  EXPECT_EQ(cpu.state().p, finished.p);
}

TEST(Cpu65816CycleEngine, AMidInstructionSnapshotCarriesTheProgressFields) {
  auto bus = busWith({{0x001000, 0xA9}, {0x001001, 0x34}, {0x001002, 0x12}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000});
  cpu.stepCycle(bus);
  cpu.stepCycle(bus);
  const Cpu65816State midway = cpu.state();
  EXPECT_EQ(midway.ir, 0xA9);
  EXPECT_EQ(midway.tcu, 2);
  EXPECT_EQ(midway.tmp, 0x0034);  // the low operand byte, latched while the high pends
}

TEST(Cpu65816CycleEngine, StepInstructionCalledMidInstructionFinishesThatOne) {
  auto bus = busWith({{0x001000, 0xA9}, {0x001001, 0x34}, {0x001002, 0x12},
                      {0x001003, 0xEA}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000});
  cpu.stepCycle(bus);
  // Two cycles remain of the three, and they are what stepInstruction runs — it
  // does not start the NOP that follows.
  EXPECT_EQ(cpu.stepInstruction(bus), 2u);
  EXPECT_EQ(cpu.state().a, 0x1234);
  EXPECT_EQ(cpu.state().pc, 0x1003);
}

// ---- the widths settle on the fetch cycle ----

TEST(Cpu65816CycleEngine, TheModeInvariantsSettleOnTheFetchCycle) {
  // An index high byte that is live while the index registers are 16-bit is cleared
  // as soon as an instruction begins under an 8-bit index width.
  auto bus = busWith({{0x001000, 0xEA}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .x = 0xBEEF, .p = kCpuFlagX});
  cpu.stepCycle(bus);
  EXPECT_EQ(cpu.state().x, 0x00EF);
}

// ---- the interrupt lines ----

TEST(Cpu65816CycleEngine, TheNmiLineLatchesOnItsEdge) {
  Cpu65816 cpu;
  EXPECT_FALSE(cpu.state().nmiPending);
  cpu.setNmiLine(true);
  EXPECT_TRUE(cpu.state().nmiPending);
}

TEST(Cpu65816CycleEngine, AnAlreadyAssertedNmiLineLatchesNothingNew) {
  // NMI is an edge, not a level: holding the line down does not queue a second
  // request, so a request cleared while the line is still asserted stays cleared.
  Cpu65816 cpu;
  cpu.setNmiLine(true);
  Cpu65816State state = cpu.state();
  state.nmiPending = false;
  cpu.restore(state);

  cpu.setNmiLine(true);
  EXPECT_FALSE(cpu.state().nmiPending);
  // Releasing and pulling the line again is a new edge, and latches.
  cpu.setNmiLine(false);
  cpu.setNmiLine(true);
  EXPECT_TRUE(cpu.state().nmiPending);
}

TEST(Cpu65816CycleEngine, TheIrqLineIsALevel) {
  // IRQ is sampled, not latched: the core holds whatever the line currently says.
  Cpu65816 cpu;
  EXPECT_FALSE(cpu.state().irqLine);
  cpu.setIrqLine(true);
  EXPECT_TRUE(cpu.state().irqLine);
  cpu.setIrqLine(false);
  EXPECT_FALSE(cpu.state().irqLine);
}

// ---- a halted core ----

TEST(Cpu65816CycleEngine, AHaltedCoreTouchesNothing) {
  auto bus = busWith({{0x001000, 0xEA}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .run = CpuRunState::Stopped});
  cpu.stepCycle(bus);
  EXPECT_TRUE(bus.trace.empty());
  EXPECT_EQ(cpu.state().pc, 0x1000);
}

// ---- the two execution paths agree ----

TEST(Cpu65816CycleEngine, TheEngineCarriesTheImpliedImmediateAndMemoryInstructions) {
  // The families this core runs cycle by cycle. The control-flow instructions are
  // outside them and are not decoded at all yet.
  EXPECT_TRUE(Cpu65816::cycleStepped(0xEA));   // NOP
  EXPECT_TRUE(Cpu65816::cycleStepped(0xA9));   // LDA #imm
  EXPECT_TRUE(Cpu65816::cycleStepped(0xC2));   // REP #imm
  EXPECT_TRUE(Cpu65816::cycleStepped(0xEB));   // XBA
  EXPECT_TRUE(Cpu65816::cycleStepped(0xFB));   // XCE
  EXPECT_TRUE(Cpu65816::cycleStepped(0x42));   // WDM
  EXPECT_TRUE(Cpu65816::cycleStepped(0xA5));   // LDA dir
  EXPECT_TRUE(Cpu65816::cycleStepped(0xB5));   // LDA dir,X
  EXPECT_TRUE(Cpu65816::cycleStepped(0xB6));   // LDX dir,Y
  EXPECT_TRUE(Cpu65816::cycleStepped(0x06));   // ASL dir
  EXPECT_TRUE(Cpu65816::cycleStepped(0xAD));   // LDA abs
  EXPECT_TRUE(Cpu65816::cycleStepped(0xBD));   // LDA abs,X
  EXPECT_TRUE(Cpu65816::cycleStepped(0xB9));   // LDA abs,Y
  EXPECT_TRUE(Cpu65816::cycleStepped(0xAF));   // LDA long
  EXPECT_TRUE(Cpu65816::cycleStepped(0xBF));   // LDA long,X
  EXPECT_TRUE(Cpu65816::cycleStepped(0x1E));   // ASL abs,X
  EXPECT_TRUE(Cpu65816::cycleStepped(0xB2));   // LDA (dir)
  EXPECT_TRUE(Cpu65816::cycleStepped(0xA1));   // LDA (dir,X)
  EXPECT_TRUE(Cpu65816::cycleStepped(0xB1));   // LDA (dir),Y
  EXPECT_TRUE(Cpu65816::cycleStepped(0xA7));   // LDA [dir]
  EXPECT_TRUE(Cpu65816::cycleStepped(0xB7));   // LDA [dir],Y
  EXPECT_TRUE(Cpu65816::cycleStepped(0xA3));   // LDA sr,S
  EXPECT_TRUE(Cpu65816::cycleStepped(0xB3));   // LDA (sr,S),Y
  EXPECT_TRUE(Cpu65816::cycleStepped(0x48));   // PHA
  EXPECT_TRUE(Cpu65816::cycleStepped(0x28));   // PLP
  EXPECT_TRUE(Cpu65816::cycleStepped(0xD4));   // PEI dir
  EXPECT_TRUE(Cpu65816::cycleStepped(0x90));   // BCC rel
  EXPECT_TRUE(Cpu65816::cycleStepped(0x82));   // BRL
  EXPECT_TRUE(Cpu65816::cycleStepped(0x4C));   // JMP abs
  EXPECT_TRUE(Cpu65816::cycleStepped(0x6C));   // JMP (abs)
  EXPECT_TRUE(Cpu65816::cycleStepped(0x7C));   // JMP (abs,X)
  EXPECT_TRUE(Cpu65816::cycleStepped(0x20));   // JSR abs
  EXPECT_TRUE(Cpu65816::cycleStepped(0x22));   // JSL
  EXPECT_TRUE(Cpu65816::cycleStepped(0x60));   // RTS
  EXPECT_TRUE(Cpu65816::cycleStepped(0x40));   // RTI
  EXPECT_FALSE(Cpu65816::cycleStepped(0x44));  // MVP
}

TEST(Cpu65816CycleEngine, AnUndecodedOpcodeReportsNoCyclesRatherThanGuessing) {
  // Every opcode the core decodes runs on the cycle engine. One it does not decode
  // reports a zero count from stepInstruction, which a recorded case's non-zero
  // count rejects loudly rather than passing in silence.
  auto bus = busWith({{0x001000, 0x44}, {0x001001, 0x34}, {0x001002, 0x12}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .s = 0x01FF, .p = kCpuFlagM | kCpuFlagX});
  EXPECT_EQ(cpu.stepInstruction(bus), 0u);
  EXPECT_TRUE(cpu.atInstructionBoundary());
}

TEST(Cpu65816CycleEngine, AnInstructionOffTheEngineCannotBeSteppedACycleAtATime) {
  // Asking for one cycle of an instruction the engine does not carry produces no
  // bus activity at all — the cycle that was asked for plainly did not happen,
  // rather than being filled in with invented traffic.
  auto bus = busWith({{0x001000, 0x44}, {0x001001, 0x34}, {0x001002, 0x12}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .s = 0x01FF, .p = kCpuFlagM | kCpuFlagX});
  cpu.stepCycle(bus);  // the opcode fetch, which every instruction shares
  ASSERT_EQ(bus.trace.size(), 1u);
  cpu.stepCycle(bus);
  EXPECT_EQ(bus.trace.size(), 1u);
  EXPECT_TRUE(cpu.atInstructionBoundary());
}

// ---- what a cycle's pin string reports ----

TEST(Cpu65816CycleEngine, TheWidthsReportedAreTheOnesTheCycleRanUnder) {
  // REP clears the 8-bit widths, and the cycle that carries it out still runs under
  // the old ones: the change takes effect at the end of that cycle, not before it.
  auto bus = busWith({{0x001000, 0xC2}, {0x001001, 0x30}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .p = kCpuFlagM | kCpuFlagX});
  bus.cpu = &cpu;
  cpu.stepInstruction(bus);
  ASSERT_EQ(bus.trace.size(), 3u);
  EXPECT_EQ(bus.trace[0].signals, "dp-r-mx-");
  EXPECT_EQ(bus.trace[1].signals, "-p-r-mx-");
  EXPECT_EQ(bus.trace[2].signals, "---r-mx-");
  EXPECT_EQ(cpu.state().p & (kCpuFlagM | kCpuFlagX), 0);
}

TEST(Cpu65816CycleEngine, EmulationModeIsReportedByTheCycleThatLeavesIt) {
  // XCE swaps carry with the emulation flag at the end of its second cycle, so both
  // cycles report the mode the instruction started in.
  auto bus = busWith({{0x001000, 0xFB}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .p = kCpuFlagM | kCpuFlagX, .e = true});
  bus.cpu = &cpu;
  cpu.stepInstruction(bus);
  ASSERT_EQ(bus.trace.size(), 2u);
  EXPECT_EQ(bus.trace[0].signals, "dp-remx-");
  EXPECT_EQ(bus.trace[1].signals, "---remx-");
  EXPECT_FALSE(cpu.state().e);
  EXPECT_TRUE(cpu.state().p & kCpuFlagC);
}

}  // namespace
