// The sprites the PPU draws, and the two passes that find them: both OAM tables
// decoded, the eight size pairs, the character table and the wrap that is its own
// and not a background's, both flips and the rule a rectangular sprite keeps, the
// palettes above the backgrounds', the four sprite places in Mode 1's priority
// chart, index order and the rule that only the topmost sprite answers the
// backgrounds, the main screen's own bit, and the pass across a line's dots that
// reads $2101 as it stands at each.
//
// Then what the chip can afford and where the walk begins: the sprites Range
// keeps and the tiles Time loads, in the opposite directions they run in, the two
// overflow flags at the points that raise them, the one position nine bits reach
// that is counted somewhere other than where it draws, and the sprite $2103 and
// the sprite-table port put in front of every other.
//
// Every expectation is computed by hand from the register page; the pictures are
// placed as a program would have left them. The staged cartridges at the end are
// run and nothing more.

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

#include "gtest/gtest.h"
#include "snaggletooth/snes/snes.h"

namespace snaggletooth {
namespace {

// Where these pictures keep their pieces, each clear of the others. A map base
// counts whole 32x32 screens of $400 words, so one screen of map is $800 bytes; a
// background's character base counts 8 KB blocks; and a sprite's counts 8 K-word
// blocks, which is 16 KB.
constexpr std::uint8_t kBg1MapBase = 0x04u;  // $2107: screen 1, size 00
constexpr std::uint8_t kBg2MapBase = 0x08u;  // $2108: screen 2
constexpr std::uint8_t kBg3MapBase = 0x0Cu;  // $2109: screen 3
constexpr std::uint8_t kBgCharBases12 = 0x54u;  // $210B: BG1 at $8000, BG2 at $A000
constexpr std::uint8_t kBg3CharBase = 0x03u;    // $210C: BG3 at $6000

constexpr std::uint32_t kBg1MapByte = 0x0800u;
constexpr std::uint32_t kBg2MapByte = 0x1000u;
constexpr std::uint32_t kBg3MapByte = 0x1800u;
constexpr std::uint32_t kBg1CharByte = 0x8000u;
constexpr std::uint32_t kBg2CharByte = 0xA000u;
constexpr std::uint32_t kBg3CharByte = 0x6000u;

// The sprite character base these cases use: $2101 bits 2-0 hold 3, so the first
// table is at word $6000 and byte $C000. With the Name bits at 0 the second table
// follows it immediately, $1000 words on, at byte $E000.
constexpr std::uint8_t kSpriteBase = 0x03u;
constexpr std::uint32_t kSpriteCharByte = 0xC000u;
constexpr std::uint32_t kSpriteCharByte2 = 0xE000u;

// A sprite's character is always sixteen colours: four bitplanes, thirty-two bytes.
constexpr unsigned kSpritePlanes = 4u;
constexpr std::uint32_t kSpriteCharBytes = 32u;

// The palette words these cases use. A word is 15 bits, blue-green-red from the top.
constexpr std::uint16_t kBackdrop = 0x0C41u;  // red 1, green 2, blue 3
constexpr std::uint16_t kRed = 0x001Fu;
constexpr std::uint16_t kGreen = 0x03E0u;
constexpr std::uint16_t kBlue = 0x7C00u;
constexpr std::uint16_t kWhite = 0x7FFFu;
constexpr std::uint16_t kYellow = 0x03FFu;
constexpr std::uint16_t kCyan = 0x7FE0u;
constexpr std::uint16_t kMagenta = 0x7C1Fu;

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
constexpr Rgba kCyanOut{0u, 255u, 255u, 255u};
constexpr Rgba kMagentaOut{255u, 0u, 255u, 255u};

// The first palette a sprite can name, and the words its eight palettes occupy:
// sixteen colours each from CGRAM word 128 up.
constexpr unsigned kSpritePaletteBase = 128u;
constexpr unsigned kSpritePaletteColours = 16u;

// The frames a run finished, the first of them kept whole.
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

  // The four bytes at a position in the kept frame.
  [[nodiscard]] Rgba at(unsigned x, unsigned y) const {
    const std::size_t index = (static_cast<std::size_t>(y) * width + x) * 4u;
    return Rgba{pixels.at(index), pixels.at(index + 1u), pixels.at(index + 2u),
                pixels.at(index + 3u)};
  }
};

constexpr std::uint8_t kStp = 0xDBu;

// A cartridge that stops at once, so the beam runs while the CPU touches nothing.
std::vector<std::uint8_t> haltedCartridge() {
  std::vector<std::uint8_t> program{kStp};
  program.resize(0x8000u, 0x00u);
  program[0x7FFCu] = 0x00u;  // reset -> $8000
  program[0x7FFDu] = 0x80u;
  return program;
}

// More than an NTSC frame of master cycles, so a run from power-on reaches the next
// frame's first line, where a finished frame is handed over.
constexpr std::uint64_t kFrameMaster = 357364u;
constexpr std::uint64_t kOneFrame = kFrameMaster + 20000u;

// A machine holding `ppu`, halted at once, from power-on.
Snes machineWith(const PpuState& ppu) {
  Snes machine(SnesConfig{.rom = haltedCartridge()});
  SnesState state = machine.state();
  state.ppu = ppu;
  machine.restore(state);
  return machine;
}

// Runs such a machine and returns what it drew.
Picture draw(const PpuState& ppu) {
  Snes machine = machineWith(ppu);
  Picture picture;
  machine.setFrameObserver(&picture);
  machine.run(kOneFrame);
  return picture;
}

// A palette word as CGRAM holds it: the low byte, then the high.
void putColour(PpuState& ppu, unsigned word, std::uint16_t colour) {
  ppu.cgram[word * 2u] = static_cast<std::uint8_t>(colour & 0xFFu);
  ppu.cgram[word * 2u + 1u] = static_cast<std::uint8_t>(colour >> 8);
}

// A colour into one of a sprite palette's sixteen words.
void putSpriteColour(PpuState& ppu, unsigned palette, unsigned index, std::uint16_t colour) {
  putColour(ppu, kSpritePaletteBase + palette * kSpritePaletteColours + index, colour);
}

using TileRows = std::array<std::array<std::uint8_t, 8>, 8>;

// A character at a byte address, at as many bitplanes as the caller names: eight
// rows of eight colour indices, planes 0 and 1 in the low and high bytes of eight
// words and planes 2 and 3 in the same form sixteen bytes on, with the leftmost
// pixel of a row in bit 7.
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
TileRows solid(std::uint8_t index) {
  TileRows rows{};
  for (auto& row : rows) row.fill(index);
  return rows;
}

// A sprite character in the first table, by its number in the 16x16 table.
void putSpriteTile(PpuState& ppu, unsigned number, const TileRows& rows) {
  putTileAt(ppu, kSpriteCharByte + number * kSpriteCharBytes, kSpritePlanes, rows);
}

// The same in the second table.
void putSpriteTile2(PpuState& ppu, unsigned number, const TileRows& rows) {
  putTileAt(ppu, kSpriteCharByte2 + number * kSpriteCharBytes, kSpritePlanes, rows);
}

// Every character a sprite as big as `width` by `height` reads when its first is
// `tile`, filled with one colour index — the numbers wrapping in each nibble the
// way the character table does.
void putSpriteBlock(PpuState& ppu, unsigned tile, unsigned width, unsigned height,
                    std::uint8_t index) {
  for (unsigned down = 0u; down < height / 8u; ++down) {
    for (unsigned across = 0u; across < width / 8u; ++across) {
      const unsigned number =
          ((tile + across) & 0x0Fu) | ((((tile >> 4) + down) & 0x0Fu) << 4);
      putSpriteTile(ppu, number, solid(index));
    }
  }
}

// One sprite's record: the low table's four bytes, then the two bits the high
// table holds for it — the ninth bit of X from the low of the pair, the size flag
// above it.
void putSprite(PpuState& ppu, unsigned index, int x, std::uint8_t y, std::uint8_t tile,
               std::uint8_t attributes, bool large) {
  const unsigned wide = static_cast<unsigned>(x & 0x1FF);
  ppu.oam[index * 4u] = static_cast<std::uint8_t>(wide & 0xFFu);
  ppu.oam[index * 4u + 1u] = y;
  ppu.oam[index * 4u + 2u] = tile;
  ppu.oam[index * 4u + 3u] = attributes;

  const unsigned shift = (index & 3u) * 2u;
  const auto bits = static_cast<std::uint8_t>(((wide >> 8) & 1u) | (large ? 2u : 0u));
  auto& byte = ppu.oam[512u + index / 4u];
  byte = static_cast<std::uint8_t>((byte & ~(3u << shift)) | (bits << shift));
}

// Every sprite parked off the left edge, where Range keeps none of them whatever
// size they are — the widest is 64 pixels, so a sprite at X = -64 has no column on
// the picture. A sprite table of zeros is 128 sprites at the origin, which is 128
// sprites in range on the picture's first line, so a case that means to place one
// sprite parks the rest first.
void parkSprites(PpuState& ppu) {
  for (unsigned index = 0u; index < kSprites; ++index) {
    putSprite(ppu, index, -64, 0u, 0u, 0u, false);
  }
}

// A sprite's attribute byte, vhoopppN.
constexpr std::uint8_t attributes(bool flipVertical, bool flipHorizontal,
                                  unsigned priority, unsigned palette, bool secondTable) {
  return static_cast<std::uint8_t>((flipVertical ? 0x80u : 0u) |
                                   (flipHorizontal ? 0x40u : 0u) | (priority << 4) |
                                   (palette << 1) | (secondTable ? 1u : 0u));
}

// A tilemap entry `index` words into a background's map.
void putEntry(PpuState& ppu, std::uint32_t mapByte, unsigned index, std::uint16_t entry) {
  ppu.vram[(mapByte + index * 2u) & 0xFFFFu] = static_cast<std::uint8_t>(entry & 0xFFu);
  ppu.vram[(mapByte + index * 2u + 1u) & 0xFFFFu] = static_cast<std::uint8_t>(entry >> 8);
}

// A PPU placed to show sprites and nothing else: Mode 1 at full brightness, the
// sprite layer alone on the main screen, the bases above, and the backdrop in
// palette word 0.
//
// A sprite whose Y is N has its top row on picture line N + 1, and the console
// outputs no line 0 — so its top row is row N of the frame handed over, and a
// sprite's Y reads straight off the picture.
PpuState screen() {
  PpuState ppu;
  ppu.inidisp = 0x0Fu;  // the screen on, brightness 15
  ppu.bgmode = 0x01u;   // Mode 1, every layer in 8x8 tiles
  ppu.objsel = kSpriteBase;
  ppu.tm = 0x10u;  // the sprites on the main screen
  parkSprites(ppu);
  putColour(ppu, 0u, kBackdrop);
  putSpriteColour(ppu, 0u, 1u, kRed);
  putSpriteColour(ppu, 0u, 2u, kGreen);
  putSpriteColour(ppu, 0u, 15u, kWhite);
  return ppu;
}

// The same, with Mode 1's three backgrounds placed and shown beside the sprites:
// each at its own bases, each scrolled by the -1 that puts a tilemap's first row
// on the picture's first line, and all four layers on the main screen.
PpuState screenWithBackgrounds() {
  PpuState ppu = screen();
  ppu.bg1sc = kBg1MapBase;
  ppu.bg2sc = kBg2MapBase;
  ppu.bg3sc = kBg3MapBase;
  ppu.bg12nba = kBgCharBases12;
  ppu.bg34nba = kBg3CharBase;
  ppu.bg1vofs = 0x3FFu;
  ppu.bg2vofs = 0x3FFu;
  ppu.bg3vofs = 0x3FFu;
  ppu.tm = 0x17u;  // BG1, BG2, BG3 and the sprites
  putColour(ppu, 1u, kYellow);   // BG1 and BG2 palette 0 colour 1
  putColour(ppu, 2u, kCyan);     // BG1 and BG2 palette 0 colour 2
  putColour(ppu, 5u, kMagenta);  // BG3 palette 1 colour 1
  putColour(ppu, 9u, kBlue);     // BG3 palette 2 colour 1
  return ppu;
}

// A background covered edge to edge by one character in one colour, at the tile
// priority named. The map is one 32x32 screen, so 32 entries across and 32 down
// reach every position of the picture.
void coverBackground(PpuState& ppu, std::uint32_t mapByte, std::uint32_t charByte,
                     unsigned planes, unsigned tile, unsigned palette, bool priority,
                     std::uint8_t index) {
  putTileAt(ppu, charByte + tile * 8u * planes, planes, solid(index));
  const auto entry = static_cast<std::uint16_t>(tile | (palette << 10) |
                                                (priority ? 0x2000u : 0u));
  for (unsigned at = 0u; at < 32u * 32u; ++at) putEntry(ppu, mapByte, at, entry);
}

// ---- what a sprite is --------------------------------------------------------

TEST(Sprites, ASpriteDrawsWhereItsRecordPutsIt) {
  PpuState ppu = screen();
  putSpriteTile(ppu, 0u, solid(1u));
  putSprite(ppu, 0u, 100, 50u, 0u, attributes(false, false, 3u, 0u, false), false);

  const Picture picture = draw(ppu);
  // Eight pixels across from X, and eight rows down from Y.
  EXPECT_EQ(picture.at(100u, 50u), kRedOut);
  EXPECT_EQ(picture.at(107u, 57u), kRedOut);
  // And nothing outside it.
  EXPECT_EQ(picture.at(99u, 50u), kBackdropOut);
  EXPECT_EQ(picture.at(108u, 50u), kBackdropOut);
  EXPECT_EQ(picture.at(100u, 49u), kBackdropOut);
  EXPECT_EQ(picture.at(100u, 58u), kBackdropOut);
}

TEST(Sprites, TheNinthBitOfXHangsASpriteOffTheLeftEdge) {
  PpuState ppu = screen();
  putSpriteBlock(ppu, 0u, 16u, 16u, 1u);
  // X = -12: the leftmost four pixels of the sprite stand on the picture.
  putSprite(ppu, 0u, -12, 20u, 0u, attributes(false, false, 3u, 0u, false), true);

  const Picture picture = draw(ppu);
  EXPECT_EQ(picture.at(0u, 20u), kRedOut);
  EXPECT_EQ(picture.at(3u, 20u), kRedOut);
  EXPECT_EQ(picture.at(4u, 20u), kBackdropOut);
  // Nothing of it wrapped round to the right edge.
  EXPECT_EQ(picture.at(255u, 20u), kBackdropOut);
}

TEST(Sprites, ASpriteRunningOffTheRightEdgeIsCutThere) {
  PpuState ppu = screen();
  putSpriteTile(ppu, 0u, solid(1u));
  putSprite(ppu, 0u, 252, 30u, 0u, attributes(false, false, 3u, 0u, false), false);

  const Picture picture = draw(ppu);
  EXPECT_EQ(picture.at(252u, 30u), kRedOut);
  EXPECT_EQ(picture.at(255u, 30u), kRedOut);
  EXPECT_EQ(picture.at(0u, 30u), kBackdropOut);  // and none of it came back at the left
}

TEST(Sprites, ASpriteHungAboveThePictureComesInAtTheTop) {
  PpuState ppu = screen();
  // Sixteen rows, each its own colour index, so which rows show can be read off.
  TileRows top{};
  TileRows bottom{};
  for (unsigned row = 0u; row < 8u; ++row) {
    top[row].fill(1u);
    bottom[row].fill(2u);
  }
  putSpriteTile(ppu, 0u, top);
  putSpriteTile(ppu, 1u, top);
  putSpriteTile(ppu, 16u, bottom);
  putSpriteTile(ppu, 17u, bottom);

  // Y = 248 is -8: the sprite's first eight rows are above the picture and its
  // last eight come in at the top.
  putSprite(ppu, 0u, 40, 248u, 0u, attributes(false, false, 3u, 0u, false), true);

  const Picture picture = draw(ppu);
  EXPECT_EQ(picture.at(40u, 0u), kGreenOut);
  EXPECT_EQ(picture.at(40u, 7u), kGreenOut);
  EXPECT_EQ(picture.at(40u, 8u), kBackdropOut);
}

TEST(Sprites, TheEightSizePairsGiveTheirTwoSizes) {
  // The pairs $2101 bits 7-5 name, small then large, from the register page.
  struct Pair {
    unsigned smallWidth;
    unsigned smallHeight;
    unsigned largeWidth;
    unsigned largeHeight;
  };
  constexpr std::array<Pair, 8> kPairs{{
      {.smallWidth = 8u, .smallHeight = 8u, .largeWidth = 16u, .largeHeight = 16u},
      {.smallWidth = 8u, .smallHeight = 8u, .largeWidth = 32u, .largeHeight = 32u},
      {.smallWidth = 8u, .smallHeight = 8u, .largeWidth = 64u, .largeHeight = 64u},
      {.smallWidth = 16u, .smallHeight = 16u, .largeWidth = 32u, .largeHeight = 32u},
      {.smallWidth = 16u, .smallHeight = 16u, .largeWidth = 64u, .largeHeight = 64u},
      {.smallWidth = 32u, .smallHeight = 32u, .largeWidth = 64u, .largeHeight = 64u},
      {.smallWidth = 16u, .smallHeight = 32u, .largeWidth = 32u, .largeHeight = 64u},
      {.smallWidth = 16u, .smallHeight = 32u, .largeWidth = 32u, .largeHeight = 32u},
  }};

  for (unsigned sss = 0u; sss < 8u; ++sss) {
    for (unsigned large = 0u; large < 2u; ++large) {
      const Pair& pair = kPairs[sss];
      const unsigned width = large != 0u ? pair.largeWidth : pair.smallWidth;
      const unsigned height = large != 0u ? pair.largeHeight : pair.smallHeight;

      PpuState ppu = screen();
      ppu.objsel = static_cast<std::uint8_t>(kSpriteBase | (sss << 5));
      putSpriteBlock(ppu, 0u, 64u, 64u, 1u);  // every character the largest size reads
      putSprite(ppu, 0u, 8, 8u, 0u, attributes(false, false, 3u, 0u, false), large != 0u);

      const Picture picture = draw(ppu);
      const std::string where = "size " + std::to_string(sss) + (large != 0u ? " large" : " small");
      EXPECT_EQ(picture.at(8u + width - 1u, 8u), kRedOut) << where << ": its last column";
      EXPECT_EQ(picture.at(8u + width, 8u), kBackdropOut) << where << ": past its last column";
      EXPECT_EQ(picture.at(8u, 8u + height - 1u), kRedOut) << where << ": its last row";
      EXPECT_EQ(picture.at(8u, 8u + height), kBackdropOut) << where << ": past its last row";
    }
  }
}

// ---- the character table -----------------------------------------------------

TEST(Sprites, TheNameBaseCountsEightThousandWordBlocks) {
  PpuState ppu = screen();
  // The base is bits 2-0 of $2101 and the address is Base << 13 words, so base 3
  // puts the table at word $6000 and byte $C000 — which is where these cases
  // write their characters.
  putSpriteTile(ppu, 0u, solid(1u));
  putSprite(ppu, 0u, 60, 60u, 0u, attributes(false, false, 3u, 0u, false), false);
  EXPECT_EQ(draw(ppu).at(60u, 60u), kRedOut);

  // Move the base one block on and the same record reads a table that is not there.
  ppu.objsel = static_cast<std::uint8_t>((kSpriteBase + 1u) & 0x07u);
  EXPECT_EQ(draw(ppu).at(60u, 60u), kBackdropOut);
}

TEST(Sprites, TheSecondTableSitsTheNameGapPastTheFirst) {
  PpuState ppu = screen();
  // The Name bits are 0, so the second table follows the first immediately: the
  // gap is (Name + 1) << 12 words, which is exactly one table of 256 characters
  // at sixteen words each.
  putSpriteTile(ppu, 0u, solid(1u));    // first table: red
  putSpriteTile2(ppu, 0u, solid(2u));   // second table: green
  putSprite(ppu, 0u, 30, 30u, 0u, attributes(false, false, 3u, 0u, false), false);
  putSprite(ppu, 1u, 60, 30u, 0u, attributes(false, false, 3u, 0u, true), false);

  const Picture picture = draw(ppu);
  EXPECT_EQ(picture.at(30u, 30u), kRedOut);
  EXPECT_EQ(picture.at(60u, 30u), kGreenOut);
}

TEST(Sprites, ASpritesCharactersWrapInsideTheTableWhereABackgroundsBlockRunsOn) {
  // The two rules side by side, because they are opposite and easy to unify by
  // mistake. A sprite's number is a row and a column in a 16x16 table and each
  // nibble wraps on its own; a background's 16x16 block runs on from its number.
  PpuState ppu = screenWithBackgrounds();
  ppu.bgmode = 0x11u;  // Mode 1, BG1 in 16x16 blocks
  ppu.tm = 0x11u;      // BG1 and the sprites

  // A 16x16 sprite whose first character is $FF is made of $FF, $F0, $0F and $00.
  putSpriteTile(ppu, 0xFFu, solid(1u));
  putSpriteTile(ppu, 0xF0u, solid(2u));
  putSpriteTile(ppu, 0x0Fu, solid(15u));
  putSpriteTile(ppu, 0x00u, solid(1u));
  putSprite(ppu, 0u, 100, 100u, 0xFFu, attributes(false, false, 3u, 0u, false), true);

  // A BG1 block whose entry names tile $2FF is made of $2FF, $300, $30F and $310.
  putTileAt(ppu, kBg1CharByte + 0x2FFu * 8u * 4u, 4u, solid(1u));
  putTileAt(ppu, kBg1CharByte + 0x300u * 8u * 4u, 4u, solid(2u));
  putTileAt(ppu, kBg1CharByte + 0x30Fu * 8u * 4u, 4u, solid(1u));
  putTileAt(ppu, kBg1CharByte + 0x310u * 8u * 4u, 4u, solid(2u));
  putEntry(ppu, kBg1MapByte, 0u, 0x02FFu);  // the block at the picture's top left

  const Picture picture = draw(ppu);
  // The sprite's four quarters, in the colours its wrapped numbers name.
  EXPECT_EQ(picture.at(100u, 100u), kRedOut);    // $FF
  EXPECT_EQ(picture.at(108u, 100u), kGreenOut);  // $F0, the column wrapped
  EXPECT_EQ(picture.at(100u, 108u), kWhiteOut);  // $0F, the row wrapped
  EXPECT_EQ(picture.at(108u, 108u), kRedOut);    // $00, both

  // The background's four, in the colours its running-on numbers name.
  EXPECT_EQ(picture.at(0u, 0u), kYellowOut);  // $2FF, index 1 of BG1's palette 0
  EXPECT_EQ(picture.at(8u, 0u), kCyanOut);    // $300
  EXPECT_EQ(picture.at(0u, 8u), kYellowOut);  // $30F
  EXPECT_EQ(picture.at(8u, 8u), kCyanOut);    // $310
}

// ---- the flips ---------------------------------------------------------------

TEST(Sprites, AHorizontalFlipReversesTheWholeSprite) {
  PpuState ppu = screen();
  // A 16x8 run of pixels would do, but the pair of characters is the point: the
  // flip must exchange them, not merely reverse each.
  putSpriteBlock(ppu, 0u, 16u, 16u, 1u);
  putSpriteTile(ppu, 1u, solid(2u));   // the sprite's right-hand character
  putSpriteTile(ppu, 17u, solid(2u));

  putSprite(ppu, 0u, 20, 20u, 0u, attributes(false, false, 3u, 0u, false), true);
  putSprite(ppu, 1u, 60, 20u, 0u, attributes(false, true, 3u, 0u, false), true);

  const Picture picture = draw(ppu);
  EXPECT_EQ(picture.at(20u, 20u), kRedOut);    // unflipped: left half red
  EXPECT_EQ(picture.at(28u, 20u), kGreenOut);  // right half green
  EXPECT_EQ(picture.at(60u, 20u), kGreenOut);  // flipped: the halves exchanged
  EXPECT_EQ(picture.at(68u, 20u), kRedOut);
}

TEST(Sprites, AVerticalFlipReversesTheWholeSprite) {
  PpuState ppu = screen();
  putSpriteBlock(ppu, 0u, 16u, 16u, 1u);
  putSpriteTile(ppu, 16u, solid(2u));  // the sprite's lower characters
  putSpriteTile(ppu, 17u, solid(2u));

  putSprite(ppu, 0u, 20, 20u, 0u, attributes(false, false, 3u, 0u, false), true);
  putSprite(ppu, 1u, 60, 20u, 0u, attributes(true, false, 3u, 0u, false), true);

  const Picture picture = draw(ppu);
  EXPECT_EQ(picture.at(20u, 20u), kRedOut);    // unflipped: top half red
  EXPECT_EQ(picture.at(20u, 28u), kGreenOut);  // bottom half green
  EXPECT_EQ(picture.at(60u, 20u), kGreenOut);  // flipped: the halves exchanged
  EXPECT_EQ(picture.at(60u, 28u), kRedOut);
}

TEST(Sprites, ARectangularSpriteFlipsVerticallyAsTwoSquaresStacked) {
  // A 16x32 sprite is two 16x16 squares stacked, and a vertical flip reverses
  // each square where it stands rather than reversing the whole sprite: rows
  // "01234567" become "32107654", not "76543210".
  PpuState ppu = screen();
  ppu.objsel = static_cast<std::uint8_t>(kSpriteBase | (6u << 5));  // 16x32 and 32x64

  // The four rows of characters, each its own colour index, so the order they end
  // up in can be read straight off the picture.
  putSpriteColour(ppu, 0u, 3u, kBlue);
  constexpr std::array<std::uint8_t, 4> kBands{1u, 2u, 15u, 3u};
  for (unsigned band = 0u; band < kBands.size(); ++band) {
    putSpriteTile(ppu, band * 16u, solid(kBands[band]));
    putSpriteTile(ppu, band * 16u + 1u, solid(kBands[band]));
  }

  putSprite(ppu, 0u, 40, 40u, 0u, attributes(true, false, 3u, 0u, false), false);

  const Picture picture = draw(ppu);
  // Rows 0,1,2,3 of characters become 1,0,3,2: the halves keep their places.
  EXPECT_EQ(picture.at(40u, 40u), kGreenOut);  // character row 1
  EXPECT_EQ(picture.at(40u, 48u), kRedOut);    // character row 0
  EXPECT_EQ(picture.at(40u, 56u), kBlueOut);   // character row 3
  EXPECT_EQ(picture.at(40u, 64u), kWhiteOut);  // character row 2
}

TEST(Sprites, ObjectInterlaceMakesA16By32SpriteA16By16One) {
  // Under object interlace ($2133 bit 1) a 16x32 sprite is a 16x16 one: its lower
  // half is not read and its upper half is shown at half height, so it stands on
  // eight lines rather than sixteen (Errata, PPU p.2). The larger sizes of the
  // pair are untouched.
  PpuState ppu = screen();
  ppu.setini = 0x02u;  // object interlace
  ppu.objsel = static_cast<std::uint8_t>(kSpriteBase | (6u << 5));  // small 16x32, large 32x64
  putSpriteBlock(ppu, 0u, 32u, 64u, 1u);  // every character either size reads

  putSprite(ppu, 0u, 8, 8u, 0u, attributes(false, false, 3u, 0u, false), false);  // the 16x32
  const Picture small = draw(ppu);
  EXPECT_EQ(small.at(8u, 15u), kRedOut);       // its last of eight lines
  EXPECT_EQ(small.at(8u, 16u), kBackdropOut);  // the ninth line, and the lower half, are gone

  putSprite(ppu, 0u, 8, 8u, 0u, attributes(false, false, 3u, 0u, false), true);  // the 32x64
  const Picture large = draw(ppu);
  EXPECT_EQ(large.at(8u, 16u), kRedOut);  // the larger size keeps its full interlaced height
}

// ---- the palettes ------------------------------------------------------------

TEST(Sprites, SpritePalettesBeginAtCgramWordOneHundredAndTwentyEight) {
  PpuState ppu = screen();
  // Palette 5 colour 1 is word 128 + 5 * 16 + 1.
  putSpriteColour(ppu, 5u, 1u, kMagenta);
  putSpriteTile(ppu, 0u, solid(1u));
  putSprite(ppu, 0u, 70, 70u, 0u, attributes(false, false, 3u, 5u, false), false);

  EXPECT_EQ(draw(ppu).at(70u, 70u), kMagentaOut);
}

TEST(Sprites, ColourZeroOfASpritePaletteIsTransparent) {
  PpuState ppu = screen();
  // A character whose left half is colour 0 and right half colour 1.
  TileRows rows{};
  for (auto& row : rows) {
    for (unsigned column = 0u; column < 8u; ++column) row[column] = column < 4u ? 0u : 1u;
  }
  putSpriteTile(ppu, 0u, rows);
  putSprite(ppu, 0u, 80, 80u, 0u, attributes(false, false, 3u, 0u, false), false);

  const Picture picture = draw(ppu);
  EXPECT_EQ(picture.at(80u, 80u), kBackdropOut);  // the backdrop shows through
  EXPECT_EQ(picture.at(84u, 80u), kRedOut);
}

// ---- the priority chart ------------------------------------------------------

// The chart Mode 1 keeps, front to back, writing a digit for a sprite at that
// priority and a letter for a background's tiles:
//
//    3 A B 2 a b 1 C 0 c        and with $2105 bit 3 set:   C 3 A B 2 a b 1 0 c
//
// Each case below pins one adjacency in it, with the two layers overlapping at a
// position nothing else claims.

// A picture with one background covering everything at the tile priority named,
// and one sprite over it at the sprite priority named.
Picture chart(unsigned backgrounds, std::uint32_t mapByte, std::uint32_t charByte,
              unsigned planes, unsigned palette, bool tilePriority, std::uint8_t index,
              unsigned spritePriority, bool bg3InFront) {
  PpuState ppu = screenWithBackgrounds();
  ppu.tm = static_cast<std::uint8_t>(backgrounds | 0x10u);
  if (bg3InFront) ppu.bgmode = static_cast<std::uint8_t>(ppu.bgmode | 0x08u);
  coverBackground(ppu, mapByte, charByte, planes, 1u, palette, tilePriority, index);
  putSpriteTile(ppu, 0u, solid(1u));
  putSprite(ppu, 0u, 100, 100u, 0u,
            attributes(false, false, spritePriority, 0u, false), false);
  return draw(ppu);
}

TEST(SpriteChart, ASpriteAtPriorityThreeIsInFrontOfBg1sHighTiles) {
  const Picture picture = chart(0x01u, kBg1MapByte, kBg1CharByte, 4u, 0u, true, 1u, 3u, false);
  EXPECT_EQ(picture.at(100u, 100u), kRedOut);     // the sprite
  EXPECT_EQ(picture.at(100u, 120u), kYellowOut);  // BG1 elsewhere
}

TEST(SpriteChart, Bg2sHighTilesAreInFrontOfASpriteAtPriorityTwo) {
  const Picture picture = chart(0x02u, kBg2MapByte, kBg2CharByte, 4u, 0u, true, 1u, 2u, false);
  EXPECT_EQ(picture.at(100u, 100u), kYellowOut);  // BG2's high tiles win
}

TEST(SpriteChart, ASpriteAtPriorityTwoIsInFrontOfBg1sLowTiles) {
  const Picture picture = chart(0x01u, kBg1MapByte, kBg1CharByte, 4u, 0u, false, 1u, 2u, false);
  EXPECT_EQ(picture.at(100u, 100u), kRedOut);
}

TEST(SpriteChart, Bg2sLowTilesAreInFrontOfASpriteAtPriorityOne) {
  const Picture picture = chart(0x02u, kBg2MapByte, kBg2CharByte, 4u, 0u, false, 1u, 1u, false);
  EXPECT_EQ(picture.at(100u, 100u), kYellowOut);
}

TEST(SpriteChart, ASpriteAtPriorityOneIsInFrontOfBg3sHighTiles) {
  // BG3 is four colours, so two bitplanes, and its palette 1 colour 1 is word 5.
  const Picture picture = chart(0x04u, kBg3MapByte, kBg3CharByte, 2u, 1u, true, 1u, 1u, false);
  EXPECT_EQ(picture.at(100u, 100u), kRedOut);
  EXPECT_EQ(picture.at(100u, 120u), kMagentaOut);  // BG3 elsewhere
}

TEST(SpriteChart, Bg3sHighTilesAreInFrontOfASpriteAtPriorityZero) {
  const Picture picture = chart(0x04u, kBg3MapByte, kBg3CharByte, 2u, 1u, true, 1u, 0u, false);
  EXPECT_EQ(picture.at(100u, 100u), kMagentaOut);
}

TEST(SpriteChart, ASpriteAtPriorityZeroIsInFrontOfBg3sLowTiles) {
  const Picture picture = chart(0x04u, kBg3MapByte, kBg3CharByte, 2u, 1u, false, 1u, 0u, false);
  EXPECT_EQ(picture.at(100u, 100u), kRedOut);
}

TEST(SpriteChart, Bg3InFrontLiftsItAboveEverySprite) {
  // $2105 bit 3 takes BG3's high-priority tiles from behind a sprite at priority 1
  // to in front of one at priority 3.
  const Picture behind = chart(0x04u, kBg3MapByte, kBg3CharByte, 2u, 1u, true, 1u, 3u, false);
  EXPECT_EQ(behind.at(100u, 100u), kRedOut);

  const Picture inFront = chart(0x04u, kBg3MapByte, kBg3CharByte, 2u, 1u, true, 1u, 3u, true);
  EXPECT_EQ(inFront.at(100u, 100u), kMagentaOut);
}

// ---- sprites among themselves ------------------------------------------------

TEST(Sprites, TheLowerIndexSpriteIsInFront) {
  PpuState ppu = screen();
  putSpriteTile(ppu, 0u, solid(1u));  // red
  putSpriteTile(ppu, 1u, solid(2u));  // green
  putSprite(ppu, 3u, 50, 50u, 0u, attributes(false, false, 3u, 0u, false), false);
  putSprite(ppu, 7u, 50, 50u, 1u, attributes(false, false, 3u, 0u, false), false);

  EXPECT_EQ(draw(ppu).at(50u, 50u), kRedOut);  // sprite 3, not sprite 7
}

TEST(Sprites, OnlyTheTopmostSpritesPriorityAnswersTheBackgrounds) {
  // Two sprites in the same place, the one in front at priority 0 and the one
  // behind at priority 3. A background that shows above priority 0 hides both —
  // the sprite behind does not come out from under it.
  PpuState ppu = screenWithBackgrounds();
  ppu.tm = 0x11u;  // BG1 and the sprites
  coverBackground(ppu, kBg1MapByte, kBg1CharByte, 4u, 1u, 0u, true, 1u);  // BG1 high tiles
  putSpriteTile(ppu, 0u, solid(1u));   // red, in front, priority 0
  putSpriteTile(ppu, 1u, solid(2u));   // green, behind, priority 3
  putSprite(ppu, 4u, 100, 100u, 0u, attributes(false, false, 0u, 0u, false), false);
  putSprite(ppu, 5u, 100, 100u, 1u, attributes(false, false, 3u, 0u, false), false);

  EXPECT_EQ(draw(ppu).at(100u, 100u), kYellowOut);  // BG1, over both of them
}

// ---- the screens -------------------------------------------------------------

TEST(Sprites, TheMainScreenBitShowsTheSprites) {
  PpuState ppu = screen();
  putSpriteTile(ppu, 0u, solid(1u));
  putSprite(ppu, 0u, 90, 90u, 0u, attributes(false, false, 3u, 0u, false), false);
  EXPECT_EQ(draw(ppu).at(90u, 90u), kRedOut);

  ppu.tm = 0u;
  EXPECT_EQ(draw(ppu).at(90u, 90u), kBackdropOut);
}

TEST(Sprites, SpritesOnTheSubScreenAloneShowNowhere) {
  PpuState ppu = screen();
  ppu.tm = 0u;
  ppu.ts = 0x10u;  // the sprites on the sub screen only
  putSpriteTile(ppu, 0u, solid(1u));
  putSprite(ppu, 0u, 90, 90u, 0u, attributes(false, false, 3u, 0u, false), false);

  EXPECT_EQ(draw(ppu).at(90u, 90u), kBackdropOut);
}

// ---- the two passes ----------------------------------------------------------

// Far enough into the frame's first line for every sprite's dot to have gone by,
// and not so far as to reach the next line, where the pass begins again: sprite
// 127 is examined at dot 22 + 127 * 2 = 276, which is 1104 master cycles into a
// line of 1364.
constexpr std::uint64_t kPastEverySpritesDot = 1200u;

// The state a run of `cycles` from power-on leaves, with `ppu` restored first.
SnesState after(const PpuState& ppu, std::uint64_t cycles) {
  Snes machine = machineWith(ppu);
  machine.run(cycles);
  return machine.state();
}

TEST(SpritePasses, RangeKeepsTheSpritesTheNextLineCrosses) {
  PpuState ppu = screen();
  putSpriteTile(ppu, 0u, solid(1u));
  // Three sprites the picture's line 1 crosses and one it does not: a sprite whose
  // Y is N draws on lines N + 1 through N + 8.
  putSprite(ppu, 2u, 10, 0u, 0u, attributes(false, false, 3u, 0u, false), false);
  putSprite(ppu, 5u, 20, 0u, 0u, attributes(false, false, 3u, 0u, false), false);
  putSprite(ppu, 9u, 30, 100u, 0u, attributes(false, false, 3u, 0u, false), false);
  putSprite(ppu, 60u, 40, 0u, 0u, attributes(false, false, 3u, 0u, false), false);

  const SnesState state = after(ppu, kPastEverySpritesDot);
  ASSERT_EQ(state.ppu.sprites.found, 3u);
  EXPECT_EQ(state.ppu.sprites.inRange[0], 2u);
  EXPECT_EQ(state.ppu.sprites.inRange[1], 5u);
  EXPECT_EQ(state.ppu.sprites.inRange[2], 60u);
}

TEST(SpritePasses, RangeLeavesOutASpriteWithNothingOnThePicture) {
  PpuState ppu = screen();
  putSpriteTile(ppu, 0u, solid(1u));
  // An 8x8 sprite at X = -8 has no column at or right of the picture's left edge,
  // so Range does not keep it; one at X = -7 has one.
  putSprite(ppu, 0u, -8, 0u, 0u, attributes(false, false, 3u, 0u, false), false);
  putSprite(ppu, 1u, -7, 0u, 0u, attributes(false, false, 3u, 0u, false), false);

  const SnesState state = after(ppu, kPastEverySpritesDot);
  ASSERT_EQ(state.ppu.sprites.found, 1u);
  EXPECT_EQ(state.ppu.sprites.inRange[0], 1u);
}

TEST(SpritePasses, ARangePassReadsObselAsItStandsAtEachSpritesOwnDot) {
  // The pass walks OAM across a line's visible dots, two dots a sprite, so a write
  // to $2101 partway along the line reaches the sprites whose dots have not gone by
  // and none of the ones already examined. A renderer that evaluated the whole line
  // at one instant would take the write either wholly early or wholly late.
  PpuState ppu = screen();
  ppu.objsel = static_cast<std::uint8_t>(kSpriteBase | (0u << 5));  // 8x8 and 16x16
  putSpriteBlock(ppu, 0u, 64u, 64u, 1u);
  // Every sprite is large, and its Y of 216 is -40 — so the line being gathered,
  // the picture's first, is 40 rows below the sprite's own top: inside a 64-row
  // sprite and past the bottom of a 16-row one.
  for (unsigned index = 0u; index < kSprites; ++index) {
    putSprite(ppu, index, 8, 216u, 0u, attributes(false, false, 3u, 0u, false), true);
  }

  // Stop partway through line 0's pass, change the size pair, and finish the line.
  Snes machine = machineWith(ppu);
  machine.run(600u);
  SnesState state = machine.state();
  const std::uint8_t examined = state.ppu.sprites.scanned;
  ASSERT_GT(examined, 0u);
  ASSERT_LT(examined, kSprites);
  ASSERT_EQ(state.ppu.sprites.found, 0u);  // none of them at 16x16

  state.ppu.objsel = static_cast<std::uint8_t>(kSpriteBase | (2u << 5));  // 8x8 and 64x64
  machine.restore(state);
  machine.run(kPastEverySpritesDot - 600u);

  // Every sprite whose dot came after the write is in range and the ones before it
  // are not, so the thirty-two Range keeps are the thirty-two beginning there.
  const SnesState finished = machine.state();
  ASSERT_EQ(finished.ppu.sprites.found, 32u);
  EXPECT_EQ(finished.ppu.sprites.inRange[0], examined);
  EXPECT_EQ(finished.ppu.sprites.inRange[31], static_cast<std::uint8_t>(examined + 31u));
}

TEST(SpritePasses, ForcedBlankWalksNowhereAndGathersNothing) {
  PpuState ppu = screen();
  ppu.inidisp = 0x8Fu;  // forced blank, brightness 15
  putSpriteTile(ppu, 0u, solid(1u));
  putSprite(ppu, 0u, 50, 0u, 0u, attributes(false, false, 3u, 0u, false), false);

  const SnesState state = after(ppu, kPastEverySpritesDot);
  EXPECT_EQ(state.ppu.sprites.scanned, 0u);
  EXPECT_EQ(state.ppu.sprites.found, 0u);
  EXPECT_EQ(state.ppu.sprites.line, 0u);
}

TEST(SpritePasses, TheSpriteLineIsPartOfTheMachinesState) {
  PpuState ppu = screen();
  putSpriteTile(ppu, 0u, solid(1u));
  putSprite(ppu, 0u, 50, 20u, 0u, attributes(false, false, 3u, 0u, false), false);

  Snes machine = machineWith(ppu);
  machine.run(40u * 1364u);  // well inside the picture, with a line gathered
  const SnesState taken = machine.state();
  ASSERT_GT(taken.ppu.sprites.line, 0u);

  // A snapshot carries the buffer and the pass alike, and comparing PpuState
  // compares them.
  Snes other = machineWith(PpuState{});
  other.restore(taken);
  EXPECT_TRUE(other.state().ppu == taken.ppu);

  PpuState changed = taken.ppu;
  changed.sprites.word[50] = static_cast<std::uint8_t>(changed.sprites.word[50] ^ 0xFFu);
  EXPECT_FALSE(changed == taken.ppu);
}

TEST(SpritePasses, EvaluationRunsOnAMachineNobodyIsWatching) {
  // The overflow flags a pass sets are readable, so the pass is not the observer's
  // to ask for: two machines run identically, one watched and one not, are in the
  // same state afterwards — the sprite line included.
  PpuState ppu = screen();
  putSpriteTile(ppu, 0u, solid(1u));
  for (unsigned index = 0u; index < 8u; ++index) {
    putSprite(ppu, index, static_cast<int>(index) * 20, 20u, 0u,
              attributes(false, false, 3u, 0u, false), false);
  }

  Snes watched = machineWith(ppu);
  Picture picture;
  watched.setFrameObserver(&picture);
  watched.run(kOneFrame);

  Snes unwatched = machineWith(ppu);
  unwatched.run(kOneFrame);

  EXPECT_GT(picture.frames, 0u);
  EXPECT_TRUE(watched.state().ppu == unwatched.state().ppu);
  EXPECT_GT(watched.state().ppu.sprites.line, 0u);
}

// ---- what the chip can afford ------------------------------------------------

// A line of an NTSC frame, in master cycles, and a point inside line `line` past
// every sprite's dot: the pass running there has finished walking OAM and is
// gathering for the line after it, and the line's own Time pass has already run
// in the blank that began it.
constexpr std::uint64_t kLineMaster = 1364u;
std::uint64_t pastTheDotsOf(unsigned line) {
  return static_cast<std::uint64_t>(line) * kLineMaster + kPastEverySpritesDot;
}

// A row of sprites all alike, at one position, from index `from` up.
void putSpriteRun(PpuState& ppu, unsigned from, unsigned count, int x, std::uint8_t y,
                  bool large) {
  for (unsigned index = from; index < from + count; ++index) {
    putSprite(ppu, index, x, y, 0u, attributes(false, false, 3u, 0u, false), large);
  }
}

TEST(SpriteLimits, RangeKeepsThirtyTwoSpritesAndTheThirtyThirdRaisesItsFlag) {
  PpuState ppu = screen();
  putSpriteTile(ppu, 0u, solid(1u));
  putSpriteRun(ppu, 0u, 32u, 0, 0u, false);

  SnesState state = after(ppu, kPastEverySpritesDot);
  EXPECT_EQ(state.ppu.sprites.found, 32u);
  EXPECT_EQ(state.ppu.sprites.inRange[31], 31u);
  EXPECT_FALSE(state.ppu.rangeOver);

  putSpriteRun(ppu, 32u, 1u, 0, 0u, false);
  state = after(ppu, kPastEverySpritesDot);
  EXPECT_EQ(state.ppu.sprites.found, 32u);  // the thirty-third is not kept
  EXPECT_EQ(state.ppu.sprites.inRange[31], 31u);
  EXPECT_TRUE(state.ppu.rangeOver);
}

TEST(SpriteLimits, TheRangeOverflowFlagWaitsForTheOverflowingSpritesOwnDot) {
  PpuState ppu = screen();
  putSpriteTile(ppu, 0u, solid(1u));
  putSpriteRun(ppu, 0u, 32u, 0, 0u, false);
  putSpriteRun(ppu, 100u, 1u, 0, 0u, false);

  // Sprite 100 is examined at dot 22 + 100 * 2 = 222, which is 888 master cycles
  // into the line. 600 master cycles is dot 150: the thirty-two sprites Range can
  // keep are all found, and the sprite that is one too many has not been reached.
  const SnesState early = after(ppu, 600u);
  EXPECT_EQ(early.ppu.sprites.found, 32u);
  EXPECT_FALSE(early.ppu.rangeOver);

  EXPECT_TRUE(after(ppu, kPastEverySpritesDot).ppu.rangeOver);
}

TEST(SpriteLimits, BothOverflowFlagsAreRaisedWhetherOrNotTheSpritesAreShown) {
  PpuState ppu = screen();
  ppu.tm = 0x00u;  // nothing at all on the main screen
  putSpriteBlock(ppu, 0u, 16u, 16u, 1u);
  // Thirty-five sprites of two tiles a row: three more than Range keeps, and the
  // thirty-two it does keep are sixty-four tiles against Time's thirty-four.
  putSpriteRun(ppu, 0u, 35u, 0, 0u, true);

  const SnesState state = after(ppu, pastTheDotsOf(2u));
  EXPECT_TRUE(state.ppu.rangeOver);
  EXPECT_TRUE(state.ppu.timeOver);
}

TEST(SpriteLimits, TimeLoadsThirtyFourTilesBackwardsFromTheLastSpriteRangeKept) {
  PpuState ppu = screen();
  ppu.objsel = static_cast<std::uint8_t>(kSpriteBase | (2u << 5));  // 8x8 and 64x64
  putSpriteBlock(ppu, 0u, 64u, 64u, 1u);
  putSpriteColour(ppu, 1u, 1u, kGreen);
  putSpriteColour(ppu, 2u, 1u, kBlue);

  // Five sprites of eight tiles a row, forty tiles against Time's thirty-four.
  // Range keeps them in index order; Time walks that order backwards, so sprite 4
  // is loaded whole, sprites 3, 2 and 1 take the count to thirty-two, and sprite 0
  // — the first Range kept — gets the two that are left, its leftmost two.
  putSprite(ppu, 0u, 0, 10u, 0u, attributes(false, false, 3u, 1u, false), true);
  putSpriteRun(ppu, 1u, 3u, 128, 10u, true);
  putSprite(ppu, 4u, 192, 10u, 0u, attributes(false, false, 3u, 2u, false), true);

  const Picture picture = draw(ppu);
  EXPECT_EQ(picture.at(0u, 10u), kGreenOut);
  EXPECT_EQ(picture.at(15u, 10u), kGreenOut);
  EXPECT_EQ(picture.at(16u, 10u), kBackdropOut);
  EXPECT_EQ(picture.at(63u, 10u), kBackdropOut);
  EXPECT_EQ(picture.at(128u, 10u), kRedOut);
  EXPECT_EQ(picture.at(191u, 10u), kRedOut);
  // Loaded whole, which is what says the count ran from this end and not the other.
  EXPECT_EQ(picture.at(192u, 10u), kBlueOut);
  EXPECT_EQ(picture.at(255u, 10u), kBlueOut);
  EXPECT_TRUE(after(ppu, pastTheDotsOf(11u)).ppu.timeOver);
}

TEST(SpriteLimits, TimeCountsOnlyTheTilesStandingOnThePicture) {
  PpuState ppu = screen();
  putSpriteBlock(ppu, 0u, 16u, 16u, 1u);

  // Eighteen sprites of two tiles a row: thirty-six tiles, two past what Time
  // loads, so on the picture they raise the flag.
  putSpriteRun(ppu, 0u, 18u, 0, 0u, true);
  EXPECT_TRUE(after(ppu, pastTheDotsOf(8u)).ppu.timeOver);

  // At X = -15 each sprite's left tile stands at -15 and its right at -7, and only
  // the second of those counts: eighteen tiles, inside the count.
  putSpriteRun(ppu, 0u, 18u, -15, 0u, true);
  EXPECT_FALSE(after(ppu, pastTheDotsOf(8u)).ppu.timeOver);

  // At X = 250 the left tile stands at 250 and the right at 258, and again only
  // one of the two counts.
  putSpriteRun(ppu, 0u, 18u, 250, 0u, true);
  EXPECT_FALSE(after(ppu, pastTheDotsOf(8u)).ppu.timeOver);
}

TEST(SpriteLimits, ASpriteAtTheFarSideOfNineBitsDrawsNowhereAndStillFillsARangeSlot) {
  PpuState ppu = screen();
  putSpriteBlock(ppu, 0u, 16u, 16u, 1u);
  putSpriteColour(ppu, 1u, 1u, kGreen);

  // X = -256 is the one position nine bits reach that is the picture's own left
  // edge a whole screen over. Range counts it there and keeps it; it draws where
  // its X puts it, which is nowhere.
  putSprite(ppu, 0u, -256, 20u, 0u, attributes(false, false, 3u, 1u, false), true);
  putSprite(ppu, 1u, 0, 20u, 0u, attributes(false, false, 3u, 0u, false), true);

  const SnesState state = after(ppu, pastTheDotsOf(21u));
  EXPECT_EQ(state.ppu.sprites.found, 2u);
  EXPECT_EQ(state.ppu.sprites.inRange[0], 0u);

  // Sprite 0 is in front of sprite 1, so drawing it at the position Range counted
  // it at would put its colour here instead.
  const Picture picture = draw(ppu);
  EXPECT_EQ(picture.at(0u, 20u), kRedOut);
  EXPECT_EQ(picture.at(15u, 20u), kRedOut);
}

TEST(SpriteLimits, ASpriteAtTheFarSideOfNineBitsTakesItsTilesFromTimesCount) {
  PpuState ppu = screen();
  putSpriteBlock(ppu, 0u, 16u, 16u, 1u);
  putSpriteColour(ppu, 1u, 1u, kGreen);

  // Seventeen sprites of two tiles take Time's whole count, so the sprite Range
  // kept first has nothing left and does not draw.
  putSprite(ppu, 0u, 100, 20u, 0u, attributes(false, false, 3u, 1u, false), true);
  putSpriteRun(ppu, 1u, 17u, 0, 20u, true);
  EXPECT_EQ(after(ppu, pastTheDotsOf(21u)).ppu.sprites.found, 18u);
  EXPECT_EQ(draw(ppu).at(100u, 20u), kBackdropOut);

  // The last of them at the far side of nine bits draws nothing and takes its two
  // tiles all the same.
  putSprite(ppu, 17u, -256, 20u, 0u, attributes(false, false, 3u, 0u, false), true);
  EXPECT_EQ(after(ppu, pastTheDotsOf(21u)).ppu.sprites.found, 18u);
  EXPECT_EQ(draw(ppu).at(100u, 20u), kBackdropOut);

  // The same sprite parked off the left edge instead is out of Range, and the two
  // tiles it is not taking are the two the first sprite draws with.
  putSprite(ppu, 17u, -64, 20u, 0u, attributes(false, false, 3u, 0u, false), true);
  EXPECT_EQ(after(ppu, pastTheDotsOf(21u)).ppu.sprites.found, 17u);
  EXPECT_EQ(draw(ppu).at(100u, 20u), kGreenOut);
}

// ---- where the walk begins ---------------------------------------------------

TEST(SpritePriorityRotation, TheWalkBeginsAtTheSpriteThePortsAddressStandsIn) {
  PpuState ppu = screen();
  putSpriteTile(ppu, 0u, solid(1u));
  for (unsigned index = 0u; index < 4u; ++index) {
    putSprite(ppu, index, static_cast<int>(index) * 20, 20u, 0u,
              attributes(false, false, 3u, 0u, false), false);
  }

  // The reload value $104 stands the port at word $104, which is sprite 2's first
  // word. Without $2103 bit 7 the walk begins at sprite 0 regardless.
  ppu.oamadd = 0x0104u;
  ppu.oamAddress = 0x0208u;
  SnesState state = after(ppu, pastTheDotsOf(21u));
  ASSERT_EQ(state.ppu.sprites.found, 4u);
  EXPECT_EQ(state.ppu.sprites.first, 0u);
  EXPECT_EQ(state.ppu.sprites.inRange[0], 0u);
  EXPECT_EQ(state.ppu.sprites.inRange[3], 3u);

  // With it, the walk begins at sprite 2 and wraps past 127 to reach the two below
  // it — an order that reads differently from either end.
  ppu.oamadd = 0x8104u;
  state = after(ppu, pastTheDotsOf(21u));
  ASSERT_EQ(state.ppu.sprites.found, 4u);
  EXPECT_EQ(state.ppu.sprites.first, 2u);
  EXPECT_EQ(state.ppu.sprites.inRange[0], 2u);
  EXPECT_EQ(state.ppu.sprites.inRange[1], 3u);
  EXPECT_EQ(state.ppu.sprites.inRange[2], 0u);
  EXPECT_EQ(state.ppu.sprites.inRange[3], 1u);
}

TEST(SpritePriorityRotation, TheSpriteTheWalkBeginsAtIsInFrontOfEveryOther) {
  PpuState ppu = screen();
  putSpriteTile(ppu, 0u, solid(1u));
  putSpriteColour(ppu, 1u, 1u, kGreen);
  putSprite(ppu, 0u, 50, 20u, 0u, attributes(false, false, 3u, 0u, false), false);
  putSprite(ppu, 2u, 50, 20u, 0u, attributes(false, false, 3u, 1u, false), false);

  ppu.oamadd = 0x0104u;
  ppu.oamAddress = 0x0208u;
  EXPECT_EQ(draw(ppu).at(50u, 20u), kRedOut);

  ppu.oamadd = 0x8104u;
  EXPECT_EQ(draw(ppu).at(50u, 20u), kGreenOut);
}

TEST(SpritePriorityRotation, ThePortParkedOnARecordsLastByteAddsTheLineToIt) {
  PpuState ppu = screen();
  putSpriteTile(ppu, 0u, solid(1u));
  putSpriteColour(ppu, 1u, 1u, kGreen);
  putSpriteColour(ppu, 2u, 1u, kBlue);

  // Every sprite at Y = 63 and at the same position, each one hiding the ones the
  // walk reaches after it. Two of them are told apart by their palettes.
  putSpriteRun(ppu, 0u, kSprites, 0, 63u, false);
  putSprite(ppu, 63u, 0, 63u, 0u, attributes(false, false, 3u, 1u, false), false);
  putSprite(ppu, 64u, 0, 63u, 0u, attributes(false, false, 3u, 2u, false), false);

  // The port at byte 3 is standing on the last byte of sprite 0's record, so the
  // walk begins at sprite 0 plus the line it is matching: sprite 63 leads the pass
  // for picture line 64, sprite 64 the pass for line 65.
  ppu.oamadd = 0x8000u;
  ppu.oamAddress = 0x0003u;
  const Picture odd = draw(ppu);
  EXPECT_EQ(odd.at(0u, 63u), kGreenOut);
  EXPECT_EQ(odd.at(0u, 64u), kBlueOut);

  // One byte back it is standing inside the record instead, and the line is not
  // added: sprite 0 leads every line.
  ppu.oamAddress = 0x0002u;
  const Picture plain = draw(ppu);
  EXPECT_EQ(plain.at(0u, 63u), kRedOut);
  EXPECT_EQ(plain.at(0u, 64u), kRedOut);
}

// ---- the staged cartridges ---------------------------------------------------

// The cartridges under SNAGGLETOOTH_PPU_ROMS are run and nothing else: no source
// of theirs is opened and nothing from any of them is written down here. What one
// of them exercises is learned by running it, the way a commercial cartridge's
// register writes are. Unset, these cases register and skip with a reason.
//
//   cmake -B build -DSNAGGLETOOTH_PPU_ROMS=/path/to/ppu-testroms
//
// SNAGGLETOOTH_REQUIRE_PPU_ROMS in the environment turns a missing cartridge into
// a failure instead of a skip, for a machine that is meant to have them.
const char* romDirectory() { return SNAGGLETOOTH_PPU_ROMS; }

bool romsRequired() {
  const char* required = std::getenv("SNAGGLETOOTH_REQUIRE_PPU_ROMS");
  return required != nullptr && *required != '\0';
}

// The first file of that name anywhere under the directory, or nothing.
std::string findRom(const std::string& name) {
  const std::string root = romDirectory();
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

std::vector<std::uint8_t> readRom(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(file)),
                                   std::istreambuf_iterator<char>());
}

// What a cartridge's own run made of the chip's two sprite limits: the most
// sprites any line kept, and whether either flag was raised anywhere in it.
struct Crowding {
  unsigned mostKept = 0;
  bool rangeOver = false;
  bool timeOver = false;
};

Crowding crowd(const std::vector<std::uint8_t>& rom, unsigned lines) {
  Snes machine(SnesConfig{.rom = rom});
  Crowding seen;
  for (unsigned line = 0u; line < lines; ++line) {
    machine.run(kLineMaster);
    const PpuState& ppu = machine.state().ppu;
    if (static_cast<unsigned>(ppu.sprites.found) > seen.mostKept) {
      seen.mostKept = ppu.sprites.found;
    }
    if (ppu.rangeOver) seen.rangeOver = true;
    if (ppu.timeOver) seen.timeOver = true;
  }
  return seen;
}

// Loads a staged cartridge, or skips the calling case when there is none to load.
// `ran` reports whether `rom` holds one, so a caller stops rather than asserting
// on a cartridge it never read.
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
  rom = readRom(path);
  if (rom.size() < 0x8000u) {
    ADD_FAILURE() << name << " is too small to be a cartridge image";
    return;
  }
  ran = true;
}

// Three seconds of run at sixty frames of 262 lines.
constexpr unsigned kThreeSeconds = 3u * 60u * 262u;

TEST(PpuCartridges, ACartridgeThatCrowdsALineIsHeldToWhatRangeCanKeep) {
  std::vector<std::uint8_t> rom;
  bool ran = false;
  load("gradient-test.sfc", rom, ran);
  if (!ran) return;

  // This one puts every sprite OAM can describe across one line, which is four
  // times what the chip keeps — so it says both halves of the rule: no line holds
  // more than the count, and the flag a program can read is raised.
  const Crowding seen = crowd(rom, kThreeSeconds);
  EXPECT_LE(seen.mostKept, 32u);
  EXPECT_TRUE(seen.rangeOver);
}

TEST(PpuCartridges, ACartridgeThatCrowdsNothingRaisesNeitherFlag) {
  std::vector<std::uint8_t> rom;
  bool ran = false;
  load("twoship.sfc", rom, ran);
  if (!ran) return;

  // The passes run on every cartridge; the flags belong to the ones that ask for
  // more than the chip has.
  const Crowding seen = crowd(rom, kThreeSeconds);
  EXPECT_LE(seen.mostKept, 32u);
  EXPECT_FALSE(seen.rangeOver);
  EXPECT_FALSE(seen.timeOver);
}

}  // namespace
}  // namespace snaggletooth
