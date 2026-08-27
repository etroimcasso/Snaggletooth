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

### The stream advances at the pitch through the silent samples

Both documents hold the decode position still until the startup ends: Anomie's account above has "no
interpolation update" through `#4` with fixed BRR group preloads in their place, and fullsnes quotes
the same shape. The test ROM refutes the stillness (`KON/kon decoding when another kon`): it keys a
voice, freezes its pitch mid-startup by zeroing `VxPITCH`, and reads the voice's output back through
the echo buffer — a pure function of where the decode cursor stood when the pitch froze. The expected
table walks: across re-keys frozen at one-sample-later instants, the read moves through the source's
loud region exactly as a cursor advancing **two stream samples per output sample** (the sub-test's
pitch) would place it. A cursor holding at its preload prints all zeros; fixed four-sample group
preloads land the loud readings in the wrong places.

The first silent sample is the exception — it performs the start-address read and decodes nothing,
so the walk begins on the second. Only the *output* is silent during the startup; the position is
live throughout. The pitch the walk uses is the one the key-on itself captured — that sub-test's
freezes land because each rides a `KON` write of its own; a pitch write with no key-on behind it
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

The walk above is a whole-sample walk. Sample positions cross and decode exactly as the pitch
dictates, but the fractional remainder does not accumulate: when the first sounding sample
interpolates, its Gaussian index is 0 — the exact start of the position the walk reached — and the
fraction begins climbing only with the advance after it. Neither document says anything about the
fraction during the startup; the accounts that hold the position still imply nothing to discard, and
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

---

## The intra-sample schedule

Each 32 kHz sample is built over 32 clock slots, and register reads land on documented slots — so a
CPU write lands where the hardware takes it rather than at a sample boundary. The map below merges
fullsnes's per-cycle access chart with Anomie's voice loop; Anomie's slot *k* is fullsnes's T*(k+1)*.

| Slot | Work |
|---|---|
| T2, T5, T8, T11, T14, T17, T20 | Voices 1-7 each run a whole compute (stream, noise, envelope, amplitude). |
| T23 / T24 | Echo reads; the echo value is computed here. |
| T27 / T28 | Left then right output finalize — master and echo volume, then the mute gate. |
| T30 / T31 | The echo buffer's left word, then its right word. |
| T31 | Voice 0's whole compute; the global counter; the noise step; the `KON`/`KOFF` load. |

Voice 0 computes at the last slot and applies at the following sample's first slots, so its output
rides one sample behind the others while its state trajectory stays in step.

### The echo buffer writes land at T30/T31

Both documents say so — fullsnes as explicit chart rows, Anomie at his cycles 29 and 30 — but it is
easy to lump the write in with the echo *read* at T23/T24, six or seven slots early. It is
CPU-visible: a driver reading the buffer at a fixed offset from its own key-on races the write, and
the six-slot error changes which side of the race it lands on.

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

### The header the check reads is the decoder's block, one sample behind a crossing

The check runs early in the voice's sample and the decode after it — Anomie's V3c carries the 'e'/'l'
check while V4 decodes and adjusts the BRR pointer — so the header it reads belongs to the block the
decoder last ran in, never to a block the same sample's advance enters. A stream crossing into an
End+Mute block sets `ENDX` at once but is released only at the next sample's check.

The gap widens across a key-on's startup, because the decoder idles through it: fullsnes has the five
empty samples land "before envelope updates and BRR decoding actually begin", even though the cursor
walks at the pitch the whole time (the section above). A cursor that crosses into an End+Mute block
during those empty samples is therefore released one sample **after** the first envelope step — the
step runs and its level reaches `VxENVX` for one sample before the release lands.

`KON/kon when prev sample at end` is what settles this, by refusing to run otherwise: its sample
chains one silent block into an End+Mute block that loops on itself, its body leaves pitch `$3F00`
standing where the shared sync keys a rate-15 attack, and the sync then spins until `VxENVX` reads
non-zero. The racing startup crosses into the End+Mute block as the countdown ends, so a check that
saw the crossing at the first live step would kill the level at zero and spin forever — on hardware
the sync exits, on the strength of that one published step. The stationary case is pinned from the
other side by `KON/kon then set sample's end flag`: a header pulse already standing at the first live
step's sample reads back as a voice that never sounded, so the kill itself stays ahead of the
envelope update within the sample — what lags a crossing is the header address, not the check's
place in the sample.

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
- **The 128 kHz pitch crop.** fullsnes marks its own placement uncertain; Anomie's clamp on the
  post-add interpolation index is concrete and is what is implemented.
- **Register-order edge cases** — writing `VxADSR2` or `VxGAIN` before `VxADSR1` within a sample — are
  noted in the Errata and not modeled at slot granularity.

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
