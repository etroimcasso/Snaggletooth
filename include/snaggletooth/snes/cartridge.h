#pragma once

// A cartridge as a value: its header, the board it is on, and where each bus
// address lands.
//
// Every cartridge carries a header at a fixed place in its image, and the
// header's own layout is the same under every map. This file reads it, decides
// which map the image uses, reads the board — the map, the chip beside the ROM
// and the save — and translates between a bus address and an image offset both
// ways. The machine and the tools built over it both read a cartridge through
// these functions, so they can never disagree about where a byte is.
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

// The copier that wrote a dump's own file header. A copier was a device that
// read a cartridge into a file, or ran a file in the cartridge's place, and it
// wrote 512 bytes of its own ahead of the image so it could load the file
// again. The console never sees those bytes. `Unnamed` is a header no copier
// signed, known only by the file's length.
enum class Copier : std::uint8_t { SuperWildCard, ProFighter, GameDoctor, SuperUfo, Unnamed };

// Every copier's header is this long, and the image starts right after it.
constexpr std::size_t kCopierHeaderBytes = 512u;

// What a copier's header says, read as its layout lays it out. Which fields are
// written depends on the copier: the Super Wild Card and the Pro Fighter declare
// the ROM size and the mapping; the Game Doctor and the Super UFO carry an ID
// and nothing this reads. A field a copier does not write keeps its default.
struct CopierHeader {
  Copier copier = Copier::Unnamed;
  std::size_t declaredRomBytes = 0;   // bytes 0-1 in 8 KB units: the Super Wild Card, the Pro Fighter, and an unnamed header, which shares the site
  std::optional<CartridgeMap> programMapping;  // LoROM or HiROM: the Super Wild Card's mode bit 4, the Pro Fighter's byte 3
  std::optional<CartridgeMap> saveMapping;     // the Super Wild Card's mode bit 5
  std::optional<std::size_t> saveRamBytes;     // the Super Wild Card's mode bits 3-2: 32 KB, 8 KB, 2 KB or none
  bool jumpEntry = false;    // the Super Wild Card's mode bit 7: start at $8000 rather than the reset vector
  bool multiFile = false;    // further files follow: the Super Wild Card's mode bit 6, the Pro Fighter's byte 2
  std::uint8_t fileType = 0; // the Super Wild Card's byte 10 as written: $04 a program, $05 a battery save, $08 a real-time save
  std::uint16_t modeWord = 0;  // the Pro Fighter's bytes 4-5 as written: $8377 ROM, $8347 ROM and a DSP-1, $82FD ROM, a DSP-1 and save RAM
  bool dsp1 = false;         // the Pro Fighter's mode word names a DSP-1
  bool hasSaveRam = false;   // the Pro Fighter's mode word names save RAM
};

// The copier header a file carries ahead of its image, or nothing when the file
// begins with the image. A copier is named by its own bytes: the Super Wild Card
// by its file ID at bytes 8-9, the Game Doctor by the sixteen-byte ID it opens
// with, the Super UFO by the ID at bytes 8-15, the Pro Fighter by one of its
// three mode words at bytes 4-5 under a ROM-mode byte at 3 of $00 or $80. A
// file none of them signed carries an unnamed header when its length is 512
// past a multiple of 1 KB, the size every headerless dump has. A file shorter
// than a header carries none.
[[nodiscard]] std::optional<CopierHeader> readCopierHeader(std::span<const std::uint8_t> file) noexcept;

// One line saying what the header is and declares, for a tool's report: the
// copier, the ROM size it declares, the mappings and save it names, and, when
// the image that follows is not the size declared, the difference.
[[nodiscard]] std::string describeCopierHeader(const CopierHeader& header, std::size_t imageBytes);

// The bytes of save RAM a cartridge header declares, from its size code: zero for
// a cartridge with none, otherwise 1 KB shifted by the code. Sizes beyond what a
// cartridge can address are clamped to 128 KB.
[[nodiscard]] std::size_t declaredSaveRamBytes(std::span<const std::uint8_t> rom) noexcept;

// The board a cartridge is on, as its header declares it: the map the image lays
// across the bus under, the chip beside the ROM, and the save. What a bus
// address reaches depends on all three — a LoROM board with no save reads the
// image through its save window, and a coprocessor's board gives lower halves
// of its cartridge banks to the chip — so the functions below take the board.
struct CartridgeBoard {
  CartridgeMap map = CartridgeMap::LoRom;
  Coprocessor coprocessor = Coprocessor::None;
  std::size_t saveRamBytes = 0;  // zero for a cartridge with no save
};

// The board an image's header declares: the map from the site the header is
// read at, the coprocessor from the chipset byte, and the save from its size
// code, each as `detectCartridgeMap`, `parseCartridgeHeader` and
// `declaredSaveRamBytes` read it. An image with no header is a plain LoROM board
// with no save.
[[nodiscard]] CartridgeBoard cartridgeBoard(std::span<const std::uint8_t> rom) noexcept;

// What a bus address reaches on a board.
enum class CartridgeRegion : std::uint8_t {
  System,       // the lower half of a system bank: work-RAM mirror, registers, expansion
  WorkRam,      // banks $7E-$7F
  Rom,          // the cartridge image
  SaveRam,      // the cartridge's save, in the map's window
  Coprocessor,  // the lower half of a LoROM cartridge bank a coprocessor's board gives to the chip
};

// The region a 24-bit bus address lands in on `board`.
//
// Under LoROM the lower half of a cartridge bank — $40-$7D and $C0-$FF below
// $8000 — is the image on a plain board, reaching the bytes the bank's upper half
// reaches, because the board leaves the cartridge's A15 unconnected. On a
// coprocessor's board every such half outside the save window is the chip's,
// `Coprocessor`.
//
// The save window is `SaveRam` only on a board with a save. With none, what the
// window answers is the board's: LoROM's window sits in cartridge banks, so its
// lower halves are `Rom` on a plain board and the chip's on a coprocessor's;
// HiROM's and ExHiROM's sit in the expansion area and are `System`, which reads
// open bus. A coprocessor on a HiROM or ExHiROM board changes nothing here: the
// chips' own maps on those boards are not modelled.
[[nodiscard]] CartridgeRegion cartridgeRegion(const CartridgeBoard& board,
                                              std::uint32_t address) noexcept;

// The image byte a ROM address reads on `board`, for an image of `imageBytes`.
// Nothing when the address is not `Rom` on the board — a save window with a
// save behind it, a chip's half — or the image is empty. Through a LoROM save
// window with no save it is the upper half's byte. An address past the image
// repeats it the way the board does: a cartridge carries one chip per power of
// two in its size, wired one after another, and an address past a chip reads
// that chip again rather than running into the next one.
[[nodiscard]] std::optional<std::size_t> romOffset(const CartridgeBoard& board,
                                                   std::uint32_t address,
                                                   std::size_t imageBytes) noexcept;

// The bus address that reads image offset `offset` whole under the map, in the
// banks that carry the image without a gap: $00-$7D and $FE-$FF under LoROM,
// $C0-$FF under HiROM, and under ExHiROM $C0-$FF for the first 4 MB then $40-$7D.
// Nothing when no address reads that offset — an offset beyond what the map can
// address, or one that lands where the console keeps its work RAM.
[[nodiscard]] std::optional<std::uint32_t> romAddress(CartridgeMap map,
                                                      std::size_t offset) noexcept;

// The offset into the save an address reaches on `board`, before it is reduced
// to the save's size; nothing when the address is outside the save window, or
// the board has no save. LoROM keeps the save in the lower halves of banks
// $70-$7D and $F0-$FF; HiROM in $20-$3F and $A0-$BF at $6000-$7FFF; ExHiROM in
// $80-$BF at $6000-$7FFF.
[[nodiscard]] std::optional<std::size_t> saveRamOffset(const CartridgeBoard& board,
                                                       std::uint32_t address) noexcept;

}  // namespace snaggletooth
