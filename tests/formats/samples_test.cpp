// The listening copy: a BRR sample's blocks decoded through the machine's own
// decoder into a WAV at the DSP's rate, the filter history carried block to
// block, the loop not followed, and bytes that are not a sample refused with the
// reason.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "formats/brr.h"
#include "gtest/gtest.h"
#include "snaggletooth/apu/dsp.h"

namespace snaggletooth::formats {
namespace {

std::uint16_t readU16(const std::vector<std::uint8_t>& b, std::size_t at) {
  return static_cast<std::uint16_t>(b[at] | (b[at + 1] << 8));
}

std::uint32_t readU32(const std::vector<std::uint8_t>& b, std::size_t at) {
  return static_cast<std::uint32_t>(b[at]) | (static_cast<std::uint32_t>(b[at + 1]) << 8) |
         (static_cast<std::uint32_t>(b[at + 2]) << 16) | (static_cast<std::uint32_t>(b[at + 3]) << 24);
}

std::int16_t frameAt(const std::vector<std::uint8_t>& wav, std::size_t frame, bool right) {
  const std::size_t at = 44u + frame * 4u + (right ? 2u : 0u);
  return static_cast<std::int16_t>(readU16(wav, at));
}

// Two blocks, shift 11 and filter 0, the second carrying the end and loop flags.
const std::vector<std::uint8_t> kTwoBlocks = {0xB0, 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0,
                                              0xB3, 0x0F, 0xED, 0xCB, 0xA9, 0x87, 0x65, 0x43, 0x21};

}  // namespace

TEST(Samples, ATwoBlockSampleIsThirtyTwoFramesAtTheDspsRate) {
  const Bytes wav = encodeBrrWav(kTwoBlocks);
  ASSERT_TRUE(wav.ok()) << wav.error;
  ASSERT_EQ(wav.bytes.size(), 44u + 32u * 4u);
  EXPECT_EQ(readU16(wav.bytes, 22), 2u) << "two channels";
  EXPECT_EQ(readU32(wav.bytes, 24), 32000u);
  EXPECT_EQ(readU16(wav.bytes, 34), 16u) << "bits a sample";
  EXPECT_EQ(readU32(wav.bytes, 40), 32u * 4u) << "the data chunk";
}

TEST(Samples, EachFrameIsTheDecodersValueOnBothChannels) {
  const Bytes wav = encodeBrrWav(kTwoBlocks);
  ASSERT_TRUE(wav.ok()) << wav.error;
  const BrrBlock first = decodeBrrBlock(std::span<const std::uint8_t, 9>(kTwoBlocks.data(), 9), 0, 0);
  const BrrBlock second = decodeBrrBlock(std::span<const std::uint8_t, 9>(kTwoBlocks.data() + 9, 9), first.last, first.prev);
  for (std::size_t i = 0; i < 16; ++i) {
    EXPECT_EQ(frameAt(wav.bytes, i, false), first.samples[i]) << i;
    EXPECT_EQ(frameAt(wav.bytes, i, true), first.samples[i]) << i;
    EXPECT_EQ(frameAt(wav.bytes, 16u + i, false), second.samples[i]) << i;
    EXPECT_EQ(frameAt(wav.bytes, 16u + i, true), second.samples[i]) << i;
  }
  EXPECT_NE(first.samples[0], 0) << "shift 11 makes the first nibble audible";
}

TEST(Samples, TheFilterHistoryCarriesIntoTheNextBlock) {
  // The second block under filter 2 reads the first block's last two outputs —
  // its last is zero, its second to last is not — so decoded from a zero
  // history it would differ.
  std::vector<std::uint8_t> blocks = kTwoBlocks;
  blocks[9] = 0xBB;  // shift 11, filter 2, end and loop
  const Bytes wav = encodeBrrWav(blocks);
  ASSERT_TRUE(wav.ok()) << wav.error;
  const BrrBlock first = decodeBrrBlock(std::span<const std::uint8_t, 9>(blocks.data(), 9), 0, 0);
  const BrrBlock carried = decodeBrrBlock(std::span<const std::uint8_t, 9>(blocks.data() + 9, 9), first.last, first.prev);
  const BrrBlock fresh = decodeBrrBlock(std::span<const std::uint8_t, 9>(blocks.data() + 9, 9), 0, 0);
  ASSERT_NE(carried.samples, fresh.samples);
  for (std::size_t i = 0; i < 16; ++i) EXPECT_EQ(frameAt(wav.bytes, 16u + i, false), carried.samples[i]) << i;
}

TEST(Samples, TheLoopIsNotFollowed) {
  // A looping sample is written once, start to end: two blocks, thirty-two
  // frames, however the loop points.
  const Bytes wav = encodeBrrWav(kTwoBlocks);
  ASSERT_TRUE(wav.ok()) << wav.error;
  EXPECT_EQ((wav.bytes.size() - 44u) / 4u, 32u);
}

TEST(Samples, BytesThatAreNotWholeBlocksAreRefused) {
  std::vector<std::uint8_t> blocks = kTwoBlocks;
  blocks.pop_back();
  const Bytes wav = encodeBrrWav(blocks);
  EXPECT_FALSE(wav.ok());
  EXPECT_NE(wav.error.find("whole number of nine-byte blocks"), std::string::npos) << wav.error;
  EXPECT_NE(wav.error.find("17 bytes"), std::string::npos) << wav.error;
  EXPECT_TRUE(wav.bytes.empty());
}

TEST(Samples, NoBlocksIsRefused) {
  const Bytes wav = encodeBrrWav(std::vector<std::uint8_t>{});
  EXPECT_FALSE(wav.ok());
  EXPECT_NE(wav.error.find("at least one block"), std::string::npos) << wav.error;
}

TEST(Samples, AnEndFlagBeforeTheLastBlockIsRefused) {
  std::vector<std::uint8_t> blocks = kTwoBlocks;
  blocks[0] = 0xB1;  // the first block ends the sample
  const Bytes wav = encodeBrrWav(blocks);
  EXPECT_FALSE(wav.ok());
  EXPECT_NE(wav.error.find("block 0 carries the end flag before the last block"), std::string::npos) << wav.error;
}

TEST(Samples, ALastBlockWithoutTheEndFlagIsRefused) {
  std::vector<std::uint8_t> blocks = kTwoBlocks;
  blocks[9] = 0xB2;  // loop without end
  const Bytes wav = encodeBrrWav(blocks);
  EXPECT_FALSE(wav.ok());
  EXPECT_NE(wav.error.find("the last block does not carry the end flag"), std::string::npos) << wav.error;
}

}  // namespace snaggletooth::formats
