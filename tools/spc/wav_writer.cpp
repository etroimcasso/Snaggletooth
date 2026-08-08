#include "wav_writer.h"

#include <cstddef>

namespace snaggletooth::spc {
namespace {

constexpr std::uint16_t kChannels = 2;
constexpr std::uint16_t kBitsPerSample = 16;
constexpr std::uint32_t kBytesPerFrame = kChannels * (kBitsPerSample / 8);  // 4

void putU16(std::vector<std::uint8_t>& out, std::uint16_t v) {
  out.push_back(static_cast<std::uint8_t>(v & 0xFF));
  out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
}

void putU32(std::vector<std::uint8_t>& out, std::uint32_t v) {
  out.push_back(static_cast<std::uint8_t>(v & 0xFF));
  out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
  out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFF));
  out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFF));
}

// Emits the four ASCII bytes of a chunk tag (the trailing NUL of the literal is not
// written).
void putTag(std::vector<std::uint8_t>& out, const char (&tag)[5]) {
  for (std::size_t i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(tag[i]));
}

}  // namespace

std::vector<std::uint8_t> writeWav(std::span<const StereoFrame> frames,
                                   std::uint32_t sampleRate) {
  const std::uint32_t dataBytes =
      static_cast<std::uint32_t>(frames.size()) * kBytesPerFrame;

  std::vector<std::uint8_t> out;
  out.reserve(static_cast<std::size_t>(44) + dataBytes);

  putTag(out, "RIFF");
  putU32(out, 36 + dataBytes);  // size of everything after this field
  putTag(out, "WAVE");

  putTag(out, "fmt ");
  putU32(out, 16);         // PCM fmt-chunk body size
  putU16(out, 1);          // audio format: PCM
  putU16(out, kChannels);
  putU32(out, sampleRate);
  putU32(out, sampleRate * kBytesPerFrame);  // byte rate
  putU16(out, static_cast<std::uint16_t>(kBytesPerFrame));  // block align
  putU16(out, kBitsPerSample);

  putTag(out, "data");
  putU32(out, dataBytes);
  for (const StereoFrame& f : frames) {
    putU16(out, static_cast<std::uint16_t>(f.left));
    putU16(out, static_cast<std::uint16_t>(f.right));
  }
  return out;
}

}  // namespace snaggletooth::spc
