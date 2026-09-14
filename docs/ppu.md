# The PPU

The picture processor is the part of the [machine](snes-machine.md) a program reaches at
`$2100`–`$213F`. This page describes what is built of it: the whole register file, exactly as the
console keeps it — every write with the latches it passes through, every read with the value it
answers and the side effects it has, the windows in which the three video memories can be reached,
and the picture the chip draws from them, which a host watches through a frame observer.

Where the published documentation is incomplete, ambiguous or wrong, [ppu-behavior.md](ppu-behavior.md)
records what the sources say, how they disagree and what settles each case — the tilemap and character
bases, the picture's edges, the brightness law and the latch flag among them.

## Contents

- [The surface](#the-surface)
- [What the chip is told](#what-the-chip-is-told)
- [The frame](#the-frame)
- [The picture](#the-picture)
- [Writes and their latches](#writes-and-their-latches)
- [The memories and their windows](#the-memories-and-their-windows)
- [The multiplier](#the-multiplier)
- [The counter latch](#the-counter-latch)
- [The status registers and the three open buses](#the-status-registers-and-the-three-open-buses)
- [Power-on values](#power-on-values)
- [What remains open](#what-remains-open)

## The surface

The PPU's state is one value, `PpuState`, held inside the machine's state as `SnesState::ppu`
(`include/snaggletooth/snes/ppu.h`). A snapshot of the machine carries it whole and a restore puts
it back, mid-latch included. Every register that is stored as written keeps the name the hardware
gives it (`objsel`, `bgmode`, `w12sel`, `cgadsub`, …); the three memories are `vram`, `cgram` and
`oam`, and the machine's `vram()`, `cgram()` and `oam()` faces read them.

```cpp
Snes m(SnesConfig{.rom = rom});
// ... run the program ...
const PpuState& p = m.state().ppu;
p.bgmode;            // $2105 as last written
p.bg1hofs;           // BG1's horizontal offset as its two writes assembled it
p.multiplyResult();  // the signed 24-bit product at $2134-$2136
m.vram()[0x20];      // the low byte of VRAM word $0010
```

`Ppu` is the chip's behaviour over that state: `read`, `write`, `latchCounters` and `beginVblank`.
The machine builds one over its own state for each access and never keeps it; a host that runs the
machine never needs to touch it.

## What the chip is told

At every access the machine hands the PPU its input pins as `PpuInputs`: the beam's dot and line,
the frame parity, the vertical- and horizontal-blank signals as `$4212` reports them, the clock
rate, and the level of the counter-latch line, which is bit 7 of the I/O port written at `$4201`.
The chip learns nothing else about the machine; every rule below is stated in those terms.

The dot is not a quarter of the line's master cycle throughout. A line carries 340 dots and most are
four master cycles, but **dots 323 and 327 are six** — which is what the counter latch answers, and why
the last dot of an ordinary line is 339 rather than 340. Dot 340 exists on one line only: PAL's line 311
on an odd interlaced field, which runs 1368 cycles. The short line, NTSC's 240 on an odd field with
interlace off, is 340 four-cycle dots and has no long ones.

| Master cycles into the line | Dot |
|---|---|
| 0–1291 | 0–322, four cycles each |
| 1292–1297 | 323 |
| 1298–1309 | 324–326 |
| 1310–1315 | 327 |
| 1316–1363 | 328–339 |
| 1364–1367 | 340, on the long line only |

## The frame

The chip is told the frame's shape through `$2133`, and two of its bits change what the beam does. The
machine's own account of the line and frame lengths, the frame parity and every event's master offset is
under [the video counters](snes-machine.md#the-video-counters-and-interrupts); what the chip does with
them is here.

**Bit 2 — the taller picture.** Vertical blank begins at line 240 instead of 225, so the picture is 239
lines rather than 224. The chip reads the bit from its own register, so a change reaches the beam at the
start of the next line that asks.

**Bit 0 — interlace.** The frame of even parity runs one line longer, and PAL's line 311 of an odd field
runs four cycles long. The parity is `$213F` bit 7, which the chip answers from its field pin.

Three events belong to the chip rather than the machine:

- **The sprite table's address returns to its reload value at dot 10 of vertical blank's first line** —
  which line that is, the chip takes from its own bit 2 — unless the screen is in forced blank, when it
  was not drawing and the address stands. Releasing forced blank during that line reloads it there and
  then.
- **The two sprite overflow flags in `$213E` clear as the frame's first line begins**, unless the screen
  is in forced blank: they belong to the picture just finished, and a frame the chip spent blank leaves
  whatever they hold.
- **The counter latch** captures the dot above and the line, on a read of `$2137` or the latch line
  falling.

## The picture

A host that wants to see what a program draws sets a frame observer on the machine
(`include/snaggletooth/snes/video_frame.h`). It is told every frame the chip finishes, as the beam
reaches the next frame's first line:

```cpp
struct Watcher final : FrameObserver {
  void frame(const VideoFrame& picture) override {
    picture.pixels;  // row-major from the top, four bytes a pixel: red, green, blue, 255
    picture.width;   // 256
    picture.height;  // 224, or 239 under the taller picture
    picture.field;   // the parity the frame ran under
  }
};

Snes m(SnesConfig{.rom = rom});
Watcher watcher;
m.setFrameObserver(&watcher);
m.run(cycles);
```

The span is the machine's own buffer and is valid for the call; a host that keeps a picture copies
it. The observer is not part of the state — a snapshot does not carry it and `restore()` leaves it
in place — and the chip resolves pixels only while one is set, so a machine nobody is watching
draws nothing and a program cannot tell the difference.

**One pixel per visible dot.** The picture is dots 22 to 277 of every line the frame's own vertical
blank leaves below it; the frame's first line draws nothing, which is why a background offset of
−1 is what puts a tilemap's first row on the picture's first line. Each pixel is resolved from the
registers and the memories **as they stand at its own dot**, so a write that lands mid-line changes
the dots after it and not the ones before.

**Mode 1 is what the chip draws**: its three backgrounds, each on the main screen `$212C` enables.
BG1 and BG2 are sixteen colours, BG3 is four.

- The tilemap entry for a position is `(Base << 10) + ((Y & 0x1F) << 5) + (X & 0x1F)` words, plus
  the terms a wide or tall map adds — `Base` being bits 2–7 of the background's own screen
  register (`$2107`, `$2108`, `$2109`), which count whole 32×32 screens of `$400` words, and the
  map's own size wrapping the position.
- The entry is `vhopppcc cccccccc`: both flips, the tile's priority, its palette, its number.
- The character is `(Base << 13) + Tile × 8 × planes` bytes, `Base` being the background's nibble
  of `$210B` (BG1 low, BG2 high) or `$210C` (BG3 low). Planes 0 and 1 are the low and high bytes of
  eight words and each further pair is sixteen bytes on, so a four-colour character is sixteen
  bytes and a sixteen-colour one is thirty-two. The leftmost pixel of a row is bit 7.
- The palette a tile shows in begins `ppp` × its colours into CGRAM — sixteen words apart for BG1
  and BG2, four for BG3 — and Mode 1 gives none of the three a starting palette of its own, so
  BG3's palette 1 and BG1's palette 0 name the same words. Colour 0 of any palette is transparent.
- `$2105` bits 4, 5 and 6 make each entry of BG1, BG2 or BG3 a 16×16 block of `Tile`, `Tile+1`,
  `Tile+16`, `Tile+17`. The numbers run on rather than wrapping inside the block, and a flip
  reverses the block whole.
- Each background scrolls by its own pair of offset registers: `$210D`/`$210E` for BG1,
  `$210F`/`$2110` for BG2, `$2111`/`$2112` for BG3.

**The order the three are drawn in**, front to back, is the one `$2105` names. Writing `A` and `a`
for BG1's tiles at priority 1 and 0 and the same for the others, it is

```
A B a b C c        and with $2105 bit 3 set:   C A B a b c
```

so bit 3 lifts BG3's high-priority tiles from behind everything to in front of everything, and
leaves its low-priority tiles where they are. The first background in that order with a
non-transparent pixel is the one shown; where none has one, the backdrop — palette word 0 — shows.
Sprites take their own places in this order and are not drawn yet.

**The converter drives eight bits a channel.** A palette word is five bits a channel, and `INIDISP`
brightness N scales each by `(N + 1) / 16`, computed as `round(c × (N + 1) × 255 / (31 × 16))` in
integers. Brightness 0 is the screen off, and forced blank is black; both give a completed black
frame rather than no frame.

**What is not drawn yet**, so a reader does not go looking for it: sprites; BG4, which Mode 1 does
not have, so `$212C` bit 3 shows nothing; the sub screen, so a layer enabled only on `$212D` shows
nowhere; the windows and colour math; mosaic; and every mode but 1, which show their backdrop. Each
arrives with its own work.

## Writes and their latches

Most registers store the byte written and nothing more. Three groups do not.

**The scroll registers** (`$210D`–`$2114`) are written twice, low byte then high, through one
latch all eight share. A vertical offset takes `(value << 8) | latch`. A horizontal offset takes
`(value << 8) | (latch & ~7) | (its own previous value >> 8 & 7)`: the low three bits come from
the register's own high byte, not the latch. After either, the latch is the byte just written. The
value is kept as the two writes assembled it; the offset the renderer uses is its low ten bits.
Because the latch is shared, writing the registers in a mixed order gives a mixed result — the
register page's own caution.

**The Mode 7 registers** (`$211A`–`$2120`, and `$210D`/`$210E` again as `M7HOFS`/`M7VOFS`) are
written twice through a second latch of their own: `(value << 8) | latch`, then the latch is the
byte. A write to `$210D` or `$210E` updates BG1's offset through the scroll latch *and* Mode 7's
through this one; the two are separate registers with separate histories. Every write to `$211C`
is also the multiplier's 8-bit operand.

**The fixed colour** (`$2132`) is three 5-bit channels, `fixedRed`, `fixedGreen` and `fixedBlue`;
a write stores its low five bits into each channel whose select bit (5, 6, 7) is set.

The OAM address (`$2102`/`$2103`), the VRAM port control and address (`$2115`–`$2117`) and the
palette address (`$2121`) behave as described under the machine's
[video memory ports](snes-machine.md#the-ppu-register-file).

## The memories and their windows

A program can reach the three memories only when the chip is not using them:

| Memory | Reachable in |
|---|---|
| VRAM (`$2118`/`$2119` written, `$2139`/`$213A` read, `$2116`/`$2117` prefetching) | vertical blank or forced blank |
| OAM (`$2104` written, `$2138` read) | vertical blank or forced blank |
| CGRAM (`$2122` written, `$213B` read) | vertical blank, horizontal blank or forced blank |

Outside its window a write is ignored: the byte does not land, and the access reported to the
[bus observer](snes-machine.md#the-bus-observer) carries no landing. The address steps all the
same — VRAM's by the step `$2115` selects, OAM's by one, CGRAM's flip-flop and word address as they
would have. A read outside the window answers with the chip's open bus for that port (below) and
steps the same way; a VRAM read does not refill the prefetch register, and setting the VRAM address
does not load it.

Vertical blank here is the machine's latched fact, not a comparison of the line: it runs from the line
`$2133` bit 2 chooses — 225 or 240 — to the end of the frame, and line 0 is not part of it. One state
parts the windows from that fact. **Asking for the taller picture after the blank has already begun shuts
the memories again until line 240**: it resumes neither the picture nor anything the blank stopped, but
the chip holds VRAM, the sprite table and the palette's blank-only half as though it were still drawing.
Forced blank opens them regardless.

```cpp
// The screen on, the beam inside the picture: nothing lands, the address moves.
// LDA #$0F ; STA $2100     forced blank off
// LDA #$80 ; STA $2115     a word address, stepped after the high byte
// STZ $2116 ; STZ $2117    word 0
// LDA #$34 ; STA $2118     ignored
// LDA #$12 ; STA $2119     ignored; the address is now word 1
```

## The multiplier

`$2134`–`$2136` hold the signed 24-bit product of the signed 16-bit matrix A (`$211B`) and the
signed byte last written to `$211C`. It is available as soon as either is written, with no delay,
and `PpuState::multiplyResult()` computes the same value. A transfer engine can read the three
bytes onto the A bus like any B-bus register, which is how a program fills memory from it. At
power-on both operands are `-1`, so the product reads `$000001`.

## The counter latch

Reading `$2137`, or the I/O port's bit 7 falling from 1 to 0 (a write to `$4201`), latches the
beam's dot into `OPHCT` (`$213C`) and its line into `OPVCT` (`$213D`) and raises the latch flag,
bit 6 of `$213F`. Reading `$2137` latches only while bit 7 of the port is high; the value the read
returns is the CPU's open bus. Each counter reads out in two halves through its own flip-flop:
first the low byte, then the ninth bit in bit 0 under the second half's open bus in bits 7–1.
Reading `$213F` clears the flag and resets both flip-flops; latching does not reset them. The port
reads back at `$4213` as it was written — nothing on the console drives any of its lines.

```cpp
// LDA $2137                 latch (the port powers on with bit 7 high)
// LDA $213C ; STA $50       the dot's low byte
// LDA $213C ; STA $51       bit 0: the dot's ninth bit; bits 7-1: open bus
// LDA $213D ; STA $52       the line's low byte
// LDA $213D ; STA $53       its ninth bit
// LDA $213F                 the flag read and cleared, the halves reset
```

## The status registers and the three open buses

`$213E` (`STAT77`): bit 7 the sprite time-over flag, bit 6 the range-over flag, bit 5 zero (master),
bit 4 the first half's open bus, bits 3–0 the version, 1.

`$213F` (`STAT78`): bit 7 the frame parity, bit 6 the latch flag, bit 5 the second half's open bus,
bit 4 the clock rate (set for a machine built with `Region::Pal`), bits 3–0 the version, 3.

The chip is two halves, and each remembers the last byte read through its own ports and answers
with it where a read has nothing to say:

| | Remembers the last read of | Answers |
|---|---|---|
| First half | `$2134`–`$2136`, `$2138`–`$213A`, `$213E` | a read of `$2104`–`$2106`, `$2108`–`$210A`, `$2114`–`$2116`, `$2118`–`$211A`, `$2124`–`$2126`, `$2128`–`$212A`, in every bit; bit 4 of `$213E` |
| Second half | `$213B`–`$213D`, `$213F` | bit 7 of `$213B`'s second half; bits 7–1 of `$213C`'s and `$213D`'s second halves; bit 5 of `$213F` |

A read of any other write-only register, and of `$2137`, returns the CPU's own open bus — the
last byte the data bus carried, which for an absolute read is the instruction's own operand byte.
So a palette read whose low half returned a byte with bit 7 set reads that bit back in its high
half, and `LDA $2104` after `LDA $2134` returns what the multiplier's low byte was.

## Power-on values

Where the hardware's power-on value is documented the state starts there: `INIDISP` `$80` (forced
blank), `BGMODE` `$0F`, `VMAIN` `$0F`, `M7A` `$FFFF`, `M7B` `$FFFF`, `SETINI` `$00`, both counters
`$01FF` with the flag clear, the I/O port `$FF`. Everything else starts at zero. A program that
sends to VRAM without first writing `$2115` inherits a step of 128 words through the 8-bit
translation, which is what the console does with it.

## What remains open

Each of these is a question the documentation leaves, recorded rather than decided by invention:

- Whether line 0 counts as vertical blank for the memory windows. The blank flag is clear on it;
  the chip is fetching the first line's sprites. It is treated as not blank.
- What the VRAM prefetch register holds after a read outside its window. The documentation says
  invalid data; here the register keeps what it had.
- Where a CGRAM write inside the picture lands. The documentation says the wrong address, which is
  the address the chip's own palette fetch was at; until the fetch is built there is no honest
  address, so the write does not land.
- Whether reading `$213F` clears the latch flag while a condition that sets it is still active.
- Whether the software latch works when the port's bit 7 was high and has since fallen.
- The pixel or so the chip draws from the old value when `INIDISP` is written (the register page's
  early-read note); a drawing-side matter.
- Bit 7 of `VMAIN` at power-on, which the documentation marks unknown and this state leaves clear.
- How to count the line's long dots. The register page states the dot clock as four five-cycle dots and
  its own measured latch quantities as two six-cycle ones; the latch is what a program can read, so the
  two six-cycle dots are what the chip answers.
- What `$4212` bit 7 reports while the taller picture is asked for after vertical blank has begun. The
  memories shut; the flag stays the blank's latched fact.
- When a mid-frame change to the interlace bit reaches the frame's irregular lines. Each length is
  decided by the state as its own line runs.
- Whether the sprite table reloads on *any* fall of `INIDISP` bit 7, as anomie has it, or only during
  vertical blank's first line, as the register page has it. The register page is followed.
- Exactly where in the frame's first line the overflow flags clear. The register page marks the dot
  itself uncertain; they clear as the line begins.
- Whether the overflow flags clear in a frame the chip spent in forced blank. anomie has them reset at
  vertical blank's end with no exception; the register page excepts forced blank, which is followed.
- Whether the overflow flags are set regardless of the sprite enables, as the register page states. The
  flags have nothing to set them until the chip draws sprites.
- Where the picture's last dot is. The event list gives the visible span as dots 22–277 and marks it
  with its own question mark; the span is taken as written rather than rounded to something tidier.
- How far ahead of a dot the chip fetches that dot's map entry and character. A pixel is resolved from
  the registers and memories as they stand at its own dot, which is where a mid-picture write lands;
  the distance itself is a measurement against a test ROM that has not been made.
- What a write to a scroll register mid-line does to a tile whose entry the chip has already fetched.
- How the palette's own mid-line access window sits against the chip's fetch of the colours it is
  drawing with.
- What the memory refresh's pause does to a counter latched inside it.
