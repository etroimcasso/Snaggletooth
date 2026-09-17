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
- [Direct colour](#direct-colour)
  - [fullsnes's prose transposes direct colour's channels and its own table does not](#fullsness-prose-transposes-direct-colours-channels-and-its-own-table-does-not)
  - [What the regions and the fixed colour do to a composed pixel is undocumented](#what-the-regions-and-the-fixed-colour-do-to-a-composed-pixel-is-undocumented)
- [Mode 7](#mode-7)
  - [The multiplier's ports while Mode 7 draws: fullsnes gives a schedule in one place and garbage in another](#the-multipliers-ports-while-mode-7-draws-fullsnes-gives-a-schedule-in-one-place-and-garbage-in-another)
  - [The transform's products are truncated before the sum, on one source's word and another's guess](#the-transforms-products-are-truncated-before-the-sum-on-one-sources-word-and-anothers-guess)
  - [A flipped line reads row L XOR 255, which only the formula says](#a-flipped-line-reads-row-l-xor-255-which-only-the-formula-says)
- [Offset-per-tile](#offset-per-tile)
  - [The first column is exempt and tile T reads entry T−1, on one source's word](#the-first-column-is-exempt-and-tile-t-reads-entry-t1-on-one-sources-word)
  - [Under a fine scroll, anomie's prose and his formula name different entries](#under-a-fine-scroll-anomies-prose-and-his-formula-name-different-entries)
  - [The two rows ignore the line, and the vertical one is eight lines below](#the-two-rows-ignore-the-line-and-the-vertical-one-is-eight-lines-below)
  - [A 16×16 table serves two columns an entry, and can serve both axes from one word](#a-1616-table-serves-two-columns-an-entry-and-can-serve-both-axes-from-one-word)
- [Mosaic](#mosaic)
  - [A size written part-way down a row: fullsnes gives a counter, anomie a restart](#a-size-written-part-way-down-a-row-fullsnes-gives-a-counter-anomie-a-restart)
  - [Mode 7's blocks stand in the picture, and EXTBG reads two bits for two axes](#mode-7s-blocks-stand-in-the-picture-and-extbg-reads-two-bits-for-two-axes)
  - [The multiplier's line term subtracts a mosaic index nobody else defines](#the-multipliers-line-term-subtracts-a-mosaic-index-nobody-else-defines)
- [The windows](#the-windows)
  - [fullsnes prints the window-area field values off by one](#fullsnes-prints-the-window-area-field-values-off-by-one)
- [Colour math](#colour-math)
  - [Colour math does not consult the sub screen pixel's priority](#colour-math-does-not-consult-the-sub-screen-pixels-priority)
  - [The halving happens before the range is held, and the sub-screen backdrop is exempt](#the-halving-happens-before-the-range-is-held-and-the-sub-screen-backdrop-is-exempt)
  - [A force-blacked main pixel is not also halved](#a-force-blacked-main-pixel-is-not-also-halved)
- [Hires](#hires)
  - [The sub screen is on the left half, three sources to one](#the-sub-screen-is-on-the-left-half-three-sources-to-one)
  - [Which of a tile's sixteen pixels go to which screen is one source's](#which-of-a-tiles-sixteen-pixels-go-to-which-screen-is-one-sources)
  - [An empty sub screen's half is colour 0](#an-empty-sub-screens-half-is-colour-0)
  - [A left half takes the math and the colour window of the main pixel to its left](#a-left-half-takes-the-math-and-the-colour-window-of-the-main-pixel-to-its-left)
  - [Position 0's left half has nothing to its left, and no source says what it takes](#position-0s-left-half-has-nothing-to-its-left-and-no-source-says-what-it-takes)
  - [Mode 6's offset columns are eight positions wide whatever BG3's size bit says](#mode-6s-offset-columns-are-eight-positions-wide-whatever-bg3s-size-bit-says)
  - [Mosaic is counted in half-pixels in modes 5 and 6 and in positions under $2133 bit 3](#mosaic-is-counted-in-half-pixels-in-modes-5-and-6-and-in-positions-under-2133-bit-3)
- [Interlace](#interlace)
  - [The even field shows the even half-lines](#the-even-field-shows-the-even-half-lines)
  - [Sprites at half height answer to bit 1 alone](#sprites-at-half-height-answer-to-bit-1-alone)
  - [Mosaic under interlace reads the block's even half-line in both fields](#mosaic-under-interlace-reads-the-blocks-even-half-line-in-both-fields)
  - [Where a television puts the taller picture is not the raster's](#where-a-television-puts-the-taller-picture-is-not-the-rasters)
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

## Direct colour

### fullsnes's prose transposes direct colour's channels and its own table does not

`$2130` bit 0 reads a 256-colour background's eight-bit pixel as a colour. fullsnes says in prose
(line 1433) that the pixel's bits and the tile's three palette bits make `"BBb00:RRRr0:GGGg0"`,
which puts red in bits 9-5 and green in bits 4-0. That is the opposite of the palette word it
documents four lines earlier at 1406-1409, where blue is 14-10, green 9-5 and red 4-0 — and the
opposite of its own bit table immediately below at 1438-1446, which gives red the low five bits.
Anomie (line 1660) states it as `Red=RRRr0, Green=GGGg0, Blue=BBb00`, agreeing with the table.

*Documented but contested, decided by a cartridge written to ask it.* The composition is

```
red   = RRR r 0          green = GGG g 0          blue = BB b 0 0
```

and fullsnes's prose string is a transposition. The line numbers are here so nobody re-derives the
order from the prose.

**What settled it.** A cartridge of ours draws a grid of the eight `bgr` values against a ramp of
thirteen pixel values — every bit of the pixel byte moving on its own, each field saturating once,
and the pair that tells red from green (`$07` against `$38`) as two of its cells — beside a control
that clears the bit and draws the same grid through CGRAM instead. It was run on three independent
implementations. One of them draws all 104 cells byte for byte as the composition above; the other
two apply a colour treatment of their own, and answer every question the grid asks that a treatment
cannot move: each cell is strongest in the channel its field names, each channel's ramp climbs at
every step, and the tile's own bit lifts its own channel and nothing else. All three controls draw
one flat colour, which is what says the bit is doing the work. **Three implementations, no silicon:
the console's own reading is still owed.**

### A direct-colour pixel of zero is transparent

There is no black in direct colour: a pixel whose eight bits are zero is transparent as it is at
every other depth, rather than composing to black.

*Documented and measured.* Anomie states it; the sweep cartridge shows it in every row — the first
cell of all eight rows is the backdrop, on all three implementations.

### What the regions and the fixed colour do to a composed pixel is undocumented

Both sources say direct colour is not colour math and that `$2130`'s other fields do not gate it.
Neither says what the two regions and the fixed colour do to a pixel that was composed rather than
looked up — whether `$2130` bits 7-6 blacken it like any other main-screen pixel, whether bits 5-4
leave it as composed, and whether it is an operand like any other when `$2131` names its layer.

*Undocumented, measured.* The mechanism answers it — a composed colour replaces the palette lookup
and nothing else, so everything downstream sees a colour and cannot tell how it was made — and the
same cartridge carries four strips that put each question to the chip. On all three implementations:
the black region blackens a composed pixel like any other; **preventing math leaves the strip
identical to the grid's first row, byte for byte**; the fixed colour raises every channel of every
cell it is added to; and halving leaves every channel below what the same sum unhalved gave. **Three
implementations, no silicon.**

## Mode 7

The four sources agree on Mode 7's layout, its register widths, both priority orders, the three
screen-over behaviours and the flip bits, and the clip of the offset-minus-centre term is two sources
in different notation that reduce to one function: fullsnes (1183–1184) clears bits 12–10 of the
difference and sets them again where the difference is negative, anomie (424) keeps the low ten bits
and fills the rest with bit 13, and both keep the low ten bits under the difference's own sign. Three
things are weaker than that, and each has a cartridge of ours asking it.

### The multiplier's ports while Mode 7 draws: fullsnes gives a schedule in one place and garbage in another

fullsnes 1206–1219 states exactly what `$2134`–`$2136` hold while a Mode 7 picture is drawn: two
products a dot, the offset and line products in the line's first three dots and then matrix A times
the column in one half of each dot and matrix C times it in the other, each "divided by eight".
fullsnes 3443–3446, describing the same ports, says that in mode 7 they are usable only in vertical
and forced blank and "return garbage" while drawing. Anomie 396–398 says the product "may not be
operative during Mode 7 rendering"; the register page says nothing.

*Documented but contested, measured in part, provisional — console pending.* The schedule is built
as fullsnes states it: a program reading the ports there gets something on hardware, and the plain
product is the one answer every source says is wrong there. The cartridge that asks reads the
product's middle byte thirty-two times in a tight loop in the middle of a Mode 7 picture and paints
the samples as a bar of colours: one flat value is the plain product, a bar of small values climbing
beside `$Fx` values is the schedule, and unrelated values are garbage. Its control takes the same
reads in forced blank and must paint the plain product.

**What settled what it has.** On an FPGA reconstruction of the console the bar is there and varies
along its length, and the control paints one flat value — so the ports move while a Mode 7 picture
draws and hold the plain product in forced blank, which is the categorical half of the question and
the half the reconstruction can carry. Which values they hold at which dot is the finer half, and a
photograph of a television does not carry it; the schedule's values stand as built. The three
software implementations the cartridge was also run on paint no bar at all, for the image and its
control alike, though the program runs to its end on them; that is a fact about them running this
cartridge and not a reading of the chip, and where they and the reconstruction disagree the
reconstruction's reading is the one taken.

### The transform's products are truncated before the sum, on one source's word and another's guess

fullsnes 1185–1188 drops the low six bits of each product of a matrix term with the clipped offset or
with the line before summing them (`AND NOT 3Fh`), adds the per-pixel product whole, and at 1221–1223
gives the "/8" of the multiplier's readout as the reason to believe it. Anomie 423–431 prints the same
masks (`&~63`) under "the bit-accurate formula seems to be something along the lines of". Two
readings that agree, one of them hedged, and neither a measurement.

*Documented but contested, measured on three implementations, provisional — console pending.* The
truncation is built. It moves about a quarter of the columns by one pixel at a matrix of `$013F` and
an offset of 1, which is what the sweep cartridge's second band draws: a ruler whose colour names the
field column, so every column says which of the two readings the chip took. **On Mesen, bsnes and
snes9x every one of the 63 telling columns is the truncated reading**, and the two bands beside it
settle the clip the same way on all three: an offset of 1024 draws the ruler, so it clips to 0, and
an offset of −1025 leaves the first column to the backdrop and moves the ruler by one, so it clips to
−1. Three implementations, no silicon.

### A flipped line reads row L XOR 255, which only the formula says

fullsnes 1175–1176 and 1181–1182 flip the vertical axis as `SCREEN.Y XOR FFh` over lines 1–224, so
the first line drawn reads field row 254 and row 255 is never shown. Anomie 370–371 and the register
page say that the screen is flipped and no more; a plain mirror of the picture would read row 255
first, one row away.

*Documented once, measured on three implementations, provisional — console pending.* The formula is
built. The sweep cartridge's first band flips a field whose rows alternate two colours, so an even
line reading an odd row is the formula and an even line reading an even row is the mirror, a colour
rather than a count. **On Mesen, bsnes and snes9x every line of the band reads the other parity: the
formula, not the mirror.** Three implementations, no silicon.

## Offset-per-tile

How the chip *finds* an offset stands on one document. fullsnes's own section (1932–1934) is "under
construction (see Anomie's docs for now)", and the two wiki pages say only that BG3 encodes the
offsets. The sources do agree on everything else: which modes have a table (fullsnes 1043–1047, the
register page's mode table, anomie 152–166), the entry's layout (fullsnes 1322–1329, anomie
1750–1751 and 1818–1825), that the entry's low three bits are not read horizontally, and mode 4's
single entry with bit 15 naming its axis. A cartridge of ours,
`offset-per-tile/sweep.sfc`, asks each of the four questions below in a band of its own, with a
control image that has no table anywhere.

### The first column is exempt and tile T reads entry T−1, on one source's word

Anomie 1746 and 1755–1762: the leftmost visible tile of BG1 or BG2 takes its registers "in all cases
(although as little as 1 pixel may be visible)", and each later tile T reads BG3's tile T−1. Nothing
else says either.

*Documented once, provisional — console pending.* Built as stated. The cartridge's first band puts
the table's entry for BG3's last column at a different offset from the rest, so a first column that
read the table would show a colour the exempt one cannot.

### Under a fine scroll, anomie's prose and his formula name different entries

Anomie's formula (1748–1749, under "Hopefully these calculations are right") finds the entry by the
screen's own eighths, `((X − 8) & ~7)`, and rebuilds the position from `X`'s eighth with the fine
scroll's bits laid in. His prose (1755–1758) counts BGn's own tiles and says "it doesn't matter
whether or not the tiles actually align in any way". With `BGnHOFS & 7 = 0` the two are one reading.
Under a fine scroll of 3 they part: BGn's tile 1 covers columns 5–12, and the formula sends columns
5–7 to the entry before BG3's first — the map's last column, wrapped — and draws columns 13–15 of
every tile from the wrong position.

*Documented once and self-contradictory, provisional — console pending.* The prose is built: a table
read as each tile of the background is fetched is aligned to the background's tiles, not to the
screen's. The cartridge's second band scrolls by 3 over a table whose last column differs from its
first, so the two readings are a colour apart at six columns of every eight-pixel run.

### The two rows ignore the line, and the vertical one is eight lines below

Anomie 1748–1749 reads the horizontal entry at `BG3VOFS` and the vertical at `BG3VOFS + 8`, and
1764–1766 says "the current Y position on the screen does not affect which row of the BG3 tilemap to
reference, it's as if Y were always 0". Nothing else says either.

*Documented once, provisional — console pending.* Built as stated. The cartridge's third band places
the two rows away from the top of the table and fills every other row with offsets they do not hold,
so a row read that moved with the line would draw a different picture.

### A 16×16 table serves two columns an entry, and can serve both axes from one word

Anomie 1768–1771: a 16×16 BG3 applies each entry to "all the corresponding 8x8 subtiles", and "we may
end up using the same tile for Hval and Vval". That follows from reading the table through BG3's own
tile size, and is built that way rather than as a rule of its own; a 16×16 BG1 or BG2 still takes an
entry per 8-pixel column (1768–1769).

*Documented once, provisional — console pending.* The cartridge's fourth band reads a table whose
entries alternate by column at BG3VOFS 0: in pairs, and with the vertical offset the same word
supplies, if the size is honoured; column by column, and with no vertical offset, if it is not.

## Mosaic

The sources agree on the register (fullsnes 1054–1063, anomie 178–183, the register page), on each
block showing its upper-left pixel (fullsnes 1055–1057, anomie 184–186 and 2012–2014), on the first
block standing at the picture's left edge (fullsnes 1065, anomie 186) and its first line (fullsnes
1066), and on mosaic applying after the scroll and before the windows and colour math (anomie
197–200, 2012–2013). Three things are weaker, and a cartridge of ours, `mosaic/sweep.sfc`, asks the
first two with a control image that has mosaic off everywhere.

**What has been read of it.** The cartridge was run on an FPGA reconstruction of the console and on
Mesen, bsnes and snes9x. The reconstruction draws the sweep as this machine does, band for band; the
three software implementations each draw something else, and where they and the reconstruction
disagree the reconstruction's reading is the one taken. A reconstruction is not silicon, so every
finding below stays provisional until the console runs it.

### A size written part-way down a row: fullsnes gives a counter, anomie a restart

fullsnes 1067–1070 says the hardware "does first finish [the] current block (using the old vertical
size) before applying the new vertical size", and that vertical mosaic is implemented by subtracting
the index within the current block; 27031–27032 put the counter's reload in vertical blank and its
count on every line. Anomie 188–195 says the blocks start on "the scanline where $2106 was written",
and marks with an XXX that writing the same value does not restart them and that he does not know
which changes do.

From a block of size 0 the two agree, since every line ends a block, which is the case a program
turning mosaic on part-way down the picture exercises. From a block of size 3 with a new size written
on its second line they part: fullsnes's rows run on to the old row's end, anomie's restart where the
write landed.

*Documented twice and contested, corroborated on a reconstruction, provisional — console pending.*
fullsnes's counter is built, as the
mechanism and as the reading that gives anomie's same-value observation for free. The cartridge's
second band turns 4×4 blocks on and then writes 3×3 two lines later; the reader finds both lines from
the picture itself and scores both readings, each with its first row on the line the blocks appear or
on the line after, so a transfer landing a line later than expected does not decide the question.

### Mode 7's blocks stand in the picture, and EXTBG reads two bits for two axes

Anomie 207–214 and 2029–2037 say the matrix does not move the blocks, so BG1's corner is a picture
position the matrix then reads; and that EXTBG's BG2 reads bit 0 as vertical mosaic and bit 1 as
horizontal, so `$F1` gives 1×16 blocks, `$F2` 16×1 and `$F3` 16×16 while BG1 reads bit 0 for both.
One source, said twice.

*Documented once, corroborated on a reconstruction, provisional — console pending.* Built as stated.
The cartridge's third band turns
the field a quarter turn under 4×4 blocks, so blocks aligned to the picture and blocks aligned to the
field show different rows of it; its fourth shows EXTBG's layer alone under `$F1`, `$F2` and `$F3`.

### The multiplier's line term subtracts a mosaic index nobody else defines

fullsnes 1215–1216 gives the schedule's line term as `(SCREEN.Y − MOSAIC.Y) XOR (yflip × FFh)` and
defines `MOSAIC.Y` nowhere. It is read as the index the same document's 1069–1070 describes,
subtracted before the flip as the parentheses place it, and taken as zero where BG1's bit is clear,
since a subtraction that applied with the bit clear would mosaic a picture nobody asked to be.

*Documented once, unmeasured.* The values the ports hold at each dot are the finer half of the
multiplier question above, which no reading here has carried yet; this term is built as stated and
recorded.

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

## Hires

**fullsnes's own high-resolution section (1835–1848) is four headings over four `...` stubs**, so its
whole account of a line drawn in half-pixels is the `$2133` bit table (1005–1019), the two tile-size
notes (1032–1033), the three-layer-math paragraph (1625–1632) and eleven lines of "Hires Notes"
(1850–1866). The one description of how colour math and the colour window reach the sub screen's
half-pixel is anomie's. The two wiki pages and all three sources agree on the modes' depths and
orders, on the tile being two characters wide with the size bit choosing its height, on horizontal
scrolling counting positions (fullsnes 1851–1854, anomie 297–298), and on the half-pixel line being
forced in modes 5 and 6 and asked for by `$2133` bit 3 in the others. The rest is weaker, and a
cartridge of ours asks each question with a control image in which no line is split.

**What has been read of it.** The cartridge was run on an FPGA reconstruction of the console and on
bsnes, and our machine's frame of it fits both. Neither is silicon, so every finding below stays
provisional until the console runs it.

### The sub screen is on the left half, three sources to one

fullsnes 1012–1013 ("shift subscreen half dot to the left"), the register page p.10 ("the sub screen
to render pixels on even columns … and the main screen to render on odd columns") and anomie 612–615
put the sub screen's pixel on the even half-pixel. The Backgrounds page p.2 says the reverse. The three
are built.

*Documented three times against one, corroborated on a reconstruction and bsnes, provisional —
console pending.*

### Which of a tile's sixteen pixels go to which screen is one source's

Anomie 1845–1848 alone: a mode 5 tile's even pixels are what it shows on the sub screen and its odd
pixels what it shows on the main. Both wiki pages say only that the layers are interleaved. Built as
stated.

*Documented once, corroborated on a reconstruction and bsnes, provisional — console pending.*

### An empty sub screen's half is colour 0

fullsnes 1861–1863 alone: on a line drawn in half-pixels both screens' backdrops are colour 0, rather
than the fixed colour the sub screen otherwise stands for. It speaks of the pixel shown, and says
nothing of the addend a main pixel's math takes where the sub screen is empty; that addend keeps the
fixed colour, unhalved, as on any other line.

*Documented once, corroborated on a reconstruction and bsnes for both the shown pixel and the addend,
provisional — console pending.*

### A left half takes the math and the colour window of the main pixel to its left

Anomie 2064–2081 alone: the sub screen's half-pixel is mathed by the operation the main pixel to its
left took — none, the fixed colour, or that main pixel's own colour before its math where its addend
was the sub screen — and halved where it was halved. His example: a cyan block on the main screen over
a magenta one on the sub screen, subtracted, is green and red half-pixels. Anomie 617–620 and
2001–2002 say the colour window's two effects reach the sub half-pixel the same way, from the main
pixel to its left; fullsnes 1864–1865 calls it "an odd glitch in hires mode?" without saying what.
fullsnes 1625–1632 describes the effect without the rule, and at 1051 says modes 5 and 6 "don't
support screen addition/subtraction" — which its own 1625–1632, both wiki pages and anomie
contradict. The sub half-pixel's layer masks are no source's: they are built at its own position.

*Documented once, contradicted by one line of another, corroborated on a reconstruction and bsnes for
the math, the halving, the colour window and the masks, provisional — console pending.*

### Position 0's left half has nothing to its left, and no source says what it takes

Anomie 620, 1963 and 2072 say so outright. It is built as taking neither black nor math.

*Undocumented, read on a reconstruction and bsnes as built, provisional — console pending.*

### Mode 6's offset columns are eight positions wide whatever BG3's size bit says

Anomie 1873–1877 alone ("this applies to BG3 as well as BG1"), with the register page's footnote
("OPT entries are always 16 pixels wide", counting half-pixels) beside it. Built as stated: BG3's
size bit changes only how tall an entry is.

*Documented once, corroborated on a reconstruction and bsnes, provisional — console pending.*

### Mosaic is counted in half-pixels in modes 5 and 6 and in positions under $2133 bit 3

fullsnes 1857–1858 and anomie 2019–2026 agree on the first: a block is `2(N + 1)` half-pixels, so a
size of 0 already shows. The second is anomie 616–617 alone ("Mosaic operates as normal"); fullsnes
1859–1860 hedges a different case with "presumably?". On a layer drawn in positions, blocks of
`2(N + 1)` half-pixels whose corner is a left half are the same blocks as `N + 1` positions, so what
the cartridge separates is "as normal" from a mosaic taken on the combined 512-half-pixel line.

*The first documented twice, the second once; both corroborated on a reconstruction and bsnes,
provisional — console pending.*

## Interlace

The sources agree on the field toggling every frame and the even field running one line longer
(fullsnes 1691 and 26996, the register page p.9, anomie 637–645), and on modes 5 and 6 reading their
tilemaps in half-lines with the vertical offset counted in half-lines (fullsnes 1855–1856, the
Backgrounds page p.3 and p.5, anomie 299–300 and 1649–1650). A cartridge of ours asks the three
weaker questions below, read as a television shows it — two fields woven, the even field's line above
the odd one's — with a control image in which nothing is interlaced.

**What has been read of it.** The cartridge was run on software implementations, and our machine's
fields match theirs in every band. None of them is silicon, so every finding below stays provisional
until the console runs it.

### The even field shows the even half-lines

The Backgrounds page p.3 and the register page p.9 say the odd field is lowered half a line; anomie
1851–1852 says the field decides which half-lines are drawn without saying which is which. Built as
the two have it: line `L` of parity `F` reads half-line `2L + F`.

*Documented twice, corroborated in software, provisional — console pending.*

### Sprites at half height answer to bit 1 alone

Anomie 630–635 says `$2133` bit 1 halves the sprites "regardless of BG mode" and that bit 0 controls
only the signal sent to the television; fullsnes 1015–1018 and the register page p.10 describe bit 1
under interlace and say nothing of the other case. Anomie's is the only statement about that case and
is built. Which of a sprite's rows each field shows no source says; the pairing above is taken, with
the rows counted after the eight-bit subtraction that finds them.

*Documented once for the gating, undocumented for the rows, both corroborated in software,
provisional — console pending.*

### Mosaic under interlace reads the block's even half-line in both fields

Anomie 2020 states a `2X × 2X` block of half-pixels under interlaced modes 5 and 6; fullsnes 1858–1859
says "reportedly?". Built as stated: both fields read the half-line of the block's corner, which is
the even field's, so a size of 0 already moves the odd field.

*Documented once and hedged by another, corroborated in software, provisional — console pending.*

### Where a television puts the taller picture is not the raster's

The register page p.10 says the taller picture "shifts everything up 8 lines"; anomie 649–672 gives
his own television's behaviour line by line, down to a lost vertical sync. Both are statements about
a display. The raster is the lines the chip drew, 239 of them, in the chip's own coordinates, and
where a display places them is the display's.

*Documented, and not modelled.*

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
- Direct colour's channel order and what `$2130`'s regions and the fixed colour do to a composed
  pixel, both above: measured on three implementations and open only to the console, which no
  cartridge here has been run on yet.
- Mode 7's three, above: the truncation of the transform's products and the flipped line's row,
  read on three implementations and open only to the console; and the values the multiplier's
  ports hold at each dot while it draws, which a reconstruction has shown to move and nothing has
  yet read.
- Whether a Mode 7 register written mid-line reaches that line's offset and line terms or only the
  per-pixel ones; every term is read at the dot until something says otherwise.
- What `$2133` bit 6 shows outside Mode 7. fullsnes says garbage from an external input; no source
  gives a picture, and nothing drawn changes.
- Offset-per-tile's four, above: each built as its one source states it or, where the source parts
  from itself, as the reading described, with a cartridge of ours asking the question and nothing yet
  read from it.
- Mosaic's two, above: corroborated on a reconstruction and open to the console.
- What a mosaic size written part-way along a line does to the rest of that line. The width is read at
  the dot, so the rest of the line takes it.
- Whether a mosaic block's corner is read once or re-read from the registers at each dot of the block.
  Every dot reads the registers as they stand.
- Hires's seven, above: corroborated on a reconstruction and bsnes and open to the console — among
  them what position 0's left half takes, which no source knows, and the addend a main pixel's math
  takes where the sub screen is empty on a line drawn in half-pixels.
- Interlace's three, above: corroborated in software and open to the console.
