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
constexpr std::size_t kHeaderSaveSize = 0x18u;
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

constexpr std::uint8_t kMapModeFast = 0x10u;
constexpr std::uint8_t kMapModeExHiRom = 0x05u;  // the low nibble that names ExHiROM

// The largest save a cartridge can address through any window.
constexpr std::size_t kMaxSaveRamBytes = 128u * 1024u;

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
  header.title.assign(reinterpret_cast<const char*>(h + kHeaderTitle), kHeaderTitleLength);
  while (!header.title.empty() && (header.title.back() == ' ' || header.title.back() == '\0')) {
    header.title.pop_back();
  }
  header.mapMode = h[kHeaderMapMode];
  header.fastRom = (header.mapMode & kMapModeFast) != 0u;
  header.saveSizeCode = h[kHeaderSaveSize];
  header.saveRamBytes = saveBytesFromCode(header.saveSizeCode);
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
