// A MOV to memory reads its destination before writing it. On flat RAM the read
// is invisible (its value is discarded), so the vector oracle cannot see it; a
// bus that records its accesses can. These tests pin that the documented dummy
// read is issued for the MOV-to-memory forms, that MOV dp,dp reads its source
// instead, and that MOVW reads only the low byte of its destination.

#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <vector>

#include <gtest/gtest.h>

#include "snaggletooth/apu/spc700.h"

namespace {

using snaggletooth::Spc700;
using snaggletooth::Spc700State;

// A bus over flat RAM that records the address of every read.
struct RecordingBus {
  std::array<std::uint8_t, 65536> ram{};
  std::vector<std::uint16_t> reads;

  std::uint8_t read(std::uint16_t address) {
    reads.push_back(address);
    return ram[address];
  }
  void write(std::uint16_t address, std::uint8_t value) { ram[address] = value; }

  [[nodiscard]] bool sawRead(std::uint16_t address) const {
    return std::find(reads.begin(), reads.end(), address) != reads.end();
  }
};

// Runs one instruction from `program` (loaded at $0200) on a bus, returning the
// bus so its recorded reads and final RAM can be inspected. `init` seeds the CPU
// registers; its PC is overridden to $0200.
RecordingBus runOne(std::initializer_list<std::uint8_t> program, Spc700State init = {}) {
  RecordingBus bus;
  std::uint16_t addr = 0x0200;
  for (std::uint8_t byte : program) bus.ram[addr++] = byte;
  init.pc = 0x0200;
  Spc700 cpu(init);
  cpu.step(bus);
  return bus;
}

TEST(Spc700DummyRead, MovDpAReadsAndWritesDestination) {
  Spc700State s{}; s.a = 0xAA;
  RecordingBus bus = runOne({0xC4, 0x10}, s);  // MOV $10,A
  EXPECT_TRUE(bus.sawRead(0x0010));
  EXPECT_EQ(bus.ram[0x0010], 0xAA);
}

TEST(Spc700DummyRead, MovDpImmReadsDestination) {
  // The documented case: MOV $FF,#$00 reads $FF (which is what resets T2OUT).
  RecordingBus bus = runOne({0x8F, 0x00, 0xFF});  // MOV $FF,#$00
  EXPECT_TRUE(bus.sawRead(0x00FF));
}

TEST(Spc700DummyRead, MovAbsAReadsDestination) {
  Spc700State s{}; s.a = 0x5A;
  RecordingBus bus = runOne({0xC5, 0x34, 0x12}, s);  // MOV !$1234,A
  EXPECT_TRUE(bus.sawRead(0x1234));
  EXPECT_EQ(bus.ram[0x1234], 0x5A);
}

TEST(Spc700DummyRead, MovIndirectYReadsResolvedDestination) {
  Spc700State s{}; s.a = 0x77; s.y = 0x05;
  RecordingBus bus;
  bus.ram[0x0040] = 0x00; bus.ram[0x0041] = 0x30;  // pointer at $40 -> $3000
  bus.ram[0x0200] = 0xD7; bus.ram[0x0201] = 0x40;  // MOV [$40]+Y,A -> $3005
  s.pc = 0x0200;
  Spc700 cpu(s);
  cpu.step(bus);
  EXPECT_TRUE(bus.sawRead(0x3005));
  EXPECT_EQ(bus.ram[0x3005], 0x77);
}

TEST(Spc700DummyRead, MovXPlusAReadsDestinationThenIncrements) {
  Spc700State s{}; s.a = 0x11; s.x = 0x20;
  RecordingBus bus = runOne({0xAF}, s);  // MOV (X)+,A -> $0020, X++
  EXPECT_TRUE(bus.sawRead(0x0020));
  EXPECT_EQ(bus.ram[0x0020], 0x11);
}

TEST(Spc700DummyRead, MovDpDpReadsSourceNotDestination) {
  // The counterexample: MOV $FF,$00 reads the source ($00), never the
  // destination ($FF) — so it does not reset a timer output at $FF.
  RecordingBus bus = runOne({0xFA, 0x00, 0xFF});  // MOV $FF,$00 (dst $FF, src $00)
  EXPECT_TRUE(bus.sawRead(0x0000));
  EXPECT_FALSE(bus.sawRead(0x00FF));
}

TEST(Spc700DummyRead, MovwReadsOnlyTheLowByteOfDestination) {
  Spc700State s{}; s.a = 0x34; s.y = 0x12;
  RecordingBus bus = runOne({0xDA, 0x30}, s);  // MOVW $30,YA
  EXPECT_TRUE(bus.sawRead(0x0030));    // low byte is read
  EXPECT_FALSE(bus.sawRead(0x0031));   // high byte is not
  EXPECT_EQ(bus.ram[0x0030], 0x34);
  EXPECT_EQ(bus.ram[0x0031], 0x12);
}

}  // namespace
