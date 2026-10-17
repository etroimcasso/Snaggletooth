// The machine's parts as bytes: a DMA channel and a joypad. Each is checked for
// the layout's size, each field at its offset, the round trip both ways and the
// bytes the decoder refuses. A filled value — every field distinct and non-zero,
// every button pressed — is what reaches an omitted, swapped or misplaced field.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <vector>

#include "gtest/gtest.h"
#include "snaggletooth/snes/snes.h"

namespace snaggletooth {
namespace {

// The FNV-1a 64 digest of a value's bytes. Every platform builds the same bytes
// from the same value, so every platform reads one value for each.
std::uint64_t digest(std::span<const std::uint8_t> bytes) {
  std::uint64_t hash = 0xCBF29CE484222325ull;
  for (const std::uint8_t byte : bytes) {
    hash ^= byte;
    hash *= 0x100000001B3ull;
  }
  return hash;
}

// ---- a DMA channel --------------------------------------------------------

constexpr std::size_t kChannelBytes = 12u;
constexpr std::uint64_t kFilledChannelDigest = 0x222D454F170E2075ull;

DmaChannel filledChannel() {
  DmaChannel c;
  c.dmap = 0x01u;
  c.bbad = 0x02u;
  c.a1t = 0x0403u;
  c.a1b = 0x05u;
  c.das = 0x0706u;
  c.dasb = 0x08u;
  c.a2a = 0x0A09u;
  c.nltr = 0x0Bu;
  c.unused = 0x0Cu;
  return c;
}

TEST(DmaChannelBytes, ByteSizeIsTheLayouts) {
  static_assert(DmaChannel::byteSize() == kChannelBytes);
  EXPECT_EQ(DmaChannel::byteSize(), kChannelBytes);
  EXPECT_EQ(filledChannel().asBytes().size(), kChannelBytes);
}

TEST(DmaChannelBytes, ADefaultStateRoundTrips) {
  const DmaChannel original;
  const std::optional<DmaChannel> decoded = DmaChannel::fromBytes(original.asBytes());
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(*decoded, original);
}

TEST(DmaChannelBytes, AFilledStateRoundTrips) {
  const DmaChannel original = filledChannel();
  const std::vector<std::uint8_t> bytes = original.asBytes();

  const std::optional<DmaChannel> decoded = DmaChannel::fromBytes(bytes);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(*decoded, original);

  DmaChannel into;
  ASSERT_TRUE(DmaChannel::fromBytes(bytes, into));
  EXPECT_EQ(into, original);
}

TEST(DmaChannelBytes, FieldsSitWhereTheLayoutSays) {
  // The filled channel's fields, in declaration order, are the bytes 1 to 12.
  const std::vector<std::uint8_t> bytes = filledChannel().asBytes();
  ASSERT_EQ(bytes.size(), kChannelBytes);
  for (std::size_t i = 0u; i < kChannelBytes; ++i) {
    EXPECT_EQ(bytes[i], i + 1u) << "at " << i;
  }
}

TEST(DmaChannelBytes, TheFilledBytesHaveOneDigest) {
  EXPECT_EQ(digest(filledChannel().asBytes()), kFilledChannelDigest);
}

TEST(DmaChannelBytes, WrongSizeIsRefused) {
  std::vector<std::uint8_t> bytes = filledChannel().asBytes();
  const DmaChannel before;

  const std::vector<std::uint8_t> shorter(bytes.begin(), bytes.end() - 1);
  EXPECT_FALSE(DmaChannel::fromBytes(shorter).has_value());
  DmaChannel intoShort = before;
  EXPECT_FALSE(DmaChannel::fromBytes(shorter, intoShort));
  EXPECT_EQ(intoShort, before);

  bytes.push_back(0u);
  EXPECT_FALSE(DmaChannel::fromBytes(bytes).has_value());
  DmaChannel intoLong = before;
  EXPECT_FALSE(DmaChannel::fromBytes(bytes, intoLong));
  EXPECT_EQ(intoLong, before);
}

TEST(DmaChannelBytes, AnyTwelveBytesAreAChannel) {
  // A channel has no flag and no enum: every byte of its register file takes any
  // value, so no twelve bytes are refused. All $FF is the power-on channel.
  const std::vector<std::uint8_t> ones(kChannelBytes, 0xFFu);
  const std::optional<DmaChannel> powerOn = DmaChannel::fromBytes(ones);
  ASSERT_TRUE(powerOn.has_value());
  EXPECT_EQ(*powerOn, DmaChannel{});

  const std::vector<std::uint8_t> zeros(kChannelBytes, 0u);
  const std::optional<DmaChannel> cleared = DmaChannel::fromBytes(zeros);
  ASSERT_TRUE(cleared.has_value());
  EXPECT_EQ(cleared->a1t, 0u);
  EXPECT_EQ(cleared->unused, 0u);
}

TEST(DmaChannelBytes, AcceptedBytesAreCanonical) {
  const std::vector<std::uint8_t> bytes = filledChannel().asBytes();
  const std::optional<DmaChannel> decoded = DmaChannel::fromBytes(bytes);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->asBytes(), bytes);
}

// ---- a joypad -------------------------------------------------------------

constexpr std::size_t kPadBytes = 12u;
constexpr std::uint64_t kFilledPadDigest = 0x49BE60FC79A8CF41ull;

Joypad filledPad() {
  Joypad pad;
  for (const Button button : buttons()) pad.hold(button, true);
  return pad;
}

TEST(JoypadBytes, ByteSizeIsTheLayouts) {
  static_assert(Joypad::byteSize() == kPadBytes);
  EXPECT_EQ(Joypad::byteSize(), kPadBytes);
  EXPECT_EQ(filledPad().asBytes().size(), kPadBytes);
}

TEST(JoypadBytes, ADefaultStateRoundTrips) {
  const Joypad original;
  const std::optional<Joypad> decoded = Joypad::fromBytes(original.asBytes());
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(*decoded, original);
}

TEST(JoypadBytes, AFilledStateRoundTrips) {
  const Joypad original = filledPad();
  const std::vector<std::uint8_t> bytes = original.asBytes();

  const std::optional<Joypad> decoded = Joypad::fromBytes(bytes);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(*decoded, original);

  Joypad into;
  ASSERT_TRUE(Joypad::fromBytes(bytes, into));
  EXPECT_EQ(into, original);

  // Every other button, so two buttons trading places shows.
  Joypad alternate;
  alternate.b = alternate.select = alternate.up = alternate.left = true;
  alternate.a = alternate.l = true;
  const std::optional<Joypad> alternateBack = Joypad::fromBytes(alternate.asBytes());
  ASSERT_TRUE(alternateBack.has_value());
  EXPECT_EQ(*alternateBack, alternate);
}

TEST(JoypadBytes, FieldsSitWhereTheLayoutSays) {
  // One byte a button, in the order the pad shifts them out.
  std::size_t index = 0u;
  for (const Button button : buttons()) {
    Joypad pad;
    pad.hold(button, true);
    const std::vector<std::uint8_t> bytes = pad.asBytes();
    ASSERT_EQ(bytes.size(), kPadBytes);
    for (std::size_t i = 0u; i < kPadBytes; ++i) {
      EXPECT_EQ(bytes[i], i == index ? 1u : 0u) << buttonName(button) << " at " << i;
    }
    ++index;
  }
}

TEST(JoypadBytes, TheFilledBytesHaveOneDigest) {
  EXPECT_EQ(digest(filledPad().asBytes()), kFilledPadDigest);
}

TEST(JoypadBytes, WrongSizeIsRefused) {
  std::vector<std::uint8_t> bytes = filledPad().asBytes();
  const Joypad before;

  const std::vector<std::uint8_t> shorter(bytes.begin(), bytes.end() - 1);
  EXPECT_FALSE(Joypad::fromBytes(shorter).has_value());
  Joypad intoShort = before;
  EXPECT_FALSE(Joypad::fromBytes(shorter, intoShort));
  EXPECT_EQ(intoShort, before);

  bytes.push_back(0u);
  EXPECT_FALSE(Joypad::fromBytes(bytes).has_value());
  Joypad intoLong = before;
  EXPECT_FALSE(Joypad::fromBytes(bytes, intoLong));
  EXPECT_EQ(intoLong, before);
}

TEST(JoypadBytes, ABoolOrEnumOutOfRangeIsRefused) {
  const std::vector<std::uint8_t> good = filledPad().asBytes();
  const Joypad before;
  for (const std::size_t at : {std::size_t{0u}, std::size_t{11u}}) {
    std::vector<std::uint8_t> bad = good;
    bad[at] = 2u;
    EXPECT_FALSE(Joypad::fromBytes(bad).has_value()) << "at " << at;
    Joypad into = before;
    EXPECT_FALSE(Joypad::fromBytes(bad, into)) << "at " << at;
    EXPECT_EQ(into, before) << "at " << at;
  }
}

TEST(JoypadBytes, AcceptedBytesAreCanonical) {
  const std::vector<std::uint8_t> bytes = filledPad().asBytes();
  const std::optional<Joypad> decoded = Joypad::fromBytes(bytes);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->asBytes(), bytes);
}

}  // namespace
}  // namespace snaggletooth
