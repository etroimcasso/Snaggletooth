// The OAM codec: a low table of whole sprites round-trips, a table that reaches
// into the high table round-trips as it lies, a partial low table keeps its
// leftover bytes, the ninth tile bit survives, and a malformed line is refused.

#include <cstdint>
#include <string>
#include <vector>

#include "formats/oam.h"
#include "gtest/gtest.h"

namespace snaggletooth::formats {
namespace {

std::vector<std::uint8_t> pattern(std::size_t n) {
  std::vector<std::uint8_t> out(n);
  for (std::size_t i = 0; i < n; ++i) out[i] = static_cast<std::uint8_t>((i * 29 + 7) & 0xFF);
  return out;
}

std::vector<std::uint8_t> roundTrip(const std::vector<std::uint8_t>& oam) {
  const Text text = encodeOam(oam);
  EXPECT_TRUE(text.ok()) << text.error;
  const Bytes back = decodeOam(text.text);
  EXPECT_TRUE(back.ok()) << back.error;
  return back.bytes;
}

TEST(Oam, LowTableSpritesRoundTrip) {
  const auto oam = pattern(4 * 6);  // six sprites
  EXPECT_EQ(roundTrip(oam), oam);
}

TEST(Oam, FullTableWithHighBytesRoundTrips) {
  const auto oam = pattern(512 + 32);  // 128 sprites plus the whole high table
  EXPECT_EQ(roundTrip(oam), oam);
}

TEST(Oam, HighTableStartedButNotWholeRoundTrips) {
  const auto oam = pattern(512 + 5);  // low table, then five high-table bytes
  EXPECT_EQ(roundTrip(oam), oam);
}

TEST(Oam, PartialLowTableKeepsLeftoverBytes) {
  const auto oam = pattern(6);  // one sprite plus two leftover bytes
  EXPECT_EQ(roundTrip(oam), oam);
}

TEST(Oam, NinthTileBitSurvives) {
  const std::vector<std::uint8_t> oam = {0x10, 0x20, 0x55, 0x01};  // tile bit 8 set -> $155
  const Text text = encodeOam(oam);
  ASSERT_TRUE(text.ok());
  EXPECT_NE(text.text.find("155"), std::string::npos);
  EXPECT_EQ(roundTrip(oam), oam);
}

TEST(Oam, FlipFlagsSurvive) {
  const std::vector<std::uint8_t> oam = {0x00, 0x00, 0x10, 0xC0};  // both flip bits
  const Text text = encodeOam(oam);
  ASSERT_TRUE(text.ok());
  EXPECT_NE(text.text.find(":HV"), std::string::npos);
  EXPECT_EQ(roundTrip(oam), oam);
}

TEST(Oam, MalformedLineIsRefused) {
  const Bytes back = decodeOam("this is not oam data\n");
  EXPECT_FALSE(back.ok());
}

}  // namespace
}  // namespace snaggletooth::formats
