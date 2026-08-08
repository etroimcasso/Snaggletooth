#pragma once

// The WAV writer — encodes stereo frames as a canonical PCM WAV file. The output is
// a 44-byte RIFF/fmt /data header followed by the frames as 16-bit little-endian
// signed samples in left/right interleave. No resampling, no dither, no gain, no
// metadata chunks: the bytes are the frames. The sample rate is written into the
// header (the S-DSP's native rate is 32000 Hz).

#include <cstdint>
#include <span>
#include <vector>

#include "snaggletooth/apu/dsp.h"

namespace snaggletooth::spc {

// Encodes `frames` as a PCM WAV at `sampleRate` and returns the whole file as bytes
// (header plus data); the caller writes them. An empty span yields a bare 44-byte
// header with a zero-length data chunk.
[[nodiscard]] std::vector<std::uint8_t> writeWav(std::span<const StereoFrame> frames,
                                                 std::uint32_t sampleRate);

}  // namespace snaggletooth::spc
