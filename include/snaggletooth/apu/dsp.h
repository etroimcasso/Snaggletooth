#pragma once

// The S-DSP — the sound chip the SPC700 drives through the DSPADDR/DSPDATA
// registers. This header carries the DSP's state as a value and the voice
// sample pipeline's pure mechanisms: BRR sample decode (turning a 9-byte
// compressed block into sixteen 15-bit samples), the sample-directory read,
// the per-voice BRR streaming interlock under a pitch counter, and 4-point
// Gaussian interpolation over the stream's four most recent samples.
//
// A BRR block is decoded exactly as the hardware does — the four integer
// filters, the shift-13..15 anomaly, and the clamp-to-16-then-clip-to-15
// sequence whose dirt-effect and lost-sign glitches are behaviors to
// reproduce, not to sanitize. The same exactness holds for interpolation: the
// Gaussian table is the hardware's ROM data, and the kernel keeps the
// hardware's partial overflow handling, documented wrap included.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace snaggletooth {

// The four most recently decoded samples of a voice's BRR stream — the window
// the Gaussian kernel interpolates over. The hardware references name the
// taps new/old/older/oldest; `new` is a C++ keyword, so the newest tap is
// `newest` here.
struct SampleWindow {
  std::int16_t newest = 0;
  std::int16_t old = 0;
  std::int16_t older = 0;
  std::int16_t oldest = 0;
};

// A voice's streaming position through its BRR data. The pitch counter's bits
// 15-12 are the sample position within the current block, bits 11-4 the
// Gaussian interpolation index, and the low bits accumulate the fractional
// step. The decode cursor runs three samples ahead of the counter's position,
// so the window's `newest` at position p is stream sample p+3 — the alignment
// the hardware's decode buffering produces. `brrSampleIndex` is the next
// sample the cursor decodes within its block; 16 means the block is exhausted
// and the next decode performs the block transition (chaining to the next
// block, or jumping to the loop address after an end block) before decoding.
struct VoiceState {
  std::uint16_t brrAddress = 0;
  std::uint8_t brrSampleIndex = 0;
  std::uint16_t pitchCounter = 0;
  SampleWindow window{};
};

// The S-DSP's state as a value: snapshot by copy, restore by assignment. The
// 128-byte register file the CPU reaches through DSPADDR/DSPDATA lives here
// (ApuState carries only the DSPADDR latch beside it). Indexing or
// iterating a DspState reaches its register file, so the machine's overlay
// reads and writes a DSP register as dsp[reg]; the sample clock and the
// voices' streaming state are named members beside it.
struct DspState {
  std::array<std::uint8_t, 128> regs{};

  // The DSP samples at 32 kHz — one sample every 32 machine cycles. This divider
  // free-runs on delivered cycles like the timers' stage-1 divider: it aligns to
  // zero at power-on and is retained across reset (a free-running divider cannot
  // be reset). Nothing consumes its sample ticks yet.
  std::uint16_t sampleDivider = 0;

  // The eight voices' streaming state, beside the register file.
  std::array<VoiceState, 8> voices{};

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

// Interpolates a window at an 8-bit index (pitch counter bits 11-4) through
// the S-DSP's 4-point Gaussian kernel, in the hardware's exact arithmetic.
// Each tap is (gauss * sample) SAR 10 over the transcribed 512-entry hardware
// table. The additions of the older and old taps run in 16 bits with no
// overflow handling — the old tap's addition can wrap, a documented hardware
// bug the kernel reproduces — while the newest tap's addition saturates to
// -8000h/+7FFFh. The result is shifted right once, to 15 bits.
[[nodiscard]] std::int16_t gaussInterpolate(SampleWindow window, std::uint8_t index) noexcept;

// Points voice `voice` (0-7) at its sample's start address — read live from
// DIR and VxSRCN — and primes the stream: the counter returns to position
// zero and the first four samples are decoded, so the window holds stream
// samples 3..0 as newest..oldest. The filter history enters as zeros, so a
// first block using a filter other than 0 still decodes deterministically.
// Entering an end block sets the voice's ENDX bit immediately.
void startVoice(DspState& dsp, std::span<const std::uint8_t, 65536> ram,
                std::size_t voice) noexcept;

// Advances voice `voice` (0-7) by one 32 kHz output sample. The pitch counter
// gains the voice's 14-bit step (VxPITCHL/H bits 0-13, read live, so a pitch
// write takes effect on the next step; bits 14-15 are stored but never used),
// and every sample position the counter passes is decoded through the stream —
// following block chaining, loop jumps and ENDX on the way. Returns the
// freshly interpolated 15-bit sample, exactly as interpolatedSample reads it.
std::int16_t stepVoice(DspState& dsp, std::span<const std::uint8_t, 65536> ram,
                       std::size_t voice) noexcept;

// The voice's current interpolated 15-bit sample: the Gaussian kernel over its
// window at the pitch counter's current index. Pure over the state — no
// advance, no memory access.
[[nodiscard]] std::int16_t interpolatedSample(const DspState& dsp, std::size_t voice) noexcept;

}  // namespace snaggletooth
