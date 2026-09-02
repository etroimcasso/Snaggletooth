// Where the BRR filter's history lives: one pair per voice, not one pair shared
// by the decoder.
//
// A BRR block is reconstructed from the two samples decoded before it, so the
// filter's history is part of a voice's own stream. No sub-test in the staged
// test ROM distinguishes a per-voice pair from a single shared one — none of
// them ever has two voices decoding through a non-Direct filter in the same
// sample, which is the only condition that tells the two apart. These cases pin
// it directly: a second voice decoding alongside the first must not reach into
// the first's reconstruction.
//
// Filter 0 is deliberately not used here. It ignores the history entirely
// (fullsnes, "SNES APU DSP BRR Samples"), so a filter-0 stream would pass under
// either shape and pin nothing.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "snaggletooth/apu/dsp.h"

namespace {

using snaggletooth::cpuWriteDspRegister;
using snaggletooth::DspState;
using snaggletooth::stepDspSample;

using Ram = std::array<std::uint8_t, 65536>;

constexpr std::uint8_t kKon = 0x4C;
constexpr std::uint8_t kDir = 0x5D;

// Shift 8, filter 1, loop+end set so the stream repeats instead of muting.
constexpr std::uint8_t kFilter1Looping = 0x87;

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

void setPitch(DspState& dsp, std::size_t voice, std::uint16_t pitch) {
  dsp[voice * 0x10 + 0x02] = static_cast<std::uint8_t>(pitch & 0xFF);
  dsp[voice * 0x10 + 0x03] = static_cast<std::uint8_t>(pitch >> 8);
}

std::uint8_t outx(const DspState& dsp, std::size_t v) {
  return dsp[v * 0x10 + 0x09];
}

// Two sources with different content, each decoded through filter 1 so the
// history is load-bearing on every block after the first.
constexpr std::array<std::uint8_t, 8> kDataA = {0x12, 0x34, 0x56, 0x78,
                                                0x12, 0x34, 0x56, 0x78};
constexpr std::array<std::uint8_t, 8> kDataB = {0xFE, 0xDC, 0xBA, 0x98,
                                                0xFE, 0xDC, 0xBA, 0x98};

// Voice 0 always plays source A from 1000h; voice 1, when `withSecondVoice`,
// plays source B from 2000h. Both key on together at unity pitch with a
// constant Direct-Gain envelope, so both decode in the same samples.
struct Scene {
  DspState dsp;
  Ram ram{};

  explicit Scene(bool withSecondVoice) {
    writeDirectoryEntry(ram, 0x02, 0, 0x1000, 0x1000);
    writeBlock(ram, 0x1000, kFilter1Looping, kDataA);
    writeDirectoryEntry(ram, 0x02, 1, 0x2000, 0x2000);
    writeBlock(ram, 0x2000, kFilter1Looping, kDataB);
    dsp[kDir] = 0x02;

    dsp[0x0C] = 0x7F;  // MVOLL
    dsp[0x1C] = 0x7F;  // MVOLR
    for (std::size_t v = 0; v < (withSecondVoice ? 2u : 1u); ++v) {
      dsp[v * 0x10 + 0x04] = static_cast<std::uint8_t>(v);  // VxSRCN
      dsp[v * 0x10 + 0x07] = 0x7F;                          // Direct Gain, constant level
      dsp[v * 0x10 + 0x00] = 0x40;                          // VxVOLL unity
      dsp[v * 0x10 + 0x01] = 0x40;                          // VxVOLR unity
      setPitch(dsp, v, 0x1000);                             // one stream sample per output sample
    }
    // Through the CPU write path, not straight into the register array: a key-on
    // is latched and polled, so a bare array store arms nothing.
    cpuWriteDspRegister(dsp, kKon, withSecondVoice ? 0x03 : 0x01);
  }

  // Runs `samples` output samples, returning voice 0's VxOUTX after each — the
  // per-voice amplitude before the mix, so the observation is voice 0's own
  // reconstruction and not the summed frame.
  std::vector<std::uint8_t> runCapturingVoice0(int samples) {
    std::vector<std::uint8_t> seen;
    seen.reserve(static_cast<std::size_t>(samples));
    for (int i = 0; i < samples; ++i) {
      // The mixed frame is not the observation here: VxOUTX is per voice, so it
      // reports voice 0's own reconstruction rather than a sum a second voice
      // would change for reasons that have nothing to do with the filter.
      const snaggletooth::StereoFrame frame =
          stepDspSample(dsp, std::span<const std::uint8_t, 65536>{ram});
      (void)frame;
      seen.push_back(outx(dsp, 0));
    }
    return seen;
  }
};

// Enough samples to carry both voices past their first block, so the filter is
// running on history it decoded rather than on the zeros a key-on leaves.
constexpr int kSamples = 48;

TEST(FilterHistory, ASecondVoiceDecodingAlongsideDoesNotAlterTheFirstsStream) {
  // The pin. One shared pair would hand voice 0 the tail of voice 1's stream on
  // every block boundary, so voice 0's own output would depend on whether voice
  // 1 happens to be playing. It does not.
  const std::vector<std::uint8_t> alone = Scene{false}.runCapturingVoice0(kSamples);
  const std::vector<std::uint8_t> together = Scene{true}.runCapturingVoice0(kSamples);

  ASSERT_EQ(alone.size(), together.size());
  EXPECT_EQ(alone, together);
}

TEST(FilterHistory, TheStreamUsedForThePinActuallyExercisesTheFilter) {
  // Guards the case above from passing vacuously: a silent or constant voice 0
  // would compare equal under either shape. The stream has to move.
  const std::vector<std::uint8_t> alone = Scene{false}.runCapturingVoice0(kSamples);
  int distinct = 0;
  for (std::size_t i = 1; i < alone.size(); ++i)
    if (alone[i] != alone[i - 1]) ++distinct;
  EXPECT_GT(distinct, 0) << "voice 0 produced a flat stream; the pin proves nothing";
}

TEST(FilterHistory, EachVoiceCarriesItsOwnPairWhileBothDecode) {
  // The same claim read from the state rather than the output: two voices
  // decoding different content in the same samples hold different histories.
  // A single shared pair forces these equal by construction.
  Scene scene{true};
  scene.runCapturingVoice0(kSamples);

  const auto& a = scene.dsp.voices[0];
  const auto& b = scene.dsp.voices[1];
  EXPECT_TRUE(a.decodePrev1 != b.decodePrev1 || a.decodePrev2 != b.decodePrev2)
      << "both voices hold the same filter history after decoding different sources";
}

}  // namespace
