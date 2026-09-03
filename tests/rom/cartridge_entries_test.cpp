// The entry points a cartridge header names, and which disassembler owns each
// part of the bus. The headers here are values the test fills in, so each case
// pins one rule about which vectors become entries and in what order.

#include <cstdint>
#include <vector>

#include "gtest/gtest.h"
#include "rom/cartridge_entries.h"
#include "snaggletooth/snes/cartridge.h"

namespace snaggletooth::disasm {
namespace {

CartridgeHeader headerWithEveryVectorInRom() {
  CartridgeHeader h;
  h.map = CartridgeMap::LoRom;
  h.native = NativeVectors{.cop = 0x8E04u, .brk = 0x8E06u, .abort = 0x8E08u, .nmi = 0x8E0Au, .irq = 0x8E0Eu};
  h.emulation =
      EmulationVectors{.cop = 0x8F04u, .abort = 0x8F08u, .nmi = 0x8F0Au, .reset = 0x8F0Cu, .irq = 0x8F0Eu};
  return h;
}

TEST(CartridgeEntries, EveryVectorInRomIsAnEntryWithResetFirst) {
  const std::vector<VectorEntry> entries = vectorEntries(headerWithEveryVectorInRom());
  ASSERT_EQ(entries.size(), 10u);
  EXPECT_EQ(entries[0].address, 0x8F0Cu);
  EXPECT_EQ(entries[0].name, "reset");
  EXPECT_EQ(entries[1].address, 0x8F0Au);
  EXPECT_EQ(entries[1].name, "nmi");
  EXPECT_EQ(entries[2].address, 0x8F0Eu);
  EXPECT_EQ(entries[2].name, "irq");
  EXPECT_EQ(entries[3].address, 0x8F04u);
  EXPECT_EQ(entries[3].name, "cop");
  EXPECT_EQ(entries[4].address, 0x8F08u);
  EXPECT_EQ(entries[4].name, "abort");
  EXPECT_EQ(entries[5].address, 0x8E0Au);
  EXPECT_EQ(entries[5].name, "nmi_native");
  EXPECT_EQ(entries[6].address, 0x8E0Eu);
  EXPECT_EQ(entries[6].name, "irq_native");
  EXPECT_EQ(entries[7].address, 0x8E04u);
  EXPECT_EQ(entries[7].name, "cop_native");
  EXPECT_EQ(entries[8].address, 0x8E06u);
  EXPECT_EQ(entries[8].name, "brk_native");
  EXPECT_EQ(entries[9].address, 0x8E08u);
  EXPECT_EQ(entries[9].name, "abort_native");
}

TEST(CartridgeEntries, EntriesLandInBankZero) {
  for (const VectorEntry& e : vectorEntries(headerWithEveryVectorInRom())) {
    EXPECT_EQ(e.address & 0xFF0000u, 0u);
  }
}

// A vector pointing below $8000 names RAM or a register, where no image byte
// can be traced, so it contributes no entry — under any map.
TEST(CartridgeEntries, AVectorOutsideRomIsLeftOut) {
  for (const CartridgeMap map : {CartridgeMap::LoRom, CartridgeMap::HiRom, CartridgeMap::ExHiRom}) {
    CartridgeHeader h = headerWithEveryVectorInRom();
    h.map = map;
    h.emulation.cop = 0x0000u;
    h.emulation.abort = 0x1234u;
    h.native.brk = 0x7FFFu;
    const std::vector<VectorEntry> entries = vectorEntries(h);
    ASSERT_EQ(entries.size(), 7u);
    for (const VectorEntry& e : entries) {
      EXPECT_NE(e.name, "cop");
      EXPECT_NE(e.name, "abort");
      EXPECT_NE(e.name, "brk_native");
      EXPECT_GE(e.address, 0x8000u);
    }
  }
}

TEST(CartridgeEntries, TwoVectorsNamingOneHandlerGiveTwoEntries) {
  CartridgeHeader h = headerWithEveryVectorInRom();
  h.emulation.nmi = 0x9000u;
  h.native.nmi = 0x9000u;
  const std::vector<VectorEntry> entries = vectorEntries(h);
  ASSERT_EQ(entries.size(), 10u);
  EXPECT_EQ(entries[1].address, 0x9000u);
  EXPECT_EQ(entries[1].name, "nmi");
  EXPECT_EQ(entries[5].address, 0x9000u);
  EXPECT_EQ(entries[5].name, "nmi_native");
}

TEST(CartridgeEntries, CodeOwnerFollowsTheRegion) {
  for (const CartridgeMap map : {CartridgeMap::LoRom, CartridgeMap::HiRom, CartridgeMap::ExHiRom}) {
    EXPECT_EQ(codeOwner(map, 0x008000u), CodeOwner::Cpu65816);
    EXPECT_EQ(codeOwner(map, 0xC08000u), CodeOwner::Cpu65816);
    EXPECT_EQ(codeOwner(map, 0x7E0000u), CodeOwner::None);  // work RAM
    EXPECT_EQ(codeOwner(map, 0x002140u), CodeOwner::None);  // a register
  }
  EXPECT_EQ(codeOwner(CartridgeMap::LoRom, 0x400000u), CodeOwner::None);      // nothing there
  EXPECT_EQ(codeOwner(CartridgeMap::LoRom, 0xC00000u), CodeOwner::None);      // a lower half
  EXPECT_EQ(codeOwner(CartridgeMap::LoRom, 0x700000u), CodeOwner::None);      // the save
  EXPECT_EQ(codeOwner(CartridgeMap::HiRom, 0x400000u), CodeOwner::Cpu65816);  // a whole bank
  EXPECT_EQ(codeOwner(CartridgeMap::HiRom, 0x206000u), CodeOwner::None);      // the save
  EXPECT_EQ(codeOwner(CartridgeMap::ExHiRom, 0x806000u), CodeOwner::None);    // the save
}

// The chain end to end: a header read from an image gives the entries a trace
// of that image starts from.
TEST(CartridgeEntries, AParsedHeaderGivesItsEntries) {
  std::vector<std::uint8_t> rom(512u * 1024u, 0u);
  const std::size_t site = 0x7FC0u;
  for (std::size_t i = 0; i < 21; ++i) rom[site + i] = 'A';
  rom[site + 0x15] = 0x20u;
  rom[site + 0x1C] = 0x34u;
  rom[site + 0x1D] = 0x12u;
  rom[site + 0x1E] = 0xCBu;
  rom[site + 0x1F] = 0xEDu;
  rom[site + 0x3C] = 0x00u;  // reset -> $8000
  rom[site + 0x3D] = 0x80u;
  rom[site + 0x3A] = 0x50u;  // NMI -> $8150
  rom[site + 0x3B] = 0x81u;
  const std::optional<CartridgeHeader> h = parseCartridgeHeader(rom);
  ASSERT_TRUE(h.has_value());
  const std::vector<VectorEntry> entries = vectorEntries(*h);
  ASSERT_EQ(entries.size(), 2u);  // every other vector is $0000
  EXPECT_EQ(entries[0].address, 0x8000u);
  EXPECT_EQ(entries[0].name, "reset");
  EXPECT_EQ(entries[1].address, 0x8150u);
  EXPECT_EQ(entries[1].name, "nmi");
}

}  // namespace
}  // namespace snaggletooth::disasm
