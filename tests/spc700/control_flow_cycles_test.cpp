// Where the cycles go in the instructions that move the program counter: what a taken
// branch costs over an untaken one, the order a call reaches the stack and its
// destination in, which way round a push and a pop spend their internal cycle, and how
// the chip enters a halt. Pinned on hand-written programs rather than on the vector
// corpus, so the laws stay readable next to the code that implements them.

#include "snaggletooth/apu/spc700.h"
#include "vector_harness.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

namespace {

using snaggletooth::RunState;
using snaggletooth::Spc700;
using snaggletooth::Spc700State;
using snaggletooth::test::CycleEvent;
using snaggletooth::test::RecordingFlatBus;

constexpr std::uint16_t kProgram = 0x0200;

// A bus holding `program` at $0200.
RecordingFlatBus busWith(std::initializer_list<std::uint8_t> program) {
  RecordingFlatBus bus;
  std::uint16_t address = kProgram;
  for (std::uint8_t byte : program) bus.ram[address++] = byte;
  return bus;
}

// One instruction, driven a cycle at a time. A cycle the core reaches memory on is
// recorded as it happened; a cycle it reaches nothing on is recorded as a wait — the
// same synthesis the vector runner makes.
std::vector<CycleEvent> traceOne(RecordingFlatBus& bus, Spc700& cpu) {
  std::vector<CycleEvent> trace;
  do {
    const std::size_t narrated = bus.events.size();
    cpu.stepCycle(bus);
    EXPECT_LE(bus.events.size(), narrated + 1) << "cycle " << trace.size();
    trace.push_back(bus.events.size() == narrated ? CycleEvent{} : bus.events.back());
  } while (!cpu.atInstructionBoundary());
  return trace;
}

// A trace's shape as a readable string: `r` a read, `w` a write, `.` a wait.
std::string shapeOf(const std::vector<CycleEvent>& trace) {
  std::string shape;
  for (const CycleEvent& cycle : trace) {
    switch (cycle.kind) {
      case CycleEvent::Kind::Read: shape += 'r'; break;
      case CycleEvent::Kind::Write: shape += 'w'; break;
      case CycleEvent::Kind::Wait: shape += '.'; break;
    }
  }
  return shape;
}

// The shape one instruction runs, from a program and the state it starts in.
std::string shapeRun(std::initializer_list<std::uint8_t> program, Spc700State start) {
  RecordingFlatBus bus = busWith(program);
  start.pc = kProgram;
  Spc700 cpu(start);
  return shapeOf(traceOne(bus, cpu));
}

TEST(Spc700ControlFlowCycles, ATakenBranchCostsTwoCyclesInsideTheChip) {
  // An untaken branch is over as soon as its displacement has arrived. A taken one
  // spends two more cycles reaching memory not at all, and the program counter moves
  // on the last of them.
  EXPECT_EQ(shapeRun({0xF0, 0x05}, {.psw = snaggletooth::kFlagZ}), "rr..") << "BEQ, taken";
  EXPECT_EQ(shapeRun({0xF0, 0x05}, {}), "rr") << "BEQ, not taken";
  EXPECT_EQ(shapeRun({0xD0, 0x05}, {}), "rr..") << "BNE, taken";
  EXPECT_EQ(shapeRun({0x2F, 0x05}, {}), "rr..") << "BRA, which is always taken";

  RecordingFlatBus bus = busWith({0x2F, 0x05});  // BRA +5
  Spc700 cpu(Spc700State{.pc = kProgram});
  traceOne(bus, cpu);
  EXPECT_EQ(cpu.state().pc, kProgram + 2 + 5)
      << "the displacement counts from past the whole instruction";
}

TEST(Spc700ControlFlowCycles, ABitBranchReadsItsByteBeforeItsDisplacement) {
  // BBS and BBC reach the direct-page byte first, spend a cycle testing it, and only
  // then read the displacement — which is the third byte of the instruction.
  RecordingFlatBus bus = busWith({0x03, 0x10, 0x05});  // BBS $10.0,+5
  bus.ram[0x0010] = 0x01;
  Spc700 cpu(Spc700State{.pc = kProgram});
  const std::vector<CycleEvent> trace = traceOne(bus, cpu);

  EXPECT_EQ(shapeOf(trace), "rrr.r..");
  ASSERT_EQ(trace.size(), 7u);
  EXPECT_EQ(*trace[2].address, 0x0010) << "the byte the bit lives in";
  EXPECT_EQ(*trace[4].address, kProgram + 2) << "then the displacement";
  EXPECT_EQ(cpu.state().pc, kProgram + 3 + 5);

  EXPECT_EQ(shapeRun({0x03, 0x10, 0x05}, {}), "rrr.r") << "the bit is clear, so it stays";
  EXPECT_EQ(shapeRun({0x13, 0x10, 0x05}, {}), "rrr.r..") << "BBC branches on it clear";
  EXPECT_EQ(shapeRun({0x2E, 0x10, 0x05}, {.a = 0x7F}), "rrr.r..")
      << "CBNE dp runs the same shape, comparing against A";
}

TEST(Spc700ControlFlowCycles, TheIndexedCompareSpendsACycleBeforeEachOfItsReads) {
  // CBNE dp+X pays the indexing cycle every indexed mode pays, before it reaches the
  // byte — and another before the displacement, which puts it one cycle behind CBNE dp.
  RecordingFlatBus bus = busWith({0xDE, 0x10, 0x05});  // CBNE $10+X,+5
  bus.ram[0x0014] = 0x01;
  Spc700 cpu(Spc700State{.pc = kProgram, .x = 0x04});
  const std::vector<CycleEvent> trace = traceOne(bus, cpu);

  EXPECT_EQ(shapeOf(trace), "rr.r.r..");
  ASSERT_EQ(trace.size(), 8u);
  EXPECT_EQ(*trace[3].address, 0x0014) << "the indexed byte";
  EXPECT_EQ(*trace[5].address, kProgram + 2) << "then the displacement";
  EXPECT_EQ(cpu.state().pc, kProgram + 3 + 5);
}

TEST(Spc700ControlFlowCycles, TheDirectPageDecrementWritesBeforeItReadsItsDisplacement) {
  // DBNZ dp puts the decremented byte back before the displacement arrives, so the
  // store lands whether or not the branch is taken.
  RecordingFlatBus taken = busWith({0x6E, 0x10, 0x05});  // DBNZ $10,+5
  taken.ram[0x0010] = 0x02;
  Spc700 takenCpu(Spc700State{.pc = kProgram});
  const std::vector<CycleEvent> trace = traceOne(taken, takenCpu);

  EXPECT_EQ(shapeOf(trace), "rrrwr..");
  ASSERT_EQ(trace.size(), 7u);
  EXPECT_EQ(*trace[2].address, 0x0010) << "the byte is read";
  EXPECT_EQ(*trace[3].address, 0x0010) << "and written back one cycle later";
  EXPECT_EQ(int{*trace[3].value}, 0x01);
  EXPECT_EQ(*trace[4].address, kProgram + 2) << "only then does the displacement arrive";

  RecordingFlatBus done = busWith({0x6E, 0x10, 0x05});
  done.ram[0x0010] = 0x01;
  Spc700 doneCpu(Spc700State{.pc = kProgram});
  EXPECT_EQ(shapeOf(traceOne(done, doneCpu)), "rrrwr");
  EXPECT_EQ(int{done.ram[0x0010]}, 0x00) << "the count reached zero, and still went back";
  EXPECT_EQ(doneCpu.state().pc, kProgram + 3);
}

TEST(Spc700ControlFlowCycles, TheYDecrementReadsItsDisplacementTwice) {
  // DBNZ Y has one operand byte and reads it twice: once as the displacement, and
  // again after the cycle the decrement takes. Y settles on the last cycle.
  RecordingFlatBus bus = busWith({0xFE, 0x05});  // DBNZ Y,+5
  Spc700 cpu(Spc700State{.pc = kProgram, .y = 0x02});
  const std::vector<CycleEvent> trace = traceOne(bus, cpu);

  EXPECT_EQ(shapeOf(trace), "rr.r..");
  ASSERT_EQ(trace.size(), 6u);
  EXPECT_EQ(*trace[1].address, kProgram + 1);
  EXPECT_EQ(*trace[3].address, kProgram + 1) << "the same byte, read a second time";
  EXPECT_EQ(int{cpu.state().y}, 0x01);
  EXPECT_EQ(cpu.state().pc, kProgram + 2 + 5);

  EXPECT_EQ(shapeRun({0xFE, 0x05}, {.y = 0x01}), "rr.r") << "the count reached zero";
}

TEST(Spc700ControlFlowCycles, TheJumpThroughAPointerDoesNotWrapInsideAPage) {
  // JMP [!abs+X] is the one address in the core that steps linearly: the pointer's
  // high byte is at the 16-bit address one past its low byte, not at the one wrapped
  // inside the page the low byte sits in.
  RecordingFlatBus bus = busWith({0x1F, 0xFF, 0x02});  // JMP [!$02FF+X]
  bus.ram[0x02FF] = 0x78;
  bus.ram[0x0300] = 0x56;
  Spc700 cpu(Spc700State{.pc = kProgram});
  const std::vector<CycleEvent> trace = traceOne(bus, cpu);

  EXPECT_EQ(shapeOf(trace), "rrr.rr");
  ASSERT_EQ(trace.size(), 6u);
  EXPECT_EQ(*trace[4].address, 0x02FF);
  EXPECT_EQ(*trace[5].address, 0x0300) << "one past it, across the page boundary";
  EXPECT_EQ(cpu.state().pc, 0x5678);

  EXPECT_EQ(shapeRun({0x5F, 0x00, 0x03}, {}), "rrr")
      << "JMP !abs spends no cycle inside the chip at all";
}

TEST(Spc700ControlFlowCycles, ACallReachesItsDestinationBeforeItReachesTheStack) {
  // CALL takes its destination off the program stream first, then writes the return
  // address high byte first, and ends with two cycles inside the chip.
  RecordingFlatBus bus = busWith({0x3F, 0x00, 0x03});  // CALL !$0300
  Spc700 cpu(Spc700State{.pc = kProgram, .sp = 0xEF});
  const std::vector<CycleEvent> trace = traceOne(bus, cpu);

  EXPECT_EQ(shapeOf(trace), "rrr.ww..");
  ASSERT_EQ(trace.size(), 8u);
  EXPECT_EQ(*trace[4].address, 0x01EF) << "the stack, at the byte SP names";
  EXPECT_EQ(int{*trace[4].value}, 0x02) << "the return address, high byte first";
  EXPECT_EQ(*trace[5].address, 0x01EE);
  EXPECT_EQ(int{*trace[5].value}, 0x03) << "so the low byte lands at the lower address";
  EXPECT_EQ(cpu.state().pc, 0x0300);
  EXPECT_EQ(int{cpu.state().sp}, 0xED) << "SP settles on the last cycle, two entries down";

  EXPECT_EQ(shapeRun({0x4F, 0x40}, {.sp = 0xEF}), "rr.ww.")
      << "PCALL has one operand byte, so its pushes begin a cycle earlier";
}

TEST(Spc700ControlFlowCycles, TheVectorCallPushesBeforeItReadsItsDestination) {
  // TCALL takes its destination from the table below $FFDE, two bytes per entry — and
  // reaches it only after the return address is already on the stack.
  RecordingFlatBus bus = busWith({0x01});  // TCALL 0
  bus.ram[0xFFDE] = 0x00;
  bus.ram[0xFFDF] = 0x40;
  Spc700 cpu(Spc700State{.pc = kProgram, .sp = 0xEF});
  const std::vector<CycleEvent> trace = traceOne(bus, cpu);

  EXPECT_EQ(shapeOf(trace), "rr.ww.rr");
  ASSERT_EQ(trace.size(), 8u);
  EXPECT_EQ(*trace[1].address, kProgram + 1) << "the discarded byte after the opcode";
  EXPECT_EQ(*trace[3].address, 0x01EF);
  EXPECT_EQ(int{*trace[3].value}, 0x02) << "the return address goes down first";
  EXPECT_EQ(*trace[4].address, 0x01EE);
  EXPECT_EQ(int{*trace[4].value}, 0x01);
  EXPECT_EQ(*trace[6].address, 0xFFDE) << "and the destination is read afterwards";
  EXPECT_EQ(*trace[7].address, 0xFFDF);
  EXPECT_EQ(cpu.state().pc, 0x4000);
  EXPECT_EQ(int{cpu.state().sp}, 0xED);

  RecordingFlatBus fifteenth = busWith({0xF1});  // TCALL 15
  Spc700 fifteenthCpu(Spc700State{.pc = kProgram, .sp = 0xEF});
  const std::vector<CycleEvent> entry = traceOne(fifteenth, fifteenthCpu);
  EXPECT_EQ(*entry[6].address, 0xFFDE - 2 * 15) << "the table counts down from entry zero";
}

TEST(Spc700ControlFlowCycles, BreakPushesTheStatusByteAndStartsItsPushesEarlier) {
  // BRK reaches the same table entry TCALL 0 does, and pushes the status byte under
  // the return address — but where TCALL spends a cycle inside the chip before its
  // pushes, BRK begins them immediately.
  RecordingFlatBus bus = busWith({0x0F});  // BRK
  bus.ram[0xFFDE] = 0x00;
  bus.ram[0xFFDF] = 0x40;
  Spc700 cpu(Spc700State{
      .pc = kProgram, .sp = 0xEF, .psw = snaggletooth::kFlagC | snaggletooth::kFlagI});
  const std::vector<CycleEvent> trace = traceOne(bus, cpu);

  EXPECT_EQ(shapeOf(trace), "rrwww.rr");
  ASSERT_EQ(trace.size(), 8u);
  EXPECT_EQ(*trace[2].address, 0x01EF);
  EXPECT_EQ(int{*trace[2].value}, 0x02) << "the return address, high byte first";
  EXPECT_EQ(*trace[3].address, 0x01EE);
  EXPECT_EQ(int{*trace[3].value}, 0x01);
  EXPECT_EQ(*trace[4].address, 0x01ED);
  EXPECT_EQ(int{*trace[4].value}, int{snaggletooth::kFlagC | snaggletooth::kFlagI})
      << "the status byte as it stood, before the instruction changed it";
  EXPECT_EQ(*trace[6].address, 0xFFDE);
  EXPECT_EQ(cpu.state().pc, 0x4000);
  EXPECT_EQ(int{cpu.state().sp}, 0xEC) << "three entries down, not two";
  EXPECT_EQ(int{cpu.state().psw},
            int{snaggletooth::kFlagC | snaggletooth::kFlagB})
      << "the break flag is set and the interrupt enable cleared";
}

TEST(Spc700ControlFlowCycles, AReturnTakesItsAddressBackLowByteFirst) {
  // RET spends a cycle inside the chip and then reads the two bytes upwards from
  // where SP points — the reverse of the order CALL wrote them.
  RecordingFlatBus bus = busWith({0x6F});  // RET
  bus.ram[0x01FE] = 0x34;
  bus.ram[0x01FF] = 0x12;
  Spc700 cpu(Spc700State{.pc = kProgram, .sp = 0xFD});
  const std::vector<CycleEvent> trace = traceOne(bus, cpu);

  EXPECT_EQ(shapeOf(trace), "rr.rr");
  ASSERT_EQ(trace.size(), 5u);
  EXPECT_EQ(*trace[3].address, 0x01FE) << "the low byte, at the lower address";
  EXPECT_EQ(*trace[4].address, 0x01FF);
  EXPECT_EQ(cpu.state().pc, 0x1234);
  EXPECT_EQ(int{cpu.state().sp}, 0xFF);
}

TEST(Spc700ControlFlowCycles, TheInterruptReturnTakesTheStatusByteBackFirst) {
  // RETI is RET with one more read under it: the status byte comes off the stack
  // before the return address, because it was pushed after it.
  RecordingFlatBus bus = busWith({0x7F});  // RETI
  bus.ram[0x01FD] = snaggletooth::kFlagN;
  bus.ram[0x01FE] = 0x34;
  bus.ram[0x01FF] = 0x12;
  Spc700 cpu(Spc700State{.pc = kProgram, .sp = 0xFC});
  const std::vector<CycleEvent> trace = traceOne(bus, cpu);

  EXPECT_EQ(shapeOf(trace), "rr.rrr");
  ASSERT_EQ(trace.size(), 6u);
  EXPECT_EQ(*trace[3].address, 0x01FD) << "the status byte";
  EXPECT_EQ(*trace[4].address, 0x01FE);
  EXPECT_EQ(*trace[5].address, 0x01FF);
  EXPECT_EQ(cpu.state().pc, 0x1234);
  EXPECT_EQ(int{cpu.state().psw}, int{snaggletooth::kFlagN});
  EXPECT_EQ(int{cpu.state().sp}, 0xFF);
}

TEST(Spc700ControlFlowCycles, APushWritesThenWaitsAndAPopWaitsThenReads) {
  // The two stack transfers are mirror images: the push reaches memory on its third
  // cycle and spends the fourth inside the chip, the pop spends the third inside the
  // chip and reaches memory on the fourth.
  RecordingFlatBus pushing = busWith({0x2D});  // PUSH A
  Spc700 pushCpu(Spc700State{.pc = kProgram, .a = 0x5A, .sp = 0xEF});
  const std::vector<CycleEvent> pushTrace = traceOne(pushing, pushCpu);

  EXPECT_EQ(shapeOf(pushTrace), "rrw.");
  ASSERT_EQ(pushTrace.size(), 4u);
  EXPECT_EQ(*pushTrace[2].address, 0x01EF);
  EXPECT_EQ(int{*pushTrace[2].value}, 0x5A);
  EXPECT_EQ(int{pushCpu.state().sp}, 0xEE);

  RecordingFlatBus popping = busWith({0xAE});  // POP A
  popping.ram[0x01F0] = 0x99;
  Spc700 popCpu(Spc700State{.pc = kProgram, .sp = 0xEF});
  const std::vector<CycleEvent> popTrace = traceOne(popping, popCpu);

  EXPECT_EQ(shapeOf(popTrace), "rr.r");
  ASSERT_EQ(popTrace.size(), 4u);
  EXPECT_EQ(*popTrace[3].address, 0x01F0) << "one entry above the one SP named";
  EXPECT_EQ(int{popCpu.state().a}, 0x99);
  EXPECT_EQ(int{popCpu.state().sp}, 0xF0);
  EXPECT_EQ(int{popCpu.state().psw}, 0) << "popping a register sets no flag";

  RecordingFlatBus status = busWith({0x8E});  // POP PSW
  status.ram[0x01F0] = snaggletooth::kFlagV;
  Spc700 statusCpu(Spc700State{.pc = kProgram, .sp = 0xEF});
  traceOne(status, statusCpu);
  EXPECT_EQ(int{statusCpu.state().psw}, int{snaggletooth::kFlagV})
      << "the status byte comes back whole";
}

TEST(Spc700ControlFlowCycles, AHaltReadsTheByteAfterItsOpcodeThreeTimesOver) {
  // SLEEP and STOP take seven cycles: three reads of the byte after the opcode, each
  // followed by a cycle inside the chip. The core halts on an instruction boundary,
  // with the program counter left on the byte it kept reading.
  RecordingFlatBus bus = busWith({0xEF});  // SLEEP
  Spc700 cpu(Spc700State{.pc = kProgram});
  const std::vector<CycleEvent> trace = traceOne(bus, cpu);

  EXPECT_EQ(shapeOf(trace), "rr.r.r.");
  ASSERT_EQ(trace.size(), 7u);
  EXPECT_EQ(*trace[1].address, kProgram + 1);
  EXPECT_EQ(*trace[3].address, kProgram + 1);
  EXPECT_EQ(*trace[5].address, kProgram + 1);
  EXPECT_EQ(cpu.state().pc, kProgram + 1) << "the byte is read, never stepped over";
  EXPECT_EQ(cpu.state().run, RunState::Sleeping);
  EXPECT_TRUE(cpu.atInstructionBoundary());

  // A halted core reaches nothing at all, and the cycle its host prices changes no
  // state either.
  const std::size_t narrated = bus.events.size();
  cpu.stepCycle(bus);
  EXPECT_EQ(bus.events.size(), narrated);
  EXPECT_EQ(cpu.stepInstruction(bus), 2u);
  EXPECT_EQ(bus.events.size(), narrated);
  EXPECT_EQ(cpu.state().pc, kProgram + 1);

  EXPECT_EQ(shapeRun({0xFF}, {}), "rr.r.r.") << "STOP runs the same shape";
}

TEST(Spc700ControlFlowCycles, AFlagOperationSettlesOnItsSecondCycleOrItsThird) {
  // The flag operations read the byte after the opcode and discard it. Most settle
  // there; the three that do not spend one further cycle inside the chip.
  EXPECT_EQ(shapeRun({0x60}, {}), "rr") << "CLRC";
  EXPECT_EQ(shapeRun({0x80}, {}), "rr") << "SETC";
  EXPECT_EQ(shapeRun({0x20}, {}), "rr") << "CLRP";
  EXPECT_EQ(shapeRun({0x40}, {}), "rr") << "SETP";
  EXPECT_EQ(shapeRun({0xE0}, {}), "rr") << "CLRV";
  EXPECT_EQ(shapeRun({0x00}, {}), "rr") << "NOP";
  EXPECT_EQ(shapeRun({0xED}, {}), "rr.") << "NOTC";
  EXPECT_EQ(shapeRun({0xA0}, {}), "rr.") << "EI";
  EXPECT_EQ(shapeRun({0xC0}, {}), "rr.") << "DI";

  RecordingFlatBus bus = busWith({0xE0});  // CLRV
  Spc700 cpu(Spc700State{
      .pc = kProgram,
      .psw = snaggletooth::kFlagV | snaggletooth::kFlagH | snaggletooth::kFlagC});
  traceOne(bus, cpu);
  EXPECT_EQ(int{cpu.state().psw}, int{snaggletooth::kFlagC})
      << "CLRV clears the half-carry with the overflow";

  RecordingFlatBus enabling = busWith({0xA0});  // EI
  Spc700 enableCpu(Spc700State{.pc = kProgram});
  traceOne(enabling, enableCpu);
  EXPECT_EQ(int{enableCpu.state().psw}, int{snaggletooth::kFlagI})
      << "I is an enable: EI sets it";
}

}  // namespace
