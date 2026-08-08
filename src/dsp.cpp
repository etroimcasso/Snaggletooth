#include "snaggletooth/apu/dsp.h"

namespace snaggletooth {

namespace {

// DSP register addresses the voice stream reads and writes. Per-voice
// registers live at voice*10h plus the per-voice offset.
constexpr std::uint8_t kDspMvolLeft = 0x0C;
constexpr std::uint8_t kDspMvolRight = 0x1C;
constexpr std::uint8_t kDspPmon = 0x2D;
constexpr std::uint8_t kDspNon = 0x3D;
constexpr std::uint8_t kDspKon = 0x4C;
constexpr std::uint8_t kDspKoff = 0x5C;
constexpr std::uint8_t kDspDir = 0x5D;
constexpr std::uint8_t kDspFlg = 0x6C;
constexpr std::uint8_t kDspEndx = 0x7C;

// FLG bit masks: bits 0-4 are the noise rate, bit 5 disables echo writes, bit 6
// mutes the output amplifier, bit 7 is the per-sample soft reset.
constexpr std::uint8_t kFlgNoiseRate = 0x1F;
constexpr std::uint8_t kFlgMute = 0x40;
constexpr std::uint8_t kFlgSoftReset = 0x80;

// The pitch step's 128 kHz ceiling: the counter advances at most four source
// samples (four times the 32 kHz output rate) per output sample. The base 14-bit
// step tops out at 3FFFh, so only a PMON-scaled step reaches the cap.
constexpr std::uint32_t kMaxPitchStep = 0x3FFF;
constexpr std::uint8_t kVoiceVolLeft = 0x00;
constexpr std::uint8_t kVoiceVolRight = 0x01;
constexpr std::uint8_t kVoicePitchLow = 0x02;
constexpr std::uint8_t kVoicePitchHigh = 0x03;
constexpr std::uint8_t kVoiceSrcn = 0x04;
constexpr std::uint8_t kVoiceAdsr1 = 0x05;
constexpr std::uint8_t kVoiceAdsr2 = 0x06;
constexpr std::uint8_t kVoiceGain = 0x07;
constexpr std::uint8_t kVoiceEnvx = 0x08;
constexpr std::uint8_t kVoiceOutx = 0x09;

// The global counter's power-on / wrap value: it counts down from 0x77FF.
constexpr std::uint16_t kGlobalCounterReload = 0x77FF;

// The envelope/noise rate tables — parser-generated, cross-checked against the
// references (see the generated file's header). kEnvelopePeriod[0] == 0 is the
// 'never' sentinel (rate 0 = Infinite); kEnvelopePeriod[31] == 1 fires every
// sample. kEnvelopeOffset delays each rate's phase within its period.
#include "generated/envelope_counter_tables.inc"

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

// The exponential envelope decrease shared by Decay, Sustain and GAIN
// Exp-Decrease: level -= 1, then level -= level >> 8 (the right shift is
// arithmetic, so the two-step form never drives the level below 0).
[[nodiscard]] int expDecrease(int level) noexcept {
  const int stepped = level - 1;
  return stepped - (stepped >> 8);
}

// Saturates an envelope value to the unsigned 11-bit range 0..0x7FF (the
// hardware clamps rather than wraps: a decrease past 0 sticks at 0, an attack
// past 0x7FF sticks at 0x7FF).
[[nodiscard]] std::uint16_t clampEnvelope(int level) noexcept {
  if (level < 0) return 0;
  if (level > 0x7FF) return 0x7FF;
  return static_cast<std::uint16_t>(level);
}

// The voice's base 14-bit pitch step, read live from VxPITCHL/H; the register
// pair's bits 14-15 are stored but never used.
[[nodiscard]] std::uint32_t voicePitch(const DspState& dsp, std::size_t voice) noexcept {
  const std::uint32_t low = dsp[voiceRegister(voice, kVoicePitchLow)];
  const std::uint32_t high = dsp[voiceRegister(voice, kVoicePitchHigh)];
  return ((high << 8) | low) & 0x3FFF;
}

// The pitch step this output sample, including pitch modulation. For a voice in
// 1..7 with its PMON bit set, the base step is scaled by the previous voice's
// current-sample amplitude: factor = (prevAmplitude SAR 4) + 400h (the range
// 000h..7FFh, i.e. 0.00..1.99), and step = (base * factor) SAR 10. A silent
// previous voice gives factor 400h, an unmodulated x1.0, so no special case is
// needed. The result is capped at the 128 kHz ceiling; the base step never
// reaches it, so the cap applies only to a modulated voice.
[[nodiscard]] std::uint32_t pitchStep(const DspState& dsp, std::size_t voice,
                                      int prevAmplitude) noexcept {
  std::uint32_t step = voicePitch(dsp, voice);
  const bool modulate = voice > 0 && ((dsp[kDspPmon] >> voice) & 1) != 0;
  if (modulate) {
    const int factor = (prevAmplitude >> 4) + 0x400;  // -400h..+3FFh -> 000h..7FFh
    step = static_cast<std::uint32_t>((static_cast<int>(step) * factor) >> 10);
  }
  return step > kMaxPitchStep ? kMaxPitchStep : step;
}

// Advances the shared noise generator one step: a 15-bit right rotation whose new
// top bit is bit0 XOR bit1 of the old level. The level is held as the signed
// sample it outputs (-4000h..+3FFFh), so the step works over its 15-bit pattern.
[[nodiscard]] std::int16_t nextNoiseLevel(std::int16_t level) noexcept {
  const unsigned pattern = static_cast<unsigned>(level) & 0x7FFFu;
  const unsigned feedback = ((pattern ^ (pattern >> 1)) & 1u) << 14;
  const unsigned next = ((pattern >> 1) & 0x3FFFu) | feedback;
  return static_cast<std::int16_t>((next ^ 0x4000u) - 0x4000u);  // sign-extend 15 bits
}

// Advances a voice's decode cursor by one sample. An exhausted block chains to
// the following block — or, after an end block, to the loop address read live
// from DIR and VxSRCN, which is how a DIR or VxSRCN change takes effect at the
// next loop and not before. Entering any block whose header carries the end
// flag sets the voice's ENDX bit — the hardware sets it at the START of
// decoding the end block, not after it. The decoded sample shifts into the
// window; the filter's history is the window's two newest taps.
//
// Returns whether the sample just entered an End+Mute block (header code 1: end
// set, loop clear) — the caller silences the voice and drops its envelope to 0.
// An End+Loop block (code 3) returns false: it loops without muting.
bool decodeStreamSample(DspState& dsp, std::span<const std::uint8_t, 65536> ram,
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
  bool enteredEndMute = false;
  if (v.brrSampleIndex == 0 && (header & 0x01) != 0) {
    dsp[kDspEndx] |= static_cast<std::uint8_t>(1u << voice);
    enteredEndMute = (header & 0x02) == 0;  // end set, loop clear = code 1
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
  return enteredEndMute;
}

// Advances a voice's stream by the samples this 32 kHz output sample passes: the
// pitch counter gains `step`, and every whole sample position it crosses is
// decoded. Returns whether any of those decodes entered an End+Mute block. Shared
// by stepVoice (which reports the interpolated result) and stepDspSample (which
// also needs the mute signal, and supplies a pitch-modulated step).
bool advanceVoiceStream(DspState& dsp, std::span<const std::uint8_t, 65536> ram,
                        std::size_t voice, std::uint32_t step) noexcept {
  VoiceState& v = dsp.voices[voice];
  const std::uint32_t advanced = v.pitchCounter + step;
  const std::uint32_t passed = (advanced >> 12) - (v.pitchCounter >> 12);
  v.pitchCounter = static_cast<std::uint16_t>(advanced);
  bool endMute = false;
  for (std::uint32_t n = 0; n < passed; ++n) endMute |= decodeStreamSample(dsp, ram, voice);
  return endMute;
}

// Clamps a mix accumulator to signed 16 bits — the hardware clamps (never wraps)
// after each addition in the output sum.
[[nodiscard]] std::int32_t clampSigned16(std::int32_t value) noexcept {
  if (value > 0x7FFF) return 0x7FFF;
  if (value < -0x8000) return -0x8000;
  return value;
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
  advanceVoiceStream(dsp, ram, voice, voicePitch(dsp, voice));
  return interpolatedSample(dsp, voice);
}

std::int16_t interpolatedSample(const DspState& dsp, std::size_t voice) noexcept {
  const VoiceState& v = dsp.voices[voice];
  return gaussInterpolate(v.window, static_cast<std::uint8_t>((v.pitchCounter >> 4) & 0xFF));
}

std::uint16_t nextGlobalCounter(std::uint16_t counter) noexcept {
  return counter == 0 ? kGlobalCounterReload : static_cast<std::uint16_t>(counter - 1);
}

bool envelopeRateFires(std::uint16_t counter, std::uint8_t rate) noexcept {
  const int period = kEnvelopePeriod[rate & 0x1F];
  if (period == 0) return false;  // rate 0 (Infinite) never fires
  const int offset = kEnvelopeOffset[rate & 0x1F];
  return (counter + offset) % period == 0;
}

void tickDspSample(DspState& dsp) noexcept {
  dsp.globalCounter = nextGlobalCounter(dsp.globalCounter);
  ++dsp.sampleIndex;
}

StereoFrame stepDspSample(DspState& dsp,
                          std::span<const std::uint8_t, 65536> ram) noexcept {
  pollKeying(dsp, ram);

  const std::uint8_t flg = dsp[kDspFlg];
  const bool softReset = (flg & kFlgSoftReset) != 0;

  std::int32_t left = 0;
  std::int32_t right = 0;
  int prevAmplitude = 0;  // the previous voice's amplitude this sample, for PMON
  for (std::size_t voice = 0; voice < 8; ++voice) {
    VoiceState& v = dsp.voices[voice];
    const std::uint32_t step = pitchStep(dsp, voice, prevAmplitude);

    int amplitude;
    if (softReset) {
      // FLG bit 7 keys every voice off and forces its envelope to 0 each sample.
      // BRR decoding keeps running (ENDX and loop transitions still fire); only
      // the emitted amplitude is silenced. A key-on that fired at this sample's
      // poll starts and is immediately re-silenced, so nothing sounds until the
      // bit clears.
      v.phase = EnvPhase::Release;
      if (v.konDelay > 0)
        --v.konDelay;
      else
        advanceVoiceStream(dsp, ram, voice, step);
      v.envelope = 0;
      dsp[voiceRegister(voice, kVoiceEnvx)] = 0;
      amplitude = 0;
    } else if (v.konDelay > 0) {
      // Startup: the voice is silent, and neither the stream nor the envelope
      // advances past the countdown (stepVoiceEnvelope decrements it).
      stepVoiceEnvelope(dsp, voice, false);
      amplitude = 0;
    } else {
      const bool endMute = advanceVoiceStream(dsp, ram, voice, step);
      // A voice whose NON bit is set outputs the shared noise level in place of
      // its interpolated BRR sample; the stream still advanced above, so decoding
      // and ENDX are unaffected, and neither pitch nor Gaussian interpolation
      // touches noise.
      const bool noise = ((dsp[kDspNon] >> voice) & 1) != 0;
      const int sample = noise ? dsp.noiseLevel : interpolatedSample(dsp, voice);
      const std::uint16_t envelope = stepVoiceEnvelope(dsp, voice, endMute);
      // The envelope scales the sample into the internal -4000h..+3FFFh
      // amplitude — the value VxOUTX reports and the next voice's PMON reads.
      amplitude = (sample * static_cast<int>(envelope)) >> 11;
    }

    // VxOUTX returns the high byte of the 15-bit amplitude (-128..+127).
    dsp[voiceRegister(voice, kVoiceOutx)] = static_cast<std::uint8_t>((amplitude >> 7) & 0xFF);
    prevAmplitude = amplitude;

    // Each channel scales by its signed 8-bit volume and recovers the low bit
    // the BRR decoder dropped: (amplitude * VxVOL) >> 6. The sum clamps to
    // signed 16 bits after every voice.
    const int volLeft = static_cast<std::int8_t>(dsp[voiceRegister(voice, kVoiceVolLeft)]);
    const int volRight = static_cast<std::int8_t>(dsp[voiceRegister(voice, kVoiceVolRight)]);
    left = clampSigned16(left + ((amplitude * volLeft) >> 6));
    right = clampSigned16(right + ((amplitude * volRight) >> 6));
  }

  // Master volume scales the summed mix: sum * MVOL SAR 7. The multiply truncates
  // to 16 bits with no clamp — the one overflowing case (MVOL -128 against a
  // full-scale sum) wraps, matching the hardware. The echo the mixer adds after
  // this is present once the echo unit is enabled.
  const int mvolLeft = static_cast<std::int8_t>(dsp[kDspMvolLeft]);
  const int mvolRight = static_cast<std::int8_t>(dsp[kDspMvolRight]);
  left = static_cast<std::int16_t>((left * mvolLeft) >> 7);
  right = static_cast<std::int16_t>((right * mvolRight) >> 7);

  // FLG bit 6 mutes the emitted frame to silence; every internal mechanism above
  // already ran, so mute stops output only.
  if ((flg & kFlgMute) != 0) {
    left = 0;
    right = 0;
  }

  // Advance the shared noise generator at the FLG noise rate (rate 0 holds it),
  // then tick the per-sample global state — both on the sample boundary.
  if (envelopeRateFires(dsp.globalCounter, flg & kFlgNoiseRate))
    dsp.noiseLevel = nextNoiseLevel(dsp.noiseLevel);
  tickDspSample(dsp);
  return StereoFrame{.left = static_cast<std::int16_t>(left),
                     .right = static_cast<std::int16_t>(right)};
}

void keyOnVoice(DspState& dsp, std::span<const std::uint8_t, 65536> ram,
                std::size_t voice) noexcept {
  startVoice(dsp, ram, voice);  // primes the stream and resets the voice state
  VoiceState& v = dsp.voices[voice];
  v.phase = EnvPhase::Attack;
  v.konDelay = 5;
  // Key-on clears this voice's ENDX bit; a same-sample end-block set does not
  // override the clear, so clearing after the priming decode is correct.
  dsp[kDspEndx] &= static_cast<std::uint8_t>(~(1u << voice));
}

void keyOffVoice(DspState& dsp, std::size_t voice) noexcept {
  dsp.voices[voice].phase = EnvPhase::Release;
}

void pollKeying(DspState& dsp, std::span<const std::uint8_t, 65536> ram) noexcept {
  // The poll runs on even-indexed samples counted from power-on — a fixed
  // choice that settles the hardware's probabilistic power-on poll phase.
  if (dsp.sampleIndex % 2 != 0) return;

  const std::uint8_t kon = dsp[kDspKon];
  const std::uint8_t koff = dsp[kDspKoff];
  for (std::size_t voice = 0; voice < 8; ++voice) {
    const std::uint8_t bit = static_cast<std::uint8_t>(1u << voice);
    if ((koff & bit) != 0) keyOffVoice(dsp, voice);
    // KON is applied after KOFF, so a voice with both bits set keys on.
    if ((kon & bit) != 0) keyOnVoice(dsp, ram, voice);
  }
  // Load the internal-KON latch; the previous poll's latch (about two samples
  // old) is replaced here.
  dsp.internalKon = kon;
}

std::uint16_t stepVoiceEnvelope(DspState& dsp, std::size_t voice, bool brrEndMute) noexcept {
  VoiceState& v = dsp.voices[voice];
  const auto writeEnvx = [&] {
    dsp[voiceRegister(voice, kVoiceEnvx)] = static_cast<std::uint8_t>(v.envelope >> 4);
  };

  // Post-key-on startup: five silent samples during which the level holds at 0
  // (set at key-on) and neither the envelope nor the stream advances.
  if (v.konDelay > 0) {
    --v.konDelay;
    writeEnvx();
    return v.envelope;
  }

  // A BRR End+Mute block moves the voice to Release and drops the level to 0
  // immediately (the documented -0x800 step reaches 0 from any level).
  if (brrEndMute) {
    v.phase = EnvPhase::Release;
    v.envelope = 0;
    v.bentGainRef = 0;
    writeEnvx();
    return 0;
  }

  const std::uint8_t adsr1 = dsp[voiceRegister(voice, kVoiceAdsr1)];
  const std::uint8_t adsr2 = dsp[voiceRegister(voice, kVoiceAdsr2)];
  const std::uint8_t gain = dsp[voiceRegister(voice, kVoiceGain)];
  const int level = v.envelope;

  int newLevel = level;
  bool fires = false;

  if (v.phase == EnvPhase::Release) {
    fires = true;  // Release runs every sample (rate 31)
    newLevel = level - 8;
  } else if ((adsr1 & 0x80) != 0) {
    switch (v.phase) {
      case EnvPhase::Attack: {
        const auto rate = static_cast<std::uint8_t>((adsr1 & 0x0F) * 2 + 1);
        fires = envelopeRateFires(dsp.globalCounter, rate);
        if (fires) newLevel = level + ((adsr1 & 0x0F) == 0x0F ? 1024 : 32);
        break;
      }
      case EnvPhase::Decay: {
        const auto rate = static_cast<std::uint8_t>(((adsr1 >> 4) & 0x07) * 2 + 16);
        fires = envelopeRateFires(dsp.globalCounter, rate);
        if (fires) newLevel = expDecrease(level);
        break;
      }
      case EnvPhase::Sustain: {
        fires = envelopeRateFires(dsp.globalCounter, static_cast<std::uint8_t>(adsr2 & 0x1F));
        if (fires) newLevel = expDecrease(level);
        break;
      }
      case EnvPhase::Release:
        break;  // handled above
    }
  } else if ((gain & 0x80) == 0) {
    fires = true;  // Direct Gain: a fixed level, no rate
    newLevel = (gain & 0x7F) << 4;
  } else {
    fires = envelopeRateFires(dsp.globalCounter, static_cast<std::uint8_t>(gain & 0x1F));
    if (fires) {
      switch ((gain >> 5) & 0x03) {
        case 0: newLevel = level - 32; break;         // Linear Decrease
        case 1: newLevel = expDecrease(level); break;  // Exp Decrease
        case 2: newLevel = level + 32; break;          // Linear Increase
        default:                                       // Bent Increase
          newLevel = level + (v.bentGainRef < 0x600 ? 32 : 8);
          break;
      }
    }
  }

  if (!fires) {
    writeEnvx();
    return v.envelope;
  }

  // The Attack->Decay switch needs the pre-clamp value to exceed 0x7FF (Anomie):
  // an attack reaching 0x800 clips to 0x7FF and enters Decay. The Bent-Increase
  // reference is the new value clipped, not clamped, to 11 bits.
  const bool attackToDecay = (v.phase == EnvPhase::Attack) && (newLevel > 0x7FF);
  v.bentGainRef = static_cast<std::uint16_t>(newLevel & 0x7FF);
  v.envelope = clampEnvelope(newLevel);

  // Decay->Sustain when the level's upper 3 bits reach the sustain level.
  if (v.phase == EnvPhase::Decay) {
    if ((v.envelope >> 8) == ((adsr2 >> 5) & 0x07)) v.phase = EnvPhase::Sustain;
  }
  if (attackToDecay) v.phase = EnvPhase::Decay;

  writeEnvx();
  return v.envelope;
}

}  // namespace snaggletooth
