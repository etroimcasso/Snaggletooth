// The controller ports: a pad presented to the machine and read by a program the
// two ways the console offers — the auto-read into $4218-$421F once a frame, and
// the serial ports at $4016/$4017 strobed and clocked by hand. The bit order, the
// identity bits and the padding are the documented ones; where the auto-read
// begins on vertical blank's first line, its 256-cycle grid from one frame to the
// next, the registers shifting as each bit arrives, the strobe held high and the
// auto-read's use of the same clock lines are pinned as the documentation states
// them (fullsnes "AUTO JOYPAD READ", anomie's timing notes and register notes for
// $4016 and $4218). Programs run from the cartridge and store what they read into
// work RAM; the auto-read is observed on the machine state after a run of exact
// length.

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "gtest/gtest.h"
#include "snaggletooth/snes/snes.h"

namespace snaggletooth {
namespace {

constexpr std::uint32_t kLine = 1364u;
constexpr std::uint32_t kVblankLine = 225u;

// The frame's first vertical-blank line begins here, and the machine's first
// auto-read 298 master cycles into it: H = 74.5. A stopped machine advances in
// six-cycle steps, so a point is reached by running to the multiple of six at or
// past it, and not yet reached by running to the one before.
constexpr std::uint64_t kFirstVblankLine = kVblankLine * kLine;    // 306,900
constexpr std::uint64_t kFirstStart = kFirstVblankLine + 298u;    // 307,198
constexpr std::uint64_t kSecondVblankLine = kFirstVblankLine + 262u * kLine - 4u;  // the first frame's line 240 is short: 664,264

std::uint64_t toSixes(std::uint64_t point) { return (point + 5u) / 6u * 6u; }

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
    p.push_back(0xADu);                                       // LDA $401x
    p.push_back(static_cast<std::uint8_t>(port & 0xFFu));
    p.push_back(static_cast<std::uint8_t>(port >> 8));
    p.push_back(0x8Du);                                       // STA $00xx
    p.push_back(static_cast<std::uint8_t>(0x10u + i));
    p.push_back(0x00u);
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

TEST(SnesController, TheButtonSetIsTheWireOrder) {
  ASSERT_EQ(buttons().size(), kButtonCount);
  EXPECT_EQ(buttons().front(), Button::B) << "B is the first bit on the wire";
  EXPECT_EQ(buttons().back(), Button::R) << "R is the twelfth";
  // The set and the wire layout cannot drift apart: the Nth button of the set is
  // the Nth bit the pad shifts out, which the layout above pins independently.
  for (std::size_t i = 0; i < kButtonCount; ++i) {
    Joypad pad;
    pad.hold(buttons()[i], true);
    EXPECT_EQ(pad.bits(), static_cast<std::uint16_t>(0x8000u >> i)) << buttonName(buttons()[i]);
  }
}

TEST(SnesController, EveryButtonIsNamedAndReadBackInAnyCase) {
  for (const Button button : buttons()) {
    const std::string_view name = buttonName(button);
    EXPECT_FALSE(name.empty());
    EXPECT_EQ(buttonFromName(name), button) << name;
    std::string shouted(name);
    for (char& c : shouted) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    EXPECT_EQ(buttonFromName(shouted), button) << shouted;
  }
  EXPECT_EQ(buttonName(Button::B), "b");
  EXPECT_EQ(buttonName(Button::Select), "select");
  EXPECT_EQ(buttonName(Button::R), "r");
  EXPECT_FALSE(buttonFromName("fire").has_value()) << "a word that names no button";
  EXPECT_FALSE(buttonFromName("").has_value());
  EXPECT_FALSE(buttonFromName("bb").has_value()) << "a longer word beginning with one";
}

TEST(SnesController, AButtonIsHeldAndReleasedByName) {
  for (const Button button : buttons()) {
    Joypad pad;
    EXPECT_FALSE(pad.holds(button)) << buttonName(button);
    pad.hold(button, true);
    for (const Button other : buttons()) {
      EXPECT_EQ(pad.holds(other), other == button)
          << buttonName(other) << " while " << buttonName(button) << " is held";
    }
    pad.hold(button, false);
    EXPECT_EQ(pad, Joypad{}) << "releasing it leaves nothing pressed";
  }
}

// ---- the auto-read ----------------------------------------------------------------

TEST(SnesController, AnEmptyPortReadsZeroThroughTheAutoRead) {
  Snes m = stoppedMachine(true);
  m.run(2u * 262u * kLine);
  EXPECT_NE(m.state().autoJoyStart, 0u) << "a read has run";
  for (std::size_t i = 0; i < 8; ++i) EXPECT_EQ(m.state().joy[i], 0u) << i;
  EXPECT_FALSE(m.joypad(JoypadPort::One).has_value());
  EXPECT_FALSE(m.joypad(JoypadPort::Two).has_value());
}

TEST(SnesController, TheFirstReadBeginsAtDot74AndAHalf) {
  // fullsnes: "it begins at H=74.5 on the first frame" — 298 master cycles into
  // vertical blank's first line, not at the line's start.
  Snes m = stoppedMachine(true);
  m.setJoypad(JoypadPort::One, Joypad{.b = true});
  m.run(kFirstVblankLine + 294u);
  EXPECT_EQ(m.state().autoJoyStart, 0u) << "not yet begun 294 cycles into the line";
  EXPECT_EQ(m.state().joyClocks[0], 0u) << "the pads are not strobed before it begins";
  m.run(6u);  // the step that passes cycle 298
  EXPECT_EQ(m.state().autoJoyStart, kFirstStart);
  EXPECT_EQ(m.state().autoJoyClocked, 0u) << "begun, nothing clocked";
}

TEST(SnesController, TheNextReadBeginsOnThe256CycleGridFromTheLast) {
  // fullsnes: "thereafter some multiple of 256 cycles after the start of the
  // previous read that falls within [H=32.5, H=95.5]". From 307,198 the grid's
  // first point at or past H = 32.5 (130 cycles) of the second frame's first
  // vertical-blank line, which begins at 664,264, is 664,574: H = 77.5.
  Snes m = stoppedMachine(true);
  m.run(kSecondVblankLine + 306u);  // the machine stops at 308 into the line
  EXPECT_EQ(m.state().autoJoyStart, kFirstStart) << "the second frame's read has not begun 308 cycles in";
  m.run(6u);  // passes 664,574
  EXPECT_EQ(m.state().autoJoyStart, kSecondVblankLine + 310u);
}

TEST(SnesController, TheRegistersShiftAsEachBitIsClocked) {
  // The strobe pulse takes 128 cycles and each bit 256 more, every port's register
  // shifting up one and taking the bit at the bottom. Start is the fourth bit on
  // the wire, so it enters at bit 0 after four clocks and reaches its resting
  // place, bit 12, as the sixteenth lands.
  Snes m = stoppedMachine(true);
  m.setJoypad(JoypadPort::One, Joypad{.start = true});
  m.run(toSixes(kFirstStart + 128u + 4u * 256u) - 6u);  // the step before the fourth clock lands
  EXPECT_EQ(m.state().autoJoyClocked, 3u);
  EXPECT_EQ(m.state().joy[0], 0x00u) << "B, Y and Select are up";
  m.run(6u);
  EXPECT_EQ(m.state().autoJoyClocked, 4u);
  EXPECT_EQ(m.state().joy[0], 0x01u) << "Start has just entered at bit 0";
  EXPECT_EQ(m.state().joy[1], 0x00u);
  m.run(toSixes(kFirstStart + 128u + 8u * 256u) - toSixes(kFirstStart + 128u + 4u * 256u));
  EXPECT_EQ(m.state().autoJoyClocked, 8u);
  EXPECT_EQ(m.state().joy[0], 0x10u) << "four clocks later it has moved to bit 4";
  m.run(toSixes(kFirstStart + 4224u) - 6u - toSixes(kFirstStart + 128u + 8u * 256u));
  EXPECT_EQ(m.state().autoJoyClocked, 15u);
  EXPECT_EQ(m.state().joy[1], 0x08u) << "one clock short, Start sits at bit 11";
  EXPECT_EQ(m.state().joy[0], 0x00u);
  m.run(6u);  // the sixteenth bit lands 4224 cycles after the start
  EXPECT_EQ(m.state().autoJoyClocked, 16u);
  EXPECT_EQ(m.state().joy[1], 0x10u) << "$4219: Start in bit 4";
  EXPECT_EQ(m.state().joy[0], 0x00u);
}

TEST(SnesController, ThePreviousFrameShiftsOutAboveThisOneShiftingIn) {
  // anomie: reading $4218-f while the busy bit is set "will return incorrect
  // values". Start was read on the first frame; on the second nothing is held,
  // and two clocks in the register shows Start's bit moved up to where Y rests.
  Snes m = stoppedMachine(true);
  m.setJoypad(JoypadPort::One, Joypad{.start = true});
  m.run(kSecondVblankLine);
  EXPECT_EQ(m.state().joy[1], 0x10u) << "the first frame's read landed Start";
  m.setJoypad(JoypadPort::One, Joypad{});
  const std::uint64_t secondStart = kSecondVblankLine + 310u;
  m.run(toSixes(secondStart + 128u + 2u * 256u) - kSecondVblankLine);
  EXPECT_EQ(m.state().autoJoyClocked, 2u);
  EXPECT_EQ(m.state().joy[1], 0x40u) << "Start's bit stands where Y rests, though nothing is pressed";
  m.run(toSixes(secondStart + 4224u) - toSixes(secondStart + 128u + 2u * 256u));
  EXPECT_EQ(m.state().joy[1], 0x00u) << "and is gone once all sixteen are in";
  EXPECT_EQ(m.state().joy[0], 0x00u);
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

TEST(SnesController, NoAutoReadRunsWhenItIsDisabled) {
  Snes m = stoppedMachine(false);
  m.setJoypad(JoypadPort::One, Joypad{.start = true});
  m.run(2u * 262u * kLine);
  EXPECT_EQ(m.state().autoJoyStart, 0u);
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

// Enables the auto-read, waits for a read to begin and then to finish, then
// reads the serial port without strobing, then strobes and reads again.
const std::vector<std::uint8_t> kReadAfterAutoRead = {
    0xA9u, 0x01u, 0x8Du, 0x00u, 0x42u,  // $8000 LDA #$01 / STA $4200   auto-read on
    0xADu, 0x12u, 0x42u,                // $8005 LDA $4212
    0x29u, 0x01u,                       // $8008 AND #$01
    0xF0u, 0xF9u,                       // $800A BEQ $8005              until a read has begun
    0xADu, 0x12u, 0x42u,                // $800C LDA $4212
    0x29u, 0x01u,                       // $800F AND #$01
    0xD0u, 0xF9u,                       // $8011 BNE $800C              until it is done
    0xADu, 0x16u, 0x40u,                // $8013 LDA $4016              no strobe first
    0x8Du, 0x10u, 0x00u,                // $8016 STA $0010
    0xA9u, 0x01u, 0x8Du, 0x16u, 0x40u,  // $8019 LDA #$01 / STA $4016
    0x9Cu, 0x16u, 0x40u,                // $801E STZ $4016              strobe
    0xADu, 0x16u, 0x40u,                // $8021 LDA $4016
    0x8Du, 0x11u, 0x00u,                // $8024 STA $0011
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

// Enables the auto-read, waits for the busy bit, then — with `readInside` — reads
// the serial port once while the read is in progress, waits for the busy bit to
// clear, and stores $4218/$4219.
std::vector<std::uint8_t> readDuringAutoRead(bool readInside) {
  std::vector<std::uint8_t> p = {
      0xA9u, 0x01u, 0x8Du, 0x00u, 0x42u,  // $8000 LDA #$01 / STA $4200   auto-read on
      0xADu, 0x12u, 0x42u,                // $8005 LDA $4212
      0x29u, 0x01u,                       // $8008 AND #$01
      0xF0u, 0xF9u};                      // $800A BEQ $8005              until busy
  if (readInside) {
    p.insert(p.end(), {0xADu, 0x16u, 0x40u});  // $800C LDA $4016          one clock, inside the window
  } else {
    p.insert(p.end(), {0xEAu, 0xEAu, 0xEAu});  // $800C NOP NOP NOP
  }
  p.insert(p.end(), {0x8Du, 0x10u, 0x00u,      // $800F STA $0010
                     0xADu, 0x12u, 0x42u,      // $8012 LDA $4212
                     0x29u, 0x01u,             // $8015 AND #$01
                     0xD0u, 0xF9u,             // $8017 BNE $8012          until done
                     0xADu, 0x18u, 0x42u, 0x8Du, 0x11u, 0x00u,  // LDA $4218 / STA $0011
                     0xADu, 0x19u, 0x42u, 0x8Du, 0x12u, 0x00u,  // LDA $4219 / STA $0012
                     0xDBu});
  return p;
}

TEST(SnesController, ASerialReadInsideTheWindowTakesAClockFromTheAutoRead) {
  // The serial port and the auto-read drive one clock line, so a program's read
  // inside the window advances the pad past a bit the auto-read then never sees:
  // every later bit lands one place higher and the padding, 1, enters last. The
  // program's read lands well inside the strobe pulse and the first clock, so the
  // bit it takes is B.
  const Joypad all{.b = true, .y = true, .select = true, .start = true, .up = true, .down = true,
                   .left = true, .right = true, .a = true, .x = true, .l = true, .r = true};
  {
    Snes m = machineWith(readDuringAutoRead(false));
    m.setJoypad(JoypadPort::One, all);
    runToStop(m);
    EXPECT_EQ(m.state().wram[0x11u], 0xF0u) << "$4218 with the window left alone";
    EXPECT_EQ(m.state().wram[0x12u], 0xFFu) << "$4219";
  }
  {
    Snes m = machineWith(readDuringAutoRead(true));
    m.setJoypad(JoypadPort::One, all);
    runToStop(m);
    EXPECT_EQ(m.state().wram[0x10u] & 1u, 1u) << "the program's own read took B";
    EXPECT_EQ(m.state().wram[0x11u], 0xE1u) << "$4218: R has moved up to bit 5 and the padding sits at bit 0";
    EXPECT_EQ(m.state().wram[0x12u], 0xFFu) << "$4219: Y through Right, then A";
  }
}

TEST(SnesController, TheStrobeHeldHighThroughTheAutoReadRepeatsB) {
  // Strobe high, then the auto-read enabled and waited for, its beginning and its
  // end; both result bytes read.
  const std::vector<std::uint8_t> program = {
      0xA9u, 0x01u, 0x8Du, 0x16u, 0x40u,  // LDA #$01 / STA $4016   strobe held high
      0x8Du, 0x00u, 0x42u,                // STA $4200              auto-read on
      0xADu, 0x12u, 0x42u,                // $8008 LDA $4212
      0x29u, 0x01u,                       // AND #$01
      0xF0u, 0xF9u,                       // BEQ $8008              until a read has begun
      0xADu, 0x12u, 0x42u,                // $800F LDA $4212
      0x29u, 0x01u,                       // AND #$01
      0xD0u, 0xF9u,                       // BNE $800F              until it is done
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
