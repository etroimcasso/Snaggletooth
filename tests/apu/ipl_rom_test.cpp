// The audio unit's boot-ROM window: a 64-byte image mapped over $FFC0-$FFFF for
// SPC700 reads while CONTROL bit 7 is set (Apu::mapIplRom). Reads in the window
// return the image; writes always reach the RAM beneath; clearing the bit, or
// mapping no image, exposes that RAM. The mapping is only visible from the SPC700
// side, so every assertion drives the real interpreter through the machine bus.
//
// Derived from the SNESdev SPC700 register documentation: CONTROL ($F1) bit 7
// enables the boot-ROM mapping over $FFC0-$FFFF, and writes to that range affect
// the RAM regardless of the setting.

#include <array>
#include <cstdint>
#include <initializer_list>

#include <gtest/gtest.h>

#include "snaggletooth/apu/apu.h"

namespace {

using snaggletooth::Apu;
using snaggletooth::ApuState;

// A machine with CONTROL seeded to `control` (bit 7 is the window enable), the rest
// at its power-on ready state. Seeding CONTROL directly avoids the port-clear and
// timer-reset side effects a CPU write to $F1 would carry.
Apu machineWithControl(std::uint8_t control) {
  ApuState state = Apu{}.state();
  state.control = control;
  return Apu(state);
}

// Writes `code` into RAM at `at` (through the host seam, which bypasses the overlay
// and the window).
void loadProgram(Apu& apu, std::uint16_t at, std::initializer_list<std::uint8_t> code) {
  std::uint16_t address = at;
  for (std::uint8_t byte : code) apu.writeRam(address++, byte);
}

// A window image whose first byte is `first` and whose remaining bytes are distinct
// markers, so a read of any window address is identifiable.
std::array<std::uint8_t, 64> markerImage(std::uint8_t first) {
  std::array<std::uint8_t, 64> image{};
  image[0] = first;
  for (std::size_t i = 1; i < image.size(); ++i)
    image[i] = static_cast<std::uint8_t>(0xC0u + i);
  return image;
}

constexpr std::uint8_t kBit7 = 0x80u;  // CONTROL bit 7: the window enable

// SPC700 opcodes for observing the window from the CPU side.
constexpr std::uint8_t kMovAImm = 0xE8u;   // MOV A,#imm
constexpr std::uint8_t kMovAAbs = 0xE5u;   // MOV A,!abs
constexpr std::uint8_t kMovAbsA = 0xC5u;   // MOV !abs,A
constexpr std::uint8_t kMovDpImm = 0x8Fu;  // MOV dp,#imm (immediate first, then dp)

// A data read of $FFC0 with bit 7 set returns the mapped image, not the RAM beneath.
TEST(IplRom, DataReadReturnsImageWhenEnabled) {
  Apu apu = machineWithControl(kBit7);
  apu.mapIplRom(markerImage(0x5Au));
  apu.writeRam(0xFFC0u, 0x11u);  // the RAM under the window differs from the image
  loadProgram(apu, 0x0200u, {kMovAAbs, 0xC0u, 0xFFu});  // MOV A,$FFC0
  apu.setPc(0x0200u);
  apu.step();
  EXPECT_EQ(apu.state().cpu.a, 0x5Au);
}

// An instruction fetch from the window with bit 7 set runs the image's code, not the
// RAM beneath it.
TEST(IplRom, InstructionFetchRunsImageWhenEnabled) {
  Apu apu = machineWithControl(kBit7);
  std::array<std::uint8_t, 64> image{};
  image[0] = kMovAImm;  // MOV A,#$42 at the window base
  image[1] = 0x42u;
  apu.mapIplRom(image);
  loadProgram(apu, 0xFFC0u, {kMovAImm, 0x99u});  // the RAM under the window: MOV A,#$99
  apu.setPc(0xFFC0u);
  apu.step();
  EXPECT_EQ(apu.state().cpu.a, 0x42u);
}

// A write to the window with bit 7 set lands in the RAM beneath; a mapped read still
// returns the image, unchanged by the write.
TEST(IplRom, WriteLandsInRamBeneathAndImageIsUnchanged) {
  Apu apu = machineWithControl(kBit7);
  apu.mapIplRom(markerImage(0x5Au));
  loadProgram(apu, 0x0200u, {
                                kMovAImm, 0x77u,          // MOV A,#$77
                                kMovAbsA, 0xC0u, 0xFFu,   // MOV $FFC0,A  (into RAM under the window)
                                kMovAAbs, 0xC0u, 0xFFu,   // MOV A,$FFC0  (mapped read)
                            });
  apu.setPc(0x0200u);
  for (int i = 0; i < 3; ++i) apu.step();
  EXPECT_EQ(apu.readRam(0xFFC0u), 0x77u);  // the write reached RAM
  EXPECT_EQ(apu.state().cpu.a, 0x5Au);     // the mapped read still returns the image
}

// Clearing bit 7 exposes the RAM the driver wrote under the window.
TEST(IplRom, ClearingBit7ExposesTheRamWrite) {
  Apu apu = machineWithControl(kBit7);
  apu.mapIplRom(markerImage(0x5Au));
  apu.writeRam(0xFFC0u, 0x77u);  // a byte living under the window
  loadProgram(apu, 0x0200u, {
                                kMovAAbs, 0xC0u, 0xFFu,   // MOV A,$FFC0  -> image (bit 7 set)
                                kMovDpImm, 0x00u, 0xF1u,  // MOV $F1,#$00 -> CONTROL, clears bit 7
                                kMovAAbs, 0xC0u, 0xFFu,   // MOV A,$FFC0  -> RAM (bit 7 clear)
                            });
  apu.setPc(0x0200u);
  apu.step();
  EXPECT_EQ(apu.state().cpu.a, 0x5Au);  // first read is the image
  apu.step();                           // clear CONTROL bit 7
  apu.step();
  EXPECT_EQ(apu.state().cpu.a, 0x77u);  // second read is the RAM beneath
}

// With bit 7 clear, a mapped image is inert: the window reads RAM.
TEST(IplRom, Bit7ClearReadsRam) {
  Apu apu = machineWithControl(0x00u);
  apu.mapIplRom(markerImage(0x5Au));
  apu.writeRam(0xFFC0u, 0x22u);
  loadProgram(apu, 0x0200u, {kMovAAbs, 0xC0u, 0xFFu});  // MOV A,$FFC0
  apu.setPc(0x0200u);
  apu.step();
  EXPECT_EQ(apu.state().cpu.a, 0x22u);
}

// A machine with no image mapped reads plain RAM in the window even with bit 7 set.
TEST(IplRom, NoImageMappedReadsRamWithBit7Set) {
  Apu apu = machineWithControl(kBit7);
  apu.writeRam(0xFFC0u, 0x33u);
  loadProgram(apu, 0x0200u, {kMovAAbs, 0xC0u, 0xFFu});  // MOV A,$FFC0
  apu.setPc(0x0200u);
  apu.step();
  EXPECT_EQ(apu.state().cpu.a, 0x33u);
}

// restore() keeps the mapped image — it is configuration, not part of the state value.
TEST(IplRom, RestoreKeepsTheImage) {
  Apu apu = machineWithControl(kBit7);
  apu.mapIplRom(markerImage(0x5Au));
  ApuState snapshot = machineWithControl(kBit7).state();
  apu.restore(snapshot);  // a state value that carries no image
  apu.writeRam(0xFFC0u, 0x11u);
  loadProgram(apu, 0x0200u, {kMovAAbs, 0xC0u, 0xFFu});  // MOV A,$FFC0
  apu.setPc(0x0200u);
  apu.step();
  EXPECT_EQ(apu.state().cpu.a, 0x5Au);  // the image survived restore()
}

// reset() keeps the mapped image. reset() re-seeds CONTROL to $B0 (bit 7 set) and
// retains RAM above zero page, so the loaded program survives and the window maps.
TEST(IplRom, ResetKeepsTheImage) {
  Apu apu = machineWithControl(kBit7);
  apu.mapIplRom(markerImage(0x5Au));
  apu.writeRam(0xFFC0u, 0x11u);
  loadProgram(apu, 0x0200u, {kMovAAbs, 0xC0u, 0xFFu});  // MOV A,$FFC0
  apu.reset();
  apu.setPc(0x0200u);
  apu.step();
  EXPECT_EQ(apu.state().cpu.a, 0x5Au);  // the image survived reset()
}

// The host readRam seam bypasses the window: it returns the RAM byte, never the
// mapped image, regardless of bit 7.
TEST(IplRom, HostReadRamBypassesTheWindow) {
  Apu apu = machineWithControl(kBit7);
  apu.mapIplRom(markerImage(0x5Au));
  apu.writeRam(0xFFC0u, 0x44u);
  EXPECT_EQ(apu.readRam(0xFFC0u), 0x44u);
}

}  // namespace
