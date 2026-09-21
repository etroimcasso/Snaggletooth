// How a cartridge image lays across the bus, and where its save RAM lives. Both
// layouts are exercised on authored images: a real cartridge carries no ground
// truth, so an assertion against one could only ratify whatever bytes were there.
// Each image is built with a header the test writes and a byte pattern that says
// where in the image a byte came from, and every address is reached the way the
// console reaches it — by running a program on the cartridge and reading what it
// left in work RAM.

#include <cstdint>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "snaggletooth/snes/snes.h"

namespace snaggletooth {
namespace {

constexpr std::size_t kLoRomHeaderBase = 0x7FC0u;
constexpr std::size_t kHiRomHeaderBase = 0xFFC0u;

// Where a layout puts the bytes that bank $00 sees at $8000 and at $FFFC.
constexpr std::size_t programBase(bool hiRom) { return hiRom ? 0x8000u : 0x0000u; }
constexpr std::size_t vectorBase(bool hiRom) { return hiRom ? 0xFFFCu : 0x7FFCu; }

// Every byte of an authored image names its own offset. All three bytes of the
// offset go into the value, so two addresses that differ only in their bank still
// hold different bytes — which is what lets a test tell one layout from another.
std::uint8_t patternAt(std::size_t index) {
  return static_cast<std::uint8_t>(index ^ (index >> 8) ^ (index >> 16));
}

// A cartridge image whose every byte names its own offset, with a header at the
// site its layout uses: a checksum agreeing with its complement (the mark of a
// real header), the map mode, and the save-RAM size code.
std::vector<std::uint8_t> authoredCartridge(std::size_t bytes, bool hiRom, std::uint8_t mapMode,
                                            std::uint8_t saveCode) {
  std::vector<std::uint8_t> rom(bytes);
  for (std::size_t i = 0; i < bytes; ++i) rom[i] = patternAt(i);
  const std::size_t base = hiRom ? kHiRomHeaderBase : kLoRomHeaderBase;
  for (std::size_t i = 0; i < 21; ++i) rom[base + i] = static_cast<std::uint8_t>('A' + (i % 26));
  rom[base + 0x15] = mapMode;
  rom[base + 0x16] = 0x02;  // ROM+RAM+battery
  rom[base + 0x18] = saveCode;
  rom[base + 0x1C] = 0x34;  // complement, then the checksum it agrees with
  rom[base + 0x1D] = 0x12;
  rom[base + 0x1E] = 0xCB;
  rom[base + 0x1F] = 0xED;
  return rom;
}

void put24(std::vector<std::uint8_t>& rom, std::size_t at, std::uint32_t address) {
  rom[at] = static_cast<std::uint8_t>(address & 0xFFu);
  rom[at + 1] = static_cast<std::uint8_t>((address >> 8) & 0xFFu);
  rom[at + 2] = static_cast<std::uint8_t>((address >> 16) & 0xFFu);
}

// Places a program at the cartridge byte bank $00 reads at $8000 and points the
// reset vector there. The program runs in emulation mode, where the long forms of
// LDA and STA still reach any bank, which is all these tests need.
void placeProgram(std::vector<std::uint8_t>& rom, bool hiRom,
                  const std::vector<std::uint8_t>& program) {
  const std::size_t at = programBase(hiRom);
  for (std::size_t i = 0; i < program.size(); ++i) rom[at + i] = program[i];
  rom[vectorBase(hiRom)] = 0x00u;      // reset vector -> $8000
  rom[vectorBase(hiRom) + 1] = 0x80u;
}

// Copies each source address to work RAM at $7E0000 + its position, then stops.
std::vector<std::uint8_t> copyProgram(const std::vector<std::uint32_t>& sources) {
  std::vector<std::uint8_t> p;
  for (std::size_t i = 0; i < sources.size(); ++i) {
    p.push_back(0xAFu);                                   // LDA long
    p.insert(p.end(), 3, 0u);
    put24(p, p.size() - 3, sources[i]);
    p.push_back(0x8Fu);                                   // STA long
    p.insert(p.end(), 3, 0u);
    put24(p, p.size() - 3, 0x7E0000u + static_cast<std::uint32_t>(i));
  }
  p.push_back(0xDBu);                                     // STP
  return p;
}

void runToStop(Snes& m, int cap = 400) {
  for (int i = 0; i < cap && m.state().cpu.run == CpuRunState::Running; ++i) m.step();
}

// Runs a machine that copies `sources` into work RAM and returns what it copied.
std::vector<std::uint8_t> readThroughBus(std::vector<std::uint8_t> rom, bool hiRom,
                                         const std::vector<std::uint32_t>& sources,
                                         std::optional<CartridgeMap> map = std::nullopt) {
  placeProgram(rom, hiRom, copyProgram(sources));
  Snes m(SnesConfig{.rom = rom, .iplStub = false, .map = map});
  runToStop(m);
  std::vector<std::uint8_t> got;
  for (std::size_t i = 0; i < sources.size(); ++i) got.push_back(m.state().wram[i]);
  return got;
}

// ---- which layout an image declares ---------------------------------------

TEST(CartridgeMap, DetectsLoRomFromItsOwnHeader) {
  EXPECT_EQ(detectCartridgeMap(authoredCartridge(512u * 1024u, false, 0x20u, 0u)),
            CartridgeMap::LoRom);
}

TEST(CartridgeMap, DetectsHiRomFromItsOwnHeader) {
  EXPECT_EQ(detectCartridgeMap(authoredCartridge(1024u * 1024u, true, 0x31u, 0u)),
            CartridgeMap::HiRom);
}

TEST(CartridgeMap, AnExplicitMapOverridesTheHeader) {
  // One image, read twice under the two maps. The program and its reset vector go
  // in at both layouts' sites so the machine runs it either way, and the address
  // it reads is above $8000, which both maps carry — so the only thing that can
  // move the answer is which map the machine was told to use.
  std::vector<std::uint8_t> rom = authoredCartridge(1024u * 1024u, true, 0x31u, 0u);
  const std::vector<std::uint8_t> program = copyProgram({0xC38100u});
  for (const bool hiRom : {false, true}) placeProgram(rom, hiRom, program);

  Snes hi(SnesConfig{.rom = rom, .iplStub = false, .map = CartridgeMap::HiRom});
  runToStop(hi);
  EXPECT_EQ(hi.state().wram[0], patternAt(0x038100u));

  Snes lo(SnesConfig{.rom = rom, .iplStub = false, .map = CartridgeMap::LoRom});
  runToStop(lo);
  EXPECT_EQ(lo.state().wram[0], patternAt(0x018100u));
}

// ---- where a byte comes from ----------------------------------------------

TEST(CartridgeMap, LoRomLaysEachBanksUpperHalfEndToEnd) {
  const std::vector<std::uint8_t> rom = authoredCartridge(512u * 1024u, false, 0x20u, 0u);
  const std::vector<std::uint8_t> got =
      readThroughBus(rom, false, {0x00FFFFu, 0x018000u, 0x029234u});
  EXPECT_EQ(got[0], patternAt(0x007FFFu));
  EXPECT_EQ(got[1], patternAt(0x008000u));  // the next bank continues where this one ended
  EXPECT_EQ(got[2], patternAt(0x011234u));
}

TEST(CartridgeMap, HiRomLaysWholeBanksEndToEnd) {
  const std::vector<std::uint8_t> rom = authoredCartridge(1024u * 1024u, true, 0x31u, 0u);
  const std::vector<std::uint8_t> got =
      readThroughBus(rom, true, {0xC0FFFFu, 0xC10000u, 0xC31234u});
  EXPECT_EQ(got[0], patternAt(0x00FFFFu));  // a whole bank, not a half
  EXPECT_EQ(got[1], patternAt(0x010000u));
  EXPECT_EQ(got[2], patternAt(0x031234u));
}

TEST(CartridgeMap, HiRomReachesTheSameBytesThroughASystemBank) {
  const std::vector<std::uint8_t> rom = authoredCartridge(1024u * 1024u, true, 0x31u, 0u);
  const std::vector<std::uint8_t> got = readThroughBus(rom, true, {0x03ABCDu, 0xC3ABCDu});
  EXPECT_EQ(got[0], got[1]);
  EXPECT_EQ(got[0], patternAt(0x03ABCDu));
}

TEST(CartridgeMap, HiRomCarriesRomBelowEightThousand) {
  const std::vector<std::uint8_t> rom = authoredCartridge(1024u * 1024u, true, 0x31u, 0u);
  const std::vector<std::uint8_t> got = readThroughBus(rom, true, {0x401000u, 0xC20100u});
  EXPECT_EQ(got[0], patternAt(0x001000u));
  EXPECT_EQ(got[1], patternAt(0x020100u));
}

// A LoROM board leaves the cartridge's A15 unconnected, so a cartridge bank's lower
// half reads the bytes its upper half reads (fullsnes, the plain LoROM boards: "ROM
// Mirrors 40-7D,C0-FF:0000-7FFF"; the memory map page, on the same banks).
TEST(CartridgeMap, ALoRomCartridgeBanksLowerHalfRepeatsItsUpperHalf) {
  const std::vector<std::uint8_t> rom = authoredCartridge(4u * 1024u * 1024u, false, 0x20u, 0u);
  const std::vector<std::uint8_t> got = readThroughBus(
      rom, false, {0x401234u, 0x409234u, 0xC50777u, 0xC58777u, 0x6F7FFFu, 0x6FFFFFu});
  EXPECT_EQ(got[0], patternAt(0x201234u));  // bank $40 is 2 MB in, whichever half names it
  EXPECT_EQ(got[1], patternAt(0x201234u));
  EXPECT_EQ(got[2], patternAt(0x228777u));  // and the second region's banks the same
  EXPECT_EQ(got[3], patternAt(0x228777u));
  EXPECT_EQ(got[4], patternAt(0x37FFFFu));  // the last bank below the save window
  EXPECT_EQ(got[5], patternAt(0x37FFFFu));
}

// The save window's lower halves are the save only on a board that carries one. A
// cartridge with no save RAM has nothing to decode there, and those halves repeat
// their upper halves as every other cartridge bank's does.
TEST(CartridgeMap, WithNoSaveRamTheSaveWindowsLowerHalvesRepeatTheirUpperHalves) {
  const std::vector<std::uint8_t> rom = authoredCartridge(4u * 1024u * 1024u, false, 0x20u, 0u);
  const std::vector<std::uint8_t> got =
      readThroughBus(rom, false, {0x701234u, 0x709234u, 0xFF0100u, 0xFF8100u});
  EXPECT_EQ(got[0], patternAt(0x381234u));
  EXPECT_EQ(got[1], patternAt(0x381234u));
  EXPECT_EQ(got[2], patternAt(0x3F8100u));
  EXPECT_EQ(got[3], patternAt(0x3F8100u));
}

// ---- an image repeating across its window ---------------------------------

// A cartridge carries one ROM chip per power of two in its size, and an address
// past a chip repeats that chip. An image that is itself a power of two is one
// chip, so the whole image repeats.
TEST(CartridgeMap, APowerOfTwoImageRepeatsWhole) {
  const std::vector<std::uint8_t> rom = authoredCartridge(512u * 1024u, false, 0x20u, 0u);
  // Bank $10 at $8234 is image offset $080234, one whole image past $000234 —
  // clear of the code this test wrote at the start of the image.
  const std::vector<std::uint8_t> got = readThroughBus(rom, false, {0x108234u, 0x118234u});
  EXPECT_EQ(got[0], patternAt(0x000234u));
  EXPECT_EQ(got[1], patternAt(0x008234u));
}

// A 3 MB cartridge is a 2 MB chip and a 1 MB chip. The megabyte above the image
// repeats the second chip, so an address a whole image past the start reaches
// 2 MB in — not offset zero, which is where treating the image as one chip would
// send it.
TEST(CartridgeMap, AnImageOfTwoChipsRepeatsOnlyItsSecond) {
  const std::vector<std::uint8_t> rom = authoredCartridge(3u * 1024u * 1024u, false, 0x20u, 0u);
  const std::vector<std::uint8_t> got = readThroughBus(rom, false, {0x608000u, 0x6A8000u});
  EXPECT_EQ(got[0], patternAt(0x200000u));  // $300000 repeats the 1 MB chip at $200000
  EXPECT_NE(got[0], patternAt(0x000000u));  // and emphatically not the start of the image
  EXPECT_EQ(got[1], patternAt(0x250000u));  // half a megabyte into that chip
}

// ---- save RAM --------------------------------------------------------------

TEST(CartridgeMap, SaveRamSizeComesFromTheHeader) {
  EXPECT_EQ(declaredSaveRamBytes(authoredCartridge(65536u, false, 0x20u, 0u)), 0u);
  EXPECT_EQ(declaredSaveRamBytes(authoredCartridge(65536u, false, 0x20u, 1u)), 2048u);
  EXPECT_EQ(declaredSaveRamBytes(authoredCartridge(65536u, false, 0x20u, 3u)), 8192u);
  EXPECT_EQ(declaredSaveRamBytes(authoredCartridge(65536u, false, 0x20u, 5u)), 32768u);
}

// Writes `value` to `address`, then reads `readBack` into work RAM at $7E0000.
Snes runStoreThenLoad(std::vector<std::uint8_t> rom, bool hiRom, std::uint32_t address,
                      std::uint8_t value, std::uint32_t readBack) {
  std::vector<std::uint8_t> p = {0xA9u, value, 0x8Fu, 0u, 0u, 0u};
  put24(p, 3, address);
  p.push_back(0xAFu);
  p.insert(p.end(), 3, 0u);
  put24(p, p.size() - 3, readBack);
  p.push_back(0x8Fu);
  p.insert(p.end(), 3, 0u);
  put24(p, p.size() - 3, 0x7E0000u);
  p.push_back(0xDBu);
  placeProgram(rom, hiRom, p);
  Snes m(SnesConfig{.rom = rom, .iplStub = false});
  runToStop(m);
  return m;
}

TEST(CartridgeMap, LoRomKeepsItsSaveAboveTheCartridgeBanks) {
  const std::vector<std::uint8_t> rom = authoredCartridge(512u * 1024u, false, 0x20u, /*8 KB*/ 3u);
  const Snes m = runStoreThenLoad(rom, false, 0x701FFFu, 0x5Au, 0x701FFFu);
  ASSERT_EQ(m.state().sram.size(), 8192u);
  EXPECT_EQ(m.state().wram[0], 0x5Au);
  EXPECT_EQ(m.state().sram[0x1FFFu], 0x5Au);
}

// A cartridge whose header declares a coprocessor is on that chip's board, which
// gives lower halves of its cartridge banks to the chip: fullsnes's memory map puts a
// DSP at 600000h-6F7FFFh on the 2 MB LoROM boards and the ST010's ports and RAM at
// 600000h-6FFFFFh. The machine carries no such chip, so the image is not read there
// and the read answers the last byte the bus carried — the operand's bank.
TEST(CartridgeMap, ACoprocessorsBoardKeepsItsLowerHalvesForTheChip) {
  std::vector<std::uint8_t> rom = authoredCartridge(4u * 1024u * 1024u, false, 0x20u, 0u);
  rom[kLoRomHeaderBase + 0x16] = 0x05u;  // ROM, a coprocessor, RAM and a battery: a DSP
  const std::vector<std::uint8_t> got =
      readThroughBus(rom, false, {0xE00000u, 0x681234u, 0x401234u, 0x701234u, 0x409234u});
  EXPECT_EQ(got[0], 0xE0u);
  EXPECT_EQ(got[1], 0x68u);
  EXPECT_EQ(got[2], 0x40u);
  EXPECT_EQ(got[3], 0x70u);
  EXPECT_EQ(got[4], patternAt(0x201234u));  // the upper halves are the image, as on any board

  // The board is the cartridge's, so a machine moved to a new place still knows it.
  placeProgram(rom, false, copyProgram({0xE00000u}));
  Snes built(SnesConfig{.rom = rom, .iplStub = false});
  Snes moved(std::move(built));
  runToStop(moved);
  EXPECT_EQ(moved.state().wram[0], 0xE0u);
}

// With save RAM on the board the window's lower half is the save and the image is
// not read there; the upper half of the same bank is the image still.
TEST(CartridgeMap, WithSaveRamTheWindowsLowerHalfIsTheSaveAndItsUpperHalfTheImage) {
  const std::vector<std::uint8_t> rom =
      authoredCartridge(4u * 1024u * 1024u, false, 0x20u, /*8 KB*/ 3u);
  const std::vector<std::uint8_t> got = readThroughBus(rom, false, {0x701234u, 0x709234u});
  EXPECT_EQ(got[0], 0x00u);                 // the save, which nothing has written
  EXPECT_NE(patternAt(0x381234u), 0x00u);   // and which the image byte would not read as
  EXPECT_EQ(got[1], patternAt(0x381234u));
}

// The window runs through bank $FF (fullsnes: "SRAM at 70h-7Dh,F0h-FFh:0000h-7FFFh",
// and every LoROM board with save RAM in its table). A store through $FE or $FF
// lands in the save and reads back through $70.
TEST(CartridgeMap, LoRomsSaveWindowRunsThroughBankFF) {
  const std::vector<std::uint8_t> rom = authoredCartridge(512u * 1024u, false, 0x20u, /*8 KB*/ 3u);
  const Snes viaFE = runStoreThenLoad(rom, false, 0xFE0010u, 0xA5u, 0x700010u);
  EXPECT_EQ(viaFE.state().wram[0], 0xA5u);
  EXPECT_EQ(viaFE.state().sram[0x0010u], 0xA5u);
  const Snes viaFF = runStoreThenLoad(rom, false, 0xFF1FFFu, 0x3Cu, 0x701FFFu);
  EXPECT_EQ(viaFF.state().wram[0], 0x3Cu);
  EXPECT_EQ(viaFF.state().sram[0x1FFFu], 0x3Cu);
}

// A store into a lower half that repeats the image changes nothing: it is ROM.
TEST(CartridgeMap, AStoreIntoARepeatedLowerHalfChangesNothing) {
  const std::vector<std::uint8_t> rom = authoredCartridge(4u * 1024u * 1024u, false, 0x20u, 0u);
  const Snes m = runStoreThenLoad(rom, false, 0x701234u, 0xEEu, 0x701234u);
  EXPECT_TRUE(m.state().sram.empty());
  EXPECT_EQ(m.state().wram[0], patternAt(0x381234u));
}

TEST(CartridgeMap, HiRomKeepsItsSaveInTheExpansionWindow) {
  const std::vector<std::uint8_t> rom = authoredCartridge(1024u * 1024u, true, 0x31u, /*2 KB*/ 1u);
  const Snes m = runStoreThenLoad(rom, true, 0x2067FFu, 0x22u, 0x2067FFu);
  ASSERT_EQ(m.state().sram.size(), 2048u);
  EXPECT_EQ(m.state().wram[0], 0x22u);
  EXPECT_EQ(m.state().sram[0x07FFu], 0x22u);
}

// A save smaller than its window repeats within it. This is what a game checks
// when it wants to know how much save RAM the cartridge really has, so the
// declared size has to be what decides — not the window, and not a maximum.
TEST(CartridgeMap, ASmallSaveRepeatsAcrossItsWindow) {
  const std::vector<std::uint8_t> rom = authoredCartridge(1024u * 1024u, true, 0x31u, /*2 KB*/ 1u);
  // The write lands 2 KB and a bit into the window, so the cell it reaches is a
  // long way from the start of the save. A rule that merely stopped at the end of
  // the save would put it at zero instead, and read back the wrong byte.
  const Snes m = runStoreThenLoad(rom, true, 0x206900u, 0x77u, 0x206100u);
  ASSERT_EQ(m.state().sram.size(), 2048u);
  EXPECT_EQ(m.state().wram[0], 0x77u);
  EXPECT_EQ(m.state().sram[0x100u], 0x77u);
  EXPECT_EQ(m.state().sram[0], 0x00u);  // and nothing landed at the start
}

TEST(CartridgeMap, ACartridgeWithNoSaveReachesNothingThere) {
  const std::vector<std::uint8_t> rom = authoredCartridge(1024u * 1024u, true, 0x31u, /*none*/ 0u);
  const Snes m = runStoreThenLoad(rom, true, 0x206000u, 0xEEu, 0x206000u);
  EXPECT_TRUE(m.state().sram.empty());
  EXPECT_NE(m.state().wram[0], 0xEEu);
  // HiROM's window is in the expansion area, where a board with no save has nothing
  // at all: the read answers the last byte the bus carried, the operand's bank.
  EXPECT_EQ(m.state().wram[0], 0x20u);
}

TEST(CartridgeMap, AnExplicitSaveSizeOverridesTheHeader) {
  const std::vector<std::uint8_t> rom = authoredCartridge(1024u * 1024u, true, 0x31u, /*8 KB*/ 3u);
  Snes m(SnesConfig{.rom = rom, .iplStub = false, .saveRamBytes = std::size_t{0}});
  EXPECT_TRUE(m.state().sram.empty());
}

// The save is machine state, so a snapshot carries it and a restore puts it back.
TEST(CartridgeMap, TheSaveSurvivesASnapshotAndRestore) {
  const std::vector<std::uint8_t> rom = authoredCartridge(1024u * 1024u, true, 0x31u, /*2 KB*/ 1u);
  Snes m = runStoreThenLoad(rom, true, 0x206010u, 0x3Cu, 0x206010u);
  ASSERT_EQ(m.state().sram[0x10u], 0x3Cu);
  const SnesState snapshot = m.state();

  SnesState scribbled = snapshot;
  scribbled.sram[0x10u] = 0xC3u;
  m.restore(scribbled);
  ASSERT_EQ(m.state().sram[0x10u], 0xC3u);

  m.restore(snapshot);
  EXPECT_EQ(m.state().sram[0x10u], 0x3Cu);
}

}  // namespace
}  // namespace snaggletooth
