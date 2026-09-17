// Modes 5 and 6, whose tiles are two characters wide and whose lines are drawn in
// half-pixels; the half-pixel line any other mode draws under $2133 bit 3; and the
// interlaced picture.
//
// Every expectation is computed by hand from the register pages, fullsnes and
// anomie. A hires line is 512 half-pixels: half-pixel h belongs to full pixel h / 2,
// the sub screen's pixel is on the even half and the main screen's on the odd one.
// The tiles are drawn so that each of a tile's sixteen pixels is its own colour, so
// which screen a half-pixel shows, and which of the tile's pixels, reads as a colour.

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
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
constexpr std::uint16_t word(unsigned red, unsigned green, unsigned blue) {
  return static_cast<std::uint16_t>(red | (green << 5) | (blue << 10));
}

constexpr Rgba kBlack{0u, 0u, 0u, 255u};

// The palette words these cases use. Word 0 is the backdrop on both screens of a
// hires line.
constexpr std::uint16_t kBackdrop = 0x0C41u;  // red 1, green 2, blue 3

// BG1's sixteen-colour characters take palette 1, words 16-31, and word 16 + i is
// colour i: fifteen colours, every one different.
constexpr unsigned kBg1Palette = 1u;
constexpr unsigned kBg1Words = 16u;
std::uint16_t bg1Colour(unsigned index) {
  return word((index * 2u) & 0x1Fu, 31u - index * 2u, (index * 5u) & 0x1Fu);
}
// BG2's four-colour characters take palette 0: words 1-3.
constexpr std::array<std::uint16_t, 4> kBg2Colours{0u, 0x7C1Fu, 0x7FE0u, 0x01FFu};
// Sprite palette 0's colour 1.
constexpr unsigned kSpriteWord = 129u;
constexpr std::uint16_t kSpriteColour = 0x5294u;

// Where these pictures keep their pieces. A map base counts whole 32x32 screens of
// $400 words; a background's character base counts 8 KB blocks.
constexpr std::uint8_t kBg1MapBase = 0x04u;  // $2107: screen 1
constexpr std::uint8_t kBg2MapBase = 0x08u;  // $2108: screen 2
constexpr std::uint8_t kBg3MapBase = 0x0Cu;  // $2109: screen 3
constexpr std::uint8_t kCharBases12 = 0x54u; // $210B: BG1 at $8000, BG2 at $A000
constexpr std::uint8_t kSpriteBase = 0x03u;  // $2101: 8x8 and 16x16, the table at $C000

constexpr std::uint32_t kBg1Map = 0x0800u;
constexpr std::uint32_t kBg2Map = 0x1000u;
constexpr std::uint32_t kBg3Map = 0x1800u;
constexpr std::uint32_t kBg1Chars = 0x8000u;
constexpr std::uint32_t kBg2Chars = 0xA000u;
constexpr std::uint32_t kSpriteChars = 0xC000u;

constexpr std::uint16_t kToBg1 = 0x2000u;
constexpr std::uint16_t kToBg2 = 0x4000u;

constexpr std::uint8_t kStp = 0xDBu;
constexpr std::uint8_t kLdaImm = 0xA9u;
constexpr std::uint8_t kLdaAbs = 0xADu;
constexpr std::uint8_t kStaAbs = 0x8Du;
constexpr std::uint8_t kCmpImm = 0xC9u;
constexpr std::uint8_t kAndImm = 0x29u;
constexpr std::uint8_t kBne = 0xD0u;
constexpr std::uint8_t kBeq = 0xF0u;
constexpr std::uint8_t kBcc = 0x90u;

// More than an NTSC frame of master cycles, so a run from power-on reaches the line
// a finished frame is handed over on.
constexpr std::uint64_t kOneFrame = 357364u + 20000u;

// The first frame a run finished, kept whole.
struct Picture final : FrameObserver {
  unsigned frames = 0;
  unsigned width = 0;
  unsigned height = 0;
  unsigned field = 0;
  std::vector<std::uint8_t> pixels;

  void frame(const VideoFrame& picture) override {
    ++frames;
    if (frames != 1u) return;
    width = picture.width;
    height = picture.height;
    field = picture.field;
    pixels.assign(picture.pixels.begin(), picture.pixels.end());
  }

  // The four bytes at column `column` of line `line`, the picture's first line
  // being line 1: a half-pixel in a 512-wide frame, a pixel in a 256-wide one.
  [[nodiscard]] Rgba at(unsigned column, unsigned line) const {
    const std::size_t index = (static_cast<std::size_t>(line - 1u) * width + column) * 4u;
    return Rgba{pixels.at(index), pixels.at(index + 1u), pixels.at(index + 2u),
                pixels.at(index + 3u)};
  }
};

std::vector<std::uint8_t> cartridge(std::vector<std::uint8_t> program) {
  program.resize(0x8000u, 0x00u);
  program[0x7FFCu] = 0x00u;  // reset -> $8000
  program[0x7FFDu] = 0x80u;
  return program;
}

// A frame drawn from a placed PPU state by a machine that begins `program` with the
// beam at the first picture line of a frame of parity `field`.
Picture drawWith(const PpuState& ppu, std::vector<std::uint8_t> program, std::uint8_t field = 0u) {
  program.push_back(kStp);
  const std::vector<std::uint8_t> rom = cartridge(std::move(program));
  Snes machine(SnesConfig{.rom = rom});
  SnesState state = machine.state();
  state.ppu = ppu;
  state.vpos = 1u;
  state.hpos = 0u;
  state.field = field;
  machine.restore(state);
  Picture picture;
  machine.setFrameObserver(&picture);
  machine.run(kOneFrame);
  return picture;
}

// A frame drawn from a placed PPU state by a machine whose program halts at once.
Picture draw(const PpuState& ppu) { return drawWith(ppu, {}); }

// The same, for a frame of the parity named.
Picture drawField(const PpuState& ppu, std::uint8_t field) { return drawWith(ppu, {}, field); }

// LDA #v ; STA $21xx
std::vector<std::uint8_t> store(std::uint8_t low, std::uint8_t value) {
  return {kLdaImm, value, kStaAbs, low, 0x21u};
}

// Waits for the beam to reach line `line` — the counter latch read back, compared
// until it matches — then for the horizontal blank at that line's end, so what
// follows lands before the next line's first dot.
std::vector<std::uint8_t> afterLine(std::uint8_t line) {
  return {kLdaAbs, 0x3Fu, 0x21u,  // $213F: the counter flip-flops to their low halves
          kLdaAbs, 0x37u, 0x21u,  // $2137: latch the beam
          kLdaAbs, 0x3Du, 0x21u,  // $213D: the line's low byte
          kCmpImm, line,  kBne,  0xF3u,
          kLdaAbs, 0x12u, 0x42u,  // $4212 bit 6: horizontal blank
          kAndImm, 0x40u, kBeq,  0xF9u};
}

// Waits for the beam to reach line `line`, then for a dot at or past `dot`.
std::vector<std::uint8_t> atDot(std::uint8_t line, std::uint8_t dot) {
  return {kLdaAbs, 0x3Fu, 0x21u,
          kLdaAbs, 0x37u, 0x21u,
          kLdaAbs, 0x3Du, 0x21u,
          kCmpImm, line,  kBne,  0xF3u,
          kLdaAbs, 0x3Fu, 0x21u,
          kLdaAbs, 0x37u, 0x21u,
          kLdaAbs, 0x3Cu, 0x21u,  // $213C: the dot's low byte
          kCmpImm, dot,   kBcc,  0xF3u};
}

std::vector<std::uint8_t> joined(std::initializer_list<std::vector<std::uint8_t>> parts) {
  std::vector<std::uint8_t> program;
  for (const std::vector<std::uint8_t>& part : parts) {
    program.insert(program.end(), part.begin(), part.end());
  }
  return program;
}

void putColour(PpuState& ppu, unsigned at, std::uint16_t colour) {
  ppu.cgram[at * 2u] = static_cast<std::uint8_t>(colour & 0xFFu);
  ppu.cgram[at * 2u + 1u] = static_cast<std::uint8_t>(colour >> 8);
}

void putEntry(PpuState& ppu, std::uint32_t map, unsigned tileX, unsigned tileY,
              std::uint16_t value) {
  const std::uint32_t at = map + (tileY * 32u + tileX) * 2u;
  ppu.vram[at & 0xFFFFu] = static_cast<std::uint8_t>(value & 0xFFu);
  ppu.vram[(at + 1u) & 0xFFFFu] = static_cast<std::uint8_t>(value >> 8);
}

void fillMap(PpuState& ppu, std::uint32_t map, std::uint16_t value) {
  for (unsigned tileY = 0u; tileY < 32u; ++tileY) {
    for (unsigned tileX = 0u; tileX < 32u; ++tileX) putEntry(ppu, map, tileX, tileY, value);
  }
}

constexpr std::uint16_t entry(unsigned tile, unsigned palette, bool priority) {
  return static_cast<std::uint16_t>(tile | (palette << 10) | (priority ? 0x2000u : 0u));
}

// A character whose pixel at (column, row) is `index(column, row)`, at as many
// bitplanes as the caller names: planes 0 and 1 in the low and high bytes of eight
// words, every further pair sixteen bytes on.
using Pixels = std::function<unsigned(unsigned column, unsigned row)>;
void putCharacter(PpuState& ppu, std::uint32_t characters, unsigned planes, unsigned number,
                  const Pixels& index) {
  const std::uint32_t base = characters + number * 8u * planes;
  for (unsigned row = 0u; row < 8u; ++row) {
    for (unsigned plane = 0u; plane < planes; ++plane) {
      std::uint8_t byte = 0u;
      for (unsigned column = 0u; column < 8u; ++column) {
        if (((index(column, row) >> plane) & 1u) != 0u) byte |= static_cast<std::uint8_t>(0x80u >> column);
      }
      ppu.vram[(base + (plane / 2u) * 16u + (plane % 2u) + row * 2u) & 0xFFFFu] = byte;
    }
  }
}

void putSolid(PpuState& ppu, std::uint32_t characters, unsigned planes, unsigned number,
              unsigned index) {
  putCharacter(ppu, characters, planes, number, [index](unsigned, unsigned) { return index; });
}

// A mode 5 or 6 tile: characters `tile` and `tile + 1` side by side, pixel p of the
// sixteen being `index(p, row)`.
using TilePixels = std::function<unsigned(unsigned p, unsigned row)>;
void putWideTile(PpuState& ppu, std::uint32_t characters, unsigned planes, unsigned tile,
                 const TilePixels& index) {
  putCharacter(ppu, characters, planes, tile,
               [&index](unsigned column, unsigned row) { return index(column, row); });
  putCharacter(ppu, characters, planes, tile + 1u,
               [&index](unsigned column, unsigned row) { return index(column + 8u, row); });
}

// The colour index of pixel p of the ruler tile: p + 1 for the first fifteen, and
// the sixteenth shares colour 1 with the first, since a character has fifteen.
unsigned rulerIndex(unsigned p) { return p < 15u ? p + 1u : 1u; }

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

void parkSprites(PpuState& ppu) {
  for (unsigned index = 0u; index < kSprites; ++index) putSprite(ppu, index, -64, 0u, 0u, 0u);
}

// A solid 8x8 sprite in the colour at word 129 at x, standing on lines 41-48.
constexpr unsigned kSpriteLine = 41u;
void spriteAt(PpuState& ppu, int x, unsigned priority) {
  putSprite(ppu, 0u, x, 40u, 0u, static_cast<std::uint8_t>(priority << 4));
  putSolid(ppu, kSpriteChars, 4u, 0u, 1u);
}

// A PPU at full brightness in `mode` with the maps and characters placed, nothing on
// either screen, every vertical offset -1 so a map's first row is on the picture's
// first line, every sprite parked, and every colour these cases name in the palette.
PpuState placed(std::uint8_t mode) {
  PpuState ppu;
  ppu.inidisp = 0x0Fu;
  ppu.objsel = kSpriteBase;
  ppu.bgmode = mode;
  ppu.bg1sc = kBg1MapBase;
  ppu.bg2sc = kBg2MapBase;
  ppu.bg3sc = kBg3MapBase;
  ppu.bg12nba = kCharBases12;
  ppu.bg1vofs = 0x3FFu;
  ppu.bg2vofs = 0x3FFu;
  ppu.bg4vofs = 0x3FFu;
  putColour(ppu, 0u, kBackdrop);
  for (unsigned n = 1u; n < 16u; ++n) putColour(ppu, kBg1Words + n, bg1Colour(n));
  for (unsigned n = 1u; n < 4u; ++n) putColour(ppu, n, kBg2Colours[n]);
  putColour(ppu, kSpriteWord, kSpriteColour);
  parkSprites(ppu);
  return ppu;
}

// BG1 as the ruler tile everywhere, in palette 1.
PpuState rulerPicture(std::uint8_t mode, std::uint8_t mainScreen, std::uint8_t subScreen) {
  PpuState ppu = placed(mode);
  ppu.tm = mainScreen;
  ppu.ts = subScreen;
  putWideTile(ppu, kBg1Chars, 4u, 2u, [](unsigned p, unsigned) { return rulerIndex(p); });
  fillMap(ppu, kBg1Map, entry(2u, kBg1Palette, false));
  return ppu;
}

Rgba ruler(unsigned p) { return out(bg1Colour(rulerIndex(p % 16u))); }

// ---- the two modes -------------------------------------------------------------

TEST(SnesPpuHiresModes, ModeFiveDrawsBg1AtSixteenColoursAndBg2AtFour) {
  // BG1's colour 7 in palette 1 is word 1 x 16 + 7 = 23; BG2's colour 3 in palette 0
  // is word 3. BG1 on the main screen shows on the odd halves, BG2 on the sub
  // screen on the even ones.
  PpuState ppu = placed(0x05u);
  ppu.tm = 0x01u;
  ppu.ts = 0x02u;
  putWideTile(ppu, kBg1Chars, 4u, 2u, [](unsigned, unsigned) { return 7u; });
  putWideTile(ppu, kBg2Chars, 2u, 2u, [](unsigned, unsigned) { return 3u; });
  fillMap(ppu, kBg1Map, entry(2u, kBg1Palette, false));
  fillMap(ppu, kBg2Map, entry(2u, 0u, false));
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.frames, 1u);
  ASSERT_EQ(picture.width, 512u);
  EXPECT_EQ(picture.at(1u, 1u), out(bg1Colour(7u)));
  EXPECT_EQ(picture.at(0u, 1u), out(kBg2Colours[3]));
  EXPECT_EQ(picture.at(301u, 100u), out(bg1Colour(7u)));
  EXPECT_EQ(picture.at(300u, 100u), out(kBg2Colours[3]));
}

TEST(SnesPpuHiresModes, ModeFivesBg3BitNamesNoPlace) {
  // $2105 bit 3 lifts BG3's high tiles in mode 1 alone; mode 5 has no BG3, and the
  // bit changes nothing it draws.
  PpuState ppu = placed(0x05u);
  ppu.tm = 0x03u;
  ppu.ts = 0x03u;
  putWideTile(ppu, kBg1Chars, 4u, 2u, [](unsigned p, unsigned) { return rulerIndex(p); });
  putWideTile(ppu, kBg2Chars, 2u, 2u, [](unsigned p, unsigned) { return 1u + p % 3u; });
  for (unsigned tileX = 0u; tileX < 32u; ++tileX) {
    for (unsigned tileY = 0u; tileY < 32u; ++tileY) {
      putEntry(ppu, kBg1Map, tileX, tileY, entry(2u, kBg1Palette, (tileX & 1u) != 0u));
      putEntry(ppu, kBg2Map, tileX, tileY, entry(2u, 0u, (tileY & 1u) != 0u));
    }
  }
  // Map column 0 of BG1 is empty, and a sprite at priority 2 stands there over
  // BG2's high tiles: mode 5's chart puts the sprite in front, so the bit moving
  // BG2 would show.
  for (unsigned tileY = 0u; tileY < 32u; ++tileY) putEntry(ppu, kBg1Map, 0u, tileY, 0x0000u);
  spriteAt(ppu, 0, 2u);
  ppu.tm |= 0x10u;
  ppu.ts |= 0x10u;
  const Picture plain = draw(ppu);
  ppu.bgmode = 0x0Du;
  const Picture lifted = draw(ppu);
  ASSERT_EQ(plain.width, 512u);
  EXPECT_EQ(plain.at(0u, kSpriteLine), out(kSpriteColour));
  EXPECT_EQ(plain.pixels, lifted.pixels);
}

TEST(SnesPpuHiresModes, ModeSixDrawsBg1AloneAndNoBg2) {
  // BG1 at sixteen colours on the left eight full pixels and transparent after; BG2
  // on both screens with a solid tile shows nowhere, so the backdrop does.
  PpuState ppu = placed(0x06u);
  ppu.tm = 0x03u;
  ppu.ts = 0x03u;
  putWideTile(ppu, kBg1Chars, 4u, 2u, [](unsigned, unsigned) { return 5u; });
  putEntry(ppu, kBg1Map, 0u, 0u, entry(2u, kBg1Palette, false));
  putWideTile(ppu, kBg2Chars, 2u, 2u, [](unsigned, unsigned) { return 2u; });
  fillMap(ppu, kBg2Map, entry(2u, 0u, true));
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.width, 512u);
  EXPECT_EQ(picture.at(0u, 1u), out(bg1Colour(5u)));
  EXPECT_EQ(picture.at(15u, 1u), out(bg1Colour(5u)));
  EXPECT_EQ(picture.at(16u, 1u), out(kBackdrop));
  EXPECT_EQ(picture.at(17u, 1u), out(kBackdrop));
  EXPECT_EQ(picture.at(0u, 9u), out(kBackdrop));
}

// One place in a chart: a layer and its priority. 'O' a sprite, '1' BG1, '2' BG2.
struct Place {
  char layer;
  unsigned priority;
};

// A picture holding exactly the two places, on both screens, and the colour each
// one shows. Every tile solid, so each half of a full pixel shows the same place.
PpuState twoPlaces(std::uint8_t mode, Place front, Place back) {
  PpuState ppu = placed(mode);
  for (const Place& place : {front, back}) {
    if (place.layer == 'O') {
      spriteAt(ppu, 0, place.priority);
      ppu.tm |= 0x10u;
    } else if (place.layer == '1') {
      putWideTile(ppu, kBg1Chars, 4u, 2u, [](unsigned, unsigned) { return 9u; });
      fillMap(ppu, kBg1Map, entry(2u, kBg1Palette, place.priority != 0u));
      ppu.tm |= 0x01u;
    } else {
      putWideTile(ppu, kBg2Chars, 2u, 2u, [](unsigned, unsigned) { return 1u; });
      fillMap(ppu, kBg2Map, entry(2u, 0u, place.priority != 0u));
      ppu.tm |= 0x02u;
    }
  }
  ppu.ts = ppu.tm;
  return ppu;
}

Rgba placeColour(Place place) {
  if (place.layer == 'O') return out(kSpriteColour);
  if (place.layer == '1') return out(bg1Colour(9u));
  return out(kBg2Colours[1]);
}

void expectChart(std::uint8_t mode, std::initializer_list<Place> chart) {
  const std::vector<Place> places(chart);
  for (std::size_t n = 0u; n + 1u < places.size(); ++n) {
    const Picture picture = draw(twoPlaces(mode, places[n], places[n + 1u]));
    ASSERT_EQ(picture.width, 512u);
    for (unsigned h = 0u; h < 16u; ++h) {
      EXPECT_EQ(picture.at(h, kSpriteLine), placeColour(places[n]))
          << "mode " << unsigned{mode} << ", place " << n << " (" << places[n].layer
          << places[n].priority << ") in front of " << places[n + 1u].layer
          << places[n + 1u].priority << ", half-pixel " << h;
    }
  }
}

TEST(SnesPpuHiresModes, ModeFivesChartIsModeThrees) {
  // 3 A 2 B 1 a 0 b, front to back.
  expectChart(0x05u, {{'O', 3u}, {'1', 1u}, {'O', 2u}, {'2', 1u},
                      {'O', 1u}, {'1', 0u}, {'O', 0u}, {'2', 0u}});
}

TEST(SnesPpuHiresModes, ModeSixsChartHasBg1Alone) {
  // 3 A 2 1 a 0, front to back.
  expectChart(0x06u, {{'O', 3u}, {'1', 1u}, {'O', 2u}, {'O', 1u}, {'1', 0u}, {'O', 0u}});
}

TEST(SnesPpuHiresModes, DirectColourReachesNeitherMode) {
  // Neither mode has a 256-colour background, so $2130 bit 0 changes nothing.
  for (const std::uint8_t mode : {std::uint8_t{0x05u}, std::uint8_t{0x06u}}) {
    PpuState ppu = rulerPicture(mode, 0x01u, 0x01u);
    const Picture plain = draw(ppu);
    ppu.cgwsel = 0x01u;
    const Picture direct = draw(ppu);
    ASSERT_EQ(plain.width, 512u);
    EXPECT_EQ(plain.pixels, direct.pixels) << "mode " << unsigned{mode};
  }
}

// ---- the tile ------------------------------------------------------------------

TEST(SnesPpuHiresTile, BothScreensShowAllSixteenPixelsAcrossEightFullPixels) {
  // Half-pixel h of full pixel x shows tile pixel 2x on the sub screen and 2x + 1 on
  // the main: the even pixels to the sub screen, the odd to the main.
  const Picture picture = draw(rulerPicture(0x05u, 0x01u, 0x01u));
  ASSERT_EQ(picture.width, 512u);
  for (unsigned h = 0u; h < 32u; ++h) {
    EXPECT_EQ(picture.at(h, 1u), ruler(h)) << "half-pixel " << h;
  }
}

TEST(SnesPpuHiresTile, TheMainScreenAloneShowsTheOddPixelsOnTheOddHalves) {
  const Picture picture = draw(rulerPicture(0x05u, 0x01u, 0x00u));
  ASSERT_EQ(picture.width, 512u);
  for (unsigned x = 0u; x < 16u; ++x) {
    EXPECT_EQ(picture.at(2u * x + 1u, 1u), ruler(2u * x + 1u)) << "full pixel " << x;
    EXPECT_EQ(picture.at(2u * x, 1u), out(kBackdrop)) << "full pixel " << x;
  }
}

TEST(SnesPpuHiresTile, TheSubScreenAloneShowsTheEvenPixelsOnTheEvenHalves) {
  const Picture picture = draw(rulerPicture(0x05u, 0x00u, 0x01u));
  ASSERT_EQ(picture.width, 512u);
  for (unsigned x = 0u; x < 16u; ++x) {
    EXPECT_EQ(picture.at(2u * x, 1u), ruler(2u * x)) << "full pixel " << x;
    EXPECT_EQ(picture.at(2u * x + 1u, 1u), out(kBackdrop)) << "full pixel " << x;
  }
}

// A tile whose pixel p on row r is 1 + (p + 4r) mod 15: every row a different
// sequence, so a row read says which row it is.
unsigned rowedIndex(unsigned p, unsigned row) { return 1u + (p + 4u * row) % 15u; }

TEST(SnesPpuHiresTile, TheSizeBitClearIsSixteenByEight) {
  // Tile 2 is characters 2 and 3 on rows 0-7; line 9 is the map's next row, which
  // holds nothing.
  PpuState ppu = placed(0x05u);
  ppu.tm = 0x01u;
  ppu.ts = 0x01u;
  putWideTile(ppu, kBg1Chars, 4u, 2u, rowedIndex);
  putWideTile(ppu, kBg1Chars, 4u, 4u, [](unsigned, unsigned) { return 12u; });
  putEntry(ppu, kBg1Map, 0u, 0u, entry(2u, kBg1Palette, false));
  putEntry(ppu, kBg1Map, 0u, 1u, entry(4u, kBg1Palette, false));
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.width, 512u);
  for (unsigned line = 1u; line <= 8u; ++line) {
    for (unsigned h = 0u; h < 16u; ++h) {
      EXPECT_EQ(picture.at(h, line), out(bg1Colour(rowedIndex(h, line - 1u))))
          << "line " << line << ", half-pixel " << h;
    }
  }
  EXPECT_EQ(picture.at(0u, 9u), out(bg1Colour(12u)));
  EXPECT_EQ(picture.at(16u, 1u), out(kBackdrop));
}

TEST(SnesPpuHiresTile, TheSizeBitSetIsTheBlockOfFour) {
  // $2105 bit 4: tile 2 is characters 2, 3, 18 and 19, sixteen lines tall and still
  // eight full pixels wide.
  PpuState ppu = placed(0x15u);
  ppu.tm = 0x01u;
  ppu.ts = 0x01u;
  putWideTile(ppu, kBg1Chars, 4u, 2u, rowedIndex);
  putWideTile(ppu, kBg1Chars, 4u, 18u,
              [](unsigned p, unsigned row) { return rowedIndex(p, row + 8u); });
  putWideTile(ppu, kBg1Chars, 4u, 4u, [](unsigned, unsigned) { return 12u; });
  putWideTile(ppu, kBg1Chars, 4u, 20u, [](unsigned, unsigned) { return 12u; });
  putEntry(ppu, kBg1Map, 0u, 0u, entry(2u, kBg1Palette, false));
  putEntry(ppu, kBg1Map, 1u, 0u, entry(4u, kBg1Palette, false));
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.width, 512u);
  for (unsigned line = 1u; line <= 16u; ++line) {
    for (unsigned h = 0u; h < 16u; ++h) {
      EXPECT_EQ(picture.at(h, line), out(bg1Colour(rowedIndex(h, line - 1u))))
          << "line " << line << ", half-pixel " << h;
    }
  }
  for (unsigned line = 1u; line <= 16u; ++line) {
    EXPECT_EQ(picture.at(16u, line), out(bg1Colour(12u))) << "line " << line;
  }
  EXPECT_EQ(picture.at(32u, 1u), out(kBackdrop));
  EXPECT_EQ(picture.at(0u, 17u), out(kBackdrop));
}

TEST(SnesPpuHiresTile, AHorizontalFlipReversesAllSixteenPixels) {
  PpuState ppu = rulerPicture(0x05u, 0x01u, 0x01u);
  fillMap(ppu, kBg1Map, static_cast<std::uint16_t>(entry(2u, kBg1Palette, false) | 0x4000u));
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.width, 512u);
  for (unsigned h = 0u; h < 16u; ++h) {
    EXPECT_EQ(picture.at(h, 1u), ruler(15u - h)) << "half-pixel " << h;
  }
}

TEST(SnesPpuHiresTile, AVerticalFlipOfASixteenByEightTileShowsRowSevenFirst) {
  PpuState ppu = placed(0x05u);
  ppu.tm = 0x01u;
  ppu.ts = 0x01u;
  putWideTile(ppu, kBg1Chars, 4u, 2u, rowedIndex);
  putEntry(ppu, kBg1Map, 0u, 0u, static_cast<std::uint16_t>(entry(2u, kBg1Palette, false) | 0x8000u));
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.width, 512u);
  for (unsigned line = 1u; line <= 8u; ++line) {
    for (unsigned h = 0u; h < 16u; ++h) {
      EXPECT_EQ(picture.at(h, line), out(bg1Colour(rowedIndex(h, 8u - line))))
          << "line " << line << ", half-pixel " << h;
    }
  }
}

TEST(SnesPpuHiresTile, TheHorizontalOffsetCountsFullPixels) {
  // An offset of 1 moves the picture one full pixel, two half-pixels; an offset of 8
  // moves it a whole tile.
  PpuState ppu = rulerPicture(0x05u, 0x01u, 0x01u);
  ppu.bg1hofs = 1u;
  const Picture one = draw(ppu);
  ASSERT_EQ(one.width, 512u);
  for (unsigned h = 0u; h < 32u; ++h) EXPECT_EQ(one.at(h, 1u), ruler(h + 2u)) << "half-pixel " << h;

  // A second tile beside the ruler, at map column 1, in colour 12 throughout.
  ppu = rulerPicture(0x05u, 0x01u, 0x01u);
  putWideTile(ppu, kBg1Chars, 4u, 4u, [](unsigned, unsigned) { return 12u; });
  putEntry(ppu, kBg1Map, 1u, 0u, entry(4u, kBg1Palette, false));
  ppu.bg1hofs = 8u;
  const Picture eight = draw(ppu);
  for (unsigned h = 0u; h < 16u; ++h) {
    EXPECT_EQ(eight.at(h, 1u), out(bg1Colour(12u))) << "half-pixel " << h;
  }
}

TEST(SnesPpuHiresTile, TheVerticalOffsetCountsLines) {
  PpuState ppu = placed(0x05u);
  ppu.tm = 0x01u;
  ppu.ts = 0x01u;
  ppu.bg1vofs = 2u;  // -1 moved three lines down the map: line 1 reads row 3
  putWideTile(ppu, kBg1Chars, 4u, 2u, rowedIndex);
  fillMap(ppu, kBg1Map, entry(2u, kBg1Palette, false));
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.width, 512u);
  for (unsigned h = 0u; h < 16u; ++h) {
    EXPECT_EQ(picture.at(h, 1u), out(bg1Colour(rowedIndex(h, 3u)))) << "half-pixel " << h;
    EXPECT_EQ(picture.at(h, 5u), out(bg1Colour(rowedIndex(h, 7u)))) << "half-pixel " << h;
  }
}

// ---- the width -----------------------------------------------------------------

// Whether every full pixel of line `line` shows its colour on both halves, and what
// the line holds read that way.
bool doubled(const Picture& picture, unsigned line, unsigned from, unsigned to) {
  for (unsigned x = from; x < to; ++x) {
    if (picture.at(2u * x, line) != picture.at(2u * x + 1u, line)) return false;
  }
  return true;
}

TEST(SnesPpuHiresWidth, AModeFiveFrameIsFiveHundredAndTwelveWide) {
  const Picture picture = draw(rulerPicture(0x05u, 0x01u, 0x01u));
  ASSERT_EQ(picture.frames, 1u);
  EXPECT_EQ(picture.width, 512u);
  EXPECT_EQ(picture.height, 224u);
  EXPECT_EQ(picture.pixels.size(), 512u * 224u * 4u);
}

TEST(SnesPpuHiresWidth, AModeOneFrameIsTwoHundredAndFiftySixWide) {
  const Picture picture = draw(rulerPicture(0x01u, 0x01u, 0x01u));
  ASSERT_EQ(picture.frames, 1u);
  EXPECT_EQ(picture.width, 256u);
  EXPECT_EQ(picture.pixels.size(), 256u * 224u * 4u);
}

TEST(SnesPpuHiresWidth, AFrameThatTurnsHiresPartWayDownDoublesTheLinesAbove) {
  // Mode 1 until line 99's horizontal blank, then mode 5. The same state drawn in
  // mode 1 throughout is the reference for the lines above: each of its pixels
  // appears twice. Line 100 on is the ruler in half-pixels.
  const PpuState ppu = rulerPicture(0x01u, 0x01u, 0x01u);
  const Picture plain = draw(ppu);
  const Picture picture = drawWith(ppu, joined({afterLine(99u), store(0x05u, 0x05u)}));
  ASSERT_EQ(picture.frames, 1u);
  ASSERT_EQ(picture.width, 512u);
  for (unsigned line = 1u; line <= 99u; ++line) {
    for (unsigned x = 0u; x < 256u; ++x) {
      ASSERT_EQ(picture.at(2u * x, line), plain.at(x, line)) << "line " << line << ", x " << x;
      ASSERT_EQ(picture.at(2u * x + 1u, line), plain.at(x, line)) << "line " << line << ", x " << x;
    }
  }
  for (unsigned h = 0u; h < 32u; ++h) {
    EXPECT_EQ(picture.at(h, 100u), ruler(h)) << "half-pixel " << h;
    EXPECT_EQ(picture.at(h, 224u), ruler(h)) << "half-pixel " << h;
  }
}

TEST(SnesPpuHiresWidth, AWritePartWayAlongALineDoublesThatLinesPixelsBeforeIt) {
  // The mode changes at a dot at or past 150 of line 100, which is picture column
  // 128 or later. Before that column line 100 is mode 1's, doubled; after it, the
  // ruler in half-pixels; the lines above are doubled whole.
  const PpuState ppu = rulerPicture(0x01u, 0x01u, 0x01u);
  const Picture plain = draw(ppu);
  const Picture picture = drawWith(ppu, joined({atDot(100u, 150u), store(0x05u, 0x05u)}));
  ASSERT_EQ(picture.frames, 1u);
  ASSERT_EQ(picture.width, 512u);
  EXPECT_TRUE(doubled(picture, 99u, 0u, 256u));
  unsigned split = 256u;
  for (unsigned x = 0u; x < 256u; ++x) {
    if (picture.at(2u * x, 100u) != plain.at(x, 100u) ||
        picture.at(2u * x + 1u, 100u) != plain.at(x, 100u)) {
      split = x;
      break;
    }
  }
  EXPECT_GE(split, 128u);
  EXPECT_LT(split, 200u);
  for (unsigned h = 2u * split + 16u; h < 2u * split + 48u; ++h) {
    EXPECT_EQ(picture.at(h, 100u), ruler(h)) << "half-pixel " << h;
  }
  for (unsigned h = 0u; h < 32u; ++h) EXPECT_EQ(picture.at(h, 101u), ruler(h)) << "half-pixel " << h;
}

TEST(SnesPpuHiresWidth, TheTallerPictureInModeFiveIsFiveHundredAndTwelveByTwoHundredAndThirtyNine) {
  PpuState ppu = rulerPicture(0x05u, 0x01u, 0x01u);
  ppu.setini = 0x04u;
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.frames, 1u);
  EXPECT_EQ(picture.width, 512u);
  EXPECT_EQ(picture.height, 239u);
  EXPECT_EQ(picture.pixels.size(), 512u * 239u * 4u);
}

TEST(SnesPpuHiresWidth, AFrameAfterAWideOneIsNarrowAgain) {
  // Mode 5 until the picture's last line has ended, then mode 1: the first frame is
  // 512 wide and the one after it 256.
  const std::vector<std::uint8_t> rom =
      cartridge(joined({afterLine(224u), store(0x05u, 0x01u), {kStp}}));
  Snes machine(SnesConfig{.rom = rom});
  SnesState state = machine.state();
  state.ppu = rulerPicture(0x05u, 0x01u, 0x01u);
  state.vpos = 1u;
  machine.restore(state);
  struct Widths final : FrameObserver {
    std::vector<unsigned> seen;
    void frame(const VideoFrame& picture) override { seen.push_back(picture.width); }
  } widths;
  machine.setFrameObserver(&widths);
  machine.run(2u * kOneFrame);
  ASSERT_GE(widths.seen.size(), 2u);
  EXPECT_EQ(widths.seen[0], 512u);
  EXPECT_EQ(widths.seen[1], 256u);
}

TEST(SnesPpuHiresWidth, AFrameWhoseLastLineIsHiresIsWide) {
  const PpuState ppu = rulerPicture(0x01u, 0x01u, 0x01u);
  const Picture plain = draw(ppu);
  const Picture picture = drawWith(ppu, joined({afterLine(223u), store(0x05u, 0x05u)}));
  ASSERT_EQ(picture.width, 512u);
  EXPECT_TRUE(doubled(picture, 1u, 0u, 256u));
  EXPECT_TRUE(doubled(picture, 223u, 0u, 256u));
  EXPECT_EQ(picture.at(0u, 223u), plain.at(0u, 223u));
  for (unsigned h = 0u; h < 32u; ++h) EXPECT_EQ(picture.at(h, 224u), ruler(h)) << "half-pixel " << h;
}

// ---- the half-pixel line ---------------------------------------------------------

TEST(SnesPpuHiresLine, ASpriteCoversBothHalvesOfEveryFullPixelItClaims) {
  // A sprite at x = 10 claims full pixels 10-17: half-pixels 20-35 on a screen that
  // shows it, and not half-pixel 10 or 19.
  PpuState ppu = placed(0x05u);
  spriteAt(ppu, 10, 3u);
  ppu.tm = 0x10u;
  ppu.ts = 0x10u;
  Picture picture = draw(ppu);
  ASSERT_EQ(picture.width, 512u);
  for (unsigned h = 20u; h < 36u; ++h) {
    EXPECT_EQ(picture.at(h, kSpriteLine), out(kSpriteColour)) << "half-pixel " << h;
  }
  EXPECT_EQ(picture.at(19u, kSpriteLine), out(kBackdrop));
  EXPECT_EQ(picture.at(10u, kSpriteLine), out(kBackdrop));
  EXPECT_EQ(picture.at(36u, kSpriteLine), out(kBackdrop));

  // On the sub screen alone, the even halves.
  ppu.tm = 0x00u;
  picture = draw(ppu);
  EXPECT_EQ(picture.at(20u, kSpriteLine), out(kSpriteColour));
  EXPECT_EQ(picture.at(21u, kSpriteLine), out(kBackdrop));
}

TEST(SnesPpuHiresLine, AWindowMasksBothHalvesOfAFullPixel) {
  // Window 1 from full pixel 4 masks BG1 on both screens: half-pixels 8 and 9 show
  // the backdrop, 6 and 7 the ruler.
  PpuState ppu = rulerPicture(0x05u, 0x01u, 0x01u);
  ppu.w12sel = 0x02u;
  ppu.wh0 = 4u;
  ppu.wh1 = 255u;
  ppu.tmw = 0x01u;
  ppu.tsw = 0x01u;
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.width, 512u);
  EXPECT_EQ(picture.at(6u, 1u), ruler(6u));
  EXPECT_EQ(picture.at(7u, 1u), ruler(7u));
  EXPECT_EQ(picture.at(8u, 1u), out(kBackdrop));
  EXPECT_EQ(picture.at(9u, 1u), out(kBackdrop));
}

TEST(SnesPpuHiresLine, AnEmptySubScreenShowsColourZeroNotTheFixedColour) {
  // Provisional until the hires calibration cartridge's band c1 is read.
  PpuState ppu = rulerPicture(0x05u, 0x01u, 0x00u);
  ppu.fixedRed = 20u;
  ppu.fixedGreen = 4u;
  ppu.fixedBlue = 9u;
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.width, 512u);
  for (unsigned x = 0u; x < 8u; ++x) {
    EXPECT_EQ(picture.at(2u * x, 1u), out(kBackdrop)) << "full pixel " << x;
  }
}

TEST(SnesPpuHiresLine, TheMainHalfAddsTheFixedColourUnhalvedWhereTheSubScreenIsEmpty) {
  // Math from the sub screen, halved, on BG1, with nothing on the sub screen: the
  // addend is the fixed colour and no half is taken — the rule a line that is not
  // hires keeps. Ruler pixel 1 is colour 2, red 4 green 27 blue 10; the fixed colour
  // adds red 3, green 1 and blue 2.
  PpuState ppu = rulerPicture(0x05u, 0x01u, 0x00u);
  ppu.cgwsel = 0x02u;
  ppu.cgadsub = 0x41u;
  ppu.fixedRed = 3u;
  ppu.fixedGreen = 1u;
  ppu.fixedBlue = 2u;
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.width, 512u);
  ASSERT_EQ(bg1Colour(2u), word(4u, 27u, 10u));
  EXPECT_EQ(picture.at(1u, 1u), rgb(7u, 28u, 12u));
}

TEST(SnesPpuHiresLine, ForcedBlankAndBrightnessZeroAreBlackOnBothHalves) {
  PpuState ppu = rulerPicture(0x05u, 0x01u, 0x01u);
  ppu.inidisp = 0x8Fu;
  Picture picture = draw(ppu);
  ASSERT_EQ(picture.width, 512u);
  for (unsigned h = 0u; h < 16u; ++h) EXPECT_EQ(picture.at(h, 1u), kBlack) << "half-pixel " << h;
  ppu.inidisp = 0x00u;
  picture = draw(ppu);
  ASSERT_EQ(picture.width, 512u);
  for (unsigned h = 0u; h < 16u; ++h) EXPECT_EQ(picture.at(h, 1u), kBlack) << "half-pixel " << h;
}

// ---- mode 6's offset table -------------------------------------------------------

// Mode 6's BG1 on both screens: map column C holds a tile of one colour, colour
// 1 + C mod 15, and every row alike unless the case says otherwise.
PpuState columnsPicture(std::uint8_t mode) {
  PpuState ppu = placed(mode);
  ppu.tm = 0x01u;
  ppu.ts = 0x01u;
  for (unsigned column = 0u; column < 32u; ++column) {
    const unsigned index = 1u + column % 15u;
    putWideTile(ppu, kBg1Chars, 4u, 2u * column, [index](unsigned, unsigned) { return index; });
    for (unsigned row = 0u; row < 32u; ++row) {
      putEntry(ppu, kBg1Map, column, row, entry(2u * column, kBg1Palette, false));
    }
  }
  return ppu;
}

Rgba column(unsigned c) { return out(bg1Colour(1u + (c % 32u) % 15u)); }

void putTable(PpuState& ppu, unsigned tableColumn, unsigned row, std::uint16_t value) {
  putEntry(ppu, kBg3Map, tableColumn, row, value);
}

TEST(SnesPpuHiresOffsets, Bg1sTileOneReadsEntryZeroInEightPixelColumns) {
  // Entry 0 moves BG1 three tiles: full pixels 8-15 show map column 4. Entry 1 moves
  // it five: 16-23 show column 7.
  PpuState ppu = columnsPicture(0x06u);
  putTable(ppu, 0u, 0u, static_cast<std::uint16_t>(kToBg1 | 24u));
  putTable(ppu, 1u, 0u, static_cast<std::uint16_t>(kToBg1 | 40u));
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.width, 512u);
  for (unsigned h = 16u; h < 32u; ++h) EXPECT_EQ(picture.at(h, 1u), column(4u)) << "half-pixel " << h;
  for (unsigned h = 32u; h < 48u; ++h) EXPECT_EQ(picture.at(h, 1u), column(7u)) << "half-pixel " << h;
  for (unsigned h = 48u; h < 64u; ++h) EXPECT_EQ(picture.at(h, 1u), column(3u)) << "half-pixel " << h;
}

TEST(SnesPpuHiresOffsets, TheFirstColumnTakesTheRegisters) {
  PpuState ppu = columnsPicture(0x06u);
  for (unsigned c = 0u; c < 32u; ++c) putTable(ppu, c, 0u, static_cast<std::uint16_t>(kToBg1 | 24u));
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.width, 512u);
  for (unsigned h = 0u; h < 16u; ++h) EXPECT_EQ(picture.at(h, 1u), column(0u)) << "half-pixel " << h;
  for (unsigned x = 8u; x < 256u; x += 8u) {
    EXPECT_EQ(picture.at(2u * x, 1u), column(x / 8u + 3u)) << "full pixel " << x;
  }
}

TEST(SnesPpuHiresOffsets, OnlyBg1sBitApplies) {
  // An entry naming BG2 alone moves nothing in a mode that has no BG2.
  PpuState ppu = columnsPicture(0x06u);
  for (unsigned c = 0u; c < 32u; ++c) putTable(ppu, c, 0u, static_cast<std::uint16_t>(kToBg2 | 24u));
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.width, 512u);
  for (unsigned x = 0u; x < 256u; x += 8u) {
    EXPECT_EQ(picture.at(2u * x + 1u, 1u), column(x / 8u)) << "full pixel " << x;
  }
}

TEST(SnesPpuHiresOffsets, ASixteenBySixteenTableStillServesEightPixelColumns) {
  // Provisional until the hires calibration cartridge's band b is read. BG3's tile
  // is eight full pixels wide in this mode whatever its size bit says, so tile 2
  // reads entry 1, not entry 0 again.
  PpuState ppu = columnsPicture(0x46u);
  putTable(ppu, 0u, 0u, static_cast<std::uint16_t>(kToBg1 | 16u));
  putTable(ppu, 1u, 0u, static_cast<std::uint16_t>(kToBg1 | 40u));
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.width, 512u);
  EXPECT_EQ(picture.at(16u, 1u), column(1u + 2u));
  EXPECT_EQ(picture.at(32u, 1u), column(2u + 5u));
}

TEST(SnesPpuHiresOffsets, ASixteenBySixteenTableGivesBothAxesFromOneWord) {
  // Provisional until band b is read. With BG3's vertical offset 0 the vertical
  // entry's row, eight lines below, is inside the same sixteen-line tile, so entry 0
  // moves BG1 sixteen pixels across and puts its vertical offset at 16: line 1 reads
  // map row 2, column 3. Map row R column C is colour 1 + (C + 3R) mod 15.
  PpuState ppu = columnsPicture(0x46u);
  for (unsigned index = 1u; index < 16u; ++index) {
    putWideTile(ppu, kBg1Chars, 4u, 2u * index, [index](unsigned, unsigned) { return index; });
  }
  for (unsigned c = 0u; c < 32u; ++c) {
    for (unsigned r = 0u; r < 32u; ++r) {
      putEntry(ppu, kBg1Map, c, r, entry(2u * (1u + (c + 3u * r) % 15u), kBg1Palette, false));
    }
  }
  putTable(ppu, 0u, 0u, static_cast<std::uint16_t>(kToBg1 | 16u));
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.width, 512u);
  EXPECT_EQ(picture.at(16u, 1u), out(bg1Colour(1u + (3u + 3u * 2u) % 15u)));
  EXPECT_EQ(picture.at(32u, 1u), out(bg1Colour(1u + (2u + 3u * 0u) % 15u)));
}

// ---- pseudo-hires ---------------------------------------------------------------

// Mode 1's BG1 on the main screen, character 6 with pixel c in colour 1 + c of
// palette 1, and BG2 on the sub screen, a solid character in colour 2 of palette 0.
PpuState pseudoPicture(std::uint8_t mode, std::uint8_t setini) {
  PpuState ppu = placed(mode);
  ppu.setini = setini;
  ppu.tm = 0x01u;
  ppu.ts = 0x02u;
  putCharacter(ppu, kBg1Chars, 4u, 6u, [](unsigned column, unsigned) { return 1u + column; });
  putSolid(ppu, kBg2Chars, 4u, 6u, 2u);
  fillMap(ppu, kBg1Map, entry(6u, kBg1Palette, false));
  fillMap(ppu, kBg2Map, entry(6u, 0u, false));
  return ppu;
}

TEST(SnesPpuPseudoHires, Bit3SplitsAModeOneLineBetweenTheScreens) {
  const Picture picture = draw(pseudoPicture(0x01u, 0x08u));
  ASSERT_EQ(picture.width, 512u);
  for (unsigned x = 0u; x < 16u; ++x) {
    EXPECT_EQ(picture.at(2u * x, 1u), out(kBg2Colours[2])) << "full pixel " << x;
    EXPECT_EQ(picture.at(2u * x + 1u, 1u), out(bg1Colour(1u + x % 8u))) << "full pixel " << x;
  }
  // Without the bit the line is mode 1's, one pixel a position.
  EXPECT_EQ(draw(pseudoPicture(0x01u, 0x00u)).width, 256u);
}

TEST(SnesPpuPseudoHires, Bit3SplitsAModeZeroLine) {
  // Mode 0: BG1's colour 3 in palette 4 is word 4 x 4 + 3 = 19; BG2's colour 1 in
  // palette 0 is word 32 + 1 = 33.
  PpuState ppu = placed(0x00u);
  ppu.setini = 0x08u;
  ppu.tm = 0x01u;
  ppu.ts = 0x02u;
  putSolid(ppu, kBg1Chars, 2u, 5u, 3u);
  putSolid(ppu, kBg2Chars, 2u, 5u, 1u);
  fillMap(ppu, kBg1Map, entry(5u, 4u, false));
  fillMap(ppu, kBg2Map, entry(5u, 0u, false));
  putColour(ppu, 33u, kSpriteColour);
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.width, 512u);
  EXPECT_EQ(picture.at(40u, 1u), out(kSpriteColour));
  EXPECT_EQ(picture.at(41u, 1u), out(bg1Colour(3u)));
}

TEST(SnesPpuPseudoHires, Bit3SplitsAModeSevenLineWithItsSecondLayerOnTheSubScreen) {
  // The field is character 1 everywhere, every pixel $85: BG1's word $85 on the main
  // screen, the second layer's word $05 on the sub screen.
  PpuState ppu;
  ppu.inidisp = 0x0Fu;
  ppu.bgmode = 0x07u;
  ppu.setini = 0x48u;
  ppu.tm = 0x01u;
  ppu.ts = 0x02u;
  ppu.m7a = 0x0100u;
  ppu.m7d = 0x0100u;
  for (std::size_t w = 0u; w < 0x4000u; ++w) ppu.vram[w * 2u] = 0x01u;
  for (std::size_t w = 0x40u; w < 0x80u; ++w) ppu.vram[w * 2u + 1u] = 0x85u;
  putColour(ppu, 0x85u, kSpriteColour);
  putColour(ppu, 0x05u, bg1Colour(4u));
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.width, 512u);
  for (unsigned h = 0u; h < 16u; ++h) {
    EXPECT_EQ(picture.at(h, 50u), (h & 1u) != 0u ? out(kSpriteColour) : out(bg1Colour(4u)))
        << "half-pixel " << h;
  }
}

TEST(SnesPpuPseudoHires, Bit3ChangesNothingInModeFive) {
  PpuState ppu = rulerPicture(0x05u, 0x01u, 0x01u);
  const Picture plain = draw(ppu);
  ppu.setini = 0x08u;
  EXPECT_EQ(draw(ppu).pixels, plain.pixels);
}

TEST(SnesPpuPseudoHires, Bit3WrittenPartWayAlongALineSplitsTheRestOfIt) {
  // Written at a dot at or past 100 of line 50, which is picture column 78 or later.
  const PpuState ppu = pseudoPicture(0x01u, 0x00u);
  const Picture picture = drawWith(ppu, joined({atDot(50u, 100u), store(0x33u, 0x08u)}));
  ASSERT_EQ(picture.width, 512u);
  EXPECT_TRUE(doubled(picture, 49u, 0u, 256u));
  unsigned split = 256u;
  for (unsigned x = 0u; x < 256u; ++x) {
    if (picture.at(2u * x, 50u) == out(kBg2Colours[2])) {
      split = x;
      break;
    }
  }
  EXPECT_GE(split, 78u);
  EXPECT_LT(split, 150u);
  EXPECT_TRUE(doubled(picture, 50u, 0u, split));
  for (unsigned x = split; x < 256u; ++x) {
    EXPECT_EQ(picture.at(2u * x, 50u), out(kBg2Colours[2])) << "full pixel " << x;
    EXPECT_EQ(picture.at(2u * x + 1u, 50u), out(bg1Colour(1u + x % 8u))) << "full pixel " << x;
  }
}

TEST(SnesPpuPseudoHires, AnEmptySubScreenShowsColourZeroOnEveryLeftHalf) {
  PpuState ppu = pseudoPicture(0x01u, 0x08u);
  ppu.ts = 0x00u;
  ppu.fixedRed = 31u;
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.width, 512u);
  for (unsigned x = 0u; x < 256u; ++x) {
    EXPECT_EQ(picture.at(2u * x, 1u), out(kBackdrop)) << "full pixel " << x;
  }
}

// ---- colour math on a sub half-pixel -----------------------------------------------

// Mode 5 with BG1 on the main screen alone and BG2 on the sub screen alone. BG1's
// tile shows kEvenMain on full pixels whose index in the tile is even and kOddMain
// on odd ones; BG2 is kSub everywhere.
constexpr std::uint16_t kEvenMain = 0x0004u;  // red 4
constexpr std::uint16_t kOddMain = 0x0100u;   // green 8
constexpr std::uint16_t kSub = 0x1800u;       // blue 6

std::uint16_t mainAt(unsigned x) { return (x % 2u) == 0u ? kEvenMain : kOddMain; }

PpuState mathPicture(std::uint16_t even, std::uint16_t odd, std::uint16_t sub) {
  PpuState ppu = placed(0x05u);
  ppu.tm = 0x01u;
  ppu.ts = 0x02u;
  putColour(ppu, kBg1Words + 1u, even);
  putColour(ppu, kBg1Words + 2u, odd);
  putColour(ppu, 1u, sub);
  putWideTile(ppu, kBg1Chars, 4u, 2u, [](unsigned p, unsigned) { return 1u + (p / 2u) % 2u; });
  putWideTile(ppu, kBg2Chars, 2u, 2u, [](unsigned, unsigned) { return 1u; });
  fillMap(ppu, kBg1Map, entry(2u, kBg1Palette, false));
  fillMap(ppu, kBg2Map, entry(2u, 0u, false));
  return ppu;
}

// Five bits a channel, added or subtracted and held to the range, halved first
// where asked.
std::uint16_t combine(std::uint16_t a, std::uint16_t b, bool subtract, bool halve) {
  std::uint16_t result = 0u;
  for (unsigned shift = 0u; shift < 15u; shift += 5u) {
    const unsigned x = (a >> shift) & 0x1Fu;
    const unsigned y = (b >> shift) & 0x1Fu;
    unsigned value = subtract ? (x > y ? x - y : 0u) : x + y;
    if (halve) value >>= 1;
    result = static_cast<std::uint16_t>(result | ((value > 31u ? 31u : value) << shift));
  }
  return result;
}

constexpr std::uint16_t kFixed = 0x0822u;  // red 2, green 1, blue 2
void setFixed(PpuState& ppu) {
  ppu.fixedRed = 2u;
  ppu.fixedGreen = 1u;
  ppu.fixedBlue = 2u;
}

TEST(SnesPpuHiresMath, TheMainHalfIsMathedAgainstTheSubScreenAsOnAnyLine) {
  PpuState ppu = mathPicture(kEvenMain, kOddMain, kSub);
  ppu.cgwsel = 0x02u;
  ppu.cgadsub = 0x01u;
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.width, 512u);
  for (unsigned x = 0u; x < 16u; ++x) {
    EXPECT_EQ(picture.at(2u * x + 1u, 1u), out(combine(mainAt(x), kSub, false, false))) << "full pixel " << x;
  }
}

TEST(SnesPpuHiresMath, TheSubHalfAddsTheMainPixelToItsLeft) {
  // Provisional until the hires calibration cartridge's band d is read.
  PpuState ppu = mathPicture(kEvenMain, kOddMain, kSub);
  ppu.cgwsel = 0x02u;
  ppu.cgadsub = 0x01u;
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.width, 512u);
  for (unsigned x = 1u; x < 16u; ++x) {
    EXPECT_EQ(picture.at(2u * x, 1u), out(combine(kSub, mainAt(x - 1u), false, false))) << "full pixel " << x;
  }
}

TEST(SnesPpuHiresMath, TheFixedColourAddedToTheMainPixelIsAddedToTheSubHalfAfterIt) {
  PpuState ppu = mathPicture(kEvenMain, kOddMain, kSub);
  setFixed(ppu);
  ppu.cgadsub = 0x01u;
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.width, 512u);
  for (unsigned x = 1u; x < 16u; ++x) {
    EXPECT_EQ(picture.at(2u * x + 1u, 1u), out(combine(mainAt(x), kFixed, false, false))) << "full pixel " << x;
    EXPECT_EQ(picture.at(2u * x, 1u), out(combine(kSub, kFixed, false, false))) << "full pixel " << x;
  }
}

TEST(SnesPpuHiresMath, NoMathOnTheMainPixelMeansNoneOnTheSubHalfAfterIt) {
  // BG2's bit is set and BG1's is not: the main pixel, BG1's, takes none, and the
  // sub half after it takes none either, though it shows BG2.
  PpuState ppu = mathPicture(kEvenMain, kOddMain, kSub);
  setFixed(ppu);
  ppu.cgadsub = 0x02u;
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.width, 512u);
  for (unsigned x = 1u; x < 16u; ++x) {
    EXPECT_EQ(picture.at(2u * x, 1u), out(kSub)) << "full pixel " << x;
    EXPECT_EQ(picture.at(2u * x + 1u, 1u), out(mainAt(x))) << "full pixel " << x;
  }
}

TEST(SnesPpuHiresMath, TheSubHalfIsHalvedWhereTheMainPixelToItsLeftWas) {
  // Provisional until band d is read. Where the sub screen is empty the main pixel
  // takes the fixed colour and no half, so the sub half after it — colour 0 there —
  // takes the fixed colour and no half too.
  PpuState ppu = mathPicture(kEvenMain, kOddMain, kSub);
  setFixed(ppu);
  ppu.cgwsel = 0x02u;
  ppu.cgadsub = 0x41u;
  putEntry(ppu, kBg2Map, 2u, 0u, entry(4u, 0u, false));  // tile 4 is empty: full pixels 16-23
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.width, 512u);
  for (unsigned x = 1u; x < 16u; ++x) {
    EXPECT_EQ(picture.at(2u * x, 1u), out(combine(kSub, mainAt(x - 1u), false, true))) << "full pixel " << x;
  }
  EXPECT_EQ(picture.at(2u * 17u, 1u), out(combine(kBackdrop, kFixed, false, false)));
  EXPECT_EQ(picture.at(2u * 17u + 1u, 1u), out(combine(mainAt(17u), kFixed, false, false)));
  // Full pixel 24 has the sub screen back, but the main pixel to its left took no
  // half.
  EXPECT_EQ(picture.at(2u * 24u, 1u), out(combine(kSub, kFixed, false, false)));
  EXPECT_EQ(picture.at(2u * 25u, 1u), out(combine(kSub, mainAt(24u), false, true)));
}

TEST(SnesPpuHiresMath, TheSubHalfTakesTheOperationOfTheMainPixelToItsLeft) {
  // Adding the fixed colour until $2131 bit 7 is written part-way along line 50,
  // subtracting it after. The first main pixel that subtracts has an adding one to
  // its left, so the sub half beside it still adds; the sub half after it subtracts.
  PpuState ppu = mathPicture(kEvenMain, kOddMain, kSub);
  setFixed(ppu);
  ppu.cgadsub = 0x01u;
  const Picture picture = drawWith(ppu, joined({atDot(50u, 100u), store(0x31u, 0x81u)}));
  ASSERT_EQ(picture.width, 512u);
  unsigned split = 256u;
  for (unsigned x = 1u; x < 256u; ++x) {
    if (picture.at(2u * x + 1u, 50u) == out(combine(mainAt(x), kFixed, true, false))) {
      split = x;
      break;
    }
  }
  ASSERT_GE(split, 78u);
  ASSERT_LT(split, 250u);
  EXPECT_EQ(picture.at(2u * split - 1u, 50u), out(combine(mainAt(split - 1u), kFixed, false, false)));
  EXPECT_EQ(picture.at(2u * split, 50u), out(combine(kSub, kFixed, false, false)));
  EXPECT_EQ(picture.at(2u * split + 2u, 50u), out(combine(kSub, kFixed, true, false)));
}

TEST(SnesPpuHiresMath, SubtractingMagentaFromCyanIsGreenOnTheMainHalvesAndRedOnTheSub) {
  // anomie's example.
  const std::uint16_t cyan = word(0u, 31u, 31u);
  const std::uint16_t magenta = word(31u, 0u, 31u);
  PpuState ppu = mathPicture(cyan, cyan, magenta);
  ppu.cgwsel = 0x02u;
  ppu.cgadsub = 0x81u;
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.width, 512u);
  for (unsigned x = 1u; x < 16u; ++x) {
    EXPECT_EQ(picture.at(2u * x + 1u, 1u), out(word(0u, 31u, 0u))) << "full pixel " << x;
    EXPECT_EQ(picture.at(2u * x, 1u), out(word(31u, 0u, 0u))) << "full pixel " << x;
  }
}

TEST(SnesPpuHiresMath, ARestoredSnapshotDrawsTheNextSubHalfAsTheUnbrokenLine) {
  // One machine draws a frame straight through; another is restored from its state
  // part-way along line 50 and draws the rest. From the first position the second
  // drew, line 50 is the same in both, and its first sub half needs the main pixel
  // the first machine drew before the snapshot.
  PpuState ppu = mathPicture(kEvenMain, kOddMain, kSub);
  ppu.cgwsel = 0x02u;
  ppu.cgadsub = 0x01u;
  const std::vector<std::uint8_t> rom = cartridge({kStp});

  Snes whole(SnesConfig{.rom = rom});
  SnesState state = whole.state();
  state.ppu = ppu;
  state.vpos = 1u;
  state.hpos = 0u;
  whole.restore(state);
  Picture unbroken;
  whole.setFrameObserver(&unbroken);
  // A line is 1364 master cycles: this stops on line 50, part-way along it.
  whole.run(49u * 1364u + 400u);
  const SnesState snapshot = whole.state();
  whole.run(kOneFrame);
  ASSERT_EQ(unbroken.width, 512u);
  ASSERT_EQ(snapshot.vpos, 50u);

  Snes resumed(SnesConfig{.rom = rom});
  resumed.restore(snapshot);
  Picture after;
  resumed.setFrameObserver(&after);
  resumed.run(kOneFrame);
  ASSERT_EQ(after.width, 512u);
  unsigned first = 512u;
  for (unsigned h = 0u; h < 512u; ++h) {
    if (after.at(h, 50u) != kBlack) {
      first = h;
      break;
    }
  }
  ASSERT_LT(first, 510u);
  ASSERT_EQ(first % 2u, 0u);
  for (unsigned h = first; h < first + 8u; ++h) {
    EXPECT_EQ(after.at(h, 50u), unbroken.at(h, 50u)) << "half-pixel " << h;
  }
}

// ---- the colour window on a sub half-pixel ---------------------------------------

// Window 1 is full pixel 8 alone, and it is the colour window.
void colourWindowAtEight(PpuState& ppu) {
  ppu.wh0 = 8u;
  ppu.wh1 = 8u;
  ppu.wobjsel = 0x20u;
}

TEST(SnesPpuHiresWindow, BlackingTheMainPixelBlacksTheSubHalfToItsRight) {
  // Provisional until band e is read.
  PpuState ppu = mathPicture(kEvenMain, kOddMain, kSub);
  colourWindowAtEight(ppu);
  ppu.cgwsel = 0x80u;
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.width, 512u);
  EXPECT_EQ(picture.at(17u, 1u), kBlack);
  EXPECT_EQ(picture.at(18u, 1u), kBlack);
  EXPECT_EQ(picture.at(16u, 1u), out(kSub));
  EXPECT_EQ(picture.at(15u, 1u), out(mainAt(7u)));
  EXPECT_EQ(picture.at(19u, 1u), out(mainAt(9u)));
  EXPECT_EQ(picture.at(20u, 1u), out(kSub));
}

TEST(SnesPpuHiresWindow, PreventingMathAtTheMainPixelPreventsItAtTheSubHalfToItsRight) {
  PpuState ppu = mathPicture(kEvenMain, kOddMain, kSub);
  colourWindowAtEight(ppu);
  setFixed(ppu);
  ppu.cgwsel = 0x20u;
  ppu.cgadsub = 0x03u;
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.width, 512u);
  EXPECT_EQ(picture.at(17u, 1u), out(mainAt(8u)));
  EXPECT_EQ(picture.at(18u, 1u), out(kSub));
  EXPECT_EQ(picture.at(16u, 1u), out(combine(kSub, kFixed, false, false)));
  EXPECT_EQ(picture.at(20u, 1u), out(combine(kSub, kFixed, false, false)));
}

TEST(SnesPpuHiresWindow, ColumnZerosSubHalfTakesNoMathAndNoBlack) {
  // Provisional: no source says what column 0's sub half takes, and nothing stands
  // to its left.
  PpuState ppu = mathPicture(kEvenMain, kOddMain, kSub);
  setFixed(ppu);
  ppu.cgadsub = 0x01u;
  // Line 2's position 0 follows line 1's last pixel along the beam, and takes
  // nothing from it either.
  Picture picture = draw(ppu);
  ASSERT_EQ(picture.width, 512u);
  for (unsigned line = 1u; line <= 3u; ++line) {
    EXPECT_EQ(picture.at(0u, line), out(kSub)) << "line " << line;
    EXPECT_EQ(picture.at(2u, line), out(combine(kSub, kFixed, false, false))) << "line " << line;
  }

  ppu.cgadsub = 0x00u;
  ppu.cgwsel = 0xC0u;
  picture = draw(ppu);
  for (unsigned line = 1u; line <= 3u; ++line) {
    EXPECT_EQ(picture.at(0u, line), out(kSub)) << "line " << line;
    EXPECT_EQ(picture.at(1u, line), kBlack) << "line " << line;
    EXPECT_EQ(picture.at(2u, line), kBlack) << "line " << line;
  }
}

TEST(SnesPpuHiresWindow, ALayerMaskActsAtTheSubHalfsOwnFullPixel) {
  // BG2 masked on the sub screen by window 1 at full pixel 8: its sub half shows
  // colour 0, the ones either side BG2.
  PpuState ppu = mathPicture(kEvenMain, kOddMain, kSub);
  ppu.wh0 = 8u;
  ppu.wh1 = 8u;
  ppu.w12sel = 0x20u;
  ppu.tsw = 0x02u;
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.width, 512u);
  EXPECT_EQ(picture.at(16u, 1u), out(kBackdrop));
  EXPECT_EQ(picture.at(14u, 1u), out(kSub));
  EXPECT_EQ(picture.at(18u, 1u), out(kSub));
}

// ---- mosaic on a line drawn in half-pixels ---------------------------------------

// anomie's example: a red pixel at tile pixel 0 of BG1's first row on the main
// screen, a blue one in the same place on BG2 on the sub screen, and nothing else.
PpuState redAndBlue(std::uint8_t mosaic) {
  PpuState ppu = placed(0x05u);
  ppu.tm = 0x01u;
  ppu.ts = 0x02u;
  ppu.mosaic = mosaic;
  ppu.mosaicBlockSize = static_cast<std::uint8_t>(mosaic >> 4);
  putColour(ppu, kBg1Words + 1u, 0x001Fu);
  putColour(ppu, 1u, 0x7C00u);
  putWideTile(ppu, kBg1Chars, 4u, 2u, [](unsigned p, unsigned row) { return p == 0u && row == 0u ? 1u : 0u; });
  putWideTile(ppu, kBg2Chars, 2u, 2u, [](unsigned p, unsigned row) { return p == 0u && row == 0u ? 1u : 0u; });
  putEntry(ppu, kBg1Map, 0u, 0u, entry(2u, kBg1Palette, false));
  putEntry(ppu, kBg2Map, 0u, 0u, entry(2u, 0u, false));
  return ppu;
}

TEST(SnesPpuHiresMosaic, SizeZeroAlreadyShowsTheEvenPixelOnTheMainHalf) {
  // Provisional until band g is read. $00: the blue pixel alone. $03: blocks of two
  // half-pixels whose corner is the even one, so the main half reads BG1's pixel 0.
  Picture picture = draw(redAndBlue(0x00u));
  ASSERT_EQ(picture.width, 512u);
  EXPECT_EQ(picture.at(0u, 1u), out(0x7C00u));
  EXPECT_EQ(picture.at(1u, 1u), out(kBackdrop));
  picture = draw(redAndBlue(0x03u));
  EXPECT_EQ(picture.at(0u, 1u), out(0x7C00u));
  EXPECT_EQ(picture.at(1u, 1u), out(0x001Fu));
  EXPECT_EQ(picture.at(2u, 1u), out(kBackdrop));
}

TEST(SnesPpuHiresMosaic, SizeOneShowsBlueRedBlueRedOnTwoLines) {
  const Picture picture = draw(redAndBlue(0x13u));
  ASSERT_EQ(picture.width, 512u);
  for (unsigned line = 1u; line <= 2u; ++line) {
    EXPECT_EQ(picture.at(0u, line), out(0x7C00u)) << "line " << line;
    EXPECT_EQ(picture.at(1u, line), out(0x001Fu)) << "line " << line;
    EXPECT_EQ(picture.at(2u, line), out(0x7C00u)) << "line " << line;
    EXPECT_EQ(picture.at(3u, line), out(0x001Fu)) << "line " << line;
    EXPECT_EQ(picture.at(4u, line), out(kBackdrop)) << "line " << line;
  }
  EXPECT_EQ(picture.at(0u, 3u), out(kBackdrop));
}

TEST(SnesPpuHiresMosaic, ABlockIsTwiceItsSizeInHalfPixelsWithTheEvenOneAtItsCorner) {
  PpuState ppu = rulerPicture(0x05u, 0x01u, 0x01u);
  ppu.mosaic = 0x11u;
  ppu.mosaicBlockSize = 1u;
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.width, 512u);
  for (unsigned h = 0u; h < 32u; ++h) {
    EXPECT_EQ(picture.at(h, 1u), ruler(h - h % 4u)) << "half-pixel " << h;
  }
}

TEST(SnesPpuHiresMosaic, PseudoHiresMosaicsEachScreenInFullPixelsBeforeTheSplit) {
  // Provisional until band h is read. BG1 on the main screen in two-pixel blocks;
  // BG2 on the sub screen, not mosaiced, sharp.
  PpuState ppu = pseudoPicture(0x01u, 0x08u);
  ppu.mosaic = 0x11u;
  ppu.mosaicBlockSize = 1u;
  putCharacter(ppu, kBg2Chars, 4u, 6u, [](unsigned column, unsigned) { return 1u + column % 3u; });
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.width, 512u);
  for (unsigned x = 0u; x < 16u; ++x) {
    EXPECT_EQ(picture.at(2u * x + 1u, 1u), out(bg1Colour(1u + (x - x % 2u) % 8u))) << "full pixel " << x;
    EXPECT_EQ(picture.at(2u * x, 1u), out(kBg2Colours[1u + (x % 8u) % 3u])) << "full pixel " << x;
  }
}

TEST(SnesPpuHiresMosaic, AWindowStillCutsABlock) {
  // Window 1 masks BG1 at full pixel 1 on both screens: half-pixels 2 and 3 show the
  // backdrop inside a block whose other half-pixels show its corner.
  PpuState ppu = rulerPicture(0x05u, 0x01u, 0x01u);
  ppu.mosaic = 0x11u;
  ppu.mosaicBlockSize = 1u;
  ppu.w12sel = 0x02u;
  ppu.wh0 = 1u;
  ppu.wh1 = 1u;
  ppu.tmw = 0x01u;
  ppu.tsw = 0x01u;
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.width, 512u);
  EXPECT_EQ(picture.at(0u, 1u), ruler(0u));
  EXPECT_EQ(picture.at(1u, 1u), ruler(0u));
  EXPECT_EQ(picture.at(2u, 1u), out(kBackdrop));
  EXPECT_EQ(picture.at(3u, 1u), out(kBackdrop));
}

// ---- the interlaced picture ------------------------------------------------------

// Mode 5 with BG1 on both screens as the rowed tile everywhere, $2133 as given, and
// BG1's vertical offset as given.
PpuState rowedPicture(std::uint8_t mode, std::uint8_t setini, std::uint16_t vertical) {
  PpuState ppu = placed(mode);
  ppu.tm = 0x01u;
  ppu.ts = 0x01u;
  ppu.setini = setini;
  ppu.bg1vofs = vertical;
  putWideTile(ppu, kBg1Chars, 4u, 2u, rowedIndex);
  fillMap(ppu, kBg1Map, entry(2u, kBg1Palette, false));
  return ppu;
}

Rgba rowed(unsigned p, unsigned row) { return out(bg1Colour(rowedIndex(p, row % 8u))); }

TEST(SnesPpuInterlace, EachFieldOfAnInterlacedModeFivePictureShowsEveryOtherHalfLine) {
  // Provisional until the interlace cartridge's band i is read. With an offset of
  // -2, line L of field F reads half-line 2L - 2 + F: field 0 shows rows 0, 2, 4, 6
  // on lines 1-4, field 1 rows 1, 3, 5, 7.
  const PpuState ppu = rowedPicture(0x05u, 0x01u, 0x3FEu);
  const Picture even = drawField(ppu, 0u);
  const Picture odd = drawField(ppu, 1u);
  ASSERT_EQ(even.width, 512u);
  ASSERT_EQ(odd.width, 512u);
  EXPECT_EQ(even.field, 0u);
  EXPECT_EQ(odd.field, 1u);
  for (unsigned line = 1u; line <= 4u; ++line) {
    for (unsigned h = 0u; h < 16u; ++h) {
      EXPECT_EQ(even.at(h, line), rowed(h, 2u * line - 2u)) << "line " << line << ", half-pixel " << h;
      EXPECT_EQ(odd.at(h, line), rowed(h, 2u * line - 1u)) << "line " << line << ", half-pixel " << h;
    }
  }
}

TEST(SnesPpuInterlace, TheVerticalOffsetCountsHalfLines) {
  // An offset of -1 is one half-line on from -2: field 0 shows the odd rows.
  const Picture picture = drawField(rowedPicture(0x05u, 0x01u, 0x3FFu), 0u);
  ASSERT_EQ(picture.width, 512u);
  for (unsigned line = 1u; line <= 4u; ++line) {
    EXPECT_EQ(picture.at(0u, line), rowed(0u, 2u * line - 1u)) << "line " << line;
  }
}

TEST(SnesPpuInterlace, ATileCoversFourLinesOfAFieldAndAScreenOfMapOneHundredAndTwentyEight) {
  // Map row R holds the ruler where R is even and a tile of colour 12 where it is
  // odd. Eight half-lines a row are four lines of a field, and thirty-two rows are
  // 256 half-lines, so line 129 is row 0 again.
  PpuState ppu = rowedPicture(0x05u, 0x01u, 0x3FEu);
  putWideTile(ppu, kBg1Chars, 4u, 4u, [](unsigned, unsigned) { return 12u; });
  for (unsigned row = 1u; row < 32u; row += 2u) {
    for (unsigned column = 0u; column < 32u; ++column) {
      putEntry(ppu, kBg1Map, column, row, entry(4u, kBg1Palette, false));
    }
  }
  const Picture picture = drawField(ppu, 0u);
  ASSERT_EQ(picture.width, 512u);
  EXPECT_EQ(picture.at(0u, 4u), rowed(0u, 6u));
  EXPECT_EQ(picture.at(0u, 5u), out(bg1Colour(12u)));
  EXPECT_EQ(picture.at(0u, 8u), out(bg1Colour(12u)));
  EXPECT_EQ(picture.at(0u, 9u), rowed(0u, 0u));
  EXPECT_EQ(picture.at(0u, 128u), out(bg1Colour(12u)));
  EXPECT_EQ(picture.at(0u, 129u), rowed(0u, 0u));
}

TEST(SnesPpuInterlace, ModeOneShowsTheSameLinesInBothFields) {
  // In any mode but 5 and 6 the bit changes no background: both fields read the
  // same lines, and they are the lines a picture that is not interlaced reads.
  PpuState ppu = rulerPicture(0x01u, 0x01u, 0x00u);
  putCharacter(ppu, kBg1Chars, 4u, 8u, [](unsigned column, unsigned row) { return 1u + (column + 3u * row) % 15u; });
  fillMap(ppu, kBg1Map, entry(8u, kBg1Palette, false));
  const Picture progressive = drawField(ppu, 0u);
  ppu.setini = 0x01u;
  const Picture even = drawField(ppu, 0u);
  const Picture odd = drawField(ppu, 1u);
  ASSERT_EQ(even.width, 256u);
  EXPECT_EQ(even.pixels, progressive.pixels);
  EXPECT_EQ(odd.pixels, progressive.pixels);
}

TEST(SnesPpuInterlace, PseudoHiresUnderInterlaceShowsTheSameLinesInBothFields) {
  PpuState ppu = pseudoPicture(0x01u, 0x09u);
  putCharacter(ppu, kBg1Chars, 4u, 6u, [](unsigned column, unsigned row) { return 1u + (column + 3u * row) % 15u; });
  const Picture even = drawField(ppu, 0u);
  const Picture odd = drawField(ppu, 1u);
  ASSERT_EQ(even.width, 512u);
  EXPECT_EQ(even.pixels, odd.pixels);
  ppu.setini = 0x08u;
  EXPECT_EQ(drawField(ppu, 1u).pixels, even.pixels);
}

// Every frame a run finished: its parity and its size.
struct Fields final : FrameObserver {
  struct Seen {
    unsigned field;
    unsigned width;
    unsigned height;
  };
  std::vector<Seen> seen;
  void frame(const VideoFrame& picture) override {
    seen.push_back(Seen{.field = picture.field, .width = picture.width, .height = picture.height});
  }
};

TEST(SnesPpuInterlace, AnInterlacedRunHandsOverFramesOfAlternatingParity) {
  const std::vector<std::uint8_t> rom = cartridge({kStp});
  Snes machine(SnesConfig{.rom = rom});
  SnesState state = machine.state();
  state.ppu = rowedPicture(0x05u, 0x01u, 0x3FEu);
  state.vpos = 1u;
  machine.restore(state);
  Fields fields;
  machine.setFrameObserver(&fields);
  machine.run(4u * kOneFrame);
  ASSERT_GE(fields.seen.size(), 4u);
  for (std::size_t n = 0u; n < 4u; ++n) {
    EXPECT_EQ(fields.seen[n].field, n % 2u) << "frame " << n;
    EXPECT_EQ(fields.seen[n].width, 512u) << "frame " << n;
    EXPECT_EQ(fields.seen[n].height, 224u) << "frame " << n;
  }
}

TEST(SnesPpuInterlace, TheTallerInterlacedPictureIsTwoHundredAndThirtyNineLinesAField) {
  const Picture picture = drawField(rowedPicture(0x05u, 0x05u, 0x3FEu), 1u);
  EXPECT_EQ(picture.width, 512u);
  EXPECT_EQ(picture.height, 239u);
  EXPECT_EQ(picture.at(0u, 238u), rowed(0u, 2u * 238u - 1u));
}

// ---- sprites at half height -----------------------------------------------------

// A 16x16 sprite at (40, y) whose row r is colour 1 + r mod 15 of sprite palette 0,
// on the main screen alone, in mode 1.
PpuState rowedSprite(std::uint8_t setini, std::uint8_t y, bool flip) {
  PpuState ppu = placed(0x01u);
  ppu.tm = 0x10u;
  ppu.setini = setini;
  for (unsigned n = 1u; n < 16u; ++n) putColour(ppu, 128u + n, bg1Colour(n));
  const auto rows = [](unsigned top) {
    return [top](unsigned, unsigned row) { return 1u + (top + row) % 15u; };
  };
  putCharacter(ppu, kSpriteChars, 4u, 0u, rows(0u));
  putCharacter(ppu, kSpriteChars, 4u, 1u, rows(0u));
  putCharacter(ppu, kSpriteChars, 4u, 16u, rows(8u));
  putCharacter(ppu, kSpriteChars, 4u, 17u, rows(8u));
  putSprite(ppu, 0u, 40, y, 0u, static_cast<std::uint8_t>(0x30u | (flip ? 0x80u : 0u)));
  ppu.oam[512u] = static_cast<std::uint8_t>(ppu.oam[512u] | 0x02u);  // the large size
  return ppu;
}

Rgba spriteRow(unsigned row) { return out(bg1Colour(1u + row % 15u)); }

// The lines a sprite shows on, and for each the row, read at x = 44.
void expectSpriteRows(const Picture& picture, unsigned firstLine, const std::vector<unsigned>& rows) {
  ASSERT_EQ(picture.frames, 1u);
  const unsigned column = picture.width == 512u ? 88u : 44u;
  EXPECT_EQ(picture.at(column, firstLine - 1u), out(kBackdrop)) << "the line above";
  for (std::size_t n = 0u; n < rows.size(); ++n) {
    EXPECT_EQ(picture.at(column, firstLine + static_cast<unsigned>(n)), spriteRow(rows[n]))
        << "line " << firstLine + n;
  }
  EXPECT_EQ(picture.at(column, firstLine + static_cast<unsigned>(rows.size())), out(kBackdrop))
      << "the line below";
}

TEST(SnesPpuObjInterlace, Bit1DrawsASpriteAtHalfHeightTheFieldPickingTheRows) {
  // Provisional until band j is read. Y = 9: lines 10-17.
  const PpuState ppu = rowedSprite(0x03u, 9u, false);
  expectSpriteRows(drawField(ppu, 0u), 10u, {0u, 2u, 4u, 6u, 8u, 10u, 12u, 14u});
  expectSpriteRows(drawField(ppu, 1u), 10u, {1u, 3u, 5u, 7u, 9u, 11u, 13u, 15u});
}

TEST(SnesPpuObjInterlace, Bit1ActsWithoutBit0) {
  // Provisional until band j is read.
  const PpuState ppu = rowedSprite(0x02u, 9u, false);
  expectSpriteRows(drawField(ppu, 0u), 10u, {0u, 2u, 4u, 6u, 8u, 10u, 12u, 14u});
  expectSpriteRows(drawField(ppu, 1u), 10u, {1u, 3u, 5u, 7u, 9u, 11u, 13u, 15u});
}

TEST(SnesPpuObjInterlace, AVerticalFlipReversesTheRowsTheFieldPicksFrom) {
  // The flip reverses the sprite a row is picked from: field 0 shows row 15 first.
  const PpuState ppu = rowedSprite(0x02u, 9u, true);
  expectSpriteRows(drawField(ppu, 0u), 10u, {15u, 13u, 11u, 9u, 7u, 5u, 3u, 1u});
  expectSpriteRows(drawField(ppu, 1u), 10u, {14u, 12u, 10u, 8u, 6u, 4u, 2u, 0u});
}

TEST(SnesPpuObjInterlace, ASpriteHungAboveThePictureWrapsBeforeTheRowIsDoubled) {
  // Y = 251: line 2 is (2 - 1 - 251) & 255 = 6 lines in, row 12 on field 0; line 3
  // row 14; line 4 would be row 16, past the sprite. (Line 1's sprites are gathered
  // on line 0, which these pictures begin after.)
  //
  // The doubling comes after the eight-bit wrap: a second sprite at Y = 15 is 134
  // lines in on line 150, which doubles to 268 and misses, though 268 wrapped to
  // eight bits would be row 12.
  PpuState ppu = rowedSprite(0x02u, 251u, false);
  putSprite(ppu, 1u, 40, 15u, 0u, 0x30u);
  ppu.oam[512u] = static_cast<std::uint8_t>(ppu.oam[512u] | 0x08u);  // the large size
  const Picture picture = drawField(ppu, 0u);
  EXPECT_EQ(picture.at(44u, 2u), spriteRow(12u));
  EXPECT_EQ(picture.at(44u, 3u), spriteRow(14u));
  EXPECT_EQ(picture.at(44u, 4u), out(kBackdrop));
  EXPECT_EQ(picture.at(44u, 16u), spriteRow(0u));
  EXPECT_EQ(picture.at(44u, 150u), out(kBackdrop));
}

TEST(SnesPpuObjInterlace, RangeCountsAHalfHeightSpriteOnlyOnTheLinesItStandsOn) {
  // Seventeen 16x16 sprites at Y = 9 and sixteen at Y = 17. At full height lines
  // 18-25 cross all thirty-three and Range overflows; at half height the first
  // group stands on 10-17 and the second on 18-25, seventeen at most a line.
  const auto overflows = [](std::uint8_t setini) {
    PpuState ppu = rowedSprite(setini, 9u, false);
    for (unsigned index = 0u; index < 33u; ++index) {
      putSprite(ppu, index, static_cast<int>(index * 6u), static_cast<std::uint8_t>(index < 17u ? 9u : 17u), 0u,
                0x30u);
    }
    for (unsigned n = 0u; n < 9u; ++n) ppu.oam[512u + n] = 0xAAu;
    const std::vector<std::uint8_t> rom = cartridge({kStp});
    Snes machine(SnesConfig{.rom = rom});
    SnesState state = machine.state();
    state.ppu = ppu;
    state.vpos = 1u;
    machine.restore(state);
    machine.run(40u * 1364u);
    return machine.state().ppu.rangeOver;
  };
  EXPECT_TRUE(overflows(0x00u));
  EXPECT_FALSE(overflows(0x02u));
}

TEST(SnesPpuObjInterlace, WithoutBit1ASpriteStandsOnItsWholeHeight) {
  const PpuState ppu = rowedSprite(0x01u, 9u, false);
  std::vector<unsigned> all;
  for (unsigned row = 0u; row < 16u; ++row) all.push_back(row);
  expectSpriteRows(drawField(ppu, 0u), 10u, all);
  expectSpriteRows(drawField(ppu, 1u), 10u, all);
}

// ---- mosaic under interlace -----------------------------------------------------

TEST(SnesPpuInterlace, MosaicedModeFiveReadsTheCornersEvenHalfLineInBothFields) {
  // Provisional until band k is read. $11 with an offset of -2: a block is four
  // half-lines, two lines of a field, and both fields read its corner's half-line
  // 2 x (line - index) - 2.
  PpuState ppu = rowedPicture(0x05u, 0x01u, 0x3FEu);
  ppu.mosaic = 0x11u;
  ppu.mosaicBlockSize = 1u;
  const Picture even = drawField(ppu, 0u);
  const Picture odd = drawField(ppu, 1u);
  ASSERT_EQ(odd.width, 512u);
  for (unsigned line = 1u; line <= 8u; ++line) {
    const unsigned corner = line - (line - 1u) % 2u;
    EXPECT_EQ(odd.at(0u, line), rowed(0u, 2u * corner - 2u)) << "line " << line;
    EXPECT_EQ(even.at(0u, line), rowed(0u, 2u * corner - 2u)) << "line " << line;
  }
}

TEST(SnesPpuInterlace, MosaicSizeZeroAlreadyMovesTheOddField) {
  PpuState ppu = rowedPicture(0x05u, 0x01u, 0x3FEu);
  ppu.mosaic = 0x01u;
  const Picture odd = drawField(ppu, 1u);
  ASSERT_EQ(odd.width, 512u);
  for (unsigned line = 1u; line <= 4u; ++line) {
    EXPECT_EQ(odd.at(1u, line), rowed(0u, 2u * line - 2u)) << "line " << line;
  }
  EXPECT_EQ(drawField(ppu, 0u).pixels, odd.pixels);
}

TEST(SnesPpuInterlace, MosaicInModeOneIsTheSameInBothFields) {
  PpuState ppu = rulerPicture(0x01u, 0x01u, 0x00u);
  ppu.setini = 0x01u;
  putCharacter(ppu, kBg1Chars, 4u, 8u, [](unsigned column, unsigned row) { return 1u + (column + 3u * row) % 15u; });
  fillMap(ppu, kBg1Map, entry(8u, kBg1Palette, false));
  for (const std::uint8_t mosaic : {std::uint8_t{0x01u}, std::uint8_t{0x11u}}) {
    ppu.mosaic = mosaic;
    ppu.mosaicBlockSize = static_cast<std::uint8_t>(mosaic >> 4);
    EXPECT_EQ(drawField(ppu, 0u).pixels, drawField(ppu, 1u).pixels) << "$2106 " << unsigned{mosaic};
  }
}

// ---- what is unchanged ------------------------------------------------------------

TEST(SnesPpuHiresOptIn, APowerOnPpuCarriesNoDecision) {
  const PpuState ppu;
  EXPECT_FALSE(ppu.lastMain.present);
  EXPECT_TRUE(ppu.lastMain == PpuState::MainDecision{});
}

// ---- the staged cartridges -------------------------------------------------------
//
// The cartridges under SNAGGLETOOTH_PPU_ROMS are run and nothing else: no source of
// theirs is opened and nothing from any of them is written down here. What each one
// does was learned by running it. Unset, these cases register and skip with a
// reason.

bool romsRequired() {
  const char* required = std::getenv("SNAGGLETOOTH_REQUIRE_PPU_ROMS");
  return required != nullptr && *required != '\0';
}

std::string findRom(const std::string& name) {
  const std::string root = SNAGGLETOOTH_PPU_ROMS;
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

constexpr std::uint64_t kSecond = 60ull * 357364ull;

// What a staged run showed: the brightest frame and the registers as it ended, and
// over every frame the widths, the parities and the $2133 values seen, and whether
// any frame of 512 had a position whose two halves differ.
struct Watched final : FrameObserver {
  const Snes* machine = nullptr;
  std::vector<std::uint8_t> brightest;
  unsigned brightestWidth = 0;
  unsigned brightestHeight = 0;
  std::uint64_t light = 0;
  PpuState atBrightest;
  std::set<unsigned> widths;
  std::set<std::uint8_t> setinis;
  std::vector<unsigned> fields;
  std::vector<std::vector<std::uint8_t>> lastTwo;
  bool halvesDiffer = false;

  void frame(const VideoFrame& picture) override {
    widths.insert(picture.width);
    fields.push_back(picture.field);
    const PpuState& ppu = machine->state().ppu;
    setinis.insert(ppu.setini);
    if (picture.width == 512u) {
      for (std::size_t at = 0u; at + 8u <= picture.pixels.size() && !halvesDiffer; at += 8u) {
        halvesDiffer = picture.pixels[at] != picture.pixels[at + 4u] ||
                       picture.pixels[at + 1u] != picture.pixels[at + 5u] ||
                       picture.pixels[at + 2u] != picture.pixels[at + 6u];
      }
    }
    lastTwo.emplace_back(picture.pixels.begin(), picture.pixels.end());
    if (lastTwo.size() > 2u) lastTwo.erase(lastTwo.begin());
    std::uint64_t sum = 0;
    for (std::size_t at = 0u; at < picture.pixels.size(); at += 4u) {
      sum += picture.pixels[at] + picture.pixels[at + 1u] + picture.pixels[at + 2u];
    }
    if (sum <= light) return;
    light = sum;
    brightest.assign(picture.pixels.begin(), picture.pixels.end());
    brightestWidth = picture.width;
    brightestHeight = picture.height;
    atBrightest = ppu;
  }
};

// Runs a staged cartridge for `seconds` with `button` held from `from` to `to`, and
// watches only the last second.
void runStaged(Snes& machine, Watched& watch, unsigned seconds, Button button, unsigned from,
               unsigned to) {
  watch.machine = &machine;
  for (unsigned second = 0u; second < seconds; ++second) {
    Joypad pad;
    pad.hold(button, second >= from && second < to);
    machine.setJoypad(JoypadPort::One, pad);
    if (second + 1u == seconds) machine.setFrameObserver(&watch);
    machine.run(kSecond);
  }
}

TEST(PpuHiresCartridges, AModeFiveMosaicCartridgeDrawsBlocksCountedInHalfPixels) {
  // What the cartridge does was learned by running it: it settles in mode 5 with BG1
  // on both screens, interlaced, mosaic enabled on BG1, and R steps the size up every
  // eight frames. After a second of R, in the brightest frame, every block of
  // 2 x (size + 1) half-pixels counted from each line's first is one colour.
  std::vector<std::uint8_t> rom;
  bool ran = false;
  loadRom("MosaicMode5.sfc", rom, ran);
  if (!ran) return;
  Snes machine(SnesConfig{.rom = rom});
  Watched watch;
  runStaged(machine, watch, 3u, Button::R, 1u, 2u);
  ASSERT_FALSE(watch.brightest.empty());
  EXPECT_EQ(watch.atBrightest.bgmode & 0x07u, 5u);
  EXPECT_NE(watch.atBrightest.mosaic & 0x01u, 0u);
  ASSERT_GT(watch.atBrightest.mosaic >> 4, 0u);
  ASSERT_EQ(watch.brightestWidth, 512u);
  const unsigned block = 2u * ((watch.atBrightest.mosaic >> 4) + 1u);
  unsigned blocks = 0u;
  unsigned uneven = 0u;
  std::set<std::uint32_t> colours;
  for (unsigned line = 0u; line < watch.brightestHeight; ++line) {
    for (unsigned left = 0u; left + block <= 512u; left += block) {
      const std::size_t corner = (static_cast<std::size_t>(line) * 512u + left) * 4u;
      colours.insert(watch.brightest[corner] | (watch.brightest[corner + 1u] << 8) |
                     (watch.brightest[corner + 2u] << 16));
      bool even = true;
      for (unsigned h = 1u; h < block; ++h) {
        const std::size_t at = corner + h * 4u;
        even = even && watch.brightest[at] == watch.brightest[corner] &&
               watch.brightest[at + 1u] == watch.brightest[corner + 1u] &&
               watch.brightest[at + 2u] == watch.brightest[corner + 2u];
      }
      ++blocks;
      if (!even) ++uneven;
    }
  }
  EXPECT_GT(blocks, 0u);
  EXPECT_EQ(uneven, 0u) << "of " << blocks << " blocks of " << block << " half-pixels";
  EXPECT_GT(colours.size(), 1u) << "the frame is one flat colour";
}

TEST(PpuHiresCartridges, AHiresDemoReachesModeFiveAndDrawsInHalfPixels) {
  // Learned by running it: A takes it from its mode 1 screen into mode 5, interlaced.
  std::vector<std::uint8_t> rom;
  bool ran = false;
  loadRom("twoship.sfc", rom, ran);
  if (!ran) return;
  Snes machine(SnesConfig{.rom = rom});
  Watched watch;
  runStaged(machine, watch, 8u, Button::A, 2u, 6u);
  EXPECT_EQ(watch.atBrightest.bgmode & 0x07u, 5u);
  EXPECT_EQ(watch.widths, (std::set<unsigned>{512u}));
  EXPECT_TRUE(watch.halvesDiffer);
}

TEST(PpuHiresCartridges, APseudoHiresTransferImageDrawsInHalfPixels) {
  // Learned by running it: mode 1, $2133 bit 3 held, a layer on each screen.
  std::vector<std::uint8_t> rom;
  bool ran = false;
  loadRom("HiColor64PerTileRowPseudoHiRes.sfc", rom, ran);
  if (!ran) return;
  Snes machine(SnesConfig{.rom = rom});
  Watched watch;
  runStaged(machine, watch, 3u, Button::A, 9u, 9u);
  EXPECT_EQ(watch.atBrightest.bgmode & 0x07u, 1u);
  EXPECT_NE(watch.atBrightest.setini & 0x08u, 0u);
  EXPECT_NE(watch.atBrightest.tm, 0u);
  EXPECT_NE(watch.atBrightest.ts, 0u);
  EXPECT_EQ(watch.widths, (std::set<unsigned>{512u}));
  EXPECT_TRUE(watch.halvesDiffer);
}

// The three colour-blend images, learned by running them: mode 3, BG1 on the main
// screen and BG2 on the sub screen, the sub screen added to BG1, and $2133 written
// only with 0 — so no line is drawn in half-pixels. The sum is what they show: the
// last frame drawn again from the same state with $2131 cleared is a different
// picture.
void expectBlend(const std::string& name) {
  std::vector<std::uint8_t> rom;
  bool ran = false;
  loadRom(name, rom, ran);
  if (!ran) return;
  Snes machine(SnesConfig{.rom = rom});
  Watched watch;
  runStaged(machine, watch, 3u, Button::A, 9u, 9u);
  const PpuState& ppu = watch.atBrightest;
  EXPECT_EQ(ppu.bgmode & 0x07u, 3u) << name;
  EXPECT_NE(ppu.tm & 0x01u, 0u) << name;
  EXPECT_NE(ppu.ts & 0x02u, 0u) << name;
  EXPECT_NE(ppu.cgwsel & 0x02u, 0u) << name;
  EXPECT_NE(ppu.cgadsub & 0x01u, 0u) << name;
  for (const std::uint8_t setini : watch.setinis) EXPECT_EQ(setini & 0x08u, 0u) << name;
  EXPECT_EQ(watch.widths, (std::set<unsigned>{256u})) << name;

  SnesState state = machine.state();
  Snes summed(SnesConfig{.rom = rom});
  summed.restore(state);
  Watched withMath;
  withMath.machine = &summed;
  summed.setFrameObserver(&withMath);
  summed.run(kSecond / 30u);
  state.ppu.cgadsub = 0x00u;
  Snes plain(SnesConfig{.rom = rom});
  plain.restore(state);
  Watched withoutMath;
  withoutMath.machine = &plain;
  plain.setFrameObserver(&withoutMath);
  plain.run(kSecond / 30u);
  ASSERT_FALSE(withMath.lastTwo.empty()) << name;
  ASSERT_FALSE(withoutMath.lastTwo.empty()) << name;
  EXPECT_NE(withMath.lastTwo.back(), withoutMath.lastTwo.back()) << name;
}

TEST(PpuHiresCartridges, TheFirstBlendImageAddsItsSubScreen) { expectBlend("HiColor3840.sfc"); }
TEST(PpuHiresCartridges, TheSecondBlendImageAddsItsSubScreen) { expectBlend("HiColor575Myst.sfc"); }
TEST(PpuHiresCartridges, TheThirdBlendImageAddsItsSubScreen) { expectBlend("HiColor1241DLair.sfc"); }

// An interlaced image, learned by running it: mode 5 with $2133 bit 0, and each
// field a steady picture of its own. `halfHeight` says whether it holds bit 1 and
// shows sprites as well.
void expectInterlaced(const std::string& name, bool halfHeight) {
  std::vector<std::uint8_t> rom;
  bool ran = false;
  loadRom(name, rom, ran);
  if (!ran) return;
  Snes machine(SnesConfig{.rom = rom});
  Watched watch;
  runStaged(machine, watch, 3u, Button::A, 9u, 9u);
  const PpuState& ppu = watch.atBrightest;
  EXPECT_EQ(ppu.bgmode & 0x07u, 5u) << name;
  EXPECT_NE(ppu.setini & 0x01u, 0u) << name;
  EXPECT_EQ((ppu.setini & 0x02u) != 0u, halfHeight) << name;
  if (halfHeight) {
    EXPECT_NE(ppu.tm & 0x10u, 0u) << name;
  }
  EXPECT_EQ(watch.widths, (std::set<unsigned>{512u})) << name;
  ASSERT_GE(watch.fields.size(), 4u) << name;
  for (std::size_t n = 1u; n < watch.fields.size(); ++n) {
    EXPECT_NE(watch.fields[n], watch.fields[n - 1u]) << name << ", frame " << n;
  }
  ASSERT_EQ(watch.lastTwo.size(), 2u) << name;
  EXPECT_NE(watch.lastTwo[0], watch.lastTwo[1]) << name;
}

TEST(PpuHiresCartridges, AnInterlacedFontImageAlternatesItsFields) {
  expectInterlaced("InterlaceFont.sfc", false);
}
TEST(PpuHiresCartridges, AnInterlacedSpriteImageHalvesItsSpritesAndAlternatesItsFields) {
  expectInterlaced("InterlaceRPG.sfc", true);
}
TEST(PpuHiresCartridges, AnInterlacedPictureImageAlternatesItsFields) {
  expectInterlaced("InterlaceMoogle.sfc", false);
}

}  // namespace
}  // namespace snaggletooth
