# The S-DSP voice pipeline

The S-DSP is the chip that turns compressed sample data into sound. It reads BRR-encoded waveforms
from the APU's RAM, resamples each of eight voices to its own pitch, shapes each with a volume
envelope, and sums them into a 32 kHz stereo stream. The [APU machine](apu-machine.md) drives it: as
the SPC700 runs, the machine clocks the DSP one sample every 32 cycles and collects the frames it
produces.

This page covers the voice pipeline as it stands: BRR decode, pitch and Gaussian interpolation, the
volume envelope, key-on/key-off, and the output stage that mixes the voices. Echo, the noise
generator, pitch modulation, and the master volume are the DSP's remaining registers — they are
present in the register file but not yet wired, so a frame here is the dry per-voice mix, not the
final DAC output. The status table in the [README](../README.md) tracks what is live.

Every value in the pipeline is derived from public hardware documentation and validated against it;
the exactness — the BRR overflow glitches, the Gaussian table's ROM values, the envelope rate tables
— is deliberate, because a sample-accurate core is the point.

## Running the DSP through the machine

You do not step the DSP yourself. It runs as part of the machine: `step()` and `run()` advance it
alongside the CPU and timers, and `takeFrames()` drains the stereo samples it has produced.

```cpp
#include "snaggletooth/apu/apu.h"

snaggletooth::Apu apu;
// ... load a sound driver into RAM, point the CPU at it, set up DSP registers ...

apu.run(32000 * 32 / 1000);           // run ~1 ms of audio time
std::vector<snaggletooth::StereoFrame> audio = apu.takeFrames();
// audio holds one { left, right } pair per 32 kHz sample produced this run
```

A `StereoFrame` is one output sample: signed 16-bit `left` and `right`. The machine appends one per
32-cycle boundary its DSP sample clock crosses, so a run of *n* cycles yields about *n*/32 frames.
Frames accumulate until you drain them — drain periodically to keep the queue bounded. They are
output, not machine state: a `restore()` or `reset()` discards any that are pending, and a snapshot
does not carry them.

## The register file

The DSP has a 128-byte register file, reached from the CPU through the machine's `DSPADDR`/`DSPDATA`
overlay (see the [APU machine](apu-machine.md)). Registers are laid out by voice: the low nibble of
an address selects a field, the high nibble selects the voice, so voice *x* owns `$x0`–`$x9`.

Per voice:

| Address | Register | Role |
|---|---|---|
| `$x0` / `$x1` | VxVOLL / VxVOLR | Left / right volume, signed 8-bit (negative inverts the phase). |
| `$x2` / `$x3` | VxPITCHL / VxPITCHH | 14-bit pitch step (`$1000` = unity, one octave per doubling). |
| `$x4` | VxSRCN | Source number — the index into the sample directory. |
| `$x5` / `$x6` | VxADSR1 / VxADSR2 | The ADSR envelope settings. |
| `$x7` | VxGAIN | The GAIN envelope settings (used when ADSR is off). |
| `$x8` | VxENVX | Read-back of the current envelope level (high 7 bits). |
| `$x9` | VxOUTX | Read-back of the current output amplitude (high byte). |

Global registers the voices use:

| Address | Register | Role |
|---|---|---|
| `$4C` | KON | Key-on flags — a set bit starts that voice. |
| `$5C` | KOFF | Key-off flags — a set bit releases that voice. |
| `$5D` | DIR | High byte of the sample directory's address (`DIR × $100`). |
| `$7C` | ENDX | Per-voice end flags; the DSP sets a bit when a voice reaches an end block. Any write clears all bits. |

VxENVX and VxOUTX are written by the DSP every sample; a value the CPU writes to them is overwritten
at the next sample.

## How a voice makes sound

Each 32 kHz sample, every voice runs the same path.

**BRR decode.** A voice's waveform is a chain of 9-byte BRR blocks: a header (shift, filter, and
loop/end flags) and sixteen 4-bit samples. `VxSRCN` and `DIR` locate the chain's start and loop
addresses through the sample directory. Each block's samples are reconstructed through one of four
integer filters over the two previous outputs, then clamped to 16 bits and clipped to 15 — the
sequence that produces the hardware's documented dirt-effect and lost-sign behavior on overdriven
data.

**Pitch and interpolation.** A 16-bit pitch counter gains the voice's 14-bit step each sample. Its
top four bits index the sample within the block, its next eight bits index a 4-point Gaussian kernel
over the four most recently decoded samples. The kernel is the hardware's 512-entry ROM table with
its exact partial-overflow arithmetic, so interpolated output matches the chip bit for bit — a step
of `$1000` plays at the source rate, larger steps play higher and skip samples, smaller steps play
lower and interpolate between them.

**Envelope.** The interpolated sample is scaled by an 11-bit volume envelope. In ADSR mode the
envelope rises on key-on (Attack), falls to a sustain level (Decay), holds and decays (Sustain), and
falls to zero on key-off (Release); in GAIN mode the level is driven directly or ramped linearly or
exponentially. A global counter gates the per-rate timing, driven by the documented rate tables.

**Output.** The enveloped sample is the voice's amplitude — an internal signed value in the range
`-$4000`…`+$3FFF`, of which `VxOUTX` reports the high byte. Each channel scales it by the signed
per-voice volume and adds it into the mix; the running sum is clamped to signed 16 bits after every
voice. The eight scaled amplitudes summed are the frame.

## Key-on and key-off

Writing a `KON` bit starts a voice: its envelope resets to zero, it enters Attack, its stream restarts
from the source's start address, and its end flag clears. There are **five empty samples** after a
key-on before the envelope and decoding begin — a voice you key on is silent for five samples, then
sounds. A driver writes the `KON` bit and then clears the register; leaving it set re-keys the voice
every sample.

Writing a `KOFF` bit releases a voice: its envelope decreases by 8 each sample until it reaches zero,
regardless of the ADSR or GAIN settings. Decoding does not stop for a released voice — only the
envelope changes. `KON` and `KOFF` are polled every second sample (16 kHz), so two writes within one
poll window may collapse to the later one.

A block whose header marks it End+Mute releases the voice and drops its envelope to zero the moment
the voice reaches it; an End+Loop block loops without muting. Both set the voice's `ENDX` bit.

## Inspecting the pipeline directly

The pipeline's stages are also plain functions over a `DspState`, for tests and tools that drive the
DSP outside a running machine. `stepDspSample(dsp, ram)` produces one frame and advances the whole
DSP — poll, then per voice stream, envelope, and mix. Below it, `decodeBrrBlock` decodes a block,
`gaussInterpolate` runs the kernel, `stepVoice` advances one voice's stream, `stepVoiceEnvelope`
advances one envelope, and `keyOnVoice` / `keyOffVoice` / `pollKeying` drive keying. The
`DspState` is a value: copy it to snapshot, assign it to restore.

## Gotchas

- **The frame is the dry mix.** Master volume, echo, the noise generator, pitch modulation, and the
  FLG mute are not applied yet, so a frame is the summed per-voice output and nothing more. It is not
  the final DAC signal.
- **Register writes take effect at the next sample.** The DSP reads its registers once per sample.
  Changes are sample-granular; the hardware's finer intra-sample access schedule is not modeled.
- **A released voice keeps decoding.** Key-off changes only the envelope. `ENDX` bits can be set by a
  voice you have keyed off, because its stream is still running.
- **`VxOUTX` is the high byte of the internal amplitude.** The full amplitude is `-$4000`…`+$3FFF`;
  the register carries `-128`…`+127`.

## Where to look

- [The APU machine](apu-machine.md) — how the DSP is clocked and how the CPU reaches its registers.
- [The SPC700 core](spc700-cpu.md) — the CPU that drives it.
- `include/snaggletooth/apu/dsp.h` — the pipeline's types and functions.
