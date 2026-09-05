#include "snaggletooth/snes/cartridge.h"

#include <bit>

namespace snaggletooth {
namespace {

// Where each map keeps the header in the image. Every site is $FFC0 in the bank
// whose upper half the console reads at $00:8000-$00:FFFF: the first 32 KB window
// under LoROM, the first 64 KB bank under HiROM, and the first bank of the second
// 4 MB under ExHiROM.
constexpr std::size_t kLoRomHeader = 0x7FC0u;
constexpr std::size_t kHiRomHeader = 0xFFC0u;
constexpr std::size_t kExHiRomHeader = 0x40FFC0u;

// The header's fields, as offsets from its site. The vectors follow the header
// in the same 64-byte span the console reads at $00:FFC0-$00:FFFF.
constexpr std::size_t kHeaderTitle = 0x00u;
constexpr std::size_t kHeaderTitleLength = 21u;
constexpr std::size_t kHeaderMapMode = 0x15u;
constexpr std::size_t kHeaderChipset = 0x16u;
constexpr std::size_t kHeaderRomSize = 0x17u;
constexpr std::size_t kHeaderSaveSize = 0x18u;
constexpr std::size_t kHeaderCountry = 0x19u;
constexpr std::size_t kHeaderDeveloper = 0x1Au;
constexpr std::size_t kHeaderVersion = 0x1Bu;
constexpr std::size_t kHeaderComplement = 0x1Cu;
constexpr std::size_t kHeaderChecksum = 0x1Eu;
constexpr std::size_t kHeaderBytes = 0x20u;
constexpr std::size_t kNativeCop = 0x24u;
constexpr std::size_t kNativeBrk = 0x26u;
constexpr std::size_t kNativeAbort = 0x28u;
constexpr std::size_t kNativeNmi = 0x2Au;
constexpr std::size_t kNativeIrq = 0x2Eu;
constexpr std::size_t kEmulationCop = 0x34u;
constexpr std::size_t kEmulationAbort = 0x38u;
constexpr std::size_t kEmulationNmi = 0x3Au;
constexpr std::size_t kEmulationReset = 0x3Cu;
constexpr std::size_t kEmulationIrq = 0x3Eu;
constexpr std::size_t kHeaderWithVectors = 0x40u;

// The extended header's fields, as offsets from its own site sixteen bytes
// ahead of the header, at $FFB0.
constexpr std::size_t kExtendedBytes = 0x10u;
constexpr std::size_t kExtendedMaker = 0x00u;
constexpr std::size_t kExtendedMakerLength = 2u;
constexpr std::size_t kExtendedGame = 0x02u;
constexpr std::size_t kExtendedGameLength = 4u;
constexpr std::size_t kExtendedFlashSize = 0x0Cu;
constexpr std::size_t kExtendedRamSize = 0x0Du;
constexpr std::size_t kExtendedSpecialVersion = 0x0Eu;
constexpr std::size_t kExtendedChipsetSubtype = 0x0Fu;

constexpr std::uint8_t kMapModeFast = 0x10u;
constexpr std::uint8_t kMapModeExHiRom = 0x05u;  // the low nibble that names ExHiROM
constexpr std::uint8_t kDeveloperExtended = 0x33u;  // the developer byte that says an extended header follows

// The largest save a cartridge can address through any window.
constexpr std::size_t kMaxSaveRamBytes = 128u * 1024u;

// The largest image a map can address: ExHiROM's two halves.
constexpr std::size_t kMaxRomBytes = 8u * 1024u * 1024u;

// The first 4 MB of an ExHiROM image, the part banks $80-$FF serve.
constexpr std::size_t kExHiRomHalf = 0x400000u;

[[nodiscard]] std::uint16_t word(std::span<const std::uint8_t> rom, std::size_t at) noexcept {
  return static_cast<std::uint16_t>(rom[at] | (rom[at + 1] << 8));
}

// How well a candidate site reads as a header. A checksum agreeing with its own
// complement is the strongest signal a site is real; a map-mode byte naming the
// site it sits in is next; a title that reads as text breaks the remaining ties.
// Negative when the site lies outside the image.
[[nodiscard]] int headerScore(std::span<const std::uint8_t> rom, std::size_t base,
                              CartridgeMap site) noexcept {
  if (base + kHeaderBytes > rom.size()) return -1;
  const std::uint8_t* h = rom.data() + base;
  int score = 0;
  const unsigned complement = h[kHeaderComplement] | (h[kHeaderComplement + 1] << 8);
  const unsigned checksum = h[kHeaderChecksum] | (h[kHeaderChecksum + 1] << 8);
  if ((complement ^ checksum) == 0xFFFFu) score += 32;
  const std::uint8_t mode = h[kHeaderMapMode];
  const bool modeNamesSite = site == CartridgeMap::ExHiRom
                                 ? (mode & 0x0Fu) == kMapModeExHiRom
                                 : ((mode & 0x01u) != 0u) == (site == CartridgeMap::HiRom);
  if (modeNamesSite) score += 16;
  for (std::size_t i = 0; i < kHeaderTitleLength; ++i) {
    const std::uint8_t c = h[kHeaderTitle + i];
    if (c >= 0x20u && c < 0x7Fu) ++score;
  }
  return score;
}

struct Site {
  CartridgeMap map;
  std::size_t base;
  int score;
};

// The site that reads best as a header, or nothing when none does. Ties go to the
// map that addresses more of the image — the ExHiROM site only scores at all on an
// image past 4 MB — and between LoROM and HiROM to LoROM.
[[nodiscard]] std::optional<Site> bestSite(std::span<const std::uint8_t> rom) noexcept {
  const Site lo{CartridgeMap::LoRom, kLoRomHeader, headerScore(rom, kLoRomHeader, CartridgeMap::LoRom)};
  const Site hi{CartridgeMap::HiRom, kHiRomHeader, headerScore(rom, kHiRomHeader, CartridgeMap::HiRom)};
  const Site ex{CartridgeMap::ExHiRom, kExHiRomHeader,
                headerScore(rom, kExHiRomHeader, CartridgeMap::ExHiRom)};
  Site best = lo;
  if (hi.score > best.score) best = hi;
  if (ex.score >= best.score) best = ex;
  if (best.score < 0) return std::nullopt;
  return best;
}

[[nodiscard]] std::size_t saveBytesFromCode(std::uint8_t code) noexcept {
  if (code == 0) return 0;
  if (code > 0x0Fu) return 0;  // a size code this large is not a size; treat it as none
  const std::size_t bytes = std::size_t{1} << (code + 10u);
  return bytes > kMaxSaveRamBytes ? kMaxSaveRamBytes : bytes;
}

// The ROM size code read literally: 1 KB shifted by the code, so a code of zero
// is 1 KB. A code past $0F is not a size.
[[nodiscard]] std::size_t romBytesFromCode(std::uint8_t code) noexcept {
  if (code > 0x0Fu) return 0;
  const std::size_t bytes = std::size_t{1} << (code + 10u);
  return bytes > kMaxRomBytes ? kMaxRomBytes : bytes;
}

// The coprocessor a chipset byte's high nibble names, with the sub-type byte
// telling the custom ones apart.
[[nodiscard]] Coprocessor coprocessorFromNibble(std::uint8_t high, std::uint8_t subtype) noexcept {
  if (high == 0xFu) {
    switch (subtype) {
      case 0x00u: return Coprocessor::Spc7110;
      case 0x01u: return Coprocessor::St010;
      case 0x02u: return Coprocessor::St018;
      case 0x10u: return Coprocessor::Cx4;
      default: return Coprocessor::Unknown;
    }
  }
  switch (high) {
    case 0x0u: return Coprocessor::Dsp;
    case 0x1u: return Coprocessor::Gsu;
    case 0x2u: return Coprocessor::Obc1;
    case 0x3u: return Coprocessor::Sa1;
    case 0x4u: return Coprocessor::Sdd1;
    case 0x5u: return Coprocessor::Srtc;
    case 0xEu: return Coprocessor::Other;
    default: return Coprocessor::Unknown;
  }
}

// Reads the chipset byte into the header: what sits beside the ROM from the low
// nibble, the coprocessor from the high nibble when the low nibble says one is
// present. A low nibble of $2 under a nonzero high nibble is the $5 form.
void readChipset(CartridgeHeader& header) noexcept {
  const std::uint8_t high = static_cast<std::uint8_t>(header.chipset >> 4);
  std::uint8_t low = static_cast<std::uint8_t>(header.chipset & 0x0Fu);
  if (low == 0x2u && high != 0u) low = 0x5u;
  bool coprocessor = false;
  switch (low) {
    case 0x0u: break;
    case 0x1u: header.hasRam = true; break;
    case 0x2u: header.hasRam = true; header.hasBattery = true; break;
    case 0x3u: coprocessor = true; break;
    case 0x4u: coprocessor = true; header.hasRam = true; break;
    case 0x5u: coprocessor = true; header.hasRam = true; header.hasBattery = true; break;
    case 0x6u: coprocessor = true; header.hasBattery = true; break;
    case 0x9u:
      coprocessor = true;
      header.hasRam = true;
      header.hasBattery = true;
      header.hasClock = true;
      break;
    case 0xAu: coprocessor = true; header.hasRam = true; header.hasBattery = true; break;
    default: header.coprocessor = Coprocessor::Unknown; return;
  }
  if (coprocessor) header.coprocessor = coprocessorFromNibble(high, header.chipsetSubtype);
}

// The video standard a country byte implies. Japan, the USA, South Korea, Canada
// and Brazil run at 60 Hz; the run from Europe to Indonesia and Australia at 50;
// the other codes name no standard.
[[nodiscard]] VideoStandard videoFromCountry(std::uint8_t country) noexcept {
  switch (country) {
    case 0x00u: case 0x01u: case 0x0Du: case 0x0Fu: case 0x10u: return VideoStandard::Ntsc;
    case 0x11u: return VideoStandard::Pal;
    default: return country >= 0x02u && country <= 0x0Cu ? VideoStandard::Pal : VideoStandard::Unknown;
  }
}

// Text from the image with trailing spaces and zero bytes removed.
[[nodiscard]] std::string trimmedText(const std::uint8_t* at, std::size_t length) {
  std::string text(reinterpret_cast<const char*>(at), length);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\0')) text.pop_back();
  return text;
}

// The image index an address reaches before the image's own size folds it: the
// map's linear layout, with nothing when the address is not ROM.
[[nodiscard]] std::optional<std::size_t> linearRomIndex(CartridgeMap map,
                                                        std::uint32_t address) noexcept {
  if (cartridgeRegion(map, address) != CartridgeRegion::Rom) return std::nullopt;
  const std::size_t bank = (address >> 16) & 0xFFu;
  const std::size_t offset = address & 0xFFFFu;
  switch (map) {
    case CartridgeMap::LoRom:
      // A bank's high bit only selects the waitstate region, so it is masked off.
      // Each bank's upper half follows the last one's.
      return ((bank & 0x7Fu) << 15) | (offset & 0x7FFFu);
    case CartridgeMap::HiRom:
      // Whole banks end to end; a system bank's upper half reaches the same
      // bytes as the matching cartridge bank.
      return ((bank & 0x3Fu) << 16) | offset;
    case CartridgeMap::ExHiRom:
      // HiROM's layout in $80-$FF; the same layout again in $00-$7D, 4 MB in.
      return (((bank & 0x3Fu) << 16) | offset) | ((bank & 0x80u) != 0u ? 0u : kExHiRomHalf);
  }
  return std::nullopt;
}

// The image byte an index past the image reaches. A cartridge carries one ROM
// chip per power of two in its size, wired one after another, and the board
// leaves the address lines above a chip undecoded — so an address past a chip
// repeats that chip rather than running into the next one. A 3 MB cartridge is a
// 2 MB chip then a 1 MB chip, and the megabyte above it repeats the second chip,
// not the whole image. An image that is itself a power of two is one chip and
// folds on the first pass.
[[nodiscard]] std::size_t mirroredIndex(std::size_t index, std::size_t size) noexcept {
  std::size_t base = 0;
  while (index >= size) {
    const std::size_t chip = std::bit_floor(index);
    index -= chip;
    if (size > chip) {
      size -= chip;
      base += chip;
    }
  }
  return base + index;
}

}  // namespace

std::optional<CartridgeHeader> parseCartridgeHeader(std::span<const std::uint8_t> rom) {
  const std::optional<Site> site = bestSite(rom);
  if (!site.has_value() || site->base + kHeaderWithVectors > rom.size()) return std::nullopt;
  const std::size_t base = site->base;
  const std::uint8_t* h = rom.data() + base;

  CartridgeHeader header;
  header.offset = base;
  header.map = site->map;
  header.title = trimmedText(h + kHeaderTitle, kHeaderTitleLength);
  header.mapMode = h[kHeaderMapMode];
  header.fastRom = (header.mapMode & kMapModeFast) != 0u;
  const std::uint8_t* x = h - kExtendedBytes;  // every site lies past $FFB0 of its bank
  header.chipset = h[kHeaderChipset];
  header.chipsetSubtype = x[kExtendedChipsetSubtype];
  readChipset(header);
  header.romSizeCode = h[kHeaderRomSize];
  header.romSizeBytes = romBytesFromCode(header.romSizeCode);
  header.saveSizeCode = h[kHeaderSaveSize];
  header.saveRamBytes = saveBytesFromCode(header.saveSizeCode);
  header.country = h[kHeaderCountry];
  header.video = videoFromCountry(header.country);
  header.developer = h[kHeaderDeveloper];
  header.version = h[kHeaderVersion];
  if (header.developer == kDeveloperExtended) {
    ExtendedHeader extended;
    extended.makerCode.assign(reinterpret_cast<const char*>(x + kExtendedMaker), kExtendedMakerLength);
    extended.gameCode = trimmedText(x + kExtendedGame, kExtendedGameLength);
    extended.expansionFlashSizeCode = x[kExtendedFlashSize];
    extended.expansionRamSizeCode = x[kExtendedRamSize];
    extended.expansionRamBytes = saveBytesFromCode(extended.expansionRamSizeCode);
    extended.specialVersion = x[kExtendedSpecialVersion];
    header.extended = extended;
  }
  header.complement = word(rom, base + kHeaderComplement);
  header.checksum = word(rom, base + kHeaderChecksum);
  header.checksumAgrees = (header.complement ^ header.checksum) == 0xFFFFu;
  header.native.cop = word(rom, base + kNativeCop);
  header.native.brk = word(rom, base + kNativeBrk);
  header.native.abort = word(rom, base + kNativeAbort);
  header.native.nmi = word(rom, base + kNativeNmi);
  header.native.irq = word(rom, base + kNativeIrq);
  header.emulation.cop = word(rom, base + kEmulationCop);
  header.emulation.abort = word(rom, base + kEmulationAbort);
  header.emulation.nmi = word(rom, base + kEmulationNmi);
  header.emulation.reset = word(rom, base + kEmulationReset);
  header.emulation.irq = word(rom, base + kEmulationIrq);
  return header;
}

CartridgeMap detectCartridgeMap(std::span<const std::uint8_t> rom) noexcept {
  const std::optional<Site> site = bestSite(rom);
  return site.has_value() ? site->map : CartridgeMap::LoRom;
}

std::size_t declaredSaveRamBytes(std::span<const std::uint8_t> rom) noexcept {
  const std::optional<Site> site = bestSite(rom);
  if (!site.has_value()) return 0;
  return saveBytesFromCode(rom[site->base + kHeaderSaveSize]);
}

CartridgeRegion cartridgeRegion(CartridgeMap map, std::uint32_t address) noexcept {
  const std::uint8_t bank = static_cast<std::uint8_t>((address >> 16) & 0xFFu);
  const std::uint16_t offset = static_cast<std::uint16_t>(address & 0xFFFFu);
  if (bank >= 0x7E && bank <= 0x7F) return CartridgeRegion::WorkRam;
  const bool systemBank = bank <= 0x3F || (bank >= 0x80 && bank <= 0xBF);
  if (systemBank) {
    if (offset >= 0x8000u) return CartridgeRegion::Rom;
    return saveRamOffset(map, address).has_value() ? CartridgeRegion::SaveRam
                                                   : CartridgeRegion::System;
  }
  // A cartridge bank: whole under HiROM and ExHiROM, the upper half under LoROM.
  if (map != CartridgeMap::LoRom || offset >= 0x8000u) return CartridgeRegion::Rom;
  return saveRamOffset(map, address).has_value() ? CartridgeRegion::SaveRam
                                                 : CartridgeRegion::Unmapped;
}

std::optional<std::size_t> romOffset(CartridgeMap map, std::uint32_t address,
                                     std::size_t imageBytes) noexcept {
  if (imageBytes == 0) return std::nullopt;
  const std::optional<std::size_t> linear = linearRomIndex(map, address);
  if (!linear.has_value()) return std::nullopt;
  return mirroredIndex(*linear, imageBytes);
}

std::optional<std::uint32_t> romAddress(CartridgeMap map, std::size_t offset) noexcept {
  switch (map) {
    case CartridgeMap::LoRom: {
      if (offset >= 0x400000u) return std::nullopt;
      std::uint32_t bank = static_cast<std::uint32_t>(offset >> 15);
      if (bank == 0x7Eu || bank == 0x7Fu) bank |= 0x80u;  // those banks are work RAM
      return (bank << 16) | 0x8000u | static_cast<std::uint32_t>(offset & 0x7FFFu);
    }
    case CartridgeMap::HiRom: {
      if (offset >= 0x400000u) return std::nullopt;
      return 0xC00000u | static_cast<std::uint32_t>(offset);
    }
    case CartridgeMap::ExHiRom: {
      if (offset >= 2u * kExHiRomHalf) return std::nullopt;
      if (offset < kExHiRomHalf) return 0xC00000u | static_cast<std::uint32_t>(offset);
      const std::size_t upper = offset - kExHiRomHalf;
      const std::uint32_t bank = 0x40u | static_cast<std::uint32_t>(upper >> 16);
      const std::uint32_t within = static_cast<std::uint32_t>(upper & 0xFFFFu);
      if (bank < 0x7Eu) return (bank << 16) | within;
      // Banks $7E-$7F are work RAM; their upper halves are reachable through
      // $3E-$3F, their lower halves through nothing.
      if (within < 0x8000u) return std::nullopt;
      return ((bank & 0x3Fu) << 16) | within;
    }
  }
  return std::nullopt;
}

std::optional<std::size_t> saveRamOffset(CartridgeMap map, std::uint32_t address) noexcept {
  const std::uint8_t bank = static_cast<std::uint8_t>((address >> 16) & 0xFFu);
  const std::uint16_t offset = static_cast<std::uint16_t>(address & 0xFFFFu);
  switch (map) {
    case CartridgeMap::LoRom: {
      const bool window = (bank >= 0x70 && bank <= 0x7D) || (bank >= 0xF0 && bank <= 0xFD);
      if (!window || offset > 0x7FFFu) return std::nullopt;
      return (static_cast<std::size_t>(bank & 0x0Fu) << 15) | static_cast<std::size_t>(offset);
    }
    case CartridgeMap::HiRom: {
      const bool window = (bank >= 0x20 && bank <= 0x3F) || (bank >= 0xA0 && bank <= 0xBF);
      if (!window || offset < 0x6000u || offset > 0x7FFFu) return std::nullopt;
      return (static_cast<std::size_t>(bank & 0x1Fu) << 13) |
             (static_cast<std::size_t>(offset) - 0x6000u);
    }
    case CartridgeMap::ExHiRom: {
      const bool window = bank >= 0x80 && bank <= 0xBF;
      if (!window || offset < 0x6000u || offset > 0x7FFFu) return std::nullopt;
      return (static_cast<std::size_t>(bank & 0x3Fu) << 13) |
             (static_cast<std::size_t>(offset) - 0x6000u);
    }
  }
  return std::nullopt;
}

}  // namespace snaggletooth
