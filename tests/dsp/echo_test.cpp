// The S-DSP echo unit: the ring buffer in APU RAM, the per-channel 8-tap FIR
// filter, the EON feedback write path, the echo output add through EVOL, and the
// FLG bit-5 write disable. The echo buffer in RAM is the digital oracle — the
// tests assert the bytes the unit writes and reads, not captured audio.
//
// Expected values are hand-derived from fullsnes's echo section (the primary
// contract): the entry layout and ESA base at lines 3208-3226, EDL sizing at
// 3228-3253, the FIR formula (taps oldest*FIR0 ... newest*FIR7, each SAR 6, seven
// wrapping adds then a saturating add) and the write path at 3267-3289, and the
// output-mixer order (sum*MVOL SAR 7, then + fir*EVOL SAR 7) at 3021-3033.
// Anomie's S-DSP doc is the cross-check: the same FIR formula at lines 927-942,
// the echo split after per-voice volume at 253-261, EDL's D<<9 entries with D=0
// giving one entry at 899-901, and the 16-bit buffer wrap at 884-886.
//
// The two sources place the FIR output's bit-0 mask differently: fullsnes feeds
// the unmasked 16-bit sum to EVOL/EFB and masks only the buffer write (AND
// FFFEh, line 3282), while Anomie makes the filter's own output 15 bits (lines
// 46-47 and 940-942), so the low bit is gone before either volume reads it.
// Anomie's placement is the one the hardware exhibits, which the SPC DSP test ROM
// pins on the feedback path; the buffer write is masked as well, since the volume
// multiply can put a low bit back.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "snaggletooth/apu/apu.h"
#include "snaggletooth/apu/dsp.h"

namespace {

using snaggletooth::Apu;
using snaggletooth::ApuState;
using snaggletooth::DspState;
using snaggletooth::EnvPhase;
using snaggletooth::stepDspSample;
using snaggletooth::StereoFrame;

using Ram = std::array<std::uint8_t, 65536>;

constexpr std::uint8_t kMvolLeft = 0x0C;
constexpr std::uint8_t kMvolRight = 0x1C;
constexpr std::uint8_t kEfb = 0x0D;
constexpr std::uint8_t kEvolLeft = 0x2C;
constexpr std::uint8_t kEvolRight = 0x3C;
constexpr std::uint8_t kEon = 0x4D;
constexpr std::uint8_t kFlg = 0x6C;
constexpr std::uint8_t kEsa = 0x6D;
constexpr std::uint8_t kEdl = 0x7D;
constexpr std::uint8_t kFir0 = 0x0F;
constexpr std::uint8_t kFir7 = 0x7F;

// The writable-RAM overload: the echo unit reads and writes the buffer in `ram`.
StereoFrame step(DspState& dsp, Ram& ram) {
  return stepDspSample(dsp, std::span<std::uint8_t, 65536>{ram});
}

// Reads a stored 16-bit little-endian echo sample at a buffer byte address.
int entry16(const Ram& ram, std::uint16_t address) {
  return static_cast<std::int16_t>(ram[address] | (ram[static_cast<std::uint16_t>(address + 1)] << 8));
}

// Places voice `v` mid-play at a single Gaussian tap so its amplitude is exactly
// 1294 (older=800h -> interp 519h=1305; Direct Gain 7Fh -> envelope 7F0h=2032;
// amplitude = (1305*2032)>>11 = 1294), with the given per-voice left/right volume
// and a stationary pitch so every sample repeats it. The master volume is left to
// the caller; the echo-send is taken after the per-voice volume, before MVOL.
void soundingVoice(DspState& dsp, std::size_t v, std::uint8_t left, std::uint8_t right) {
  dsp.voices[v].window = {.newest = 0, .old = 0, .older = 0x0800, .oldest = 0};
  dsp.voices[v].pitchCounter = 0;
  dsp.voices[v].phase = EnvPhase::Sustain;
  dsp.voices[v].konDelay = 0;
  dsp[v * 0x10 + 0x07] = 0x7F;  // VxGAIN: Direct Gain, level 7F0h every sample
  dsp[v * 0x10 + 0x00] = left;  // VxVOLL
  dsp[v * 0x10 + 0x01] = right; // VxVOLR
}

// ── Write geometry ───────────────────────────────────────────────────────────

TEST(EchoWrite, EonSendIsWrittenLittleEndianAtEsaBaseWithBitZeroCleared) {
  // A voice at amplitude 1294 with VxVOL 7Fh sends (1294*127)>>6 = 2567 into the
  // echo mix (fullsnes Output Mixer SAR 6, Anomie 253 "echo splits off after the
  // per-voice volume"). With FIR and EFB zero the feedback term is zero, so the
  // written value is the send masked to even: 2567 AND FFFEh = 2566 = 0A06h. The
  // entry is L then R, little-endian, based at ESA*100h (fullsnes 3212-3218).
  DspState dsp;
  Ram ram{};
  soundingVoice(dsp, 0, 0x7F, 0x7F);
  dsp[kEon] = 0x01;    // voice 0 feeds the echo write
  dsp[kEsa] = 0x20;    // base 2000h
  dsp[kEdl] = 0x00;    // one entry
  dsp[kMvolLeft] = 0x00;
  dsp[kMvolRight] = 0x00;
  step(dsp, ram);
  EXPECT_EQ(ram[0x2000], 0x06);  // 2566 low byte, bit 0 clear
  EXPECT_EQ(ram[0x2001], 0x0A);  // 2566 high byte
  EXPECT_EQ(ram[0x2002], 0x06);  // right channel, same send
  EXPECT_EQ(ram[0x2003], 0x0A);
  EXPECT_EQ(entry16(ram, 0x2000), 2566);
}

TEST(EchoWrite, ANonEonVoiceDoesNotFeedTheEchoWrite) {
  // EON gates the write mix (fullsnes 3201-3206). The same sounding voice with its
  // EON bit clear contributes nothing to the buffer; with FIR/EFB zero the entry
  // stays zero.
  DspState dsp;
  Ram ram{};
  soundingVoice(dsp, 0, 0x7F, 0x7F);
  dsp[kEon] = 0x00;
  dsp[kEsa] = 0x20;
  step(dsp, ram);
  EXPECT_EQ(entry16(ram, 0x2000), 0);
  EXPECT_EQ(entry16(ram, 0x2002), 0);
}

TEST(EchoWrite, EsaRelocatesTheBuffer) {
  // ESA is read every sample, so moving it lands the next write at the new base
  // (fullsnes 3208-3210, 3223-3226). No FIR/EFB, so each write is the raw send.
  DspState dsp;
  Ram ram{};
  soundingVoice(dsp, 0, 0x40, 0x40);  // send (1294*64)>>6 = 1294
  dsp[kEon] = 0x01;
  dsp[kMvolLeft] = 0x00;
  dsp[kMvolRight] = 0x00;
  dsp[kEsa] = 0x20;
  step(dsp, ram);
  EXPECT_EQ(entry16(ram, 0x2000), 1294);
  dsp[kEsa] = 0x30;  // relocate
  step(dsp, ram);
  EXPECT_EQ(entry16(ram, 0x3000), 1294);  // the new base holds the write
}

TEST(EchoWrite, TheEntryAddressWrapsWithinSixteenBits) {
  // "The echo buffer will wrap within 16 bits" (Anomie 884-886; fullsnes 3269's
  // AND FFFFh). At ESA=FFh (base FF00h) and ring index 64, the entry address is
  // FF00h + 64*4 = 10000h, which wraps to 0000h.
  DspState dsp;
  Ram ram{};
  soundingVoice(dsp, 0, 0x40, 0x40);  // send 1294
  dsp[kEon] = 0x01;
  dsp[kMvolLeft] = 0x00;
  dsp[kMvolRight] = 0x00;
  dsp[kEsa] = 0xFF;
  dsp.echoIndex = 64;
  dsp.echoLength = 512;  // index 64 does not wrap the ring here
  step(dsp, ram);
  EXPECT_EQ(entry16(ram, 0x0000), 1294);  // wrapped to the base of RAM
}

// ── The FIR filter ───────────────────────────────────────────────────────────

// Sets up a static one-entry buffer at 2000h holding a known left sample, echo
// writes disabled (FLG bit 5) so the buffer never changes and reads repeat, and
// EVOLL as the readout scale. The FIR history is the caller's to seed.
void staticBuffer(DspState& dsp, Ram& ram, int leftSample) {
  const int stored = leftSample << 1;  // 15-bit sample stored left-justified
  ram[0x2000] = static_cast<std::uint8_t>(stored & 0xFF);
  ram[0x2001] = static_cast<std::uint8_t>((stored >> 8) & 0xFF);
  dsp[kEsa] = 0x20;
  dsp[kEdl] = 0x00;         // one entry
  dsp[kFlg] = 0x20;         // echo writes disabled: a static buffer, reads continue
  dsp[kMvolLeft] = 0x00;    // isolate the echo output in the frame
  dsp[kMvolRight] = 0x00;
}

TEST(EchoFir, TheNewestTapReadsTheEntryJustRead) {
  // FIR7 multiplies the newest history sample — the entry read this sample
  // (fullsnes 3270, 3278). With only FIR7 = 40h (a unity tap: sample*64>>6 =
  // sample), fir = 800h = 2048, and EVOLL 40h passes (2048*64)>>7 = 1024 into the
  // frame (fullsnes 3280 audio_output = NormalVoices + fir*EVOL SAR 7).
  DspState dsp;
  Ram ram{};
  staticBuffer(dsp, ram, 0x0800);
  dsp[kFir7] = 0x40;
  dsp[kEvolLeft] = 0x40;
  const StereoFrame frame = step(dsp, ram);
  EXPECT_EQ(frame.left, 1024);
  EXPECT_EQ(entry16(ram, 0x2000), 0x0800 << 1);  // buffer unchanged: writes disabled
}

TEST(EchoFir, TheZerothTapReadsTheOldestHistorySample) {
  // FIR0 multiplies the oldest history sample, buf[(i-7)&7] (fullsnes 3271). With
  // echoFirPos 0 the read fills the newest slot 0; the oldest is slot 1. Seeding
  // slot 1 = 100 and setting only FIR0 = 40h gives fir = 100, so EVOLL 40h yields
  // (100*64)>>7 = 50 — not the newest 800h that FIR7 would have read.
  DspState dsp;
  Ram ram{};
  staticBuffer(dsp, ram, 0x0800);
  dsp.echoFirPos = 0;
  dsp.echoFirLeft[1] = 100;  // the oldest slot
  dsp[kFir0] = 0x40;
  dsp[kEvolLeft] = 0x40;
  const StereoFrame frame = step(dsp, ram);
  EXPECT_EQ(frame.left, 50);  // reads the oldest 100, not the newest 800h
}

TEST(EchoFir, TheLeftAndRightChannelsFilterSeparately) {
  // "the left and right stereo channels are filtered separately (no crosstalk),
  // but with identical coefficients" (fullsnes 3288-3289; Anomie 944-945). A left
  // sample of 800h and a right sample of 400h, both through FIR7 40h and EVOL 40h,
  // give distinct frame channels: 1024 and 512.
  DspState dsp;
  Ram ram{};
  staticBuffer(dsp, ram, 0x0800);
  const int rightStored = 0x0400 << 1;
  ram[0x2002] = static_cast<std::uint8_t>(rightStored & 0xFF);
  ram[0x2003] = static_cast<std::uint8_t>((rightStored >> 8) & 0xFF);
  dsp[kFir7] = 0x40;
  dsp[kEvolLeft] = 0x40;
  dsp[kEvolRight] = 0x40;
  const StereoFrame frame = step(dsp, ram);
  EXPECT_EQ(frame.left, 1024);
  EXPECT_EQ(frame.right, 512);
}

TEST(EchoFir, TheFirstSevenAddsWrapAndTheFinalAddSaturates) {
  // "these additions are done without overflow handling" for FIR0..FIR6, and "with
  // overflow handling" — saturating — for the final FIR7 add (fullsnes 3271-3279;
  // Anomie 929-939). Eight history slots of 3FFFh (16383) through eight 40h taps
  // (each contributing the value) accumulate: 16383, 32766, 49149->wrap -16387, -4,
  // 16379, 32762, 49145->wrap -16391, then the saturating final add -16391+16383 =
  // -8. A saturate-every-add filter would instead reach and hold 7FFFh. EVOLL 40h
  // maps fir -8 to (-8*64)>>7 = -4; the wrong 7FFFh would give 16383.
  DspState dsp;
  Ram ram{};
  staticBuffer(dsp, ram, 0x3FFF);       // the read keeps the newest slot at 16383
  dsp.echoFirLeft.fill(16383);
  dsp[kFir0] = 0x40;
  dsp[0x1F] = 0x40;
  dsp[0x2F] = 0x40;
  dsp[0x3F] = 0x40;
  dsp[0x4F] = 0x40;
  dsp[0x5F] = 0x40;
  dsp[0x6F] = 0x40;
  dsp[kFir7] = 0x40;
  dsp[kEvolLeft] = 0x40;
  const StereoFrame frame = step(dsp, ram);
  EXPECT_EQ(frame.left, -4);  // fir = -8 (wrapping mid-sum), not 7FFFh (all-saturate)
}

TEST(EchoFir, TheOutputDropsItsLowBitBeforeTheEchoVolume) {
  // The filter's output is a 15-bit sample (Anomie 46-47, 940-942), so an odd sum
  // reaches EVOL one lower. A newest sample of 7Fh through a unity tap (FIR7 40h)
  // sums to 127, which the drop takes to 126; EVOLL 7Fh then passes
  // (126*127)>>7 = 125 into the frame, where the unmasked 127 would give 126.
  DspState dsp;
  Ram ram{};
  staticBuffer(dsp, ram, 0x7F);
  dsp[kFir7] = 0x40;
  dsp[kEvolLeft] = 0x7F;
  const StereoFrame frame = step(dsp, ram);
  EXPECT_EQ(frame.left, 125);
}

// ── The feedback write path ──────────────────────────────────────────────────

TEST(EchoFeedback, TheOutputDropsItsLowBitBeforeTheFeedbackMultiply) {
  // The same 15-bit output feeds EFB, which is what a buffer holding a single odd
  // sample shows: an entry of 2 reads as 1, a unity tap (FIR7 40h) sums to 1, and
  // the drop takes it to 0 — so full negative feedback (EFB 80h) with no EON send
  // writes 0 back. Feeding the unmasked 1 instead would write (1*-128)>>7 = -1,
  // which the write's own mask turns into -2 rather than silence.
  DspState dsp;
  Ram ram{};
  ram[0x2000] = 0x02;  // one odd sample: 2 stored, read back as 1
  ram[0x2001] = 0x00;
  dsp[kEsa] = 0x20;
  dsp[kEdl] = 0x00;    // one entry
  dsp[kFlg] = 0x00;    // echo writes enabled
  dsp[kFir7] = 0x40;
  dsp[kEfb] = 0x80;    // -128: the feedback inverts and holds its full scale
  dsp[kEon] = 0x00;    // no voice send, so the write is feedback alone
  step(dsp, ram);
  EXPECT_EQ(entry16(ram, 0x2000), 0);
}

TEST(EchoFeedback, TheFirOutputDecaysThroughEfbAcrossPasses) {
  // With one entry, no voices (EON send 0), a unity newest tap (FIR7 40h) and echo
  // feedback EFB 40h, each pass writes echo_input = (fir*EFB)>>7 back over the
  // entry (fullsnes 3281-3283). fir is the read sample buf = stored>>1, so the
  // stored value quarters each pass: 4000h -> 1000h -> 0400h -> 0100h. This decay,
  // visible in RAM, is the echo repeating with decreasing volume.
  DspState dsp;
  Ram ram{};
  ram[0x2000] = 0x00;  // seed the entry with 4000h
  ram[0x2001] = 0x40;
  dsp[kEsa] = 0x20;
  dsp[kEdl] = 0x00;
  dsp[kFlg] = 0x00;    // echo writes enabled
  dsp[kFir7] = 0x40;
  dsp[kEfb] = 0x40;
  step(dsp, ram);
  EXPECT_EQ(entry16(ram, 0x2000), 0x1000);  // 16384 read -> written 4096
  step(dsp, ram);
  EXPECT_EQ(entry16(ram, 0x2000), 0x0400);  // 1024
  step(dsp, ram);
  EXPECT_EQ(entry16(ram, 0x2000), 0x0100);  // 256
}

TEST(EchoFeedback, DisablingEchoWritesFreezesTheBufferButKeepsTheOutput) {
  // FLG bit 5 disables echo writes without disabling reads: "the echo buffer is
  // still output" (fullsnes 3074, 3078-3079; Anomie 259-264 "a static sample
  // buffer"). The entry does not change across passes, yet the FIR output still
  // reaches the frame through EVOL.
  DspState dsp;
  Ram ram{};
  ram[0x2000] = 0x00;  // 4000h
  ram[0x2001] = 0x40;
  dsp[kEsa] = 0x20;
  dsp[kEdl] = 0x00;
  dsp[kFlg] = 0x20;    // writes disabled
  dsp[kFir7] = 0x40;
  dsp[kEfb] = 0x40;
  dsp[kEvolLeft] = 0x40;
  const StereoFrame first = step(dsp, ram);
  const StereoFrame second = step(dsp, ram);
  EXPECT_EQ(entry16(ram, 0x2000), 0x4000);  // frozen
  EXPECT_EQ(first.left, 4096);              // fir 8192 * EVOL 64 >> 7
  EXPECT_EQ(second.left, 4096);             // still output, unchanged
}

TEST(EchoFeedback, EchoProcessingContinuesUnderMuteAndSoftReset) {
  // Both mute (bit 6) and soft reset (bit 7) leave the echo unit running: "all
  // sound/echo generation is kept operating" (fullsnes 3079-3081), so the feedback
  // write still decays the buffer (4000h read -> 1000h written) under either. They
  // differ at the output gate — only mute zeroes the emitted frame (fullsnes 3033
  // gates the sum on MUTE alone); soft reset silences the voices but leaves the
  // echo output, so the FIR contribution still reaches the frame through EVOL.
  struct Case {
    std::uint8_t flg;
    std::int16_t expectedLeft;
  };
  for (const Case c : {Case{.flg = 0x40, .expectedLeft = 0},        // mute: frame silenced
                       Case{.flg = 0x80, .expectedLeft = 4096}}) {  // soft reset: echo survives
    DspState dsp;
    Ram ram{};
    ram[0x2000] = 0x00;  // 4000h
    ram[0x2001] = 0x40;
    dsp[kEsa] = 0x20;
    dsp[kEdl] = 0x00;
    dsp[kFlg] = c.flg;    // both leave echo writes enabled (bit 5 clear)
    dsp[kFir7] = 0x40;
    dsp[kEfb] = 0x40;
    dsp[kEvolLeft] = 0x40;
    const StereoFrame frame = step(dsp, ram);
    EXPECT_EQ(frame.left, c.expectedLeft) << "flg " << static_cast<int>(c.flg);
    EXPECT_EQ(entry16(ram, 0x2000), 0x1000) << "flg " << static_cast<int>(c.flg);  // still written
  }
}

// ── The ring buffer index and EDL sizing ─────────────────────────────────────

TEST(EchoRing, EdlZeroGivesASingleEntryBuffer) {
  // "when D=0 the buffer is 4 bytes (1 sample)" (Anomie 899-901). The latched count
  // EDL<<9 is 0, and the wrap rule (increment, then if idx>=count reset to 0) keeps
  // the index at 0 forever, reusing one entry.
  DspState dsp;
  Ram ram{};
  dsp[kEsa] = 0x20;
  dsp[kEdl] = 0x00;
  for (int n = 0; n < 5; ++n) step(dsp, ram);
  EXPECT_EQ(dsp.echoIndex, 0u);
  EXPECT_EQ(dsp.echoLength, 0u);
}

TEST(EchoRing, EdlSizesTheRingInEntries) {
  // The entry count is EDL<<9 (Anomie 899; fullsnes 3249 idx_max = EDL<<9). EDL 1
  // gives 512 entries, and the index walks 0, 1, 2, ... latched at the first idx 0.
  DspState dsp;
  Ram ram{};
  dsp[kEsa] = 0x20;
  dsp[kEdl] = 0x01;
  step(dsp, ram);
  EXPECT_EQ(dsp.echoLength, 512u);  // latched at index 0 on the first sample
  EXPECT_EQ(dsp.echoIndex, 1u);
  step(dsp, ram);
  step(dsp, ram);
  EXPECT_EQ(dsp.echoIndex, 3u);
}

TEST(EchoRing, EdlIsConsultedOnlyWhenTheIndexIsZero) {
  // "If idx==0, set idx_max = EDL<<9" — the count is latched only at the wrap point
  // (fullsnes 3247-3253; Anomie 904-910). Changing EDL while the index is non-zero
  // does not resize the ring until it next returns to 0.
  DspState dsp;
  Ram ram{};
  dsp[kEsa] = 0x20;
  dsp[kEdl] = 0x01;
  step(dsp, ram);            // index 0 -> 1, length latched 512
  dsp[kEdl] = 0x0F;          // request a larger buffer mid-traversal
  for (int n = 0; n < 5; ++n) step(dsp, ram);
  EXPECT_EQ(dsp.echoLength, 512u);  // still the old size — index never hit 0

  // Returning to index 0 re-latches: from the single-entry buffer (index pinned at
  // 0), a raised EDL takes effect on the very next sample.
  DspState reland;
  Ram ram2{};
  reland[kEsa] = 0x20;
  reland[kEdl] = 0x00;
  step(reland, ram2);        // index stays 0, length 0
  reland[kEdl] = 0x01;
  step(reland, ram2);        // at index 0, re-latches 512
  EXPECT_EQ(reland.echoLength, 512u);
  EXPECT_EQ(reland.echoIndex, 1u);
}

// ── End to end through the machine ───────────────────────────────────────────

TEST(ApuEcho, ASoundingEonVoiceEchoesIntoItsBufferAndTheFrame) {
  // The whole path through the machine: a voice sounding through Direct Gain, its
  // EON bit set and FLG cleared (echo writes enabled), a one-entry buffer at 2000h
  // with a unity newest FIR tap and full echo volume. The Apu drives 32 kHz
  // samples; the echo unit writes the send into RAM (proving the machine's writable
  // path) and the FIR output reaches the frames through EVOL.
  ApuState s{};
  soundingVoice(s.dsp, 0, 0x40, 0x40);  // send 1294 into the echo mix
  s.dsp[kEon] = 0x01;
  s.dsp[kFlg] = 0x00;                    // echo writes enabled (power-on FLG is E0h)
  s.dsp[kEsa] = 0x20;
  s.dsp[kEdl] = 0x00;
  s.dsp[kFir7] = 0x40;                   // unity newest tap
  s.dsp[kEvolLeft] = 0x7F;
  s.dsp[kEvolRight] = 0x7F;
  s.dsp[kMvolLeft] = 0x00;               // isolate the echo output in the frame
  s.dsp[kMvolRight] = 0x00;

  Apu apu(s);
  apu.run(64 * 32);  // many 32-cycle samples, well past the buffer round-trip
  const std::vector<StereoFrame> frames = apu.takeFrames();

  ASSERT_FALSE(frames.empty());
  EXPECT_NE(entry16(apu.state().ram, 0x2000), 0);  // the machine wrote the buffer
  bool anyEcho = false;
  for (const StereoFrame& f : frames)
    if (f.left != 0) anyEcho = true;
  EXPECT_TRUE(anyEcho);  // the FIR output reached the frame through EVOL
}

}  // namespace
