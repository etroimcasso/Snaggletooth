#include "snaggletooth/apu/dsp.h"

namespace snaggletooth {

namespace {

// DSP register addresses the voice stream reads and writes. Per-voice
// registers live at voice*10h plus the per-voice offset.
constexpr std::uint8_t kDspDir = 0x5D;
constexpr std::uint8_t kDspEndx = 0x7C;
constexpr std::uint8_t kVoicePitchLow = 0x02;
constexpr std::uint8_t kVoicePitchHigh = 0x03;
constexpr std::uint8_t kVoiceSrcn = 0x04;

[[nodiscard]] std::uint8_t voiceRegister(std::size_t voice, std::uint8_t offset) noexcept {
  return static_cast<std::uint8_t>(voice * 0x10 + offset);
}

// The S-DSP's Gaussian interpolation table — hardware ROM data, transcribed
// from the references by tools/parse_dsp_tables.py (see the generated file's
// header). The four-tap sums land in 7FFh..801h rather than a constant 800h;
// the 801h rows are what make the kernel's documented wrap reachable.
constexpr std::array<std::int16_t, 512> kGaussTable = {
#include "generated/gauss_table.inc"
};

// Converts a signed 4-bit BRR nibble (-8..+7) to a sample: sample = nibble
// scaled by the block's shift, then arithmetic-shifted right one. A shift of
// 13..15 is anomalous — decoding proceeds as if shift were 12 over (nibble SAR
// 3). The scale is a multiply (never a left shift of a negative value) so the
// arithmetic is well-defined and warning-clean across compilers.
[[nodiscard]] int decodeNibble(int nibble, int shift) noexcept {
  if (shift >= 13) {
    shift = 12;
    nibble >>= 3;  // arithmetic: -8..-1 -> -1, 0..+7 -> 0
  }
  return (nibble * (1 << shift)) >> 1;
}

// Extracts one of a BRR data byte's two signed nibbles: the high nibble is the
// first sample, the low nibble the second.
[[nodiscard]] int signedNibble(std::uint8_t byte, bool second) noexcept {
  const int raw = second ? (byte & 0x0F) : (byte >> 4);
  return raw - ((raw & 0x08) ? 16 : 0);  // sign-extend to -8..+7
}

// The four BRR filters, in fullsnes's exact integer forms (cross-checked 1:1
// against Anomie's S-DSP doc). `old` is the previous 15-bit output, `older` the
// one before it. The right shifts are arithmetic over signed values.
[[nodiscard]] int applyFilter(int filter, int sample, int old, int older) noexcept {
  switch (filter) {
    case 0:
      return sample;
    case 1:
      return sample + old + ((-old) >> 4);
    case 2:
      return sample + old * 2 + ((-old * 3) >> 5) - older + (older >> 4);
    default:  // filter 3
      return sample + old * 2 + ((-old * 13) >> 6) - older + ((older * 3) >> 4);
  }
}

// Clamps to signed 16 bits, then clips to signed 15 by re-reading bit 14 as the
// sign. This is the hardware's clamp-then-clip: a value below -8000h clamps to
// -8000h and then clips to 0 (the dirt-effect), and a magnitude that overflows
// 15 bits folds to the opposite sign (lost-sign). In-range values pass through.
[[nodiscard]] std::int16_t clampAndClip(int value) noexcept {
  if (value > 0x7FFF) value = 0x7FFF;
  if (value < -0x8000) value = -0x8000;
  return static_cast<std::int16_t>(((value & 0x7FFF) ^ 0x4000) - 0x4000);
}

// The voice's 14-bit pitch step, read live from VxPITCHL/H; the register
// pair's bits 14-15 are stored but never used. Pitch modulation (PMON) is not
// implemented yet: it scales this step by the previous voice's output, and
// this is where it applies.
[[nodiscard]] std::uint32_t voicePitch(const DspState& dsp, std::size_t voice) noexcept {
  const std::uint32_t low = dsp[voiceRegister(voice, kVoicePitchLow)];
  const std::uint32_t high = dsp[voiceRegister(voice, kVoicePitchHigh)];
  return ((high << 8) | low) & 0x3FFF;
}

// Advances a voice's decode cursor by one sample. An exhausted block chains to
// the following block — or, after an end block, to the loop address read live
// from DIR and VxSRCN, which is how a DIR or VxSRCN change takes effect at the
// next loop and not before. Entering any block whose header carries the end
// flag sets the voice's ENDX bit — the hardware sets it at the START of
// decoding the end block, not after it. The decoded sample shifts into the
// window; the filter's history is the window's two newest taps.
void decodeStreamSample(DspState& dsp, std::span<const std::uint8_t, 65536> ram,
                        std::size_t voice) noexcept {
  VoiceState& v = dsp.voices[voice];
  if (v.brrSampleIndex >= 16) {
    const std::uint8_t endedHeader = ram[v.brrAddress];
    if ((endedHeader & 0x01) != 0) {
      v.brrAddress =
          readBrrSource(ram, dsp[kDspDir], dsp[voiceRegister(voice, kVoiceSrcn)]).loop;
    } else {
      v.brrAddress = static_cast<std::uint16_t>(v.brrAddress + 9);
    }
    v.brrSampleIndex = 0;
  }
  const std::uint8_t header = ram[v.brrAddress];
  if (v.brrSampleIndex == 0 && (header & 0x01) != 0) {
    dsp[kDspEndx] |= static_cast<std::uint8_t>(1u << voice);
  }
  const std::uint8_t byte =
      ram[static_cast<std::uint16_t>(v.brrAddress + 1 + v.brrSampleIndex / 2)];
  const int sample = decodeNibble(signedNibble(byte, (v.brrSampleIndex & 1) != 0),
                                  header >> 4);
  const std::int16_t decoded =
      clampAndClip(applyFilter((header >> 2) & 0x03, sample, v.window.newest, v.window.old));
  v.window.oldest = v.window.older;
  v.window.older = v.window.old;
  v.window.old = v.window.newest;
  v.window.newest = decoded;
  ++v.brrSampleIndex;
}

}  // namespace

BrrSource readBrrSource(std::span<const std::uint8_t, 65536> ram, std::uint8_t dir,
                        std::uint8_t srcn) noexcept {
  const int base = dir * 0x100 + srcn * 4;
  auto at = [&](int offset) -> std::uint16_t {
    return ram[static_cast<std::uint16_t>(base + offset)];
  };
  return BrrSource{
      .start = static_cast<std::uint16_t>(at(0) | (at(1) << 8)),
      .loop = static_cast<std::uint16_t>(at(2) | (at(3) << 8)),
  };
}

BrrBlock decodeBrrBlock(std::span<const std::uint8_t, 9> block, std::int16_t old,
                        std::int16_t older) noexcept {
  const std::uint8_t header = block[0];
  const int shift = header >> 4;
  const int filter = (header >> 2) & 0x03;

  BrrBlock out;
  out.endBlock = (header & 0x01) != 0;
  out.loopBlock = (header & 0x02) != 0;

  int recent = old;     // S(x-1) entering each sample
  int earlier = older;  // S(x-2)
  for (int i = 0; i < 16; ++i) {
    const std::uint8_t byte = block[1 + i / 2];
    const int sample = decodeNibble(signedNibble(byte, (i & 1) != 0), shift);
    const std::int16_t decoded = clampAndClip(applyFilter(filter, sample, recent, earlier));
    out.samples[static_cast<std::size_t>(i)] = decoded;
    earlier = recent;
    recent = decoded;
  }
  out.last = static_cast<std::int16_t>(recent);
  out.prev = static_cast<std::int16_t>(earlier);
  return out;
}

std::int16_t gaussInterpolate(SampleWindow window, std::uint8_t index) noexcept {
  const int i = index;
  const auto tap = [&](int entry, std::int16_t sample) {
    return (kGaussTable[static_cast<std::size_t>(entry)] * sample) >> 10;
  };
  // The first value cannot exceed 16 bits, and the first addition cannot
  // overflow them. The second addition has no overflow handling — it wraps,
  // reproducing the documented bug the table's 801h rows expose. The third
  // addition saturates.
  int out = tap(0x0FF - i, window.oldest);
  out = static_cast<std::int16_t>(out + tap(0x1FF - i, window.older));
  out = static_cast<std::int16_t>(out + tap(0x100 + i, window.old));
  out = out + tap(0x000 + i, window.newest);
  if (out > 0x7FFF) out = 0x7FFF;
  if (out < -0x8000) out = -0x8000;
  return static_cast<std::int16_t>(out >> 1);
}

void startVoice(DspState& dsp, std::span<const std::uint8_t, 65536> ram,
                std::size_t voice) noexcept {
  const BrrSource source =
      readBrrSource(ram, dsp[kDspDir], dsp[voiceRegister(voice, kVoiceSrcn)]);
  dsp.voices[voice] = VoiceState{.brrAddress = source.start};
  for (int n = 0; n < 4; ++n) decodeStreamSample(dsp, ram, voice);
}

std::int16_t stepVoice(DspState& dsp, std::span<const std::uint8_t, 65536> ram,
                       std::size_t voice) noexcept {
  VoiceState& v = dsp.voices[voice];
  const std::uint32_t advanced = v.pitchCounter + voicePitch(dsp, voice);
  const std::uint32_t passed = (advanced >> 12) - (v.pitchCounter >> 12);
  v.pitchCounter = static_cast<std::uint16_t>(advanced);
  for (std::uint32_t n = 0; n < passed; ++n) decodeStreamSample(dsp, ram, voice);
  return interpolatedSample(dsp, voice);
}

std::int16_t interpolatedSample(const DspState& dsp, std::size_t voice) noexcept {
  const VoiceState& v = dsp.voices[voice];
  return gaussInterpolate(v.window, static_cast<std::uint8_t>((v.pitchCounter >> 4) & 0xFF));
}

}  // namespace snaggletooth
