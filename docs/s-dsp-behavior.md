# S-DSP behavior and provenance

A reference for the parts of the SNES S-DSP where the published hardware documentation is
incomplete, ambiguous, or wrong — and for what settles each case.

`dsp.md` is the usage guide: what Snaggletooth's DSP surface is and how to drive it. This page is the
evidence behind it. Every claim below names where it comes from, and where two sources disagree it
names the disagreement and what decided it.

## The evidentiary standard

Three kinds of claim appear here, and they are not equally strong:

- **Documented and corroborated.** Two independent published sources agree. Taken as given.
- **Documented but contested.** The published sources disagree, or one is self-contradictory. Decided
  by a hardware test ROM, and the decision is recorded with the measurement that forced it.
- **Undocumented.** No published source states it. Derived from a test ROM's expected output, which
  means the claim is only as good as the reconstruction behind it — so the reconstruction is
  described, not just asserted.

Where a test ROM decides a question, it outranks both documents. A document describes what someone
understood; a test ROM's expected output is what the hardware actually produced when the ROM's author
measured it.

**Nothing here is derived from another emulator.** Snaggletooth is a clean-room implementation: the
contract is public hardware documentation plus the observable behavior of test ROMs. That constraint
is what makes this page publishable, and it is also why the corrections below took real work —
copying a known-good implementation would have skipped the questions entirely and taught nothing.

## Sources

| Source | Role |
|---|---|
| Martin Korth's fullsnes, "SNES APU DSP" | Primary. Register tables, the per-cycle access chart, the BRR and Gaussian arithmetic. |
| Anomie's S-DSP Doc (Rev 1157) | Cross-check. The per-sample voice loop, the envelope update list, the key-on account. |
| SNESdev Wiki, "DSP envelopes" | Secondary. Counter period/offset tables and the counter fire rule. Cites Anomie as its only source, so it is a restatement rather than an independent third derivation. |
| SNESdev Wiki, "Errata" | Secondary, quirks only. |
| Blargg's DSP test ROMs | **Arbiter.** Where documents conflict or are silent, these decide. |

---

## The envelope

### The update runs every sample; the counter gates only the store

Anomie's numbered list is precise and easy to misread. Per sample, the selected mode computes a
candidate level, and then:

1. **If the rate counter fires**, the envelope takes the candidate, clamped to 11 bits.
2. If the mode is Decay and the sustain boundary is matched, enter Sustain.
3. If the mode is Attack and the candidate exceeds `$7FF`, enter Decay.
4. Save the candidate as the reference the next Bent-Increase step reads.

**The counter condition sits on step 1 alone.** Steps 2, 3 and 4 happen whether or not it fires. An
implementation that returns early when the counter has not fired cannot change phase while a rate
holds a level still — and a rate of 0 holds it still forever.

*Arbitrated by `Envelope/attack->decay during gain`.* Its blocks end on a rate of 0 and still leave
Attack, which is what the sub-test's name means.

### Attack ends when the step leaves the 11-bit range, not at a fixed level

fullsnes says Attack switches to Decay at `Level >= $7E0`. That is **refuted**. The switch fires when
the candidate leaves the unsigned 11-bit range — in either direction.

The test ROM brackets each mode around the boundary, and the pairs only fit the range rule:

| Mode | Step | Stays in Attack | Leaves Attack |
|---|---|---|---|
| Linear Increase | +32 | `$7DF` | `$7E0` |
| Bent Increase | +8 | `$7F7` | `$7F8` |
| Linear Decrease | −32 | `$020` | `$01F` |

fullsnes's `$7E0` coincides with the Linear-Increase pair only because `$7E0 + 32 = $800`; the
Bent-Increase pair sits `$18` higher, and the Linear-Decrease pair is driven *below zero*
(`$1F − 32 = −1`). The boundary is the step, not a level. Anomie's model — pretend Linear Increase,
overshoot, clip — is the coherent one, and his note that "a negative value for the new value will
result in the clipped version being greater than 0x600" already implies the comparison sees an
unclamped value.

Implemented as: the candidate is outside the unsigned 11-bit range.

### Decay reads its sustain boundary from whichever register is driving the envelope

Under ADSR the boundary is `VxADSR2` bits 7-5. **Under a GAIN mode it is `VxGAIN` bits 7-5** — which
are the gain mode and its enable bit, not a sustain level at all, so a voice keyed on under GAIN
reaches Sustain at a boundary its author never chose. fullsnes states this in "Gain Notes" and calls
it what it is: "accidently reading a garbage boundary value".

The comparison is on the upper 3 bits (Anomie's form), against fullsnes's `Level <= Boundary`. The two
differ only when a decay lands exactly on a `$100` boundary, and Anomie's is the exact one.

The comparison reads the **candidate the mode computes, every sample**, rather than a counter-gated
stored level — so a voice parked at rate 0 still changes phase when the value its mode computes
reaches the boundary.

*Arbitrated by `Envelope/decay->sustain during gain`.*

### A decay whose level already sits in the sustain band sustains without a step

Under ADSR, a voice in Decay whose stored level's upper three bits already equal the sustain level
is in Sustain before its step: the phase changes and the level holds, whatever the decay rate does
on that sample. The case that shows it is a decay entered from the top — ADSR `$EF/$E0`: attack rate
31 takes the level to `$7FF`, the sustain level is 7, so the level is in band the sample Decay
begins, and the sustain rate is 0. A model that runs the decay step first stores `$7F7` when rate 28
(period 4) fires, one sample in four; the tape carries `$7FF` from every counter phase.

Everywhere the level is still above the band, the documented order stands: the step is computed,
stored if the decay rate fires, and the candidate is what the boundary check reads. And the
stored-level check is ADSR's alone — under a GAIN mode `Envelope/attack->decay during gain` reads
the candidate, not the stored level.

The driver of `Random/voice volumes` is what makes the phase visible: its sync gadget locks the CPU
to the sample and to the key-on poll's parity, which is a lock modulo 2, while rate 28 fires modulo
4 — so two arrivals of the same driver at the same in-sample phase can sit on different counter
phases. On hardware the arrival phase is not under the driver's control (the SNES-side upload
crosses two unrelated clocks), so a test that hashes the tape has to read the same envelope from
every phase, and it does.

Attack→Decay is not the same switch: its store stays under the attack rate. Gating it by the decay
rate regresses `Envelope/envelope rates` and `Envelope/hidden env after adsr`.

Two other forms fit the same evidence — the sample that detects the switch storing nothing under
ADSR, or its store gated by the sustain rate instead of the decay rate — because the one ADSR arbiter
has a sustain rate of 0 and a level already in band. Both would also hold a decay arriving at the
boundary from above, where the documented order stores the step; a sub-test that decays into the band
from above with a firing sustain rate would separate them.

*Arbitrated by `Random/voice volumes`, run from two arrival phases two samples apart.*

### The Bent-Increase reference is saved every sample and is not clipped

Bent Increase adds +32 below `$600` and +8 at or above it. The value it compares is a reference saved
the previous sample. Both documents describe that reference as "the new value, *clipped* to 11 bits".
Two corrections:

- **It is saved outside the counter gate.** A mode parked at rate 0 computes a value every sample and
  supplies it as the reference while the level stands still. The ROM's pair proves it: `$5E0` under a
  rate-0 Linear Increase computes `$600`, and the following Bent Increase takes **+8** from a level
  *below* the boundary; `$606` under a rate-0 Exponential Decrease computes `$5FF` and takes **+32**
  from a level *above* it. Only a reference read from the parked mode's own computed value inverts
  those two.
- **It is not 11 bits.** `$7E0 + 32 = $800` leaves the range and the hardware still takes **+8**;
  masked to 11 bits it would read `$000` and take +32. Its bracket `$7DF + 32 = $7FF` stays inside and
  also takes +8, so the pair differs by one in the level and not in the step. **Exactly 11 bits is
  refuted**; 12 or more satisfies every measurement.

Both documents are wrong about the width. They are right about the negative case, which is the half
they illustrate.

*Arbitrated by `Envelope/gain $E0 threshold`.*

### The envelope is applied before it is updated

**A sample is scaled by the level already standing — the one the previous sample's update left — and
this sample's update runs afterwards.** So a level in motion reaches the output one sample after the
step that produced it, and a level holding still is indistinguishable either way.

Anomie's voice loop applies the volume envelope early and updates it last, and his key-on account
states the consequence outright: at the sample where envelope updating begins, "the sample output is
still '0000', because of the order in which voice operations are performed".

*Arbitrated by `KON/gain used on last kon sample`.* Its driver keys a voice on, walks Direct Gain
through `$20`/`$30`/`$40`/`$50`/`$60` at one-sample intervals, and captures the voice through the echo
buffer, so the captured row is the envelope trajectory. Expected against scale-by-the-new-level:

```
hardware     0000 0000 0400 0500 0600 0600 0600 0600
new level    0000 0400 0500 0600 0600 0600 0600 0600
```

Identical values, displaced by exactly one sample. That displacement can only come from the apply /
update order: a change in *when* the envelope goes live would change *which* `VxGAIN` the first live
step reads, and so would change the values themselves.

**Why this is easy to miss:** a level that is not moving gives the same answer under either order, and
most envelope measurements read a level at rest. It takes a case where the level is moving at the
instant it is read.

---

## Key-on and key-off

### A key-on is followed by six silent samples, not five

The documented figure is five empty samples "before envelope updates and BRR decoding actually
begin", and that is accurate as stated — but it counts the samples *before* the first envelope step,
not the silent ones. Anomie's numbered account is the precise version:

| Sample | What happens |
|---|---|
| `#0` | The key-on itself. Envelope set to 0, state set to Attack, interpolation index reset. No envelope update. |
| `#1`–`#4` | No envelope update, no interpolation update. BRR groups preload. |
| `#5` | **Envelope updating begins — and the output is still `0000`**, because the level scaling this sample is the zero the key-on set. |
| `#6` | The first data sample. |

So six samples are silent. The fifth-versus-sixth confusion is entirely a consequence of the
apply-before-update order above.

### Whether the silent samples advance the stream depends on the voice the key-on lands on

Anomie's account above holds the decode position still until the startup ends — "no interpolation
update" through `#4` with fixed BRR group preloads in their place — and fullsnes quotes the same
shape. The test ROMs split the question by the state of the voice being keyed:

- **A key-on that interrupts a sounding voice walks.** `KON/kon decoding when another kon` re-keys a
  voice its driver keyed 21 samples earlier — sounding when the re-key lands — freezes its pitch
  mid-startup by zeroing `VxPITCH`, and reads the voice's output back through the echo buffer — a
  pure function of where the decode cursor stood when the pitch froze. The expected table walks:
  across re-keys frozen at one-sample-later instants, the read moves through the source's loud
  region exactly as a cursor advancing **two stream samples per output sample** (the sub-test's
  pitch) would place it. A cursor holding at its preload prints all zeros; fixed four-sample group
  preloads land the loud readings in the wrong places.
- **A key-on of a silent voice holds, exactly as the documents describe.** `Misc/brr addr
  wrap-around` keys a long-released voice (envelope at 0) at pitch `$1000` and records its first
  samples through the echo buffer. The expected rows have the first sounding sample interpolating
  the stream's **first four samples** at Gaussian index 0 — the primed window, unmoved through the
  whole silent span *and* that first sounding sample — with the position advancing one stream
  sample per output sample only from the sample after. A stream that walks through the silence
  starts six samples deep and prints a different table.

- **A key-on that interrupts a voice that has sounded for long holds too.** `Random/brr before
  playing` fills RAM with random BRR blocks, keys voice 0 over them at pitch `$1000` and direct gain
  `$7F0`, and hashes a 30 KB echo tape of its output — eight times over, the later seven key-ons
  landing on the voice while it is still playing the previous pass's data, about a second after its
  own key-on. The hash the ROM expects is reproduced only when every pass's tape has the same
  alignment as the first — the pass that keys the voice from silence: the first sounding sample
  interpolates the stream's first four samples at index 0. A model that walks the seven re-keys
  lands their data six frames early and hashes to a different constant.

Two conditions therefore gate the walk, both read as the restart applies. The envelope: at 0 the
startup holds (`Misc/brr addr wrap-around`; a register write that drops the level to zero on the
consuming sample gives a sounding voice a held startup — see [the final pre-key-on
sample](#a-full-restarts-consuming-sample-still-emits-the-final-pre-key-on-sample), after which
the standing envelope has run its own update once more). And the old OUTPUT: a voice whose output
was sounding holds, one whose level stands above zero while its output is still silent — a voice
inside an earlier startup's own silence — walks. The 21-samples-later re-key that walks (`KON/kon
decoding when another kon`) lands on exactly such a voice; the re-keys that hold land on voices
whose old streams were audibly playing — about a second old at `$7F0` (`Random/brr before
playing`) and some sixty samples old, loud, at direct gain `$400` (`Timing/Voice/V3 BRR.sample.*`,
whose thirty-four rows per voice each re-key it and read every row's data bytes on the held
cadence). No age window separates the cases — sixty samples holds and no record of the key-on
need outlast the silence — and no threshold on the level does either: `$010` walks, `$7F0` holds,
a level-0 voice holds. Nor the envelope phase: the `$7F0` voice stands in Attack, as
`Envelope/attack->decay during gain`'s seventh block shows for a direct-gain level set above
`$7E0`.

In the walking case the first silent sample is the exception — it decodes nothing (the start
pointer is read at the next sample's directory slot; see
[below](#the-start-pointer-is-read-at-the-voices-directory-slot-one-sample-after-the-consuming-compute)),
so the walk begins on the second. Only the *output* is silent during the
startup; the position is live throughout. The pitch the walk uses is the one the key-on itself
captured — that sub-test's freezes land because each rides a `KON` write of its own; a pitch write
with no key-on behind it
waits out the capture hold (see [the pitch capture](#the-pitch-is-captured-once-per-sample--and-a-key-on-holds-the-capture-for-seven)).

`KON`/`KOFF` are polled every other sample, at the last slot of the sample — the same slot at which
voice 0 runs its whole compute. **The keying load runs first.** A keyed voice 0 therefore takes the
load's own slot as the first of its silent startup calls, while voices 1-7 take theirs beginning in
the following sample.

Anomie is the only source that orders the events inside that slot, so this was single-sourced until a
test ROM settled it. *Arbitrated by `KON/envx during kon`*, whose expected table has all eight voices'
key-on startup reading identically — which the opposite order cannot produce for voice 0, under any
read slot.

The startup counter is **five, uniform across all eight voices**. The shared-slot asymmetry, not a
per-voice count, is what makes the eight voices read alike.

### The startup walk keeps no interpolation fraction

The walk above (the sounding-voice case) is a whole-sample walk. Sample positions cross and decode
exactly as the pitch dictates, but the fractional remainder does not accumulate: when the first
sounding sample interpolates, its Gaussian index is 0 — the exact start of the position the walk
reached — and the fraction begins climbing only with the advance after it. A held startup has no
fraction to discard: its position never moves. Neither document says anything about the fraction
during the startup; the accounts that hold the position still imply nothing to discard, and
the walk the previous section establishes leaves the question open.

*Derived from `KON/pitch at kon`.* The sub-test keys a voice at pitch `$0010` — one Gaussian index
per sample, never crossing a sample position — over a ramp block, and reads the first twenty audible
samples back through the echo buffer. The expected readings walk the interpolation kernel from index
0 upward: `0000 0008 0018 0026 0036 …`. A walk that banks its startup fraction starts six indices
deep instead — the five advancing silent samples plus the first sounding sample's own advance — and
plays `0000 0064 0074 0084 …`, which is exactly the sequence the fraction-free rule forbids. The
silence's length is identical either way, so the sub-test isolates the fraction alone: the sound
begins at the same sample in both models and differs only in where the kernel's walk starts.

The two rules compose rather than conflict: `KON/kon decoding when another kon` measures the
cursor's whole-sample positions and is blind to the fraction (its pitch is a whole-sample step);
`KON/pitch at kon` measures the fraction and never crosses a position. What the pair establishes
jointly is a startup that decodes at the pitch while discarding the sub-sample remainder.

A `KON` write arms the next poll, which starts the armed voices and then disarms itself — so a bit
left set does not start the voice again, and the register keeps its value for read-back. `KOFF` is
read from its register at every poll instead, so it keeps releasing for as long as the bit stands.
`FLG` bit 7 is polled every sample rather than every other one.

### The 128 kHz ceiling is a clamp on the position, not a cap on the step

A voice's pitch step is added to the low fourteen bits of its pitch counter — its position within
the four-sample group it is consuming — and the sum clamps at `$7FFF` before the crossed sample
positions are counted. The step itself is never capped: a pitch-modulated step reaches `$7FEE`.
The clamp is what holds the advance to four source samples per output sample, and it has a
consequence a step cap does not: a position the clamp catches lands on the group's last fraction
(`$3FFF` once the group turns) and stays pinned there for as long as the step exceeds the group,
so the first smaller step afterwards — even a step of `1` — crosses into the next group at once.
A step cap would leave the fraction free to drift, and a later step of `1` would need up to `$1000`
samples to cross.

fullsnes states that the step or the counter result "is cropped to 128kHz max" and marks the
placement unknown (its line 2793, "XXX somewhere here"). Anomie places it concretely: the
interpolation index gains the pitch and is clamped to `$7FFF` (lines 372–374). Only the clamp on
the position produces the parking behaviour; a cap on the step does not, and the two are otherwise
indistinguishable at any base pitch, since a base step cannot exceed `$3FFF`.

*Derived from `Misc/interp pos clamped at $7FFF`.* The sub-test modulates voice 2 by a stationary
voice 1 (a constant `+$3800` block at pitch 0, direct gain `$13`) with base pitch `$3FBA`, so the
modulated step is about `$4800` — past the group — and lets the voice run for a few samples; then
it sets the pitch to `0`, then to `1`, and reads twenty echo-buffer words of voice 2's output. Under
the position clamp the voice's position parks at `$3FFF` within a few samples and the step of `1`
crosses on its first sample; under a step cap the fraction drifts (`$3FFF`, `$7FFE`, `$BFFD`, …)
and the step of `1` crosses nine samples later, so the two models play different samples into the
buffer. The clamped model's twenty words reproduce the driver's compare constant exactly, and the
ROM ratifies by advancing.

### A key-on landing on a voice inside its silent span is absorbed or rewinds the silence

Both documents state that keying a voice that is already playing restarts it in full — envelope to
zero, stream to the start, the empty startup samples again, the audible click. Both are silent on
the narrower case: a key-on consumed by the poll while the voice is still silent from the last one.
The test ROM decides it (`KON/kon clears independent`): two `KON` writes ~53 SPC cycles
apart, deliberately inside one 64-cycle poll period and synchronized so a poll falls between them,
the first naming voice 0 and the second naming voices 0 and 1. The expected capture shows voice 0
sounding **alone for exactly two samples** — one poll period — before voice 1 joins, and voice 0
never restarting. So the second poll's key-on of voice 0 was absorbed: a key-on that lands during
the startup neither resets the countdown nor restarts the stream.

The alternative reading — that the second write could not re-arm voice 0 because its register bit
never returned to 0 — fits this capture equally well, but is refuted by `Envelope/hidden env 0 at
kon`, whose driver re-keys a long-playing voice by rewriting a `KON` value whose bit stood at 1
throughout, and expects the restart. The register's history does not gate the arm; the voice's own
startup state gates the action.

**Full absorption is only one poll deep; a later in-span key-on rewinds the silence without moving
the stream.** A key-on produces six silent samples — the five empty ones plus the first envelope
step's still-silent sample — and what a key-on consumed inside those six does splits by which poll
takes it:

- **Consumed at the poll immediately after the one that keyed the voice: absorbed outright.**
  Countdown, stream and envelope schedule all stand — this is the `KON/kon clears independent`
  case above, and `KON/kon then another kon`'s first two readings confirm the envelope emerges on
  the original key-on's schedule.
- **Consumed at a later poll inside the span: the silence rewinds, the stream does not.** The
  five-sample countdown re-arms in full and the envelope drops to zero, so the level emerges
  exactly as late as a full restart would place it — but the decode cursor keeps the position it
  has walked to and keeps advancing at the pitch.

One consumed from the first sounding sample on restarts the voice in full — and so does one
consumed on a voice keyed off inside the span, because the tiers presuppose a startup that is
still standing (the section after the shield, below). Three ROMs together pin the standing-startup
shape, each blind to what the others measure. `KON/kon decoding when another kon` watches only
the *stream*: its expected table holds a frozen cursor position through re-keys consumed up to six
samples after the load, so no in-span key-on re-primes the decode. `KON/kon then another kon`
watches only the *envelope*: it re-keys a voice at a widening spacing and counts samples until
`VxENVX` turns non-zero, and its expected counts (`3 2 5 4 5 4 5 4`) read restart-late from the
second in-span poll on — while a model that fully absorbs those key-ons reads `3 2 1 0 0 0` there,
and one that also rewinds at the first poll reads `5 4` everywhere. `KON/envx during kon` bounds the
far edge: a re-key consumed eight computes after the load restarts in full, stream included.

### A key-on's consumption sample is shielded from a soft reset

Both documents describe `FLG` bit 7 the same way: every voice keyed off, its envelope forced to
zero, applied every sample rather than on the `KON`/`KOFF` poll grid. Neither says what happens when
the same sample both consumes a voice's key-on and carries a standing soft reset.

The test ROM decides it (`KON/kon then flg.80`): key a voice on, raise `FLG` bit 7 for exactly one
sample, sliding the pulse one sample later per reading, and count samples until `VxENVX` turns
non-zero. The expected readings are `06 05 80 80 80 80 80 80 80 80`, uniform across the eight
voices:

- A pulse over the sample **before** the consuming poll falls on a voice that is not yet keyed —
  harmless — and the key-on starts on schedule one sample later (six samples to a non-zero read,
  counted from the pulse).
- A pulse over the consuming poll's **own** sample leaves the startup's `VxENVX` schedule exactly
  where an unmolested key-on places it: five samples to a non-zero read, one fewer than the reading
  before only because the startup began one sample closer to the counting.
- A pulse over **any later** sample — still inside the silent span or past it — keys the voice off
  for good: envelope forced to zero, release, and with nothing re-arming it the count never
  terminates.

So a soft reset does not key a voice off in the sample its key-on is consumed: the fresh consumption
wins, the same precedence `KON` has over `KOFF` at the poll itself. The shield is exactly one sample
wide — the same one-sample pulse, slid one sample in each direction, pins both of its edges.

### A soft reset silences the sample after the one it lands on

Both documents describe what `FLG` bit 7 does — key-off, envelope to zero, every sample — without
saying whether the sample it lands on is itself silent. Anomie's per-voice list carries the answer
implicitly: the envelope is applied and `VxOUTX` formed, *then* bit 7 is checked, *then* the
envelope is updated (his V3c), which puts the reset in the same position as any other envelope
update — one sample behind the output.

The test ROM decides it (`Order/flg.80 after env used`): voice 0 is keyed on over a self-looping
full-scale block at Direct Gain `$01` with `VxVOLL` at −1 and its echo send on, so every live sample
writes `$FFFE` to the echo tape and every silent one `$0000`; nine samples after the key-on `FLG`
bit 7 is raised, and the tape's frames 7–16 are checksummed:

```
hardware     FFFE FFFE FFFE FFFE FFFE 0000 0000 0000 0000 0000
reset-sample silent
             FFFE FFFE FFFE FFFE 0000 0000 0000 0000 0000 0000
```

One frame more of sound. The sample the reset lands on is emitted in full under the level the voice
carried in; the zeroed level is what the next sample scales by. It is the "applied before it is
updated" displacement above, seen from the reset rather than from a gain write, and the tape shows
it directly because the voice's level is at full scale when the reset arrives.

### A key-on consumed on a keyed-off in-span voice restarts it in full

The in-span tiers above — absorption one poll deep, the rewind at later polls — describe a key-on
landing on a startup that is still running. Neither document says what a key-on does to a voice
that was keyed off *while still inside its span*, where the span's samples are still counting but
the startup they were counting for is dead.

The test ROM decides it (`KON/kon then flg.80 then kon`): key a voice on, then slide a
fixed-offset pair — a one-sample `FLG` bit-7 pulse followed a fixed few samples later by a second
`KON` write held for only a few cycles — one sample later per reading, and count samples until
`VxENVX` turns non-zero. The expected readings are `04 80 06 80 06 80 06 80`, uniform across the
eight voices, and the alternation is the poll grid: the second key-on's brief write is visible to
the every-other-sample poll on even alignments only.

- **Reading 0**: the pulse covers the first key-on's consumption sample — the shield above — and
  the second key-on is not seen, so the count is the original startup's, already two samples in.
- **Even readings 2–6**: the pulse kills the startup (release, envelope forced to zero), and the
  second key-on is consumed two samples later. The count is a full restart's — *identical whether
  the kill and re-key fall inside the span or past it* — so the consumed key-on neither absorbs
  into nor rewinds the dead startup: the voice restarts in full, countdown re-armed, envelope from
  zero, stream re-primed to the start address.
- **Odd readings**: the poll parity misses the second key-on's brief write, and the reset's
  key-off stands unanswered — the count never terminates.

The rule is the voice's keyed-off state, not the reset's doing. One release condition on the
keying poll's in-span tiers carries the whole family: `KON/kon then koff` (a `KOFF` kill inside
the span, then a re-key) and `KON/kon then set sample's end flag` (an End+Mute block header's
release) pass with the same arm that passes `KON/kon then flg.80 then kon`. It is also what lets
`KON` win when a single poll delivers `KON` and `KOFF` together to an in-span voice — the `KOFF`
releases first within the poll, and the key-on then lands on a keyed-off voice and restarts it.

### A full restart's consuming sample still emits the final pre-key-on sample

A full restart does not silence its voice on the sample whose poll consumes the key-on. The
voice's own compute in that sample prepares one more sample from the state standing before the
restart — the old stream takes its final decode and advance, and its sample is emitted under the
standing envelope — and only then does the restart apply: stream re-primed, envelope to zero,
Attack, with the startup countdown counting from that same call, so every later sample of the
startup lands exactly where a key-on always places it.

The test ROM decides it (`KON/kon unaffected by pitch`): a sounding voice — constant full-scale
sample data, so its live output is a fixed word and pitch cannot change it — is re-keyed, and the
echo buffer, serving as a per-sample tape of the voice's output, is read back over a seven-frame
window spanning the re-key. The expected window is `FFFE 0000 0000 0000 0000 0000 FFFE`: the old
data still sounds on the window's first frame, the restart's silence fills the middle, and the
new startup's first sounding sample lands at the same frame a from-silence key-on would produce.
The same window repeats identically with the pitch at `$3F00` and at zero — the sub-test's
namesake, and the constant-data design that makes the timing the only signal. A model that
silences the voice on the consuming sample itself prints `0000` in the first frame and misses
nothing else — a one-frame difference in a fourteen-word row, in both pitch runs.

Anomie's per-sample key-on account states the same rule from the hardware side: "After the final
pre-KON sample is prepared, the envelope is set to 0 … The final pre-KON BRR decode also occurs
here." The final decode is real, not an idle detail: it can complete an end block, and the
key-on's `ENDX` clear then erases that same sample's set — the suppression `KON/kon stops endx
of prev sample` measures.

The standing envelope, too, takes its own update on that compute before the restart reads it.
What the restart reads decides whether the new startup walks or holds (the split above), and it
is the level the voice would have carried into the next sample, not the one the final pre-key-on
sample was emitted with. `Order/endx after final brr decode` forces this: its sync gadget leaves
voice 4 sounding at full level, restores the voice's `ADSR1` to zero — direct gain, level zero — on
the compute just before the poll that consumes the test's own key-on of that voice, and the
expected row for voice 4 is a held startup's, identical to the seven voices keyed from silence. A
restart reading the level before the update walks that voice and prints its `ENDX` six samples
early.

---

## The intra-sample schedule

Each 32 kHz sample is built over 32 clock slots, and register reads land on documented slots — so a
CPU write lands where the hardware takes it rather than at a sample boundary. The map below merges
fullsnes's per-cycle access chart with Anomie's voice loop; Anomie's slot *k* is fullsnes's T*(k+1)*.

| Slot | Work |
|---|---|
| T1, T4, T7, T10, T13, T16, T19; T22 | Each voice's directory read — the loop address its compute's loop jump takes (voices 1-7 one slot before their compute; voice 0 at T22, nine before its), and a keyed voice's start pointer, in the sample after the compute that applied the key-on. |
| T2, T5, T8, T11, T14, T17, T20 | Voices 1-7 each run a whole compute (stream, noise, envelope, amplitude). |
| T3, T6, T9, T12, T15, T18, T21, T24 | Each voice's `ENDX` set becomes readable, three slots after its compute — voice 0's at T3 of the following sample. |
| T23 / T24 | Echo reads; the echo value is computed here. |
| T27 / T28 | Left then right output finalize — master and echo volume, then the mute gate. |
| T29 / T30 | `FLG` bit 5 loads, one slot ahead of each echo word it gates. |
| T30 / T31 | The echo buffer's left word, then its right word. |
| T30 → T31 | Raw `ESA`/`EDL` load, then the ring advance applies them and steps the index. |
| T31 | The `KON`/`KOFF` load, the global counter's advance, the noise step, then voice 0's whole compute. |

Voice 0 computes at the last slot and applies at the following sample's first slots, so its output
rides one sample behind the others while its state trajectory stays in step.

### The counter advances ahead of the checks that share its slot

The global counter's once-per-sample advance runs at T31 **before** voice 0's compute and the noise
step in that same slot. Every rate check therefore reads the just-advanced value: voice 0's envelope
check and the noise step at T31 itself, and voices 1-7's checks at their compute slots in the frame
that follows. Between two advances all eight voices read one counter value, so a rate fires for all
eight on the same 32 kHz sample.

Anomie's cycle-30 listing carries the opposite order for the envelope half — it names voice 0's V3c
first and "update global counter" after it (with the noise update after the counter, which the ROM
agrees with). The listing's within-cycle sequence is not the execution order: an implementation that
lets voice 0's check read the pre-advance value measures every envelope step one sample late against
a phase reference taken through another voice. The noise step sits on the same side of voice 0's
compute as the counter (next section).

*Derived from `Misc/counter rate synchronizations`.* The sub-test locks itself to the counter's
absolute phase through voice 4 — GAIN linear increase at rate 1 until its `ENVX` moves, then two
windows in which rate 2 and rate 3 must *not* fire, restarting until the phase fits — and then, for
each GAIN value `$C1`–`$DF` (rates 1–31), keys voice 0 on and counts polling iterations until
`V0ENVX` first moves, checksumming the 31 recorded counts. The lock and the measurement deliberately
sit on different voices, and that cross-reference is the whole discriminator: rates 2–31 are each
timed from the previous measurement's own exit, so a uniform one-sample shift of voice 0's fires
cancels row to row, while rate 1 alone is timed from the voice-4 lock. The two orders differ in
exactly that one word of the 31 — the pre-advance read measures rate 1 one poll longer — and the
driver's checksum pins it: with the advance ahead of voice 0's compute the table hits the expected
accumulator exactly and the ROM advances.

### The noise steps ahead of voice 0's compute in the same slot

The T31 noise step — the `FLG` bits 0–4 read and the LFSR advance — runs **before** voice 0's compute,
so voice 0 outputs the level this sample's step produced, the same level voices 1–7 read at their
compute slots in the frame that follows. One noise level reaches the output per delivered frame,
whichever voice carries it. Anomie's cycle 30 lists voice 0's V3c ahead of "load FLG bits 0-4 and
update noise sample"; as with the counter, the listing's order is not the execution order — a voice 0
reading the pre-step level trails the other seven by one step for as long as the noise runs.

*Derived from `Order/noise rate flg.1F`.* The sub-test runs voice 0 alone on noise (`NON`, `EON`,
direct gain `$7F`, `VOLL` −128) into a one-entry echo buffer at `$0000`, sets rate 31 and spins on
the entry until voice 0's noise sample reads back as zero — the LFSR walking down to level 1, which
the envelope scales to nothing — then stops the rate in the next few cycles. The level left standing
depends on how many steps fit between the sample that read as zero and the rate write: the ROM's
expected table (recovered from the driver's CRC constant as the unique walk along the LFSR) is
`$2000` — two steps, `1 → $4000 → $2000` — where a voice 0 reading the pre-step level publishes its
zero one sample later, the CPU exits one iteration later, and three steps fit. The sub-test then
moves the buffer to `$F000`, re-enables the rate and freezes the buffer eleven samples on; the first
stepped level lands on frame 6 of the tape, one frame earlier than the trailing model puts it. Both
halves of the row move by the single reordering.

### The echo buffer writes land at T30/T31

Both documents say so — fullsnes as explicit chart rows, Anomie at his cycles 29 and 30 — but it is
easy to lump the write in with the echo *read* at T23/T24, six or seven slots early. It is
CPU-visible: a driver reading the buffer at a fixed offset from its own key-on races the write, and
the six-slot error changes which side of the race it lands on.

### `ESA` and `EDL` apply one sample after the write

Anomie's chart carries both halves — the raw registers are loaded at his cycle 29 "for future use"
and applied at cycle 30, `EDL` only when the ring index is 0 — while fullsnes says only that "ESA is
accessed during cycle 29", which does not distinguish the load from its application. The lag is the
half that matters: the sample in flight when `ESA` changes still reads **and writes** the old base,
and the new base takes over from the next sample.

*Derived from `Misc/$F0-$FF are not ram`.* The sub-test parks a single-entry echo buffer at `$F000`,
writes `EDL`=1 so the ring index starts climbing, then — a counted number of one-sample wait loops
later — flips `ESA` to `$00` and opens `FLG` bit 5 for exactly seven samples. On hardware the
in-flight sample's write lands at the old `$F0xx` base (the ROM seeds guard bytes around it), and
the `$00`-based burst begins at entry `$EC` — one entry past the driver's own 16-bit wait counter at
`$EA`/`$EB`. Applying `ESA` on the sample of the write starts the burst one entry early, at `$E8`:
the echo overwrites the wait counter, the disable write never runs, and the ring sweeps its whole
2 KB over the zero page, the stack page, and the driver — the CPU ends in a `STOP` with no verdict.
The mirrored second half of the sub-test flips `ESA` the other way (`$00` → `$F0`) and reads both
regions back, pinning the lag in both directions.

### The `$F8`/`$F9` port bytes are out of the echo's reach

The same sub-test sweeps its echo burst across the RAM beneath the `$F0`–`$FF` register overlay and
then reads the sixteen addresses back through the CPU. The echo bytes land in that RAM — the DSP
addresses memory directly, below the overlay — but the CPU's reads return register values, and at
`$F8`/`$F9` the value is the port's own byte: Anomie marks the pair "normal RAM, except … not
altered by S-DSP echo buffer writes". A CPU write still mirrors into the RAM beneath, so the two
stores agree until an echo write splits them.

### `VxENVX` carries an extra sample of read-back pipeline

**Neither document mentions this.** `VxENVX` publishes at the voice's own slot, but the value it
publishes is the envelope computed **one sample earlier** — so a CPU read lags the envelope by a full
sample beyond the publish slot.

*Derived from `KON/envx during kon`*, which pins the first read-visible envelope step one sample later
than a same-sample publish allows, while a separate sub-test pins the first live *compute* to an
earlier instant. The two only reconcile through a value lag in the register.

The publish slots themselves are contested: fullsnes's array column reads `VxENVX` at each voice's
sixth step slot and `VxOUTX` at the seventh; Anomie prepares `VxOUTX` at the sixth and `VxENVX` at the
seventh, each readable two slots later. **Anomie's `VxENVX` slot is confirmed** — moving it three
slots earlier regresses an otherwise-passing sub-test. The `VxOUTX` half of the disagreement is
untested and remains open.

### The pitch is captured once per sample — and a key-on holds the capture for seven

**Neither document mentions any of this.** Both read `VxPITCHL`/`VxPITCHH` at the voice's own slots,
which would make a pitch write reach each voice's stream within a sample of landing, at a per-voice
instant. It does not, in two ways.

**The read is a shared capture with a one-sample pipeline.** The pitch registers are captured for
all eight voices at the first slot of every sample, and the capture reaches a voice's advance one
sample deep: voice 0's compute at the last slot is the only one late enough to consume its own
sample's capture, while voices 1-7 advance by the previous sample's. A pitch value standing for
exactly one sample is therefore consumed for exactly one advance by every voice, whatever slot its
writes land on.

**A key-on suspends that capture for seven samples.** Every key-on the poll consumes — including
one absorbed or rewound inside the silent span — schedules its voice's capture for the first slot
of the poll-parity sample after the consuming poll, then holds the per-sample capture off for seven
samples counted from the poll. Through the hold, the startup's stream walk uses the pitch the
scheduled capture took. A bare pitch write inside the hold never reaches the stream; one landing
between the poll and the parity sample's first slot does, because it is what the scheduled capture
reads. The hold is anchored to the poll, so it covers the same samples for every voice.

*Derived from `KON/kon then change pitch`.* The sub-test keys a voice with pitch zero, pulses
`VxPITCHL` to `$10` for exactly one sample at an offset that grows one sample per reading, and reads
the cursor's total advance back through the echo buffer. The expected readings are `$0008` for the
first four offsets and `$0018` — exactly one sample at the pulsed pitch — for the last four, on all
eight rows alike. Nothing lands while the hold stands, one sample lands after it, and both halves
hold at *every* alignment: an every-other-sample capture leaves half the visible alignments blind
(the alternating table this project's earlier model printed), and moving the hold's edge by one
sample in either direction moves the boundary off the ROM's.

*`KON/kon decoding when another kon` re-read.* That ROM's frozen-pitch readings quantize in pairs,
which first read as a capture grid two samples wide. Each of its freezes rides nine cycles behind a
`KON` write, so what its table actually quantizes is the **keying poll's** every-other-sample grid,
reached through the key-on's scheduled capture — the pairs and the row-uniformity both follow, and
the two ROMs stop contradicting each other. The nine-cycle gap also pins the scheduled capture's
instant from the early side: a write that close behind the consumed `KON` is reliably seen, while
`KON/kon then change pitch`'s earliest pulse — completing about three cycles after the parity
sample's first slot — is reliably not.

### The BRR header is read every sample

The header byte is loaded and its end/loop bits are checked **every sample**, not only when the pitch
counter advances far enough to decode new data. Anomie loads it "every time"; fullsnes carries the row
for every voice in every sample and notes that the DSP's RAM strobes go low "even when no new BRR/DIR
data is needed".

The consequence is easy to miss: a voice whose pitch is zero decodes nothing, yet a block that turns
End+Mute underneath it still releases the voice.

The check goes live from **each voice's third compute after a key-on, counting the load's own slot**
— a uniform per-voice count. The load sits at T31, which is voice 0's own compute slot with the load
running first, so the load's sample is voice 0's first count while voices 1-7 begin theirs in the
next sample; that shared-slot asymmetry is what lets one uniform count produce the ROM's eight
identical per-voice rows. Both documents are silent on when the check goes live at all.

### The decoder runs eight samples ahead of the cursor, and the check trails it by one

The header the per-sample check reads is the **decoder's** block — and the decoder is not where the
sound is. A key-on primes the voice's whole first block before the voice sounds, and from then on
the decoder enters each following block while the interpolation cursor is still eight samples back
in the block before: it resolves there where the chain goes (the next block, or the loop address
for an end block — read from the directory at the voice's directory slot earlier in that sample,
see [below](#the-loop-address-is-read-at-the-voices-directory-slot-ahead-of-the-compute) — and,
for an end block, when `ENDX` is
set; see [below](#endx-is-set-when-the-decoder-leaves-the-end-block-and-reads-back-three-slots-after-the-compute)).
Neither reference states the lead — Anomie describes a twelve-sample ring
turned "when the interpolation index passes 0x4000" without fixing the decoder's distance ahead,
and fullsnes does not mention the buffer at all. The ROM measures it.

`Misc/brr early end at many pitches` is the measurement: two silent blocks chained to an End+Mute
block, keyed from silence once per pitch `$0000, $0200 … $3E00` under direct gain, counting driver
polls of `VxENVX` until it reads zero. The expected per-pitch table — recovered closed-form from
the driver's CRC-32 compare constant (`$08396832`) as the unique fit over the walk-anchor ×
lead-distance plane, then ratified by the ROM advancing — has every count land exactly where the
release publishes two samples after the cursor's twentieth walked sample: the End+Mute block sits
thirty-two samples in, and the decoder is there twelve samples early — its full-block priming plus
the eight-sample lead. The voice dies with the last eight samples of the middle block unplayed, and
the End+Mute block's own samples never sound — "the samples in the final block will never be
output" (Anomie), at every pitch, by a fixed sample distance.

The check itself trails the decoder by one sample: Anomie's V3c carries the 'e'/'l' check early in
the voice's sample while V4 decodes after it, so a sample whose advance carries the decoder into an
End+Mute block still sounds, and the release lands at the next sample's check, one sample after
the decoder moved in.

Around a key-on the check's view stands still longer — through the five empty samples and the first
two live samples. `KON/kon as prev sample ends` bounds this from below: it parks a voice at pitch
zero over a sixteen-sample silent block chained to an End+Mute block, starts the stream at `$1800`,
then re-keys at twenty swept phases and reads `VxENVX` nine samples after each key-on. The re-keyed
startup walks four samples through its countdown and crosses the decoder into the End+Mute block on
its first live advance — yet every one of the twenty reads returns the full direct-gain level, so
that advance cannot reach the check until the second live sample has passed. `KON/kon when prev
sample at end` pins the same window from the envelope's side: its racing startup publishes its
first attack step and the ROM's sync spins until `VxENVX` reads non-zero — a check that saw the
decoder's crossing at the first live step would kill the level at zero and spin forever. The
stationary case is pinned by `KON/kon then set sample's end flag`: a header pulse already standing
at the first live step's sample reads back as a voice that never sounded, so the kill itself stays
ahead of the envelope update within the sample — what lags a crossing is the header address, not
the check's place in the sample.

### BRR data is read in groups of four, ahead of the sound — twelve samples at the key-on

The decoder's lead is not only an address: the sample **data** is read from RAM ahead of the
sound too. Anomie describes the structure — a twelve-sample ring of three four-sample groups,
the key-on decoding three groups before the voice sounds, a new group turned in "when the
interpolation index passes 0x4000" — and his V4 note says this sub-test's name out loud:
decoding a group "is definately not done when not necessary" [sic]. What neither reference
states is the observable consequence: how much already-read data a RAM rewrite cannot reach.

`Misc/brr not always decoding` measures exactly that, using the echo buffer as an oscilloscope.
It keys two voices at pitch zero — parked cursors — under direct gain, voice 0 on a block whose
own data goes quiet four samples early, voice 1 on a block loud to its sixteenth sample, both
chained to a silent loop. While both voices are parked, the driver rewrites voice 1's block to
silence. It then points the echo unit at free RAM (`ESA` `$F0`, `EDL` `$01`, both voices in
`EON`, writes enabled), sets both pitches to `$1000` so the voices move, and freezes the buffer
twenty-odd samples later — leaving a recording of twenty stereo samples of the voices' own
outputs, voice 0 on the left channel, voice 1 on the right, which the driver checksums.

The expected table — recovered closed-form from the driver's compare constant (`$A0CBCF78`) as
the unique fit over the two channels' roll-off positions, then ratified by the ROM advancing —
holds both channels at the parked level for exactly twelve samples, roll-offs aligned. Voice 0's
twelve are its own data. Voice 1's block held sixteen loud samples and was rewritten to silence
while the voice was parked: the twelve it still plays are the twelve its key-on primed, read
from RAM before the rewrite. A machine that reads data at consumption plays four — the
interpolation window primed at the key-on — and rolls off eight samples early, which is the
failing row this machine produced. One that buffers the whole first block plays sixteen and
rolls off four late. The ROM pins twelve: three groups, read at the key-on, immune to everything
RAM does afterward.

The group reads compose with the block-level facts already pinned: with the prime covering
stream samples 0–11, the group a moving voice needs at each group-aligned consume is the one
eight stream samples on — and that group always lies in the decoder's block, which
`Misc/brr early end at many pitches` pins as entered at the eighth consume of the block before.
The parked voice is the limiting case: it consumes nothing, so it decodes nothing — its primed
samples play untouched whenever it moves again — while its header check still reads RAM every
sample (the section above).

The exact sample at which a moving voice reads each later group is settled by the next section:
this ROM rewrites RAM only while both voices are parked, so any schedule that reads a group after
the rewrite and before the group's consumption fits its table, and `Order/pitch after brr` is the
one that lands a rewrite mid-motion.

*Arbitrated by `Misc/brr not always decoding`.*

### A later group's header is read at its scheduling, its data bytes one sample later

The group a boundary crossing calls for is not served at the crossing. Its decode is scheduled by
the consumption of the current group's THIRD stream sample — two stream samples after the boundary
— and the reads split across two samples from there: the **header** is read at the scheduling
itself, and the **data bytes** at the next sample, the first at the voice's BRR load slot and the
second one slot after its compute. Each read sits where the timing tests measure it, and every
byte is captured when read: a RAM write landing after a byte's slot no longer reaches the group.

`Order/pitch after brr` pins the header's sample with a header that changes once per sample. Its
block is eight `$44` bytes — every nibble `+4` — under header `$C3` (shift 12, filter 0, end+loop
on itself), so a voice's output reads as the shift its groups were decoded with: `$2000` at shift
12, `$1000` at 11, `$0800` at 10, `$0400` at 9. Each of the eight voices in turn is keyed at pitch
zero under direct gain `$20` and `VOLL` `$20` with `EON` set, given a step of `$2000` — two stream
samples per output sample, so the scheduling's sample is the frame after the boundary's — and the
header rewritten `$B3`, `$A3`, `$93` at one-sample spacing. The expected row, recovered from the
driver's compare constant (`$57A05856`) as the unique fit over every non-increasing walk of the
four levels through a four-tap window moving zero or two samples a frame, is
`0400 0400 0374 0100 00E8 0080 0080`: the group crossed into while `$B3` stood decodes at shift
**10** — the header standing one frame after the crossing, the scheduling's — where a decode at
the crossing reads 11 (`… 03A2 0200 01BA …`).

`Timing/Voice/V3 BRR.sample.lsb` and `.msb` pin the data bytes to the cycle. Each keys one voice
at a time at pitch `$1000` over one looping block, pulses the group's first or second data byte to
zero for five cycles at a per-row offset from that row's own key-on, and hashes which rows the
voice's output caught. The recovered read cycles — one structured solution per hash — put the
first data byte at the voice's BRR load slot (`T26` for voice 0, its compute slot for voices 1-7)
of the sample after the scheduling, and the second one slot after that sample's compute (`T0` of
the following sample for voice 0).

Two consequences. The bytes behind a sample are read six to ten stream samples ahead of it; the
twelve-sample prime at the key-on is unchanged. And a modulated step that crosses two boundaries
in one sample (the position clamp at `$7FFF` lets an advance pass up to seven) has both groups'
bytes read in full before the cursor can touch them — under a drain that steep, one slot ahead of
the second byte's usual place.

*Arbitrated by `Order/pitch after brr` with `Timing/Voice/V3 BRR.sample.lsb`/`.msb`, the timing
pair run from two arrival phases.*

### The loop address is read at the voice's directory slot, ahead of the compute

The loop jump the decoder makes as it leaves an end block takes an address read earlier in the
same sample: the directory entry `DIR`/`VxSRCN` select is read at the voice's **directory slot** —
T22 for voice 0, nine slots before its compute, and the slot before the compute for voices 1-7
(T1, T4 … T19). Both references place it there: fullsnes's chart carries a `VxSRCN/DIR.lsb/msb` RAM
access on exactly those rows, and Anomie's V2 step "loads the sample pointer" one step ahead of the
header read, his V4 having "flagged the loop address for loading next step V2".

`Timing/Voice/V2 dir.loop.lsb` and `.msb` measure the slot to the cycle. Each keys the eight
voices in turn at pitch `$3000` over a looping block, with the entry's loop address at the block
itself; per row it pulses one loop byte to a value that points at a silent block for five cycles at
a per-row offset from that row's own key-on, and hashes whether the voice went quiet — thirty-six
rows a voice, one character each, behind the voice's index. The recovered read cycles — one
structured solution per hash, both bytes identical — put the read nine slots before voice 0's
compute and one before each other voice's; a read at the compute itself catches a different set of
rows for every voice.

The consequence is narrow but exact: a directory write that lands after the directory slot and
before the compute reaches the next sample's read, not this jump.

*Arbitrated by `Timing/Voice/V2 dir.loop.lsb`/`.msb`.*

### The start pointer is read at the voice's directory slot, one sample after the consuming compute

A key-on's start address is read from the directory at the same directory slot — T22 for voice 0,
T(3v−2) for voices 1-7 — in the sample **after** the compute that applied the key-on, the compute
that emits the voice's final pre-key-on sample. The start block's header and data bytes follow at
that sample's BRR load slot, ahead of its compute, so the countdown's first silent call reads
nothing at all. That is Anomie's order — the key-on taken at V3c, "load the sample pointer" at the
next V2, the header at that sample's V3b — and fullsnes's `VxSRCN/DIR.lsb/msb` rows on the same
slots.

`Timing/Voice/V2 dir.start.lsb` and `.msb` measure it as the loop pair does: per row a five-cycle
pulse into one start byte, at a per-row offset from that row's own key-on write, points the entry at
a silent block, and the hash records whether the voice went quiet. The recovered read cycles — the
same constant as the loop pair, one structured solution per voice, both bytes identical — sit 56
slots after voice 0's key-on write and 67 + 3(v−1) after the others': one sample after the
consuming compute, at the directory slot. A read at the consuming compute itself catches no row at
all for voices 0-5 and the wrong rows for voices 6 and 7.

The consequence: a `DIR` or `VxSRCN` write that lands after the key-on's consuming compute and
before the next sample's directory slot is what the started voice plays from; one landing after
that slot reaches only the voice's next loop.

*Arbitrated by `Timing/Voice/V2 dir.start.lsb`/`.msb`.*

### Pitch modulation reads the previous voice's output from the previous sample

fullsnes's pitch-counter listing scales the step by `VxOUTX(x-1)` (line 2790) and Anomie's V3c
marks the enveloped sample as "the value used for modulating the next voice's pitch" (lines 83–85);
neither says which sample's. The slot map answers it: voice *x*−1 computes three slots ahead of voice
*x* in every sample (voice 0 at T31 of the sample before), so a modulator reading the fresh value
would see the same sample's output. The hardware does not. Voice *x*'s step is scaled by the
amplitude voice *x*−1 produced one sample earlier — the value that stood before the compute three
slots back replaced it — for every pair, voice 0 → 1 included. A change in the modulating voice's
output therefore reaches the modulated voice's pitch one sample after it reaches the mix.

`Order/pitch mod uses prev sample` measures it with an impulse. Voice 1 plays a self-looping block
with a single `+7` nibble among zeros (header `$C3`, shift 12) under direct gain `$7F` at pitch
`$2000`, so its output is one loud sample every eight; voice 2 plays a block of alternating `+7`/`-8`
nibbles at the same pitch under direct gain `$01` and `VOLL` `$20` with `EON` set, so at an even
position it reads one polarity and at an odd one the other — its output is its phase. With `PMON`
bit 2 set the two are keyed on together and, after 23 samples, the ten left echo words from
`$F030` (frames 12–21) are checksummed. Each impulse kicks voice 2's step by the modulation factor
(`$3800 → $77C`, a step of `$EF80` clamped by the position ceiling), which flips its phase; the
frame on which the flip shows is the frame the impulse reached the modulator. A machine that reads
the same sample's amplitude prints `0000 ×4, FFFE ×6` and fails; one that reads the previous
sample's passes.

*Arbitrated by `Order/pitch mod uses prev sample`.*

### `ENDX` is set when the decoder leaves the end block, and reads back three slots after the compute

Anomie says it both ways. His BRR section: the bit "is set when the block is complete and the next
block will be that pointed to by the loop pointer" — the decoder's exit from the end block. His
`$7C` entry: "the bit is set at the START of decoding the BRR block, not at the end" — its entry,
sixteen stream samples earlier. fullsnes carries the second wording verbatim. The two differ by a
whole block, and the sub-test's name says which is under test: *endx after final brr decode*.

`Order/endx after final brr decode` measures it per voice. Each voice in turn is keyed at pitch
`$1000` — one stream sample per output sample — over a plain block chained to an End+Mute block,
left alone for twenty-four samples in a 32-cycle loop, and `ENDX` is then read three times exactly
one sample apart, the reads landing on the twenty-sixth, twenty-seventh and twenty-eighth samples
after the key-on. The expected row — recovered from the driver's compare constant (`$9FAF237D`) as
the unique fit over every monotone clear-then-set pattern, then ratified by the ROM advancing — is
**clear, set, set for every one of the eight voices**. The decoder enters the end block on the
eleventh sample after the key-on (its eight-sample lead plus the startup) and leaves it on the
twenty-seventh: the bit appears at the exit. A machine that sets it at the entry reads set on all
three samples; one that sets it when the end block's last group is decoded, four samples before
the exit, reads set on the first as well.

Two of the eight rows carry more than the placement:

- **Voice 0** computes at the sample's last slot, and the driver's reads sit at the sample
  boundary. A set made readable at the compute itself is seen by the first read; the ROM says it is
  not. The bit is staged and reaches the register three slots after the compute — Anomie's V7,
  "cycles: 0:2 1:5 2:8 …", one ahead of the slot numbering here — which for voice 0 is the following
  sample's fourth slot. Voices 1-7 land inside their own sample either way; voice 0 is the row that
  fixes the stage.
- **Voice 4** is the sync gadget's voice, still sounding at full level when the test re-keys it —
  and its row is a held startup's. The gadget's `ADSR1` restore to zero (direct gain, level zero)
  lands on the compute just before the consuming poll, so the level is zero by the time the restart
  reads it: the restart reads the envelope *after* the consuming compute's own update (the section
  above). Read before it, the voice walks its startup and its bit comes six samples early.

**Anomie's BRR wording is right; his and fullsnes's `$7C` wording is wrong.** The bit says a
sample has finished, not that its final block has begun.

Unarbitrated residue: the acknowledge write's reach into a set still staged. Anomie has a write
"up to 2 cycles earlier" overwriting the new value; here any acknowledge drops a staged set along
with the readable bits. A sub-test that acknowledges within the three slots between a voice's
compute and its publish would settle it.

*Arbitrated by `Order/endx after final brr decode`.*

---

## Echo

### The FIR output is 15 bits

Anomie masks the FIR output's low bit before echo volume and feedback consume it; fullsnes feeds the
unmasked 16-bit sum to both and masks only the buffer write. A difference of at most one LSB before a
multiply — and the ROM catches it.

With `EON` clear, echo volume 0 and feedback `$80`, the buffer write is feedback-only, so an odd FIR
sum of 1 becomes `(1 × −128) >> 7 = −1`, and the write mask takes that to **−2** where the ROM expects
**0**. Anomie's placement yields 0.

**Anomie is right.** The buffer write keeps its own mask as well, since the feedback multiply can
reintroduce the low bit.

*Arbitrated by `Echo/echo calc`.*

---

## What remains open

Stated plainly, because a reference that hides its soft spots is worth less than one that marks them:

- **The `VxOUTX` publish slot.** The fullsnes / Anomie disagreement described above is settled for
  `VxENVX` and untested for `VxOUTX`.
- **Echo reads, and the `ESA`/`EDL` latch and ring-index advance**, are modeled together at T24.
  Anomie places the latch and advance at his cycle 30. No measurement distinguishes them yet.
- **Register-order edge cases** — writing `VxADSR2` or `VxGAIN` before `VxADSR1` within a sample — are
  noted in the Errata and not modeled at slot granularity.
- **An `ENDX` acknowledge landing between a voice's compute and its publish slot.** Modeled as
  dropping the staged set; Anomie bounds the overwrite window at two cycles. Untested.

## How a contested claim gets settled here

Blargg's DSP test ROMs do not print what they expect. Each sub-test checksums a byte stream it builds
from the hardware's responses and compares against a constant compiled into its own driver, then
prints only a failure code. So the useful question is not "what did the ROM print" but "what stream
would have produced the constant it wanted".

That is recoverable. The checksum is a reflected CRC-32, which is affine over GF(2): a stream's
accumulator is the all-zero stream's accumulator XORed with a per-position, per-byte contribution.
Precomputing those contributions turns each candidate stream into a handful of XOR operations, which
makes an exhaustive sweep of a constrained family cheap enough to run in seconds.

Two disciplines make the result trustworthy rather than merely plausible:

- **Self-prove the reconstruction first.** Rebuild the stream from what your *own* implementation
  produced and confirm it reproduces the accumulator the ROM printed for your run. If that fails, the
  stream layout is wrong and every hit that follows is noise.
- **Predict from the documentation, then let the search confirm.** A single hit that matches a value
  already derived by hand is strong evidence. A bare hit found by changing three or more unknowns is
  not — it is a plausible stream, and plausible streams are not scarce.

The searches behind this page returned exactly one candidate each, against families of up to
5.7 million, with the reconstruction self-proving beforehand.
