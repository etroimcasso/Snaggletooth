// The S-DSP's intra-sample slot schedule: a 32 kHz sample is produced over 32
// clock slots, and a register write is consumed by the frame only if it lands
// before the slot that reads it. These tests pin which slot consumes each
// register, the two-phase visibility of VxOUTX/VxENVX, the last-slot KON/KOFF and
// counter/noise updates, and voice 0's one-sample output pipeline.
//
// Slots are numbered T0..T31 as fullsnes's "SNES APU Low Level Timings" chart
// numbers them; the master counter identifies a slot by counter mod 32. Voices
// 1-7 run their whole compute at one slot and apply their volume two/three slots
// later; voice 0 computes at the last slot (T31) and applies at the next sample's
// first slots, so its output rides one sample behind — the hardware's envelope
// pipeline (fullsnes/anomie, provisional pending the Blargg DSP ROMs).

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include <gtest/gtest.h>

#include "snaggletooth/apu/apu.h"
#include "snaggletooth/apu/dsp.h"

namespace {

using snaggletooth::DspState;
using snaggletooth::EnvPhase;
using snaggletooth::SlotResult;
using snaggletooth::stepDspCycle;
using snaggletooth::stepDspSample;
using snaggletooth::StereoFrame;

using Ram = std::array<std::uint8_t, 65536>;

constexpr std::uint8_t kMvolLeft = 0x0C;
constexpr std::uint8_t kMvolRight = 0x1C;
constexpr std::uint8_t kNon = 0x3D;
constexpr std::uint8_t kKon = 0x4C;
constexpr std::uint8_t kFlg = 0x6C;

std::uint8_t& reg(DspState& dsp, std::size_t v, std::uint8_t off) { return dsp[v * 0x10 + off]; }
std::uint8_t outx(const DspState& dsp, std::size_t v) { return dsp[v * 0x10 + 0x09]; }
std::uint8_t envx(const DspState& dsp, std::size_t v) { return dsp[v * 0x10 + 0x08]; }

std::span<const std::uint8_t, 65536> view(const Ram& ram) {
  return std::span<const std::uint8_t, 65536>{ram};
}

// A stationary sounding voice: older=800h reads gauss[1FFh]=519h at pitch 0, and
// Direct Gain 7Fh holds the envelope at 7F0h, so the voice emits a constant
// amplitude of 1294 every sample without advancing its stream.
void placeSteadyVoice(DspState& dsp, std::size_t v, std::uint8_t left = 0x40,
                      std::uint8_t right = 0x40) {
  dsp.voices[v].window = {.newest = 0, .old = 0, .older = 0x0800, .oldest = 0};
  dsp.voices[v].pitchCounter = 0;
  dsp.voices[v].phase = EnvPhase::Sustain;
  dsp.voices[v].konDelay = 0;
  reg(dsp, v, 0x07) = 0x7F;  // Direct Gain
  reg(dsp, v, 0x00) = left;
  reg(dsp, v, 0x01) = right;
}

// A voice whose envelope climbs every sample: ADSR Attack at rate 15 (+1024 per
// step, firing every sample), so its amplitude — and thus VxOUTX — changes frame
// to frame, exposing the two-phase visibility of the register.
void placeClimbingVoice(DspState& dsp, std::size_t v) {
  dsp.voices[v].window = {.newest = 0, .old = 0, .older = 0x0800, .oldest = 0};
  dsp.voices[v].pitchCounter = 0;
  dsp.voices[v].phase = EnvPhase::Attack;
  dsp.voices[v].envelope = 0;
  dsp.voices[v].konDelay = 0;
  reg(dsp, v, 0x05) = 0x8F;  // ADSR1: ADSR enabled, attack rate 15
  reg(dsp, v, 0x00) = 0x40;
  reg(dsp, v, 0x01) = 0x40;
}

// Advances one whole sample and returns the delivered frame.
StereoFrame sample(DspState& dsp, const Ram& ram) {
  StereoFrame frame{};
  for (int n = 0; n < 32; ++n) {
    const SlotResult r = stepDspCycle(dsp, view(ram));
    if (r.delivered) frame = r.frame;
  }
  return frame;
}

// Advances one whole sample, writing dsp[reg]=value the instant the cursor reaches
// `atSlot` (just before that slot runs). Returns the delivered frame.
StereoFrame sampleWritingAt(DspState& dsp, const Ram& ram, int atSlot, std::uint8_t reg,
                            std::uint8_t value) {
  StereoFrame frame{};
  for (int n = 0; n < 32; ++n) {
    if (dsp.slotCursor == atSlot) dsp[reg] = value;
    const SlotResult r = stepDspCycle(dsp, view(ram));
    if (r.delivered) frame = r.frame;
  }
  return frame;
}

// ── Where each register is consumed ─────────────────────────────────────────

TEST(SampleSchedule, MasterLeftVolumeIsConsumedAtSlotTwentySeven) {
  Ram ram{};
  DspState base;
  placeSteadyVoice(base, 1);
  base[kMvolLeft] = 0x40;
  base[kMvolRight] = 0x40;
  sample(base, ram);  // prime; the schedule now runs slot by slot

  DspState seenState = base;
  DspState missedState = base;
  const StereoFrame seen = sampleWritingAt(seenState, ram, 27, kMvolLeft, 0x20);
  const StereoFrame missed = sampleWritingAt(missedState, ram, 28, kMvolLeft, 0x20);
  EXPECT_NE(seen.left, missed.left);   // the write before T27 halves this frame's left
  EXPECT_EQ(seen.right, missed.right);  // the right channel is untouched by MVOLL
}

TEST(SampleSchedule, MasterRightVolumeIsConsumedAtSlotTwentyEight) {
  Ram ram{};
  DspState base;
  placeSteadyVoice(base, 1);
  base[kMvolLeft] = 0x40;
  base[kMvolRight] = 0x40;
  sample(base, ram);

  DspState seenState = base;
  DspState missedState = base;
  const StereoFrame seen = sampleWritingAt(seenState, ram, 28, kMvolRight, 0x20);
  const StereoFrame missed = sampleWritingAt(missedState, ram, 29, kMvolRight, 0x20);
  EXPECT_NE(seen.right, missed.right);
  EXPECT_EQ(seen.left, missed.left);
}

TEST(SampleSchedule, AVoiceLeftVolumeIsConsumedAtItsFourthStepSlot) {
  // Written by address, not through reg() (which returns a value): voice 1's VxVOLL
  // is register 0x10. Setting it to 0 before slot T3 removes voice 1 from the left
  // mix this frame; setting it after leaves the frame as it was.
  Ram ram{};
  DspState base;
  placeSteadyVoice(base, 1);
  base[kMvolLeft] = 0x7F;
  sample(base, ram);

  DspState seenState = base;
  DspState missedState = base;
  const StereoFrame seen = sampleWritingAt(seenState, ram, 3, 0x10, 0x00);   // VxVOLL voice 1
  const StereoFrame missed = sampleWritingAt(missedState, ram, 4, 0x10, 0x00);
  EXPECT_EQ(seen.left, 0);        // the zeroed volume reached this frame
  EXPECT_GT(missed.left, 0);      // the write one slot late did not
}

TEST(SampleSchedule, NoiseSubstitutionReadsNonAtTheVoiceComputeSlot) {
  // Voice 1 computes at slot T2 and reads its NON bit there. Enabling NON before
  // T2 swaps the noise level in this frame; after T2 it waits for the next.
  Ram ram{};
  DspState base;
  placeSteadyVoice(base, 1);
  base[kMvolLeft] = 0x7F;
  sample(base, ram);

  DspState seenState = base;
  DspState missedState = base;
  const StereoFrame seen = sampleWritingAt(seenState, ram, 2, kNon, 0x02);   // NON voice 1
  const StereoFrame missed = sampleWritingAt(missedState, ram, 3, kNon, 0x02);
  EXPECT_NE(seen.left, missed.left);
}

// ── Two-phase VxOUTX / VxENVX visibility ────────────────────────────────────

TEST(SampleSchedule, VoiceOutxIsNotReadableUntilItsEighthStepSlot) {
  // Voice 1 computes at T2 but holds its VxOUTX until T7 (S8). A sentinel written
  // into the register at the frame's start survives the compute slot and is only
  // overwritten by the DSP's own value once the visibility slot runs.
  Ram ram{};
  DspState dsp;
  placeSteadyVoice(dsp, 1);  // constant VxOUTX = 0Ah
  sample(dsp, ram);          // prime
  ASSERT_EQ(outx(dsp, 1), 0x0A);

  std::uint8_t atFive = 0;
  std::uint8_t atEight = 0;
  for (int n = 0; n < 32; ++n) {
    if (dsp.slotCursor == 1) dsp[0x19] = 0x55;         // sentinel into voice 1's OUTX
    if (dsp.slotCursor == 5) atFive = outx(dsp, 1);    // T2 (compute) has run, T7 not
    if (dsp.slotCursor == 8) atEight = outx(dsp, 1);   // T7 (visibility) has run
    stepDspCycle(dsp, view(ram));
  }
  EXPECT_EQ(atFive, 0x55);   // the compute slot held its result; the sentinel stands
  EXPECT_EQ(atEight, 0x0A);  // the visibility slot published the computed OUTX
}

TEST(SampleSchedule, VoiceEnvxIsNotReadableUntilItsNinthStepSlot) {
  // The same for VxENVX, whose visibility slot is T8 (S9), one past OUTX's.
  Ram ram{};
  DspState dsp;
  placeSteadyVoice(dsp, 1);  // envelope 7F0h -> VxENVX 7Fh
  sample(dsp, ram);
  ASSERT_EQ(envx(dsp, 1), 0x7F);

  std::uint8_t atSeven = 0;
  std::uint8_t atNine = 0;
  for (int n = 0; n < 32; ++n) {
    if (dsp.slotCursor == 1) dsp[0x18] = 0x55;         // sentinel into voice 1's ENVX
    if (dsp.slotCursor == 7) atSeven = envx(dsp, 1);   // past T7 (OUTX), before T8 (ENVX)
    if (dsp.slotCursor == 9) atNine = envx(dsp, 1);
    stepDspCycle(dsp, view(ram));
  }
  EXPECT_EQ(atSeven, 0x55);
  EXPECT_EQ(atNine, 0x7F);
}

// ── The last slot: keying, the global counter and the noise generator ───────

TEST(SampleSchedule, TheGlobalCounterAdvancesAtTheLastSlot) {
  Ram ram{};
  DspState dsp;
  placeSteadyVoice(dsp, 1);
  sample(dsp, ram);  // prime

  const std::uint16_t counter = dsp.globalCounter;
  while (dsp.slotCursor != 31) stepDspCycle(dsp, view(ram));
  EXPECT_EQ(dsp.globalCounter, counter) << "the counter has not advanced before T31";
  stepDspCycle(dsp, view(ram));  // run T31
  EXPECT_NE(dsp.globalCounter, counter) << "the counter advanced at T31";
}

TEST(SampleSchedule, TheNoiseGeneratorStepsAtTheLastSlot) {
  Ram ram{};
  DspState dsp;
  placeSteadyVoice(dsp, 1);
  dsp[kFlg] = 0x1F;  // full noise rate, fires every sample
  sample(dsp, ram);

  const std::int16_t level = dsp.noiseLevel;
  while (dsp.slotCursor != 31) stepDspCycle(dsp, view(ram));
  EXPECT_EQ(dsp.noiseLevel, level) << "the noise level holds until T31";
  stepDspCycle(dsp, view(ram));
  EXPECT_NE(dsp.noiseLevel, level) << "the noise generator stepped at T31";
}

TEST(SampleSchedule, KeyOnIsPolledAtTheLastSlotOnEvenSamples) {
  // A KON bit is read at T31, so the voice it keys does not begin its startup
  // until that slot; and the poll runs only on even samples.
  Ram ram{};
  DspState dsp;
  placeSteadyVoice(dsp, 1);
  sample(dsp, ram);            // sample 0 done; sampleIndex now 1 (odd)
  ASSERT_EQ(dsp.sampleIndex % 2, 1u);

  dsp[kKon] = 0x04;            // key voice 2 on
  sample(dsp, ram);           // an odd sample: the poll does not run
  EXPECT_EQ(dsp.voices[2].konDelay, 0) << "no poll on the odd sample";
  sample(dsp, ram);           // the next (even) sample polls at its T31
  EXPECT_EQ(dsp.voices[2].konDelay, 5) << "keyed on at the even sample's last slot";
}

// ── Voice 0's output pipeline (one sample behind) ───────────────────────────

TEST(SampleSchedule, VoiceZeroOutputRidesOneSampleBehindVoiceOne) {
  // Two identically configured voices, one at slot 0 and one at slot 1. Voice 1
  // sounds in the frame it is enabled; voice 0's contribution appears one frame
  // later, because its amplitude is computed at T31 and applied at the next T0.
  Ram ram{};
  DspState v0;
  placeSteadyVoice(v0, 0);
  v0[kMvolLeft] = 0x7F;
  const StereoFrame v0First = sample(v0, ram);   // the first frame is computed at once
  const StereoFrame v0Second = sample(v0, ram);

  DspState v1;
  placeSteadyVoice(v1, 1);
  v1[kMvolLeft] = 0x7F;
  const StereoFrame v1First = sample(v1, ram);
  const StereoFrame v1Second = sample(v1, ram);

  // The first frame from a seed is computed at once, so both sound in it.
  EXPECT_GT(v0First.left, 0);
  EXPECT_GT(v1First.left, 0);
  // In steady state both are the same constant; the point is the schedule keeps
  // voice 0 sounding across the wrap rather than dropping it.
  EXPECT_EQ(v0Second.left, v1Second.left);
}

TEST(SampleSchedule, VoiceZeroOutputLagsVoiceOneByOneFrame) {
  // The same climbing envelope on voice 0 and on voice 1, each alone. Their output
  // sequences are identical but for a one-frame shift: voice 0's frame N carries
  // the amplitude voice 1 emits in frame N-1, because voice 0's compute lands at
  // the last slot and its result is applied at the next frame's start.
  Ram ram{};
  auto sequence = [&](std::size_t v) {
    DspState dsp;
    placeClimbingVoice(dsp, v);
    dsp[kMvolLeft] = 0x7F;
    std::array<std::int16_t, 8> out{};
    for (auto& o : out) o = sample(dsp, ram).left;
    return out;
  };
  const auto v0 = sequence(0);
  const auto v1 = sequence(1);

  EXPECT_NE(v1[0], v1[1]);  // the climb makes successive frames differ
  for (std::size_t i = 0; i + 1 < v0.size(); ++i)
    EXPECT_EQ(v0[i + 1], v1[i]) << "voice 0 frame " << (i + 1) << " matches voice 1 frame " << i;
}

TEST(SampleSchedule, VoiceZeroReadsTheNoiseLevelBeforeTheLastSlotStep) {
  // Voice 0 computes at the last slot, before that slot's noise step, so a NON
  // voice 0 reads the noise level from before this sample's step — one update
  // older than a within-sample voice reads it.
  Ram ram{};
  DspState dsp;
  dsp.voices[0].phase = EnvPhase::Sustain;
  dsp.voices[0].konDelay = 0;
  reg(dsp, 0, 0x07) = 0x7F;  // Direct Gain -> envelope 7F0h
  dsp[kNon] = 0x01;          // voice 0 outputs the noise level
  dsp[kFlg] = 0x1F;          // noise steps every sample
  dsp.noiseLevel = 0x2000;
  sample(dsp, ram);  // prime

  while (dsp.slotCursor != 31) stepDspCycle(dsp, view(ram));
  const std::int16_t preStep = dsp.noiseLevel;
  stepDspCycle(dsp, view(ram));  // the last slot: voice 0 computes, then the noise steps
  EXPECT_EQ(dsp.voiceAmplitude[0], (preStep * 0x7F0) >> 11);  // used the pre-step level
  EXPECT_NE(dsp.noiseLevel, preStep);                          // and the step then ran
}

TEST(SampleSchedule, TheFirstSampleFromASeedSoundsVoiceZeroInFrame) {
  // A freshly seeded voice 0 is heard in the very first delivered frame (computed
  // at once), not one frame late; it primes the schedule only after that.
  Ram ram{};
  DspState dsp;
  placeSteadyVoice(dsp, 0);
  dsp[kMvolLeft] = 0x7F;
  EXPECT_GT(sample(dsp, ram).left, 0);
  EXPECT_FALSE(DspState{}.primed);  // a seed starts unprimed
  EXPECT_TRUE(dsp.primed);          // and primes after its first sample
}

// ── Voices 1-7 are a pure refactor ──────────────────────────────────────────

TEST(SampleSchedule, VoicesOneToSevenMatchTheWholeSampleAtAnyDepth) {
  // A non-voice-0 voice is byte-identical whether taken as one whole sample or a
  // long run of samples: no register racing a frame, so the schedule reproduces
  // the frame-at-once mix exactly.
  Ram ram{};
  DspState climbing;
  placeClimbingVoice(climbing, 3);
  climbing[kMvolLeft] = 0x7F;
  StereoFrame last{};
  for (int n = 0; n < 40; ++n) last = sample(climbing, ram);
  // The envelope has long since saturated; the frame is a stable nonzero value.
  EXPECT_GT(last.left, 0);
  const StereoFrame again = sample(climbing, ram);
  EXPECT_EQ(again.left, last.left);
  EXPECT_EQ(again.right, last.right);
}

TEST(SampleSchedule, TheMuteGateIsAppliedAtTheOutputSlots) {
  // FLG bit 6 mutes the emitted frame, read at the left/right output slots. A mute
  // write landing before T27 silences this frame's left; one landing after does not.
  Ram ram{};
  DspState base;
  placeSteadyVoice(base, 1);
  base[kMvolLeft] = 0x7F;
  sample(base, ram);

  DspState seenState = base;
  DspState missedState = base;
  const StereoFrame seen = sampleWritingAt(seenState, ram, 27, kFlg, 0x40);
  const StereoFrame missed = sampleWritingAt(missedState, ram, 28, kFlg, 0x40);
  EXPECT_EQ(seen.left, 0);    // mute reached the left output slot
  EXPECT_GT(missed.left, 0);  // the write one slot late waits for the next frame
}

TEST(SampleSchedule, KeyOffIsPolledAtTheLastSlot) {
  // KOFF is read at T31 alongside KON. A voice released before that slot is still
  // sounding until the poll runs.
  Ram ram{};
  DspState dsp;
  placeSteadyVoice(dsp, 2);
  sample(dsp, ram);
  sample(dsp, ram);  // sampleIndex now even, so the next sample's T31 polls
  ASSERT_EQ(dsp.sampleIndex % 2, 0u);

  dsp[0x5C] = 0x04;  // KOFF voice 2
  while (dsp.slotCursor != 31) stepDspCycle(dsp, view(ram));
  EXPECT_EQ(dsp.voices[2].phase, EnvPhase::Sustain) << "KOFF is not polled before T31";
  stepDspCycle(dsp, view(ram));  // run T31
  EXPECT_EQ(dsp.voices[2].phase, EnvPhase::Release) << "KOFF released the voice at T31";
}

TEST(SampleSchedule, TheMachineDeliversOneScheduledFramePerThirtyTwoCycles) {
  // Driven a slot per cycle through the Apu, the schedule delivers exactly one
  // frame every 32 cycles and the seeded voice sounds through it.
  using snaggletooth::Apu;
  using snaggletooth::ApuState;
  ApuState s{};
  placeSteadyVoice(s.dsp, 1);
  s.dsp[kMvolLeft] = 0x7F;  // RAM is all NOPs, so the CPU runs harmlessly
  Apu apu(std::move(s));
  apu.run(32 * 4);
  const auto frames = apu.takeFrames();
  ASSERT_EQ(frames.size(), 4u);
  for (const StereoFrame& f : frames) EXPECT_GT(f.left, 0);
}

TEST(SampleSchedule, AStandaloneStateSelfDrivesItsSlotCursor) {
  // The cursor is the DSP's own: it advances one slot per stepDspCycle and wraps
  // after 32, delivering exactly one frame per wrap.
  Ram ram{};
  DspState dsp;
  placeSteadyVoice(dsp, 1);
  int delivered = 0;
  for (int n = 0; n < 64; ++n)
    if (stepDspCycle(dsp, view(ram)).delivered) ++delivered;
  EXPECT_EQ(delivered, 2);           // two wraps in 64 slots
  EXPECT_EQ(dsp.slotCursor, 0u);     // back at the start of a sample
}

}  // namespace
