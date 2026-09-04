# The SNES machine

The `Snes` class wires the 5A22's 65816 core to the console's memory: the cartridge under its map, the
128 KB of work RAM, and the APU across the communication ports. Where the [65816 core](65816-cpu.md)
runs over any bus you hand it, the machine *is* the bus — it maps a 24-bit address the way the
hardware does, prices every cycle by the region it reaches, and paces the [APU](apu-machine.md)
against the CPU on its own clock.

The machine is the system minus the picture. It does not draw. What it has is a complete memory map, an
exact clock, the video counters with their vertical-blank NMI and H/V-timer IRQ, the DMA and HDMA
engines, the hardware multiply/divide unit, the two controller ports, a PPU register file that fills
video memory without rendering it, and the audio machine running underneath — enough to load a
cartridge, run its code under interrupts, play it, and hear it. A host that wants to watch the bus
rather than the state sets an [observer](#the-bus-observer), and is told every access in order.

## Contents

- [Building a machine](#building-a-machine)
- [The memory map](#the-memory-map)
  - [How a cartridge lays across the bus](#how-a-cartridge-lays-across-the-bus)
  - [Save RAM](#save-ram)
- [Stepping and running](#stepping-and-running)
- [Memory speed](#memory-speed)
- [The APU clock](#the-apu-clock)
- [The audio upload stub](#the-audio-upload-stub)
  - [Running a console's own boot ROM](#running-a-consoles-own-boot-rom)
- [The video counters and interrupts](#the-video-counters-and-interrupts)
- [The controller ports](#the-controller-ports)
- [The multiply/divide unit](#the-multiplydivide-unit)
- [The video registers (a stub)](#the-video-registers-a-stub)
- [DMA and HDMA](#dma-and-hdma)
  - [General-purpose DMA](#general-purpose-dma)
  - [HDMA](#hdma)
- [The bus observer](#the-bus-observer)
- [Snapshot and restore](#snapshot-and-restore)
- [Gotchas](#gotchas)
- [See also](#see-also)

## Building a machine

A machine is built from a value: the cartridge image and the console clock rate. The image is copied
in, so the span it comes from need not outlive the call.

```cpp
#include "snaggletooth/snes/snes.h"
using namespace snaggletooth;

std::vector<std::uint8_t> cartridge = /* a cartridge image */;
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

The bus maps a 24-bit address the way the console does:

| Address | Content |
|---|---|
| `$7E-$7F:$0000-$FFFF` | the 128 KB of work RAM |
| `$00-$3F` / `$80-$BF:$0000-$1FFF` | the first 8 KB of work RAM, mirrored into every system bank |
| `$00-$3F` / `$80-$BF:$2140-$217F` | the APU communication ports (four registers, mirrored every four bytes) |
| `$00-$3F` / `$80-$BF:$2180-$2183` | the work-RAM data port |
| `$00-$3F` / `$80-$BF:$420D` | MEMSEL, the second region's speed select |
| `$8000-$FFFF` (any bank) | the cartridge |
| `$40-$7D` / `$C0-$FF:$0000-$FFFF` | the cartridge across the whole bank, under HiROM and ExHiROM |
| `$70-$7D` / `$F0-$FD:$0000-$7FFF` | save RAM, under LoROM |
| `$20-$3F` / `$A0-$BF:$6000-$7FFF` | save RAM, under HiROM |
| `$80-$BF:$6000-$7FFF` | save RAM, under ExHiROM |

A read of an address the machine does not map returns the last value the data bus carried — the open-bus
behavior real hardware shows. The cartridge is read-only: a write to a ROM address changes nothing.

### How a cartridge lays across the bus

The three layouts differ in how much of a bank the cartridge gets and how much image the bus can
reach. **LoROM** gives each bank its upper 32 KB and lays those halves end to end. **HiROM** gives
each of `$40-$7D` and `$C0-$FF` a whole 64 KB and lays those end to end, reaching the same bytes
through the matching system bank's upper half. **ExHiROM** is HiROM with a second 4 MB: banks
`$80-$FF` serve the first 4 MB as HiROM does and banks `$00-$7D` serve the second. The bus reads the
image through the [cartridge functions](snes-cartridge.md), which is where each map is spelled out
address by address.

`SnesConfig::map` chooses among them. Left absent it is read from the image's own header, which is
what lets any cartridge boot without the caller knowing its layout; set it to run an image whose
header is wrong, absent, or not a header at all.

```cpp
Snes machine(SnesConfig{.rom = image});                              // the header decides
Snes forced(SnesConfig{.rom = image, .map = CartridgeMap::HiRom});   // the caller decides
```

`detectCartridgeMap` answers the same question on its own, without building a machine, and
`parseCartridgeHeader` reads the whole header.

**An image repeats across the window it does not fill.** A cartridge carries one ROM chip per power of
two in its size, wired one after another, and the board leaves the address lines above a chip
undecoded — so an address past a chip reads that chip again rather than running into the next one. A
512 KB image is one chip and repeats whole. A 3 MB image is a 2 MB chip and a 1 MB chip, and the
megabyte above it repeats **the second** chip, not the image.

### Save RAM

`SnesState::sram` is the cartridge's save, as large as its header declares and empty when it declares
none. It is machine state rather than configuration: a snapshot carries the save and `restore()` puts
it back, so a game persists one by reading it out.

The size comes from the header, and it matters that it is exact — an address past the end of the save
repeats it from the start, and a game that writes twice and reads back once is measuring how much save
RAM the cartridge really has. `SnesConfig::saveRamBytes` overrides the header; `declaredSaveRamBytes`
answers what an image asks for without building a machine. A cartridge declaring none leaves those
addresses reading open bus. Each map keeps the save in its own window, listed in the table above and
described in [the cartridge page](snes-cartridge.md#save-ram).

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
machine.run(357'368);  // one NTSC frame's worth of master cycles (262 lines of 1364)
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
relative speed. The echo is the last thing a step does, and it is what releases the main CPU: once it
appears, every input port is the main CPU's again, and a game that rewrites ports 2 and 3 on the same
instruction it reads the acknowledgement cannot disturb a command already taken. A command reads the
destination from ports 2 and 3 as a single 16-bit word, so no address is ever assembled from two
different values.

The stub occupies `$FFC0-$FFFF`, the window the console maps its boot program to. The
APU serves the stub image over that window while CONTROL bit 7 is set (the APU machine's
[boot-ROM window](apu-machine.md#the-boot-rom-window)): a driver may scratch-write the RAM beneath the
window and still re-enter `$FFC0` to receive more code, because a read there returns the mapped image,
not the driver's scratch bytes — the way the console's boot ROM reads back after an upload writes over
the RAM under it. Entering the stub at `$FFC0` clears the ports and re-runs the handshake from the
ready bytes.

With `iplStub` off, none of this runs: the APU keeps the state it booted with, which is how a program
placed directly into audio RAM skips the handshake.

### Running a console's own boot ROM

`SnesConfig::bootRom` takes a 64-byte boot image to run in place of the stub:

```cpp
std::array<std::uint8_t, kIplWindowBytes> image = readBootRom();  // your own dump
Snes machine(SnesConfig{.rom = cartridge, .bootRom = image});
```

The supplied image is seeded into audio RAM and mapped over the `$FFC0` window exactly as the stub is,
so everything above applies unchanged — the audio unit simply executes those bytes instead. Left absent,
the machine runs the stub. The field is ignored when `iplStub` is off, which skips the boot sequence
entirely.

The image is configuration rather than machine state: it is not part of `SnesState`, and it survives
`restore()`. A snapshot therefore carries the RAM beneath the window, never the mapped image.

Snaggletooth ships no console boot code and none is required — the stub runs the same documented
handshake. Supplying a dump matters when a program checks the window's contents rather than its
behaviour: test software that checksums the boot ROM is satisfied only by the console's own bytes.

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
the active picture, which spans master cycles 88 to 1112), and bit 0 while the
[auto-read](#the-controller-ports) of the controllers is busy.

```cpp
// A minimal vblank-NMI loop: enable the NMI, then let the machine run into vblank.
// LDA #$80 ; STA $4200 ; ...   the handler at the $FFFA vector runs once per frame.
```

## The controller ports

A controller is a value, `Joypad` — the twelve buttons of a standard pad, each a `bool`, true when
pressed — and a port holds one or nothing. The machine starts with both ports empty. `setJoypad` plugs a
pad in or pulls it out; `joypad` reads back what a port holds.

```cpp
machine.setJoypad(JoypadPort::One, Joypad{.start = true});   // Start held on port 1
machine.run(357'368);                                          // a frame: the program reads it
machine.setJoypad(JoypadPort::One, Joypad{});                  // released, still plugged in
machine.setJoypad(JoypadPort::One, std::nullopt);              // unplugged
```

The program reads a pad the two ways the console offers, and both see the same value:

- **The auto-read.** With `$4200` bit 0 set, the machine reads all sixteen bits of each port at the start
  of every vertical blank, holding `$4212` bit 0 busy for the 4224 master cycles the read takes, and
  lands the result in `$4218-$421F` as the window closes: port 1 in `$4218/$4219`, port 2 in
  `$421A/$421B`. The high byte carries B, Y, Select, Start, Up, Down, Left and Right from bit 7 down;
  the low byte A, X, L and R from bit 7 to bit 4, and the pad's identity code, zero for a standard pad,
  in bits 3-0. A program reads the registers after the busy flag clears; while the read is in progress
  they hold the previous frame's result. `$421C-$421F`, the ports' second data lines, stay zero — nothing
  is modelled on them.
- **The serial ports.** A write of 1 then 0 to `$4016` bit 0 strobes both pads, latching their sixteen
  bits; each read of `$4016` returns port 1's next bit in bit 0, and each read of `$4017` port 2's, in
  the order above — B first, the identity bits last. Past the sixteenth bit a pad returns 1, and an
  empty port returns 0 on every read. `$4017` bits 4-2 are wired low and read as 1; the bits neither
  port drives are open bus.

`Joypad::bits()` is the sixteen-bit word in the auto-read's layout, `$4219` in the high byte and
`$4218` in the low. A snapshot carries the pads with the rest of the machine, so a restore resumes with
the same controllers plugged in.

Two consequences of the hardware sharing one set of lines are modelled. The auto-read strobes and
clocks the same shift register the serial ports read, so after it runs a program reading `$4016`
without strobing first is past the sixteenth bit and sees padding; a program that uses both paths
strobes before it reads. And while the strobe is held high the pads reload continuously, so every
bit read — by the serial ports or by the auto-read — is the B button's state; a program that leaves
`$4016` at 1 sees `$4218/$4219` as all ones or all zeros.

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

## The bus observer

`state()` says what the machine holds after a step. An observer says what the machine *did* to get
there: every access that crossed the bus, in order, and every CPU cycle that drove an address without
one. A host that needs the order of two writes inside one instruction, the address of every read, or a
count of CPU cycles between two instruction boundaries sets one; a host that needs none of that never
pays for it — with no observer set, an access costs one check, and the machine runs exactly as it does
without the feature.

```cpp
#include "snaggletooth/snes/snes.h"
using namespace snaggletooth;

struct Log final : BusObserver {
  std::vector<BusAccess> accesses;
  std::uint32_t cpuCycles = 0;
  void access(const BusAccess& a) override {
    accesses.push_back(a);
    if (a.source == AccessSource::Cpu) ++cpuCycles;
  }
  void internal(std::uint32_t, std::optional<CycleKind>) override { ++cpuCycles; }
};

Log log;
machine.setObserver(&log);
machine.step();               // one instruction: every access it made is in log.accesses,
                              // and log.cpuCycles is how many CPU cycles it took
machine.setObserver(nullptr);
```

A `BusAccess` carries the 24-bit address, the byte that crossed the bus (what the bus answered for a
read, what the source drove for a write), which way it went, the `CycleKind` the core drove — an
opcode or operand fetch, a data read or write, a read-modify-write's read, unmodified write and
write-back, a vector pull — and who made it:

| `AccessSource` | Who |
|---|---|
| `Cpu` | The core, one access per cycle that reaches memory, with the kind it drove |
| `Dma` | The general-purpose DMA engine, each byte as a read on one bus then a write on the other |
| `Hdma` | The HDMA engine, its table reads and each byte it delivers the same way |
| `WramPort` | The work-RAM data port, reaching work RAM on its own behalf when a byte moves through `$2180` |

`internal` is called for a CPU cycle that drives an address without a valid access; `kind` is set when
the pins say what the cycle was for — a read-modify-write's modify cycle — and absent for a plain
internal cycle. A halted cycle, while the CPU waits or has stopped, drives nothing and reports nothing;
so do a transfer engine's overhead cycles. Every CPU access plus every internal cycle is therefore the
CPU's cycle count, which is what `tests/snes/observer_test.cpp` holds it to instruction by
instruction.

A byte moved through the data port is reported twice: the port's own access to work RAM, at the
bank-`$7E` address it reached, and then the access to `$2180` that moved it — the CPU's or an
engine's. The port's comes first, because it completes inside the cycle that caused it and the
causing access is reported once its value is settled.

The observer is the host's object, not part of the state: a snapshot does not carry it, `restore()`
leaves it in place, and it must outlive every step it is set for. `observer()` reads back what is
set; the machine starts with none.

The [intermediate representation](ir.md#running-beside-the-machine) is its first consumer: a run
replayed instruction by instruction with an interpreter beside the core, held to every access the
observer reports.

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
- A pad set during a frame is read on that frame's vertical blank, not the one before it: set the pad,
  then run the frame. An empty port and a pad with nothing pressed differ past the sixteenth serial
  bit, which is how a program tells a controller from no controller.
- A DMA byte count of zero (`das`) transfers the whole 65536 bytes, not none.
- A transfer's A-bus bank is fixed: the address wraps within its bank and never crosses into the next.
- HDMA re-initialises every frame and delivers only on the visible lines; arm `$420C` and set the table
  before line 0, and it stops on its own at vblank.
- An observer sees fetches too: to count only the data an instruction touched, drop `OpcodeFetch` and
  `OperandFetch`. To count only what the program did, drop the engines' sources; to see every cycle
  the CPU spent, count its accesses and the internal cycles together.

## See also

- [The cartridge](snes-cartridge.md) — the header, the three maps, and where every address lands.
- [The 65816 CPU core](65816-cpu.md) — the instruction set the machine runs.
- [The APU machine](apu-machine.md) — the audio machine on the other side of the ports.
