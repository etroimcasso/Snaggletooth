#include "snaggletooth/apu/dsp.h"

namespace snaggletooth {

namespace {

// DSP register addresses the voice stream reads and writes. Per-voice
// registers live at voice*10h plus the per-voice offset.
constexpr std::uint8_t kDspMvolLeft = 0x0C;
constexpr std::uint8_t kDspMvolRight = 0x1C;
constexpr std::uint8_t kDspEfb = 0x0D;
constexpr std::uint8_t kDspEvolLeft = 0x2C;
constexpr std::uint8_t kDspEvolRight = 0x3C;
constexpr std::uint8_t kDspPmon = 0x2D;
constexpr std::uint8_t kDspNon = 0x3D;
constexpr std::uint8_t kDspEon = 0x4D;
constexpr std::uint8_t kDspKoff = 0x5C;
constexpr std::uint8_t kDspDir = 0x5D;
constexpr std::uint8_t kDspFlg = 0x6C;
constexpr std::uint8_t kDspEsa = 0x6D;
constexpr std::uint8_t kDspEndx = 0x7C;
constexpr std::uint8_t kDspEdl = 0x7D;

// FLG bit masks: bits 0-4 are the noise rate, bit 5 disables echo writes, bit 6
// mutes the output amplifier, bit 7 is the per-sample soft reset.
constexpr std::uint8_t kFlgNoiseRate = 0x1F;
constexpr std::uint8_t kFlgEchoWriteDisable = 0x20;
constexpr std::uint8_t kFlgMute = 0x40;
constexpr std::uint8_t kFlgSoftReset = 0x80;

// The per-voice offset of the echo FIR coefficient registers: FIR0..FIR7 live at
// $0F, $1F, ... $7F (tap*10h + this), each a signed 8-bit coefficient.
constexpr std::uint8_t kFirCoeff = 0x0F;

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

// The startup countdown a key-on arms. Its first call is the sample after the
// keying poll's own, which loads the start address and makes no header check.
inline constexpr std::uint8_t kKeyOnStartupCalls = 4;

// A BRR header's low two bits are its end/loop code. Code 1 — end set, loop clear
// — is End+Mute: the voice releases and its level drops to 0. Code 3 loops without
// muting, and codes 0 and 2 run on into the next block.
[[nodiscard]] bool headerIsEndMute(std::uint8_t header) noexcept {
  return (header & 0x03) == 0x01;
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
    enteredEndMute = headerIsEndMute(header);
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

// The echo unit's per-channel FIR output, added into the main mix through EVOL.
struct EchoOutput {
  int left = 0;
  int right = 0;
};

// Advances the echo unit one 32 kHz sample and returns its FIR output for the two
// channels. It reads the oldest 4-byte ring entry (based at ESA*100h, offset by the
// ring index) into the per-channel FIR history, runs the 8-tap filter over the last
// eight entries, and — when echoRam is non-null and FLG bit 5 is clear — mixes the
// EON send (sendLeft/sendRight, the EON-enabled voices' post-VxVOL sums) with the
// FIR feedback (EFB) and writes the result back over the entry it read. The ring
// index and FIR history advance every sample regardless of the write, so a
// read-only caller (echoRam null) sees the static-buffer behaviour FLG bit 5
// produces. The two channels filter separately with the same coefficients.
EchoOutput stepEcho(DspState& dsp, std::span<const std::uint8_t, 65536> ram,
                    std::uint8_t* echoRam, int sendLeft, int sendRight) noexcept {
  // EDL is consulted only when the ring index is 0: latch the entry count there.
  // EDL<<9 entries; EDL 0 latches a count of 0, which the wrap rule below turns
  // into a single reused entry, and an EDL change is not seen until the next wrap.
  if (dsp.echoIndex == 0)
    dsp.echoLength = static_cast<std::uint16_t>((dsp[kDspEdl] & 0x0F) << 9);

  const std::uint16_t base = static_cast<std::uint16_t>(dsp[kDspEsa] * 0x100);
  const std::uint16_t entry = static_cast<std::uint16_t>(base + dsp.echoIndex * 4);
  const auto at = [&](int offset) -> std::uint16_t {
    return static_cast<std::uint16_t>(entry + offset);  // wraps within the 64KB space
  };

  // Read the entry: a 16-bit little-endian left sample then right, each stored with
  // bit 0 cleared; the arithmetic SAR 1 recovers the 15-bit value into the FIR
  // history's newest slot.
  const auto sample16 = [&](int lowByte) -> int {
    return static_cast<std::int16_t>(ram[at(lowByte)] | (ram[at(lowByte + 1)] << 8));
  };
  dsp.echoFirLeft[dsp.echoFirPos] = static_cast<std::int16_t>(sample16(0) >> 1);
  dsp.echoFirRight[dsp.echoFirPos] = static_cast<std::int16_t>(sample16(2) >> 1);

  // FIR: taps oldest*FIR0 ... newest*FIR7, each product SAR 6. The first seven
  // additions wrap at 16 bits; only the final (newest) addition saturates. The
  // newest history slot is echoFirPos, so tap k reads slot (echoFirPos+1+k) & 7.
  // The output is a 15-bit sample left-aligned in 16 bits, so the sum's low bit
  // is dropped before EVOL and EFB read it.
  const auto filter = [&](const std::array<std::int16_t, 8>& history) -> int {
    int sum = 0;
    for (int tap = 0; tap < 8; ++tap) {
      const std::uint8_t slot = static_cast<std::uint8_t>((dsp.echoFirPos + 1 + tap) & 7);
      const int coeff =
          static_cast<std::int8_t>(dsp[voiceRegister(static_cast<std::size_t>(tap), kFirCoeff)]);
      const int product = (history[slot] * coeff) >> 6;
      sum = tap < 7 ? static_cast<std::int16_t>(sum + product) : clampSigned16(sum + product);
    }
    return sum & ~1;
  };
  const int firLeft = filter(dsp.echoFirLeft);
  const int firRight = filter(dsp.echoFirRight);

  // Feedback: the EON send plus fir*EFB SAR 7, clamped, bit 0 cleared, written back
  // over the entry — unless echo writes are disabled or the caller passed no
  // writable RAM. The entry holds a 15-bit sample, so the write drops the low bit
  // the volume multiply can reintroduce.
  const bool writeEnabled = echoRam != nullptr && (dsp[kDspFlg] & kFlgEchoWriteDisable) == 0;
  if (writeEnabled) {
    const int efb = static_cast<std::int8_t>(dsp[kDspEfb]);
    const int writeLeft = clampSigned16(sendLeft + ((firLeft * efb) >> 7)) & ~1;
    const int writeRight = clampSigned16(sendRight + ((firRight * efb) >> 7)) & ~1;
    // The bytes do not land here: the buffer writes have their own slots — the
    // left word at T30, the right word at T31 (both references' access charts) —
    // so the value computed now is latched and the slot runner (or the
    // frame-at-once caller) performs the write when its slot arrives.
    dsp.echoWritePending = true;
    dsp.echoWriteEntry = entry;
    dsp.echoWriteBytes[0] = static_cast<std::uint8_t>(writeLeft & 0xFF);
    dsp.echoWriteBytes[1] = static_cast<std::uint8_t>((writeLeft >> 8) & 0xFF);
    dsp.echoWriteBytes[2] = static_cast<std::uint8_t>(writeRight & 0xFF);
    dsp.echoWriteBytes[3] = static_cast<std::uint8_t>((writeRight >> 8) & 0xFF);
  }

  // Advance the FIR history cursor and the ring index; wrap the index at the
  // latched length (a count of 0 collapses to the single-entry buffer).
  dsp.echoFirPos = static_cast<std::uint8_t>((dsp.echoFirPos + 1) & 7);
  dsp.echoIndex = static_cast<std::uint16_t>(dsp.echoIndex + 1);
  if (dsp.echoIndex >= dsp.echoLength) dsp.echoIndex = 0;

  return EchoOutput{.left = firLeft, .right = firRight};
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

// One voice's per-sample body: advance it by one 32 kHz output sample and return
// its enveloped amplitude — the internal -4000h..+3FFFh value VxOUTX reports and
// the next voice's PMON reads. `prevAmplitude` is the previous voice's amplitude
// this sample (for pitch modulation); `softReset` is FLG bit 7. Writes VxOUTX;
// advances the stream and the envelope (or the key-on countdown). Extracted so the
// frame-at-once path and the per-slot schedule compute a voice bit-for-bit alike.
static int computeVoiceAmplitude(DspState& dsp, std::span<const std::uint8_t, 65536> ram,
                                 std::size_t voice, int prevAmplitude, bool softReset) noexcept {
  VoiceState& v = dsp.voices[voice];
  const std::uint32_t step = pitchStep(dsp, voice, prevAmplitude);

  // The header byte is loaded from RAM every sample, whatever the pitch counter is
  // doing, and its end/loop bits are read the same sample. So a block that turns
  // End+Mute under a voice whose stream has stopped still releases it, which the
  // decode below cannot see — it reports only a header it reached.
  //
  // The check goes live two sample periods after the keying poll loads a key-on.
  // The poll sits at the last slot of a sample, and that is also where voice 0
  // reads its header, so voice 0 crosses that point on its second compute after
  // the load while the voices reading earlier in the sample cross it on their
  // third. The counter stops at the fifth compute, which is past both.
  const int computeSinceKeyOn = kKeyOnStartupCalls + 1 - v.konDelay;
  const bool headerEndMute = computeSinceKeyOn >= (voice == 0 ? 2 : 3) &&
                             headerIsEndMute(ram[v.brrAddress]);

  int amplitude;
  if (softReset) {
    // FLG bit 7 keys every voice off and forces its envelope to 0 each sample. BRR
    // decoding keeps running (ENDX and loop transitions still fire); only the
    // emitted amplitude is silenced.
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
    // advances past the countdown (stepVoiceEnvelope decrements it). The header
    // check still runs — the hardware preloads BRR groups through these samples.
    stepVoiceEnvelope(dsp, voice, headerEndMute);
    amplitude = 0;
  } else {
    const bool endMute = advanceVoiceStream(dsp, ram, voice, step) || headerEndMute;
    // A voice whose NON bit is set outputs the shared noise level in place of its
    // interpolated BRR sample; the stream still advanced above, so decoding and
    // ENDX are unaffected.
    const bool noise = ((dsp[kDspNon] >> voice) & 1) != 0;
    const int sample = noise ? dsp.noiseLevel : interpolatedSample(dsp, voice);
    const std::uint16_t envelope = stepVoiceEnvelope(dsp, voice, endMute);
    amplitude = (sample * static_cast<int>(envelope)) >> 11;
  }

  // VxOUTX returns the high byte of the 15-bit amplitude (-128..+127).
  dsp[voiceRegister(voice, kVoiceOutx)] = static_cast<std::uint8_t>((amplitude >> 7) & 0xFF);
  return amplitude;
}

// Folds one voice's amplitude into the running left mix and the EON echo send
// through its left volume (the S4 slot's work), clamping to signed 16 bits.
static void applyVoiceLeft(DspState& dsp, std::size_t voice) noexcept {
  const int vol = static_cast<std::int8_t>(dsp[voiceRegister(voice, kVoiceVolLeft)]);
  const int send = (dsp.voiceAmplitude[voice] * vol) >> 6;
  dsp.mixLeft = clampSigned16(dsp.mixLeft + send);
  if (((dsp[kDspEon] >> voice) & 1) != 0)
    dsp.echoSendLeft = clampSigned16(dsp.echoSendLeft + send);
}

// The S5 slot's work — the same fold for the right channel.
static void applyVoiceRight(DspState& dsp, std::size_t voice) noexcept {
  const int vol = static_cast<std::int8_t>(dsp[voiceRegister(voice, kVoiceVolRight)]);
  const int send = (dsp.voiceAmplitude[voice] * vol) >> 6;
  dsp.mixRight = clampSigned16(dsp.mixRight + send);
  if (((dsp[kDspEon] >> voice) & 1) != 0)
    dsp.echoSendRight = clampSigned16(dsp.echoSendRight + send);
}

// The frame-at-once pipeline, run for a freshly seeded (unprimed) DspState's first
// sample. An unprimed state has no prepared voice-0 output, so its first sample
// reads every register at once and every voice in-frame — byte-identical to a
// machine with no intra-sample schedule — then primes the schedule for the samples
// that follow. echoRam is the machine RAM the echo unit writes into, or null for a
// read-only caller (the FLG bit 5 case).
static StereoFrame stepDspSampleAtomic(DspState& dsp, std::span<const std::uint8_t, 65536> ram,
                                       std::uint8_t* echoRam) noexcept {
  pollKeying(dsp, ram);

  const std::uint8_t flg = dsp[kDspFlg];
  const bool softReset = (flg & kFlgSoftReset) != 0;

  std::int32_t left = 0;
  std::int32_t right = 0;
  std::int32_t echoLeft = 0;
  std::int32_t echoRight = 0;
  int prevAmplitude = 0;
  for (std::size_t voice = 0; voice < 8; ++voice) {
    const int amplitude = computeVoiceAmplitude(dsp, ram, voice, prevAmplitude, softReset);
    // Record each voice's amplitude: voice 0's becomes the value the following
    // slot-scheduled frame applies (its output rides one frame behind), so the
    // first sample's voice-0 advance is not repeated. Seed the prepared OUTX/ENVX
    // too, so the first slot-scheduled frame's visibility slots publish this
    // sample's values rather than an empty scratch.
    dsp.voiceAmplitude[voice] = amplitude;
    dsp.preparedOutx[voice] = dsp[voiceRegister(voice, kVoiceOutx)];
    dsp.preparedEnvx[voice] = dsp[voiceRegister(voice, kVoiceEnvx)];
    prevAmplitude = amplitude;
    const int volLeft = static_cast<std::int8_t>(dsp[voiceRegister(voice, kVoiceVolLeft)]);
    const int volRight = static_cast<std::int8_t>(dsp[voiceRegister(voice, kVoiceVolRight)]);
    const int sendLeft = (amplitude * volLeft) >> 6;
    const int sendRight = (amplitude * volRight) >> 6;
    left = clampSigned16(left + sendLeft);
    right = clampSigned16(right + sendRight);
    if (((dsp[kDspEon] >> voice) & 1) != 0) {
      echoLeft = clampSigned16(echoLeft + sendLeft);
      echoRight = clampSigned16(echoRight + sendRight);
    }
  }

  const int mvolLeft = static_cast<std::int8_t>(dsp[kDspMvolLeft]);
  const int mvolRight = static_cast<std::int8_t>(dsp[kDspMvolRight]);
  left = static_cast<std::int16_t>((left * mvolLeft) >> 7);
  right = static_cast<std::int16_t>((right * mvolRight) >> 7);

  const EchoOutput echo = stepEcho(dsp, ram, echoRam, echoLeft, echoRight);
  // The frame-at-once path delivers a whole sample in one call, so the latched
  // echo write lands now — within-sample slot timing has no meaning here.
  if (dsp.echoWritePending && echoRam != nullptr) {
    for (int b = 0; b < 4; ++b)
      echoRam[static_cast<std::uint16_t>(dsp.echoWriteEntry + b)] = dsp.echoWriteBytes[b];
  }
  dsp.echoWritePending = false;
  const int evolLeft = static_cast<std::int8_t>(dsp[kDspEvolLeft]);
  const int evolRight = static_cast<std::int8_t>(dsp[kDspEvolRight]);
  left = clampSigned16(left + ((echo.left * evolLeft) >> 7));
  right = clampSigned16(right + ((echo.right * evolRight) >> 7));

  if ((flg & kFlgMute) != 0) {
    left = 0;
    right = 0;
  }

  if (envelopeRateFires(dsp.globalCounter, flg & kFlgNoiseRate))
    dsp.noiseLevel = nextNoiseLevel(dsp.noiseLevel);
  tickDspSample(dsp);
  return StereoFrame{.left = static_cast<std::int16_t>(left),
                     .right = static_cast<std::int16_t>(right)};
}

// The per-voice schedule. s3Slot is where a voice runs its whole compute body;
// s4Slot/s5Slot are where its left/right volume folds into the mix. Voice 0's
// compute sits at slot T31 and feeds the following frame's output at T0..T5 — the
// one-update lag the S-DSP's envelope pipeline produces.
constexpr std::array<std::uint8_t, 8> kVoiceS3Slot = {31, 2, 5, 8, 11, 14, 17, 20};
constexpr std::array<std::uint8_t, 8> kVoiceS4Slot = {0, 3, 6, 9, 12, 15, 18, 21};
constexpr std::array<std::uint8_t, 8> kVoiceS5Slot = {1, 4, 7, 10, 13, 16, 19, 22};
// The two-phase visibility slots: VxOUTX becomes readable at S8, VxENVX at S9.
constexpr std::array<std::uint8_t, 8> kVoiceS8Slot = {4, 7, 10, 13, 16, 19, 22, 25};
constexpr std::array<std::uint8_t, 8> kVoiceS9Slot = {5, 8, 11, 14, 17, 20, 23, 26};

// Computes voice `voice` at its S3 slot, storing the amplitude the S4/S5 slots
// apply. voice n reads voice n-1's stored amplitude for pitch modulation; voice 0
// reads voice 7's (from this frame — it does not modulate, so the value is inert).
// VxOUTX and VxENVX are computed here but held back from the register file until
// the voice's S8/S9 slots — a CPU read before then sees the previous sample's
// value, the overwrite window the hardware exposes.
static void computeVoiceSlot(DspState& dsp, std::span<const std::uint8_t, 65536> ram,
                             std::size_t voice, bool softReset) noexcept {
  const int prev = dsp.voiceAmplitude[(voice + 7) & 7];
  const std::uint8_t heldOutx = dsp[voiceRegister(voice, kVoiceOutx)];
  const std::uint8_t heldEnvx = dsp[voiceRegister(voice, kVoiceEnvx)];
  dsp.voiceAmplitude[voice] = computeVoiceAmplitude(dsp, ram, voice, prev, softReset);
  dsp.preparedOutx[voice] = dsp[voiceRegister(voice, kVoiceOutx)];
  dsp.preparedEnvx[voice] = dsp[voiceRegister(voice, kVoiceEnvx)];
  dsp[voiceRegister(voice, kVoiceOutx)] = heldOutx;
  dsp[voiceRegister(voice, kVoiceEnvx)] = heldEnvx;
}

// Finalizes the left output (slot T27): the dry mix scaled by MVOLL, the echo FIR
// output added through EVOLL, and the mute gate — the same arithmetic the
// frame-at-once path runs, split by channel so MVOLL is consumed one slot before
// MVOLR.
static void finalizeLeft(DspState& dsp) noexcept {
  const int mvol = static_cast<std::int8_t>(dsp[kDspMvolLeft]);
  const int evol = static_cast<std::int8_t>(dsp[kDspEvolLeft]);
  std::int32_t out = static_cast<std::int16_t>((dsp.mixLeft * mvol) >> 7);
  out = clampSigned16(out + ((dsp.echoFirOutLeft * evol) >> 7));
  if ((dsp[kDspFlg] & kFlgMute) != 0) out = 0;
  dsp.slotFrame.left = static_cast<std::int16_t>(out);
}

// Finalizes the right output (slot T28): MVOLR, EVOLR and the mute gate.
static void finalizeRight(DspState& dsp) noexcept {
  const int mvol = static_cast<std::int8_t>(dsp[kDspMvolRight]);
  const int evol = static_cast<std::int8_t>(dsp[kDspEvolRight]);
  std::int32_t out = static_cast<std::int16_t>((dsp.mixRight * mvol) >> 7);
  out = clampSigned16(out + ((dsp.echoFirOutRight * evol) >> 7));
  if ((dsp[kDspFlg] & kFlgMute) != 0) out = 0;
  dsp.slotFrame.right = static_cast<std::int16_t>(out);
}

// Runs one 32-clock slot of a primed sample. Each register read lands on its
// documented slot, so a mid-frame DSPDATA write is seen only if it precedes its
// consuming slot; every no-write sample reproduces the frame-at-once output for
// voices 1-7, and voice 0 rides one update behind.
static void runPrimedSlot(DspState& dsp, std::span<const std::uint8_t, 65536> ram,
                          std::uint8_t* echoRam, std::uint8_t slot) noexcept {
  const bool softReset = (dsp[kDspFlg] & kFlgSoftReset) != 0;

  if (slot == 0) {
    // Frame start: clear the mix the previous frame delivered, then apply voice 0's
    // amplitude — prepared at the last frame's T31 (the pipeline lag).
    dsp.mixLeft = 0;
    dsp.mixRight = 0;
    dsp.echoSendLeft = 0;
    dsp.echoSendRight = 0;
  }

  // Voice compute at each voice's S3 slot (voice 0's is T31, handled in the tail).
  for (std::size_t voice = 1; voice < 8; ++voice)
    if (kVoiceS3Slot[voice] == slot) computeVoiceSlot(dsp, ram, voice, softReset);

  // Voice volume folds at the S4 (left) and S5 (right) slots, in voice order; the
  // computed OUTX/ENVX bytes become readable at the S8/S9 slots.
  for (std::size_t voice = 0; voice < 8; ++voice) {
    if (kVoiceS4Slot[voice] == slot) applyVoiceLeft(dsp, voice);
    if (kVoiceS5Slot[voice] == slot) applyVoiceRight(dsp, voice);
    if (kVoiceS8Slot[voice] == slot) dsp[voiceRegister(voice, kVoiceOutx)] = dsp.preparedOutx[voice];
    if (kVoiceS9Slot[voice] == slot) dsp[voiceRegister(voice, kVoiceEnvx)] = dsp.preparedEnvx[voice];
  }

  switch (slot) {
    case 24: {
      // The echo unit reads its buffer, filters, and writes back its feedback —
      // the EON sends are all folded by T22, so the whole unit runs here and its
      // FIR output is held for the output slots.
      const EchoOutput echo = stepEcho(dsp, ram, echoRam, dsp.echoSendLeft, dsp.echoSendRight);
      dsp.echoFirOutLeft = echo.left;
      dsp.echoFirOutRight = echo.right;
      break;
    }
    case 27:
      finalizeLeft(dsp);
      break;
    case 28:
      finalizeRight(dsp);
      break;
    case 30:
      // The left echo word lands at its write slot, T30 — the value was computed
      // when the echo unit ran at T24 and held since.
      if (dsp.echoWritePending && echoRam != nullptr) {
        echoRam[dsp.echoWriteEntry] = dsp.echoWriteBytes[0];
        echoRam[static_cast<std::uint16_t>(dsp.echoWriteEntry + 1)] = dsp.echoWriteBytes[1];
      }
      break;
    case 31: {
      // Voice 0's compute runs before the counter, noise and keying updates that
      // share this slot, so its envelope/noise/keying inputs are one update older
      // than voices 1-7's — the hardware's envelope pipeline.
      computeVoiceSlot(dsp, ram, 0, softReset);
      // The right echo word lands at its write slot, T31.
      if (dsp.echoWritePending && echoRam != nullptr) {
        echoRam[static_cast<std::uint16_t>(dsp.echoWriteEntry + 2)] = dsp.echoWriteBytes[2];
        echoRam[static_cast<std::uint16_t>(dsp.echoWriteEntry + 3)] = dsp.echoWriteBytes[3];
      }
      dsp.echoWritePending = false;
      // KON/KOFF load here, keeping the even-sample parity (the poll reads the
      // pre-tick sample index, as the frame-at-once entry did); the keyed voices'
      // next compute is the following frame's, so keying lands one frame later
      // than the frame-at-once model and voice 0 one load behind voices 1-7.
      pollKeying(dsp, ram);
      if (envelopeRateFires(dsp.globalCounter, dsp[kDspFlg] & kFlgNoiseRate))
        dsp.noiseLevel = nextNoiseLevel(dsp.noiseLevel);
      tickDspSample(dsp);
      break;
    }
    default:
      break;
  }
}

// Runs one slot. An unprimed state runs its whole first sample at the wrap slot
// T31 — the same cycle the frame-at-once model delivered on, so a write during the
// first sample's slots reaches it exactly as before — then primes the schedule for
// voice 0; every later sample is slot-scheduled.
static SlotResult stepDspCycleImpl(DspState& dsp, std::span<const std::uint8_t, 65536> ram,
                                   std::uint8_t* echoRam) noexcept {
  const std::uint8_t slot = dsp.slotCursor;

  if (!dsp.primed) {
    if (slot == 31) {
      // The frame-at-once path advances voice 0 exactly once and records its
      // amplitude in voiceAmplitude[0]; the next (slot-scheduled) frame applies
      // that value at T0..T5, so voice 0's output rides one frame behind while its
      // state trajectory stays the frame-at-once one.
      dsp.slotFrame = stepDspSampleAtomic(dsp, ram, echoRam);
      dsp.primed = true;
    }
  } else {
    runPrimedSlot(dsp, ram, echoRam, slot);
  }

  dsp.slotCursor = static_cast<std::uint8_t>((slot + 1) & 31);
  SlotResult result;
  if (dsp.slotCursor == 0) {
    result.frame = dsp.slotFrame;
    result.delivered = true;  // the sample's 32 slots are complete
  }
  return result;
}

SlotResult stepDspCycle(DspState& dsp, std::span<std::uint8_t, 65536> ram) noexcept {
  return stepDspCycleImpl(dsp, ram, ram.data());
}

SlotResult stepDspCycle(DspState& dsp, std::span<const std::uint8_t, 65536> ram) noexcept {
  return stepDspCycleImpl(dsp, ram, nullptr);
}

// Runs a whole sample's 32 slots and returns the frame they finalize. The machine
// drives the DSP one slot per cycle instead (stepDspCycle), delivering the frame at
// the wrap; a standalone caller uses this.
static StereoFrame stepDspSampleLoop(DspState& dsp, std::span<const std::uint8_t, 65536> ram,
                                     std::uint8_t* echoRam) noexcept {
  StereoFrame frame{};
  for (int n = 0; n < 32; ++n) {
    const SlotResult result = stepDspCycleImpl(dsp, ram, echoRam);
    if (result.delivered) frame = result.frame;
  }
  return frame;
}

StereoFrame stepDspSample(DspState& dsp, std::span<std::uint8_t, 65536> ram) noexcept {
  return stepDspSampleLoop(dsp, ram, ram.data());
}

StereoFrame stepDspSample(DspState& dsp,
                          std::span<const std::uint8_t, 65536> ram) noexcept {
  return stepDspSampleLoop(dsp, ram, nullptr);
}

void keyOnVoice(DspState& dsp, std::span<const std::uint8_t, 65536> ram,
                std::size_t voice) noexcept {
  startVoice(dsp, ram, voice);  // primes the stream and resets the voice state
  VoiceState& v = dsp.voices[voice];
  v.phase = EnvPhase::Attack;
  // The startup countdown. The keying poll's own sample is the first of the five
  // silent startup samples, and it takes no envelope call for the keyed voice
  // (voice 0's compute at that slot runs before the poll; voices 1-7 compute in
  // later slots of the following samples), so the counter covers the remaining
  // four: four silent calls, then the fifth call after the load takes the first
  // envelope step. A count of five put the first step one sample late, measured
  // against the spc_dsp6 key-on tests.
  v.konDelay = kKeyOnStartupCalls;
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

  // KOFF is read from the register, which exerts its influence at every poll
  // for as long as the bit stays set. KON is read from the internal value a
  // write arms, and the poll clears it — so a key-on happens once per write,
  // however long the register keeps the bit.
  const std::uint8_t kon = dsp.internalKon;
  const std::uint8_t koff = dsp[kDspKoff];
  for (std::size_t voice = 0; voice < 8; ++voice) {
    const std::uint8_t bit = static_cast<std::uint8_t>(1u << voice);
    if ((koff & bit) != 0) keyOffVoice(dsp, voice);
    // KON is applied after KOFF, so a voice with both bits set keys on.
    if ((kon & bit) != 0) keyOnVoice(dsp, ram, voice);
  }
  dsp.internalKon = 0;
}

std::uint16_t stepVoiceEnvelope(DspState& dsp, std::size_t voice, bool brrEndMute) noexcept {
  VoiceState& v = dsp.voices[voice];
  const auto writeEnvx = [&] {
    dsp[voiceRegister(voice, kVoiceEnvx)] = static_cast<std::uint8_t>(v.envelope >> 4);
  };

  // A BRR End+Mute block moves the voice to Release and drops the level to 0
  // immediately (the documented -0x800 step reaches 0 from any level). It is read
  // before the startup countdown below, because the header check that raises it
  // goes live while the countdown still has samples left to run.
  if (brrEndMute) {
    v.phase = EnvPhase::Release;
    v.envelope = 0;
    v.bentGainRef = 0;
    if (v.konDelay > 0) --v.konDelay;
    writeEnvx();
    return 0;
  }

  // Post-key-on startup: the level holds at 0 (set at key-on) and neither the
  // envelope nor the stream advances while the countdown runs. The call that
  // takes the counter to 0 is itself silent, so a fresh key-on yields four
  // silent calls before the first live step (see keyOnVoice for the count).
  if (v.konDelay > 0) {
    --v.konDelay;
    writeEnvx();
    return v.envelope;
  }

  const std::uint8_t adsr1 = dsp[voiceRegister(voice, kVoiceAdsr1)];
  const std::uint8_t adsr2 = dsp[voiceRegister(voice, kVoiceAdsr2)];
  const std::uint8_t gain = dsp[voiceRegister(voice, kVoiceGain)];
  const int level = v.envelope;

  // The selected mode computes a candidate level every sample. The rate counter
  // decides only whether the envelope takes that candidate; the Attack->Decay
  // switch below reads it either way, which is what lets a voice change phase
  // while a rate of 0 holds its level still.
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
        newLevel = level + ((adsr1 & 0x0F) == 0x0F ? 1024 : 32);
        break;
      }
      case EnvPhase::Decay: {
        const auto rate = static_cast<std::uint8_t>(((adsr1 >> 4) & 0x07) * 2 + 16);
        fires = envelopeRateFires(dsp.globalCounter, rate);
        newLevel = expDecrease(level);
        break;
      }
      case EnvPhase::Sustain: {
        fires = envelopeRateFires(dsp.globalCounter, static_cast<std::uint8_t>(adsr2 & 0x1F));
        newLevel = expDecrease(level);
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
    switch ((gain >> 5) & 0x03) {
      case 0: newLevel = level - 32; break;          // Linear Decrease
      case 1: newLevel = expDecrease(level); break;  // Exp Decrease
      case 2: newLevel = level + 32; break;          // Linear Increase
      default:                                       // Bent Increase
        newLevel = level + (v.bentGainRef < 0x600 ? 32 : 8);
        break;
    }
  }

  // The Attack->Decay switch fires when the candidate leaves the unsigned 11-bit
  // range: an attack overshooting 0x7FF, or a decreasing mode driven below zero.
  // The level itself is clamped, so the overshoot is only visible here.
  const bool attackToDecay = (v.phase == EnvPhase::Attack) && ((newLevel & ~0x7FF) != 0);

  // The boundary Decay->Sustain compares against comes from whichever register
  // is driving the envelope: ADSR2's sustain level under ADSR, and GAIN's top
  // three bits while a GAIN mode drives the level. Those bits are the gain
  // mode and its enable, not a sustain level, so a voice keyed on under GAIN
  // reaches Sustain at a boundary its author never chose.
  const int sustainBoundary =
      (adsr1 & 0x80) != 0 ? ((adsr2 >> 5) & 0x07) : ((gain >> 5) & 0x07);

  // Decay->Sustain reads the candidate, and reads it every sample: a voice
  // parked in a rate-0 mode never takes the value, yet still changes phase when
  // the value the mode computes reaches the boundary. A candidate outside the
  // 11-bit range is the Attack->Decay case above and matches no boundary.
  const bool decayToSustain = (v.phase == EnvPhase::Decay) && ((newLevel & ~0x7FF) == 0) &&
                              ((newLevel >> 8) == sustainBoundary);

  // The counter decides only whether the level takes the candidate.
  if (fires) v.envelope = clampEnvelope(newLevel);

  // The Bent-Increase reference is the value the mode computes, saved every
  // sample whether or not the counter fires — so a voice parked in a rate-0
  // mode still hands the next Bent-Increase step that mode's own value rather
  // than the level standing still. It is saved unclipped: a candidate driven
  // below zero and one carried past 0x7FF both read at or past 0x600 and take
  // the +8 branch.
  v.bentGainRef = static_cast<std::uint16_t>(newLevel);
  if (decayToSustain) v.phase = EnvPhase::Sustain;
  if (attackToDecay) v.phase = EnvPhase::Decay;

  writeEnvx();
  return v.envelope;
}

}  // namespace snaggletooth
