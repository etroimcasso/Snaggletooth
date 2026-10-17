// The machine and its parts as bytes: a DMA channel, a joypad, and the whole
// machine state. Each is checked for the layout's size, each field at its offset,
// the round trip both ways and the bytes the decoder refuses. A filled value —
// every field distinct and non-zero, every button pressed — is what reaches an
// omitted, swapped or misplaced field. The machine state's last cases restore the
// bytes into a second machine and run the two on, with each thread off and on.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
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

// ---- the whole machine ----------------------------------------------------

// Every byte before the save, and where each part begins.
constexpr std::size_t kBytesBeforeSave = 268858u;
constexpr std::size_t kCpuAt = 131200u;
constexpr std::size_t kApuAt = 131231u;
constexpr std::size_t kPortsAt = 198064u;
constexpr std::size_t kPpuAt = 198090u;
constexpr std::size_t kDmaAt = 268762u;
constexpr std::size_t kSaveAt = kBytesBeforeSave;
constexpr std::size_t kFilledSave = 2048u;
constexpr std::uint64_t kFilledMachineDigest = 0xD8FBD7F204895665ull;

// One NTSC frame in master cycles.
constexpr std::uint64_t kFrame = 357366u;

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

// Each part's filled shape, as the part's own file fills it.
void fillCpu(Counter& c, Cpu65816State& s) {
  s.pc = c.u16();
  s.s = c.u16();
  s.a = c.u16();
  s.x = c.u16();
  s.y = c.u16();
  s.d = c.u16();
  s.p = c.u8();
  s.dbr = c.u8();
  s.pbr = c.u8();
  s.e = true;
  s.ir = c.u8();
  s.tcu = c.u8();
  s.ea = c.u32();
  s.ptr = c.u16();
  s.tmp = c.u16();
  s.pageCross = true;
  s.nmiPending = true;
  s.irqLine = true;
  s.servicing = InterruptRequest::Nmi;
  s.run = CpuRunState::Stopped;
}

void fillSoundCpu(Counter& c, Spc700State& s) {
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

void fillApu(Counter& c, ApuState& s) {
  for (std::uint8_t& byte : s.ram) byte = c.u8();
  s.test = c.u8();
  s.control = c.u8();
  s.dspAddr = c.u8();
  for (std::uint8_t& port : s.inputPorts) port = c.u8();
  for (std::uint8_t& port : s.outputPorts) port = c.u8();
  for (std::uint8_t& port : s.auxPorts) port = c.u8();
  s.divider = c.u16();
  fillSoundCpu(c, s.cpu);
  fillDsp(c, s.dsp);
  for (TimerState& timer : s.timers) {
    timer.stage2 = c.u8();
    timer.stage3 = c.u8();
    timer.target = c.u8();
  }
}

void fillPpu(Counter& c, PpuState& s) {
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
}

void fillChannel(Counter& c, DmaChannel& channel) {
  channel.dmap = c.u8();
  channel.bbad = c.u8();
  channel.a1t = c.u16();
  channel.a1b = c.u8();
  channel.das = c.u16();
  channel.dasb = c.u8();
  channel.a2a = c.u16();
  channel.nltr = c.u8();
  channel.unused = c.u8();
}

// Every own field set from the counter, every part filled in its own shape, a pad
// in each port — every button in the first, every other in the second — and a
// 2 KB save of distinct bytes. A quarter of a megabyte, so it is built in place.
void fillMachine(SnesState& s) {
  Counter c;
  fillCpu(c, s.cpu);
  for (std::uint8_t& byte : s.wram) byte = c.u8();
  s.sram.resize(kFilledSave);
  for (std::uint8_t& byte : s.sram) byte = c.u8();
  fillApu(c, s.apu);
  s.wmadd = c.u32();
  s.memsel = c.u8();
  s.mdr = c.u8();
  s.master = c.u64();
  s.consumed = c.u64();
  s.apuPhase = c.u32();
  s.hpos = c.u16();
  s.vpos = c.u16();
  s.field = c.u8();
  s.inVblank = true;
  s.vblankBeginLine = c.u16();
  s.previousLineMaster = c.u16();
  s.refreshAt = c.u64();
  s.refreshLeft = c.u8();
  s.nmitimen = c.u8();
  s.vblankNmi = true;
  s.timeup = true;
  s.htime = c.u16();
  s.vtime = c.u16();
  s.wrmpya = c.u8();
  s.wrmpyb = c.u8();
  s.wrdiv = c.u16();
  s.wrdivb = c.u8();
  s.rddiv = c.u16();
  s.rdmpy = c.u16();
  s.mathClocks = c.u8();
  s.mathOp = MathOp::Divide;
  s.mathLeft = c.u16();
  s.mathRight = c.u8();
  Joypad every;
  for (const Button button : buttons()) every.hold(button, true);
  Joypad alternate;
  alternate.b = alternate.select = alternate.up = alternate.left = true;
  alternate.a = alternate.l = true;
  s.pads[0] = every;
  s.pads[1] = alternate;
  s.joyStrobe = true;
  for (std::uint16_t& latched : s.joyLatch) latched = c.u16();
  for (std::uint8_t& clocks : s.joyClocks) clocks = c.u8();
  s.autoJoyStart = c.u64();
  s.autoJoyClocked = c.u8();
  for (std::uint8_t& byte : s.joy) byte = c.u8();
  s.wrio = c.u8();
  s.counterLatchAt = c.u64();
  fillPpu(c, s.ppu);
  for (DmaChannel& channel : s.dma) fillChannel(c, channel);
  s.mdmaen = c.u8();
  s.hdmaen = c.u8();
  s.dmaArm = c.u8();
  s.dmaRunning = true;
  s.dmaOpened = true;
  s.dmaChannelOpened = true;
  s.dmaUnit = c.u8();
  s.dmaPauseMaster = c.u64();
  s.dmaResumePad = true;
  s.hdmaActive = c.u8();
  s.hdmaEnded = c.u8();
  s.hdmaDoWrite = c.u8();
  s.hdmaInited = true;
  s.hdmaLineFired = true;
  s.hdmaRunPending = true;
  s.hdmaIniting = true;
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

// A one-bank LoROM cartridge that runs frame after frame on its own interrupt
// and reads the pad, in emulation mode:
//
//   $8000  LDA #$81 ; STA $4200          NMI and the pad auto-read on
//   $8005  WAI                           wait for the vertical blank
//   $8006  INC $0020 ; BRA $8005         count the passes of the loop
//
//   $8100  LDA $4210                     acknowledge the NMI
//          LDA $4218 ; STA $0010         the pad's A/X/L/R byte
//          CLC ; ADC $0016 ; STA $0016   the running sum of every pad byte
//          STA $2140                     the sum to the audio machine
//          INC $0012 ; RTI               count the frame
std::vector<std::uint8_t> cartridge() {
  std::vector<std::uint8_t> rom(0x8000u, 0x00u);
  const auto put = [&rom](std::uint32_t address, std::initializer_list<std::uint8_t> code) {
    std::size_t at = address - 0x8000u;
    for (const std::uint8_t byte : code) rom[at++] = byte;
  };
  put(0x8000u, {0xA9u, 0x81u, 0x8Du, 0x00u, 0x42u, 0xCBu, 0xEEu, 0x20u, 0x00u, 0x80u, 0xFAu});
  put(0x8100u, {0xADu, 0x10u, 0x42u, 0xADu, 0x18u, 0x42u, 0x8Du, 0x10u, 0x00u, 0x18u, 0x6Du,
                0x16u, 0x00u, 0x8Du, 0x16u, 0x00u, 0x8Du, 0x40u, 0x21u, 0xEEu, 0x12u, 0x00u,
                0x40u});
  rom[0x7FFAu] = 0x00u;  // emulation-mode NMI -> $8100
  rom[0x7FFBu] = 0x81u;
  rom[0x7FFCu] = 0x00u;  // reset -> $8000
  rom[0x7FFDu] = 0x80u;
  return rom;
}

// The pads, frame by frame: A on every third frame, X on every other.
std::vector<Joypad> trace(std::size_t frames) {
  std::vector<Joypad> pads(frames);
  for (std::size_t i = 0; i < frames; ++i) {
    pads[i].a = i % 3u == 0u;
    pads[i].x = i % 2u == 0u;
  }
  return pads;
}

// Plays `pads` one frame each and answers every audio frame drained.
std::vector<StereoFrame> play(Snes& m, std::span<const Joypad> pads) {
  std::vector<StereoFrame> heard;
  std::array<StereoFrame, 256> buffer{};
  for (const Joypad& pad : pads) {
    m.setJoypad(JoypadPort::One, pad);
    m.run(kFrame);
    for (std::size_t n = m.takeFrames(buffer); n != 0; n = m.takeFrames(buffer)) {
      heard.insert(heard.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(n));
    }
  }
  return heard;
}

// Counts the pictures handed over, so a machine with one set draws them.
struct Pictures final : FrameObserver {
  std::size_t count = 0;
  void frame(const VideoFrame&) override { ++count; }
};

// Plays four frames on one machine, carries its state as bytes into two more —
// one through each decode form — and plays all three twelve frames further.
void restoredMachinesPlayOnTheSame(const SnesConfig& config) {
  const std::vector<Joypad> pads = trace(16);
  const std::span<const Joypad> head(pads.data(), 4);
  const std::span<const Joypad> rest(pads.data() + 4, pads.size() - 4);
  Pictures picturesA;
  Pictures picturesB;
  Pictures picturesC;

  Snes a(config);
  a.setFrameObserver(&picturesA);
  play(a, head);
  const std::vector<std::uint8_t> bytes = a.state().asBytes();

  Snes b(config);
  b.setFrameObserver(&picturesB);
  const std::optional<SnesState> decoded = SnesState::fromBytes(bytes);
  ASSERT_TRUE(decoded.has_value());
  ASSERT_TRUE(b.restore(*decoded));

  Snes c(config);
  c.setFrameObserver(&picturesC);
  const auto into = std::make_unique<SnesState>();
  ASSERT_TRUE(SnesState::fromBytes(bytes, *into));
  ASSERT_TRUE(c.restore(*into));

  const std::vector<StereoFrame> heardA = play(a, rest);
  const std::vector<StereoFrame> heardB = play(b, rest);
  const std::vector<StereoFrame> heardC = play(c, rest);
  EXPECT_TRUE(a.state() == b.state());
  EXPECT_TRUE(a.state() == c.state());
  EXPECT_EQ(heardA, heardB);
  EXPECT_EQ(heardA, heardC);
  EXPECT_FALSE(heardA.empty());
  EXPECT_EQ(a.state().asBytes(), b.state().asBytes());
  EXPECT_EQ(a.state().asBytes(), c.state().asBytes());
  EXPECT_EQ(picturesB.count, 12u);
  EXPECT_EQ(picturesC.count, picturesB.count);
  EXPECT_GE(a.state().wram[0x12], 15u) << "the cartridge ran on its own interrupt";
}

TEST(SnesStateBytes, ByteSizeIsTheLayoutsPlusTheSave) {
  const std::vector<std::uint8_t> rom = cartridge();
  const Snes none(SnesConfig{.rom = rom});
  EXPECT_EQ(none.state().byteSize(), kBytesBeforeSave);
  EXPECT_EQ(none.state().asBytes().size(), kBytesBeforeSave);
  const Snes small(SnesConfig{.rom = rom, .saveRamBytes = std::size_t{8192u}});
  EXPECT_EQ(small.state().byteSize(), kBytesBeforeSave + 8192u);
  EXPECT_EQ(small.state().asBytes().size(), kBytesBeforeSave + 8192u);
  const Snes largest(SnesConfig{.rom = rom, .saveRamBytes = kMaxSaveRamBytes});
  EXPECT_EQ(largest.state().byteSize(), kBytesBeforeSave + 131072u);
  EXPECT_EQ(largest.state().asBytes().size(), kBytesBeforeSave + 131072u);
}

TEST(SnesStateBytes, TheHeaderIsTheVersionAndTheSaveLength) {
  static_assert(SnesState::kBytesVersion == 1u);
  const auto state = std::make_unique<SnesState>();
  fillMachine(*state);
  const std::vector<std::uint8_t> bytes = state->asBytes();
  ASSERT_GE(bytes.size(), 6u);
  EXPECT_EQ(bytes[0], 0x01u);
  EXPECT_EQ(bytes[1], 0x00u);
  EXPECT_EQ(bytes[2], 0x00u);  // 2048, low byte first
  EXPECT_EQ(bytes[3], 0x08u);
  EXPECT_EQ(bytes[4], 0x00u);
  EXPECT_EQ(bytes[5], 0x00u);
}

TEST(SnesStateBytes, ThePartsAndTheSaveSitWhereTheLayoutSays) {
  const auto state = std::make_unique<SnesState>();
  fillMachine(*state);
  const std::vector<std::uint8_t> bytes = state->asBytes();
  ASSERT_EQ(bytes.size(), kBytesBeforeSave + kFilledSave);
  const auto sitsAt = [&bytes](const std::vector<std::uint8_t>& part, std::size_t at) {
    return std::equal(part.begin(), part.end(), bytes.begin() + static_cast<std::ptrdiff_t>(at));
  };
  EXPECT_TRUE(sitsAt(state->cpu.asBytes(), kCpuAt));
  EXPECT_TRUE(sitsAt(state->apu.asBytes(), kApuAt));
  // Each port: a presence byte of 1, then the pad's own bytes.
  EXPECT_EQ(bytes[kPortsAt], 1u);
  EXPECT_TRUE(sitsAt(state->pads[0]->asBytes(), kPortsAt + 1u));
  EXPECT_EQ(bytes[kPortsAt + 13u], 1u);
  EXPECT_TRUE(sitsAt(state->pads[1]->asBytes(), kPortsAt + 14u));
  EXPECT_TRUE(sitsAt(state->ppu.asBytes(), kPpuAt));
  for (std::size_t channel = 0u; channel < 8u; ++channel) {
    EXPECT_TRUE(sitsAt(state->dma[channel].asBytes(), kDmaAt + channel * 12u)) << channel;
  }
  EXPECT_TRUE(sitsAt(state->sram, kSaveAt));
}

TEST(SnesStateBytes, ADefaultMachinesStateRoundTrips) {
  const std::vector<std::uint8_t> rom = cartridge();
  const Snes m(SnesConfig{.rom = rom});
  const std::optional<SnesState> decoded = SnesState::fromBytes(m.state().asBytes());
  ASSERT_TRUE(decoded.has_value());
  EXPECT_TRUE(*decoded == m.state());
}

TEST(SnesStateBytes, AFilledStateRoundTrips) {
  const auto original = std::make_unique<SnesState>();
  fillMachine(*original);
  const std::vector<std::uint8_t> bytes = original->asBytes();

  const std::optional<SnesState> decoded = SnesState::fromBytes(bytes);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_TRUE(*decoded == *original);

  const auto into = std::make_unique<SnesState>();
  ASSERT_TRUE(SnesState::fromBytes(bytes, *into));
  EXPECT_TRUE(*into == *original);
}

TEST(SnesStateBytes, FieldsSitWhereTheLayoutSays) {
  const auto state = std::make_unique<SnesState>();
  fillMachine(*state);
  state->wram[0] = 0x5Au;
  state->wram[131071] = 0xA5u;
  state->wmadd = 0x0001FFFEu;
  state->master = 0x0102030405060708ull;
  state->inVblank = false;
  state->mathOp = MathOp::Multiply;
  state->joyLatch[1] = 0xBEEFu;
  state->counterLatchAt = 0x1122334455667788ull;
  state->hdmaIniting = false;
  const std::vector<std::uint8_t> bytes = state->asBytes();
  ASSERT_EQ(bytes.size(), kBytesBeforeSave + kFilledSave);

  // Work RAM straight after the header.
  EXPECT_EQ(bytes[6], 0x5Au);
  EXPECT_EQ(bytes[131077], 0xA5u);
  // 32 and 64-bit fields, low byte first.
  EXPECT_EQ(word32(bytes, 131078), 0x0001FFFEu);
  EXPECT_EQ(bytes[131078], 0xFEu);
  EXPECT_EQ(word64(bytes, 131084), 0x0102030405060708ull);
  EXPECT_EQ(bytes[131084], 0x08u);
  // A false flag, and an enum at its underlying value.
  EXPECT_EQ(bytes[131109], 0u);
  EXPECT_EQ(bytes[131140], 1u);  // MathOp::Multiply
  // The second port's latch, 16-bit.
  EXPECT_EQ(word16(bytes, 131147), 0xBEEFu);
  EXPECT_EQ(word64(bytes, 131169), 0x1122334455667788ull);
  // The last own byte, just before the parts.
  EXPECT_EQ(bytes[131199], 0u);
  EXPECT_EQ(bytes[131198], 1u);
}

TEST(SnesStateBytes, TheFilledBytesHaveOneDigest) {
  const auto state = std::make_unique<SnesState>();
  fillMachine(*state);
  EXPECT_EQ(digest(state->asBytes()), kFilledMachineDigest);
}

TEST(SnesStateBytes, TheAllocatingAndSpanFormsAgree) {
  const auto state = std::make_unique<SnesState>();
  fillMachine(*state);
  const std::size_t size = state->byteSize();
  const std::vector<std::uint8_t> allocated = state->asBytes();

  std::vector<std::uint8_t> exact(size);
  EXPECT_EQ(state->asBytes(exact), size);
  EXPECT_EQ(exact, allocated);

  std::vector<std::uint8_t> shortBuffer(size - 1u, 0xA5u);
  EXPECT_EQ(state->asBytes(shortBuffer), 0u);
  EXPECT_TRUE(std::all_of(shortBuffer.begin(), shortBuffer.end(),
                          [](std::uint8_t byte) { return byte == 0xA5u; }));

  std::vector<std::uint8_t> longBuffer(size + 1u, 0xA5u);
  EXPECT_EQ(state->asBytes(longBuffer), size);
  EXPECT_TRUE(std::equal(longBuffer.begin(), longBuffer.begin() + static_cast<std::ptrdiff_t>(size),
                         allocated.begin(), allocated.end()));
  EXPECT_EQ(longBuffer[size], 0xA5u);
}

TEST(SnesStateBytes, AcceptedBytesAreCanonical) {
  const auto state = std::make_unique<SnesState>();
  fillMachine(*state);
  const std::vector<std::uint8_t> bytes = state->asBytes();
  const std::optional<SnesState> decoded = SnesState::fromBytes(bytes);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->asBytes(), bytes);
}

// Answers whether `bytes` are refused by both forms, the into-form leaving its
// object as it was.
::testing::AssertionResult refused(const std::vector<std::uint8_t>& bytes) {
  if (SnesState::fromBytes(bytes).has_value()) {
    return ::testing::AssertionFailure() << "the allocating form accepted them";
  }
  const auto into = std::make_unique<SnesState>();
  const auto before = std::make_unique<SnesState>();
  if (SnesState::fromBytes(bytes, *into)) {
    return ::testing::AssertionFailure() << "the into-form accepted them";
  }
  if (!(*into == *before)) return ::testing::AssertionFailure() << "the into-form changed its object";
  return ::testing::AssertionSuccess();
}

TEST(SnesStateBytes, WrongSizeIsRefused) {
  const auto state = std::make_unique<SnesState>();
  fillMachine(*state);
  const std::vector<std::uint8_t> good = state->asBytes();

  const std::vector<std::uint8_t> shorter(good.begin(), good.end() - 1);
  EXPECT_TRUE(refused(shorter));
  std::vector<std::uint8_t> longer = good;
  longer.push_back(0u);
  EXPECT_TRUE(refused(longer));
  // The header says 2049 bytes of save where the buffer holds 2048.
  std::vector<std::uint8_t> disagreeing = good;
  disagreeing[2] = 0x01u;
  EXPECT_TRUE(refused(disagreeing));
  // Too short to hold the header at all.
  EXPECT_TRUE(refused(std::vector<std::uint8_t>(good.begin(), good.begin() + 5)));
}

TEST(SnesStateBytes, AnotherVersionIsRefused) {
  const auto state = std::make_unique<SnesState>();
  fillMachine(*state);
  std::vector<std::uint8_t> bytes = state->asBytes();
  bytes[0] = 2u;
  EXPECT_TRUE(refused(bytes));
  bytes[0] = 0u;
  EXPECT_TRUE(refused(bytes));
}

TEST(SnesStateBytes, ASaveLongerThanTheCapIsRefused) {
  const auto state = std::make_unique<SnesState>();
  fillMachine(*state);
  state->sram.assign(kMaxSaveRamBytes + 1u, 0x5Au);
  const std::vector<std::uint8_t> bytes = state->asBytes();
  ASSERT_EQ(bytes.size(), kBytesBeforeSave + 131073u);
  EXPECT_EQ(word32(bytes, 2), 131073u);
  EXPECT_TRUE(refused(bytes));
}

TEST(SnesStateBytes, ABoolOrEnumOutOfRangeIsRefused) {
  const auto state = std::make_unique<SnesState>();
  fillMachine(*state);
  const std::vector<std::uint8_t> good = state->asBytes();
  ASSERT_TRUE(SnesState::fromBytes(good).has_value());
  const auto set = [&good](std::size_t at, unsigned value) {
    std::vector<std::uint8_t> bad = good;
    bad[at] = static_cast<std::uint8_t>(value);
    return bad;
  };
  EXPECT_TRUE(refused(set(131109u, 2u)));  // inVblank
  EXPECT_TRUE(refused(set(131140u, 3u)));  // mathOp, one past Divide
  EXPECT_TRUE(refused(set(kCpuAt + 15u, 2u)));  // the CPU's emulation flag
  // The last voice's envelope phase, one past Release.
  EXPECT_TRUE(refused(set(kApuAt + 65568u + 484u + 7u * 96u + 55u, 4u)));
  // The last tile read's last flag, the PPU's last byte.
  EXPECT_TRUE(refused(set(kPpuAt + 70671u, 2u)));
  // The first port's presence byte.
  EXPECT_TRUE(refused(set(kPortsAt, 2u)));
}

TEST(SnesStateBytes, TheIntoFormAllocatesNothingAtTheSameSaveSize) {
  const auto state = std::make_unique<SnesState>();
  fillMachine(*state);
  const std::vector<std::uint8_t> bytes = state->asBytes();
  const auto into = std::make_unique<SnesState>();
  into->sram.assign(kFilledSave, 0u);
  const std::uint8_t* const storage = into->sram.data();
  ASSERT_TRUE(SnesState::fromBytes(bytes, *into));
  EXPECT_EQ(into->sram.data(), storage);
  EXPECT_TRUE(*into == *state);
}

TEST(SnesStateBytes, ByteSizeIsOneValueAcrossARun) {
  const std::vector<std::uint8_t> rom = cartridge();
  Snes m(SnesConfig{.rom = rom, .saveRamBytes = std::size_t{8192u}});
  const std::size_t size = m.state().byteSize();
  EXPECT_EQ(size, kBytesBeforeSave + 8192u);
  for (const Joypad& pad : trace(16)) {
    play(m, std::span<const Joypad>(&pad, 1));
    EXPECT_EQ(m.state().byteSize(), size);
  }
}

TEST(SnesStateBytes, AMachineRestoredFromTheBytesPlaysOnTheSame) {
  const std::vector<std::uint8_t> rom = cartridge();
  restoredMachinesPlayOnTheSame(SnesConfig{.rom = rom});
}

TEST(SnesStateBytes, AMachineRestoredFromTheBytesPlaysOnTheSameWithThePictureThreadOn) {
  const std::vector<std::uint8_t> rom = cartridge();
  restoredMachinesPlayOnTheSame(SnesConfig{.rom = rom, .ppuThread = true});
}

TEST(SnesStateBytes, AMachineRestoredFromTheBytesPlaysOnTheSameWithTheAudioThreadOn) {
  const std::vector<std::uint8_t> rom = cartridge();
  restoredMachinesPlayOnTheSame(SnesConfig{.rom = rom, .apuThread = true});
}

TEST(SnesStateBytes, AMachineRestoredMidInstructionFromTheBytesResumesOnTheCycle) {
  // NOP, then a branch back to it, forever.
  std::vector<std::uint8_t> rom(0x8000u, 0x00u);
  rom[0] = 0xEAu;
  rom[1] = 0x80u;
  rom[2] = 0xFDu;
  rom[0x7FFCu] = 0x00u;
  rom[0x7FFDu] = 0x80u;
  Snes a(SnesConfig{.rom = rom});
  a.run(20);  // part-way through the branch that follows the first NOP
  ASSERT_NE(a.state().cpu.tcu, 0u);
  Snes b(SnesConfig{.rom = rom});
  const std::optional<SnesState> decoded = SnesState::fromBytes(a.state().asBytes());
  ASSERT_TRUE(decoded.has_value());
  ASSERT_TRUE(b.restore(*decoded));
  a.step();
  b.step();
  EXPECT_TRUE(a.state() == b.state());
  EXPECT_EQ(a.state().cpu.tcu, 0u);
}

// The port's thirteen bytes inside the machine's: a presence byte, then the pad or
// twelve zeros, which the decoder requires.
TEST(JoypadBytes, AnAbsentPadIsAPresenceByteAndTwelveZeros) {
  const auto state = std::make_unique<SnesState>();
  fillMachine(*state);
  state->pads[0] = std::nullopt;
  const std::vector<std::uint8_t> bytes = state->asBytes();
  for (std::size_t i = 0u; i < 13u; ++i) EXPECT_EQ(bytes[kPortsAt + i], 0u) << "at " << i;
  EXPECT_EQ(bytes[kPortsAt + 13u], 1u);  // the second port still holds its pad

  const std::optional<SnesState> decoded = SnesState::fromBytes(bytes);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_FALSE(decoded->pads[0].has_value());
  EXPECT_TRUE(decoded->pads[1].has_value());

  // An empty port with a button byte set is not one the encoder writes.
  std::vector<std::uint8_t> pressed = bytes;
  pressed[kPortsAt + 1u] = 1u;
  EXPECT_FALSE(SnesState::fromBytes(pressed).has_value());
  std::vector<std::uint8_t> lastPressed = bytes;
  lastPressed[kPortsAt + 12u] = 1u;
  EXPECT_FALSE(SnesState::fromBytes(lastPressed).has_value());
}

}  // namespace
}  // namespace snaggletooth
