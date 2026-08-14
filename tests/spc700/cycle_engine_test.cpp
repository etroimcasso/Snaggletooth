// The cycle engine itself: what one cycle is, where an instruction's boundaries are,
// and that a snapshot taken between two cycles restores to the same place. Then the
// shapes the moves run — which cycle reads, which writes, and which reaches memory not
// at all — pinned on hand-written programs rather than on the vector corpus, so the
// laws stay readable next to the code that implements them.

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
    // One cycle is at most one access, by construction.
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

TEST(Spc700CycleEngine, BoundaryHoldsUntilTheInstructionEnds) {
  RecordingFlatBus bus = busWith({0xE4, 0x10});  // MOV A,$10
  bus.ram[0x0010] = 0x7E;
  Spc700 cpu(Spc700State{.pc = kProgram});

  EXPECT_TRUE(cpu.atInstructionBoundary());
  cpu.stepCycle(bus);  // the opcode fetch
  EXPECT_FALSE(cpu.atInstructionBoundary());
  cpu.stepCycle(bus);  // the offset
  EXPECT_FALSE(cpu.atInstructionBoundary());
  EXPECT_EQ(int{cpu.state().a}, 0) << "the load settles on the last cycle, not before";
  cpu.stepCycle(bus);  // the operand, and the load
  EXPECT_TRUE(cpu.atInstructionBoundary());
  EXPECT_EQ(int{cpu.state().a}, 0x7E);
}

TEST(Spc700CycleEngine, SnapshotMidInstructionRestoresToTheSameCycle) {
  // A five-cycle store, snapshotted two cycles in and finished on a fresh core over a
  // fresh bus: the two runs end identically, registers and memory alike.
  RecordingFlatBus first = busWith({0xD4, 0x30});  // MOV $30+X,A
  Spc700 cpu(Spc700State{.pc = kProgram, .a = 0x5A, .x = 0x04});
  cpu.stepCycle(first);
  cpu.stepCycle(first);
  const Spc700State snapshot = cpu.state();
  EXPECT_FALSE(cpu.atInstructionBoundary());

  RecordingFlatBus second = busWith({0xD4, 0x30});
  Spc700 resumed;
  resumed.restore(snapshot);
  while (!cpu.atInstructionBoundary()) {
    cpu.stepCycle(first);
    resumed.stepCycle(second);
  }

  EXPECT_TRUE(resumed.atInstructionBoundary());
  EXPECT_EQ(resumed.state().pc, cpu.state().pc);
  EXPECT_EQ(int{resumed.state().a}, int{cpu.state().a});
  EXPECT_EQ(second.ram, first.ram);
  EXPECT_EQ(int{second.ram[0x0034]}, 0x5A);
}

TEST(Spc700CycleEngine, HaltedCoreReachesNothingAndKeepsItsState) {
  RecordingFlatBus bus = busWith({0xE8, 0x2A});  // MOV A,#$2A, never executed
  Spc700 cpu(Spc700State{.pc = kProgram, .run = RunState::Stopped});

  cpu.stepCycle(bus);
  EXPECT_TRUE(bus.events.empty());
  EXPECT_EQ(cpu.state().pc, kProgram);
  EXPECT_TRUE(cpu.atInstructionBoundary());

  // The machine around the core keeps time, so a halted instruction step still costs
  // the cadence the machine prices — it just does nothing with it.
  EXPECT_EQ(cpu.stepInstruction(bus), 2u);
  EXPECT_TRUE(bus.events.empty());
  EXPECT_EQ(int{cpu.state().a}, 0);
}

TEST(Spc700CycleEngine, StepRunsTheSameInstructionAsTheCycles) {
  RecordingFlatBus whole = busWith({0xC7, 0x40});  // MOV [$40+X],A
  whole.ram[0x0044] = 0x00;
  whole.ram[0x0045] = 0x30;
  const Spc700State start{.pc = kProgram, .a = 0x99, .x = 0x04};

  Spc700 byInstruction(start);
  const std::uint32_t cycles = byInstruction.step(whole);

  RecordingFlatBus stepped = busWith({0xC7, 0x40});
  stepped.ram[0x0044] = 0x00;
  stepped.ram[0x0045] = 0x30;
  Spc700 byCycle(start);
  const std::vector<CycleEvent> trace = traceOne(stepped, byCycle);

  EXPECT_EQ(cycles, trace.size());
  EXPECT_EQ(cycles, 7u);
  EXPECT_EQ(byInstruction.state().pc, byCycle.state().pc);
  EXPECT_EQ(whole.ram, stepped.ram);
  EXPECT_EQ(int{stepped.ram[0x3000]}, 0x99);
}

TEST(Spc700CycleEngine, OneByteMoveReadsTheByteAfterTheOpcodeAndDiscardsIt) {
  // Nothing follows the opcode, but the cycle after it still reads the byte there —
  // and the program counter does not step over it.
  RecordingFlatBus bus = busWith({0x7D});  // MOV A,X
  Spc700 cpu(Spc700State{.pc = kProgram, .x = 0x6C});
  const std::vector<CycleEvent> trace = traceOne(bus, cpu);

  EXPECT_EQ(shapeOf(trace), "rr");
  ASSERT_TRUE(trace[1].address.has_value());
  EXPECT_EQ(*trace[1].address, kProgram + 1);
  EXPECT_EQ(cpu.state().pc, kProgram + 1);
  EXPECT_EQ(int{cpu.state().a}, 0x6C);
}

TEST(Spc700CycleEngine, StoreReadsItsDestinationBeforeWritingIt) {
  RecordingFlatBus bus = busWith({0xC4, 0x10});  // MOV $10,A
  Spc700 cpu(Spc700State{.pc = kProgram, .a = 0xAA});
  const std::vector<CycleEvent> trace = traceOne(bus, cpu);

  EXPECT_EQ(shapeOf(trace), "rrrw");
  ASSERT_TRUE(trace[2].address.has_value());
  EXPECT_EQ(*trace[2].address, 0x0010) << "the destination read";
  ASSERT_TRUE(trace[3].address.has_value());
  EXPECT_EQ(*trace[3].address, 0x0010);
  EXPECT_EQ(int{bus.ram[0x0010]}, 0xAA);
}

TEST(Spc700CycleEngine, AutoIncrementStoreWaitsWhereTheOtherStoresRead) {
  // Same four cycles as MOV (X),A, but the cycle before the write reaches memory not
  // at all — this store never reads its destination.
  RecordingFlatBus bus = busWith({0xAF});  // MOV (X)+,A
  Spc700 cpu(Spc700State{.pc = kProgram, .a = 0x11, .x = 0x20});
  const std::vector<CycleEvent> trace = traceOne(bus, cpu);

  EXPECT_EQ(shapeOf(trace), "rr.w");
  ASSERT_TRUE(trace[3].address.has_value());
  EXPECT_EQ(*trace[3].address, 0x0020);
  EXPECT_EQ(int{bus.ram[0x0020]}, 0x11);
  EXPECT_EQ(int{cpu.state().x}, 0x21);
}

TEST(Spc700CycleEngine, IndirectIndexedSpendsItsWaitOnDifferentCyclesByDirection) {
  // The read form waits before reading its pointer; the write form reads the pointer
  // first and waits after it.
  RecordingFlatBus reading = busWith({0xF7, 0x40});  // MOV A,[$40]+Y
  reading.ram[0x0040] = 0x00;
  reading.ram[0x0041] = 0x30;
  reading.ram[0x3005] = 0x42;
  Spc700 readCpu(Spc700State{.pc = kProgram, .y = 0x05});
  const std::vector<CycleEvent> readTrace = traceOne(reading, readCpu);

  EXPECT_EQ(shapeOf(readTrace), "rr.rrr");
  EXPECT_EQ(int{readCpu.state().a}, 0x42);

  RecordingFlatBus writing = busWith({0xD7, 0x40});  // MOV [$40]+Y,A
  writing.ram[0x0040] = 0x00;
  writing.ram[0x0041] = 0x30;
  Spc700 writeCpu(Spc700State{.pc = kProgram, .a = 0x77, .y = 0x05});
  const std::vector<CycleEvent> writeTrace = traceOne(writing, writeCpu);

  EXPECT_EQ(shapeOf(writeTrace), "rrrr.rw");
  EXPECT_EQ(int{writing.ram[0x3005]}, 0x77);
}

TEST(Spc700CycleEngine, IndexedIndirectWaitsBeforeItsPointerEitherDirection) {
  RecordingFlatBus reading = busWith({0xE7, 0x40});  // MOV A,[$40+X]
  reading.ram[0x0044] = 0x00;
  reading.ram[0x0045] = 0x30;
  reading.ram[0x3000] = 0x42;
  Spc700 readCpu(Spc700State{.pc = kProgram, .x = 0x04});
  EXPECT_EQ(shapeOf(traceOne(reading, readCpu)), "rr.rrr");
  EXPECT_EQ(int{readCpu.state().a}, 0x42);

  RecordingFlatBus writing = busWith({0xC7, 0x40});  // MOV [$40+X],A
  writing.ram[0x0044] = 0x00;
  writing.ram[0x0045] = 0x30;
  Spc700 writeCpu(Spc700State{.pc = kProgram, .a = 0x77, .x = 0x04});
  EXPECT_EQ(shapeOf(traceOne(writing, writeCpu)), "rr.rrrw");
  EXPECT_EQ(int{writing.ram[0x3000]}, 0x77);
}

TEST(Spc700CycleEngine, IndexedModesSpendTheirWaitBeforeTheAccess) {
  RecordingFlatBus dp = busWith({0xF4, 0x30});  // MOV A,$30+X
  dp.ram[0x0034] = 0x0B;
  Spc700 dpCpu(Spc700State{.pc = kProgram, .x = 0x04});
  const std::vector<CycleEvent> dpTrace = traceOne(dp, dpCpu);
  EXPECT_EQ(shapeOf(dpTrace), "rr.r");
  ASSERT_TRUE(dpTrace[3].address.has_value());
  EXPECT_EQ(*dpTrace[3].address, 0x0034);

  RecordingFlatBus abs = busWith({0xF5, 0x00, 0x30});  // MOV A,!$3000+X
  abs.ram[0x3004] = 0x0C;
  Spc700 absCpu(Spc700State{.pc = kProgram, .x = 0x04});
  const std::vector<CycleEvent> absTrace = traceOne(abs, absCpu);
  EXPECT_EQ(shapeOf(absTrace), "rrr.r");
  ASSERT_TRUE(absTrace[4].address.has_value());
  EXPECT_EQ(*absTrace[4].address, 0x3004);
}

TEST(Spc700CycleEngine, TwoOperandMovesDifferInWhichAddressTheyRead) {
  // MOV dp,dp reads its source and never its destination; MOV dp,#imm has its byte
  // already and reads the destination instead.
  RecordingFlatBus copy = busWith({0xFA, 0x00, 0xFF});  // MOV $FF,$00
  copy.ram[0x0000] = 0x3C;
  Spc700 copyCpu(Spc700State{.pc = kProgram});
  const std::vector<CycleEvent> copyTrace = traceOne(copy, copyCpu);
  EXPECT_EQ(shapeOf(copyTrace), "rrrrw");
  ASSERT_TRUE(copyTrace[2].address.has_value());
  EXPECT_EQ(*copyTrace[2].address, 0x0000) << "the source read";
  EXPECT_EQ(int{copy.ram[0x00FF]}, 0x3C);

  RecordingFlatBus immediate = busWith({0x8F, 0x00, 0xFF});  // MOV $FF,#$00
  Spc700 immediateCpu(Spc700State{.pc = kProgram});
  const std::vector<CycleEvent> immediateTrace = traceOne(immediate, immediateCpu);
  EXPECT_EQ(shapeOf(immediateTrace), "rrrrw");
  ASSERT_TRUE(immediateTrace[3].address.has_value());
  EXPECT_EQ(*immediateTrace[3].address, 0x00FF) << "the destination read";
}

TEST(Spc700CycleEngine, TheAutoIncrementLoadStepsXOnItsLastCycle) {
  RecordingFlatBus bus = busWith({0xBF});  // MOV A,(X)+
  bus.ram[0x0020] = 0x5E;
  Spc700 cpu(Spc700State{.pc = kProgram, .x = 0x20});

  cpu.stepCycle(bus);
  cpu.stepCycle(bus);
  cpu.stepCycle(bus);  // the operand read
  EXPECT_EQ(int{cpu.state().x}, 0x20) << "X steps with the load, on the last cycle";
  EXPECT_EQ(int{cpu.state().a}, 0);
  cpu.stepCycle(bus);
  EXPECT_TRUE(cpu.atInstructionBoundary());
  EXPECT_EQ(int{cpu.state().a}, 0x5E);
  EXPECT_EQ(int{cpu.state().x}, 0x21);
}

TEST(Spc700CycleEngine, EveryOpcodeFinishesOneCycleAtATime) {
  // The whole instruction set runs on the cycle engine. Driving each of the 256
  // opcodes from a boundary reaches the next boundary within the longest instruction
  // the CPU has, so an opcode with no cycle sequence would show up here as one that
  // never finishes rather than as one that quietly does nothing.
  constexpr std::size_t kLongestInstruction = 12;  // DIV YA,X
  for (unsigned opcode = 0; opcode <= 0xFF; ++opcode) {
    RecordingFlatBus bus = busWith({static_cast<std::uint8_t>(opcode)});
    Spc700 cpu(Spc700State{.pc = kProgram});

    std::size_t cycles = 0;
    do {
      cpu.stepCycle(bus);
      ++cycles;
    } while (!cpu.atInstructionBoundary() && cycles <= kLongestInstruction);

    EXPECT_TRUE(cpu.atInstructionBoundary())
        << "opcode " << opcode << " had not finished after " << cycles << " cycles";
    EXPECT_GE(cycles, 2u) << "opcode " << opcode << " (every instruction reads the "
                             "byte after its opcode)";

    // And the whole-instruction call agrees with the cycles it is made of.
    RecordingFlatBus whole = busWith({static_cast<std::uint8_t>(opcode)});
    Spc700 wholeCpu(Spc700State{.pc = kProgram});
    EXPECT_EQ(wholeCpu.stepInstruction(whole), cycles)
        << "opcode " << opcode << " (the two calls disagree on its length)";
    EXPECT_EQ(whole.ram, bus.ram) << "opcode " << opcode;
  }
}

}  // namespace
