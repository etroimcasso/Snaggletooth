# The PPU

The picture processor is the part of the [machine](snes-machine.md) a program reaches at
`$2100`–`$213F`. This page describes what is built of it: the whole register file, exactly as the
console keeps it — every write with the latches it passes through, every read with the value it
answers and the side effects it has, and the windows in which the three video memories can be
reached. Nothing here draws. The register file is the state a renderer reads, and it is complete
so that a program's dialogue with the chip is already what the hardware would have seen.

## Contents

- [The surface](#the-surface)
- [What the chip is told](#what-the-chip-is-told)
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
does not load it. Vertical blank is lines 225 to the end of the frame; line 0 is not part of it.

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
- Whether the software latch works when the port's bit 7 *was* high but is no longer.
- The pixel or so the chip draws from the old value when `INIDISP` is written (the register page's
  early-read note); a drawing-side matter.
- Bit 7 of `VMAIN` at power-on, which the documentation marks unknown and this state leaves clear.
