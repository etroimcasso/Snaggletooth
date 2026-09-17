// The windows the PPU masks its layers with: the two horizontal spans and the
// rules their edges keep, the enable and inversion bits every layer holds for each
// window, the four ways a layer combines the two, and the register that decides
// which layers the combination masks on the main screen — with the backdrop under
// them all, which no window reaches.
//
// A window is read at the dot it shapes, so an edge written part-way across a line
// changes the rest of that line and nothing before it, and an edge a transfer
// delivers in the blank between two lines shapes the whole of the line that
// follows. Both are cases here.
//
// Every expectation is computed by hand from the register contract; the pictures
// are placed as a program would have left them.

#include <array>
#include <cstdint>
#include <initializer_list>
#include <vector>

#include "gtest/gtest.h"
#include "snaggletooth/snes/snes.h"

namespace snaggletooth {
namespace {

// Where these pictures keep their pieces, each clear of the others. A map base
// counts whole 32x32 screens of $400 words, so one screen of map is $800 bytes; a
// background's character base counts 8 KB blocks; and a sprite's counts 8 K-word
// blocks, which is 16 KB.
constexpr std::uint8_t kBg1MapBase = 0x04u;     // $2107: screen 1, size 00
constexpr std::uint8_t kBg2MapBase = 0x08u;     // $2108: screen 2
constexpr std::uint8_t kBg3MapBase = 0x0Cu;     // $2109: screen 3
constexpr std::uint8_t kBgCharBases12 = 0x54u;  // $210B: BG1 at $8000, BG2 at $A000
constexpr std::uint8_t kBg3CharBase = 0x03u;    // $210C: BG3 at $6000

constexpr std::uint32_t kBg1MapByte = 0x0800u;
constexpr std::uint32_t kBg2MapByte = 0x1000u;
constexpr std::uint32_t kBg3MapByte = 0x1800u;
constexpr std::uint32_t kBg1CharByte = 0x8000u;
constexpr std::uint32_t kBg2CharByte = 0xA000u;
constexpr std::uint32_t kBg3CharByte = 0x6000u;

// The sprite character base these cases use: $2101 bits 2-0 hold 3, so the first
// table is at word $6000 and byte $C000.
constexpr std::uint8_t kSpriteBase = 0x03u;
constexpr std::uint32_t kSpriteCharByte = 0xC000u;

// A sprite's character is always sixteen colours: four bitplanes.
constexpr unsigned kSpritePlanes = 4u;

// The first palette word a sprite can name: its eight palettes are sixteen colours
// each from CGRAM word 128 up.
constexpr unsigned kSpritePaletteBase = 128u;

// The palette words these cases use. A word is 15 bits, blue-green-red from the top.
constexpr std::uint16_t kBackdrop = 0x0C41u;  // red 1, green 2, blue 3
constexpr std::uint16_t kRed = 0x001Fu;
constexpr std::uint16_t kGreen = 0x03E0u;
constexpr std::uint16_t kBlue = 0x7C00u;
constexpr std::uint16_t kWhite = 0x7FFFu;

using Rgba = std::array<std::uint8_t, 4>;

// What the converter drives for each of those at brightness 15, by hand from
// round(c * (N + 1) * 255 / (31 * 16)): 31 gives 255, 3 gives 25, 2 gives 16 and
// 1 gives 8.
constexpr Rgba kBackdropOut{8u, 16u, 25u, 255u};
constexpr Rgba kRedOut{255u, 0u, 0u, 255u};
constexpr Rgba kGreenOut{0u, 255u, 0u, 255u};
constexpr Rgba kBlueOut{0u, 0u, 255u, 255u};
constexpr Rgba kWhiteOut{255u, 255u, 255u, 255u};

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

// LDA #value ; STA $21xx
std::vector<std::uint8_t> storePort(std::uint8_t low, std::uint8_t value) {
  return {kLdaImm, value, kStaAbs, low, 0x21u};
}

// More than an NTSC frame of master cycles, so a run from power-on reaches the next
// frame's first line, where a finished frame is handed over.
constexpr std::uint64_t kFrameMaster = 357364u;
constexpr std::uint64_t kOneFrame = kFrameMaster + 20000u;

// A line of an NTSC frame, in master cycles, and the dot the picture's leftmost
// pixel is drawn at: a line's pixel x is drawn at dot 22 + x, four master cycles a
// dot, and the picture's row r is drawn on line r + 1.
constexpr std::uint64_t kLineMaster = 1364u;
constexpr std::uint64_t kFirstPictureDot = 22u;

// The master cycle within a line that the picture's pixel `x` is drawn at.
constexpr std::uint64_t dotOf(unsigned x) { return (kFirstPictureDot + x) * 4u; }

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

// A frame drawn by a machine that begins `program` with the beam at (line, hpos),
// so the program's own writes land on the picture rather than between frames. hpos
// counts master cycles into the line, four to a picture position, and a
// load-and-store pair costs 46 of them.
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

// A palette word as CGRAM holds it: the low byte, then the high.
void putColour(PpuState& ppu, unsigned word, std::uint16_t colour) {
  ppu.cgram[word * 2u] = static_cast<std::uint8_t>(colour & 0xFFu);
  ppu.cgram[word * 2u + 1u] = static_cast<std::uint8_t>(colour >> 8);
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

// A tilemap entry `index` words into a background's map.
void putEntry(PpuState& ppu, std::uint32_t mapByte, unsigned index, std::uint16_t entry) {
  ppu.vram[(mapByte + index * 2u) & 0xFFFFu] = static_cast<std::uint8_t>(entry & 0xFFu);
  ppu.vram[(mapByte + index * 2u + 1u) & 0xFFFFu] = static_cast<std::uint8_t>(entry >> 8);
}

// A background covered edge to edge by one character in one colour, at tile
// priority 0. The map is one 32x32 screen, so 32 entries across and 32 down reach
// every position of the picture.
void coverBackground(PpuState& ppu, std::uint32_t mapByte, std::uint32_t charByte,
                     unsigned planes, unsigned palette, std::uint8_t index) {
  putTileAt(ppu, charByte, planes, solid(index));
  const auto entry = static_cast<std::uint16_t>(palette << 10);
  for (unsigned at = 0u; at < 32u * 32u; ++at) putEntry(ppu, mapByte, at, entry);
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
// the picture.
void parkSprites(PpuState& ppu) {
  for (unsigned index = 0u; index < kSprites; ++index) {
    putSprite(ppu, index, -64, 0u, 0u, 0u, false);
  }
}

// A sprite's attribute byte, vhoopppN.
constexpr std::uint8_t attributes(bool flipVertical, bool flipHorizontal, unsigned priority,
                                 unsigned palette, bool secondTable) {
  return static_cast<std::uint8_t>((flipVertical ? 0x80u : 0u) |
                                   (flipHorizontal ? 0x40u : 0u) | (priority << 4) |
                                   (palette << 1) | (secondTable ? 1u : 0u));
}

// A PPU showing Mode 1's three backgrounds at full brightness, each covered edge to
// edge in one colour and all three at tile priority 0 — so BG1 is in front, BG2
// under it, BG3 under that and the backdrop under them all. A window that masks a
// layer uncovers the next.
PpuState screen() {
  PpuState ppu;
  ppu.inidisp = 0x0Fu;  // the screen on, brightness 15
  ppu.bgmode = 0x01u;   // Mode 1, every layer in 8x8 tiles, BG3 in its usual place
  ppu.objsel = kSpriteBase;
  ppu.bg1sc = kBg1MapBase;
  ppu.bg2sc = kBg2MapBase;
  ppu.bg3sc = kBg3MapBase;
  ppu.bg12nba = kBgCharBases12;
  ppu.bg34nba = kBg3CharBase;
  ppu.tm = 0x07u;  // the three backgrounds on the main screen
  parkSprites(ppu);

  putColour(ppu, 0u, kBackdrop);
  putColour(ppu, 1u, kRed);    // BG1 and BG2 palette 0, colour 1
  putColour(ppu, 2u, kGreen);  // and colour 2
  putColour(ppu, 5u, kBlue);   // BG3 palette 1, colour 1
  coverBackground(ppu, kBg1MapByte, kBg1CharByte, 4u, 0u, 1u);
  coverBackground(ppu, kBg2MapByte, kBg2CharByte, 4u, 0u, 2u);
  coverBackground(ppu, kBg3MapByte, kBg3CharByte, 2u, 1u, 1u);
  return ppu;
}

// The same with a sprite covering the picture's left half at priority 3, in front
// of every background, and the sprites shown on the main screen.
PpuState screenWithSprite() {
  PpuState ppu = screen();
  ppu.tm = 0x17u;
  putColour(ppu, kSpritePaletteBase + 1u, kWhite);  // sprite palette 0, colour 1
  putTileAt(ppu, kSpriteCharByte, kSpritePlanes, solid(1u));
  // Sixteen 8x8 sprites side by side cover x = 0..127 of every line they are on; a
  // sprite whose Y is N has its top row on the picture's row N.
  for (unsigned index = 0u; index < 16u; ++index) {
    putSprite(ppu, index, static_cast<int>(index * 8u), 40u, 0u,
              attributes(false, false, 3u, 0u, false), false);
  }
  return ppu;
}

// One layer's four bits of a window selector: the two enables and the two
// inversions, in the order the register holds them.
constexpr std::uint8_t selector(bool enable1, bool invert1, bool enable2, bool invert2) {
  return static_cast<std::uint8_t>((invert1 ? 0x01u : 0u) | (enable1 ? 0x02u : 0u) |
                                   (invert2 ? 0x04u : 0u) | (enable2 ? 0x08u : 0u));
}

// The same four bits for the high layer of a selector — BG2, BG4 or the colour
// window.
constexpr std::uint8_t highLayer(std::uint8_t bits) {
  return static_cast<std::uint8_t>(bits << 4);
}

// The window logic codes, as $212A and $212B hold them two bits to a layer.
constexpr std::uint8_t kOr = 0u;
constexpr std::uint8_t kAnd = 1u;
constexpr std::uint8_t kXor = 2u;
constexpr std::uint8_t kXnor = 3u;

// The picture row these cases read, clear of the sprite rows.
constexpr unsigned kRow = 10u;

// A PPU with window 1 over BG1 alone, spanning `left` to `right` and masking BG1 on
// the main screen.
PpuState bg1Window(std::uint8_t left, std::uint8_t right) {
  PpuState ppu = screen();
  ppu.wh0 = left;
  ppu.wh1 = right;
  ppu.w12sel = selector(true, false, false, false);
  ppu.tmw = 0x01u;  // BG1
  return ppu;
}

// ---- what a window is --------------------------------------------------------

TEST(Windows, AWindowCoversItsTwoEdgesAndEverythingBetween) {
  const Picture picture = draw(bg1Window(100u, 105u));

  EXPECT_EQ(picture.at(99u, kRow), kRedOut);     // outside, BG1 shows
  EXPECT_EQ(picture.at(100u, kRow), kGreenOut);  // the left edge is inside
  EXPECT_EQ(picture.at(103u, kRow), kGreenOut);
  EXPECT_EQ(picture.at(105u, kRow), kGreenOut);  // and so is the right
  EXPECT_EQ(picture.at(106u, kRow), kRedOut);
}

TEST(Windows, ALeftEdgePastTheRightIsAWindowWithNoRangeAtAll) {
  const Picture picture = draw(bg1Window(106u, 100u));

  for (unsigned x = 95u; x <= 110u; ++x) {
    EXPECT_EQ(picture.at(x, kRow), kRedOut) << "x = " << x;
  }
  EXPECT_EQ(picture.at(0u, kRow), kRedOut);
  EXPECT_EQ(picture.at(255u, kRow), kRedOut);
}

TEST(Windows, AWindowWhoseEdgesMeetIsOnePixelWide) {
  const Picture picture = draw(bg1Window(100u, 100u));

  EXPECT_EQ(picture.at(99u, kRow), kRedOut);
  EXPECT_EQ(picture.at(100u, kRow), kGreenOut);
  EXPECT_EQ(picture.at(101u, kRow), kRedOut);
}

TEST(Windows, TheSecondWindowHasItsOwnEdgesAndItsOwnEnableBit) {
  PpuState ppu = screen();
  ppu.wh0 = 20u;   // window 1's span, which nothing enables
  ppu.wh1 = 30u;
  ppu.wh2 = 100u;  // window 2's
  ppu.wh3 = 105u;
  ppu.w12sel = selector(false, false, true, false);
  ppu.tmw = 0x01u;

  const Picture picture = draw(ppu);
  EXPECT_EQ(picture.at(25u, kRow), kRedOut);  // window 1 is not enabled
  EXPECT_EQ(picture.at(99u, kRow), kRedOut);
  EXPECT_EQ(picture.at(100u, kRow), kGreenOut);
  EXPECT_EQ(picture.at(105u, kRow), kGreenOut);
  EXPECT_EQ(picture.at(106u, kRow), kRedOut);
}

TEST(Windows, InversionReplacesAWindowWithItsInverse) {
  PpuState ppu = bg1Window(100u, 105u);
  ppu.w12sel = selector(true, true, false, false);

  const Picture picture = draw(ppu);
  EXPECT_EQ(picture.at(99u, kRow), kGreenOut);  // outside the span is now covered
  EXPECT_EQ(picture.at(100u, kRow), kRedOut);   // and inside it is not
  EXPECT_EQ(picture.at(105u, kRow), kRedOut);
  EXPECT_EQ(picture.at(106u, kRow), kGreenOut);
  EXPECT_EQ(picture.at(0u, kRow), kGreenOut);
  EXPECT_EQ(picture.at(255u, kRow), kGreenOut);
}

TEST(Windows, EachWindowInvertsOnItsOwnBit) {
  PpuState ppu = screen();
  ppu.wh2 = 100u;
  ppu.wh3 = 105u;
  ppu.w12sel = selector(false, true, true, true);  // window 1's inversion bit is idle
  ppu.tmw = 0x01u;

  const Picture picture = draw(ppu);
  EXPECT_EQ(picture.at(99u, kRow), kGreenOut);
  EXPECT_EQ(picture.at(100u, kRow), kRedOut);
  EXPECT_EQ(picture.at(105u, kRow), kRedOut);
  EXPECT_EQ(picture.at(106u, kRow), kGreenOut);
}

// ---- combining the two windows -----------------------------------------------

// Both windows over BG1, overlapping in their middle: window 1 is 100..115 and
// window 2 is 110..125, so x = 105 is window 1 alone, 112 is both, 120 is window 2
// alone and 90 is neither — four positions on which the four logics disagree.
PpuState bothWindows(std::uint8_t logic) {
  PpuState ppu = screen();
  ppu.wh0 = 100u;
  ppu.wh1 = 115u;
  ppu.wh2 = 110u;
  ppu.wh3 = 125u;
  ppu.w12sel = selector(true, false, true, false);
  ppu.wbglog = logic;  // BG1's two bits are the low pair
  ppu.tmw = 0x01u;
  return ppu;
}

constexpr unsigned kWindow1Only = 105u;
constexpr unsigned kBothWindows = 112u;
constexpr unsigned kWindow2Only = 120u;
constexpr unsigned kNeitherWindow = 90u;

TEST(WindowLogic, OrCoversWhatEitherWindowCovers) {
  const Picture picture = draw(bothWindows(kOr));
  EXPECT_EQ(picture.at(kWindow1Only, kRow), kGreenOut);
  EXPECT_EQ(picture.at(kBothWindows, kRow), kGreenOut);
  EXPECT_EQ(picture.at(kWindow2Only, kRow), kGreenOut);
  EXPECT_EQ(picture.at(kNeitherWindow, kRow), kRedOut);
}

TEST(WindowLogic, AndCoversOnlyWhereTheTwoOverlap) {
  const Picture picture = draw(bothWindows(kAnd));
  EXPECT_EQ(picture.at(kWindow1Only, kRow), kRedOut);
  EXPECT_EQ(picture.at(kBothWindows, kRow), kGreenOut);
  EXPECT_EQ(picture.at(kWindow2Only, kRow), kRedOut);
  EXPECT_EQ(picture.at(kNeitherWindow, kRow), kRedOut);
}

TEST(WindowLogic, XorCoversWhereExactlyOneOfThemDoes) {
  const Picture picture = draw(bothWindows(kXor));
  EXPECT_EQ(picture.at(kWindow1Only, kRow), kGreenOut);
  EXPECT_EQ(picture.at(kBothWindows, kRow), kRedOut);
  EXPECT_EQ(picture.at(kWindow2Only, kRow), kGreenOut);
  EXPECT_EQ(picture.at(kNeitherWindow, kRow), kRedOut);
}

TEST(WindowLogic, XnorIsTheInverseOfXor) {
  const Picture picture = draw(bothWindows(kXnor));
  EXPECT_EQ(picture.at(kWindow1Only, kRow), kRedOut);
  EXPECT_EQ(picture.at(kBothWindows, kRow), kGreenOut);
  EXPECT_EQ(picture.at(kWindow2Only, kRow), kRedOut);
  EXPECT_EQ(picture.at(kNeitherWindow, kRow), kGreenOut);
}

TEST(WindowLogic, ALayerTakesItsLogicFromItsOwnPairOfBits) {
  // BG1's pair is the low one and BG2's the pair above it. Giving BG1 AND through
  // BG2's pair leaves BG1 on OR, which covers what AND would not.
  PpuState ppu = bothWindows(static_cast<std::uint8_t>(kAnd << 2));

  const Picture picture = draw(ppu);
  EXPECT_EQ(picture.at(kWindow1Only, kRow), kGreenOut);
  EXPECT_EQ(picture.at(kWindow2Only, kRow), kGreenOut);
}

// ---- which layers the combination masks ---------------------------------------

TEST(WindowMask, AWindowMasksNothingUntilTheMaskRegisterNamesTheLayer) {
  PpuState ppu = bg1Window(100u, 105u);
  ppu.tmw = 0x00u;

  const Picture picture = draw(ppu);
  EXPECT_EQ(picture.at(100u, kRow), kRedOut);
  EXPECT_EQ(picture.at(105u, kRow), kRedOut);
}

TEST(WindowMask, TheMaskRegisterAloneMasksNothing) {
  PpuState ppu = screen();
  ppu.wh0 = 100u;
  ppu.wh1 = 105u;
  ppu.w12sel = 0x00u;  // neither window enabled for any layer
  ppu.tmw = 0x17u;     // every layer named

  const Picture picture = draw(ppu);
  EXPECT_EQ(picture.at(100u, kRow), kRedOut);
  EXPECT_EQ(picture.at(105u, kRow), kRedOut);
}

TEST(WindowMask, AMaskedLayerFallsThroughToTheOneBehindIt) {
  // One window over all three backgrounds, brought in a layer at a time.
  PpuState ppu = screen();
  ppu.wh0 = 100u;
  ppu.wh1 = 105u;
  ppu.w12sel = static_cast<std::uint8_t>(selector(true, false, false, false) |
                                         highLayer(selector(true, false, false, false)));
  ppu.w34sel = selector(true, false, false, false);

  ppu.tmw = 0x01u;  // BG1
  EXPECT_EQ(draw(ppu).at(102u, kRow), kGreenOut);

  ppu.tmw = 0x03u;  // and BG2
  EXPECT_EQ(draw(ppu).at(102u, kRow), kBlueOut);

  ppu.tmw = 0x07u;  // and BG3
  EXPECT_EQ(draw(ppu).at(102u, kRow), kBackdropOut);
}

TEST(WindowMask, TheBackdropIsNeverMasked) {
  PpuState ppu = screen();
  ppu.wh0 = 0u;
  ppu.wh1 = 255u;  // a window over the whole line
  ppu.w12sel = static_cast<std::uint8_t>(selector(true, false, false, false) |
                                         highLayer(selector(true, false, false, false)));
  ppu.w34sel = static_cast<std::uint8_t>(selector(true, false, false, false) |
                                         highLayer(selector(true, false, false, false)));
  ppu.wobjsel = selector(true, false, false, false);
  ppu.tmw = 0x1Fu;  // every layer a mask register has a bit for

  const Picture picture = draw(ppu);
  EXPECT_EQ(picture.at(0u, kRow), kBackdropOut);
  EXPECT_EQ(picture.at(128u, kRow), kBackdropOut);
  EXPECT_EQ(picture.at(255u, kRow), kBackdropOut);
}

TEST(WindowMask, Bg3ReadsTheLowHalfOfItsOwnSelector) {
  PpuState ppu = screen();
  ppu.tm = 0x04u;  // BG3 alone on the main screen, so the backdrop is behind it
  ppu.wh0 = 100u;
  ppu.wh1 = 105u;
  ppu.w34sel = selector(true, false, false, false);  // the low half is BG3's
  ppu.tmw = 0x04u;

  const Picture picture = draw(ppu);
  EXPECT_EQ(picture.at(99u, kRow), kBlueOut);
  EXPECT_EQ(picture.at(102u, kRow), kBackdropOut);
  EXPECT_EQ(picture.at(106u, kRow), kBlueOut);
}

// ---- sprites ------------------------------------------------------------------

// A row the sprites of screenWithSprite() stand on.
constexpr unsigned kSpriteRow = 42u;

TEST(WindowMask, ASpriteIsMaskedWholeAtTheDot) {
  PpuState ppu = screenWithSprite();
  ppu.wh0 = 100u;
  ppu.wh1 = 105u;
  ppu.wobjsel = selector(true, false, false, false);  // the low half is OBJ's
  ppu.tmw = 0x10u;

  const Picture picture = draw(ppu);
  EXPECT_EQ(picture.at(99u, kSpriteRow), kWhiteOut);  // the sprite shows
  EXPECT_EQ(picture.at(100u, kSpriteRow), kRedOut);   // and BG1 shows where it does not
  EXPECT_EQ(picture.at(105u, kSpriteRow), kRedOut);
  EXPECT_EQ(picture.at(106u, kSpriteRow), kWhiteOut);
}

TEST(WindowMask, EachLayerIsMaskedOnItsOwnSelectorAndItsOwnBit) {
  // The same window enabled for the sprites and for BG1, with only the sprites
  // named in the mask register: BG1 shows through where the sprites are masked.
  PpuState ppu = screenWithSprite();
  ppu.wh0 = 100u;
  ppu.wh1 = 105u;
  ppu.wobjsel = selector(true, false, false, false);
  ppu.w12sel = selector(true, false, false, false);
  ppu.tmw = 0x10u;  // the sprites alone

  const Picture picture = draw(ppu);
  EXPECT_EQ(picture.at(102u, kSpriteRow), kRedOut);

  // Naming BG1 as well takes that away too, and BG2 shows.
  ppu.tmw = 0x11u;
  EXPECT_EQ(draw(ppu).at(102u, kSpriteRow), kGreenOut);
}

// ---- when a window is read ----------------------------------------------------

TEST(Windows, AnEdgeWrittenPartWayAlongALineChangesTheRestOfThatLine) {
  // The chip reads the window registers at each dot it draws, so an edge that moves
  // while a line is being drawn shapes the dots after it and leaves the ones
  // already drawn alone. A renderer resolving a whole line at one instant would
  // take the write either wholly early or wholly late.
  constexpr unsigned kMovedAt = 128u;
  PpuState ppu = bg1Window(0u, 255u);  // BG1 masked across the whole line

  Snes machine = machineWith(ppu);
  Picture picture;
  machine.setFrameObserver(&picture);

  // Whole lines first, then the dots of the line the write lands on — the machine's
  // own counters say where it is, rather than a cycle count predicting it.
  while (machine.state().vpos < 12u) {
    machine.run(kLineMaster - machine.state().hpos);
  }
  ASSERT_EQ(machine.state().vpos, 12u);
  ASSERT_LT(machine.state().hpos, dotOf(kMovedAt));
  machine.run(dotOf(kMovedAt) - machine.state().hpos);

  // The row that line draws, which is the one the write divides.
  const unsigned row = machine.state().vpos - 1u;

  SnesState state = machine.state();
  state.ppu.wh1 = 0u;  // the window shrinks to its leftmost pixel
  machine.restore(state);
  machine.run(kOneFrame);

  // Every dot up to the write kept the window it was drawn under, and every dot
  // after it took the new one.
  EXPECT_EQ(picture.at(0u, row), kGreenOut);
  EXPECT_EQ(picture.at(kMovedAt - 1u, row), kGreenOut);
  EXPECT_EQ(picture.at(kMovedAt + 1u, row), kRedOut);
  EXPECT_EQ(picture.at(255u, row), kRedOut);

  // The line above it was drawn whole under the old window, and the line below it
  // whole under the new — which is the one pixel at x = 0 the shrunk window still
  // covers, and nothing else.
  EXPECT_EQ(picture.at(255u, row - 1u), kGreenOut);
  EXPECT_EQ(picture.at(0u, row + 1u), kGreenOut);
  EXPECT_EQ(picture.at(1u, row + 1u), kRedOut);
  EXPECT_EQ(picture.at(255u, row + 1u), kRedOut);
}

// Which positions a window covers for a layer, the chip works out from the three
// selectors, the two logic registers and the four edges at the dot it is drawing —
// so a write to any of them divides the line it lands on. Each case below begins
// its program at master cycle 300 of line 50, where a first store pair lands at
// column 65, and compares a position before the landing against the frame the write
// was never made in and two after it against the frame carrying the written value
// throughout.
constexpr unsigned kWrittenRow = 49u;

TEST(Windows, ASelectorWrittenPartWayAlongALineMasksTheRestOfThatLine) {
  // $2123 <- BG1's window 1 enable, over a window spanning the whole line: the
  // positions before the landing show BG1 and the ones after it show BG2, which
  // masking BG1 uncovers.
  PpuState before = screen();
  before.wh0 = 0u;
  before.wh1 = 255u;
  before.w12sel = 0u;  // neither window is BG1's yet
  before.tmw = 0x01u;  // and the windows mask BG1 on the main screen
  PpuState after = before;
  after.w12sel = selector(true, false, false, false);
  const Picture never = draw(before);
  const Picture always = draw(after);
  const Picture picture = drawWith(before, storePort(0x23u, after.w12sel), 50u, 300u);

  EXPECT_NE(never.at(60u, kWrittenRow), always.at(60u, kWrittenRow));
  EXPECT_NE(never.at(66u, kWrittenRow), always.at(66u, kWrittenRow));
  EXPECT_NE(never.at(80u, kWrittenRow), always.at(80u, kWrittenRow));
  EXPECT_EQ(picture.at(60u, kWrittenRow), never.at(60u, kWrittenRow));
  EXPECT_EQ(picture.at(66u, kWrittenRow), always.at(66u, kWrittenRow));
  EXPECT_EQ(picture.at(80u, kWrittenRow), always.at(80u, kWrittenRow));
}

TEST(Windows, TheLogicWrittenPartWayAlongALineRecombinesTheRestOfThatLine) {
  // Both of BG1's windows enabled — window 1 across the whole line, window 2 over
  // its last fifty-six positions — and $212A written from OR to AND: OR covers every
  // position and AND only the ones both windows hold, so the positions after the
  // landing show BG1 again where the ones before it showed BG2.
  PpuState before = screen();
  before.wh0 = 0u;
  before.wh1 = 255u;
  before.wh2 = 200u;
  before.wh3 = 255u;
  before.w12sel = selector(true, false, true, false);
  before.wbglog = kOr;  // BG1's two bits are the low pair
  before.tmw = 0x01u;
  PpuState after = before;
  after.wbglog = kAnd;
  const Picture never = draw(before);
  const Picture always = draw(after);
  const Picture picture = drawWith(before, storePort(0x2Au, kAnd), 50u, 300u);

  EXPECT_NE(never.at(60u, kWrittenRow), always.at(60u, kWrittenRow));
  EXPECT_NE(never.at(66u, kWrittenRow), always.at(66u, kWrittenRow));
  EXPECT_NE(never.at(80u, kWrittenRow), always.at(80u, kWrittenRow));
  EXPECT_EQ(picture.at(60u, kWrittenRow), never.at(60u, kWrittenRow));
  EXPECT_EQ(picture.at(66u, kWrittenRow), always.at(66u, kWrittenRow));
  EXPECT_EQ(picture.at(80u, kWrittenRow), always.at(80u, kWrittenRow));
}

TEST(Windows, TheColourWindowsLogicWrittenPartWayAlongALineMovesTheBlackRegion) {
  // $2130's top field names the colour window as where the main screen's pixel
  // stands as black, both of the colour window's own windows are enabled the same
  // way, and $212B is written from OR to AND — which leaves black only where both
  // windows hold, so the positions after the landing show BG1.
  PpuState before = screen();
  before.cgwsel = 0x80u;  // black inside the colour window
  before.wobjsel = highLayer(selector(true, false, true, false));
  before.wobjlog = static_cast<std::uint8_t>(kOr << 2);  // the colour window's pair
  before.wh0 = 0u;
  before.wh1 = 255u;
  before.wh2 = 200u;
  before.wh3 = 255u;
  PpuState after = before;
  after.wobjlog = static_cast<std::uint8_t>(kAnd << 2);
  const Picture never = draw(before);
  const Picture always = draw(after);
  const Picture picture = drawWith(before, storePort(0x2Bu, after.wobjlog), 50u, 300u);

  EXPECT_NE(never.at(60u, kWrittenRow), always.at(60u, kWrittenRow));
  EXPECT_NE(never.at(66u, kWrittenRow), always.at(66u, kWrittenRow));
  EXPECT_NE(never.at(80u, kWrittenRow), always.at(80u, kWrittenRow));
  EXPECT_EQ(picture.at(60u, kWrittenRow), never.at(60u, kWrittenRow));
  EXPECT_EQ(picture.at(66u, kWrittenRow), always.at(66u, kWrittenRow));
  EXPECT_EQ(picture.at(80u, kWrittenRow), always.at(80u, kWrittenRow));
}

TEST(Windows, ASnapshotRestoredPartWayAlongALineMasksAsTheRestoredSelectorSays) {
  // A state taken part-way along a line, BG1's window 1 enable set in the copy, and
  // the copy restored: the rest of that line is drawn under the copy's selector.
  constexpr unsigned kMaskedAt = 128u;
  PpuState ppu = screen();
  ppu.wh0 = 0u;
  ppu.wh1 = 255u;
  ppu.w12sel = 0u;
  ppu.tmw = 0x01u;

  Snes machine = machineWith(ppu);
  Picture picture;
  machine.setFrameObserver(&picture);

  while (machine.state().vpos < 12u) {
    machine.run(kLineMaster - machine.state().hpos);
  }
  ASSERT_EQ(machine.state().vpos, 12u);
  ASSERT_LT(machine.state().hpos, dotOf(kMaskedAt));
  machine.run(dotOf(kMaskedAt) - machine.state().hpos);
  const unsigned row = machine.state().vpos - 1u;

  SnesState state = machine.state();
  state.ppu.w12sel = selector(true, false, false, false);
  machine.restore(state);
  machine.run(kOneFrame);

  EXPECT_EQ(picture.at(0u, row), kRedOut);
  EXPECT_EQ(picture.at(kMaskedAt - 1u, row), kRedOut);
  EXPECT_EQ(picture.at(kMaskedAt + 1u, row), kGreenOut);
  EXPECT_EQ(picture.at(255u, row), kGreenOut);
  EXPECT_EQ(picture.at(255u, row - 1u), kRedOut);
  EXPECT_EQ(picture.at(0u, row + 1u), kGreenOut);
}

// A machine drawing `ppu` with HDMA channel 0 armed against a table in work RAM,
// delivering each of the table's bytes to the register whose low address byte is
// `port`.
Snes hdmaMachine(const PpuState& ppu, std::uint8_t port,
                 std::initializer_list<std::uint8_t> table) {
  Snes machine(SnesConfig{.rom = haltedCartridge()});
  SnesState state = machine.state();
  state.ppu = ppu;
  state.dma[0] = DmaChannel{.dmap = 0x00u, .bbad = port, .a1t = 0x0300u, .a1b = 0x7Eu};
  state.hdmaen = 0x01u;
  std::uint16_t at = 0x0300u;
  for (std::uint8_t byte : table) state.wram[at++] = byte;
  machine.restore(state);
  return machine;
}

TEST(Windows, AnEdgeDeliveredBetweenTwoLinesShapesTheWholeOfTheNext) {
  // A transfer delivers its line's bytes past the picture's last dot, so a window
  // edge it moves is the one the whole of the following line is drawn under. This
  // is how a program shapes a window down the picture, which it has no vertical
  // extent of its own to do.
  PpuState ppu = bg1Window(0u, 255u);
  ppu.wh1 = 0u;  // the table drives the right edge

  // Thirty-two lines at 100, then thirty-two at 140, then the table ends.
  Snes machine = hdmaMachine(ppu, 0x27u, {0x20u, 100u, 0x20u, 140u, 0x00u});
  Picture picture;
  machine.setFrameObserver(&picture);
  machine.run(kOneFrame);

  // Two rows well inside the two blocks, each masked to its own edge across the
  // whole line.
  EXPECT_EQ(picture.at(0u, 10u), kGreenOut);
  EXPECT_EQ(picture.at(100u, 10u), kGreenOut);
  EXPECT_EQ(picture.at(101u, 10u), kRedOut);
  EXPECT_EQ(picture.at(255u, 10u), kRedOut);

  EXPECT_EQ(picture.at(0u, 40u), kGreenOut);
  EXPECT_EQ(picture.at(140u, 40u), kGreenOut);
  EXPECT_EQ(picture.at(141u, 40u), kRedOut);
  EXPECT_EQ(picture.at(255u, 40u), kRedOut);
}


// ---- colour math: the vocabulary these cases are written in ------------------

// The byte the converter drives for a five-bit channel at brightness 15, from the
// law the brightness scale states — value * (N + 1) * 255 / (31 * 16), rounded
// once. By hand: 31 gives 255, 23 gives 189, 16 gives 132, 15 gives 123, 8 gives
// 66, 3 gives 25, 2 gives 16 and 1 gives 8.
constexpr std::uint8_t out(unsigned channel) {
  return static_cast<std::uint8_t>((channel * 16u * 255u + 248u) / 496u);
}

// What the picture drives for a colour given as three five-bit channels.
constexpr Rgba rgb(unsigned red, unsigned green, unsigned blue) {
  return Rgba{out(red), out(green), out(blue), 255u};
}

// A palette word from three five-bit channels: red low, then green, then blue.
constexpr std::uint16_t colourOf(unsigned red, unsigned green, unsigned blue) {
  return static_cast<std::uint16_t>(red | (green << 5) | (blue << 10));
}

// The six things $2131 can reach, in the order it numbers them.
constexpr std::uint8_t kMathBg1 = 0x01u;
constexpr std::uint8_t kMathBg2 = 0x02u;
constexpr std::uint8_t kMathBg3 = 0x04u;
constexpr std::uint8_t kMathBg4 = 0x08u;
constexpr std::uint8_t kMathObj = 0x10u;
constexpr std::uint8_t kMathBackdrop = 0x20u;

// $2131: the operator, the halving, and the layers the math reaches.
constexpr std::uint8_t cgadsub(bool subtract, bool half, std::uint8_t layers) {
  return static_cast<std::uint8_t>((subtract ? 0x80u : 0u) | (half ? 0x40u : 0u) | layers);
}

// Each of $2130's two regions is named against the colour window.
constexpr std::uint8_t kNowhere = 0u;
constexpr std::uint8_t kOutside = 1u;
constexpr std::uint8_t kInside = 2u;
constexpr std::uint8_t kEverywhere = 3u;

// $2130: where the main colour is forced black, where math is prevented, and
// whether the addend is the sub screen rather than the fixed colour.
constexpr std::uint8_t cgwsel(std::uint8_t black, std::uint8_t prevent, bool subScreen) {
  return static_cast<std::uint8_t>((black << 6) | (prevent << 4) | (subScreen ? 0x02u : 0u));
}

// The fixed colour $2132 holds, three five-bit channels.
void putFixed(PpuState& ppu, unsigned red, unsigned green, unsigned blue) {
  ppu.fixedRed = static_cast<std::uint8_t>(red);
  ppu.fixedGreen = static_cast<std::uint8_t>(green);
  ppu.fixedBlue = static_cast<std::uint8_t>(blue);
}

// BG1 alone on the main screen in one colour, nothing on the sub screen, and
// colour math off until a case turns it on.
PpuState mathScreen(std::uint16_t mainColour) {
  PpuState ppu = screen();
  ppu.tm = 0x01u;
  ppu.ts = 0x00u;
  putColour(ppu, 1u, mainColour);
  return ppu;
}

// The colour window over the picture's left half, and nothing else windowed. Its
// four bits live in the high nibble of $2125 and its logic in bits 3-2 of $212B.
void putColourWindow(PpuState& ppu, std::uint8_t left, std::uint8_t right) {
  ppu.wh0 = left;
  ppu.wh1 = right;
  ppu.wobjsel = highLayer(selector(true, false, false, false));
}

// ---- what the converter does with a mathed colour ----------------------------

TEST(ColourMathVocabulary, TheChannelTableAgreesWithTheHandDerivedColours) {
  // The helper above is the brightness law written out; these four are the values
  // this file already derived by hand for its own palette, so the two agree or one
  // of them is wrong.
  EXPECT_EQ(rgb(31u, 0u, 0u), kRedOut);
  EXPECT_EQ(rgb(0u, 31u, 0u), kGreenOut);
  EXPECT_EQ(rgb(0u, 0u, 31u), kBlueOut);
  EXPECT_EQ(rgb(1u, 2u, 3u), kBackdropOut);
}

// ---- the operation -----------------------------------------------------------

TEST(ColourMath, NothingIsMathedUntilTheLayersBitIsSet) {
  PpuState ppu = mathScreen(colourOf(16u, 0u, 0u));
  putFixed(ppu, 0u, 16u, 0u);
  ppu.cgwsel = cgwsel(kNowhere, kNowhere, false);
  ppu.cgadsub = cgadsub(false, false, 0u);  // no layer
  EXPECT_EQ(draw(ppu).at(100u, kRow), rgb(16u, 0u, 0u));
}

TEST(ColourMath, TheFixedColourIsAddedChannelByChannel) {
  PpuState ppu = mathScreen(colourOf(16u, 0u, 4u));
  putFixed(ppu, 3u, 16u, 1u);
  ppu.cgwsel = cgwsel(kNowhere, kNowhere, false);
  ppu.cgadsub = cgadsub(false, false, kMathBg1);
  EXPECT_EQ(draw(ppu).at(100u, kRow), rgb(19u, 16u, 5u));
}

TEST(ColourMath, AnAdditionIsHeldAtTheTopOfAChannel) {
  PpuState ppu = mathScreen(colourOf(31u, 20u, 0u));
  putFixed(ppu, 16u, 20u, 0u);
  ppu.cgwsel = cgwsel(kNowhere, kNowhere, false);
  ppu.cgadsub = cgadsub(false, false, kMathBg1);
  // 31 + 16 and 20 + 20 both pass the top of a channel and stop there.
  EXPECT_EQ(draw(ppu).at(100u, kRow), rgb(31u, 31u, 0u));
}

TEST(ColourMath, ASubtractionTakesTheAddendAway) {
  PpuState ppu = mathScreen(colourOf(24u, 31u, 8u));
  putFixed(ppu, 8u, 1u, 3u);
  ppu.cgwsel = cgwsel(kNowhere, kNowhere, false);
  ppu.cgadsub = cgadsub(true, false, kMathBg1);
  EXPECT_EQ(draw(ppu).at(100u, kRow), rgb(16u, 30u, 5u));
}

TEST(ColourMath, ASubtractionIsHeldAtZero) {
  PpuState ppu = mathScreen(colourOf(8u, 2u, 0u));
  putFixed(ppu, 24u, 31u, 9u);
  ppu.cgwsel = cgwsel(kNowhere, kNowhere, false);
  ppu.cgadsub = cgadsub(true, false, kMathBg1);
  EXPECT_EQ(draw(ppu).at(100u, kRow), rgb(0u, 0u, 0u));
}

// ---- the halving -------------------------------------------------------------

TEST(ColourMath, TheHalvingComesBeforeTheChannelIsHeld) {
  PpuState ppu = mathScreen(colourOf(31u, 31u, 31u));
  putFixed(ppu, 31u, 31u, 31u);
  ppu.cgwsel = cgwsel(kNowhere, kNowhere, false);
  ppu.cgadsub = cgadsub(false, true, kMathBg1);
  // Two full channels added are 62, halved 31 — and not the 15 that holding the
  // sum first and halving afterwards would give.
  EXPECT_EQ(draw(ppu).at(100u, kRow), rgb(31u, 31u, 31u));
}

TEST(ColourMath, TheHalvingAppliesToASubtractionToo) {
  PpuState ppu = mathScreen(colourOf(31u, 20u, 4u));
  putFixed(ppu, 1u, 4u, 2u);
  ppu.cgwsel = cgwsel(kNowhere, kNowhere, false);
  ppu.cgadsub = cgadsub(true, true, kMathBg1);
  EXPECT_EQ(draw(ppu).at(100u, kRow), rgb(15u, 8u, 1u));
}

TEST(ColourMath, TheFixedColourAddendIsHalvedLikeAnyOther) {
  PpuState ppu = mathScreen(colourOf(31u, 0u, 0u));
  putFixed(ppu, 16u, 0u, 0u);
  ppu.cgwsel = cgwsel(kNowhere, kNowhere, false);  // bit 1 clear: the fixed colour
  ppu.cgadsub = cgadsub(false, true, kMathBg1);
  EXPECT_EQ(draw(ppu).at(100u, kRow), rgb(23u, 0u, 0u));  // (31 + 16) / 2
}

TEST(ColourMath, TheSubScreensBackdropIsNotHalved) {
  PpuState ppu = mathScreen(colourOf(31u, 0u, 0u));
  putFixed(ppu, 16u, 0u, 0u);
  ppu.cgwsel = cgwsel(kNowhere, kNowhere, true);  // the sub screen, which is empty
  ppu.cgadsub = cgadsub(false, true, kMathBg1);
  // The same two colours as the case above, reached by the other path: the sub
  // screen shows nothing, so its backdrop is the fixed colour and the halving does
  // not apply — 31 + 16 held at the top rather than (31 + 16) / 2.
  EXPECT_EQ(draw(ppu).at(100u, kRow), rgb(31u, 0u, 0u));
}

// ---- forcing the main colour black -------------------------------------------

TEST(ColourMath, AForcedBlackPixelStillTakesTheAddendWhole) {
  PpuState ppu = mathScreen(colourOf(31u, 31u, 31u));
  putFixed(ppu, 16u, 8u, 0u);
  ppu.cgwsel = cgwsel(kEverywhere, kNowhere, false);
  ppu.cgadsub = cgadsub(false, false, kMathBg1);
  EXPECT_EQ(draw(ppu).at(100u, kRow), rgb(16u, 8u, 0u));
}

TEST(ColourMath, AForcedBlackPixelIsNotHalved) {
  PpuState ppu = mathScreen(colourOf(31u, 31u, 31u));
  putFixed(ppu, 16u, 8u, 0u);
  ppu.cgwsel = cgwsel(kEverywhere, kNowhere, false);
  ppu.cgadsub = cgadsub(false, true, kMathBg1);
  // The halving is asked for and does not happen, so the addend lands whole.
  EXPECT_EQ(draw(ppu).at(100u, kRow), rgb(16u, 8u, 0u));
}

TEST(ColourMath, SubtractingFromAForcedBlackPixelLeavesItBlack) {
  PpuState ppu = mathScreen(colourOf(31u, 31u, 31u));
  putFixed(ppu, 16u, 8u, 4u);
  ppu.cgwsel = cgwsel(kEverywhere, kNowhere, false);
  ppu.cgadsub = cgadsub(true, false, kMathBg1);
  EXPECT_EQ(draw(ppu).at(100u, kRow), rgb(0u, 0u, 0u));
}

// ---- the two regions, and the colour window they are named against -----------

TEST(ColourMath, TheBlackRegionAppliesInsideTheColourWindowAlone) {
  PpuState ppu = mathScreen(colourOf(31u, 0u, 0u));
  putFixed(ppu, 0u, 16u, 0u);
  putColourWindow(ppu, 0u, 127u);
  ppu.cgwsel = cgwsel(kInside, kNowhere, false);
  ppu.cgadsub = cgadsub(false, false, kMathBg1);
  const Picture picture = draw(ppu);
  EXPECT_EQ(picture.at(0u, kRow), rgb(0u, 16u, 0u));    // forced black, then the addend
  EXPECT_EQ(picture.at(127u, kRow), rgb(0u, 16u, 0u));
  EXPECT_EQ(picture.at(128u, kRow), rgb(31u, 16u, 0u));  // outside it, the colour stands
  EXPECT_EQ(picture.at(255u, kRow), rgb(31u, 16u, 0u));
}

TEST(ColourMath, TheBlackRegionAppliesOutsideTheColourWindowAlone) {
  PpuState ppu = mathScreen(colourOf(31u, 0u, 0u));
  putFixed(ppu, 0u, 16u, 0u);
  putColourWindow(ppu, 0u, 127u);
  ppu.cgwsel = cgwsel(kOutside, kNowhere, false);
  ppu.cgadsub = cgadsub(false, false, kMathBg1);
  const Picture picture = draw(ppu);
  EXPECT_EQ(picture.at(0u, kRow), rgb(31u, 16u, 0u));
  EXPECT_EQ(picture.at(128u, kRow), rgb(0u, 16u, 0u));
}

TEST(ColourMath, TheBlackRegionCanApplyNowhereAndEverywhere) {
  PpuState ppu = mathScreen(colourOf(31u, 0u, 0u));
  putFixed(ppu, 0u, 16u, 0u);
  putColourWindow(ppu, 0u, 127u);
  ppu.cgadsub = cgadsub(false, false, kMathBg1);

  ppu.cgwsel = cgwsel(kNowhere, kNowhere, false);
  const Picture never = draw(ppu);
  EXPECT_EQ(never.at(0u, kRow), rgb(31u, 16u, 0u));
  EXPECT_EQ(never.at(200u, kRow), rgb(31u, 16u, 0u));

  ppu.cgwsel = cgwsel(kEverywhere, kNowhere, false);
  const Picture always = draw(ppu);
  EXPECT_EQ(always.at(0u, kRow), rgb(0u, 16u, 0u));
  EXPECT_EQ(always.at(200u, kRow), rgb(0u, 16u, 0u));
}

TEST(ColourMath, ThePreventRegionStopsMathWhereItNames) {
  PpuState ppu = mathScreen(colourOf(16u, 0u, 0u));
  putFixed(ppu, 0u, 16u, 0u);
  putColourWindow(ppu, 0u, 127u);
  ppu.cgwsel = cgwsel(kNowhere, kInside, false);
  ppu.cgadsub = cgadsub(false, false, kMathBg1);
  const Picture picture = draw(ppu);
  EXPECT_EQ(picture.at(0u, kRow), rgb(16u, 0u, 0u));    // prevented: the raw colour
  EXPECT_EQ(picture.at(128u, kRow), rgb(16u, 16u, 0u));  // and mathed outside it
}

TEST(ColourMath, TheTwoRegionsAreIndependentOfEachOther) {
  // The upper region blacks the whole line and the lower prevents nothing, so every
  // position is both forced black and mathed — the two fields do not gate one
  // another.
  PpuState ppu = mathScreen(colourOf(31u, 31u, 0u));
  putFixed(ppu, 4u, 8u, 12u);
  putColourWindow(ppu, 0u, 127u);
  ppu.cgwsel = cgwsel(kEverywhere, kNowhere, false);
  ppu.cgadsub = cgadsub(false, false, kMathBg1);
  const Picture picture = draw(ppu);
  EXPECT_EQ(picture.at(0u, kRow), rgb(4u, 8u, 12u));
  EXPECT_EQ(picture.at(200u, kRow), rgb(4u, 8u, 12u));
}

TEST(ColourMath, WithNoColourWindowEnabledEveryPositionIsOutside) {
  PpuState ppu = mathScreen(colourOf(31u, 0u, 0u));
  putFixed(ppu, 0u, 16u, 0u);
  ppu.wh0 = 0u;
  ppu.wh1 = 127u;
  ppu.wobjsel = 0u;  // the colour window enables neither of its windows
  ppu.cgwsel = cgwsel(kOutside, kNowhere, false);
  ppu.cgadsub = cgadsub(false, false, kMathBg1);
  const Picture picture = draw(ppu);
  // Nothing is inside, so "outside" covers the whole line.
  EXPECT_EQ(picture.at(0u, kRow), rgb(0u, 16u, 0u));
  EXPECT_EQ(picture.at(200u, kRow), rgb(0u, 16u, 0u));
}

TEST(ColourMath, TheColourWindowReadsItsOwnBitsAndNotTheSprites) {
  PpuState ppu = mathScreen(colourOf(31u, 0u, 0u));
  putFixed(ppu, 0u, 16u, 0u);
  ppu.wh0 = 0u;
  ppu.wh1 = 127u;
  // The sprites' nibble of $2125, which is the low one, must not reach the colour
  // window in the high one.
  ppu.wobjsel = selector(true, false, false, false);
  ppu.cgwsel = cgwsel(kInside, kNowhere, false);
  ppu.cgadsub = cgadsub(false, false, kMathBg1);
  const Picture picture = draw(ppu);
  EXPECT_EQ(picture.at(0u, kRow), rgb(31u, 16u, 0u));  // nothing is inside
  EXPECT_EQ(picture.at(200u, kRow), rgb(31u, 16u, 0u));
}

// ---- which pixels take math --------------------------------------------------

TEST(ColourMath, EachLayerIsReachedByItsOwnBit) {
  PpuState ppu = screen();
  ppu.tm = 0x02u;  // BG2 alone on the main screen
  ppu.ts = 0x00u;
  putColour(ppu, 2u, colourOf(16u, 0u, 0u));
  putFixed(ppu, 0u, 16u, 0u);
  ppu.cgwsel = cgwsel(kNowhere, kNowhere, false);

  ppu.cgadsub = cgadsub(false, false, kMathBg1);  // BG1's bit reaches BG2 nowhere
  EXPECT_EQ(draw(ppu).at(100u, kRow), rgb(16u, 0u, 0u));

  ppu.cgadsub = cgadsub(false, false, kMathBg2);
  EXPECT_EQ(draw(ppu).at(100u, kRow), rgb(16u, 16u, 0u));
}

TEST(ColourMath, Bg3IsReachedByItsOwnBitToo) {
  PpuState ppu = screen();
  ppu.tm = 0x04u;  // BG3 alone on the main screen
  ppu.ts = 0x00u;
  putColour(ppu, 5u, colourOf(8u, 0u, 0u));  // BG3 draws palette 1, colour 1
  putFixed(ppu, 0u, 16u, 0u);
  ppu.cgwsel = cgwsel(kNowhere, kNowhere, false);

  ppu.cgadsub = cgadsub(false, false, kMathBg2);
  EXPECT_EQ(draw(ppu).at(100u, kRow), rgb(8u, 0u, 0u));

  ppu.cgadsub = cgadsub(false, false, kMathBg3);
  EXPECT_EQ(draw(ppu).at(100u, kRow), rgb(8u, 16u, 0u));
}

TEST(ColourMath, TheBackdropHasABitOfItsOwn) {
  PpuState ppu = screen();
  ppu.tm = 0x00u;  // nothing on the main screen, so the backdrop shows
  ppu.ts = 0x00u;
  putColour(ppu, 0u, colourOf(8u, 0u, 0u));
  putFixed(ppu, 0u, 16u, 0u);
  ppu.cgwsel = cgwsel(kNowhere, kNowhere, false);

  ppu.cgadsub = cgadsub(false, false, kMathBg1);
  EXPECT_EQ(draw(ppu).at(100u, kRow), rgb(8u, 0u, 0u));

  ppu.cgadsub = cgadsub(false, false, kMathBackdrop);
  EXPECT_EQ(draw(ppu).at(100u, kRow), rgb(8u, 16u, 0u));
}

TEST(ColourMath, Bg4sBitReachesNothingInModeOne) {
  PpuState ppu = mathScreen(colourOf(16u, 0u, 0u));
  putFixed(ppu, 0u, 16u, 0u);
  ppu.cgwsel = cgwsel(kNowhere, kNowhere, false);
  ppu.cgadsub = cgadsub(false, false, kMathBg4);
  // Mode 1 draws three backgrounds, so the fourth's bit names a layer that is not
  // there and the pixel keeps its colour.
  EXPECT_EQ(draw(ppu).at(100u, kRow), rgb(16u, 0u, 0u));
}

TEST(ColourMath, ASpriteFromTheUpperPalettesTakesMath) {
  PpuState ppu = screen();
  ppu.tm = 0x10u;  // the sprites alone on the main screen
  ppu.ts = 0x00u;
  putTileAt(ppu, kSpriteCharByte, kSpritePlanes, solid(1u));
  putColour(ppu, kSpritePaletteBase + 4u * 16u + 1u, colourOf(16u, 0u, 0u));
  for (unsigned index = 0u; index < 16u; ++index) {
    putSprite(ppu, index, static_cast<int>(index * 8u), static_cast<std::uint8_t>(kRow), 0u,
              attributes(false, false, 3u, 4u, false), false);
  }
  putFixed(ppu, 0u, 16u, 0u);
  ppu.cgwsel = cgwsel(kNowhere, kNowhere, false);
  ppu.cgadsub = cgadsub(false, false, kMathObj);
  EXPECT_EQ(draw(ppu).at(100u, kRow), rgb(16u, 16u, 0u));
}

TEST(ColourMath, ASpriteFromTheLowerPalettesNeverDoes) {
  PpuState ppu = screen();
  ppu.tm = 0x10u;
  ppu.ts = 0x00u;
  putTileAt(ppu, kSpriteCharByte, kSpritePlanes, solid(1u));
  putColour(ppu, kSpritePaletteBase + 3u * 16u + 1u, colourOf(16u, 0u, 0u));
  for (unsigned index = 0u; index < 16u; ++index) {
    putSprite(ppu, index, static_cast<int>(index * 8u), static_cast<std::uint8_t>(kRow), 0u,
              attributes(false, false, 3u, 3u, false), false);
  }
  putFixed(ppu, 0u, 16u, 0u);
  ppu.cgwsel = cgwsel(kNowhere, kNowhere, false);
  ppu.cgadsub = cgadsub(false, false, kMathObj);
  // The same bit as the case above, and a palette below the fourth, so no math.
  EXPECT_EQ(draw(ppu).at(100u, kRow), rgb(16u, 0u, 0u));
}

// ---- the sub screen ----------------------------------------------------------

TEST(SubScreen, ALayerOnTheSubScreenAloneShowsNowhere) {
  PpuState ppu = screen();
  ppu.tm = 0x01u;  // BG1 on the main screen
  ppu.ts = 0x02u;  // BG2 on the sub screen and nowhere else
  putColour(ppu, 1u, colourOf(16u, 0u, 0u));
  putColour(ppu, 2u, colourOf(0u, 31u, 0u));
  ppu.cgwsel = cgwsel(kNowhere, kNowhere, false);
  ppu.cgadsub = cgadsub(false, false, 0u);  // math off
  EXPECT_EQ(draw(ppu).at(100u, kRow), rgb(16u, 0u, 0u));
}

TEST(SubScreen, TheSubScreensFrontMostPixelIsTheAddend) {
  PpuState ppu = screen();
  ppu.tm = 0x01u;
  ppu.ts = 0x02u;
  putColour(ppu, 1u, colourOf(16u, 0u, 0u));
  putColour(ppu, 2u, colourOf(0u, 8u, 4u));
  putFixed(ppu, 31u, 31u, 31u);  // which the sub screen displaces
  ppu.cgwsel = cgwsel(kNowhere, kNowhere, true);
  ppu.cgadsub = cgadsub(false, false, kMathBg1);
  EXPECT_EQ(draw(ppu).at(100u, kRow), rgb(16u, 8u, 4u));
}

TEST(SubScreen, ItsBackdropIsTheFixedColourRatherThanPaletteWordZero) {
  PpuState ppu = mathScreen(colourOf(8u, 0u, 0u));
  putColour(ppu, 0u, colourOf(0u, 0u, 31u));  // the main backdrop, which is not it
  putFixed(ppu, 0u, 16u, 0u);
  ppu.cgwsel = cgwsel(kNowhere, kNowhere, true);
  ppu.cgadsub = cgadsub(false, false, kMathBg1);
  EXPECT_EQ(draw(ppu).at(100u, kRow), rgb(8u, 16u, 0u));
}

TEST(SubScreen, TswMasksALayerSoItsBackdropReachesTheAddend) {
  PpuState ppu = screen();
  ppu.tm = 0x01u;
  ppu.ts = 0x02u;
  putColour(ppu, 1u, colourOf(8u, 0u, 0u));
  putColour(ppu, 2u, colourOf(0u, 31u, 0u));
  putFixed(ppu, 0u, 4u, 0u);
  ppu.wh0 = 0u;
  ppu.wh1 = 127u;
  ppu.w12sel = highLayer(selector(true, false, false, false));  // BG2's nibble
  ppu.tsw = 0x02u;
  ppu.cgwsel = cgwsel(kNowhere, kNowhere, true);
  ppu.cgadsub = cgadsub(false, false, kMathBg1);
  const Picture picture = draw(ppu);
  EXPECT_EQ(picture.at(0u, kRow), rgb(8u, 4u, 0u));    // masked: the sub backdrop
  EXPECT_EQ(picture.at(200u, kRow), rgb(8u, 31u, 0u));  // and BG2 where it is not
}

TEST(SubScreen, ItIsResolvedByTheSameOrderAsTheMainScreen) {
  PpuState ppu = screen();
  ppu.tm = 0x02u;  // BG2 on the main screen
  ppu.ts = 0x05u;  // BG1 and BG3 on the sub screen
  putColour(ppu, 2u, colourOf(8u, 0u, 0u));   // BG2, the main pixel
  putColour(ppu, 1u, colourOf(0u, 16u, 0u));  // BG1, in front on the sub screen
  putColour(ppu, 5u, colourOf(0u, 0u, 16u));  // BG3, behind it
  ppu.cgwsel = cgwsel(kNowhere, kNowhere, true);
  ppu.cgadsub = cgadsub(false, false, kMathBg2);
  // Mode 1 puts BG1 in front of BG3 at the same tile priority, so BG1 is what the
  // sub screen offers.
  EXPECT_EQ(draw(ppu).at(100u, kRow), rgb(8u, 16u, 0u));
}

// ---- the picture around the math ---------------------------------------------

TEST(ColourMath, BrightnessScalesTheMathedResult) {
  PpuState ppu = mathScreen(colourOf(16u, 0u, 0u));
  putFixed(ppu, 0u, 16u, 0u);
  ppu.cgwsel = cgwsel(kNowhere, kNowhere, false);
  ppu.cgadsub = cgadsub(false, false, kMathBg1);
  ppu.inidisp = 0x07u;  // brightness 7, the screen on
  const Rgba drawn = draw(ppu).at(100u, kRow);
  const auto scaled = [](unsigned channel) {
    return static_cast<std::uint8_t>((channel * 8u * 255u + 248u) / 496u);
  };
  EXPECT_EQ(drawn, (Rgba{scaled(16u), scaled(16u), scaled(0u), 255u}));
}

TEST(ColourMath, ForcedBlankIsBlackWithEveryPartOfTheMathOn) {
  PpuState ppu = mathScreen(colourOf(31u, 31u, 31u));
  putFixed(ppu, 31u, 31u, 31u);
  ppu.cgwsel = cgwsel(kNowhere, kNowhere, false);
  ppu.cgadsub = cgadsub(false, false, kMathBg1);
  ppu.inidisp = 0x8Fu;
  EXPECT_EQ(draw(ppu).at(100u, kRow), (Rgba{0u, 0u, 0u, 255u}));
}

TEST(ColourMath, AMachineWithAnObserverAndOneWithoutAgreeInEveryField) {
  PpuState ppu = mathScreen(colourOf(16u, 4u, 0u));
  putFixed(ppu, 8u, 8u, 8u);
  ppu.cgwsel = cgwsel(kInside, kNowhere, true);
  ppu.cgadsub = cgadsub(false, true, kMathBg1);
  putColourWindow(ppu, 40u, 200u);

  Snes watched = machineWith(ppu);
  Picture picture;
  watched.setFrameObserver(&picture);
  watched.run(kOneFrame);

  Snes unwatched = machineWith(ppu);
  unwatched.run(kOneFrame);

  // Colour math adds no state, so a frame of it leaves the two machines identical —
  // the pin the picture unit set, re-asserted by the first block since it to touch
  // the dot path.
  EXPECT_EQ(watched.state().ppu.cgwsel, unwatched.state().ppu.cgwsel);
  EXPECT_EQ(watched.state().ppu.cgadsub, unwatched.state().ppu.cgadsub);
  EXPECT_EQ(watched.state().ppu.sprites.line, unwatched.state().ppu.sprites.line);
  EXPECT_EQ(watched.state().ppu.vram, unwatched.state().ppu.vram);
  EXPECT_EQ(watched.state().ppu.cgram, unwatched.state().ppu.cgram);
}

}  // namespace
}  // namespace snaggletooth
