// The HDMA codec: a direct table round-trips with its terminator and any trailing
// bytes, an indirect table round-trips its pointers, a repeat entry carries a unit
// a line, the first line states the unit and mode, and a table that cannot be read
// to its end under the unit is refused.

#include <cstdint>
#include <string>
#include <vector>

#include "formats/hdma.h"
#include "gtest/gtest.h"

namespace snaggletooth::formats {
namespace {

std::vector<std::uint8_t> roundTrip(const std::vector<std::uint8_t>& table, unsigned unit,
                                    bool indirect) {
  const Text text = encodeHdma(table, unit, indirect);
  EXPECT_TRUE(text.ok()) << text.error;
  const Bytes back = decodeHdma(text.text);
  EXPECT_TRUE(back.ok()) << back.error;
  return back.bytes;
}

TEST(Hdma, DirectTableRoundTrips) {
  const std::vector<std::uint8_t> table = {0x02, 0xAA, 0xBB, 0x01, 0xCC, 0xDD, 0x00};
  EXPECT_EQ(roundTrip(table, 2, false), table);
}

TEST(Hdma, TrailingBytesAfterEndAreKept) {
  const std::vector<std::uint8_t> table = {0x02, 0xAA, 0xBB, 0x00, 0xEE, 0xFF};
  EXPECT_EQ(roundTrip(table, 2, false), table);
}

TEST(Hdma, IndirectTableRoundTrips) {
  const std::vector<std::uint8_t> table = {0x03, 0x34, 0x12, 0x02, 0x78, 0x56, 0x00};
  EXPECT_EQ(roundTrip(table, 2, true), table);
}

TEST(Hdma, RepeatEntryCarriesAUnitALine) {
  // count 0x82 -> repeat two lines, so two units of data (unit 2 -> four bytes)
  const std::vector<std::uint8_t> table = {0x82, 0xAA, 0xBB, 0xCC, 0xDD, 0x00};
  const Text text = encodeHdma(table, 2, false);
  ASSERT_TRUE(text.ok());
  // One entry holding all four bytes, then the terminator — not an entry of two
  // bytes followed by a second entry read out of the data.
  EXPECT_NE(text.text.find("lines 2 repeat $AA $BB $CC $DD\nend\n"), std::string::npos) << text.text;
  EXPECT_EQ(roundTrip(table, 2, false), table);
}

TEST(Hdma, FirstLineStatesUnitAndMode) {
  const std::vector<std::uint8_t> table = {0x01, 0xAA, 0xBB, 0xCC, 0xDD, 0x00};
  const Text text = encodeHdma(table, 4, false);
  ASSERT_TRUE(text.ok());
  EXPECT_EQ(text.text.rfind("unit 4 direct", 0), 0u);  // the very first line
}

TEST(Hdma, EntryPastEndIsRefused) {
  const std::vector<std::uint8_t> table = {0x02, 0xAA};  // needs two data bytes, has one
  const Text text = encodeHdma(table, 2, false);
  EXPECT_FALSE(text.ok());
}

TEST(Hdma, UnitMustBe124) {
  const std::vector<std::uint8_t> table = {0x00};
  const Text text = encodeHdma(table, 3, false);
  EXPECT_FALSE(text.ok());
}

}  // namespace
}  // namespace snaggletooth::formats
