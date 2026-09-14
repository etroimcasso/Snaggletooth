#pragma once

// The video writer — records the machine's pictures as an uncompressed AVI.
//
// A recording is written a picture at a time: the headers go down as it opens, each
// picture as it arrives, and the index and the two sizes only the finished file
// knows when it closes. So a recording costs one picture of memory however long it
// runs, which a minute of video at sixty pictures a second needs.
//
// The pictures are stored exactly as the machine drove them — blue, green and red a
// byte each, with no encoder between — so a recording is an oracle to set beside a
// capture from the console itself rather than an approximation of one. It plays in
// VLC and QuickTime and converts with ffmpeg.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

#include "snaggletooth/snes/video_frame.h"

namespace snaggletooth::video {

// A recording's frame rate, as `rate` frames every `scale` seconds: the console's
// rate is not a whole number of frames a second, so it is carried as the ratio it
// is and written into the file as one.
struct FrameRate {
  std::uint32_t rate = 60u;
  std::uint32_t scale = 1u;
};

// One recording, open from construction until finish() or destruction.
class AviRecording {
 public:
  // Opens `path` for pictures `width` by `height` at `rate`. Whether the file opened
  // is open(); a recording that did not open accepts pictures and writes nothing, so
  // a caller that wants to know asks.
  AviRecording(const std::filesystem::path& path, unsigned width, unsigned height,
               FrameRate rate);
  ~AviRecording();

  // A recording is the file it holds open, so it is neither copied nor moved.
  AviRecording(const AviRecording&) = delete;
  AviRecording& operator=(const AviRecording&) = delete;

  [[nodiscard]] bool open() const noexcept { return file_.is_open(); }
  [[nodiscard]] std::uint32_t frames() const noexcept { return frames_; }

  // Adds one picture. A picture whose size is not the recording's is not written —
  // the file holds one shape, which its header has already declared.
  void add(const VideoFrame& picture);

  // Writes the index and the sizes the whole recording decides, and closes the file.
  // Doing it twice does nothing the second time; the destructor does it for a
  // recording nobody finished.
  void finish();

 private:
  // The bytes one picture becomes: its chunk tag and size, then the rows bottom up,
  // three bytes a pixel, each row run out to a four-byte boundary.
  void writeChunk(const VideoFrame& picture);

  std::ofstream file_;
  unsigned width_ = 0;
  unsigned height_ = 0;
  std::uint32_t frames_ = 0;
  std::uint32_t movieBytes_ = 4u;  // the 'movi' tag itself, before any picture
  std::vector<std::uint32_t> offsets_;  // where each picture begins, for the index
  std::vector<std::uint8_t> chunk_;     // one picture's bytes, reused
};

}  // namespace snaggletooth::video
