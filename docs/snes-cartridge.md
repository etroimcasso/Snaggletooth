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
| `CartridgeHeader` | What a header says: the site it was read from, its map, the title, the map-mode byte and its fast bit, the chipset byte and what it names, the two size codes and the bytes they mean, the country byte and its video standard, the developer and version bytes, the checksum pair and whether they agree, the extended header when there is one, and the interrupt vectors. |
| `Coprocessor` | What the chipset byte names beside the ROM: `None`, `Dsp`, `Gsu`, `Obc1`, `Sa1`, `Sdd1`, `Srtc`, `Other`, `Spc7110`, `St010`, `St018`, `Cx4`, or `Unknown` for a code the layout does not list. |
| `VideoStandard` | `Ntsc`, `Pal` or `Unknown` — what the country byte implies. |
| `ExtendedHeader` | The sixteen bytes ahead of the header: the maker and game codes, the expansion size codes, the special version. |
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
header->title;            // the 21-byte title, trailing spaces and zero bytes removed
header->mapMode;          // 0x31 — the byte as written
header->fastRom;          // true — bit 4 of the map-mode byte
header->saveRamBytes;     // 0, or the bytes the size code means
header->checksumAgrees;   // true when complement and checksum are each other's inverse
header->emulation.reset;  // where the CPU starts

header->chipset;          // 0x35 — the chipset byte as written
header->coprocessor;      // Coprocessor::Sa1
header->hasRam;           // true
header->hasBattery;       // true
header->romSizeBytes;     // 4 MB, from a code of 0x0C
header->video;            // VideoStandard::Ntsc, from a country byte of 0x01
header->developer;        // 0x33 — an extended header follows
header->extended->gameCode;  // "ARWE"
```

`map` is the map of the site the header was found at, which is what the machine lays the image out
under. `mapMode` is the byte as written; its low nibble names a map and bit 4 says the cartridge runs
at the fast rate in banks `$80-$FF`, but a header at the LoROM site that claims another map is still
a LoROM cartridge, and `map` says so.

The save-RAM size code is 1 KB shifted left by the code: `$01` is 2 KB, `$03` is 8 KB, `$05` is
32 KB. A code of zero declares no save; a code beyond `$0F` is not a size and is read as none; a size
past 128 KB is clamped to it, the most a cartridge can address.

**The chipset byte** says what the board carries beside the ROM. Its low nibble names the memory and
whether a coprocessor is present — `$0` ROM alone, `$1` with RAM, `$2` with RAM and a battery, `$3` a
coprocessor, `$4` a coprocessor with RAM, `$5` with RAM and a battery, `$6` with a battery, `$9` with
RAM, a battery and a real-time clock, `$A` as `$5` — and its high nibble which coprocessor: `$0` a DSP,
`$1` the SuperFX family, `$2` the OBC1, `$3` the SA-1, `$4` the S-DD1, `$5` the S-RTC, `$E` the Super
Game Boy and Satellaview hardware, and `$F` a custom chip that the sub-type byte at `$FFBF` tells
apart: `$00` the SPC7110, `$01` the ST010 and ST011, `$02` the ST018, `$10` the Cx4. A low nibble of
`$2` under a nonzero high nibble is the `$5` form. `chipset` and `chipsetSubtype` are the bytes as
written; `coprocessor`, `hasRam`, `hasBattery` and `hasClock` are what they name, and a nibble the
layout does not list reports `Coprocessor::Unknown` with nothing beside the ROM.

**The ROM size code** is 1 KB shifted left by the code, read literally — `$08` is 256 KB, `$0C` is
4 MB, `$0D` is 8 MB — clamped to 8 MB, the most any map addresses; a code beyond `$0F` is not a size
and reads as zero. A cartridge whose chips do not add up to a power of two carries the next power up,
so a 3 MB image declares `$0C`, and `romSizeBytes` says what the header says, not what the image
measures.

**The country byte** is reported as written, and `video` is the standard it implies: `Ntsc` for the
60 Hz regions — Japan and the international code `$00`, the USA `$01`, South Korea `$0D`, Canada
`$0F`, Brazil `$10` — and `Pal` for the 50 Hz run from Europe `$02` through Indonesia `$0C` and for
Australia `$11`. The remaining codes name no standard and read as `Unknown`.

**The developer and version bytes** are reported as written. A developer byte of `$33` says the
sixteen bytes ahead of the header, `$FFB0-$FFBF`, are an extended header, and `extended` is then
present: `makerCode` is the two characters at `$FFB0`, `gameCode` the four at `$FFB2` with trailing
spaces removed (an older two-letter code is space padded), `expansionFlashSizeCode` and
`expansionRamSizeCode` the bytes at `$FFBC` and `$FFBD` with `expansionRamBytes` read by the save-RAM
rule, and `specialVersion` the byte at `$FFBE`. Under any other developer byte those bytes are
whatever the bank holds there, `extended` is absent, and only `chipsetSubtype` is read from them.

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
- `chipsetSubtype` is read from `$FFBF` on every cartridge, because a custom coprocessor needs it,
  but it means something only under a chipset high nibble of `$F` or an extended header. On a
  cartridge with neither it is whatever byte the bank holds there, often `$FF` or `$00`.
- The map-mode byte's low nibble names more than the three maps: `$2` is LoROM with an S-DD1, `$3`
  LoROM with an SA-1, `$A` HiROM with an SPC7110. `map` still comes from the site, so a cartridge with
  one of those bytes lays out under the map its site names.

## See also

- [The SNES machine](snes-machine.md) — the bus that lays a cartridge out under its map.
- [Disassembly framework](disassembly-framework.md) — the entry points a header names for a trace.
