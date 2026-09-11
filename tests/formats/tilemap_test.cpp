// The tilemap codec: BG map words round-trip through text, every flag survives,
// an entry over range is refused, and an odd trailing byte is kept.

#include <cstdint>
#include <string>
#include <vector>

#include "formats/tilemap.h"
#include "gtest/gtest.h"

namespace snaggletooth::formats {
namespace {

std::vector<std::uint8_t> wordsToBytes(const std::vector<unsigned>& words) {
  std::vector<std::uint8_t> out;
  for (unsigned w : words) {
    out.push_back(static_cast<std::uint8_t>(w & 0xFF));
    out.push_back(static_cast<std::uint8_t>((w >> 8) & 0xFF));
  }
  return out;
}

std::vector<std::uint8_t> roundTrip(const std::vector<std::uint8_t>& map) {
  const Text text = encodeTilemap(map);
  EXPECT_TRUE(text.ok()) << text.error;
  const Bytes back = decodeTilemap(text.text);
  EXPECT_TRUE(back.ok()) << back.error;
  return back.bytes;
}

TEST(Tilemap, EntriesRoundTrip) {
  const auto map = wordsToBytes({0x0000, 0x0123, 0x1555, 0x03FF});
  EXPECT_EQ(roundTrip(map), map);
}

TEST(Tilemap, EveryFlagSurvives) {
  const unsigned word = 0x123u | (5u << 10) | 0x2000u | 0x4000u | 0x8000u;  // tile,pal,P,H,V
  const auto map = wordsToBytes({word});
  const Text text = encodeTilemap(map);
  ASSERT_TRUE(text.ok());
  EXPECT_NE(text.text.find("123:5:PHV"), std::string::npos);
  EXPECT_EQ(roundTrip(map), map);
}

TEST(Tilemap, NoFlagsWriteADash) {
  const auto map = wordsToBytes({0x0042});
  const Text text = encodeTilemap(map);
  ASSERT_TRUE(text.ok());
  EXPECT_NE(text.text.find("042:0:-"), std::string::npos);
}

TEST(Tilemap, ScreensSeparateAndRoundTrip) {
  std::vector<unsigned> words;
  for (unsigned i = 0; i < 1056; ++i) words.push_back((i & 0x3FFu) | ((i & 7u) << 10));
  const auto map = wordsToBytes(words);
  const Text text = encodeTilemap(map);
  ASSERT_TRUE(text.ok());
  EXPECT_NE(text.text.find("\n\n"), std::string::npos);  // a blank line between screens
  EXPECT_EQ(roundTrip(map), map);
}

TEST(Tilemap, OddTrailingByteIsKept) {
  std::vector<std::uint8_t> map = wordsToBytes({0x0123});
  map.push_back(0xAB);
  EXPECT_EQ(roundTrip(map), map);
}

TEST(Tilemap, TileOutOfRangeIsRefused) {
  const Bytes back = decodeTilemap("500:0:-\n");  // tile 0x500 does not fit ten bits
  EXPECT_FALSE(back.ok());
}

TEST(Tilemap, UnknownFlagIsRefused) {
  const Bytes back = decodeTilemap("123:0:PX\n");
  EXPECT_FALSE(back.ok());
}

}  // namespace
}  // namespace snaggletooth::formats
