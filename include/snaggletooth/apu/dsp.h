#pragma once

// The S-DSP — the sound chip the SPC700 drives through the DSPADDR/DSPDATA
// registers. This header carries the DSP's state as a value and the voice
// sample pipeline's pure mechanisms: BRR sample decode (turning a 9-byte
// compressed block into sixteen 15-bit samples), the sample-directory read,
// the per-voice BRR streaming interlock under a pitch counter, 4-point
// Gaussian interpolation over the stream's four most recent samples, the shared
// noise generator, the volume envelope, and the output stage — envelope
// application, per-voice volume, the eight-voice sum, master volume, and the
// mute gate, into a 32 kHz stereo frame.
//
// A BRR block is decoded exactly as the hardware does — the four integer
// filters, the shift-13..15 anomaly, and the clamp-to-16-then-clip-to-15
// sequence whose dirt-effect and lost-sign glitches are behaviors to
// reproduce, not to sanitize. The same exactness holds for interpolation: the
// Gaussian table is the hardware's ROM data, and the kernel keeps the
// hardware's partial overflow handling, documented wrap included.
//
// The output stage runs the hardware's mixer chain. Each voice's sample — its
// interpolated BRR output, or the shared noise level when the voice's NON bit
// is set — is scaled by the voice's 11-bit envelope into the internal
// -4000h..+3FFFh amplitude, then by its signed per-voice left/right volume, and
// the eight results are summed with a 16-bit clamp after every addition. The
// summed mix is then scaled by the signed master volume (MVOLL/MVOLR, a
// truncating multiply where the -128 product wraps), the echo unit's output is
// added through the echo volume (EVOLL/EVOLR), and, when FLG bit 6 is set, the
// frame is muted to silence — mute stops the emitted frame only; every voice, the
// noise generator, the envelopes and the echo unit keep advancing.
//
// The echo unit is a delay line in APU RAM: each sample it reads the oldest 4-byte
// entry of a ring buffer based at ESA*100h, runs a per-channel 8-tap FIR filter
// over the last eight entries, adds that output to the main mix through EVOL, and
// — unless FLG bit 5 disables echo writes — mixes the EON-enabled voices with the
// FIR output scaled by the echo feedback (EFB) and writes the result back over the
// entry it read. The buffer size is EDL<<9 entries (EDL zero gives a single-entry
// buffer); with writes disabled the buffer is a static loop that keeps feeding the
// filter. Echo samples are 15 bits stored left-justified in a 16-bit word, so a
// read arithmetic-shifts right one and a write clears bit 0.

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

// The volume envelope's phase. Even in a GAIN mode the hardware tracks an
// Attack/Decay/Sustain phase, but that phantom tracking has no audible effect
// until a mid-note switch to ADSR mode, so only the ADSR path drives phase
// transitions here. A voice powers on in Release (envelope 0), matching the
// post-reset state where FLG holds every voice keyed off.
enum class EnvPhase : std::uint8_t { Attack, Decay, Sustain, Release };

// A voice's streaming position through its BRR data. The pitch counter's bits
// 15-12 are the sample position within the current block, bits 11-4 the
// Gaussian interpolation index, and the low bits accumulate the fractional
// step. The decode cursor runs three samples ahead of the counter's position,
// so the window's `newest` at position p is stream sample p+3 — the alignment
// the hardware's decode buffering produces. `brrSampleIndex` is the next
// sample the cursor decodes within its block; 16 means the block is exhausted
// and the next decode performs the block transition (chaining to the next
// block, or jumping to the loop address after an end block) before decoding.
//
// The envelope fields carry the 11-bit level (0..0x7FF), its phase, the 5-sample
// post-key-on startup countdown (during which the voice outputs silence and the
// envelope holds still — the stream advances from the countdown's second call
// on, the first performing only the start-address read), and the Bent-Increase
// reference — the value the selected mode computed last sample, which the GAIN
// Bent-Increase mode reads to choose its +32/+8 step. Every mode computes that
// value every sample, so a rate of 0 supplies one while holding the level still,
// and it is kept unclipped: a value driven below zero or carried past 0x7FF both
// read at or past 0x600 and force the +8 branch.
//
// computesSinceKeyOn counts the voice's per-sample compute calls since its last
// key-on load, saturating high. The keying poll reads it to grade a key-on
// landing inside the voice's silent key-on span — the five startup calls plus
// the first live compute, whose output is still silent because a sample is
// scaled by the envelope standing before its update: absorbed outright at the
// poll right after the keying, and from the next poll on the countdown re-arms
// while the stream stands.
struct VoiceState {
  std::uint16_t brrAddress = 0;
  std::uint8_t brrSampleIndex = 0;
  std::uint16_t pitchCounter = 0;
  SampleWindow window{};
  std::uint16_t envelope = 0;
  EnvPhase phase = EnvPhase::Release;
  std::uint8_t konDelay = 0;
  std::uint8_t computesSinceKeyOn = 0xFF;
  std::uint16_t bentGainRef = 0;
};

// One 32 kHz stereo output sample: the eight-voice mix through the master volume,
// the echo unit, and the mute gate.
struct StereoFrame {
  std::int16_t left = 0;
  std::int16_t right = 0;

  [[nodiscard]] bool operator==(const StereoFrame&) const noexcept = default;
};

// The S-DSP's state as a value: snapshot by copy, restore by assignment. The
// 128-byte register file the CPU reaches through DSPADDR/DSPDATA lives here
// (ApuState carries only the DSPADDR latch beside it). Indexing or
// iterating a DspState reaches its register file, so the machine's overlay
// reads and writes a DSP register as dsp[reg]; the envelope counter, the noise
// generator, the echo unit and the voices' streaming state are named members
// beside it. The sample clock is not one of them — the machine owns the single
// counter that drives both the timers and the sample boundaries.
struct DspState {
  std::array<std::uint8_t, 128> regs{};

  // The global counter that gates every envelope/noise rate. It powers on at 0,
  // decrements once per 32 kHz sample, and wraps to 0x77FF when it would pass
  // below 0 (the first sample after reset wraps). A rate fires this sample when
  // (globalCounter + offset[rate]) % period[rate] == 0.
  std::uint16_t globalCounter = 0;

  // DSP samples elapsed since power-on. Only its parity is load-bearing: the
  // KON/KOFF poll runs on even-indexed samples (a fixed choice that settles the
  // hardware's power-on poll-phase race).
  std::uint32_t sampleIndex = 0;

  // The internal key-on value (one bit per voice). A write to the KON register
  // arms it with the value written; the KON/KOFF poll keys the armed voices on
  // and clears it. So a key-on takes effect on the write and happens once,
  // while the register keeps its value to be read back — unlike KOFF, which
  // the poll reads from its register and which acts for as long as a bit stays
  // set. Two writes between polls arm only the second.
  std::uint8_t internalKon = 0;

  // The extra stage of the VxENVX read-back pipeline: each voice's S9 slot writes
  // the value computed one sample earlier and stages the fresh one, so a CPU read
  // of VxENVX lags the envelope compute by one sample more than the in-slot
  // prepared/held split alone gives. Measured against spc_dsp6's `KON/envx
  // during kon`, whose sync vernier is anchored to voice 0's ENVX publish.
  std::array<std::uint8_t, 8> envxStage{};

  // The one shared noise generator's 15-bit level, in the internal sample range
  // -4000h..+3FFFh, seeded to -4000h at power-on and reset. A voice whose NON bit
  // is set outputs this level in place of its interpolated BRR sample; the level
  // advances by the documented 15-bit LFSR at the FLG noise rate, shared by every
  // NON voice. It is machine state — snapshot and restore carry it.
  std::int16_t noiseLevel = -0x4000;

  // The echo unit's state. echoIndex is the ring position (in 4-byte entries) that
  // walks the echo buffer in APU RAM based at ESA*100h; echoLength is the latched
  // entry count (EDL<<9), read only when echoIndex is 0, so an EDL change takes up
  // to a full buffer to take effect. The per-channel FIR history holds the last
  // eight entries read from the buffer, newest at echoFirPos; the two channels
  // filter separately with the same coefficients. All plain value fields — the
  // snapshot carries them; power-on and reset default them to zero.
  std::uint16_t echoIndex = 0;
  std::uint16_t echoLength = 0;
  std::array<std::int16_t, 8> echoFirLeft{};
  std::array<std::int16_t, 8> echoFirRight{};
  std::uint8_t echoFirPos = 0;

  // The eight voices' streaming state, beside the register file.
  std::array<VoiceState, 8> voices{};

  // --- The intra-sample slot pipeline ---
  //
  // The DSP runs one of a sample's 32 clock slots per machine cycle. These value
  // fields (snapshot-legal, defaulted to the seed shape) carry the frame being
  // assembled across its slots and the voice-0 output pipeline across the frame
  // wrap. A standalone DspState is self-driving: it advances slotCursor itself, so
  // the machine never passes it redundant phase.
  //
  // slotCursor is the next slot Tk (0..31) a stepDspCycle call runs. primed marks
  // whether the pipeline scratch is live: a freshly seeded state has no prepared
  // voice-0 output, so its first frame computes at once — byte-identical to the
  // frame-at-once model — and primes the pipeline at its close; every later frame
  // is fully slot-scheduled and voice 0's output rides one update behind voices
  // 1-7 (the hardware's envelope pipeline).
  std::uint8_t slotCursor = 0;
  bool primed = false;

  // The frame under construction: the running left/right voice mix and the
  // EON-enabled echo send, each clamped to signed 16 bits after every voice's
  // volume step, and the finished stereo frame the wrap delivers (finalized by
  // the left slot T27 and the right slot T28).
  std::int32_t mixLeft = 0;
  std::int32_t mixRight = 0;
  std::int32_t echoSendLeft = 0;
  std::int32_t echoSendRight = 0;
  StereoFrame slotFrame{};

  // The echo write in flight: the echo unit computes its feedback value when it
  // runs at T24, but the buffer bytes land at the write slots — the left word at
  // T30, the right word at T31 — so a CPU read of the entry between those slots
  // still sees the old sample. Plain values: the snapshot carries a write caught
  // between its compute and its slots.
  bool echoWritePending = false;
  std::uint16_t echoWriteEntry = 0;
  std::array<std::uint8_t, 4> echoWriteBytes{};

  // Each voice's amplitude, computed at the voice's S3 slot and applied at its
  // S4/S5 volume slots. Voice 0's is prepared at slot T31 for the following frame
  // — the one-update pipeline lag — while voices 1-7 compute and apply within one
  // frame. voice n's S3 reads voice n-1's stored amplitude for pitch modulation.
  std::array<int, 8> voiceAmplitude{};

  // VxOUTX and VxENVX are computed at a voice's S3 slot but do not become readable
  // in the register file until its later output slots (S8/S9). These hold the
  // computed-but-not-yet-visible bytes; a CPU read before the visibility slot sees
  // the previous sample's value, and a CPU write up to that slot overwrites the
  // pending one. Voice 0's visibility slots fall in the following frame, so its
  // OUTX/ENVX ride one frame behind, matching its output.
  std::array<std::uint8_t, 8> preparedOutx{};
  std::array<std::uint8_t, 8> preparedEnvx{};

  // The effective-pitch capture. A voice's stream advance does not read
  // VxPITCHL/H live: the register pairs are captured for all eight voices at
  // the first slot of every other sample — the same every-other-sample grid the
  // KON/KOFF poll runs on — and a capture reaches a voice's advance one full
  // sample later. Voice 0's T31 compute is the only one late enough to see its
  // own sample's capture, so during a capture sample voices 1-7 still advance
  // by the previous capture (pitchLatchOld); on every other sample all eight
  // read the latest (pitchLatch). A freshly seeded state captures the register
  // values before its first sample, keeping that frame byte-identical to the
  // frame-at-once model. Measured against spc_dsp6's `KON/kon decoding when
  // another kon`: its frozen-pitch cursor readings quantize to this grid, which
  // no per-sample register read reproduces.
  std::array<std::uint16_t, 8> pitchLatch{};
  std::array<std::uint16_t, 8> pitchLatchOld{};

  // The echo unit's FIR output for the frame, computed when the buffer is read
  // (slot T24) and added to the master-scaled mix through the echo volume at the
  // left/right output slots (T27/T28); the write-back and ring advance land at
  // T31. The ESA base is latched at T30 and applied by the T31 write.
  int echoFirOutLeft = 0;
  int echoFirOutRight = 0;
  std::uint16_t echoWriteBase = 0;

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
// gains the voice's 14-bit step (VxPITCHL/H bits 0-13; bits 14-15 are stored
// but never used), and every sample position the counter passes is decoded
// through the stream — following block chaining, loop jumps and ENDX on the
// way. Returns the freshly interpolated 15-bit sample, exactly as
// interpolatedSample reads it. This single-voice call reads the pitch registers
// live; the whole-DSP sample paths instead read the every-other-sample capture
// (see DspState::pitchLatch), which this call neither takes nor consumes.
std::int16_t stepVoice(DspState& dsp, std::span<const std::uint8_t, 65536> ram,
                       std::size_t voice) noexcept;

// The voice's current interpolated 15-bit sample: the Gaussian kernel over its
// window at the pitch counter's current index. Pure over the state — no
// advance, no memory access.
[[nodiscard]] std::int16_t interpolatedSample(const DspState& dsp, std::size_t voice) noexcept;

// Advances the global counter one 32 kHz sample: decrement, wrapping to 0x77FF
// when it would pass below 0.
[[nodiscard]] std::uint16_t nextGlobalCounter(std::uint16_t counter) noexcept;

// Whether the envelope/noise operation at 5-bit rate `rate` fires at this global
// counter value: (counter + offset[rate]) % period[rate] == 0. Rate 0 (period
// Infinite) never fires; rate 31 (period 1) fires every sample.
[[nodiscard]] bool envelopeRateFires(std::uint16_t counter, std::uint8_t rate) noexcept;

// Advances the DSP's per-sample global state to the next 32 kHz sample: the
// global counter and the power-on sample index. Called at the end of a sample,
// after that sample's KON/KOFF poll and per-voice envelope steps — so the first
// sample after power-on carries index 0 (even) and polls.
void tickDspSample(DspState& dsp) noexcept;

// Generates one 32 kHz stereo output sample and advances the whole DSP by it.
// The order is the hardware's: poll KON/KOFF, then for each of the eight voices
// stream a sample, step its envelope, and fold its enveloped amplitude into the
// left/right sum through the per-voice volume; then scale the sum by the master
// volume, add the echo unit's FIR output through the echo volume, apply the mute
// gate, advance the shared noise generator, and tick the per-sample state.
//
// A voice in its post-key-on startup outputs silence and holds its envelope
// through the countdown; its stream already advances at the pitch from the
// countdown's second call on (the first performs only the start-address read).
// Otherwise the voice's sample is its
// interpolated BRR output, or the shared noise level when its NON bit is set,
// and the enveloped amplitude is (sample * envelope) >> 11 — the internal
// -4000h..+3FFFh value that VxOUTX returns the high byte of and the next voice's
// PMON reads. When PMON is enabled for a voice (bits 1-7), its pitch step is
// scaled by the previous voice's amplitude and capped at the 128 kHz rate. Each
// channel adds (amplitude * VxVOL) >> 6 (the signed 8-bit volume with the
// BRR-dropped low bit recovered), and the sum is clamped to signed 16 bits after
// every addition. The summed mix is scaled by the signed master volume
// (sum * MVOL >> 7, truncated so the -128 product wraps); the echo output is added
// as (fir * EVOL) >> 7 with a 16-bit clamp; FLG bit 6 mutes the returned frame to
// silence while every internal mechanism keeps running.
//
// The two overloads differ only in whether the echo unit may write its buffer.
// The first takes writable RAM and runs the full unit, including the feedback
// write the echo delay line depends on. The second takes read-only RAM: the echo
// unit still reads, filters and contributes its output, but it cannot write, which
// is identical to holding FLG bit 5 set (echo writes disabled) — the buffer is a
// static loop. Callers that exercise the echo delay use the first; callers that
// only need the voice mix and never arm echo use the second.
[[nodiscard]] StereoFrame stepDspSample(DspState& dsp,
                                        std::span<std::uint8_t, 65536> ram) noexcept;
[[nodiscard]] StereoFrame stepDspSample(DspState& dsp,
                                        std::span<const std::uint8_t, 65536> ram) noexcept;

// Runs exactly one 32-clock slot of the current sample: the slot at dsp.slotCursor,
// which is then advanced. Slot T0 (the wrap) delivers the frame the previous sample
// finalized — the return holds it and `delivered` is set true — while every other
// slot returns silence with `delivered` false. The machine calls this once per
// cycle so the sample's work spreads across its 32 slots (a mid-frame register
// write is seen only if it lands before its consuming slot); a standalone caller
// loops it (stepDspSample does exactly that). The two overloads differ only in
// whether the echo unit may write its delay line, matching stepDspSample.
struct SlotResult {
  StereoFrame frame{};
  bool delivered = false;
};
SlotResult stepDspCycle(DspState& dsp, std::span<std::uint8_t, 65536> ram) noexcept;
SlotResult stepDspCycle(DspState& dsp, std::span<const std::uint8_t, 65536> ram) noexcept;

// Runs the KON/KOFF poll. On even-indexed samples it reads the KON ($4C) and
// KOFF ($5C) registers: a set KON bit latches internal-KON and keys the voice on
// (envelope 0, Attack, the 5-sample startup, its ENDX bit cleared, its stream
// primed), and a set KOFF bit moves the voice to Release. On odd samples it does
// nothing. Voices keyed on two samples ago clear from the internal-KON latch. A
// key-on landing on a voice still inside its silent key-on span — the five
// startup calls plus the first live compute — is absorbed at the poll right
// after the keying (countdown, stream and envelope schedule all stand), and at
// any later in-span poll re-arms the countdown and drops the envelope while the
// stream keeps the position it has walked to.
void pollKeying(DspState& dsp, std::span<const std::uint8_t, 65536> ram) noexcept;

// Keys voice `voice` (0-7) on: envelope to 0 and Attack, the 5-sample startup
// begun, its ENDX bit cleared, and its BRR stream primed from the start address.
void keyOnVoice(DspState& dsp, std::span<const std::uint8_t, 65536> ram,
                std::size_t voice) noexcept;

// Keys voice `voice` (0-7) off: the envelope enters Release, decreasing by 8 each
// sample regardless of the ADSR/GAIN settings. The stream is untouched — BRR
// decoding never stops for a keyed-off voice.
void keyOffVoice(DspState& dsp, std::size_t voice) noexcept;

// Advances voice `voice`'s envelope by one 32 kHz sample and writes ENVX ($x8,
// the level's high 7 bits). During the post-key-on startup the level holds at 0
// and the countdown decrements. Otherwise one envelope operation is applied,
// gated by the global counter and shaped by the ADSR/GAIN registers — Attack
// (+32, or +1024 at attack rate 15) to Decay when the level exceeds 0x7FF, Decay
// and Sustain as exponential decreases, the four GAIN modes, and Direct Gain.
// `brrEndMute` (a BRR End+Mute block entered this sample) forces Release and
// drops the level to 0. Returns the resulting 11-bit level.
std::uint16_t stepVoiceEnvelope(DspState& dsp, std::size_t voice, bool brrEndMute) noexcept;

}  // namespace snaggletooth
