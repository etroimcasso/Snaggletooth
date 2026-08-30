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
| `$7C` | ENDX | Per-voice end flags; the DSP sets a bit when a voice's decoder leaves an end block for its loop address, readable three slots after that voice's compute. Any write clears all bits, a set still on its way included. |

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
lower and interpolate between them. The step is added to the counter's low fourteen bits — the
position within the four-sample group being consumed — and the sum clamps at `$7FFF` before the
crossed positions are counted, so one output sample consumes at most four source samples. A base
step never reaches the clamp; a pitch-modulated one can (see [Noise and pitch
modulation](#noise-and-pitch-modulation)).

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

Under ADSR, a decay whose stored level already sits in the sustain band is in Sustain before its
step: the phase changes and the level holds. A decay entered from the top — `ADSR1 = $EF`,
`ADSR2 = $E0`, where the attack ends at `$7FF`, the sustain level is 7 and the sustain rate is 0 —
never takes the `$7F7` step, from any phase of the global counter. A decay still above the band
follows the order above: the step is stored when the decay rate fires, and the candidate is what
the boundary check reads. The stored-level check is ADSR's alone; under a GAIN mode the check reads
the candidate.

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
output mixer (below), and it is also the value the next voice's pitch modulation reads — on the
following sample, not this one.

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

Whether the stream waits for the envelope depends on the voice the key-on lands on. A key-on that
interrupts a voice whose **level stands above zero while its old output is silent** — a voice still
inside an earlier startup's silence — walks: the fresh stream's decode cursor advances through the
empty samples, from the second of them on (the first decodes nothing: the start pointer is read
at the voice's directory slot of the following sample, and the start block's bytes at that sample's
BRR load slot — see below). The pitch it advances at is the one the key-on itself captured, because a key-on suspends
the pitch capture for seven samples (see
[When a register write takes effect](#when-a-register-write-takes-effect)):
a later key-on landing mid-startup re-captures the register and the walk carries its position, while
a bare pitch write waits out the hold. Only the *output* is silent during the startup. A key-on of a
**silent** voice — envelope at zero — holds instead, and so does a re-key of a voice whose old
output was **sounding**, whatever its age or level: the stream stands at its primed start through
the whole silent span *and* the first sounding sample, which interpolates the stream's first four
samples at Gaussian index 0, and advancing begins the sample after.

The walk carries no interpolation fraction: whole sample positions cross and decode, but the
fractional remainder is discarded through the silent span, so the first sounding sample interpolates
from the exact start of its stream position — Gaussian index 0 — and the fraction begins
accumulating with the advance after it. At a low pitch the difference is audible as where the
kernel's walk starts: `$0010` plays the kernel from index 0, 1, 2, …, not from the six indices the
empty samples would have banked. A held startup has no fraction to discard.

A key-on takes effect on the write, and happens once. The value written arms the next poll; that poll
starts the armed voices and disarms itself. So a `KON` bit left set does not start the voice again,
and the register keeps the value for you to read back. Two `KON` writes inside one poll window arm
only the second one.

Re-keying a voice that is already sounding restarts it in full — envelope to zero, stream back to the
start, the empty samples again — which is what causes the documented click. The restart is applied by
the voice's own compute in the consuming poll's sample, and that compute still prepares one sample
from the standing stream and envelope first: the old stream takes its final decode and advance, and
its sample sounds — **the final pre-key-on sample**. The silence therefore begins one sample after
the consuming poll, while the countdown counts from the consuming sample itself, so the first
sounding sample of the new startup lands where a key-on always places it. That compute also runs
the standing envelope's own update once more before the restart reads it, so whether the new
startup walks or holds (the split described above) is decided by the level the voice would have
carried into the next sample — a `GAIN` or `ADSR` write landing on the same sample as the key-on
counts. A key-on that lands on a
voice **still inside its silent key-on span** does less, and how much less depends on which poll
consumes it:

- **At the poll immediately after the one that started the voice, it is absorbed outright.** Nothing
  resets — countdown, stream and envelope schedule all stand. Two `KON` writes that straddle a poll,
  each naming the same voice, therefore start it once, and a voice named by the earlier of two
  straddling writes leads one named only by the later write by exactly one poll period, two samples.
- **At any later poll inside the span, it rewinds the silence but not the stream.** The five-sample
  countdown re-arms in full and the envelope drops back to zero, so the voice's level emerges as
  late as a full restart would place it — but the decode cursor is not sent back to the start
  address: a walking startup keeps the position it has walked to and keeps advancing at the pitch,
  and a held one keeps holding at its primed start. The span is the
  whole silence a key-on produces — the five empty samples *plus* the first envelope step's
  still-silent sample — so this covers a re-key consumed up to six samples after the load.

Only a re-key consumed from the first sounding sample on restarts the voice in full — with one
qualification: **both in-span tiers protect a startup that is still standing.** A voice keyed off
while inside its span — by a soft-reset pulse, a `KOFF` bit, or an End+Mute block header — has no
startup left to absorb into or rewind, and a key-on consumed on it restarts the voice in full from
wherever inside the span the kill left it: countdown re-armed, envelope from zero, stream re-primed
to the start address. The same arm is what lets `KON` win when one poll delivers `KON` and `KOFF`
together to an in-span voice.

Writing a `KOFF` bit releases a voice: its envelope decreases by 8 each sample until it reaches zero,
regardless of the ADSR or GAIN settings. Decoding does not stop for a released voice — only the
envelope changes. `KOFF` is read from its register at every poll rather than armed by the write, so a
set bit keeps releasing the voice until you write a new value. `KON` and `KOFF` are polled every
second sample (16 kHz).

`FLG` bit 7 — the soft reset — keys every voice off and forces its envelope to zero, and unlike
`KON`/`KOFF` it is applied every sample, per voice, at each voice's own compute slot. It is read
after the sample's amplitude is formed, so a live voice still emits the sample the reset lands on
under the level it carried in, and its silence begins the sample after — the same one-sample lag
every envelope update has. One coincidence is carved out: a voice whose key-on is consumed that same sample is **not** keyed off — the fresh
consumption wins, exactly as `KON` applied after `KOFF` wins at the poll itself, and the startup
proceeds as if the reset were not standing. From the voice's next compute on, a standing reset keys
it off like any other voice, so a reset pulse as short as one sample landing anywhere later — inside
the silent span or past it — silences the voice until it is re-keyed.

A voice reads its current block's header from RAM every sample and checks its loop/end bits the same
sample, whether or not its pitch counter has advanced far enough to decode anything. A header marking
the block End+Mute releases the voice and drops its envelope to zero; an End+Loop block loops without
muting. So rewriting a header under a playing voice takes effect on the next sample, including for a
voice whose pitch is zero and which is decoding nothing at all.

"Its current block" means the **decoder's** block, and the decoder runs ahead of the sound: the
key-on primes a voice's whole first block before it sounds, and from then on the decoder enters
each following block — resolving where the chain goes — while the interpolation cursor is still
eight samples back in the block before. A voice therefore goes quiet **early**: an End+Mute block
releases it while the last eight samples of the block before are still unplayed, and the End+Mute
block's own samples never sound at all. The check itself runs early in the voice's sample and
trails the decoder by one: a sample whose advance carries the decoder into an End+Mute block still
sounds, and the release lands at the next sample's check, one sample after the decoder moved in.

The sample data is read ahead of the sound too, a group of four samples at a time: a key-on decodes
the first three groups — twelve samples — as it primes the voice, and each group boundary the cursor
crosses thereafter calls for the next group, the one eight stream samples ahead of the boundary. The
reads spread over two samples: the consumption of the current group's **third** stream sample — two
past the boundary — schedules the decode and reads the group's **header** there, and the **data
bytes** are read the next sample, the first at the voice's BRR load slot and the second one slot
after its compute. A header rewritten every sample under a two-samples-per-output-sample voice shows
the header's place directly: the group crossed into under one shift takes the shift of the header
standing one frame later, and a five-cycle pulse into either data byte is caught only at that byte's
own slot, one sample later still. The bytes behind a sample are read from RAM six to ten stream
samples before it sounds, and rewriting them after their read changes nothing already decoded. A
voice parked at pitch zero is the limiting case: it consumes nothing, so it decodes nothing —
rewrite its entire block and it still plays all twelve primed samples when it moves again. Only the
header check reads RAM every sample.

The sample directory has its own slot too. The loop address the decoder takes as it leaves an end
block is read from the directory entry (`DIR × $100 + VxSRCN × 4 + 2`) at the voice's directory
slot — T22 for voice 0, nine slots before its compute, and the slot before the compute for voices
1-7 — so a directory write landing between that slot and the compute reaches the next sample's
read, not this jump. A key-on's start pointer is read at the same slot, in the sample **after** the
compute that applied the key-on (the compute that emits the voice's final pre-key-on sample); the
start block's first three groups are then read at that sample's BRR load slot. So the countdown's
first silent call reads nothing at all, and a `DIR` write that lands after the consuming compute but
before the next directory slot is what the started voice plays from. `VxSRCN` is read one step
earlier still, at the voice's source slot — four slots before its directory slot (T18 for voice 0,
T(3v−6) for voices 2-7), and for voice 1 T21 of the previous sample — so both directory reads select
the entry with the source standing there, and a `VxSRCN` write landing between the two slots reaches
the next sample's read.

Around a key-on the check's view stands still a little longer. It keeps reading the block standing
before the key-on through the five empty samples and the first two live samples — so a startup
whose decoder crosses into an End+Mute block on the way publishes its first envelope steps, and
their level reaches `VxENVX`, before the release lands. A header already standing End+Mute at the
first step's sample, by contrast, releases the voice before the step — the level never leaves zero.

`ENDX` belongs to the decoder rather than to that check, and it marks the decoder **leaving** an end
block, not entering it: the bit is set when the decoder has decoded a block carrying the end flag
through and jumps to the loop address — sixteen stream samples after it entered the block, and a
full block after an End+Mute block silenced the voice. A stopped voice's decoder never gets there.
The set is staged at the voice's compute and becomes readable three slots later; for voice 0, whose
compute is the sample's last slot, that is the following sample's fourth slot, so a read at the
sample boundary still sees the old value.

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
  `EDL << 9` entries (`EDL` = 0 gives a single entry); the size is applied only when the ring wraps to
  its start, so a change to `EDL` takes up to a full buffer to take effect. `ESA` and `EDL` are
  loaded near the end of each sample and applied as it closes, so a write to either reaches the
  buffer **one sample after the write** — the sample in flight still reads and writes the old base.
  The buffer address wraps within the 64 KB space, and the unit writes straight into RAM — a buffer
  placed over code or data overwrites it, exactly as the hardware does (the RAM beneath the
  `$F0`–`$FF` registers included; only the `$F8`/`$F9` port bytes the CPU reads are out of its
  reach — see [the APU machine](apu-machine.md)).
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
holds it), gated by the same global counter the envelopes use; the rate is read and the step taken at
the last slot of every sample, just before voice 0 computes, so all eight voices see one level per
delivered frame. A voice with its `NON` bit set outputs
the current noise level in place of its interpolated sample — pitch and Gaussian interpolation do not
apply to noise, though the voice keeps decoding its BRR data, so an End+Mute block still releases it.

**Pitch modulation.** With a voice's `PMON` bit set (voices 1–7 only), its pitch step is scaled by the
previous voice's amplitude, so voice *x*−1 frequency-modulates voice *x*. The amplitude read is the
one voice *x*−1 produced on the **previous** sample: voice *x*−1 computes three slots ahead of voice
*x* within a sample, but the modulator does not see that fresh value until the sample after, so a
change in the modulating voice reaches the modulated voice's step one sample later than it reaches the
mix. A silent previous voice leaves the step unmodulated. The modulated step itself is not capped (it reaches `$7FEE` at
the extremes); what bounds it is the clamp on the counter's in-group position, at `$7FFF` after the
step is added. A step the clamp catches consumes exactly four source samples and leaves the position
parked at the group's last fraction (`$3FFF` once the group turns) — pinned there for as long as the
step exceeds the group, and crossing into the next group on the first smaller step, however small.
A voice modulated past the ceiling and then dropped to pitch `1` therefore advances one sample on
the very next step, not after `$1000` of them.

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
| T29 / T30 | `FLG` bit 5 is loaded, one slot ahead of each echo word it gates. |
| T30 / T31 | The echo write lands: the left word at T30, the right word at T31. |
| T30 | The raw `ESA` and `EDL` are loaded for the ring advance to apply. |
| T31 | The ring advance: the loaded `ESA` becomes the next sample's base, `EDL` resizes at a wrap, the index steps. |
| T31 | `KON`/`KOFF` are polled (even samples), the global counter advances, the noise steps, then voice 0 computes. |

Three consequences are worth knowing:

- **`VxOUTX` and `VxENVX` lag their compute.** A voice computes its amplitude at its own slot but does
  not publish `VxOUTX` into the register file until a few slots later; a CPU read in between returns
  the *previous* sample's value, and a CPU write in that window overwrites the pending one — the
  hardware's read-back delay. **`VxENVX` lags one sample further:** its publish slot writes the value
  the envelope computed one sample earlier, so a CPU read of `VxENVX` is always a full sample behind
  the envelope itself.
- **Voice 0's output rides one sample behind voices 1–7.** Voice 0 computes at the last slot (T31) and
  its result is applied at the *next* sample's first slots, so its keying input is one update older
  than the other voices' for the same delivered frame. Its first sample from a
  seed is the exception — a freshly seeded state computes its whole first sample at once, so voice 0 is
  heard in it immediately, and the one-sample pipeline begins only afterward.
- **The counter advances ahead of every check that reads it.** The global counter's T31 advance runs
  before voice 0's compute and the noise step in that same slot, so voice 0's envelope-rate check,
  the noise step, and voices 1–7's checks in the following frame's slots all read one counter value —
  a rate fires for all eight voices on the same 32 kHz sample.
- **The noise steps ahead of voice 0's compute in the same slot.** Voice 0 reads the level the T31
  step just produced, and voices 1–7 read that same level at their compute slots in the frame that
  follows, so every voice outputting noise carries one noise level per delivered frame. A `FLG`
  noise-rate write is read at T31: one landing before that slot changes whether the sample's step
  fires; one landing after waits for the next sample.
- **The echo write lags its compute.** The echo unit computes its buffer write at T24 but the bytes
  land at T30 (left word) and T31 (right word), so a program reading the entry between those slots
  still sees the previous sample there.
- **`ESA` and `EDL` apply one sample late.** The echo unit addresses the base its ring advance
  applied at the previous sample's close, so a write to `ESA` moves the buffer only from the next
  sample — the sample in flight still reads and writes the old base — and an `EDL` write resizes the
  ring at its next wrap.
- **`VxPITCHL`/`VxPITCHH` are not read at a voice's own compute.** The pitch a voice's stream
  advance uses is captured for all eight voices at the first slot of every sample, and a capture
  reaches a voice's advance one sample deep: voice 0's compute at T31 is the only one late enough to
  see its own sample's capture, while voices 1–7 advance by the previous sample's. A pitch value
  standing for exactly one sample is therefore consumed for exactly one advance by every voice,
  whatever slot its writes land on. Pitch *modulation* is unaffected — `PMON` scales the captured
  step by the previous voice's current amplitude every sample.
- **A key-on holds the pitch capture for seven samples.** Each consumed key-on — including one
  absorbed or rewound inside the silent span — schedules its voice's capture for the first slot of
  the poll-parity sample after the consuming poll, and suspends the per-sample capture for seven
  samples from that poll. Through the hold a walking startup advances at the pitch that scheduled
  capture took; a bare `VxPITCH` write inside the hold never reaches the stream, while one landing between
  the poll and the parity sample's first slot does — it is what the scheduled capture reads. The
  hold is anchored to the poll, so it covers the same samples for every voice.

This intra-sample schedule is derived from the S-DSP timing charts; the key-on countdown, the last
slot's placement of the keying poll and its order against voice 0's compute, the `VxENVX` publish
slot and its one-sample value lag, the echo write slots, the startup walk of a re-keyed young
voice and the startup hold of a silent or long-sounding one, the
two-tier handling of a key-on landing inside the silent span, the per-sample pitch capture with
its key-on hold, the soft-reset shield on a key-on's consumption sample, the reset sample's own
emission under the level it carried in, the full restart a
key-on performs on a keyed-off in-span voice, the final pre-key-on sample a full restart's
consuming compute still emits, the decoder's eight-sample lead over the cursor with the check one
sample behind it — including the first envelope steps a mid-startup crossing still publishes —
the fraction-free startup walk that makes the first sounding sample interpolate from index 0, the
group-ahead data decode whose twelve key-on-primed samples a RAM rewrite cannot reach — and whose
later groups read their header at the scheduling two samples past the boundary and their data
bytes at the load and post-compute slots one sample later — the
one-sample-late application of `ESA`/`EDL`, the counter's advance ahead of the checks that share
its slot, the in-group position clamp at `$7FFF` on an uncapped pitch step, the one-sample lag on
the amplitude pitch modulation reads, the `ENDX` set at the
decoder's exit from an end block — readable three slots after the voice's compute, in the next
sample for voice 0 — the re-key's walk-or-hold decision read after the consuming compute's own
envelope update, and the echo write sweeping the RAM beneath `$F0`–`$FF`
without touching the `$F8`/`$F9` port bytes are confirmed against the Blargg DSP test ROM, while
the `VxOUTX` publish slot remains the least-certain part.

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
- **A voice ends early.** The decoder runs eight samples ahead of the interpolation cursor, so an
  End+Mute block releases the voice while the last eight samples of the block before are still
  unplayed — and the End+Mute block's own samples never sound. The loop jump happens at the
  decoder's entry to the next block, ahead of the sound by the same distance, and the loop address
  it takes was read at the voice's directory slot earlier in that sample.
- **`ENDX` comes late, not early.** The bit marks the decoder leaving the end block, a full block
  after it entered — for a voice an End+Mute block silenced, sixteen stream samples after it went
  quiet. Poll `ENDX` to learn that a sample has ended, not to learn that it is about to.
- **A same-sample `GAIN` write decides how a re-key starts.** A re-keyed voice's startup walks only
  if its level is still above zero after the consuming compute's own envelope update (and the voice
  is young — see above) and holds if not, so dropping a sounding voice to direct gain 0 on the
  sample its key-on is consumed gives it a held startup, exactly as a key-on from silence would.
- **Re-keying a voice that has sounded for long holds its stream.** A voice re-keyed about a second
  after its own key-on starts like a voice keyed from silence, whatever its level: its first sounding
  sample interpolates the stream's first four samples at index 0. Only a re-key close behind the
  voice's key-on walks.
- **A stopped voice still reads its header.** Setting `VxPITCH` to zero stops the decode, not the
  per-sample header read, so a block that becomes End+Mute underneath such a voice still releases it.
- **Rewriting sample data is not instant.** BRR bytes are read when their group is decoded — the
  sample after the cursor crosses into the group two ahead of it, seven to eleven samples ahead of
  the one playing — and a key-on reads twelve up front. So a data write reaches the output that
  many samples late, and never reaches the samples already decoded; a header shift rewritten under
  a moving voice reaches the group crossed into one sample earlier, not that one. A voice parked at
  pitch zero keeps its primed samples whatever happens to the RAM under it; the header check is the
  one live read (above).
- **A pitch write is not instant.** The stream reads a pitch capture taken each sample, not the
  register, so a `VxPITCH` write takes effect one or two samples later — one sample sooner for
  voice 0 than for voices 1–7 — and on a freshly keyed voice not until its key-on's capture hold
  runs out. See [When a register write takes effect](#when-a-register-write-takes-effect).
- **A modulated voice can park.** Pitch modulation can push a step past four source samples; the
  counter's in-group position then clamps at `$7FFF` and parks at the group's last fraction, and the
  next smaller step crosses a sample position at once. A voice driven past the ceiling and then set to
  a low pitch moves one sample on the first step rather than accumulating from zero.
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
