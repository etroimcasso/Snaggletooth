// SnesConfig::bootRom — the audio boot ROM the machine runs in place of its
// built-in upload stub. The image is seeded into audio RAM and mapped over the
// $FFC0 window, so the audio CPU executes it from power-on; a machine given no
// image runs the stub instead.
//
// The image here is a marker program rather than any console's boot code: it posts
// a distinctive byte to output port 0, which is what the assertions read. Running
// is the point — a test that only compared RAM could not tell a seeded image from
// an executed one.

#include <array>
#include <cstdint>
#include <vector>

#include "gtest/gtest.h"
#include "snaggletooth/snes/snes.h"
#include "snes_ipl_stub.h"

namespace snaggletooth {
namespace {

constexpr std::uint8_t kMarker = 0x5Au;

// A boot image that posts the marker to output port 0 and holds there.
//
//   $FFC0  20        CLRP           ; the direct page is $00xx, so $F4 is the port
//   $FFC1  8F 5A F4  MOV $F4,#$5A   ; post the marker
//   $FFC4  2F FE     BRA $FFC4      ; hold
//   $FFFE  C0 FF     reset vector -> $FFC0
std::array<std::uint8_t, kIplWindowBytes> markerImage() {
  std::array<std::uint8_t, kIplWindowBytes> image{};
  image[0] = 0x20u;
  image[1] = 0x8Fu;
  image[2] = kMarker;
  image[3] = 0xF4u;
  image[4] = 0x2Fu;
  image[5] = 0xFEu;
  image[kIplWindowBytes - 2u] = 0xC0u;
  image[kIplWindowBytes - 1u] = 0xFFu;
  return image;
}

// A cartridge that does nothing in particular: these cases read the audio side.
std::vector<std::uint8_t> cartridge() {
  std::vector<std::uint8_t> rom(0x8000u, 0x00u);
  rom[0x7FFCu] = 0x00u;  // reset vector -> $8000
  rom[0x7FFDu] = 0x80u;
  return rom;
}

// Runs the machine long enough for the audio CPU to reach its first few
// instructions, which is all any image here needs to post its byte.
void settle(Snes& machine) { machine.run(20'000u); }

// ---- which image runs -----------------------------------------------------

TEST(BootRom, SuppliedImageRunsInPlaceOfTheStub) {
  const std::vector<std::uint8_t> rom = cartridge();
  Snes machine(SnesConfig{.rom = rom, .bootRom = markerImage()});
  settle(machine);
  EXPECT_EQ(machine.state().apu.outputPorts[0], kMarker);
}

TEST(BootRom, AbsentImageRunsTheStub) {
  const std::vector<std::uint8_t> rom = cartridge();
  Snes machine(SnesConfig{.rom = rom});
  settle(machine);
  EXPECT_EQ(machine.state().apu.outputPorts[0], 0xAAu);  // the stub's ready bytes
  EXPECT_EQ(machine.state().apu.outputPorts[1], 0xBBu);
}

// ---- where it lands -------------------------------------------------------

TEST(BootRom, SuppliedImageIsSeededIntoAudioRam) {
  const std::vector<std::uint8_t> rom = cartridge();
  const std::array<std::uint8_t, kIplWindowBytes> image = markerImage();
  Snes machine(SnesConfig{.rom = rom, .bootRom = image});
  for (std::size_t i = 0; i < kIplWindowBytes; ++i) {
    EXPECT_EQ(machine.state().apu.ram[kIplWindowBase + i], image[i]) << "at window byte " << i;
  }
}

TEST(BootRom, AbsentImageSeedsTheStub) {
  const std::vector<std::uint8_t> rom = cartridge();
  Snes machine(SnesConfig{.rom = rom});
  const std::array<std::uint8_t, kIplWindowBytes>& stub = iplStubImage();
  for (std::size_t i = 0; i < kIplWindowBytes; ++i) {
    EXPECT_EQ(machine.state().apu.ram[kIplWindowBase + i], stub[i]) << "at window byte " << i;
  }
}

// ---- the boot sequence turned off ------------------------------------------

TEST(BootRom, ImageIsIgnoredWithoutTheBootSequence) {
  const std::vector<std::uint8_t> rom = cartridge();
  Snes supplied(SnesConfig{.rom = rom, .iplStub = false, .bootRom = markerImage()});
  Snes bare(SnesConfig{.rom = rom, .iplStub = false});
  for (std::size_t i = 0; i < kIplWindowBytes; ++i) {
    EXPECT_EQ(supplied.state().apu.ram[kIplWindowBase + i],
              bare.state().apu.ram[kIplWindowBase + i])
        << "at window byte " << i;
  }
  EXPECT_EQ(supplied.state().apu.cpu.pc, bare.state().apu.cpu.pc);
}

// ---- the image is configuration --------------------------------------------

// The mapped image is configuration rather than machine state, so restoring a
// snapshot keeps it. Restoring a snapshot whose window RAM is blank proves it: the
// audio CPU still executes the image, because the window serves the image and the
// RAM beneath it is what a snapshot carries.
TEST(BootRom, MappedImageSurvivesRestore) {
  const std::vector<std::uint8_t> rom = cartridge();
  Snes machine(SnesConfig{.rom = rom, .bootRom = markerImage()});
  settle(machine);
  ASSERT_EQ(machine.state().apu.outputPorts[0], kMarker);

  SnesState blanked = machine.state();
  for (std::size_t i = 0; i < kIplWindowBytes; ++i) {
    blanked.apu.ram[kIplWindowBase + i] = 0x00u;
  }
  blanked.apu.cpu.pc = kIplWindowBase;  // back to the entry
  blanked.apu.outputPorts = {};

  machine.restore(blanked);
  settle(machine);
  EXPECT_EQ(machine.state().apu.outputPorts[0], kMarker);
}

}  // namespace
}  // namespace snaggletooth
