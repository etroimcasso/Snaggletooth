# The S-DSP voice pipeline

The S-DSP is the chip that turns compressed sample data into sound. It reads BRR-encoded waveforms
from the APU's RAM, resamples each of eight voices to its own pitch, shapes each with a volume
envelope, mixes them, applies an echo delay line, and produces a 32 kHz stereo stream. The
[APU machine](apu-machine.md) drives it: as the SPC700 runs, the machine clocks the DSP one sample
every 32 cycles and collects the frames it produces.

This page covers the DSP end to end: BRR decode, pitch and Gaussian interpolation, the volume
envelope, key-on/key-off, the shared noise generator, pitch modulation, and the output mixer — the
per-voice sum, the master volume, the echo unit, and the mute gate — into the final 32 kHz stereo
output. The status table in the [README](../README.md) tracks what is live.

Every value in the pipeline is derived from public hardware documentation and validated against it;
the exactness — the BRR overflow glitches, the Gaussian table's ROM values, the envelope rate tables,
the echo FIR's wrap-then-saturate arithmetic — is deliberate, because a sample-accurate core is the
point.

Several behaviors on this page are places where the published documentation is incomplete, ambiguous
or wrong, and where a hardware test ROM settled the question.
[s-dsp-behavior.md](s-dsp-behavior.md) records each of those: what the sources say, how they
disagree, and the measurement that decided it.

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
| `$4C` | KON | Key-on flags — writing a set bit starts that voice, once. |
| `$5C` | KOFF | Key-off flags — a set bit releases that voice. |
| `$5D` | DIR | High byte of the sample directory's address (`DIR × $100`). |
| `$7C` | ENDX | Per-voice end flags; the DSP sets a bit when a voice reaches an end block. Any write clears all bits. |

Global mixer, control, and echo registers:

| Address | Register | Role |
|---|---|---|
| `$0C` / `$1C` | MVOLL / MVOLR | Master volume, signed 8-bit (negative inverts the phase). |
| `$2C` / `$3C` | EVOLL / EVOLR | Echo output volume, signed 8-bit. |
| `$0D` | EFB | Echo feedback volume, signed 8-bit. |
| `$2D` | PMON | Pitch-modulation enable per voice (voices 1–7). |
| `$3D` | NON | Noise enable per voice — the voice outputs the shared noise level. |
| `$4D` | EON | Echo-send enable per voice — the voice feeds the echo buffer write. |
| `$6C` | FLG | Soft reset (bit 7), mute (bit 6), echo-write disable (bit 5), noise rate (bits 0–4). |
| `$6D` | ESA | Echo buffer base address (`ESA × $100`). |
| `$7D` | EDL | Echo buffer size — `EDL << 9` 4-byte entries (`EDL` = 0 gives one entry). |
| `$xF` | FIRx | The eight echo FIR coefficients, signed 8-bit, at `$0F`, `$1F`, … `$7F`. |

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

**Phase and mode.** Attack, Decay, Sustain and Release are the voice's own state, not a property of
ADSR mode, and the phase keeps advancing while GAIN drives the level. Every sample the selected mode
computes the level it would move to, and the Attack-to-Decay switch reads that value whether or not
the rate lets the level change — so a voice in Attack under a GAIN mode leaves Attack as soon as that
mode's step would carry the level outside the 11-bit range, either past `$7FF` or below zero, even at
a rate of 0 that holds the level perfectly still. The boundary moves with the step: Linear Increase
steps +32 and switches from `$7E0` upward, Bent Increase steps +8 in its upper range and switches
from `$7F8`. Turn `ADSR1` bit 7 back on and the voice carries on in whatever phase it reached.

Decay to Sustain reads that computed value too, and it compares the value's upper three bits against
a boundary — but the boundary comes from whichever register is driving the level. Under ADSR that is
`ADSR2`'s sustain level, as you would expect. Under a GAIN mode it is **`GAIN`'s own bits 7-5**, which
hold the gain mode and its enable bit rather than any sustain level, so a voice in Decay under GAIN
reaches Sustain at a boundary nobody chose: `$4xx` under Linear Decrease, `$5xx` under Exponential
Decrease, `$6xx` under Linear Increase, `$7xx` under Bent Increase, and `$0xx` under a direct gain
below `$20`. This is hardware behaviour, not a simplification — reproduce it or a driver that steers
its envelope through GAIN lands in the wrong phase.

Both switches read the value the mode computed rather than the level it stored, which matters at a
rate of 0: a voice parked at `$420` under Linear Decrease never moves, yet its step computes `$400`
and the voice enters Sustain, while the same mode parked one lower at `$41F` computes `$3FF` and stays
in Decay — even though `$41F`'s own upper three bits are the boundary.

**Bent Increase reads that value too.** Its step — +32 below `$600`, +8 at or past it — is chosen from
the value the selected mode computed on the *previous* sample, never from the current level. Since
every mode computes that value every sample, a mode parked at a rate of 0 still supplies one: a voice
held at `$5E0` by a rate-0 Linear Increase computes `$600`, so the Bent Increase that follows takes the
small step from a level below the boundary. The value is kept whole rather than narrowed to the
envelope's 11 bits, so one driven below zero and one carried past `$7FF` both read at or past `$600`
and take +8 — a Linear Decrease that runs off the bottom leaves the level at 0, and the Bent Increase
after it adds 8, not 32.

**Output.** A sample is scaled by the level already standing — the one the voice's previous envelope
update left behind — and this sample's update runs after that scaling. So a level in motion reaches
the output one sample after the step producing it, and a level that is holding still is
indistinguishable either way. The scaled sample is the voice's amplitude — an internal signed value
in the range `-$4000`…`+$3FFF`, of which `VxOUTX` reports the high byte. A voice with its `NON` bit
set outputs the shared noise level here in place of its interpolated sample. The amplitude feeds the
output mixer (below), and it is also the value the next voice's pitch modulation reads.

## Key-on and key-off

Writing a `KON` bit starts a voice: its envelope resets to zero, it enters Attack, its stream restarts
from the source's start address, and its end flag clears. There are **five empty samples** at a
key-on before the envelope begins. The keying poll runs at a sample's last slot, *before*
voice 0's envelope compute in that same slot — so voice 0's first silent sample is the poll's own
while voices 1-7 take theirs starting the next sample, and the sixth envelope call after the load
takes the first step. That step's own sample is still silent, because a sample is scaled by the level
standing before the update (above) and that level is the zero the key-on set; the first sounding
sample is the one after it. The shared-slot asymmetry, together with `VxENVX`'s one-sample read-back
lag (below), is what makes all eight voices' key-on startup read identically through `VxENVX`.

The stream does not wait for the envelope: the decode cursor already advances at the voice's pitch
through the empty samples, from the second of them on — the first performs the start-address read and
decodes nothing. Only the *output* is silent during the startup; a voice whose pitch is changed (or
zeroed) mid-startup carries the position it had walked to.

A key-on takes effect on the write, and happens once. The value written arms the next poll; that poll
starts the armed voices and disarms itself. So a `KON` bit left set does not start the voice again,
and the register keeps the value for you to read back. Two `KON` writes inside one poll window arm
only the second one.

Re-keying a voice that is already sounding restarts it in full — envelope to zero, stream back to the
start, the empty samples again — which is what causes the documented click. A key-on that lands on a
voice **still inside its silent key-on span** does less, and how much less depends on which poll
consumes it:

- **At the poll immediately after the one that started the voice, it is absorbed outright.** Nothing
  resets — countdown, stream and envelope schedule all stand. Two `KON` writes that straddle a poll,
  each naming the same voice, therefore start it once, and a voice named by the earlier of two
  straddling writes leads one named only by the later write by exactly one poll period, two samples.
- **At any later poll inside the span, it rewinds the silence but not the stream.** The five-sample
  countdown re-arms in full and the envelope drops back to zero, so the voice's level emerges as
  late as a full restart would place it — but the decode cursor is not sent back to the start
  address: it keeps the position it has walked to and keeps advancing at the pitch. The span is the
  whole silence a key-on produces — the five empty samples *plus* the first envelope step's
  still-silent sample — so this covers a re-key consumed up to six samples after the load.

Only a re-key consumed from the first sounding sample on restarts the voice in full.

Writing a `KOFF` bit releases a voice: its envelope decreases by 8 each sample until it reaches zero,
regardless of the ADSR or GAIN settings. Decoding does not stop for a released voice — only the
envelope changes. `KOFF` is read from its register at every poll rather than armed by the write, so a
set bit keeps releasing the voice until you write a new value. `KON` and `KOFF` are polled every
second sample (16 kHz).

A voice reads its current block's header from RAM every sample and checks its loop/end bits the same
sample, whether or not its pitch counter has advanced far enough to decode anything. A header marking
the block End+Mute releases the voice and drops its envelope to zero; an End+Loop block loops without
muting. So rewriting a header under a playing voice takes effect on the next sample, including for a
voice whose pitch is zero and which is decoding nothing at all.

`ENDX` belongs to the decode rather than to that check: the bit is set when the voice reaches a block
carrying the end flag, which a stopped voice never does.

A freshly keyed-on voice makes no header check until two sample periods after the poll loads the
key-on. The load precedes voice 0's compute in the slot they share, so the load's slot is every
voice's first compute and the cutoff is a uniform count: the check is live from each voice's third
compute after (and including) the load's own sample.

## The output mixer

The eight voices become a frame through a fixed chain, in this order:

1. **Per-voice volume.** Each voice's amplitude is scaled by its signed `VxVOL` for each channel and
   added into the running sum, which is clamped to signed 16 bits after every voice.
2. **Master volume.** The sum is scaled by the signed `MVOL` for each channel (`sum × MVOL >> 7`). A
   negative master volume inverts the phase; the one value that overflows, `MVOL = -128` against a
   full-scale sum, wraps rather than clamping — the hardware's behavior.
3. **Echo.** The echo unit's filtered output is scaled by the signed `EVOL` and added to the
   master-scaled mix, clamped to signed 16 bits (see below).
4. **Mute.** When `FLG` bit 6 is set, the emitted frame is zeroed. Mute stops only the output — every
   voice, the envelopes, the noise generator, and the echo unit keep running.

## The echo unit

Echo is a delay line living in the APU's own RAM. Each sample, the unit reads the oldest 4-byte entry
of a ring buffer, runs it through an 8-tap FIR filter, mixes the filtered signal back into the output,
and writes a new entry built from the enabled voices plus a feedback of the filtered signal.

- **The buffer** is a ring of 4-byte entries based at `ESA × $100`: a 16-bit left sample then a 16-bit
  right sample, each holding a 15-bit value left-justified (bit 0 unused). `EDL` sizes the ring at
  `EDL << 9` entries (`EDL` = 0 gives a single entry); the size is latched only when the ring wraps to
  its start, so a change to `EDL` takes up to a full buffer to take effect. The buffer address wraps
  within the 64 KB space, and the unit writes straight into RAM — a buffer placed over code or data
  overwrites it, exactly as the hardware does.
- **The FIR filter** runs per channel over the last eight entries read: the taps are oldest × `FIR0`
  through newest × `FIR7`, each product shifted right 6. The first seven additions wrap at 16 bits and
  only the final addition saturates — the documented arithmetic. Its result is a 15-bit sample, so the
  low bit is dropped before `EVOL` and `EFB` read it. Left and right filter separately with the same
  coefficients.
- **Output and feedback.** The filtered signal is added to the main mix through `EVOL`. Separately, the
  voices enabled in `EON` are summed (after their per-voice volume) and added to the filtered signal
  scaled by `EFB`; the result, with bit 0 cleared, is written back over the entry that was read.
  `FLG` bit 5 disables the write — reads and output continue, so the buffer becomes a static loop that
  keeps feeding the filter.

A typical echo sets `ESA`/`EDL` for the delay, `EVOL` for how loud the echo is, `EFB` for how long it
repeats, and the `FIRx` coefficients (summing near `$80`) for its tone.

## Noise and pitch modulation

**Noise.** One 15-bit LFSR is shared by every voice. It advances at the rate in `FLG` bits 0–4 (rate 0
holds it), gated by the same global counter the envelopes use. A voice with its `NON` bit set outputs
the current noise level in place of its interpolated sample — pitch and Gaussian interpolation do not
apply to noise, though the voice keeps decoding its BRR data, so an End+Mute block still releases it.

**Pitch modulation.** With a voice's `PMON` bit set (voices 1–7 only), its pitch step is scaled by the
previous voice's current amplitude, so voice *x*−1 frequency-modulates voice *x*. A silent previous
voice leaves the step unmodulated. The modulated step is capped at four source samples per output
sample (128 kHz).

## When a register write takes effect

The DSP builds a sample over 32 clock slots, and it reads each register on a fixed slot of that
window. A `DSPDATA` write reaches the sample being built only if it lands before the slot that reads
the register; a write after that slot waits for the next sample. The slots, numbered T0–T31 as the
counter's residue mod 32 numbers them:

| Slot | What the DSP does there |
|---|---|
| T2, T5, …, T20 | Voices 1–7 each run their whole compute (stream, noise, envelope, amplitude). |
| a voice's T3/T4 … | That voice folds its left (`VxVOLL`) then right (`VxVOLR`) volume into the mix. |
| T24 | The echo unit reads its buffer, filters, and computes its feedback value. |
| T27 / T28 | The left / right output: `MVOLL`+`EVOLL` then `MVOLR`+`EVOLR`, then the mute gate. |
| T30 / T31 | The echo write lands: the left word at T30, the right word at T31. |
| T31 | `KON`/`KOFF` are polled (even samples), then voice 0 computes; the global counter and noise step. |

Three consequences are worth knowing:

- **`VxOUTX` and `VxENVX` lag their compute.** A voice computes its amplitude at its own slot but does
  not publish `VxOUTX` into the register file until a few slots later; a CPU read in between returns
  the *previous* sample's value, and a CPU write in that window overwrites the pending one — the
  hardware's read-back delay. **`VxENVX` lags one sample further:** its publish slot writes the value
  the envelope computed one sample earlier, so a CPU read of `VxENVX` is always a full sample behind
  the envelope itself.
- **Voice 0's output rides one sample behind voices 1–7.** Voice 0 computes at the last slot (T31) and
  its result is applied at the *next* sample's first slots, so its envelope, noise, and keying inputs
  are one update older than the other voices' for the same delivered frame. Its first sample from a
  seed is the exception — a freshly seeded state computes its whole first sample at once, so voice 0 is
  heard in it immediately, and the one-sample pipeline begins only afterward.
- **The echo write lags its compute.** The echo unit computes its buffer write at T24 but the bytes
  land at T30 (left word) and T31 (right word), so a program reading the entry between those slots
  still sees the previous sample there.
- **`VxPITCHL`/`VxPITCHH` are not read per sample.** The pitch a voice's stream advance uses is
  captured for all eight voices at the first slot of every *other* sample — the same every-other-
  sample grid the keying poll runs on — and a capture reaches a voice's advance one full sample
  later. Voice 0's compute at T31 is the only one late enough to see its own sample's capture, so a
  pitch write reaches voice 0 one sample before voices 1–7, and any write waits at least until the
  next capture sample regardless of where it lands. Pitch *modulation* is unaffected — `PMON`
  scales the captured step by the previous voice's current amplitude every sample.

This intra-sample schedule is derived from the S-DSP timing charts; the key-on countdown, the last
slot's placement of the keying poll and its order against voice 0's compute, the `VxENVX` publish
slot and its one-sample value lag, the echo write slots, the mid-startup stream advance, the
two-tier handling of a key-on landing inside the silent span, and the every-other-sample pitch
capture are confirmed against the Blargg DSP test ROM, while the `VxOUTX` publish slot remains the
least-certain part.

## Inspecting the pipeline directly

The pipeline's stages are also plain functions over a `DspState`, for tests and tools that drive the
DSP outside a running machine. `stepDspSample(dsp, ram)` produces one frame and advances the whole DSP
by one sample; `stepDspCycle(dsp, ram)` runs a single one of the sample's 32 slots and, on the wrap
slot, reports the finished frame — the machine drives the DSP one slot per cycle through the latter.
Both have two forms: given writable RAM the echo unit writes its buffer, as in a running machine;
given read-only RAM it still reads, filters, and outputs echo but cannot write, which is the same as
holding echo writes disabled. Below them, `decodeBrrBlock` decodes a block, `gaussInterpolate` runs
the kernel, `stepVoice` advances one voice's stream, `stepVoiceEnvelope` advances one envelope, and
`keyOnVoice` / `keyOffVoice` / `pollKeying` drive keying. The `DspState` is a value: copy it to
snapshot, assign it to restore.

## Gotchas

- **A register write can land mid-sample.** The DSP reads each register on a fixed slot of the sample
  it is building, so a write is picked up this sample or the next depending on where it lands — see
  [When a register write takes effect](#when-a-register-write-takes-effect). `VxOUTX`/`VxENVX` read
  back one sample behind, and voice 0's output rides one sample behind the others.
- **A released voice keeps decoding.** Key-off changes only the envelope. `ENDX` bits can be set by a
  voice you have keyed off, because its stream is still running.
- **A stopped voice still reads its header.** Setting `VxPITCH` to zero stops the decode, not the
  per-sample header read, so a block that becomes End+Mute underneath such a voice still releases it.
- **A pitch write is not instant.** The stream reads a pitch capture taken every other sample, not
  the register, so a `VxPITCH` write takes effect one to three samples later depending on where it
  lands against the capture grid — and one sample sooner for voice 0 than for voices 1–7. See
  [When a register write takes effect](#when-a-register-write-takes-effect).
- **`VxOUTX` is the high byte of the internal amplitude.** The full amplitude is `-$4000`…`+$3FFF`;
  the register carries `-128`…`+127`.
- **`FLG` starts at `$E0`.** On reset the DSP boots muted, soft-reset, with echo writes disabled and
  noise stopped. A driver clears `FLG` once it has set the DSP up; until then no sound is emitted.
- **The echo buffer is raw RAM.** The unit writes `EDL << 9` entries starting at `ESA × $100` with no
  bounds check — a buffer that overlaps a driver's code or data will overwrite it. Placement is the
  driver's responsibility.
- **`EDL` changes are slow.** The buffer size is latched only when the ring returns to its start, so a
  new `EDL` value can take up to the buffer's full length to take effect.
- **Mute and soft reset differ.** Mute (`FLG` bit 6) zeroes the emitted frame but leaves the echo
  output running internally; soft reset (bit 7) silences the voices but does not mute the echo output.
  Neither stops the echo unit from processing and writing its buffer.

## Where to look

- [The APU machine](apu-machine.md) — how the DSP is clocked and how the CPU reaches its registers.
- [The SPC700 core](spc700-cpu.md) — the CPU that drives it.
- `include/snaggletooth/apu/dsp.h` — the pipeline's types and functions.
