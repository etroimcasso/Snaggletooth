# PPU behavior and provenance

A reference for the parts of the SNES PPU where the published hardware documentation is incomplete,
ambiguous, or wrong — and for what settles each case.

[`ppu.md`](ppu.md) is the usage guide: what Snaggletooth's PPU surface is and how to drive it. This
page is the evidence behind it. Every claim below names where it comes from, and where two sources
disagree it names the disagreement and what decided it.

## Contents

- [The evidentiary standard](#the-evidentiary-standard)
- [Sources](#sources)
- [The tilemap](#the-tilemap)
  - [The screen base counts whole screens, not half-screens](#the-screen-base-counts-whole-screens-not-half-screens)
  - [A wide or tall map reaches its further screens by whole screens too](#a-wide-or-tall-map-reaches-its-further-screens-by-whole-screens-too)
- [The characters](#the-characters)
  - [The character base counts 8 KB blocks](#the-character-base-counts-8-kb-blocks)
  - [A 16×16 block's numbers run on rather than wrapping inside it](#a-1616-blocks-numbers-run-on-rather-than-wrapping-inside-it)
- [The priority order](#the-priority-order)
  - [Only BG3's high-priority tiles move when $2105 bit 3 is set](#only-bg3s-high-priority-tiles-move-when-2105-bit-3-is-set)
  - [Mode 1 gives no background a palette offset of its own](#mode-1-gives-no-background-a-palette-offset-of-its-own)
- [The picture's edges](#the-pictures-edges)
  - [The console outputs no scanline 0](#the-console-outputs-no-scanline-0)
  - [Where the visible span ends is not settled by the documents](#where-the-visible-span-ends-is-not-settled-by-the-documents)
- [The converter](#the-converter)
  - [Brightness scales by (N+1)/16, and 0 is off rather than dim](#brightness-scales-by-n116-and-0-is-off-rather-than-dim)
- [The status registers](#the-status-registers)
  - [The latch flag clears on a read only while the latch line is high](#the-latch-flag-clears-on-a-read-only-while-the-latch-line-is-high)
- [What is not settled yet](#what-is-not-settled-yet)

## The evidentiary standard

Three kinds of claim appear here, and they are not equally strong:

- **Documented and corroborated.** Two independent published sources agree. Taken as given.
- **Documented but contested.** The published sources disagree, or one is self-contradictory. Decided
  by software running on the machine, and the decision is recorded with the observation that forced
  it.
- **Undocumented.** No published source states it. Derived from observed behavior, which means the
  claim is only as good as the reconstruction behind it — so the reconstruction is described, not
  just asserted.

Where running software decides a question, it outranks both documents. A document describes what
someone understood; a cartridge that draws correctly is the hardware's own software agreeing.

**No implementation's source is read.** Snaggletooth is a clean-room implementation: the contract is
public hardware documentation plus observable behavior. That is the line, and it is about *source* —
copying another program's expression of a solution.

**Observing what another program puts on screen is not that.** A reference emulator's debugger
showing which address a layer's map sits at is an observation of behavior, the same kind of evidence
as a cartridge drawing correctly or a test ROM's expected output. Black-box observation is the
ordinary method of clean-room reverse engineering, not an exception to it. It is used below, and
named where it was, for the same reason every other piece of evidence is named: so a reader can see
what a claim rests on.

## Sources

| Source | Role |
|---|---|
| Martin Korth's fullsnes, "SNES PPU" | Primary. Register tables, the picture's event list, the tile and map layouts, the brightness law. |
| SNESdev Wiki, "PPU registers", "Backgrounds", "Sprites" | Primary. The clean register account and the background structure. |
| Anomie's PPU register document | **Cross-check only, and known wrong in one place.** Its background section prints the tilemap word with a half-screen base step; see below. Useful for the behaviors it measured, never to be taken alone. |
| Anomie's SNES Timing Doc | Cross-check for the beam and the per-line event offsets. |
| The staged PPU test ROMs | **Arbiter**, where one exercises the question. |
| Commercial cartridges run on the machine | Arbiter of last resort: software written for the hardware, drawing what its authors saw. |

---

## The tilemap

### The screen base counts whole screens, not half-screens

`$2107`–`$210A` bits 2–7 give each background's map base. **A step of that field is one whole 32×32
screen — `$400` words, 2 KB.** So the map's word address is

```
(Base << 10) + ((Y & 0x1F) << 5) + (X & 0x1F) + the size terms
```

*Documented but contested.* fullsnes gives the base in **1 K-word steps**, which is this. Anomie's
background section prints the same formula with `(Addr<<9)` — a half-screen step, which would let a
base land halfway through a screen and would make consecutive base values overlap. **fullsnes is
right and that line is wrong.**

The error is worth recording because it is silent: built the wrong way, every layer's map is read at
half its address, and the picture is then made of *valid tiles in scrambled positions* rather than
of noise — the kind of wrong that looks like a different bug. Contract tests derived from the same
misreading pass, since they encode the same arithmetic the code does.

*Arbitrated by observation.* A cartridge's map read at the wrong base produces a scrambled screen;
read at `Base << 10` the same cartridge draws its own artwork correctly. A reference emulator's
tilemap viewer reported the three layers of one cartridge at `$2000`, `$3000` and `$5000` where the
half-step gave `$1000`, `$1800` and `$2800` — the factor of two, visible in one line, on all three
layers at once.

### A wide or tall map reaches its further screens by whole screens too

Bits 0–1 select 32×32, 64×32, 32×64 or 64×64, laid out as one, two or four 32×32 screens in `$800`
-byte blocks, A / AB / AB-over-CD. The position's own terms carry it across them:

```
+ (SY ? ((Y & 0x20) << (SX ? 6 : 5)) : 0) + (SX ? ((X & 0x20) << 5) : 0)
```

Moving 32 tiles right skips one screen; moving 32 tiles down skips one screen on a tall map and two
on a map that is also wide. *Documented and corroborated* — fullsnes and Anomie agree on the terms,
and they are the same in both.

## The characters

### The character base counts 8 KB blocks

`$210B`/`$210C`, four bits per background, give the character base in **8 KB steps**: the character's
byte address is `(Base << 13) + Tile × 8 × planes`. *Documented and corroborated*, and independently
confirmed against a reference emulator's reported tileset address for three layers of one cartridge
at once.

Note the asymmetry with the map base above: the two fields count in different units, which is exactly
why one of them was got wrong and the other was not.

### A 16×16 block's numbers run on rather than wrapping inside it

With `$2105`'s size bit set, an entry names `Tile`, `Tile+1`, `Tile+16`, `Tile+17`. `$2FF` gives
`$2FF, $300, $30F, $310` — **not** `$2FF, $2F0, $20F, $200`. Only the ten-bit number itself wraps, at
`$3FF`. A flip reverses the whole 16×16 block rather than its four parts individually.

*Documented and corroborated*, and stated explicitly by both sources because the intuitive reading is
the wrong one.

## The priority order

### Only BG3's high-priority tiles move when $2105 bit 3 is set

Anomie's Mode 1 list and fullsnes's chart give the same order, but fullsnes's is easy to misread.
It prints `BG3.1a` at the very top and `BG3.1b` further down — `a` meaning the bit set and `b`
meaning it clear — and then prints `BG3.0a` and `BG3.0b` on two **adjacent** rows near the bottom,
which looks like BG3's low-priority tiles moving as well. Nothing sits between those two rows, so
they name one and the same place. Anomie's list says it in words: BG3's priority-0 tiles are last
either way, and only its priority-1 tiles change position.

*Documented and corroborated*, once fullsnes's adjacent pair is read as a single place. Both charts
then give, front to back and writing letters for backgrounds:

```
A B a b C c        and with the bit set:   C A B a b c
```

### Mode 1 gives no background a palette offset of its own

Mode 0's four backgrounds start their palettes 32 words apart — Anomie gives `ppp*4 + (BG#-1)*32`
for it — and the natural assumption is that the other modes do something similar. They do not.
Anomie's Mode 1 formula is `ppp*ncolors` with no per-background term, so BG3's four-colour palette 1
and BG1's sixteen-colour palette 0 name overlapping words. fullsnes's CGRAM index table agrees from
the other direction: its four-colour BG palettes live at `01h-1Fh` for every background *except*
BG2–BG4 in Mode 0, which are the ones given ranges of their own.

*Documented and corroborated*, by two sources that state it in opposite forms.

## The picture's edges

### The console outputs no scanline 0

The chip renders line 0 but does not output it, so the first line a viewer sees is scanline 1. This
is why so many games write `−1` into their vertical scroll registers: with an offset of `−1`, the
tilemap's first row lands on the picture's first output line. An interlaced screen wants `−2` for the
same reason.

*Documented*, and visible immediately in any game that does it: read without the offset, a picture
begins on the second row of its top tile.

### Where the visible span ends is not settled by the documents

fullsnes's event list gives the visible span as dots **22–277** and marks it with **its own question
mark**. The span is implemented as written and the uncertainty is left standing rather than rounded
to something tidier. Nothing yet measured depends on the last dot.

*Documented but uncertain at the source.*

## The converter

### Brightness scales by (N+1)/16, and 0 is off rather than dim

`$2100` bits 0–3 hold the brightness. A five-bit channel reaches the output as

```
round(c × (N + 1) × 255 / (31 × 16))
```

computed in integers, one rounding from the exact rational. **N = 0 is the screen off — black — not
one-sixteenth brightness**, which the register page states as `0="off"` rather than as a scale point.
Forced blank is black by the same rule.

*Documented and corroborated.* The rounding direction is observable: at N = 7 a full channel lands on
exactly 127.5, and rounding up gives 128.

## The status registers

### The latch flag clears on a read only while the latch line is high

`$213F` bit 6 reports that the H/V counters have been latched. Reading the register clears the flag —
**but only while `$4201` bit 7 is set.** With the latch line held low the flag survives the read, and
a second read still finds it raised.

The counters' own high/low selectors are reset by the same read **unconditionally**: that is a side
effect of reading the register, and the latch line does not gate it. The two halves of the read
behave differently, and reading them as one rule is the easy mistake.

*Documented* by the register page, which states the gate in one clause and the selector reset in a
separate note.

## What is not settled yet

Recorded rather than decided by invention. These are the PPU questions currently open; `ppu.md`'s
own "What remains open" carries the register-file ones alongside these.

- How far ahead of a dot the chip fetches that dot's map entry and character. Pixels are resolved
  from the registers as they stand at their own dot, which is where a mid-picture write lands; the
  distance itself wants a test ROM that exercises it.
- What a write to a scroll register mid-line does to a tile whose entry has already been fetched.
- How the palette's mid-line access window sits against the chip's own fetch of the colours it is
  drawing with.
- The last dot of the visible span, above.
