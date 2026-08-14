// Where the cycles go in the arithmetic, logic, increment and shift instructions:
// which cycle reads, which writes, which reaches memory not at all, and in what order
// the two-operand forms touch their two addresses. Pinned on hand-written programs
// rather than on the vector corpus, so the laws stay readable next to the code that
// implements them.

#include "snaggletooth/apu/spc700.h"
#include "vector_harness.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

namespace {

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

TEST(Spc700AluCycles, IndexingSpendsItsCycleBeforeTheAccess) {
  // Adding the index register happens inside the chip, and it happens BEFORE the
  // operand is read — so an indexed mode carries a cycle that reaches memory not at
  // all where the unindexed one goes straight to its byte.
  EXPECT_EQ(shapeRun({0x04, 0x10}, {}), "rrr") << "OR A,dp";
  EXPECT_EQ(shapeRun({0x14, 0x10}, {.x = 0x04}), "rr.r") << "OR A,dp+X";

  // The same law on the wider modes: the absolute forms settle two operand bytes
  // first, then spend the indexing cycle.
  EXPECT_EQ(shapeRun({0x05, 0x00, 0x03}, {}), "rrrr") << "OR A,!abs";
  EXPECT_EQ(shapeRun({0x15, 0x00, 0x03}, {.x = 0x04}), "rrr.r") << "OR A,!abs+X";
  EXPECT_EQ(shapeRun({0x16, 0x00, 0x03}, {.y = 0x04}), "rrr.r") << "OR A,!abs+Y";

  // And on the indirect forms, where the cycle sits ahead of the pointer reads.
  EXPECT_EQ(shapeRun({0x87, 0x10}, {.x = 0x04}), "rr.rrr") << "ADC A,[dp+X]";
  EXPECT_EQ(shapeRun({0x97, 0x10}, {.y = 0x04}), "rr.rrr") << "ADC A,[dp]+Y";
}

TEST(Spc700AluCycles, ReadModifyWriteReadsAndWritesTheSameAddress) {
  // The seat every increment, decrement, shift and rotation in memory runs: the byte
  // is read, and the result goes back where it came from on the next cycle.
  RecordingFlatBus bus = busWith({0xAB, 0x10});  // INC $10
  bus.ram[0x0010] = 0x41;
  Spc700 cpu(Spc700State{.pc = kProgram});
  const std::vector<CycleEvent> trace = traceOne(bus, cpu);

  EXPECT_EQ(shapeOf(trace), "rrrw");
  ASSERT_EQ(trace.size(), 4u);
  EXPECT_EQ(*trace[2].address, 0x0010) << "the byte is read where it lives";
  EXPECT_EQ(*trace[3].address, 0x0010) << "and written back to the same address";
  EXPECT_EQ(int{*trace[3].value}, 0x42);
  EXPECT_EQ(int{bus.ram[0x0010]}, 0x42);
}

TEST(Spc700AluCycles, ReadModifyWriteRunsTheIndexedAndAbsoluteSeatsToo) {
  EXPECT_EQ(shapeRun({0xBB, 0x10}, {.x = 0x04}), "rr.rw") << "INC dp+X";
  EXPECT_EQ(shapeRun({0xAC, 0x00, 0x03}, {}), "rrrrw") << "INC !abs";
  EXPECT_EQ(shapeRun({0x0B, 0x10}, {}), "rrrw") << "ASL dp";
  EXPECT_EQ(shapeRun({0x6C, 0x00, 0x03}, {}), "rrrrw") << "ROR !abs";
}

TEST(Spc700AluCycles, TheResultLandsOnTheCycleThatWritesIt) {
  // A modify settles at its destination, not before: the byte in memory is untouched
  // until the last cycle, and so are the flags the instruction sets.
  RecordingFlatBus bus = busWith({0xAB, 0x10});  // INC $10
  bus.ram[0x0010] = 0xFF;
  Spc700 cpu(Spc700State{.pc = kProgram});

  cpu.stepCycle(bus);  // the opcode
  cpu.stepCycle(bus);  // the offset
  cpu.stepCycle(bus);  // the byte
  EXPECT_EQ(int{bus.ram[0x0010]}, 0xFF) << "the byte is still the one that was read";
  EXPECT_EQ(int{cpu.state().psw}, 0) << "and the flags have not moved";

  cpu.stepCycle(bus);  // the write
  EXPECT_TRUE(cpu.atInstructionBoundary());
  EXPECT_EQ(int{bus.ram[0x0010]}, 0x00);
  EXPECT_EQ(int{cpu.state().psw & snaggletooth::kFlagZ}, int{snaggletooth::kFlagZ});
}

TEST(Spc700AluCycles, TheTwoIndirectOperandsAreReadYSideFirst) {
  // ADC (X),(Y) takes its source from the Y side and both its other operand and its
  // target from the X side. The Y side is read first, and the X side is the address
  // the result goes back to.
  RecordingFlatBus bus = busWith({0x99});  // ADC (X),(Y)
  bus.ram[0x0020] = 0x11;                  // the X side
  bus.ram[0x0030] = 0x22;                  // the Y side
  Spc700 cpu(Spc700State{.pc = kProgram, .x = 0x20, .y = 0x30});
  const std::vector<CycleEvent> trace = traceOne(bus, cpu);

  EXPECT_EQ(shapeOf(trace), "rrrrw");
  ASSERT_EQ(trace.size(), 5u);
  EXPECT_EQ(*trace[1].address, kProgram + 1) << "the byte after a one-byte opcode";
  EXPECT_EQ(*trace[2].address, 0x0030) << "the Y side is read first";
  EXPECT_EQ(*trace[3].address, 0x0020) << "then the X side";
  EXPECT_EQ(*trace[4].address, 0x0020) << "and the result goes back to the X side";
  EXPECT_EQ(int{*trace[4].value}, 0x33);
}

TEST(Spc700AluCycles, TheTwoDirectPageOperandsAreReadSourceFirst) {
  // OR dp,dp carries its source offset first and its destination offset second, and
  // reaches each byte as soon as the offset that names it has been read.
  RecordingFlatBus bus = busWith({0x09, 0x10, 0x20});  // OR $20,$10
  bus.ram[0x0010] = 0x0F;                              // the source
  bus.ram[0x0020] = 0xF0;                              // the destination
  Spc700 cpu(Spc700State{.pc = kProgram});
  const std::vector<CycleEvent> trace = traceOne(bus, cpu);

  EXPECT_EQ(shapeOf(trace), "rrrrrw");
  ASSERT_EQ(trace.size(), 6u);
  EXPECT_EQ(*trace[2].address, 0x0010) << "the source byte, named by the first offset";
  EXPECT_EQ(*trace[3].address, kProgram + 2) << "then the destination offset";
  EXPECT_EQ(*trace[4].address, 0x0020) << "then the destination byte";
  EXPECT_EQ(*trace[5].address, 0x0020);
  EXPECT_EQ(int{*trace[5].value}, 0xFF);
}

TEST(Spc700AluCycles, TheImmediateFormReachesOnlyItsDestination) {
  // OR dp,#imm carries its source in the instruction, so the only byte it reads is
  // the one it is about to write.
  RecordingFlatBus bus = busWith({0x18, 0x0F, 0x20});  // OR $20,#$0F
  bus.ram[0x0020] = 0xF0;
  Spc700 cpu(Spc700State{.pc = kProgram});
  const std::vector<CycleEvent> trace = traceOne(bus, cpu);

  EXPECT_EQ(shapeOf(trace), "rrrrw");
  ASSERT_EQ(trace.size(), 5u);
  EXPECT_EQ(*trace[1].address, kProgram + 1) << "the immediate byte";
  EXPECT_EQ(*trace[2].address, kProgram + 2) << "then the destination offset";
  EXPECT_EQ(*trace[3].address, 0x0020) << "then the destination byte";
  EXPECT_EQ(int{*trace[4].value}, 0xFF);
}

TEST(Spc700AluCycles, AComparisonSpendsTheCycleItsSiblingWritesOn) {
  // A comparison discards its result, so where the arithmetic forms write, it reaches
  // memory not at all. The cycle is still spent — the two forms take the same time.
  EXPECT_EQ(shapeRun({0x09, 0x10, 0x20}, {}), "rrrrrw") << "OR dp,dp";
  EXPECT_EQ(shapeRun({0x69, 0x10, 0x20}, {}), "rrrrr.") << "CMP dp,dp";

  EXPECT_EQ(shapeRun({0x18, 0x0F, 0x20}, {}), "rrrrw") << "OR dp,#imm";
  EXPECT_EQ(shapeRun({0x78, 0x0F, 0x20}, {}), "rrrr.") << "CMP dp,#imm";

  EXPECT_EQ(shapeRun({0x19}, {.x = 0x20, .y = 0x30}), "rrrrw") << "OR (X),(Y)";
  EXPECT_EQ(shapeRun({0x79}, {.x = 0x20, .y = 0x30}), "rrrr.") << "CMP (X),(Y)";
}

TEST(Spc700AluCycles, AComparisonLeavesTheByteItReadAlone) {
  RecordingFlatBus bus = busWith({0x79});  // CMP (X),(Y)
  bus.ram[0x0020] = 0x40;
  bus.ram[0x0030] = 0x40;
  Spc700 cpu(Spc700State{.pc = kProgram, .x = 0x20, .y = 0x30});
  traceOne(bus, cpu);

  EXPECT_EQ(int{bus.ram[0x0020]}, 0x40) << "the target keeps its byte";
  EXPECT_EQ(int{cpu.state().psw & snaggletooth::kFlagZ}, int{snaggletooth::kFlagZ})
      << "and the comparison still sets the flags";
  EXPECT_EQ(int{cpu.state().psw & snaggletooth::kFlagC}, int{snaggletooth::kFlagC});
}

TEST(Spc700AluCycles, TheNibbleExchangeRunsOnInternalCycles) {
  // XCN reaches memory twice — the opcode, and the byte after it that every one-byte
  // instruction reads and discards — and spends the rest of its time inside the chip.
  RecordingFlatBus bus = busWith({0x9F});  // XCN A
  Spc700 cpu(Spc700State{.pc = kProgram, .a = 0x12});
  const std::vector<CycleEvent> trace = traceOne(bus, cpu);

  EXPECT_EQ(shapeOf(trace), "rr...");
  EXPECT_EQ(cpu.state().pc, kProgram + 1) << "the discarded byte is not stepped over";
  EXPECT_EQ(int{cpu.state().a}, 0x21);
}

TEST(Spc700AluCycles, ARegisterOperationReadsTheByteAfterItAndDiscardsIt) {
  // The one-byte forms — a register's own increment, decrement, shift or rotation —
  // still issue the read the second cycle of every instruction makes.
  EXPECT_EQ(shapeRun({0xBC}, {}), "rr") << "INC A";
  EXPECT_EQ(shapeRun({0x1D}, {}), "rr") << "DEC X";
  EXPECT_EQ(shapeRun({0x1C}, {}), "rr") << "ASL A";

  RecordingFlatBus bus = busWith({0x3D});  // INC X
  Spc700 cpu(Spc700State{.pc = kProgram, .x = 0x7F});
  const std::vector<CycleEvent> trace = traceOne(bus, cpu);
  ASSERT_EQ(trace.size(), 2u);
  EXPECT_EQ(*trace[1].address, kProgram + 1);
  EXPECT_EQ(cpu.state().pc, kProgram + 1) << "the discarded byte is not an operand";
  EXPECT_EQ(int{cpu.state().x}, 0x80);
}

TEST(Spc700AluCycles, TheIndirectReadReachesTheDirectPageWithoutAnOperand) {
  // ADC A,(X) addresses its byte with X alone, so the byte after the opcode is read
  // and thrown away rather than being an offset.
  RecordingFlatBus bus = busWith({0x86});  // ADC A,(X)
  bus.ram[0x0020] = 0x01;
  Spc700 cpu(Spc700State{.pc = kProgram, .a = 0x01, .x = 0x20});
  const std::vector<CycleEvent> trace = traceOne(bus, cpu);

  EXPECT_EQ(shapeOf(trace), "rrr");
  ASSERT_EQ(trace.size(), 3u);
  EXPECT_EQ(*trace[1].address, kProgram + 1);
  EXPECT_EQ(*trace[2].address, 0x0020);
  EXPECT_EQ(cpu.state().pc, kProgram + 1);
  EXPECT_EQ(int{cpu.state().a}, 0x02);
}

}  // namespace
