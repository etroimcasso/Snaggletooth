#include "snaggletooth/cpu/cpu65816.h"
#include "vector_harness.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <initializer_list>
#include <utility>

// The interrupts and the halts, cycle by cycle. The recorded vectors prove the whole
// trace of BRK, COP, WAI and STP; the hardware interrupt sequence has no opcode and
// no recorded case, so every expectation about it is traced to the line that states
// it: W65C816S datasheet Table 5-7 row 22a (the hardware sequence), row 22j (BRK and
// COP), rows 19c and 19d (STP and WAI); notes 7, 10 and 11; Tables 5-2 and 5-3 (the
// vector addresses); sections 2.18, 2.21 and 2.28 (the two request pins and the
// vector-pull pin); and sections 7.1, 7.11, 7.12, 7.13, 7.14, 7.19 and 7.22.

namespace {

using snaggletooth::Cpu65816;
using snaggletooth::Cpu65816State;
using snaggletooth::CpuRunState;
using snaggletooth::InterruptRequest;
using snaggletooth::kCpuFlagD;
using snaggletooth::kCpuFlagI;
using snaggletooth::kCpuFlagM;
using snaggletooth::kCpuFlagX;
using snaggletooth::cpu_vectors::kHaltedSignals;
using snaggletooth::cpu_vectors::kSignalRw;
using snaggletooth::cpu_vectors::kSignalVda;
using snaggletooth::cpu_vectors::kSignalVpa;
using snaggletooth::cpu_vectors::kSignalVpb;
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

// A native-mode BRK at $00:1000 with a handler vector pointing at $2000, and the
// stack at the top of page one.
RecordingBus breakAt1000() {
  return busWith({{0x001000, 0x00}, {0x001001, 0x42},   // BRK, signature $42
                  {0x00FFE6, 0x00}, {0x00FFE7, 0x20}});  // the native BRK vector
}

// ---- BRK and COP (Table 5-7 row 22j) ----

TEST(Cpu65816Interrupt, ASoftwareInterruptIsEightCyclesInNativeMode) {
  // Row 22j: the opcode, the signature byte, the program bank, the two return
  // address bytes and the status byte, then the two vector cycles. The stack is
  // written high byte first, from the pointer downwards.
  auto bus = breakAt1000();
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .s = 0x01FF, .p = kEightBit});
  EXPECT_EQ(run(bus, cpu), 8u);
  ASSERT_EQ(bus.trace.size(), 8u);
  EXPECT_EQ(bus.trace[2].address, 0x0001FFu);  // the program bank
  EXPECT_EQ(bus.trace[3].address, 0x0001FEu);  // the return address, high byte
  EXPECT_EQ(bus.trace[4].address, 0x0001FDu);  // the return address, low byte
  EXPECT_EQ(bus.trace[5].address, 0x0001FCu);  // the status byte
  EXPECT_EQ(bus.trace[6].address, 0x00FFE6u);
  EXPECT_EQ(bus.trace[7].address, 0x00FFE7u);
  EXPECT_EQ(cpu.state().s, 0x01FB);
  EXPECT_EQ(cpu.state().pc, 0x2000);
}

TEST(Cpu65816Interrupt, ASoftwareInterruptIsSevenCyclesInEmulationMode) {
  // Note 7, "subtract 1 cycle for 6502 emulation mode", falls on row 22j's third
  // cycle: section 7.11.2 says the previous program bank is not saved there. The
  // sequence is otherwise the same, so the return address is written first.
  auto bus = busWith({{0x001000, 0x00}, {0x001001, 0x42},
                      {0x00FFFE, 0x00}, {0x00FFFF, 0x20}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .s = 0x01FF, .p = kEightBit, .e = true});
  EXPECT_EQ(run(bus, cpu), 7u);
  ASSERT_EQ(bus.trace.size(), 7u);
  EXPECT_EQ(bus.trace[2].address, 0x0001FFu);  // the return address, high byte
  EXPECT_EQ(bus.trace[3].address, 0x0001FEu);
  EXPECT_EQ(bus.trace[4].address, 0x0001FDu);  // the status byte
  EXPECT_EQ(cpu.state().s, 0x01FC);
  EXPECT_EQ(cpu.state().pc, 0x2000);
}

TEST(Cpu65816Interrupt, TheSavedAddressIsThePlaceAfterTheSignatureByte) {
  // Section 7.22: the second byte is a signature the processor does not use, and a
  // return from interrupt comes back to the location after it. So a two-byte
  // instruction at $1000 saves $1002 rather than $1001.
  auto bus = breakAt1000();
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .s = 0x01FF, .p = kEightBit});
  run(bus, cpu);
  EXPECT_EQ(bus.read(0x0001FE), 0x10);  // the high byte of $1002
  EXPECT_EQ(bus.read(0x0001FD), 0x02);
}

TEST(Cpu65816Interrupt, TheSignatureByteIsFetchedThroughTheProgramAddressPinAlone) {
  // Row 22j's second cycle carries VPA without VDA — a program fetch, like any
  // other operand byte, even though the byte fetched is never used.
  auto bus = breakAt1000();
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .s = 0x01FF, .p = kEightBit});
  run(bus, cpu);
  EXPECT_EQ(bus.trace[1].address, 0x001001u);
  EXPECT_EQ(bus.trace[1].signals[kSignalVpa], 'p');
  EXPECT_EQ(bus.trace[1].signals[kSignalVda], '-');
}

TEST(Cpu65816Interrupt, TheProgramBankIsSavedInNativeModeAndClearedInBoth) {
  // Sections 7.11.1 and 7.11.2: the handler runs in bank zero either way, and only
  // native mode puts the bank it came from on the stack.
  auto native = breakAt1000();
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .s = 0x01FF, .p = kEightBit, .pbr = 0x7E});
  run(native, cpu);
  EXPECT_EQ(native.read(0x0001FF), 0x7E);
  EXPECT_EQ(cpu.state().pbr, 0x00);

  auto emulated = busWith({{0x7E1000, 0x00}, {0x7E1001, 0x42},
                           {0x00FFFE, 0x00}, {0x00FFFF, 0x20}});
  Cpu65816 emulatedCpu(Cpu65816State{
      .pc = 0x1000, .s = 0x01FF, .p = kEightBit, .pbr = 0x7E, .e = true});
  run(emulated, emulatedCpu);
  EXPECT_EQ(emulated.read(0x0001FF), 0x10);  // the return address, not the bank
  EXPECT_EQ(emulatedCpu.state().pbr, 0x00);
}

TEST(Cpu65816Interrupt, TheTwoSoftwareInterruptsTakeDifferentVectors) {
  // Table 5-3: BRK reads $00FFE6 and COP $00FFE4 in native mode.
  auto brk = breakAt1000();
  Cpu65816 brkCpu(Cpu65816State{.pc = 0x1000, .s = 0x01FF, .p = kEightBit});
  run(brk, brkCpu);
  EXPECT_EQ(brk.trace[6].address, 0x00FFE6u);

  auto cop = busWith({{0x001000, 0x02}, {0x001001, 0x42},
                      {0x00FFE4, 0x34}, {0x00FFE5, 0x12}});
  Cpu65816 copCpu(Cpu65816State{.pc = 0x1000, .s = 0x01FF, .p = kEightBit});
  EXPECT_EQ(run(cop, copCpu), 8u);
  EXPECT_EQ(cop.trace[6].address, 0x00FFE4u);
  EXPECT_EQ(copCpu.state().pc, 0x1234);
}

TEST(Cpu65816Interrupt, TheTwoSoftwareInterruptsTakeDifferentVectorsInEmulationMode) {
  // Table 5-2: COP moves to $00FFF4, while BRK shares $00FFFE with the maskable
  // hardware request.
  auto cop = busWith({{0x001000, 0x02}, {0x001001, 0x42},
                      {0x00FFF4, 0x34}, {0x00FFF5, 0x12}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .s = 0x01FF, .p = kEightBit, .e = true});
  EXPECT_EQ(run(cop, cpu), 7u);
  EXPECT_EQ(cop.trace[5].address, 0x00FFF4u);
  EXPECT_EQ(cpu.state().pc, 0x1234);
}

TEST(Cpu65816Interrupt, TheVectorCyclesAssertTheVectorPullPin) {
  // Section 2.28: the vector-pull output is low during the last two cycles of an
  // interrupt sequence, and nowhere else — which is what lets a system watch for a
  // vector fetch and substitute its own.
  auto bus = breakAt1000();
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .s = 0x01FF, .p = kEightBit});
  run(bus, cpu);
  EXPECT_EQ(bus.trace[6].signals[kSignalVpb], 'v');
  EXPECT_EQ(bus.trace[7].signals[kSignalVpb], 'v');
  for (std::size_t i = 0; i < 6; ++i) {
    EXPECT_EQ(bus.trace[i].signals[kSignalVpb], '-') << "cycle " << i;
  }
}

TEST(Cpu65816Interrupt, TheHandlerStartsInBinaryModeWithTheMaskableRequestDisabled) {
  // Section 7.12 and the note under Table 5-3: an executed interrupt leaves D clear
  // and I set. The status byte reaching the stack is the one the interrupt found,
  // so the handler's return restores the mode it interrupted.
  auto bus = breakAt1000();
  const std::uint8_t before = static_cast<std::uint8_t>(kEightBit | kCpuFlagD);
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .s = 0x01FF, .p = before});
  run(bus, cpu);
  EXPECT_EQ(bus.read(0x0001FC), before);
  EXPECT_EQ(cpu.state().p & kCpuFlagD, 0);
  EXPECT_EQ(cpu.state().p & kCpuFlagI, kCpuFlagI);
}

TEST(Cpu65816Interrupt, TheStackStaysInPageOneInEmulationMode) {
  // Section 7.1 lists the opcodes that step outside $000100-$0001FF while pushing;
  // neither BRK nor COP is among them, so a sequence that starts at the bottom of
  // the page carries on at its top.
  auto bus = busWith({{0x001000, 0x00}, {0x001001, 0x42},
                      {0x00FFFE, 0x00}, {0x00FFFF, 0x20}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .s = 0x0100, .p = kEightBit, .e = true});
  run(bus, cpu);
  ASSERT_EQ(bus.trace.size(), 7u);
  EXPECT_EQ(bus.trace[2].address, 0x000100u);
  EXPECT_EQ(bus.trace[3].address, 0x0001FFu);
  EXPECT_EQ(bus.trace[4].address, 0x0001FEu);
  EXPECT_EQ(cpu.state().s, 0x01FD);
}

// ---- WAI and STP (Table 5-7 rows 19d and 19c) ----

TEST(Cpu65816Interrupt, WaitIsThreeCyclesAndThenDrivesNothing) {
  // Row 19d: the opcode and two internal cycles at PBR,PC+1. After them the
  // processor is in the wait, where it drives no address and touches no memory.
  auto bus = busWith({{0x001000, 0xCB}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .s = 0x01FF, .p = kEightBit});
  EXPECT_EQ(run(bus, cpu), 3u);
  ASSERT_EQ(bus.trace.size(), 3u);
  EXPECT_EQ(bus.trace[1].address, 0x001001u);
  EXPECT_EQ(bus.trace[2].address, 0x001001u);
  EXPECT_EQ(bus.trace[1].signals[kSignalVda], '-');
  EXPECT_EQ(bus.trace[1].signals[kSignalVpa], '-');
  EXPECT_EQ(cpu.state().run, CpuRunState::Waiting);
  EXPECT_EQ(cpu.state().pc, 0x1001);

  for (int i = 0; i < 4; ++i) cpu.stepCycle(bus);
  EXPECT_EQ(bus.trace.size(), 3u);
  EXPECT_TRUE(cpu.atInstructionBoundary());
}

TEST(Cpu65816Interrupt, StopIsThreeCyclesAndThenDrivesNothing) {
  // Row 19c has the same three cycles as the wait, and section 7.14 says the clock
  // itself stops: nothing further reaches the bus.
  auto bus = busWith({{0x001000, 0xDB}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .s = 0x01FF, .p = kEightBit});
  EXPECT_EQ(run(bus, cpu), 3u);
  ASSERT_EQ(bus.trace.size(), 3u);
  EXPECT_EQ(cpu.state().run, CpuRunState::Stopped);
  for (int i = 0; i < 4; ++i) cpu.stepCycle(bus);
  EXPECT_EQ(bus.trace.size(), 3u);
}

TEST(Cpu65816Interrupt, EitherRequestReleasesTheWait) {
  // Section 7.13: the non-maskable and maskable inputs both terminate the wait.
  auto irqBus = busWith({{0x001000, 0xCB}, {0x001001, 0xEA}});
  Cpu65816 irqCpu(Cpu65816State{.pc = 0x1000, .s = 0x01FF, .p = kEightBit});
  run(irqBus, irqCpu);
  ASSERT_EQ(irqCpu.state().run, CpuRunState::Waiting);
  irqCpu.setIrqLine(true);
  irqCpu.stepCycle(irqBus);
  EXPECT_EQ(irqCpu.state().run, CpuRunState::Running);

  auto nmiBus = busWith({{0x001000, 0xCB}, {0x001001, 0xEA}});
  Cpu65816 nmiCpu(Cpu65816State{.pc = 0x1000, .s = 0x01FF, .p = kEightBit});
  run(nmiBus, nmiCpu);
  nmiCpu.setNmiLine(true);
  nmiCpu.stepCycle(nmiBus);
  EXPECT_EQ(nmiCpu.state().run, CpuRunState::Running);
}

TEST(Cpu65816Interrupt, AMaskedRequestReleasesTheWaitWithoutBeingTaken) {
  // Section 7.13's sharp case: with the disable flag set, the maskable request wakes
  // the processor and the instruction after the wait runs, without going to the
  // handler at all. That is the whole point of waiting on a masked line.
  auto bus = busWith({{0x001000, 0xCB}, {0x001001, 0xEA},   // WAI then NOP
                      {0x00FFEE, 0x00}, {0x00FFEF, 0x20}});
  Cpu65816 cpu(Cpu65816State{
      .pc = 0x1000, .s = 0x01FF, .p = static_cast<std::uint8_t>(kEightBit | kCpuFlagI)});
  run(bus, cpu);
  cpu.setIrqLine(true);
  cpu.stepCycle(bus);  // the cycle that ends the wait
  EXPECT_EQ(run(bus, cpu), 2u);
  EXPECT_EQ(cpu.state().pc, 0x1002);  // past the NOP, not in a handler
  EXPECT_EQ(cpu.state().s, 0x01FF);   // and nothing was saved
}

TEST(Cpu65816Interrupt, AnUnmaskedRequestReleasesTheWaitAndIsTaken) {
  // The other half of section 7.13: with the flag clear, control transfers to the
  // handler routine instead.
  auto bus = busWith({{0x001000, 0xCB}, {0x001001, 0xEA},
                      {0x00FFEE, 0x00}, {0x00FFEF, 0x20}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .s = 0x01FF, .p = kEightBit});
  run(bus, cpu);
  cpu.setIrqLine(true);
  cpu.stepCycle(bus);
  EXPECT_EQ(run(bus, cpu), 8u);
  EXPECT_EQ(cpu.state().pc, 0x2000);
  EXPECT_EQ(bus.read(0x0001FD), 0x01);  // the address the wait would have resumed at
}

TEST(Cpu65816Interrupt, NothingReleasesAStoppedCore) {
  // Section 7.14: the reset input is the only one that restarts the processor, and a
  // reset reaches this core as a fresh state rather than as a line. So neither
  // request moves a stopped core, however long it is stepped.
  auto bus = busWith({{0x001000, 0xDB}, {0x001001, 0xEA}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .s = 0x01FF, .p = kEightBit});
  run(bus, cpu);
  cpu.setIrqLine(true);
  cpu.setNmiLine(true);
  for (int i = 0; i < 8; ++i) cpu.stepCycle(bus);
  EXPECT_EQ(cpu.state().run, CpuRunState::Stopped);
  EXPECT_EQ(bus.trace.size(), 3u);
}

TEST(Cpu65816Interrupt, AHaltedCycleNarratesNothingAndStillPasses) {
  // A halted cycle drives no address at all, which is what a recording shows as a
  // row with every pin inactive. The core reports the cycle as passed — a whole
  // instruction's worth of them, since a halt cannot be part-way through one.
  auto bus = busWith({{0x001000, 0xCB}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .s = 0x01FF, .p = kEightBit});
  run(bus, cpu);
  const std::size_t narrated = bus.trace.size();
  EXPECT_EQ(run(bus, cpu), 1u);
  EXPECT_EQ(bus.trace.size(), narrated);
  EXPECT_TRUE(cpu.atInstructionBoundary());
  EXPECT_STREQ(kHaltedSignals, "--------");
}

// ---- the hardware interrupt sequence (Table 5-7 row 22a) ----

// A native-mode program of two no-ops with both hardware vectors filled in.
RecordingBus noopsWithVectors() {
  return busWith({{0x001000, 0xEA}, {0x001001, 0xEA},
                  {0x00FFEA, 0x00}, {0x00FFEB, 0x30},   // the native NMI vector
                  {0x00FFEE, 0x00}, {0x00FFEF, 0x20}});  // the native IRQ vector
}

TEST(Cpu65816Interrupt, AHardwareRequestIsTakenBetweenInstructionsNotWithinOne) {
  // Sections 2.18 and 2.21 both say the sequence is initiated after the current
  // instruction is completed. A line asserted part-way through an instruction
  // therefore changes nothing until that instruction has finished.
  auto bus = noopsWithVectors();
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .s = 0x01FF, .p = kEightBit});
  bus.cpu = &cpu;
  cpu.stepCycle(bus);  // the opcode fetch of the first no-op
  cpu.setIrqLine(true);
  cpu.stepCycle(bus);  // its second cycle, which runs as if nothing had happened
  EXPECT_TRUE(cpu.atInstructionBoundary());
  EXPECT_EQ(cpu.state().pc, 0x1001);
  EXPECT_EQ(bus.trace.size(), 2u);
  EXPECT_EQ(cpu.state().s, 0x01FF);

  EXPECT_EQ(cpu.stepInstruction(bus), 8u);  // and now the sequence runs
  EXPECT_EQ(cpu.state().pc, 0x2000);
}

TEST(Cpu65816Interrupt, TheHardwareSequenceIsEightCyclesInNativeMode) {
  // Row 22a: two cycles at the program counter that move nothing, then the same
  // four stack writes and two vector cycles as a software interrupt.
  auto bus = noopsWithVectors();
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .s = 0x01FF, .p = kEightBit});
  cpu.setIrqLine(true);
  EXPECT_EQ(run(bus, cpu), 8u);
  ASSERT_EQ(bus.trace.size(), 8u);
  EXPECT_EQ(bus.trace[2].address, 0x0001FFu);
  EXPECT_EQ(bus.trace[5].address, 0x0001FCu);
  EXPECT_EQ(bus.trace[6].address, 0x00FFEEu);
  EXPECT_EQ(bus.trace[7].address, 0x00FFEFu);
  EXPECT_EQ(cpu.state().pc, 0x2000);
  EXPECT_EQ(cpu.state().servicing, InterruptRequest::None);
}

TEST(Cpu65816Interrupt, TheFirstTwoCyclesReadTheProgramCounterAndKeepItThere) {
  // Row 22a's first two cycles both address PBR,PC and carry an internal operation
  // on the data bus — the byte is read and thrown away. The first drives both
  // valid-address pins and the second only the data one, and neither moves the
  // program counter: the address saved below is the instruction that was
  // interrupted, so the handler's return resumes at it rather than past it.
  auto bus = noopsWithVectors();
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .s = 0x01FF, .p = kEightBit});
  cpu.setIrqLine(true);
  run(bus, cpu);
  EXPECT_EQ(bus.trace[0].address, 0x001000u);
  EXPECT_EQ(bus.trace[0].signals[kSignalVda], 'd');
  EXPECT_EQ(bus.trace[0].signals[kSignalVpa], 'p');
  EXPECT_EQ(bus.trace[1].address, 0x001000u);
  EXPECT_EQ(bus.trace[1].signals[kSignalVda], 'd');
  EXPECT_EQ(bus.trace[1].signals[kSignalVpa], '-');
  EXPECT_EQ(bus.trace[1].signals[kSignalRw], 'r');
  EXPECT_EQ(bus.read(0x0001FE), 0x10);  // the saved address is $1000 itself
  EXPECT_EQ(bus.read(0x0001FD), 0x00);
}

TEST(Cpu65816Interrupt, TheHardwareSequenceIsSevenCyclesInEmulationMode) {
  // Note 7 again: no program bank is saved, so the sequence is a cycle shorter and
  // its stack writes start with the return address.
  auto bus = busWith({{0x001000, 0xEA},
                      {0x00FFFE, 0x00}, {0x00FFFF, 0x20}});
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .s = 0x01FF, .p = kEightBit, .e = true});
  cpu.setIrqLine(true);
  EXPECT_EQ(run(bus, cpu), 7u);
  ASSERT_EQ(bus.trace.size(), 7u);
  EXPECT_EQ(bus.trace[2].address, 0x0001FFu);
  EXPECT_EQ(bus.read(0x0001FF), 0x10);  // the return address, high byte
  EXPECT_EQ(cpu.state().pc, 0x2000);
}

TEST(Cpu65816Interrupt, TheMaskableRequestIsIgnoredWhileTheDisableFlagIsSet) {
  // Section 2.18: the sequence is initiated when the disable flag is cleared. With
  // it set the line is held but nothing is taken, however many instructions run.
  auto bus = noopsWithVectors();
  Cpu65816 cpu(Cpu65816State{
      .pc = 0x1000, .s = 0x01FF, .p = static_cast<std::uint8_t>(kEightBit | kCpuFlagI)});
  cpu.setIrqLine(true);
  EXPECT_EQ(run(bus, cpu), 2u);
  EXPECT_EQ(run(bus, cpu), 2u);
  EXPECT_EQ(cpu.state().pc, 0x1002);
  EXPECT_EQ(cpu.state().s, 0x01FF);
}

TEST(Cpu65816Interrupt, TheNonMaskableRequestIsTakenWhateverTheDisableFlagSays) {
  // Section 2.21: a negative transition initiates the sequence. The disable flag
  // governs the maskable line alone, which is what non-maskable means.
  auto bus = noopsWithVectors();
  Cpu65816 cpu(Cpu65816State{
      .pc = 0x1000, .s = 0x01FF, .p = static_cast<std::uint8_t>(kEightBit | kCpuFlagI)});
  cpu.setNmiLine(true);
  EXPECT_EQ(run(bus, cpu), 8u);
  EXPECT_EQ(cpu.state().pc, 0x3000);
}

TEST(Cpu65816Interrupt, TheNonMaskableRequestOutranksTheMaskableOne) {
  // Section 7.19 ranks the non-maskable request above the maskable one, so with both
  // lines asserted the sequence takes the non-maskable vector.
  auto bus = noopsWithVectors();
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .s = 0x01FF, .p = kEightBit});
  cpu.setIrqLine(true);
  cpu.setNmiLine(true);
  run(bus, cpu);
  EXPECT_EQ(bus.trace[6].address, 0x00FFEAu);
  EXPECT_EQ(cpu.state().pc, 0x3000);
}

TEST(Cpu65816Interrupt, TheNonMaskableLatchIsClearedByTheSequenceItStarts) {
  // Section 2.21: no interrupt occurs while the line remains low after its negative
  // transition has been processed. The latch the edge set is cleared by the sequence
  // it started, so a line left asserted asks for nothing more — while a fresh edge
  // asks again.
  auto bus = noopsWithVectors();
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .s = 0x01FF, .p = kEightBit});
  cpu.setNmiLine(true);
  run(bus, cpu);
  EXPECT_FALSE(cpu.state().nmiPending);
  const std::uint16_t after = cpu.state().s;

  bus.mem[0x003000] = 0xEA;  // the handler's first instruction
  EXPECT_EQ(run(bus, cpu), 2u);
  EXPECT_EQ(cpu.state().s, after);  // the still-low line saved nothing

  cpu.setNmiLine(false);
  cpu.setNmiLine(true);  // a fresh negative transition
  EXPECT_TRUE(cpu.state().nmiPending);
}

TEST(Cpu65816Interrupt, EachHardwareSourceTakesItsOwnVector) {
  // Tables 5-2 and 5-3: the non-maskable request reads $00FFEA in native mode and
  // $00FFFA in emulation, and the maskable one $00FFEE and $00FFFE.
  auto native = noopsWithVectors();
  Cpu65816 nativeCpu(Cpu65816State{.pc = 0x1000, .s = 0x01FF, .p = kEightBit});
  nativeCpu.setNmiLine(true);
  run(native, nativeCpu);
  EXPECT_EQ(native.trace[6].address, 0x00FFEAu);

  auto emulated = busWith({{0x001000, 0xEA},
                           {0x00FFFA, 0x00}, {0x00FFFB, 0x30}});
  Cpu65816 emulatedCpu(
      Cpu65816State{.pc = 0x1000, .s = 0x01FF, .p = kEightBit, .e = true});
  emulatedCpu.setNmiLine(true);
  run(emulated, emulatedCpu);
  EXPECT_EQ(emulated.trace[5].address, 0x00FFFAu);
  EXPECT_EQ(emulatedCpu.state().pc, 0x3000);
}

TEST(Cpu65816Interrupt, AHardwareRequestClearsTheBreakFlagInEmulationMode) {
  // Note 11: the break bit reads zero in the status byte a hardware sequence saves.
  // In emulation mode the maskable request and BRK share a vector, so that bit is
  // the only thing that tells a handler which of the two it is answering.
  auto hardware = busWith({{0x001000, 0xEA}, {0x00FFFE, 0x00}, {0x00FFFF, 0x20}});
  Cpu65816 hardwareCpu(
      Cpu65816State{.pc = 0x1000, .s = 0x01FF, .p = kEightBit, .e = true});
  hardwareCpu.setIrqLine(true);
  run(hardware, hardwareCpu);
  EXPECT_EQ(hardware.read(0x0001FD) & kCpuFlagX, 0);

  auto software = busWith({{0x001000, 0x00}, {0x001001, 0x42},
                           {0x00FFFE, 0x00}, {0x00FFFF, 0x20}});
  Cpu65816 softwareCpu(
      Cpu65816State{.pc = 0x1000, .s = 0x01FF, .p = kEightBit, .e = true});
  run(software, softwareCpu);
  EXPECT_EQ(software.read(0x0001FD) & kCpuFlagX, kCpuFlagX);
}

TEST(Cpu65816Interrupt, NativeModeSavesTheIndexWidthInThatBitInstead) {
  // The break bit is an emulation-mode reading of bit 4. In native mode that bit is
  // the index width and is saved as it stands, whatever started the sequence —
  // which is why the two modes need separate handling at all.
  auto bus = noopsWithVectors();
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .s = 0x01FF, .p = kEightBit});
  cpu.setIrqLine(true);
  run(bus, cpu);
  EXPECT_EQ(bus.read(0x0001FC) & kCpuFlagX, kCpuFlagX);
}

TEST(Cpu65816Interrupt, AHandlerReturnsThroughReturnFromInterrupt) {
  // The round trip: what the sequence saves is what the return pulls back, so the
  // interrupted instruction runs after the handler as though nothing had happened.
  auto bus = noopsWithVectors();
  bus.mem[0x003000] = 0x40;  // RTI at the non-maskable handler
  Cpu65816 cpu(Cpu65816State{
      .pc = 0x1000, .s = 0x01FF, .p = static_cast<std::uint8_t>(kEightBit | kCpuFlagD),
      .pbr = 0x7E});
  bus.mem[0x7E1000] = 0xEA;
  cpu.setNmiLine(true);
  run(bus, cpu);
  ASSERT_EQ(cpu.state().pc, 0x3000);

  EXPECT_EQ(run(bus, cpu), 7u);  // the return, native mode taking a bank byte
  EXPECT_EQ(cpu.state().pc, 0x1000);
  EXPECT_EQ(cpu.state().pbr, 0x7E);
  EXPECT_EQ(cpu.state().p, kEightBit | kCpuFlagD);
  EXPECT_EQ(cpu.state().s, 0x01FF);
}

// ---- the sequence as state ----

TEST(Cpu65816Interrupt, AnInterruptSequenceSnapshotsAndRestoresPartWayThrough) {
  // A hardware sequence has no opcode to hold in the instruction register, so what
  // it has done so far lives in the state like any instruction's progress. A copy
  // taken part-way through it restores to the same cycle of the same sequence.
  auto bus = noopsWithVectors();
  Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .s = 0x01FF, .p = kEightBit});
  bus.cpu = &cpu;
  cpu.setIrqLine(true);
  for (int i = 0; i < 4; ++i) cpu.stepCycle(bus);
  ASSERT_FALSE(cpu.atInstructionBoundary());
  ASSERT_EQ(cpu.state().servicing, InterruptRequest::Irq);

  const Cpu65816State midway = cpu.state();
  for (int i = 0; i < 4; ++i) cpu.stepCycle(bus);
  const Cpu65816State finished = cpu.state();

  Cpu65816 resumed(midway);
  bus.cpu = &resumed;
  for (int i = 0; i < 4; ++i) resumed.stepCycle(bus);
  EXPECT_EQ(resumed.state().pc, finished.pc);
  EXPECT_EQ(resumed.state().s, finished.s);
  EXPECT_EQ(resumed.state().p, finished.p);
  EXPECT_EQ(resumed.state().servicing, InterruptRequest::None);
}

TEST(Cpu65816Interrupt, TheCycleEngineCarriesTheInterruptsAndTheHalts) {
  // All four run on the cycle engine, narrating every cycle, so a machine can watch a
  // sequence one cycle at a time rather than only from outside it.
  for (int opcode : {0x00, 0x02, 0xCB, 0xDB}) {  // BRK, COP, WAI, STP
    auto bus = busWith({{0x001000, static_cast<std::uint8_t>(opcode)}});
    Cpu65816 cpu(Cpu65816State{.pc = 0x1000, .s = 0x01FF});
    const std::uint32_t cycles = run(bus, cpu);
    EXPECT_GE(cycles, 2u) << "opcode " << std::hex << opcode;
    EXPECT_EQ(bus.trace.size(), cycles) << "opcode " << std::hex << opcode;
  }
}

}  // namespace
