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
- [The sprites](#the-sprites)
  - [A sprite's numbers wrap inside its table, which is the opposite of a block's](#a-sprites-numbers-wrap-inside-its-table-which-is-the-opposite-of-a-blocks)
  - [Sizes 6 and 7 are undocumented and both sources print them anyway](#sizes-6-and-7-are-undocumented-and-both-sources-print-them-anyway)
  - [The two passes' schedule: the documents give it from opposite ends and reconcile exactly](#the-two-passes-schedule-the-documents-give-it-from-opposite-ends-and-reconcile-exactly)
  - [The two counts run in opposite directions along the same sprites](#the-two-counts-run-in-opposite-directions-along-the-same-sprites)
  - [The front-sprite oddity is stated three ways and two of them agree](#the-front-sprite-oddity-is-stated-three-ways-and-two-of-them-agree)
- [The priority order](#the-priority-order)
  - [Only BG3's high-priority tiles move when $2105 bit 3 is set](#only-bg3s-high-priority-tiles-move-when-2105-bit-3-is-set)
  - [Mode 1 gives no background a palette offset of its own](#mode-1-gives-no-background-a-palette-offset-of-its-own)
- [The windows](#the-windows)
  - [fullsnes prints the window-area field values off by one](#fullsnes-prints-the-window-area-field-values-off-by-one)
- [Colour math](#colour-math)
  - [Colour math does not consult the sub screen pixel's priority](#colour-math-does-not-consult-the-sub-screen-pixels-priority)
  - [The halving happens before the range is held, and the sub-screen backdrop is exempt](#the-halving-happens-before-the-range-is-held-and-the-sub-screen-backdrop-is-exempt)
  - [A force-blacked main pixel is not also halved](#a-force-blacked-main-pixel-is-not-also-halved)
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
| Cartridges written here for one question | **Arbiter**, where no document answers and no existing ROM asks. A picture whose colours state the answer, paired with a control image that must come out the other way — because a result every cell agrees on is also what an instrument that cannot register a negative would produce. |
| Mesen | A reference implementation, and the usual first reading of a cartridge written here. Not decisive on its own: it is the outlier on the colour-math halving below. |
| bsnes | A second reference implementation, of a separate lineage. Where it and Mesen agree the reading is strong; where they differ the question is open until silicon answers. |
| snes9x | A third, independent of both. Not accuracy-first, so it does not carry a question alone — but it breaks a tie between the other two, which is what it did for the colour-math halving below. |
| The Analogue Super NT | Corroboration only, and weak. It is an FPGA reconstruction that **fails every Blargg test ROM**, so it does not carry a fine-grained behavioral question. Useful where a result is categorical — a picture that is entirely one colour or entirely another — and not otherwise. |

Neither of those is original silicon. An observation from the console itself outranks both, and is
named as the tiebreaker wherever one of them decided a question below. **Every cartridge written
here is owed a run on an original console, and none has had one yet:** the console that will run
them is waiting on an original power supply, since it is not worth risking on any other. Until it
runs them, a cartridge written here is run on Mesen, bsnes and snes9x, and the reading two of the
three give is the one taken; a finding decided that way is provisional — held as stated, and
reopened rather than defended if the console disagrees.

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

## The sprites

### A sprite's numbers wrap inside its table, which is the opposite of a block's

A sprite's first character number is a row and a column in a 16×16 table, and **each nibble wraps on
its own**: a 16×16 sprite whose first character is `$FF` is made of `$FF`, `$F0`, `$0F` and `$00`.
Set beside the rule above — where a background's 16×16 block runs on from `$2FF` to `$300` — the two
are exactly opposite, for the same layout in the same memory.

*Documented and corroborated*, and stated at length by Anomie precisely because it surprises: "tile 0
is to the right of tile `$0F` and below tile `$F0`… tile `$FF` is to the left of tile `$F0` and above
tile `$0F`."

### Sizes 6 and 7 are undocumented and both sources print them anyway

`$2101` bits 7–5 select a pair of sizes. Both sources list all eight pairs and both mark the last
two — 16×32/32×64 and 16×32/32×32 — as undocumented. They agree on the dimensions, and 44 % of a
census of 740 drawing titles use a pair beyond the first, so the two are built rather than left to be
discovered.

*Documented but contested* only in the sense that the hardware's own documentation never named them;
the two secondary sources agree exactly.

### The two passes' schedule: the documents give it from opposite ends and reconcile exactly

Anomie's timing document describes the mechanism. The PPU spends **256 memory-access cycles** on the
visible span, and "during this time, OAM is being examined to determine the first 32 sprites on the
next scanline"; then "during H-Blank, 68 memory access cycles are devoted to loading the next
scanline from 34 4-bit sprite tiles." **128 sprites over 256 cycles is two cycles a sprite.**

fullsnes states the same arithmetic from the other end, as the dots at which the overflow flags are
raised: range overflow at `H = OAM.INDEX × 2`, time overflow at `H = 0` of the following line.

The two documents' `V` agrees once the line the console renders and does not output is counted. A
sprite whose Y is N first draws on line N + 1, so the pass that first finds it runs during line N —
fullsnes's `V = OBJ.YLOC` exactly. And the horizontal blank at the end of line N contains
`V = N + 1, H = 0`, because the blank is raised at `H = 274` and lowered at `H = 1` of the next line.
fullsnes's time-overflow dot is that point exactly.

*Documented and corroborated.* **Neither source needs bending**, and the two together fix not just
that the passes happen a line early but where inside the line each sprite is examined — which is what
decides where a mid-line write to `$2101` lands. A census measured **23,471 writes to `$2101` inside
the visible picture across 70 titles, 9 of them by HDMA**, so the question is not hypothetical: a
renderer that evaluated a whole line at one instant would take every one of those writes either
wholly early or wholly late.

### The two counts run in opposite directions along the same sprites

Anomie's four numbered steps put the directions plainly. Range starts "with the FirstSprite" and
determines "the first 32 sprites on this scanline", counting only those with `−size < X < 256`, and
sets `$213E` bit 6 where there are more. Time then starts "with the last sprite in Range" and loads
"up to 34 8×8 tiles (from left-to-right, after flipping)", counting only those with `−8 < X < 256`,
and sets bit 7 where there are more. fullsnes gives the same two numbers as the flags' names — more
than 32 sprites, more than 8×34 pixels — and adds that both are set "regardless of OBJ
enable/disable in 212Ch".

*Documented and corroborated.* The consequence is the one that matters and it is not symmetric: Time
spends its count from the *back* of Range, so the tiles it runs out of belong to the sprites nearest
the front. A crowded line therefore loses the sprites the walk began at, which is exactly what the
`$2103` bit 7 walk position exists to move around.

Anomie's step 0 is the other half of the counting rule: a sprite at `X = 256` "or `X = −256`, same
difference" is considered at `X = 0` for both passes, and his step 3 adds that this "doesn't mean you
actually draw it at X=0". Nine bits of signed X reach `−256` and not `256`, so there is one such
position rather than two.

### The front-sprite oddity is stated three ways and two of them agree

With `$2103` bit 7 set, anomie gives the front sprite as `(OAMAddr & 0xFE) >> 1` from the internal OAM
*word* address, and his worked example fixes the arithmetic: `$2102/3` set to `$104` gives sprite 2,
and four bytes written past it gives sprite 3.

Then the oddity. He states it three ways in one paragraph, and they do not all agree:

1. **The algebra.** Set `$2102/3 = A`, write `4n + 2(A & 1) + 1` bytes, and the front sprite becomes
   `((OAMAddr >> 1) + Y) & 0x7F` for the line `Y`. Working the byte address through — it starts at
   `2A` and a write steps it one — that count leaves the port on **byte 1 of a record**, for every `A`.
2. **The gloss beside it**, which says the count is chosen "so the next byte written would go to the
   last byte in the 4-byte sprite record" — **byte 3**.
3. **The worked example**: 128 sprites at `Y = 63`, `$8000` written to `$2102/3`, then three bytes read
   from `$2138`, giving sprites 63 through 70 the front on successive lines. Three reads from a reload
   value of zero leave the port on **byte 3**, and `(3 >> 2) + 63` is 63 — the example's own first
   number.

*Documented but contested, decided by the majority of one document against itself.* Two of the three
statements say byte 3 and one says byte 1, and the two that agree are the ones carrying an
independently checkable number: the example's sprites 63–70 come out right on byte 3 and wrong on byte
1. So the line is added where **the port's address has both low bits set**, and the front sprite is
`((address >> 2) + line − 1) & 0x7F` — the `− 1` being the same dummy-line offset the passes' schedule
above turns on, since the pass matching a sprite at `Y = N` runs during line `N`. A test ROM that puts
128 sprites at one Y and steps the port through all four byte positions would settle it outright; none
has been run.

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
3 A B 2 a b 1 C 0 c        and with the bit set:   C 3 A B 2 a b 1 0 c
```

with a digit for a sprite at that sprite priority. Reading fullsnes's chart this way also settles
where the bit-set order leaves the `BG3.1b` row: it is the one place BG3's high-priority tiles
vacate, and a sprite at priority 1 and one at priority 0 then sit next to each other.

### Mode 1 gives no background a palette offset of its own

Mode 0's four backgrounds start their palettes 32 words apart — Anomie gives `ppp*4 + (BG#-1)*32`
for it — and the natural assumption is that the other modes do something similar. They do not.
Anomie's Mode 1 formula is `ppp*ncolors` with no per-background term, so BG3's four-colour palette 1
and BG1's sixteen-colour palette 0 name overlapping words. fullsnes's CGRAM index table agrees from
the other direction: its four-colour BG palettes live at `01h-1Fh` for every background *except*
BG2–BG4 in Mode 0, which are the ones given ranges of their own.

*Documented and corroborated*, by two sources that state it in opposite forms.

## The windows

### fullsnes prints the window-area field values off by one

Each layer holds four bits of a window selector: an enable and an inversion for each of the two
windows. fullsnes assigns those bits correctly — the inversion below the enable for each window —
and then prints the field's values as `(0..1=Disable, 1=Inside, 2=Outside)`, which does not follow
from its own bit assignment and does not agree with anomie or the register page.

Read as bits, the two-bit field is: `0` and `1` disabled, `2` enabled and not inverted, `3` enabled
and inverted. Read as fullsnes prints it, `1` is "inside" — but `1` has the enable bit clear.

*Documented but contested, decided by a cartridge that had to come out right.* Final Fantasy III
masks the outer eight pixels of each side of its picture. Its registers are `w12sel = $33` with
`wh0 = 8` and `wh1 = 247`, and under the bit reading that decodes to window 1 enabled and inverted
for BG1 and BG2 — everything outside the span `[8, 247]`, which is exactly columns 0–7 and 248–255.
No other reading of the nibble produces the picture the game is known to draw. **Built as bits.**

## Colour math

### Colour math does not consult the sub screen pixel's priority

fullsnes states that math occurs *"only if the front-most Sub Screen pixel has same or higher (XXX
or is it same or lower — or is it ANY priority?) priority than the Main Screen pixel"* — with the
author's own `XXX` inside the sentence. It names three mutually exclusive readings and commits to
none. Anomie's numbered rendering steps carry no such condition, and neither does the register page;
anomie's worked colour-math example maths a BG1-over-BG2 main screen against a sub screen of a
*different* background without mentioning their relative priorities, which is the one place the
condition would have to appear if it existed.

Nothing in the documentation settles it, and choosing one of three readings would be inventing a
direction their author did not know. So it was measured.

*Documented but contested, decided by a cartridge written for this question.* The cartridge draws a
grid of ten cells. Every cell adds the same main-screen pixel to a sub-screen pixel, with the two
colours chosen so the result states the answer: the main screen is red, every sub-screen source is
green, the operation is a plain add with no halving and no clipping, so a cell that mathed is yellow
and one that did not is red. Down the picture, two bands vary the main pixel — BG1 at tile priority
1, then BG1 at tile priority 0. Across it, five columns vary the sub pixel: BG2 at tile priority 1,
BG2 at 0, BG3 at 1, BG3 at 0, and nothing at all, which leaves the sub screen's backdrop and carries
no priority of any kind. The two backgrounds sit at four different places in the mode's order and
carry both values of the tile-priority bit between them, so a rule reading the bit and a rule reading
the position in the order would not produce the same picture.

**Every one of the ten cells mathed**, on both implementations it was run on. A control image — the
same picture with `$2131` reaching no layer — drew all ten red on both, so the grid does register a
negative and an all-yellow result is not an artifact of a picture that can only draw yellow.

So math is decided by the main pixel's layer, the two `$2130` regions and the `$2131` enables. **The
sub screen pixel's priority is not consulted, and neither is the question of whether it has one.**

This is a categorical result — ten cells of one colour, then ten of the other — which is not
something a display setting or a timing difference can produce. Neither implementation is original
silicon, so an observation from the console itself would outrank it; the cartridge is on the list
owed a console run (see [Sources](#sources)), and the finding is provisional until it has had one.

### The halving happens before the range is held, and the sub-screen backdrop is exempt

Both sources agree that `$2131` bit 6 halves the result and that the halving comes before the
channel is held to 0–31, which is observable: two full channels added and halved are full, not half.

*Documented and corroborated, and confirmed by measurement* — the arithmetic is otherwise derived
from one reading of two documents, which is the kind of claim that agrees with itself and with
nothing real. A cartridge draws seven bands, each under its own `$2130`/`$2131` pair, and across each
band a thirty-two step ramp of the main colour against a fixed addend of 16, so a band's ramp shape
states its answer. The addition, the subtraction, both with and without halving, and the clamping at
either end all came out as the documents describe.

**The sub-screen backdrop is exempt from the halving.** Where `$2130` bit 1 asks for the sub screen
as addend and the sub screen shows nothing at that position, its backdrop is the fixed colour and the
halving is not applied — whereas asking for the fixed colour directly, with bit 1 clear, does halve.
The band asking for the sub screen with halving on and nothing on the sub screen draws **the same
ramp as the band that adds without halving**, not the same as the band that adds and halves; those
two differ by one bit of `$2130` and have identical `$2131`, so if the halving applied to both they
would be one picture. Read as a comparison *within* one frame, which settles it without depending on
absolute colour.

### A force-blacked main pixel is not also halved

*Documented, corroborated, and confirmed against an independent implementation.* Both documents say
the halving does not apply. One reimplementation applies it and is the outlier.

- **anomie**, at `$2131`: *"Half color math. When set, the result of the color math is divided by 2
  (except when $2130 bit 1 is set and the fixed color is used, or when color is cliped)."* He states
  it twice more in the window section — *"the only difference is that half math will not occur"* and
  *"whether the pixel colors (and half-math) will be clipped"*.
- **fullsnes**: *"Half-Color (Bit6): Ignored if 'Force Main Screen Black' is used"*, and again —
  *"color addition can be still applied (but, with the 'Div2' not being applied)"*.
- **bsnes** does **not** halve it, agreeing with both documents.
- **snes9x** does **not** halve it either — a third codebase, independent of bsnes and of both
  documents' authors.
- **Mesen** halves it, and stands alone.

The cartridge that separates them draws four flat stripes — the unknown, a reference at full
strength, a reference at half, and a black control — so the reading is which two stripes match
rather than a judgement of shade. On bsnes the unknown matches the full-strength reference; on Mesen
it matches the half.

The two documents are not independent of each other — fullsnes credits anomie, so they may carry one
observation rather than two. What settles the balance is the implementations: bsnes and snes9x share
no lineage with each other or with either document's author, and all of them reach the same
behaviour. One implementation differing from two documents and two independent codebases is the
ordinary shape of a bug in that implementation.

**What is built:** the halving is **not** applied to a forced-black pixel.

**What would settle it:** a commercial cartridge that forces part of the main screen black while
asking for the halving, photographed on a real console — the picture its authors shipped, drawn by
the silicon they wrote for. Failing that, the console running a cartridge written for the question.
Neither implementation is silicon, so that observation would still outrank this — but bsnes and
snes9x agree with the reading here, and only Mesen does not.

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
