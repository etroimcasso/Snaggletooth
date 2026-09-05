#pragma once

// A cartridge as a value: its header, how its image lays across the bus, and
// where each bus address lands.
//
// Every cartridge carries a header at a fixed place in its image, and the
// header's own layout is the same under every map. This file reads it, decides
// which map the image uses, and translates between a bus address and an image
// offset both ways. The machine and the tools built over it both read a
// cartridge through these functions, so they can never disagree about where a
// byte is.
//
// Three maps exist. LoROM gives each bank a 32 KB window in its upper half and
// lays those windows end to end. HiROM gives each cartridge bank the whole 64 KB
// and lays those end to end, reaching the same bytes through the system banks'
// upper halves. ExHiROM is HiROM with a second 4 MB: banks $80-$FF serve the
// first 4 MB exactly as HiROM does, and banks $00-$7D serve the second.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace snaggletooth {

// How a cartridge lays its image across the bus.
enum class CartridgeMap : std::uint8_t { LoRom, HiRom, ExHiRom };

// The interrupt vectors the CPU reads in native mode, from $00:FFE4-$00:FFEF.
// Each is the 16-bit address of a handler in bank $00.
struct NativeVectors {
  std::uint16_t cop = 0;    // $FFE4
  std::uint16_t brk = 0;    // $FFE6
  std::uint16_t abort = 0;  // $FFE8
  std::uint16_t nmi = 0;    // $FFEA
  std::uint16_t irq = 0;    // $FFEE
};

// The interrupt vectors the CPU reads in emulation mode, from $00:FFF4-$00:FFFF.
// The CPU powers on in emulation mode, so `reset` is where every cartridge starts.
struct EmulationVectors {
  std::uint16_t cop = 0;    // $FFF4
  std::uint16_t abort = 0;  // $FFF8
  std::uint16_t nmi = 0;    // $FFFA
  std::uint16_t reset = 0;  // $FFFC
  std::uint16_t irq = 0;    // $FFFE, shared with BRK
};

// The coprocessor a cartridge's chipset byte names. The byte's high nibble picks
// one of the first seven; `$F` names a custom chip that the sub-type byte at
// $FFBF tells apart. `Unknown` is a code the header layout does not list.
enum class Coprocessor : std::uint8_t {
  None,
  Dsp,      // DSP-1 to DSP-4
  Gsu,      // the SuperFX family
  Obc1,
  Sa1,
  Sdd1,
  Srtc,
  Other,    // the Super Game Boy and the Satellaview BIOS
  Spc7110,  // custom, sub-type $00
  St010,    // custom, sub-type $01: ST010 and ST011
  St018,    // custom, sub-type $02
  Cx4,      // custom, sub-type $10
  Unknown,
};

// The video standard a cartridge's country byte implies: 60 Hz or 50 Hz.
enum class VideoStandard : std::uint8_t { Ntsc, Pal, Unknown };

// The sixteen bytes ahead of the header, at $FFB0-$FFBE, which a cartridge
// carries when its developer byte is $33.
struct ExtendedHeader {
  std::string makerCode;                    // two characters at $FFB0
  std::string gameCode;                     // four characters at $FFB2, trailing spaces removed
  std::uint8_t expansionFlashSizeCode = 0;  // $FFBC, as written
  std::uint8_t expansionRamSizeCode = 0;    // $FFBD, as written
  std::size_t expansionRamBytes = 0;        // the bytes that code declares, by the save-RAM rule
  std::uint8_t specialVersion = 0;          // $FFBE, as written
};

// What a cartridge header says, read from the site its map puts it at.
struct CartridgeHeader {
  std::size_t offset = 0;          // where in the image the header sits
  CartridgeMap map = CartridgeMap::LoRom;  // the map of the site the header was found at
  std::string title;               // the 21-byte title, trailing spaces and zero bytes removed
  std::uint8_t mapMode = 0;        // the map-mode byte as written
  bool fastRom = false;            // bit 4 of the map-mode byte: the cartridge runs at the fast rate in $80-$FF
  std::uint8_t chipset = 0;        // the chipset byte as written
  Coprocessor coprocessor = Coprocessor::None;  // what the chipset byte names
  bool hasRam = false;             // the chipset byte says the cartridge carries RAM
  bool hasBattery = false;         // and a battery
  bool hasClock = false;           // and a real-time clock beside its coprocessor
  std::uint8_t chipsetSubtype = 0; // the byte at $FFBF as written; tells custom coprocessors apart
  std::uint8_t romSizeCode = 0;    // the ROM size code as written
  std::size_t romSizeBytes = 0;    // the bytes that code declares
  std::uint8_t saveSizeCode = 0;   // the save-RAM size code as written
  std::size_t saveRamBytes = 0;    // the bytes of save RAM that code declares
  std::uint8_t country = 0;        // the country byte as written
  VideoStandard video = VideoStandard::Unknown;  // what that country runs at
  std::uint8_t developer = 0;      // the developer byte as written; $33 says an extended header follows
  std::uint8_t version = 0;        // the version byte as written
  std::uint16_t complement = 0;    // the checksum's complement
  std::uint16_t checksum = 0;      // the checksum
  bool checksumAgrees = false;     // whether the two are each other's complement
  std::optional<ExtendedHeader> extended;  // present only when the developer byte is $33
  NativeVectors native;
  EmulationVectors emulation;
};

// The header the image carries, read from the site that reads best as one. Each
// map puts the header at its own site — LoROM at $7FC0, HiROM at $FFC0, ExHiROM
// at $40FFC0 — and the sites are scored against each other: a header whose
// checksum agrees with its complement, whose map-mode byte names the site it sits
// in, and whose title reads as text is the real one. When two sites read equally
// well, the one addressing more of the image wins, and between LoROM and HiROM,
// LoROM. The bytes at the winning site are reported as they are, so
// `checksumAgrees` and `title` say how much to trust them; only an image too
// small to hold a header at any site has none.
//
// The chipset byte is read as its layout says: the low nibble tells what sits
// beside the ROM ($0 nothing, $1 RAM, $2 RAM and a battery, $3 a coprocessor,
// $4 with RAM, $5 with RAM and a battery, $6 with a battery, $9 with RAM, a
// battery and a clock, $A as $5) and the high nibble which coprocessor. A low
// nibble of $2 under a nonzero high nibble reads as $5. A nibble the layout does
// not list reports `Coprocessor::Unknown` and no RAM, battery or clock. The two
// size codes are 1 KB shifted left by the code: a code past $0F is not a size
// and reads as zero, and a ROM size past 8 MB is clamped to it.
[[nodiscard]] std::optional<CartridgeHeader> parseCartridgeHeader(
    std::span<const std::uint8_t> rom);

// Which map a cartridge image uses, read from its header. An image with no header
// is reported as LoROM, the denser layout.
[[nodiscard]] CartridgeMap detectCartridgeMap(std::span<const std::uint8_t> rom) noexcept;

// The bytes of save RAM a cartridge header declares, from its size code: zero for
// a cartridge with none, otherwise 1 KB shifted by the code. Sizes beyond what a
// cartridge can address are clamped to 128 KB.
[[nodiscard]] std::size_t declaredSaveRamBytes(std::span<const std::uint8_t> rom) noexcept;

// What a bus address reaches under a map.
enum class CartridgeRegion : std::uint8_t {
  System,   // the lower half of a system bank: work-RAM mirror, registers, expansion
  WorkRam,  // banks $7E-$7F
  Rom,      // the cartridge image
  SaveRam,  // the cartridge's save window
  Unmapped, // nothing: a read answers open bus
};

// The region a 24-bit bus address lands in under `map`. The save window is
// reported whether or not the cartridge declares any save RAM; a cartridge that
// declares none leaves the window reading open bus.
[[nodiscard]] CartridgeRegion cartridgeRegion(CartridgeMap map, std::uint32_t address) noexcept;

// The image byte a ROM address reads, for an image of `imageBytes`. Nothing when
// the address is not ROM under the map or the image is empty. An address past the
// image repeats it the way the board does: a cartridge carries one chip per power
// of two in its size, wired one after another, and an address past a chip reads
// that chip again rather than running into the next one.
[[nodiscard]] std::optional<std::size_t> romOffset(CartridgeMap map, std::uint32_t address,
                                                   std::size_t imageBytes) noexcept;

// The bus address that reads image offset `offset` whole under the map, in the
// banks that carry the image without a gap: $00-$7D and $FE-$FF under LoROM,
// $C0-$FF under HiROM, and under ExHiROM $C0-$FF for the first 4 MB then $40-$7D.
// Nothing when no address reads that offset — an offset beyond what the map can
// address, or one that lands where the console keeps its work RAM.
[[nodiscard]] std::optional<std::uint32_t> romAddress(CartridgeMap map,
                                                      std::size_t offset) noexcept;

// The offset into the save an address reaches, before it is reduced to the save's
// size; nothing when the address is outside the save window. LoROM keeps the save
// in the lower halves of banks $70-$7D and $F0-$FD; HiROM in $20-$3F and $A0-$BF
// at $6000-$7FFF; ExHiROM in $80-$BF at $6000-$7FFF.
[[nodiscard]] std::optional<std::size_t> saveRamOffset(CartridgeMap map,
                                                       std::uint32_t address) noexcept;

}  // namespace snaggletooth
