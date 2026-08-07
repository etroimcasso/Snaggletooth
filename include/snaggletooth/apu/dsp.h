#pragma once

// The S-DSP — the sound chip the SPC700 drives through the DSPADDR/DSPDATA
// registers. This header carries the DSP's state as a value and the pure BRR
// sample-decode mechanism: turning a 9-byte compressed block into sixteen 15-bit
// samples, and reading a voice's sample-directory entry.
//
// A BRR block is decoded exactly as the hardware does — the four integer filters,
// the shift-13..15 anomaly, and the clamp-to-16-then-clip-to-15 sequence whose
// dirt-effect and lost-sign glitches are behaviors to reproduce, not to sanitize.
// The voice pipeline that streams these blocks under a pitch counter arrives with
// the next unit; here the decode is a standalone function over its inputs.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace snaggletooth {

// The S-DSP's state as a value: snapshot by copy, restore by assignment. The
// 128-byte register file the CPU reaches through DSPADDR/DSPDATA lives here (it
// moved out of ApuState, which keeps only the DSPADDR latch). Indexing or
// iterating a DspState reaches its register file, so the machine's overlay reads
// and writes a DSP register as dsp[reg]; the sample clock is a named member
// beside it.
struct DspState {
  std::array<std::uint8_t, 128> regs{};

  // The DSP samples at 32 kHz — one sample every 32 machine cycles. This divider
  // free-runs on delivered cycles like the timers' stage-1 divider: it aligns to
  // zero at power-on and is retained across reset (a free-running divider cannot
  // be reset). Nothing consumes its sample ticks yet.
  std::uint16_t sampleDivider = 0;

  [[nodiscard]] std::uint8_t& operator[](std::size_t reg) noexcept { return regs[reg]; }
  [[nodiscard]] const std::uint8_t& operator[](std::size_t reg) const noexcept {
    return regs[reg];
  }
  [[nodiscard]] auto begin() const noexcept { return regs.begin(); }
  [[nodiscard]] auto end() const noexcept { return regs.end(); }
};

// A voice's BRR source, read from the sample directory: the start address used
// when the voice is keyed on, and the loop address jumped to when a block's end
// flag is reached.
struct BrrSource {
  std::uint16_t start = 0;
  std::uint16_t loop = 0;
};

// Reads voice source `srcn`'s four-byte directory entry from the table based at
// DIR*$100. Each entry is a little-endian start address followed by a
// little-endian loop address. Addresses wrap within the 64KB space.
[[nodiscard]] BrrSource readBrrSource(std::span<const std::uint8_t, 65536> ram,
                                      std::uint8_t dir, std::uint8_t srcn) noexcept;

// One decoded 9-byte BRR block: sixteen 15-bit samples, the two most recent
// outputs that carry as the filter history into the next block, and the block's
// loop/end flags (header bit 0 = end, header bit 1 = loop). endBlock marks the
// codes that stop the sample (End+Mute and End+Loop); loopBlock distinguishes
// End+Loop from End+Mute and is meaningful only when endBlock is set.
struct BrrBlock {
  std::array<std::int16_t, 16> samples{};
  std::int16_t last = 0;    // sample 15 — becomes `old` for the next block
  std::int16_t prev = 0;    // sample 14 — becomes `older` for the next block
  bool endBlock = false;
  bool loopBlock = false;
};

// Decodes one 9-byte BRR block (byte 0 is the header: shift 7-4, filter 3-2,
// loop/end 1-0; bytes 1-8 hold two signed nibbles each). `old` and `older` are
// the two previous 15-bit outputs the filter reads — zero for a sample's first
// block. Every sample is clamped to signed 16 bits then clipped to signed 15, so
// the documented overflow glitches emerge exactly.
[[nodiscard]] BrrBlock decodeBrrBlock(std::span<const std::uint8_t, 9> block,
                                      std::int16_t old, std::int16_t older) noexcept;

}  // namespace snaggletooth
