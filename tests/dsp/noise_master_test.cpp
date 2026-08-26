// The S-DSP's non-echo completion: the shared noise generator, pitch modulation
// (PMON), the master output stage (MVOL, mute), and the FLG soft-reset lane.
//
// Every expected value is derived from fullsnes (the primary contract),
// cross-checked against Anomie's S-DSP doc:
//  * Noise LFSR + seed -4000h: fullsnes 3097-3110; Anomie 826-837.
//  * NON substitution is pre-envelope, no pitch/Gauss, BRR keeps decoding:
//    fullsnes 3103-3116; Anomie 826-844.
//  * PMON step = (base * ((amp(x-1) SAR 4) + 400h)) SAR 10, voices 1-7, capped
//    at 128 kHz: fullsnes 2771-2794; Anomie 813-824, 372-374.
//  * Output Mixer sum*MVOL SAR 7 (truncating; -128 wraps), then mute:
//    fullsnes 3005-3033; Anomie 40-54, 657-682.
//  * FLG reset value E0h; bit 7 keys off + envelope 0, polled every sample:
//    fullsnes 3069-3081, 3133; Anomie 772-777.
//
// The single-tap window trick from the output-stage suite is reused: a window
// with only `older` set reads gauss[1FFh]=519h at index 0, so a voice with
// older=800h and Direct Gain 7Fh (envelope 7F0h) has amplitude 1294.

#include <array>
#include <cstddef>
#include <cstdint>

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

// DSP register addresses (raw offsets, matching the hardware map).
constexpr std::uint8_t kMvolLeft = 0x0C;
constexpr std::uint8_t kMvolRight = 0x1C;
constexpr std::uint8_t kPmon = 0x2D;
constexpr std::uint8_t kNon = 0x3D;
constexpr std::uint8_t kDir = 0x5D;
constexpr std::uint8_t kFlg = 0x6C;

std::uint8_t& reg(DspState& dsp, std::size_t v, std::uint8_t off) { return dsp[v * 0x10 + off]; }
std::uint8_t outx(const DspState& dsp, std::size_t v) { return dsp[v * 0x10 + 0x09]; }

std::span<const std::uint8_t, 65536> view(const Ram& ram) {
  return std::span<const std::uint8_t, 65536>{ram};
}

// Advances one 32 kHz sample and returns the frame. Wraps the [[nodiscard]]
// stepDspSample so a test that only cares about the advanced state can ignore it.
StereoFrame step(DspState& dsp, const Ram& ram) { return stepDspSample(dsp, view(ram)); }

// Sets voice `v` to a constant amplitude of 1294: older=800h isolates
// gauss[1FFh]=519h at index 0, Direct Gain 7Fh holds envelope 7F0h, and pitch 0
// keeps it stationary. Left/right volume default to unity (40h).
void placeAmplitude1294(DspState& dsp, std::size_t v, std::uint8_t left = 0x40,
                        std::uint8_t right = 0x40) {
  dsp.voices[v].window = {.newest = 0, .old = 0, .older = 0x0800, .oldest = 0};
  dsp.voices[v].pitchCounter = 0;
  dsp.voices[v].phase = EnvPhase::Sustain;
  dsp.voices[v].konDelay = 0;
  // The level scaling a sample is the standing one, so a voice placed mid-play
  // carries its Direct-Gain level from the start rather than reaching it on the
  // first step.
  dsp.voices[v].envelope = 0x7F0;
  reg(dsp, v, 0x07) = 0x7F;   // Direct Gain -> envelope 7F0h
  reg(dsp, v, 0x00) = left;
  reg(dsp, v, 0x01) = right;
}

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

// ── Noise generator ─────────────────────────────────────────────────────────

TEST(Noise, SeedsToNegativeFullScale) {
  // The noise level is -4000h at power-on (fullsnes 3103; Anomie 834).
  DspState dsp;
  EXPECT_EQ(dsp.noiseLevel, static_cast<std::int16_t>(-0x4000));
}

TEST(Noise, SteppingFollowsTheDocumentedLfsr) {
  // Level = ((Level SHR 1) AND 3FFFh) OR ((bit0 XOR bit1) SHL 14), at the FLG
  // rate. Rate 1Fh fires every sample. From the -4000h seed (pattern 4000h) the
  // low bits are zero, so the pattern halves: 2000h, 1000h, 0800h, 0400h.
  DspState dsp;
  Ram ram{};
  dsp[kFlg] = 0x1F;  // noise rate 31 (fires every sample), no mute/reset
  const std::array<std::int16_t, 4> expected = {0x2000, 0x1000, 0x0800, 0x0400};
  for (std::size_t i = 0; i < expected.size(); ++i) {
    step(dsp, ram);
    EXPECT_EQ(dsp.noiseLevel, expected[i]) << "step " << i;
  }
}

TEST(Noise, FeedbackBitIsBit0XorBit1) {
  // A pattern with bit0 != bit1 feeds a 1 into bit 14. From +2 (pattern 0002h,
  // bit0=0 bit1=1) the next level is (0001h | 4000h) = 4001h, i.e. -3FFFh.
  DspState dsp;
  Ram ram{};
  dsp[kFlg] = 0x1F;
  dsp.noiseLevel = 0x0002;
  step(dsp, ram);
  EXPECT_EQ(dsp.noiseLevel, static_cast<std::int16_t>(-0x3FFF));
}

TEST(Noise, RateZeroHoldsTheLevel) {
  // FLG noise rate 0 is 'stop' — the level never advances (fullsnes 3073).
  DspState dsp;
  Ram ram{};
  dsp[kFlg] = 0x00;  // rate 0
  for (int i = 0; i < 8; ++i) step(dsp, ram);
  EXPECT_EQ(dsp.noiseLevel, static_cast<std::int16_t>(-0x4000));  // unchanged
}

// ── NON substitution ────────────────────────────────────────────────────────

TEST(Non, SubstitutesTheNoiseLevelForTheInterpolatedSample) {
  // With the NON bit set the voice outputs the noise level, not its BRR sample.
  // The window is zero (interp 0), so a sounding voice proves the substitution:
  // noise 2000h through envelope 7F0h is amplitude (2000h*7F0h)>>11 = 8128,
  // VxOUTX = (8128>>7)&FFh = 3Fh. Without NON the same voice is silent.
  DspState dsp;
  Ram ram{};
  dsp[kFlg] = 0x00;         // rate 0 holds the noise level fixed for the read
  dsp.noiseLevel = 0x2000;
  dsp.voices[0].window = {};  // interpolation would give 0
  dsp.voices[0].pitchCounter = 0;
  dsp.voices[0].phase = EnvPhase::Sustain;
  dsp.voices[0].konDelay = 0;
  dsp.voices[0].envelope = 0x7F0;  // the standing level this sample is scaled by
  reg(dsp, 0, 0x07) = 0x7F;  // Direct Gain -> envelope 7F0h

  dsp[kNon] = 0x01;  // voice 0 outputs noise
  step(dsp, ram);
  EXPECT_EQ(outx(dsp, 0), 0x3F);

  dsp[kNon] = 0x00;  // control: BRR path, interp of a zero window
  dsp.voices[0].pitchCounter = 0;
  step(dsp, ram);
  step(dsp, ram);  // voice 0's VxOUTX reads one sample behind its output, so catch up
  EXPECT_EQ(outx(dsp, 0), 0x00);
}

TEST(Non, IgnoresPitchAndInterpolation) {
  // Noise is not pitch-adjusted or Gaussian-interpolated: a NON voice with a
  // non-zero interpolation index and a loud window still outputs the flat noise
  // level (amplitude 3Fh from noise 2000h), not the window's interpolation.
  DspState dsp;
  Ram ram{};
  dsp[kFlg] = 0x00;
  dsp.noiseLevel = 0x2000;
  dsp.voices[0].window = {.newest = 0x3FFF, .old = 0x3FFF, .older = 0x3FFF, .oldest = 0x3FFF};
  dsp.voices[0].pitchCounter = 0x0400;  // interpolation index 40h, mid-window
  dsp.voices[0].phase = EnvPhase::Sustain;
  dsp.voices[0].konDelay = 0;
  dsp.voices[0].envelope = 0x7F0;  // the standing level this sample is scaled by
  reg(dsp, 0, 0x07) = 0x7F;
  dsp[kNon] = 0x01;
  step(dsp, ram);
  EXPECT_EQ(outx(dsp, 0), 0x3F);  // the noise-derived amplitude, not the window's
}

TEST(Non, EndMuteBlockStillTerminatesNoise) {
  // Even under NON the BRR decoder runs; an End+Mute block releases the voice
  // with envelope 0, terminating the noise output (fullsnes 3111-3114). The
  // release lands at the sample after the crossing: the header check reads the
  // header standing at each sample's start, so it sees the entered block at the
  // next sample's check.
  DspState dsp;
  Ram ram{};
  writeBlock(ram, 0x1000, 0xC0, {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11});  // normal
  writeBlock(ram, 0x1009, 0xC1, {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11});  // End+Mute
  dsp[kDir] = 0x02;
  writeDirectoryEntry(ram, 0x02, 0, 0x1000, 0x1000);
  dsp[kFlg] = 0x00;
  dsp.noiseLevel = 0x2000;
  dsp[kNon] = 0x01;   // voice 0 on noise
  reg(dsp, 0, 0x07) = 0x7F;  // Direct Gain
  dsp.voices[0].phase = EnvPhase::Sustain;
  dsp.voices[0].konDelay = 0;
  dsp.voices[0].envelope = 0x7F0;  // the standing level this sample is scaled by
  dsp.voices[0].brrAddress = 0x1000;
  dsp.voices[0].brrSampleIndex = 15;  // one step from the block boundary
  reg(dsp, 0, 0x02) = 0x00;  // unity pitch
  reg(dsp, 0, 0x03) = 0x10;

  step(dsp, ram);  // finishes the normal block, still noising
  ASSERT_GT(dsp.voices[0].envelope, 0);
  step(dsp, ram);  // crosses into End+Mute; the crossing sample still noises
  step(dsp, ram);  // the standing check reads the header: the envelope terminates
  EXPECT_EQ(dsp.voices[0].phase, EnvPhase::Release);
  EXPECT_EQ(dsp.voices[0].envelope, 0);
  // The sample that zeroes the envelope is still scaled by the level standing
  // when it was taken, so silence reaches the amplitude on the sample after; and
  // voice 0's VxOUTX becomes readable at a slot falling in the sample after that.
  step(dsp, ram);
  step(dsp, ram);
  EXPECT_EQ(outx(dsp, 0), 0x00);  // noise silenced with the envelope
}

// ── Pitch modulation (PMON) ─────────────────────────────────────────────────

TEST(Pmon, ScalesTheStepByThePreviousVoiceAmplitude) {
  // Voice 0 amplitude 1294 -> factor = (1294 SAR 4) + 400h = 80 + 1024 = 1104.
  // Voice 1 base pitch 1000h -> modulated step (4096 * 1104) >> 10 = 4416, which
  // the pitch counter shows after one sample.
  DspState dsp;
  Ram ram{};
  dsp[kFlg] = 0x00;
  placeAmplitude1294(dsp, 0);
  dsp.voices[1] = {};
  dsp.voices[1].phase = EnvPhase::Release;
  reg(dsp, 1, 0x02) = 0x00;  // base pitch 1000h
  reg(dsp, 1, 0x03) = 0x10;
  dsp[kPmon] = 0x02;  // modulate voice 1 by voice 0
  step(dsp, ram);
  EXPECT_EQ(dsp.voices[1].pitchCounter, 4416);
}

TEST(Pmon, ASilentPreviousVoiceModulatesByUnity) {
  // A silent previous voice gives factor 400h, so the step is unchanged: base
  // pitch 1000h advances the counter by exactly 1000h.
  DspState dsp;
  Ram ram{};
  dsp[kFlg] = 0x00;
  dsp.voices[0] = {};  // voice 0 silent (envelope 0)
  dsp.voices[0].phase = EnvPhase::Release;
  dsp.voices[1] = {};
  dsp.voices[1].phase = EnvPhase::Release;
  reg(dsp, 1, 0x02) = 0x00;
  reg(dsp, 1, 0x03) = 0x10;  // base pitch 1000h
  dsp[kPmon] = 0x02;
  step(dsp, ram);
  EXPECT_EQ(dsp.voices[1].pitchCounter, 0x1000);
}

TEST(Pmon, Bit0AndVoice0AreNeverModulated) {
  // PMON bit 0 is unused; voice 0 has no previous voice. With PMON bit 0 set,
  // voice 0's step is its base pitch, unmodulated by anything.
  DspState dsp;
  Ram ram{};
  dsp[kFlg] = 0x00;
  dsp.voices[0] = {};
  dsp.voices[0].phase = EnvPhase::Release;
  reg(dsp, 0, 0x02) = 0x00;
  reg(dsp, 0, 0x03) = 0x10;  // base pitch 1000h
  dsp[kPmon] = 0x01;  // bit 0 set (has no effect)
  step(dsp, ram);
  EXPECT_EQ(dsp.voices[0].pitchCounter, 0x1000);
}

TEST(Pmon, TheStepIsCappedAt128kHz) {
  // A modulated step is capped at 3FFFh (128 kHz, four source samples per output
  // sample). Voice 0 amplitude 1294 (factor 1104) against base pitch 3FFFh would
  // give (16383*1104)>>10 = 17663; the cap holds the counter to 3FFFh instead.
  DspState dsp;
  Ram ram{};
  dsp[kFlg] = 0x00;
  placeAmplitude1294(dsp, 0);
  dsp.voices[1] = {};
  dsp.voices[1].phase = EnvPhase::Release;
  reg(dsp, 1, 0x02) = 0xFF;
  reg(dsp, 1, 0x03) = 0x3F;  // base pitch 3FFFh
  dsp[kPmon] = 0x02;
  step(dsp, ram);
  EXPECT_EQ(dsp.voices[1].pitchCounter, 0x3FFF);  // capped, not 17663 (44FFh)
}

// ── Master volume and mute ──────────────────────────────────────────────────

TEST(Master, NegativeVolumeInvertsThePhase) {
  // A negative MVOL inverts the sign: voice amplitude 1294 through MVOLL -40h is
  // (1294 * -64) >> 7 = -647, where +40h would give +647.
  DspState dsp;
  Ram ram{};
  dsp[kFlg] = 0x00;
  placeAmplitude1294(dsp, 0, 0x40, 0x40);
  dsp[kMvolLeft] = 0xC0;   // -64
  dsp[kMvolRight] = 0x40;  // +64
  const StereoFrame frame = step(dsp, ram);
  EXPECT_EQ(frame.left, -647);
  EXPECT_EQ(frame.right, 647);
}

TEST(Master, TheMinus128ProductWraps) {
  // MVOL -128 is the one master-volume multiply the docs let overflow: a
  // full-scale negative sum (-8000h, three voices clamping) times -128 is
  // +400000h >> 7 = 8000h, which truncates to -8000h. A clamp would have given
  // +7FFFh, so -8000h proves the truncating multiply.
  DspState dsp;
  Ram ram{};
  dsp[kFlg] = 0x00;
  for (std::size_t v = 0; v < 3; ++v) {
    dsp.voices[v].window = {.newest = 0, .old = 0, .older = 0x3000, .oldest = 0};
    dsp.voices[v].pitchCounter = 0;
    dsp.voices[v].phase = EnvPhase::Sustain;
    dsp.voices[v].konDelay = 0;
    dsp.voices[v].envelope = 0x7F0;  // the standing level this sample is scaled by
    reg(dsp, v, 0x07) = 0x7F;  // Direct Gain -> amplitude 7768
    reg(dsp, v, 0x00) = 0x81;  // VxVOLL -127 -> sum clamps to -8000h
  }
  dsp[kMvolLeft] = 0x80;  // MVOL -128
  const StereoFrame frame = step(dsp, ram);
  EXPECT_EQ(frame.left, static_cast<std::int16_t>(-0x8000));  // wrapped, not +7FFFh
}

TEST(Master, MuteZeroesTheFrameButNotInternalState) {
  // FLG bit 6 mutes the emitted frame to silence, while VxOUTX, the envelope and
  // the sample clock all still advance (mute stops output only).
  DspState dsp;
  Ram ram{};
  placeAmplitude1294(dsp, 0, 0x40, 0x40);
  dsp[kMvolLeft] = 0x7F;
  dsp[kMvolRight] = 0x7F;
  dsp[kFlg] = 0x40;  // mute only (rate 0, no soft-reset)
  const StereoFrame frame = step(dsp, ram);
  EXPECT_EQ(frame.left, 0);
  EXPECT_EQ(frame.right, 0);
  EXPECT_EQ(outx(dsp, 0), 0x0A);        // amplitude 1294 was still computed
  EXPECT_EQ(dsp.sampleIndex, 1u);       // the sample clock still ticked
}

// ── FLG seed and soft reset ─────────────────────────────────────────────────

TEST(Flg, ResetReseedsE0) {
  // reset() re-seeds FLG to E0h (the machine's power-on value), even after a
  // driver has cleared it.
  Apu apu;
  ApuState s = apu.state();
  s.dsp[kFlg] = 0x00;  // a driver cleared FLG
  apu.restore(s);
  apu.reset();
  EXPECT_EQ(apu.state().dsp[kFlg], 0xE0);
}

TEST(Flg, ResetReseedsTheNoiseLevel) {
  // The noise level is a reset value, not a free-running one: reset() returns it
  // to -4000h.
  Apu apu;
  ApuState s = apu.state();
  s.dsp.noiseLevel = 0x1234;
  apu.restore(s);
  apu.reset();
  EXPECT_EQ(apu.state().dsp.noiseLevel, static_cast<std::int16_t>(-0x4000));
}

TEST(SoftReset, SilencesAKeyedOnVoiceUntilItClears) {
  // FLG bit 7 keys every voice off and forces envelope 0 each sample. A voice
  // keyed on while it is set starts — the consumption sample itself is shielded
  // from the reset (`KON/kon then flg.80`) — and is re-silenced from the next
  // sample; nothing sounds until the bit clears and the voice is re-keyed.
  DspState dsp;
  Ram ram{};
  writeBlock(ram, 0x1000, 0xC0, {0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77});
  dsp[kDir] = 0x02;
  writeDirectoryEntry(ram, 0x02, 0, 0x1000, 0x1000);
  reg(dsp, 0, 0x02) = 0x00;  // unity pitch
  reg(dsp, 0, 0x03) = 0x10;
  reg(dsp, 0, 0x07) = 0x7F;  // Direct Gain
  reg(dsp, 0, 0x00) = 0x7F;  // VxVOLL
  dsp[kMvolLeft] = 0x7F;
  dsp[kMvolRight] = 0x7F;
  dsp[kFlg] = 0x80;        // soft reset
  dsp.internalKon = 0x01;  // a KON write arms voice 0 for the first (even) poll

  for (int i = 0; i < 8; ++i) {
    const StereoFrame f = step(dsp, ram);
    EXPECT_EQ(f.left, 0) << "soft-reset sample " << i;
    // The armed key-on is consumed once, at the first even poll, and its
    // consumption sample keeps the startup's Attack (the reset does not key a
    // voice off in the sample its key-on lands — `KON/kon then flg.80`). From
    // the next sample the standing reset keys it off like any other voice, and
    // nothing arms it again, so it reads Release and its envelope stays 0.
    if (i > 0) {
      EXPECT_EQ(dsp.voices[0].phase, EnvPhase::Release) << "soft-reset sample " << i;
    }
    EXPECT_EQ(dsp.voices[0].envelope, 0) << "soft-reset sample " << i;
  }

  dsp[kFlg] = 0x00;        // release the soft reset
  dsp.internalKon = 0x01;  // a second KON write re-arms it
  step(dsp, ram);          // the next even poll keys the voice on
  bool sounded = false;
  for (int i = 0; i < 8; ++i)
    if (step(dsp, ram).left != 0) { sounded = true; break; }
  EXPECT_TRUE(sounded);  // it sounds once the 5-sample startup completes
}

TEST(SoftReset, IsPolledEverySampleUnlikeKeyOff) {
  // Soft reset is polled every sample and every voice; KON/KOFF are polled only
  // on even samples. On an odd sample, setting FLG bit 7 silences the voice that
  // sample, while a KOFF write is not yet seen and the voice keeps sounding.
  auto sounding = [](Ram& ram) {
    DspState dsp;
    dsp.voices[0].window = {.newest = 0, .old = 0, .older = 0x0800, .oldest = 0};
    dsp.voices[0].pitchCounter = 0;
    dsp.voices[0].phase = EnvPhase::Sustain;
    dsp.voices[0].konDelay = 0;
    dsp.voices[0].envelope = 0x7F0;  // the standing level this sample is scaled by
    reg(dsp, 0, 0x07) = 0x7F;  // Direct Gain, sounds every sample
    reg(dsp, 0, 0x00) = 0x7F;
    dsp[kMvolLeft] = 0x7F;
    static_cast<void>(ram);
    return dsp;
  };

  // Voice 0's output rides one sample behind (its amplitude is computed at a
  // sample's last slot and applied at the next sample's start), so the immediacy
  // of the soft-reset poll shows in its envelope — zeroed the sample the bit is
  // read — rather than in that same frame's emitted output.
  Ram ram{};
  DspState softReset = sounding(ram);
  ASSERT_GT(stepDspSample(softReset, view(ram)).left, 0);  // sample 0 (even) sounds
  ASSERT_EQ(softReset.sampleIndex, 1u);                    // next poll is odd
  softReset[kFlg] = 0x80;
  static_cast<void>(stepDspSample(softReset, view(ram)));
  EXPECT_EQ(softReset.voices[0].envelope, 0);  // soft reset applied on the odd sample

  DspState keyOff = sounding(ram);
  ASSERT_GT(stepDspSample(keyOff, view(ram)).left, 0);
  ASSERT_EQ(keyOff.sampleIndex, 1u);
  keyOff[0x5C] = 0x01;  // KOFF voice 0, written before the odd sample
  static_cast<void>(stepDspSample(keyOff, view(ram)));
  EXPECT_GT(keyOff.voices[0].envelope, 0);  // KOFF not polled yet on the odd sample
}

}  // namespace
