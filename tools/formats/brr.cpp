#include "brr.h"

#include <string>
#include <vector>

#include "snaggletooth/apu/dsp.h"
#include "spc/wav_writer.h"

namespace snaggletooth::formats {

namespace {

// Header bit 0 is the end flag: the block that stops the sample.
constexpr std::uint8_t kEndFlag = 0x01u;

}  // namespace

Bytes encodeBrrWav(std::span<const std::uint8_t> sample) {
  Bytes out;
  if (sample.empty()) {
    out.error = "a sample holds at least one block of nine bytes";
    return out;
  }
  if (sample.size() % kBrrBlockBytes != 0) {
    out.error = "a sample is a whole number of nine-byte blocks; " + std::to_string(sample.size()) +
                " bytes is not";
    return out;
  }
  const std::size_t blocks = sample.size() / kBrrBlockBytes;
  for (std::size_t i = 0; i + 1 < blocks; ++i) {
    if ((sample[i * kBrrBlockBytes] & kEndFlag) != 0u) {
      out.error = "block " + std::to_string(i) + " carries the end flag before the last block";
      return out;
    }
  }
  if ((sample[(blocks - 1) * kBrrBlockBytes] & kEndFlag) == 0u) {
    out.error = "the last block does not carry the end flag";
    return out;
  }

  std::vector<StereoFrame> frames;
  frames.reserve(blocks * 16u);
  std::int16_t old = 0;
  std::int16_t older = 0;
  for (std::size_t i = 0; i < blocks; ++i) {
    const std::span<const std::uint8_t, kBrrBlockBytes> block =
        sample.subspan(i * kBrrBlockBytes).first<kBrrBlockBytes>();
    const BrrBlock decoded = decodeBrrBlock(block, old, older);
    for (const std::int16_t value : decoded.samples) frames.push_back(StereoFrame{.left = value, .right = value});
    old = decoded.last;
    older = decoded.prev;
  }
  out.bytes = spc::writeWav(frames, kBrrSampleRate);
  return out;
}

}  // namespace snaggletooth::formats
