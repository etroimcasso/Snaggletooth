// A MOV to memory reads its destination before writing it. On flat RAM the read is
// invisible — its value is discarded — so a comparison of final state cannot see it; a
// bus that records its accesses can. These tests pin which cycle that read falls on for
// the MOV-to-memory forms, that the auto-incrementing form makes no such read at all,
// that MOV dp,dp reads its source instead, and that MOVW reads only the low byte of its
// destination. Each read matters on a bus where reading has an effect: it is what makes
// a store to a timer output clear it.

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <vector>

#include <gtest/gtest.h>

#include "snaggletooth/apu/spc700.h"
#include "vector_harness.h"

namespace {

using snaggletooth::Spc700;
using snaggletooth::Spc700State;
using snaggletooth::test::CycleEvent;
using snaggletooth::test::RecordingFlatBus;

constexpr std::uint16_t kProgram = 0x0200;

// Runs one instruction from `program` (loaded at $0200) a cycle at a time, returning
// the trace of what each cycle did — a read, a write, or nothing at all. `init` seeds
// the CPU registers; its PC is overridden to $0200.
std::vector<CycleEvent> traceOne(RecordingFlatBus& bus, Spc700State init) {
  init.pc = kProgram;
  Spc700 cpu(init);

  std::vector<CycleEvent> trace;
  do {
    const std::size_t narrated = bus.events.size();
    cpu.stepCycle(bus);
    trace.push_back(bus.events.size() == narrated ? CycleEvent{} : bus.events.back());
  } while (!cpu.atInstructionBoundary());
  return trace;
}

// A bus holding `program` at $0200.
RecordingFlatBus busWith(std::initializer_list<std::uint8_t> program) {
  RecordingFlatBus bus;
  std::uint16_t address = kProgram;
  for (std::uint8_t byte : program) bus.ram[address++] = byte;
  return bus;
}

// Whether any cycle of the trace read that address.
bool sawRead(const std::vector<CycleEvent>& trace, std::uint16_t address) {
  for (const CycleEvent& cycle : trace) {
    if (cycle.kind == CycleEvent::Kind::Read && cycle.address == address) return true;
  }
  return false;
}

TEST(Spc700DummyRead, MovDpAReadsItsDestinationOnTheCycleBeforeItWrites) {
  RecordingFlatBus bus = busWith({0xC4, 0x10});  // MOV $10,A
  const std::vector<CycleEvent> trace = traceOne(bus, Spc700State{.a = 0xAA});

  ASSERT_EQ(trace.size(), 4u);
  EXPECT_EQ(trace[2].kind, CycleEvent::Kind::Read);
  EXPECT_EQ(*trace[2].address, 0x0010);
  EXPECT_EQ(trace[3].kind, CycleEvent::Kind::Write);
  EXPECT_EQ(*trace[3].address, 0x0010);
  EXPECT_EQ(int{bus.ram[0x0010]}, 0xAA);
}

TEST(Spc700DummyRead, MovDpImmReadsItsDestinationAfterBothOperands) {
  // The documented case: MOV $FF,#$00 reads $FF, which is what resets a timer output
  // there. The read is the fourth cycle — after the immediate and the offset.
  RecordingFlatBus bus = busWith({0x8F, 0x00, 0xFF});  // MOV $FF,#$00
  const std::vector<CycleEvent> trace = traceOne(bus, Spc700State{});

  ASSERT_EQ(trace.size(), 5u);
  EXPECT_EQ(trace[3].kind, CycleEvent::Kind::Read);
  EXPECT_EQ(*trace[3].address, 0x00FF);
  EXPECT_EQ(*trace[4].address, 0x00FF);
}

TEST(Spc700DummyRead, MovAbsAReadsItsDestinationOnceItsAddressIsWhole) {
  RecordingFlatBus bus = busWith({0xC5, 0x34, 0x12});  // MOV !$1234,A
  const std::vector<CycleEvent> trace = traceOne(bus, Spc700State{.a = 0x5A});

  ASSERT_EQ(trace.size(), 5u);
  EXPECT_EQ(trace[3].kind, CycleEvent::Kind::Read);
  EXPECT_EQ(*trace[3].address, 0x1234);
  EXPECT_EQ(int{bus.ram[0x1234]}, 0x5A);
}

TEST(Spc700DummyRead, MovIndirectYReadsTheDestinationItsPointerResolved) {
  // The write form of [dp]+Y reads its pointer first and spends its internal cycle
  // after it, so the destination read is the sixth of seven cycles.
  RecordingFlatBus bus = busWith({0xD7, 0x40});  // MOV [$40]+Y,A -> $3005
  bus.ram[0x0040] = 0x00;
  bus.ram[0x0041] = 0x30;
  const std::vector<CycleEvent> trace =
      traceOne(bus, Spc700State{.a = 0x77, .y = 0x05});

  ASSERT_EQ(trace.size(), 7u);
  EXPECT_EQ(trace[5].kind, CycleEvent::Kind::Read);
  EXPECT_EQ(*trace[5].address, 0x3005);
  EXPECT_EQ(*trace[6].address, 0x3005);
  EXPECT_EQ(int{bus.ram[0x3005]}, 0x77);
}

TEST(Spc700DummyRead, MovXPlusADoesNotReadItsDestination) {
  // The exception among the stores: the auto-incrementing form spends that cycle
  // inside the chip instead, so a store through it to a register that clears when
  // read leaves that register alone.
  RecordingFlatBus bus = busWith({0xAF});  // MOV (X)+,A -> $0020, X++
  const std::vector<CycleEvent> trace =
      traceOne(bus, Spc700State{.a = 0x11, .x = 0x20});

  ASSERT_EQ(trace.size(), 4u);
  EXPECT_EQ(trace[2].kind, CycleEvent::Kind::Wait) << "where the other stores read";
  EXPECT_FALSE(sawRead(trace, 0x0020));
  EXPECT_EQ(*trace[3].address, 0x0020);
  EXPECT_EQ(int{bus.ram[0x0020]}, 0x11);
}

TEST(Spc700DummyRead, MovDpDpReadsItsSourceAndNeverItsDestination) {
  // The counterexample: MOV $FF,$00 reads the source ($00), never the destination
  // ($FF) — so it does not reset a timer output at $FF.
  RecordingFlatBus bus = busWith({0xFA, 0x00, 0xFF});  // MOV $FF,$00
  const std::vector<CycleEvent> trace = traceOne(bus, Spc700State{});

  ASSERT_EQ(trace.size(), 5u);
  EXPECT_EQ(*trace[2].address, 0x0000) << "the source, read as soon as its offset lands";
  EXPECT_FALSE(sawRead(trace, 0x00FF));
  EXPECT_EQ(*trace[4].address, 0x00FF) << "which is written without being read";
}

TEST(Spc700DummyRead, MovwReadsOnlyTheLowByteOfItsDestination) {
  RecordingFlatBus bus = busWith({0xDA, 0x30});  // MOVW $30,YA
  const std::vector<CycleEvent> trace =
      traceOne(bus, Spc700State{.a = 0x34, .y = 0x12});

  ASSERT_EQ(trace.size(), 5u);
  EXPECT_EQ(trace[2].kind, CycleEvent::Kind::Read);
  EXPECT_EQ(*trace[2].address, 0x0030);
  EXPECT_FALSE(sawRead(trace, 0x0031)) << "the high byte is written, never read";
  EXPECT_EQ(int{bus.ram[0x0030]}, 0x34);
  EXPECT_EQ(int{bus.ram[0x0031]}, 0x12);
}

}  // namespace
