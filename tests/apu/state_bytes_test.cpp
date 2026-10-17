// The audio machine's state as bytes: the layout's size, the RAM and the
// machine's own fields before the sound CPU, the DSP and the three timers, the
// round trip both ways, the two encode forms, and the bytes the decoder refuses —
// its own and its parts'. A filled state — every field set from a counter that
// never repeats a byte inside a field and never writes zero, every `bool` true,
// every enum at its last value — is what reaches an omitted, swapped or misplaced
// field; a default state is mostly zeros and reaches none of them.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "gtest/gtest.h"
#include "snaggletooth/apu/apu.h"

namespace snaggletooth {
namespace {

// The layout's figure: 65551 bytes of the machine's own fields, the 17-byte sound
// CPU, the 1256-byte DSP and three timers of three bytes.
constexpr std::size_t kLayoutBytes = 66833u;
constexpr std::size_t kCpuAt = 65551u;
constexpr std::size_t kDspAt = 65568u;
constexpr std::size_t kTimersAt = 66824u;

// The FNV-1a 64 digest of the filled state's bytes. Every platform builds the
// same bytes from the same state, so every platform reads this one value.
constexpr std::uint64_t kFilledDigest = 0x9340709B3C85D02Eull;

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

// The sound CPU's and the DSP's filled shapes, as their own files fill them.
void fillCpu(Counter& c, Spc700State& s) {
  s.pc = c.u16();
  s.a = c.u8();
  s.x = c.u8();
  s.y = c.u8();
  s.sp = c.u8();
  s.psw = c.u8();
  s.run = RunState::Stopped;
  s.ir = c.u8();
  s.tcu = c.u8();
  s.ea = c.u16();
  s.ptr = c.u16();
  s.tmp = c.u16();
  s.taken = true;
}

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

void fillDsp(Counter& c, DspState& s) {
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
}

ApuState filled() {
  Counter c;
  ApuState s;
  for (std::uint8_t& byte : s.ram) byte = c.u8();
  s.test = c.u8();
  s.control = c.u8();
  s.dspAddr = c.u8();
  for (std::uint8_t& port : s.inputPorts) port = c.u8();
  for (std::uint8_t& port : s.outputPorts) port = c.u8();
  for (std::uint8_t& port : s.auxPorts) port = c.u8();
  s.divider = c.u16();
  fillCpu(c, s.cpu);
  fillDsp(c, s.dsp);
  for (TimerState& timer : s.timers) {
    timer.stage2 = c.u8();
    timer.stage3 = c.u8();
    timer.target = c.u8();
  }
  return s;
}

TEST(ApuStateBytes, ByteSizeIsTheLayouts) {
  static_assert(ApuState::byteSize() == kLayoutBytes);
  static_assert(ApuState::byteSize() == 65551u + Spc700State::byteSize() +
                                           DspState::byteSize() + 3u * 3u);
  EXPECT_EQ(ApuState::byteSize(), kLayoutBytes);
  EXPECT_EQ(filled().asBytes().size(), kLayoutBytes);
}

TEST(ApuStateBytes, ADefaultStateRoundTrips) {
  const ApuState original;
  const std::optional<ApuState> decoded = ApuState::fromBytes(original.asBytes());
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(*decoded, original);
}

TEST(ApuStateBytes, AFilledStateRoundTrips) {
  const ApuState original = filled();
  const std::vector<std::uint8_t> bytes = original.asBytes();

  const std::optional<ApuState> decoded = ApuState::fromBytes(bytes);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(*decoded, original);

  ApuState into;
  ASSERT_TRUE(ApuState::fromBytes(bytes, into));
  EXPECT_EQ(into, original);
}

TEST(ApuStateBytes, FieldsSitWhereTheLayoutSays) {
  ApuState state = filled();
  state.ram[0] = 0x11u;
  state.ram[0xFFFF] = 0x22u;
  state.test = 0x33u;
  state.auxPorts[0] = 0x44u;
  state.auxPorts[1] = 0x55u;
  state.divider = 0x7766u;
  state.cpu.pc = 0x9988u;
  state.dsp.regs[0] = 0xAAu;
  state.timers[0].stage2 = 0xBBu;
  state.timers[2].target = 0xCCu;
  const std::vector<std::uint8_t> bytes = state.asBytes();
  ASSERT_EQ(bytes.size(), kLayoutBytes);

  // The RAM first, then the machine's own registers.
  EXPECT_EQ(bytes[0], 0x11u);
  EXPECT_EQ(bytes[0xFFFF], 0x22u);
  EXPECT_EQ(bytes[65536], 0x33u);
  EXPECT_EQ(bytes[65547], 0x44u);
  EXPECT_EQ(bytes[65548], 0x55u);
  // divider, 16-bit, low byte first.
  EXPECT_EQ(bytes[65549], 0x66u);
  EXPECT_EQ(bytes[65550], 0x77u);
  // The parts in order: the sound CPU, the DSP, the timers.
  EXPECT_EQ(bytes[kCpuAt], 0x88u);
  EXPECT_EQ(bytes[kCpuAt + 1u], 0x99u);
  EXPECT_EQ(bytes[kDspAt], 0xAAu);
  EXPECT_EQ(bytes[kTimersAt], 0xBBu);
  EXPECT_EQ(bytes[kLayoutBytes - 1u], 0xCCu);
  // Each part's slice is the part's own bytes.
  const std::vector<std::uint8_t> cpu = state.cpu.asBytes();
  EXPECT_TRUE(std::equal(cpu.begin(), cpu.end(), bytes.begin() + kCpuAt));
  const std::vector<std::uint8_t> dsp = state.dsp.asBytes();
  EXPECT_TRUE(std::equal(dsp.begin(), dsp.end(), bytes.begin() + kDspAt));
}

TEST(ApuStateBytes, TheFilledBytesHaveOneDigest) {
  EXPECT_EQ(digest(filled().asBytes()), kFilledDigest);
}

TEST(ApuStateBytes, TheAllocatingAndSpanFormsAgree) {
  const ApuState state = filled();
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

TEST(ApuStateBytes, WrongSizeIsRefused) {
  std::vector<std::uint8_t> bytes = filled().asBytes();
  const ApuState before;

  const std::vector<std::uint8_t> shorter(bytes.begin(), bytes.end() - 1);
  EXPECT_FALSE(ApuState::fromBytes(shorter).has_value());
  ApuState intoShort = before;
  EXPECT_FALSE(ApuState::fromBytes(shorter, intoShort));
  EXPECT_EQ(intoShort, before);

  bytes.push_back(0u);
  EXPECT_FALSE(ApuState::fromBytes(bytes).has_value());
  ApuState intoLong = before;
  EXPECT_FALSE(ApuState::fromBytes(bytes, intoLong));
  EXPECT_EQ(intoLong, before);
}

TEST(ApuStateBytes, ABoolOrEnumOutOfRangeIsRefused) {
  const std::vector<std::uint8_t> good = filled().asBytes();
  const ApuState before;

  // The sound CPU's run state, one past Stopped.
  std::vector<std::uint8_t> badEnum = good;
  badEnum[kCpuAt + 7u] = 3u;
  EXPECT_FALSE(ApuState::fromBytes(badEnum).has_value());
  ApuState intoEnum = before;
  EXPECT_FALSE(ApuState::fromBytes(badEnum, intoEnum));
  EXPECT_EQ(intoEnum, before);

  // The DSP's primed flag.
  std::vector<std::uint8_t> badBool = good;
  badBool[kDspAt + 351u] = 2u;
  EXPECT_FALSE(ApuState::fromBytes(badBool).has_value());
  ApuState intoBool = before;
  EXPECT_FALSE(ApuState::fromBytes(badBool, intoBool));
  EXPECT_EQ(intoBool, before);
}

TEST(ApuStateBytes, AcceptedBytesAreCanonical) {
  const std::vector<std::uint8_t> bytes = filled().asBytes();
  const std::optional<ApuState> decoded = ApuState::fromBytes(bytes);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->asBytes(), bytes);
}

}  // namespace
}  // namespace snaggletooth
