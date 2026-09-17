// Mode 7: the field of packed pixels the chip reads through its matrix, the
// three ways it treats a position outside that field, the two flips, the
// second layer EXTBG makes of the same pixels, and what the multiplier's ports
// answer while the field is being drawn.
//
// Every expectation is computed by hand from the register page and fullsnes's
// formula; the field is placed as a program would have left it. The last group
// runs the staged cartridges as images and asserts what their own runs measure.

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
constexpr std::uint16_t kYellow = 0x03FFu;
constexpr std::uint16_t kCyan = 0x7FE0u;
constexpr std::uint16_t kMagenta = 0x7C1Fu;
constexpr std::uint16_t kWhite = 0x7FFFu;
constexpr std::uint16_t kOrange = 0x01FFu;

// The ruler: a field whose every pixel is 1 + its column within the character,
// so a picture pixel's colour names the field column it was read from, mod 8.
constexpr std::array<std::uint16_t, 8> kRuler{kRed, kGreen, kBlue, kYellow,
                                             kCyan, kMagenta, kWhite, kOrange};
constexpr Rgba rulerAt(unsigned fieldColumn) { return out(kRuler[fieldColumn & 7u]); }

// A sprite's characters begin at byte $C000 and its palettes at word 128.
constexpr std::uint8_t kSpriteBase = 0x03u;
constexpr std::uint32_t kSpriteChars = 0xC000u;
constexpr unsigned kSpritePaletteBase = 128u;
constexpr std::uint16_t kSpriteColour = kWhite;

constexpr std::uint8_t kStp = 0xDBu;
constexpr std::uint8_t kLdaImm = 0xA9u;
constexpr std::uint8_t kLdaAbs = 0xADu;
constexpr std::uint8_t kStaAbs = 0x8Du;

// More than an NTSC frame of master cycles, so a run from power-on reaches the line
// a finished frame is handed over on.
constexpr std::uint64_t kOneFrame = 357364u + 20000u;
constexpr std::uint64_t kLineMaster = 1364u;

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

std::vector<std::uint8_t> haltedCartridge() { return cartridge({kStp}); }

// A frame drawn from a placed PPU state by a machine whose program halts at once.
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

void putColour(PpuState& ppu, unsigned word, std::uint16_t colour) {
  ppu.cgram[word * 2u] = static_cast<std::uint8_t>(colour & 0xFFu);
  ppu.cgram[word * 2u + 1u] = static_cast<std::uint8_t>(colour >> 8);
}

// The field's map: the low byte of word (tileY << 7 | tileX).
void putFieldEntry(PpuState& ppu, unsigned tileX, unsigned tileY, std::uint8_t character) {
  ppu.vram[((tileY << 7) | tileX) << 1] = character;
}

// A character's pixel: the high byte of word (n << 6 | row << 3 | column).
void putFieldPixel(PpuState& ppu, unsigned character, unsigned column, unsigned row,
                   std::uint8_t value) {
  ppu.vram[(((character << 6) | (row << 3) | column) << 1) | 1u] = value;
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

// A sprite of sixteen-colour pixels, all colour 1, covering the eight pixels from
// x on line 41: a sprite whose Y is N first draws on line N + 1.
void spriteAt(PpuState& ppu, unsigned index, int x, unsigned priority) {
  putSprite(ppu, index, x, 40u, 0u, static_cast<std::uint8_t>(priority << 4));
  for (unsigned row = 0u; row < 8u; ++row) {
    ppu.vram[(kSpriteChars + row * 2u) & 0xFFFFu] = 0xFFu;  // plane 0 set: colour 1 everywhere
  }
  putColour(ppu, kSpritePaletteBase + 1u, kSpriteColour);
}
constexpr unsigned kSpriteLine = 41u;

// Mode 7 at full brightness, the identity matrix, no scroll, the centre at the
// origin, the field wrapping, BG1 alone on the main screen, and the ruler as
// character 1 in every tile of the map.
PpuState modeSeven() {
  PpuState ppu;
  ppu.inidisp = 0x0Fu;
  ppu.objsel = kSpriteBase;
  ppu.bgmode = 0x07u;
  ppu.tm = 0x01u;
  ppu.m7a = 0x0100u;
  ppu.m7b = 0x0000u;
  ppu.m7c = 0x0000u;
  ppu.m7d = 0x0100u;
  ppu.m7bByte = 0x00u;
  putColour(ppu, 0u, kBackdrop);
  for (unsigned n = 0u; n < kRuler.size(); ++n) putColour(ppu, 1u + n, kRuler[n]);
  for (unsigned p = 0u; p < 64u; ++p) {
    putFieldPixel(ppu, 1u, p & 7u, p >> 3, static_cast<std::uint8_t>(1u + (p & 7u)));
  }
  for (unsigned tile = 0u; tile < 128u * 128u; ++tile) {
    putFieldEntry(ppu, tile & 127u, tile >> 7, 1u);
  }
  parkSprites(ppu);
  return ppu;
}

// One field pixel given its own value: the tile it lies in is pointed at a fresh
// character copied from the ruler with that one pixel changed, so the rest of
// the field reads as it did. Characters 16 and up are free for this.
struct Marked {
  PpuState* ppu;
  unsigned next = 16u;
  std::array<std::uint8_t, 128u * 128u> characterOf{};

  void mark(unsigned fieldX, unsigned fieldY, std::uint8_t value) {
    const unsigned tileX = fieldX >> 3;
    const unsigned tileY = fieldY >> 3;
    const std::size_t tile = (static_cast<std::size_t>(tileY) << 7) | tileX;
    if (characterOf[tile] == 0u) {
      const unsigned fresh = next++;
      for (unsigned p = 0u; p < 64u; ++p) {
        putFieldPixel(*ppu, fresh, p & 7u, p >> 3, ppu->vram[(((1u << 6) | p) << 1) | 1u]);
      }
      putFieldEntry(*ppu, tileX, tileY, static_cast<std::uint8_t>(fresh));
      characterOf[tile] = static_cast<std::uint8_t>(fresh);
    }
    putFieldPixel(*ppu, characterOf[tile], fieldX & 7u, fieldY & 7u, value);
  }
};

// LDA #v ; STA $21xx   and   LDA $21xx ; STA $00nn
std::vector<std::uint8_t> store(std::uint8_t low, std::uint8_t value) {
  return {kLdaImm, value, kStaAbs, low, 0x21u};
}
std::vector<std::uint8_t> load(std::uint8_t low, std::uint8_t wram) {
  return {kLdaAbs, low, 0x21u, kStaAbs, wram, 0x00u};
}

// ---- the layout ---------------------------------------------------------------

TEST(SnesPpuMode7, TheIdentityShowsFieldColumnXOnPictureColumnXAndRowLineOnLine) {
  PpuState ppu = modeSeven();
  Marked field{&ppu};
  field.mark(10u, 1u, 20u);
  putColour(ppu, 20u, kOrange);
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.frames, 1u);
  EXPECT_EQ(picture.at(10u, 1u), out(kOrange));
  EXPECT_EQ(picture.at(9u, 1u), rulerAt(9u));
  EXPECT_EQ(picture.at(11u, 1u), rulerAt(11u));
  EXPECT_EQ(picture.at(10u, 2u), rulerAt(10u));
}

TEST(SnesPpuMode7, FieldRowZeroIsNeverShown) {
  // Line 1 is the first drawn and reads row 1, so a mark on row 0 appears nowhere.
  PpuState ppu = modeSeven();
  Marked field{&ppu};
  field.mark(10u, 0u, 20u);
  putColour(ppu, 20u, kOrange);
  const Picture picture = draw(ppu);
  for (unsigned line = 1u; line <= 224u; ++line) {
    ASSERT_NE(picture.at(10u, line), out(kOrange)) << "line " << line;
  }
}

TEST(SnesPpuMode7, TheMapEntryIsTheLowByteOfTheWordItsTileNames) {
  // Tile (3, 2) pointed at a solid character 2: field columns 24-31 on rows 16-23.
  PpuState ppu = modeSeven();
  for (unsigned p = 0u; p < 64u; ++p) putFieldPixel(ppu, 2u, p & 7u, p >> 3, 20u);
  putColour(ppu, 20u, kOrange);
  putFieldEntry(ppu, 3u, 2u, 2u);
  EXPECT_EQ(ppu.vram[((2u << 7) | 3u) << 1], 2u);
  const Picture picture = draw(ppu);
  EXPECT_EQ(picture.at(24u, 16u), out(kOrange));
  EXPECT_EQ(picture.at(31u, 23u), out(kOrange));
  EXPECT_EQ(picture.at(23u, 16u), rulerAt(23u));
  EXPECT_EQ(picture.at(24u, 15u), rulerAt(24u));
  EXPECT_EQ(picture.at(32u, 16u), rulerAt(32u));
}

TEST(SnesPpuMode7, TheCharactersPixelIsTheHighByteAtItsRowAndColumn) {
  // Character 1's pixel (3, 2) is byte $A7: changing it moves field (3, 2) and,
  // the ruler naming character 1 everywhere, every eighth column of every eighth row.
  PpuState ppu = modeSeven();
  ppu.vram[0xA7u] = 20u;
  putColour(ppu, 20u, kOrange);
  const Picture picture = draw(ppu);
  EXPECT_EQ(picture.at(3u, 2u), out(kOrange));
  EXPECT_EQ(picture.at(11u, 10u), out(kOrange));
  EXPECT_EQ(picture.at(4u, 2u), rulerAt(4u));
  EXPECT_EQ(picture.at(3u, 3u), rulerAt(3u));
}

TEST(SnesPpuMode7, TilesAreEightByEightWhateverTheSizeBitsHold) {
  PpuState ppu = modeSeven();
  const std::uint64_t plain = digest(draw(ppu));
  ppu.bgmode = 0xF7u;  // every size bit set
  EXPECT_EQ(digest(draw(ppu)), plain);
}

TEST(SnesPpuMode7, TheLayerRegistersAreNotRead) {
  PpuState ppu = modeSeven();
  const std::uint64_t plain = digest(draw(ppu));
  ppu.bg1sc = 0x7Cu;
  ppu.bg12nba = 0xFFu;
  ppu.bg34nba = 0xFFu;
  ppu.bg1hofs = 0x03FFu;  // BG1's own offsets, which are not Mode 7's
  ppu.bg1vofs = 0x03FFu;
  EXPECT_EQ(digest(draw(ppu)), plain);
}

// ---- the transform ---------------------------------------------------------------

TEST(SnesPpuMode7, TheHorizontalScrollMovesTheFieldLeft) {
  PpuState ppu = modeSeven();
  ppu.m7hofs = 5u;
  Marked field{&ppu};
  field.mark(15u, 1u, 20u);
  putColour(ppu, 20u, kOrange);
  EXPECT_EQ(draw(ppu).at(10u, 1u), out(kOrange));
}

TEST(SnesPpuMode7, AVerticalScrollOfMinusOnePutsRowZeroOnTheFirstLine) {
  PpuState ppu = modeSeven();
  ppu.m7vofs = 0x1FFFu;  // -1 in thirteen bits
  Marked field{&ppu};
  field.mark(10u, 0u, 20u);
  putColour(ppu, 20u, kOrange);
  EXPECT_EQ(draw(ppu).at(10u, 1u), out(kOrange));
}

TEST(SnesPpuMode7, TheCentreWithNoRotationIsTheSameAsTheScroll) {
  PpuState scrolled = modeSeven();
  scrolled.m7hofs = 100u;
  Marked one{&scrolled};
  one.mark(110u, 1u, 20u);
  putColour(scrolled, 20u, kOrange);

  PpuState centred = scrolled;
  centred.m7hofs = 100u;  // the scroll and the centre cancel in the offset term...
  centred.m7x = 100u;     // ...and the centre is added back whole
  EXPECT_EQ(draw(scrolled).at(10u, 1u), out(kOrange));
  EXPECT_EQ(digest(draw(centred)), digest(draw(scrolled)));
}

TEST(SnesPpuMode7, AQuarterTurnAboutTheCentre) {
  // A = 0, B = 1, C = -1, D = 0 about (128, 128) with the scroll at 128: picture
  // (x, line) reads field (128 + line, 128 - x), so field (130, 120) is x 8, line 2.
  PpuState ppu = modeSeven();
  ppu.m7a = 0x0000u;
  ppu.m7b = 0x0100u;
  ppu.m7c = 0xFF00u;
  ppu.m7d = 0x0000u;
  ppu.m7x = 128u;
  ppu.m7y = 128u;
  ppu.m7hofs = 128u;
  ppu.m7vofs = 128u;
  Marked field{&ppu};
  field.mark(130u, 120u, 20u);
  putColour(ppu, 20u, kOrange);
  const Picture picture = draw(ppu);
  EXPECT_EQ(picture.at(8u, 2u), out(kOrange));
  // Columns of the field run down the picture now: the ruler's colour at x depends
  // on the line rather than on x.
  EXPECT_EQ(picture.at(0u, 3u), rulerAt(131u));
  EXPECT_EQ(picture.at(50u, 3u), rulerAt(131u));
}

TEST(SnesPpuMode7, ScaleTwoPicksEveryOtherPixel) {
  PpuState ppu = modeSeven();
  ppu.m7a = 0x0200u;
  ppu.m7d = 0x0200u;
  Marked field{&ppu};
  field.mark(20u, 2u, 20u);
  field.mark(21u, 2u, 21u);
  field.mark(22u, 2u, 22u);
  putColour(ppu, 20u, kOrange);
  putColour(ppu, 21u, kMagenta);
  putColour(ppu, 22u, kCyan);
  const Picture picture = draw(ppu);
  EXPECT_EQ(picture.at(10u, 1u), out(kOrange));  // column 20, row 2
  EXPECT_EQ(picture.at(11u, 1u), out(kCyan));    // column 22: 21 is skipped
}

TEST(SnesPpuMode7, ScaleHalfDoublesThem) {
  PpuState ppu = modeSeven();
  ppu.m7a = 0x0080u;
  ppu.m7d = 0x0080u;
  Marked field{&ppu};
  field.mark(5u, 1u, 20u);
  putColour(ppu, 20u, kOrange);
  const Picture picture = draw(ppu);
  EXPECT_EQ(picture.at(10u, 2u), out(kOrange));
  EXPECT_EQ(picture.at(11u, 2u), out(kOrange));
  EXPECT_EQ(picture.at(10u, 3u), out(kOrange));
  EXPECT_EQ(picture.at(11u, 3u), out(kOrange));
  EXPECT_EQ(picture.at(12u, 2u), rulerAt(6u));
}

TEST(SnesPpuMode7, TheOffsetProductIsTruncatedBeforeTheSum) {
  // A = $013F with the scroll at 1: the offset product 319 is truncated to 256,
  // and at x 4 the position is (256 + 1276) >> 8 = 5, not (319 + 1276) >> 8 = 6.
  // Provisional until the sweep cartridge is read: the one row anomie hedges.
  PpuState ppu = modeSeven();
  ppu.m7a = 0x013Fu;
  ppu.m7hofs = 1u;
  EXPECT_EQ(draw(ppu).at(4u, 1u), rulerAt(5u));
  EXPECT_NE(rulerAt(5u), rulerAt(6u));
}

TEST(SnesPpuMode7, AMatrixWrittenMidLineChangesTheRestOfTheLine) {
  // A program beginning at dot 75 of line 50 whose write of $02 to $211B lands
  // some twelve dots later: A becomes $0200 through the latch, and the line's
  // later columns read a field scaled by two while the columns drawn between the
  // run's start and the write were read as the identity. The dots before the run
  // began were drawn by nobody and are black.
  const Picture picture = drawWith(modeSeven(), store(0x1Bu, 0x02u), 50u, 300u);
  EXPECT_EQ(picture.at(56u, 50u), rulerAt(56u));
  EXPECT_EQ(picture.at(201u, 50u), rulerAt(402u));
  EXPECT_EQ(picture.at(201u, 51u), rulerAt(402u));
  EXPECT_EQ(picture.at(10u, 50u), (Rgba{0u, 0u, 0u, 255u}));
}

TEST(SnesPpuMode7, AMatrixWrittenBetweenLinesChangesTheLinesAfter) {
  // The same write in line 50's horizontal blank, reached by a run that began at
  // the line's start and spent seventy-five NOPs getting there: line 50 is drawn
  // whole as the identity and line 51 is the first scaled one, which is how a
  // program draws a floor by transfer.
  std::vector<std::uint8_t> program(75u, 0xEAu);
  const std::vector<std::uint8_t> write = store(0x1Bu, 0x02u);
  program.insert(program.end(), write.begin(), write.end());
  const Picture picture = drawWith(modeSeven(), program, 50u, 0u);
  EXPECT_EQ(picture.at(201u, 50u), rulerAt(201u));
  EXPECT_EQ(picture.at(255u, 50u), rulerAt(255u));
  EXPECT_EQ(picture.at(201u, 51u), rulerAt(402u));
}

// ---- the clip ---------------------------------------------------------------------

TEST(SnesPpuMode7, AnOffsetOfAThousandAndTwentyFourClipsToZero) {
  PpuState ppu = modeSeven();
  ppu.m7sel = 0x80u;  // outside the field is transparent, so an unclipped 1024 shows nothing
  ppu.m7hofs = 0x0400u;
  EXPECT_EQ(draw(ppu).at(10u, 1u), rulerAt(10u));
}

TEST(SnesPpuMode7, AnOffsetOfMinusAThousandAndTwentyFiveClipsToMinusOne) {
  PpuState ppu = modeSeven();
  ppu.m7sel = 0x80u;
  ppu.m7hofs = 0x1BFFu;
  const Picture picture = draw(ppu);
  EXPECT_EQ(picture.at(0u, 1u), out(kBackdrop));  // column -1 is outside
  EXPECT_EQ(picture.at(10u, 1u), rulerAt(9u));
}

TEST(SnesPpuMode7, ANegativeDifferenceKeepsItsSignAboveTenBits) {
  // Scroll 0 and centre 2048: the difference is -2048, whose low ten bits are
  // zero and whose sign fills the rest, so it clips to -1024 and the position is
  // 1024 + x — outside the field. Unclipped it would be -2048 and the field would
  // sit exactly where the identity puts it.
  PpuState ppu = modeSeven();
  ppu.m7sel = 0x80u;
  ppu.m7x = 0x0800u;
  EXPECT_EQ(draw(ppu).at(10u, 1u), out(kBackdrop));
  ppu.m7sel = 0x00u;  // wrapping, 1024 + x is x again
  EXPECT_EQ(draw(ppu).at(10u, 1u), rulerAt(10u));
}

// ---- the flips and screen-over ------------------------------------------------------

TEST(SnesPpuMode7, TheHorizontalFlipMirrorsTheColumns) {
  PpuState ppu = modeSeven();
  ppu.m7sel = 0x01u;
  const Picture picture = draw(ppu);
  EXPECT_EQ(picture.at(0u, 1u), rulerAt(255u));
  EXPECT_EQ(picture.at(10u, 1u), rulerAt(245u));
}

TEST(SnesPpuMode7, TheVerticalFlipPutsRowTwoHundredAndFiftyFourOnTheFirstLine) {
  // Line L reads row L XOR 255: line 1 reads 254 and row 255 is never shown.
  // Provisional until the sweep cartridge is read: fullsnes's formula alone says so.
  PpuState ppu = modeSeven();
  ppu.m7sel = 0x02u;
  Marked field{&ppu};
  field.mark(10u, 254u, 20u);
  field.mark(10u, 255u, 21u);
  putColour(ppu, 20u, kOrange);
  putColour(ppu, 21u, kMagenta);
  const Picture picture = draw(ppu);
  EXPECT_EQ(picture.at(10u, 1u), out(kOrange));
  for (unsigned line = 1u; line <= 224u; ++line) {
    ASSERT_NE(picture.at(10u, line), out(kMagenta)) << "line " << line;
  }
}

TEST(SnesPpuMode7, ScreenOverZeroWrapsAtAThousandAndTwentyFour) {
  PpuState ppu = modeSeven();
  ppu.m7hofs = 1020u;
  Marked field{&ppu};
  field.mark(6u, 1u, 20u);  // column 1030 wraps to 6
  putColour(ppu, 20u, kOrange);
  EXPECT_EQ(draw(ppu).at(10u, 1u), out(kOrange));
  // And a column between 512 and 1023 is its own, not wrapped early: 610 is 610.
  PpuState wide = modeSeven();
  wide.m7hofs = 600u;
  Marked far{&wide};
  far.mark(610u, 1u, 20u);
  putColour(wide, 20u, kOrange);
  EXPECT_EQ(draw(wide).at(10u, 1u), out(kOrange));
}

TEST(SnesPpuMode7, ScreenOverOneIsTheSameAsZero) {
  PpuState ppu = modeSeven();
  ppu.m7hofs = 1020u;
  Marked field{&ppu};
  field.mark(6u, 1u, 20u);
  putColour(ppu, 20u, kOrange);
  const std::uint64_t wrapped = digest(draw(ppu));
  ppu.m7sel = 0x40u;
  EXPECT_EQ(digest(draw(ppu)), wrapped);
}

TEST(SnesPpuMode7, ScreenOverTwoShowsTheBackdropOutside) {
  PpuState ppu = modeSeven();
  ppu.m7sel = 0x80u;
  ppu.m7hofs = 1020u;
  putFieldPixel(ppu, 0u, 6u, 1u, 20u);  // character 0 has a pixel, and it is not drawn here
  putColour(ppu, 20u, kOrange);
  const Picture picture = draw(ppu);
  EXPECT_EQ(picture.at(3u, 1u), rulerAt(1023u));   // the field's last column
  EXPECT_EQ(picture.at(4u, 1u), out(kBackdrop));   // and nothing past it
  EXPECT_EQ(picture.at(10u, 1u), out(kBackdrop));
}

// Screen-over 3 with character 0 carrying one pixel, (6, 1), so the fill outside
// the field shows that pixel wherever the transformed position's low three bits
// name it and nothing elsewhere.
PpuState filledOutside() {
  PpuState ppu = modeSeven();
  ppu.m7sel = 0xC0u;
  ppu.m7hofs = 1020u;
  putFieldPixel(ppu, 0u, 6u, 1u, 20u);
  putColour(ppu, 20u, kOrange);
  return ppu;
}

TEST(SnesPpuMode7, ScreenOverThreeShowsCharacterZeroOutsideTransformed) {
  const Picture picture = draw(filledOutside());
  EXPECT_EQ(picture.at(10u, 1u), out(kOrange));    // column 1030: bits 6, row 1
  EXPECT_EQ(picture.at(18u, 1u), out(kOrange));    // column 1038: bits 6 again
  EXPECT_EQ(picture.at(10u, 9u), out(kOrange));    // row 9: bits 1 again
  EXPECT_EQ(picture.at(3u, 1u), rulerAt(1023u));   // inside the field, the field
}

TEST(SnesPpuMode7, AZeroPixelInTheFillIsTransparent) {
  const Picture picture = draw(filledOutside());
  EXPECT_EQ(picture.at(11u, 1u), out(kBackdrop));  // column 1031: bits 7, which is zero
  EXPECT_EQ(picture.at(10u, 2u), out(kBackdrop));  // row 2, which is zero
}

// ---- the colour -----------------------------------------------------------------

TEST(SnesPpuMode7, ThePixelByteIsThePaletteWord) {
  PpuState ppu = modeSeven();
  Marked field{&ppu};
  field.mark(10u, 1u, 200u);
  putColour(ppu, 200u, kCyan);
  EXPECT_EQ(draw(ppu).at(10u, 1u), out(kCyan));
}

TEST(SnesPpuMode7, AZeroPixelIsTransparent) {
  // Word 0 is the backdrop's own colour, so the backdrop is made to take colour
  // math and the field not to: a transparent pixel shows the backdrop plus the
  // fixed colour, where a pixel drawn as word 0 would show the plain colour.
  PpuState ppu = modeSeven();
  ppu.cgadsub = 0x20u;
  ppu.fixedRed = 8u;
  Marked field{&ppu};
  field.mark(10u, 1u, 0u);
  EXPECT_EQ(draw(ppu).at(10u, 1u), rgb(9u, 2u, 3u));
  EXPECT_EQ(draw(ppu).at(11u, 1u), rulerAt(11u));
}

TEST(SnesPpuMode7, DirectColourReadsThePixelAsAColourWithNoTileBits) {
  // $FF is red 28, green 28, blue 24 — the tile bits that would lift each channel
  // do not exist in this map — and $01 is red 4.
  PpuState ppu = modeSeven();
  ppu.cgwsel = 0x01u;
  Marked field{&ppu};
  field.mark(10u, 1u, 0xFFu);
  field.mark(11u, 1u, 0x01u);
  const Picture picture = draw(ppu);
  EXPECT_EQ(picture.at(10u, 1u), rgb(28u, 28u, 24u));
  EXPECT_EQ(picture.at(11u, 1u), rgb(4u, 0u, 0u));
}

TEST(SnesPpuMode7, WithTheBitClearTheSamePixelReadsThePalette) {
  PpuState ppu = modeSeven();
  Marked field{&ppu};
  field.mark(10u, 1u, 0xFFu);
  putColour(ppu, 0xFFu, kMagenta);
  EXPECT_EQ(draw(ppu).at(10u, 1u), out(kMagenta));
}

TEST(SnesPpuMode7, TheFieldOnTheSubScreenIsTheAddend) {
  // Nothing on the main screen but the backdrop, which takes math; the field on
  // the sub screen, read as a colour: red 28 added to the backdrop's 1, 2, 3.
  PpuState ppu = modeSeven();
  ppu.tm = 0x00u;
  ppu.ts = 0x01u;
  ppu.cgwsel = 0x03u;   // direct colour, and the sub screen is the addend
  ppu.cgadsub = 0x20u;  // the backdrop takes math, add, no half
  Marked field{&ppu};
  field.mark(10u, 1u, 0x07u);
  EXPECT_EQ(draw(ppu).at(10u, 1u), rgb(29u, 2u, 3u));
}

// ---- the order -------------------------------------------------------------------
//
// Front to back: S3 S2 S1 BG1 S0.

PpuState fieldUnderSprite(unsigned priority) {
  PpuState ppu = modeSeven();
  ppu.tm = 0x11u;
  spriteAt(ppu, 0u, 0, priority);
  return ppu;
}

TEST(SnesPpuMode7, SpritesAtPrioritiesThreeTwoAndOneAreInFrontOfTheField) {
  EXPECT_EQ(draw(fieldUnderSprite(3u)).at(0u, kSpriteLine), out(kSpriteColour));
  EXPECT_EQ(draw(fieldUnderSprite(2u)).at(0u, kSpriteLine), out(kSpriteColour));
  EXPECT_EQ(draw(fieldUnderSprite(1u)).at(0u, kSpriteLine), out(kSpriteColour));
}

TEST(SnesPpuMode7, TheFieldIsInFrontOfTheSpriteAtPriorityZero) {
  EXPECT_EQ(draw(fieldUnderSprite(0u)).at(0u, kSpriteLine), rulerAt(0u));
}

TEST(SnesPpuMode7, WhereTheFieldIsTransparentTheSpriteAtPriorityZeroShows) {
  PpuState ppu = fieldUnderSprite(0u);
  Marked field{&ppu};
  field.mark(0u, kSpriteLine, 0u);
  EXPECT_EQ(draw(ppu).at(0u, kSpriteLine), out(kSpriteColour));
  EXPECT_EQ(draw(ppu).at(1u, kSpriteLine), rulerAt(1u));
}

TEST(SnesPpuMode7, BitThreeOfTheModeRegisterMovesNothing) {
  PpuState ppu = fieldUnderSprite(0u);
  spriteAt(ppu, 1u, 16, 1u);
  const std::uint64_t plain = digest(draw(ppu));
  ppu.bgmode = 0x0Fu;
  EXPECT_EQ(digest(draw(ppu)), plain);
}

TEST(SnesPpuMode7, TheSecondLayerBitShowsNothingWithoutExtbg) {
  PpuState ppu = modeSeven();
  ppu.tm = 0x02u;
  const Picture picture = draw(ppu);
  EXPECT_EQ(picture.at(10u, 1u), out(kBackdrop));
  EXPECT_EQ(picture.at(100u, 100u), out(kBackdrop));
}

// ---- EXTBG --------------------------------------------------------------------------
//
// SETINI bit 6 gives Mode 7 a BG2 that is the same pixel with its bit 7 as
// priority and its low seven bits as the word. Front to back: S3 S2 2H S1 BG1 S0 2L.

PpuState extended() {
  PpuState ppu = modeSeven();
  ppu.setini = 0x40u;
  ppu.tm = 0x02u;
  return ppu;
}

TEST(SnesPpuMode7, ExtbgShowsASecondLayerFromTheSameField) {
  PpuState ppu = extended();
  Marked field{&ppu};
  field.mark(10u, 1u, 5u);
  const Picture picture = draw(ppu);
  EXPECT_EQ(picture.at(10u, 1u), out(kRuler[4]));  // word 5
  EXPECT_EQ(picture.at(11u, 1u), rulerAt(11u));
}

TEST(SnesPpuMode7, ThePixelsTopBitIsItsPriorityAndTheRestItsWord) {
  // $85 on BG2 is word 5, not word 133.
  PpuState ppu = extended();
  Marked field{&ppu};
  field.mark(10u, 1u, 0x85u);
  putColour(ppu, 0x85u, kMagenta);
  EXPECT_EQ(draw(ppu).at(10u, 1u), out(kRuler[4]));
}

TEST(SnesPpuMode7, ExtbgPixelZeroIsTransparentWhateverBitSevenHolds) {
  PpuState ppu = extended();
  Marked field{&ppu};
  field.mark(10u, 1u, 0x80u);
  putColour(ppu, 0x80u, kMagenta);
  EXPECT_EQ(draw(ppu).at(10u, 1u), out(kBackdrop));
}

// EXTBG's BG2 alone with a sprite over its first eight pixels on line 41, the
// field's pixel there given `value`.
PpuState secondLayerUnderSprite(std::uint8_t value, unsigned priority, bool withFirst) {
  PpuState ppu = extended();
  ppu.tm = withFirst ? 0x13u : 0x12u;
  Marked field{&ppu};
  field.mark(0u, kSpriteLine, value);
  spriteAt(ppu, 0u, 0, priority);
  return ppu;
}

TEST(SnesPpuMode7, ExtbgHighPixelsSitBehindSpritesAtPrioritiesThreeAndTwo) {
  EXPECT_EQ(draw(secondLayerUnderSprite(0x85u, 3u, false)).at(0u, kSpriteLine), out(kSpriteColour));
  EXPECT_EQ(draw(secondLayerUnderSprite(0x85u, 2u, false)).at(0u, kSpriteLine), out(kSpriteColour));
}

TEST(SnesPpuMode7, ExtbgHighPixelsBeatTheSpriteAtPriorityOneAndTheFirstLayer) {
  // BG1 shows the same pixel as word 133; BG2's high pixel is in front of it.
  PpuState ppu = secondLayerUnderSprite(0x85u, 1u, true);
  putColour(ppu, 0x85u, kMagenta);
  EXPECT_EQ(draw(ppu).at(0u, kSpriteLine), out(kRuler[4]));
}

TEST(SnesPpuMode7, ExtbgLowPixelsSitBehindTheSpriteAtPriorityOne) {
  EXPECT_EQ(draw(secondLayerUnderSprite(0x05u, 1u, false)).at(0u, kSpriteLine), out(kSpriteColour));
}

TEST(SnesPpuMode7, TheFirstLayerStandsInFrontOfExtbgsLowPixels) {
  // Both layers show word 5 for a low pixel, so which one is in front is told by
  // colour math: BG1 alone takes it, with red 8 as the addend.
  PpuState ppu = extended();
  ppu.tm = 0x03u;
  ppu.cgadsub = 0x01u;
  ppu.fixedRed = 8u;
  Marked field{&ppu};
  field.mark(10u, 1u, 0x05u);
  EXPECT_EQ(draw(ppu).at(10u, 1u), rgb(8u, 31u, 31u));  // word 5 is cyan, plus red 8
  ppu.tm = 0x02u;
  EXPECT_EQ(draw(ppu).at(10u, 1u), out(kCyan));         // BG2 alone takes no math
}

TEST(SnesPpuMode7, ExtbgLowPixelsAreTheLastPlace) {
  // With BG1 off, the sprite at priority 0 beats a low BG2 pixel...
  EXPECT_EQ(draw(secondLayerUnderSprite(0x05u, 0u, false)).at(0u, kSpriteLine), out(kSpriteColour));
  // ...and where the sprite is not, the low pixel shows over the backdrop.
  EXPECT_EQ(draw(secondLayerUnderSprite(0x05u, 0u, false)).at(8u, kSpriteLine), rulerAt(8u));
}

TEST(SnesPpuMode7, ExtbgNeverReadsThePixelAsAColour) {
  PpuState ppu = extended();
  ppu.cgwsel = 0x01u;
  Marked field{&ppu};
  field.mark(10u, 1u, 0x07u);
  EXPECT_EQ(draw(ppu).at(10u, 1u), out(kRuler[6]));  // word 7 through the palette
  ppu.tm = 0x01u;                                     // BG1 beside it, the same pixel
  EXPECT_EQ(draw(ppu).at(10u, 1u), rgb(28u, 0u, 0u));
}

TEST(SnesPpuMode7, ExtbgUsesTheModeSevenScrollAndNotItsOwn) {
  PpuState ppu = extended();
  Marked field{&ppu};
  field.mark(15u, 1u, 5u);
  ppu.bg2hofs = 5u;
  EXPECT_EQ(draw(ppu).at(15u, 1u), out(kRuler[4]));
  EXPECT_EQ(draw(ppu).at(10u, 1u), rulerAt(10u));
  ppu.m7hofs = 5u;
  EXPECT_EQ(draw(ppu).at(10u, 1u), out(kRuler[4]));
}

TEST(SnesPpuMode7, ExtbgIsMaskedLikeAnySecondLayer) {
  PpuState ppu = extended();
  ppu.w12sel = 0x20u;  // window 1 enabled for BG2
  ppu.wh0 = 0u;
  ppu.wh1 = 255u;
  ppu.tmw = 0x02u;
  EXPECT_EQ(draw(ppu).at(10u, 1u), out(kBackdrop));
  ppu.tmw = 0x00u;
  EXPECT_EQ(draw(ppu).at(10u, 1u), rulerAt(10u));
}

TEST(SnesPpuMode7, ExtbgInModeOneDrawsModeOneUnchanged) {
  // A Mode 1 picture with a sixteen-colour BG2: a solid character at tile (0, 0)
  // of a map at screen 2, drawn with the bit clear and then set.
  PpuState ppu;
  ppu.inidisp = 0x0Fu;
  ppu.objsel = kSpriteBase;
  ppu.bgmode = 0x01u;
  ppu.tm = 0x02u;
  ppu.bg2sc = 0x08u;     // screen 2: byte $1000
  ppu.bg12nba = 0x50u;   // BG2's characters at byte $A000
  ppu.bg2vofs = 0x3FFu;
  putColour(ppu, 0u, kBackdrop);
  putColour(ppu, 1u, kOrange);
  parkSprites(ppu);
  ppu.vram[0x1000u] = 0x01u;  // tile 1
  for (unsigned row = 0u; row < 8u; ++row) ppu.vram[0xA020u + row * 2u] = 0xFFu;  // plane 0
  const Picture plain = draw(ppu);
  ASSERT_EQ(plain.at(0u, 1u), out(kOrange));
  ppu.setini = 0x40u;
  EXPECT_EQ(digest(draw(ppu)), digest(plain));
}

TEST(SnesPpuMode7, ExtbgSwitchedPartWayAlongALineAddsTheSecondLayerToTheRestOfIt) {
  // The chart the chip resolves a dot by is the one SETINI names at that dot. Every
  // field pixel here holds $85, which the first layer shows as word $85 and the
  // second — once bit 6 is set — as word 5 in front of it. The program begins at
  // dot 75 of line 50, where its store pair lands at column 65: the positions of
  // that line before the landing are drawn by Mode 7's plain chart and the ones
  // after it by the chart EXTBG extends.
  PpuState before = modeSeven();
  before.tm = 0x03u;  // both layers on the main screen
  for (unsigned p = 0u; p < 64u; ++p) {
    putFieldPixel(before, 1u, p & 7u, p >> 3, 0x85u);
  }
  putColour(before, 0x85u, kMagenta);
  PpuState after = before;
  after.setini = 0x40u;
  const Picture never = draw(before);
  const Picture always = draw(after);
  const Picture picture = drawWith(before, store(0x33u, 0x40u), 50u, 300u);

  EXPECT_NE(never.at(60u, 50u), always.at(60u, 50u));
  EXPECT_NE(never.at(66u, 50u), always.at(66u, 50u));
  EXPECT_NE(never.at(80u, 50u), always.at(80u, 50u));
  EXPECT_EQ(picture.at(60u, 50u), never.at(60u, 50u));
  EXPECT_EQ(picture.at(66u, 50u), always.at(66u, 50u));
  EXPECT_EQ(picture.at(80u, 50u), always.at(80u, 50u));
}

// ---- the multiplier ---------------------------------------------------------------
//
// One byte of $2134-$2136 read by a program whose LDA resolves thirty master
// cycles after the beam is placed: three slow fetches and the fast access.

std::uint8_t readPort(PpuState ppu, std::uint8_t low, std::uint16_t line, std::uint16_t hpos) {
  std::vector<std::uint8_t> program = load(low, 0x50u);
  program.push_back(kStp);
  const std::vector<std::uint8_t> rom = cartridge(std::move(program));
  Snes m(SnesConfig{.rom = rom});
  SnesState s = m.state();
  s.ppu = ppu;
  s.vpos = line;
  s.hpos = hpos;
  s.inVblank = line >= s.ppu.vblankStartLine();
  if (s.inVblank) s.vblankBeginLine = s.ppu.vblankStartLine();
  m.restore(s);
  while (m.state().cpu.run == CpuRunState::Running) m.step();
  return m.state().wram[0x50];
}

std::uint32_t readProduct(const PpuState& ppu, std::uint16_t line, std::uint16_t hpos) {
  return static_cast<std::uint32_t>(readPort(ppu, 0x34u, line, hpos)) |
         (static_cast<std::uint32_t>(readPort(ppu, 0x35u, line, hpos)) << 8) |
         (static_cast<std::uint32_t>(readPort(ppu, 0x36u, line, hpos)) << 16);
}

// Mode 7 drawing with A = 1.0, B = 2.0 (so the plain product is $0100 x $02),
// C = -1.0, D = 1.0, the scroll at 5 and the centre at the origin.
PpuState multiplying() {
  PpuState ppu = modeSeven();
  ppu.m7b = 0x0200u;
  ppu.m7bByte = 0x02u;
  ppu.m7c = 0xFF00u;
  ppu.m7hofs = 5u;
  ppu.m7vofs = 3u;
  return ppu;
}
constexpr std::uint32_t kPlainProduct = 0x000200u;

// The beam placement whose read resolves at master cycle `at` of the same line.
constexpr std::uint16_t placedFor(std::uint16_t at) { return static_cast<std::uint16_t>(at - 30u); }

TEST(SnesPpuMode7Multiplier, InAnotherModeTheProductStandsDuringThePicture) {
  PpuState ppu = multiplying();
  ppu.bgmode = 0x01u;
  EXPECT_EQ(readProduct(ppu, 100u, placedFor(428u)), kPlainProduct);
}

TEST(SnesPpuMode7Multiplier, InVerticalBlankAndInForcedBlankTheProductStands) {
  EXPECT_EQ(readProduct(multiplying(), 230u, placedFor(428u)), kPlainProduct);
  PpuState blank = multiplying();
  blank.inidisp = 0x8Fu;
  EXPECT_EQ(readProduct(blank, 100u, placedFor(428u)), kPlainProduct);
}

TEST(SnesPpuMode7Multiplier, TheFirstHalfOfADotHoldsATimesTheColumn) {
  // Dot 107, whose column is 104: $0100 x 104 >> 3 = 3328 = $000D00.
  EXPECT_EQ(readProduct(multiplying(), 100u, placedFor(428u)), 0x000D00u);
}

TEST(SnesPpuMode7Multiplier, TheSecondHalfHoldsCTimesTheColumn) {
  // -$0100 x 104 >> 3 = -3328 = $FFF300.
  EXPECT_EQ(readProduct(multiplying(), 100u, placedFor(430u)), 0xFFF300u);
}

TEST(SnesPpuMode7Multiplier, TheHorizontalFlipInvertsTheColumn) {
  // 104 XOR 255 = 151: $0100 x 151 >> 3 = 4832 = $0012E0.
  PpuState ppu = multiplying();
  ppu.m7sel = 0x01u;
  EXPECT_EQ(readProduct(ppu, 100u, placedFor(428u)), 0x0012E0u);
}

TEST(SnesPpuMode7Multiplier, TheLinesFirstDotsHoldTheOffsetAndLineProducts) {
  // Placed on the line before so the read resolves as the next line begins: dot 0's
  // first half is A x (scroll - centre) = $0100 x 5 >> 3 = 160; dot 2's second half
  // is D x the line = $0100 x 100 >> 3 = 3200 = $000C80.
  EXPECT_EQ(readProduct(multiplying(), 99u, static_cast<std::uint16_t>(kLineMaster - 30u)),
            0x0000A0u);
  EXPECT_EQ(readProduct(multiplying(), 99u, static_cast<std::uint16_t>(kLineMaster + 10u - 30u)),
            0x000C80u);
}

TEST(SnesPpuMode7Multiplier, HorizontalBlankOfAPictureLineFollowsTheScheduleToo) {
  // Dot 300, column (300 - 3) & 255 = 41: $0100 x 41 >> 3 = 1312 = $000520.
  EXPECT_EQ(readProduct(multiplying(), 100u, placedFor(1200u)), 0x000520u);
}

// ---- the staged cartridges ---------------------------------------------------
//
// The cartridges under SNAGGLETOOTH_PPU_ROMS are run and nothing else: no source
// of theirs is opened and nothing from any of them is written down here. Unset,
// these cases register and skip with a reason.

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
  for (const std::filesystem::directory_entry& entry : walk) {
    if (entry.is_regular_file(failed) && entry.path().filename() == name) {
      return entry.path().string();
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

// What three seconds of a cartridge's own run leave: the machine as it stood, the
// last frame, how many distinct colours it held, whether the matrix moved between
// frames, and whether a transfer channel delivered to the matrix.
struct Played {
  SnesState state;
  std::size_t colours = 0;
  unsigned lit = 0;
  bool matrixMoved = false;
  bool matrixByTransfer = false;
};

struct LastFrame final : FrameObserver {
  const Snes* machine = nullptr;
  std::vector<std::uint8_t> pixels;
  std::array<std::uint16_t, 4> matrix{};
  bool matrixMoved = false;
  bool matrixByTransfer = false;
  unsigned frames = 0;
  void frame(const VideoFrame& picture) override {
    ++frames;
    pixels.assign(picture.pixels.begin(), picture.pixels.end());
    if (machine == nullptr) return;
    const PpuState& p = machine->state().ppu;
    const std::array<std::uint16_t, 4> now{p.m7a, p.m7b, p.m7c, p.m7d};
    if (frames > 60u && now != matrix) matrixMoved = true;
    matrix = now;
    // A channel HDMA is enabled on whose register is one of the matrix's four.
    for (unsigned index = 0u; index < machine->state().dma.size(); ++index) {
      const DmaChannel& channel = machine->state().dma[index];
      if (((machine->state().hdmaen >> index) & 1u) != 0u && channel.bbad >= 0x1Bu &&
          channel.bbad <= 0x1Eu) {
        matrixByTransfer = true;
      }
    }
  }
};

Played play(const std::vector<std::uint8_t>& rom) {
  Snes machine(SnesConfig{.rom = rom});
  LastFrame watch;
  watch.machine = &machine;
  machine.setFrameObserver(&watch);
  machine.run(3ull * 60ull * 357364ull);
  Played ran;
  ran.state = machine.state();
  ran.matrixMoved = watch.matrixMoved;
  ran.matrixByTransfer = watch.matrixByTransfer;
  std::set<std::uint32_t> colours;
  for (std::size_t at = 0u; at + 3u < watch.pixels.size(); at += 4u) {
    const std::uint32_t colour =
        watch.pixels[at] | (watch.pixels[at + 1u] << 8) | (watch.pixels[at + 2u] << 16);
    colours.insert(colour);
    if (colour != 0u) ++ran.lit;
  }
  ran.colours = colours.size();
  return ran;
}

void modeSevenCartridge(const std::string& name, bool expectMoving, bool expectTransfer) {
  std::vector<std::uint8_t> rom;
  bool ran = false;
  loadRom(name, rom, ran);
  if (!ran) return;
  const Played played = play(rom);
  EXPECT_EQ(played.state.ppu.bgmode & 0x07u, 0x07u) << name;
  EXPECT_NE(played.state.ppu.tm & 0x01u, 0u) << name << " does not show BG1";
  EXPECT_NE(played.state.ppu.m7a, 0xFFFFu) << name << " left the matrix at power-on";
  EXPECT_GT(played.lit, 1000u) << name << " drew nothing";
  EXPECT_GT(played.colours, 4u) << name;  // a field of several colours, not one flat one
  if (expectMoving) {
    EXPECT_TRUE(played.matrixMoved) << name << "'s matrix never changed";
  }
  if (expectTransfer) {
    EXPECT_TRUE(played.matrixByTransfer) << name << " never transferred to the matrix";
  }
}

// What each cartridge does was learned by running it: which mode it settles in,
// whether its matrix changes between frames with no pad pressed, and whether a
// transfer channel names a matrix register.

TEST(PpuMode7Cartridges, ARotatingAndZoomingCartridgeDrawsItsField) {
  // Its matrix moves only under the pad, which nothing here presses.
  modeSevenCartridge("RotZoom.sfc", false, false);
}

TEST(PpuMode7Cartridges, APerspectiveCartridgeDeliversItsMatrixByTransfer) {
  modeSevenCartridge("Perspective.sfc", false, true);
}

TEST(PpuMode7Cartridges, AScrollingTextCartridgeMovesItsMatrixEveryFrame) {
  modeSevenCartridge("StarWars.sfc", true, false);
}

TEST(PpuMode7Cartridges, ATransferDrivenCartridgeDeliversItsMatrixByTransfer) {
  modeSevenCartridge("Mode7HDMA.sfc", false, true);
}

}  // namespace
}  // namespace snaggletooth
