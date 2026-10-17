// The 65816 state as bytes: the layout's size, each field at its offset and width,
// the round trip both ways, the two encode forms, and the bytes the decoder
// refuses. A filled state — every field a distinct non-zero value, every `bool`
// true, every enum at its last value — is what reaches an omitted, swapped or
// misplaced field; a default state is mostly zeros and reaches none of them.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "gtest/gtest.h"
#include "snaggletooth/cpu/cpu65816.h"

namespace snaggletooth {
namespace {

// The layout's figure: twelve bytes of 16-bit registers, three bytes, the
// emulation flag, two progress bytes, a 32-bit address, two 16-bit scratch words,
// three flags and two enums.
constexpr std::size_t kLayoutBytes = 31u;

// The FNV-1a 64 digest of the filled state's bytes. Every platform builds the
// same bytes from the same state, so every platform reads this one value.
constexpr std::uint64_t kFilledDigest = 0x0C9FD0ADB80BD04Eull;

std::uint64_t digest(std::span<const std::uint8_t> bytes) {
  std::uint64_t hash = 0xCBF29CE484222325ull;
  for (const std::uint8_t byte : bytes) {
    hash ^= byte;
    hash *= 0x100000001B3ull;
  }
  return hash;
}

Cpu65816State filled() {
  Cpu65816State s;
  s.pc = 0x0201u;
  s.s = 0x0403u;
  s.a = 0x0605u;
  s.x = 0x0807u;
  s.y = 0x0A09u;
  s.d = 0x0C0Bu;
  s.p = 0x0Du;
  s.dbr = 0x0Eu;
  s.pbr = 0x0Fu;
  s.e = true;
  s.ir = 0x10u;
  s.tcu = 0x11u;
  s.ea = 0x15141312u;
  s.ptr = 0x1716u;
  s.tmp = 0x1918u;
  s.pageCross = true;
  s.nmiPending = true;
  s.irqLine = true;
  s.servicing = InterruptRequest::Nmi;
  s.run = CpuRunState::Stopped;
  return s;
}

TEST(Cpu65816StateBytes, ByteSizeIsTheLayouts) {
  static_assert(Cpu65816State::byteSize() == kLayoutBytes);
  EXPECT_EQ(Cpu65816State::byteSize(), kLayoutBytes);
  EXPECT_EQ(filled().asBytes().size(), kLayoutBytes);
}

TEST(Cpu65816StateBytes, ADefaultStateRoundTrips) {
  const Cpu65816State original;
  const std::optional<Cpu65816State> decoded = Cpu65816State::fromBytes(original.asBytes());
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(*decoded, original);
}

TEST(Cpu65816StateBytes, AFilledStateRoundTrips) {
  const Cpu65816State original = filled();
  const std::vector<std::uint8_t> bytes = original.asBytes();

  const std::optional<Cpu65816State> decoded = Cpu65816State::fromBytes(bytes);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(*decoded, original);

  Cpu65816State into;
  ASSERT_TRUE(Cpu65816State::fromBytes(bytes, into));
  EXPECT_EQ(into, original);
}

TEST(Cpu65816StateBytes, FieldsSitWhereTheLayoutSays) {
  const std::vector<std::uint8_t> bytes = filled().asBytes();
  ASSERT_EQ(bytes.size(), kLayoutBytes);
  // pc, a 16-bit field, low byte first.
  EXPECT_EQ(bytes[0], 0x01u);
  EXPECT_EQ(bytes[1], 0x02u);
  // d, the last of the six 16-bit registers.
  EXPECT_EQ(bytes[10], 0x0Bu);
  EXPECT_EQ(bytes[11], 0x0Cu);
  // p and pbr, single bytes.
  EXPECT_EQ(bytes[12], 0x0Du);
  EXPECT_EQ(bytes[14], 0x0Fu);
  // e, a bool, as 1.
  EXPECT_EQ(bytes[15], 1u);
  // ea, the 32-bit field, low byte first.
  EXPECT_EQ(bytes[18], 0x12u);
  EXPECT_EQ(bytes[19], 0x13u);
  EXPECT_EQ(bytes[20], 0x14u);
  EXPECT_EQ(bytes[21], 0x15u);
  // tmp, the last 16-bit field.
  EXPECT_EQ(bytes[24], 0x18u);
  EXPECT_EQ(bytes[25], 0x19u);
  // the three flags, then the two enums at their underlying values.
  EXPECT_EQ(bytes[26], 1u);
  EXPECT_EQ(bytes[28], 1u);
  EXPECT_EQ(bytes[29], 2u);  // InterruptRequest::Nmi
  EXPECT_EQ(bytes[30], 2u);  // CpuRunState::Stopped

  // A false flag and the first enum value write zero.
  Cpu65816State cleared = filled();
  cleared.e = false;
  cleared.run = CpuRunState::Running;
  const std::vector<std::uint8_t> clearedBytes = cleared.asBytes();
  EXPECT_EQ(clearedBytes[15], 0u);
  EXPECT_EQ(clearedBytes[30], 0u);
}

TEST(Cpu65816StateBytes, TheFilledBytesHaveOneDigest) {
  EXPECT_EQ(digest(filled().asBytes()), kFilledDigest);
}

TEST(Cpu65816StateBytes, TheAllocatingAndSpanFormsAgree) {
  const Cpu65816State state = filled();
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

TEST(Cpu65816StateBytes, WrongSizeIsRefused) {
  std::vector<std::uint8_t> bytes = filled().asBytes();
  const Cpu65816State before;

  const std::vector<std::uint8_t> shorter(bytes.begin(), bytes.end() - 1);
  EXPECT_FALSE(Cpu65816State::fromBytes(shorter).has_value());
  Cpu65816State intoShort = before;
  EXPECT_FALSE(Cpu65816State::fromBytes(shorter, intoShort));
  EXPECT_EQ(intoShort, before);

  bytes.push_back(0u);
  EXPECT_FALSE(Cpu65816State::fromBytes(bytes).has_value());
  Cpu65816State intoLong = before;
  EXPECT_FALSE(Cpu65816State::fromBytes(bytes, intoLong));
  EXPECT_EQ(intoLong, before);
}

TEST(Cpu65816StateBytes, ABoolOrEnumOutOfRangeIsRefused) {
  const std::vector<std::uint8_t> good = filled().asBytes();
  const Cpu65816State before;

  std::vector<std::uint8_t> badBool = good;
  badBool[15] = 2u;  // e
  EXPECT_FALSE(Cpu65816State::fromBytes(badBool).has_value());
  Cpu65816State intoBool = before;
  EXPECT_FALSE(Cpu65816State::fromBytes(badBool, intoBool));
  EXPECT_EQ(intoBool, before);

  std::vector<std::uint8_t> badEnum = good;
  badEnum[30] = 3u;  // run, one past Stopped
  EXPECT_FALSE(Cpu65816State::fromBytes(badEnum).has_value());
  Cpu65816State intoEnum = before;
  EXPECT_FALSE(Cpu65816State::fromBytes(badEnum, intoEnum));
  EXPECT_EQ(intoEnum, before);

  std::vector<std::uint8_t> badServicing = good;
  badServicing[29] = 3u;  // servicing, one past Nmi
  EXPECT_FALSE(Cpu65816State::fromBytes(badServicing).has_value());
}

TEST(Cpu65816StateBytes, AcceptedBytesAreCanonical) {
  const std::vector<std::uint8_t> bytes = filled().asBytes();
  const std::optional<Cpu65816State> decoded = Cpu65816State::fromBytes(bytes);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->asBytes(), bytes);
}

}  // namespace
}  // namespace snaggletooth
