// Where the cycles go in the 16-bit and single-bit instructions: how a word instruction
// reaches its two direct-page bytes, which of the bit instructions write and which only
// move the carry flag, and how long the chip spends computing inside itself. Pinned on
// hand-written programs rather than on the vector corpus, so the laws stay readable next
// to the code that implements them.

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

TEST(Spc700WordBitCycles, AWordSpendsACycleBetweenItsTwoHalvesUnlessItOnlyCompares) {
  // The arithmetic and move forms read the low byte, spend a cycle inside the chip, and
  // then read the high one. The comparison reads both back to back — it is the one word
  // instruction with no internal cycle at all, and the one cycle shorter for it.
  EXPECT_EQ(shapeRun({0x7A, 0x10}, {}), "rrr.r") << "ADDW YA,dp";
  EXPECT_EQ(shapeRun({0x9A, 0x10}, {}), "rrr.r") << "SUBW YA,dp";
  EXPECT_EQ(shapeRun({0xBA, 0x10}, {}), "rrr.r") << "MOVW YA,dp";
  EXPECT_EQ(shapeRun({0x5A, 0x10}, {}), "rrrr") << "CMPW YA,dp";
}

TEST(Spc700WordBitCycles, TheHighByteOfAWordWrapsInsideTheDirectPage) {
  // One offset names both bytes, and the second address is the first plus one *within
  // the page*: a word at $FF takes its high byte from the page base, not from the page
  // above it.
  RecordingFlatBus bus = busWith({0xBA, 0xFF});  // MOVW YA,$FF
  bus.ram[0x00FF] = 0x34;
  bus.ram[0x0000] = 0x12;
  Spc700 cpu(Spc700State{.pc = kProgram});
  const std::vector<CycleEvent> trace = traceOne(bus, cpu);

  ASSERT_EQ(trace.size(), 5u);
  EXPECT_EQ(*trace[2].address, 0x00FF) << "the low byte";
  EXPECT_EQ(*trace[4].address, 0x0000) << "and the high byte, wrapped inside the page";
  EXPECT_EQ(int{cpu.state().a}, 0x34);
  EXPECT_EQ(int{cpu.state().y}, 0x12);
}

TEST(Spc700WordBitCycles, TheWordIncrementWritesEachHalfBeforeItReadsTheNext) {
  // INCW does not read the whole word and then write it back. It reads the low byte,
  // writes it, reads the high byte, and writes that — four cycles reaching two
  // addresses in turn, which is visible on any bus whose reads or writes have effects.
  RecordingFlatBus bus = busWith({0x3A, 0x10});  // INCW $10
  bus.ram[0x0010] = 0xFF;
  bus.ram[0x0011] = 0x01;
  Spc700 cpu(Spc700State{.pc = kProgram});
  const std::vector<CycleEvent> trace = traceOne(bus, cpu);

  EXPECT_EQ(shapeOf(trace), "rrrwrw");
  ASSERT_EQ(trace.size(), 6u);
  EXPECT_EQ(*trace[2].address, 0x0010) << "the low byte is read";
  EXPECT_EQ(*trace[3].address, 0x0010) << "and written back before the high one is read";
  EXPECT_EQ(int{*trace[3].value}, 0x00);
  EXPECT_EQ(*trace[4].address, 0x0011);
  EXPECT_EQ(*trace[5].address, 0x0011);
  EXPECT_EQ(int{*trace[5].value}, 0x02) << "the low byte wrapped, so the high one steps";
  EXPECT_EQ(int{cpu.state().psw}, 0) << "the flags are those of the whole word";
}

TEST(Spc700WordBitCycles, TheWordDecrementBorrowsOnlyWhenTheLowByteWraps) {
  RecordingFlatBus borrowing = busWith({0x1A, 0x10});  // DECW $10
  borrowing.ram[0x0010] = 0x00;
  borrowing.ram[0x0011] = 0x02;
  Spc700 borrowCpu(Spc700State{.pc = kProgram});
  traceOne(borrowing, borrowCpu);
  EXPECT_EQ(int{borrowing.ram[0x0010]}, 0xFF);
  EXPECT_EQ(int{borrowing.ram[0x0011]}, 0x01) << "the high byte follows the borrow";

  RecordingFlatBus plain = busWith({0x1A, 0x10});
  plain.ram[0x0010] = 0x02;
  plain.ram[0x0011] = 0x02;
  Spc700 plainCpu(Spc700State{.pc = kProgram});
  traceOne(plain, plainCpu);
  EXPECT_EQ(int{plain.ram[0x0010]}, 0x01);
  EXPECT_EQ(int{plain.ram[0x0011]}, 0x02) << "and stays put when there is none";
}

TEST(Spc700WordBitCycles, TheWordStoreReadsOnlyItsLowByte) {
  // MOVW dp,YA reads the byte it is about to overwrite — once, at the low address — and
  // then writes both halves. The high byte is never read, so a store through it clears a
  // register that clears when read only if that register is the low one.
  RecordingFlatBus bus = busWith({0xDA, 0x30});  // MOVW $30,YA
  Spc700 cpu(Spc700State{.pc = kProgram, .a = 0x34, .y = 0x12});
  const std::vector<CycleEvent> trace = traceOne(bus, cpu);

  EXPECT_EQ(shapeOf(trace), "rrrww");
  ASSERT_EQ(trace.size(), 5u);
  EXPECT_EQ(*trace[2].address, 0x0030) << "the low byte is read";
  EXPECT_EQ(*trace[3].address, 0x0030) << "then written";
  EXPECT_EQ(int{*trace[3].value}, 0x34) << "A is the low half of the pair";
  EXPECT_EQ(*trace[4].address, 0x0031) << "then the high byte, never read";
  EXPECT_EQ(int{*trace[4].value}, 0x12);
}

TEST(Spc700WordBitCycles, SettingOneBitRunsTheReadModifyWriteSeat) {
  // SET1 and CLR1 run the same four cycles as an increment in the direct page: the byte
  // is read, and it goes back changed on the next cycle. Neither touches a flag.
  RecordingFlatBus bus = busWith({0xA2, 0x10});  // SET1 $10.5
  bus.ram[0x0010] = 0x01;
  Spc700 cpu(Spc700State{.pc = kProgram});
  const std::vector<CycleEvent> trace = traceOne(bus, cpu);

  EXPECT_EQ(shapeOf(trace), "rrrw");
  ASSERT_EQ(trace.size(), 4u);
  EXPECT_EQ(*trace[2].address, 0x0010);
  EXPECT_EQ(*trace[3].address, 0x0010);
  EXPECT_EQ(int{*trace[3].value}, 0x21) << "bit 5, from the opcode's top three bits";
  EXPECT_EQ(int{cpu.state().psw}, 0) << "no flag moves";

  EXPECT_EQ(shapeRun({0x12, 0x10}, {}), "rrrw") << "CLR1 $10.0";
  RecordingFlatBus clearing = busWith({0xB2, 0x10});  // CLR1 $10.5
  clearing.ram[0x0010] = 0xFF;
  Spc700 clearCpu(Spc700State{.pc = kProgram});
  traceOne(clearing, clearCpu);
  EXPECT_EQ(int{clearing.ram[0x0010]}, 0xDF);
}

TEST(Spc700WordBitCycles, TestAndSetReadsItsByteTwice) {
  // TSET1 and TCLR1 are six cycles, and the fifth is a second read of the address the
  // fourth already read — not a cycle inside the chip. Both reads are real, so a
  // register that clears when read is reached twice.
  RecordingFlatBus bus = busWith({0x0E, 0x00, 0x03});  // TSET1 !$0300
  bus.ram[0x0300] = 0x0F;
  Spc700 cpu(Spc700State{.pc = kProgram, .a = 0x30});
  const std::vector<CycleEvent> trace = traceOne(bus, cpu);

  EXPECT_EQ(shapeOf(trace), "rrrrrw");
  ASSERT_EQ(trace.size(), 6u);
  EXPECT_EQ(*trace[3].address, 0x0300) << "the byte is read";
  EXPECT_EQ(*trace[4].address, 0x0300) << "and then read again";
  EXPECT_EQ(*trace[5].address, 0x0300);
  EXPECT_EQ(int{*trace[5].value}, 0x3F) << "A's bits are set in it";
  EXPECT_EQ(int{cpu.state().a}, 0x30) << "A itself is untouched";
  EXPECT_EQ(int{cpu.state().psw & snaggletooth::kFlagZ}, 0)
      << "the flags are those of A minus the byte";

  EXPECT_EQ(shapeRun({0x4E, 0x00, 0x03}, {}), "rrrrrw") << "TCLR1 !abs";
}

TEST(Spc700WordBitCycles, TheCarryBitFormsDifferByOneInternalCycle) {
  // AND1 and MOV1 C,m.b settle as the byte arrives. OR1 and EOR1 pay one more cycle
  // inside the chip after it, reaching memory not at all.
  EXPECT_EQ(shapeRun({0x4A, 0x00, 0x03}, {}), "rrrr") << "AND1 C,m.b";
  EXPECT_EQ(shapeRun({0x6A, 0x00, 0x03}, {}), "rrrr") << "AND1 C,/m.b";
  EXPECT_EQ(shapeRun({0xAA, 0x00, 0x03}, {}), "rrrr") << "MOV1 C,m.b";
  EXPECT_EQ(shapeRun({0x0A, 0x00, 0x03}, {}), "rrrr.") << "OR1 C,m.b";
  EXPECT_EQ(shapeRun({0x2A, 0x00, 0x03}, {}), "rrrr.") << "OR1 C,/m.b";
  EXPECT_EQ(shapeRun({0x8A, 0x00, 0x03}, {}), "rrrr.") << "EOR1 C,m.b";
}

TEST(Spc700WordBitCycles, ABitOperandCarriesItsAddressBelowItsBitIndex) {
  // The two operand bytes are one 16-bit value: the address in the low 13 bits, the bit
  // index in the three above them. Only the address reaches memory.
  RecordingFlatBus bus = busWith({0xAA, 0x00, 0x63});  // MOV1 C,$0300.3
  bus.ram[0x0300] = 0x08;
  Spc700 cpu(Spc700State{.pc = kProgram});
  const std::vector<CycleEvent> trace = traceOne(bus, cpu);

  ASSERT_EQ(trace.size(), 4u);
  EXPECT_EQ(*trace[3].address, 0x0300) << "the low 13 bits are the address";
  EXPECT_EQ(int{cpu.state().psw & snaggletooth::kFlagC}, int{snaggletooth::kFlagC})
      << "and bit 3 of the byte is the bit that reached the carry flag";

  bus.ram[0x0300] = 0xF7;  // every bit but the third
  Spc700 clear(Spc700State{.pc = kProgram, .psw = snaggletooth::kFlagC});
  RecordingFlatBus again = bus;
  again.events.clear();
  traceOne(again, clear);
  EXPECT_EQ(int{clear.state().psw & snaggletooth::kFlagC}, 0);
}

TEST(Spc700WordBitCycles, TheBitStoreSpendsACycleBeforeItWrites) {
  // NOT1 flips its bit and writes on the cycle after the read. MOV1 m.b,C writes the
  // carry flag into the byte instead, and pays a cycle inside the chip in between.
  RecordingFlatBus flipping = busWith({0xEA, 0x00, 0x63});  // NOT1 $0300.3
  flipping.ram[0x0300] = 0x08;
  Spc700 flipCpu(Spc700State{.pc = kProgram});
  const std::vector<CycleEvent> flipTrace = traceOne(flipping, flipCpu);
  EXPECT_EQ(shapeOf(flipTrace), "rrrrw");
  EXPECT_EQ(int{flipping.ram[0x0300]}, 0x00);

  RecordingFlatBus storing = busWith({0xCA, 0x00, 0x63});  // MOV1 $0300.3,C
  Spc700 storeCpu(Spc700State{.pc = kProgram, .psw = snaggletooth::kFlagC});
  const std::vector<CycleEvent> storeTrace = traceOne(storing, storeCpu);
  EXPECT_EQ(shapeOf(storeTrace), "rrrr.w");
  ASSERT_EQ(storeTrace.size(), 6u);
  EXPECT_EQ(*storeTrace[3].address, 0x0300) << "the byte is read";
  EXPECT_EQ(*storeTrace[5].address, 0x0300) << "and written two cycles later";
  EXPECT_EQ(int{*storeTrace[5].value}, 0x08);
}

TEST(Spc700WordBitCycles, TheMultiplyAndTheDivideRunEntirelyInsideTheChip) {
  // Both reach memory exactly twice — the opcode, and the byte after it that every
  // one-byte instruction reads and discards — and spend every remaining cycle
  // computing. They are the two longest instructions the CPU has.
  RecordingFlatBus bus = busWith({0xCF});  // MUL YA
  Spc700 cpu(Spc700State{.pc = kProgram, .a = 0x03, .y = 0x02});
  const std::vector<CycleEvent> trace = traceOne(bus, cpu);

  EXPECT_EQ(shapeOf(trace), "rr.......");
  EXPECT_EQ(*trace[1].address, kProgram + 1);
  EXPECT_EQ(cpu.state().pc, kProgram + 1) << "the discarded byte is not stepped over";
  EXPECT_EQ(int{cpu.state().a}, 0x06);
  EXPECT_EQ(int{cpu.state().y}, 0x00);

  EXPECT_EQ(shapeRun({0x9E}, {.a = 0x11, .x = 0x05}), "rr..........") << "DIV YA,X";
}

TEST(Spc700WordBitCycles, TheDecimalAdjustsSpendOneCycleInsideTheChip) {
  RecordingFlatBus bus = busWith({0xDF});  // DAA A
  Spc700 cpu(Spc700State{.pc = kProgram, .a = 0x0B});
  const std::vector<CycleEvent> trace = traceOne(bus, cpu);

  EXPECT_EQ(shapeOf(trace), "rr.");
  EXPECT_EQ(int{cpu.state().a}, 0x11);
  EXPECT_EQ(shapeRun({0xBE}, {.a = 0x40, .psw = snaggletooth::kFlagC}), "rr.") << "DAS A";
}

}  // namespace
