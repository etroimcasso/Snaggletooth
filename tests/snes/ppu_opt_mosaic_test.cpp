// Modes 2 and 4, where BG3's tilemap is a table of offsets for the other two
// backgrounds rather than a layer; and mosaic, which shows each block of a
// background as the pixel at its top-left corner.
//
// Every expectation is computed by hand from the register pages and anomie's
// description of the lookup; the pictures are placed as a program would have left
// them. The offset pictures read one BG1 through a table: its tile column C is
// drawn in hue C mod 8, bright on an even tile row and dark on an odd one, so a
// pixel's colour names the background position it was read from.

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <set>
#include <string>
#include <system_error>
#include <vector>

#include "gtest/gtest.h"
#include "snaggletooth/snes/snes.h"

namespace snaggletooth {
namespace {

using Rgba = std::array<std::uint8_t, 4>;

// The byte the converter drives for one five-bit channel at full brightness.
constexpr std::uint8_t channel(unsigned value) {
  return static_cast<std::uint8_t>((value * 255u + 15u) / 31u);
}
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
constexpr std::uint16_t kOrange = 0x01FFu;
constexpr std::uint16_t kGrey = 0x35ADu;

constexpr Rgba kBlack{0u, 0u, 0u, 255u};

// The hues of a tile column mod 8: bright at words 1-8 for an even tile row, dark at
// words 17-24 for an odd one.
constexpr std::array<std::uint16_t, 8> kBright{0x001Fu, 0x03E0u, 0x7C00u, 0x03FFu,
                                               0x7FE0u, 0x7C1Fu, 0x01FFu, 0x7C0Fu};
constexpr std::array<std::uint16_t, 8> kDark{0x0010u, 0x0200u, 0x4000u, 0x0210u,
                                             0x4200u, 0x4010u, 0x03F0u, 0x7FF0u};
constexpr unsigned kDarkWords = 16u;

// Where these pictures keep their pieces. A map base counts whole 32x32 screens of
// $400 words; a background's character base counts 8 KB blocks.
constexpr std::uint8_t kBg1MapBase = 0x04u;  // $2107: screen 1
constexpr std::uint8_t kBg2MapBase = 0x08u;  // $2108: screen 2
constexpr std::uint8_t kBg3MapBase = 0x0Cu;  // $2109: screen 3
constexpr std::uint8_t kCharBases12 = 0x54u; // $210B: BG1 at $8000, BG2 at $A000
constexpr std::uint8_t kCharBases34 = 0x03u; // $210C: BG3 at $6000
constexpr std::uint8_t kSpriteBase = 0x03u;  // $2101: the first sprite table at $C000

constexpr std::uint32_t kBg1Map = 0x0800u;
constexpr std::uint32_t kBg2Map = 0x1000u;
constexpr std::uint32_t kBg3Map = 0x1800u;
constexpr std::uint32_t kBg1Chars = 0x8000u;
constexpr std::uint32_t kBg2Chars = 0xA000u;
constexpr std::uint32_t kBg3Chars = 0x6000u;
constexpr std::uint32_t kSpriteChars = 0xC000u;
constexpr unsigned kSpritePlanes = 4u;
constexpr unsigned kSpritePaletteBase = 128u;

// A BG3 entry's two apply bits and mode 4's axis bit.
constexpr std::uint16_t kToBg1 = 0x2000u;
constexpr std::uint16_t kToBg2 = 0x4000u;
constexpr std::uint16_t kVertical = 0x8000u;

constexpr std::uint8_t kStp = 0xDBu;
constexpr std::uint8_t kLdaImm = 0xA9u;
constexpr std::uint8_t kStaAbs = 0x8Du;

// More than an NTSC frame of master cycles, so a run from power-on reaches the line
// a finished frame is handed over on.
constexpr std::uint64_t kOneFrame = 357364u + 20000u;

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

  // The pixel at picture column x on line `line`. Line 1 is the picture's first
  // row: the frame's line 0 draws nothing.
  [[nodiscard]] Rgba at(unsigned x, unsigned line) const {
    const std::size_t index = (static_cast<std::size_t>(line - 1u) * width + x) * 4u;
    return Rgba{pixels.at(index), pixels.at(index + 1u), pixels.at(index + 2u),
                pixels.at(index + 3u)};
  }
};

std::uint64_t digest(const Picture& picture) {
  std::uint64_t hash = 14695981039346656037ull;
  for (const std::uint8_t byte : picture.pixels) {
    hash ^= byte;
    hash *= 1099511628211ull;
  }
  return hash;
}

std::vector<std::uint8_t> cartridge(std::vector<std::uint8_t> program) {
  program.resize(0x8000u, 0x00u);
  program[0x7FFCu] = 0x00u;  // reset -> $8000
  program[0x7FFDu] = 0x80u;
  return program;
}

// A frame drawn from a placed PPU state by a machine whose program halts at once.
Picture draw(const PpuState& ppu) {
  const std::vector<std::uint8_t> rom = cartridge({kStp});
  Snes machine(SnesConfig{.rom = rom});
  SnesState state = machine.state();
  state.ppu = ppu;
  machine.restore(state);
  Picture picture;
  machine.setFrameObserver(&picture);
  machine.run(kOneFrame);
  return picture;
}

// A frame drawn from a placed PPU state by a machine that begins `program` with
// the beam at (line, hpos): the program's own writes land on the picture.
Picture drawWith(const PpuState& ppu, std::vector<std::uint8_t> program, std::uint16_t line,
                 std::uint16_t hpos) {
  program.push_back(kStp);
  const std::vector<std::uint8_t> rom = cartridge(std::move(program));
  Snes machine(SnesConfig{.rom = rom});
  SnesState state = machine.state();
  state.ppu = ppu;
  state.vpos = line;
  state.hpos = hpos;
  machine.restore(state);
  Picture picture;
  machine.setFrameObserver(&picture);
  machine.run(kOneFrame);
  return picture;
}

// LDA #v ; STA $21xx
std::vector<std::uint8_t> store(std::uint8_t low, std::uint8_t value) {
  return {kLdaImm, value, kStaAbs, low, 0x21u};
}

std::vector<std::uint8_t> joined(std::initializer_list<std::vector<std::uint8_t>> parts) {
  std::vector<std::uint8_t> program;
  for (const std::vector<std::uint8_t>& part : parts) {
    program.insert(program.end(), part.begin(), part.end());
  }
  return program;
}

void putColour(PpuState& ppu, unsigned word, std::uint16_t colour) {
  ppu.cgram[word * 2u] = static_cast<std::uint8_t>(colour & 0xFFu);
  ppu.cgram[word * 2u + 1u] = static_cast<std::uint8_t>(colour >> 8);
}

// A tilemap entry at a tile position of a 32-wide map: vhopppcc cccccccc, or in
// BG3's table the offset and its bits.
void putEntry(PpuState& ppu, std::uint32_t map, unsigned tileX, unsigned tileY,
              std::uint16_t value) {
  const std::uint32_t at = map + (tileY * 32u + tileX) * 2u;
  ppu.vram[at & 0xFFFFu] = static_cast<std::uint8_t>(value & 0xFFu);
  ppu.vram[(at + 1u) & 0xFFFFu] = static_cast<std::uint8_t>(value >> 8);
}

void putTable(PpuState& ppu, unsigned column, unsigned row, std::uint16_t value) {
  putEntry(ppu, kBg3Map, column, row, value);
}

void fillTableRow(PpuState& ppu, unsigned row, std::uint16_t value) {
  for (unsigned column = 0u; column < 32u; ++column) putTable(ppu, column, row, value);
}

constexpr std::uint16_t entry(unsigned tile, unsigned palette, bool priority) {
  return static_cast<std::uint16_t>(tile | (palette << 10) | (priority ? 0x2000u : 0u));
}

// A character every pixel of which is one colour index, at as many bitplanes as
// the caller names: planes 0 and 1 in the low and high bytes of eight words, every
// further pair sixteen bytes on.
void putSolid(PpuState& ppu, std::uint32_t characters, unsigned planes, unsigned tile,
              unsigned index) {
  const std::uint32_t base = characters + tile * 8u * planes;
  for (unsigned row = 0u; row < 8u; ++row) {
    for (unsigned plane = 0u; plane < planes; ++plane) {
      const std::uint32_t at = base + (plane / 2u) * 16u + (plane % 2u) + row * 2u;
      ppu.vram[at & 0xFFFFu] = ((index >> plane) & 1u) != 0u ? 0xFFu : 0x00u;
    }
  }
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

// A solid sprite in the colour at word 129, covering the eight pixels from x on line
// 41: a sprite whose Y is N first draws on line N + 1.
constexpr unsigned kSpriteLine = 41u;
void spriteAt(PpuState& ppu, unsigned index, int x, unsigned priority) {
  putSprite(ppu, index, x, 40u, 0u, static_cast<std::uint8_t>(priority << 4));
  putSolid(ppu, kSpriteChars, kSpritePlanes, 0u, 1u);
}

// A PPU at full brightness with the maps and characters placed, nothing on either
// screen, every scroll at zero but the vertical ones, which are -1 so a map's first
// row is on the picture's first line, and every sprite parked.
PpuState placed(std::uint8_t mode) {
  PpuState ppu;
  ppu.inidisp = 0x0Fu;
  ppu.objsel = kSpriteBase;
  ppu.bgmode = mode;
  ppu.bg1sc = kBg1MapBase;
  ppu.bg2sc = kBg2MapBase;
  ppu.bg3sc = kBg3MapBase;
  ppu.bg12nba = kCharBases12;
  ppu.bg34nba = kCharBases34;
  ppu.bg1vofs = 0x3FFu;
  ppu.bg2vofs = 0x3FFu;
  ppu.bg3vofs = 0u;
  ppu.bg4vofs = 0x3FFu;
  putColour(ppu, 0u, kBackdrop);
  parkSprites(ppu);
  return ppu;
}

// BG1 as the offset pictures read it, alone on the main screen: tile column C in
// hue C mod 8, and tile row R bright or dark by its parity. A sixteen-colour BG1
// takes the hue from its character and the shade from its palette; a 256-colour one
// has a character for each of the sixteen words.
PpuState offsetPicture(std::uint8_t mode) {
  PpuState ppu = placed(mode);
  ppu.tm = 0x01u;
  const bool full = (mode & 0x07u) == 4u;
  for (unsigned n = 0u; n < 8u; ++n) {
    putColour(ppu, 1u + n, kBright[n]);
    putColour(ppu, kDarkWords + 1u + n, kDark[n]);
    if (full) {
      putSolid(ppu, kBg1Chars, 8u, 1u + n, 1u + n);
      putSolid(ppu, kBg1Chars, 8u, 9u + n, kDarkWords + 1u + n);
    } else {
      putSolid(ppu, kBg1Chars, 4u, 1u + n, 1u + n);
    }
  }
  for (unsigned tileY = 0u; tileY < 32u; ++tileY) {
    for (unsigned tileX = 0u; tileX < 32u; ++tileX) {
      const unsigned hue = tileX % 8u;
      const unsigned odd = tileY & 1u;
      const std::uint16_t value = full ? static_cast<std::uint16_t>(1u + hue + 8u * odd)
                                       : entry(1u + hue, odd, false);
      putEntry(ppu, kBg1Map, tileX, tileY, value);
    }
  }
  return ppu;
}

// What that BG1 shows at one of its own positions, both wrapped at its 256 pixels.
Rgba bg1At(unsigned bgX, unsigned bgY) {
  const unsigned column = (bgX & 0xFFu) >> 3;
  const unsigned row = (bgY & 0xFFu) >> 3;
  return out(((row & 1u) != 0u ? kDark : kBright)[column % 8u]);
}

// What it shows at a picture position with its registers as offsetPicture leaves
// them and nothing moved: the column is x, and the vertical offset of -1 puts row
// line - 1 on the line.
Rgba straight(unsigned x, unsigned line) { return bg1At(x, line - 1u); }

// ---- the two modes -------------------------------------------------------------

TEST(SnesPpuOffsetModes, ModeTwoDrawsBg1AndBg2AtSixteenColoursEach) {
  // BG1's tile at (0, 0) is colour 15 in palette 2: word 2 x 16 + 15 = 47. BG2's at
  // (1, 0) is colour 9 in palette 3: word 3 x 16 + 9 = 57.
  PpuState ppu = placed(0x02u);
  ppu.tm = 0x03u;
  putSolid(ppu, kBg1Chars, 4u, 1u, 15u);
  putSolid(ppu, kBg2Chars, 4u, 1u, 9u);
  putEntry(ppu, kBg1Map, 0u, 0u, entry(1u, 2u, false));
  putEntry(ppu, kBg2Map, 1u, 0u, entry(1u, 3u, false));
  putColour(ppu, 47u, kRed);
  putColour(ppu, 57u, kGreen);
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.frames, 1u);
  EXPECT_EQ(picture.at(0u, 1u), out(kRed));
  EXPECT_EQ(picture.at(8u, 1u), out(kGreen));
  EXPECT_EQ(picture.at(16u, 1u), out(kBackdrop));
}

TEST(SnesPpuOffsetModes, ModeTwoShowsNoBg3WhateverTheMainScreenHolds) {
  // BG3 is on the main screen and its tile at (0, 0) is a solid character, but in
  // this mode its map is the table: nothing of it is drawn, and the backdrop shows
  // where BG1 and BG2 are empty. The same state in mode 1 draws it.
  PpuState ppu = placed(0x02u);
  ppu.tm = 0x07u;
  ppu.bg3vofs = 0x3FFu;
  putSolid(ppu, kBg3Chars, 2u, 1u, 1u);
  putEntry(ppu, kBg3Map, 0u, 0u, entry(1u, 0u, false));
  putColour(ppu, 1u, kOrange);
  EXPECT_EQ(draw(ppu).at(0u, 1u), out(kBackdrop));
  ppu.bgmode = 0x01u;
  EXPECT_EQ(draw(ppu).at(0u, 1u), out(kOrange));
}

TEST(SnesPpuOffsetModes, ModeFourDrawsBg1AtTwoHundredAndFiftySixColours) {
  // The pixel is the word: $C5, whatever the entry's palette field holds.
  PpuState ppu = placed(0x04u);
  ppu.tm = 0x01u;
  putSolid(ppu, kBg1Chars, 8u, 1u, 0xC5u);
  putEntry(ppu, kBg1Map, 0u, 0u, entry(1u, 7u, false));
  putColour(ppu, 0xC5u, kOrange);
  EXPECT_EQ(draw(ppu).at(3u, 1u), out(kOrange));
}

TEST(SnesPpuOffsetModes, ModeFourDrawsBg2AtFourColours) {
  // Colour 3 of a four-colour character, sixteen bytes a character: palette 1 is
  // word 1 x 4 + 3 = 7 and palette 5 is word 23.
  PpuState ppu = placed(0x04u);
  ppu.tm = 0x02u;
  putSolid(ppu, kBg2Chars, 2u, 1u, 3u);
  putEntry(ppu, kBg2Map, 0u, 0u, entry(1u, 1u, false));
  putEntry(ppu, kBg2Map, 1u, 0u, entry(1u, 5u, false));
  putColour(ppu, 7u, kRed);
  putColour(ppu, 23u, kGreen);
  const Picture picture = draw(ppu);
  EXPECT_EQ(picture.at(0u, 1u), out(kRed));
  EXPECT_EQ(picture.at(8u, 1u), out(kGreen));
}

TEST(SnesPpuOffsetModes, DirectColourReachesModeFoursBg1) {
  // Pixel $FF with $2130 bit 0 set: red 7 << 2 = 28, green 7 << 2 = 28, blue 3 << 3
  // = 24, which is $639C. Palette 5 adds its b and r bits: blue 28, red 30, $739E.
  PpuState ppu = placed(0x04u);
  ppu.tm = 0x01u;
  ppu.cgwsel = 0x01u;
  putSolid(ppu, kBg1Chars, 8u, 1u, 0xFFu);
  putEntry(ppu, kBg1Map, 0u, 0u, entry(1u, 0u, false));
  putEntry(ppu, kBg1Map, 1u, 0u, entry(1u, 5u, false));
  putColour(ppu, 0xFFu, kGrey);
  const Picture picture = draw(ppu);
  EXPECT_EQ(picture.at(0u, 1u), out(0x639Cu));
  EXPECT_EQ(picture.at(8u, 1u), out(0x739Eu));
}

// The chart both modes keep, front to back: sprites at 3, BG1's high tiles, sprites
// at 2, BG2's high tiles, sprites at 1, BG1's low tiles, sprites at 0, BG2's low
// tiles. Each adjacent pair is drawn alone at one position and the front one shows.
enum class Thing { Sprite, Bg1, Bg2 };
struct ChartPlace {
  Thing thing;
  unsigned priority;
};
constexpr std::array<ChartPlace, 8> kModeTwoAndFourChart{{{Thing::Sprite, 3u},
                                                          {Thing::Bg1, 1u},
                                                          {Thing::Sprite, 2u},
                                                          {Thing::Bg2, 1u},
                                                          {Thing::Sprite, 1u},
                                                          {Thing::Bg1, 0u},
                                                          {Thing::Sprite, 0u},
                                                          {Thing::Bg2, 0u}}};

// BG1 red, BG2 green and the sprite blue, each at (64, 41) and only where named.
PpuState chartPicture(std::uint8_t mode, ChartPlace front, ChartPlace back) {
  PpuState ppu = placed(mode);
  ppu.tm = 0x13u;
  const bool full = (mode & 0x07u) == 4u;
  putSolid(ppu, kBg1Chars, full ? 8u : 4u, 1u, 1u);
  putSolid(ppu, kBg2Chars, full ? 2u : 4u, 1u, 2u);
  putColour(ppu, 1u, kRed);
  putColour(ppu, 2u, kGreen);
  putColour(ppu, kSpritePaletteBase + 1u, kBlue);
  for (const ChartPlace& place : {front, back}) {
    switch (place.thing) {
      case Thing::Sprite:
        spriteAt(ppu, 0u, 64, place.priority);
        break;
      case Thing::Bg1:
        putEntry(ppu, kBg1Map, 8u, 5u, entry(1u, 0u, place.priority != 0u));
        break;
      case Thing::Bg2:
        putEntry(ppu, kBg2Map, 8u, 5u, entry(1u, 0u, place.priority != 0u));
        break;
    }
  }
  return ppu;
}

Rgba colourOf(Thing thing) {
  switch (thing) {
    case Thing::Sprite: return out(kBlue);
    case Thing::Bg1: return out(kRed);
    case Thing::Bg2: return out(kGreen);
  }
  return kBlack;
}

void expectChart(std::uint8_t mode) {
  for (std::size_t n = 0u; n + 1u < kModeTwoAndFourChart.size(); ++n) {
    const ChartPlace front = kModeTwoAndFourChart[n];
    const ChartPlace back = kModeTwoAndFourChart[n + 1u];
    EXPECT_EQ(draw(chartPicture(mode, front, back)).at(64u, kSpriteLine), colourOf(front.thing))
        << "mode " << unsigned{mode} << ", place " << n << " against place " << n + 1u;
  }
}

TEST(SnesPpuOffsetModes, ModeTwoKeepsModeThreesChart) { expectChart(0x02u); }

TEST(SnesPpuOffsetModes, ModeFourKeepsModeThreesChart) { expectChart(0x04u); }

TEST(SnesPpuOffsetModes, BitThreeOfTheModeRegisterMovesNothingInEither) {
  for (const std::uint8_t mode : {std::uint8_t{0x02u}, std::uint8_t{0x04u}}) {
    PpuState ppu = offsetPicture(mode);
    fillTableRow(ppu, 0u, kToBg1 | 16u);
    spriteAt(ppu, 0u, 64, 1u);
    putColour(ppu, kSpritePaletteBase + 1u, kWhite);
    ppu.tm = 0x13u;
    const Picture plain = draw(ppu);
    ppu.bgmode = static_cast<std::uint8_t>(mode | 0x08u);
    EXPECT_EQ(digest(draw(ppu)), digest(plain)) << "mode " << unsigned{mode};
  }
}

// ---- the first column ----------------------------------------------------------

TEST(SnesPpuOffsetModes, WithNoFineScrollTheFirstEightColumnsReadTheRegisters) {
  // Every entry of row 0 moves BG1 by 16. Columns 0-7 are BG1's first tile and take
  // none of it; column 8 is the second, 8 + 16 = 24, BG1's tile column 3.
  // Provisional until the offset-per-tile cartridge is read: anomie alone.
  PpuState ppu = offsetPicture(0x02u);
  fillTableRow(ppu, 0u, kToBg1 | 16u);
  const Picture picture = draw(ppu);
  for (unsigned x = 0u; x < 8u; ++x) EXPECT_EQ(picture.at(x, 1u), straight(x, 1u)) << x;
  EXPECT_EQ(picture.at(8u, 1u), bg1At(24u, 0u));
  EXPECT_NE(straight(8u, 1u), bg1At(24u, 0u));
}

TEST(SnesPpuOffsetModes, UnderAFineScrollOfThreeTheFirstTileEndsAtColumnFour) {
  // BG1HOFS 3: columns 0-4 are background positions 3-7, the first tile. Column 5
  // is the second tile: 5 + (16 | 3) = 24, tile column 3, where the registers alone
  // would read position 8, tile column 1. Provisional: anomie alone.
  PpuState ppu = offsetPicture(0x02u);
  ppu.bg1hofs = 3u;
  fillTableRow(ppu, 0u, kToBg1 | 16u);
  const Picture picture = draw(ppu);
  EXPECT_EQ(picture.at(4u, 1u), bg1At(7u, 0u));
  EXPECT_EQ(picture.at(5u, 1u), bg1At(24u, 0u));
  EXPECT_NE(bg1At(8u, 0u), bg1At(24u, 0u));
}

TEST(SnesPpuOffsetModes, TheSecondBackgroundsFirstTileIsExemptToo) {
  // BG2 alone on the screen, carrying the same ruler from the same characters, and
  // every entry applying to BG2. Column 3 reads BG2 straight; column 8 reads
  // position 24.
  PpuState ppu = offsetPicture(0x02u);
  ppu.bg12nba = 0x44u;  // BG2's characters where BG1's are
  ppu.bg2vofs = 0x3FFu;
  ppu.tm = 0x02u;
  fillTableRow(ppu, 0u, kToBg2 | 16u);
  for (unsigned tileY = 0u; tileY < 32u; ++tileY) {
    for (unsigned tileX = 0u; tileX < 32u; ++tileX) {
      putEntry(ppu, kBg2Map, tileX, tileY, entry(1u + tileX % 8u, tileY & 1u, false));
    }
  }
  const Picture picture = draw(ppu);
  EXPECT_EQ(picture.at(3u, 1u), straight(3u, 1u));
  EXPECT_EQ(picture.at(8u, 1u), bg1At(24u, 0u));
}

// ---- the lookup ----------------------------------------------------------------

// Row 0 of the table moving BG1 by 16 in its even columns and by 32 in its odd ones.
PpuState alternating() {
  PpuState ppu = offsetPicture(0x02u);
  for (unsigned column = 0u; column < 32u; ++column) {
    putTable(ppu, column, 0u, static_cast<std::uint16_t>(kToBg1 | ((column & 1u) != 0u ? 32u : 16u)));
  }
  return ppu;
}

TEST(SnesPpuOffsetModes, BackgroundTileTReadsTableColumnTMinusOne) {
  // Columns 8-15 are BG1's tile 1 and read table column 0, +16: position 24, tile
  // column 3. Columns 16-23 are tile 2 and read column 1, +32: position 48, tile
  // column 6.
  const Picture picture = draw(alternating());
  EXPECT_EQ(picture.at(8u, 1u), bg1At(24u, 0u));
  EXPECT_EQ(picture.at(15u, 1u), bg1At(31u, 0u));
  EXPECT_EQ(picture.at(16u, 1u), bg1At(48u, 0u));
  EXPECT_EQ(picture.at(23u, 1u), bg1At(55u, 0u));
}

TEST(SnesPpuOffsetModes, TheTablesHorizontalScrollMovesWhichColumnIsRead) {
  // BG3HOFS 8: tile 1 reads table column 1 (+32), position 40, tile column 5; tile 2
  // reads column 2 (+16), position 32, tile column 4.
  PpuState ppu = alternating();
  ppu.bg3hofs = 8u;
  const Picture picture = draw(ppu);
  EXPECT_EQ(picture.at(8u, 1u), bg1At(40u, 0u));
  EXPECT_EQ(picture.at(16u, 1u), bg1At(32u, 0u));
}

TEST(SnesPpuOffsetModes, TheTablesFineScrollIsNotRead) {
  // BG3HOFS 11 reads the columns 8 does.
  PpuState eight = alternating();
  eight.bg3hofs = 8u;
  PpuState eleven = alternating();
  eleven.bg3hofs = 11u;
  EXPECT_EQ(digest(draw(eleven)), digest(draw(eight)));
}

TEST(SnesPpuOffsetModes, TheTwoRowsAreTheTablesVerticalScrollAndEightBelowOnEveryLine) {
  // BG3VOFS 16: the horizontal entry is row 2, +32, and the vertical entry row 3,
  // +8, on every line. Rows 0, 1 and 4-31 move BG1 by 40 both ways, which a row
  // read that moved with the line would reach. Line 1 column 8: position (40, 9),
  // tile (5, 1); line 100 column 8: position (40, 108), tile (5, 13). Provisional:
  // anomie alone.
  PpuState ppu = offsetPicture(0x02u);
  ppu.bg3vofs = 16u;
  for (unsigned row = 0u; row < 32u; ++row) fillTableRow(ppu, row, kToBg1 | 40u);
  fillTableRow(ppu, 2u, kToBg1 | 32u);
  fillTableRow(ppu, 3u, kToBg1 | 8u);
  const Picture picture = draw(ppu);
  EXPECT_EQ(picture.at(8u, 1u), bg1At(40u, 9u));
  EXPECT_EQ(picture.at(8u, 100u), bg1At(40u, 108u));
  EXPECT_NE(bg1At(40u, 9u), bg1At(40u, 0u));
}

TEST(SnesPpuOffsetModes, TheTablesScreenRegisterSteersIt) {
  // The table moved to screen 4 and made 64 columns wide, which puts columns 32-63
  // on the next screen, $400 words on. With BG3HOFS 256, tile 1 reads column 32:
  // that screen's first entry, +32, so position 40, tile column 5. Every entry of the
  // first screen's row 0 is +16.
  PpuState ppu = offsetPicture(0x02u);
  constexpr std::uint32_t kTable = 0x2000u;
  constexpr std::uint32_t kTablesRightHalf = kTable + 0x800u;
  ppu.bg3sc = 0x11u;  // screen 4, 64 x 32
  ppu.bg3hofs = 256u;
  for (unsigned column = 0u; column < 32u; ++column) {
    putEntry(ppu, kTable, column, 0u, kToBg1 | 16u);
  }
  putEntry(ppu, kTablesRightHalf, 0u, 0u, kToBg1 | 32u);
  EXPECT_EQ(draw(ppu).at(8u, 1u), bg1At(40u, 0u));
  ppu.bg3sc = 0x10u;  // screen 4, 32 x 32: column 32 wraps to column 0
  EXPECT_EQ(draw(ppu).at(8u, 1u), bg1At(24u, 0u));
}

TEST(SnesPpuOffsetModes, TheTablesCharacterBaseIsNotRead) {
  PpuState ppu = alternating();
  const Picture before = draw(ppu);
  ppu.bg34nba = 0x06u;  // three blocks from the placed $03: an odd number of columns
  EXPECT_EQ(digest(draw(ppu)), digest(before));
}

TEST(SnesPpuOffsetModes, ASixteenBySixteenTableServesTwoColumnsAnEntry) {
  // $2105 bit 6: a table entry covers sixteen BG3 positions. BG1's tiles 1 and 2
  // read BG3 positions 0 and 8, both in entry 0, +16; tiles 3 and 4 read 16 and 24,
  // entry 1, +32. The vertical entry at position 8 is the same entry, so line 1
  // reads position 17 under the first pair and 33 under the second.
  // Provisional: anomie alone.
  PpuState ppu = alternating();
  ppu.bgmode = 0x42u;
  const Picture picture = draw(ppu);
  EXPECT_EQ(picture.at(8u, 1u), bg1At(24u, 17u));
  EXPECT_EQ(picture.at(16u, 1u), bg1At(32u, 17u));
  EXPECT_EQ(picture.at(24u, 1u), bg1At(56u, 33u));
  EXPECT_EQ(picture.at(32u, 1u), bg1At(64u, 33u));
  EXPECT_NE(bg1At(32u, 17u), bg1At(48u, 17u));
}

TEST(SnesPpuOffsetModes, ASixteenBySixteenTableCanServeBothAxesFromOneWord) {
  // BG3VOFS 0 at 16x16: the vertical entry at position 8 is the same 16-row block
  // as the horizontal one at 0. Every entry of row 0 is +8 to BG1 and row 1 applies
  // nothing, so the one word moves column 8 to position (16, 9), tile (2, 1).
  // Provisional: anomie alone.
  PpuState ppu = offsetPicture(0x42u);
  fillTableRow(ppu, 0u, kToBg1 | 8u);
  EXPECT_EQ(draw(ppu).at(8u, 1u), bg1At(16u, 9u));
  ppu.bgmode = 0x02u;  // at 8x8 the vertical entry is row 1, which applies nothing
  EXPECT_EQ(draw(ppu).at(8u, 1u), bg1At(16u, 0u));
}

TEST(SnesPpuOffsetModes, ASixteenBySixteenBackgroundTakesItsOffsetsPerEightPixels) {
  // BG1 at 16x16 blocks: its tiles are counted in eights all the same, so columns
  // 8-15 and 16-23 read different table columns.
  PpuState ppu = alternating();
  ppu.bgmode = 0x12u;
  const Picture picture = draw(ppu);
  PpuState plain = ppu;
  fillTableRow(plain, 0u, 0u);
  const Picture unmoved = draw(plain);
  // Column 8 is block position 24 against 8; column 16 is 48 against 16.
  EXPECT_EQ(picture.at(8u, 1u), unmoved.at(24u, 1u));
  EXPECT_EQ(picture.at(16u, 1u), unmoved.at(48u, 1u));
  EXPECT_NE(unmoved.at(16u, 1u), unmoved.at(48u, 1u));
}

// ---- the offsets ---------------------------------------------------------------

TEST(SnesPpuOffsetModes, AHorizontalEntryDropsItsLowThreeBits) {
  // +19 moves by 16: column 13 reads position 29, tile column 3, where 32 would be
  // tile column 4.
  PpuState ppu = offsetPicture(0x02u);
  fillTableRow(ppu, 0u, kToBg1 | 19u);
  EXPECT_EQ(draw(ppu).at(13u, 1u), bg1At(29u, 0u));
  EXPECT_NE(bg1At(29u, 0u), bg1At(32u, 0u));
}

TEST(SnesPpuOffsetModes, AHorizontalEntryKeepsTheRegistersFineScroll) {
  // BG1HOFS 3 and +16: column 13 is 13 + 16 + 3 = 32, tile column 4.
  PpuState ppu = offsetPicture(0x02u);
  ppu.bg1hofs = 3u;
  fillTableRow(ppu, 0u, kToBg1 | 16u);
  EXPECT_EQ(draw(ppu).at(13u, 1u), bg1At(32u, 0u));
}

TEST(SnesPpuOffsetModes, TheSecondApplyBitMovesBg2AndNotBg1) {
  PpuState ppu = offsetPicture(0x02u);
  fillTableRow(ppu, 0u, kToBg2 | 16u);
  EXPECT_EQ(draw(ppu).at(8u, 1u), straight(8u, 1u));
}

TEST(SnesPpuOffsetModes, AnEntryWithNeitherBitMovesNeitherAxis) {
  PpuState ppu = offsetPicture(0x02u);
  fillTableRow(ppu, 0u, 16u);
  fillTableRow(ppu, 1u, 8u);
  EXPECT_EQ(draw(ppu).at(8u, 1u), straight(8u, 1u));
}

TEST(SnesPpuOffsetModes, AVerticalEntryPutsTheLinePlusItOnTheLine) {
  // Row 1 holds +8: line 1 reads position 9, tile row 1.
  PpuState ppu = offsetPicture(0x02u);
  fillTableRow(ppu, 1u, kToBg1 | 8u);
  const Picture picture = draw(ppu);
  EXPECT_EQ(picture.at(8u, 1u), bg1At(8u, 9u));
  EXPECT_NE(bg1At(8u, 9u), straight(8u, 1u));
}

TEST(SnesPpuOffsetModes, AVerticalEntryReplacesTheRegisterWhole) {
  // BG1VOFS 5 and +8: line 3 reads position 3 + 8 = 11, tile row 1 — not 3 + 13 =
  // 16, tile row 2.
  PpuState ppu = offsetPicture(0x02u);
  ppu.bg1vofs = 5u;
  fillTableRow(ppu, 1u, kToBg1 | 8u);
  EXPECT_EQ(draw(ppu).at(8u, 3u), bg1At(8u, 11u));
  EXPECT_NE(bg1At(8u, 11u), bg1At(8u, 16u));
}

TEST(SnesPpuOffsetModes, AReplacedOffsetWrapsAtTheMapsSize) {
  // +1016 on a 256-pixel map: column 8 is position 1024, which is 0.
  PpuState ppu = offsetPicture(0x02u);
  fillTableRow(ppu, 0u, kToBg1 | 1016u);
  EXPECT_EQ(draw(ppu).at(8u, 1u), bg1At(0u, 0u));
}

// ---- mode 4's one entry --------------------------------------------------------

TEST(SnesPpuOffsetModes, ModeFourReadsAnEntryWithBitFifteenClearAsHorizontal) {
  // Row 0 is +16 to BG1 and row 1 +8: only row 0 is read, and only horizontally, so
  // column 8 is position (24, 0).
  PpuState ppu = offsetPicture(0x04u);
  fillTableRow(ppu, 0u, kToBg1 | 16u);
  fillTableRow(ppu, 1u, kToBg1 | 8u);
  EXPECT_EQ(draw(ppu).at(8u, 1u), bg1At(24u, 0u));
}

TEST(SnesPpuOffsetModes, ModeFourReadsAnEntryWithBitFifteenSetAsVertical) {
  // Row 0 is +8 with bit 15: column 8 is position (8, 9), and the horizontal offset
  // stays the register's.
  PpuState ppu = offsetPicture(0x04u);
  fillTableRow(ppu, 0u, kVertical | kToBg1 | 8u);
  EXPECT_EQ(draw(ppu).at(8u, 1u), bg1At(8u, 9u));
}

TEST(SnesPpuOffsetModes, ModeFoursEntryStillNeedsItsApplyBit) {
  PpuState ppu = offsetPicture(0x04u);
  fillTableRow(ppu, 0u, kVertical | 8u);
  EXPECT_EQ(draw(ppu).at(8u, 1u), straight(8u, 1u));
}

// ---- the table at the dot ------------------------------------------------------

TEST(SnesPpuOffsetModes, TheTablesScrollWrittenMidLineMovesTheColumnsAfterTheWrite) {
  // A program beginning at dot 75 of line 50 writes BG3HOFS = 8 in two halves, the
  // second landing some twenty-five dots later. Column 56 was drawn before either
  // write. Column 209 is tile 26: before the write it reads table column 25, +32,
  // and after it column 26, +16. Each column is compared with the frame the same
  // table draws with the scroll set throughout and with it never set.
  PpuState before = alternating();
  PpuState after = alternating();
  after.bg3hofs = 8u;
  const Picture never = draw(before);
  const Picture always = draw(after);
  const Picture picture =
      drawWith(before, joined({store(0x11u, 0x08u), store(0x11u, 0x00u)}), 50u, 300u);
  EXPECT_EQ(picture.at(56u, 50u), never.at(56u, 50u));
  EXPECT_EQ(picture.at(209u, 50u), always.at(209u, 50u));
  EXPECT_NE(never.at(209u, 50u), always.at(209u, 50u));
  EXPECT_EQ(picture.at(209u, 51u), always.at(209u, 51u));
  EXPECT_EQ(picture.at(10u, 50u), kBlack);
}

TEST(SnesPpuOffsetModes, TheTablesEntryWrittenUnderForcedBlankReachesTheSameLineWhenTheBlankLifts) {
  // A program beginning at master cycle 300 of line 50 forces the blank at picture
  // column 65, writes BG3's entry for the tile column at 136 through the video port
  // while the memory is reachable, and lifts the blank at column 134. The positions
  // the blank covered are black; the ones after it read the offset the program wrote,
  // on the same line it wrote it on. The table's entry for picture tile T is BG3's
  // tile T - 1, so the tile at column 136 reads BG3's tile 16.
  PpuState before = offsetPicture(0x02u);
  PpuState after = before;
  putTable(after, 16u, 0u, kToBg1 | 8u);
  const Picture never = draw(before);
  const Picture always = draw(after);
  const Picture picture = drawWith(before,
                                   joined({
                                       store(0x00u, 0x80u),  // forced blank
                                       store(0x15u, 0x80u),  // step after the high byte
                                       store(0x16u, 0x10u),  // word $0C10: BG3's tile 16, row 0
                                       store(0x17u, 0x0Cu),
                                       store(0x18u, 0x08u),  // the offset
                                       store(0x19u, 0x20u),  // and the bit that applies it to BG1
                                       store(0x00u, 0x0Fu),  // the screen on again
                                   }),
                                   50u, 300u);
  EXPECT_EQ(picture.at(60u, 50u), never.at(60u, 50u));
  EXPECT_EQ(picture.at(100u, 50u), kBlack);
  EXPECT_NE(never.at(140u, 50u), always.at(140u, 50u));
  EXPECT_EQ(picture.at(140u, 50u), always.at(140u, 50u));
  EXPECT_EQ(picture.at(140u, 51u), always.at(140u, 51u));
  EXPECT_EQ(picture.at(10u, 50u), kBlack);
}

TEST(SnesPpuOffsetModes, AnEntryWrittenInVerticalBlankReachesTheNextPicture) {
  // A program begun in vertical blank writes table entry (0, 0) = +16 through the
  // video port. The next picture is the first one delivered, and it carries it.
  PpuState ppu = offsetPicture(0x02u);
  const std::vector<std::uint8_t> program = joined({
      store(0x15u, 0x80u),  // step after the high byte
      store(0x16u, 0x00u),  // word $0C00: the table's first entry
      store(0x17u, 0x0Cu),
      store(0x18u, 16u),
      store(0x19u, 0x20u),
      {kStp},
  });
  const std::vector<std::uint8_t> rom = cartridge(program);
  Snes machine(SnesConfig{.rom = rom});
  SnesState state = machine.state();
  state.ppu = ppu;
  state.vpos = 230u;
  state.inVblank = true;
  state.vblankBeginLine = kVblankStartLine;
  machine.restore(state);
  Picture picture;
  machine.setFrameObserver(&picture);
  machine.run(2u * kOneFrame);
  ASSERT_GE(picture.frames, 1u);
  EXPECT_EQ(picture.at(8u, 1u), bg1At(24u, 0u));
  EXPECT_EQ(picture.at(16u, 1u), straight(16u, 1u));
}

// ---- mosaic --------------------------------------------------------------------
//
// The mosaic pictures read one pattern: a pixel at background column X and row Y is
// hue (Y mod 4) + 4 (X mod 2) of the bright eight, so its colour names the row
// (mod 4) and the column (mod 2) its block's corner was read from.

constexpr std::uint64_t kLineMaster = 1364u;

constexpr unsigned patternIndex(unsigned column, unsigned row) {
  return 1u + (row & 3u) + 4u * (column & 1u);
}
Rgba patternAt(unsigned bgX, unsigned bgY) { return out(kBright[patternIndex(bgX, bgY) - 1u]); }

// A sixteen-colour character drawing the pattern: its pixel at (column, row) is
// patternIndex(column, row).
void putPatternCharacter(PpuState& ppu, std::uint32_t characters, unsigned tile) {
  const std::uint32_t base = characters + tile * 32u;
  for (unsigned row = 0u; row < 8u; ++row) {
    for (unsigned plane = 0u; plane < 4u; ++plane) {
      std::uint8_t bits = 0u;
      for (unsigned column = 0u; column < 8u; ++column) {
        const unsigned index = patternIndex(column, row);
        bits = static_cast<std::uint8_t>(bits | (((index >> plane) & 1u) << (7u - column)));
      }
      ppu.vram[base + (plane / 2u) * 16u + (plane % 2u) + row * 2u] = bits;
    }
  }
}

// Mode 1 with BG1 and BG2 both drawing the pattern, BG1 alone on the main screen,
// and $2106 as given.
PpuState mosaicPicture(std::uint8_t mosaic) {
  PpuState ppu = placed(0x01u);
  ppu.tm = 0x01u;
  ppu.mosaic = mosaic;
  for (unsigned n = 0u; n < 8u; ++n) putColour(ppu, 1u + n, kBright[n]);
  putPatternCharacter(ppu, kBg1Chars, 1u);
  putPatternCharacter(ppu, kBg2Chars, 1u);
  for (unsigned tileY = 0u; tileY < 32u; ++tileY) {
    for (unsigned tileX = 0u; tileX < 32u; ++tileX) {
      putEntry(ppu, kBg1Map, tileX, tileY, entry(1u, 0u, false));
      putEntry(ppu, kBg2Map, tileX, tileY, entry(1u, 0u, false));
    }
  }
  return ppu;
}

// The pattern at a picture position with the vertical offset of -1 and nothing
// mosaiced.
Rgba sharp(unsigned x, unsigned line) { return patternAt(x, line - 1u); }

// ---- the block -----------------------------------------------------------------

TEST(SnesPpuMosaic, SizeOneMakesTwoByTwoBlocksOfTheCornerPixel) {
  // $11: 2x2 blocks on BG1, rows of them beginning on lines 1, 3, 5 ...
  const Picture picture = draw(mosaicPicture(0x11u));
  EXPECT_EQ(picture.at(1u, 1u), sharp(0u, 1u));
  EXPECT_EQ(picture.at(3u, 2u), sharp(2u, 1u));
  EXPECT_EQ(picture.at(3u, 3u), sharp(2u, 3u));
  EXPECT_NE(sharp(1u, 1u), sharp(0u, 1u));
  EXPECT_NE(sharp(3u, 2u), sharp(2u, 1u));
}

TEST(SnesPpuMosaic, SizeFifteenMakesSixteenBySixteenBlocks) {
  const Picture picture = draw(mosaicPicture(0xF1u));
  EXPECT_EQ(picture.at(15u, 16u), sharp(0u, 1u));
  EXPECT_EQ(picture.at(31u, 32u), sharp(16u, 17u));
  EXPECT_EQ(picture.at(16u, 17u), sharp(16u, 17u));
  EXPECT_NE(sharp(15u, 16u), sharp(0u, 1u));
}

TEST(SnesPpuMosaic, SizeZeroIsThePictureUnchanged) {
  EXPECT_EQ(digest(draw(mosaicPicture(0x01u))), digest(draw(mosaicPicture(0x00u))));
}

TEST(SnesPpuMosaic, ASizeWithNoLayerEnabledChangesNothing) {
  EXPECT_EQ(digest(draw(mosaicPicture(0xF0u))), digest(draw(mosaicPicture(0x00u))));
}

TEST(SnesPpuMosaic, BitOneMosaicsBg2AndLeavesBg1Sharp) {
  PpuState ppu = mosaicPicture(0x12u);
  EXPECT_EQ(digest(draw(ppu)), digest(draw(mosaicPicture(0x00u))));
  ppu.tm = 0x02u;
  EXPECT_EQ(draw(ppu).at(1u, 1u), sharp(0u, 1u));
}

TEST(SnesPpuMosaic, BitThreeMosaicsBg4) {
  // Mode 0's BG4 at four colours: its character is colour 1 in even columns and 3
  // in odd ones, and its subset begins at word 96, so words 97 and 99.
  PpuState ppu = placed(0x00u);
  constexpr std::uint32_t kBg4Chars = 0xE000u;
  constexpr std::uint32_t kBg4Map = 0x2000u;
  ppu.tm = 0x08u;
  ppu.bg4sc = 0x10u;
  ppu.bg34nba = 0x73u;
  for (unsigned row = 0u; row < 8u; ++row) {
    ppu.vram[kBg4Chars + 16u + row * 2u] = 0xFFu;       // plane 0: every pixel
    ppu.vram[kBg4Chars + 16u + row * 2u + 1u] = 0x55u;  // plane 1: the odd columns
  }
  putEntry(ppu, kBg4Map, 0u, 0u, entry(1u, 0u, false));
  putColour(ppu, 97u, kRed);
  putColour(ppu, 99u, kGreen);
  ppu.mosaic = 0x38u;
  EXPECT_EQ(draw(ppu).at(1u, 1u), out(kRed));
  ppu.mosaic = 0x37u;
  EXPECT_EQ(draw(ppu).at(1u, 1u), out(kGreen));
}

TEST(SnesPpuMosaic, ATransparentCornerMakesATransparentBlock) {
  // BG1's tile (0, 0) is the pattern with its top-left pixel clear; BG2 behind it is
  // a solid orange. Under $11 the block at (0-1, 1-2) takes that clear pixel, so
  // BG2 shows through all of it.
  PpuState ppu = mosaicPicture(0x11u);
  ppu.tm = 0x03u;
  putPatternCharacter(ppu, kBg1Chars, 2u);
  for (unsigned plane = 0u; plane < 4u; ++plane) {
    std::uint8_t& bits = ppu.vram[kBg1Chars + 64u + (plane / 2u) * 16u + (plane % 2u)];
    bits = static_cast<std::uint8_t>(bits & 0x7Fu);
  }
  putEntry(ppu, kBg1Map, 0u, 0u, entry(2u, 0u, false));
  putSolid(ppu, kBg2Chars, 4u, 1u, 9u);
  putColour(ppu, 9u, kOrange);
  const Picture picture = draw(ppu);
  EXPECT_EQ(picture.at(1u, 1u), out(kOrange));
  EXPECT_EQ(picture.at(1u, 2u), out(kOrange));
  EXPECT_EQ(picture.at(2u, 1u), sharp(2u, 1u));
}

// ---- what travels with the corner ----------------------------------------------

TEST(SnesPpuMosaic, TheCornersPriorityBitDecidesTheWholeBlock) {
  // BG1's first tile column is at priority 1 and its second at 0; a sprite at
  // priority 2 covers x 8-15 of line 41. Mode 1 puts BG1's high tiles in front of
  // that sprite and its low ones behind. Under $F1 the block 0-15 x 33-48 takes its
  // corner from the first column, so the sprite is behind all of it.
  PpuState ppu = mosaicPicture(0xF1u);
  ppu.tm = 0x11u;
  for (unsigned tileY = 0u; tileY < 32u; ++tileY) {
    putEntry(ppu, kBg1Map, 0u, tileY, entry(1u, 0u, true));
  }
  spriteAt(ppu, 0u, 8, 2u);
  putColour(ppu, kSpritePaletteBase + 1u, kWhite);
  EXPECT_EQ(draw(ppu).at(8u, kSpriteLine), sharp(0u, 33u));
  ppu.mosaic = 0x00u;
  EXPECT_EQ(draw(ppu).at(8u, kSpriteLine), out(kWhite));
}

TEST(SnesPpuMosaic, TheCornersDirectColourFillsTheBlock) {
  // Mode 3, direct colour: the character's top-left pixel is $FF, $639C composed,
  // and every other pixel $01, red 4. Under $F1 all of block 0-15 x 1-16 is $639C.
  PpuState ppu = placed(0x03u);
  ppu.tm = 0x01u;
  ppu.cgwsel = 0x01u;
  ppu.mosaic = 0xF1u;
  putSolid(ppu, kBg1Chars, 8u, 1u, 0x01u);
  for (unsigned plane = 1u; plane < 8u; ++plane) {
    std::uint8_t& bits = ppu.vram[kBg1Chars + 64u + (plane / 2u) * 16u + (plane % 2u)];
    bits = static_cast<std::uint8_t>(bits | 0x80u);
  }
  for (unsigned tileY = 0u; tileY < 32u; ++tileY) {
    for (unsigned tileX = 0u; tileX < 32u; ++tileX) {
      putEntry(ppu, kBg1Map, tileX, tileY, entry(1u, 0u, false));
    }
  }
  const Picture picture = draw(ppu);
  EXPECT_EQ(picture.at(5u, 5u), out(0x639Cu));
  EXPECT_EQ(picture.at(15u, 16u), out(0x639Cu));
  ppu.mosaic = 0x00u;
  EXPECT_EQ(draw(ppu).at(5u, 5u), out(0x0004u));
}

TEST(SnesPpuMosaic, ASpriteOverAMosaicedBackgroundKeepsItsOwnPixels) {
  // A sprite whose columns alternate colours 1 and 2, in front of BG1 under $F1.
  PpuState ppu = mosaicPicture(0xF1u);
  ppu.tm = 0x11u;
  putSprite(ppu, 0u, 64, 40u, 0u, 0x30u);
  for (unsigned row = 0u; row < 8u; ++row) {
    ppu.vram[kSpriteChars + row * 2u] = 0xAAu;       // plane 0: the even columns
    ppu.vram[kSpriteChars + row * 2u + 1u] = 0x55u;  // plane 1: the odd columns
  }
  putColour(ppu, kSpritePaletteBase + 1u, kWhite);
  putColour(ppu, kSpritePaletteBase + 2u, kOrange);
  const Picture picture = draw(ppu);
  EXPECT_EQ(picture.at(64u, kSpriteLine), out(kWhite));
  EXPECT_EQ(picture.at(65u, kSpriteLine), out(kOrange));
}

// ---- where the blocks stand ----------------------------------------------------

TEST(SnesPpuMosaic, AFineScrollLeavesTheBlockEdgesOnEvenColumns) {
  // BG1HOFS 3 under $11: columns 2 and 3 are one block, cornered at column 2, which
  // is background column 5. Blocks aligned to the background would corner column 3
  // at background column 6.
  PpuState ppu = mosaicPicture(0x11u);
  ppu.bg1hofs = 3u;
  const Picture picture = draw(ppu);
  EXPECT_EQ(picture.at(2u, 1u), patternAt(5u, 0u));
  EXPECT_EQ(picture.at(3u, 1u), patternAt(5u, 0u));
  EXPECT_NE(patternAt(6u, 0u), patternAt(5u, 0u));
}

TEST(SnesPpuMosaic, TheFirstRowOfBlocksBeginsOnLineOneWhateverTheVerticalScroll) {
  // BG1VOFS 1 under $21: rows of three from line 1, so line 3 shows line 1, which is
  // background row 2. Rows aligned to the background would begin on line 2 and
  // show row 3 there.
  PpuState ppu = mosaicPicture(0x21u);
  ppu.bg1vofs = 1u;
  const Picture picture = draw(ppu);
  EXPECT_EQ(picture.at(0u, 3u), patternAt(0u, 2u));
  EXPECT_EQ(picture.at(0u, 4u), patternAt(0u, 5u));
  EXPECT_NE(patternAt(0u, 3u), patternAt(0u, 2u));
}

TEST(SnesPpuMosaic, TheTallerPictureCountsItsRowsFromLineOneToo) {
  // SETINI bit 2 under $21: rows of three from line 1 reach line 235, 238; line 237
  // shows line 235 and line 239 shows line 238.
  PpuState ppu = mosaicPicture(0x21u);
  ppu.setini = 0x04u;
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.height, 239u);
  EXPECT_EQ(picture.at(0u, 237u), sharp(0u, 235u));
  EXPECT_EQ(picture.at(0u, 239u), sharp(0u, 238u));
  EXPECT_NE(sharp(0u, 237u), sharp(0u, 235u));
}

// ---- the counter ---------------------------------------------------------------

TEST(SnesPpuMosaic, SizeTwoGivesRowsBeginningOnLinesOneFourAndSeven) {
  // Rows of three lines. The pattern repeats every four rows, so a size whose rows
  // are four lines would begin every row on the same class of line and could not
  // tell one row from another; rows of three begin on lines of three classes.
  const Picture picture = draw(mosaicPicture(0x21u));
  EXPECT_EQ(picture.at(0u, 3u), sharp(0u, 1u));
  EXPECT_EQ(picture.at(0u, 5u), sharp(0u, 4u));
  EXPECT_EQ(picture.at(0u, 8u), sharp(0u, 7u));
  EXPECT_EQ(picture.at(0u, 10u), sharp(0u, 10u));
  EXPECT_NE(sharp(0u, 3u), sharp(0u, 1u));
  EXPECT_NE(sharp(0u, 5u), sharp(0u, 4u));
  EXPECT_NE(sharp(0u, 8u), sharp(0u, 7u));
  EXPECT_NE(sharp(0u, 4u), sharp(0u, 1u));
}

// A frame drawn by a program begun at the start of line 6, with the counter as a
// run from the top of the frame under $31 leaves it there: the row of blocks begun
// on line 5.
Picture drawFromLineSix(std::vector<std::uint8_t> program) {
  PpuState ppu = mosaicPicture(0x31u);
  ppu.mosaicBlockLine = 5u;
  ppu.mosaicBlockSize = 3u;
  return drawWith(ppu, std::move(program), 6u, 0u);
}

TEST(SnesPpuMosaic, ASizeWrittenPartWayDownARowTakesEffectWhenTheRowEnds) {
  // $11 written on line 6: the row begun on line 5 runs its four lines, so line 8
  // shows line 5; rows of two then begin on 9 and 11. Provisional until the mosaic
  // cartridge is read: fullsnes, against anomie's restart on the written line.
  const Picture picture = drawFromLineSix(store(0x06u, 0x11u));
  EXPECT_EQ(picture.at(0u, 8u), sharp(0u, 5u));
  EXPECT_EQ(picture.at(0u, 10u), sharp(0u, 9u));
  EXPECT_EQ(picture.at(0u, 12u), sharp(0u, 11u));
  EXPECT_NE(sharp(0u, 8u), sharp(0u, 5u));
  EXPECT_NE(sharp(0u, 10u), sharp(0u, 9u));
}

TEST(SnesPpuMosaic, RewritingTheSizeItHoldsChangesNothing) {
  const std::vector<std::uint8_t> idle(5u, 0xEAu);  // the same length, no write
  EXPECT_EQ(digest(drawFromLineSix(store(0x06u, 0x31u))), digest(drawFromLineSix(idle)));
}

TEST(SnesPpuMosaic, SizeZeroWrittenPartWayDownARowEndsItWhereItWouldHaveEnded) {
  // $01 on line 6: line 8 still shows line 5, and from line 9 every line is its own.
  const Picture picture = drawFromLineSix(store(0x06u, 0x01u));
  EXPECT_EQ(picture.at(1u, 8u), sharp(1u, 5u));
  EXPECT_EQ(picture.at(1u, 9u), sharp(1u, 9u));
  EXPECT_EQ(picture.at(1u, 10u), sharp(1u, 10u));
}

TEST(SnesPpuMosaic, ASnapshotTakenPartWayDownARowDrawsTheRestOfIt) {
  // One machine runs into line 7 under $31; a second is restored from its state
  // there. Both draw line 8 from line 5.
  const std::vector<std::uint8_t> rom = cartridge({kStp});
  Snes first(SnesConfig{.rom = rom});
  SnesState state = first.state();
  state.ppu = mosaicPicture(0x31u);
  first.restore(state);
  Picture unbroken;
  first.setFrameObserver(&unbroken);
  first.run(kLineMaster * 7u + 200u);
  const SnesState middle = first.state();
  ASSERT_EQ(middle.vpos, 7u);
  EXPECT_EQ(middle.ppu.mosaicBlockLine, 5u);

  Snes second(SnesConfig{.rom = rom});
  second.restore(middle);
  Picture restored;
  second.setFrameObserver(&restored);
  first.run(kOneFrame);
  second.run(kOneFrame);
  for (unsigned x = 0u; x < 256u; x += 5u) {
    EXPECT_EQ(restored.at(x, 8u), unbroken.at(x, 8u)) << x;
  }
  EXPECT_EQ(restored.at(1u, 8u), sharp(0u, 5u));
}

// ---- before the windows and colour math ----------------------------------------

TEST(SnesPpuMosaic, AWindowClipsPartOfABlock) {
  // Window 1 from column 3 masks BG1 on the main screen. Under $31 the block at
  // columns 0-3 keeps its corner pixel in columns 0-2 and loses column 3.
  PpuState ppu = mosaicPicture(0x31u);
  ppu.w12sel = 0x02u;
  ppu.wh0 = 3u;
  ppu.wh1 = 255u;
  ppu.tmw = 0x01u;
  const Picture picture = draw(ppu);
  EXPECT_EQ(picture.at(1u, 1u), sharp(0u, 1u));
  EXPECT_EQ(picture.at(2u, 1u), sharp(0u, 1u));
  EXPECT_EQ(picture.at(3u, 1u), out(kBackdrop));
}

TEST(SnesPpuMosaic, AMosaicedLayerTakesColourMathLikeASharpOne) {
  // BG1's corner at (0, 1) is red 31, and the sub screen's BG2 is grey 1 there:
  // added, red 31, green 1, blue 1.
  PpuState ppu = mosaicPicture(0x31u);
  ppu.ts = 0x02u;
  ppu.cgwsel = 0x02u;
  ppu.cgadsub = 0x01u;
  putSolid(ppu, kBg2Chars, 4u, 1u, 9u);
  putColour(ppu, 9u, 0x0421u);
  ASSERT_EQ(sharp(0u, 1u), out(kRed));
  EXPECT_EQ(draw(ppu).at(1u, 1u), rgb(31u, 1u, 1u));
}

TEST(SnesPpuMosaic, TheBlackRegionBlackensPartOfABlock) {
  // $2130's upper region set to inside the colour window, which runs from column 3.
  PpuState ppu = mosaicPicture(0x31u);
  ppu.wobjsel = 0x20u;
  ppu.wh0 = 3u;
  ppu.wh1 = 255u;
  ppu.cgwsel = 0x80u;
  const Picture picture = draw(ppu);
  EXPECT_EQ(picture.at(1u, 1u), sharp(0u, 1u));
  EXPECT_EQ(picture.at(3u, 1u), kBlack);
  EXPECT_NE(sharp(1u, 1u), sharp(0u, 1u));
}

// ---- mosaic with the offset table ----------------------------------------------

TEST(SnesPpuMosaic, ABlockCarriesTheOffsetResolvedCorner) {
  // The alternating table under $F1: column 26 is in the block cornered at column
  // 16, BG1's tile 2, which reads table column 1, +32: position 48, tile column 6.
  // Column 26 on its own is tile 3, +16: tile column 5.
  PpuState ppu = alternating();
  ppu.mosaic = 0xF1u;
  EXPECT_EQ(draw(ppu).at(26u, 1u), bg1At(48u, 0u));
  EXPECT_NE(bg1At(48u, 0u), bg1At(42u, 0u));
}

TEST(SnesPpuMosaic, TheLookupCountsFromTheBlocksCornerColumn) {
  // BG1HOFS 3, every entry +16, under $31: column 6 is in the block cornered at
  // column 4, which is BG1's first tile and takes no offset — position 7. Column 6
  // on its own is the second tile, at position 25.
  PpuState ppu = offsetPicture(0x02u);
  ppu.bg1hofs = 3u;
  ppu.mosaic = 0x31u;
  fillTableRow(ppu, 0u, kToBg1 | 16u);
  EXPECT_EQ(draw(ppu).at(6u, 1u), bg1At(7u, 0u));
  EXPECT_NE(bg1At(7u, 0u), bg1At(25u, 0u));
}

// ---- the size at the dot -------------------------------------------------------

TEST(SnesPpuMosaic, ASizeWrittenMidLineChangesTheBlocksAfterTheWrite) {
  // Line 50 in a row of two blocks begun on line 49, and $21 written from dot 75:
  // column 57 was drawn in blocks of two, cornered at 56; column 202 in blocks of
  // three, cornered at 201. Both are read from line 49. Three-wide blocks are used
  // because their corners fall on odd columns, which the pattern can tell apart.
  PpuState ppu = mosaicPicture(0x11u);
  ppu.mosaicBlockLine = 49u;
  ppu.mosaicBlockSize = 1u;
  const Picture picture = drawWith(ppu, store(0x06u, 0x21u), 50u, 300u);
  EXPECT_EQ(picture.at(57u, 50u), sharp(56u, 49u));
  EXPECT_EQ(picture.at(202u, 50u), sharp(201u, 49u));
  EXPECT_NE(sharp(56u, 49u), sharp(57u, 49u));
  EXPECT_NE(sharp(202u, 49u), sharp(201u, 49u));
}

// ---- nothing else moves ----------------------------------------------------------

TEST(SnesPpuMosaic, ThePowerOnPhaseIsARowBegunOnLineOneAtSizeZero) {
  const PpuState ppu;
  EXPECT_EQ(ppu.mosaicBlockLine, 1u);
  EXPECT_EQ(ppu.mosaicBlockSize, 0u);
  PpuState moved = ppu;
  moved.mosaicBlockLine = 2u;
  EXPECT_FALSE(moved == ppu);
  moved = ppu;
  moved.mosaicBlockSize = 1u;
  EXPECT_FALSE(moved == ppu);
}

// ---- Mode 7 --------------------------------------------------------------------
//
// The field draws the same pattern: its pixel at field column X and row Y is
// patternIndex(X, Y).

PpuState fieldPicture(std::uint8_t mosaic) {
  PpuState ppu = placed(0x07u);
  ppu.tm = 0x01u;
  ppu.mosaic = mosaic;
  ppu.m7a = 0x0100u;
  ppu.m7b = 0x0000u;
  ppu.m7c = 0x0000u;
  ppu.m7d = 0x0100u;
  for (unsigned n = 0u; n < 8u; ++n) putColour(ppu, 1u + n, kBright[n]);
  for (unsigned p = 0u; p < 64u; ++p) {
    ppu.vram[(((1u << 6) | p) << 1) | 1u] = static_cast<std::uint8_t>(patternIndex(p, p >> 3));
  }
  for (unsigned word = 0u; word < 128u * 128u; ++word) ppu.vram[word << 1] = 1u;
  return ppu;
}

Rgba fieldAt(int fieldX, int fieldY) {
  return patternAt(static_cast<unsigned>(fieldX), static_cast<unsigned>(fieldY));
}

TEST(SnesPpuMosaic, ModeSevenBlocksAreAlignedToThePicture) {
  // A quarter turn — A 0, B 1.0, C -1.0, D 0 — with the scroll at -2 puts picture
  // (x, line) at field (line, 2 - x). Under $31 columns 0-3 of lines 1-4 all take
  // the corner at (0, 1): field (1, 2). Provisional until the mosaic cartridge is
  // read: anomie alone.
  PpuState ppu = fieldPicture(0x31u);
  ppu.m7a = 0x0000u;
  ppu.m7b = 0x0100u;
  ppu.m7c = 0xFF00u;
  ppu.m7d = 0x0000u;
  ppu.m7hofs = 0x1FFEu;
  const Picture picture = draw(ppu);
  EXPECT_EQ(picture.at(1u, 1u), fieldAt(1, 2));
  EXPECT_EQ(picture.at(3u, 3u), fieldAt(1, 2));
  EXPECT_EQ(picture.at(4u, 1u), fieldAt(1, -2));
  EXPECT_NE(fieldAt(1, 1), fieldAt(1, 2));
  EXPECT_NE(fieldAt(3, -1), fieldAt(1, 2));
}

TEST(SnesPpuMosaic, ModeSevensFlipIsAppliedToTheCorner) {
  // The vertical flip under $11: line 2 is in the row begun on line 1, which reads
  // field row 1 XOR 255 = 254.
  PpuState ppu = fieldPicture(0x11u);
  ppu.m7sel = 0x02u;
  EXPECT_EQ(draw(ppu).at(0u, 2u), fieldAt(0, 254));
  EXPECT_NE(fieldAt(0, 253), fieldAt(0, 254));
}

// EXTBG's layer alone, or BG1 alone, under a given $2106: column 3 of line 6.
Rgba extendedAt(std::uint8_t mosaic, std::uint8_t mainScreen) {
  PpuState ppu = fieldPicture(mosaic);
  ppu.setini = 0x40u;
  ppu.tm = mainScreen;
  return draw(ppu).at(3u, 6u);
}

// Provisional until the mosaic cartridge is read: anomie alone.
TEST(SnesPpuMosaic, ExtbgReadsBitZeroAsDownAndBitOneAsAcross) {
  // Rows of sixteen from line 1 and blocks of sixteen from column 0. $F1: EXTBG's
  // layer mosaiced down only, (3, 1); BG1 both ways, (0, 1).
  EXPECT_EQ(extendedAt(0xF1u, 0x02u), fieldAt(3, 1));
  EXPECT_EQ(extendedAt(0xF1u, 0x01u), fieldAt(0, 1));
  // $F2: across only, (0, 6); BG1 sharp.
  EXPECT_EQ(extendedAt(0xF2u, 0x02u), fieldAt(0, 6));
  EXPECT_EQ(extendedAt(0xF2u, 0x01u), fieldAt(3, 6));
  // $F3: both; $F0: neither.
  EXPECT_EQ(extendedAt(0xF3u, 0x02u), fieldAt(0, 1));
  EXPECT_EQ(extendedAt(0xF0u, 0x02u), fieldAt(3, 6));
  EXPECT_NE(fieldAt(3, 1), fieldAt(0, 1));
  EXPECT_NE(fieldAt(0, 6), fieldAt(3, 6));
  EXPECT_NE(fieldAt(3, 1), fieldAt(3, 6));
}

// One byte of the multiplier's ports read by a program whose LDA resolves thirty
// master cycles after the beam is placed.
std::uint8_t readPort(const PpuState& ppu, std::uint8_t low, std::uint16_t line,
                      std::uint16_t hpos) {
  const std::vector<std::uint8_t> rom =
      cartridge({0xADu, low, 0x21u, kStaAbs, 0x50u, 0x00u, kStp});
  Snes machine(SnesConfig{.rom = rom});
  SnesState state = machine.state();
  state.ppu = ppu;
  state.vpos = line;
  state.hpos = hpos;
  machine.restore(state);
  while (machine.state().cpu.run == CpuRunState::Running) machine.step();
  return machine.state().wram[0x50];
}

std::uint32_t readProduct(const PpuState& ppu, std::uint16_t line, std::uint16_t hpos) {
  return static_cast<std::uint32_t>(readPort(ppu, 0x34u, line, hpos)) |
         (static_cast<std::uint32_t>(readPort(ppu, 0x35u, line, hpos)) << 8) |
         (static_cast<std::uint32_t>(readPort(ppu, 0x36u, line, hpos)) << 16);
}

TEST(SnesPpuMosaic, TheMultipliersLineTermSubtractsTheRowIndex) {
  // Placed at the end of line 5, in a row of four begun there, so the read resolves
  // at master 10 of line 6 — dot 2's second half, D times the line term. Under $31
  // the term is 6 - 1 = 5: $0100 x 5 >> 3 = $0000A0. With BG1's bit clear it is 6:
  // $0000C0. Documented once, in fullsnes's schedule.
  PpuState ppu = fieldPicture(0x31u);
  ppu.mosaicBlockLine = 5u;
  ppu.mosaicBlockSize = 3u;
  const auto beam = static_cast<std::uint16_t>(kLineMaster + 10u - 30u);
  EXPECT_EQ(readProduct(ppu, 5u, beam), 0x0000A0u);
  ppu.mosaic = 0x30u;
  EXPECT_EQ(readProduct(ppu, 5u, beam), 0x0000C0u);
}

// ---- the staged cartridge ------------------------------------------------------
//
// The cartridge under SNAGGLETOOTH_PPU_ROMS is run and nothing else: no source of
// its is opened and nothing from it is written down here. Unset, the case
// registers and skips with a reason.

const char* romDirectory() { return SNAGGLETOOTH_PPU_ROMS; }

bool romsRequired() {
  const char* required = std::getenv("SNAGGLETOOTH_REQUIRE_PPU_ROMS");
  return required != nullptr && *required != '\0';
}

std::string findRom(const std::string& name) {
  const std::string root = romDirectory();
  if (root.empty()) return {};
  std::error_code failed;
  std::filesystem::recursive_directory_iterator walk(root, failed);
  if (failed) return {};
  for (const std::filesystem::directory_entry& found : walk) {
    if (found.is_regular_file(failed) && found.path().filename() == name) {
      return found.path().string();
    }
  }
  return {};
}

void loadRom(const std::string& name, std::vector<std::uint8_t>& rom, bool& ran) {
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
  rom.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
  if (rom.size() < 0x8000u) {
    ADD_FAILURE() << name << " is too small to be a cartridge image";
    return;
  }
  ran = true;
}

// The brightest frame a run delivered, and the mosaic phase the chip held as that
// frame ended.
struct Brightest final : FrameObserver {
  const Snes* machine = nullptr;
  std::vector<std::uint8_t> pixels;
  std::uint64_t light = 0;
  std::uint8_t mosaic = 0;
  std::uint8_t size = 0;
  std::uint8_t mode = 0;
  std::uint8_t mainScreen = 0;
  void frame(const VideoFrame& picture) override {
    std::uint64_t sum = 0;
    for (std::size_t at = 0u; at < picture.pixels.size(); at += 4u) {
      sum += picture.pixels[at] + picture.pixels[at + 1u] + picture.pixels[at + 2u];
    }
    if (sum <= light) return;
    light = sum;
    pixels.assign(picture.pixels.begin(), picture.pixels.end());
    const PpuState& ppu = machine->state().ppu;
    mosaic = ppu.mosaic;
    size = ppu.mosaicBlockSize;
    mode = ppu.bgmode & 0x07u;
    mainScreen = ppu.tm;
  }
};

TEST(PpuMosaicCartridges, AMosaicCartridgeDrawsBlocksAlignedToThePicture) {
  // What the cartridge does was learned by running it: it settles in mode 3 with
  // BG1 shown and mosaic enabled on it at size 0, and R steps the size up every
  // eight frames. A second of R leaves a size above 0, and in the brightest frame
  // of the second after it every whole block, counted from the picture's left edge
  // and first line at the size its rows were drawn with, is one colour.
  std::vector<std::uint8_t> rom;
  bool ran = false;
  loadRom("MosaicMode3.sfc", rom, ran);
  if (!ran) return;
  constexpr std::uint64_t kSecond = 60ull * 357364ull;
  Snes machine(SnesConfig{.rom = rom});
  machine.setJoypad(JoypadPort::One, Joypad{});
  machine.run(kSecond);
  Joypad right;
  right.hold(Button::R, true);
  machine.setJoypad(JoypadPort::One, right);
  machine.run(kSecond);
  machine.setJoypad(JoypadPort::One, Joypad{});
  machine.run(kSecond / 6u);
  Brightest watch;
  watch.machine = &machine;
  machine.setFrameObserver(&watch);
  machine.run(kSecond);
  ASSERT_FALSE(watch.pixels.empty());
  EXPECT_EQ(watch.mode, 3u);
  EXPECT_NE(watch.mainScreen & 0x01u, 0u);
  EXPECT_NE(watch.mosaic & 0x01u, 0u);
  ASSERT_GT(watch.size, 0u);

  const unsigned side = watch.size + 1u;
  const unsigned lines = static_cast<unsigned>(watch.pixels.size() / 4u / kPictureWidth);
  unsigned blocks = 0u;
  unsigned uneven = 0u;
  std::set<std::uint32_t> colours;
  for (unsigned top = 0u; top + side <= lines; top += side) {
    for (unsigned left = 0u; left + side <= kPictureWidth; left += side) {
      const std::size_t corner = (static_cast<std::size_t>(top) * kPictureWidth + left) * 4u;
      const std::uint32_t colour = watch.pixels[corner] | (watch.pixels[corner + 1u] << 8) |
                                   (watch.pixels[corner + 2u] << 16);
      colours.insert(colour);
      bool even = true;
      for (unsigned dy = 0u; dy < side; ++dy) {
        for (unsigned dx = 0u; dx < side; ++dx) {
          const std::size_t at =
              (static_cast<std::size_t>(top + dy) * kPictureWidth + left + dx) * 4u;
          even = even && watch.pixels[at] == watch.pixels[corner] &&
                 watch.pixels[at + 1u] == watch.pixels[corner + 1u] &&
                 watch.pixels[at + 2u] == watch.pixels[corner + 2u];
        }
      }
      ++blocks;
      if (!even) ++uneven;
    }
  }
  EXPECT_GT(blocks, 0u);
  EXPECT_EQ(uneven, 0u) << "of " << blocks << " blocks of " << side;
  EXPECT_GT(colours.size(), 1u) << "the frame is one flat colour";
}

}  // namespace
}  // namespace snaggletooth
