// The volume envelope: the global counter, the ADSR phase machine, the GAIN
// modes, release, and the KON/KOFF keying with its five-sample startup.
//
// Every expected value is hand-derived from fullsnes's "SNES APU DSP ADSR/Gain
// Envelope" section (the primary contract): the ADSR fields and steps at lines
// 2899-2913, the GAIN modes at 2931-2937, the Bent-Increase reference at
// 2992-2995, ENVX at 2942-2948, and the KON/KOFF registers at 3044-3066. Anomie's
// S-DSP doc is the cross-check and the carrier of the exact counter scheme (the
// Modulus[]/Offset[] tables and the (counter+offset)%period fire rule, line 230)
// and the phase-transition thresholds: the Attack->Decay switch fires on a value
// outside the 11-bit range (an attack reaching 0x800 clips to 0x7FF, and a
// decrease below zero counts too), read from the mode's candidate whether or not
// the counter lets the level take it; Decay->Sustain fires when the candidate's
// upper 3 bits reach the boundary, which is the sustain level under ADSR but
// VxGAIN's own bits 7-5 while a GAIN mode drives the level (fullsnes's "Gain
// Notes", 2977-2984).

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include <gtest/gtest.h>

#include "snaggletooth/apu/dsp.h"

namespace {

using snaggletooth::DspState;
using snaggletooth::EnvPhase;
using snaggletooth::envelopeRateFires;
using snaggletooth::keyOffVoice;
using snaggletooth::keyOnVoice;
using snaggletooth::nextGlobalCounter;
using snaggletooth::pollKeying;
using snaggletooth::stepDspCycle;
using snaggletooth::stepDspSample;
using snaggletooth::stepVoiceEnvelope;
using snaggletooth::tickDspSample;

using Ram = std::array<std::uint8_t, 65536>;

constexpr std::uint8_t kKon = 0x4C;
constexpr std::uint8_t kKoff = 0x5C;
constexpr std::uint8_t kDir = 0x5D;
constexpr std::uint8_t kEndx = 0x7C;

std::uint8_t& adsr1(DspState& dsp, std::size_t v) { return dsp[v * 0x10 + 0x05]; }
std::uint8_t& adsr2(DspState& dsp, std::size_t v) { return dsp[v * 0x10 + 0x06]; }
std::uint8_t& gain(DspState& dsp, std::size_t v) { return dsp[v * 0x10 + 0x07]; }
std::uint8_t envx(const DspState& dsp, std::size_t v) {
  return const_cast<DspState&>(dsp)[v * 0x10 + 0x08];
}

// Sets a voice straight into a phase at a level, out of startup, so a single
// stepVoiceEnvelope exercises one envelope operation.
void placeEnvelope(DspState& dsp, std::size_t v, EnvPhase phase, std::uint16_t level) {
  dsp.voices[v].phase = phase;
  dsp.voices[v].envelope = level;
  dsp.voices[v].konDelay = 0;
}

// A RAM carrying one filter-0 block at 1000h as voice 0's self-looping source
// (directory page 02h, srcn 0), enough for keyOnVoice to prime a stream.
struct Keyable {
  DspState dsp;
  Ram ram{};

  Keyable() {
    const std::uint16_t base = 0x02 * 0x100;
    ram[base] = 0x00;      // start low
    ram[base + 1] = 0x10;  // start high -> 1000h
    ram[base + 2] = 0x00;  // loop -> 1000h
    ram[base + 3] = 0x10;
    ram[0x1000] = 0xC0;  // shift 12, filter 0, no end
    dsp[kDir] = 0x02;
  }
};

// ── The global counter and the rate fire predicate ──────────────────────────

TEST(GlobalCounter, WrapsFromZeroToSevenSevenFF) {
  // "counts from 0x77ff to zero, decrementing by one each sample" (Anomie,
  // COUNTERS); the SNESdev wiki adds it starts at 0 and the first tick wraps.
  EXPECT_EQ(nextGlobalCounter(0), 0x77FF);
  EXPECT_EQ(nextGlobalCounter(0x77FF), 0x77FE);
  EXPECT_EQ(nextGlobalCounter(1), 0);
}

TEST(GlobalCounter, TickAdvancesTheCounterAndSampleIndex) {
  DspState dsp;
  tickDspSample(dsp);
  EXPECT_EQ(dsp.globalCounter, 0x77FF);
  EXPECT_EQ(dsp.sampleIndex, 1u);
}

TEST(RateFires, RateZeroNeverFiresAndRate31Always) {
  // Rate 0 has period Infinite (the 'never' sentinel); rate 31 has period 1
  // (every sample). Both hold at any counter value.
  for (std::uint16_t c : {std::uint16_t(0), std::uint16_t(0x1234), std::uint16_t(0x77FF)}) {
    EXPECT_FALSE(envelopeRateFires(c, 0)) << "counter " << c;
    EXPECT_TRUE(envelopeRateFires(c, 31)) << "counter " << c;
  }
}

TEST(RateFires, PeriodAndOffsetMatchTheTranscribedTables) {
  // (counter + offset[rate]) % period[rate] == 0 (Anomie line 230). Rate 1:
  // period 2048, offset 0 -> fires when counter % 2048 == 0. Rate 2: period
  // 1536, offset 1040 -> fires at counter 496 (496+1040 = 1536). Rate 3:
  // period 1280, offset 536 -> fires at counter 744 (744+536 = 1280).
  EXPECT_TRUE(envelopeRateFires(0, 1));
  EXPECT_FALSE(envelopeRateFires(1, 1));
  EXPECT_TRUE(envelopeRateFires(496, 2));
  EXPECT_FALSE(envelopeRateFires(497, 2));
  EXPECT_TRUE(envelopeRateFires(744, 3));
  EXPECT_FALSE(envelopeRateFires(745, 3));
}

// ── Attack ──────────────────────────────────────────────────────────────────

TEST(Attack, AddsThirtyTwoWhenTheCounterFires) {
  // "Attack rate ... Step=+32" (fullsnes line 2902). Attack index 0 gives rate
  // 1 (offset 0), which fires at counter 0.
  DspState dsp;
  adsr1(dsp, 0) = 0x80;  // ADSR mode, attack index 0
  placeEnvelope(dsp, 0, EnvPhase::Attack, 0);
  EXPECT_EQ(stepVoiceEnvelope(dsp, 0, false), 0x20);
  EXPECT_EQ(dsp.voices[0].phase, EnvPhase::Attack);
  EXPECT_EQ(envx(dsp, 0), 0x02);  // 0x20 >> 4
}

TEST(Attack, AddsOneThousandTwentyFourAtRateFifteen) {
  // "or Step=+1024 when Rate=31" — attack index 15 (fullsnes line 2902), which
  // is also rate 31 and fires every sample.
  DspState dsp;
  adsr1(dsp, 0) = 0x8F;  // ADSR mode, attack index 15
  placeEnvelope(dsp, 0, EnvPhase::Attack, 0);
  EXPECT_EQ(stepVoiceEnvelope(dsp, 0, false), 0x400);
  EXPECT_EQ(dsp.voices[0].phase, EnvPhase::Attack);
}

TEST(Attack, SwitchesToDecayWhenTheValueExceedsSevenFF) {
  // The switch fires when the new value exceeds 0x7FF (Anomie). From 0x7E0 the
  // +32 step reaches 0x800, which clips to 0x7FF and enters Decay — the attack
  // peak is 0x7FF.
  DspState dsp;
  adsr1(dsp, 0) = 0x80;
  placeEnvelope(dsp, 0, EnvPhase::Attack, 0x7E0);
  EXPECT_EQ(stepVoiceEnvelope(dsp, 0, false), 0x7FF);
  EXPECT_EQ(dsp.voices[0].phase, EnvPhase::Decay);
}

TEST(Attack, DoesNotSwitchAtSevenEZero) {
  // Reaching exactly 0x7E0 stays in Attack: the switch needs the value to exceed
  // 0x7FF, so 0x7E0 is one +32 step short of leaving Attack.
  DspState dsp;
  adsr1(dsp, 0) = 0x80;
  placeEnvelope(dsp, 0, EnvPhase::Attack, 0x7C0);
  EXPECT_EQ(stepVoiceEnvelope(dsp, 0, false), 0x7E0);
  EXPECT_EQ(dsp.voices[0].phase, EnvPhase::Attack);
}

// ── Decay and Sustain ───────────────────────────────────────────────────────

TEST(Decay, TakesTheExponentialStep) {
  // "Decay ... Step=-(((Level-1) SAR 8)+1)" (fullsnes line 2903): level -= 1,
  // then level -= level >> 8. From 0x600: 0x5FF - (0x5FF>>8=5) = 0x5FA.
  DspState dsp;
  adsr1(dsp, 0) = 0x80;   // decay index 0 -> rate 16 (offset 0), fires at counter 0
  adsr2(dsp, 0) = 0x00;   // sustain level 0
  placeEnvelope(dsp, 0, EnvPhase::Decay, 0x600);
  EXPECT_EQ(stepVoiceEnvelope(dsp, 0, false), 0x5FA);
  EXPECT_EQ(dsp.voices[0].phase, EnvPhase::Decay);
}

TEST(Decay, SwitchesToSustainWhenUpperBitsReachTheSustainLevel) {
  // Decay->Sustain fires when (level >> 8) == sustain level (Anomie). Sustain
  // level 3 (adsr2 bits 7-5 = 011): from 0x401 the step gives 0x3FC, whose
  // upper 3 bits are 3 -> Sustain.
  DspState dsp;
  adsr1(dsp, 0) = 0x80;
  adsr2(dsp, 0) = 0x60;  // sustain level 3, sustain rate 0
  placeEnvelope(dsp, 0, EnvPhase::Decay, 0x401);
  EXPECT_EQ(stepVoiceEnvelope(dsp, 0, false), 0x3FC);
  EXPECT_EQ(dsp.voices[0].phase, EnvPhase::Sustain);
}

TEST(Sustain, TakesTheExponentialStep) {
  // "Sustain rate ... Step=-(((Level-1) SAR 8)+1)" (fullsnes line 2905): the
  // same exponential as Decay, at the sustain rate. From 0x300: 0x2FD.
  DspState dsp;
  adsr1(dsp, 0) = 0x80;
  adsr2(dsp, 0) = 0x10;  // sustain rate 16 (offset 0), sustain level 0
  placeEnvelope(dsp, 0, EnvPhase::Sustain, 0x300);
  EXPECT_EQ(stepVoiceEnvelope(dsp, 0, false), 0x2FD);
  EXPECT_EQ(dsp.voices[0].phase, EnvPhase::Sustain);
}

// ── Release ─────────────────────────────────────────────────────────────────

TEST(Release, SubtractsEightEverySample) {
  // "Release rate ... Rate=31, Step=-8" (fullsnes line 2907) — every sample,
  // overriding the ADSR/GAIN settings.
  DspState dsp;
  placeEnvelope(dsp, 0, EnvPhase::Release, 0x100);
  EXPECT_EQ(stepVoiceEnvelope(dsp, 0, false), 0xF8);
  EXPECT_EQ(dsp.voices[0].phase, EnvPhase::Release);
}

TEST(Release, StopsAtZero) {
  // The level clamps at 0 rather than wrapping.
  DspState dsp;
  placeEnvelope(dsp, 0, EnvPhase::Release, 4);
  EXPECT_EQ(stepVoiceEnvelope(dsp, 0, false), 0);
}

TEST(Release, BrrEndMuteDropsToZeroImmediately) {
  // "Step=-800h when BRR-end" (fullsnes line 2907) / Anomie: a code-1 End+Mute
  // block sends the voice to Release with the envelope at 0 immediately.
  DspState dsp;
  placeEnvelope(dsp, 0, EnvPhase::Attack, 0x500);
  EXPECT_EQ(stepVoiceEnvelope(dsp, 0, /*brrEndMute=*/true), 0);
  EXPECT_EQ(dsp.voices[0].phase, EnvPhase::Release);
}

// ── GAIN ────────────────────────────────────────────────────────────────────

TEST(Gain, DirectSetsAFixedLevel) {
  // "Direct Gain ... Envelope Level = N*16, Rate=Infinite" (fullsnes lines
  // 2920-2922): with ADSR1.7=0 and GAIN.7=0 the level is GGGGGGG*16 every sample.
  DspState dsp;
  adsr1(dsp, 0) = 0x00;
  gain(dsp, 0) = 0x40;  // direct, value 0x40 -> 0x400
  placeEnvelope(dsp, 0, EnvPhase::Decay, 0x123);
  EXPECT_EQ(stepVoiceEnvelope(dsp, 0, false), 0x400);
}

TEST(Gain, CustomLinearDecreaseSubtractsThirtyTwo) {
  // "Mode 0 = Linear Decrease ... Step=-32" (fullsnes line 2933).
  DspState dsp;
  adsr1(dsp, 0) = 0x00;
  gain(dsp, 0) = 0x90;  // custom, mode 0, rate 16 (offset 0)
  placeEnvelope(dsp, 0, EnvPhase::Sustain, 0x100);
  EXPECT_EQ(stepVoiceEnvelope(dsp, 0, false), 0xE0);
}

TEST(Gain, CustomLinearIncreaseAddsThirtyTwo) {
  // "Mode 2 = Linear Increase ... Step=+32" (fullsnes line 2935).
  DspState dsp;
  adsr1(dsp, 0) = 0x00;
  gain(dsp, 0) = 0xD0;  // custom, mode 2, rate 16
  placeEnvelope(dsp, 0, EnvPhase::Sustain, 0x100);
  EXPECT_EQ(stepVoiceEnvelope(dsp, 0, false), 0x120);
}

TEST(Gain, CustomExpDecreaseTakesTheExponentialStep) {
  // "Mode 1 = Exp Decrease ... Step=-(((Level-1) SAR 8)+1)" (fullsnes 2934).
  DspState dsp;
  adsr1(dsp, 0) = 0x00;
  gain(dsp, 0) = 0xB0;  // custom, mode 1, rate 16
  placeEnvelope(dsp, 0, EnvPhase::Sustain, 0x300);
  EXPECT_EQ(stepVoiceEnvelope(dsp, 0, false), 0x2FD);
}

TEST(Gain, CustomBentIncreaseAddsThirtyTwoBelowSixHundred) {
  // "Mode 3 = Bent Increase ... If Level<600h then Step=+32 else Step=+8"
  // (fullsnes line 2936), using the previous sample's reference value.
  DspState dsp;
  adsr1(dsp, 0) = 0x00;
  gain(dsp, 0) = 0xF0;  // custom, mode 3, rate 16
  placeEnvelope(dsp, 0, EnvPhase::Sustain, 0x100);
  dsp.voices[0].bentGainRef = 0x100;  // < 0x600
  EXPECT_EQ(stepVoiceEnvelope(dsp, 0, false), 0x120);
}

TEST(Gain, CustomBentIncreaseAddsEightAtOrAboveSixHundred) {
  DspState dsp;
  adsr1(dsp, 0) = 0x00;
  gain(dsp, 0) = 0xF0;
  placeEnvelope(dsp, 0, EnvPhase::Sustain, 0x100);
  dsp.voices[0].bentGainRef = 0x600;  // >= 0x600
  EXPECT_EQ(stepVoiceEnvelope(dsp, 0, false), 0x108);
}

TEST(Gain, BentIncreaseUsesTheNegativeReference) {
  // "a negative value for the new value will result in the clipped version
  // being greater than 0x600" (fullsnes lines 2992-2995): a Linear Decrease
  // past 0 saves a negative reference, which reads past 0x600, so the following
  // Bent-Increase sample takes the +8 branch even though the level is 0.
  DspState dsp;
  adsr1(dsp, 0) = 0x00;
  gain(dsp, 0) = 0x90;  // Linear Decrease, rate 16
  placeEnvelope(dsp, 0, EnvPhase::Sustain, 0x10);
  EXPECT_EQ(stepVoiceEnvelope(dsp, 0, false), 0);         // 0x10 - 32 clamps to 0
  EXPECT_EQ(dsp.voices[0].bentGainRef, 0xFFF0);           // -16, unclipped
  gain(dsp, 0) = 0xF0;  // switch to Bent Increase
  EXPECT_EQ(stepVoiceEnvelope(dsp, 0, false), 8);         // +8, not +32, at level 0
}

// ── The Bent-Increase reference ─────────────────────────────────────────────
//
// Bent Increase chooses its step from the value the mode computed last sample,
// never from the level itself. The mode computes that value every sample, so a
// rate of 0 — which never lets the level move — still supplies a reference of
// its own, and a reference driven outside the 11-bit range keeps the sign or the
// carry that took it there. Each pair below brackets one edge of the comparison.

TEST(BentReference, ARateOfZeroSuppliesTheValueItsModeComputed) {
  // A Linear Increase parked at rate 0 computes 0x5E0 + 32 = 0x600 every sample
  // while holding the level still, so the Bent Increase that follows reads a
  // reference at the boundary and takes +8 — from a level below it.
  DspState dsp;
  adsr1(dsp, 0) = 0x00;
  gain(dsp, 0) = 0xC0;  // custom, Linear Increase, rate 0
  placeEnvelope(dsp, 0, EnvPhase::Sustain, 0x5E0);
  EXPECT_EQ(stepVoiceEnvelope(dsp, 0, false), 0x5E0);  // the rate holds the level
  EXPECT_EQ(dsp.voices[0].bentGainRef, 0x600);
  gain(dsp, 0) = 0xFF;  // Bent Increase, rate 31
  EXPECT_EQ(stepVoiceEnvelope(dsp, 0, false), 0x5E8);
}

TEST(BentReference, ARateOfZeroOneStepShortOfTheBoundaryTakesTheLargerStep) {
  // The bracket to the case above, from the other side and by another mode: an
  // Exponential Decrease parked at rate 0 over 0x606 computes 0x5FF, one below
  // the boundary, so the following step is +32 from a level above it.
  DspState dsp;
  adsr1(dsp, 0) = 0x00;
  gain(dsp, 0) = 0xA0;  // custom, Exponential Decrease, rate 0
  placeEnvelope(dsp, 0, EnvPhase::Sustain, 0x606);
  EXPECT_EQ(stepVoiceEnvelope(dsp, 0, false), 0x606);
  EXPECT_EQ(dsp.voices[0].bentGainRef, 0x5FF);
  gain(dsp, 0) = 0xFF;
  EXPECT_EQ(stepVoiceEnvelope(dsp, 0, false), 0x626);
}

TEST(BentReference, AReferenceCarriedPastTheRangeReadsPastTheBoundary) {
  // 0x7E0 + 32 = 0x800 leaves the 11-bit range. The reference keeps the carry,
  // so it reads past 0x600 and the step is +8; a reference clipped to 11 bits
  // would read 0x000 and take +32, saturating the level at 0x7FF instead.
  DspState dsp;
  adsr1(dsp, 0) = 0x00;
  gain(dsp, 0) = 0xC0;  // custom, Linear Increase, rate 0
  placeEnvelope(dsp, 0, EnvPhase::Sustain, 0x7E0);
  EXPECT_EQ(stepVoiceEnvelope(dsp, 0, false), 0x7E0);
  EXPECT_EQ(dsp.voices[0].bentGainRef, 0x800);
  gain(dsp, 0) = 0xFF;
  EXPECT_EQ(stepVoiceEnvelope(dsp, 0, false), 0x7E8);
}

TEST(BentReference, AReferenceOneShortOfTheRangeTakesTheSameStep) {
  // Its bracket: 0x7DF + 32 = 0x7FF stays inside the range and reads past 0x600
  // on its own, so the pair differs by one in the level and not in the step.
  DspState dsp;
  adsr1(dsp, 0) = 0x00;
  gain(dsp, 0) = 0xC0;
  placeEnvelope(dsp, 0, EnvPhase::Sustain, 0x7DF);
  EXPECT_EQ(stepVoiceEnvelope(dsp, 0, false), 0x7DF);
  EXPECT_EQ(dsp.voices[0].bentGainRef, 0x7FF);
  gain(dsp, 0) = 0xFF;
  EXPECT_EQ(stepVoiceEnvelope(dsp, 0, false), 0x7E7);
}

// ── ENVX ────────────────────────────────────────────────────────────────────

TEST(Envx, HoldsTheHighSevenBitsAndOverwritesWrites) {
  // "VxENVX ... Upper 7bit of the 11bit envelope volume" (fullsnes 2944), and
  // "whatever value you write will be overwritten at 32000 Hz" (2947-2948).
  DspState dsp;
  adsr1(dsp, 0) = 0x80;
  placeEnvelope(dsp, 0, EnvPhase::Attack, 0x2E0);
  dsp[0 * 0x10 + 0x08] = 0x55;  // a stray CPU write to ENVX
  stepVoiceEnvelope(dsp, 0, false);  // 0x2E0 + 0x20 = 0x300
  EXPECT_EQ(envx(dsp, 0), 0x30);     // 0x300 >> 4, the write overwritten
}

// ── The counter gate ────────────────────────────────────────────────────────

TEST(EnvelopeGate, HoldsTheLevelWhenTheCounterDoesNotFire) {
  // Between fires the level and phase are untouched (the Bent-Increase reference
  // is not — it follows the mode, not the counter). Rate 1 fires only at
  // counter % 2048 == 0; counter 1 does not fire.
  DspState dsp;
  adsr1(dsp, 0) = 0x80;  // attack index 0 -> rate 1
  placeEnvelope(dsp, 0, EnvPhase::Attack, 0x40);
  dsp.globalCounter = 1;
  EXPECT_EQ(stepVoiceEnvelope(dsp, 0, false), 0x40);
  EXPECT_EQ(dsp.voices[0].phase, EnvPhase::Attack);
  EXPECT_EQ(envx(dsp, 0), 0x04);
}

// ── The phase machine under GAIN ────────────────────────────────────────────
//
// The selected mode computes a candidate level every sample; the counter decides
// only whether the level takes it. So the Attack->Decay switch can fire while a
// rate of 0 holds the level perfectly still, and it fires on the mode's own step
// — the value that leaves the unsigned 11-bit range, whether by overshooting
// 0x7FF or by going below zero. Each pair below brackets one mode's step.

TEST(PhaseMachine, LinearIncreaseLeavesAttackWhenItsStepWouldOvershoot) {
  DspState dsp;
  adsr1(dsp, 0) = 0x00;  // GAIN mode
  gain(dsp, 0) = 0xC0;   // custom, Linear Increase, rate 0 - never updates
  placeEnvelope(dsp, 0, EnvPhase::Attack, 0x7E0);
  EXPECT_EQ(stepVoiceEnvelope(dsp, 0, false), 0x7E0);  // 0x7E0 + 32 = 0x800
  EXPECT_EQ(dsp.voices[0].phase, EnvPhase::Decay);
}

TEST(PhaseMachine, LinearIncreaseHoldsAttackWhenItsStepStillFits) {
  DspState dsp;
  adsr1(dsp, 0) = 0x00;
  gain(dsp, 0) = 0xC0;
  placeEnvelope(dsp, 0, EnvPhase::Attack, 0x7DF);
  EXPECT_EQ(stepVoiceEnvelope(dsp, 0, false), 0x7DF);  // 0x7DF + 32 = 0x7FF
  EXPECT_EQ(dsp.voices[0].phase, EnvPhase::Attack);
}

TEST(PhaseMachine, BentIncreaseLeavesAttackOnItsOwnSmallerStep) {
  // Bent Increase steps +8 once its reference reaches 0x600, so it switches 0x18
  // higher than the Linear Increase pair above: the step, not a fixed level, sets
  // the boundary.
  DspState dsp;
  adsr1(dsp, 0) = 0x00;
  gain(dsp, 0) = 0xE0;  // custom, Bent Increase, rate 0
  placeEnvelope(dsp, 0, EnvPhase::Attack, 0x7F8);
  dsp.voices[0].bentGainRef = 0x7F8;
  EXPECT_EQ(stepVoiceEnvelope(dsp, 0, false), 0x7F8);  // 0x7F8 + 8 = 0x800
  EXPECT_EQ(dsp.voices[0].phase, EnvPhase::Decay);
}

TEST(PhaseMachine, BentIncreaseHoldsAttackOneStepBelow) {
  DspState dsp;
  adsr1(dsp, 0) = 0x00;
  gain(dsp, 0) = 0xE0;
  placeEnvelope(dsp, 0, EnvPhase::Attack, 0x7F7);
  dsp.voices[0].bentGainRef = 0x7F7;
  EXPECT_EQ(stepVoiceEnvelope(dsp, 0, false), 0x7F7);  // 0x7F7 + 8 = 0x7FF
  EXPECT_EQ(dsp.voices[0].phase, EnvPhase::Attack);
}

TEST(PhaseMachine, ADecreasingModeLeavesAttackWhenItsStepGoesBelowZero) {
  // The switch reads a value outside the 11-bit range, which a decrease reaches
  // from underneath: 0x1F - 32 is negative and takes the voice out of Attack.
  DspState dsp;
  adsr1(dsp, 0) = 0x00;
  gain(dsp, 0) = 0x80;  // custom, Linear Decrease, rate 0
  placeEnvelope(dsp, 0, EnvPhase::Attack, 0x1F);
  EXPECT_EQ(stepVoiceEnvelope(dsp, 0, false), 0x1F);
  EXPECT_EQ(dsp.voices[0].phase, EnvPhase::Decay);
}

TEST(PhaseMachine, ADecreasingModeHoldsAttackWhenItsStepReachesExactlyZero) {
  DspState dsp;
  adsr1(dsp, 0) = 0x00;
  gain(dsp, 0) = 0x80;
  placeEnvelope(dsp, 0, EnvPhase::Attack, 0x20);
  EXPECT_EQ(stepVoiceEnvelope(dsp, 0, false), 0x20);  // 0x20 - 32 = 0
  EXPECT_EQ(dsp.voices[0].phase, EnvPhase::Attack);
}

TEST(PhaseMachine, DecayToSustainIgnoresACandidateOutsideTheRange) {
  // A candidate below zero is the Attack->Decay case, and it belongs to no
  // boundary: this voice sits at 0x1F under a Linear Decrease, so its candidate
  // is -1 sample after sample and it stays in Decay however long it is stepped.
  DspState dsp;
  adsr1(dsp, 0) = 0x00;
  adsr2(dsp, 0) = 0x00;
  gain(dsp, 0) = 0x80;  // Linear Decrease, rate 0
  placeEnvelope(dsp, 0, EnvPhase::Attack, 0x1F);
  stepVoiceEnvelope(dsp, 0, false);
  ASSERT_EQ(dsp.voices[0].phase, EnvPhase::Decay);
  for (int i = 0; i < 4; ++i) stepVoiceEnvelope(dsp, 0, false);
  EXPECT_EQ(dsp.voices[0].phase, EnvPhase::Decay);
  EXPECT_EQ(dsp.voices[0].envelope, 0x1F);
}

// ── Decay->Sustain: the boundary comes from the register in charge ───────────
//
// Under ADSR the boundary is VxADSR2's sustain level. Under a GAIN mode the
// hardware reads it from VxGAIN bits 7-5 instead — the gain mode and its enable
// bit, which are not a sustain level at all — so each mode sustains at its own
// fixed boundary: 4 for Linear Decrease, 5 for Exp Decrease, 6 for Linear
// Increase, 7 for Bent Increase (fullsnes 2977-2984, "though accidently reading
// a garbage boundary value from VxGAIN.Bit7-5"). The comparison reads the
// candidate every sample, so a rate of 0 that never moves the level still
// changes the phase. Every case below sets a sustain level that does NOT match,
// so a boundary read from VxADSR2 could not produce these answers.

TEST(SustainBoundary, DirectGainSustainsOnItsOwnTopThreeBits) {
  DspState dsp;
  adsr1(dsp, 0) = 0x00;
  adsr2(dsp, 0) = 0x20;  // sustain level 1 - not the boundary in force
  gain(dsp, 0) = 0x0F;   // direct, level 0x0F0; bits 7-5 are 0
  placeEnvelope(dsp, 0, EnvPhase::Decay, 0x0F0);
  stepVoiceEnvelope(dsp, 0, false);  // candidate 0x0F0, upper 3 bits 0
  EXPECT_EQ(dsp.voices[0].phase, EnvPhase::Sustain);
}

TEST(SustainBoundary, DirectGainHoldsDecayOneBandAbove) {
  DspState dsp;
  adsr1(dsp, 0) = 0x00;
  adsr2(dsp, 0) = 0x20;
  gain(dsp, 0) = 0x10;  // direct, level 0x100; bits 7-5 are still 0
  placeEnvelope(dsp, 0, EnvPhase::Decay, 0x100);
  stepVoiceEnvelope(dsp, 0, false);  // candidate 0x100, upper 3 bits 1
  EXPECT_EQ(dsp.voices[0].phase, EnvPhase::Decay);
}

TEST(SustainBoundary, LinearDecreaseSustainsWhenItsStepReachesBoundaryFour) {
  DspState dsp;
  adsr1(dsp, 0) = 0x00;
  adsr2(dsp, 0) = 0x20;
  gain(dsp, 0) = 0x80;  // Linear Decrease, rate 0; bits 7-5 are 4
  placeEnvelope(dsp, 0, EnvPhase::Decay, 0x420);
  EXPECT_EQ(stepVoiceEnvelope(dsp, 0, false), 0x420);  // 0x420 - 32 = 0x400
  EXPECT_EQ(dsp.voices[0].phase, EnvPhase::Sustain);
}

TEST(SustainBoundary, LinearDecreaseReadsTheCandidateAndNotTheStoredLevel) {
  // The case that separates the two readings: 0x41F's own upper 3 bits are
  // already 4, so a comparison against the stored level would sustain here. The
  // step is what is compared, and 0x41F - 32 = 0x3FF sits one band below.
  DspState dsp;
  adsr1(dsp, 0) = 0x00;
  adsr2(dsp, 0) = 0x20;
  gain(dsp, 0) = 0x80;
  placeEnvelope(dsp, 0, EnvPhase::Decay, 0x41F);
  EXPECT_EQ(stepVoiceEnvelope(dsp, 0, false), 0x41F);
  EXPECT_EQ(dsp.voices[0].phase, EnvPhase::Decay);
}

TEST(SustainBoundary, ExpDecreaseSustainsWhenItsStepReachesBoundaryFive) {
  DspState dsp;
  adsr1(dsp, 0) = 0x00;
  adsr2(dsp, 0) = 0x20;
  gain(dsp, 0) = 0xA0;  // Exp Decrease, rate 0; bits 7-5 are 5
  placeEnvelope(dsp, 0, EnvPhase::Decay, 0x506);
  EXPECT_EQ(stepVoiceEnvelope(dsp, 0, false), 0x506);  // 0x506 - 6 = 0x500
  EXPECT_EQ(dsp.voices[0].phase, EnvPhase::Sustain);
}

TEST(SustainBoundary, ExpDecreaseHoldsDecayOneStepShort) {
  DspState dsp;
  adsr1(dsp, 0) = 0x00;
  adsr2(dsp, 0) = 0x20;
  gain(dsp, 0) = 0xA0;
  placeEnvelope(dsp, 0, EnvPhase::Decay, 0x505);
  EXPECT_EQ(stepVoiceEnvelope(dsp, 0, false), 0x505);  // 0x505 - 6 = 0x4FF
  EXPECT_EQ(dsp.voices[0].phase, EnvPhase::Decay);
}

TEST(SustainBoundary, LinearIncreaseSustainsWhenItsStepReachesBoundarySix) {
  DspState dsp;
  adsr1(dsp, 0) = 0x00;
  adsr2(dsp, 0) = 0x20;
  gain(dsp, 0) = 0xC0;  // Linear Increase, rate 0; bits 7-5 are 6
  placeEnvelope(dsp, 0, EnvPhase::Decay, 0x5E0);
  EXPECT_EQ(stepVoiceEnvelope(dsp, 0, false), 0x5E0);  // 0x5E0 + 32 = 0x600
  EXPECT_EQ(dsp.voices[0].phase, EnvPhase::Sustain);
}

TEST(SustainBoundary, LinearIncreaseHoldsDecayOneStepShort) {
  DspState dsp;
  adsr1(dsp, 0) = 0x00;
  adsr2(dsp, 0) = 0x20;
  gain(dsp, 0) = 0xC0;
  placeEnvelope(dsp, 0, EnvPhase::Decay, 0x5DF);
  EXPECT_EQ(stepVoiceEnvelope(dsp, 0, false), 0x5DF);  // 0x5DF + 32 = 0x5FF
  EXPECT_EQ(dsp.voices[0].phase, EnvPhase::Decay);
}

TEST(SustainBoundary, BentIncreaseSustainsOnItsOwnSmallerStepAtBoundarySeven) {
  DspState dsp;
  adsr1(dsp, 0) = 0x00;
  adsr2(dsp, 0) = 0x20;
  gain(dsp, 0) = 0xE0;  // Bent Increase, rate 0; bits 7-5 are 7
  placeEnvelope(dsp, 0, EnvPhase::Decay, 0x6F8);
  dsp.voices[0].bentGainRef = 0x6F8;                   // at or past 0x600, so +8
  EXPECT_EQ(stepVoiceEnvelope(dsp, 0, false), 0x6F8);  // 0x6F8 + 8 = 0x700
  EXPECT_EQ(dsp.voices[0].phase, EnvPhase::Sustain);
}

TEST(SustainBoundary, BentIncreaseHoldsDecayOneStepShort) {
  DspState dsp;
  adsr1(dsp, 0) = 0x00;
  adsr2(dsp, 0) = 0x20;
  gain(dsp, 0) = 0xE0;
  placeEnvelope(dsp, 0, EnvPhase::Decay, 0x6F7);
  dsp.voices[0].bentGainRef = 0x6F7;
  EXPECT_EQ(stepVoiceEnvelope(dsp, 0, false), 0x6F7);  // 0x6F7 + 8 = 0x6FF
  EXPECT_EQ(dsp.voices[0].phase, EnvPhase::Decay);
}

TEST(SustainBoundary, AdsrModeReadsTheSustainLevelFromAdsr2) {
  // With ADSR selected the boundary is the sustain level again, and VxGAIN's
  // bits are ignored: this GAIN would name boundary 7, and the voice sustains at
  // 1 because ADSR2 says so.
  DspState dsp;
  adsr1(dsp, 0) = 0x80;  // ADSR, decay rate 0
  adsr2(dsp, 0) = 0x20;  // sustain level 1
  gain(dsp, 0) = 0xE0;
  placeEnvelope(dsp, 0, EnvPhase::Decay, 0x200);
  dsp.globalCounter = 0;
  stepVoiceEnvelope(dsp, 0, false);  // 0x200 - 2 = 0x1FE, upper 3 bits 1
  EXPECT_EQ(dsp.voices[0].phase, EnvPhase::Sustain);
}

TEST(SustainBoundary, AdsrModeHoldsDecayWhenTheStepMissesTheSustainLevel) {
  DspState dsp;
  adsr1(dsp, 0) = 0x80;
  adsr2(dsp, 0) = 0x20;
  gain(dsp, 0) = 0xE0;
  placeEnvelope(dsp, 0, EnvPhase::Decay, 0x300);
  dsp.globalCounter = 0;
  stepVoiceEnvelope(dsp, 0, false);  // 0x300 - 3 = 0x2FD, upper 3 bits 2
  EXPECT_EQ(dsp.voices[0].phase, EnvPhase::Decay);
}

// ── Keying ──────────────────────────────────────────────────────────────────

TEST(Keying, KeyOnEntersAttackWithTheStartupCountdown) {
  // "there are 5 'empty' samples before envelope updates and BRR decoding
  // actually begin" (fullsnes lines 3053-3054); KON sets the state to Attack
  // and the envelope to 0 (3050-3051), and clears the voice's ENDX bit. The
  // counter holds all five: the keying load precedes voice 0's compute in the
  // slot they share, so the load's own slot is the keyed voice's first silent
  // call (the count and order arbitrated by spc_dsp6's key-on sub-tests,
  // `KON/envx during kon` in particular).
  Keyable k;
  k.dsp[kEndx] = 0xFF;
  keyOnVoice(k.dsp, k.ram, 0);
  EXPECT_EQ(k.dsp.voices[0].phase, EnvPhase::Attack);
  EXPECT_EQ(k.dsp.voices[0].envelope, 0);
  EXPECT_EQ(k.dsp.voices[0].konDelay, 5);
  EXPECT_EQ(k.dsp.voices[0].brrAddress, 0x1000);   // stream primed from the start
  EXPECT_EQ(k.dsp[kEndx] & 0x01, 0);               // this voice's ENDX bit cleared
  EXPECT_EQ(k.dsp[kEndx] & 0xFE, 0xFE);            // the others untouched
}

TEST(Keying, FiveSilentCallsPrecedeTheFirstAttackStep) {
  // The five silent startup samples are five silent envelope calls — the first
  // of them the load's own slot for voice 0, the following samples for voices
  // 1-7 — and the sixth call takes the first attack step.
  Keyable k;
  adsr1(k.dsp, 0) = 0x80;  // attack index 0, fires at counter 0
  keyOnVoice(k.dsp, k.ram, 0);
  for (int n = 0; n < 5; ++n) {
    EXPECT_EQ(stepVoiceEnvelope(k.dsp, 0, false), 0) << "startup call " << n;
  }
  EXPECT_EQ(k.dsp.voices[0].konDelay, 0);
  EXPECT_EQ(stepVoiceEnvelope(k.dsp, 0, false), 0x20);  // Attack begins
}

TEST(Keying, KeyOffEntersRelease) {
  // "Setting 1 to the KOFF bit will transition the voice to the Release state"
  // (fullsnes lines 3063-3064).
  DspState dsp;
  placeEnvelope(dsp, 0, EnvPhase::Sustain, 0x400);
  keyOffVoice(dsp, 0);
  EXPECT_EQ(dsp.voices[0].phase, EnvPhase::Release);
}

TEST(Keying, PollRunsOnEvenSamplesOnly) {
  // KON/KOFF are polled every second sample (Anomie; SNESdev Errata); the pin
  // runs the poll on even sample indices. An odd-index poll is a no-op.
  Keyable k;
  k.dsp.internalKon = 0x01;

  k.dsp.sampleIndex = 1;  // odd -> ignored
  pollKeying(k.dsp, k.ram);
  EXPECT_EQ(k.dsp.voices[0].phase, EnvPhase::Release);  // unchanged power-on state
  EXPECT_EQ(k.dsp.voices[0].konDelay, 0);
  EXPECT_EQ(k.dsp.internalKon, 0x01);  // still pending

  k.dsp.sampleIndex = 0;  // even -> polled
  pollKeying(k.dsp, k.ram);
  EXPECT_EQ(k.dsp.voices[0].konDelay, 5);
  EXPECT_TRUE(k.dsp.voices[0].restartPending)
      << "the poll arms the restart; the voice's own compute applies it";
  EXPECT_EQ(k.dsp.internalKon, 0x00);  // consumed
}

TEST(Keying, TheKeyOnComesFromTheInternalValueNotTheRegister) {
  // "If the 'internal' value of KON has the channel's bit set, perform the KON
  // actions" (Anomie 720-721, fullsnes 3141-3142). A register bit with no
  // pending internal bit keys nothing on.
  Keyable k;
  k.dsp[kKon] = 0x01;       // register set
  k.dsp.internalKon = 0x00;  // nothing pending
  k.dsp.sampleIndex = 0;
  pollKeying(k.dsp, k.ram);
  EXPECT_EQ(k.dsp.voices[0].phase, EnvPhase::Release);
  EXPECT_EQ(k.dsp.voices[0].konDelay, 0);
}

TEST(Keying, AKeyOnHappensOnceWhileTheKonRegisterStaysSet) {
  // "KON effectively takes effect 'on write', even though a non-zero value can
  // be read back much later" (Anomie 725-727, fullsnes 3148-3150). The poll
  // clears the internal value, so a register bit left set does not re-key the
  // voice on every later poll -- which would hold it in the five-sample startup
  // forever and freeze its envelope at 0.
  Keyable k;
  adsr1(k.dsp, 0) = 0x80;  // attack index 0, fires at counter 0
  k.dsp[kKon] = 0x01;
  k.dsp.internalKon = 0x01;

  k.dsp.sampleIndex = 0;
  pollKeying(k.dsp, k.ram);
  EXPECT_EQ(k.dsp.voices[0].konDelay, 5);

  // The poll arms the restart; the voice's own compute applies it and runs the
  // startup's first call. The whole-sample step ticks the global counter, so
  // put it back where the attack-fires-at-counter-0 premise needs it.
  (void)stepDspSample(k.dsp, std::span<const std::uint8_t, 65536>{k.ram});
  k.dsp.globalCounter = 0;
  EXPECT_EQ(k.dsp.voices[0].konDelay, 4);

  // Two more startup samples, then the next poll with the register still set.
  stepVoiceEnvelope(k.dsp, 0, false);
  stepVoiceEnvelope(k.dsp, 0, false);
  EXPECT_EQ(k.dsp.voices[0].konDelay, 2);

  k.dsp.sampleIndex = 2;
  pollKeying(k.dsp, k.ram);
  EXPECT_EQ(k.dsp.voices[0].konDelay, 2);  // not re-keyed back to 5

  // The startup runs out and the envelope leaves 0.
  for (int n = 0; n < 2; ++n) EXPECT_EQ(stepVoiceEnvelope(k.dsp, 0, false), 0);
  EXPECT_EQ(stepVoiceEnvelope(k.dsp, 0, false), 0x20);
  EXPECT_EQ(k.dsp[kKon], 0x01);  // the register still reads back its value
}

TEST(Keying, PollKeysOffFromTheKoffRegister) {
  Keyable k;
  placeEnvelope(k.dsp, 1, EnvPhase::Sustain, 0x400);
  k.dsp[kKoff] = 0x02;  // voice 1
  k.dsp.sampleIndex = 0;
  pollKeying(k.dsp, k.ram);
  EXPECT_EQ(k.dsp.voices[1].phase, EnvPhase::Release);
}

TEST(Keying, KoffActsOnEveryPollWhileItsBitStays) {
  // The contrast the contract draws: "KOFF and FLG.7 ... exert their influence
  // constantly until a new value is written" (Anomie 726-728, fullsnes
  // 3149-3151). Unlike KON, KOFF is read from the register at every poll.
  Keyable k;
  k.dsp[kKoff] = 0x02;  // voice 1

  k.dsp.sampleIndex = 0;
  pollKeying(k.dsp, k.ram);
  EXPECT_EQ(k.dsp.voices[1].phase, EnvPhase::Release);

  placeEnvelope(k.dsp, 1, EnvPhase::Sustain, 0x400);  // put it back
  k.dsp.sampleIndex = 2;
  pollKeying(k.dsp, k.ram);
  EXPECT_EQ(k.dsp.voices[1].phase, EnvPhase::Release);  // the bit still acts
}

// ── The per-sample BRR header check ─────────────────────────────────────────

TEST(BrrHeaderCheck, AStoppedVoiceStillReadsItsHeaderAndReleasesOnEndMute) {
  // Step V3b loads "the BRR header byte (every time)" and V3c checks its 'e' and
  // 'l' bits "to determine if the voice ends" (Anomie 80-88), both every sample.
  // Header code 1 is End+Mute: "Release, Env=000h" (fullsnes 2712). Neither the
  // load nor the check waits on the pitch counter reaching a new sample, so a
  // voice sitting still sees a block that turns End+Mute underneath it.
  Keyable k;
  const std::span<const std::uint8_t, 65536> ram{k.ram};
  gain(k.dsp, 0) = 0x7F;  // direct gain: a level that holds still at 7F0h
  keyOnVoice(k.dsp, ram, 0);
  for (int n = 0; n < 8; ++n) (void)stepDspSample(k.dsp, ram);
  ASSERT_EQ(envx(k.dsp, 0), 0x7F);
  const std::uint8_t restingIndex = k.dsp.voices[0].brrSampleIndex;

  k.ram[0x1000] = 0xC1;  // shift 12, filter 0, end set and loop clear
  (void)stepDspSample(k.dsp, ram);

  EXPECT_EQ(k.dsp.voices[0].phase, EnvPhase::Release);
  EXPECT_EQ(k.dsp.voices[0].envelope, 0);
  EXPECT_EQ(k.dsp.voices[0].brrSampleIndex, restingIndex);  // the pitch is 0: nothing decoded

  // Voice 0 computes at the last slot of a sample, and the S9 slot writes the
  // value computed one sample earlier (the ENVX read-back pipeline), so the
  // register carries the released level two samples later.
  (void)stepDspSample(k.dsp, ram);
  (void)stepDspSample(k.dsp, ram);
  EXPECT_EQ(envx(k.dsp, 0), 0x00);
}

TEST(BrrHeaderCheck, AnEndLoopHeaderLeavesAStoppedVoiceAlone) {
  // Code 3 is End+Loop, which loops without muting (fullsnes 2714); only code 1
  // releases the voice.
  Keyable k;
  const std::span<const std::uint8_t, 65536> ram{k.ram};
  gain(k.dsp, 0) = 0x7F;
  keyOnVoice(k.dsp, ram, 0);
  for (int n = 0; n < 8; ++n) (void)stepDspSample(k.dsp, ram);

  k.ram[0x1000] = 0xC3;  // end set, loop set
  (void)stepDspSample(k.dsp, ram);

  EXPECT_NE(k.dsp.voices[0].phase, EnvPhase::Release);
  EXPECT_EQ(envx(k.dsp, 0), 0x7F);
}

TEST(BrrHeaderCheck, TheCheckGoesLiveOnTheThirdComputeAfterTheKeyOnLoad) {
  // Anomie's numbered startup account has the sample after the load read the
  // start address and perform "no BRR decoding or header checks" (345-347). The
  // point the check resumes is two sample periods past the load. With the load
  // preceding voice 0's compute in the slot they share, the load's slot is
  // every voice's first compute and the cutoff is a uniform count: the check is
  // live from the third compute — which is what keeps spc_dsp6's one-sample-wide
  // header window reading identically for all eight voices.
  Keyable k;
  const std::span<const std::uint8_t, 65536> ram{k.ram};
  k.ram[0x1000] = 0xC1;  // End+Mute before either voice is keyed on
  gain(k.dsp, 0) = 0x7F;
  gain(k.dsp, 1) = 0x7F;
  keyOnVoice(k.dsp, ram, 0);
  keyOnVoice(k.dsp, ram, 1);

  (void)stepDspSample(k.dsp, ram);  // first compute: neither voice checks
  EXPECT_EQ(k.dsp.voices[0].phase, EnvPhase::Attack);
  EXPECT_EQ(k.dsp.voices[1].phase, EnvPhase::Attack);

  (void)stepDspSample(k.dsp, ram);  // second: still neither
  EXPECT_EQ(k.dsp.voices[0].phase, EnvPhase::Attack);
  EXPECT_EQ(k.dsp.voices[1].phase, EnvPhase::Attack);

  (void)stepDspSample(k.dsp, ram);  // third: both check
  EXPECT_EQ(k.dsp.voices[0].phase, EnvPhase::Release);
  EXPECT_EQ(k.dsp.voices[1].phase, EnvPhase::Release);
}

TEST(BrrHeaderCheck, ACrossingIntoAnEndMuteBlockSilencesAtTheNextSamplesCheck) {
  // The header check reads the header standing at the sample's start — the
  // hardware checks before it decodes — so the decoder entering an End+Mute
  // block mid-sample silences the voice one sample later, at the next check;
  // and the check's view stands one sample past the countdown, so an entry
  // during the startup is seen only from the second live sample's check.
  // spc_dsp6 `KON/kon when prev sample at end` pins that the first live
  // envelope step publishes its level before the check lands, and `KON/kon as
  // prev sample ends` reads that level after each of its swept key-ons —
  // two published samples carry it, the kill landing on the third. (The
  // stationary counterpart, `KON/kon then set sample's end flag`, pins the
  // other half: a header ALREADY standing at the first live step's start
  // kills before the step, publishing nothing.)
  Keyable k;
  const std::span<const std::uint8_t, 65536> ram{k.ram};
  k.ram[0x1000] = 0x00;                // the first block: silent, no end
  k.ram[0x1009] = 0x01;                // the next: end set, loop clear
  k.ram[0x02 * 0x100 + 2] = 0x09;      // loop -> 1009h, the End+Mute block itself
  k.dsp[0x03] = 0x3F;                  // V0PITCHH: ~3.94 samples per call
  gain(k.dsp, 0) = 0x7F;               // direct gain: the first live step is 7F0h
  // A sounding voice keyed directly — a young one, its computesAtRestart at
  // 0 — so the key-on's startup walks and the crossing happens mid-countdown
  // (a key-on of a silent voice holds its stream — `Misc/brr addr
  // wrap-around` — and so does a re-key of a long-sounding voice —
  // `Random/brr before playing`).
  k.dsp.voices[0].envelope = 0x100;
  keyOnVoice(k.dsp, ram, 0);

  std::array<std::uint8_t, 12> published{};
  for (std::uint8_t& sample : published) {
    (void)stepDspSample(k.dsp, ram);
    sample = envx(k.dsp, 0);
  }

  EXPECT_EQ(std::count(published.begin(), published.end(), 0x7F), 2);
  EXPECT_EQ(std::count(published.begin(), published.end(), 0x00), 10);
  EXPECT_EQ(k.dsp.voices[0].phase, EnvPhase::Release);
  EXPECT_EQ(k.dsp.voices[0].envelope, 0);
}

TEST(BrrHeaderCheck, TheCheckLeavesEndxToTheDecode) {
  // ENDX belongs to the decode: the bit is set when the voice reaches a block
  // carrying the end flag (Anomie 326-328). The header check releases the voice
  // without decoding anything, so it leaves the bit where the decode left it.
  Keyable k;
  const std::span<const std::uint8_t, 65536> ram{k.ram};
  gain(k.dsp, 0) = 0x7F;
  keyOnVoice(k.dsp, ram, 0);
  for (int n = 0; n < 8; ++n) (void)stepDspSample(k.dsp, ram);
  ASSERT_EQ(k.dsp[kEndx] & 0x01, 0);

  k.ram[0x1000] = 0xC1;
  (void)stepDspSample(k.dsp, ram);

  ASSERT_EQ(k.dsp.voices[0].phase, EnvPhase::Release);
  EXPECT_EQ(k.dsp[kEndx] & 0x01, 0);
}

TEST(BrrHeaderCheck, TheDecoderMeetsAnEndMuteBlockEightSamplesBeforeTheStream) {
  // The decoder that fills a voice's sample buffer runs eight samples ahead of
  // the interpolation cursor, so an End+Mute block silences the voice while
  // the cursor is still eight samples back in the block before. spc_dsp6
  // `Misc/brr early end at many pitches` pins the lead exactly: two silent
  // blocks chained to an End+Mute block, keyed silent at one stream sample
  // per output sample, dies with its twentieth walked sample — the block
  // sits thirty-two samples in, but the decoder is there twelve early
  // (its own full-block priming plus the eight-sample lead), and the check
  // lands one sample after the entry. One count narrower or wider fails the
  // ROM's per-pitch table.
  Keyable k;
  const std::span<const std::uint8_t, 65536> ram{k.ram};
  k.ram[0x1000] = 0x00;  // two silent blocks, then End+Mute
  k.ram[0x1009] = 0x00;
  k.ram[0x1012] = 0x01;
  k.dsp[0x03] = 0x10;    // V0PITCHH: one stream sample per output sample
  gain(k.dsp, 0) = 0x7F;
  keyOnVoice(k.dsp, ram, 0);  // envelope 0: a held startup, walking from call 8

  // Five countdown calls, two held live calls, then nineteen walked samples:
  // the decoder is still in the second block and the voice still sounds.
  for (int n = 1; n <= 26; ++n) (void)stepDspSample(k.dsp, ram);
  EXPECT_EQ(k.dsp[kEndx] & 0x01, 0);
  ASSERT_GT(k.dsp.voices[0].envelope, 0);

  // The twentieth walked sample: the decoder enters the End+Mute block while
  // the voice's own sample still sounds. ENDX does not move — it belongs to
  // the decoder LEAVING the end block (see the case after this one).
  (void)stepDspSample(k.dsp, ram);
  EXPECT_EQ(k.dsp[kEndx] & 0x01, 0);
  EXPECT_GT(k.dsp.voices[0].envelope, 0);

  // The next sample's check reads the entered block: Release, envelope 0.
  (void)stepDspSample(k.dsp, ram);
  EXPECT_EQ(k.dsp.voices[0].phase, EnvPhase::Release);
  EXPECT_EQ(k.dsp.voices[0].envelope, 0);
}

TEST(BrrHeaderCheck, EndxSetsWhenTheDecoderLeavesTheEndBlock) {
  // ENDX is set when the decoder has decoded an end block through and jumps
  // to the loop address — "when the block is complete and the next block will
  // be that pointed to by the loop pointer" (Anomie 326-328) — not when it
  // enters the block. spc_dsp6 `Order/endx after final brr decode` measures
  // it: a plain block chained to an End+Mute block, keyed at one stream sample
  // per output sample, ENDX read on three consecutive samples from the
  // twenty-sixth after the key-on — clear, set, set, for every voice. The
  // decoder enters the end block at sample 11 (its lead, eight samples ahead
  // of the cursor) and leaves it sixteen samples later, at voice 0's compute
  // in the slot that closes sample 27; the set becomes readable at the
  // voice's S7 slot, which for voice 0 is the third slot of sample 28 — the
  // boundary crossing that keeps the ROM's first read clear.
  Keyable k;
  const std::span<const std::uint8_t, 65536> ram{k.ram};
  k.ram[0x1000] = 0x00;  // a plain block, then End+Mute
  k.ram[0x1009] = 0x01;
  k.dsp[0x03] = 0x10;    // V0PITCHH: one stream sample per output sample
  gain(k.dsp, 0) = 0x7F;
  keyOnVoice(k.dsp, ram, 0);

  for (int n = 1; n <= 27; ++n) {
    (void)stepDspSample(k.dsp, ram);
    EXPECT_EQ(k.dsp[kEndx] & 0x01, 0) << "sample " << n;
  }
  EXPECT_EQ(k.dsp.preparedEndx & 0x01, 0x01);  // staged at the closing slot
  (void)stepDspSample(k.dsp, ram);  // sample 28: readable from its third slot
  EXPECT_EQ(k.dsp[kEndx] & 0x01, 0x01);
  (void)stepDspSample(k.dsp, ram);
  EXPECT_EQ(k.dsp[kEndx] & 0x01, 0x01);
}

TEST(BrrHeaderCheck, AStagedEndxSetBecomesReadableAtTheVoicesS7Slot) {
  // A voice's end-flag set is computed at its S3 slot and reaches the register
  // three slots later, at S7 — voice 0's S3 is the sample's last slot, so its
  // set lands in the following sample's fourth slot (Anomie: ENDX "is updated
  // during voice processing step V7, cycles: 0:2 1:5 …", one ahead of this
  // machine's slot numbering). spc_dsp6 `Order/endx after final brr decode`
  // reads voice 0's bit at the sample boundary and sees the old value: a set
  // readable at the compute itself fails its row. Same setup as the case
  // above; the bit is staged when sample 27 closes.
  Keyable k;
  const std::span<const std::uint8_t, 65536> ram{k.ram};
  k.ram[0x1000] = 0x00;
  k.ram[0x1009] = 0x01;
  k.dsp[0x03] = 0x10;
  gain(k.dsp, 0) = 0x7F;
  keyOnVoice(k.dsp, ram, 0);
  for (int n = 1; n <= 27; ++n) (void)stepDspSample(k.dsp, ram);
  ASSERT_EQ(k.dsp.preparedEndx & 0x01, 0x01);
  ASSERT_EQ(k.dsp.slotCursor, 0);

  for (int slot = 0; slot < 3; ++slot) {
    (void)stepDspCycle(k.dsp, ram);  // T0, T1, T2
    EXPECT_EQ(k.dsp[kEndx] & 0x01, 0) << "after slot T" << slot;
  }
  (void)stepDspCycle(k.dsp, ram);  // T3: voice 0's S7
  EXPECT_EQ(k.dsp[kEndx] & 0x01, 0x01);
  EXPECT_EQ(k.dsp.preparedEndx & 0x01, 0);
}

TEST(Keying, ARestartReadsTheEnvelopeAfterTheConsumingComputesOwnUpdate) {
  // Whether a re-key's startup walks or holds is decided by the level the
  // voice would have carried into the next sample: the consuming compute
  // emits the final pre-key-on sample under the standing envelope, runs the
  // selected mode once more, and only then applies the restart. spc_dsp6
  // `Order/endx after final brr decode` re-keys its sync gadget's voice —
  // sounding at full level — with direct gain restored to 0 on the compute
  // before the poll, and reads the same ENDX timing as a fresh key-on: a held
  // startup. A restart reading the level the sample emitted with walks.
  const auto rekey = [](std::uint8_t gainAtRekey) {
    Keyable k;
    const std::span<const std::uint8_t, 65536> ram{k.ram};
    k.dsp[0x03] = 0x10;
    gain(k.dsp, 0) = 0x7F;  // direct gain, full level
    keyOnVoice(k.dsp, ram, 0);
    for (int n = 1; n <= 12; ++n) (void)stepDspSample(k.dsp, ram);
    EXPECT_EQ(k.dsp.voices[0].envelope, 0x7F0);
    gain(k.dsp, 0) = gainAtRekey;
    k.dsp.internalKon = 0x01;
    for (int n = 1; n <= 4; ++n) (void)stepDspSample(k.dsp, ram);
    EXPECT_FALSE(k.dsp.voices[0].restartPending);
    EXPECT_EQ(k.dsp.voices[0].konDelay, 1);  // the restart applied, countdown running
    return k.dsp.voices[0].startupWalks;
  };
  EXPECT_FALSE(rekey(0x00));  // level 0 by the restart: the startup holds
  EXPECT_TRUE(rekey(0x7F));   // level still standing: it walks
}

}  // namespace
