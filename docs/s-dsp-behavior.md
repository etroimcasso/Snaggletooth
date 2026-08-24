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

### The keying poll precedes voice 0's compute in the slot they share

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

### Key-on takes effect on the write; key-off exerts itself continuously

A `KON` write arms the next poll, which starts the armed voices and then disarms itself — so a bit
left set does not start the voice again, and the register keeps its value for read-back. `KOFF` is
read from its register at every poll instead, so it keeps releasing for as long as the bit stands.
`FLG` bit 7 is polled every sample rather than every other one.

### A key-on landing on a voice mid-startup is absorbed

Both documents state that keying a voice that is already playing restarts it in full — envelope to
zero, stream to the start, the empty startup samples again, the audible click. Both are silent on
the narrower case: a key-on consumed by the poll while the voice is still inside that empty-sample
startup. The test ROM decides it (`KON/kon clears independent`): two `KON` writes ~53 SPC cycles
apart, deliberately inside one 64-cycle poll period and synchronized so a poll falls between them,
the first naming voice 0 and the second naming voices 0 and 1. The expected capture shows voice 0
sounding **alone for exactly two samples** — one poll period — before voice 1 joins, and voice 0
never restarting. So the second poll's key-on of voice 0 was absorbed: a key-on that lands during
the startup countdown neither resets the countdown nor restarts the stream.

The alternative reading — that the second write could not re-arm voice 0 because its register bit
never returned to 0 — fits this capture equally well, but is refuted by `Envelope/hidden env 0 at
kon`, whose driver re-keys a long-playing voice by rewriting a `KON` value whose bit stood at 1
throughout, and expects the restart. The register's history does not gate the arm; the voice's own
startup state gates the action.

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

### The BRR header is read every sample

The header byte is loaded and its end/loop bits are checked **every sample**, not only when the pitch
counter advances far enough to decode new data. Anomie loads it "every time"; fullsnes carries the row
for every voice in every sample and notes that the DSP's RAM strobes go low "even when no new BRR/DIR
data is needed".

The consequence is easy to miss: a voice whose pitch is zero decodes nothing, yet a block that turns
End+Mute underneath it still releases the voice.

The check goes live **two sample periods after the key-on load, measured from the load slot** — not
after a fixed number of the voice's own computes. Because the load sits at T31 and voice 0 computes at
that same slot while the others compute early in the next sample, one absolute cutoff falls on voice
0's *second* compute and on the others' *third*. That is the only way the ROM's eight identical
per-voice rows can hold. Both documents are silent on the distinction.

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
