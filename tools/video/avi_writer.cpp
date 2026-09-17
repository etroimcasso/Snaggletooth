#include "avi_writer.h"

#include <algorithm>
#include <cstddef>
#include <system_error>

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

// The 224 bytes of headers for pictures `width` by `height` at `rate`, the sizes the
// whole recording decides left as zero.
[[nodiscard]] std::vector<std::uint8_t> headers(unsigned width, unsigned height, FrameRate rate) {
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
  return out;
}

// The index and the four values only the whole recording knows, written to a file
// whose headers and pictures are already down.
void close(std::ofstream& file, const std::vector<std::uint8_t>& index, std::uint32_t frames,
           std::uint32_t movieBytes) {
  write(file, index);
  const std::uint32_t riff = kMovieTagAt + movieBytes + static_cast<std::uint32_t>(index.size()) - 8u;
  std::vector<std::uint8_t> patch;
  const auto put = [&file, &patch](std::uint32_t at, std::uint32_t value) {
    patch.clear();
    putU32(patch, value);
    file.seekp(at);
    write(file, patch);
  };
  put(kRiffSizeAt, riff);
  put(kTotalFramesAt, frames);
  put(kStreamLengthAt, frames);
  put(kMovieSizeAt, movieBytes);
  file.close();
}

// One index entry: where a picture begins and how many bytes it holds.
void putEntry(std::vector<std::uint8_t>& index, std::uint32_t offset, std::uint32_t bytes) {
  putTag(index, "00db");
  putU32(index, kKeyFrame);  // every stored picture stands on its own
  putU32(index, offset);
  putU32(index, bytes);
}

}  // namespace

AviRecording::AviRecording(const std::filesystem::path& path, unsigned width, unsigned height,
                           FrameRate rate)
    : path_(path),
      file_(path, std::ios::binary | std::ios::trunc),
      width_(width),
      height_(height),
      rate_(rate) {
  if (!file_.is_open()) return;
  write(file_, headers(width, height, rate));
}

AviRecording::~AviRecording() { finish(); }

void AviRecording::writeChunk(const VideoFrame& picture) {
  const unsigned width = picture.width;
  const unsigned height = picture.height;
  const std::size_t stride = rowBytes(width);
  chunk_.clear();
  chunk_.reserve(8u + stride * height);
  putTag(chunk_, "00db");
  putU32(chunk_, static_cast<std::uint32_t>(stride * height));
  for (unsigned row = height; row-- > 0u;) {  // the bottom row of the picture first
    const std::size_t source = static_cast<std::size_t>(row) * width * kSourceBytesPerPixel;
    for (unsigned column = 0u; column < width; ++column) {
      const std::size_t pixel = source + static_cast<std::size_t>(column) * kSourceBytesPerPixel;
      chunk_.push_back(picture.pixels[pixel + 2u]);  // blue
      chunk_.push_back(picture.pixels[pixel + 1u]);  // green
      chunk_.push_back(picture.pixels[pixel]);       // red
    }
    for (std::size_t pad = static_cast<std::size_t>(width) * kStoredBytesPerPixel; pad < stride;
         ++pad) {
      chunk_.push_back(0u);
    }
  }
  write(file_, chunk_);
}

void AviRecording::add(const VideoFrame& picture) {
  if (!file_.is_open()) return;
  if (picture.pixels.size() <
      static_cast<std::size_t>(picture.width) * picture.height * kSourceBytesPerPixel) {
    return;
  }
  written_.push_back(Written{.offset = movieBytes_, .width = picture.width, .height = picture.height});
  writeChunk(picture);
  movieBytes_ += 8u + pictureBytes(picture.width, picture.height);
  ++frames_;
}

void AviRecording::finish() {
  if (!file_.is_open()) return;

  // The index: where each picture is, measured from the movie list's own tag.
  std::vector<std::uint8_t> index;
  index.reserve(8u + written_.size() * kIndexEntryBytes);
  putTag(index, "idx1");
  putU32(index, static_cast<std::uint32_t>(written_.size()) * kIndexEntryBytes);
  unsigned widest = 0u;
  unsigned tallest = 0u;
  bool oneShape = true;
  for (const Written& picture : written_) {
    putEntry(index, picture.offset, pictureBytes(picture.width, picture.height));
    widest = std::max(widest, picture.width);
    tallest = std::max(tallest, picture.height);
    oneShape = oneShape && picture.width == width_ && picture.height == height_;
  }
  close(file_, index, frames_, movieBytes_);
  if (!oneShape) layOut(widest, tallest);
}

void AviRecording::layOut(unsigned width, unsigned height) {
  // The finished file is read a picture at a time and the pictures written again,
  // at the new shape, into a file beside it; that file then takes the name. A
  // recording that cannot be laid out is left as it was written.
  std::filesystem::path laid = path_;
  laid += ".laid";
  std::ifstream in(path_, std::ios::binary);
  std::ofstream out(laid, std::ios::binary | std::ios::trunc);
  if (!in.is_open() || !out.is_open()) return;
  write(out, headers(width, height, rate_));

  const std::size_t stride = rowBytes(width);
  const std::uint32_t bytes = pictureBytes(width, height);
  std::vector<std::uint8_t> index;
  index.reserve(8u + written_.size() * kIndexEntryBytes);
  putTag(index, "idx1");
  putU32(index, static_cast<std::uint32_t>(written_.size()) * kIndexEntryBytes);
  std::vector<std::uint8_t> source;
  std::uint32_t movieBytes = 4u;
  bool complete = true;
  for (const Written& picture : written_) {
    const std::size_t sourceStride = rowBytes(picture.width);
    source.resize(sourceStride * picture.height);
    in.seekg(static_cast<std::streamoff>(kMovieTagAt) + picture.offset + 8);
    in.read(reinterpret_cast<char*>(source.data()), static_cast<std::streamsize>(source.size()));
    if (!in) {
      complete = false;
      break;
    }

    const unsigned left = (width - picture.width) / 2u;
    const unsigned top = (height - picture.height) / 2u;
    chunk_.clear();
    putTag(chunk_, "00db");
    putU32(chunk_, bytes);
    chunk_.resize(8u + stride * height, 0u);  // black wherever the picture does not reach
    // Stored rows run bottom up in both, so output row r from the bottom is picture
    // row (height - 1 - r) - top from the top.
    for (unsigned stored = 0u; stored < height; ++stored) {
      const unsigned fromTop = height - 1u - stored;
      if (fromTop < top || fromTop >= top + picture.height) continue;
      const unsigned pictureStored = picture.height - 1u - (fromTop - top);
      const auto from = source.begin() + static_cast<std::ptrdiff_t>(pictureStored * sourceStride);
      std::copy(from, from + static_cast<std::ptrdiff_t>(picture.width * kStoredBytesPerPixel),
                chunk_.begin() + static_cast<std::ptrdiff_t>(8u + stored * stride + left * kStoredBytesPerPixel));
    }
    write(out, chunk_);
    putEntry(index, movieBytes, bytes);
    movieBytes += 8u + bytes;
  }
  in.close();
  if (!complete) {
    out.close();
    std::error_code ignored;
    std::filesystem::remove(laid, ignored);
    return;
  }
  close(out, index, frames_, movieBytes);
  std::error_code failed;
  std::filesystem::rename(laid, path_, failed);
  if (failed) std::filesystem::remove(laid, failed);
}

}  // namespace snaggletooth::video
