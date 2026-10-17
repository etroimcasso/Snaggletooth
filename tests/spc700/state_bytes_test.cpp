// The SPC700 state as bytes: the layout's size, each field at its offset and
// width, the round trip both ways, the two encode forms, and the bytes the decoder
// refuses. A filled state — every field a distinct non-zero value, the flag true,
// the run state at its last value — is what reaches an omitted, swapped or
// misplaced field; a default state is mostly zeros and reaches none of them.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "gtest/gtest.h"
#include "snaggletooth/apu/spc700.h"

namespace snaggletooth {
namespace {

// The layout's figure: the 16-bit program counter, five register bytes, the run
// state, two progress bytes, three 16-bit scratch words and the branch flag.
constexpr std::size_t kLayoutBytes = 17u;

// The FNV-1a 64 digest of the filled state's bytes. Every platform builds the
// same bytes from the same state, so every platform reads this one value.
constexpr std::uint64_t kFilledDigest = 0xF4E3EB6AFE04E0BAull;

std::uint64_t digest(std::span<const std::uint8_t> bytes) {
  std::uint64_t hash = 0xCBF29CE484222325ull;
  for (const std::uint8_t byte : bytes) {
    hash ^= byte;
    hash *= 0x100000001B3ull;
  }
  return hash;
}

Spc700State filled() {
  Spc700State s;
  s.pc = 0x0201u;
  s.a = 0x03u;
  s.x = 0x04u;
  s.y = 0x05u;
  s.sp = 0x06u;
  s.psw = 0x07u;
  s.run = RunState::Stopped;
  s.ir = 0x08u;
  s.tcu = 0x09u;
  s.ea = 0x0B0Au;
  s.ptr = 0x0D0Cu;
  s.tmp = 0x0F0Eu;
  s.taken = true;
  return s;
}

TEST(Spc700StateBytes, ByteSizeIsTheLayouts) {
  static_assert(Spc700State::byteSize() == kLayoutBytes);
  EXPECT_EQ(Spc700State::byteSize(), kLayoutBytes);
  EXPECT_EQ(filled().asBytes().size(), kLayoutBytes);
}

TEST(Spc700StateBytes, ADefaultStateRoundTrips) {
  const Spc700State original;
  const std::optional<Spc700State> decoded = Spc700State::fromBytes(original.asBytes());
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(*decoded, original);
}

TEST(Spc700StateBytes, AFilledStateRoundTrips) {
  const Spc700State original = filled();
  const std::vector<std::uint8_t> bytes = original.asBytes();

  const std::optional<Spc700State> decoded = Spc700State::fromBytes(bytes);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(*decoded, original);

  Spc700State into;
  ASSERT_TRUE(Spc700State::fromBytes(bytes, into));
  EXPECT_EQ(into, original);
}

TEST(Spc700StateBytes, FieldsSitWhereTheLayoutSays) {
  const std::vector<std::uint8_t> bytes = filled().asBytes();
  ASSERT_EQ(bytes.size(), kLayoutBytes);
  // pc, low byte first.
  EXPECT_EQ(bytes[0], 0x01u);
  EXPECT_EQ(bytes[1], 0x02u);
  // a and psw, single bytes.
  EXPECT_EQ(bytes[2], 0x03u);
  EXPECT_EQ(bytes[6], 0x07u);
  // run, an enum at its underlying value.
  EXPECT_EQ(bytes[7], 2u);  // RunState::Stopped
  EXPECT_EQ(bytes[8], 0x08u);
  // ea and tmp, 16-bit, low byte first.
  EXPECT_EQ(bytes[10], 0x0Au);
  EXPECT_EQ(bytes[11], 0x0Bu);
  EXPECT_EQ(bytes[14], 0x0Eu);
  EXPECT_EQ(bytes[15], 0x0Fu);
  // taken, a bool, as 1.
  EXPECT_EQ(bytes[16], 1u);

  Spc700State cleared = filled();
  cleared.taken = false;
  cleared.run = RunState::Running;
  const std::vector<std::uint8_t> clearedBytes = cleared.asBytes();
  EXPECT_EQ(clearedBytes[7], 0u);
  EXPECT_EQ(clearedBytes[16], 0u);
}

TEST(Spc700StateBytes, TheFilledBytesHaveOneDigest) {
  EXPECT_EQ(digest(filled().asBytes()), kFilledDigest);
}

TEST(Spc700StateBytes, TheAllocatingAndSpanFormsAgree) {
  const Spc700State state = filled();
  const std::vector<std::uint8_t> allocated = state.asBytes();

  std::array<std::uint8_t, kLayoutBytes> exact{};
  EXPECT_EQ(state.asBytes(exact), kLayoutBytes);
  EXPECT_TRUE(std::equal(exact.begin(), exact.end(), allocated.begin(), allocated.end()));

  std::array<std::uint8_t, kLayoutBytes - 1u> shortBuffer{};
  shortBuffer.fill(0xA5u);
  EXPECT_EQ(state.asBytes(shortBuffer), 0u);
  for (const std::uint8_t byte : shortBuffer) EXPECT_EQ(byte, 0xA5u);

  std::array<std::uint8_t, kLayoutBytes + 1u> longBuffer{};
  longBuffer.fill(0xA5u);
  EXPECT_EQ(state.asBytes(longBuffer), kLayoutBytes);
  EXPECT_TRUE(std::equal(longBuffer.begin(), longBuffer.begin() + kLayoutBytes,
                         allocated.begin(), allocated.end()));
  EXPECT_EQ(longBuffer[kLayoutBytes], 0xA5u);
}

TEST(Spc700StateBytes, WrongSizeIsRefused) {
  std::vector<std::uint8_t> bytes = filled().asBytes();
  const Spc700State before;

  const std::vector<std::uint8_t> shorter(bytes.begin(), bytes.end() - 1);
  EXPECT_FALSE(Spc700State::fromBytes(shorter).has_value());
  Spc700State intoShort = before;
  EXPECT_FALSE(Spc700State::fromBytes(shorter, intoShort));
  EXPECT_EQ(intoShort, before);

  bytes.push_back(0u);
  EXPECT_FALSE(Spc700State::fromBytes(bytes).has_value());
  Spc700State intoLong = before;
  EXPECT_FALSE(Spc700State::fromBytes(bytes, intoLong));
  EXPECT_EQ(intoLong, before);
}

TEST(Spc700StateBytes, ABoolOrEnumOutOfRangeIsRefused) {
  const std::vector<std::uint8_t> good = filled().asBytes();
  const Spc700State before;

  std::vector<std::uint8_t> badBool = good;
  badBool[16] = 2u;  // taken
  EXPECT_FALSE(Spc700State::fromBytes(badBool).has_value());
  Spc700State intoBool = before;
  EXPECT_FALSE(Spc700State::fromBytes(badBool, intoBool));
  EXPECT_EQ(intoBool, before);

  std::vector<std::uint8_t> badEnum = good;
  badEnum[7] = 3u;  // run, one past Stopped
  EXPECT_FALSE(Spc700State::fromBytes(badEnum).has_value());
  Spc700State intoEnum = before;
  EXPECT_FALSE(Spc700State::fromBytes(badEnum, intoEnum));
  EXPECT_EQ(intoEnum, before);
}

TEST(Spc700StateBytes, AcceptedBytesAreCanonical) {
  const std::vector<std::uint8_t> bytes = filled().asBytes();
  const std::optional<Spc700State> decoded = Spc700State::fromBytes(bytes);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->asBytes(), bytes);
}

}  // namespace
}  // namespace snaggletooth
