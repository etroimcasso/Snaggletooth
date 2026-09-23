# The SNES machine

The `Snes` class wires the 5A22's 65816 core to the console's memory: the cartridge under its map, the
128 KB of work RAM, and the APU across the communication ports. Where the [65816 core](65816-cpu.md)
runs over any bus you hand it, the machine *is* the bus — it maps a 24-bit address the way the
hardware does, prices every cycle by the region it reaches, and paces the [APU](apu-machine.md)
against the CPU on its own clock.

The machine is the system minus the picture. It does not draw. What it has is a complete memory map, an
exact clock, the video counters with their vertical-blank NMI and H/V-timer IRQ, the DMA and HDMA
engines, the hardware multiply/divide unit, the two controller ports, the [PPU's](ppu.md) complete
register file and the three video memories it fills through its ports, the picture it draws from
them, and the audio machine running underneath — enough to load a cartridge, run its code under
interrupts, play it, watch it and hear it. A host that wants to watch the bus rather than the state
sets an [observer](#the-bus-observer), and is told every access in order; one that wants the picture
sets a frame observer ([ppu.md §The picture](ppu.md#the-picture)) and is handed every frame the PPU
finishes.

## Contents

- [Building a machine](#building-a-machine)
- [The memory map](#the-memory-map)
  - [How a cartridge lays across the bus](#how-a-cartridge-lays-across-the-bus)
  - [Save RAM](#save-ram)
- [Stepping and running](#stepping-and-running)
- [The reset line](#the-reset-line)
- [Memory speed](#memory-speed)
- [The APU clock](#the-apu-clock)
- [The audio upload stub](#the-audio-upload-stub)
  - [Running a console's own boot ROM](#running-a-consoles-own-boot-rom)
- [The video counters and interrupts](#the-video-counters-and-interrupts)
  - [The line and the frame](#the-line-and-the-frame)
  - [Vertical blank](#vertical-blank)
  - [The blank flags](#the-blank-flags)
  - [The interrupts](#the-interrupts)
  - [The memory refresh](#the-memory-refresh)
- [The controller ports](#the-controller-ports)
- [The multiply/divide unit](#the-multiplydivide-unit)
- [The PPU register file](#the-ppu-register-file)
- [DMA and HDMA](#dma-and-hdma)
  - [General-purpose DMA](#general-purpose-dma)
  - [HDMA](#hdma)
- [The bus observer](#the-bus-observer)
- [The save observer](#the-save-observer)
- [Snapshot and restore](#snapshot-and-restore)
- [Reaching into the machine](#reaching-into-the-machine)
  - [Memory by bus address](#memory-by-bus-address)
  - [The memories the bus cannot name](#the-memories-the-bus-cannot-name)
  - [The CPU register file](#the-cpu-register-file)
  - [Draining audio without allocating](#draining-audio-without-allocating)
- [Gotchas](#gotchas)
- [What remains open](#what-remains-open)
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
| `$40-$7D` / `$C0-$FF:$0000-$7FFF` | under LoROM, the bytes the same bank's upper half reads |
| `$70-$7D` / `$F0-$FF:$0000-$7FFF` | save RAM, under LoROM, on a cartridge that has any |
| `$20-$3F` / `$A0-$BF:$6000-$7FFF` | save RAM, under HiROM |
| `$80-$BF:$6000-$7FFF` | save RAM, under ExHiROM |

A read of an address the machine does not map returns the last value the data bus carried — the open-bus
behavior real hardware shows. The cartridge is read-only: a write to a ROM address changes nothing.

### How a cartridge lays across the bus

The three layouts differ in how much of a bank the cartridge gets and how much image the bus can
reach. **LoROM** gives each bank its upper 32 KB and lays those halves end to end; the board leaves
the cartridge's A15 unconnected, so a cartridge bank's lower half reads what its upper half reads.
**HiROM** gives
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
answers what an image asks for without building a machine. Each map keeps the save in its own window,
listed in the table above and described in [the cartridge page](snes-cartridge.md#save-ram).

A LoROM cartridge whose header declares a coprocessor is on that chip's board, which gives lower
halves of its cartridge banks to the chip — `$60-$6F` to a DSP on the 2 MB boards, and to an ST010's
ports and RAM. The machine carries no coprocessor, so on such a cartridge every cartridge bank's lower
half reads open bus, as an absent chip's ports do, and the image is read through the upper halves
alone.

A cartridge with no save answers in the window as its board does. HiROM's and ExHiROM's windows sit in
the expansion area and read open bus. LoROM's sits in cartridge banks, where a board with no save RAM
decodes nothing, so the window's lower halves repeat their upper halves like every other cartridge
bank's: on a LoROM cartridge with no save, `$70:1234` reads the byte at `$70:9234`, and a store there
changes nothing.

Work RAM is reachable three ways that all name the same 128 KB: directly in banks `$7E-$7F`, through
the low-page mirror of any system bank, and through the data port. The data port holds a 17-bit address
in `$2181` (low), `$2182` (middle), and `$2183` (bit 16); each read or write of `$2180` moves a byte at
that address and steps it, so a block of work RAM streams through one register.

Work RAM is one chip on both buses, and a DMA or HDMA byte that names it on both — a work-RAM address
on the A bus and `$2180-$2183` on the B bus — is not copied. The port's side of the byte is open bus:
from work RAM into `$2180` nothing is written, from `$2180` into work RAM the byte written is the one
the data bus already held, and the port's address steps in neither case. From work RAM into
`$2181-$2183` the address registers keep what they had. A transfer into the port from the cartridge, or
out of it into save RAM, copies as any other does.

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

## The reset line

`reset()` is the button on the console: the reset line pulled and let go.

```cpp
machine.reset();
```

The machine starts again where construction starts it in time — the CPU about to fetch the first opcode
at the cartridge's reset vector, the beam at H = 0, V = 0 with the frame parity clear, which is where
the console starts after the line is released, and the master counter at zero, since every grid the
console counts "since reset" counts from there. What a reset initialises takes its value, and
everything else keeps what it held:

| | After `reset()` |
|---|---|
| the CPU | the direct register, both bank registers and the high bytes of X and Y zero; the stack pointer's high byte `$01`; emulation mode; the m, x and i flags set and the decimal flag clear; a wait or a stop ended; the program counter at the vector. The accumulator, the low bytes of X and Y, and the N, V, Z and C flags keep what they held. The stack pointer's low byte ends three lower: the reset sequence runs an interrupt's three stack cycles as reads. |
| `$4200`, `$420B`, `$420C`, `$420D` | `$00` |
| `$4201` | `$FF` |
| the NMI and IRQ flags, the joypad strobe, `$4218-$421F`, the work-RAM port's address | clear |
| `$4202-$420A`, `$4214-$4217`, every `$43xx` register | what they held |
| work RAM, the save, the pads in the ports | what they held |
| the PPU | forced blank, at the brightness it had; every other register and the three video memories as they were |
| the audio machine | `Apu::reset()` ([apu-machine.md](apu-machine.md)): the timer outputs clear, the targets, the divider and RAM above zero page kept — then the boot program again, when the machine was built to run one. The program is fetched from the image mapped over `$FFC0-$FFFF`; the RAM beneath the window is not rewritten. |

A transfer, a multiplication or division, an auto-read or a counter latch in progress is abandoned, and
the picture the beam was part-way down is never delivered: the next frame the frame observer is handed
is the first whole one drawn afterwards, black until the program lifts the forced blank. The observers
stay set.

The reset sequence's seven cycles are not spent and its five reads — three of the stack, two of the
vector — are not made or reported, as construction does not make them: both leave the machine at the
instant the sequence ends.

Call it between `step()` and `run()` calls, never from inside an observer's call, which arrives
part-way through a cycle. A budget `run()` was still owed is dropped with the counter.

A cartridge that pulls the slot's reset pin itself resets the CPU, the APU and the work-RAM chip and
leaves the PPU alone. Nothing the machine carries does that, and `reset()` is not it.

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
current scanline and `vpos` is the scanline down the frame; both advance as the machine runs, so a
mid-frame snapshot resumes on the exact cycle.

### The line and the frame

A scanline is 1364 master cycles and carries 340 dots. Most dots are four cycles, but **dots 323 and 327
are six**, which is what the counter latch at `$213C` answers and why a dot is not a quarter of `hpos`
past dot 322. Two lines a frame are not 1364 cycles long:

| Line | Length | When |
|---|---|---|
| Any line | 1364 | ordinarily |
| NTSC line 240 | 1360 — 340 four-cycle dots, and no long ones | on an odd field, interlace off |
| PAL line 311 | 1368 — the four extra cycles are dot 340's | on an odd field, interlace on |

Dot 340 therefore exists only on that one PAL line; no read anywhere else latches it.

A frame is 262 lines on NTSC and 312 on PAL, and an **interlaced frame of even parity runs one line
longer** — NTSC's line 262, PAL's line 312, which belongs to vertical blank like the lines before it. The
parity is `field`, which `$213F` bit 7 reports: 0 names the first frame of an interlaced pair and 1 the
second. It toggles as each frame's first line reaches H = 1, four master cycles in, whether or not
`$2133` bit 0 asks for interlace. The console leaves reset at H = 0 of line 0 with the flag clear, so the
first frame it runs is the pair's second.

Two frames together are a whole number of colour clocks, which is what the irregular lines are for:
714,732 master cycles on NTSC and 716,100 interlaced; 851,136 on PAL and 852,504 interlaced.

fullsnes's 426,936 for a PAL interlaced frame sums the extra line and the long line into one frame. They
fall in different fields — the long line is the odd field's and the extra line the even field's — so it is
a figure for neither frame of the pair, whose two lengths are 425,572 and 426,932.

### Vertical blank

`$2133` bit 2 chooses the line vertical blank begins on, 225 or 240. Beginning is a latched fact rather
than a comparison of `vpos`: the machine decides at the start of every line from 225 on while the blank
has not begun, and begins it if the line is 240 or later or if the bit is clear as that line starts. Once
begun it holds to the frame's end and clears at the start of line 0.

That gives the bit three useful behaviours. Set before line 225, it moves the blank to line 240. Set and
then cleared between the two lines, it begins the blank at the start of the next line. Set *after* the
blank has begun, it resumes nothing — but the PPU holds its video memories shut as though the picture
were still running, to line 240 ([the PPU's windows](ppu.md#the-memories-and-their-windows)).

From the line the blank begins on, in order: the vertical-blank flag at the line's start, the NMI flag two
master cycles later, the HDMA channels deactivating for the rest of the frame, the sprite table's address
returning to its reload value at dot 10, and the [auto-read](#the-controller-ports) of the controllers
beginning between H = 32.5 and H = 95.5. A later change to `$2133` re-fires none of them.

### The blank flags

`$4212` reports the beam directly. Bit 7 is the vertical-blank fact above. **Bit 6 is raised when the beam
passes H = 274 and lowered when it passes H = 1 of the next line** — on every line of the frame, vertical
blank's own lines and a forced-blank frame included. The line's first four master cycles still carry the
previous line's blank. Bit 0 is set while the auto-read is busy.

### The interrupts

Two interrupt sources reach the CPU, both driven from these counters:

- **The vertical-blank NMI.** The flag at `$4210` bit 7 sets two master cycles into the blank's first
  line and clears at the start of line 0, and reading `$4210` acknowledges it. While the flag is set and
  `$4200` bit 7 enables NMIs, the NMI line is asserted; enabling NMIs mid-blank raises the line there and
  then. The CPU latches the NMI on the flag's rise, so a program polling `$4210` that reads it in the
  very cycle the flag sets gets the flag set, clears it, and takes the NMI at the end of that instruction
  all the same. Reading the flag before re-enabling avoids taking an old NMI twice.
- **The H/V-timer IRQ.** `$4200` bits 5-4 pick the compare: a horizontal position (`$4207/$4208`), a
  vertical line (`$4209/$420A`), or both. The flag at `$4211` bit 7 is raised when the beam passes the
  point the mode names, and the IRQ line follows it; reading `$4211`, writing it, or selecting no compare
  acknowledges it. A handler that does none of them runs again. A read in the very cycle the flag rises
  is the exception: it receives bit 7 set and acknowledges nothing, so the flag still stands after it
  and the IRQ is taken.

  *The write is documented and uncorroborated.* anomie's register document states it, and no reading
  has confirmed it.

  | Mode | The point |
  |---|---|
  | H only | 14 + 4 × HTIME master cycles into every line |
  | H and V | the same point, on line VTIME |
  | V only, and either H mode with HTIME = 0 | 1374 master cycles after the previous line began — ten into a line following a normal one, fourteen after the short line, six after the long one |

  The crossing is noted as the cycle ticks and the flag is raised at the cycle's end under the mode the
  cycle leaves behind, so a write to `$4200` that arms the timer in the very cycle its point is crossed
  is in time, and one that disarms it in that cycle keeps the flag down.

  **HTIME = 153 raises no flag on the short line, nor on a frame's last line** — anomie's measurement,
  with no mechanism documented. The exception is that dot on those lines; every other HTIME raises its
  flag there, and the V-only point keeps its own place.

```cpp
// A minimal vblank-NMI loop: enable the NMI, then let the machine run into vblank.
// LDA #$80 ; STA $4200 ; ...   the handler at the $FFFA vector runs once per frame.
```

### The memory refresh

Once a line the CPU is paused for **40 master cycles** while memory refreshes. The point walks
an eight-cycle grid near the middle of the line — 538 cycles into line 0 of the first frame, then the
point on that grid nearest 536 into each line after it, so consecutive ordinary lines come up 538 and 534
and a line of another length re-phases the pair. `SnesState::refreshAt` names the next one and
`refreshLeft` the cycles left in one under way.

fullsnes's latch histogram — dot 133 three times, 134 once, 135 to 142 never, 143 once, 144 three times —
is this alternation seen without its grid: averaged over the two parities, the dots a pause covers are the
dots no read ever latches.

What it means for a caller:

- **An instruction whose cycles span the point costs 40 more**, and `step()` returns that. A program runs
  about 3 % slower against the beam, the APU and every HDMA and IRQ event, which is the console.
- **`run()` may stop inside a pause** and carries the rest of it, so its overshoot stays within one
  access and `run(a)` then `run(b)` still advances the machine exactly as `run(a + b)`.
- **A halted core is paused as well.** A machine waiting on WAI whose interrupt arrives inside a pause
  wakes when the pause ends; a machine stopped on STP keeps its place and spends the pause as any core
  does.
- **The observer is told nothing.** A pause is neither an access nor a CPU cycle, any more than a
  transfer's overhead cycles are, so cycle counts taken through the observer are unchanged.
- **A snapshot taken inside a pause restores into the rest of it.**

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

- **The auto-read.** With `$4200` bit 0 set, the machine reads all sixteen bits of each port once a
  frame, on the first line of vertical blank. The machine's first read begins at H = 74.5 of that line;
  every later one begins at the first point on a 256-master-cycle grid, carried from the previous read's
  start, that lies at or past H = 32.5 — so the start wanders between H = 32.5 and H = 95.5 from frame
  to frame. `$4212` bit 0 is busy for the 4224 master cycles the read takes: the strobe pulse for 128,
  then one bit every 256. **The registers shift as each bit arrives.** Every clock moves each port's
  register up one place and puts the bit it read at bit 0, so the sixteenth bit, 4224 cycles after the
  start, is what leaves the result in place: port 1 in `$4218/$4219`, port 2 in `$421A/$421B`, the high
  byte carrying B, Y, Select, Start, Up, Down, Left and Right from bit 7 down, the low byte A, X, L and R
  from bit 7 to bit 4 and the pad's identity code, zero for a standard pad, in bits 3-0. A program that
  reads the registers while the busy flag is set sees the previous frame's bits shifted part-way out
  above this frame's shifted part-way in; a program reads them after the flag clears. `$421C-$421F`,
  the ports' second data lines, stay zero — nothing is modelled on them.
- **The serial ports.** A write of 1 then 0 to `$4016` bit 0 strobes both pads, latching their sixteen
  bits; each read of `$4016` returns port 1's next bit in bit 0, and each read of `$4017` port 2's, in
  the order above — B first, the identity bits last. Past the sixteenth bit a pad returns 1, and an
  empty port returns 0 on every read. `$4017` bits 4-2 are wired low and read as 1; the bits neither
  port drives are open bus.

`Joypad::bits()` is the sixteen-bit word in the auto-read's layout, `$4219` in the high byte and
`$4218` in the low. A snapshot carries the pads with the rest of the machine, so a restore resumes with
the same controllers plugged in.

Three consequences of the hardware sharing one set of lines are modelled. The auto-read strobes and
clocks the same shift register the serial ports read, so after it runs a program reading `$4016`
without strobing first is past the sixteenth bit and sees padding; a program that uses both paths
strobes before it reads. A serial read made while the auto-read is busy takes a bit the auto-read then
never sees: every bit after it lands one place higher in `$4218-$421B` and the padding enters last, so
the identity code comes out as 1. And while the strobe is held high the pads reload continuously, so
every bit read — by the serial ports or by the auto-read — is the B button's state; a program that
leaves `$4016` at 1 sees `$4218/$4219` as all ones or all zeros.

## The multiply/divide unit

The unsigned multiply and divide are the CPU's, not the PPU's. Set the multiplicand at `$4202` and write
the multiplier to `$4203` to start a multiply; the 16-bit product is at `$4216/$4217` eight cycles later.
Set the 16-bit dividend at `$4204/$4205` and write the divisor to `$4206` to start a divide; the quotient
is at `$4214/$4215` and the remainder at `$4216/$4217` sixteen cycles later. The unit is clocked by the
CPU, so the wait is the same number of instructions regardless of the memory speed. The operation runs
on the operands as they stand at the write that starts it: a program can load the next dividend into
`$4204/$4205` while a division runs and still read that division's quotient, which is how a game
pipelines the four divisions of a Mode 7 matrix.

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

## The PPU register file

The PPU is reached at `$2100-$213F`, and its whole register file is here, kept exactly as the console
keeps it: every write with the latches it passes through, every read with the value it answers, and
the windows in which the video memories can be reached. Its state is one value inside the machine's,
`state().ppu`, so a snapshot carries it. Nothing renders; [ppu.md](ppu.md) describes the register file
in full — the scroll and Mode 7 latches, the multiplier, the counter latch, the status registers, the
open-bus values, the power-on state. This section covers the three memory ports, which the machine's
read faces are the other side of: `vram()` returns the 64 KB of video RAM, `cgram()` the 512-byte
palette and `oam()` the 544-byte sprite table.

The VRAM port is a word address at `$2116/$2117` and a data pair at `$2118/$2119`. `$2115` selects the
increment (after the low or the high byte, by 1, 32, or 128 words) and an optional address translation
for bitmap layouts. Reads come through `$2139/$213A` and carry the hardware's prefetch behaviour: the
first word after setting the address is returned twice, because the prefetch register fills before the
address steps rather than after. The palette port is an address at `$2121` and a two-write word at `$2122`
(read back through `$213B`); the high byte keeps seven bits.

The OAM port is an address at `$2102/$2103` and a byte at `$2104` (read back through `$2138`). The
address written is a nine-bit reload value, doubled into the ten-bit byte address the port is at, so a
write always lands on an even byte; each access steps the address. Below `$200` a write to an even byte
is held and the odd byte after it commits the pair, as the palette port does; from `$200` a write is one
byte, and `$220-$3FF` mirror the thirty-two bytes of high bits at `$200-$21F`. The PPU takes the address
back to the reload value at the start of every vertical blank while the screen is on, and when forced
blank is released during the first line of vertical blank — so a program that sets the address once and
sends its sprite table every frame lands it at the same place every frame, and one that sends while the
screen is off continues from wherever the last access left the address.

Each port reaches its memory only in the window the hardware allows — VRAM and OAM in vertical blank or
forced blank, the palette in horizontal blank too. Outside it a write is ignored and the access reported
to the [observer](#the-bus-observer) carries no landing, while the address steps as it would have; the
rule is stated in full in [ppu.md](ppu.md#the-memories-and-their-windows). The screen powers on in
forced blank, so a program that fills the memories before turning the picture on reaches them freely.

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
(`$2100-$21FF`, `$4000-$41FF`, `$4200-$421F`, `$4300-$437F`): a read there returns open bus. Nor can it
copy work RAM through the work-RAM port, in either direction; see [the memory map](#the-memory-map).

HDMA outranks a DMA in progress. An HDMA event on other channels holds the transfer for as long as the
event takes and the transfer then goes on. An event that involves the channel the transfer is on —
the frame's initialisation of it, or a line's delivery from it — ends that channel's transfer where it
stands: its `$420B` bit clears, `das` keeps the count that was left, the HDMA runs on the channel's
registers as the transfer left them, and any other selected channel runs after it as it would have.

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

When an indirect channel's count runs out part-way down the picture, the engine reads the next count and
then the pointer after it, whatever the count was. A `$00` therefore ends the channel with `das` loaded
from the two bytes that follow it and `a2a` three bytes on. The last channel delivering on the line is
the exception: on a `$00` it reads one byte, into the high half of `das` over a low half of `$00`, which
leaves `a2a` two bytes on and takes eight master cycles fewer. At the frame's initialisation a `$00`
first count ends the channel with no pointer read.

*The pointer read after a `$00` is documented and uncorroborated.* anomie's register document states
it, and no reading has confirmed it.

`$420C` is read on every line, not only at the frame's start, so a program can take a channel out of the
picture part-way down and put it back. A channel taken out delivers nothing while its bit is clear and
keeps its place in its table, so putting it back resumes where it stood — and a channel whose table has
already stopped stays stopped until the next frame.

A channel armed part-way down a picture has missed the initialisation, so it is not reloaded: it runs from
the cursor in `a2a` and the count in `nltr` exactly as the program leaves them, and `a1t` is not consulted
until the next frame begins. Set both before arming it. Whether its first line delivers depends on what
else is running: a channel that joins channels already delivering writes a unit before it fetches anything,
so point `a2a` one unit below the table; a channel armed while `$420C` is empty fetches first, so point
`a2a` at the table itself.

```cpp
// Armed part-way down a picture, with nothing else running: give the channel its own
// cursor and a count of one, which the first line spends reaching the table's entry.
//   $43x8/$43x9 = the table address        $43xA = $01        then $420C
```

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
write-back, a vector pull — who made it, for an engine's access, which of the eight channels it
served (`channel`), whether the HDMA engine was reading its own table (`table`) and whether that read
lay past the table's end (`pastTableEnd`), and, for a write through a video data port, where the port
put the byte (`landed`):

| `AccessSource` | Who |
|---|---|
| `Cpu` | The core, one access per cycle that reaches memory, with the kind it drove |
| `Dma` | The general-purpose DMA engine, each byte as a read on one bus then a write on the other |
| `Hdma` | The HDMA engine, its table reads and each byte it delivers the same way |
| `WramPort` | The work-RAM data port, reaching work RAM on its own behalf when a byte moves through `$2180` |

`channel` is 0–7 on a `Dma` or `Hdma` access and 0 on the CPU's and the port's. `table` is true on the
HDMA engine's reads of the table it is walking — a line count, a direct table's inline value, an
indirect entry's two pointer bytes — and false on every other access: the byte an indirect entry
points at is read from where the pointer says and is not the table's, and a write never is. The two
together let a host follow every byte an engine moves back to the channel and the table it came from,
which is how the [cartridge disassembler](snes-disassembler.md#what-a-run-moved) records what a run
moved.

`pastTableEnd` is true on the one or two `table` reads an indirect channel makes after a `$00` count:
the engine reads where an entry's pointer would be, and those bytes belong to whatever follows the
table. A host collecting a table's bytes leaves them out; see [HDMA](#hdma).

`landed` is set on a write through a video data port, whoever made it, and says where the port put
the byte: the VRAM word address for a write to `$2118` or `$2119`, after any address translation; the
palette word for a write to `$2122`, on both halves; the OAM byte for a write to `$2104`, on both
halves, within the 544. It is absent on every other access — a read, a write to any other register, a
write to memory. The address and the value alone cannot tell it, since the port's address steps and
translates as it goes; the port can, and the disassembler reads it to say
[where a transfer landed](snes-disassembler.md#what-a-run-moved).

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

The audio machine has an observer of its own, the [`ApuObserver`](apu-machine.md#the-observer), told
every access the sound CPU makes and every instruction boundary it crosses. `setApuObserver` sets it
on the audio machine inside the console, under the same terms — the host's object, not part of the
state, none by default — and `apuObserver()` reads it back. Because the audio machine runs inside the
CPU's cycles, its report arrives from within `step()` and `run()`, between the bus observer's
accesses. `peekApu` answers what a fetch by the sound CPU at an address returns, without making one —
the boot-ROM image while the window is mapped, the RAM byte otherwise — so a host can decode the
instruction the sound CPU is about to run.

```cpp
struct AudioLog final : ApuObserver {
  std::uint64_t instructions = 0;
  void access(std::uint16_t, std::uint8_t, bool) override {}
  void instruction(const Spc700State&, const Spc700State&, std::uint32_t) override { ++instructions; }
};

AudioLog audio;
machine.setApuObserver(&audio);
machine.run(21'477'272);      // one second: every instruction the sound CPU ran is counted
machine.setApuObserver(nullptr);
```

The [intermediate representation](ir.md#running-beside-the-machine) is the first consumer of both:
a run replayed instruction by instruction with an interpreter beside each core, held to every access
the two observers report.

## The save observer

A cartridge with a battery keeps what it writes, in a window the machine holds as state
([snes-cartridge.md](snes-cartridge.md)). Reading that window says what it holds; the save observer
says when it *changed*, so a program keeping the save for a person does not have to compare the whole
window against a copy of its own to find out.

```cpp
struct Keeper final : SaveObserver {
  void changed(std::span<const std::uint8_t> save) override {
    // `save` is the whole window at the size the cartridge declares.
  }
};

Keeper keeper;
machine.setSaveObserver(&keeper);
```

It is told once at the end of a frame that changed the window, however many stores landed in it, and
not at all in a frame where none did — and again after `restore()`, a caller replacing the window
having changed it as surely as a store would. The span is the machine's own storage, valid for the
call.

The report is handed over between cycles, beside a finished picture, rather than from inside the line
that ends a frame: what a program does with a save — writing a file, most plainly — may fail, and it
is free to throw here.

On the same terms as the others: the host's object, not part of the state, so a snapshot does not
carry it and `restore()` leaves it in place; `saveObserver()` reads back what is set; the machine
starts with none, and one that has none runs exactly as it would have.

Nothing here knows where a save goes. `snes_player` keeps one as the file other emulators read —
[user-files.md](user-files.md) — which is the whole of what a tool built on this has to decide.

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

`state()` returns the machine's own state, not a copy of it: the audio machine runs inside
`state().apu` (the [APU machine](apu-machine.md#running-in-storage-you-hold) built over that
object), so reading the state after every step costs no copy of its 64 KB of sound RAM. A machine is
moved, never copied; a moved machine carries its audio machine after its state.

## Reaching into the machine

Beside the whole-state snapshot, a host reaches into individual places the machine holds, reading and
writing them without spending a cycle and without a register's side effect. This is for a host that
edits memory, seeds a value, or reads one out between runs — not for the program the machine runs.

### Memory by bus address

`peek` answers the byte a 24-bit bus address holds, `poke` writes it, and `addressable` says whether a
span is memory the face reaches. The three agree, because `addressable` is `peek`'s own answer.

```cpp
std::optional<std::uint8_t> byte = machine.peek(0x7E0000);  // a work-RAM byte
machine.poke(0x008000, 0x42);                               // a byte of the cartridge image
bool ok = machine.addressable(0x008000, 2);                 // two ROM bytes: true
```

`peek` reaches work RAM, the cartridge's ROM and its save, and answers `std::nullopt` for anything the
face does not reach as memory — a register, or an address the cartridge leaves open. `poke` returns
whether the byte landed. A `poke` to ROM changes the machine's own copy of the image, not a file, and no
snapshot carries it; a `poke` to the save writes the save without reporting it to the save observer,
which reports the program's stores rather than the host's own edits. A register address is refused by
both: reading a register on the console would change it, and the register file is already in the state a
host holds.

### The memories the bus cannot name

Video RAM, the palette, the sprite table and the audio machine's RAM are written by name, each the way
the chip reads it: no port address steps, no latch moves, no increment happens. The picture path reads
these at every dot, so a write shows at the next one.

```cpp
machine.writeVram(0x1234, 0xAB);    // 64 KB
machine.writeCgram(0x00, 0x1F);     // 512 bytes
machine.writeOam(0x00, 0x80);       // 544 bytes
machine.writeApuRam(0x0200, 0x5C);  // the audio machine's RAM
```

An address past a memory's end is ignored. The read-only spans `vram()`, `cgram()`, `oam()` and
`peekApu()` hand the same bytes back.

### The CPU register file

`cpuState()` reads the 65816's registers whole, and `setCpuState()` writes them, reloading the live core
so the written set is live on the next cycle. Instruction progress is part of the value, so a machine
written mid-instruction resumes exactly where the value says.

```cpp
Cpu65816State regs = machine.cpuState();
regs.pc = 0x8000;
machine.setCpuState(regs);  // the next step runs from $8000
```

### Draining audio without allocating

`takeFrames()` returns the stereo frames produced since the last drain in a fresh vector.
`takeFrames(std::span<StereoFrame>)` drains into the caller's own storage instead and returns how many
it wrote; frames past the end of the span stay queued for the next drain, and nothing is allocated — a
host producing sound on a callback that must not allocate drains through this form.

```cpp
std::array<StereoFrame, 512> buffer;
std::size_t written = machine.takeFrames(buffer);
```

## Answering an access

Beside the observer, which reports every settled access and cannot change it, a host answers accesses
before they take effect. It sets an `AccessWatcher` and arms the places it cares about; each access to
an armed place asks the watcher what happens — let it through, prevent it, or stand a byte in its place.

```cpp
struct Guard final : Snes::AccessWatcher {
  Snes::AccessAnswer read(std::uint32_t address, std::uint8_t value, AccessSource source) override {
    return Snes::AccessAnswer::instead(0xFF);  // answer $FF wherever the program reads here
  }
  Snes::AccessAnswer write(std::uint32_t address, std::uint8_t value, AccessSource source) override {
    return Snes::AccessAnswer::veto();          // drop the program's writes here
  }
};

Guard guard;
machine.setAccessWatcher(&guard);
machine.watchAccess(0x7E0100, 16, /*onRead=*/true, /*onWrite=*/true);  // 16 bytes, both directions
```

An answer is one of three: `proceed()` lets the access happen as it would; `veto()` prevents a write,
and leaves a read as it stands, since nothing can stop the chip receiving a byte; `instead(byte)` puts
`byte` in the access's place — the read delivers it, the write stores it.

`watchAccess` arms `bytes` bytes from an address, for reads, writes, or both; `unwatchAccess` disarms
them. The two directions are independent, arming a place already armed does nothing, and a machine that
has never armed a place holds no table at all — so a watch nobody arms costs nothing. A watch sees every
access to an armed place — the CPU's, either transfer engine's, and the work-RAM port's — and `source`
tells the watcher which made it.

The watcher is the host's object, not part of the state: a snapshot does not carry it and `restore()`
leaves it in place. With none set, or nothing armed, an access pays a single test.

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
- A DMA from work RAM into `$2180` moves nothing, and one from `$2180` into work RAM fills its target
  with the last byte on the bus. Work RAM is copied to work RAM by the CPU.
- A DMA on a channel HDMA is using stops at the next HDMA event with bytes left in `das`. Keep the two
  on different channels.
- A transfer's A-bus bank is fixed: the address wraps within its bank and never crosses into the next.
- HDMA re-initialises every frame and delivers only on the visible lines; arm `$420C` and set the table
  before line 0, and it stops on its own at vblank.
- An observer sees fetches too: to count only the data an instruction touched, drop `OpcodeFetch` and
  `OperandFetch`. To count only what the program did, drop the engines' sources; to see every cycle
  the CPU spent, count its accesses and the internal cycles together.
- An opcode fetch is a read like any other: arming a code address for reads has the watcher answer the
  CPU's fetches of it, so `instead(byte)` there feeds the core a different opcode.
- The access watch and the bus observer are separate mechanisms. A read the watch substitutes is what
  the observer reports, because the machine answered that byte; a write the watch vetoes or substitutes
  the observer still reports as the source drove it — the watch changed the effect, not the drive.
- A `step()` that crosses the line's refresh returns 40 master cycles more than the instruction's own.
  Timing a routine by summing `step()` over a frame includes about 260 of those pauses, which is what
  the console spends.
- A WAI whose interrupt lands inside a pause returns from `step()` after the pause, not before it. A
  program that wakes once a line and writes a PPU register straight away lands that write up to 40
  cycles further along the line than the interrupt itself arrived.
- The frame parity a machine runs its first frame at is 1, not 0: the console leaves reset with the flag
  clear and the toggle at H = 1 of line 0 is four cycles away. A program reading `$213F` bit 7 to pick a
  field sees the pair's second frame first.
- `hpos` is master cycles, not dots, and past dot 322 the two stop being a factor of four apart. Read
  the dot through `$213C` or the PPU's own input rather than dividing.
- `reset()` starts `SnesState::master` again at zero. A host timing a run across one adds what ran
  before it, and calls it between two `run()` calls — an observer is called from inside one.
- A LoROM program that reads below `$8000` in a cartridge bank is reading the image, not open bus.
  Whether `$70-$7D` and `$F0-$FF` answer with the save or the image there depends on whether the
  cartridge has a save, which `SnesConfig::saveRamBytes` decides when it is set.

## What remains open

Questions the documentation leaves about the beam, recorded rather than decided by invention:

- **Whether a cycle in progress when the pause begins is stretched by it or completes first.** Here it
  completes; fullsnes's latch histogram reads either way.
- **Whether the refresh grid and a transfer's alignment grid are one.** A transfer aligns to a multiple
  of eight master cycles since power-on and the refresh's grid is offset two from it. A cartridge of ours
  asks it.
- **Whether a transfer in flight is cut by the refresh.** Here a DMA byte or an HDMA event completes and
  the pause follows it. A cartridge of ours asks it.
- **What `$4212` bit 7 shows** when the taller picture is asked for after vertical blank has begun. The
  memories shut; the flag here stays the latched fact. A cartridge of ours asks it.
- **Asking for the taller picture at the very start of line 225.** anomie measures the NMI one line
  later, at 226, with the last HDMA still on line 224 — the two effects skewed against each other — and
  reports that asking for it at any later line does nothing at all. No mechanism is given for either,
  and the machine here holds vertical blank to line 240 instead. Clearing the bit in that window is
  measured and modelled: the blank begins at the start of the line that follows. A cartridge of ours
  asks it.
- **When a mid-frame change to the interlace bit reaches the extra, short and long lines.** Each length
  is decided by the state as its own line runs. A cartridge of ours asks it.
- **Where inside the auto-read's window each bit lands.** The documents give the window's length and the
  256-cycle grid its start keeps, not the point at which each of the sixteen bits is clocked. Here the
  strobe pulse takes 128 cycles and each bit 256, so the sixteenth lands as the busy flag clears. A
  cartridge of ours asks it.
- **How long the timer's condition holds for a read of `$4211` to catch it.** fullsnes gives four to
  eight master cycles. Here it is the one CPU cycle the flag rises in, whatever that cycle's length. A
  cartridge of ours asks it, and whether a write clears the flag with it.
- **Whether the frame's HDMA initialisation reads an indirect pointer after a `$00` first count.**
  anomie's register document ends the channel "immediately" and hedges it. Here no pointer is read.
  A cartridge of ours asks it.
- **The arithmetic unit's result ports during the countdown.** The ports here hold the previous value
  until the whole result lands. A cartridge of ours asks it.
- **Reading or writing `$4016` inside the auto-read's window.** One clock line is shared, so a read there
  takes a clock the auto-read then never sees. A cartridge of ours asks it.
- **`$4201` across a reset.** fullsnes's I/O map gives `$FF`; anomie's register document says
  "unchanged on reset" with a question mark. Here it is `$FF`. A cartridge of ours asks it.
- **`SETINI` across a reset.** fullsnes gives "00h?" and no other document gives anything. Here the
  register keeps what it held, as every PPU register but `INIDISP`'s top bit does. A cartridge of
  ours asks it.
- **An arithmetic job in progress at a reset.** No document says. Here it is abandoned and the result
  registers keep what they held. A cartridge of ours asks it.

## See also

- [The cartridge](snes-cartridge.md) — the header, the three maps, and where every address lands.
- [The 65816 CPU core](65816-cpu.md) — the instruction set the machine runs.
- [The APU machine](apu-machine.md) — the audio machine on the other side of the ports, and its
  own observer, which `setApuObserver` reaches.
