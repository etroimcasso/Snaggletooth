// The end-to-end render path: a loaded .spc image driven through the machine into
// WAV bytes. The machine is deterministic, so the same image and the same budget
// produce identical bytes, and a request for N samples yields exactly N frames.

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "snaggletooth/apu/apu.h"
#include "snaggletooth/apu/dsp.h"
#include "spc/spc_loader.h"
#include "spc/wav_writer.h"

namespace {

using snaggletooth::Apu;
using snaggletooth::ApuState;
using snaggletooth::StereoFrame;
using snaggletooth::spc::parseSpc;
using snaggletooth::spc::writeWav;

// A valid, machine-complete .spc image with zeroed RAM and DSP registers. The
// zeroed RAM runs the CPU through NOPs and keeps every voice keyed off, so the DSP
// output is deterministic silence — enough to pin the determinism and frame-count
// contracts without any audio content.
std::vector<std::uint8_t> baseImage() {
  std::vector<std::uint8_t> img(0x10200, 0);
  constexpr std::string_view magic = "SNES-SPC700 Sound File Data v0.30";
  for (std::size_t i = 0; i < magic.size(); ++i)
    img[i] = static_cast<std::uint8_t>(magic[i]);
  img[0x21] = 26;
  img[0x22] = 26;
  return img;
}

// Renders `samples` frames from a restored machine, truncating any overshoot the
// final instruction produces past the boundary — the same sequence the CLI runs.
std::vector<StereoFrame> render(const ApuState& state, std::uint64_t samples) {
  Apu apu{state};
  apu.run(samples * 32);
  std::vector<StereoFrame> frames = apu.takeFrames();
  if (frames.size() > samples) frames.resize(samples);
  return frames;
}

TEST(SpcRender, SameImageAndBudgetProduceIdenticalBytes) {
  const std::vector<std::uint8_t> img = baseImage();
  const ApuState a = parseSpc(img).state.value();
  const ApuState b = parseSpc(img).state.value();
  EXPECT_EQ(writeWav(render(a, 128), 32000), writeWav(render(b, 128), 32000));
}

TEST(SpcRender, ProducesExactlyTheRequestedSampleCount) {
  const ApuState st = parseSpc(baseImage()).state.value();
  const std::vector<StereoFrame> frames = render(st, 200);
  EXPECT_EQ(frames.size(), 200u);
  EXPECT_EQ(writeWav(frames, 32000).size(), 44u + 200u * 4u);
}

TEST(SpcRender, ZeroSamplesProduceABareHeader) {
  const ApuState st = parseSpc(baseImage()).state.value();
  EXPECT_EQ(writeWav(render(st, 0), 32000).size(), 44u);
}

}  // namespace
