#include "avi_writer.h"

#include <cstddef>

namespace snaggletooth::video {
namespace {

// The file's shape. The headers are a fixed 224 bytes — the RIFF tag, the header
// list with the main header and the one stream's header and format, and the movie
// list's own tag — so the places the closing pass patches are known addresses.
constexpr std::uint32_t kHeaderBytes = 224u;
constexpr std::uint32_t kRiffSizeAt = 4u;
constexpr std::uint32_t kTotalFramesAt = 48u;
constexpr std::uint32_t kStreamLengthAt = 140u;
constexpr std::uint32_t kMovieSizeAt = 216u;
constexpr std::uint32_t kMovieTagAt = 220u;

constexpr std::uint32_t kHeaderListBytes = 192u;  // 'hdrl', the main header, the stream list
constexpr std::uint32_t kStreamListBytes = 116u;  // 'strl', the stream header, the format
constexpr std::uint32_t kMainHeaderBytes = 56u;
constexpr std::uint32_t kStreamHeaderBytes = 56u;
constexpr std::uint32_t kFormatBytes = 40u;
constexpr std::uint32_t kIndexEntryBytes = 16u;
constexpr std::uint32_t kHasIndex = 0x10u;   // the main header's flag for a file with one
constexpr std::uint32_t kKeyFrame = 0x10u;   // an index entry's flag for a picture that stands alone
constexpr std::uint32_t kDefaultQuality = 0xFFFFFFFFu;
constexpr std::size_t kSourceBytesPerPixel = 4u;  // what the machine hands over
constexpr std::size_t kStoredBytesPerPixel = 3u;  // what the file holds

void putU16(std::vector<std::uint8_t>& out, std::uint16_t value) {
  out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
  out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
}

void putU32(std::vector<std::uint8_t>& out, std::uint32_t value) {
  out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
  out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
  out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFFu));
  out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFFu));
}

// The four ASCII bytes of a tag, without the literal's trailing NUL.
void putTag(std::vector<std::uint8_t>& out, const char (&tag)[5]) {
  for (std::size_t at = 0u; at < 4u; ++at) out.push_back(static_cast<std::uint8_t>(tag[at]));
}

void write(std::ofstream& file, const std::vector<std::uint8_t>& bytes) {
  file.write(reinterpret_cast<const char*>(bytes.data()),
             static_cast<std::streamsize>(bytes.size()));
}

// A stored row runs out to a four-byte boundary.
[[nodiscard]] std::size_t rowBytes(unsigned width) {
  const std::size_t bytes = static_cast<std::size_t>(width) * kStoredBytesPerPixel;
  return (bytes + 3u) & ~std::size_t{3u};
}

[[nodiscard]] std::uint32_t pictureBytes(unsigned width, unsigned height) {
  return static_cast<std::uint32_t>(rowBytes(width) * height);
}

}  // namespace

AviRecording::AviRecording(const std::filesystem::path& path, unsigned width, unsigned height,
                           FrameRate rate)
    : file_(path, std::ios::binary | std::ios::trunc), width_(width), height_(height) {
  if (!file_.is_open()) return;

  const std::uint32_t picture = pictureBytes(width, height);
  const std::uint32_t perSecond =
      rate.scale == 0u ? 0u : static_cast<std::uint32_t>(rate.rate / rate.scale);

  std::vector<std::uint8_t> out;
  out.reserve(kHeaderBytes);
  putTag(out, "RIFF");
  putU32(out, 0u);  // the size of everything after this field, patched as the file closes
  putTag(out, "AVI ");

  putTag(out, "LIST");
  putU32(out, kHeaderListBytes);
  putTag(out, "hdrl");

  putTag(out, "avih");
  putU32(out, kMainHeaderBytes);
  putU32(out, perSecond == 0u ? 0u : 1000000u / perSecond);  // microseconds a picture
  putU32(out, picture * perSecond);                          // the bytes a second it takes
  putU32(out, 0u);                                           // no padding granularity
  putU32(out, kHasIndex);
  putU32(out, 0u);  // the pictures there are, patched as the file closes
  putU32(out, 0u);  // no pictures before the first
  putU32(out, 1u);  // one stream
  putU32(out, picture);
  putU32(out, width);
  putU32(out, height);
  for (unsigned reserved = 0u; reserved < 4u; ++reserved) putU32(out, 0u);

  putTag(out, "LIST");
  putU32(out, kStreamListBytes);
  putTag(out, "strl");

  putTag(out, "strh");
  putU32(out, kStreamHeaderBytes);
  putTag(out, "vids");  // a video stream
  putTag(out, "DIB ");  // of stored pictures, which is what uncompressed means here
  putU32(out, 0u);      // no flags
  putU16(out, 0u);      // no priority
  putU16(out, 0u);      // no language
  putU32(out, 0u);      // no pictures before the first
  putU32(out, rate.scale);
  putU32(out, rate.rate);
  putU32(out, 0u);  // the stream starts at the file's start
  putU32(out, 0u);  // its length in pictures, patched as the file closes
  putU32(out, picture);
  putU32(out, kDefaultQuality);
  putU32(out, picture);  // every picture is the same size
  putU16(out, 0u);       // the frame rectangle: the whole picture
  putU16(out, 0u);
  putU16(out, static_cast<std::uint16_t>(width));
  putU16(out, static_cast<std::uint16_t>(height));

  putTag(out, "strf");
  putU32(out, kFormatBytes);
  putU32(out, kFormatBytes);  // the format's own size, which names its version
  putU32(out, width);
  putU32(out, height);  // positive: the rows are stored from the bottom up
  putU16(out, 1u);      // one plane
  putU16(out, 24u);     // three bytes a pixel
  putU32(out, 0u);      // stored, not compressed
  putU32(out, picture);
  putU32(out, 0u);  // no pixels-per-metre either way
  putU32(out, 0u);
  putU32(out, 0u);  // no palette
  putU32(out, 0u);

  putTag(out, "LIST");
  putU32(out, 0u);  // the movie list's size, patched as the file closes
  putTag(out, "movi");
  write(file_, out);
}

AviRecording::~AviRecording() { finish(); }

void AviRecording::writeChunk(const VideoFrame& picture) {
  const std::size_t stride = rowBytes(width_);
  chunk_.clear();
  chunk_.reserve(8u + stride * height_);
  putTag(chunk_, "00db");
  putU32(chunk_, static_cast<std::uint32_t>(stride * height_));
  for (unsigned row = height_; row-- > 0u;) {  // the bottom row of the picture first
    const std::size_t source = static_cast<std::size_t>(row) * width_ * kSourceBytesPerPixel;
    for (unsigned column = 0u; column < width_; ++column) {
      const std::size_t pixel = source + static_cast<std::size_t>(column) * kSourceBytesPerPixel;
      chunk_.push_back(picture.pixels[pixel + 2u]);  // blue
      chunk_.push_back(picture.pixels[pixel + 1u]);  // green
      chunk_.push_back(picture.pixels[pixel]);       // red
    }
    for (std::size_t pad = static_cast<std::size_t>(width_) * kStoredBytesPerPixel; pad < stride;
         ++pad) {
      chunk_.push_back(0u);
    }
  }
  write(file_, chunk_);
}

void AviRecording::add(const VideoFrame& picture) {
  if (!file_.is_open()) return;
  if (picture.width != width_ || picture.height != height_) return;
  if (picture.pixels.size() < static_cast<std::size_t>(width_) * height_ * kSourceBytesPerPixel) {
    return;
  }
  offsets_.push_back(movieBytes_);  // where this picture begins, past the movie list's tag
  writeChunk(picture);
  movieBytes_ += 8u + pictureBytes(width_, height_);
  ++frames_;
}

void AviRecording::finish() {
  if (!file_.is_open()) return;

  // The index: where each picture is, measured from the movie list's own tag.
  std::vector<std::uint8_t> out;
  out.reserve(8u + offsets_.size() * kIndexEntryBytes);
  putTag(out, "idx1");
  putU32(out, static_cast<std::uint32_t>(offsets_.size()) * kIndexEntryBytes);
  for (const std::uint32_t offset : offsets_) {
    putTag(out, "00db");
    putU32(out, kKeyFrame);  // every stored picture stands on its own
    putU32(out, offset);
    putU32(out, pictureBytes(width_, height_));
  }
  write(file_, out);

  // The three sizes and the count the whole recording decides.
  const std::uint32_t indexBytes = 8u + static_cast<std::uint32_t>(offsets_.size()) * kIndexEntryBytes;
  const std::uint32_t riff = kMovieTagAt + movieBytes_ + indexBytes - 8u;
  std::vector<std::uint8_t> patch;
  const auto put = [this, &patch](std::uint32_t at, std::uint32_t value) {
    patch.clear();
    putU32(patch, value);
    file_.seekp(at);
    write(file_, patch);
  };
  put(kRiffSizeAt, riff);
  put(kTotalFramesAt, frames_);
  put(kStreamLengthAt, frames_);
  put(kMovieSizeAt, movieBytes_);
  file_.close();
}

}  // namespace snaggletooth::video
