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

#include <algorithm>
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
  // A sample is scaled by the level already standing, so a steady voice carries
  // its Direct-Gain level from the placement rather than reaching it on step one.
  dsp.voices[v].envelope = 0x7F0;
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
  // ENVX carries one extra pipeline stage (the S9 slot writes the value
  // computed a sample earlier), so the stage is warmed with one slot-scheduled
  // sample before the sentinel round; with a steady envelope the staged value
  // is the same 7Fh and only the visibility slot is under test here.
  Ram ram{};
  DspState dsp;
  placeSteadyVoice(dsp, 1);  // envelope 7F0h -> VxENVX 7Fh
  sample(dsp, ram);
  sample(dsp, ram);  // first slot-scheduled sample stages the value...
  sample(dsp, ram);  // ...and the second publishes it
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

TEST(SampleSchedule, VoiceEnvxReadsTheValueComputedOneSampleEarlier) {
  // The ENVX read-back pipeline: each voice's S9 slot writes the value computed
  // one sample earlier and stages the fresh one, so a CPU read lags the
  // envelope compute by a full sample beyond the visibility slot. Measured
  // against spc_dsp6 `KON/envx during kon`, whose sync vernier anchors the
  // CPU's read phase to voice 0's ENVX publish and whose expected table reads
  // one step behind a same-sample publish for every voice.
  Ram ram{};
  DspState dsp;
  placeClimbingVoice(dsp, 1);
  reg(dsp, 1, 0x05) = 0x00;  // ADSR off: custom gain drives the level instead
  reg(dsp, 1, 0x07) = 0xDF;  // Linear Increase at rate 31: +32 every sample
  sample(dsp, ram);          // prime (atomic; writes ENVX live)
  sample(dsp, ram);          // first slot-scheduled sample warms the stage

  // From here each sample's compute raises the envelope by 32 (ENVX by 2); the
  // register after a sample's S9 slot carries the PREVIOUS sample's value.
  const std::uint16_t levelBefore = dsp.voices[1].envelope;
  sample(dsp, ram);
  EXPECT_EQ(envx(dsp, 1), static_cast<std::uint8_t>(levelBefore >> 4))
      << "the register carries the value the compute produced one sample ago";
  EXPECT_EQ(dsp.voices[1].envelope, levelBefore + 32);
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

TEST(SampleSchedule, VoiceZeroEnvelopeReadsTheCounterTheLastSlotJustAdvanced) {
  // The counter advances at T31 before voice 0's compute in the same slot, so
  // voice 0's envelope-rate check reads the advanced value — the value voices
  // 1-7 read at their compute slots in the following frame. GAIN $D8 (linear
  // increase, rate 24) fires when (counter + 536) % 10 == 0, i.e. at counter
  // ≡ 4 (mod 10): a sample whose advance lands ON a firing value steps the
  // envelope, and a sample entered AT a firing value does not — the advance
  // has moved the counter off it before the check runs. Measured against
  // spc_dsp6 `Misc/counter rate synchronizations`, which locks the counter
  // phase through one voice and measures another's time-to-first-step.
  Ram ram{};
  DspState dsp;
  placeSteadyVoice(dsp, 1);
  placeSteadyVoice(dsp, 0);
  dsp.voices[0].envelope = 0x100;
  reg(dsp, 0, 0x07) = 0xD8;  // GAIN: linear increase, rate 24
  sample(dsp, ram);  // prime

  dsp.globalCounter = 15;  // T31 advances to 14 ≡ 4 (mod 10): the check fires
  sample(dsp, ram);
  EXPECT_EQ(dsp.voices[0].envelope, 0x120) << "the advanced value fires the check";

  dsp.globalCounter = 14;  // ≡ 4 pre-advance; T31 moves it to 13: no fire
  const int held = dsp.voices[0].envelope;
  sample(dsp, ram);
  EXPECT_EQ(dsp.voices[0].envelope, held) << "the pre-advance value is never read";
}

TEST(SampleSchedule, TheNoiseStepReadsTheCounterTheLastSlotJustAdvanced) {
  // The noise step at T31 follows the counter's advance in the same slot, so
  // its rate check reads the advanced value, exactly as voice 0's envelope
  // check does. Noise rate 24 in FLG fires at counter ≡ 4 (mod 10).
  Ram ram{};
  DspState dsp;
  placeSteadyVoice(dsp, 1);
  dsp[kFlg] = 0x18;  // noise rate 24
  sample(dsp, ram);  // prime

  dsp.globalCounter = 15;  // T31 advances to 14 ≡ 4 (mod 10): the step runs
  const std::int16_t before = dsp.noiseLevel;
  sample(dsp, ram);
  EXPECT_NE(dsp.noiseLevel, before) << "the advanced value fires the step";

  dsp.globalCounter = 14;  // ≡ 4 pre-advance; the advance moves off it: no step
  const std::int16_t held = dsp.noiseLevel;
  sample(dsp, ram);
  EXPECT_EQ(dsp.noiseLevel, held) << "the pre-advance value is never read";
}

TEST(SampleSchedule, KeyOnIsPolledAtTheLastSlotOnEvenSamples) {
  // A KON bit is read at T31, so the voice it keys does not begin its startup
  // until that slot; and the poll runs only on even samples.
  Ram ram{};
  DspState dsp;
  placeSteadyVoice(dsp, 1);
  sample(dsp, ram);            // sample 0 done; sampleIndex now 1 (odd)
  ASSERT_EQ(dsp.sampleIndex % 2, 1u);

  dsp.internalKon = 0x04;      // a KON write arms voice 2
  sample(dsp, ram);           // an odd sample: the poll does not run
  EXPECT_EQ(dsp.voices[2].konDelay, 0) << "no poll on the odd sample";
  sample(dsp, ram);           // the next (even) sample polls at its T31
  EXPECT_EQ(dsp.voices[2].konDelay, 5) << "keyed on at the even sample's last slot";
}

TEST(SampleSchedule, TheKeyingLoadPrecedesVoiceZerosComputeAtTheLastSlot) {
  // The KON/KOFF load and voice 0's compute share T31, and the load runs FIRST:
  // a keyed voice 0 takes that very slot as its first silent startup call, so
  // its countdown already reads one lower than a voice computing later in the
  // next sample. This in-slot order is what makes the eight voices' key-on
  // startup read-uniform through VxENVX (spc_dsp6 `KON/envx during kon`).
  Ram ram{};
  DspState dsp;
  placeSteadyVoice(dsp, 1);
  sample(dsp, ram);            // sample 0 done; sampleIndex now 1 (odd)
  ASSERT_EQ(dsp.sampleIndex % 2, 1u);
  sample(dsp, ram);            // run the odd sample so the next one polls

  dsp.internalKon = 0x05;      // one KON write arms voices 0 and 2
  sample(dsp, ram);            // the even sample polls at its T31
  EXPECT_EQ(dsp.voices[0].konDelay, 4)
      << "voice 0's compute followed the load in the shared slot";
  EXPECT_EQ(dsp.voices[2].konDelay, 5)
      << "voice 2's first startup call is still ahead of it";
}

TEST(SampleSchedule, AKeyOnDuringTheStartupCountdownIsAbsorbed) {
  // A key-on consumed by the poll while the voice is still inside its startup
  // countdown is absorbed: the countdown is not reset and the stream is not
  // re-primed, so two polls each consuming a write that names the same voice
  // key it once. A voice past its startup restarts in full — the documented
  // click/pop case. Measured against spc_dsp6's `KON/kon clears independent`,
  // whose two KON writes straddle one poll and expect the first voice to lead
  // the second by exactly the poll period.
  Ram ram{};
  DspState dsp;
  placeSteadyVoice(dsp, 1);
  sample(dsp, ram);            // sample 0 done; sampleIndex now 1 (odd)
  ASSERT_EQ(dsp.sampleIndex % 2, 1u);
  sample(dsp, ram);            // run the odd sample so the next one polls

  dsp.internalKon = 0x04;      // a KON write arms voice 2
  sample(dsp, ram);            // the even sample polls: voice 2 keys on
  ASSERT_EQ(dsp.voices[2].konDelay, 5);

  dsp.internalKon = 0x04;      // a second write names voice 2 again
  sample(dsp, ram);            // odd sample: the countdown runs, no poll
  EXPECT_EQ(dsp.voices[2].konDelay, 4);
  sample(dsp, ram);            // even sample: the poll consumes the arm...
  EXPECT_EQ(dsp.voices[2].konDelay, 3)
      << "...and absorbs it — the countdown is not reset";
  EXPECT_EQ(dsp.internalKon, 0) << "the arm is consumed either way";

  // Voice 1 has been sounding all along with its countdown at 0: the same
  // write restarts it in full.
  dsp.internalKon = 0x02;
  sample(dsp, ram);            // odd sample: no poll
  sample(dsp, ram);            // even sample: the poll re-keys it
  EXPECT_EQ(dsp.voices[1].konDelay, 5)
      << "a re-key past the startup begins a fresh countdown";
}

TEST(SampleSchedule, AVoiceIsScaledByTheLevelStandingBeforeItsUpdate) {
  // A voice's sample is scaled by the level its previous update left behind, and
  // the update runs after — so a moving envelope reaches the output one sample
  // after the step producing it. Anomie's V3c applies the volume envelope before
  // updating it, and his per-sample key-on account states the consequence at #5:
  // envelope updating begins there, yet "the sample output is still '0000',
  // because of the order in which voice operations are performed".
  Ram ram{};
  DspState dsp;
  placeClimbingVoice(dsp, 1);  // ADSR Attack at rate 15: +1024 a sample from 0
  dsp[0x0C] = 0x7F;            // MVOLL = +127
  dsp[0x1C] = 0x7F;            // MVOLR = +127

  const StereoFrame first = sample(dsp, ram);
  EXPECT_EQ(first.left, 0) << "scaled by the zero it started from, not by its own step";
  ASSERT_EQ(dsp.voices[1].envelope, 1024) << "while the step itself ran";

  const StereoFrame second = sample(dsp, ram);
  EXPECT_EQ(dsp.voiceAmplitude[1], (1305 * 1024) >> 11)
      << "the next sample carries the level the first one computed";
  EXPECT_GT(second.left, 0);
}

TEST(SampleSchedule, TheEchoWriteLandsAtItsOwnSlots) {
  // The echo unit computes its buffer write when it runs at T24, but the bytes
  // land at the write slots — the left word at T30, the right word at T31
  // (fullsnes chart rows WrEchoLeft/WrEchoRight; anomie's cycles 29-30) — so a
  // read of the entry between T24 and T30 still sees the previous sample.
  Ram ram{};
  DspState dsp;
  placeClimbingVoice(dsp, 1);  // a changing amplitude, so each frame's write differs
  dsp[0x4D] = 0x02;            // EON: voice 1 feeds the echo buffer
  // ESA and EDL default to 0: the single echo entry sits at $0000-$0003.
  const std::span<std::uint8_t, 65536> wram{ram};

  for (int n = 0; n < 32; ++n) stepDspCycle(dsp, wram);  // the first (atomic) frame

  const auto word = [&](int lo) { return ram[lo] | (ram[lo + 1] << 8); };
  const int leftBefore = word(0);
  const int rightBefore = word(2);
  while (dsp.slotCursor != 30) {
    stepDspCycle(dsp, wram);
    EXPECT_EQ(word(0), leftBefore)
        << "left word held before T30 (cursor " << int(dsp.slotCursor) << ")";
    EXPECT_EQ(word(2), rightBefore)
        << "right word held before T30 (cursor " << int(dsp.slotCursor) << ")";
  }
  stepDspCycle(dsp, wram);  // T30 runs
  EXPECT_NE(word(0), leftBefore) << "the left word landed at T30";
  EXPECT_EQ(word(2), rightBefore) << "the right word still held at T30";
  stepDspCycle(dsp, wram);  // T31 runs
  EXPECT_NE(word(2), rightBefore) << "the right word landed at T31";
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

TEST(SampleSchedule, TheStartupStreamAdvancesFromItsSecondCall) {
  // A key-on that interrupts a sounding voice advances its fresh stream at the
  // pitch through the startup countdown — the voice is silent, but its decode
  // cursor walks — except on the first startup call, which performs the
  // start-address read and decodes nothing. Measured against spc_dsp6's
  // `KON/kon decoding when another kon`, which freezes the pitch mid-startup of
  // a re-keyed sounding voice and reads where the cursor stood. (A key-on of a
  // SILENT voice holds its stream instead — `Misc/brr addr wrap-around` — so
  // the voice here sounds before the key-on.)
  Ram ram{};
  DspState dsp;
  dsp.voices[2].phase = EnvPhase::Sustain;
  dsp.voices[2].envelope = 0x100;
  reg(dsp, 2, 0x07) = 0x7F;    // Direct Gain holds the level above zero
  reg(dsp, 2, 0x03) = 0x20;    // VxPITCHH: two stream samples per output sample
  sample(dsp, ram);            // sample 0 (atomic); sampleIndex now 1
  sample(dsp, ram);            // odd sample done, so the next one polls
  ASSERT_EQ(dsp.sampleIndex % 2, 0u);

  dsp.internalKon = 0x04;      // a KON write arms voice 2
  sample(dsp, ram);            // the poll at this sample's T31 arms the restart
  ASSERT_EQ(dsp.voices[2].konDelay, 5);

  sample(dsp, ram);            // the voice's compute applies it: the preload,
                               // and the startup's first call advances nothing
  EXPECT_EQ(dsp.voices[2].konDelay, 4);
  EXPECT_EQ(dsp.voices[2].brrSampleIndex, 4) << "the key-on preload decodes four samples";
  EXPECT_EQ(dsp.voices[2].pitchCounter, 0x0000);

  for (int n = 0; n < 4; ++n) sample(dsp, ram);  // the countdown's other calls
  EXPECT_EQ(dsp.voices[2].konDelay, 0);
  EXPECT_EQ(dsp.voices[2].brrSampleIndex, 12)
      << "four advancing startup calls walk the cursor two samples each";
  EXPECT_EQ(dsp.voices[2].pitchCounter, 0x8000);
}

TEST(SampleSchedule, AKeyOnOfASilentVoiceHoldsItsStreamThroughTheFirstLiveSample) {
  // A key-on of a silent voice — envelope at 0 as the restart applies — holds
  // its fresh stream at the primed start through the whole silent span AND the
  // first live compute: that sample interpolates the stream's first four
  // samples at fraction 0, and advancing begins the sample after. Measured
  // against spc_dsp6's `Misc/brr addr wrap-around`, whose first half-envelope
  // sample interpolates the wrapped block's first four samples and whose rows
  // then step one stream sample per output sample.
  Ram ram{};
  DspState dsp;
  reg(dsp, 2, 0x03) = 0x10;    // pitch $1000: one stream sample a call
  sample(dsp, ram);
  sample(dsp, ram);
  ASSERT_EQ(dsp.sampleIndex % 2, 0u);

  dsp.internalKon = 0x04;      // a KON write arms voice 2, long silent
  sample(dsp, ram);            // the poll arms the restart
  sample(dsp, ram);            // the voice's compute applies it: the preload
  ASSERT_EQ(dsp.voices[2].konDelay, 4);
  EXPECT_EQ(dsp.voices[2].brrSampleIndex, 4) << "the key-on preload decodes four samples";

  for (int n = 0; n < 4; ++n) sample(dsp, ram);  // the countdown's other calls
  ASSERT_EQ(dsp.voices[2].konDelay, 0);
  EXPECT_EQ(dsp.voices[2].brrSampleIndex, 4) << "the silent span leaves the stream standing";
  EXPECT_EQ(dsp.voices[2].pitchCounter, 0x0000);

  // The countdown's end still emits one silent sample (the envelope's first
  // move reaches the output a sample late), and the first sounding sample then
  // reads the primed window: both are inside the hold.
  sample(dsp, ram);
  sample(dsp, ram);
  EXPECT_EQ(dsp.voices[2].brrSampleIndex, 4)
      << "the first sounding sample interpolates the primed window";
  EXPECT_EQ(dsp.voices[2].pitchCounter, 0x0000);

  sample(dsp, ram);            // advancing begins the sample after
  EXPECT_EQ(dsp.voices[2].brrSampleIndex, 5);
  EXPECT_EQ(dsp.voices[2].pitchCounter, 0x1000);
}

TEST(SampleSchedule, ALivePitchWriteLandsPerSampleWhateverTheParity) {
  // A live voice's stream advance never reads VxPITCHL/H at its own compute:
  // the registers are captured for all eight voices at the first slot of EVERY
  // sample, and the capture reaches a voice one sample deep — voice 0's T31
  // compute is the only one late enough to see its own sample's capture, while
  // voices 1-7 advance by the previous sample's. The cadence carries no
  // parity: a write frozen before a sample behaves identically whichever
  // sample it lands on. Measured against spc_dsp6's `KON/kon then change
  // pitch`, whose one-sample pitch pulse is seen at every alignment — an
  // every-other-sample capture leaves half of them invisible.
  for (int phase = 0; phase < 2; ++phase) {
    Ram ram{};
    DspState dsp;
    for (std::size_t v : {std::size_t{0}, std::size_t{2}}) {
      dsp.voices[v].konDelay = 0;
      dsp.voices[v].phase = EnvPhase::Sustain;
      dsp.voices[v].envelope = 0x100;
      reg(dsp, v, 0x03) = 0x10;  // one stream sample per output sample
    }
    sample(dsp, ram);            // sample 0 (atomic) seeds the capture
    for (int n = 0; n < phase; ++n) sample(dsp, ram);  // stagger the parity
    sample(dsp, ram);
    const std::uint16_t v0 = dsp.voices[0].pitchCounter;
    const std::uint16_t v2 = dsp.voices[2].pitchCounter;

    // Freeze both voices' pitch before the next sample begins. Its first-slot
    // capture reads the zero: voice 0's T31 compute consumes it this sample,
    // voice 2's early compute still advances by the previous capture and
    // consumes it the sample after.
    dsp[0 * 0x10 + 0x03] = 0x00;
    dsp[2 * 0x10 + 0x03] = 0x00;
    sample(dsp, ram);
    EXPECT_EQ(dsp.voices[0].pitchCounter, v0)
        << "voice 0 reads its own sample's capture (phase " << phase << ")";
    EXPECT_EQ(dsp.voices[2].pitchCounter, static_cast<std::uint16_t>(v2 + 0x1000))
        << "voices 1-7 still advance by the previous capture (phase " << phase << ")";

    sample(dsp, ram);
    sample(dsp, ram);
    EXPECT_EQ(dsp.voices[0].pitchCounter, v0);
    EXPECT_EQ(dsp.voices[2].pitchCounter, static_cast<std::uint16_t>(v2 + 0x1000));
  }
}

TEST(SampleSchedule, AKeyOnCapturesThePitchAtTheNextPollParitySample) {
  // A consumed key-on schedules the voice's pitch capture for the next
  // poll-parity sample's first slot, and that capture propagates like any
  // other: voice 0 consumes it at that sample's T31, voices 1-7 one sample
  // later. A pitch write landing between the poll and that instant is
  // therefore seen — `KON/kon decoding when another kon`'s freezes ride nine
  // cycles behind their KON write and land — and every voice stops with the
  // same one stale-pitch advance taken before the capture.
  Ram ram{};
  DspState dsp;
  for (std::size_t v : {std::size_t{0}, std::size_t{2}}) {
    // Sounding voices, so the re-key's startup walks (a key-on of a silent
    // voice holds its stream — `Misc/brr addr wrap-around`).
    dsp.voices[v].phase = EnvPhase::Sustain;
    dsp.voices[v].envelope = 0x100;
    reg(dsp, v, 0x07) = 0x7F;  // Direct Gain holds the level above zero
    reg(dsp, v, 0x03) = 0x20;  // two stream samples per output sample
  }
  sample(dsp, ram);            // sample 0 (atomic); the idle captures track
  sample(dsp, ram);
  ASSERT_EQ(dsp.sampleIndex % 2, 0u);

  dsp.internalKon = 0x05;      // a KON write arms voices 0 and 2
  sample(dsp, ram);            // the poll keys both; voice 0's compute 1 runs
  ASSERT_EQ(dsp.voices[0].konDelay, 4);
  ASSERT_EQ(dsp.voices[2].konDelay, 5);

  // The freeze lands after the poll, before the parity sample: the scheduled
  // capture reads it, so each voice takes exactly one advance at the stale
  // pitch (voice 0 on this sample, voice 2 on the parity sample) and stops.
  dsp[0 * 0x10 + 0x03] = 0x00;
  dsp[2 * 0x10 + 0x03] = 0x00;
  for (int n = 0; n < 8; ++n) sample(dsp, ram);
  EXPECT_EQ(dsp.voices[0].pitchCounter, 0x2000)
      << "one stale advance before the scheduled capture lands";
  EXPECT_EQ(dsp.voices[2].pitchCounter, dsp.voices[0].pitchCounter)
      << "the scheduled capture reaches the eight voices uniformly";
}

TEST(SampleSchedule, ABarePitchWriteDuringTheKeyOnHoldNeverLands) {
  // From a consumed key-on, the per-sample pitch capture holds for seven
  // samples — anchored to the poll, uniform for the eight voices — so a pitch
  // write that rides no KON is invisible for the hold's whole width: the
  // countdown keeps advancing at the pitch the key-on's own capture took, and
  // the write is only picked up by the first live capture after the hold.
  // Measured against spc_dsp6's `KON/kon then change pitch`: its pulses land
  // nothing through the fourth reading and exactly one sample from the fifth.
  Ram ram{};
  DspState dsp;
  for (std::size_t v : {std::size_t{0}, std::size_t{2}}) {
    // Sounding voices, so the re-key's startup walks (a key-on of a silent
    // voice holds its stream — `Misc/brr addr wrap-around`).
    dsp.voices[v].phase = EnvPhase::Sustain;
    dsp.voices[v].envelope = 0x100;
    reg(dsp, v, 0x07) = 0x7F;  // Direct Gain holds the level above zero
    reg(dsp, v, 0x03) = 0x20;  // two stream samples per output sample
  }
  sample(dsp, ram);
  sample(dsp, ram);
  ASSERT_EQ(dsp.sampleIndex % 2, 0u);

  dsp.internalKon = 0x05;
  sample(dsp, ram);            // the poll keys voices 0 and 2
  sample(dsp, ram);
  sample(dsp, ram);            // the scheduled capture has taken $2000

  // A bare write during the hold: the countdown walks on at the captured
  // pitch. Each voice takes six advancing calls — the first startup call
  // decodes nothing — before the first post-hold capture stops it.
  dsp[0 * 0x10 + 0x03] = 0x00;
  dsp[2 * 0x10 + 0x03] = 0x00;
  for (int n = 0; n < 8; ++n) sample(dsp, ram);
  EXPECT_EQ(dsp.voices[0].pitchCounter, 0xC000)
      << "six advances at the held pitch; the write lands only past the hold";
  EXPECT_EQ(dsp.voices[2].pitchCounter, dsp.voices[0].pitchCounter)
      << "the hold is poll-anchored, so the eight voices stop together";
}

TEST(SampleSchedule, AOneSamplePitchPulseAdvancesEveryVoiceBySameOneSample) {
  // A pitch value standing for exactly one sample is consumed for exactly one
  // advance by every live voice, whatever its compute slot: the pulse covers
  // exactly one first-slot capture, and the one-sample propagation hands that
  // capture to each voice once. Measured against spc_dsp6's `KON/kon then
  // change pitch`, whose expected readings are $10 — one sample at the pulsed
  // pitch — on all eight rows alike.
  Ram ram{};
  DspState dsp;
  for (std::size_t v : {std::size_t{0}, std::size_t{2}}) {
    dsp.voices[v].konDelay = 0;
    dsp.voices[v].phase = EnvPhase::Sustain;
    dsp.voices[v].envelope = 0x100;
  }
  sample(dsp, ram);            // pitch $0000: nothing advances
  sample(dsp, ram);

  dsp[0 * 0x10 + 0x03] = 0x20;  // the pulse: up before one sample...
  dsp[2 * 0x10 + 0x03] = 0x20;
  sample(dsp, ram);
  dsp[0 * 0x10 + 0x03] = 0x00;  // ...and back before the next
  dsp[2 * 0x10 + 0x03] = 0x00;
  for (int n = 0; n < 4; ++n) sample(dsp, ram);

  EXPECT_EQ(dsp.voices[0].pitchCounter, 0x2000) << "exactly one advance at the pulse";
  EXPECT_EQ(dsp.voices[2].pitchCounter, 0x2000)
      << "the same one advance on an early-slot voice";
}

TEST(SampleSchedule, AKeyOnMidCountdownPastTheFirstPollRewindsTheHold) {
  // A key-on consumed while the voice is mid-countdown, at any poll past the
  // one immediately following its keying, rewinds the silent hold: the
  // countdown re-arms in full and the envelope drops to 0, while the stream
  // stands where it was — neither re-primed nor stalled. Measured against
  // spc_dsp6's `KON/kon then another kon` (the level re-emerges as late as a
  // restart would place it) with `KON/kon decoding when another kon` holding
  // the cursor's walk (the same re-key leaves the frozen positions standing).
  Ram ram{};
  DspState dsp;
  // A sounding voice, so the key-on's startup walks (a key-on of a silent
  // voice holds its stream — `Misc/brr addr wrap-around`).
  dsp.voices[2].phase = EnvPhase::Sustain;
  dsp.voices[2].envelope = 0x100;
  reg(dsp, 2, 0x07) = 0x7F;    // Direct Gain, so the envelope moves once live
  reg(dsp, 2, 0x03) = 0x10;    // pitch $1000: one stream sample a call
  sample(dsp, ram);
  sample(dsp, ram);
  ASSERT_EQ(dsp.sampleIndex % 2, 0u);

  dsp.internalKon = 0x04;
  sample(dsp, ram);            // the poll keys voice 2 on
  ASSERT_EQ(dsp.voices[2].konDelay, 5);

  for (int n = 0; n < 3; ++n) sample(dsp, ram);  // computes 1-3 run
  ASSERT_EQ(dsp.voices[2].konDelay, 2);

  // The poll two periods after the keying consumes a fresh arm with the
  // countdown still running: the hold rewinds, the stream stays put — three
  // advancing calls' worth of pitch, not a re-primed zero.
  dsp.internalKon = 0x04;
  sample(dsp, ram);
  EXPECT_EQ(dsp.voices[2].konDelay, 5) << "the silent hold re-arms in full";
  EXPECT_EQ(dsp.voices[2].envelope, 0u);
  EXPECT_EQ(dsp.voices[2].pitchCounter, 0x3000) << "the cursor is not re-primed";
  EXPECT_EQ(dsp.voices[2].brrSampleIndex, 7);
}

TEST(SampleSchedule, AKeyOnLateInTheSilentSpanRewindsTheHoldAndPastItRestarts) {
  // The one-poll absorption does not cover the voice's whole silent span. A
  // key-on consumed at the poll right after the first live compute — the last
  // silent sample — rewinds the hold: countdown re-armed, envelope dropped,
  // stream standing and still walking at the pitch. One poll later the span is
  // over and the same write restarts the voice in full, stream re-primed.
  // Measured against spc_dsp6's `KON/kon then another kon` (the re-key's level
  // re-emerges restart-late), `KON/kon decoding when another kon` (the cursor
  // keeps its walk through the re-key), and `KON/envx during kon` (a re-key
  // past the span restarts).
  Ram ram{};
  DspState dsp;
  // A sounding voice, so the key-on's startup walks (a key-on of a silent
  // voice holds its stream — `Misc/brr addr wrap-around`).
  dsp.voices[2].phase = EnvPhase::Sustain;
  dsp.voices[2].envelope = 0x100;
  reg(dsp, 2, 0x07) = 0x7F;    // Direct Gain, so the envelope moves once live
  reg(dsp, 2, 0x03) = 0x10;    // pitch $1000: one stream sample a call
  sample(dsp, ram);
  sample(dsp, ram);
  ASSERT_EQ(dsp.sampleIndex % 2, 0u);

  dsp.internalKon = 0x04;
  sample(dsp, ram);            // the poll keys voice 2 on
  ASSERT_EQ(dsp.voices[2].konDelay, 5);

  for (int n = 0; n < 5; ++n) sample(dsp, ram);  // the countdown runs out
  ASSERT_EQ(dsp.voices[2].konDelay, 0);

  // The next sample runs the first live compute (still silent, and it lifts
  // the level) and then polls: the consumed key-on rewinds the hold. Computes
  // 2-6 advanced and decoded five stream samples.
  dsp.internalKon = 0x04;
  sample(dsp, ram);
  EXPECT_EQ(dsp.voices[2].konDelay, 5) << "the silent hold re-arms in full";
  EXPECT_EQ(dsp.voices[2].envelope, 0u) << "the level the live compute lifted drops";
  EXPECT_EQ(dsp.voices[2].pitchCounter, 0x5000) << "the cursor is not re-primed";
  EXPECT_EQ(dsp.voices[2].brrSampleIndex, 9);

  // The rewound hold is not a fresh key-on: its next call advances the stream
  // as normal instead of repeating the decode-nothing start-address call.
  sample(dsp, ram);
  EXPECT_EQ(dsp.voices[2].konDelay, 4);
  EXPECT_EQ(dsp.voices[2].pitchCounter, 0x6000) << "the re-held span keeps walking";

  // The next poll sits past the silent span: the same write restarts the
  // voice in full — countdown, envelope, and a re-primed stream. The poll
  // arms it; the old stream stands through the arming sample (the voice's
  // compute emits one more sample from it — the final pre-key-on sample —
  // before the restart applies).
  dsp.internalKon = 0x04;
  sample(dsp, ram);
  EXPECT_EQ(dsp.voices[2].konDelay, 5) << "a re-key past the silent span restarts";
  EXPECT_EQ(dsp.voices[2].pitchCounter, 0x7000)
      << "the old stream still stands at the arming poll's sample";
  sample(dsp, ram);
  EXPECT_EQ(dsp.voices[2].konDelay, 4);
  EXPECT_EQ(dsp.voices[2].pitchCounter, 0x0000) << "the stream is re-primed";
  EXPECT_EQ(dsp.voices[2].brrSampleIndex, 4);
}

TEST(SampleSchedule, AKeyOnConsumedUnderASoftResetKeepsItsStartup) {
  // A soft reset standing in the very sample a keying poll consumes a voice's
  // key-on does not key that voice off: the fresh consumption wins, exactly as
  // KON applied after KOFF wins at the poll itself, and the startup's whole
  // ENVX schedule proceeds as if the reset were not standing. Measured against
  // spc_dsp6's `KON/kon then flg.80`: a one-sample FLG bit-7 pulse over the
  // consuming poll's sample leaves every voice's ENVX recovery exactly where
  // an unmolested key-on places it, at both poll-relative compute positions
  // (voice 0 computes after the poll in its sample; voices 1-7 take their
  // first startup call the sample after).
  auto trajectory = [](bool pulsed) {
    Ram ram{};
    DspState dsp;
    reg(dsp, 0, 0x07) = 0x7F;  // Direct Gain: ENVX jumps to 7Fh once live
    reg(dsp, 2, 0x07) = 0x7F;
    sample(dsp, ram);
    sample(dsp, ram);
    EXPECT_EQ(dsp.sampleIndex % 2, 0u);
    dsp.internalKon = 0x05;      // a KON write arms voices 0 and 2
    if (pulsed) dsp[kFlg] = 0x80;
    sample(dsp, ram);            // the poll consumes under the standing reset
    if (pulsed) dsp[kFlg] = 0x00;
    std::array<std::uint8_t, 24> out{};
    for (std::size_t n = 0; n < 12; ++n) {
      sample(dsp, ram);
      out[2 * n] = envx(dsp, 0);
      out[2 * n + 1] = envx(dsp, 2);
    }
    return out;
  };
  const auto clean = trajectory(false);
  const auto shielded = trajectory(true);
  EXPECT_EQ(shielded, clean) << "the startup's ENVX schedule is untouched";
  EXPECT_EQ(clean.back(), 0x7F) << "and it is a startup that completes and sounds";
}

TEST(SampleSchedule, ASoftResetPulseOneSampleLaterKeysTheVoiceOff) {
  // The consumption sample is the shield's whole width. The same one-sample
  // reset pulse landing one sample after a voice's key-on is consumed — or
  // anywhere later in or past the countdown — keys it off like any other
  // voice: Release, envelope forced to 0, and with nothing re-arming it the
  // voice never sounds. Measured against spc_dsp6's `KON/kon then flg.80`,
  // whose readings print the no-recovery mark from its second alignment on.
  Ram ram{};
  DspState dsp;
  reg(dsp, 0, 0x07) = 0x7F;  // Direct Gain
  sample(dsp, ram);
  sample(dsp, ram);
  ASSERT_EQ(dsp.sampleIndex % 2, 0u);
  dsp.internalKon = 0x01;
  sample(dsp, ram);            // a clean consumption; voice 0's startup begins
  ASSERT_EQ(dsp.voices[0].konDelay, 4);
  ASSERT_EQ(dsp.voices[0].phase, EnvPhase::Attack);

  dsp[kFlg] = 0x80;
  sample(dsp, ram);            // the pulse covers only the following sample
  dsp[kFlg] = 0x00;
  EXPECT_EQ(dsp.voices[0].phase, EnvPhase::Release) << "one sample past the shield";

  for (int n = 0; n < 12; ++n) sample(dsp, ram);
  EXPECT_EQ(dsp.voices[0].phase, EnvPhase::Release);
  EXPECT_EQ(dsp.voices[0].envelope, 0u);
  EXPECT_EQ(envx(dsp, 0), 0) << "the keyed-off voice never recovers";
}

TEST(SampleSchedule, AKeyOnConsumedOnAResetKilledStartupRestartsInFull) {
  // The in-span absorption tiers protect a STANDING startup. A soft-reset
  // pulse landing inside the span keys the voice off and kills its startup for
  // good, and a key-on consumed after that kill restarts the voice in full —
  // countdown re-armed, envelope from 0, stream re-primed — even though the
  // old span's samples have not run out. Measured against spc_dsp6's `KON/kon
  // then flg.80 then kon`: on the alignments whose second key-on the poll
  // sees, ENVX recovers with a full restart's count, identical inside and past
  // the span.
  Ram ram{};
  DspState dsp;
  // A sounding voice, so the key-on's startup walks (a key-on of a silent
  // voice holds its stream — `Misc/brr addr wrap-around`).
  dsp.voices[2].phase = EnvPhase::Sustain;
  dsp.voices[2].envelope = 0x100;
  reg(dsp, 2, 0x07) = 0x7F;    // Direct Gain: ENVX jumps to 7Fh once live
  reg(dsp, 2, 0x03) = 0x10;    // pitch $1000: one stream sample a call
  sample(dsp, ram);
  sample(dsp, ram);
  ASSERT_EQ(dsp.sampleIndex % 2, 0u);

  dsp.internalKon = 0x04;
  sample(dsp, ram);            // the poll keys voice 2 on
  ASSERT_EQ(dsp.voices[2].konDelay, 5);
  sample(dsp, ram);            // compute 1 runs; the shield's sample is over

  dsp[kFlg] = 0x80;
  sample(dsp, ram);            // the one-sample pulse kills the startup
  dsp[kFlg] = 0x00;
  ASSERT_EQ(dsp.voices[2].phase, EnvPhase::Release);
  ASSERT_GT(dsp.voices[2].konDelay, 0) << "the kill lands mid-countdown";
  sample(dsp, ram);
  ASSERT_NE(dsp.voices[2].pitchCounter, 0u) << "the cursor has moved off the prime";

  // The next poll consumes a key-on with the span's samples still counting.
  // On the dead startup it is neither absorbed nor a rewind: the voice
  // restarts in full, stream re-primed. The poll arms it; the dead startup's
  // state stands through the arming sample and the voice's own compute
  // applies the restart.
  dsp.internalKon = 0x04;
  sample(dsp, ram);
  EXPECT_EQ(dsp.voices[2].konDelay, 5) << "a key-on on a dead startup restarts";
  EXPECT_EQ(dsp.voices[2].phase, EnvPhase::Release)
      << "the killed startup still stands at the arming poll's sample";
  sample(dsp, ram);
  EXPECT_EQ(dsp.voices[2].konDelay, 4);
  EXPECT_EQ(dsp.voices[2].phase, EnvPhase::Attack);
  EXPECT_EQ(dsp.voices[2].pitchCounter, 0x0000) << "the stream is re-primed";
  EXPECT_EQ(dsp.voices[2].brrSampleIndex, 4);

  for (int n = 0; n < 7; ++n) sample(dsp, ram);
  EXPECT_EQ(envx(dsp, 2), 0x7F) << "and it is a restart that completes and sounds";
}

TEST(SampleSchedule, AKeyOnConsumedOnAKeyedOffStartupRestartsInFull) {
  // The dead-startup rule is the voice's keyed-off state, not the reset's
  // doing: a KOFF landing inside the span kills the startup the same way, and
  // a key-on consumed after it restarts the voice in full from inside the old
  // span. One Release condition carries both — spc_dsp6's `KON/kon then koff`
  // and `KON/kon then set sample's end flag` clear with the same arm that
  // clears `KON/kon then flg.80 then kon`.
  Ram ram{};
  DspState dsp;
  reg(dsp, 2, 0x07) = 0x7F;    // Direct Gain
  reg(dsp, 2, 0x03) = 0x10;    // pitch $1000: one stream sample a call
  sample(dsp, ram);
  sample(dsp, ram);
  ASSERT_EQ(dsp.sampleIndex % 2, 0u);

  dsp.internalKon = 0x04;
  sample(dsp, ram);            // the poll keys voice 2 on
  ASSERT_EQ(dsp.voices[2].konDelay, 5);
  sample(dsp, ram);            // compute 1 runs

  dsp[0x5C] = 0x04;            // KOFF: the next poll keys the voice off
  sample(dsp, ram);
  dsp[0x5C] = 0x00;
  ASSERT_EQ(dsp.voices[2].phase, EnvPhase::Release);
  ASSERT_GT(dsp.voices[2].konDelay, 0) << "the kill lands mid-countdown";
  sample(dsp, ram);

  dsp.internalKon = 0x04;
  sample(dsp, ram);
  EXPECT_EQ(dsp.voices[2].konDelay, 5) << "a key-on on a keyed-off startup restarts";
  EXPECT_EQ(dsp.voices[2].phase, EnvPhase::Release)
      << "the killed startup still stands at the arming poll's sample";
  sample(dsp, ram);
  EXPECT_EQ(dsp.voices[2].konDelay, 4);
  EXPECT_EQ(dsp.voices[2].phase, EnvPhase::Attack);
  EXPECT_EQ(dsp.voices[2].pitchCounter, 0x0000) << "the stream is re-primed";

  for (int n = 0; n < 7; ++n) sample(dsp, ram);
  EXPECT_EQ(envx(dsp, 2), 0x7F) << "and it is a restart that completes and sounds";
}

TEST(SampleSchedule, AReKeyOnASoundingVoiceEmitsTheFinalPreKeyOnSample) {
  // A full restart consumed by the poll silences its voice only from the
  // sample after the consuming compute: that compute still prepares one sample
  // from the standing stream and envelope — the final pre-key-on sample — and
  // the restart applies after it. Measured against spc_dsp6's `KON/kon
  // unaffected by pitch`, whose echo window records the re-keyed voice's old
  // data on the consuming sample and the startup's silence only after it,
  // identically at pitch $3F00 and pitch 0.
  Ram ram{};
  DspState dsp;
  placeSteadyVoice(dsp, 1);
  dsp[kMvolLeft] = 0x7F;
  dsp[kMvolRight] = 0x7F;
  sample(dsp, ram);
  sample(dsp, ram);
  ASSERT_EQ(dsp.sampleIndex % 2, 0u);
  const StereoFrame steady = sample(dsp, ram);
  ASSERT_GT(steady.left, 0);
  sample(dsp, ram);            // odd sample, so the next one polls
  ASSERT_EQ(dsp.sampleIndex % 2, 0u);

  dsp.internalKon = 0x02;      // re-key the sounding voice
  const StereoFrame atThePoll = sample(dsp, ram);  // computed before its T31 poll
  const StereoFrame consuming = sample(dsp, ram);  // the consuming compute
  const StereoFrame after = sample(dsp, ram);      // the startup's first silence
  EXPECT_EQ(atThePoll.left, steady.left);
  EXPECT_EQ(consuming.left, steady.left) << "the final pre-key-on sample still sounds";
  EXPECT_EQ(after.left, 0) << "the restart's silence begins the sample after";
}

TEST(SampleSchedule, TheFinalPreKeyOnSampleIsTheOldStreamsOwnNext) {
  // The consuming compute does not replay the voice's standing output: the old
  // stream takes its final decode and advance first (the sample a un-keyed
  // voice would have emitted), and only then does the restart wipe it. Pinned
  // against a control machine: a walking voice re-keyed mid-stream matches the
  // control's frames exactly through the consuming sample and goes silent the
  // sample after, while the control keeps sounding.
  Ram ram{};
  ram[0x0200] = 0x00;  // directory entry 0: start $0300
  ram[0x0201] = 0x03;
  ram[0x0202] = 0x00;  // loop -> $0300
  ram[0x0203] = 0x03;
  ram[0x0300] = 0xC3;  // shift 12, filter 0, end+loop: loops over itself
  for (int n = 0; n < 8; ++n)
    ram[static_cast<std::size_t>(0x0301 + n)] = static_cast<std::uint8_t>(0x10 * (n + 1) + n);

  const auto place = [](DspState& dsp) {
    dsp[0x5D] = 0x02;          // DIR -> $0200
    dsp[kMvolLeft] = 0x7F;
    dsp[kMvolRight] = 0x7F;
    dsp.voices[1].phase = EnvPhase::Sustain;
    dsp.voices[1].konDelay = 0;
    dsp.voices[1].envelope = 0x7F0;
    dsp.voices[1].brrAddress = 0x0300;
    reg(dsp, 1, 0x03) = 0x10;  // pitch $1000: one stream sample per call
    reg(dsp, 1, 0x07) = 0x7F;  // Direct Gain holds the level
    reg(dsp, 1, 0x00) = 0x40;
    reg(dsp, 1, 0x01) = 0x40;
  };
  DspState keyed;
  DspState control;
  place(keyed);
  place(control);
  for (int n = 0; n < 4; ++n) {
    const StereoFrame a = sample(keyed, ram);
    const StereoFrame b = sample(control, ram);
    ASSERT_EQ(a, b);
  }
  ASSERT_EQ(keyed.sampleIndex % 2, 0u);

  keyed.internalKon = 0x02;
  const StereoFrame armA = sample(keyed, ram);
  const StereoFrame armB = sample(control, ram);
  EXPECT_EQ(armA, armB) << "computed before the arming poll";
  const StereoFrame lastA = sample(keyed, ram);
  const StereoFrame lastB = sample(control, ram);
  EXPECT_EQ(lastA, lastB) << "the final pre-key-on sample is the advanced one";
  ASSERT_NE(lastB.left, 0) << "a frame with signal, or the clause pins nothing";
  const StereoFrame afterA = sample(keyed, ram);
  const StereoFrame afterB = sample(control, ram);
  EXPECT_EQ(afterA.left, 0) << "the restart's silence begins here";
  EXPECT_NE(afterB.left, 0) << "while the control keeps sounding";
}

TEST(SampleSchedule, TheFirstSoundingSampleInterpolatesFromIndexZero) {
  // A key-on's stream walk carries no interpolation fraction: the pitch
  // counter's fractional bits are cleared through the silent span and the
  // first sounding compute, so the first audible sample reads the Gaussian
  // kernel at index 0 and the fraction begins accumulating only with the
  // advance after it. Measured against spc_dsp6's `KON/pitch at kon`: a voice
  // keyed at pitch $0010 over a ramp block plays the kernel walked from index
  // 0, 1, 2, … — a retained startup fraction starts the walk six indices deep.
  Ram ram{};
  ram[0x0200] = 0x00;  // directory entry 0: start $0300
  ram[0x0201] = 0x03;
  ram[0x0202] = 0x00;  // loop -> $0300
  ram[0x0203] = 0x03;
  // The ROM's own ramp block: shift 12, filter 0, end+loop over itself.
  const std::array<std::uint8_t, 9> block = {0xC3, 0x10, 0xFE, 0xDC, 0xBA,
                                             0x98, 0x76, 0x54, 0x32};
  std::copy(block.begin(), block.end(), ram.begin() + 0x0300);

  DspState dsp;
  dsp[0x5D] = 0x02;          // DIR -> $0200
  reg(dsp, 2, 0x02) = 0x10;  // VxPITCHL: one interpolation index per sample
  reg(dsp, 2, 0x07) = 0x7F;  // Direct Gain 7F0h
  reg(dsp, 2, 0x00) = 0x40;
  reg(dsp, 2, 0x01) = 0x40;
  sample(dsp, ram);  // sample 0 (atomic); sampleIndex now 1
  sample(dsp, ram);  // odd sample done, so the next one polls
  ASSERT_EQ(dsp.sampleIndex % 2, 0u);

  dsp.internalKon = 0x04;  // a KON write arms voice 2
  sample(dsp, ram);        // the poll at this sample's T31 arms the restart

  int silent = 0;
  while (dsp.voiceAmplitude[2] == 0 && silent < 16) {
    sample(dsp, ram);
    ++silent;
  }
  ASSERT_LT(silent, 16) << "the voice never sounded";
  EXPECT_EQ(silent, 7) << "the restart-apply call plus six silent computes";

  // The stream never crosses a sample position over this span, so the window
  // stands still and every audible value is the kernel alone.
  const snaggletooth::SampleWindow window = dsp.voices[2].window;
  const auto at = [&](std::uint8_t index) {
    return (snaggletooth::gaussInterpolate(window, index) * 0x7F0) >> 11;
  };
  ASSERT_NE(at(0), at(6)) << "index 0 and a retained fraction differ, or "
                             "the clause pins nothing";
  EXPECT_EQ(dsp.voiceAmplitude[2], at(0)) << "the first audible sample reads index 0";
  sample(dsp, ram);
  EXPECT_EQ(dsp.voiceAmplitude[2], at(1)) << "the fraction accumulates from here";
  sample(dsp, ram);
  EXPECT_EQ(dsp.voiceAmplitude[2], at(2));
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
