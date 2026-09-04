// The controller ports: a pad presented to the machine and read by a program the
// two ways the console offers — the auto-read into $4218-$421F once a frame, and
// the serial ports at $4016/$4017 strobed and clocked by hand. The bit order, the
// identity bits and the padding are the documented ones; the strobe held high and
// the auto-read's use of the same clock lines are pinned as the documentation
// states them. Programs run from the cartridge and store what they read into work
// RAM; the auto-read is observed on the machine state after a run of exact length.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "gtest/gtest.h"
#include "snaggletooth/snes/snes.h"

namespace snaggletooth {
namespace {

constexpr std::uint32_t kLine = 1364u;
constexpr std::uint32_t kVblankLine = 225u;

// A machine whose CPU is halted with the counters at the frame origin, so run()
// advances it in exact six-cycle idle steps, with the auto-read enabled or not.
Snes stoppedMachine(bool autoRead) {
  std::vector<std::uint8_t> rom(0x8000u, 0x00u);
  rom[0] = 0xDBu;         // STP
  rom[0x7FFCu] = 0x00u;   // reset -> $8000
  rom[0x7FFDu] = 0x80u;
  Snes m(SnesConfig{.rom = rom});
  m.step();
  SnesState s = m.state();
  s.master = 0; s.consumed = 0; s.apuPhase = 0;
  s.hpos = 0; s.vpos = 0; s.field = 0;
  s.nmitimen = autoRead ? 0x01u : 0x00u;
  m.restore(s);
  return m;
}

// A machine whose cartridge runs `program` from $8000, in emulation mode with an
// eight-bit accumulator, as the console powers on.
Snes machineWith(const std::vector<std::uint8_t>& program) {
  std::vector<std::uint8_t> rom = program;
  rom.resize(0x8000u, 0x00u);
  rom[0x7FFCu] = 0x00u;   // reset -> $8000
  rom[0x7FFDu] = 0x80u;
  return Snes(SnesConfig{.rom = rom});
}

void runToStop(Snes& m, int cap = 400'000) {
  for (int i = 0; i < cap && m.state().cpu.run == CpuRunState::Running; ++i) m.step();
  ASSERT_NE(m.state().cpu.run, CpuRunState::Running) << "the program did not reach its STP";
}

// A program that strobes the pads, then reads port 1's line `reads` times into
// $0010 onward, then stops.
std::vector<std::uint8_t> strobeAndRead(std::uint16_t port, int reads) {
  std::vector<std::uint8_t> p = {0xA9u, 0x01u,          // LDA #$01
                                 0x8Du, 0x16u, 0x40u,   // STA $4016   strobe high
                                 0x9Cu, 0x16u, 0x40u};  // STZ $4016   strobe low: latched
  for (int i = 0; i < reads; ++i) {
    p.insert(p.end(), {0xADu, static_cast<std::uint8_t>(port & 0xFFu),
                       static_cast<std::uint8_t>(port >> 8),                       // LDA $401x
                       0x8Du, static_cast<std::uint8_t>(0x10u + i), 0x00u});     // STA $00xx
  }
  p.push_back(0xDBu);  // STP
  return p;
}

// ---- the value ------------------------------------------------------------------

TEST(SnesController, TheBitsFollowTheWireOrder) {
  EXPECT_EQ(Joypad{}.bits(), 0x0000u);
  EXPECT_EQ((Joypad{.b = true}).bits(), 0x8000u) << "B is the first bit on the wire";
  EXPECT_EQ((Joypad{.start = true}).bits(), 0x1000u);
  EXPECT_EQ((Joypad{.right = true}).bits(), 0x0100u);
  EXPECT_EQ((Joypad{.a = true}).bits(), 0x0080u);
  EXPECT_EQ((Joypad{.r = true}).bits(), 0x0010u) << "R is the twelfth";
  const Joypad all{.b = true, .y = true, .select = true, .start = true, .up = true, .down = true,
                   .left = true, .right = true, .a = true, .x = true, .l = true, .r = true};
  EXPECT_EQ(all.bits(), 0xFFF0u) << "the identity bits of a standard pad are zero";
}

// ---- the auto-read ----------------------------------------------------------------

TEST(SnesController, AnEmptyPortReadsZeroThroughTheAutoRead) {
  Snes m = stoppedMachine(true);
  m.run(2u * 262u * kLine);
  for (std::size_t i = 0; i < 8; ++i) EXPECT_EQ(m.state().joy[i], 0u) << i;
  EXPECT_FALSE(m.joypad(JoypadPort::One).has_value());
  EXPECT_FALSE(m.joypad(JoypadPort::Two).has_value());
}

TEST(SnesController, TheAutoReadLaysAPadOutAsDocumented) {
  Snes m = stoppedMachine(true);
  m.setJoypad(JoypadPort::One, Joypad{.start = true, .a = true});
  m.setJoypad(JoypadPort::Two, Joypad{.b = true, .r = true});
  m.run(230u * kLine);  // past the first frame's window
  EXPECT_EQ(m.state().joy[0], 0x80u) << "$4218: A in bit 7";
  EXPECT_EQ(m.state().joy[1], 0x10u) << "$4219: Start in bit 4";
  EXPECT_EQ(m.state().joy[2], 0x10u) << "$421A: R in bit 4";
  EXPECT_EQ(m.state().joy[3], 0x80u) << "$421B: B in bit 7";
  for (std::size_t i = 4; i < 8; ++i) EXPECT_EQ(m.state().joy[i], 0u) << "nothing on the second lines";
}

TEST(SnesController, TheResultLandsAsTheWindowEnds) {
  Snes m = stoppedMachine(true);
  m.setJoypad(JoypadPort::One, Joypad{.start = true});
  m.run(kVblankLine * kLine + 1200u);  // inside the 4224-cycle window
  EXPECT_NE(m.state().autoJoyClocks, 0u);
  EXPECT_EQ(m.state().joy[1], 0u) << "the registers hold the previous result while the read is busy";
  m.run(4u * kLine);
  EXPECT_EQ(m.state().autoJoyClocks, 0u);
  EXPECT_EQ(m.state().joy[1], 0x10u);
}

TEST(SnesController, NoAutoReadRunsWhenItIsDisabled) {
  Snes m = stoppedMachine(false);
  m.setJoypad(JoypadPort::One, Joypad{.start = true});
  m.run(2u * 262u * kLine);
  EXPECT_EQ(m.state().autoJoyClocks, 0u);
  for (std::size_t i = 0; i < 8; ++i) EXPECT_EQ(m.state().joy[i], 0u) << i;
}

TEST(SnesController, APadChangedBetweenFramesIsReadTheNextFrame) {
  Snes m = stoppedMachine(true);
  m.setJoypad(JoypadPort::One, Joypad{.start = true});
  m.run(230u * kLine);
  EXPECT_EQ(m.state().joy[1], 0x10u);
  m.setJoypad(JoypadPort::One, Joypad{.b = true});
  m.run(262u * kLine);  // the same point in the next frame
  EXPECT_EQ(m.state().joy[1], 0x80u);
  EXPECT_EQ(m.state().joy[0], 0x00u);
  m.setJoypad(JoypadPort::One, std::nullopt);
  m.run(262u * kLine);
  EXPECT_EQ(m.state().joy[1], 0x00u) << "the pad unplugged reads as no controller";
}

// ---- the serial ports -------------------------------------------------------------

TEST(SnesController, ASerialReadShiftsTheBitsInTheDocumentedOrder) {
  Snes m = machineWith(strobeAndRead(0x4016u, 16));
  m.setJoypad(JoypadPort::One, Joypad{.y = true, .right = true, .x = true});
  runToStop(m);
  // 1st B, 2nd Y, 3rd Select, 4th Start, 5th Up, 6th Down, 7th Left, 8th Right,
  // 9th A, 10th X, 11th L, 12th R, then four identity bits.
  const int expected[16] = {0, 1, 0, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0};
  for (int i = 0; i < 16; ++i) {
    const std::uint8_t v = m.state().wram[0x10u + static_cast<std::size_t>(i)];
    EXPECT_EQ(v & 1u, expected[i]) << "bit " << (i + 1);
    EXPECT_EQ(v & 2u, 0u) << "nothing is on the port's second line";
  }
}

TEST(SnesController, PastSixteenBitsAPadReturnsItsPadding) {
  Snes m = machineWith(strobeAndRead(0x4016u, 18));
  m.setJoypad(JoypadPort::One, Joypad{});
  runToStop(m);
  for (int i = 0; i < 16; ++i) EXPECT_EQ(m.state().wram[0x10u + static_cast<std::size_t>(i)] & 1u, 0u) << i;
  EXPECT_EQ(m.state().wram[0x20u] & 1u, 1u) << "the seventeenth bit";
  EXPECT_EQ(m.state().wram[0x21u] & 1u, 1u) << "and every one after";
}

TEST(SnesController, AnEmptyPortReadsZeroOnEveryClock) {
  Snes m = machineWith(strobeAndRead(0x4016u, 18));
  runToStop(m);
  for (int i = 0; i < 18; ++i) {
    EXPECT_EQ(m.state().wram[0x10u + static_cast<std::size_t>(i)] & 3u, 0u) << i;
  }
}

TEST(SnesController, PortTwoReadsOnItsOwnRegister) {
  Snes m = machineWith(strobeAndRead(0x4017u, 2));
  m.setJoypad(JoypadPort::Two, Joypad{.b = true});
  runToStop(m);
  EXPECT_EQ(m.state().wram[0x10u] & 1u, 1u) << "B first";
  EXPECT_EQ(m.state().wram[0x11u] & 1u, 0u) << "then Y";
  EXPECT_EQ(m.state().wram[0x10u] & 0x1Cu, 0x1Cu) << "$4017 bits 4-2 are wired low and read as ones";
  EXPECT_EQ(m.state().wram[0x10u] & 0x02u, 0u);
}

TEST(SnesController, PortOneIsNotPortTwo) {
  Snes m = machineWith(strobeAndRead(0x4016u, 1));
  m.setJoypad(JoypadPort::Two, Joypad{.b = true});
  runToStop(m);
  EXPECT_EQ(m.state().wram[0x10u] & 1u, 0u) << "a pad in port 2 is not on port 1's line";
}

TEST(SnesController, TheStrobeHeldHighRepeatsTheFirstBit) {
  // Strobe high and never lowered, then four reads.
  const std::vector<std::uint8_t> program = {
      0xA9u, 0x01u, 0x8Du, 0x16u, 0x40u,  // LDA #$01 / STA $4016
      0xADu, 0x16u, 0x40u, 0x8Du, 0x10u, 0x00u,
      0xADu, 0x16u, 0x40u, 0x8Du, 0x11u, 0x00u,
      0xADu, 0x16u, 0x40u, 0x8Du, 0x12u, 0x00u,
      0xADu, 0x16u, 0x40u, 0x8Du, 0x13u, 0x00u,
      0xDBu};
  {
    Snes m = machineWith(program);
    m.setJoypad(JoypadPort::One, Joypad{.b = true});
    runToStop(m);
    for (std::size_t i = 0x10; i < 0x14; ++i) EXPECT_EQ(m.state().wram[i] & 1u, 1u) << "B, every time";
  }
  {
    Snes m = machineWith(program);
    m.setJoypad(JoypadPort::One, Joypad{.y = true});
    runToStop(m);
    for (std::size_t i = 0x10; i < 0x14; ++i) EXPECT_EQ(m.state().wram[i] & 1u, 0u) << "never Y";
  }
}

// ---- the two paths share the lines --------------------------------------------------

// Enables the auto-read, waits for a vertical blank and for the read to finish,
// then reads the serial port without strobing, then strobes and reads again.
const std::vector<std::uint8_t> kReadAfterAutoRead = {
    0xA9u, 0x01u, 0x8Du, 0x00u, 0x42u,  // $8000 LDA #$01 / STA $4200   auto-read on
    0xADu, 0x12u, 0x42u,                // $8005 LDA $4212
    0x10u, 0xFBu,                       // $8008 BPL $8005              until vblank
    0xADu, 0x12u, 0x42u,                // $800A LDA $4212
    0x29u, 0x01u,                       // $800D AND #$01
    0xD0u, 0xF9u,                       // $800F BNE $800A              until the read is done
    0xADu, 0x16u, 0x40u,                // $8011 LDA $4016              no strobe first
    0x8Du, 0x10u, 0x00u,                // $8014 STA $0010
    0xA9u, 0x01u, 0x8Du, 0x16u, 0x40u,  // $8017 LDA #$01 / STA $4016
    0x9Cu, 0x16u, 0x40u,                // $801C STZ $4016              strobe
    0xADu, 0x16u, 0x40u,                // $801F LDA $4016
    0x8Du, 0x11u, 0x00u,                // $8022 STA $0011
    0xDBu};

TEST(SnesController, TheAutoReadClocksTheSameRegisterTheSerialPortReads) {
  // Y is down and B is not, so the padding (1) and a fresh first bit (B, 0) differ.
  Snes m = machineWith(kReadAfterAutoRead);
  m.setJoypad(JoypadPort::One, Joypad{.y = true});
  runToStop(m);
  EXPECT_EQ(m.state().joy[1], 0x40u) << "the auto-read saw Y";
  EXPECT_EQ(m.state().wram[0x10u] & 1u, 1u)
      << "after the auto-read's sixteen clocks the port is at its padding, not back at B";
  EXPECT_EQ(m.state().wram[0x11u] & 1u, 0u) << "a strobe starts it over at B, which is up";
}

TEST(SnesController, TheStrobeHeldHighThroughTheAutoReadRepeatsB) {
  // Strobe high, then the auto-read enabled and waited for; both result bytes read.
  const std::vector<std::uint8_t> program = {
      0xA9u, 0x01u, 0x8Du, 0x16u, 0x40u,  // LDA #$01 / STA $4016   strobe held high
      0x8Du, 0x00u, 0x42u,                // STA $4200              auto-read on
      0xADu, 0x12u, 0x42u,                // $8008 LDA $4212
      0x10u, 0xFBu,                       // BPL $8008
      0xADu, 0x12u, 0x42u,                // $800D LDA $4212
      0x29u, 0x01u,                       // AND #$01
      0xD0u, 0xF9u,                       // BNE $800D
      0xADu, 0x18u, 0x42u, 0x8Du, 0x10u, 0x00u,  // LDA $4218 / STA $0010
      0xADu, 0x19u, 0x42u, 0x8Du, 0x11u, 0x00u,  // LDA $4219 / STA $0011
      0xDBu};
  {
    Snes m = machineWith(program);
    m.setJoypad(JoypadPort::One, Joypad{.b = true});
    runToStop(m);
    EXPECT_EQ(m.state().wram[0x10u], 0xFFu) << "every bit is B";
    EXPECT_EQ(m.state().wram[0x11u], 0xFFu);
  }
  {
    Snes m = machineWith(program);
    m.setJoypad(JoypadPort::One, Joypad{.y = true, .start = true});
    runToStop(m);
    EXPECT_EQ(m.state().wram[0x10u], 0x00u) << "and B is not pressed";
    EXPECT_EQ(m.state().wram[0x11u], 0x00u);
  }
}

// ---- state ----------------------------------------------------------------------

TEST(SnesController, ThePadIsPartOfTheSnapshot) {
  Snes m = stoppedMachine(true);
  const Joypad pad{.select = true, .l = true};
  m.setJoypad(JoypadPort::One, pad);
  const SnesState snapshot = m.state();

  Snes other = stoppedMachine(true);
  EXPECT_FALSE(other.joypad(JoypadPort::One).has_value());
  other.restore(snapshot);
  ASSERT_TRUE(other.joypad(JoypadPort::One).has_value());
  EXPECT_EQ(*other.joypad(JoypadPort::One), pad);
  EXPECT_FALSE(other.joypad(JoypadPort::Two).has_value());
  other.run(230u * kLine);
  EXPECT_EQ(other.state().joy[1], 0x20u) << "Select in bit 5";
  EXPECT_EQ(other.state().joy[0], 0x20u) << "L in bit 5";
}

}  // namespace
}  // namespace snaggletooth
