# The cartridge

`snaggletooth/snes/cartridge.h` reads a cartridge image as a value: its header, the map it lays across
the bus under, and where each bus address lands in the image. The [machine](snes-machine.md) reads a
cartridge through these functions, and so does every tool built over it, so the two can never disagree
about where a byte is.

Nothing here runs anything. A header is parsed from a span of bytes; a map translates between a 24-bit
bus address and an image offset in both directions; a region says what an address reaches. Building a
machine is the caller's next step, or not — a disassembler wants the header's vectors and the map and
never a machine at all.

## Contents

- [Surface](#surface)
- [The header](#the-header)
- [The three maps](#the-three-maps)
- [Where an address lands](#where-an-address-lands)
- [Save RAM](#save-ram)
- [Gotchas](#gotchas)
- [See also](#see-also)

## Surface

Everything lives in `snaggletooth`.

| Symbol | Purpose |
|---|---|
| `CartridgeMap` | `LoRom`, `HiRom` or `ExHiRom` — how an image lays across the bus. |
| `CartridgeHeader` | What a header says: the site it was read from, its map, the title, the map-mode byte and its fast bit, the save-RAM size code and the bytes it means, the checksum pair and whether they agree, and the interrupt vectors. |
| `NativeVectors`, `EmulationVectors` | The handler addresses the CPU reads in each mode. |
| `parseCartridgeHeader(rom)` | The header the image carries, or nothing when the image is too small to hold one. |
| `detectCartridgeMap(rom)` | The map alone. |
| `declaredSaveRamBytes(rom)` | The save-RAM size alone. |
| `CartridgeRegion` | `System`, `WorkRam`, `Rom`, `SaveRam` or `Unmapped` — what a bus address reaches. |
| `cartridgeRegion(map, address)` | The region an address lands in under a map. |
| `romOffset(map, address, imageBytes)` | The image byte a ROM address reads, mirrored across the image. |
| `romAddress(map, offset)` | The bus address that reads an image offset whole. |
| `saveRamOffset(map, address)` | The offset into the save an address reaches, before the save's size reduces it. |

## The header

Every cartridge carries a 32-byte header followed by the 32 bytes of interrupt vectors, and the CPU
reads that 64-byte block at `$00:FFC0-$00:FFFF`. Where it sits in the *image* depends on the map:
LoROM puts it at the end of the first 32 KB window, `$7FC0`; HiROM at the end of the first 64 KB
bank, `$FFC0`; ExHiROM at the end of the first bank of its second 4 MB, `$40FFC0`.

`parseCartridgeHeader` scores the three sites against each other and reads the one that reads best as
a header: a checksum agreeing with its complement is the strongest sign, a map-mode byte naming the
site it sits in is next, and a title that reads as text breaks the remaining ties. When two sites read
equally well the map that addresses more of the image wins, and between LoROM and HiROM, LoROM. The
bytes at the winning site are reported as they are, so a header whose checksum does not agree, or
whose title is not text, still comes back — `checksumAgrees` and `title` are how a caller judges it.

```cpp
#include "snaggletooth/snes/cartridge.h"
using namespace snaggletooth;

std::vector<std::uint8_t> image = /* a cartridge image, copier header removed */;
const std::optional<CartridgeHeader> header = parseCartridgeHeader(image);
if (!header) { /* too small to be a cartridge at all */ }

header->map;              // CartridgeMap::HiRom — the site the header was read from
header->title;            // "EARTHWORM JIM" — 21 bytes, trailing spaces and zero bytes removed
header->mapMode;          // 0x31 — the byte as written
header->fastRom;          // true — bit 4 of the map-mode byte
header->saveRamBytes;     // 0, or the bytes the size code means
header->checksumAgrees;   // true when complement and checksum are each other's inverse
header->emulation.reset;  // where the CPU starts
```

`map` is the map of the site the header was found at, which is what the machine lays the image out
under. `mapMode` is the byte as written; its low nibble names a map and bit 4 says the cartridge runs
at the fast rate in banks `$80-$FF`, but a header at the LoROM site that claims another map is still
a LoROM cartridge, and `map` says so.

The save-RAM size code is 1 KB shifted left by the code: `$01` is 2 KB, `$03` is 8 KB, `$05` is
32 KB. A code of zero declares no save; a code beyond `$0F` is not a size and is read as none; a size
past 128 KB is clamped to it, the most a cartridge can address.

**The vectors.** `native` holds the handlers the CPU reads in native mode from `$FFE4-$FFEF` — COP,
BRK, ABORT, NMI and IRQ — and `emulation` those it reads in emulation mode from `$FFF4-$FFFF` — COP,
ABORT, NMI, RESET and IRQ, the last shared with BRK. Each is a 16-bit address in bank `$00`. The CPU
powers on in emulation mode, so `emulation.reset` is where every cartridge starts.

## The three maps

**LoROM** gives each bank its upper 32 KB and lays those windows end to end: `$00:8000-$00:FFFF` is
the first 32 KB of the image, `$01:8000-$01:FFFF` the next, up to 4 MB. A bank's high bit only selects
the memory speed, so `$80:8000` reads the same byte as `$00:8000`. The lower halves of the cartridge
banks `$40-$7D` and `$C0-$FF` reach nothing.

**HiROM** gives each of the cartridge banks `$40-$7D` and `$C0-$FF` the whole 64 KB and lays those end
to end, up to 4 MB, and a system bank's upper half reaches the same bytes as the matching cartridge
bank: `$00:8000` is the byte at `$C0:8000`.

**ExHiROM** is HiROM with a second 4 MB. Banks `$80-$FF` serve the first 4 MB exactly as HiROM does;
banks `$00-$7D` serve the second, so `$40:0000` is image offset `$400000` and `$00:8000` its mirror at
`$408000`. Banks `$7E-$7F` are work RAM under every map, so the second 4 MB's last two banks are
reachable only through `$3E-$3F`, and only their upper halves.

**An image repeats across the window it does not fill.** A cartridge carries one ROM chip per power
of two in its size, wired one after another, and the board leaves the address lines above a chip
undecoded — so an address past a chip reads that chip again rather than running into the next one. A
512 KB image is one chip and repeats whole. A 3 MB image is a 2 MB chip and a 1 MB chip, and the
megabyte above it repeats **the second** chip, not the image. `romOffset` applies the rule, which is
why it takes the image size.

## Where an address lands

```cpp
const std::size_t size = image.size();
romOffset(CartridgeMap::HiRom, 0xC11234, size);   // 0x011234
romOffset(CartridgeMap::HiRom, 0x008000, size);   // 0x008000 — the system bank's upper half
romOffset(CartridgeMap::HiRom, 0x001000, size);   // nothing: that is the system area
romOffset(CartridgeMap::LoRom, 0x018000, size);   // 0x008000
romOffset(CartridgeMap::LoRom, 0x400000, size);   // nothing: a LoROM lower half
romOffset(CartridgeMap::ExHiRom, 0x401000, size); // 0x401000 — the second 4 MB

romAddress(CartridgeMap::HiRom, 0x123456);        // $D2:3456
romAddress(CartridgeMap::LoRom, 0x008000);        // $01:8000
romAddress(CartridgeMap::ExHiRom, 0x7E0000);      // nothing: no address reads it
```

`romOffset` answers for any 24-bit address and returns nothing when the address is not ROM under the
map, so it can be asked about an address before knowing what is there. `romAddress` goes the other
way and picks the banks that carry the image without a gap: `$00-$7D` and `$FE-$FF` under LoROM,
`$C0-$FF` under HiROM, and under ExHiROM `$C0-$FF` for the first 4 MB then `$40-$7D`. It returns
nothing for an offset the map cannot reach — beyond 4 MB under LoROM and HiROM, beyond 8 MB under
ExHiROM, or under ExHiROM the lower halves of the two banks work RAM hides.

`cartridgeRegion` names what an address reaches:

| Region | Where |
|---|---|
| `WorkRam` | banks `$7E-$7F` |
| `System` | the lower half of a system bank (`$00-$3F`, `$80-$BF`): the work-RAM mirror, the registers, the expansion area |
| `Rom` | the upper half of every bank; the whole of a cartridge bank under HiROM and ExHiROM |
| `SaveRam` | the save window, whether or not the cartridge declares a save |
| `Unmapped` | a LoROM cartridge bank's lower half outside the save window |

## Save RAM

Each map keeps the save in its own window. LoROM banks it above the cartridge banks, in the lower
halves of `$70-$7D` and `$F0-$FD`, 32 KB per bank. HiROM fits it into the system banks' expansion
window, `$20-$3F` and `$A0-$BF` at `$6000-$7FFF`, 8 KB per bank. ExHiROM keeps it in `$80-$BF` at
`$6000-$7FFF`.

`saveRamOffset` gives the linear offset into the save an address reaches, before the save's own size
folds it — `$71:1234` under LoROM is offset `$9234`, `$21:6000` under HiROM is `$2000`. A save
smaller than its window repeats within it: the machine reduces the offset to the declared size, and a
caller holding a save does the same.

```cpp
saveRamOffset(CartridgeMap::LoRom, 0x711234);   // 0x9234
saveRamOffset(CartridgeMap::HiRom, 0x216000);   // 0x2000
saveRamOffset(CartridgeMap::HiRom, 0x205FFF);   // nothing: below the window
```

## Gotchas

- Remove a copier header first. Some dumps carry 512 bytes ahead of the image; an image whose length
  is 512 past a multiple of 1 KB has one, and every site is off by 512 until it is dropped.
- `map` comes from the site, not the map-mode byte. A header that claims one map from another map's
  site is reported with the site's map and the byte as written.
- `detectCartridgeMap` and `declaredSaveRamBytes` answer for any image, even one too small to hold a
  header: LoROM and zero. `parseCartridgeHeader` returns nothing for that image.
- A vector of `$0000` points at work RAM and one of `$FFFF` at the last ROM byte; neither is a
  handler. A cartridge leaves a vector it does not use at either.
- `cartridgeRegion` reports the save window for every map whether or not the cartridge declares a
  save; the machine leaves an undeclared save reading open bus.

## See also

- [The SNES machine](snes-machine.md) — the bus that lays a cartridge out under its map.
- [Disassembly framework](disassembly-framework.md) — the entry points a header names for a trace.
