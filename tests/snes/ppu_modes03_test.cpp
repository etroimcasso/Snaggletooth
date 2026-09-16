// The two modes this block adds and the colour they can be read in: Mode 0's four
// four-colour backgrounds with a palette subset each, Mode 3's 256-colour BG1 over
// a sixteen-colour BG2, the priority order each mode keeps, the register that
// exchanges one of Mode 1's places and does nothing in either of these, and direct
// colour — a 256-colour background's pixel read as a colour rather than as an index.
//
// Every expectation is computed by hand from the register page and the priority
// chart; the pictures are placed as a program would have left them. The last group
// is the grid a cartridge of ours draws, cell for cell, so the picture an emulator
// is asked for and the picture this suite asks for are the same arithmetic.

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <system_error>
#include <vector>

#include "gtest/gtest.h"
#include "snaggletooth/snes/snes.h"

namespace snaggletooth {
namespace {

// Where these pictures keep their pieces, each clear of the others. A map base
// counts whole 32x32 screens of $400 words, so one screen of map is $800 bytes; a
// background's character base counts 8 KB blocks; a sprite's counts 8 K-word blocks.
constexpr std::uint8_t kBg1MapBase = 0x04u;  // $2107: screen 1, size 00
constexpr std::uint8_t kBg2MapBase = 0x08u;  // $2108: screen 2
constexpr std::uint8_t kBg3MapBase = 0x0Cu;  // $2109: screen 3
constexpr std::uint8_t kBg4MapBase = 0x10u;  // $210A: screen 4
constexpr std::uint8_t kCharBases12 = 0x54u; // $210B: BG1 at $8000, BG2 at $A000
constexpr std::uint8_t kCharBases34 = 0x23u; // $210C: BG3 at $6000, BG4 at $4000
constexpr std::uint8_t kSpriteBase = 0x03u;  // $2101: the first table at byte $C000

constexpr std::uint32_t kBg1Map = 0x0800u;
constexpr std::uint32_t kBg2Map = 0x1000u;
constexpr std::uint32_t kBg3Map = 0x1800u;
constexpr std::uint32_t kBg4Map = 0x2000u;
constexpr std::uint32_t kBg1Chars = 0x8000u;
constexpr std::uint32_t kBg2Chars = 0xA000u;
constexpr std::uint32_t kBg3Chars = 0x6000u;
constexpr std::uint32_t kBg4Chars = 0x4000u;
constexpr std::uint32_t kSpriteChars = 0xC000u;

// A sprite's character is always sixteen colours, and its eight palettes begin at
// CGRAM word 128.
constexpr unsigned kSpritePlanes = 4u;
constexpr unsigned kSpritePaletteBase = 128u;

// Mode 0 gives each background thirty-two words of its own, in the order the
// backgrounds are numbered.
constexpr unsigned kModeZeroSubset = 32u;

using Rgba = std::array<std::uint8_t, 4>;

// The byte the converter drives for one five-bit channel at full brightness: the
// exact value * 255 / 31, rounded once. Written here from the register page's own
// arithmetic; the four values the other picture suites compute by hand — 31 giving
// 255, 3 giving 25, 2 giving 16 and 1 giving 8 — are asserted against it below.
constexpr std::uint8_t channel(unsigned value) {
  return static_cast<std::uint8_t>((value * 255u + 15u) / 31u);
}

// What the converter drives for a five-bit triple, and for a whole palette word.
constexpr Rgba rgb(unsigned red, unsigned green, unsigned blue) {
  return Rgba{channel(red), channel(green), channel(blue), 255u};
}
constexpr Rgba out(std::uint16_t word) {
  return rgb(word & 0x1Fu, (word >> 5) & 0x1Fu, (word >> 10) & 0x1Fu);
}

// The palette words these cases use. A word is 15 bits, blue-green-red from the top.
constexpr std::uint16_t kBackdrop = 0x0C41u;  // red 1, green 2, blue 3
constexpr std::uint16_t kRed = 0x001Fu;
constexpr std::uint16_t kGreen = 0x03E0u;
constexpr std::uint16_t kBlue = 0x7C00u;
constexpr std::uint16_t kWhite = 0x7FFFu;
constexpr std::uint16_t kYellow = 0x03FFu;
constexpr std::uint16_t kCyan = 0x7FE0u;
constexpr std::uint16_t kMagenta = 0x7C1Fu;
constexpr std::uint16_t kGrey = 0x35ADu;  // red 13, green 13, blue 13 — odd in each

constexpr Rgba kBlack{0u, 0u, 0u, 255u};

// The frames a run finished, the first kept whole.
struct Picture final : FrameObserver {
  unsigned frames = 0;
  unsigned width = 0;
  unsigned height = 0;
  std::vector<std::uint8_t> pixels;

  void frame(const VideoFrame& picture) override {
    ++frames;
    if (frames != 1u) return;
    width = picture.width;
    height = picture.height;
    pixels.assign(picture.pixels.begin(), picture.pixels.end());
  }

  [[nodiscard]] Rgba at(unsigned x, unsigned y) const {
    const std::size_t index = (static_cast<std::size_t>(y) * width + x) * 4u;
    return Rgba{pixels.at(index), pixels.at(index + 1u), pixels.at(index + 2u),
                pixels.at(index + 3u)};
  }
};

constexpr std::uint8_t kStp = 0xDBu;

std::vector<std::uint8_t> haltedCartridge() {
  std::vector<std::uint8_t> program{kStp};
  program.resize(0x8000u, 0x00u);
  program[0x7FFCu] = 0x00u;  // reset -> $8000
  program[0x7FFDu] = 0x80u;
  return program;
}

// More than an NTSC frame of master cycles, so a run from power-on reaches the line
// a finished frame is handed over on.
constexpr std::uint64_t kOneFrame = 357364u + 20000u;

Picture draw(const PpuState& ppu) {
  const std::vector<std::uint8_t> rom = haltedCartridge();
  Snes machine(SnesConfig{.rom = rom});
  SnesState state = machine.state();
  state.ppu = ppu;
  machine.restore(state);
  Picture picture;
  machine.setFrameObserver(&picture);
  machine.run(kOneFrame);
  return picture;
}

void putColour(PpuState& ppu, unsigned word, std::uint16_t colour) {
  ppu.cgram[word * 2u] = static_cast<std::uint8_t>(colour & 0xFFu);
  ppu.cgram[word * 2u + 1u] = static_cast<std::uint8_t>(colour >> 8);
}

// A tilemap entry at a tile position: vhopppcc cccccccc.
void putEntry(PpuState& ppu, std::uint32_t map, unsigned tileX, unsigned tileY,
              std::uint16_t entry) {
  const std::uint32_t at = map + (tileY * 32u + tileX) * 2u;
  ppu.vram[at & 0xFFFFu] = static_cast<std::uint8_t>(entry & 0xFFu);
  ppu.vram[(at + 1u) & 0xFFFFu] = static_cast<std::uint8_t>(entry >> 8);
}

constexpr std::uint16_t entry(unsigned tile, unsigned palette, bool priority) {
  return static_cast<std::uint16_t>(tile | (palette << 10) | (priority ? 0x2000u : 0u));
}

using TileRows = std::array<std::array<std::uint8_t, 8>, 8>;

// A character at a byte address at as many bitplanes as the caller names: planes 0
// and 1 in the low and high bytes of eight words, every further pair sixteen bytes
// on, the leftmost pixel of a row in bit 7.
void putTileAt(PpuState& ppu, std::uint32_t byteAddress, unsigned planes,
               const TileRows& rows) {
  for (unsigned row = 0u; row < 8u; ++row) {
    for (unsigned plane = 0u; plane < planes; ++plane) {
      std::uint8_t bits = 0u;
      for (unsigned column = 0u; column < 8u; ++column) {
        bits = static_cast<std::uint8_t>(
            bits | (((rows[row][column] >> plane) & 1u) << (7u - column)));
      }
      const std::uint32_t at = byteAddress + (plane / 2u) * 16u + (plane % 2u) + row * 2u;
      ppu.vram[at & 0xFFFFu] = bits;
    }
  }
}

// A character every pixel of which is one colour index.
void putSolid(PpuState& ppu, std::uint32_t characters, unsigned planes, unsigned tile,
              std::uint8_t index) {
  TileRows rows{};
  for (auto& row : rows) row.fill(index);
  putTileAt(ppu, characters + tile * 8u * planes, planes, rows);
}

void putSprite(PpuState& ppu, unsigned index, int x, std::uint8_t y, std::uint8_t tile,
               std::uint8_t attributes) {
  const unsigned wide = static_cast<unsigned>(x & 0x1FF);
  ppu.oam[index * 4u] = static_cast<std::uint8_t>(wide & 0xFFu);
  ppu.oam[index * 4u + 1u] = y;
  ppu.oam[index * 4u + 2u] = tile;
  ppu.oam[index * 4u + 3u] = attributes;
  const unsigned shift = (index & 3u) * 2u;
  auto& byte = ppu.oam[512u + index / 4u];
  byte = static_cast<std::uint8_t>((byte & ~(3u << shift)) | (((wide >> 8) & 1u) << shift));
}

// Every sprite parked off the left edge, where Range keeps none of them.
void parkSprites(PpuState& ppu) {
  for (unsigned index = 0u; index < kSprites; ++index) putSprite(ppu, index, -64, 0u, 0u, 0u);
}

constexpr std::uint8_t attributes(unsigned priority, unsigned palette) {
  return static_cast<std::uint8_t>((priority << 4) | (palette << 1));
}

// A sprite at picture line 41, covering the eight pixels from x: a sprite whose Y is
// N first draws on line N + 1.
void spriteAt(PpuState& ppu, unsigned index, int x, unsigned priority) {
  putSprite(ppu, index, x, 40u, 0u, attributes(priority, 0u));
  putTileAt(ppu, kSpriteChars, kSpritePlanes, [] {
    TileRows rows{};
    for (auto& row : rows) row.fill(1u);
    return rows;
  }());
}

// Picture line 8t + 1 shows tile row t, its first pixel row: the offset every
// background carries here is -1 in the ten bits the register keeps, which is what
// puts a map's first row on the picture's first line.
constexpr unsigned lineOf(unsigned tileRow) { return tileRow * 8u + 1u; }

// A PPU at full brightness with the four backgrounds placed, nothing on either
// screen yet, and the palette words these cases read.
PpuState placed() {
  PpuState ppu;
  ppu.inidisp = 0x0Fu;
  ppu.objsel = kSpriteBase;
  ppu.bg1sc = kBg1MapBase;
  ppu.bg2sc = kBg2MapBase;
  ppu.bg3sc = kBg3MapBase;
  ppu.bg4sc = kBg4MapBase;
  ppu.bg12nba = kCharBases12;
  ppu.bg34nba = kCharBases34;
  ppu.bg1vofs = 0x3FFu;
  ppu.bg2vofs = 0x3FFu;
  ppu.bg3vofs = 0x3FFu;
  ppu.bg4vofs = 0x3FFu;
  putColour(ppu, 0u, kBackdrop);
  parkSprites(ppu);
  return ppu;
}

// Mode 0 with its four backgrounds shown, each carrying one solid character and
// each reading colour 1 of its own subset's first palette.
PpuState modeZero() {
  PpuState ppu = placed();
  ppu.bgmode = 0x00u;
  ppu.tm = 0x0Fu;  // all four backgrounds on the main screen
  putSolid(ppu, kBg1Chars, 2u, 1u, 1u);
  putSolid(ppu, kBg2Chars, 2u, 1u, 1u);
  putSolid(ppu, kBg3Chars, 2u, 1u, 1u);
  putSolid(ppu, kBg4Chars, 2u, 1u, 1u);
  putColour(ppu, 1u, kRed);                           // BG1's palette 0, colour 1
  putColour(ppu, kModeZeroSubset + 1u, kGreen);       // BG2's
  putColour(ppu, kModeZeroSubset * 2u + 1u, kBlue);   // BG3's
  putColour(ppu, kModeZeroSubset * 3u + 1u, kWhite);  // BG4's
  return ppu;
}

// Mode 3 with its two backgrounds shown: BG1 at eight bitplanes and BG2 at four.
PpuState modeThree() {
  PpuState ppu = placed();
  ppu.bgmode = 0x03u;
  ppu.tm = 0x03u;
  putSolid(ppu, kBg1Chars, 8u, 1u, 1u);
  putSolid(ppu, kBg2Chars, 4u, 1u, 1u);
  putColour(ppu, 1u, kRed);
  putColour(ppu, 17u, kGreen);  // BG2's palette 1, colour 1
  return ppu;
}

// ---- Mode 0's four layers ----------------------------------------------------

TEST(SnesPpuModes, TheConverterAgreesWithTheValuesTheOtherSuitesComputeByHand) {
  EXPECT_EQ(channel(31u), 255u);
  EXPECT_EQ(channel(3u), 25u);
  EXPECT_EQ(channel(2u), 16u);
  EXPECT_EQ(channel(1u), 8u);
  EXPECT_EQ(channel(0u), 0u);
}

TEST(SnesPpuModes, ModeZeroDrawsFourBackgrounds) {
  PpuState ppu = modeZero();
  // One tile each, four tile columns apart, so each is the only layer showing there.
  putEntry(ppu, kBg1Map, 0u, 0u, entry(1u, 0u, false));
  putEntry(ppu, kBg2Map, 1u, 0u, entry(1u, 0u, false));
  putEntry(ppu, kBg3Map, 2u, 0u, entry(1u, 0u, false));
  putEntry(ppu, kBg4Map, 3u, 0u, entry(1u, 0u, false));
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.frames, 1u);
  EXPECT_EQ(picture.at(0u, lineOf(0u)), out(kRed));
  EXPECT_EQ(picture.at(8u, lineOf(0u)), out(kGreen));
  EXPECT_EQ(picture.at(16u, lineOf(0u)), out(kBlue));
  EXPECT_EQ(picture.at(24u, lineOf(0u)), out(kWhite));
  EXPECT_EQ(picture.at(32u, lineOf(0u)), out(kBackdrop));
}

TEST(SnesPpuModes, TheFourthBackgroundReadsItsOwnScreenRegister) {
  PpuState ppu = modeZero();
  ppu.tm = 0x08u;  // BG4 alone, so nothing else can stand in for it
  putEntry(ppu, kBg4Map, 0u, 0u, entry(1u, 0u, false));
  EXPECT_EQ(draw(ppu).at(0u, lineOf(0u)), out(kWhite));

  ppu.bg4sc = kBg3MapBase;  // pointed at BG3's map, which has nothing in it
  EXPECT_EQ(draw(ppu).at(0u, lineOf(0u)), out(kBackdrop));
}

TEST(SnesPpuModes, TheFourthBackgroundReadsItsOwnCharacterBase) {
  PpuState ppu = modeZero();
  ppu.tm = 0x08u;
  putEntry(ppu, kBg4Map, 0u, 0u, entry(1u, 0u, false));
  // BG3's block holds a different character 1, so a background reading the wrong
  // nibble of $210C shows a different colour rather than the same one.
  putSolid(ppu, kBg3Chars, 2u, 1u, 3u);
  putColour(ppu, kModeZeroSubset * 3u + 3u, kYellow);
  EXPECT_EQ(draw(ppu).at(0u, lineOf(0u)), out(kWhite));

  ppu.bg34nba = 0x33u;  // BG4's nibble moved to BG3's block
  EXPECT_EQ(draw(ppu).at(0u, lineOf(0u)), out(kYellow));
}

TEST(SnesPpuModes, TheFourthBackgroundReadsItsOwnScrollRegisters) {
  PpuState ppu = modeZero();
  ppu.tm = 0x08u;
  putEntry(ppu, kBg4Map, 1u, 0u, entry(1u, 0u, false));
  EXPECT_EQ(draw(ppu).at(8u, lineOf(0u)), out(kWhite));
  EXPECT_EQ(draw(ppu).at(0u, lineOf(0u)), out(kBackdrop));

  ppu.bg4hofs = 8u;  // the picture moves left by a tile, so the tile arrives at x 0
  EXPECT_EQ(draw(ppu).at(0u, lineOf(0u)), out(kWhite));
}

TEST(SnesPpuModes, TheFourthBackgroundTakesItsTileSizeFromBitSeven) {
  PpuState ppu = modeZero();
  ppu.tm = 0x08u;
  ppu.bgmode = 0x80u;  // Mode 0, BG4 in 16x16 blocks
  putEntry(ppu, kBg4Map, 0u, 0u, entry(1u, 0u, false));
  putSolid(ppu, kBg4Chars, 2u, 2u, 1u);  // a block is Tile, Tile + 1, Tile + 16, Tile + 17
  const Picture picture = draw(ppu);
  // The block's right half is the character one on, which is solid too, so the
  // sixteen pixels across are one colour where an 8x8 tile would give eight.
  EXPECT_EQ(picture.at(0u, lineOf(0u)), out(kWhite));
  EXPECT_EQ(picture.at(8u, lineOf(0u)), out(kWhite));
  EXPECT_EQ(picture.at(16u, lineOf(0u)), out(kBackdrop));
}

TEST(SnesPpuModes, TheFourthBackgroundShowsOnlyWhereTheMainScreenEnablesIt) {
  PpuState ppu = modeZero();
  ppu.tm = 0x07u;  // the first three, not BG4
  putEntry(ppu, kBg4Map, 0u, 0u, entry(1u, 0u, false));
  EXPECT_EQ(draw(ppu).at(0u, lineOf(0u)), out(kBackdrop));

  ppu.tm = 0x0Fu;
  EXPECT_EQ(draw(ppu).at(0u, lineOf(0u)), out(kWhite));
}

// ---- Mode 0's palette subsets ------------------------------------------------

TEST(SnesPpuModes, ModeZeroGivesEachBackgroundItsOwnThirtyTwoWords) {
  PpuState ppu = modeZero();
  // Each background's colour 1 is a different word, and the four words hold four
  // different colours: words 1, 33, 65 and 97.
  putEntry(ppu, kBg1Map, 0u, 0u, entry(1u, 0u, false));
  putEntry(ppu, kBg2Map, 1u, 0u, entry(1u, 0u, false));
  putEntry(ppu, kBg3Map, 2u, 0u, entry(1u, 0u, false));
  putEntry(ppu, kBg4Map, 3u, 0u, entry(1u, 0u, false));
  const Picture picture = draw(ppu);
  EXPECT_EQ(picture.at(0u, lineOf(0u)), out(kRed));
  EXPECT_EQ(picture.at(8u, lineOf(0u)), out(kGreen));
  EXPECT_EQ(picture.at(16u, lineOf(0u)), out(kBlue));
  EXPECT_EQ(picture.at(24u, lineOf(0u)), out(kWhite));
}

TEST(SnesPpuModes, ModeZerosPaletteFieldStepsFourWords) {
  PpuState ppu = modeZero();
  putColour(ppu, 5u, kCyan);  // BG1's palette 1, colour 1
  putEntry(ppu, kBg1Map, 0u, 0u, entry(1u, 1u, false));
  ppu.tm = 0x01u;
  EXPECT_EQ(draw(ppu).at(0u, lineOf(0u)), out(kCyan));
}

TEST(SnesPpuModes, TheLastWordOfASubsetIsTheLastPalettesLastColour) {
  PpuState ppu = modeZero();
  ppu.tm = 0x01u;
  putColour(ppu, 31u, kMagenta);  // BG1's palette 7, colour 3 — the subset's last word
  putSolid(ppu, kBg1Chars, 2u, 2u, 3u);
  putEntry(ppu, kBg1Map, 0u, 0u, entry(2u, 7u, false));
  EXPECT_EQ(draw(ppu).at(0u, lineOf(0u)), out(kMagenta));
}

TEST(SnesPpuModes, TheSecondBackgroundsSubsetBeginsWhereTheFirstsEnds) {
  PpuState ppu = modeZero();
  ppu.tm = 0x02u;  // BG2 alone
  putColour(ppu, 32u, kYellow);  // its palette 0 colour 0, which is transparent
  putColour(ppu, 35u, kCyan);    // its palette 0 colour 3
  putSolid(ppu, kBg2Chars, 2u, 2u, 3u);
  putEntry(ppu, kBg2Map, 0u, 0u, entry(2u, 0u, false));
  EXPECT_EQ(draw(ppu).at(0u, lineOf(0u)), out(kCyan));

  putSolid(ppu, kBg2Chars, 2u, 2u, 0u);  // colour 0 is transparent, whatever word 32 holds
  EXPECT_EQ(draw(ppu).at(0u, lineOf(0u)), out(kBackdrop));
}

TEST(SnesPpuModes, ModeOneGivesNoBackgroundAPaletteOffsetOfItsOwn) {
  PpuState ppu = placed();
  ppu.bgmode = 0x01u;
  ppu.tm = 0x02u;  // Mode 1's BG2, which is sixteen colours
  putColour(ppu, 1u, kRed);
  putSolid(ppu, kBg2Chars, 4u, 1u, 1u);
  putEntry(ppu, kBg2Map, 0u, 0u, entry(1u, 0u, false));
  EXPECT_EQ(draw(ppu).at(0u, lineOf(0u)), out(kRed));
}

// ---- Mode 0's order ----------------------------------------------------------
//
// Front to back: S3 1H 2H S2 1L 2L S1 3H 4H S0 3L 4L. Each case puts two
// neighbours in the chart at the same place and reads which one the picture shows.

// All four backgrounds and a sprite over one tile, each at the priority the caller
// names, so the chart decides the pixel.
PpuState modeZeroStack(bool bg1High, bool bg2High, bool bg3High, bool bg4High,
                       unsigned spritePriority, bool withSprite) {
  PpuState ppu = modeZero();
  ppu.tm = 0x1Fu;  // every layer, sprites included
  putEntry(ppu, kBg1Map, 0u, 5u, entry(1u, 0u, bg1High));
  putEntry(ppu, kBg2Map, 0u, 5u, entry(1u, 0u, bg2High));
  putEntry(ppu, kBg3Map, 0u, 5u, entry(1u, 0u, bg3High));
  putEntry(ppu, kBg4Map, 0u, 5u, entry(1u, 0u, bg4High));
  putColour(ppu, kSpritePaletteBase + 1u, kYellow);
  if (withSprite) spriteAt(ppu, 0u, 0, spritePriority);
  return ppu;
}

TEST(SnesPpuModes, ModeZerosSpriteAtPriorityThreeIsInFrontOfEverything) {
  const PpuState ppu = modeZeroStack(true, true, true, true, 3u, true);
  EXPECT_EQ(draw(ppu).at(0u, lineOf(5u)), out(kYellow));
}

TEST(SnesPpuModes, ModeZerosFirstBackgroundIsInFrontOfItsSecondAtEitherPriority) {
  EXPECT_EQ(draw(modeZeroStack(true, true, true, true, 0u, false)).at(0u, lineOf(5u)),
            out(kRed));
  EXPECT_EQ(draw(modeZeroStack(false, false, true, true, 0u, false)).at(0u, lineOf(5u)),
            out(kRed));
}

TEST(SnesPpuModes, ModeZerosSpriteAtPriorityTwoSitsBetweenTheFirstTwosPriorities) {
  // High tiles beat it.
  EXPECT_EQ(draw(modeZeroStack(false, true, true, true, 2u, true)).at(0u, lineOf(5u)),
            out(kGreen));
  // Low ones do not.
  EXPECT_EQ(draw(modeZeroStack(false, false, true, true, 2u, true)).at(0u, lineOf(5u)),
            out(kYellow));
}

// The last six places of Mode 0's chart stand below both of the first two
// backgrounds' priorities, so those two show nothing where these cases read: the
// blank character, which is transparent whatever palette names it.
PpuState lowerHalf(bool bg3High, bool bg4High, unsigned spritePriority, bool withSprite) {
  PpuState ppu = modeZeroStack(false, false, bg3High, bg4High, spritePriority, withSprite);
  putEntry(ppu, kBg1Map, 0u, 5u, entry(0u, 0u, false));
  putEntry(ppu, kBg2Map, 0u, 5u, entry(0u, 0u, false));
  return ppu;
}

TEST(SnesPpuModes, ModeZerosSpriteAtPriorityOneIsInFrontOfTheLastTwoBackgrounds) {
  EXPECT_EQ(draw(lowerHalf(true, true, 1u, true)).at(0u, lineOf(5u)), out(kYellow));
  // And with it gone, BG3's high tiles beat BG4's.
  EXPECT_EQ(draw(lowerHalf(true, true, 1u, false)).at(0u, lineOf(5u)), out(kBlue));
}

TEST(SnesPpuModes, ModeZerosLastPlacesAreTheThirdAndFourthBackgroundsLowTiles) {
  // The sprite at priority 0 beats both of them.
  EXPECT_EQ(draw(lowerHalf(false, false, 0u, true)).at(0u, lineOf(5u)), out(kYellow));
  // Without it, BG3's low tiles beat BG4's, which are the last place in the chart.
  EXPECT_EQ(draw(lowerHalf(false, false, 0u, false)).at(0u, lineOf(5u)), out(kBlue));
  // And with BG3 blank as well, BG4's low tiles are what is left.
  PpuState ppu = lowerHalf(false, false, 0u, false);
  putEntry(ppu, kBg3Map, 0u, 5u, entry(0u, 0u, false));
  EXPECT_EQ(draw(ppu).at(0u, lineOf(5u)), out(kWhite));
}

// ---- $2105 bit 3 -------------------------------------------------------------

TEST(SnesPpuModes, TheThirdBackgroundsPriorityBitMovesNothingInModeZero) {
  // BG3's high tiles sit behind BG1 in Mode 0's chart and the bit names no place
  // there, so setting it changes nothing.
  PpuState ppu = modeZeroStack(true, false, true, false, 0u, false);
  EXPECT_EQ(draw(ppu).at(0u, lineOf(5u)), out(kRed));
  ppu.bgmode = static_cast<std::uint8_t>(ppu.bgmode | 0x08u);
  EXPECT_EQ(draw(ppu).at(0u, lineOf(5u)), out(kRed));
}

TEST(SnesPpuModes, TheThirdBackgroundsPriorityBitMovesNothingInModeThree) {
  PpuState ppu = modeThree();
  putEntry(ppu, kBg1Map, 0u, 5u, entry(1u, 0u, false));
  putEntry(ppu, kBg2Map, 0u, 5u, entry(1u, 1u, true));
  // BG2's high tiles beat BG1's low ones in Mode 3's chart.
  EXPECT_EQ(draw(ppu).at(0u, lineOf(5u)), out(kGreen));
  ppu.bgmode = 0x0Bu;  // Mode 3 with the bit set, which Mode 3 has no place for
  EXPECT_EQ(draw(ppu).at(0u, lineOf(5u)), out(kGreen));
}

TEST(SnesPpuModes, ModeOnesThirdBackgroundStillAnswersTheBit) {
  PpuState ppu = placed();
  ppu.bgmode = 0x01u;
  ppu.tm = 0x05u;  // BG1 and BG3
  putColour(ppu, 1u, kRed);
  putColour(ppu, 5u, kCyan);  // BG3's palette 1 colour 1, which Mode 1 does not offset
  putSolid(ppu, kBg1Chars, 4u, 1u, 1u);
  putSolid(ppu, kBg3Chars, 2u, 1u, 1u);
  putEntry(ppu, kBg1Map, 0u, 5u, entry(1u, 0u, true));
  putEntry(ppu, kBg3Map, 0u, 5u, entry(1u, 1u, true));
  EXPECT_EQ(draw(ppu).at(0u, lineOf(5u)), out(kRed));
  ppu.bgmode = 0x09u;  // the bit lifts BG3's high tiles in front of everything
  EXPECT_EQ(draw(ppu).at(0u, lineOf(5u)), out(kCyan));
}

// ---- Mode 3's two layers -----------------------------------------------------

TEST(SnesPpuModes, ModeThreeDrawsATwoHundredAndFiftySixColourBackground) {
  PpuState ppu = modeThree();
  ppu.tm = 0x01u;
  putColour(ppu, 255u, kMagenta);  // the last word of the palette, which only 8bpp reaches
  putSolid(ppu, kBg1Chars, 8u, 2u, 255u);
  putEntry(ppu, kBg1Map, 0u, 0u, entry(2u, 0u, false));
  EXPECT_EQ(draw(ppu).at(0u, lineOf(0u)), out(kMagenta));
}

TEST(SnesPpuModes, TheEightBitplanesAreFourPairsSixteenBytesApart) {
  PpuState ppu = modeThree();
  ppu.tm = 0x01u;
  putColour(ppu, 0x80u, kCyan);  // index 128: plane 7 alone, the last pair's high byte
  putColour(ppu, 0x40u, kBlue);  // index 64: plane 6, the same pair's low byte
  putSolid(ppu, kBg1Chars, 8u, 2u, 0x80u);
  putSolid(ppu, kBg1Chars, 8u, 3u, 0x40u);
  putEntry(ppu, kBg1Map, 0u, 0u, entry(2u, 0u, false));
  putEntry(ppu, kBg1Map, 1u, 0u, entry(3u, 0u, false));
  const Picture picture = draw(ppu);
  EXPECT_EQ(picture.at(0u, lineOf(0u)), out(kCyan));
  EXPECT_EQ(picture.at(8u, lineOf(0u)), out(kBlue));
}

TEST(SnesPpuModes, ModeThreesSecondBackgroundIsSixteenColours) {
  PpuState ppu = modeThree();
  ppu.tm = 0x02u;
  putColour(ppu, 17u, kGreen);  // palette 1, colour 1 — sixteen words a palette
  putEntry(ppu, kBg2Map, 0u, 0u, entry(1u, 1u, false));
  EXPECT_EQ(draw(ppu).at(0u, lineOf(0u)), out(kGreen));
}

TEST(SnesPpuModes, ModeThreesFirstBackgroundIgnoresItsTilesPaletteField) {
  PpuState ppu = modeThree();
  ppu.tm = 0x01u;
  putEntry(ppu, kBg1Map, 0u, 0u, entry(1u, 0u, false));
  putEntry(ppu, kBg1Map, 1u, 0u, entry(1u, 7u, false));
  const Picture picture = draw(ppu);
  // The same pixel under two palette fields is the same colour: a 256-colour
  // background's pixel IS its word.
  EXPECT_EQ(picture.at(0u, lineOf(0u)), out(kRed));
  EXPECT_EQ(picture.at(8u, lineOf(0u)), out(kRed));
}

// ---- Mode 3's order ----------------------------------------------------------
//
// Front to back: S3 1H S2 2H S1 1L S0 2L.

PpuState modeThreeStack(bool bg1High, bool bg2High, unsigned spritePriority, bool withSprite) {
  PpuState ppu = modeThree();
  ppu.tm = 0x13u;
  putEntry(ppu, kBg1Map, 0u, 5u, entry(1u, 0u, bg1High));
  putEntry(ppu, kBg2Map, 0u, 5u, entry(1u, 1u, bg2High));
  putColour(ppu, kSpritePaletteBase + 1u, kYellow);
  if (withSprite) spriteAt(ppu, 0u, 0, spritePriority);
  return ppu;
}

TEST(SnesPpuModes, ModeThreesSpriteAtPriorityThreeIsInFrontOfEverything) {
  EXPECT_EQ(draw(modeThreeStack(true, true, 3u, true)).at(0u, lineOf(5u)), out(kYellow));
}

TEST(SnesPpuModes, ModeThreesFirstBackgroundsHighTilesBeatTheSpriteAtPriorityTwo) {
  EXPECT_EQ(draw(modeThreeStack(true, true, 2u, true)).at(0u, lineOf(5u)), out(kRed));
  EXPECT_EQ(draw(modeThreeStack(false, true, 2u, true)).at(0u, lineOf(5u)), out(kYellow));
}

TEST(SnesPpuModes, ModeThreesSecondBackgroundsHighTilesBeatTheSpriteAtPriorityOne) {
  EXPECT_EQ(draw(modeThreeStack(false, true, 1u, true)).at(0u, lineOf(5u)), out(kGreen));
  EXPECT_EQ(draw(modeThreeStack(false, false, 1u, true)).at(0u, lineOf(5u)), out(kYellow));
}

TEST(SnesPpuModes, ModeThreesLastPlaceIsItsSecondBackgroundsLowTiles) {
  // The first background's low tiles beat the sprite at priority 0.
  EXPECT_EQ(draw(modeThreeStack(false, false, 0u, true)).at(0u, lineOf(5u)), out(kRed));
  // And with BG1 showing nothing there, the sprite beats BG2's low tiles.
  PpuState ppu = modeThreeStack(false, false, 0u, true);
  putEntry(ppu, kBg1Map, 0u, 5u, entry(0u, 0u, false));  // the blank character
  EXPECT_EQ(draw(ppu).at(0u, lineOf(5u)), out(kYellow));
}

// ---- direct colour -----------------------------------------------------------
//
// CGWSEL bit 0 reads a 256-colour background's pixel as BBGGGRRR and its tile's
// three palette bits as bgr: red = RRRr0, green = GGGg0, blue = BBb00.

// Mode 3 in direct colour, BG1 alone, with one character of the pixel value given
// under the palette field given.
PpuState direct(std::uint8_t pixel, unsigned bgr) {
  PpuState ppu = modeThree();
  ppu.tm = 0x01u;
  ppu.cgwsel = 0x01u;
  putSolid(ppu, kBg1Chars, 8u, 2u, pixel);
  putEntry(ppu, kBg1Map, 0u, 0u, entry(2u, bgr, false));
  return ppu;
}

TEST(SnesPpuModes, DirectColourComposesThePixelAndTheTilesPaletteBits) {
  // $B6 is BB = 2, GGG = 6, RRR = 6, and bgr = 5 is b and r set: so red is
  // 6 * 4 + 2 = 26, green is 6 * 4 = 24, and blue is 2 * 8 + 4 = 20.
  EXPECT_EQ(draw(direct(0xB6u, 5u)).at(0u, lineOf(0u)), rgb(26u, 24u, 20u));
}

TEST(SnesPpuModes, TheTilesRedBitIsRedsSecondBitAndMovesNothingElse) {
  EXPECT_EQ(draw(direct(0x07u, 0u)).at(0u, lineOf(0u)), rgb(28u, 0u, 0u));
  EXPECT_EQ(draw(direct(0x07u, 1u)).at(0u, lineOf(0u)), rgb(30u, 0u, 0u));
}

TEST(SnesPpuModes, TheTilesGreenBitIsGreensSecondBitAndMovesNothingElse) {
  EXPECT_EQ(draw(direct(0x38u, 0u)).at(0u, lineOf(0u)), rgb(0u, 28u, 0u));
  EXPECT_EQ(draw(direct(0x38u, 2u)).at(0u, lineOf(0u)), rgb(0u, 30u, 0u));
}

TEST(SnesPpuModes, TheTilesBlueBitIsBluesThirdBitAndMovesNothingElse) {
  EXPECT_EQ(draw(direct(0xC0u, 0u)).at(0u, lineOf(0u)), rgb(0u, 0u, 24u));
  EXPECT_EQ(draw(direct(0xC0u, 4u)).at(0u, lineOf(0u)), rgb(0u, 0u, 28u));
}

TEST(SnesPpuModes, TheChannelsAreNotTransposed) {
  // The one disagreement between the sources: a pixel that is red alone must not
  // come out green, and the pixel that is green alone must not come out red.
  EXPECT_EQ(draw(direct(0x07u, 0u)).at(0u, lineOf(0u)), rgb(28u, 0u, 0u));
  EXPECT_EQ(draw(direct(0x38u, 0u)).at(0u, lineOf(0u)), rgb(0u, 28u, 0u));
}

TEST(SnesPpuModes, APixelOfZeroIsStillTransparentInDirectColour) {
  // Even under a palette field that would compose to something, and even though
  // there is no black in direct colour.
  EXPECT_EQ(draw(direct(0x00u, 7u)).at(0u, lineOf(0u)), out(kBackdrop));
}

TEST(SnesPpuModes, TheBitReachesOnlyATwoHundredAndFiftySixColourBackground) {
  // Mode 3's BG2 is sixteen colours, and a four-bit pixel has no BBGGGRRR in it:
  // the bit leaves it reading its palette, beside a BG1 the same bit composes.
  PpuState ppu = modeThree();
  ppu.cgwsel = 0x01u;
  ppu.tm = 0x03u;
  putSolid(ppu, kBg1Chars, 8u, 2u, 0x07u);
  putEntry(ppu, kBg1Map, 0u, 0u, entry(2u, 0u, false));
  putEntry(ppu, kBg2Map, 1u, 0u, entry(1u, 1u, false));
  const Picture picture = draw(ppu);
  EXPECT_EQ(picture.at(0u, lineOf(0u)), rgb(28u, 0u, 0u));  // composed
  EXPECT_EQ(picture.at(8u, lineOf(0u)), out(kGreen));       // palette word 17, as ever
}

TEST(SnesPpuModes, WithTheBitClearTheSamePixelReadsThroughThePalette) {
  PpuState ppu = direct(0xB6u, 5u);
  ppu.cgwsel = 0x00u;
  putColour(ppu, 0xB6u, kMagenta);
  EXPECT_EQ(draw(ppu).at(0u, lineOf(0u)), out(kMagenta));
}

TEST(SnesPpuModes, ADirectColourPixelReachesTheSubScreensAddend) {
  // BG2 on the main screen taking math, BG1 in direct colour on the sub screen as
  // the addend: green 16 plus the composed red 28 is a colour neither alone is.
  PpuState ppu = modeThree();
  ppu.cgwsel = 0x03u;  // direct colour, and the sub screen is the addend
  ppu.cgadsub = 0x02u; // BG2 takes math, add, no half
  ppu.tm = 0x02u;
  ppu.ts = 0x01u;
  putColour(ppu, 17u, 0x0200u);  // BG2's colour: green 16
  putSolid(ppu, kBg1Chars, 8u, 2u, 0x07u);
  putEntry(ppu, kBg2Map, 0u, 0u, entry(1u, 1u, false));
  putEntry(ppu, kBg1Map, 0u, 0u, entry(2u, 0u, false));
  EXPECT_EQ(draw(ppu).at(0u, lineOf(0u)), rgb(28u, 16u, 0u));
}

TEST(SnesPpuModes, DirectColourIsNotColourMath) {
  // With no layer taking math the composition stands exactly as it is.
  PpuState ppu = direct(0xB6u, 5u);
  ppu.cgadsub = 0x00u;
  EXPECT_EQ(draw(ppu).at(0u, lineOf(0u)), rgb(26u, 24u, 20u));
}

// ---- direct colour under $2130 -----------------------------------------------
//
// The four strips of the sweep cartridge: what the two regions and the fixed
// colour do to a composed pixel, which no source states either way.

TEST(SnesPpuModes, TheBlackRegionBlackensADirectColourPixel) {
  PpuState ppu = direct(0xB6u, 5u);
  ppu.cgwsel = 0xC1u;  // bits 7-6: the black region everywhere
  EXPECT_EQ(draw(ppu).at(0u, lineOf(0u)), kBlack);
}

TEST(SnesPpuModes, PreventingMathLeavesADirectColourPixelAsComposed) {
  PpuState ppu = direct(0xB6u, 5u);
  ppu.cgwsel = 0x31u;   // bits 5-4: math prevented everywhere
  ppu.cgadsub = 0x01u;  // though BG1 is named
  EXPECT_EQ(draw(ppu).at(0u, lineOf(0u)), rgb(26u, 24u, 20u));
}

TEST(SnesPpuModes, TheFixedColourAddsToADirectColourPixel) {
  PpuState ppu = direct(0xB6u, 5u);
  ppu.cgadsub = 0x01u;  // BG1 takes math, add, no half; the sub screen is not the addend
  ppu.fixedRed = 8u;
  ppu.fixedGreen = 8u;
  ppu.fixedBlue = 8u;
  EXPECT_EQ(draw(ppu).at(0u, lineOf(0u)), rgb(31u, 31u, 28u));  // 26+8 and 24+8 both hold at 31
}

TEST(SnesPpuModes, TheFixedColourHalvesAgainstADirectColourPixel) {
  PpuState ppu = direct(0xB6u, 5u);
  ppu.cgadsub = 0x41u;  // add and halve
  ppu.fixedRed = 8u;
  ppu.fixedGreen = 8u;
  ppu.fixedBlue = 8u;
  // The halving comes before the hold: 34 >> 1 is 17, 32 >> 1 is 16, 28 >> 1 is 14.
  EXPECT_EQ(draw(ppu).at(0u, lineOf(0u)), rgb(17u, 16u, 14u));
}

// ---- the sweep cartridge's own grid ------------------------------------------

// The thirteen pixel values the cartridge's cells carry, in its order. Every bit of
// the byte moves on its own; each field saturates once; $07 against $38 is the pair
// that tells red from green.
constexpr std::array<std::uint8_t, 13> kSweepValues{0x00u, 0x01u, 0x02u, 0x04u, 0x07u,
                                                    0x08u, 0x10u, 0x20u, 0x38u, 0x40u,
                                                    0x80u, 0xC0u, 0xFFu};

// The composition, written from the bit table: each channel's own field shifted up
// with the tile's own bit under it.
constexpr Rgba composed(unsigned bgr, std::uint8_t value) {
  return rgb(((value & 0x07u) << 2) | ((bgr & 1u) << 1),
             (((value >> 3) & 0x07u) << 2) | (bgr & 2u),
             (((value >> 6) & 0x03u) << 3) | (bgr & 4u));
}

TEST(SnesPpuModes, TheSweepCartridgesGridComesOutCellByCell) {
  // The cartridge's picture: eight rows, one per value of the tile's palette field,
  // and thirteen cells across each. Its backdrop is a grey odd in all three
  // channels, which direct colour cannot make, and its other palette words are
  // white for the same reason — so a cell holding either is CGRAM answering.
  PpuState ppu = placed();
  ppu.bgmode = 0x03u;
  ppu.cgwsel = 0x01u;
  ppu.tm = 0x01u;
  putColour(ppu, 0u, kGrey);
  for (unsigned word = 1u; word < 256u; ++word) putColour(ppu, word, kWhite);
  for (unsigned cell = 0u; cell < kSweepValues.size(); ++cell) {
    putSolid(ppu, kBg1Chars, 8u, cell, kSweepValues[cell]);
    for (unsigned bgr = 0u; bgr < 8u; ++bgr) {
      putEntry(ppu, kBg1Map, cell, bgr, entry(cell, bgr, false));
    }
  }

  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.frames, 1u);
  for (unsigned bgr = 0u; bgr < 8u; ++bgr) {
    for (unsigned cell = 0u; cell < kSweepValues.size(); ++cell) {
      const std::uint8_t value = kSweepValues[cell];
      const Rgba want = value == 0u ? out(kGrey) : composed(bgr, value);
      EXPECT_EQ(picture.at(cell * 8u, lineOf(bgr)), want)
          << "bgr " << bgr << ", pixel " << static_cast<unsigned>(value);
    }
  }
}

// ---- what was drawn before is drawn still ------------------------------------

// A digest of a whole frame, so an opt-in proof is one number rather than a
// sampling: the 64-bit FNV-1a of every byte the observer was handed.
std::uint64_t digest(const Picture& picture) {
  std::uint64_t hash = 1469598103934665603ull;
  for (const std::uint8_t byte : picture.pixels) {
    hash = (hash ^ byte) * 1099511628211ull;
  }
  return hash;
}

TEST(SnesPpuModes, ModeOneDrawsWhatItDrewBefore) {
  // Mode 1's three backgrounds, a sprite of each priority, and a window masking
  // one of them: a frame with every part of the old path in it. The number is the
  // frame this suite drew before either of this block's modes existed.
  PpuState ppu = placed();
  ppu.bgmode = 0x09u;  // Mode 1 with BG3's high tiles lifted, which only Mode 1 has
  ppu.tm = 0x17u;
  ppu.ts = 0x03u;
  ppu.cgwsel = 0x02u;
  ppu.cgadsub = 0x21u;
  ppu.tmw = 0x02u;
  ppu.w12sel = 0x20u;  // BG2's window 1, not inverted
  ppu.wh0 = 40u;
  ppu.wh1 = 200u;
  putColour(ppu, 1u, kRed);
  putColour(ppu, 17u, kGreen);
  putColour(ppu, 5u, kCyan);
  putColour(ppu, kSpritePaletteBase + 1u, kYellow);
  putSolid(ppu, kBg1Chars, 4u, 1u, 1u);
  putSolid(ppu, kBg2Chars, 4u, 1u, 1u);
  putSolid(ppu, kBg3Chars, 2u, 1u, 1u);
  for (unsigned tileY = 0u; tileY < 28u; ++tileY) {
    for (unsigned tileX = 0u; tileX < 32u; ++tileX) {
      putEntry(ppu, kBg1Map, tileX, tileY, entry(1u, 0u, (tileX & 1u) != 0u));
      putEntry(ppu, kBg2Map, tileX, tileY, entry(1u, 1u, (tileY & 1u) != 0u));
      putEntry(ppu, kBg3Map, tileX, tileY, entry(1u, 1u, (tileX & 2u) != 0u));
    }
  }
  for (unsigned n = 0u; n < 4u; ++n) {
    putSprite(ppu, n, static_cast<int>(64u + n * 16u), 40u, 0u, attributes(n, 0u));
  }
  putTileAt(ppu, kSpriteChars, kSpritePlanes, [] {
    TileRows rows{};
    for (auto& row : rows) row.fill(1u);
    return rows;
  }());

  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.frames, 1u);
  EXPECT_EQ(digest(picture), 4067333833209197443ull);
}

// ---- the staged cartridges ---------------------------------------------------

// The cartridges under SNAGGLETOOTH_PPU_ROMS are run and nothing else: no source
// of theirs is opened and nothing from any of them is written down here. What one
// of them exercises is learned by running it. Unset, these cases register and skip
// with a reason.
//
//   cmake -B build -DSNAGGLETOOTH_PPU_ROMS=/path/to/ppu-testroms
//
// SNAGGLETOOTH_REQUIRE_PPU_ROMS in the environment turns a missing cartridge into
// a failure instead of a skip, for a machine that is meant to have them.
bool romsRequired() {
  const char* required = std::getenv("SNAGGLETOOTH_REQUIRE_PPU_ROMS");
  return required != nullptr && *required != '\0';
}

// The first file of that name anywhere under the directory, or nothing.
std::string findRom(const std::string& name) {
  const std::string root = SNAGGLETOOTH_PPU_ROMS;
  if (root.empty()) return {};
  std::error_code failed;
  std::filesystem::recursive_directory_iterator walk(root, failed);
  if (failed) return {};
  for (const std::filesystem::directory_entry& entry : walk) {
    if (entry.is_regular_file(failed) && entry.path().filename() == name) {
      return entry.path().string();
    }
  }
  return {};
}

// Loads a staged cartridge, or skips the calling case when there is none to load.
void load(const std::string& name, std::vector<std::uint8_t>& rom, bool& ran) {
  ran = false;
  const std::string path = findRom(name);
  if (path.empty()) {
    if (romsRequired()) {
      ADD_FAILURE() << name
                    << " not found under SNAGGLETOOTH_PPU_ROMS and the cartridges are required";
      return;
    }
    GTEST_SKIP() << name << ": set SNAGGLETOOTH_PPU_ROMS to the staged cartridge directory to run it";
  }
  std::ifstream file(path, std::ios::binary);
  rom.assign((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  if (rom.size() < 0x8000u) {
    ADD_FAILURE() << name << " is too small to be a cartridge image";
    return;
  }
  ran = true;
}

// What a second of a cartridge's own run leaves: the last frame it drew, the
// machine as it stood, and the digest and colour count of that picture.
struct Played {
  SnesState state;
  Picture picture;
  std::uint64_t digest = 0;
  std::size_t colours = 0;
  unsigned lit = 0;
};

// The frames a cartridge draws, the last one kept — its own run needs a second to
// reach the picture it settles on.
struct LastFrame final : FrameObserver {
  unsigned frames = 0;
  unsigned width = 0;
  unsigned height = 0;
  std::vector<std::uint8_t> pixels;
  void frame(const VideoFrame& picture) override {
    ++frames;
    width = picture.width;
    height = picture.height;
    pixels.assign(picture.pixels.begin(), picture.pixels.end());
  }
};

Played play(const std::vector<std::uint8_t>& rom, std::uint64_t cycles = 60ull * 357364ull) {
  Snes machine(SnesConfig{.rom = rom});
  LastFrame watch;
  machine.setFrameObserver(&watch);
  machine.run(cycles);
  Played ran;
  ran.state = machine.state();
  ran.picture.frames = watch.frames;
  ran.picture.width = watch.width;
  ran.picture.height = watch.height;
  ran.picture.pixels = watch.pixels;
  std::set<std::uint32_t> colours;
  for (std::size_t at = 0u; at + 3u < ran.picture.pixels.size(); at += 4u) {
    const std::uint32_t colour = ran.picture.pixels[at] | (ran.picture.pixels[at + 1u] << 8) |
                                 (ran.picture.pixels[at + 2u] << 16);
    colours.insert(colour);
    if (colour != 0u) ++ran.lit;
  }
  ran.colours = colours.size();
  ran.digest = digest(ran.picture);
  return ran;
}

// A cartridge drawing one of Mode 0's four backgrounds and nothing else, named by
// the bit of $212C its own program sets.
void modeZeroCartridge(const std::string& name, std::uint8_t expected) {
  std::vector<std::uint8_t> rom;
  bool ran = false;
  load(name, rom, ran);
  if (!ran) return;
  const Played played = play(rom);
  EXPECT_EQ(played.state.ppu.bgmode & 0x07u, 0x00u);
  EXPECT_EQ(played.state.ppu.tm, expected);
  EXPECT_GT(played.lit, 1000u) << name << " drew nothing";
  // Four colours a palette and eight palettes: a picture with more than one
  // palette's worth in it is the subsets being reached.
  EXPECT_GT(played.colours, 4u);
}

TEST(PpuBackgroundCartridges, AModeZeroCartridgeDrawsThroughItsFirstBackground) {
  modeZeroCartridge("8x8BG1Map2BPP32x328PAL.sfc", 0x01u);
}

TEST(PpuBackgroundCartridges, AModeZeroCartridgeDrawsThroughItsSecondBackground) {
  modeZeroCartridge("8x8BG2Map2BPP32x328PAL.sfc", 0x02u);
}

TEST(PpuBackgroundCartridges, AModeZeroCartridgeDrawsThroughItsThirdBackground) {
  modeZeroCartridge("8x8BG3Map2BPP32x328PAL.sfc", 0x04u);
}

TEST(PpuBackgroundCartridges, AModeZeroCartridgeDrawsThroughItsFourthBackground) {
  // The one layer no mode but this one has, drawn by a program of its own rather
  // than by a placed state.
  modeZeroCartridge("8x8BG4Map2BPP32x328PAL.sfc", 0x08u);
}

TEST(PpuBackgroundCartridges, ThreeOfThoseCartridgesDifferOnlyInWhichBackgroundTheyUse) {
  std::vector<std::uint8_t> second, third, fourth;
  bool ran = false;
  load("8x8BG2Map2BPP32x328PAL.sfc", second, ran);
  if (!ran) return;
  load("8x8BG3Map2BPP32x328PAL.sfc", third, ran);
  if (!ran) return;
  load("8x8BG4Map2BPP32x328PAL.sfc", fourth, ran);
  if (!ran) return;
  // The same picture through three different backgrounds, each reading its own
  // screen register, its own character base, its own offsets and its own thirty-two
  // palette words: they come out pixel for pixel alike.
  EXPECT_EQ(play(second).digest, play(third).digest);
  EXPECT_EQ(play(third).digest, play(fourth).digest);
}

TEST(PpuBackgroundCartridges, AModeThreeCartridgeDrawsTwoHundredAndFiftySixColours) {
  std::vector<std::uint8_t> rom;
  bool ran = false;
  load("8x8BGMap8BPP32x32.sfc", rom, ran);
  if (!ran) return;
  const Played played = play(rom);
  EXPECT_EQ(played.state.ppu.bgmode & 0x07u, 0x03u);
  EXPECT_EQ(played.state.ppu.tm, 0x01u);
  EXPECT_GT(played.lit, 1000u);
  // More colours at once than any sixteen-colour palette holds.
  EXPECT_GT(played.colours, 16u);
}

TEST(PpuBackgroundCartridges, ItsMapSizeChangesWhatTheSameCartridgeDraws) {
  std::vector<std::uint8_t> single, wide, tall, both;
  bool ran = false;
  load("8x8BGMap8BPP32x32.sfc", single, ran);
  if (!ran) return;
  load("8x8BGMap8BPP64x32.sfc", wide, ran);
  if (!ran) return;
  load("8x8BGMap8BPP32x64.sfc", tall, ran);
  if (!ran) return;
  load("8x8BGMap8BPP64x64.sfc", both, ran);
  if (!ran) return;
  // A single-screen map repeats where a larger one reaches a further screen, so
  // the one-screen picture is the odd one out; the three larger maps show the same
  // first screen as each other.
  const std::uint64_t one = play(single).digest;
  const std::uint64_t large = play(wide).digest;
  EXPECT_NE(one, large);
  EXPECT_EQ(large, play(tall).digest);
  EXPECT_EQ(large, play(both).digest);
}

TEST(PpuBackgroundCartridges, AModeThreeCartridgeDrawsItsSecondBackground) {
  std::vector<std::uint8_t> rom;
  bool ran = false;
  load("8x8BGMap4BPP32x328PAL.sfc", rom, ran);
  if (!ran) return;
  const Played played = play(rom);
  EXPECT_EQ(played.state.ppu.bgmode & 0x07u, 0x03u);
  EXPECT_EQ(played.state.ppu.tm, 0x02u);  // BG2 alone, which Mode 3 keeps at sixteen colours
  EXPECT_GT(played.lit, 1000u);
  EXPECT_GT(played.colours, 16u);         // eight palettes of it
}

TEST(PpuBackgroundCartridges, ACartridgeThatFlipsItsTilesDraws) {
  std::vector<std::uint8_t> rom;
  bool ran = false;
  load("8x8BGMapTileFlip.sfc", rom, ran);
  if (!ran) return;
  const Played played = play(rom);
  EXPECT_EQ(played.state.ppu.bgmode & 0x07u, 0x03u);
  EXPECT_GT(played.lit, 1000u);
}

TEST(PpuBackgroundCartridges, TheseCartridgesSetTheThirdBackgroundsPriorityBitAndItMovesNothing) {
  std::vector<std::uint8_t> rom;
  bool ran = false;
  load("8x8BGMap8BPP32x32.sfc", rom, ran);
  if (!ran) return;
  const Played played = play(rom);
  // A real program in Mode 3 with $2105 bit 3 set: the bit is Mode 1's, and this
  // picture is what says so — drawn again from the same state with the bit taken
  // away, it comes out the same picture.
  ASSERT_NE(played.state.ppu.bgmode & 0x08u, 0u);
  const auto again = [&played](bool clearBit) {
    PpuState ppu = played.state.ppu;
    if (clearBit) ppu.bgmode = static_cast<std::uint8_t>(ppu.bgmode & ~0x08u);
    return digest(draw(ppu));
  };
  EXPECT_EQ(again(false), again(true));
}

}  // namespace
}  // namespace snaggletooth
