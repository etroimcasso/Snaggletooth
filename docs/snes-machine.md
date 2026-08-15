# The SNES machine

The `Snes` class wires the 5A22's 65816 core to the console's memory: the LoROM cartridge, the
128 KB of work RAM, and the APU across the communication ports. Where the [65816 core](65816-cpu.md)
runs over any bus you hand it, the machine *is* the bus — it maps a 24-bit address the way the
hardware does, prices every cycle by the region it reaches, and paces the [APU](apu-machine.md)
against the CPU on its own clock.

The machine is the system minus the picture. It has no PPU, no DMA, and no interrupt sources yet —
those are later components. What it has is a complete memory map, an exact clock, and the audio
machine running underneath, which is enough to load a cartridge, run its code, and hear it.

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

`SnesConfig::iplStub` asks for the APU's upload stub to be seeded. The stub itself arrives with a later
component; for now the APU boots into its ready state either way, so the field is accepted but has no
effect yet.

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

## See also

- [The 65816 CPU core](65816-cpu.md) — the instruction set the machine runs.
- [The APU machine](apu-machine.md) — the audio machine on the other side of the ports.
