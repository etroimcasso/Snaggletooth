// The DSP state as bytes: the layout's size, fields of every width at their
// offsets, the voices and the stereo frame after the DSP's own fields, the round
// trip both ways, the two encode forms, and the bytes the decoder refuses. A
// filled state — every field set from a counter that never repeats a byte inside
// a field and never writes zero, every `bool` true, every envelope phase at
// Release — is what reaches an omitted, swapped or misplaced field; a default
// state is mostly zeros and reaches none of them.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "gtest/gtest.h"
#include "snaggletooth/apu/dsp.h"

namespace snaggletooth {
namespace {

// The layout's figure: 484 bytes of the DSP's own fields, eight voices of 96, and
// the four-byte stereo frame.
constexpr std::size_t kLayoutBytes = 1256u;
constexpr std::size_t kVoicesAt = 484u;
constexpr std::size_t kVoiceBytes = 96u;
constexpr std::size_t kSlotFrameAt = 1252u;

// The FNV-1a 64 digest of the filled state's bytes. Every platform builds the
// same bytes from the same state, so every platform reads this one value.
constexpr std::uint64_t kFilledDigest = 0xF2A025036D57BA65ull;

std::uint64_t digest(std::span<const std::uint8_t> bytes) {
  std::uint64_t hash = 0xCBF29CE484222325ull;
  for (const std::uint8_t byte : bytes) {
    hash ^= byte;
    hash *= 0x100000001B3ull;
  }
  return hash;
}

// Hands out the bytes 1 to 255 in turn, then 1 again, and builds wider values
// from consecutive bytes, low byte first.
class Counter {
 public:
  std::uint8_t u8() {
    next_ = static_cast<std::uint8_t>(next_ == 0xFFu ? 1u : next_ + 1u);
    return next_;
  }
  std::int8_t i8() { return static_cast<std::int8_t>(u8()); }
  std::uint16_t u16() {
    const std::uint16_t low = u8();
    const std::uint16_t high = u8();
    return static_cast<std::uint16_t>(low | (high << 8));
  }
  std::int16_t i16() { return static_cast<std::int16_t>(u16()); }
  std::uint32_t u32() {
    const std::uint32_t low = u16();
    const std::uint32_t high = u16();
    return low | (high << 16);
  }
  std::int32_t i32() { return static_cast<std::int32_t>(u32()); }
  std::uint64_t u64() {
    const std::uint64_t low = u32();
    const std::uint64_t high = u32();
    return low | (high << 32);
  }

 private:
  std::uint8_t next_ = 0u;
};

void fillVoice(Counter& c, VoiceState& v) {
  v.brrAddress = c.u16();
  v.decoderAddress = c.u16();
  v.headerAddress = c.u16();
  v.brrSampleIndex = c.u8();
  v.pitchCounter = c.u16();
  v.window.newest = c.i16();
  v.window.old = c.i16();
  v.window.older = c.i16();
  v.window.oldest = c.i16();
  for (std::int16_t& sample : v.pending) sample = c.i16();
  v.pendingHead = c.u8();
  v.pendingCount = c.u8();
  for (VoiceState::GroupDecode& group : v.scheduledDecodes) {
    group.address = c.u16();
    group.offset = c.u8();
    group.header = c.u8();
    group.firstByte = c.u8();
    group.bytesLoaded = true;
    group.decodedSamples = c.u8();
    group.headerCaptured = true;
  }
  v.scheduledDecodeCount = c.u8();
  v.loadedHeader = c.u8();
  v.headerLoaded = true;
  v.loopPointer = c.u16();
  v.loopPointerLoaded = true;
  v.srcn = c.u8();
  v.srcnLoaded = true;
  v.pitchLow = c.u8();
  v.pitchPending = c.u16();
  v.pitchPendingValid = true;
  v.adsr1 = c.u8();
  v.adsr1Loaded = true;
  v.decodePrev1 = c.i16();
  v.decodePrev2 = c.i16();
  v.envelope = c.u16();
  v.phase = EnvPhase::Release;
  v.konDelay = c.u8();
  v.computesSinceKeyOn = c.u8();
  v.pitchCaptureHold = c.u8();
  v.restartPending = true;
  v.startPending = true;
  v.startupWalks = true;
  v.bentGainRef = c.u16();
}

DspState filled() {
  Counter c;
  DspState s;
  for (std::uint8_t& reg : s.regs) reg = c.u8();
  s.globalCounter = c.u16();
  s.sampleIndex = c.u32();
  s.internalKon = c.u8();
  s.consumedKon = c.u8();
  s.latchedPmon = c.u8();
  s.latchedNon = c.u8();
  s.latchedEon = c.u8();
  s.latchedDir = c.u8();
  for (std::uint8_t& stage : s.envxStage) stage = c.u8();
  s.preparedEndx = c.u8();
  s.pendingEndxClear = c.u8();
  s.cycleCount = c.u64();
  for (std::uint64_t& stamp : s.outxWriteCycle) stamp = c.u64();
  for (std::uint64_t& stamp : s.envxWriteCycle) stamp = c.u64();
  s.endxWriteCycle = c.u64();
  s.konWriteCycle = c.u64();
  s.noiseLevel = c.i16();
  s.echoIndex = c.u16();
  s.echoLength = c.u16();
  s.echoAppliedEsa = c.u8();
  for (std::int16_t& sample : s.echoFirLeft) sample = c.i16();
  for (std::int16_t& sample : s.echoFirRight) sample = c.i16();
  for (std::int8_t& coefficient : s.echoFirCoeff) coefficient = c.i8();
  s.echoFirPos = c.u8();
  for (VoiceState& voice : s.voices) fillVoice(c, voice);
  s.slotCursor = c.u8();
  s.primed = true;
  s.mixLeft = c.i32();
  s.mixRight = c.i32();
  s.echoSendLeft = c.i32();
  s.echoSendRight = c.i32();
  s.slotFrame.left = c.i16();
  s.slotFrame.right = c.i16();
  s.echoWritePending = true;
  s.echoWriteEntry = c.u16();
  for (std::uint8_t& byte : s.echoWriteBytes) byte = c.u8();
  for (int& amplitude : s.voiceAmplitude) amplitude = c.i32();
  for (int& amplitude : s.modulatorAmplitude) amplitude = c.i32();
  for (std::uint8_t& outx : s.preparedOutx) outx = c.u8();
  for (std::uint8_t& envx : s.preparedEnvx) envx = c.u8();
  for (std::uint16_t& step : s.pitchLatch) step = c.u16();
  s.pitchReloadPending = c.u8();
  s.echoFirOutLeft = c.i32();
  s.echoFirOutRight = c.i32();
  s.echoGateLeft = true;
  s.echoGateRight = true;
  s.echoLatchedEsa = c.u8();
  s.echoLatchedEdl = c.u8();
  return s;
}

std::uint16_t word16(const std::vector<std::uint8_t>& bytes, std::size_t at) {
  return static_cast<std::uint16_t>(bytes[at] | (bytes[at + 1u] << 8));
}

std::uint32_t word32(const std::vector<std::uint8_t>& bytes, std::size_t at) {
  return static_cast<std::uint32_t>(word16(bytes, at)) |
         (static_cast<std::uint32_t>(word16(bytes, at + 2u)) << 16);
}

std::uint64_t word64(const std::vector<std::uint8_t>& bytes, std::size_t at) {
  return static_cast<std::uint64_t>(word32(bytes, at)) |
         (static_cast<std::uint64_t>(word32(bytes, at + 4u)) << 32);
}

TEST(DspStateBytes, ByteSizeIsTheLayouts) {
  static_assert(DspState::byteSize() == kLayoutBytes);
  EXPECT_EQ(DspState::byteSize(), kLayoutBytes);
  EXPECT_EQ(filled().asBytes().size(), kLayoutBytes);
}

TEST(DspStateBytes, ADefaultStateRoundTrips) {
  const DspState original;
  const std::optional<DspState> decoded = DspState::fromBytes(original.asBytes());
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(*decoded, original);
}

TEST(DspStateBytes, AFilledStateRoundTrips) {
  const DspState original = filled();
  const std::vector<std::uint8_t> bytes = original.asBytes();

  const std::optional<DspState> decoded = DspState::fromBytes(bytes);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(*decoded, original);

  DspState into;
  ASSERT_TRUE(DspState::fromBytes(bytes, into));
  EXPECT_EQ(into, original);
}

TEST(DspStateBytes, FieldsSitWhereTheLayoutSays) {
  DspState state = filled();
  state.regs[0] = 0x5Au;
  state.regs[127] = 0xA5u;
  state.globalCounter = 0x77FEu;
  state.sampleIndex = 0x01020304u;
  state.cycleCount = 0x1122334455667788ull;
  state.outxWriteCycle[0] = kNoCpuWrite;
  state.konWriteCycle = 0x0102030405060708ull;
  state.noiseLevel = -2;
  state.echoFirCoeff[0] = -128;
  state.primed = false;
  state.mixLeft = -0x12345678;
  state.echoWritePending = true;
  state.voiceAmplitude[0] = -3;
  state.echoFirOutRight = 0x7FFF;
  state.echoLatchedEdl = 0x0Fu;
  state.voices[0].brrAddress = 0xBEEFu;
  state.voices[0].phase = EnvPhase::Decay;
  state.voices[0].window.newest = -0x4000;
  state.voices[0].scheduledDecodes[0].address = 0x1234u;
  state.voices[7].bentGainRef = 0x07FFu;
  state.voices[7].scheduledDecodes[2].headerCaptured = false;
  state.slotFrame.left = -1;
  state.slotFrame.right = 0x0102;
  const std::vector<std::uint8_t> bytes = state.asBytes();
  ASSERT_EQ(bytes.size(), kLayoutBytes);

  // The register file first, a byte a register.
  EXPECT_EQ(bytes[0], 0x5Au);
  EXPECT_EQ(bytes[127], 0xA5u);
  // 16, 32 and 64-bit values, low byte first.
  EXPECT_EQ(word16(bytes, 128), 0x77FEu);
  EXPECT_EQ(bytes[128], 0xFEu);
  EXPECT_EQ(word32(bytes, 130), 0x01020304u);
  EXPECT_EQ(bytes[130], 0x04u);
  EXPECT_EQ(word64(bytes, 150), 0x1122334455667788ull);
  EXPECT_EQ(bytes[150], 0x88u);
  EXPECT_EQ(word64(bytes, 158), kNoCpuWrite);
  EXPECT_EQ(word64(bytes, 294), 0x0102030405060708ull);
  // Signed values in two's complement at their widths.
  EXPECT_EQ(word16(bytes, 302), 0xFFFEu);
  EXPECT_EQ(bytes[341], 0x80u);
  EXPECT_EQ(word32(bytes, 352), 0xEDCBA988u);
  // A false and a true flag.
  EXPECT_EQ(bytes[351], 0u);
  EXPECT_EQ(bytes[368], 1u);
  // An `int` as four bytes.
  EXPECT_EQ(word32(bytes, 375), 0xFFFFFFFDu);
  EXPECT_EQ(word32(bytes, 476), 0x00007FFFu);
  // The last own byte, then the first voice.
  EXPECT_EQ(bytes[483], 0x0Fu);
  EXPECT_EQ(word16(bytes, kVoicesAt + 0), 0xBEEFu);
  EXPECT_EQ(bytes[kVoicesAt + 55], 1u);  // EnvPhase::Decay
  EXPECT_EQ(word16(bytes, kVoicesAt + 64), 0xC000u);
  EXPECT_EQ(word16(bytes, kVoicesAt + 72), 0x1234u);
  // The last voice's own last field and its last group's last byte.
  EXPECT_EQ(word16(bytes, kVoicesAt + 7 * kVoiceBytes + 62), 0x07FFu);
  EXPECT_EQ(bytes[kVoicesAt + 7 * kVoiceBytes + 95], 0u);
  // The stereo frame last.
  EXPECT_EQ(word16(bytes, kSlotFrameAt), 0xFFFFu);
  EXPECT_EQ(word16(bytes, kSlotFrameAt + 2), 0x0102u);
}

TEST(DspStateBytes, TheFilledBytesHaveOneDigest) {
  EXPECT_EQ(digest(filled().asBytes()), kFilledDigest);
}

TEST(DspStateBytes, TheAllocatingAndSpanFormsAgree) {
  const DspState state = filled();
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

TEST(DspStateBytes, WrongSizeIsRefused) {
  std::vector<std::uint8_t> bytes = filled().asBytes();
  const DspState before;

  const std::vector<std::uint8_t> shorter(bytes.begin(), bytes.end() - 1);
  EXPECT_FALSE(DspState::fromBytes(shorter).has_value());
  DspState intoShort = before;
  EXPECT_FALSE(DspState::fromBytes(shorter, intoShort));
  EXPECT_EQ(intoShort, before);

  bytes.push_back(0u);
  EXPECT_FALSE(DspState::fromBytes(bytes).has_value());
  DspState intoLong = before;
  EXPECT_FALSE(DspState::fromBytes(bytes, intoLong));
  EXPECT_EQ(intoLong, before);
}

TEST(DspStateBytes, ABoolOrEnumOutOfRangeIsRefused) {
  const std::vector<std::uint8_t> good = filled().asBytes();
  const DspState before;

  // primed, one of the DSP's own flags.
  std::vector<std::uint8_t> badBool = good;
  badBool[351] = 2u;
  EXPECT_FALSE(DspState::fromBytes(badBool).has_value());
  DspState intoBool = before;
  EXPECT_FALSE(DspState::fromBytes(badBool, intoBool));
  EXPECT_EQ(intoBool, before);

  // The first voice's envelope phase, one past Release.
  std::vector<std::uint8_t> badEnum = good;
  badEnum[kVoicesAt + 55] = 4u;
  EXPECT_FALSE(DspState::fromBytes(badEnum).has_value());
  DspState intoEnum = before;
  EXPECT_FALSE(DspState::fromBytes(badEnum, intoEnum));
  EXPECT_EQ(intoEnum, before);

  // A flag in the last voice's last scheduled group.
  std::vector<std::uint8_t> badGroup = good;
  badGroup[kVoicesAt + 7 * kVoiceBytes + 95] = 2u;
  EXPECT_FALSE(DspState::fromBytes(badGroup).has_value());
  DspState intoGroup = before;
  EXPECT_FALSE(DspState::fromBytes(badGroup, intoGroup));
  EXPECT_EQ(intoGroup, before);
}

TEST(DspStateBytes, AcceptedBytesAreCanonical) {
  const std::vector<std::uint8_t> bytes = filled().asBytes();
  const std::optional<DspState> decoded = DspState::fromBytes(bytes);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->asBytes(), bytes);
}

}  // namespace
}  // namespace snaggletooth
