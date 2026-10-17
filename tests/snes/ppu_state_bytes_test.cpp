// The PPU's state as bytes: the layout's size, the three memories and the
// register file before the sprite line, the last main decision and the tile
// reads, the round trip both ways, the two encode forms, and the bytes the
// decoder refuses. A filled state — every field set from a counter that never
// repeats a byte inside a field and never writes zero, every `bool` true — is
// what reaches an omitted, swapped or misplaced field; a default state is mostly
// zeros and reaches none of them.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <vector>

#include "gtest/gtest.h"
#include "snaggletooth/snes/ppu.h"

namespace snaggletooth {
namespace {

// The layout's figure: 66684 bytes of the chip's own fields, the 549-byte sprite
// line, the 7-byte last main decision and 132 tile reads of 26 bytes.
constexpr std::size_t kLayoutBytes = 70672u;
constexpr std::size_t kSpritesAt = 66684u;
constexpr std::size_t kLastMainAt = 67233u;
constexpr std::size_t kTilesAt = 67240u;
constexpr std::size_t kTileBytes = 26u;

// The FNV-1a 64 digest of the filled state's bytes. Every platform builds the
// same bytes from the same state, so every platform reads this one value.
constexpr std::uint64_t kFilledDigest = 0xB07E3A2BF4460664ull;

std::uint64_t digest(std::span<const std::uint8_t> bytes) {
  std::uint64_t hash = 0xCBF29CE484222325ull;
  for (const std::uint8_t byte : bytes) {
    hash ^= byte;
    hash *= 0x100000001B3ull;
  }
  return hash;
}

// Hands out the bytes 1 to 255 in turn, then 1 again, and builds 16-bit values
// from consecutive bytes, low byte first.
class Counter {
 public:
  std::uint8_t u8() {
    next_ = static_cast<std::uint8_t>(next_ == 0xFFu ? 1u : next_ + 1u);
    return next_;
  }
  std::uint16_t u16() {
    const std::uint16_t low = u8();
    const std::uint16_t high = u8();
    return static_cast<std::uint16_t>(low | (high << 8));
  }

 private:
  std::uint8_t next_ = 0u;
};

PpuState filled() {
  Counter c;
  PpuState s;
  for (std::uint8_t& byte : s.vram) byte = c.u8();
  for (std::uint8_t& byte : s.cgram) byte = c.u8();
  for (std::uint8_t& byte : s.oam) byte = c.u8();
  for (std::uint8_t& word : s.sprites.word) word = c.u8();
  for (std::uint8_t& priority : s.sprites.priority) priority = c.u8();
  s.sprites.line = c.u16();
  for (std::uint8_t& sprite : s.sprites.inRange) sprite = c.u8();
  s.sprites.found = c.u8();
  s.sprites.scanned = c.u8();
  s.sprites.first = c.u8();
  s.inidisp = c.u8();
  s.objsel = c.u8();
  s.bgmode = c.u8();
  s.mosaic = c.u8();
  s.bg1sc = c.u8();
  s.bg2sc = c.u8();
  s.bg3sc = c.u8();
  s.bg4sc = c.u8();
  s.bg12nba = c.u8();
  s.bg34nba = c.u8();
  s.setini = c.u8();
  s.bg1hofs = c.u16();
  s.bg1vofs = c.u16();
  s.bg2hofs = c.u16();
  s.bg2vofs = c.u16();
  s.bg3hofs = c.u16();
  s.bg3vofs = c.u16();
  s.bg4hofs = c.u16();
  s.bg4vofs = c.u16();
  s.bgLatch = c.u8();
  s.m7sel = c.u8();
  s.m7a = c.u16();
  s.m7b = c.u16();
  s.m7c = c.u16();
  s.m7d = c.u16();
  s.m7x = c.u16();
  s.m7y = c.u16();
  s.m7hofs = c.u16();
  s.m7vofs = c.u16();
  s.m7Latch = c.u8();
  s.m7bByte = c.u8();
  s.vmain = c.u8();
  s.vmadd = c.u16();
  s.vramLatch = c.u16();
  s.cgadd = c.u8();
  s.cgLatchHigh = true;
  s.cgLatch = c.u8();
  s.oamadd = c.u16();
  s.oamAddress = c.u16();
  s.oamLatch = c.u8();
  s.w12sel = c.u8();
  s.w34sel = c.u8();
  s.wobjsel = c.u8();
  s.wh0 = c.u8();
  s.wh1 = c.u8();
  s.wh2 = c.u8();
  s.wh3 = c.u8();
  s.wbglog = c.u8();
  s.wobjlog = c.u8();
  s.tm = c.u8();
  s.ts = c.u8();
  s.tmw = c.u8();
  s.tsw = c.u8();
  s.cgwsel = c.u8();
  s.cgadsub = c.u8();
  s.fixedRed = c.u8();
  s.fixedGreen = c.u8();
  s.fixedBlue = c.u8();
  s.ophct = c.u16();
  s.opvct = c.u16();
  s.ophctHigh = true;
  s.opvctHigh = true;
  s.countersLatched = true;
  s.mosaicBlockLine = c.u16();
  s.mosaicBlockSize = c.u8();
  s.lastMain.present = true;
  s.lastMain.clipped = true;
  s.lastMain.addend = c.u8();
  s.lastMain.subtract = true;
  s.lastMain.halved = true;
  s.lastMain.colour = c.u16();
  for (std::array<TileRead, kTileColumns>& background : s.tiles) {
    for (TileRead& tile : background) {
      for (std::uint8_t& pixel : tile.pixel) pixel = c.u8();
      tile.entry = c.u16();
      tile.bgX = c.u16();
      tile.bgY = c.u16();
      tile.planes = c.u8();
      tile.fine = c.u8();
      tile.large = true;
      tile.hires = true;
    }
  }
  s.rangeOver = true;
  s.timeOver = true;
  s.ppu1Bus = c.u8();
  s.ppu2Bus = c.u8();
  return s;
}

std::uint16_t word16(const std::vector<std::uint8_t>& bytes, std::size_t at) {
  return static_cast<std::uint16_t>(bytes[at] | (bytes[at + 1u] << 8));
}

TEST(PpuStateBytes, ByteSizeIsTheLayouts) {
  static_assert(PpuState::byteSize() == kLayoutBytes);
  static_assert(kTilesAt + 4u * kTileColumns * kTileBytes == kLayoutBytes);
  EXPECT_EQ(PpuState::byteSize(), kLayoutBytes);
  EXPECT_EQ(filled().asBytes().size(), kLayoutBytes);
}

TEST(PpuStateBytes, ADefaultStateRoundTrips) {
  const PpuState original;
  const std::optional<PpuState> decoded = PpuState::fromBytes(original.asBytes());
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(*decoded, original);
}

TEST(PpuStateBytes, AFilledStateRoundTrips) {
  const PpuState original = filled();
  const std::vector<std::uint8_t> bytes = original.asBytes();

  const std::optional<PpuState> decoded = PpuState::fromBytes(bytes);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(*decoded, original);

  PpuState into;
  ASSERT_TRUE(PpuState::fromBytes(bytes, into));
  EXPECT_EQ(into, original);
}

TEST(PpuStateBytes, FieldsSitWhereTheLayoutSays) {
  PpuState state = filled();
  state.vram[0] = 0x01u;
  state.vram[0xFFFF] = 0x02u;
  state.cgram[0] = 0x03u;
  state.oam[0] = 0x04u;
  state.inidisp = 0x8Fu;
  state.bg1hofs = 0x03FFu;
  state.m7a = 0xFF00u;
  state.cgLatchHigh = false;
  state.oamAddress = 0x021Fu;
  state.fixedBlue = 0x1Fu;
  state.ophct = 0x0155u;
  state.countersLatched = true;
  state.mosaicBlockLine = 0x00E1u;
  state.ppu2Bus = 0x5Au;
  state.sprites.word[0] = 0x80u;
  state.sprites.line = 0x00EFu;
  state.sprites.first = 0x7Fu;
  state.lastMain.present = true;
  state.lastMain.colour = 0x7C1Fu;
  state.tiles[0][0].pixel[0] = 0x0Au;
  state.tiles[0][1].pixel[0] = 0x0Bu;
  state.tiles[1][0].entry = 0x4321u;
  state.tiles[3][32].hires = false;
  const std::vector<std::uint8_t> bytes = state.asBytes();
  ASSERT_EQ(bytes.size(), kLayoutBytes);

  // The three memories first, VRAM, the palette, the sprite table.
  EXPECT_EQ(bytes[0], 0x01u);
  EXPECT_EQ(bytes[0xFFFF], 0x02u);
  EXPECT_EQ(bytes[65536], 0x03u);
  EXPECT_EQ(bytes[66048], 0x04u);
  // The register file in declaration order, 16-bit values low byte first.
  EXPECT_EQ(bytes[66592], 0x8Fu);
  EXPECT_EQ(word16(bytes, 66603), 0x03FFu);
  EXPECT_EQ(bytes[66603], 0xFFu);
  EXPECT_EQ(word16(bytes, 66621), 0xFF00u);
  EXPECT_EQ(bytes[66645], 0u);
  EXPECT_EQ(word16(bytes, 66649), 0x021Fu);
  EXPECT_EQ(bytes[66669], 0x1Fu);
  EXPECT_EQ(word16(bytes, 66670), 0x0155u);
  EXPECT_EQ(bytes[66676], 1u);
  EXPECT_EQ(word16(bytes, 66677), 0x00E1u);
  EXPECT_EQ(bytes[66683], 0x5Au);
  // The sprite line.
  EXPECT_EQ(bytes[kSpritesAt], 0x80u);
  EXPECT_EQ(word16(bytes, kSpritesAt + 512u), 0x00EFu);
  EXPECT_EQ(bytes[kSpritesAt + 548u], 0x7Fu);
  // The last main decision.
  EXPECT_EQ(bytes[kLastMainAt], 1u);
  EXPECT_EQ(word16(bytes, kLastMainAt + 5u), 0x7C1Fu);
  // The tile reads, BG1's columns first, then BG2's.
  EXPECT_EQ(bytes[kTilesAt], 0x0Au);
  EXPECT_EQ(bytes[kTilesAt + kTileBytes], 0x0Bu);
  EXPECT_EQ(word16(bytes, kTilesAt + kTileColumns * kTileBytes + 16u), 0x4321u);
  // The last byte is BG4's last column's hires flag.
  EXPECT_EQ(bytes[kLayoutBytes - 1u], 0u);
}

TEST(PpuStateBytes, TheFilledBytesHaveOneDigest) {
  EXPECT_EQ(digest(filled().asBytes()), kFilledDigest);
}

TEST(PpuStateBytes, TheAllocatingAndSpanFormsAgree) {
  const PpuState state = filled();
  const std::vector<std::uint8_t> allocated = state.asBytes();

  std::vector<std::uint8_t> exact(kLayoutBytes);
  EXPECT_EQ(state.asBytes(exact), kLayoutBytes);
  EXPECT_EQ(exact, allocated);

  std::vector<std::uint8_t> shortBuffer(kLayoutBytes - 1u, 0xA5u);
  EXPECT_EQ(state.asBytes(shortBuffer), 0u);
  EXPECT_TRUE(std::all_of(shortBuffer.begin(), shortBuffer.end(),
                          [](std::uint8_t byte) { return byte == 0xA5u; }));

  std::vector<std::uint8_t> longBuffer(kLayoutBytes + 1u, 0xA5u);
  EXPECT_EQ(state.asBytes(longBuffer), kLayoutBytes);
  EXPECT_TRUE(std::equal(longBuffer.begin(), longBuffer.begin() + kLayoutBytes,
                         allocated.begin(), allocated.end()));
  EXPECT_EQ(longBuffer[kLayoutBytes], 0xA5u);
}

TEST(PpuStateBytes, WrongSizeIsRefused) {
  std::vector<std::uint8_t> bytes = filled().asBytes();
  const PpuState before;

  const std::vector<std::uint8_t> shorter(bytes.begin(), bytes.end() - 1);
  EXPECT_FALSE(PpuState::fromBytes(shorter).has_value());
  PpuState intoShort = before;
  EXPECT_FALSE(PpuState::fromBytes(shorter, intoShort));
  EXPECT_EQ(intoShort, before);

  bytes.push_back(0u);
  EXPECT_FALSE(PpuState::fromBytes(bytes).has_value());
  PpuState intoLong = before;
  EXPECT_FALSE(PpuState::fromBytes(bytes, intoLong));
  EXPECT_EQ(intoLong, before);
}

TEST(PpuStateBytes, ABoolOrEnumOutOfRangeIsRefused) {
  const std::vector<std::uint8_t> good = filled().asBytes();
  const PpuState before;
  // A flag of the chip's own, the last main decision's first, and the last byte
  // of the last tile read.
  for (const std::size_t at : {std::size_t{66645u}, kLastMainAt, kLayoutBytes - 1u}) {
    std::vector<std::uint8_t> bad = good;
    bad[at] = 2u;
    EXPECT_FALSE(PpuState::fromBytes(bad).has_value()) << "at " << at;
    PpuState into = before;
    EXPECT_FALSE(PpuState::fromBytes(bad, into)) << "at " << at;
    EXPECT_EQ(into, before) << "at " << at;
  }
}

TEST(PpuStateBytes, AcceptedBytesAreCanonical) {
  const std::vector<std::uint8_t> bytes = filled().asBytes();
  const std::optional<PpuState> decoded = PpuState::fromBytes(bytes);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->asBytes(), bytes);
}

}  // namespace
}  // namespace snaggletooth
