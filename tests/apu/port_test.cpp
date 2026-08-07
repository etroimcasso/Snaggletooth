// The comm ports: the eight latches behind $F4-$F7 (the host and SPC700 sides),
// and the CONTROL-register input-port clears.
//
// The ports are the other side of a bus: writePort/readPort is the host face a
// future 5A22 drives at $2140-$2143. The SPC700 side is exercised by stepping
// real MOV instructions through the overlay. Assertions are derived from the
// SNESdev SPC700 register and boot documentation.

#include <cstdint>
#include <initializer_list>

#include <gtest/gtest.h>

#include "snaggletooth/apu/apu.h"

namespace {

using snaggletooth::Apu;

// Loads `code` at $0200 and points the CPU there without running it.
void load(Apu& apu, std::initializer_list<std::uint8_t> code) {
  std::uint16_t addr = 0x0200;
  for (std::uint8_t byte : code) apu.writeRam(addr++, byte);
  apu.setPc(0x0200);
}

// Loads and runs `instructions` of `code` on a fresh machine.
Apu run(std::initializer_list<std::uint8_t> code, int instructions) {
  Apu apu;
  load(apu, code);
  for (int i = 0; i < instructions; ++i) apu.step();
  return apu;
}

// ── The two sides of the latches ────────────────────────────────────────────

TEST(ApuPorts, PowerOnPostsReadyBytes) {
  // The ready state posts $AA to output port 0 and $BB to output port 1.
  Apu apu;
  EXPECT_EQ(apu.readPort(0), 0xAA);
  EXPECT_EQ(apu.readPort(1), 0xBB);
}

TEST(ApuPorts, SpcWriteSetsOutputAndHostReads) {
  // An SPC-side write to a port sets the output latch the host reads.
  Apu apu = run({0x8F, 0x11, 0xF4}, 1);  // MOV $F4,#$11
  EXPECT_EQ(apu.readPort(0), 0x11);
  EXPECT_EQ(apu.state().outputPorts[0], 0x11);
}

TEST(ApuPorts, HostWriteSetsInputAndSpcReads) {
  // A host-side write sets the input latch the SPC700 reads.
  Apu apu;
  apu.writePort(1, 0x22);
  load(apu, {0xE4, 0xF5});  // MOV A,$F5
  apu.step();
  EXPECT_EQ(apu.state().cpu.a, 0x22);
}

TEST(ApuPorts, WritingOutputDoesNotDisturbInput) {
  // Writing an output port never changes the corresponding input port.
  Apu apu;
  apu.writePort(0, 0x33);
  load(apu, {0x8F, 0x44, 0xF4,   // MOV $F4,#$44  (write output 0)
             0xE4, 0xF4});       // MOV A,$F4     (read input 0)
  apu.step();
  EXPECT_EQ(apu.readPort(0), 0x44);
  apu.step();
  EXPECT_EQ(apu.state().cpu.a, 0x33);
}

TEST(ApuPorts, WritingInputDoesNotDisturbOutput) {
  // A host input write never changes the corresponding output latch.
  Apu apu;
  load(apu, {0x8F, 0x55, 0xF4,   // MOV $F4,#$55  (write output 0)
             0xE4, 0xF4});       // MOV A,$F4     (read input 0)
  apu.step();
  apu.writePort(0, 0x66);
  apu.step();
  EXPECT_EQ(apu.state().cpu.a, 0x66);
  EXPECT_EQ(apu.readPort(0), 0x55);
}

// ── CONTROL-register input-port clears ──────────────────────────────────────

TEST(ApuPorts, ControlBit4ClearsInputPorts0And1) {
  Apu apu;
  apu.writePort(0, 0xA1); apu.writePort(1, 0xB2);
  apu.writePort(2, 0xC3); apu.writePort(3, 0xD4);
  load(apu, {0x8F, 0x10, 0xF1});  // MOV $F1,#$10
  apu.step();
  EXPECT_EQ(apu.state().inputPorts[0], 0x00);
  EXPECT_EQ(apu.state().inputPorts[1], 0x00);
  EXPECT_EQ(apu.state().inputPorts[2], 0xC3);
  EXPECT_EQ(apu.state().inputPorts[3], 0xD4);
}

TEST(ApuPorts, ControlBit5ClearsInputPorts2And3) {
  Apu apu;
  apu.writePort(0, 0xA1); apu.writePort(1, 0xB2);
  apu.writePort(2, 0xC3); apu.writePort(3, 0xD4);
  load(apu, {0x8F, 0x20, 0xF1});  // MOV $F1,#$20
  apu.step();
  EXPECT_EQ(apu.state().inputPorts[2], 0x00);
  EXPECT_EQ(apu.state().inputPorts[3], 0x00);
  EXPECT_EQ(apu.state().inputPorts[0], 0xA1);
  EXPECT_EQ(apu.state().inputPorts[1], 0xB2);
}

TEST(ApuPorts, ControlClearHappensOnEveryWriteNotJustTransition) {
  // The zeroing occurs whenever 1 is written, not on a 0->1 transition: a second
  // write with the bit still set clears a refilled port.
  Apu apu;
  apu.writePort(0, 0x11);
  load(apu, {0x8F, 0x10, 0xF1,   // MOV $F1,#$10  (clears input 0)
             0x8F, 0x10, 0xF1});  // MOV $F1,#$10  (bit still set)
  apu.step();
  EXPECT_EQ(apu.state().inputPorts[0], 0x00);
  apu.writePort(0, 0x99);         // host refills input 0
  apu.step();
  EXPECT_EQ(apu.state().inputPorts[0], 0x00);
}

TEST(ApuPorts, ControlClearLeavesOutputPortsAlone) {
  Apu apu;
  load(apu, {0x8F, 0x77, 0xF4,   // MOV $F4,#$77  (output 0 := $77)
             0x8F, 0x30, 0xF1});  // MOV $F1,#$30  (bits 4+5 clear inputs)
  apu.step();
  apu.writePort(0, 0x88);
  apu.step();
  EXPECT_EQ(apu.readPort(0), 0x77);
  EXPECT_EQ(apu.state().inputPorts[0], 0x00);
}

TEST(ApuPorts, ControlWithoutClearBitsLeavesInputsIntact) {
  Apu apu;
  apu.writePort(0, 0x5A);
  load(apu, {0x8F, 0x01, 0xF1});  // MOV $F1,#$01  (a timer enable, no clear bits)
  apu.step();
  EXPECT_EQ(apu.state().inputPorts[0], 0x5A);
}

TEST(ApuPorts, ControlReadsBackZero) {
  // CONTROL is write-only.
  Apu apu = run({0x8F, 0x01, 0xF1,   // MOV $F1,#$01
                 0xE4, 0xF1}, 2);    // MOV A,$F1
  EXPECT_EQ(apu.state().cpu.a, 0x00);
  EXPECT_EQ(apu.readRam(0x00F1), 0x01);  // the write still landed in RAM
}

}  // namespace
