// The voice streaming interlock and 4-point Gaussian interpolation.
//
// Every expected value is hand-derived from fullsnes's "SNES APU DSP BRR
// Pitch" section (the primary contract): the pitch counter at lines 2785-2797,
// the interpolation kernel at 2805-2814, the table at 2816-2849, its overflow
// notes at 2851-2861, and the ENDX start-of-block rule at 3083-3094 — with
// Anomie's S-DSP doc as the cross-check where both speak (the key-on startup
// list pins the window alignment: at position p the newest tap is stream
// sample p+3). Line references below are to the staged fullsnes text. The
// synthetic BRR streams are authored inline: byte 0 of a block is the header
// (shift 7-4, filter 3-2, loop/end 1-0), bytes 1-8 each hold two signed
// nibbles, high nibble first.

#include <array>
#include <cstddef>
#include <cstdint>

#include <gtest/gtest.h>

#include "snaggletooth/apu/dsp.h"

namespace {

using snaggletooth::BrrBlock;
using snaggletooth::decodeBrrBlock;
using snaggletooth::DspState;
using snaggletooth::gaussInterpolate;
using snaggletooth::interpolatedSample;
using snaggletooth::SampleWindow;
using snaggletooth::startVoice;
using snaggletooth::stepVoice;

using Ram = std::array<std::uint8_t, 65536>;

constexpr std::uint8_t kDir = 0x5D;
constexpr std::uint8_t kEndx = 0x7C;

void writeBlock(Ram& ram, std::uint16_t address, std::uint8_t header,
                std::array<std::uint8_t, 8> data) {
  ram[address] = header;
  for (std::size_t i = 0; i < 8; ++i) {
    ram[static_cast<std::uint16_t>(address + 1 + i)] = data[i];
  }
}

// Writes the four-byte directory entry for `srcn` under directory page `dir`
// (the table lives at dir*100h, entries are start then loop, little-endian).
void writeDirectoryEntry(Ram& ram, std::uint8_t dir, std::uint8_t srcn,
                         std::uint16_t start, std::uint16_t loop) {
  const std::uint16_t base = static_cast<std::uint16_t>(dir * 0x100 + srcn * 4);
  ram[base] = static_cast<std::uint8_t>(start & 0xFF);
  ram[static_cast<std::uint16_t>(base + 1)] = static_cast<std::uint8_t>(start >> 8);
  ram[static_cast<std::uint16_t>(base + 2)] = static_cast<std::uint8_t>(loop & 0xFF);
  ram[static_cast<std::uint16_t>(base + 3)] = static_cast<std::uint8_t>(loop >> 8);
}

void setPitch(DspState& dsp, std::size_t voice, std::uint16_t pitch) {
  dsp[voice * 0x10 + 0x02] = static_cast<std::uint8_t>(pitch & 0xFF);
  dsp[voice * 0x10 + 0x03] = static_cast<std::uint8_t>(pitch >> 8);
}

void expectWindow(const SampleWindow& window, int newest, int old, int older,
                  int oldest) {
  EXPECT_EQ(window.newest, newest);
  EXPECT_EQ(window.old, old);
  EXPECT_EQ(window.older, older);
  EXPECT_EQ(window.oldest, oldest);
}

// A shift-12 filter-0 ramp block: nibbles 0,1,2,..,7 twice, so stream sample k
// is (k mod 8) * 2048 (line 2718: sample = (nibble SHL 12) SAR 1).
constexpr std::array<std::uint8_t, 8> kRampData = {0x01, 0x23, 0x45, 0x67,
                                                   0x01, 0x23, 0x45, 0x67};
constexpr int ramp(int k) { return (k % 8) * 2048; }

// A fixture with the ramp block at 1000h as voice 0's source (directory page
// 02h, srcn 0), unity pitch unless a test overrides it.
struct Stream {
  DspState dsp;
  Ram ram{};

  explicit Stream(std::uint16_t loop = 0x1000) {
    writeDirectoryEntry(ram, 0x02, 0, 0x1000, loop);
    writeBlock(ram, 0x1000, 0xC0, kRampData);
    dsp[kDir] = 0x02;
    setPitch(dsp, 0, 0x1000);
  }
};

// ── The Gaussian kernel and its transcribed table ───────────────────────────
//
// A window holding 800h in exactly one tap isolates one table entry: the tap
// is (gauss * 800h) SAR 10 = gauss * 2, the other taps contribute zero, and
// the final SAR 1 (line 2814) halves it back — the kernel returns the entry
// itself.

TEST(Gauss, CornerCoefficientsMatchTheDocumentedTable) {
  // Table corners and two mid-rows, from the block at lines 2818-2849: the
  // newest tap reads gauss[000h+i], old gauss[100h+i], older gauss[1FFh-i],
  // oldest gauss[0FFh-i] (lines 2810-2813).
  EXPECT_EQ(gaussInterpolate({.newest = 0x800}, 0x00), 0x000);  // gauss[000h]
  EXPECT_EQ(gaussInterpolate({.newest = 0x800}, 0xFF), 0x172);  // gauss[0FFh]
  EXPECT_EQ(gaussInterpolate({.newest = 0x800}, 0x80), 0x03A);  // gauss[080h]
  EXPECT_EQ(gaussInterpolate({.old = 0x800}, 0x00), 0x176);     // gauss[100h]
  EXPECT_EQ(gaussInterpolate({.old = 0x800}, 0xFF), 0x519);     // gauss[1FFh]
  EXPECT_EQ(gaussInterpolate({.old = 0x800}, 0x80), 0x3C9);     // gauss[180h]
  EXPECT_EQ(gaussInterpolate({.older = 0x800}, 0x00), 0x519);   // gauss[1FFh]
  EXPECT_EQ(gaussInterpolate({.older = 0x800}, 0xFF), 0x176);   // gauss[100h]
  EXPECT_EQ(gaussInterpolate({.oldest = 0x800}, 0x00), 0x172);  // gauss[0FFh]
  EXPECT_EQ(gaussInterpolate({.oldest = 0x800}, 0xFF), 0x000);  // gauss[000h]
}

TEST(Gauss, FourTapSumsStayInTheDocumentedBand) {
  // "Theoretically, each four values ... should sum up to 800h, but in
  // practice they do sum up to 7FFh..801h" (lines 2851-2853). A window of all
  // 800h returns the four-tap sum directly. A table regenerated from the
  // theoretical curve would sum to a constant 800h — the hardware's does not.
  const SampleWindow all{.newest = 0x800, .old = 0x800, .older = 0x800, .oldest = 0x800};
  bool sawHigh = false;
  bool sawOffCenter = false;
  for (int i = 0; i <= 0xFF; ++i) {
    const int sum = gaussInterpolate(all, static_cast<std::uint8_t>(i));
    EXPECT_GE(sum, 0x7FF) << "index " << i;
    EXPECT_LE(sum, 0x801) << "index " << i;
    sawHigh = sawHigh || sum == 0x801;
    sawOffCenter = sawOffCenter || sum != 0x800;
  }
  EXPECT_TRUE(sawHigh);       // "801h can cause math overflows" (line 2853)
  EXPECT_TRUE(sawOffCenter);  // the documented deviation from the curve
}

TEST(Gauss, SecondAdditionWrapsIntoThePositiveGlitch) {
  // "when outputting three or more '-8 SHL 12' BRR samples with Filter 0,
  // some interpolation results will be +3FF8h (instead of -4000h)" (lines
  // 2853-2856). At index 0 the weights sum to 801h and the newest tap's
  // weight is 0: the running sum reaches -8010h at the second addition, which
  // has no overflow handling (line 2812) and wraps to +7FF0h; SAR 1 gives
  // +3FF8h.
  const SampleWindow allMin{
      .newest = -0x4000, .old = -0x4000, .older = -0x4000, .oldest = -0x4000};
  EXPECT_EQ(gaussInterpolate(allMin, 0x00), 0x3FF8);
}

TEST(Gauss, ThirdAdditionSaturatesInsteadOfWrapping) {
  // "the 3rd addition can overflow (when i=20h..FFh) ... the 3rd one is
  // saturated to Min=-8000h/Max=+7FFFh (giving -4000h/+3FFFh after the final
  // SAR 1)" (lines 2857-2861). Over that index range an all-minimum window
  // lands on -4000h or -3FF8h — never the wrapped positive the second
  // addition would produce.
  const SampleWindow allMin{
      .newest = -0x4000, .old = -0x4000, .older = -0x4000, .oldest = -0x4000};
  for (int i = 0x20; i <= 0xFF; ++i) {
    const int out = gaussInterpolate(allMin, static_cast<std::uint8_t>(i));
    EXPECT_TRUE(out == -0x4000 || out == -0x3FF8) << "index " << i << " gave " << out;
  }
}

TEST(Gauss, NewestTapIsSilentAtIndexZero) {
  // gauss[000h] = 0, so at index 0 the newest sample cannot reach the output
  // — the window really is oriented with the newest tap on gauss[000h+i].
  const SampleWindow quiet{.old = 1000, .older = 2000, .oldest = 3000};
  SampleWindow loud = quiet;
  loud.newest = 0x3FFF;
  EXPECT_EQ(gaussInterpolate(quiet, 0x00), gaussInterpolate(loud, 0x00));
}

TEST(Gauss, OldestTapIsSilentAtIndexMax) {
  // At index FFh the oldest tap's weight is gauss[0FFh-FFh] = gauss[0] = 0.
  const SampleWindow quiet{.newest = 1000, .old = 2000, .older = 3000};
  SampleWindow loud = quiet;
  loud.oldest = 0x3FFF;
  EXPECT_EQ(gaussInterpolate(quiet, 0xFF), gaussInterpolate(loud, 0xFF));
}

// ── The pitch counter and the streaming interlock ───────────────────────────

TEST(VoiceStream, StartVoicePrimesTheWindowFromTheSampleStart) {
  // startVoice reads the directory entry (lines 2685-2692) and decodes the
  // first four samples, so at position 0 the newest tap is stream sample 3 —
  // the alignment of Anomie's key-on startup list, where the first output at
  // interpolation position 0 reads samples 0..3.
  Stream s;
  startVoice(s.dsp, s.ram, 0);
  expectWindow(s.dsp.voices[0].window, ramp(3), ramp(2), ramp(1), ramp(0));
  EXPECT_EQ(s.dsp.voices[0].pitchCounter, 0);
  EXPECT_EQ(s.dsp.voices[0].brrAddress, 0x1000);
  EXPECT_EQ(s.dsp.voices[0].brrSampleIndex, 4);
  EXPECT_EQ(s.dsp[kEndx], 0);  // the ramp block carries no end flag
}

TEST(VoiceStream, UnityPitchAdvancesOneSamplePerStep) {
  // Step = VxPitch, Counter = Counter + Step (lines 2788, 2794); 1000h is one
  // sample per 32 kHz step (line 2764), so every step shifts one new sample
  // into the window and the interpolation index (Counter bits 11-4) stays 0.
  Stream s;
  startVoice(s.dsp, s.ram, 0);
  const std::int16_t out1 = stepVoice(s.dsp, s.ram, 0);
  expectWindow(s.dsp.voices[0].window, ramp(4), ramp(3), ramp(2), ramp(1));
  EXPECT_EQ(s.dsp.voices[0].pitchCounter, 0x1000);
  // Hand trace at index 0 over {8192,6144,4096,2048} (weights 000/176/519/172,
  // lines 2810-2813): 0 + 6*176h + 4*519h + 2*172h = 200Ch; SAR 1 = 1006h.
  EXPECT_EQ(out1, 0x1006);
  const std::int16_t out2 = stepVoice(s.dsp, s.ram, 0);
  expectWindow(s.dsp.voices[0].window, ramp(5), ramp(4), ramp(3), ramp(2));
  EXPECT_EQ(out2, gaussInterpolate(s.dsp.voices[0].window, 0));
}

TEST(VoiceStream, FractionalPitchAlternatesTheInterpolationIndex) {
  // At pitch 800h the counter gains half a sample per step: the position
  // advances every second step, and the interpolation index (bits 11-4)
  // alternates 80h, 00h (lines 2794-2797).
  Stream s;
  setPitch(s.dsp, 0, 0x0800);
  startVoice(s.dsp, s.ram, 0);
  const std::int16_t outHalf = stepVoice(s.dsp, s.ram, 0);
  expectWindow(s.dsp.voices[0].window, ramp(3), ramp(2), ramp(1), ramp(0));
  EXPECT_EQ(s.dsp.voices[0].pitchCounter, 0x0800);
  // Hand trace at index 80h over {6144,4096,2048,0} (weights 03Ah/3C9h/3C5h/
  // 038h at rows 080h/180h/17Fh/07Fh): 6*3Ah + 4*3C9h + 2*3C5h + 0 = 180Ah;
  // SAR 1 = C05h.
  EXPECT_EQ(outHalf, 0x0C05);
  const std::int16_t outWhole = stepVoice(s.dsp, s.ram, 0);
  expectWindow(s.dsp.voices[0].window, ramp(4), ramp(3), ramp(2), ramp(1));
  EXPECT_EQ(s.dsp.voices[0].pitchCounter, 0x1000);
  EXPECT_EQ(outWhole, gaussInterpolate(s.dsp.voices[0].window, 0x00));
  stepVoice(s.dsp, s.ram, 0);
  EXPECT_EQ(s.dsp.voices[0].pitchCounter, 0x1800);
  EXPECT_EQ(interpolatedSample(s.dsp, 0),
            gaussInterpolate(s.dsp.voices[0].window, 0x80));
}

TEST(VoiceStream, PitchZeroFreezesTheStream) {
  // "0=stop" (line 2764): the counter never moves, no sample is decoded, and
  // the output holds.
  Stream s;
  setPitch(s.dsp, 0, 0x0000);
  startVoice(s.dsp, s.ram, 0);
  const std::int16_t held = interpolatedSample(s.dsp, 0);
  for (int n = 0; n < 3; ++n) {
    EXPECT_EQ(stepVoice(s.dsp, s.ram, 0), held);
  }
  expectWindow(s.dsp.voices[0].window, ramp(3), ramp(2), ramp(1), ramp(0));
  EXPECT_EQ(s.dsp.voices[0].pitchCounter, 0);
}

TEST(VoiceStream, PitchHighBitsAreIgnored) {
  // VxPITCH bits 14-15 are "Not used (read/write-able)" (line 2765): a pitch
  // written as FF00h steps exactly like 3F00h.
  Stream s;
  writeDirectoryEntry(s.ram, 0x02, 1, 0x1000, 0x1000);
  s.dsp[0x14] = 1;  // V1SRCN — same source as voice 0
  setPitch(s.dsp, 0, 0xFF00);
  setPitch(s.dsp, 1, 0x3F00);
  startVoice(s.dsp, s.ram, 0);
  startVoice(s.dsp, s.ram, 1);
  stepVoice(s.dsp, s.ram, 0);
  stepVoice(s.dsp, s.ram, 1);
  EXPECT_EQ(s.dsp.voices[0].pitchCounter, 0x3F00);
  EXPECT_EQ(s.dsp.voices[1].pitchCounter, 0x3F00);
  expectWindow(s.dsp.voices[0].window, s.dsp.voices[1].window.newest,
               s.dsp.voices[1].window.old, s.dsp.voices[1].window.older,
               s.dsp.voices[1].window.oldest);
}

TEST(VoiceStream, MaxPitchPassesSeveralSamplesPerStep) {
  // At pitch 3FFFh (line 2764: the fastest rate, 128 kHz) each step passes
  // three or four sample positions; every one of them is decoded through the
  // filter, so the window stays contiguous with the stream.
  Stream s;
  setPitch(s.dsp, 0, 0x3FFF);
  startVoice(s.dsp, s.ram, 0);
  stepVoice(s.dsp, s.ram, 0);
  EXPECT_EQ(s.dsp.voices[0].pitchCounter, 0x3FFF);
  expectWindow(s.dsp.voices[0].window, ramp(6), ramp(5), ramp(4), ramp(3));
  stepVoice(s.dsp, s.ram, 0);
  EXPECT_EQ(s.dsp.voices[0].pitchCounter, 0x7FFE);
  expectWindow(s.dsp.voices[0].window, ramp(10), ramp(9), ramp(8), ramp(7));
  EXPECT_EQ(interpolatedSample(s.dsp, 0),
            gaussInterpolate(s.dsp.voices[0].window, 0xFF));
}

TEST(VoiceStream, ChainedBlocksCarryTheFilterHistory) {
  // A block without the end flag chains to the following 9 bytes (line 2711),
  // and the filter history flows across the boundary (lines 2745-2746). The
  // streamed samples of a filter-1 second block must equal the block decoder
  // fed the first block's carry-out.
  Stream s;
  writeBlock(s.ram, 0x1009, 0x54, {0x70, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
  const BrrBlock a = decodeBrrBlock(
      std::span<const std::uint8_t, 9>(&s.ram[0x1000], 9), 0, 0);
  const BrrBlock b = decodeBrrBlock(
      std::span<const std::uint8_t, 9>(&s.ram[0x1009], 9), a.last, a.prev);
  startVoice(s.dsp, s.ram, 0);
  for (int n = 1; n <= 12; ++n) stepVoice(s.dsp, s.ram, 0);
  EXPECT_EQ(s.dsp.voices[0].brrAddress, 0x1000);  // still the first block
  stepVoice(s.dsp, s.ram, 0);                     // decodes the next block's sample 0
  EXPECT_EQ(s.dsp.voices[0].brrAddress, 0x1009);
  EXPECT_EQ(s.dsp.voices[0].window.newest, b.samples[0]);
  stepVoice(s.dsp, s.ram, 0);
  EXPECT_EQ(s.dsp.voices[0].window.newest, b.samples[1]);
  EXPECT_EQ(s.dsp.voices[0].window.old, b.samples[0]);
}

TEST(VoiceStream, ARamRewriteAfterKeyOnDoesNotReachThePrimedSamples) {
  // BRR bytes are read ahead of consumption: the key-on primes the first
  // three groups — twelve samples — and later groups are decoded only when
  // the stream requires them (Anomie's V4: decoding a group "is definately
  // not done when not necessary"). So a RAM write after the key-on reaches
  // only samples not yet decoded: spc_dsp6 `Misc/brr not always decoding`
  // rewrites a parked voice's block to silence and the voice still plays all
  // twelve primed samples when it moves, with the rewrite audible from stream
  // sample 12 on. Both bounds matter: one fewer primed sample reddens the
  // sample-11 read, one more reddens the sample-12 read.
  Stream s;
  startVoice(s.dsp, s.ram, 0);
  writeBlock(s.ram, 0x1000, 0xC0, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
  for (int sample = 4; sample <= 11; ++sample) {
    stepVoice(s.dsp, s.ram, 0);
    EXPECT_EQ(s.dsp.voices[0].window.newest, ramp(sample)) << "sample " << sample;
  }
  stepVoice(s.dsp, s.ram, 0);  // sample 12: decoded after the rewrite
  EXPECT_EQ(s.dsp.voices[0].window.newest, 0);  // ramp(12) = 8192 before it
}

TEST(VoiceStream, EndxSetsAtTheStartOfDecodingTheEndBlock) {
  // "the bit is set at the START of decoding the BRR block, not at the end"
  // (lines 3092-3093) — and the DECODER starts a block while the cursor is
  // still eight samples back in the block before (spc_dsp6 `Misc/brr early
  // end at many pitches`). With the end block second, the bit appears when
  // the decoder enters it: the step that consumes the first block's eighth
  // sample, four steps past the primed window.
  Stream s;
  writeBlock(s.ram, 0x1009, 0xC3, {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11});
  startVoice(s.dsp, s.ram, 0);
  for (int n = 1; n <= 3; ++n) stepVoice(s.dsp, s.ram, 0);
  EXPECT_EQ(s.dsp[kEndx], 0);
  stepVoice(s.dsp, s.ram, 0);  // the decoder moves on to the end block
  EXPECT_EQ(s.dsp[kEndx], 0x01);
}

TEST(VoiceStream, AnEndLoopBlockJumpsToTheLoopAddress) {
  // Code 3 = End+Loop: "jump to Loop-address, set ENDx flag" (line 2714). The
  // end block's own sixteen samples still stream — a seamless loop plays its
  // final block — and the block after it is the loop target.
  Stream s(0x2000);
  writeBlock(s.ram, 0x1009, 0xC3, {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11});
  writeBlock(s.ram, 0x2000, 0xC0, {0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22});
  startVoice(s.dsp, s.ram, 0);
  for (int n = 1; n <= 28; ++n) stepVoice(s.dsp, s.ram, 0);
  EXPECT_EQ(s.dsp.voices[0].brrAddress, 0x1009);  // the end block, fully decoded
  EXPECT_EQ(s.dsp.voices[0].window.newest, 2048);  // its nibble-1 samples streamed
  stepVoice(s.dsp, s.ram, 0);
  EXPECT_EQ(s.dsp.voices[0].brrAddress, 0x2000);
  EXPECT_EQ(s.dsp.voices[0].window.newest, 4096);  // the loop target's nibble 2
}

TEST(VoiceStream, AnEndMuteBlockAlsoJumpsToTheLoopAddress) {
  // Code 1 = End+Mute also reads "jump to Loop-address" (line 2712): decoding
  // never stops, the loop-flag difference is envelope behavior (Anomie: "it
  // always loops after reaching a block with 'e' set").
  Stream s(0x2000);
  writeBlock(s.ram, 0x1009, 0xC1, {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11});
  writeBlock(s.ram, 0x2000, 0xC0, {0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22});
  startVoice(s.dsp, s.ram, 0);
  for (int n = 1; n <= 29; ++n) stepVoice(s.dsp, s.ram, 0);
  EXPECT_EQ(s.dsp.voices[0].brrAddress, 0x2000);
  EXPECT_EQ(s.dsp.voices[0].window.newest, 4096);
  EXPECT_EQ(s.dsp[kEndx], 0x01);
}

TEST(VoiceStream, EndxReSetsWhenTheDecoderReachesTheEndAgain) {
  // ENDX re-arms after its acknowledge: "bits may get set ... once when the
  // BRR decoder reaches an End-code" (lines 3087-3090). A self-looping end
  // block sets its bit at the prime, again on every pass — and the decoder
  // reaches it again eight cursor samples in, when it resolves the loop and
  // finds the same end block waiting (spc_dsp6 `Misc/brr early end at many
  // pitches` pins the decoder's lead).
  Ram ram{};
  DspState dsp;
  writeDirectoryEntry(ram, 0x02, 0, 0x1000, 0x1000);
  writeBlock(ram, 0x1000, 0xC3, kRampData);
  dsp[kDir] = 0x02;
  setPitch(dsp, 0, 0x1000);
  startVoice(dsp, ram, 0);
  EXPECT_EQ(dsp[kEndx], 0x01);  // the first block is itself an end block
  dsp[kEndx] = 0;               // the CPU's write-acknowledge cleared it
  for (int n = 1; n <= 3; ++n) stepVoice(dsp, ram, 0);
  EXPECT_EQ(dsp[kEndx], 0);
  stepVoice(dsp, ram, 0);  // the decoder resolves the loop back into the end
  EXPECT_EQ(dsp[kEndx], 0x01);
  EXPECT_EQ(dsp.voices[0].brrAddress, 0x1000);
}

TEST(VoiceStream, DirIsReadLiveAtTheLoopJump) {
  // "Changing DIR or VxSRCN has no immediate effect (until/unless voices are
  // newly Looped or Keyed-ON)" (lines 2694-2695) — so a DIR change redirects
  // the NEXT loop jump and nothing before it.
  Stream s(0x2000);
  writeBlock(s.ram, 0x1009, 0xC3, {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11});
  writeBlock(s.ram, 0x2000, 0xC0, {0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22});
  writeDirectoryEntry(s.ram, 0x03, 0, 0x1000, 0x3000);
  writeBlock(s.ram, 0x3000, 0xC0, {0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33});
  startVoice(s.dsp, s.ram, 0);
  s.dsp[kDir] = 0x03;  // redirect while the voice is mid-stream
  for (int n = 1; n <= 29; ++n) stepVoice(s.dsp, s.ram, 0);
  EXPECT_EQ(s.dsp.voices[0].brrAddress, 0x3000);
  EXPECT_EQ(s.dsp.voices[0].window.newest, 6144);  // the new target's nibble 3
}

TEST(VoiceStream, AStreamOfMinimumSamplesReproducesTheKernelOverflow) {
  // The mandatory glitch, end to end: "-8 SHL 12" samples under Filter 0
  // decode to -4000h each (line 2718), and at interpolation index 0 the
  // kernel wraps to +3FF8h instead of -4000h (lines 2853-2856).
  Ram ram{};
  DspState dsp;
  writeDirectoryEntry(ram, 0x02, 0, 0x1000, 0x1000);
  writeBlock(ram, 0x1000, 0xC3, {0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88});
  dsp[kDir] = 0x02;
  setPitch(dsp, 0, 0x1000);
  startVoice(dsp, ram, 0);
  expectWindow(dsp.voices[0].window, -0x4000, -0x4000, -0x4000, -0x4000);
  EXPECT_EQ(interpolatedSample(dsp, 0), 0x3FF8);
  EXPECT_EQ(stepVoice(dsp, ram, 0), 0x3FF8);  // the loop keeps the stream at minimum
}

TEST(VoiceStream, APitchWriteTakesEffectOnTheNextStep) {
  // The step is read live from VxPITCHL/H each output sample, so a write
  // lands cleanly on the following step — never mid-step.
  Stream s;
  startVoice(s.dsp, s.ram, 0);
  stepVoice(s.dsp, s.ram, 0);
  EXPECT_EQ(s.dsp.voices[0].pitchCounter, 0x1000);
  setPitch(s.dsp, 0, 0x0000);
  stepVoice(s.dsp, s.ram, 0);
  EXPECT_EQ(s.dsp.voices[0].pitchCounter, 0x1000);
  expectWindow(s.dsp.voices[0].window, ramp(4), ramp(3), ramp(2), ramp(1));
}

TEST(VoiceStream, ASnapshotRestoresTheStreamMidBlock) {
  // The voices' streaming state lives in the DspState value, so the
  // whole-machine snapshot law holds mid-stream: copy, diverge, restore by
  // assignment, and the replay is sample-exact.
  Stream s;
  startVoice(s.dsp, s.ram, 0);
  for (int n = 1; n <= 5; ++n) stepVoice(s.dsp, s.ram, 0);
  const DspState snapshot = s.dsp;
  std::array<std::int16_t, 3> first{};
  for (auto& out : first) out = stepVoice(s.dsp, s.ram, 0);
  s.dsp = snapshot;
  std::array<std::int16_t, 3> replay{};
  for (auto& out : replay) out = stepVoice(s.dsp, s.ram, 0);
  EXPECT_EQ(first, replay);
  expectWindow(s.dsp.voices[0].window, ramp(11), ramp(10), ramp(9), ramp(8));
}

}  // namespace
