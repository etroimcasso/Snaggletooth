// The cartridge header and the three maps, as the public cartridge functions
// report them. Every image here is authored: a header is written at the site a
// map uses, with every field set to a value the test chose, so each assertion
// pins a documented fact about where a byte or a field is — never whatever a
// real cartridge happened to hold.

#include <cstdint>
#include <optional>
#include <vector>

#include "gtest/gtest.h"
#include "snaggletooth/snes/cartridge.h"
#include "snaggletooth/snes/snes.h"

namespace snaggletooth {
namespace {

constexpr std::size_t kLoRomSite = 0x7FC0u;
constexpr std::size_t kHiRomSite = 0xFFC0u;
constexpr std::size_t kExHiRomSite = 0x40FFC0u;
constexpr std::size_t kMegabyte = 1024u * 1024u;

// Every byte of an authored image names its own offset, all three bytes of it,
// so two addresses that differ only in their bank still hold different bytes.
std::uint8_t patternAt(std::size_t index) {
  return static_cast<std::uint8_t>(index ^ (index >> 8) ^ (index >> 16));
}

void put16(std::vector<std::uint8_t>& rom, std::size_t at, std::uint16_t value) {
  rom[at] = static_cast<std::uint8_t>(value & 0xFFu);
  rom[at + 1] = static_cast<std::uint8_t>(value >> 8);
}

// Writes a header at `site`: a title, the map-mode byte, the save-size code, a
// checksum agreeing with its complement, and the ten vectors set to distinct
// values that each name their slot.
void writeHeader(std::vector<std::uint8_t>& rom, std::size_t site, std::uint8_t mapMode,
                 std::uint8_t saveCode, const char* title = "AUTHORED TITLE") {
  for (std::size_t i = 0; i < 21; ++i) rom[site + i] = ' ';
  for (std::size_t i = 0; i < 21 && title[i] != '\0'; ++i) {
    rom[site + i] = static_cast<std::uint8_t>(title[i]);
  }
  rom[site + 0x15] = mapMode;
  rom[site + 0x18] = saveCode;
  put16(rom, site + 0x1C, 0x1234u);  // the complement
  put16(rom, site + 0x1E, 0xEDCBu);  // and the checksum it agrees with
  put16(rom, site + 0x24, 0x8E04u);  // native COP
  put16(rom, site + 0x26, 0x8E06u);  // native BRK
  put16(rom, site + 0x28, 0x8E08u);  // native ABORT
  put16(rom, site + 0x2A, 0x8E0Au);  // native NMI
  put16(rom, site + 0x2E, 0x8E0Eu);  // native IRQ
  put16(rom, site + 0x34, 0x8F04u);  // emulation COP
  put16(rom, site + 0x38, 0x8F08u);  // emulation ABORT
  put16(rom, site + 0x3A, 0x8F0Au);  // emulation NMI
  put16(rom, site + 0x3C, 0x8F0Cu);  // emulation RESET
  put16(rom, site + 0x3E, 0x8F0Eu);  // emulation IRQ/BRK
}

std::vector<std::uint8_t> patternedImage(std::size_t bytes) {
  std::vector<std::uint8_t> rom(bytes);
  for (std::size_t i = 0; i < bytes; ++i) rom[i] = patternAt(i);
  return rom;
}

std::vector<std::uint8_t> authored(std::size_t bytes, std::size_t site, std::uint8_t mapMode,
                                   std::uint8_t saveCode = 0) {
  std::vector<std::uint8_t> rom = patternedImage(bytes);
  writeHeader(rom, site, mapMode, saveCode);
  return rom;
}

// ---- which site the header is read from --------------------------------------

TEST(CartridgeHeader, LoRomKeepsItsHeaderAtTheEndOfTheFirstWindow) {
  const std::optional<CartridgeHeader> h = parseCartridgeHeader(authored(512u * 1024u, kLoRomSite, 0x20u, 0u));
  ASSERT_TRUE(h.has_value());
  EXPECT_EQ(h->offset, kLoRomSite);
  EXPECT_EQ(h->map, CartridgeMap::LoRom);
  EXPECT_EQ(h->mapMode, 0x20u);
  EXPECT_FALSE(h->fastRom);
}

TEST(CartridgeHeader, HiRomKeepsItsHeaderAtTheEndOfTheFirstBank) {
  const std::optional<CartridgeHeader> h = parseCartridgeHeader(authored(kMegabyte, kHiRomSite, 0x21u, 0u));
  ASSERT_TRUE(h.has_value());
  EXPECT_EQ(h->offset, kHiRomSite);
  EXPECT_EQ(h->map, CartridgeMap::HiRom);
  EXPECT_EQ(detectCartridgeMap(authored(kMegabyte, kHiRomSite, 0x21u, 0u)), CartridgeMap::HiRom);
}

TEST(CartridgeHeader, ExHiRomKeepsItsHeaderFourMegabytesIn) {
  const std::vector<std::uint8_t> rom = authored(6u * kMegabyte, kExHiRomSite, 0x25u, 0u);
  const std::optional<CartridgeHeader> h = parseCartridgeHeader(rom);
  ASSERT_TRUE(h.has_value());
  EXPECT_EQ(h->offset, kExHiRomSite);
  EXPECT_EQ(h->map, CartridgeMap::ExHiRom);
  EXPECT_EQ(detectCartridgeMap(rom), CartridgeMap::ExHiRom);
}

// A large image often carries the same header at the HiROM site too. Both sites
// read equally well, and the map that addresses the whole image is the one.
TEST(CartridgeHeader, AnExHiRomImageWithACopyAtTheHiRomSiteIsExHiRom) {
  std::vector<std::uint8_t> rom = authored(6u * kMegabyte, kExHiRomSite, 0x25u, 0u);
  writeHeader(rom, kHiRomSite, 0x25u, 0u);
  EXPECT_EQ(detectCartridgeMap(rom), CartridgeMap::ExHiRom);
  EXPECT_EQ(parseCartridgeHeader(rom)->offset, kExHiRomSite);
}

// The ExHiROM site is named by a mode byte whose low nibble is $5. A copy of a
// HiROM header sitting 4 MB in — mode $21, checksum and title intact — does not
// name that site, so the HiROM site keeps the tie it would otherwise lose.
TEST(CartridgeHeader, TheExHiRomSiteIsNamedOnlyByModeFive) {
  std::vector<std::uint8_t> rom = authored(6u * kMegabyte, kHiRomSite, 0x21u, 0u);
  writeHeader(rom, kExHiRomSite, 0x21u, 0u);
  EXPECT_EQ(detectCartridgeMap(rom), CartridgeMap::HiRom);
  writeHeader(rom, kExHiRomSite, 0x25u, 0u);
  EXPECT_EQ(detectCartridgeMap(rom), CartridgeMap::ExHiRom);
}

// An image under 4 MB has no ExHiROM site at all, whatever its mode byte says.
TEST(CartridgeHeader, AnImageUnderFourMegabytesIsNeverExHiRom) {
  const std::vector<std::uint8_t> rom = authored(kMegabyte, kHiRomSite, 0x25u, 0u);
  EXPECT_EQ(detectCartridgeMap(rom), CartridgeMap::HiRom);
}

// The map is the site's, not the mode byte's: a header at the LoROM site that
// claims HiROM still lays out as LoROM, and the claim is reported as written.
TEST(CartridgeHeader, TheMapIsTheSitesAndTheModeByteIsReportedAsWritten) {
  const std::vector<std::uint8_t> rom = authored(512u * 1024u, kLoRomSite, 0x21u, 0u);
  const std::optional<CartridgeHeader> h = parseCartridgeHeader(rom);
  ASSERT_TRUE(h.has_value());
  EXPECT_EQ(h->map, CartridgeMap::LoRom);
  EXPECT_EQ(h->mapMode, 0x21u);
}

// An image always carries bytes at a header site; what it carries there is
// reported as it is, and the checksum and title say how much to trust it. Only
// an image too small to hold a header at any site has none.
TEST(CartridgeHeader, AnImageWithNothingAtAnySiteIsReadAsItIs) {
  const std::vector<std::uint8_t> blank(512u * 1024u, 0u);
  const std::optional<CartridgeHeader> h = parseCartridgeHeader(blank);
  ASSERT_TRUE(h.has_value());
  EXPECT_EQ(h->map, CartridgeMap::LoRom);
  EXPECT_EQ(h->title, "");
  EXPECT_FALSE(h->checksumAgrees);
  EXPECT_EQ(h->emulation.reset, 0u);
  EXPECT_EQ(detectCartridgeMap(blank), CartridgeMap::LoRom);
  EXPECT_EQ(declaredSaveRamBytes(blank), 0u);

  const std::vector<std::uint8_t> tiny(0x100u, 0u);
  EXPECT_FALSE(parseCartridgeHeader(tiny).has_value());
  EXPECT_EQ(detectCartridgeMap(tiny), CartridgeMap::LoRom);
  EXPECT_EQ(declaredSaveRamBytes(tiny), 0u);
}

// ---- the fields ---------------------------------------------------------------

TEST(CartridgeHeader, TheFastBitIsBitFourOfTheModeByte) {
  const std::optional<CartridgeHeader> lo = parseCartridgeHeader(authored(512u * 1024u, kLoRomSite, 0x30u, 0u));
  ASSERT_TRUE(lo.has_value());
  EXPECT_TRUE(lo->fastRom);
  EXPECT_EQ(lo->map, CartridgeMap::LoRom);
  const std::optional<CartridgeHeader> hi = parseCartridgeHeader(authored(kMegabyte, kHiRomSite, 0x31u, 0u));
  ASSERT_TRUE(hi.has_value());
  EXPECT_TRUE(hi->fastRom);
  EXPECT_EQ(hi->map, CartridgeMap::HiRom);
}

TEST(CartridgeHeader, TheTitleIsTwentyOneBytesWithTrailingPaddingRemoved) {
  std::vector<std::uint8_t> rom = patternedImage(512u * 1024u);
  writeHeader(rom, kLoRomSite, 0x20u, 0u, "TWO  WORDS");
  EXPECT_EQ(parseCartridgeHeader(rom)->title, "TWO  WORDS");
  writeHeader(rom, kLoRomSite, 0x20u, 0u, "EXACTLY TWENTY-ONE CH");
  EXPECT_EQ(parseCartridgeHeader(rom)->title, "EXACTLY TWENTY-ONE CH");
  writeHeader(rom, kLoRomSite, 0x20u, 0u, "");
  EXPECT_EQ(parseCartridgeHeader(rom)->title, "");
  writeHeader(rom, kLoRomSite, 0x20u, 0u, "ZERO PADDED");
  for (std::size_t i = 11; i < 21; ++i) rom[kLoRomSite + i] = 0u;
  EXPECT_EQ(parseCartridgeHeader(rom)->title, "ZERO PADDED");
}

TEST(CartridgeHeader, TheChecksumPairIsReportedAsWrittenAndJudged) {
  std::vector<std::uint8_t> rom = authored(512u * 1024u, kLoRomSite, 0x20u, 0u);
  std::optional<CartridgeHeader> h = parseCartridgeHeader(rom);
  ASSERT_TRUE(h.has_value());
  EXPECT_EQ(h->complement, 0x1234u);
  EXPECT_EQ(h->checksum, 0xEDCBu);
  EXPECT_TRUE(h->checksumAgrees);

  put16(rom, kLoRomSite + 0x1E, 0xEDCAu);  // one bit off
  h = parseCartridgeHeader(rom);
  ASSERT_TRUE(h.has_value());  // the mode byte and the title still make it the header
  EXPECT_EQ(h->checksum, 0xEDCAu);
  EXPECT_FALSE(h->checksumAgrees);
}

TEST(CartridgeHeader, TheSaveSizeCodeIsOneKilobyteShiftedAndClamped) {
  const auto bytesFor = [](std::uint8_t code) {
    const std::vector<std::uint8_t> rom = authored(512u * 1024u, kLoRomSite, 0x20u, code);
    const std::optional<CartridgeHeader> h = parseCartridgeHeader(rom);
    EXPECT_EQ(h->saveSizeCode, code);
    EXPECT_EQ(h->saveRamBytes, declaredSaveRamBytes(rom));
    return h->saveRamBytes;
  };
  EXPECT_EQ(bytesFor(0u), 0u);
  EXPECT_EQ(bytesFor(1u), 2048u);
  EXPECT_EQ(bytesFor(3u), 8192u);
  EXPECT_EQ(bytesFor(5u), 32768u);
  EXPECT_EQ(bytesFor(7u), 131072u);
  EXPECT_EQ(bytesFor(8u), 131072u);   // beyond what a cartridge can address
  EXPECT_EQ(bytesFor(0x10u), 0u);     // not a size
}

TEST(CartridgeHeader, TheVectorsFollowTheHeaderInTheirDocumentedSlots) {
  const std::optional<CartridgeHeader> h = parseCartridgeHeader(authored(kMegabyte, kHiRomSite, 0x21u, 0u));
  ASSERT_TRUE(h.has_value());
  EXPECT_EQ(h->native.cop, 0x8E04u);
  EXPECT_EQ(h->native.brk, 0x8E06u);
  EXPECT_EQ(h->native.abort, 0x8E08u);
  EXPECT_EQ(h->native.nmi, 0x8E0Au);
  EXPECT_EQ(h->native.irq, 0x8E0Eu);
  EXPECT_EQ(h->emulation.cop, 0x8F04u);
  EXPECT_EQ(h->emulation.abort, 0x8F08u);
  EXPECT_EQ(h->emulation.nmi, 0x8F0Au);
  EXPECT_EQ(h->emulation.reset, 0x8F0Cu);
  EXPECT_EQ(h->emulation.irq, 0x8F0Eu);
}

// ---- where a bus address lands -------------------------------------------------

TEST(CartridgeHeader, LoRomLaysUpperHalvesEndToEnd) {
  const std::size_t size = 4u * kMegabyte;
  EXPECT_EQ(romOffset(CartridgeMap::LoRom, 0x008000u, size), 0x000000u);
  EXPECT_EQ(romOffset(CartridgeMap::LoRom, 0x00FFFFu, size), 0x007FFFu);
  EXPECT_EQ(romOffset(CartridgeMap::LoRom, 0x018000u, size), 0x008000u);
  EXPECT_EQ(romOffset(CartridgeMap::LoRom, 0x808000u, size), 0x000000u);  // the high bit only picks the speed
  EXPECT_EQ(romOffset(CartridgeMap::LoRom, 0xFF8000u, size), 0x3F8000u);
  EXPECT_EQ(romOffset(CartridgeMap::LoRom, 0x400000u, size), std::nullopt);  // a lower half reaches nothing
  EXPECT_EQ(romOffset(CartridgeMap::LoRom, 0x700000u, size), std::nullopt);  // the save window
  EXPECT_EQ(romOffset(CartridgeMap::LoRom, 0x7E8000u, size), std::nullopt);  // work RAM
}

TEST(CartridgeHeader, HiRomLaysWholeBanksEndToEnd) {
  const std::size_t size = 4u * kMegabyte;
  EXPECT_EQ(romOffset(CartridgeMap::HiRom, 0xC00000u, size), 0x000000u);
  EXPECT_EQ(romOffset(CartridgeMap::HiRom, 0xC11234u, size), 0x011234u);
  EXPECT_EQ(romOffset(CartridgeMap::HiRom, 0x401000u, size), 0x001000u);   // the first cartridge region, whole
  EXPECT_EQ(romOffset(CartridgeMap::HiRom, 0x008000u, size), 0x008000u);   // a system bank's upper half
  EXPECT_EQ(romOffset(CartridgeMap::HiRom, 0x3FFFFFu, size), 0x3FFFFFu);
  EXPECT_EQ(romOffset(CartridgeMap::HiRom, 0x001000u, size), std::nullopt);  // the system area
  EXPECT_EQ(romOffset(CartridgeMap::HiRom, 0x206000u, size), std::nullopt);  // the save window
}

TEST(CartridgeHeader, ExHiRomServesTheSecondFourMegabytesInTheLowBanks) {
  const std::size_t size = 8u * kMegabyte;
  EXPECT_EQ(romOffset(CartridgeMap::ExHiRom, 0xC00000u, size), 0x000000u);  // the first 4 MB, as HiROM
  EXPECT_EQ(romOffset(CartridgeMap::ExHiRom, 0x808000u, size), 0x008000u);
  EXPECT_EQ(romOffset(CartridgeMap::ExHiRom, 0x400000u, size), 0x400000u);  // the second 4 MB
  EXPECT_EQ(romOffset(CartridgeMap::ExHiRom, 0x7D1234u, size), 0x7D1234u);
  EXPECT_EQ(romOffset(CartridgeMap::ExHiRom, 0x008000u, size), 0x408000u);  // its mirror in the system banks
  EXPECT_EQ(romOffset(CartridgeMap::ExHiRom, 0x3E8000u, size), 0x7E8000u);  // where work RAM hides a bank
  EXPECT_EQ(romOffset(CartridgeMap::ExHiRom, 0x7E0000u, size), std::nullopt);
  EXPECT_EQ(romOffset(CartridgeMap::ExHiRom, 0x806000u, size), std::nullopt);  // the save window
  EXPECT_EQ(romOffset(CartridgeMap::ExHiRom, 0x206000u, size), std::nullopt);  // not a save window here: the system area
}

TEST(CartridgeHeader, AnAddressPastTheImageRepeatsTheChipItLandsOn) {
  // A 512 KB image is one chip and repeats whole. A 3 MB image is a 2 MB chip
  // then a 1 MB chip, and the megabyte above it repeats the second chip.
  EXPECT_EQ(romOffset(CartridgeMap::LoRom, 0x108234u, 512u * 1024u), 0x000234u);
  EXPECT_EQ(romOffset(CartridgeMap::LoRom, 0x608000u, 3u * kMegabyte), 0x200000u);
  EXPECT_EQ(romOffset(CartridgeMap::LoRom, 0x6A8000u, 3u * kMegabyte), 0x250000u);
  EXPECT_EQ(romOffset(CartridgeMap::HiRom, 0xF00000u, 3u * kMegabyte), 0x200000u);
  EXPECT_EQ(romOffset(CartridgeMap::ExHiRom, 0x600000u, 6u * kMegabyte), 0x400000u);  // 6 MB past the start: the 2 MB chip again
  EXPECT_EQ(romOffset(CartridgeMap::HiRom, 0xC00000u, 0u), std::nullopt);  // no image, no byte
}

TEST(CartridgeHeader, ARomAddressReadsItsOffsetBackWhole) {
  const std::size_t size = 8u * kMegabyte;
  for (const CartridgeMap map : {CartridgeMap::LoRom, CartridgeMap::HiRom, CartridgeMap::ExHiRom}) {
    for (const std::size_t offset : {std::size_t{0}, std::size_t{0x7FFF}, std::size_t{0x8000},
                                     std::size_t{0x123456}, std::size_t{0x3F8000}, std::size_t{0x3FFFFF}}) {
      const std::optional<std::uint32_t> address = romAddress(map, offset);
      ASSERT_TRUE(address.has_value()) << "map " << static_cast<int>(map) << " offset " << offset;
      EXPECT_EQ(romOffset(map, *address, size), offset) << "map " << static_cast<int>(map);
    }
  }
  EXPECT_EQ(romAddress(CartridgeMap::LoRom, 0x000000u), 0x008000u);
  EXPECT_EQ(romAddress(CartridgeMap::LoRom, 0x3F0000u), 0xFE8000u);  // banks $7E-$7F are work RAM
  EXPECT_EQ(romAddress(CartridgeMap::HiRom, 0x123456u), 0xD23456u);
  EXPECT_EQ(romAddress(CartridgeMap::ExHiRom, 0x123456u), 0xD23456u);
  EXPECT_EQ(romAddress(CartridgeMap::ExHiRom, 0x400000u), 0x400000u);
  EXPECT_EQ(romAddress(CartridgeMap::ExHiRom, 0x7E8000u), 0x3E8000u);
  EXPECT_EQ(romOffset(CartridgeMap::ExHiRom, 0x3E8000u, size), 0x7E8000u);
  EXPECT_EQ(romAddress(CartridgeMap::ExHiRom, 0x7E0000u), std::nullopt);  // no address reads it
  EXPECT_EQ(romAddress(CartridgeMap::LoRom, 0x400000u), std::nullopt);    // beyond the map
  EXPECT_EQ(romAddress(CartridgeMap::HiRom, 0x400000u), std::nullopt);
  EXPECT_EQ(romAddress(CartridgeMap::ExHiRom, 0x800000u), std::nullopt);
}

TEST(CartridgeHeader, RegionsUnderEachMap) {
  using R = CartridgeRegion;
  for (const CartridgeMap map : {CartridgeMap::LoRom, CartridgeMap::HiRom, CartridgeMap::ExHiRom}) {
    EXPECT_EQ(cartridgeRegion(map, 0x7E0000u), R::WorkRam);
    EXPECT_EQ(cartridgeRegion(map, 0x7FFFFFu), R::WorkRam);
    EXPECT_EQ(cartridgeRegion(map, 0x000000u), R::System);
    EXPECT_EQ(cartridgeRegion(map, 0x802140u), R::System);
    EXPECT_EQ(cartridgeRegion(map, 0x008000u), R::Rom);
    EXPECT_EQ(cartridgeRegion(map, 0xBFFFFFu), R::Rom);
    EXPECT_EQ(cartridgeRegion(map, 0xC08000u), R::Rom);
  }
  EXPECT_EQ(cartridgeRegion(CartridgeMap::LoRom, 0xC00000u), R::Unmapped);
  EXPECT_EQ(cartridgeRegion(CartridgeMap::HiRom, 0xC00000u), R::Rom);
  EXPECT_EQ(cartridgeRegion(CartridgeMap::ExHiRom, 0xC00000u), R::Rom);
  EXPECT_EQ(cartridgeRegion(CartridgeMap::LoRom, 0x400000u), R::Unmapped);
  EXPECT_EQ(cartridgeRegion(CartridgeMap::LoRom, 0x700000u), R::SaveRam);
  EXPECT_EQ(cartridgeRegion(CartridgeMap::LoRom, 0x708000u), R::Rom);
  EXPECT_EQ(cartridgeRegion(CartridgeMap::LoRom, 0x206000u), R::System);
  EXPECT_EQ(cartridgeRegion(CartridgeMap::HiRom, 0x400000u), R::Rom);
  EXPECT_EQ(cartridgeRegion(CartridgeMap::HiRom, 0x206000u), R::SaveRam);
  EXPECT_EQ(cartridgeRegion(CartridgeMap::HiRom, 0xA07FFFu), R::SaveRam);
  EXPECT_EQ(cartridgeRegion(CartridgeMap::HiRom, 0x1F6000u), R::System);
  EXPECT_EQ(cartridgeRegion(CartridgeMap::HiRom, 0x700000u), R::Rom);
  EXPECT_EQ(cartridgeRegion(CartridgeMap::ExHiRom, 0x400000u), R::Rom);
  EXPECT_EQ(cartridgeRegion(CartridgeMap::ExHiRom, 0x806000u), R::SaveRam);
  EXPECT_EQ(cartridgeRegion(CartridgeMap::ExHiRom, 0xBF7FFFu), R::SaveRam);
  EXPECT_EQ(cartridgeRegion(CartridgeMap::ExHiRom, 0x206000u), R::System);
  EXPECT_EQ(cartridgeRegion(CartridgeMap::ExHiRom, 0x3E8000u), R::Rom);
}

TEST(CartridgeHeader, SaveWindowsUnderEachMap) {
  EXPECT_EQ(saveRamOffset(CartridgeMap::LoRom, 0x700000u), 0x0000u);
  EXPECT_EQ(saveRamOffset(CartridgeMap::LoRom, 0x711234u), 0x9234u);
  EXPECT_EQ(saveRamOffset(CartridgeMap::LoRom, 0xF00000u), 0x0000u);
  EXPECT_EQ(saveRamOffset(CartridgeMap::LoRom, 0x7D7FFFu), (std::size_t{0x0D} << 15) | 0x7FFFu);
  EXPECT_EQ(saveRamOffset(CartridgeMap::LoRom, 0x708000u), std::nullopt);
  EXPECT_EQ(saveRamOffset(CartridgeMap::LoRom, 0x7E0000u), std::nullopt);
  EXPECT_EQ(saveRamOffset(CartridgeMap::LoRom, 0x6F0000u), std::nullopt);

  EXPECT_EQ(saveRamOffset(CartridgeMap::HiRom, 0x206000u), 0x0000u);
  EXPECT_EQ(saveRamOffset(CartridgeMap::HiRom, 0x216000u), 0x2000u);
  EXPECT_EQ(saveRamOffset(CartridgeMap::HiRom, 0xA06000u), 0x0000u);
  EXPECT_EQ(saveRamOffset(CartridgeMap::HiRom, 0x3F7FFFu), (std::size_t{0x1F} << 13) | 0x1FFFu);
  EXPECT_EQ(saveRamOffset(CartridgeMap::HiRom, 0x205FFFu), std::nullopt);
  EXPECT_EQ(saveRamOffset(CartridgeMap::HiRom, 0x1F6000u), std::nullopt);
  EXPECT_EQ(saveRamOffset(CartridgeMap::HiRom, 0x700000u), std::nullopt);

  EXPECT_EQ(saveRamOffset(CartridgeMap::ExHiRom, 0x806000u), 0x0000u);
  EXPECT_EQ(saveRamOffset(CartridgeMap::ExHiRom, 0x816000u), 0x2000u);
  EXPECT_EQ(saveRamOffset(CartridgeMap::ExHiRom, 0xBF7FFFu), (std::size_t{0x3F} << 13) | 0x1FFFu);
  EXPECT_EQ(saveRamOffset(CartridgeMap::ExHiRom, 0x206000u), std::nullopt);
  EXPECT_EQ(saveRamOffset(CartridgeMap::ExHiRom, 0x805FFFu), std::nullopt);
  EXPECT_EQ(saveRamOffset(CartridgeMap::ExHiRom, 0xC06000u), std::nullopt);
}

// ---- the machine reads through the same functions -------------------------------

// Boots an ExHiROM image and copies bytes from both halves into work RAM, so the
// machine's bus is shown serving the second 4 MB where the map puts it.
TEST(CartridgeHeader, TheMachineServesAnExHiRomImageWhereTheHeaderSaysItIs) {
  std::vector<std::uint8_t> rom = authored(6u * kMegabyte, kExHiRomSite, 0x25u, 0u);
  // Bank $00 at $8000 mirrors the second 4 MB's first bank, so the program goes
  // at file $408000 and the reset vector at file $40FFFC.
  const std::vector<std::uint32_t> sources = {0x401000u, 0xC01000u, 0x3E9000u};
  std::vector<std::uint8_t> program;
  auto put24 = [&](std::uint32_t address) {
    program.push_back(static_cast<std::uint8_t>(address & 0xFFu));
    program.push_back(static_cast<std::uint8_t>((address >> 8) & 0xFFu));
    program.push_back(static_cast<std::uint8_t>((address >> 16) & 0xFFu));
  };
  for (std::size_t i = 0; i < sources.size(); ++i) {
    program.push_back(0xAFu);  // LDA long
    put24(sources[i]);
    program.push_back(0x8Fu);  // STA long
    put24(0x7E0000u + static_cast<std::uint32_t>(i));
  }
  program.push_back(0xDBu);  // STP
  for (std::size_t i = 0; i < program.size(); ++i) rom[0x408000u + i] = program[i];
  put16(rom, 0x40FFFCu, 0x8000u);

  Snes m(SnesConfig{.rom = rom, .iplStub = false});
  for (int i = 0; i < 400 && m.state().cpu.run == CpuRunState::Running; ++i) m.step();
  EXPECT_EQ(m.state().wram[0], patternAt(0x401000u));  // the second 4 MB
  EXPECT_EQ(m.state().wram[1], patternAt(0x001000u));  // the first
  EXPECT_EQ(m.state().wram[2], patternAt(0x5E9000u));  // 6 MB image: bank $3E reaches the 2 MB chip again
}

}  // namespace
}  // namespace snaggletooth
