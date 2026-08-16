# The SNES machine

The `Snes` class wires the 5A22's 65816 core to the console's memory: the LoROM cartridge, the
128 KB of work RAM, and the APU across the communication ports. Where the [65816 core](65816-cpu.md)
runs over any bus you hand it, the machine *is* the bus — it maps a 24-bit address the way the
hardware does, prices every cycle by the region it reaches, and paces the [APU](apu-machine.md)
against the CPU on its own clock.

The machine is the system minus the picture. It does not draw and has no DMA engine. What it has is a
complete memory map, an exact clock, the video counters with their vertical-blank NMI and H/V-timer IRQ,
the hardware multiply/divide unit, a PPU register file that fills video memory without rendering it, and
the audio machine running underneath — enough to load a cartridge, run its code under interrupts, and
hear it.

## Building a machine

A machine is built from a value: the cartridge image and the console clock rate. The image is copied
in, so the span it comes from need not outlive the call.

```cpp
#include "snaggletooth/snes/snes.h"
using namespace snaggletooth;

std::vector<std::uint8_t> cartridge = /* a LoROM image */;
Snes machine(SnesConfig{.rom = cartridge, .region = Region::Ntsc});
```

Construction seeds the power-on state: work RAM cleared, the APU in its post-boot ready state, and the
CPU in emulation mode with the interrupt-disable flag set and its program counter at the cartridge's
reset vector (the 16-bit word at `$00:$FFFC`). The machine is ready to run its first instruction.

`Region::Ntsc` and `Region::Pal` select the console clock rate. The choice is fixed for the machine's
life, like the cartridge, and `region()` reports it back.

`SnesConfig::iplStub` seeds the APU's upload stub, on by default. With it on, the APU boots the small
program that runs the upload handshake, the way the console does; with it off, the APU boots straight
into its ready state and a host loads a program into audio RAM directly. See
[The audio upload stub](#the-audio-upload-stub).

## The memory map

The bus maps a 24-bit address as LoROM does:

| Address | Content |
|---|---|
| `$7E-$7F:$0000-$FFFF` | the 128 KB of work RAM |
| `$00-$3F` / `$80-$BF:$0000-$1FFF` | the first 8 KB of work RAM, mirrored into every system bank |
| `$00-$3F` / `$80-$BF:$2140-$217F` | the APU communication ports (four registers, mirrored every four bytes) |
| `$00-$3F` / `$80-$BF:$2180-$2183` | the work-RAM data port |
| `$00-$3F` / `$80-$BF:$420D` | MEMSEL, the second region's speed select |
| `$8000-$FFFF` (any bank) | the cartridge, laid out in the LoROM window |

A read of an address the machine does not map returns the last value the data bus carried — the open-bus
behavior real hardware shows. The cartridge is read-only: a write to a ROM address changes nothing.

Work RAM is reachable three ways that all name the same 128 KB: directly in banks `$7E-$7F`, through
the low-page mirror of any system bank, and through the data port. The data port holds a 17-bit address
in `$2181` (low), `$2182` (middle), and `$2183` (bit 16); each read or write of `$2180` moves a byte at
that address and steps it, so a block of work RAM streams through one register.

```cpp
// Point the port at $00100 and stream two bytes into work RAM.
// (From CPU code: STA $2181/$2182/$2183 to set the address, then STA $2180 twice.)
```

## Stepping and running

The machine advances two ways. `step()` runs one whole CPU instruction and returns the master cycles
it took:

```cpp
std::uint32_t cost = machine.step();  // e.g. 16 for an immediate load from slow ROM
```

`run(budget)` spends an exact number of master cycles and returns that count:

```cpp
machine.run(357'954);  // one NTSC scanline's worth of master cycles
```

A cycle is priced by its region, so a budget rarely lands on a cycle boundary. When it falls
part-way through a cycle, the machine finishes that cycle and carries the small overshoot into the
next call — so `run(a)` followed by `run(b)` advances the machine exactly as `run(a + b)` would,
and `run(0)` does nothing. `step()` always finishes on an instruction boundary; called after a `run()`
stopped mid-instruction, it completes the instruction in progress rather than starting a new one.

## Memory speed

The console runs three memory speeds, counted in master cycles per access:

| Speed | Master cycles | Regions |
|---|---|---|
| fast | 6 | registers `$2000-$3FFF` and `$4200-$5FFF`; the second LoROM region when MEMSEL is set |
| slow | 8 | work RAM, the expansion region, and the first LoROM region |
| extra slow | 12 | the manual joypad ports `$4000-$41FF` |

MEMSEL (`$420D` bit 0) selects the speed of the second region — banks `$80-$BF:$8000-$FFFF` and
`$C0-$FF` — between slow (its power-on default) and fast. Internal CPU cycles, which drive an address
without reaching memory, run at the fast rate. The map is the same in both regions; only the master
clock's absolute rate differs.

## The APU clock

The APU keeps its own clock, and it runs at the same speed on every console: a 24.576 MHz crystal
divided by 24, a 1,024,000 Hz cycle rate. Only the master clock changes by region, and only slightly —
21,477,273 Hz on NTSC versus 21,281,370 Hz on PAL, about a 1% difference. So the APU advances at very
nearly the same pace either way: roughly one APU cycle for every 21 master cycles, and precisely

| Region | Master clock | Master cycles per APU cycle |
|---|---|---|
| NTSC | 236,250,000 / 11 Hz (≈ 21.477 MHz) | ≈ 20.97 |
| PAL | 21,281,370 Hz (≈ 21.281 MHz) | ≈ 20.78 |

PAL's master clock is a touch slower, so its APU runs a touch faster relative to it. The machine paces the APU by exact integer
arithmetic rather than by these decimals, so a run is reproducible to the byte and never drifts: it
carries the exact ratio `5632 / 118125` on NTSC and `102400 / 2128137` on PAL (each just
`1,024,000 / master clock` reduced to lowest terms).

Frames the APU produces accumulate as the machine runs; drain them with `takeFrames()`, which returns
the 32 kHz stereo frames delivered since the last drain and empties the queue.

```cpp
machine.run(21'477'273);                      // about one NTSC second of master cycles
auto frames = machine.takeFrames();           // 32000 stereo frames — the APU's 32 kHz rate, one second's worth
```

Communication with the APU is the CPU's job: a store to `$2140-$2143` reaches the APU's input latches,
and a read returns its output latches — the two ready bytes `$AA` and `$BB` on ports 0 and 1 at
power-on. The APU advances in step with the CPU, so a value written on one cycle is there for the APU
on the next.

## The audio upload stub

When the machine boots with `iplStub` on, the APU starts where the console starts it: a small program
in the top of audio RAM that waits for the main CPU to hand it a driver. The main CPU sends a program
across the four communication ports, and the stub writes it into audio RAM and jumps to it.

The handshake runs entirely through the ports at `$2140-$2143` (the APU reads them as `$F4-$F7`):

1. The stub posts `$AA` to port 0 and `$BB` to port 1 to signal it is ready.
2. The main CPU writes a destination address to ports 2 and 3, a non-zero value to port 1, and `$CC`
   to port 0. The stub acknowledges by echoing port 0.
3. For each byte, the main CPU writes the byte to port 1 and the running index to port 0; the stub
   stores the byte and echoes the index. The index counts every byte, and the destination follows it
   past a page boundary, so a block of any length lands where it was addressed.
4. To start the program, the main CPU writes zero to port 1 and a fresh value to port 0; the stub jumps
   to the address in ports 2 and 3.

Every step waits for the stub's echo before the next, so the two processors stay in step whatever their
relative speed. The stub occupies `$FFC0-$FFFF`, the window the console maps its boot program to. The
APU serves the stub image over that window while CONTROL bit 7 is set (the APU machine's
[boot-ROM window](apu-machine.md#the-boot-rom-window)): a driver may scratch-write the RAM beneath the
window and still re-enter `$FFC0` to receive more code, because a read there returns the mapped image,
not the driver's scratch bytes — the way the console's boot ROM reads back after an upload writes over
the RAM under it. Entering the stub at `$FFC0` clears the ports and re-runs the handshake from the
ready bytes.

With `iplStub` off, none of this runs: the APU keeps the state it booted with, which is how a program
placed directly into audio RAM skips the handshake.

## The video counters and interrupts

The machine tracks where the beam is even though it draws nothing. `hpos` is the master cycle within the
current scanline and `vpos` is the scanline down the frame; both advance as the machine runs. A scanline
is 1364 master cycles (341 dots of four cycles each), and a frame is 262 lines on NTSC or 312 on PAL. The
visible picture is lines 1 to 224; vertical blank runs from line 225 to the last line, and line 0 is the
line after it. To keep the colour signal in step, NTSC shortens line 240 by one dot on every other frame;
the `field` bit alternates each frame and selects it. (Interlace and overscan belong to the real PPU and
are out of scope here — the structure modelled is the non-interlace one.)

Two interrupt sources reach the CPU, both driven from these counters:

- **The vertical-blank NMI.** The flag at `$4210` bit 7 sets at the start of vblank and clears at its
  end, and reading `$4210` acknowledges it. While the flag is set and `$4200` bit 7 enables NMIs, the NMI
  line is asserted; enabling NMIs mid-vblank raises the line there and then. Reading the flag before
  re-enabling avoids taking an old NMI twice.
- **The H/V-timer IRQ.** `$4200` bits 5-4 pick the compare: at a horizontal dot (`$4207/$4208`), at a
  vertical line (`$4209/$420A`), or at both. When the counter reaches it, `$4211` bit 7 latches and the
  IRQ line asserts; reading `$4211` or disabling the IRQ acknowledges it. An IRQ handler must acknowledge,
  or it runs again.

`$4212` reports the current position directly: bit 7 is set during vblank, bit 6 during hblank (outside
the active picture, which spans master cycles 88 to 1112), and bit 0 while the auto-joypad read is busy.

```cpp
// A minimal vblank-NMI loop: enable the NMI, then let the machine run into vblank.
// LDA #$80 ; STA $4200 ; ...   the handler at the $FFFA vector runs once per frame.
```

The auto-joypad read is a stub: with `$4200` bit 0 enabled, `$4212` bit 0 reads busy for a fixed window
early in each frame's vblank, and `$4218-$421F` read back zero — the reliable "no buttons" result — since
no controller is modelled.

## The multiply/divide unit

The unsigned multiply and divide are the CPU's, not the PPU's. Set the multiplicand at `$4202` and write
the multiplier to `$4203` to start a multiply; the 16-bit product is at `$4216/$4217` eight cycles later.
Set the 16-bit dividend at `$4204/$4205` and write the divisor to `$4206` to start a divide; the quotient
is at `$4214/$4215` and the remainder at `$4216/$4217` sixteen cycles later. The unit is clocked by the
CPU, so the wait is the same number of instructions regardless of the memory speed.

A read before the result lands returns the register's previous contents — the intermediate is not
modelled, because it is not documented; wait the cycles the way hardware programs do. Two quirks are
modelled: starting a multiply immediately loads the quotient register with the multiplier (the two
operations share the unit), and dividing by zero yields an all-ones quotient with the dividend as the
remainder.

```cpp
// LDA #7 ; STA $4202 ; LDA #9 ; STA $4203   start 7 * 9
// ... a few cycles ...
// LDA $4216                                 -> 63
```

## The video registers (a stub)

There is no rendering PPU, but the register file that feeds one is here so a program can fill video
memory and a host can read what it drew. `vram()` returns the 64 KB of video RAM and `cgram()` the
512-byte palette; both are read faces, filled through the ports the console uses.

The VRAM port is a word address at `$2116/$2117` and a data pair at `$2118/$2119`. `$2115` selects the
increment (after the low or the high byte, by 1, 32, or 128 words) and an optional address translation
for bitmap layouts. Reads come through `$2139/$213A` and carry the hardware's prefetch behaviour: the
first word after setting the address is returned twice, because the prefetch register fills before the
address steps rather than after. The palette port is an address at `$2121` and a two-write word at `$2122`
(read back through `$213B`); the high byte keeps seven bits. `$2100` (forced blank and brightness), the
background base registers (`$2107-$210C`), and the main-screen enables (`$212C`) store their values for a
PPU to read. The screen powers on in forced blank.

## DMA and HDMA

DMA copies bytes between the A bus (memory) and the B bus (the `$2100-$21FF` registers) far faster than
the CPU can. A read on one bus is a write on the other, so a transfer always crosses buses. There are
eight channels, shared between two modes: general-purpose DMA, which halts the CPU and runs a block in one
burst, and HDMA, which delivers a table's values to a register once per visible scanline while the picture
draws. Each channel is a sixteen-byte register file at `$43n0-$43nF` (channel `n` = 0..7), and
`state().dma[n]` exposes it:

| Register | Address | Meaning |
|---|---|---|
| `dmap` | `$43n0` | direction (bit 7: 0 = A→B, 1 = B→A), indirect HDMA (bit 6), address step (bits 4-3), transfer pattern (bits 2-0) |
| `bbad` | `$43n1` | the B-bus register: the low byte of a `$21xx` address |
| `a1t` / `a1b` | `$43n2-$43n4` | the DMA source address (HDMA: the table start); the bank is fixed across a transfer |
| `das` | `$43n5/$43n6` | the DMA byte count (HDMA: the running indirect address) |
| `dasb` | `$43n7` | the bank of an HDMA indirect address |
| `a2a` | `$43n8/$43n9` | the HDMA table's current position |
| `nltr` | `$43nA` | the HDMA line counter (bits 6-0) and the repeat flag (bit 7) |
| `unused` | `$43nB`/`$43nF` | one spare byte, at two addresses; `$43nC-$43nE` read open bus |

The transfer pattern (bits 2-0 of `dmap`) chooses which B-bus registers a unit touches, as offsets from
`bbad`, and so how many bytes a unit is:

| Pattern | Bytes | B offsets | Typical use |
|---|---|---|---|
| 0 | 1 | +0 | WRAM, Mode 7 |
| 1 | 2 | +0 +1 | VRAM (`$2118/$2119`) |
| 2 | 2 | +0 +0 | OAM, palette |
| 3 | 4 | +0 +0 +1 +1 | scroll, Mode 7 parameters |
| 4 | 4 | +0 +1 +2 +3 | window |
| 5 | 4 | +0 +1 +0 +1 | — |
| 6, 7 | | as 2, 3 | — |

### General-purpose DMA

Set a channel's registers, then write `$420B` with a bit set for each channel to run. The transfer engages
after one more CPU cycle — in the middle of the following instruction — and the CPU is halted until every
selected channel is done, lowest channel number first. The address step (bits 4-3 of `dmap`) walks the
A-bus address after each byte: increment (0), decrement (2), or hold it fixed (1 or 3) to fill from one
source byte. A byte count of zero means the whole 65536; when the transfer finishes, `das` is zero and the
channel's `$420B` bit clears. DMA cannot reach the memory-mapped registers on the A bus
(`$2100-$21FF`, `$4000-$41FF`, `$4200-$421F`, `$4300-$437F`): a read there returns open bus.

```cpp
// A ROM->VRAM copy: channel 0, pattern 1, source $7E:0010, 8 bytes to $2118.
//   $4300 = $01   ; A->B, increment, pattern 1
//   $4301 = $18   ; B-bus = $2118 (VMDATAL)
//   $4302 = $10 ; $4303 = $00 ; $4304 = $7E   ; source $7E:0010
//   $4305 = $08 ; $4306 = $00                 ; 8 bytes
//   $420B = $01                               ; run channel 0
```

Each transferred byte is eight master cycles regardless of the region it reaches, on top of eight per
channel and eight for the whole transfer; the transfer also aligns to an eight-cycle boundary before it
starts and rounds up to a whole CPU cycle before the CPU resumes. Because the CPU is halted a byte at a
time, a snapshot taken with `run()` mid-transfer resumes on the exact byte.

### HDMA

Write `$420C` with a bit set per channel to arm HDMA. At the start of each frame the armed channels
initialise from `a1t`, and on every visible scanline each active channel delivers one entry to its B-bus
register, so a table can change a register — a scroll position, the brightness, a Mode 7 matrix — as the
beam moves down the screen. All channels deactivate at the start of vblank.

A table entry is a line-count byte followed by data. The line-count byte is `$00` to stop the channel for
the frame, `$01-$80` to write one unit and then wait that many scanlines, or `$81-$FF` (the repeat flag)
to write a unit on each of the next `count` lines. A direct table holds the data inline; an indirect table
(bit 6 of `dmap`) holds a 16-bit pointer per entry, and the data is read from `dasb:das`.

```cpp
// Change brightness partway down the screen: write $2100 on line 0, then again on line 2.
//   table: 02 0A 01 0B 00
//          ^^ write once, wait 2 lines   ^^ write once, wait 1   ^^ stop
```

## Snapshot and restore

The whole mutable machine is a value. `state()` returns a `SnesState` coherent at any cycle the machine
has stopped on, mid-instruction included; `restore()` replaces the mutable machine and resumes exactly
there. The cartridge and clock rate are the machine's fixed identity and are not part of the snapshot —
restoring a state keeps them in place.

```cpp
SnesState saved = machine.state();
machine.run(100'000);
machine.restore(saved);   // back to the saved cycle, exactly
```

## Gotchas

- The reset vector is read from the cartridge at construction. An image with a zero vector starts the
  CPU at `$0000`, which is work RAM.
- `run()`'s budget is in *master* cycles, not CPU cycles; a single CPU cycle is 6, 8, or 12 of them.
- MEMSEL is slow at power-on. Code that wants the fast second region sets `$420D` bit 0 itself.
- Frames are output, not state: a snapshot does not carry pending frames, and `restore()` discards any.
- A multiply or divide result is not there immediately; read it after the documented cycles. Reading
  early returns the old value, not a partial one.
- The first VRAM read after setting the address returns the same word twice — the documented prefetch
  glitch. Issue a dummy read, or account for it.
- An IRQ handler must acknowledge the timer (read `$4211` or disable the IRQ); an NMI handler need not,
  but reading `$4210` before re-enabling NMIs avoids taking a stale one.
- A DMA byte count of zero (`das`) transfers the whole 65536 bytes, not none.
- A transfer's A-bus bank is fixed: the address wraps within its bank and never crosses into the next.
- HDMA re-initialises every frame and delivers only on the visible lines; arm `$420C` and set the table
  before line 0, and it stops on its own at vblank.

## See also

- [The 65816 CPU core](65816-cpu.md) — the instruction set the machine runs.
- [The APU machine](apu-machine.md) — the audio machine on the other side of the ports.
