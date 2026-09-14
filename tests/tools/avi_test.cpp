// The video writer's file: the chunks an uncompressed AVI is made of, the exact
// bytes of its headers, the rows stored from the bottom up as blue, green and red
// with each row run out to four bytes, the index that says where every picture is,
// and the sizes the recording patches in as it closes. Every expectation is computed
// by hand from the format; a reader written here takes the pictures back out.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "snaggletooth/snes/video_frame.h"
#include "video/avi_writer.h"

namespace snaggletooth::video {
namespace {

// A directory of this case's own, removed when the case ends.
class Recording : public ::testing::Test {
 protected:
  void SetUp() override {
    directory_ = std::filesystem::temp_directory_path() /
                 ("snaggletooth-avi-" + std::to_string(::testing::UnitTest::GetInstance()
                                                           ->current_test_info()
                                                           ->line()));
    std::filesystem::remove_all(directory_);
    std::filesystem::create_directories(directory_);
  }
  void TearDown() override { std::filesystem::remove_all(directory_); }

  [[nodiscard]] std::filesystem::path file() const { return directory_ / "capture.avi"; }

  std::filesystem::path directory_;
};

// The whole file as bytes.
[[nodiscard]] std::vector<std::uint8_t> readFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in),
                                   std::istreambuf_iterator<char>());
}

[[nodiscard]] std::string tagAt(const std::vector<std::uint8_t>& bytes, std::size_t at) {
  return std::string(bytes.begin() + static_cast<std::ptrdiff_t>(at),
                     bytes.begin() + static_cast<std::ptrdiff_t>(at + 4u));
}

[[nodiscard]] std::uint32_t u32At(const std::vector<std::uint8_t>& bytes, std::size_t at) {
  return static_cast<std::uint32_t>(bytes.at(at)) |
         (static_cast<std::uint32_t>(bytes.at(at + 1u)) << 8) |
         (static_cast<std::uint32_t>(bytes.at(at + 2u)) << 16) |
         (static_cast<std::uint32_t>(bytes.at(at + 3u)) << 24);
}

[[nodiscard]] std::uint16_t u16At(const std::vector<std::uint8_t>& bytes, std::size_t at) {
  return static_cast<std::uint16_t>(bytes.at(at) | (bytes.at(at + 1u) << 8));
}

// The addresses the format puts each field at, counted by hand: the RIFF size, the
// header list, the main header, the stream list with its header and format, and the
// movie list's tag at the end of a fixed 224 bytes.
constexpr std::size_t kRiffSize = 4u;
constexpr std::size_t kMainHeader = 24u;
constexpr std::size_t kMicroseconds = 32u;
constexpr std::size_t kTotalFrames = 48u;
constexpr std::size_t kStreams = 56u;
constexpr std::size_t kWidth = 64u;
constexpr std::size_t kHeight = 68u;
constexpr std::size_t kStreamHeader = 100u;
constexpr std::size_t kStreamType = 108u;
constexpr std::size_t kScale = 128u;
constexpr std::size_t kRate = 132u;
constexpr std::size_t kStreamLength = 140u;
constexpr std::size_t kFormat = 164u;
constexpr std::size_t kFormatWidth = 176u;
constexpr std::size_t kFormatHeight = 180u;
constexpr std::size_t kBitCount = 186u;
constexpr std::size_t kCompression = 188u;
constexpr std::size_t kMovieList = 212u;
constexpr std::size_t kMovieSize = 216u;
constexpr std::size_t kMovieTag = 220u;
constexpr std::size_t kHeaderBytes = 224u;

// A picture whose pixel at (x, y) is (x, y, x + y) with an opaque fourth byte, so
// every position is its own colour and a row read back out names itself.
struct Pixels {
  unsigned width = 0;
  unsigned height = 0;
  std::vector<std::uint8_t> bytes;

  Pixels(unsigned across, unsigned down) : width(across), height(down) {
    bytes.resize(static_cast<std::size_t>(across) * down * 4u);
    for (unsigned y = 0u; y < down; ++y) {
      for (unsigned x = 0u; x < across; ++x) {
        const std::size_t at = (static_cast<std::size_t>(y) * across + x) * 4u;
        bytes[at] = static_cast<std::uint8_t>(x);
        bytes[at + 1u] = static_cast<std::uint8_t>(y);
        bytes[at + 2u] = static_cast<std::uint8_t>(x + y);
        bytes[at + 3u] = 255u;
      }
    }
  }

  [[nodiscard]] VideoFrame frame() const {
    return VideoFrame{.pixels = bytes, .width = width, .height = height, .field = 0u};
  }
};

constexpr FrameRate kSixty{.rate = 60u, .scale = 1u};

TEST_F(Recording, TheHeadersAreTheChunksAnAviIsMadeOf) {
  {
    AviRecording recording(file(), 4u, 2u, kSixty);
    ASSERT_TRUE(recording.open());
    recording.finish();
  }
  const std::vector<std::uint8_t> bytes = readFile(file());
  ASSERT_GE(bytes.size(), kHeaderBytes);
  EXPECT_EQ(tagAt(bytes, 0u), "RIFF");
  EXPECT_EQ(tagAt(bytes, 8u), "AVI ");
  EXPECT_EQ(tagAt(bytes, 12u), "LIST");
  EXPECT_EQ(tagAt(bytes, 20u), "hdrl");
  EXPECT_EQ(tagAt(bytes, kMainHeader), "avih");
  EXPECT_EQ(u32At(bytes, kMainHeader + 4u), 56u);
  EXPECT_EQ(tagAt(bytes, 88u), "LIST");
  EXPECT_EQ(tagAt(bytes, 96u), "strl");
  EXPECT_EQ(tagAt(bytes, kStreamHeader), "strh");
  EXPECT_EQ(u32At(bytes, kStreamHeader + 4u), 56u);
  EXPECT_EQ(tagAt(bytes, kStreamType), "vids");
  EXPECT_EQ(tagAt(bytes, kStreamType + 4u), "DIB ");
  EXPECT_EQ(tagAt(bytes, kFormat), "strf");
  EXPECT_EQ(u32At(bytes, kFormat + 4u), 40u);
  EXPECT_EQ(u32At(bytes, kFormat + 8u), 40u);  // the format names its own size first
  EXPECT_EQ(tagAt(bytes, kMovieList), "LIST");
  EXPECT_EQ(tagAt(bytes, kMovieTag), "movi");
}

TEST_F(Recording, TheHeadersCarryThePicturesShapeAndRate) {
  {
    AviRecording recording(file(), 4u, 2u, kSixty);
    ASSERT_TRUE(recording.open());
    recording.finish();
  }
  const std::vector<std::uint8_t> bytes = readFile(file());
  EXPECT_EQ(u32At(bytes, kMicroseconds), 1000000u / 60u);
  EXPECT_EQ(u32At(bytes, kStreams), 1u);
  EXPECT_EQ(u32At(bytes, kWidth), 4u);
  EXPECT_EQ(u32At(bytes, kHeight), 2u);
  EXPECT_EQ(u32At(bytes, kScale), 1u);
  EXPECT_EQ(u32At(bytes, kRate), 60u);
  EXPECT_EQ(u32At(bytes, kFormatWidth), 4u);
  EXPECT_EQ(u32At(bytes, kFormatHeight), 2u);  // positive: the rows run bottom up
  EXPECT_EQ(u16At(bytes, kBitCount), 24u);
  EXPECT_EQ(u32At(bytes, kCompression), 0u);  // stored, not compressed
}

TEST_F(Recording, AFrameRateThatIsNotAWholeNumberIsKeptAsTheRatioItIs) {
  // The console's rate is its master clock over the master cycles a picture takes.
  {
    AviRecording recording(file(), 4u, 2u, FrameRate{.rate = 236250000u, .scale = 3931026u});
    ASSERT_TRUE(recording.open());
    recording.finish();
  }
  const std::vector<std::uint8_t> bytes = readFile(file());
  EXPECT_EQ(u32At(bytes, kRate), 236250000u);
  EXPECT_EQ(u32At(bytes, kScale), 3931026u);
}

TEST_F(Recording, AFileWithNoPicturesIsItsHeadersAndAnEmptyIndex) {
  {
    AviRecording recording(file(), 4u, 2u, kSixty);
    ASSERT_TRUE(recording.open());
    recording.finish();
    EXPECT_EQ(recording.frames(), 0u);
  }
  const std::vector<std::uint8_t> bytes = readFile(file());
  EXPECT_EQ(bytes.size(), kHeaderBytes + 8u);
  EXPECT_EQ(u32At(bytes, kTotalFrames), 0u);
  EXPECT_EQ(u32At(bytes, kStreamLength), 0u);
  EXPECT_EQ(u32At(bytes, kMovieSize), 4u);  // the movie list's own tag, and nothing else
  EXPECT_EQ(tagAt(bytes, kHeaderBytes), "idx1");
  EXPECT_EQ(u32At(bytes, kHeaderBytes + 4u), 0u);
  EXPECT_EQ(u32At(bytes, kRiffSize), bytes.size() - 8u);
}

TEST_F(Recording, EachPictureIsAChunkOfRowsFromTheBottomUpInBlueGreenRed) {
  const Pixels picture(4u, 2u);
  {
    AviRecording recording(file(), 4u, 2u, kSixty);
    ASSERT_TRUE(recording.open());
    recording.add(picture.frame());
    recording.finish();
    EXPECT_EQ(recording.frames(), 1u);
  }
  const std::vector<std::uint8_t> bytes = readFile(file());
  EXPECT_EQ(tagAt(bytes, kHeaderBytes), "00db");
  EXPECT_EQ(u32At(bytes, kHeaderBytes + 4u), 4u * 3u * 2u);  // four pixels, three bytes, two rows

  // The stored rows are the picture's last first, and each pixel is blue, green then
  // red — so the first three bytes are row 1's pixel 0: (0, 1, 1) stored as 1, 1, 0.
  const std::size_t data = kHeaderBytes + 8u;
  EXPECT_EQ(bytes.at(data), 1u);       // blue: x + y
  EXPECT_EQ(bytes.at(data + 1u), 1u);  // green: y
  EXPECT_EQ(bytes.at(data + 2u), 0u);  // red: x
  // The last stored row is the picture's first: pixel 3 of row 0 is (3, 0, 3).
  const std::size_t lastPixel = data + 4u * 3u + 3u * 3u;
  EXPECT_EQ(bytes.at(lastPixel), 3u);
  EXPECT_EQ(bytes.at(lastPixel + 1u), 0u);
  EXPECT_EQ(bytes.at(lastPixel + 2u), 3u);
}

TEST_F(Recording, ARowIsRunOutToAFourByteBoundary) {
  // Three pixels are nine bytes, so each stored row carries three bytes of padding.
  const Pixels picture(3u, 2u);
  {
    AviRecording recording(file(), 3u, 2u, kSixty);
    ASSERT_TRUE(recording.open());
    recording.add(picture.frame());
    recording.finish();
  }
  const std::vector<std::uint8_t> bytes = readFile(file());
  EXPECT_EQ(u32At(bytes, kHeaderBytes + 4u), 12u * 2u);
  const std::size_t data = kHeaderBytes + 8u;
  EXPECT_EQ(bytes.at(data + 9u), 0u);
  EXPECT_EQ(bytes.at(data + 10u), 0u);
  EXPECT_EQ(bytes.at(data + 11u), 0u);
  // The second stored row begins after that padding, at the twelfth byte.
  EXPECT_EQ(bytes.at(data + 12u), 0u);  // row 0's pixel 0 is (0, 0, 0)
}

TEST_F(Recording, TheIndexNamesWhereEveryPictureBeginsPastTheMovieTag) {
  const Pixels picture(4u, 2u);
  {
    AviRecording recording(file(), 4u, 2u, kSixty);
    ASSERT_TRUE(recording.open());
    recording.add(picture.frame());
    recording.add(picture.frame());
    recording.add(picture.frame());
    recording.finish();
    EXPECT_EQ(recording.frames(), 3u);
  }
  const std::vector<std::uint8_t> bytes = readFile(file());
  const std::uint32_t stored = 4u * 3u * 2u;
  const std::size_t index = kHeaderBytes + 3u * (8u + stored);
  ASSERT_EQ(tagAt(bytes, index), "idx1");
  EXPECT_EQ(u32At(bytes, index + 4u), 3u * 16u);
  for (std::uint32_t picture_ = 0u; picture_ < 3u; ++picture_) {
    const std::size_t entry = index + 8u + picture_ * 16u;
    EXPECT_EQ(tagAt(bytes, entry), "00db");
    EXPECT_EQ(u32At(bytes, entry + 4u), 0x10u);  // every picture stands on its own
    EXPECT_EQ(u32At(bytes, entry + 8u), 4u + picture_ * (8u + stored));
    EXPECT_EQ(u32At(bytes, entry + 12u), stored);
  }
}

TEST_F(Recording, ClosingPatchesTheCountsAndTheSizesTheRecordingDecides) {
  const Pixels picture(4u, 2u);
  {
    AviRecording recording(file(), 4u, 2u, kSixty);
    ASSERT_TRUE(recording.open());
    recording.add(picture.frame());
    recording.add(picture.frame());
    recording.finish();
  }
  const std::vector<std::uint8_t> bytes = readFile(file());
  const std::uint32_t stored = 4u * 3u * 2u;
  EXPECT_EQ(u32At(bytes, kTotalFrames), 2u);
  EXPECT_EQ(u32At(bytes, kStreamLength), 2u);
  EXPECT_EQ(u32At(bytes, kMovieSize), 4u + 2u * (8u + stored));
  EXPECT_EQ(u32At(bytes, kRiffSize), bytes.size() - 8u);
}

TEST_F(Recording, ARecordingNobodyFinishesIsFinishedWhenItGoesAway) {
  const Pixels picture(4u, 2u);
  {
    AviRecording recording(file(), 4u, 2u, kSixty);
    ASSERT_TRUE(recording.open());
    recording.add(picture.frame());
  }
  const std::vector<std::uint8_t> bytes = readFile(file());
  EXPECT_EQ(u32At(bytes, kTotalFrames), 1u);
  EXPECT_EQ(u32At(bytes, kRiffSize), bytes.size() - 8u);
}

TEST_F(Recording, APictureOfAnotherShapeIsNotRecorded) {
  const Pixels other(8u, 4u);
  {
    AviRecording recording(file(), 4u, 2u, kSixty);
    ASSERT_TRUE(recording.open());
    recording.add(other.frame());
    recording.finish();
    EXPECT_EQ(recording.frames(), 0u);
  }
  const std::vector<std::uint8_t> bytes = readFile(file());
  EXPECT_EQ(u32At(bytes, kTotalFrames), 0u);
}

TEST_F(Recording, ThePicturesComeBackOutAsTheyWentIn) {
  // The round trip: the index says where each picture is and the format says how its
  // rows are laid out, so a reader built from the file alone gets the pixels back.
  const Pixels first(6u, 3u);
  Pixels second(6u, 3u);
  for (std::uint8_t& byte : second.bytes) byte = static_cast<std::uint8_t>(255u - byte);
  {
    AviRecording recording(file(), 6u, 3u, kSixty);
    ASSERT_TRUE(recording.open());
    recording.add(first.frame());
    recording.add(second.frame());
    recording.finish();
  }
  const std::vector<std::uint8_t> bytes = readFile(file());
  const unsigned width = u32At(bytes, kFormatWidth);
  const unsigned height = u32At(bytes, kFormatHeight);
  const std::size_t stride = ((static_cast<std::size_t>(width) * 3u) + 3u) & ~std::size_t{3u};
  const std::size_t index = kHeaderBytes + 2u * (8u + stride * height);
  ASSERT_EQ(tagAt(bytes, index), "idx1");
  ASSERT_EQ(u32At(bytes, kTotalFrames), 2u);

  for (unsigned picture = 0u; picture < 2u; ++picture) {
    const std::size_t entry = index + 8u + picture * 16u;
    const std::size_t at = kMovieTag + u32At(bytes, entry + 8u) + 8u;  // past the chunk's tag
    const std::vector<std::uint8_t>& source = picture == 0u ? first.bytes : second.bytes;
    for (unsigned y = 0u; y < height; ++y) {
      for (unsigned x = 0u; x < width; ++x) {
        const std::size_t stored = at + (height - 1u - y) * stride + x * 3u;
        const std::size_t original = (static_cast<std::size_t>(y) * width + x) * 4u;
        EXPECT_EQ(bytes.at(stored), source.at(original + 2u)) << "blue at " << x << ", " << y;
        EXPECT_EQ(bytes.at(stored + 1u), source.at(original + 1u)) << "green at " << x << ", " << y;
        EXPECT_EQ(bytes.at(stored + 2u), source.at(original)) << "red at " << x << ", " << y;
      }
    }
  }
}

}  // namespace
}  // namespace snaggletooth::video
