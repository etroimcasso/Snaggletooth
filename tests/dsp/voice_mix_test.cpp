// The DSP output stage: envelope application, VxOUTX, the per-voice left/right
// volume, the eight-voice sum, and the master volume into a 32 kHz stereo frame
// — plus the frames the Apu surfaces as it runs.
//
// Expected values are hand-derived from fullsnes's output pipeline (the primary
// contract): VxOUTX at lines 2950-2954 (the high byte of the internal
// -4000h..+3FFFh amplitude), the Output Mixer at 3021-3034 (each voice added as
// sample*VxVOL SAR 6 with 16-bit overflow handling after every addition, then
// sum*MVOL SAR 7), the VxVOL registers at 3012-3019, the MVOL registers at
// 3005-3010, and the five empty key-on samples at 3053-3055. Anomie's S-DSP doc
// is the cross-check: it states the same chain (lines 40-63) and pins the
// per-voice shift by converting the 15-bit sample "to 16-bits by adding a 0 bit
// on the low end" before the >>7 volume adjust — the dropped BRR low bit
// recovered — which is fullsnes's SAR 6 on the raw 15-bit amplitude.
//
// The master volume is +127 ($7F) in every sounding setup: sum*127 SAR 7, which
// is 127/128 of the summed mix (there is no unity master volume, since $7F is
// 127/128). Each frame below is the dry per-voice sum passed through that scale.
//
// The interpolated amplitude feeding the output stage is isolated to a single
// Gaussian tap so its value is exact: a window with only `older` set reads
// gauss[1FFh]=519h at index 0 (the table's last entry, per the pitch/gauss
// suite), so the kernel returns `older`*519h>>10>>1 — for older=800h that is
// 519h, with no wrap.

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "snaggletooth/apu/apu.h"
#include "snaggletooth/apu/dsp.h"

namespace {

using snaggletooth::Apu;
using snaggletooth::ApuState;
using snaggletooth::DspState;
using snaggletooth::EnvPhase;
using snaggletooth::interpolatedSample;
using snaggletooth::stepDspSample;
using snaggletooth::StereoFrame;

using Ram = std::array<std::uint8_t, 65536>;

constexpr std::uint8_t kKon = 0x4C;
constexpr std::uint8_t kDir = 0x5D;

std::uint8_t& volLeft(DspState& dsp, std::size_t v) { return dsp[v * 0x10 + 0x00]; }
std::uint8_t& volRight(DspState& dsp, std::size_t v) { return dsp[v * 0x10 + 0x01]; }
std::uint8_t& gain(DspState& dsp, std::size_t v) { return dsp[v * 0x10 + 0x07]; }
std::uint8_t outx(DspState& dsp, std::size_t v) { return dsp[v * 0x10 + 0x09]; }

void writeBlock(Ram& ram, std::uint16_t address, std::uint8_t header,
                std::array<std::uint8_t, 8> data) {
  ram[address] = header;
  for (std::size_t i = 0; i < 8; ++i)
    ram[static_cast<std::uint16_t>(address + 1 + i)] = data[i];
}

void writeDirectoryEntry(Ram& ram, std::uint8_t dir, std::uint8_t srcn,
                         std::uint16_t start, std::uint16_t loop) {
  const std::uint16_t base = static_cast<std::uint16_t>(dir * 0x100 + srcn * 4);
  ram[base] = static_cast<std::uint8_t>(start & 0xFF);
  ram[static_cast<std::uint16_t>(base + 1)] = static_cast<std::uint8_t>(start >> 8);
  ram[static_cast<std::uint16_t>(base + 2)] = static_cast<std::uint8_t>(loop & 0xFF);
  ram[static_cast<std::uint16_t>(base + 3)] = static_cast<std::uint8_t>(loop >> 8);
}

// Places voice `v` mid-play with a single-tap window (interp = older*519h>>11),
// a fixed Direct-Gain envelope, a stationary pitch counter, and the given
// left/right volume — so one stepDspSample exercises the output stage alone.
// Direct Gain (VxGAIN bit 7 clear) drives the level to (gain&7Fh)<<4 every
// sample, so the envelope is constant and the frame is exact.
void placeAmplitudeVoice(DspState& dsp, std::size_t v, std::int16_t older,
                         std::uint8_t directGain, std::uint8_t left, std::uint8_t right) {
  dsp.voices[v].window = {.newest = 0, .old = 0, .older = older, .oldest = 0};
  dsp.voices[v].pitchCounter = 0;   // index 0, and pitch 0 keeps it stationary
  dsp.voices[v].phase = EnvPhase::Sustain;  // any non-Release phase; Direct Gain ignores it
  dsp.voices[v].konDelay = 0;
  gain(dsp, v) = directGain;        // bit 7 clear -> Direct Gain, level (gain&7Fh)<<4
  volLeft(dsp, v) = left;
  volRight(dsp, v) = right;
  dsp[0x0C] = 0x7F;                 // MVOLL = +127 (sounding: sum*127 SAR 7)
  dsp[0x1C] = 0x7F;                 // MVOLR = +127
}

// ── Silence ─────────────────────────────────────────────────────────────────

TEST(OutputStage, NoActiveVoicesSumToSilence) {
  // Every voice powers on in Release at envelope 0, so a sample with nothing
  // keyed is 0/0 — and the per-sample tick still advances.
  DspState dsp;
  Ram ram{};
  const StereoFrame frame = stepDspSample(dsp, std::span<const std::uint8_t, 65536>{ram});
  EXPECT_EQ(frame.left, 0);
  EXPECT_EQ(frame.right, 0);
  EXPECT_EQ(dsp.sampleIndex, 1u);          // tickDspSample ran
  EXPECT_EQ(dsp.globalCounter, 0x77FF);    // decremented from 0, wrapping
}

// ── Envelope, VxOUTX and per-voice volume ───────────────────────────────────

TEST(OutputStage, EnvelopeAndVolumeScaleAPositiveVoice) {
  // older=800h isolates gauss[1FFh]=519h, so interp = 519h = 1305. Direct Gain
  // 7Fh gives envelope 7F0h = 2032, so amplitude = (1305 * 2032) >> 11 = 1294.
  // VxOUTX is the high byte: (1294 >> 7) = 0Ah. VxVOLL=+40h gives (1294*64)>>6 =
  // 1294 exactly (the SAR-6/low-bit-recovery: a 40h volume is unity, not half),
  // and VxVOLR=-40h inverts the phase to -1294. The master volume then scales:
  // (1294*127)>>7 = 1283, and (-1294*127)>>7 = -1284 (SAR 7 rounds toward -inf).
  DspState dsp;
  Ram ram{};
  placeAmplitudeVoice(dsp, 0, 0x0800, 0x7F, 0x40, 0xC0);
  ASSERT_EQ(interpolatedSample(dsp, 0), 0x519);

  const StereoFrame frame = stepDspSample(dsp, std::span<const std::uint8_t, 65536>{ram});
  EXPECT_EQ(outx(dsp, 0), 0x0A);  // VxOUTX is pre-master-volume, unchanged
  EXPECT_EQ(frame.left, 1283);
  EXPECT_EQ(frame.right, -1284);
}

TEST(OutputStage, UnityVolumeRecoversTheDroppedLowBit) {
  // The per-voice volume shift is 6, not 7: a +40h (=64) volume scales the
  // amplitude by 64/64 = 1. A >>7 would halve it. Through the +127 master volume
  // the frame is (1294*127)>>7 = 1283; a per-voice >>7 would have made it
  // (647*127)>>7 = 641, so 1283 still proves the recovered low bit (fullsnes
  // Output Mixer SAR 6 / Anomie's 15->16-bit conversion before the >>7 adjust).
  DspState dsp;
  Ram ram{};
  placeAmplitudeVoice(dsp, 0, 0x0800, 0x7F, 0x40, 0x40);
  const StereoFrame frame = stepDspSample(dsp, std::span<const std::uint8_t, 65536>{ram});
  EXPECT_EQ(frame.left, 1283);   // (amplitude 1294)*127>>7, not (647)*127>>7=641
  EXPECT_EQ(frame.right, 1283);
}

TEST(OutputStage, NegativeAmplitudeGivesASignedOutxAndFrame) {
  // older=-800h -> interp = -519h = -1305; amplitude = (-1305 * 2032) >> 11 =
  // -1295 (arithmetic shift rounds toward -inf). VxOUTX = (-1295 >> 7) & FFh =
  // -11 & FFh = F5h. VxVOLL=+40h keeps the sign: (-1295 * 64) >> 6 = -1295, and
  // the +127 master volume gives (-1295*127)>>7 = -1285.
  DspState dsp;
  Ram ram{};
  placeAmplitudeVoice(dsp, 0, static_cast<std::int16_t>(-0x0800), 0x7F, 0x40, 0x40);
  ASSERT_EQ(interpolatedSample(dsp, 0), -0x519);

  const StereoFrame frame = stepDspSample(dsp, std::span<const std::uint8_t, 65536>{ram});
  EXPECT_EQ(outx(dsp, 0), 0xF5);  // VxOUTX is pre-master-volume, unchanged
  EXPECT_EQ(frame.left, -1285);
  EXPECT_EQ(frame.right, -1285);
}

// ── Eight-voice sum with per-addition clamp ─────────────────────────────────

TEST(OutputStage, VoicesSumTogether) {
  // Two voices of the same amplitude add. older=800h, gain 7Fh, VxVOLL=7Fh:
  // amplitude 1294, contribution (1294*127)>>6 = 2567. Two of them = 5134, and
  // the +127 master volume gives (5134*127)>>7 = 5093.
  DspState dsp;
  Ram ram{};
  placeAmplitudeVoice(dsp, 0, 0x0800, 0x7F, 0x7F, 0x00);
  placeAmplitudeVoice(dsp, 1, 0x0800, 0x7F, 0x7F, 0x00);
  const StereoFrame frame = stepDspSample(dsp, std::span<const std::uint8_t, 65536>{ram});
  EXPECT_EQ(frame.left, 5093);
  EXPECT_EQ(frame.right, 0);
}

TEST(OutputStage, ThePositiveSumClampsToSignedSixteenBits) {
  // older=3000h -> interp 3D2Ch>>1 = 1E96h = 7830 (no wrap: 519h*3000h>>10 =
  // 15660 < 8000h). Direct Gain 7Fh -> amplitude (7830*2032)>>11 = 7768;
  // VxVOLL=7Fh -> contribution (7768*127)>>6 = 15414. Two voices = 30828 (under
  // the cap); a third would reach 46242, so the sum clamps to 7FFFh rather than
  // wrapping — the "16-bit overflow handling after each addition". The +127
  // master volume then gives (7FFFh*127)>>7 = 32511; had the sum wrapped to
  // -19294 the frame would be -19143, so 32511 discriminates clamp from wrap.
  DspState dsp;
  Ram ram{};
  for (std::size_t v = 0; v < 3; ++v) placeAmplitudeVoice(dsp, v, 0x3000, 0x7F, 0x7F, 0x00);
  const StereoFrame frame = stepDspSample(dsp, std::span<const std::uint8_t, 65536>{ram});
  EXPECT_EQ(frame.left, 32511);
}

TEST(OutputStage, TheNegativeSumClampsToSignedSixteenBits) {
  // The same three voices with an inverting VxVOLL (81h = -127): each
  // contribution is (7768 * -127) >> 6 = -15415, and three sum past -8000h, so
  // the mix clamps to -8000h. The +127 master volume gives (-8000h*127)>>7 =
  // -32512, discriminating the clamp from a wrap the same way.
  DspState dsp;
  Ram ram{};
  for (std::size_t v = 0; v < 3; ++v) placeAmplitudeVoice(dsp, v, 0x3000, 0x7F, 0x81, 0x00);
  const StereoFrame frame = stepDspSample(dsp, std::span<const std::uint8_t, 65536>{ram});
  EXPECT_EQ(frame.left, -32512);
}

// ── End+Mute through the frame loop ─────────────────────────────────────────

TEST(OutputStage, EnteringAnEndMuteBlockReleasesTheVoice) {
  // A voice streaming out of a normal block into a code-1 End+Mute block goes to
  // Release with the envelope at 0 the sample it enters (fullsnes line 2712:
  // "End+Mute ... Release, Env=000h"). The frame loop derives brrEndMute from
  // the stream advance and hands it to the envelope step.
  DspState dsp;
  Ram ram{};
  writeBlock(ram, 0x1000, 0xC0, {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11});  // normal
  writeBlock(ram, 0x1009, 0xC1, {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11});  // End+Mute
  dsp[kDir] = 0x02;
  writeDirectoryEntry(ram, 0x02, 0, 0x1000, 0x1000);

  // Mid-play at the last sample of the normal block, one unity-pitch step from
  // the boundary, sounding via Direct Gain.
  placeAmplitudeVoice(dsp, 0, 0x0800, 0x7F, 0x40, 0x40);
  dsp.voices[0].brrAddress = 0x1000;
  dsp.voices[0].brrSampleIndex = 15;
  dsp[0x02] = 0x00;  // VxPITCHL/H = 1000h: one stream sample per output sample
  dsp[0x03] = 0x10;

  const std::span<const std::uint8_t, 65536> ram_span{ram};
  static_cast<void>(stepDspSample(dsp, ram_span));  // finishes the normal block, still sounding
  EXPECT_EQ(dsp.voices[0].phase, EnvPhase::Sustain);
  ASSERT_GT(dsp.voices[0].envelope, 0);

  static_cast<void>(stepDspSample(dsp, ram_span));  // crosses into the End+Mute block
  EXPECT_EQ(dsp.voices[0].phase, EnvPhase::Release);
  EXPECT_EQ(dsp.voices[0].envelope, 0);
}

// ── Key-on startup through the frame loop ───────────────────────────────────

TEST(OutputStage, KeyOnIsSilentForFiveSamplesThenSounds) {
  // "there are 5 'empty' samples before envelope updates and BRR decoding
  // actually begin" (fullsnes line 3053-3055) — the keying load precedes voice
  // 0's compute in the slot they share, so the keying sample itself takes the
  // first of the five silent calls and the following four samples the rest.
  // The sixth call takes the first envelope step, and voice 0's output rides
  // one sample behind — its amplitude is computed at the last slot of a sample
  // and applied at the next sample's start — so its first sounding frame is
  // the seventh after the keying frame began.
  DspState dsp;
  Ram ram{};
  writeBlock(ram, 0x1000, 0xC0, {0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77});
  dsp[kDir] = 0x02;
  writeDirectoryEntry(ram, 0x02, 0, 0x1000, 0x1000);
  dsp[0x02] = 0x00;  // unity pitch
  dsp[0x03] = 0x10;
  gain(dsp, 0) = 0x7F;    // Direct Gain, a full fixed level once sounding
  volLeft(dsp, 0) = 0x7F;
  volRight(dsp, 0) = 0x81;  // inverted right channel
  dsp[0x0C] = 0x7F;         // MVOLL = +127
  dsp[0x1C] = 0x7F;         // MVOLR = +127
  dsp[kKon] = 0x01;          // the register a driver wrote, kept for read-back
  dsp.internalKon = 0x01;    // and the key-on it armed, for the first (even) poll

  const std::span<const std::uint8_t, 65536> ram_span{ram};
  const StereoFrame first = stepDspSample(dsp, ram_span);  // poll keys on; startup begins
  EXPECT_EQ(first.left, 0);
  EXPECT_EQ(first.right, 0);
  for (int sample = 2; sample <= 6; ++sample) {
    const StereoFrame f = stepDspSample(dsp, ram_span);
    EXPECT_EQ(f.left, 0) << "startup sample " << sample;
    EXPECT_EQ(f.right, 0) << "startup sample " << sample;
  }
  const StereoFrame sounding = stepDspSample(dsp, ram_span);  // seventh sample sounds
  EXPECT_GT(sounding.left, 0);
  EXPECT_LT(sounding.right, 0);                                // the inverted right channel
}

// ── The Apu surfaces frames as it runs ──────────────────────────────────────

// A machine whose RAM is all NOPs (00h) at PC 0, with one voice preset to sound
// through Direct Gain at a stationary pitch (and the master volume placed by the
// helper), so each 32 kHz sample is the same exact frame. FLG is 0 in this
// default state, so nothing is muted or soft-reset.
ApuState soundingMachine() {
  ApuState s{};
  placeAmplitudeVoice(s.dsp, 0, 0x0800, 0x7F, 0x40, 0xC0);  // frame {1283, -1284}
  return s;
}

TEST(ApuFrames, RunEmitsOneFramePerThirtyTwoCycles) {
  Apu apu(soundingMachine());
  apu.run(64);  // 32 NOPs of 2 cycles -> two 32-cycle sample boundaries
  const std::vector<StereoFrame> frames = apu.takeFrames();
  ASSERT_EQ(frames.size(), 2u);
  for (const StereoFrame& f : frames) {
    EXPECT_EQ(f.left, 1283);    // (1294*127)>>7
    EXPECT_EQ(f.right, -1284);  // (-1294*127)>>7
  }
}

TEST(ApuFrames, TakeFramesDrainsTheQueue) {
  Apu apu(soundingMachine());
  apu.run(64);
  EXPECT_FALSE(apu.takeFrames().empty());
  EXPECT_TRUE(apu.takeFrames().empty());  // drained
}

TEST(ApuFrames, AShortRunEmitsNoFrame) {
  // Fewer than 32 cycles crosses no sample boundary, so no frame is produced.
  Apu apu(soundingMachine());
  apu.run(16);
  EXPECT_TRUE(apu.takeFrames().empty());
}

TEST(ApuFrames, TheFrameStreamIsDeterministic) {
  Apu a(soundingMachine());
  Apu b(soundingMachine());
  a.run(320);
  b.run(320);
  EXPECT_EQ(a.takeFrames(), b.takeFrames());
}

TEST(ApuFrames, RestoreDiscardsPendingFrames) {
  Apu apu(soundingMachine());
  apu.run(64);                     // accumulate frames without draining
  apu.restore(soundingMachine());  // a fresh state abandons them
  EXPECT_TRUE(apu.takeFrames().empty());
}

}  // namespace
