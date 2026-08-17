// The volume envelope: the global counter, the ADSR phase machine, the GAIN
// modes, release, and the KON/KOFF keying with its five-sample startup.
//
// Every expected value is hand-derived from fullsnes's "SNES APU DSP ADSR/Gain
// Envelope" section (the primary contract): the ADSR fields and steps at lines
// 2899-2913, the GAIN modes at 2931-2937, the Bent-Increase clipped reference at
// 2992-2995, ENVX at 2942-2948, and the KON/KOFF registers at 3044-3066. Anomie's
// S-DSP doc is the cross-check and the carrier of the exact counter scheme (the
// Modulus[]/Offset[] tables and the (counter+offset)%period fire rule, line 230)
// and the phase-transition thresholds: the Attack->Decay switch fires when the
// value exceeds 0x7FF (an attack reaching 0x800 clips to 0x7FF), and
// Decay->Sustain fires when the level's upper 3 bits reach the sustain level.

#include <array>
#include <cstddef>
#include <cstdint>

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

TEST(Gain, BentIncreaseUsesTheClippedNegativeReference) {
  // "a negative value for the new value will result in the clipped version
  // being greater than 0x600" (fullsnes lines 2992-2995): a Linear Decrease
  // past 0 saves a reference of (value & 0x7FF) > 0x600, so the following
  // Bent-Increase sample takes the +8 branch even though the level is 0.
  DspState dsp;
  adsr1(dsp, 0) = 0x00;
  gain(dsp, 0) = 0x90;  // Linear Decrease, rate 16
  placeEnvelope(dsp, 0, EnvPhase::Sustain, 0x10);
  EXPECT_EQ(stepVoiceEnvelope(dsp, 0, false), 0);         // 0x10 - 32 clamps to 0
  EXPECT_EQ(dsp.voices[0].bentGainRef, 0x7F0);            // (-16) & 0x7FF
  gain(dsp, 0) = 0xF0;  // switch to Bent Increase
  EXPECT_EQ(stepVoiceEnvelope(dsp, 0, false), 8);         // +8, not +32, at level 0
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
  // Between fires the level, phase and reference are untouched. Rate 1 fires
  // only at counter % 2048 == 0; counter 1 does not fire.
  DspState dsp;
  adsr1(dsp, 0) = 0x80;  // attack index 0 -> rate 1
  placeEnvelope(dsp, 0, EnvPhase::Attack, 0x40);
  dsp.globalCounter = 1;
  EXPECT_EQ(stepVoiceEnvelope(dsp, 0, false), 0x40);
  EXPECT_EQ(dsp.voices[0].phase, EnvPhase::Attack);
  EXPECT_EQ(envx(dsp, 0), 0x04);
}

// ── Keying ──────────────────────────────────────────────────────────────────

TEST(Keying, KeyOnEntersAttackWithTheStartupCountdown) {
  // "there are 5 'empty' samples before envelope updates and BRR decoding
  // actually begin" (fullsnes lines 3053-3054); KON sets the state to Attack
  // and the envelope to 0 (3050-3051), and clears the voice's ENDX bit.
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

TEST(Keying, FiveEmptySamplesPrecedeTheFirstAttackStep) {
  // The startup outputs five zero samples before the envelope moves.
  Keyable k;
  adsr1(k.dsp, 0) = 0x80;  // attack index 0, fires at counter 0
  keyOnVoice(k.dsp, k.ram, 0);
  for (int n = 0; n < 5; ++n) {
    EXPECT_EQ(stepVoiceEnvelope(k.dsp, 0, false), 0) << "startup sample " << n;
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
  EXPECT_EQ(k.dsp.voices[0].phase, EnvPhase::Attack);
  EXPECT_EQ(k.dsp.voices[0].konDelay, 5);
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

  // Two samples of startup, then the next poll with the register still set.
  stepVoiceEnvelope(k.dsp, 0, false);
  stepVoiceEnvelope(k.dsp, 0, false);
  EXPECT_EQ(k.dsp.voices[0].konDelay, 3);

  k.dsp.sampleIndex = 2;
  pollKeying(k.dsp, k.ram);
  EXPECT_EQ(k.dsp.voices[0].konDelay, 3);  // not re-keyed back to 5

  // The startup runs out and the envelope leaves 0.
  for (int n = 0; n < 3; ++n) EXPECT_EQ(stepVoiceEnvelope(k.dsp, 0, false), 0);
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

}  // namespace
