# The cartridge

`snaggletooth/snes/cartridge.h` reads a cartridge image as a value: its header, the board it is on —
the map it lays across the bus under, the chip beside the ROM, the save — and where each bus address
lands in the image. The [machine](snes-machine.md) reads a cartridge through these functions, and so
does every tool built over it, so the two can never disagree about where a byte is.

Nothing here runs anything. A header is parsed from a span of bytes; a board translates between a
24-bit bus address and an image offset in both directions; a region says what an address reaches.
Building a machine is the caller's next step, or not — a disassembler wants the header's vectors and
the board and never a machine at all.

## Contents

- [Surface](#surface)
- [A copier's header](#a-copiers-header)
- [The header](#the-header)
- [The three maps](#the-three-maps)
- [The board](#the-board)
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
| `Copier` | `SuperWildCard`, `ProFighter`, `GameDoctor`, `SuperUfo`, or `Unnamed` for a header no copier signed. |
| `CopierHeader` | What a copier's header declares: the copier, the ROM size, the mappings, the save, the file type, the entry, whether further files follow. |
| `kCopierHeaderBytes` | 512 — every copier's header is this long. |
| `readCopierHeader(file)` | The copier's header a file carries ahead of its image, or nothing when the file begins with the image. |
| `describeCopierHeader(header, imageBytes)` | One line saying what the header declares, and how the image that follows differs from it. |
| `CartridgeBoard` | The board a cartridge is on: its map, its `Coprocessor`, and its save in bytes — zero for none. |
| `cartridgeBoard(rom)` | The board an image's header declares; a plain LoROM board with no save for an image with no header. |
| `CartridgeRegion` | `System`, `WorkRam`, `Rom`, `SaveRam` or `Coprocessor` — what a bus address reaches. |
| `cartridgeRegion(board, address)` | The region an address lands in on a board. |
| `romOffset(board, address, imageBytes)` | The image byte a ROM address reads on a board, mirrored across the image. |
| `romAddress(map, offset)` | The bus address that reads an image offset whole. |
| `saveRamOffset(board, address)` | The offset into the save an address reaches on a board, before the save's size reduces it. |

## A copier's header

A dump that came through a copier — a device of the cartridge era that read a cartridge into a file,
or ran a file in a cartridge's place — carries 512 bytes of the copier's own ahead of the image, written
so the device could load the file again. The console never sees them, and every header site is 512
bytes off until they are dropped. `readCopierHeader` reads a file's first 512 bytes as the copier that
wrote them laid them out, and names the copier by its own bytes: the Super Wild Card by the file ID
`$AA $BB` at bytes 8–9, the Game Doctor by the sixteen-byte ID it opens with, the Super UFO by
`SUPERUFO` at bytes 8–15, the Pro Fighter by one of its three mode words at bytes 4–5 (`$8377` ROM,
`$8347` ROM and a DSP-1, `$82FD` ROM, a DSP-1 and save RAM) under a ROM-mode byte at 3 of `$00` or
`$80`. A file none of them signed carries an unnamed header when its length is 512 past a multiple of
1 KB, the size every headerless dump has; otherwise the file begins with the image.

```cpp
std::vector<std::uint8_t> file = /* the dump, as read from disk */;
if (const std::optional<CopierHeader> copier = readCopierHeader(file)) {
  file.erase(file.begin(), file.begin() + kCopierHeaderBytes);
  describeCopierHeader(*copier, file.size());
  // "a Super Wild Card header: 512 KB, HiROM, 8 KB of save RAM mapped HiROM, a program;
  //  the image is 3584 bytes longer than declared"
}
const std::optional<CartridgeHeader> header = parseCartridgeHeader(file);
```

Every field the copier's layout documents is read and reported as written. `declaredRomBytes` is the
ROM size the Super Wild Card, the Pro Fighter and an unnamed header write at bytes 0–1 in 8 KB
units. The Super Wild Card's mode byte gives `programMapping` and `saveMapping` (LoROM or HiROM),
`saveRamBytes` (32 KB, 8 KB, 2 KB or none), `jumpEntry` (the file starts at `$8000` rather than the
reset vector) and `multiFile` (further files follow); `fileType` is its byte 10 — `$04` a program, `$05`
a battery save, `$08` a real-time save. The Pro Fighter's `modeWord` is reported as written with `dsp1`
and `hasSaveRam` what it names, its ROM-mode byte gives `programMapping`, and its own multi-file byte
sets `multiFile`. The Game Doctor and the Super UFO carry an ID and nothing this reads. The declared
size is a fact about the copier, not about the image: a dump a translation patch grew, or one padded
after the image, disagrees with it, so `describeCopierHeader` says by how much and the image that is
there is the one read.

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

std::vector<std::uint8_t> image = /* a cartridge image, any copier's header dropped */;
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
the memory speed, so `$80:8000` reads the same byte as `$00:8000`. A LoROM board leaves the
cartridge's A15 unconnected, so the lower half of a cartridge bank — `$40-$7D` and `$C0-$FF` below
`$8000` — reads the bytes its upper half reads: `$40:1234` is the byte at `$40:9234`. The save window
is the exception, and takes the lower halves of its own banks (see [Save RAM](#save-ram)). A system
bank's lower half is the console's and reaches no cartridge.

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

## The board

The map says how the image lays across the bus; what an address reaches also depends on what else is
on the board — whether there is a save behind the map's window, and whether a coprocessor sits
beside the ROM. `CartridgeBoard` holds the three, and `cartridgeBoard` reads them from the header:
the map from the site the header is read at, the chip from the chipset byte, the save from its size
code. An image too small to hold a header is a plain LoROM board with no save.

```cpp
const CartridgeBoard board = cartridgeBoard(image);
board.map;           // CartridgeMap::LoRom
board.coprocessor;   // Coprocessor::Dsp
board.saveRamBytes;  // 8192, or 0 for a cartridge with none

// A board of the caller's own, for a cartridge whose header says the wrong thing.
const CartridgeBoard plain{.map = CartridgeMap::HiRom, .coprocessor = Coprocessor::None, .saveRamBytes = 0};
```

The board answers two questions the map cannot. **A save window with no save behind it** is whatever
the board decodes there: LoROM's window sits in cartridge banks, where a board with no save decodes
nothing, so the window's lower halves repeat their upper halves as every other cartridge bank's does
— `$70:1234` reads the byte at `$70:9234`; HiROM's and ExHiROM's windows sit in the expansion area,
where nothing else is, and read open bus. **A coprocessor's LoROM board** gives the lower halves of
its cartridge banks to the chip — `$60-$6F` to a DSP on the 2 MB boards, and to an ST010's ports and
RAM — so every cartridge bank's lower half outside the save window is the chip's, and the window's
lower halves are the chip's too when the board has no save. A coprocessor on a HiROM or ExHiROM
board changes nothing here: the chips' own maps on those boards are not modelled. The [machine](snes-machine.md#save-ram)
reads both rules through these functions.

## Where an address lands

```cpp
const std::size_t size = image.size();
const CartridgeBoard hiRom{.map = CartridgeMap::HiRom, .coprocessor = Coprocessor::None, .saveRamBytes = 8192};
const CartridgeBoard loRom{.map = CartridgeMap::LoRom, .coprocessor = Coprocessor::None, .saveRamBytes = 8192};
const CartridgeBoard bare{.map = CartridgeMap::LoRom, .coprocessor = Coprocessor::None, .saveRamBytes = 0};
const CartridgeBoard dsp{.map = CartridgeMap::LoRom, .coprocessor = Coprocessor::Dsp, .saveRamBytes = 0};
const CartridgeBoard exHiRom{.map = CartridgeMap::ExHiRom, .coprocessor = Coprocessor::None, .saveRamBytes = 8192};

romOffset(hiRom, 0xC11234, size);    // 0x011234
romOffset(hiRom, 0x008000, size);    // 0x008000 — the system bank's upper half
romOffset(hiRom, 0x001000, size);    // nothing: that is the system area
romOffset(loRom, 0x018000, size);    // 0x008000
romOffset(loRom, 0x401234, size);    // 0x201234 — a lower half, as $40:9234
romOffset(loRom, 0x701234, size);    // nothing: the save is behind the window
romOffset(bare, 0x701234, size);     // 0x381234 — no save, so the window reads as $70:9234
romOffset(dsp, 0x601234, size);      // nothing: the chip's half
romOffset(exHiRom, 0x401000, size);  // 0x401000 — the second 4 MB

romAddress(CartridgeMap::HiRom, 0x123456);        // $D2:3456
romAddress(CartridgeMap::LoRom, 0x008000);        // $01:8000
romAddress(CartridgeMap::ExHiRom, 0x7E0000);      // nothing: no address reads it
```

`romOffset` answers for any 24-bit address and returns nothing when the address is not the image on
the board, so it can be asked about an address before knowing what is there. `romAddress` goes the
other way, is the map's alone, and picks the banks that carry the image without a gap: `$00-$7D` and
`$FE-$FF` under LoROM, `$C0-$FF` under HiROM, and under ExHiROM `$C0-$FF` for the first 4 MB then
`$40-$7D`. It returns nothing for an offset the map cannot reach — beyond 4 MB under LoROM and HiROM,
beyond 8 MB under ExHiROM, or under ExHiROM the lower halves of the two banks work RAM hides.

`cartridgeRegion` names what an address reaches on the board:

| Region | Where |
|---|---|
| `WorkRam` | banks `$7E-$7F` |
| `System` | the lower half of a system bank (`$00-$3F`, `$80-$BF`): the work-RAM mirror, the registers, the expansion area — HiROM's and ExHiROM's save windows among it on a board with no save |
| `Rom` | the upper half of every bank; the whole of a cartridge bank under HiROM and ExHiROM; under LoROM a cartridge bank's lower half on a plain board, the save window's included when the board has no save |
| `SaveRam` | the save window, on a board with a save |
| `Coprocessor` | under LoROM on a coprocessor's board, a cartridge bank's lower half outside the save window — and the window's lower halves too when the board has no save |

## Save RAM

Each map keeps the save in its own window. LoROM banks it above the cartridge banks, in the lower
halves of `$70-$7D` and `$F0-$FF`, 32 KB per bank. HiROM fits it into the system banks' expansion
window, `$20-$3F` and `$A0-$BF` at `$6000-$7FFF`, 8 KB per bank. ExHiROM keeps it in `$80-$BF` at
`$6000-$7FFF`.

The window is the save's only on a board with a save: `cartridgeRegion` answers `SaveRam` there and
`romOffset` answers nothing. On a board with none the window is whatever the board decodes there —
the image through the upper half on a plain LoROM board, the chip's half on a coprocessor's, open bus
in HiROM's and ExHiROM's expansion area — as [the board](#the-board) says.

`saveRamOffset` gives the linear offset into the save an address reaches, before the save's own size
folds it — `$71:1234` under LoROM is offset `$9234`, `$21:6000` under HiROM is `$2000` — and nothing
outside the window or on a board with no save. A save smaller than its window repeats within it: the
machine reduces the offset to the declared size, and a caller holding a save does the same.

```cpp
saveRamOffset(loRom, 0x711234);   // 0x9234
saveRamOffset(hiRom, 0x216000);   // 0x2000
saveRamOffset(hiRom, 0x205FFF);   // nothing: below the window
saveRamOffset(bare, 0x711234);    // nothing: no save on the board
```

## Gotchas

- A copier's header is the file's, not the cartridge's. Drop it through `readCopierHeader` before
  reading anything else; every site is 512 bytes off while it is there. Two shapes it cannot tell
  apart: a headerless dump padded to a length 512 past a kilobyte reads as carrying an unnamed
  header, and a copier no layout names, on a padded dump, is not seen at all.
- `map` comes from the site, not the map-mode byte. A header that claims one map from another map's
  site is reported with the site's map and the byte as written.
- `detectCartridgeMap` and `declaredSaveRamBytes` answer for any image, even one too small to hold a
  header: LoROM and zero. `parseCartridgeHeader` returns nothing for that image.
- A vector of `$0000` points at work RAM and one of `$FFFF` at the last ROM byte; neither is a
  handler. A cartridge leaves a vector it does not use at either.
- The three functions answer for the board they are given. A board with `saveRamBytes` of zero reads
  its LoROM window as the image and its HiROM or ExHiROM window as the system's; one with a
  coprocessor gives its LoROM lower halves to the chip. A caller that wants the map's answer alone —
  every window the save's, every lower half the image — asks with a board that declares a save and no
  chip.
- `cartridgeBoard` reads the chip from the chipset byte as `parseCartridgeHeader` does, so a chipset
  nibble the layout does not list is `Coprocessor::Unknown`, and a board with one keeps its lower
  halves for the chip as any coprocessor's board does.
- The boards differ in ways the image does not say, and `CartridgeBoard` takes none of them: the older
  LoROM boards give the save the whole 64 KB of its banks, one HiROM family keeps the save in `$10-$1F`
  as well as `$30-$3F`, and one leaves a quarter of its ROM banks empty. The header names no board
  beyond its map, its chip and its save.
- `chipsetSubtype` is read from `$FFBF` on every cartridge, because a custom coprocessor needs it,
  but it means something only under a chipset high nibble of `$F` or an extended header. On a
  cartridge with neither it is whatever byte the bank holds there, often `$FF` or `$00`.
- The map-mode byte's low nibble names more than the three maps: `$2` is LoROM with an S-DD1, `$3`
  LoROM with an SA-1, `$A` HiROM with an SPC7110. `map` still comes from the site, so a cartridge with
  one of those bytes lays out under the map its site names.

## See also

- [The SNES machine](snes-machine.md) — the bus that lays a cartridge out under its map.
- [Disassembly framework](disassembly-framework.md) — the entry points a header names for a trace.
