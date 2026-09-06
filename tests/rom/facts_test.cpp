// What the code reaches: the register class table, the access facts a trace
// produces, and the transfers those facts describe.
//
// The class table is checked against the staged MMIO register table and the PPU
// registers page — a case per class, and the addresses where one range of
// registers ends and the next begins. The facts are checked against one hand-built
// cartridge whose reset code drives hardware the way a real one does: it sets the
// screen up, reads a status register, fills a DMA channel a byte at a time and
// starts it, fills a second channel with one sixteen-bit store, and leaves two
// more channels half-described so the absent fields have something to be absent
// on.

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "examples/example_cartridges.h"
#include "gtest/gtest.h"
#include "rom/rom_disasm.h"
#include "rom/rom_facts.h"
#include "rom/rom_verify.h"

namespace snaggletooth::disasm {
namespace {

using examples::loRomImage;
using examples::put;
using examples::threeBankImage;

// The register at an address, or a default row and a failure naming the address.
Cpu65816Register at(Address address) {
  const std::optional<Cpu65816Register> found = cpu65816Register(address);
  EXPECT_TRUE(found.has_value()) << "no register at " << std::hex << address;
  return found.value_or(Cpu65816Register{});
}

// A cartridge whose reset code drives hardware. Every site the cases name is
// commented with its address; the runs are separated by jumps, so what one run
// established is not carried into the next.
std::vector<std::uint8_t> hardwareImage() {
  std::vector<std::uint8_t> rom = loRomImage(1);

  // Run one, from the reset vector: the screen, a status read, and channel 0
  // filled a byte at a time and started.
  put(rom, 0x0000u, {
                        0x18u,                     // $8000 CLC
                        0xFBu,                     // $8001 XCE          -> native
                        0xE2u, 0x30u,              // $8002 SEP #$30     -> A8, X8
                        0xA9u, 0x8Fu,              // $8004 LDA #$8F
                        0x8Du, 0x00u, 0x21u,       // $8006 STA !$2100   INIDISP = $8F
                        0xADu, 0x10u, 0x42u,       // $8009 LDA !$4210   RDNMI, a read
                        0x9Cu, 0x0Bu, 0x42u,       // $800C STZ !$420B   MDMAEN = $00
                        0xA9u, 0x01u,              // $800F LDA #$01
                        0x8Du, 0x00u, 0x43u,       // $8011 STA !$4300   DMAP0 = $01
                        0xA9u, 0x18u,              // $8014 LDA #$18
                        0x8Du, 0x01u, 0x43u,       // $8016 STA !$4301   BBAD0 = $18
                        0xA9u, 0x00u,              // $8019 LDA #$00
                        0x8Du, 0x02u, 0x43u,       // $801B STA !$4302   A1T0L = $00
                        0xA9u, 0x80u,              // $801E LDA #$80
                        0x8Du, 0x03u, 0x43u,       // $8020 STA !$4303   A1T0H = $80
                        0xA9u, 0x01u,              // $8023 LDA #$01
                        0x8Du, 0x04u, 0x43u,       // $8025 STA !$4304   A1B0  = $01
                        0xA9u, 0x01u,              // $8028 LDA #$01
                        0x8Du, 0x0Bu, 0x42u,       // $802A STA !$420B   MDMAEN = $01
                        0x4Cu, 0x40u, 0x80u,       // $802D JMP !$8040
                    });

  // Run two: one sixteen-bit store fills a channel's first two registers, then a
  // read-modify-write and a compare.
  put(rom, 0x0040u, {
                        0xC2u, 0x20u,              // $8040 REP #$20     -> A16
                        0xA9u, 0x01u, 0x18u,       // $8042 LDA #$1801
                        0x8Du, 0x10u, 0x43u,       // $8045 STA !$4310   DMAP1 = $01, BBAD1 = $18
                        0xE2u, 0x20u,              // $8048 SEP #$20     -> A8
                        0xEEu, 0x00u, 0x43u,       // $804A INC !$4300   a read-write
                        0xCDu, 0x12u, 0x42u,       // $804D CMP !$4212   HVBJOY, a read
                        0x4Cu, 0x60u, 0x80u,       // $8050 JMP !$8060
                    });

  // Run three: the two ways a store has no value — a label on the store, and a
  // load that is not an immediate.
  put(rom, 0x0060u, {
                        0xF0u, 0x05u,              // $8060 BEQ $8067
                        0xADu, 0x00u, 0x80u,       // $8062 LDA !$8000   ordinary memory
                        0xA9u, 0x55u,              // $8065 LDA #$55
                        0x8Du, 0x00u, 0x21u,       // $8067 STA !$2100   a label sits here
                        0xA5u, 0x21u,              // $806A LDA $21      direct page
                        0x8Du, 0x00u, 0x21u,       // $806C STA !$2100   the load was not an immediate
                        0xA2u, 0x77u,              // $806F LDX #$77
                        0x8Du, 0x00u, 0x21u,       // $8071 STA !$2100   the load filled another register
                        0x4Cu, 0x80u, 0x80u,       // $8074 JMP !$8080
                    });

  // Run four: long operands, and two channels left half-described.
  put(rom, 0x0080u, {
                        0x8Fu, 0x00u, 0x21u, 0x00u,  // $8080 STA $00:2100  a long operand, bank zero
                        0x8Fu, 0x00u, 0x21u, 0x7Eu,  // $8084 STA $7E:2100  a bank with no registers
                        0xA9u, 0x33u,                // $8088 LDA #$33
                        0x8Du, 0x21u, 0x43u,         // $808A STA !$4321   BBAD2 = $33, no DMAP2
                        0xA9u, 0x04u,                // $808D LDA #$04
                        0x8Du, 0x30u, 0x43u,         // $808F STA !$4330   DMAP3 = $04, no BBAD3
                        0xA9u, 0x08u,                // $8092 LDA #$08
                        0x8Du, 0x0Cu, 0x42u,         // $8094 STA !$420C   HDMAEN = $08
                        0x7Cu, 0x00u, 0x21u,         // $8097 JMP (!$2100,X)  through a register
                    });
  return rom;
}

CartridgeDisassembly disassembled(std::span<const std::uint8_t> rom) {
  CartridgeRequest request;
  request.rom = rom;
  request.captureSound = false;
  return disassembleCartridge(request);
}

// The access at a site naming a register, or nothing.
const HardwareAccess* accessAt(const std::vector<HardwareAccess>& facts, Address site,
                               std::string_view name) {
  for (const HardwareAccess& fact : facts) {
    if (fact.site == site && fact.name == name) return &fact;
  }
  return nullptr;
}

const DmaTransfer* transferOn(const std::vector<DmaTransfer>& dmas, std::uint8_t channel) {
  for (const DmaTransfer& dma : dmas) {
    if (dma.channel == channel) return &dma;
  }
  return nullptr;
}

}  // namespace

// ---- the class table, against the staged pages ------------------------------

TEST(RomFacts, DisplayIsTheScreenItsCompositionAndItsCounters) {
  EXPECT_EQ(at(0x2100).cls, RegisterClass::Display);  // INIDISP
  EXPECT_EQ(at(0x212C).cls, RegisterClass::Display);  // TM, the layer-enable pair
  EXPECT_EQ(at(0x212D).cls, RegisterClass::Display);  // TS
  EXPECT_EQ(at(0x2133).cls, RegisterClass::Display);  // SETINI
  EXPECT_EQ(at(0x2137).cls, RegisterClass::Display);  // SLHV
  EXPECT_EQ(at(0x213C).cls, RegisterClass::Display);  // OPHCT
  EXPECT_EQ(at(0x213F).cls, RegisterClass::Display);  // STAT78
}

TEST(RomFacts, BackgroundIsTheFourLayersAndTheirScroll) {
  EXPECT_EQ(at(0x2105).cls, RegisterClass::Background);  // BGMODE
  EXPECT_EQ(at(0x2106).cls, RegisterClass::Background);  // MOSAIC
  EXPECT_EQ(at(0x2107).cls, RegisterClass::Background);  // BG1SC
  EXPECT_EQ(at(0x210B).cls, RegisterClass::Background);  // BG12NBA
  EXPECT_EQ(at(0x2114).cls, RegisterClass::Background);  // BG4VOFS
}

TEST(RomFacts, VramIsThePortAndBothWaysThroughIt) {
  EXPECT_EQ(at(0x2115).cls, RegisterClass::Vram);  // VMAIN
  EXPECT_EQ(at(0x2116).cls, RegisterClass::Vram);  // VMADDL
  EXPECT_EQ(at(0x2118).cls, RegisterClass::Vram);  // VMDATAL
  EXPECT_EQ(at(0x2139).cls, RegisterClass::Vram);  // VMDATALREAD
}

TEST(RomFacts, CgramIsThePalettePort) {
  EXPECT_EQ(at(0x2121).cls, RegisterClass::Cgram);  // CGADD
  EXPECT_EQ(at(0x2122).cls, RegisterClass::Cgram);  // CGDATA
  EXPECT_EQ(at(0x213B).cls, RegisterClass::Cgram);  // CGDATAREAD
}

TEST(RomFacts, OamIsTheSpriteTablePortAndSpriteShape) {
  EXPECT_EQ(at(0x2101).cls, RegisterClass::Oam);  // OBJSEL
  EXPECT_EQ(at(0x2102).cls, RegisterClass::Oam);  // OAMADDL
  EXPECT_EQ(at(0x2104).cls, RegisterClass::Oam);  // OAMDATA
  EXPECT_EQ(at(0x2138).cls, RegisterClass::Oam);  // OAMDATAREAD
}

TEST(RomFacts, ModeSevenIsItsMatrixAndCentre) {
  EXPECT_EQ(at(0x211A).cls, RegisterClass::Mode7);  // M7SEL
  EXPECT_EQ(at(0x211B).cls, RegisterClass::Mode7);  // M7A
  EXPECT_EQ(at(0x2120).cls, RegisterClass::Mode7);  // M7Y
}

TEST(RomFacts, WindowIsTheMasksThePositionsAndTheirEnable) {
  EXPECT_EQ(at(0x2123).cls, RegisterClass::Window);  // W12SEL
  EXPECT_EQ(at(0x2126).cls, RegisterClass::Window);  // WH0
  EXPECT_EQ(at(0x212A).cls, RegisterClass::Window);  // WBGLOG
  EXPECT_EQ(at(0x212E).cls, RegisterClass::Window);  // TMW, which is a window enable
  EXPECT_EQ(at(0x212F).cls, RegisterClass::Window);  // TSW
}

TEST(RomFacts, ColorMathIsItsSelectDesignationAndFixedColor) {
  EXPECT_EQ(at(0x2130).cls, RegisterClass::ColorMath);  // CGWSEL
  EXPECT_EQ(at(0x2131).cls, RegisterClass::ColorMath);  // CGADSUB
  EXPECT_EQ(at(0x2132).cls, RegisterClass::ColorMath);  // COLDATA
}

// The multiplication result has a section of its own on the PPU page, and a game
// uses it as a fast multiplier whether or not it is drawing Mode 7 — so it is
// arithmetic, beside the CPU's own multiplier and divider.
TEST(RomFacts, MathIsBothMultipliersAndTheDivider) {
  EXPECT_EQ(at(0x2134).cls, RegisterClass::Math);  // MPYL
  EXPECT_EQ(at(0x2136).cls, RegisterClass::Math);  // MPYH
  EXPECT_EQ(at(0x4202).cls, RegisterClass::Math);  // WRMPYA
  EXPECT_EQ(at(0x4206).cls, RegisterClass::Math);  // WRDIVB
  EXPECT_EQ(at(0x4214).cls, RegisterClass::Math);  // RDDIVL
  EXPECT_EQ(at(0x4217).cls, RegisterClass::Math);  // RDMPYH
}

TEST(RomFacts, ApuIsTheFourPorts) {
  EXPECT_EQ(at(0x2140).cls, RegisterClass::Apu);
  EXPECT_EQ(at(0x2143).cls, RegisterClass::Apu);
}

TEST(RomFacts, WramPortIsItsDataAndItsAddress) {
  EXPECT_EQ(at(0x2180).cls, RegisterClass::WramPort);  // WMDATA
  EXPECT_EQ(at(0x2183).cls, RegisterClass::WramPort);  // WMADDH
}

TEST(RomFacts, JoypadIsBothSerialPortsAndTheAutoRead) {
  EXPECT_EQ(at(0x4016).cls, RegisterClass::Joypad);
  EXPECT_EQ(at(0x4017).cls, RegisterClass::Joypad);
  EXPECT_EQ(at(0x4218).cls, RegisterClass::Joypad);  // JOY1L
  EXPECT_EQ(at(0x421F).cls, RegisterClass::Joypad);  // JOY4H
}

TEST(RomFacts, InterruptIsTheEnablesTheTimersAndTheFlags) {
  EXPECT_EQ(at(0x4200).cls, RegisterClass::Interrupt);  // NMITIMEN
  EXPECT_EQ(at(0x4207).cls, RegisterClass::Interrupt);  // HTIMEL
  EXPECT_EQ(at(0x420A).cls, RegisterClass::Interrupt);  // VTIMEH
  EXPECT_EQ(at(0x4210).cls, RegisterClass::Interrupt);  // RDNMI
  EXPECT_EQ(at(0x4212).cls, RegisterClass::Interrupt);  // HVBJOY
}

TEST(RomFacts, DmaControlIsTheTwoRegistersThatStartATransfer) {
  EXPECT_EQ(at(0x420B).cls, RegisterClass::DmaControl);  // MDMAEN
  EXPECT_EQ(at(0x420C).cls, RegisterClass::DmaControl);  // HDMAEN
}

TEST(RomFacts, DmaChannelIsEveryChannelsOwnRegisters) {
  EXPECT_EQ(at(0x4300).cls, RegisterClass::DmaChannel);  // DMAP0
  EXPECT_EQ(at(0x4301).name, "BBAD0");
  EXPECT_EQ(at(0x437A).cls, RegisterClass::DmaChannel);  // NLTR7
  EXPECT_EQ(at(0x437A).name, "NLTR7");
}

TEST(RomFacts, IoIsTheGeneralPurposeBitsOfTheControllerPorts) {
  EXPECT_EQ(at(0x4201).cls, RegisterClass::Io);  // WRIO
  EXPECT_EQ(at(0x4213).cls, RegisterClass::Io);  // RDIO
}

TEST(RomFacts, SpeedIsTheFastRomEnable) {
  EXPECT_EQ(at(0x420D).cls, RegisterClass::Speed);  // MEMSEL
  EXPECT_EQ(at(0x420D).name, "MEMSEL");
}

// ---- the table's edges ------------------------------------------------------

TEST(RomFacts, TheRangesEndWhereTheStagedTableEndsThem) {
  EXPECT_EQ(at(0x213F).name, "STAT78");                       // the last PPU register
  EXPECT_EQ(at(0x2140).name, "APUIO0");                       // the first port after it
  EXPECT_FALSE(cpu65816Register(0x2144).has_value());         // between the ports and the WRAM port
  EXPECT_EQ(at(0x2180).name, "WMDATA");
  EXPECT_FALSE(cpu65816Register(0x2184).has_value());
  EXPECT_FALSE(cpu65816Register(0x4015).has_value());
  EXPECT_EQ(at(0x4016).name, "JOYSER0/JOYOUT");
  EXPECT_FALSE(cpu65816Register(0x4018).has_value());
  EXPECT_EQ(at(0x4200).name, "NMITIMEN");
  EXPECT_FALSE(cpu65816Register(0x4220).has_value());         // past the last of the $42xx set
  EXPECT_FALSE(cpu65816Register(0x42FFu).has_value());
  EXPECT_EQ(at(0x4300).name, "DMAP0");
  EXPECT_EQ(at(0x437F).name, "UNUSED7");                      // the last DMA slot with a name
  EXPECT_FALSE(cpu65816Register(0x4380).has_value());
}

// Three of the sixteen bytes each channel occupies are named by no table, so
// they are not registers and carry no class.
TEST(RomFacts, TheUnnamedSlotsOfAChannelAreNotRegisters) {
  EXPECT_EQ(at(0x430B).name, "UNUSED0");
  EXPECT_FALSE(cpu65816Register(0x430C).has_value());
  EXPECT_FALSE(cpu65816Register(0x430D).has_value());
  EXPECT_FALSE(cpu65816Register(0x430E).has_value());
  EXPECT_EQ(at(0x430F).name, "UNUSED0");
}

// A register is a register only in the banks that show it.
TEST(RomFacts, RegistersAreNamedOnlyInTheBanksThatShowThem) {
  EXPECT_EQ(at(0x002100).name, "INIDISP");
  EXPECT_EQ(at(0x3F2100).name, "INIDISP");
  EXPECT_EQ(at(0x802100).name, "INIDISP");
  EXPECT_EQ(at(0xBF2100).name, "INIDISP");
  EXPECT_FALSE(cpu65816Register(0x402100).has_value());
  EXPECT_FALSE(cpu65816Register(0x7E2100).has_value());
  EXPECT_FALSE(cpu65816Register(0xC02100).has_value());
}

// The staged table's Type column, which is what a report may say about direction.
TEST(RomFacts, TheTableSaysWhichRegistersReadAndWhichWrite) {
  EXPECT_TRUE(at(0x2100).writes);
  EXPECT_FALSE(at(0x2100).reads);   // INIDISP is write-only
  EXPECT_TRUE(at(0x213E).reads);
  EXPECT_FALSE(at(0x213E).writes);  // STAT77 is read-only
  EXPECT_TRUE(at(0x2140).reads);
  EXPECT_TRUE(at(0x2140).writes);   // the APU ports go both ways
  EXPECT_TRUE(at(0x2180).reads);
  EXPECT_TRUE(at(0x2180).writes);   // WMDATA does too
  EXPECT_FALSE(at(0x2181).reads);
  EXPECT_TRUE(at(0x2181).writes);   // its address does not
  EXPECT_TRUE(at(0x4300).reads);
  EXPECT_TRUE(at(0x4300).writes);   // every channel register reads back
}

TEST(RomFacts, EveryClassNamesItself) {
  EXPECT_EQ(cpu65816RegisterClassName(RegisterClass::Display), "Display");
  EXPECT_EQ(cpu65816RegisterClassName(RegisterClass::DmaChannel), "DmaChannel");
  EXPECT_EQ(cpu65816RegisterClassName(RegisterClass::WramPort), "WramPort");
  EXPECT_EQ(accessKindName(AccessKind::Read), "read");
  EXPECT_EQ(accessKindName(AccessKind::Write), "write");
  EXPECT_EQ(accessKindName(AccessKind::ReadWrite), "read-write");
}

// ---- the access facts -------------------------------------------------------

TEST(RomFacts, AnAccessCarriesItsRegisterItsClassAndItsKind) {
  const std::vector<std::uint8_t> rom = hardwareImage();
  const CartridgeDisassembly disassembly = disassembled(rom);

  const HardwareAccess* write = accessAt(disassembly.accesses, 0x008006u, "INIDISP");
  ASSERT_NE(write, nullptr);
  EXPECT_EQ(write->registerAddress, 0x2100u);
  EXPECT_EQ(write->cls, RegisterClass::Display);
  EXPECT_EQ(write->kind, AccessKind::Write);

  const HardwareAccess* read = accessAt(disassembly.accesses, 0x008009u, "RDNMI");
  ASSERT_NE(read, nullptr);
  EXPECT_EQ(read->cls, RegisterClass::Interrupt);
  EXPECT_EQ(read->kind, AccessKind::Read);
  EXPECT_FALSE(read->value.has_value());

  const HardwareAccess* compared = accessAt(disassembly.accesses, 0x00804Du, "HVBJOY");
  ASSERT_NE(compared, nullptr);
  EXPECT_EQ(compared->kind, AccessKind::Read);
}

TEST(RomFacts, AReadModifyWriteDoesBoth) {
  const std::vector<std::uint8_t> rom = hardwareImage();
  const CartridgeDisassembly disassembly = disassembled(rom);
  const HardwareAccess* fact = accessAt(disassembly.accesses, 0x00804Au, "DMAP0");
  ASSERT_NE(fact, nullptr);
  EXPECT_EQ(fact->kind, AccessKind::ReadWrite);
  EXPECT_FALSE(fact->value.has_value());
}

TEST(RomFacts, AValueIsTheImmediateTheInstructionBeforeLoaded) {
  const std::vector<std::uint8_t> rom = hardwareImage();
  const CartridgeDisassembly disassembly = disassembled(rom);

  const HardwareAccess* screen = accessAt(disassembly.accesses, 0x008006u, "INIDISP");
  ASSERT_NE(screen, nullptr);
  ASSERT_TRUE(screen->value.has_value());
  EXPECT_EQ(*screen->value, 0x8Fu);

  const HardwareAccess* bbad = accessAt(disassembly.accesses, 0x008016u, "BBAD0");
  ASSERT_NE(bbad, nullptr);
  ASSERT_TRUE(bbad->value.has_value());
  EXPECT_EQ(*bbad->value, 0x18u);
}

// `STZ` needs no instruction before it: the zero is the instruction's own.
TEST(RomFacts, StoreZeroCarriesItsOwnValue) {
  const std::vector<std::uint8_t> rom = hardwareImage();
  const CartridgeDisassembly disassembly = disassembled(rom);
  const HardwareAccess* fact = accessAt(disassembly.accesses, 0x00800Cu, "MDMAEN");
  ASSERT_NE(fact, nullptr);
  ASSERT_TRUE(fact->value.has_value());
  EXPECT_EQ(*fact->value, 0x00u);
}

TEST(RomFacts, ASixteenBitStoreReachesTwoRegisters) {
  const std::vector<std::uint8_t> rom = hardwareImage();
  const CartridgeDisassembly disassembly = disassembled(rom);

  const HardwareAccess* low = accessAt(disassembly.accesses, 0x008045u, "DMAP1");
  const HardwareAccess* high = accessAt(disassembly.accesses, 0x008045u, "BBAD1");
  ASSERT_NE(low, nullptr);
  ASSERT_NE(high, nullptr);
  ASSERT_TRUE(low->value.has_value());
  ASSERT_TRUE(high->value.has_value());
  EXPECT_EQ(*low->value, 0x01u);   // the immediate's low byte
  EXPECT_EQ(*high->value, 0x18u);  // and its high byte, to the register after
}

TEST(RomFacts, ALabelOnTheStoreLeavesItWithoutAValue) {
  const std::vector<std::uint8_t> rom = hardwareImage();
  const CartridgeDisassembly disassembly = disassembled(rom);
  const HardwareAccess* fact = accessAt(disassembly.accesses, 0x008067u, "INIDISP");
  ASSERT_NE(fact, nullptr);
  EXPECT_EQ(fact->kind, AccessKind::Write);
  EXPECT_FALSE(fact->value.has_value())
      << "another path arrives at the store, so what the line above loaded is not what every "
         "arrival carries";
}

TEST(RomFacts, ALoadThatIsNotAnImmediateLeavesNoValue) {
  const std::vector<std::uint8_t> rom = hardwareImage();
  const CartridgeDisassembly disassembly = disassembled(rom);
  const HardwareAccess* fact = accessAt(disassembly.accesses, 0x00806Cu, "INIDISP");
  ASSERT_NE(fact, nullptr);
  EXPECT_FALSE(fact->value.has_value());
}

// `LDX` before `STA` says nothing about what `STA` writes.
TEST(RomFacts, AnImmediateIntoAnotherRegisterIsNotThisStoresValue) {
  const std::vector<std::uint8_t> rom = hardwareImage();
  const CartridgeDisassembly disassembly = disassembled(rom);
  const HardwareAccess* fact = accessAt(disassembly.accesses, 0x008071u, "INIDISP");
  ASSERT_NE(fact, nullptr);
  EXPECT_EQ(fact->kind, AccessKind::Write);
  EXPECT_FALSE(fact->value.has_value());
}

// A jump through a pointer held at a register address is a stop the trace already
// reports; it reaches the pointer, not the hardware, so it is no access.
TEST(RomFacts, AJumpThroughARegisterAddressIsNotAnAccess) {
  const std::vector<std::uint8_t> rom = hardwareImage();
  const CartridgeDisassembly disassembly = disassembled(rom);
  for (const HardwareAccess& fact : disassembly.accesses) {
    EXPECT_NE(fact.site, 0x008097u) << "a jump's operand is a destination, not an access";
  }
}

TEST(RomFacts, ALongOperandIsNamedByItsOwnBank) {
  const std::vector<std::uint8_t> rom = hardwareImage();
  const CartridgeDisassembly disassembly = disassembled(rom);

  const HardwareAccess* named = accessAt(disassembly.accesses, 0x008080u, "INIDISP");
  ASSERT_NE(named, nullptr);
  EXPECT_EQ(named->registerAddress, 0x002100u);

  // The same offset in a bank that shows no registers is memory.
  for (const HardwareAccess& fact : disassembly.accesses) {
    EXPECT_NE(fact.site, 0x008084u) << "$7E:2100 is work RAM, not a register";
  }
}

TEST(RomFacts, ADirectPageOperandAndOrdinaryMemoryProduceNothing) {
  const std::vector<std::uint8_t> rom = hardwareImage();
  const CartridgeDisassembly disassembly = disassembled(rom);
  for (const HardwareAccess& fact : disassembly.accesses) {
    EXPECT_NE(fact.site, 0x00806Au) << "a direct-page operand names no address the image settles";
    EXPECT_NE(fact.site, 0x008062u) << "$00:8000 is the cartridge, not a register";
  }
}

// A run of bytes between two instructions ends the run: the load did not reach
// the store by falling through it. The listing is built here rather than traced,
// because a trace gives a line reached any other way a label, and this pins the
// boundary on its own.
TEST(RomFacts, DataBetweenTwoInstructionsEndsTheRun) {
  const std::vector<std::uint8_t> code = {
      0xA9u, 0x8Fu,         // $00:8000 LDA #$8F
      0x00u,                // $00:8002 a byte the trace never entered
      0x8Du, 0x00u, 0x21u,  // $00:8003 STA !$2100
  };
  const Cpu65816Mode mode = Cpu65816Mode::native(true, true);

  const auto codeLine = [&](Address address) {
    Line line;
    line.isCode = true;
    line.address = address;
    line.context = contextOf(mode);
    const std::optional<Instruction> decoded = decodeAt(code, 0x008000u, address, mode);
    EXPECT_TRUE(decoded.has_value());
    line.instruction = decoded.value_or(Instruction{});
    return line;
  };
  Line gap;
  gap.isCode = false;
  gap.address = 0x008002u;
  gap.data = {0x00u};

  const auto factsOver = [](std::vector<Line> lines) {
    CartridgeDisassembly disassembly;
    RegionListing region;
    region.region = SourceRegion{.file = "bank_00.asm", .first = 0x008000u, .last = 0x008005u};
    region.listing.addressBits = 24;
    region.listing.lines = std::move(lines);
    disassembly.regions.push_back(std::move(region));
    return hardwareAccesses(disassembly);
  };

  const std::vector<HardwareAccess> across = factsOver({codeLine(0x008000u), gap, codeLine(0x008003u)});
  ASSERT_EQ(across.size(), 1u);
  EXPECT_EQ(across[0].name, "INIDISP");
  EXPECT_FALSE(across[0].value.has_value()) << "the data run ended the run the load was in";

  // The same two instructions with nothing between them do carry the value, so
  // the case above turns on the gap and on nothing else.
  const std::vector<HardwareAccess> adjacent = factsOver({codeLine(0x008000u), codeLine(0x008003u)});
  ASSERT_EQ(adjacent.size(), 1u);
  ASSERT_TRUE(adjacent[0].value.has_value());
  EXPECT_EQ(*adjacent[0].value, 0x8Fu);
}

// A byte the trace never entered is data, and data drives no hardware.
TEST(RomFacts, DataProducesNoFacts) {
  const std::vector<std::uint8_t> rom = hardwareImage();
  const CartridgeDisassembly disassembly = disassembled(rom);
  for (const HardwareAccess& fact : disassembly.accesses) {
    EXPECT_LE(fact.site, 0x008097u) << "every site is an instruction the trace reached";
  }
  EXPECT_FALSE(disassembly.accesses.empty());
}

// ---- the transfers ----------------------------------------------------------

TEST(RomFacts, AChannelFilledAByteAtATimeIsDescribedWhole) {
  const std::vector<std::uint8_t> rom = hardwareImage();
  const CartridgeDisassembly disassembly = disassembled(rom);

  const DmaTransfer* dma = transferOn(disassembly.dmas, 0);
  ASSERT_NE(dma, nullptr);
  EXPECT_EQ(dma->site, 0x008016u);  // where its B-bus address was written
  EXPECT_EQ(dma->direction, DmaDirection::ToBBus);
  ASSERT_TRUE(dma->destination.has_value());
  EXPECT_EQ(*dma->destination, 0x2118u);  // $18 in BBAD names VMDATAL
  EXPECT_EQ(dma->destinationName, "VMDATAL");
  ASSERT_TRUE(dma->destinationClass.has_value());
  EXPECT_EQ(*dma->destinationClass, RegisterClass::Vram);
  ASSERT_TRUE(dma->source.has_value());
  EXPECT_EQ(*dma->source, 0x018000u);
  ASSERT_TRUE(dma->startMask.has_value());
  EXPECT_EQ(*dma->startMask, 0x01u);
  EXPECT_FALSE(dma->hdma);
}

TEST(RomFacts, OneSixteenBitStoreDescribesAChannelToo) {
  const std::vector<std::uint8_t> rom = hardwareImage();
  const CartridgeDisassembly disassembly = disassembled(rom);

  const DmaTransfer* dma = transferOn(disassembly.dmas, 1);
  ASSERT_NE(dma, nullptr);
  EXPECT_EQ(dma->direction, DmaDirection::ToBBus);
  EXPECT_EQ(dma->destinationName, "VMDATAL");
  EXPECT_FALSE(dma->source.has_value()) << "no address registers were written in that run";
  EXPECT_FALSE(dma->startMask.has_value()) << "nothing started it in that run";
}

TEST(RomFacts, ADestinationWithoutADirectionSaysSoRatherThanGuess) {
  const std::vector<std::uint8_t> rom = hardwareImage();
  const CartridgeDisassembly disassembly = disassembled(rom);

  const DmaTransfer* dma = transferOn(disassembly.dmas, 2);
  ASSERT_NE(dma, nullptr);
  EXPECT_EQ(dma->site, 0x00808Au);
  EXPECT_EQ(dma->direction, DmaDirection::Unknown) << "no value for DMAP2 was written";
  ASSERT_TRUE(dma->destination.has_value());
  EXPECT_EQ(*dma->destination, 0x2133u);
  EXPECT_EQ(dma->destinationName, "SETINI");
}

TEST(RomFacts, ADirectionWithoutADestinationIsStillATransferAndStillHdma) {
  const std::vector<std::uint8_t> rom = hardwareImage();
  const CartridgeDisassembly disassembly = disassembled(rom);

  const DmaTransfer* dma = transferOn(disassembly.dmas, 3);
  ASSERT_NE(dma, nullptr);
  EXPECT_EQ(dma->site, 0x00808Fu) << "the DMAP write is the site when no BBAD was written";
  EXPECT_EQ(dma->direction, DmaDirection::ToBBus);
  EXPECT_FALSE(dma->destination.has_value());
  EXPECT_TRUE(dma->destinationName.empty());
  EXPECT_FALSE(dma->destinationClass.has_value());
  ASSERT_TRUE(dma->startMask.has_value());
  EXPECT_EQ(*dma->startMask, 0x08u);
  EXPECT_TRUE(dma->hdma) << "HDMAEN started it, not MDMAEN";
}

TEST(RomFacts, AChannelNobodyWroteHasNoTransfer) {
  const std::vector<std::uint8_t> rom = hardwareImage();
  const CartridgeDisassembly disassembly = disassembled(rom);
  EXPECT_EQ(transferOn(disassembly.dmas, 4), nullptr);
  EXPECT_EQ(disassembly.dmas.size(), 4u);
}

TEST(RomFacts, ADirectionIsReadFromTheDirectionBitAlone) {
  std::vector<HardwareAccess> accesses = {
      HardwareAccess{.site = 0x008000u,
                     .registerAddress = 0x4300u,
                     .name = "DMAP0",
                     .cls = RegisterClass::DmaChannel,
                     .kind = AccessKind::Write,
                     .value = 0x80u,
                     .run = 1u},
  };
  const std::vector<DmaTransfer> dmas = dmaTransfers(accesses);
  ASSERT_EQ(dmas.size(), 1u);
  EXPECT_EQ(dmas[0].direction, DmaDirection::ToABus);
  EXPECT_EQ(dmaDirectionName(dmas[0].direction), "from-register");
  EXPECT_EQ(dmaDirectionName(DmaDirection::ToBBus), "to-register");
}

// Two registers written in different runs are not one transfer: nothing says the
// second was reached from the first.
TEST(RomFacts, PiecesFromDifferentRunsAreNotOneTransfer) {
  std::vector<HardwareAccess> accesses = {
      HardwareAccess{.site = 0x008000u,
                     .registerAddress = 0x4301u,
                     .name = "BBAD0",
                     .cls = RegisterClass::DmaChannel,
                     .kind = AccessKind::Write,
                     .value = 0x18u,
                     .run = 1u},
      HardwareAccess{.site = 0x008010u,
                     .registerAddress = 0x4302u,
                     .name = "A1T0L",
                     .cls = RegisterClass::DmaChannel,
                     .kind = AccessKind::Write,
                     .value = 0x00u,
                     .run = 2u},
      HardwareAccess{.site = 0x008013u,
                     .registerAddress = 0x4303u,
                     .name = "A1T0H",
                     .cls = RegisterClass::DmaChannel,
                     .kind = AccessKind::Write,
                     .value = 0x80u,
                     .run = 2u},
      HardwareAccess{.site = 0x008016u,
                     .registerAddress = 0x4304u,
                     .name = "A1B0",
                     .cls = RegisterClass::DmaChannel,
                     .kind = AccessKind::Write,
                     .value = 0x01u,
                     .run = 2u},
  };
  const std::vector<DmaTransfer> dmas = dmaTransfers(accesses);
  ASSERT_EQ(dmas.size(), 1u);
  EXPECT_FALSE(dmas[0].source.has_value())
      << "the address registers were written in another run than the destination";
}

// A source address is three registers. Two of them is not an address, and the
// third is not assumed to be zero.
TEST(RomFacts, ASourceMissingItsBankIsNoSource) {
  const auto write = [](Address site, Address reg, std::string_view name, std::uint8_t value) {
    return HardwareAccess{.site = site,
                          .registerAddress = reg,
                          .name = name,
                          .cls = RegisterClass::DmaChannel,
                          .kind = AccessKind::Write,
                          .value = value,
                          .run = 1u};
  };
  std::vector<HardwareAccess> accesses = {
      write(0x008000u, 0x4301u, "BBAD0", 0x18u),
      write(0x008003u, 0x4302u, "A1T0L", 0x00u),
      write(0x008006u, 0x4303u, "A1T0H", 0x80u),
  };
  ASSERT_EQ(dmaTransfers(accesses).size(), 1u);
  EXPECT_FALSE(dmaTransfers(accesses)[0].source.has_value()) << "the bank register was never written";

  accesses.push_back(write(0x008009u, 0x4304u, "A1B0", 0x01u));
  ASSERT_EQ(dmaTransfers(accesses).size(), 1u);
  ASSERT_TRUE(dmaTransfers(accesses)[0].source.has_value());
  EXPECT_EQ(*dmaTransfers(accesses)[0].source, 0x018000u);
}

// A start register names its channels in a mask, and a channel the mask leaves
// out was not started by it.
TEST(RomFacts, AStartNamesTheChannelsItsMaskNames) {
  const auto write = [](Address site, Address reg, std::string_view name, RegisterClass cls,
                        std::uint8_t value) {
    return HardwareAccess{.site = site,
                          .registerAddress = reg,
                          .name = name,
                          .cls = cls,
                          .kind = AccessKind::Write,
                          .value = value,
                          .run = 1u};
  };
  const std::vector<HardwareAccess> accesses = {
      write(0x008000u, 0x4301u, "BBAD0", RegisterClass::DmaChannel, 0x18u),
      write(0x008003u, 0x4311u, "BBAD1", RegisterClass::DmaChannel, 0x22u),
      write(0x008006u, 0x420Bu, "MDMAEN", RegisterClass::DmaControl, 0x02u),
  };
  const std::vector<DmaTransfer> dmas = dmaTransfers(accesses);
  ASSERT_EQ(dmas.size(), 2u);
  EXPECT_EQ(dmas[0].channel, 0u);
  EXPECT_FALSE(dmas[0].startMask.has_value()) << "$02 does not name channel 0";
  EXPECT_EQ(dmas[1].channel, 1u);
  ASSERT_TRUE(dmas[1].startMask.has_value());
  EXPECT_EQ(*dmas[1].startMask, 0x02u);
}

// ---- one transfer per start ---------------------------------------------------
//
// The declaring cartridge sets channel 0 up and starts it four times in one
// stretch of straight-line code, leaves channel 6 half described, and behind a
// button sets five more channels up whole: the cases pin one transfer per start
// with the registers as they stood when the start was written, the step and
// the count, and the start's site.

namespace {

using examples::declaringImage;
using examples::liftingImage;

const DmaTransfer* transferAt(const std::vector<DmaTransfer>& dmas, Address site) {
  for (const DmaTransfer& dma : dmas) {
    if (dma.site == site) return &dma;
  }
  return nullptr;
}

std::size_t transfersOn(const std::vector<DmaTransfer>& dmas, std::uint8_t channel) {
  std::size_t count = 0;
  for (const DmaTransfer& dma : dmas) {
    if (dma.channel == channel) ++count;
  }
  return count;
}

}  // namespace

TEST(RomFacts, EveryStartInARunIsItsOwnTransfer) {
  const std::vector<std::uint8_t> rom = declaringImage();
  const CartridgeDisassembly disassembly = disassembled(rom);
  EXPECT_EQ(transfersOn(disassembly.dmas, 0), 4u) << renderManifest(disassembly);

  const DmaTransfer* tileset = transferAt(disassembly.dmas, 0x008007u);
  ASSERT_NE(tileset, nullptr);
  EXPECT_EQ(tileset->channel, 0u);
  EXPECT_EQ(tileset->destinationName, "VMDATAL");
  ASSERT_TRUE(tileset->source.has_value());
  EXPECT_EQ(*tileset->source, 0x009000u);
  ASSERT_TRUE(tileset->step.has_value());
  EXPECT_EQ(*tileset->step, MovedStep::Increment);
  ASSERT_TRUE(tileset->bytes.has_value());
  EXPECT_EQ(*tileset->bytes, 32u);
  ASSERT_TRUE(tileset->startMask.has_value());
  EXPECT_EQ(*tileset->startMask, 0x01u);
  ASSERT_TRUE(tileset->startSite.has_value());
  EXPECT_EQ(*tileset->startSite, 0x008025u);

  // The second start rewrote only the source and the count: the destination
  // is the one the first left, and the site is the first register written
  // since the start.
  const DmaTransfer* second = transferAt(disassembly.dmas, 0x00802Au);
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(second->direction, DmaDirection::ToBBus);
  EXPECT_EQ(second->destinationName, "VMDATAL");
  ASSERT_TRUE(second->source.has_value());
  EXPECT_EQ(*second->source, 0x009040u);
  ASSERT_TRUE(second->bytes.has_value());
  EXPECT_EQ(*second->bytes, 16u);
  ASSERT_TRUE(second->startSite.has_value());
  EXPECT_EQ(*second->startSite, 0x00803Eu);

  const DmaTransfer* palette = transferAt(disassembly.dmas, 0x008048u);
  ASSERT_NE(palette, nullptr);
  EXPECT_EQ(palette->destinationName, "CGDATA");
  ASSERT_TRUE(palette->source.has_value());
  EXPECT_EQ(*palette->source, 0x00920Fu);
  ASSERT_TRUE(palette->step.has_value());
  EXPECT_EQ(*palette->step, MovedStep::Decrement);
  ASSERT_TRUE(palette->bytes.has_value());
  EXPECT_EQ(*palette->bytes, 16u);
  ASSERT_TRUE(palette->startSite.has_value());
  EXPECT_EQ(*palette->startSite, 0x008066u);

  const DmaTransfer* fill = transferAt(disassembly.dmas, 0x008070u);
  ASSERT_NE(fill, nullptr);
  ASSERT_TRUE(fill->step.has_value());
  EXPECT_EQ(*fill->step, MovedStep::Fixed);
  ASSERT_TRUE(fill->bytes.has_value());
  EXPECT_EQ(*fill->bytes, 64u);
  ASSERT_TRUE(fill->startSite.has_value());
  EXPECT_EQ(*fill->startSite, 0x00808Eu);
}

TEST(RomFacts, TheCountIsWhatDasHoldsAndZeroIsSixtyFourKilobytes) {
  const std::vector<std::uint8_t> rom = declaringImage();
  const CartridgeDisassembly disassembly = disassembled(rom);
  const DmaTransfer* one = transferAt(disassembly.dmas, 0x008107u);
  ASSERT_NE(one, nullptr);
  EXPECT_EQ(one->channel, 1u);
  ASSERT_TRUE(one->bytes.has_value());
  EXPECT_EQ(*one->bytes, 48u);
  ASSERT_TRUE(one->source.has_value());
  EXPECT_EQ(*one->source, 0x009100u);
  const DmaTransfer* two = transferAt(disassembly.dmas, 0x008147u);
  ASSERT_NE(two, nullptr);
  EXPECT_EQ(two->destinationName, "OAMDATA");
  ASSERT_TRUE(two->bytes.has_value());
  EXPECT_EQ(*two->bytes, 544u);
  const DmaTransfer* three = transferAt(disassembly.dmas, 0x008187u);
  ASSERT_NE(three, nullptr);
  ASSERT_TRUE(three->bytes.has_value());
  EXPECT_EQ(*three->bytes, 65536u) << "a count of zero moves the whole sixty-four kilobytes";
  ASSERT_TRUE(three->step.has_value());
  EXPECT_EQ(*three->step, MovedStep::Fixed);
}

TEST(RomFacts, AWriteWithoutAProvenValueLeavesTheRegisterUnknown) {
  const std::vector<std::uint8_t> rom = declaringImage();
  const CartridgeDisassembly disassembly = disassembled(rom);
  const DmaTransfer* four = transferAt(disassembly.dmas, 0x0081C7u);
  ASSERT_NE(four, nullptr);
  EXPECT_FALSE(four->source.has_value()) << "the source's low byte was rewritten from a variable";
  ASSERT_TRUE(four->bytes.has_value());
  EXPECT_EQ(*four->bytes, 16u);
  ASSERT_TRUE(four->startMask.has_value());
  EXPECT_EQ(*four->startMask, 0x10u);
}

TEST(RomFacts, AChannelWrittenAndNeverStartedHasNoStart) {
  const std::vector<std::uint8_t> rom = declaringImage();
  const CartridgeDisassembly disassembly = disassembled(rom);
  const DmaTransfer* six = transferAt(disassembly.dmas, 0x008098u);
  ASSERT_NE(six, nullptr);
  EXPECT_EQ(six->channel, 6u);
  EXPECT_EQ(six->direction, DmaDirection::ToBBus);
  EXPECT_EQ(six->destinationName, "VMDATAL");
  ASSERT_TRUE(six->step.has_value());
  EXPECT_EQ(*six->step, MovedStep::Increment);
  ASSERT_TRUE(six->source.has_value());
  EXPECT_EQ(*six->source, 0x009080u);
  ASSERT_TRUE(six->bytes.has_value());
  EXPECT_EQ(*six->bytes, 16u);
  EXPECT_FALSE(six->startMask.has_value());
  EXPECT_FALSE(six->startSite.has_value());
  EXPECT_EQ(transfersOn(disassembly.dmas, 6), 1u);
}

TEST(RomFacts, AnHdmaChannelsCountIsWhatTheCodeWroteThere) {
  const std::vector<std::uint8_t> rom = declaringImage();
  const CartridgeDisassembly disassembly = disassembled(rom);
  const DmaTransfer* five = transferAt(disassembly.dmas, 0x008207u);
  ASSERT_NE(five, nullptr);
  EXPECT_TRUE(five->hdma);
  EXPECT_EQ(five->destinationName, "CGDATA");
  ASSERT_TRUE(five->startMask.has_value());
  EXPECT_EQ(*five->startMask, 0x20u);
  ASSERT_TRUE(five->startSite.has_value());
  EXPECT_EQ(*five->startSite, 0x008225u);
  ASSERT_TRUE(five->bytes.has_value());
  EXPECT_EQ(*five->bytes, 3u) << "what the code wrote, which the engine overwrites";
  ASSERT_TRUE(five->source.has_value());
  EXPECT_EQ(*five->source, 0x009700u);
}

// A count is two registers; one of them proven is no count, as one of three
// address registers is no source.
TEST(RomFacts, ACountMissingHalfIsNoCount) {
  const auto write = [](Address site, Address reg, std::string_view name, RegisterClass cls,
                        std::uint8_t value) {
    return HardwareAccess{.site = site,
                          .registerAddress = reg,
                          .name = name,
                          .cls = cls,
                          .kind = AccessKind::Write,
                          .value = value,
                          .run = 1u};
  };
  std::vector<HardwareAccess> accesses = {
      write(0x008000u, 0x4301u, "BBAD0", RegisterClass::DmaChannel, 0x18u),
      write(0x008003u, 0x4305u, "DAS0L", RegisterClass::DmaChannel, 0x20u),
      write(0x008006u, 0x420Bu, "MDMAEN", RegisterClass::DmaControl, 0x01u),
  };
  ASSERT_EQ(dmaTransfers(accesses).size(), 1u);
  EXPECT_FALSE(dmaTransfers(accesses)[0].bytes.has_value()) << "the high byte was never written";

  accesses.insert(accesses.begin() + 2, write(0x008005u, 0x4306u, "DAS0H", RegisterClass::DmaChannel, 0x02u));
  ASSERT_EQ(dmaTransfers(accesses).size(), 1u);
  ASSERT_TRUE(dmaTransfers(accesses)[0].bytes.has_value());
  EXPECT_EQ(*dmaTransfers(accesses)[0].bytes, 544u);
}

// The engine's own registers — the table's current address, the line counter —
// are the channel's too, and a write to them describes no transfer and changes
// none.
TEST(RomFacts, TheEnginesOwnRegistersAreNotATransfersFields) {
  const auto write = [](Address site, Address reg, std::string_view name, RegisterClass cls,
                        std::uint8_t value) {
    return HardwareAccess{.site = site,
                          .registerAddress = reg,
                          .name = name,
                          .cls = cls,
                          .kind = AccessKind::Write,
                          .value = value,
                          .run = 1u};
  };
  const std::vector<HardwareAccess> accesses = {
      write(0x008000u, 0x4301u, "BBAD0", RegisterClass::DmaChannel, 0x18u),
      write(0x008003u, 0x4308u, "A2A0L", RegisterClass::DmaChannel, 0x34u),
      write(0x008006u, 0x430Au, "NTRL0", RegisterClass::DmaChannel, 0x7Fu),
      write(0x008009u, 0x420Bu, "MDMAEN", RegisterClass::DmaControl, 0x01u),
      write(0x00800Cu, 0x4318u, "A2A1L", RegisterClass::DmaChannel, 0x34u),
      write(0x00800Fu, 0x420Bu, "MDMAEN", RegisterClass::DmaControl, 0x02u),
  };
  const std::vector<DmaTransfer> dmas = dmaTransfers(accesses);
  ASSERT_EQ(dmas.size(), 1u) << "channel 1 had nothing but the engine's own register written";
  EXPECT_EQ(dmas[0].channel, 0u);
  EXPECT_EQ(dmas[0].site, 0x008000u);
  EXPECT_EQ(dmas[0].destinationName, "VMDATAL");
  EXPECT_EQ(dmas[0].direction, DmaDirection::Unknown);
  EXPECT_FALSE(dmas[0].source.has_value());
  EXPECT_FALSE(dmas[0].bytes.has_value());
  ASSERT_TRUE(dmas[0].startSite.has_value());
  EXPECT_EQ(*dmas[0].startSite, 0x008009u);
}

// A channel started with neither its direction nor its destination known in
// the run is no transfer of that run: the line could say nothing but the
// start. With the destination written in the run, the start is a transfer,
// sited at the write that proved the destination.
TEST(RomFacts, AStartWithNeitherDirectionNorDestinationKnownIsNoTransfer) {
  const auto write = [](Address site, Address reg, std::string_view name, RegisterClass cls,
                        std::uint8_t value, std::uint32_t run) {
    return HardwareAccess{.site = site,
                          .registerAddress = reg,
                          .name = name,
                          .cls = cls,
                          .kind = AccessKind::Write,
                          .value = value,
                          .run = run};
  };
  std::vector<HardwareAccess> accesses = {
      write(0x008000u, 0x4301u, "BBAD0", RegisterClass::DmaChannel, 0x18u, 1u),
      write(0x008010u, 0x4302u, "A1T0L", RegisterClass::DmaChannel, 0x00u, 2u),
      write(0x008013u, 0x4303u, "A1T0H", RegisterClass::DmaChannel, 0x80u, 2u),
      write(0x008016u, 0x4304u, "A1B0", RegisterClass::DmaChannel, 0x01u, 2u),
      write(0x008019u, 0x420Bu, "MDMAEN", RegisterClass::DmaControl, 0x01u, 2u),
  };
  ASSERT_EQ(dmaTransfers(accesses).size(), 1u) << "the second run's start names nothing the line could state";

  accesses.push_back(write(0x008018u, 0x4301u, "BBAD0", RegisterClass::DmaChannel, 0x22u, 2u));
  std::sort(accesses.begin(), accesses.end(),
            [](const HardwareAccess& a, const HardwareAccess& b) { return a.site < b.site; });
  const std::vector<DmaTransfer> dmas = dmaTransfers(accesses);
  ASSERT_EQ(dmas.size(), 2u);
  EXPECT_EQ(dmas[1].site, 0x008018u) << "the write that proved the destination";
  EXPECT_EQ(dmas[1].destinationName, "CGDATA");
  ASSERT_TRUE(dmas[1].source.has_value());
  EXPECT_EQ(*dmas[1].source, 0x018000u);
  ASSERT_TRUE(dmas[1].startSite.has_value());
  EXPECT_EQ(*dmas[1].startSite, 0x008019u);
}

// A start with no register written since the last is the same set-up started
// again, under the same site.
TEST(RomFacts, ASecondStartOfTheSameSetUpIsATransferUnderTheSameSite) {
  const auto write = [](Address site, Address reg, std::string_view name, RegisterClass cls,
                        std::uint8_t value) {
    return HardwareAccess{.site = site,
                          .registerAddress = reg,
                          .name = name,
                          .cls = cls,
                          .kind = AccessKind::Write,
                          .value = value,
                          .run = 1u};
  };
  const std::vector<HardwareAccess> accesses = {
      write(0x008000u, 0x4301u, "BBAD0", RegisterClass::DmaChannel, 0x18u),
      write(0x008003u, 0x420Bu, "MDMAEN", RegisterClass::DmaControl, 0x01u),
      write(0x008006u, 0x420Bu, "MDMAEN", RegisterClass::DmaControl, 0x01u),
  };
  const std::vector<DmaTransfer> dmas = dmaTransfers(accesses);
  ASSERT_EQ(dmas.size(), 2u);
  EXPECT_EQ(dmas[0].site, 0x008000u);
  EXPECT_EQ(dmas[1].site, 0x008000u);
  ASSERT_TRUE(dmas[0].startSite.has_value());
  ASSERT_TRUE(dmas[1].startSite.has_value());
  EXPECT_EQ(*dmas[0].startSite, 0x008003u);
  EXPECT_EQ(*dmas[1].startSite, 0x008006u);
}

// Every transfer the run started from a proven set-up is the range the run
// moved from that start: the lifting cartridge's sixteen general-purpose
// starts in one straight run, and its one HDMA enable, are seventeen
// transfers, each general-purpose one equal to its twin.
TEST(RomFacts, EveryTransferTheRunStartedAgreesWithTheRangeItMoved) {
  const std::vector<std::uint8_t> rom = liftingImage();
  CartridgeRequest request;
  request.rom = rom;
  request.captureSound = false;
  request.observeRun = true;
  request.runMasterCycles = 3u * 357'954u;
  const CartridgeDisassembly disassembly = disassembleCartridge(request);
  EXPECT_EQ(disassembly.dmas.size(), 17u) << renderManifest(disassembly);
  std::size_t twins = 0;
  for (const DmaTransfer& dma : disassembly.dmas) {
    if (!dma.startSite || dma.hdma) continue;
    for (const MovedRange& range : disassembly.moved) {
      if (range.site != *dma.startSite || range.channel != dma.channel) continue;
      ++twins;
      ASSERT_TRUE(dma.source.has_value()) << renderManifest(disassembly);
      EXPECT_EQ(*dma.source, range.memory);
      ASSERT_TRUE(dma.step.has_value());
      EXPECT_EQ(*dma.step, range.step);
      ASSERT_TRUE(dma.bytes.has_value());
      EXPECT_EQ(*dma.bytes, range.bytes);
    }
  }
  EXPECT_EQ(twins, 16u);
  for (const std::string& note : disassembly.notes) {
    EXPECT_EQ(note.find("the code proves"), std::string::npos) << note;
  }
}

// ---- the manifest -----------------------------------------------------------

TEST(RomFacts, TheDmaLineSaysTheStepTheCountAndTheStart) {
  const std::vector<std::uint8_t> rom = declaringImage();
  const CartridgeDisassembly disassembly = disassembled(rom);
  const std::string manifest = renderManifest(disassembly);
  EXPECT_NE(manifest.find("dma      $00:8007 channel 0 to-register $00:2118 VMDATAL Vram source $00:9000 "
                          "increment bytes 32 start $01 from $00:8025\n"),
            std::string::npos)
      << manifest;
  EXPECT_NE(manifest.find("dma      $00:802A channel 0 to-register $00:2118 VMDATAL Vram source $00:9040 "
                          "increment bytes 16 start $01 from $00:803E\n"),
            std::string::npos)
      << manifest;
  EXPECT_NE(manifest.find("dma      $00:8048 channel 0 to-register $00:2122 CGDATA Cgram source $00:920F "
                          "decrement bytes 16 start $01 from $00:8066\n"),
            std::string::npos)
      << manifest;
  EXPECT_NE(manifest.find("dma      $00:8098 channel 6 to-register $00:2118 VMDATAL Vram source $00:9080 "
                          "increment bytes 16 start none from none\n"),
            std::string::npos)
      << manifest;
  EXPECT_NE(manifest.find("dma      $00:8187 channel 3 to-register $00:2118 VMDATAL Vram source $00:9600 "
                          "fixed bytes 65536 start $08 from $00:81A5\n"),
            std::string::npos)
      << manifest;
  EXPECT_NE(manifest.find("dma      $00:81C7 channel 4 to-register $00:2118 VMDATAL Vram source none "
                          "increment bytes 16 start $10 from $00:81EA\n"),
            std::string::npos)
      << manifest;
  EXPECT_NE(manifest.find("dma      $00:8207 channel 5 to-register $00:2122 CGDATA Cgram source $00:9700 "
                          "increment bytes 3 start-hdma $20 from $00:8225\n"),
            std::string::npos)
      << manifest;
}

TEST(RomFacts, TheManifestCarriesEveryFactWithEveryFieldPresent) {
  const std::vector<std::uint8_t> rom = hardwareImage();
  const CartridgeDisassembly disassembly = disassembled(rom);
  const std::string manifest = renderManifest(disassembly);

  EXPECT_NE(manifest.find("access   $00:8006 INIDISP Display write $8F\n"), std::string::npos)
      << manifest;
  EXPECT_NE(manifest.find("access   $00:8009 RDNMI Interrupt read none\n"), std::string::npos)
      << manifest;
  EXPECT_NE(manifest.find("access   $00:804A DMAP0 DmaChannel read-write none\n"), std::string::npos)
      << manifest;
  EXPECT_NE(
      manifest.find("dma      $00:8016 channel 0 to-register $00:2118 VMDATAL Vram source $01:8000 "
                    "increment bytes none start $01 from $00:802A\n"),
      std::string::npos)
      << manifest;
  EXPECT_NE(manifest.find("dma      $00:808A channel 2 direction-unknown $00:2133 SETINI Display source none "
                          "none bytes none start none from none\n"),
            std::string::npos)
      << manifest;
  EXPECT_NE(manifest.find("dma      $00:808F channel 3 to-register none none none source none "
                          "increment bytes none start-hdma $08 from $00:8094\n"),
            std::string::npos)
      << manifest;
}

TEST(RomFacts, TheNewKindsParseAndAnUnknownKindIsStillAnError) {
  const std::vector<std::uint8_t> rom = hardwareImage();
  const CartridgeDisassembly disassembly = disassembled(rom);
  std::string error;

  // A manifest carrying the new kinds is read by the tools that read manifests:
  // they pass over them, because nothing is read back from them.
  const std::optional<ManifestInput> input = parseManifest(renderManifest(disassembly), error);
  ASSERT_TRUE(input.has_value()) << error;

  // The stability rule is unchanged: a kind a reader does not know is an error,
  // never a line to pass over.
  EXPECT_FALSE(parseManifest("role $00:8000 Vram\n", error).has_value());
  EXPECT_NE(error.find("not a manifest line"), std::string::npos);
}

// The facts ride in the manifest and nowhere else, so a tree that verified before
// them verifies after them.
TEST(RomFacts, TheTreeStillAssemblesToItsImage) {
  const std::vector<std::uint8_t> rom = threeBankImage();
  const CartridgeDisassembly disassembly = disassembled(rom);

  std::map<std::string, std::string> tree;
  for (const RegionListing& region : disassembly.regions) {
    tree[region.region.file] = renderRegion(region, disassembly);
  }
  std::string error;
  const std::optional<ManifestInput> manifest = parseManifest(renderManifest(disassembly), error);
  ASSERT_TRUE(manifest.has_value()) << error;

  const VerifyReport report = verifyProject(*manifest, rom, [&tree](const std::string& file) {
    const auto found = tree.find(file);
    if (found == tree.end()) return std::optional<std::string>{};
    return std::optional<std::string>{found->second};
  });
  EXPECT_TRUE(report.error.empty()) << report.error;
  EXPECT_TRUE(report.identical()) << renderReport(report);
  EXPECT_EQ(report.differing, 0u);
  EXPECT_EQ(report.unplaced, 0u);
  EXPECT_EQ(report.placedTwice, 0u);
}

}  // namespace snaggletooth::disasm
