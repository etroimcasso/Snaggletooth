#pragma once

// A BRR sample as a WAV. A sample the S-DSP plays is blocks of nine bytes — a
// header carrying the shift, the filter and the loop and end flags, then sixteen
// four-bit values — decoded through the filter as each block is played, the last
// two outputs of one block carrying into the next. The WAV is what the DSP would
// play from the sample's first block to the one carrying the end flag, decoded
// once by the machine's own decoder with the loop not followed: 16-bit PCM at
// 32 000 Hz, the decoder's values as they are, the one channel written to both.
// It is a listening copy — nothing decodes it back, and the sample's bytes stay
// where they were.

#include <cstdint>
#include <span>

#include "codec.h"

namespace snaggletooth::formats {

// The bytes one BRR block holds.
constexpr std::size_t kBrrBlockBytes = 9;

// The rate the DSP plays a sample at when its pitch is $1000, and the WAV's.
constexpr std::uint32_t kBrrSampleRate = 32000;

// Encodes `sample` — whole blocks, the last carrying the end flag and no earlier
// one — as a WAV. Fails, naming what was wrong, when the bytes are not a whole
// number of blocks, when there are none, when the last block does not carry the
// end flag, or when an earlier block does.
[[nodiscard]] Bytes encodeBrrWav(std::span<const std::uint8_t> sample);

}  // namespace snaggletooth::formats
