// The picture the PPU draws, and the seam it hands it over: the frame observer's
// terms, the raster's shape, the converter's bytes, and Mode 1 complete — the
// tilemap formula at its four sizes, the character address, the bitplanes and their
// order at both depths, both flips, 16x16 blocks, the palette bits, colour 0's
// transparency, scrolling, the backdrop, the registers each of the three
// backgrounds reads, the priority chart across them and the register that exchanges
// it for the other, and the layers the main screen does not show. Every expectation
// is computed by hand from the register page; the pictures are placed as a program
// would have left them, and one case drives the whole path from a cartridge that
// writes the video memories through their ports.

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

// The other two backgrounds Mode 1 draws, placed clear of BG1: BG2's map at byte
// $4000 and BG3's at $4800, which the six base bits reach as screens 8 and 9, and
// their characters at $A000 and $C000. BG1's own 64x64 map runs to $27FF, so
// nothing here overlaps it.
constexpr std::uint8_t kMapBase2 = 0x20u;    // $2108: screen 8, size 00
constexpr std::uint8_t kMapBase3 = 0x24u;    // $2109: screen 9, size 00
constexpr std::uint8_t kCharBases12 = 0x54u; // $210B: BG1 at $8000, BG2 at $A000
constexpr std::uint8_t kCharBase3 = 0x06u;   // $210C: BG3 at $C000

// Where a background keeps its pieces and how deep its characters are: the byte
// address its screen base names, the byte address its character base names, and
// its bitplanes in Mode 1 — four for the two sixteen-colour backgrounds, two for
// BG3's four.
struct Layout {
  std::uint32_t map;
  std::uint32_t characters;
  unsigned planes;
};

constexpr Layout kBg1{.map = kMapByte, .characters = kCharByte, .planes = 4u};
constexpr Layout kBg2{.map = 0x4000u, .characters = 0xA000u, .planes = 4u};
constexpr Layout kBg3{.map = 0x4800u, .characters = 0xC000u, .planes = 2u};

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

// A frame drawn by a machine that begins `program` with the beam at (line, hpos),
// so the program's own writes land on the picture rather than between frames.
// hpos counts master cycles into the line, four to a picture position, and a
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

// LDA #value ; STA $21xx
std::vector<std::uint8_t> storePort(std::uint8_t low, std::uint8_t value) {
  return {kLdaImm, value, kStaAbs, low, 0x21u};
}

std::vector<std::uint8_t> joined(std::initializer_list<std::vector<std::uint8_t>> parts) {
  std::vector<std::uint8_t> program;
  for (const std::vector<std::uint8_t>& part : parts) {
    program.insert(program.end(), part.begin(), part.end());
  }
  return program;
}

// A palette word as CGRAM holds it: the low byte, then the high.
void putColour(PpuState& ppu, unsigned word, std::uint16_t colour) {
  ppu.cgram[word * 2u] = static_cast<std::uint8_t>(colour & 0xFFu);
  ppu.cgram[word * 2u + 1u] = static_cast<std::uint8_t>(colour >> 8);
}

// A tilemap entry `index` words into a background's map, the same way.
void putEntry(PpuState& ppu, const Layout& bg, unsigned index, std::uint16_t entry) {
  ppu.vram[(bg.map + index * 2u) & 0xFFFFu] = static_cast<std::uint8_t>(entry & 0xFFu);
  ppu.vram[(bg.map + index * 2u + 1u) & 0xFFFFu] = static_cast<std::uint8_t>(entry >> 8);
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

// The same, for a character a background's own base holds: eight bytes a bitplane.
void putTile(PpuState& ppu, const Layout& bg, unsigned tile, const TileRows& rows) {
  putTileAt(ppu, bg.characters + tile * 8u * bg.planes, bg.planes, rows);
}

// A character every pixel of which is one colour index.
void putSolidTile(PpuState& ppu, const Layout& bg, unsigned tile, std::uint8_t index) {
  TileRows rows{};
  for (auto& row : rows) row.fill(index);
  putTile(ppu, bg, tile, rows);
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
  putColour(ppu, 5u, kCyan);     // BG3's palette 1 colour 1, and BG1's palette 0 colour 5
  putColour(ppu, 9u, kMagenta);  // BG3's palette 2 colour 1
  putColour(ppu, 49u, kYellow);  // palette 3's colour 1
  return ppu;
}

// The same picture with all three of Mode 1's backgrounds placed and shown: BG2 and
// BG3 take the bases above beside BG1's, each carries the same -1 vertical offset,
// and the main screen has all three.
PpuState threeBackgrounds() {
  PpuState ppu = screen();
  ppu.bg2sc = kMapBase2;
  ppu.bg3sc = kMapBase3;
  ppu.bg12nba = kCharBases12;
  ppu.bg34nba = kCharBase3;
  ppu.bg2vofs = 0x3FFu;
  ppu.bg3vofs = 0x3FFu;
  ppu.tm = 0x07u;
  return ppu;
}

// A picture whose every position reads one character, so the entry a position holds
// is the same wherever along the line it falls and the character's own rows are what
// its pixels name.
PpuState oneCharacterEverywhere() {
  PpuState ppu = screen();
  for (unsigned index = 0u; index < kScreenEntries; ++index) putEntry(ppu, kBg1, index, 0x0001u);
  putSolidTile(ppu, kBg1, 1u, 1u);
  return ppu;
}

// The four colour indices a cycling picture steps through: red, green, blue and
// white, which screen() puts in palette words 1, 2, 4 and 8.
constexpr std::array<std::uint8_t, 4> kCycle{1u, 2u, 4u, 8u};

// A picture whose tile columns cycle through four solid characters, so a horizontal
// offset of one tile lands a different colour at every position. The characters run
// past the four the cycle names, so a 16x16 block — which reads the character after
// its own and the one sixteen on — finds one wherever it looks.
PpuState columnCyclingPicture() {
  PpuState ppu = screen();
  for (unsigned index = 0u; index < kScreenEntries; ++index) {
    putEntry(ppu, kBg1, index, static_cast<std::uint16_t>((index % 32u) % 4u + 1u));
  }
  for (unsigned tile = 1u; tile <= 24u; ++tile) {
    putSolidTile(ppu, kBg1, tile, kCycle[(tile - 1u) % 4u]);
  }
  return ppu;
}

// A picture of one character whose eight rows cycle through the same four colours,
// so a vertical offset that moves the row the line reads changes what it shows
// without moving the position's tilemap entry.
PpuState rowCyclingPicture() {
  PpuState ppu = screen();
  for (unsigned index = 0u; index < kScreenEntries; ++index) putEntry(ppu, kBg1, index, 0x0001u);
  TileRows rows{};
  for (unsigned row = 0u; row < 8u; ++row) rows[row].fill(kCycle[row % 4u]);
  putTile(ppu, kBg1, 1u, rows);
  return ppu;
}

// The row of character 1 that beam line 50 reads, and the word the video port
// reaches it at. BG1's characters begin at kCharByte and a sixteen-colour character
// is thirty-two bytes, so character 1 begins at kCharByte + $20; the -1 vertical
// offset screen() carries puts line 50 on the character's second row, two bytes
// further on. A solid character holds plane 0 all ones and the rest clear, which is
// colour index 1 across; clearing plane 0 and setting plane 1 makes the row index 2.
constexpr std::uint32_t kTileOneRowAtLineFifty = kCharByte + 0x20u + 2u;
constexpr std::uint16_t kTileOneRowWord = kTileOneRowAtLineFifty / 2u;

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
      a.joyClocks != b.joyClocks || a.autoJoyStart != b.autoJoyStart ||
      a.autoJoyClocked != b.autoJoyClocked ||
      a.joyStrobe != b.joyStrobe || a.wrio != b.wrio) {
    return differs("the controller ports");
  }
  if (a.mdmaen != b.mdmaen || a.hdmaen != b.hdmaen || a.hdmaActive != b.hdmaActive ||
      a.hdmaEnded != b.hdmaEnded || a.hdmaInited != b.hdmaInited) {
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
  putEntry(ppu, kBg1, 0u, 0x0001u);
  putSolidTile(ppu, kBg1, 1u, 1u);
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
  putEntry(ppu, kBg1, 0u, 0x0001u);
  putSolidTile(ppu, kBg1, 1u, 1u);
  ppu.inidisp = 0x8Fu;  // forced blank, brightness 15
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.frames, 1u);
  EXPECT_EQ(picture.at(0u, 0u), kBlack);
  EXPECT_EQ(picture.at(255u, 223u), kBlack);
}

TEST(SnesPpuPicture, BrightnessZeroIsABlackFrame) {
  PpuState ppu = screen();
  putEntry(ppu, kBg1, 0u, 0x0001u);
  putSolidTile(ppu, kBg1, 1u, 1u);
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
  putEntry(ppu, kBg1, 0u, 0x0008u);  // a tile in colour index 8, which is white
  putSolidTile(ppu, kBg1, 8u, 8u);
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
  putEntry(ppu, kBg1, 0u, 0x0001u);
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
  putEntry(ppu, kBg1, 0u, static_cast<std::uint16_t>((3u << 10) | 1u));
  TileRows rows{};
  for (auto& row : rows) row.fill(1u);
  rows[2][3] = 0u;  // one hole in an otherwise solid character
  putTile(ppu, kBg1, 1u, rows);
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.frames, 1u);
  EXPECT_EQ(picture.at(0u, 0u), kYellowOut);
  EXPECT_EQ(picture.at(3u, 2u), kBackdropOut);
}

TEST(SnesPpuPicture, ThePaletteBitsChooseSixteenColoursApart) {
  // Palette 3 of a 16-colour background begins at word 3 * 16 = 48, so its colour 1
  // is word 49.
  PpuState ppu = screen();
  putEntry(ppu, kBg1, 0u, static_cast<std::uint16_t>((3u << 10) | 1u));
  putSolidTile(ppu, kBg1, 1u, 1u);
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.frames, 1u);
  EXPECT_EQ(picture.at(0u, 0u), kYellowOut);
}

TEST(SnesPpuPicture, TheCharacterBaseAndTheTileNumberNameTheCharacter) {
  // $210B holding 2 puts BG1's characters at 2 << 13 = $4000, and tile 3 is 32 bytes
  // a character on from there.
  PpuState ppu = screen();
  ppu.bg12nba = 0x02u;
  putEntry(ppu, kBg1, 0u, 0x0003u);
  TileRows rows{};
  for (auto& row : rows) row.fill(2u);
  putTileAt(ppu, 0x4000u + 3u * 32u, 4u, rows);
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.frames, 1u);
  EXPECT_EQ(picture.at(0u, 0u), kGreenOut);
}

// ---- the tilemap -------------------------------------------------------------

TEST(SnesPpuPicture, TheTilemapEntryForAPositionIsRowsOfThirtyTwo) {
  // The entry for tile (X, Y) of one screen is ((Y & 0x1F) << 5) + (X & 0x1F) words
  // into the map, so tile (1, 1) covers the picture from (8, 8).
  PpuState ppu = screen();
  putEntry(ppu, kBg1, 1u * 32u + 1u, 0x0001u);
  putSolidTile(ppu, kBg1, 1u, 1u);
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
  putEntry(ppu, kBg1, 0u, 0x0001u);
  ppu.vram[0x0400u] = 0x02u;  // where a half-screen step would look: word $200
  ppu.vram[0x0401u] = 0x00u;
  putSolidTile(ppu, kBg1, 1u, 1u);
  putSolidTile(ppu, kBg1, 2u, 2u);
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.frames, 1u);
  EXPECT_EQ(picture.at(0u, 0u), kRedOut);
}

TEST(SnesPpuPicture, ASingleScreenMapRepeatsEveryTwoHundredAndFiftySixPixels) {
  PpuState ppu = screen();  // size 00: one 32x32 screen
  putEntry(ppu, kBg1, 0u, 0x0001u);                 // screen A's first tile
  putEntry(ppu, kBg1, kScreenEntries, 0x0002u);     // where a second screen would begin
  putSolidTile(ppu, kBg1, 1u, 1u);
  putSolidTile(ppu, kBg1, 2u, 2u);
  ppu.bg1hofs = 256u;
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.frames, 1u);
  EXPECT_EQ(picture.at(0u, 0u), kRedOut);  // screen A again, not the bytes beyond it
}

TEST(SnesPpuPicture, TheWideMapPutsItsSecondScreenToTheRight) {
  PpuState ppu = screen();
  ppu.bg1sc = static_cast<std::uint8_t>(kMapBase | 0x01u);  // 64x32
  putEntry(ppu, kBg1, 0u, 0x0001u);
  putEntry(ppu, kBg1, kScreenEntries, 0x0002u);
  putSolidTile(ppu, kBg1, 1u, 1u);
  putSolidTile(ppu, kBg1, 2u, 2u);
  ppu.bg1hofs = 256u;
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.frames, 1u);
  EXPECT_EQ(picture.at(0u, 0u), kGreenOut);
}

TEST(SnesPpuPicture, TheTallMapPutsItsSecondScreenBelow) {
  PpuState ppu = screen();
  ppu.bg1sc = static_cast<std::uint8_t>(kMapBase | 0x02u);  // 32x64
  putEntry(ppu, kBg1, 0u, 0x0001u);
  putEntry(ppu, kBg1, kScreenEntries, 0x0002u);
  putSolidTile(ppu, kBg1, 1u, 1u);
  putSolidTile(ppu, kBg1, 2u, 2u);
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
  putEntry(ppu, kBg1, 0u, 0x0001u);                      // A
  putEntry(ppu, kBg1, kScreenEntries, 0x0002u);          // B, to the right
  putEntry(ppu, kBg1, 2u * kScreenEntries, 0x0004u);     // C, below
  putSolidTile(ppu, kBg1, 1u, 1u);
  putSolidTile(ppu, kBg1, 2u, 2u);
  putSolidTile(ppu, kBg1, 4u, 4u);

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
  putEntry(ppu, kBg1, 0u, 0x4001u);  // bit 14: flip horizontally
  TileRows rows{};
  rows[0][0] = 1u;
  putTile(ppu, kBg1, 1u, rows);
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.frames, 1u);
  EXPECT_EQ(picture.at(7u, 0u), kRedOut);
  EXPECT_EQ(picture.at(0u, 0u), kBackdropOut);
}

TEST(SnesPpuPicture, AVerticalFlipReversesTheColumn) {
  PpuState ppu = screen();
  putEntry(ppu, kBg1, 0u, 0x8001u);  // bit 15: flip vertically
  TileRows rows{};
  rows[0][0] = 1u;
  putTile(ppu, kBg1, 1u, rows);
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.frames, 1u);
  EXPECT_EQ(picture.at(0u, 7u), kRedOut);
  EXPECT_EQ(picture.at(0u, 0u), kBackdropOut);
}

// ---- 16x16 blocks ------------------------------------------------------------

TEST(SnesPpuPicture, ABlockIsTheTileAndTheThreeAfterItByOneAndSixteen) {
  PpuState ppu = screen();
  ppu.bgmode = 0x11u;  // Mode 1, and $2105 bit 4: BG1 in 16x16 blocks
  putEntry(ppu, kBg1, 0u, 0x0001u);
  putSolidTile(ppu, kBg1, 1u, 1u);    // the block's top left
  putSolidTile(ppu, kBg1, 2u, 2u);    // Tile + 1, its top right
  putSolidTile(ppu, kBg1, 17u, 4u);   // Tile + 16, its bottom left
  putSolidTile(ppu, kBg1, 18u, 8u);   // Tile + 17, its bottom right
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
  putEntry(ppu, kBg1, 0u, 0x02FFu);
  putSolidTile(ppu, kBg1, 0x2FFu, 1u);
  putSolidTile(ppu, kBg1, 0x300u, 2u);
  putSolidTile(ppu, kBg1, 0x30Fu, 4u);
  putSolidTile(ppu, kBg1, 0x310u, 8u);
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
  putEntry(ppu, kBg1, 0u, 0xC001u);  // both flips
  putSolidTile(ppu, kBg1, 1u, 1u);
  putSolidTile(ppu, kBg1, 2u, 2u);
  putSolidTile(ppu, kBg1, 17u, 4u);
  putSolidTile(ppu, kBg1, 18u, 8u);
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
  putEntry(ppu, kBg1, 1u, 0x0001u);  // the tile at (1, 0)
  putSolidTile(ppu, kBg1, 1u, 1u);
  ppu.bg1hofs = 8u;
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.frames, 1u);
  EXPECT_EQ(picture.at(0u, 0u), kRedOut);
  EXPECT_EQ(picture.at(7u, 0u), kRedOut);
  EXPECT_EQ(picture.at(8u, 0u), kBackdropOut);
}

TEST(SnesPpuPicture, ScrollingMovesThePictureWithinATile) {
  PpuState ppu = screen();
  putEntry(ppu, kBg1, 0u, 0x0001u);
  TileRows rows{};
  rows[0][3] = 1u;  // one pixel, four columns in
  putTile(ppu, kBg1, 1u, rows);
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
  putEntry(ppu, kBg1, 0u, 0x0001u);
  TileRows rows{};
  rows[1][0] = 1u;  // the tile's second row
  putTile(ppu, kBg1, 1u, rows);
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.frames, 1u);
  EXPECT_EQ(picture.at(0u, 0u), kRedOut);
}

// ---- BG2, the second sixteen-colour background --------------------------------

TEST(SnesPpuPicture, Bg2ReadsItsMapFromItsOwnScreenRegister) {
  // $2108 names BG2's map where $2107 names BG1's, so a tile placed in one does not
  // show in the other.
  PpuState ppu = threeBackgrounds();
  ppu.tm = 0x02u;  // BG2 alone on the main screen
  putEntry(ppu, kBg1, 0u, 0x0001u);
  putEntry(ppu, kBg2, 0u, 0x0002u);
  putSolidTile(ppu, kBg1, 1u, 1u);
  putSolidTile(ppu, kBg2, 2u, 2u);
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.frames, 1u);
  EXPECT_EQ(picture.at(0u, 0u), kGreenOut);
}

TEST(SnesPpuPicture, Bg2ReadsItsCharactersFromTheHighHalfOfTheBaseRegister) {
  // $210B holds BG2's base in bits 4-7 and BG1's in bits 0-3.
  PpuState ppu = threeBackgrounds();
  ppu.tm = 0x02u;
  putEntry(ppu, kBg2, 0u, 0x0001u);
  putSolidTile(ppu, kBg1, 1u, 1u);  // at $8000, which the low half names
  putSolidTile(ppu, kBg2, 1u, 4u);  // at $A000, which the high half names
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.frames, 1u);
  EXPECT_EQ(picture.at(0u, 0u), kBlueOut);
}

TEST(SnesPpuPicture, Bg2ScrollsByItsOwnRegisters) {
  // $210F moves BG2 and leaves BG1 where it is.
  PpuState ppu = threeBackgrounds();
  ppu.tm = 0x03u;
  putEntry(ppu, kBg1, 4u, 0x0001u);  // BG1's tile at (4, 0): the picture from x = 32
  putEntry(ppu, kBg2, 1u, 0x0002u);  // BG2's at (1, 0): from x = 8
  putSolidTile(ppu, kBg1, 1u, 1u);
  putSolidTile(ppu, kBg2, 2u, 2u);
  ppu.bg2hofs = 8u;                  // which brings BG2's to the left edge
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.frames, 1u);
  EXPECT_EQ(picture.at(0u, 0u), kGreenOut);
  EXPECT_EQ(picture.at(32u, 0u), kRedOut);
}

TEST(SnesPpuPicture, Bg2TakesItsTileSizeFromItsOwnBit) {
  // $2105 bit 5 is BG2's 16x16 bit as bit 4 is BG1's, and the block is the same
  // Tile, Tile + 1, Tile + 16, Tile + 17.
  PpuState ppu = threeBackgrounds();
  ppu.tm = 0x02u;
  ppu.bgmode = 0x21u;  // Mode 1, BG2 in 16x16 blocks and BG1 in 8x8
  putEntry(ppu, kBg2, 0u, 0x0001u);
  putSolidTile(ppu, kBg2, 1u, 1u);   // the block's top left
  putSolidTile(ppu, kBg2, 18u, 2u);  // Tile + 17, its bottom right
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.frames, 1u);
  EXPECT_EQ(picture.at(0u, 0u), kRedOut);
  EXPECT_EQ(picture.at(8u, 8u), kGreenOut);
}

TEST(SnesPpuPicture, Bg2IsShownByItsOwnMainScreenBit) {
  // $212C bit 1 is BG2's. On the sub screen alone it draws nothing.
  PpuState ppu = threeBackgrounds();
  ppu.tm = 0x00u;
  ppu.ts = 0x02u;
  putEntry(ppu, kBg2, 0u, 0x0001u);
  putSolidTile(ppu, kBg2, 1u, 1u);
  const Picture hidden = draw(ppu);
  ASSERT_EQ(hidden.frames, 1u);
  EXPECT_EQ(hidden.at(0u, 0u), kBackdropOut);

  ppu.tm = 0x02u;
  const Picture shown = draw(ppu);
  ASSERT_EQ(shown.frames, 1u);
  EXPECT_EQ(shown.at(0u, 0u), kRedOut);
}

// ---- BG3, the four-colour background ------------------------------------------

TEST(SnesPpuPicture, Bg3IsTwoBitplanesAndReadsNothingAboveThem) {
  // Four colours means planes 0 and 1 alone. The sixteen bytes a sixteen-colour
  // character would keep planes 2 and 3 in belong to the next characters here, and
  // filling them changes no pixel.
  PpuState ppu = threeBackgrounds();
  ppu.tm = 0x04u;  // BG3 alone
  putEntry(ppu, kBg3, 0u, 0x0001u);
  putSolidTile(ppu, kBg3, 1u, 1u);
  for (unsigned byte = 0u; byte < 16u; ++byte) {
    ppu.vram[kBg3.characters + 16u + 16u + byte] = 0xFFu;
  }
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.frames, 1u);
  EXPECT_EQ(picture.at(0u, 0u), kRedOut);  // colour 1, not the 13 four planes would give
}

TEST(SnesPpuPicture, Bg3CharactersAreSixteenBytesApart) {
  // Eight bytes a bitplane, so a four-colour character is half a sixteen-colour one.
  PpuState ppu = threeBackgrounds();
  ppu.tm = 0x04u;
  putEntry(ppu, kBg3, 0u, 0x0003u);
  putSolidTile(ppu, kBg3, 3u, 2u);
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.frames, 1u);
  EXPECT_EQ(picture.at(0u, 0u), kGreenOut);
}

TEST(SnesPpuPicture, Bg3PalettesAreFourColoursApart) {
  // A four-colour background's palette begins ppp * 4 words in, so palette 1's
  // colour 1 is word 5 and palette 2's is word 9.
  PpuState ppu = threeBackgrounds();
  ppu.tm = 0x04u;
  putEntry(ppu, kBg3, 0u, static_cast<std::uint16_t>((1u << 10) | 1u));
  putEntry(ppu, kBg3, 1u, static_cast<std::uint16_t>((2u << 10) | 2u));
  putSolidTile(ppu, kBg3, 1u, 1u);
  putSolidTile(ppu, kBg3, 2u, 1u);
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.frames, 1u);
  EXPECT_EQ(picture.at(0u, 0u), kCyanOut);
  EXPECT_EQ(picture.at(8u, 0u), kMagentaOut);
}

TEST(SnesPpuPicture, Bg3ReadsItsMapAndCharactersFromItsOwnRegisters) {
  // $2109 names BG3's map and $210C bits 0-3 its characters, neither of which the
  // other two backgrounds' registers reach.
  PpuState ppu = threeBackgrounds();
  ppu.tm = 0x04u;
  putEntry(ppu, kBg1, 0u, 0x0001u);
  putEntry(ppu, kBg2, 0u, 0x0001u);
  putEntry(ppu, kBg3, 0u, 0x0002u);
  putSolidTile(ppu, kBg3, 2u, 2u);
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.frames, 1u);
  EXPECT_EQ(picture.at(0u, 0u), kGreenOut);
}

TEST(SnesPpuPicture, Bg3ScrollsByItsOwnRegisters) {
  PpuState ppu = threeBackgrounds();
  ppu.tm = 0x04u;
  putEntry(ppu, kBg3, 1u, 0x0001u);  // the tile at (1, 0)
  putSolidTile(ppu, kBg3, 1u, 2u);
  ppu.bg3hofs = 8u;
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.frames, 1u);
  EXPECT_EQ(picture.at(0u, 0u), kGreenOut);
}

TEST(SnesPpuPicture, Bg3TakesItsTileSizeFromItsOwnBit) {
  // $2105 bit 6 is BG3's, and a block of four-colour characters runs on by one and
  // sixteen as any other does.
  PpuState ppu = threeBackgrounds();
  ppu.tm = 0x04u;
  ppu.bgmode = 0x41u;  // Mode 1, BG3 in 16x16 blocks
  putEntry(ppu, kBg3, 0u, 0x0001u);
  putSolidTile(ppu, kBg3, 1u, 1u);
  putSolidTile(ppu, kBg3, 18u, 2u);
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.frames, 1u);
  EXPECT_EQ(picture.at(0u, 0u), kRedOut);
  EXPECT_EQ(picture.at(8u, 8u), kGreenOut);
}

TEST(SnesPpuPicture, Bg3IsShownByItsOwnMainScreenBit) {
  PpuState ppu = threeBackgrounds();
  ppu.tm = 0x00u;
  ppu.ts = 0x04u;
  putEntry(ppu, kBg3, 0u, 0x0001u);
  putSolidTile(ppu, kBg3, 1u, 1u);
  const Picture hidden = draw(ppu);
  ASSERT_EQ(hidden.frames, 1u);
  EXPECT_EQ(hidden.at(0u, 0u), kBackdropOut);

  ppu.tm = 0x04u;
  const Picture shown = draw(ppu);
  ASSERT_EQ(shown.frames, 1u);
  EXPECT_EQ(shown.at(0u, 0u), kRedOut);
}

TEST(SnesPpuPicture, Mode1GivesNoBackgroundPalettesOfItsOwn) {
  // Where Mode 0 starts each background's palettes 32 words apart, Mode 1 starts
  // all three at word 0: BG3's palette 1 colour 1 and BG1's palette 0 colour 5 are
  // one and the same word.
  PpuState third = threeBackgrounds();
  third.tm = 0x04u;
  putEntry(third, kBg3, 0u, static_cast<std::uint16_t>((1u << 10) | 1u));
  putSolidTile(third, kBg3, 1u, 1u);
  const Picture fourColour = draw(third);
  ASSERT_EQ(fourColour.frames, 1u);
  EXPECT_EQ(fourColour.at(0u, 0u), kCyanOut);

  PpuState first = threeBackgrounds();
  first.tm = 0x01u;
  putEntry(first, kBg1, 0u, 0x0001u);
  putSolidTile(first, kBg1, 1u, 5u);
  const Picture sixteenColour = draw(first);
  ASSERT_EQ(sixteenColour.frames, 1u);
  EXPECT_EQ(sixteenColour.at(0u, 0u), kCyanOut);
}

// ---- the priority chart --------------------------------------------------------

TEST(SnesPpuPicture, BothTilePrioritiesShowOverTheBackdrop) {
  PpuState ppu = screen();
  putEntry(ppu, kBg1, 0u, 0x2001u);  // bit 13: tile priority 1
  putEntry(ppu, kBg1, 1u, 0x0002u);  // the tile beside it at priority 0
  putSolidTile(ppu, kBg1, 1u, 1u);
  putSolidTile(ppu, kBg1, 2u, 2u);
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.frames, 1u);
  EXPECT_EQ(picture.at(0u, 0u), kRedOut);
  EXPECT_EQ(picture.at(8u, 0u), kGreenOut);
}

TEST(SnesPpuPicture, Bg1CoversBg2AtTheSameTilePriority) {
  PpuState ppu = threeBackgrounds();
  putEntry(ppu, kBg1, 0u, 0x0001u);
  putEntry(ppu, kBg2, 0u, 0x0001u);
  putSolidTile(ppu, kBg1, 1u, 1u);
  putSolidTile(ppu, kBg2, 1u, 2u);
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.frames, 1u);
  EXPECT_EQ(picture.at(0u, 0u), kRedOut);
}

TEST(SnesPpuPicture, HighPriorityBg2CoversLowPriorityBg1) {
  // The chart runs A B a b, so B is in front of a: both of BG2's priorities are not
  // simply behind both of BG1's.
  PpuState ppu = threeBackgrounds();
  putEntry(ppu, kBg1, 0u, 0x0001u);  // BG1 at tile priority 0
  putEntry(ppu, kBg2, 0u, 0x2001u);  // BG2 at tile priority 1
  putSolidTile(ppu, kBg1, 1u, 1u);
  putSolidTile(ppu, kBg2, 1u, 2u);
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.frames, 1u);
  EXPECT_EQ(picture.at(0u, 0u), kGreenOut);
}

TEST(SnesPpuPicture, HighPriorityBg1CoversHighPriorityBg2) {
  PpuState ppu = threeBackgrounds();
  putEntry(ppu, kBg1, 0u, 0x2001u);
  putEntry(ppu, kBg2, 0u, 0x2001u);
  putSolidTile(ppu, kBg1, 1u, 1u);
  putSolidTile(ppu, kBg2, 1u, 2u);
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.frames, 1u);
  EXPECT_EQ(picture.at(0u, 0u), kRedOut);
}

TEST(SnesPpuPicture, LowPriorityBg2CoversBg3sHighPriorityTiles) {
  // C sits behind b in the chart, so BG3's high-priority tiles are behind even the
  // low-priority tiles of the other two.
  PpuState ppu = threeBackgrounds();
  ppu.tm = 0x06u;  // BG2 and BG3
  putEntry(ppu, kBg2, 0u, 0x0001u);  // BG2 at tile priority 0
  putEntry(ppu, kBg3, 0u, 0x2001u);  // BG3 at tile priority 1
  putSolidTile(ppu, kBg2, 1u, 2u);
  putSolidTile(ppu, kBg3, 1u, 1u);
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.frames, 1u);
  EXPECT_EQ(picture.at(0u, 0u), kGreenOut);
}

TEST(SnesPpuPicture, TheBg3PriorityBitPutsItsHighTilesInFrontOfEverything) {
  // $2105 bit 3 exchanges one chart for the other: C moves from behind b to in
  // front of A.
  PpuState ppu = threeBackgrounds();
  putEntry(ppu, kBg1, 0u, 0x2001u);  // BG1 at tile priority 1
  putEntry(ppu, kBg3, 0u, 0x2001u);  // BG3 at tile priority 1
  putSolidTile(ppu, kBg1, 1u, 1u);
  putSolidTile(ppu, kBg3, 1u, 2u);

  const Picture behind = draw(ppu);
  ASSERT_EQ(behind.frames, 1u);
  EXPECT_EQ(behind.at(0u, 0u), kRedOut);

  ppu.bgmode = 0x09u;  // Mode 1 with BG3's priority bit set
  const Picture front = draw(ppu);
  ASSERT_EQ(front.frames, 1u);
  EXPECT_EQ(front.at(0u, 0u), kGreenOut);
}

TEST(SnesPpuPicture, Bg3sLowPriorityTilesStayLastUnderEitherChart) {
  // Only C moves; c is the background nearest the backdrop either way.
  PpuState ppu = threeBackgrounds();
  ppu.tm = 0x06u;
  putEntry(ppu, kBg2, 0u, 0x0001u);
  putEntry(ppu, kBg3, 0u, 0x0001u);
  putSolidTile(ppu, kBg2, 1u, 2u);
  putSolidTile(ppu, kBg3, 1u, 1u);

  const Picture normal = draw(ppu);
  ASSERT_EQ(normal.frames, 1u);
  EXPECT_EQ(normal.at(0u, 0u), kGreenOut);

  ppu.bgmode = 0x09u;
  const Picture raised = draw(ppu);
  ASSERT_EQ(raised.frames, 1u);
  EXPECT_EQ(raised.at(0u, 0u), kGreenOut);
}

TEST(SnesPpuPicture, ATransparentPixelFallsThroughToWhatIsBehindIt) {
  // Colour 0 is transparent on every background, so BG2 shows through BG1's hole
  // and the backdrop through both.
  PpuState ppu = threeBackgrounds();
  TileRows front{};
  front[0][0] = 1u;
  putEntry(ppu, kBg1, 0u, 0x0001u);
  putTile(ppu, kBg1, 1u, front);
  TileRows behind{};
  behind[0][0] = 2u;
  behind[0][1] = 2u;
  putEntry(ppu, kBg2, 0u, 0x0001u);
  putTile(ppu, kBg2, 1u, behind);
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.frames, 1u);
  EXPECT_EQ(picture.at(0u, 0u), kRedOut);
  EXPECT_EQ(picture.at(1u, 0u), kGreenOut);
  EXPECT_EQ(picture.at(2u, 0u), kBackdropOut);
}

// ---- what this block does not draw -------------------------------------------

TEST(SnesPpuPicture, ALayerEnabledOnlyOnTheSubScreenDrawsNothing) {
  PpuState ppu = screen();
  ppu.tm = 0x00u;
  ppu.ts = 0x01u;  // BG1 on the sub screen alone
  putEntry(ppu, kBg1, 0u, 0x0001u);
  putSolidTile(ppu, kBg1, 1u, 1u);
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

// ---- a write landing inside a tile ---------------------------------------------
//
// A pixel is resolved from the registers and the memories as they stand at its own
// dot, so a write landing between two positions of the same tile shows at the
// second of them and not the first. Each case below begins its program at master
// cycle 300 of line 50 — picture row 49, whose first fifty-three positions the beam
// had already passed — and every write it makes lands inside a tile rather than on
// a boundary: a program's first store pair lands at column 65, the third position of
// the tile at 64, and its second at column 77, the sixth position of the tile at 72.
// Each case compares a position drawn before the landing with the frame the same
// picture draws with the write never made, and one drawn after it with the frame
// that has the written value throughout.

TEST(SnesPpuPicture, TheHorizontalScrollWrittenInsideATileMovesThePixelsAfterTheWrite) {
  // BG1HOFS <- 8 in its two halves, so the positions after the second read the
  // tile column to their right.
  const PpuState before = columnCyclingPicture();
  PpuState after = columnCyclingPicture();
  after.bg1hofs = 8u;
  const Picture never = draw(before);
  const Picture always = draw(after);
  const Picture picture =
      drawWith(before, joined({storePort(0x0Du, 0x08u), storePort(0x0Du, 0x00u)}), 50u, 300u);
  ASSERT_EQ(picture.frames, 1u);
  EXPECT_NE(never.at(60u, 49u), always.at(60u, 49u));
  EXPECT_EQ(picture.at(60u, 49u), never.at(60u, 49u));
  EXPECT_EQ(picture.at(78u, 49u), always.at(78u, 49u));
  EXPECT_NE(never.at(90u, 49u), always.at(90u, 49u));
  EXPECT_EQ(picture.at(90u, 49u), always.at(90u, 49u));
  EXPECT_EQ(picture.at(10u, 49u), kBlack);
}

TEST(SnesPpuPicture, TheVerticalScrollWrittenInsideATileMovesTheRowTheRestOfTheLineReads) {
  // BG1VOFS <- 1. The line's positions hold the same tilemap entry either way —
  // the offset moves the row within the character, not the character.
  const PpuState before = rowCyclingPicture();
  PpuState after = rowCyclingPicture();
  after.bg1vofs = 1u;
  const Picture never = draw(before);
  const Picture always = draw(after);
  const Picture picture =
      drawWith(before, joined({storePort(0x0Eu, 0x01u), storePort(0x0Eu, 0x00u)}), 50u, 300u);
  ASSERT_EQ(picture.frames, 1u);
  EXPECT_NE(never.at(60u, 49u), always.at(60u, 49u));
  EXPECT_EQ(picture.at(60u, 49u), never.at(60u, 49u));
  EXPECT_EQ(picture.at(78u, 49u), always.at(78u, 49u));
  EXPECT_EQ(picture.at(90u, 49u), always.at(90u, 49u));
}

TEST(SnesPpuPicture, TheCharacterBaseWrittenInsideATileMovesTheRestOfTheLineToTheOtherCharacters) {
  // $210B <- 6: BG1's characters move from $8000 to $C000, where a second character
  // 1 stands in another colour. Every position keeps its entry and its tile number.
  PpuState before = oneCharacterEverywhere();
  putTileAt(before, 0xC000u + 0x20u, 4u, [] {
    TileRows rows{};
    for (auto& row : rows) row.fill(2u);
    return rows;
  }());
  PpuState after = before;
  after.bg12nba = 0x06u;
  const Picture never = draw(before);
  const Picture always = draw(after);
  const Picture picture = drawWith(before, storePort(0x0Bu, 0x06u), 50u, 300u);
  ASSERT_EQ(picture.frames, 1u);
  EXPECT_NE(never.at(60u, 49u), always.at(60u, 49u));
  EXPECT_EQ(picture.at(60u, 49u), never.at(60u, 49u));
  EXPECT_EQ(picture.at(70u, 49u), always.at(70u, 49u));
  EXPECT_EQ(picture.at(80u, 49u), always.at(80u, 49u));
}

TEST(SnesPpuPicture, TheScreenBaseWrittenInsideATileMovesTheRestOfTheLineToTheOtherMap) {
  // $2107 <- 8: BG1's map moves from screen 1 to screen 2, whose entries name
  // another character. Every position keeps the row and column it reads the map at.
  constexpr Layout kSecondMap{.map = 0x1000u, .characters = kCharByte, .planes = 4u};
  PpuState before = oneCharacterEverywhere();
  putSolidTile(before, kBg1, 2u, 2u);
  for (unsigned index = 0u; index < kScreenEntries; ++index) {
    putEntry(before, kSecondMap, index, 0x0002u);
  }
  PpuState after = before;
  after.bg1sc = 0x08u;
  const Picture never = draw(before);
  const Picture always = draw(after);
  const Picture picture = drawWith(before, storePort(0x07u, 0x08u), 50u, 300u);
  ASSERT_EQ(picture.frames, 1u);
  EXPECT_NE(never.at(60u, 49u), always.at(60u, 49u));
  EXPECT_EQ(picture.at(60u, 49u), never.at(60u, 49u));
  EXPECT_EQ(picture.at(70u, 49u), always.at(70u, 49u));
  EXPECT_EQ(picture.at(80u, 49u), always.at(80u, 49u));
}

TEST(SnesPpuPicture, TheTileSizeWrittenInsideATileChangesTheTilesAfterTheWrite) {
  // $2105 <- $11: Mode 1 with BG1 in 16x16 blocks, so the positions after the write
  // read the map at half the row and column they read it at before.
  const PpuState before = columnCyclingPicture();
  PpuState after = columnCyclingPicture();
  after.bgmode = 0x11u;
  const Picture never = draw(before);
  const Picture always = draw(after);
  const Picture picture = drawWith(before, storePort(0x05u, 0x11u), 50u, 300u);
  ASSERT_EQ(picture.frames, 1u);
  EXPECT_NE(never.at(60u, 49u), always.at(60u, 49u));
  EXPECT_EQ(picture.at(60u, 49u), never.at(60u, 49u));
  EXPECT_EQ(picture.at(70u, 49u), always.at(70u, 49u));
  EXPECT_NE(never.at(80u, 49u), always.at(80u, 49u));
  EXPECT_EQ(picture.at(80u, 49u), always.at(80u, 49u));
}

TEST(SnesPpuPicture, ABackgroundSeenThroughOneTileColumnReadsItsOwnRowOnEveryLine) {
  // The windows leave BG1 showing at four positions of one tile column and nowhere
  // else, and its map names a different character in every tile row. Every line
  // reads the map at its own row, though the only positions any line reads it at
  // are the four inside that one column — the row is as much a part of where an
  // entry is read as the column is.
  PpuState ppu = screen();
  for (unsigned index = 0u; index < kScreenEntries; ++index) {
    putEntry(ppu, kBg1, index, static_cast<std::uint16_t>((index / 32u) % 4u + 1u));
  }
  for (unsigned tile = 1u; tile <= 4u; ++tile) putSolidTile(ppu, kBg1, tile, kCycle[tile - 1u]);
  ppu.w12sel = 0x03u;  // BG1: window 1 enabled and inverted, so it masks outside it
  ppu.wh0 = 100u;
  ppu.wh1 = 103u;
  ppu.tmw = 0x01u;  // and the windows mask BG1 on the main screen
  const Picture picture = draw(ppu);
  ASSERT_EQ(picture.frames, 1u);
  EXPECT_EQ(picture.at(99u, 0u), kBackdropOut);
  EXPECT_EQ(picture.at(104u, 0u), kBackdropOut);
  // Tile rows 0, 1, 2 and 3 name the four characters, and each holds eight lines.
  EXPECT_EQ(picture.at(100u, 0u), kRedOut);
  EXPECT_EQ(picture.at(103u, 7u), kRedOut);
  EXPECT_EQ(picture.at(100u, 8u), kGreenOut);
  EXPECT_EQ(picture.at(100u, 16u), kBlueOut);
  EXPECT_EQ(picture.at(100u, 24u), kWhiteOut);
}

TEST(SnesPpuPicture, AVideoMemoryWriteUnderForcedBlankReachesTheSameLineWhenTheBlankLifts) {
  // Forced blank at column 65, the row line 50 reads exchanged through the video
  // port while the memory is reachable, and the blank lifted at column 134 — all on
  // one line. The positions the blank covered are black and the ones after it read
  // the row the program wrote, at the same address the positions before it read.
  const PpuState before = oneCharacterEverywhere();
  PpuState after = before;
  after.vram[kTileOneRowAtLineFifty] = 0x00u;
  after.vram[kTileOneRowAtLineFifty + 1u] = 0xFFu;
  const Picture never = draw(before);
  const Picture always = draw(after);
  const Picture picture = drawWith(before,
                                   joined({
                                       storePort(0x00u, 0x80u),  // forced blank
                                       storePort(0x15u, 0x80u),  // step after the high byte
                                       storePort(0x16u, kTileOneRowWord & 0xFFu),
                                       storePort(0x17u, kTileOneRowWord >> 8),
                                       storePort(0x18u, 0x00u),  // plane 0 clear
                                       storePort(0x19u, 0xFFu),  // plane 1 set
                                       storePort(0x00u, 0x0Fu),  // the screen on again
                                   }),
                                   50u, 300u);
  ASSERT_EQ(picture.frames, 1u);
  EXPECT_NE(never.at(60u, 49u), always.at(60u, 49u));
  EXPECT_EQ(picture.at(60u, 49u), never.at(60u, 49u));
  EXPECT_EQ(picture.at(100u, 49u), kBlack);
  EXPECT_EQ(picture.at(140u, 49u), always.at(140u, 49u));
}

TEST(SnesPpuPicture, ASnapshotRestoredInsideATileDrawsFromTheRestoredMemories) {
  // The beam is taken to master cycle 482, inside the position at column 98 and so
  // inside the tile the position at column 96 began; the state is taken, the row
  // line 50 reads exchanged in the copy, and the copy restored. The rest of the
  // line reads the copy's bytes at the address the positions before it read.
  const PpuState before = oneCharacterEverywhere();
  PpuState after = before;
  after.vram[kTileOneRowAtLineFifty] = 0x00u;
  after.vram[kTileOneRowAtLineFifty + 1u] = 0xFFu;
  const Picture never = draw(before);
  const Picture always = draw(after);

  const std::vector<std::uint8_t> rom = haltedCartridge();
  Snes machine(SnesConfig{.rom = rom});
  SnesState state = machine.state();
  state.ppu = before;
  state.vpos = 50u;
  state.hpos = 300u;
  machine.restore(state);
  Picture picture;
  machine.setFrameObserver(&picture);
  machine.run(180u);
  ASSERT_EQ(machine.state().hpos, 482u);
  SnesState midTile = machine.state();
  midTile.ppu.vram[kTileOneRowAtLineFifty] = 0x00u;
  midTile.ppu.vram[kTileOneRowAtLineFifty + 1u] = 0xFFu;
  machine.restore(midTile);
  machine.run(kOneFrame);

  ASSERT_EQ(picture.frames, 1u);
  EXPECT_NE(never.at(60u, 49u), always.at(60u, 49u));
  EXPECT_EQ(picture.at(60u, 49u), never.at(60u, 49u));
  EXPECT_EQ(picture.at(110u, 49u), always.at(110u, 49u));
}

// ---- a write landing part-way along a line -------------------------------------
//
// What a layer is — how deep its characters are, where in the palette its colours
// begin, which map and which characters it reads, and where it stands in the
// chart — the chip reads from its registers at the dot it draws. A write landing
// part-way along a line divides that line: the positions before it are drawn at
// what the registers held, the positions after it at what they hold. Each case
// below begins its program at master cycle 300 of line 50, where a first store pair
// lands at column 65, and compares a position before the landing against the frame
// the write was never made in and two after it — the next position inside the same
// tile, and one further along — against the frame carrying the written value
// throughout.

// A picture of one character in palette 1 whose pixels hold colour index 5. Four
// bitplanes read that as index 5 and two read the low pair alone, which is index 1;
// Mode 1 gives BG1 sixteen colours at a palette stride of sixteen, so palette 1's
// index 5 is word 21, and Mode 0 gives it four at a stride of four, so the same
// pixel is word 5.
PpuState fiveInPaletteOne() {
  PpuState ppu = screen();
  for (unsigned index = 0u; index < kScreenEntries; ++index) {
    putEntry(ppu, kBg1, index, 0x0401u);  // palette 1, character 1
  }
  putSolidTile(ppu, kBg1, 1u, 5u);
  putColour(ppu, 21u, kGreen);
  return ppu;
}

TEST(SnesPpuPicture, TheModeWrittenPartWayAlongALineChangesTheDepthTheRestOfTheLineReads) {
  // $2105 <- 0: Mode 0, where BG1 is a four-colour background at a palette stride of
  // four. The positions after the landing read two bitplanes and word 4 + index
  // where the ones before them read four and word 16 + index.
  const PpuState before = fiveInPaletteOne();
  PpuState after = fiveInPaletteOne();
  after.bgmode = 0x00u;
  const Picture never = draw(before);
  const Picture always = draw(after);
  const Picture picture = drawWith(before, storePort(0x05u, 0x00u), 50u, 300u);
  ASSERT_EQ(picture.frames, 1u);
  EXPECT_NE(never.at(60u, 49u), always.at(60u, 49u));
  EXPECT_NE(never.at(66u, 49u), always.at(66u, 49u));
  EXPECT_NE(never.at(80u, 49u), always.at(80u, 49u));
  EXPECT_EQ(picture.at(60u, 49u), never.at(60u, 49u));
  EXPECT_EQ(picture.at(66u, 49u), always.at(66u, 49u));
  EXPECT_EQ(picture.at(80u, 49u), always.at(80u, 49u));
}

TEST(SnesPpuPicture, TheSecondBackgroundsScreenBaseWrittenPartWayAlongALineMovesItToTheOtherMap) {
  // $2108 <- 8: BG2's map moves from screen 8 to screen 2, whose entries name
  // another character. BG2 is the only layer on the main screen, and every position
  // keeps the row and column it reads the map at.
  constexpr Layout kBg2SecondMap{.map = 0x1000u, .characters = 0xA000u, .planes = 4u};
  PpuState before = threeBackgrounds();
  before.tm = 0x02u;
  for (unsigned index = 0u; index < kScreenEntries; ++index) {
    putEntry(before, kBg2, index, 0x0001u);
    putEntry(before, kBg2SecondMap, index, 0x0002u);
  }
  putSolidTile(before, kBg2, 1u, 1u);
  putSolidTile(before, kBg2, 2u, 2u);
  PpuState after = before;
  after.bg2sc = 0x08u;
  const Picture never = draw(before);
  const Picture always = draw(after);
  const Picture picture = drawWith(before, storePort(0x08u, 0x08u), 50u, 300u);
  ASSERT_EQ(picture.frames, 1u);
  EXPECT_NE(never.at(60u, 49u), always.at(60u, 49u));
  EXPECT_NE(never.at(66u, 49u), always.at(66u, 49u));
  EXPECT_NE(never.at(80u, 49u), always.at(80u, 49u));
  EXPECT_EQ(picture.at(60u, 49u), never.at(60u, 49u));
  EXPECT_EQ(picture.at(66u, 49u), always.at(66u, 49u));
  EXPECT_EQ(picture.at(80u, 49u), always.at(80u, 49u));
}

TEST(SnesPpuPicture,
     TheThirdBackgroundsCharacterBaseWrittenPartWayAlongALineMovesItToTheOtherCharacters) {
  // $210C <- 7: BG3's characters move from $C000 to $E000, where a second character 1
  // stands in another colour. BG3 is the only layer on the main screen, and every
  // position keeps its entry and its tile number.
  PpuState before = threeBackgrounds();
  before.tm = 0x04u;
  for (unsigned index = 0u; index < kScreenEntries; ++index) {
    putEntry(before, kBg3, index, 0x0401u);  // palette 1, character 1
  }
  putSolidTile(before, kBg3, 1u, 1u);  // BG3's palette 1 colour 1 is word 5
  putTileAt(before, 0xE000u + 1u * 8u * kBg3.planes, kBg3.planes, [] {
    TileRows rows{};
    for (auto& row : rows) row.fill(2u);  // and its colour 2 is word 6
    return rows;
  }());
  putColour(before, 6u, kYellow);
  PpuState after = before;
  after.bg34nba = 0x07u;
  const Picture never = draw(before);
  const Picture always = draw(after);
  const Picture picture = drawWith(before, storePort(0x0Cu, 0x07u), 50u, 300u);
  ASSERT_EQ(picture.frames, 1u);
  EXPECT_NE(never.at(60u, 49u), always.at(60u, 49u));
  EXPECT_NE(never.at(66u, 49u), always.at(66u, 49u));
  EXPECT_NE(never.at(80u, 49u), always.at(80u, 49u));
  EXPECT_EQ(picture.at(60u, 49u), never.at(60u, 49u));
  EXPECT_EQ(picture.at(66u, 49u), always.at(66u, 49u));
  EXPECT_EQ(picture.at(80u, 49u), always.at(80u, 49u));
}

TEST(SnesPpuPicture, AVideoMemoryHighByteWrittenUnderForcedBlankReachesTheSameLineWhenTheBlankLifts) {
  // Forced blank at column 65, the high byte of the word line 50 reads written
  // through $2119 alone while the memory is reachable, and the blank lifted — all on
  // one line. The row's second bitplane is set where its first already is, so the
  // positions after the blank read colour index 3 where the ones before it read 1.
  PpuState before = oneCharacterEverywhere();
  putColour(before, 3u, kYellow);
  PpuState after = before;
  after.vram[kTileOneRowAtLineFifty + 1u] = 0xFFu;
  const Picture never = draw(before);
  const Picture always = draw(after);
  const Picture picture = drawWith(before,
                                   joined({
                                       storePort(0x00u, 0x80u),  // forced blank
                                       storePort(0x15u, 0x80u),  // step after the high byte
                                       storePort(0x16u, kTileOneRowWord & 0xFFu),
                                       storePort(0x17u, kTileOneRowWord >> 8),
                                       storePort(0x19u, 0xFFu),  // plane 1 set
                                       storePort(0x00u, 0x0Fu),  // the screen on again
                                   }),
                                   50u, 300u);
  ASSERT_EQ(picture.frames, 1u);
  EXPECT_NE(never.at(60u, 49u), always.at(60u, 49u));
  EXPECT_NE(never.at(140u, 49u), always.at(140u, 49u));
  EXPECT_EQ(picture.at(60u, 49u), never.at(60u, 49u));
  EXPECT_EQ(picture.at(100u, 49u), kBlack);
  EXPECT_EQ(picture.at(140u, 49u), always.at(140u, 49u));
}

TEST(SnesPpuPicture, ASnapshotRestoredPartWayAlongALineDrawsAtTheRestoredMode) {
  // The beam is taken to master cycle 482, inside the position at column 98; the
  // state is taken, the mode changed in the copy, and the copy restored. The rest of
  // the line is drawn at the copy's mode.
  const PpuState before = fiveInPaletteOne();
  PpuState after = fiveInPaletteOne();
  after.bgmode = 0x00u;
  const Picture never = draw(before);
  const Picture always = draw(after);

  const std::vector<std::uint8_t> rom = haltedCartridge();
  Snes machine(SnesConfig{.rom = rom});
  SnesState state = machine.state();
  state.ppu = before;
  state.vpos = 50u;
  state.hpos = 300u;
  machine.restore(state);
  Picture picture;
  machine.setFrameObserver(&picture);
  machine.run(180u);
  ASSERT_EQ(machine.state().hpos, 482u);
  SnesState midLine = machine.state();
  midLine.ppu.bgmode = 0x00u;
  machine.restore(midLine);
  machine.run(kOneFrame);

  ASSERT_EQ(picture.frames, 1u);
  EXPECT_NE(never.at(60u, 49u), always.at(60u, 49u));
  EXPECT_NE(never.at(110u, 49u), always.at(110u, 49u));
  EXPECT_EQ(picture.at(60u, 49u), never.at(60u, 49u));
  EXPECT_EQ(picture.at(110u, 49u), always.at(110u, 49u));
}

}  // namespace
}  // namespace snaggletooth
