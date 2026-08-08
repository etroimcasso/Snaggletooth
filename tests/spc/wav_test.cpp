// The WAV writer: the canonical 44-byte PCM header fields and the 16-bit
// little-endian left/right interleave of the frame data.

#include <cstddef>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "snaggletooth/apu/dsp.h"
#include "spc/wav_writer.h"

namespace {

using snaggletooth::StereoFrame;
using snaggletooth::spc::writeWav;

std::uint16_t readU16(const std::vector<std::uint8_t>& b, std::size_t at) {
  return static_cast<std::uint16_t>(b[at] | (b[at + 1] << 8));
}

std::uint32_t readU32(const std::vector<std::uint8_t>& b, std::size_t at) {
  return static_cast<std::uint32_t>(b[at]) |
         (static_cast<std::uint32_t>(b[at + 1]) << 8) |
         (static_cast<std::uint32_t>(b[at + 2]) << 16) |
         (static_cast<std::uint32_t>(b[at + 3]) << 24);
}

bool tagAt(const std::vector<std::uint8_t>& b, std::size_t at, const char* tag) {
  for (std::size_t i = 0; i < 4; ++i)
    if (b[at + i] != static_cast<std::uint8_t>(tag[i])) return false;
  return true;
}

TEST(WavWriter, CarriesTheRiffAndWaveTags) {
  const std::vector<std::uint8_t> wav = writeWav(std::vector<StereoFrame>(4), 32000);
  EXPECT_TRUE(tagAt(wav, 0, "RIFF"));
  EXPECT_TRUE(tagAt(wav, 8, "WAVE"));
}

TEST(WavWriter, DeclaresPcmStereo16InTheFmtChunk) {
  const std::vector<std::uint8_t> wav = writeWav(std::vector<StereoFrame>(1), 32000);
  EXPECT_TRUE(tagAt(wav, 12, "fmt "));
  EXPECT_EQ(readU32(wav, 16), 16u);  // PCM fmt-chunk body size
  EXPECT_EQ(readU16(wav, 20), 1u);   // audio format: PCM
  EXPECT_EQ(readU16(wav, 22), 2u);   // channels
  EXPECT_EQ(readU16(wav, 34), 16u);  // bits per sample
}

TEST(WavWriter, WritesTheSampleRateAndItsDerivedFields) {
  const std::vector<std::uint8_t> wav = writeWav(std::vector<StereoFrame>(1), 32000);
  EXPECT_EQ(readU32(wav, 24), 32000u);       // sample rate
  EXPECT_EQ(readU32(wav, 28), 32000u * 4u);  // byte rate = rate * block align
  EXPECT_EQ(readU16(wav, 32), 4u);           // block align = channels * 2 bytes
}

TEST(WavWriter, SizesTheDataAndRiffChunksToTheFrameCount) {
  const std::vector<std::uint8_t> wav = writeWav(std::vector<StereoFrame>(10), 32000);
  EXPECT_TRUE(tagAt(wav, 36, "data"));
  EXPECT_EQ(readU32(wav, 40), 10u * 4u);       // data body bytes
  EXPECT_EQ(readU32(wav, 4), 36u + 10u * 4u);  // RIFF size = 36 + data body
  EXPECT_EQ(wav.size(), 44u + 10u * 4u);       // whole file
}

TEST(WavWriter, InterleavesLeftThenRightAsLittleEndian16Bit) {
  const std::vector<StereoFrame> frames{StereoFrame{.left = 0x1234, .right = -1}};
  const std::vector<std::uint8_t> wav = writeWav(frames, 32000);
  EXPECT_EQ(wav[44], 0x34);  // left low byte
  EXPECT_EQ(wav[45], 0x12);  // left high byte
  EXPECT_EQ(wav[46], 0xFF);  // right (-1) low byte
  EXPECT_EQ(wav[47], 0xFF);  // right high byte
}

TEST(WavWriter, EmptyInputGivesABareHeader) {
  const std::vector<std::uint8_t> wav = writeWav({}, 32000);
  EXPECT_EQ(wav.size(), 44u);
  EXPECT_EQ(readU32(wav, 40), 0u);  // zero-length data chunk
}

}  // namespace
