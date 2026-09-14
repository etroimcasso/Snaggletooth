// The picture the PPU draws, and the seam it hands it over: the frame observer's
// terms, the raster's shape, the converter's bytes, and Mode 1's BG1 complete — the
// tilemap formula at its four sizes, the character address, the four bitplanes and
// their order, both flips, 16x16 blocks, the palette bits, colour 0's transparency,
// scrolling, the backdrop, and the layers the main screen does not enable. Every
// expectation is computed by hand from the register page; the pictures are placed
// as a program would have left them, and one case drives the whole path from a
// cartridge that writes the video memories through their ports.

#include <array>
#include <cstdint>
#include <vector>

#include "gtest/gtest.h"
#include "snaggletooth/snes/snes.h"

namespace snaggletooth {
namespace {

// Where these pictures keep their pieces. The map base is $2107 bits 2-7, counting
// whole 32x32 screens of $400 words, and the character base is $210B bits 0-3,
// counting 8 KB blocks — so these two put the map at byte $0800 and the characters
// at $8000, far enough apart that a 64x64 map's four screens, which run to $27FF,
// do not reach them.
constexpr std::uint8_t kMapBase = 0x04u;   // $2107: base 1, and size 00 (one screen)
constexpr std::uint8_t kCharBase = 0x04u;  // $210B: BG1's characters at $8000
constexpr std::uint32_t kMapByte = 0x0800u;
constexpr std::uint32_t kCharByte = 0x8000u;

// A screen's worth of map is $800 bytes, so the next screen begins $400 entries on.
constexpr unsigned kScreenEntries = 0x400u;

// The palette words these cases use. A word is 15 bits, blue-green-red from the top.
constexpr std::uint16_t kBackdrop = 0x0C41u;  // red 1, green 2, blue 3
constexpr std::uint16_t kRed = 0x001Fu;
constexpr std::uint16_t kGreen = 0x03E0u;
constexpr std::uint16_t kBlue = 0x7C00u;
constexpr std::uint16_t kWhite = 0x7FFFu;
constexpr std::uint16_t kYellow = 0x03FFu;

using Rgba = std::array<std::uint8_t, 4>;

// What the converter drives for each of those at brightness 15, by hand from
// round(c * (N + 1) * 255 / (31 * 16)): 31 gives 255, 3 gives 25, 2 gives 16 and
// 1 gives 8.
constexpr Rgba kBackdropOut{8u, 16u, 25u, 255u};
constexpr Rgba kRedOut{255u, 0u, 0u, 255u};
constexpr Rgba kGreenOut{0u, 255u, 0u, 255u};
constexpr Rgba kBlueOut{0u, 0u, 255u, 255u};
constexpr Rgba kWhiteOut{255u, 255u, 255u, 255u};
constexpr Rgba kYellowOut{255u, 255u, 0u, 255u};
constexpr Rgba kBlack{0u, 0u, 0u, 255u};

// The frames a run finished, one of them kept whole — the first, unless a case
// wants a later one because its cartridge was still setting the picture up.
struct Picture final : FrameObserver {
  unsigned keep = 1;
  unsigned frames = 0;
  unsigned width = 0;
  unsigned height = 0;
  std::vector<std::uint8_t> fields;
  std::vector<std::uint8_t> pixels;

  void frame(const VideoFrame& picture) override {
    ++frames;
    fields.push_back(picture.field);
    if (frames != keep) return;
    width = picture.width;
    height = picture.height;
    pixels.assign(picture.pixels.begin(), picture.pixels.end());
  }

  // The four bytes at a position in the kept frame.
  [[nodiscard]] Rgba at(unsigned x, unsigned y) const {
    const std::size_t index = (static_cast<std::size_t>(y) * width + x) * 4u;
    return Rgba{pixels.at(index), pixels.at(index + 1u), pixels.at(index + 2u),
                pixels.at(index + 3u)};
  }
};

constexpr std::uint8_t kStp = 0xDBu;
constexpr std::uint8_t kLdaImm = 0xA9u;
constexpr std::uint8_t kStaAbs = 0x8Du;

// A cartridge running `program` from $8000.
std::vector<std::uint8_t> cartridge(std::vector<std::uint8_t> program) {
  program.resize(0x8000u, 0x00u);
  program[0x7FFCu] = 0x00u;  // reset -> $8000
  program[0x7FFDu] = 0x80u;
  return program;
}

// A cartridge that stops at once, so the beam runs while the CPU touches nothing.
std::vector<std::uint8_t> haltedCartridge() { return cartridge({kStp}); }

// More than an NTSC frame of master cycles, so a run from power-on reaches the next
// frame's first line, where a finished frame is handed over. A frame is 262 lines of
// 1364 master cycles, less the four its short line drops.
constexpr std::uint64_t kFrameMaster = 357364u;
constexpr std::uint64_t kOneFrame = kFrameMaster + 20000u;
constexpr std::uint64_t kTwoFrames = 2u * kFrameMaster + 20000u;

// Runs a machine holding `ppu` from power-on and returns what it drew.
Picture draw(const PpuState& ppu, std::uint64_t cycles = kOneFrame) {
  const std::vector<std::uint8_t> rom = haltedCartridge();
  Snes machine(SnesConfig{.rom = rom});
  SnesState state = machine.state();
  state.ppu = ppu;
  machine.restore(state);
  Picture picture;
  machine.setFrameObserver(&picture);
  machine.run(cycles);
  return picture;
}

// A palette word as CGRAM holds it: the low byte, then the high.
void putColour(PpuState& ppu, unsigned word, std::uint16_t colour) {
  ppu.cgram[word * 2u] = static_cast<std::uint8_t>(colour & 0xFFu);
  ppu.cgram[word * 2u + 1u] = static_cast<std::uint8_t>(colour >> 8);
}

// A tilemap entry `index` words into the map, the same way.
void putEntry(PpuState& ppu, unsigned index, std::uint16_t entry) {
  ppu.vram[kMapByte + index * 2u] = static_cast<std::uint8_t>(entry & 0xFFu);
  ppu.vram[kMapByte + index * 2u + 1u] = static_cast<std::uint8_t>(entry >> 8);
}

using TileRows = std::array<std::array<std::uint8_t, 8>, 8>;

// A 16-colour character at a byte address: eight rows of eight colour indices, as
// four bitplanes — planes 0 and 1 in the low and high bytes of eight words, then
// planes 2 and 3 in the same form — with the leftmost pixel of a row in bit 7.
void putTileAt(PpuState& ppu, std::uint32_t byteAddress, const TileRows& rows) {
  for (unsigned row = 0u; row < 8u; ++row) {
    std::array<std::uint8_t, 4> planes{};
    for (unsigned column = 0u; column < 8u; ++column) {
      const unsigned bit = 7u - column;
      for (unsigned plane = 0u; plane < 4u; ++plane) {
        planes[plane] = static_cast<std::uint8_t>(
            planes[plane] | (((rows[row][column] >> plane) & 1u) << bit));
      }
    }
    ppu.vram[(byteAddress + row * 2u) & 0xFFFFu] = planes[0];
    ppu.vram[(byteAddress + row * 2u + 1u) & 0xFFFFu] = planes[1];
    ppu.vram[(byteAddress + 16u + row * 2u) & 0xFFFFu] = planes[2];
    ppu.vram[(byteAddress + 16u + row * 2u + 1u) & 0xFFFFu] = planes[3];
  }
}

// The same, for a character the base above holds: 32 bytes a character at four
// bitplanes.
void putTile(PpuState& ppu, unsigned tile, const TileRows& rows) {
  putTileAt(ppu, kCharByte + tile * 32u, rows);
}

// A character every pixel of which is one colour index.
void putSolidTile(PpuState& ppu, unsigned tile, std::uint8_t index) {
  TileRows rows{};
  for (auto& row : rows) row.fill(index);
  putTile(ppu, tile, rows);
}

// A PPU placed to draw: Mode 1 with 8x8 tiles, BG1 alone on the main screen at full
// brightness, the bases above, the backdrop in palette word 0, and the four colour
// indices these cases name.
//
// The vertical offset is the $3FF games write, which is -1 in the ten bits the
// offset keeps: the console outputs no scanline 0, so an offset of -1 is what puts
// the tilemap's first row on the first line of the picture.
PpuState screen() {
  PpuState ppu;
  ppu.inidisp = 0x0Fu;  // the screen on, brightness 15
  ppu.bgmode = 0x01u;   // Mode 1, every layer in 8x8 tiles
  ppu.bg1sc = kMapBase;
  ppu.bg12nba = kCharBase;
  ppu.tm = 0x01u;  // BG1 on the main screen
  ppu.bg1vofs = 0x3FFu;
  putColour(ppu, 0u, kBackdrop);
  putColour(ppu, 1u, kRed);
  putColour(ppu, 2u, kGreen);
  putColour(ppu, 4u, kBlue);
  putColour(ppu, 8u, kWhite);
  putColour(ppu, 49u, kYellow);  // palette 3's colour 1
  return ppu;
}

// Whether two machines are in the same state, piece by piece: the CPU's registers
// and its progress through an instruction, work RAM and the save, the audio
// machine's memory, every PPU register and the three video memories, the beam and
// the counters, and the interrupt, transfer and controller registers.
[[nodiscard]] ::testing::AssertionResult sameState(const SnesState& a, const SnesState& b) {
  const auto differs = [](const char* what) {
    return ::testing::AssertionFailure() << what << " differs";
  };
  if (a.cpu.pc != b.cpu.pc || a.cpu.s != b.cpu.s || a.cpu.a != b.cpu.a || a.cpu.x != b.cpu.x ||
      a.cpu.y != b.cpu.y || a.cpu.d != b.cpu.d || a.cpu.p != b.cpu.p || a.cpu.dbr != b.cpu.dbr ||
      a.cpu.pbr != b.cpu.pbr || a.cpu.e != b.cpu.e || a.cpu.ir != b.cpu.ir ||
      a.cpu.tcu != b.cpu.tcu || a.cpu.ea != b.cpu.ea || a.cpu.ptr != b.cpu.ptr ||
      a.cpu.tmp != b.cpu.tmp || a.cpu.nmiPending != b.cpu.nmiPending ||
      a.cpu.irqLine != b.cpu.irqLine || a.cpu.servicing != b.cpu.servicing) {
    return differs("the CPU");
  }
  if (a.wram != b.wram) return differs("work RAM");
  if (a.sram != b.sram) return differs("the save");
  if (a.apu.ram != b.apu.ram) return differs("the audio machine's memory");
  if (!(a.ppu == b.ppu)) return differs("the PPU");
  if (a.master != b.master || a.consumed != b.consumed || a.apuPhase != b.apuPhase) {
    return differs("the master counters");
  }
  if (a.hpos != b.hpos || a.vpos != b.vpos || a.field != b.field || a.inVblank != b.inVblank ||
      a.vblankBeginLine != b.vblankBeginLine || a.previousLineMaster != b.previousLineMaster ||
      a.refreshAt != b.refreshAt || a.refreshLeft != b.refreshLeft) {
    return differs("the beam");
  }
  if (a.nmitimen != b.nmitimen || a.vblankNmi != b.vblankNmi || a.timeup != b.timeup ||
      a.htime != b.htime || a.vtime != b.vtime) {
    return differs("the interrupt registers");
  }
  if (a.mdr != b.mdr || a.memsel != b.memsel || a.wmadd != b.wmadd) return differs("the bus");
  if (a.pads != b.pads || a.joy != b.joy || a.joyLatch != b.joyLatch ||
      a.joyClocks != b.joyClocks || a.autoJoyClocks != b.autoJoyClocks ||
      a.joyStrobe != b.joyStrobe || a.wrio != b.wrio) {
    return differs("the controller ports");
  }
  if (a.mdmaen != b.mdmaen || a.hdmaen != b.hdmaen || a.hdmaActive != b.hdmaActive ||
      a.hdmaInited != b.hdmaInited) {
    return differs("the transfer engines");
  }
  return ::testing::AssertionSuccess();
}

// ---- the seam ----------------------------------------------------------------

TEST(SnesPpuPicture, AFrameArrivesAsTheBeamWrapsToTheNextFrame) {
  const Picture one = draw(screen());
  EXPECT_EQ(one.frames, 1u);
  const Picture two = draw(screen(), kTwoFrames);
  EXPECT_EQ(two.frames, 2u);
}

TEST(SnesPpuPicture, TheFrameIsTwoHundredAndFiftySixAcrossAndTwoHundredAndTwentyFourDown) {
  const Picture picture = draw(screen());
  ASSERT_EQ(picture.frames, 1u);
  EXPECT_EQ(picture.width, 256u);
  EXPECT_EQ(picture.height, 224u);
  EXPECT_EQ(picture.pixels.size(), 256u * 224u * 4u);
}

TEST(SnesPpuPicture, TheTallerPictureIsTwoHundredAndThirtyNineDown) {
  PpuState ppu = screen();
  ppu.setini = 0x04u;  // SETINI bit 2: vertical blank begins on line 240
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.frames, 1u);
  EXPECT_EQ(picture.width, 256u);
  EXPECT_EQ(picture.height, 239u);
  EXPECT_EQ(picture.pixels.size(), 256u * 239u * 4u);
}

TEST(SnesPpuPicture, EachFrameCarriesTheParityItRanUnder) {
  // The parity toggles at H = 1 of every frame's first line, and the console comes
  // out of reset with it clear — so the first frame it runs is the odd one.
  const Picture picture = draw(screen(), kTwoFrames);
  ASSERT_EQ(picture.frames, 2u);
  EXPECT_EQ(picture.fields.at(0), 1u);
  EXPECT_EQ(picture.fields.at(1), 0u);
}

TEST(SnesPpuPicture, AMachineNobodyWatchesRunsExactlyAsOneBeingWatchedDoes) {
  const std::vector<std::uint8_t> rom = haltedCartridge();
  Snes watched(SnesConfig{.rom = rom});
  Snes unwatched(SnesConfig{.rom = rom});
  PpuState ppu = screen();
  putEntry(ppu, 0u, 0x0001u);
  putSolidTile(ppu, 1u, 1u);
  SnesState placed = watched.state();
  placed.ppu = ppu;
  watched.restore(placed);
  unwatched.restore(placed);

  Picture picture;
  watched.setFrameObserver(&picture);
  watched.run(kOneFrame);
  unwatched.run(kOneFrame);

  ASSERT_EQ(picture.frames, 1u);
  EXPECT_TRUE(sameState(watched.state(), unwatched.state()));
  EXPECT_EQ(unwatched.frameObserver(), nullptr);
}

TEST(SnesPpuPicture, ASnapshotDoesNotCarryTheFrameObserver) {
  const std::vector<std::uint8_t> rom = haltedCartridge();
  Snes machine(SnesConfig{.rom = rom});
  Picture picture;
  machine.setFrameObserver(&picture);
  const SnesState state = machine.state();
  machine.restore(state);
  EXPECT_EQ(machine.frameObserver(), &picture);
}

// ---- the converter -----------------------------------------------------------

TEST(SnesPpuPicture, TheBackdropShowsWhereNoLayerDraws) {
  PpuState ppu = screen();
  ppu.tm = 0u;  // nothing on the main screen
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.frames, 1u);
  EXPECT_EQ(picture.at(0u, 0u), kBackdropOut);
  EXPECT_EQ(picture.at(255u, 223u), kBackdropOut);
}

TEST(SnesPpuPicture, ForcedBlankIsABlackFrame) {
  PpuState ppu = screen();
  putEntry(ppu, 0u, 0x0001u);
  putSolidTile(ppu, 1u, 1u);
  ppu.inidisp = 0x8Fu;  // forced blank, brightness 15
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.frames, 1u);
  EXPECT_EQ(picture.at(0u, 0u), kBlack);
  EXPECT_EQ(picture.at(255u, 223u), kBlack);
}

TEST(SnesPpuPicture, BrightnessZeroIsABlackFrame) {
  PpuState ppu = screen();
  putEntry(ppu, 0u, 0x0001u);
  putSolidTile(ppu, 1u, 1u);
  ppu.inidisp = 0x00u;  // the screen on, brightness 0, which is off
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.frames, 1u);
  EXPECT_EQ(picture.at(0u, 0u), kBlack);
  EXPECT_EQ(picture.at(255u, 223u), kBlack);
}

TEST(SnesPpuPicture, BrightnessScalesEveryChannel) {
  // At brightness 7 the scale is 8/16, so 31 gives round(31 * 8 * 255 / 496) = 128 —
  // an exact half, which is rounded up — and the backdrop's 1, 2 and 3 give 4, 8
  // and 12.
  PpuState ppu = screen();
  putEntry(ppu, 0u, 0x0008u);  // a tile in colour index 8, which is white
  putSolidTile(ppu, 8u, 8u);
  ppu.inidisp = 0x07u;
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.frames, 1u);
  EXPECT_EQ(picture.at(0u, 0u), (Rgba{128u, 128u, 128u, 255u}));
  EXPECT_EQ(picture.at(255u, 0u), (Rgba{4u, 8u, 12u, 255u}));
}

// ---- the character data ------------------------------------------------------

TEST(SnesPpuPicture, TheLeftmostPixelOfARowIsBitSevenAndThePlanesAreTwoWordsApart) {
  // Row 0 of the character written plane by plane, each plane's bit one place
  // further right: plane 0 is the index's low bit and lies in the row's first byte,
  // plane 1 in its second, and planes 2 and 3 sixteen bytes on in the same order.
  PpuState ppu = screen();
  putEntry(ppu, 0u, 0x0001u);
  const std::uint32_t tile = kCharByte + 32u;
  ppu.vram[tile] = 0x80u;         // plane 0, the leftmost pixel: index 1
  ppu.vram[tile + 1u] = 0x40u;    // plane 1, one pixel right: index 2
  ppu.vram[tile + 16u] = 0x20u;   // plane 2, two right: index 4
  ppu.vram[tile + 17u] = 0x10u;   // plane 3, three right: index 8
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.frames, 1u);
  EXPECT_EQ(picture.at(0u, 0u), kRedOut);
  EXPECT_EQ(picture.at(1u, 0u), kGreenOut);
  EXPECT_EQ(picture.at(2u, 0u), kBlueOut);
  EXPECT_EQ(picture.at(3u, 0u), kWhiteOut);
}

TEST(SnesPpuPicture, ColourZeroOfATileIsTransparentAndTheBackdropShowsThrough) {
  // The tile is in palette 3, whose own colour 0 is word 48 — a colour of its own,
  // and not the backdrop. So the hole shows the backdrop because colour 0 is
  // transparent, not because the two words happen to hold the same colour.
  PpuState ppu = screen();
  putColour(ppu, 48u, kWhite);  // palette 3's colour 0, which nothing may ever show
  putEntry(ppu, 0u, static_cast<std::uint16_t>((3u << 10) | 1u));
  TileRows rows{};
  for (auto& row : rows) row.fill(1u);
  rows[2][3] = 0u;  // one hole in an otherwise solid character
  putTile(ppu, 1u, rows);
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.frames, 1u);
  EXPECT_EQ(picture.at(0u, 0u), kYellowOut);
  EXPECT_EQ(picture.at(3u, 2u), kBackdropOut);
}

TEST(SnesPpuPicture, ThePaletteBitsChooseSixteenColoursApart) {
  // Palette 3 of a 16-colour background begins at word 3 * 16 = 48, so its colour 1
  // is word 49.
  PpuState ppu = screen();
  putEntry(ppu, 0u, static_cast<std::uint16_t>((3u << 10) | 1u));
  putSolidTile(ppu, 1u, 1u);
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.frames, 1u);
  EXPECT_EQ(picture.at(0u, 0u), kYellowOut);
}

TEST(SnesPpuPicture, TheCharacterBaseAndTheTileNumberNameTheCharacter) {
  // $210B holding 2 puts BG1's characters at 2 << 13 = $4000, and tile 3 is 32 bytes
  // a character on from there.
  PpuState ppu = screen();
  ppu.bg12nba = 0x02u;
  putEntry(ppu, 0u, 0x0003u);
  TileRows rows{};
  for (auto& row : rows) row.fill(2u);
  putTileAt(ppu, 0x4000u + 3u * 32u, rows);
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.frames, 1u);
  EXPECT_EQ(picture.at(0u, 0u), kGreenOut);
}

// ---- the tilemap -------------------------------------------------------------

TEST(SnesPpuPicture, TheTilemapEntryForAPositionIsRowsOfThirtyTwo) {
  // The entry for tile (X, Y) of one screen is ((Y & 0x1F) << 5) + (X & 0x1F) words
  // into the map, so tile (1, 1) covers the picture from (8, 8).
  PpuState ppu = screen();
  putEntry(ppu, 1u * 32u + 1u, 0x0001u);
  putSolidTile(ppu, 1u, 1u);
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.frames, 1u);
  EXPECT_EQ(picture.at(8u, 8u), kRedOut);
  EXPECT_EQ(picture.at(15u, 15u), kRedOut);
  EXPECT_EQ(picture.at(7u, 8u), kBackdropOut);
  EXPECT_EQ(picture.at(8u, 7u), kBackdropOut);
}

TEST(SnesPpuPicture, TheScreenBaseStepsAWholeScreenAtATime) {
  // $2107's six base bits count whole 32x32 screens — $400 words, or 2 KB — so base
  // 1 is word $400 and not the $200 a half-screen step would give. A tile is placed
  // at each, and the picture shows the one the base names.
  PpuState ppu = screen();  // base 1
  putEntry(ppu, 0u, 0x0001u);
  ppu.vram[0x0400u] = 0x02u;  // where a half-screen step would look: word $200
  ppu.vram[0x0401u] = 0x00u;
  putSolidTile(ppu, 1u, 1u);
  putSolidTile(ppu, 2u, 2u);
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.frames, 1u);
  EXPECT_EQ(picture.at(0u, 0u), kRedOut);
}

TEST(SnesPpuPicture, ASingleScreenMapRepeatsEveryTwoHundredAndFiftySixPixels) {
  PpuState ppu = screen();  // size 00: one 32x32 screen
  putEntry(ppu, 0u, 0x0001u);                 // screen A's first tile
  putEntry(ppu, kScreenEntries, 0x0002u);     // where a second screen would begin
  putSolidTile(ppu, 1u, 1u);
  putSolidTile(ppu, 2u, 2u);
  ppu.bg1hofs = 256u;
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.frames, 1u);
  EXPECT_EQ(picture.at(0u, 0u), kRedOut);  // screen A again, not the bytes beyond it
}

TEST(SnesPpuPicture, TheWideMapPutsItsSecondScreenToTheRight) {
  PpuState ppu = screen();
  ppu.bg1sc = static_cast<std::uint8_t>(kMapBase | 0x01u);  // 64x32
  putEntry(ppu, 0u, 0x0001u);
  putEntry(ppu, kScreenEntries, 0x0002u);
  putSolidTile(ppu, 1u, 1u);
  putSolidTile(ppu, 2u, 2u);
  ppu.bg1hofs = 256u;
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.frames, 1u);
  EXPECT_EQ(picture.at(0u, 0u), kGreenOut);
}

TEST(SnesPpuPicture, TheTallMapPutsItsSecondScreenBelow) {
  PpuState ppu = screen();
  ppu.bg1sc = static_cast<std::uint8_t>(kMapBase | 0x02u);  // 32x64
  putEntry(ppu, 0u, 0x0001u);
  putEntry(ppu, kScreenEntries, 0x0002u);
  putSolidTile(ppu, 1u, 1u);
  putSolidTile(ppu, 2u, 2u);
  ppu.bg1vofs = 255u;  // the first line of the picture is row 256 of the background
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.frames, 1u);
  EXPECT_EQ(picture.at(0u, 0u), kGreenOut);
}

TEST(SnesPpuPicture, TheLargeMapPutsItsScreensRightAndBelow) {
  // 64x64 lays the four screens A B over C D, so moving 32 tiles down skips two of
  // them: (Y & 0x20) << 6 words rather than << 5.
  PpuState ppu = screen();
  ppu.bg1sc = static_cast<std::uint8_t>(kMapBase | 0x03u);
  putEntry(ppu, 0u, 0x0001u);                      // A
  putEntry(ppu, kScreenEntries, 0x0002u);          // B, to the right
  putEntry(ppu, 2u * kScreenEntries, 0x0004u);     // C, below
  putSolidTile(ppu, 1u, 1u);
  putSolidTile(ppu, 2u, 2u);
  putSolidTile(ppu, 4u, 4u);

  PpuState right = ppu;
  right.bg1hofs = 256u;
  const Picture across = draw(right);
  ASSERT_EQ(across.frames, 1u);
  EXPECT_EQ(across.at(0u, 0u), kGreenOut);

  PpuState below = ppu;
  below.bg1vofs = 255u;
  const Picture down = draw(below);
  ASSERT_EQ(down.frames, 1u);
  EXPECT_EQ(down.at(0u, 0u), kBlueOut);
}

// ---- the flips ---------------------------------------------------------------

TEST(SnesPpuPicture, AHorizontalFlipReversesTheRow) {
  PpuState ppu = screen();
  putEntry(ppu, 0u, 0x4001u);  // bit 14: flip horizontally
  TileRows rows{};
  rows[0][0] = 1u;
  putTile(ppu, 1u, rows);
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.frames, 1u);
  EXPECT_EQ(picture.at(7u, 0u), kRedOut);
  EXPECT_EQ(picture.at(0u, 0u), kBackdropOut);
}

TEST(SnesPpuPicture, AVerticalFlipReversesTheColumn) {
  PpuState ppu = screen();
  putEntry(ppu, 0u, 0x8001u);  // bit 15: flip vertically
  TileRows rows{};
  rows[0][0] = 1u;
  putTile(ppu, 1u, rows);
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.frames, 1u);
  EXPECT_EQ(picture.at(0u, 7u), kRedOut);
  EXPECT_EQ(picture.at(0u, 0u), kBackdropOut);
}

// ---- 16x16 blocks ------------------------------------------------------------

TEST(SnesPpuPicture, ABlockIsTheTileAndTheThreeAfterItByOneAndSixteen) {
  PpuState ppu = screen();
  ppu.bgmode = 0x11u;  // Mode 1, and $2105 bit 4: BG1 in 16x16 blocks
  putEntry(ppu, 0u, 0x0001u);
  putSolidTile(ppu, 1u, 1u);    // the block's top left
  putSolidTile(ppu, 2u, 2u);    // Tile + 1, its top right
  putSolidTile(ppu, 17u, 4u);   // Tile + 16, its bottom left
  putSolidTile(ppu, 18u, 8u);   // Tile + 17, its bottom right
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.frames, 1u);
  EXPECT_EQ(picture.at(0u, 0u), kRedOut);
  EXPECT_EQ(picture.at(8u, 0u), kGreenOut);
  EXPECT_EQ(picture.at(0u, 8u), kBlueOut);
  EXPECT_EQ(picture.at(8u, 8u), kWhiteOut);
}

TEST(SnesPpuPicture, ABlocksNumbersRunOnRatherThanWrappingWithinIt) {
  // Tile $2FF gives $2FF, $300, $30F and $310 — only the ten-bit total wraps.
  PpuState ppu = screen();
  ppu.bgmode = 0x11u;
  putEntry(ppu, 0u, 0x02FFu);
  putSolidTile(ppu, 0x2FFu, 1u);
  putSolidTile(ppu, 0x300u, 2u);
  putSolidTile(ppu, 0x30Fu, 4u);
  putSolidTile(ppu, 0x310u, 8u);
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.frames, 1u);
  EXPECT_EQ(picture.at(0u, 0u), kRedOut);
  EXPECT_EQ(picture.at(8u, 0u), kGreenOut);
  EXPECT_EQ(picture.at(0u, 8u), kBlueOut);
  EXPECT_EQ(picture.at(8u, 8u), kWhiteOut);
}

TEST(SnesPpuPicture, ABlockFlipsWholeRatherThanByItsParts) {
  PpuState ppu = screen();
  ppu.bgmode = 0x11u;
  putEntry(ppu, 0u, 0xC001u);  // both flips
  putSolidTile(ppu, 1u, 1u);
  putSolidTile(ppu, 2u, 2u);
  putSolidTile(ppu, 17u, 4u);
  putSolidTile(ppu, 18u, 8u);
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.frames, 1u);
  EXPECT_EQ(picture.at(0u, 0u), kWhiteOut);  // what was the block's bottom right
  EXPECT_EQ(picture.at(8u, 0u), kBlueOut);
  EXPECT_EQ(picture.at(0u, 8u), kGreenOut);
  EXPECT_EQ(picture.at(8u, 8u), kRedOut);
}

// ---- scrolling ---------------------------------------------------------------

TEST(SnesPpuPicture, ScrollingMovesThePictureByWholeTiles) {
  PpuState ppu = screen();
  putEntry(ppu, 1u, 0x0001u);  // the tile at (1, 0)
  putSolidTile(ppu, 1u, 1u);
  ppu.bg1hofs = 8u;
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.frames, 1u);
  EXPECT_EQ(picture.at(0u, 0u), kRedOut);
  EXPECT_EQ(picture.at(7u, 0u), kRedOut);
  EXPECT_EQ(picture.at(8u, 0u), kBackdropOut);
}

TEST(SnesPpuPicture, ScrollingMovesThePictureWithinATile) {
  PpuState ppu = screen();
  putEntry(ppu, 0u, 0x0001u);
  TileRows rows{};
  rows[0][3] = 1u;  // one pixel, four columns in
  putTile(ppu, 1u, rows);
  ppu.bg1hofs = 3u;
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.frames, 1u);
  EXPECT_EQ(picture.at(0u, 0u), kRedOut);
}

TEST(SnesPpuPicture, ThePictureBeginsOnTheSecondRowOfTheTileWithNoVerticalScroll) {
  // The console outputs no scanline 0, so with the offset at 0 the picture's first
  // line is the background's row 1 — which is why games write -1 into it.
  PpuState ppu = screen();
  ppu.bg1vofs = 0u;
  putEntry(ppu, 0u, 0x0001u);
  TileRows rows{};
  rows[1][0] = 1u;  // the tile's second row
  putTile(ppu, 1u, rows);
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.frames, 1u);
  EXPECT_EQ(picture.at(0u, 0u), kRedOut);
}

// ---- what this block does not draw -------------------------------------------

TEST(SnesPpuPicture, ALayerEnabledOnlyOnTheSubScreenDrawsNothing) {
  PpuState ppu = screen();
  ppu.tm = 0x00u;
  ppu.ts = 0x01u;  // BG1 on the sub screen alone
  putEntry(ppu, 0u, 0x0001u);
  putSolidTile(ppu, 1u, 1u);
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.frames, 1u);
  EXPECT_EQ(picture.at(0u, 0u), kBackdropOut);
}

TEST(SnesPpuPicture, BothTilePrioritiesShowOverTheBackdrop) {
  PpuState ppu = screen();
  putEntry(ppu, 0u, 0x2001u);  // bit 13: tile priority 1
  putEntry(ppu, 1u, 0x0002u);  // the tile beside it at priority 0
  putSolidTile(ppu, 1u, 1u);
  putSolidTile(ppu, 2u, 2u);
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.frames, 1u);
  EXPECT_EQ(picture.at(0u, 0u), kRedOut);
  EXPECT_EQ(picture.at(8u, 0u), kGreenOut);
}

TEST(SnesPpuPicture, AModeThisBlockDoesNotDrawShowsTheBackdrop) {
  PpuState ppu = screen();
  ppu.bgmode = 0x03u;  // Mode 3, whose BG1 is 256 colours
  putEntry(ppu, 0u, 0x0001u);
  putSolidTile(ppu, 1u, 1u);
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.frames, 1u);
  EXPECT_EQ(picture.at(0u, 0u), kBackdropOut);
}

// ---- the whole path ----------------------------------------------------------

TEST(SnesPpuPicture, ACartridgeThatWritesTheVideoMemoriesShowsItsPicture) {
  // The program runs in forced blank, which is when the memories are reachable: it
  // fills one character with colour index 1, points the map's first entry at it,
  // writes the backdrop and that colour, sets the bases and the mode, and then
  // turns the screen on.
  std::vector<std::uint8_t> program;
  const auto store = [&program](std::uint8_t value, std::uint16_t port) {
    program.insert(program.end(), {kLdaImm, value, kStaAbs, static_cast<std::uint8_t>(port & 0xFFu),
                                   static_cast<std::uint8_t>(port >> 8)});
  };
  store(0x80u, 0x2100u);  // forced blank, so VRAM and the palette are reachable
  store(0x80u, 0x2115u);  // VMAIN: the address steps after the high byte
  store(kMapBase, 0x2107u);
  store(kCharBase, 0x210Bu);
  store(0x01u, 0x2105u);  // Mode 1, 8x8 tiles
  store(0x01u, 0x212Cu);  // BG1 on the main screen

  // Character 1, at byte $8020, which is word $4010: eight words of $FF fill planes
  // 0 and 1, and the sixteen bytes of planes 2 and 3 stay zero — every pixel index 3.
  store(0x10u, 0x2116u);
  store(0x40u, 0x2117u);
  for (unsigned word = 0u; word < 8u; ++word) {
    store(0xFFu, 0x2118u);
    store(0xFFu, 0x2119u);
  }
  // The map's first entry, at word $400 — base 1 of $400 words: character 1,
  // palette 0, no flip. Every other entry stays zero, which names character 0 — and
  // nothing was written there, so those tiles are transparent and the backdrop shows.
  store(0x00u, 0x2116u);
  store(0x04u, 0x2117u);
  store(0x01u, 0x2118u);
  store(0x00u, 0x2119u);
  // The palette: word 0 the backdrop, word 3 red.
  store(0x00u, 0x2121u);
  store(static_cast<std::uint8_t>(kBackdrop & 0xFFu), 0x2122u);
  store(static_cast<std::uint8_t>(kBackdrop >> 8), 0x2122u);
  store(0x03u, 0x2121u);
  store(static_cast<std::uint8_t>(kRed & 0xFFu), 0x2122u);
  store(static_cast<std::uint8_t>(kRed >> 8), 0x2122u);
  store(0x0Fu, 0x2100u);  // the screen on at full brightness
  program.push_back(kStp);

  const std::vector<std::uint8_t> rom = cartridge(program);
  Snes machine(SnesConfig{.rom = rom});
  Picture picture;
  picture.keep = 2;  // the first frame was still in forced blank while this ran
  machine.setFrameObserver(&picture);
  machine.run(kTwoFrames);
  ASSERT_EQ(picture.frames, 2u);
  // The character covers the map's first entry and the program writes no scroll, so
  // its rows 1 to 7 are the picture's first seven lines: the console outputs no
  // scanline 0.
  EXPECT_EQ(picture.at(0u, 0u), kRedOut);
  EXPECT_EQ(picture.at(7u, 6u), kRedOut);
  EXPECT_EQ(picture.at(0u, 7u), kBackdropOut);
  EXPECT_EQ(picture.at(8u, 0u), kBackdropOut);
}

}  // namespace
}  // namespace snaggletooth
