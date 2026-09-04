// SNES assembly from the instruction layer, and the bank files written from it.
//
// The first cases hold the renderer to the listing over every example
// cartridge: the bytes it writes back from a node are the bytes the node was
// lifted from, the text it writes with no names is the text the listing carries,
// the cost reads the same, and the directives land where the listing put them.
// The rest pin what the bank file adds — the names in place of addresses, the
// prologue that defines them, the routine headers — and that every example tree
// still assembles back to its image.

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "cpu65816_disasm.h"
#include "disasm/disasm.h"
#include "examples/example_cartridges.h"
#include "gtest/gtest.h"
#include "ir/cpu65816_lift.h"
#include "ir/ir.h"
#include "ir/ir_render.h"
#include "rom/rom_disasm.h"
#include "rom/rom_verify.h"

namespace snaggletooth::ir {
namespace {

using disasm::CartridgeDisassembly;
using disasm::CartridgeRequest;
using disasm::Cpu65816Mode;
using disasm::Line;
using disasm::Listing;
using disasm::RegionListing;
using examples::loRomImage;
using examples::put;

CartridgeDisassembly disassemble(std::span<const std::uint8_t> rom, bool sound = false) {
  CartridgeRequest request;
  request.rom = rom;
  request.captureSound = sound;
  return disasm::disassembleCartridge(request);
}

const RegionListing& regionNamed(const CartridgeDisassembly& d, const std::string& file) {
  for (const RegionListing& region : d.regions) {
    if (region.region.file == file) return region;
  }
  ADD_FAILURE() << "no region " << file;
  return d.regions.front();
}

std::string bankFile(const CartridgeDisassembly& d, const std::string& file) {
  return disasm::renderRegion(regionNamed(d, file), d);
}

// One instruction placed at `address`, decoded under `mode` and lifted.
Node nodeOf(std::vector<std::uint8_t> bytes, Address address, const Cpu65816Mode& mode,
            bool patched = false) {
  const std::optional<disasm::Instruction> decoded =
      disasm::decodeAt(bytes, address, address, mode);
  EXPECT_TRUE(decoded.has_value());
  return liftInstruction(*decoded, mode, patched);
}

// The listing's cost as its text: base, `/taken` for a conditional branch, `?`
// where it is not known.
std::string listingCost(const disasm::CycleCost& cost) {
  if (!cost.known) return "?";
  std::string text = std::to_string(cost.base);
  if (cost.taken != 0) text += "/" + std::to_string(cost.taken);
  return text;
}

// Every code line of every region of every example cartridge, each beside the
// node lifted from it.
template <typename Visit>
void everyExampleLine(Visit visit) {
  std::size_t lines = 0;
  for (const examples::Example& example : examples::examples()) {
    const std::vector<std::uint8_t> rom = example.build();
    const CartridgeDisassembly d = disassemble(rom);
    for (const RegionListing& region : d.regions) {
      const Program program = lift65816(region.listing);
      std::size_t index = 0;
      for (const Line& line : region.listing.lines) {
        if (!line.isCode) continue;
        ASSERT_LT(index, program.nodes.size());
        visit(std::string(example.name), line, program.nodes[index++]);
        ++lines;
      }
    }
  }
  EXPECT_GT(lines, 200u);
}

// A tree as text, verified in memory the way the verifier reads one from disk.
disasm::VerifyReport verifyInMemory(const CartridgeDisassembly& d, std::span<const std::uint8_t> rom) {
  std::map<std::string, std::string> files;
  for (const RegionListing& region : d.regions) {
    files[region.region.file] = disasm::renderRegion(region, d);
  }
  if (d.sound) files[d.sound->file] = disasm::renderSoundProgram(*d.sound);
  std::string error;
  const std::optional<disasm::ManifestInput> manifest =
      disasm::parseManifest(disasm::renderManifest(d), error);
  EXPECT_TRUE(manifest.has_value()) << error;
  if (!manifest) return disasm::VerifyReport{};
  return disasm::verifyProject(*manifest, rom, [&](const std::string& file) -> std::optional<std::string> {
    const auto found = files.find(file);
    if (found == files.end()) return std::nullopt;
    return found->second;
  });
}

// ---- the renderer against the listing -----------------------------------------------

TEST(Render, EveryExampleNodeEncodesToTheBytesItWasLiftedFrom) {
  everyExampleLine([](const std::string& example, const Line& line, const Node& node) {
    EXPECT_EQ(encode(node.instruction), line.instruction.bytes)
        << example << " " << line.instruction.text;
    EXPECT_EQ(opcodeOf(node.instruction), line.instruction.opcode) << example;
  });
}

TEST(Render, EveryExampleNodeRendersTheListingsOwnText) {
  everyExampleLine([](const std::string& example, const Line& line, const Node& node) {
    EXPECT_EQ(renderInstruction(node.instruction), line.instruction.text) << example;
  });
}

TEST(Render, EveryExampleNodeCostsWhatTheListingPrints) {
  everyExampleLine([](const std::string& example, const Line& line, const Node& node) {
    EXPECT_EQ(renderCost(node), listingCost(line.instruction.cycles))
        << example << " " << line.instruction.text;
  });
}

// The directives are judged in file order, from the start of every piece and
// after every run of data, which is how the assembler reads them; over the
// examples that lands them exactly where the listing did.
TEST(Render, DirectivesLandWhereTheListingPutThem) {
  for (const examples::Example& example : examples::examples()) {
    const std::vector<std::uint8_t> rom = example.build();
    const CartridgeDisassembly d = disassemble(rom);
    for (const RegionListing& region : d.regions) {
      const Program program = lift65816(region.listing);
      SourceMode mode;
      std::size_t index = 0;
      for (const Line& line : region.listing.lines) {
        if (!line.isCode) {
          mode.reset();
          continue;
        }
        EXPECT_EQ(mode.directives(program.nodes[index++]), line.directives)
            << example.name << " " << line.instruction.text;
      }
    }
  }
}

TEST(Render, ATargetWithALabelIsWrittenBehindItsMarker) {
  const Cpu65816Mode mode = Cpu65816Mode::native(true, true);
  SourceNames names;
  names.target = "sub_008040";
  EXPECT_EQ(renderInstruction(nodeOf({0x20u, 0x40u, 0x80u}, 0x008000u, mode).instruction, names),
            "JSR !sub_008040");
  EXPECT_EQ(renderInstruction(nodeOf({0x4Cu, 0x40u, 0x80u}, 0x008000u, mode).instruction, names),
            "JMP !sub_008040");
  EXPECT_EQ(renderInstruction(nodeOf({0x22u, 0x40u, 0x80u, 0x01u}, 0x008000u, mode).instruction, names),
            "JSL >sub_008040");
  EXPECT_EQ(renderInstruction(nodeOf({0x5Cu, 0x40u, 0x80u, 0x01u}, 0x008000u, mode).instruction, names),
            "JML >sub_008040");
  EXPECT_EQ(renderInstruction(nodeOf({0xD0u, 0x3Eu}, 0x008000u, mode).instruction, names),
            "BNE sub_008040");
  EXPECT_EQ(renderInstruction(nodeOf({0x82u, 0x3Du, 0x00u}, 0x008000u, mode).instruction, names),
            "BRL sub_008040");
  // A form whose target the bytes do not name has no label to write.
  EXPECT_EQ(renderInstruction(nodeOf({0x7Cu, 0x40u, 0x80u}, 0x008000u, mode).instruction, names),
            "JMP (!$8040,X)");
}

TEST(Render, ARegisterNameStandsForAnAbsoluteDataOperand) {
  const Cpu65816Mode mode = Cpu65816Mode::native(true, true);
  SourceNames names;
  names.operand = "INIDISP";
  EXPECT_EQ(renderInstruction(nodeOf({0x8Du, 0x00u, 0x21u}, 0x008000u, mode).instruction, names),
            "STA !INIDISP");
  EXPECT_EQ(renderInstruction(nodeOf({0x9Du, 0x00u, 0x21u}, 0x008000u, mode).instruction, names),
            "STA !INIDISP,X");
  EXPECT_EQ(renderInstruction(nodeOf({0xB9u, 0x00u, 0x21u}, 0x008000u, mode).instruction, names),
            "LDA !INIDISP,Y");
  // A long operand keeps its address: the name rides in the comment.
  const Node longForm = nodeOf({0x8Fu, 0x00u, 0x21u, 0x00u}, 0x008000u, mode);
  EXPECT_EQ(renderInstruction(longForm.instruction, names), "STA $00:2100");
  EXPECT_NE(renderLine(longForm, {}, 12).find("  INIDISP\n"), std::string::npos);
  // A jump through an absolute operand leaves for it; it is not a data operand.
  EXPECT_EQ(renderInstruction(nodeOf({0x4Cu, 0x00u, 0x21u}, 0x008000u, mode).instruction, names),
            "JMP !$2100");
}

TEST(Render, ALineCarriesTheAddressTheBytesTheCostAndTheNote) {
  const Cpu65816Mode mode = Cpu65816Mode::native(true, true);
  SourceNames names;
  names.annotation = "NMITIMEN";
  EXPECT_EQ(renderLine(nodeOf({0x8Du, 0x00u, 0x42u}, 0x008020u, mode), names, 9),
            "        STA !$4200                      ; $00:8020  8D 00 42  4  NMITIMEN\n");
  EXPECT_EQ(renderLine(nodeOf({0xD0u, 0xF6u}, 0x00802Bu, mode), {}, 9),
            "        BNE $00:8023                    ; $00:802B  D0 F6     2/3\n");
  const Node patched = nodeOf({0xEAu}, 0x7E0100u, mode, true);
  EXPECT_EQ(renderLine(patched, {}, 9),
            "        NOP                             ; $7E:0100  EA        2  PATCHED at run time\n");
}

TEST(Render, TheCostIsUnknownWhereAWidthTheTraceDidNotKnowDecidesIt) {
  const Cpu65816Mode unknown = Cpu65816Mode::nativeUnknown();
  EXPECT_EQ(renderCost(nodeOf({0xADu, 0x04u, 0x01u}, 0x008000u, unknown)), "?");  // LDA abs
  EXPECT_EQ(renderCost(nodeOf({0x40u}, 0x008000u, unknown)), "7");                 // RTI
  EXPECT_EQ(renderCost(nodeOf({0xADu, 0x04u, 0x01u}, 0x008000u, Cpu65816Mode::reset())), "4");
}

// ---- the bank file --------------------------------------------------------------

TEST(Render, ThePrologueNamesWhatTheFileUsesAndNothingElse) {
  const std::vector<std::uint8_t> mixed = examples::mixedImage();
  const std::string bank0 = bankFile(disassemble(mixed), "bank_00.asm");
  const std::size_t header = bank0.find("; The hardware registers this file names");
  ASSERT_NE(header, std::string::npos);
  EXPECT_EQ(bank0.find("NMITIMEN  EQU $4200\nRDNMI     EQU $4210\n\n        ORG $00:8000\n"),
            bank0.find("NMITIMEN"));
  EXPECT_EQ(bank0.find("INIDISP"), std::string::npos) << "a register the file never names";
  EXPECT_EQ(bank0.find("sub_008040  EQU"), std::string::npos) << "a label the file itself defines";

  const std::vector<std::uint8_t> three = examples::threeBankImage();
  const CartridgeDisassembly d = disassemble(three);
  const std::string bank0three = bankFile(d, "bank_00.asm");
  EXPECT_NE(bank0three.find("sub_018000  EQU $018000\n"), std::string::npos) << bank0three;
  EXPECT_EQ(bank0three.find("EQU $4"), std::string::npos) << "no register is named in bank $00";
  EXPECT_NE(bankFile(d, "bank_01.asm").find("sub_008100  EQU $008100\n"), std::string::npos);
  // Bank $02 leaves for work RAM and through a table: nothing to define.
  EXPECT_EQ(bankFile(d, "bank_02.asm").find("EQU"), std::string::npos);
  EXPECT_EQ(bankFile(d, "bank_02.asm").find("; The hardware registers"), std::string::npos);
}

TEST(Render, ACallIntoAnotherFileIsWrittenAsItsLabel) {
  const std::vector<std::uint8_t> three = examples::threeBankImage();
  const CartridgeDisassembly d = disassemble(three);
  const std::string bank0 = bankFile(d, "bank_00.asm");
  EXPECT_NE(bank0.find("        JSL >sub_018000                 ; $00:8004  22 00 80 01  8\n"),
            std::string::npos) << bank0;
  EXPECT_EQ(bank0.find("JSL $01:8000"), std::string::npos);
  // The jump through the mirror keeps its address: the label is at the bank the
  // bytes are placed in, and the bytes name bank $82, which a symbol would not.
  EXPECT_NE(bank0.find("        JML $82:8000                    ; $00:8008  5C 00 80 82  4\n"),
            std::string::npos) << bank0;
  EXPECT_EQ(bank0.find("loc_028000"), std::string::npos);
  EXPECT_NE(bankFile(d, "bank_01.asm").find("JSL >sub_008100"), std::string::npos);
}

TEST(Render, ARegisterOperandInTheFileIsWrittenAsItsName) {
  const std::vector<std::uint8_t> mixed = examples::mixedImage();
  const std::string bank0 = bankFile(disassemble(mixed), "bank_00.asm");
  EXPECT_NE(bank0.find("        STA !NMITIMEN                   ; $00:8020  8D 00 42  4\n"),
            std::string::npos) << bank0;
  EXPECT_NE(bank0.find("        LDA !RDNMI                      ; $00:8300  AD 10 42  ?\n"),
            std::string::npos) << bank0;
  EXPECT_EQ(bank0.find("!$4200"), std::string::npos);
}

TEST(Render, ARegisterWhoseNameCannotBeASymbolStaysAnAddress) {
  std::vector<std::uint8_t> rom = loRomImage(1);
  put(rom, 0x0000u, {0xADu, 0x16u, 0x40u,   // $8000 LDA !$4016  JOYSER0/JOYOUT
                     0xDBu});               // STP
  const std::string bank0 = bankFile(disassemble(rom), "bank_00.asm");
  EXPECT_NE(bank0.find("        LDA !$4016                      ; $00:8000  AD 16 40  4  JOYSER0/JOYOUT\n"),
            std::string::npos) << bank0;
  EXPECT_EQ(bank0.find("EQU"), std::string::npos);
}

TEST(Render, ARegisterIsNotNamedWhereTheFileDefinesALabelOfThatName) {
  std::vector<std::uint8_t> rom = loRomImage(1);
  put(rom, 0x0000u, {0x8Du, 0x00u, 0x21u,   // $8000 STA !$2100  INIDISP
                     0x4Cu, 0x10u, 0x80u,   // $8003 JMP !$8010
                     0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
                     0xDBu});               // $8010 STP
  CartridgeRequest request;
  request.rom = rom;
  request.captureSound = false;
  request.entries.push_back(
      disasm::TraceEntry{.address = 0x008010u, .mode = Cpu65816Mode::reset(), .name = "INIDISP"});
  const CartridgeDisassembly d = disasm::disassembleCartridge(request);
  const std::string bank0 = bankFile(d, "bank_00.asm");
  EXPECT_NE(bank0.find("\nINIDISP:\n"), std::string::npos) << bank0;
  EXPECT_NE(bank0.find("        STA !$2100                      ; $00:8000  8D 00 21  4  INIDISP\n"),
            std::string::npos) << bank0;
  EXPECT_EQ(bank0.find("EQU"), std::string::npos);
  EXPECT_NE(bank0.find("JMP !INIDISP"), std::string::npos) << "the label, not the register";
}

TEST(Render, ARoutineHeaderNamesItsSizeRoleCallsAndCallers) {
  std::vector<std::uint8_t> rom = loRomImage(1);
  put(rom, 0x0000u, {0xA9u, 0x8Fu,          // $8000 LDA #$8F
                     0x8Du, 0x00u, 0x21u,   // $8002 STA !$2100  INIDISP, Display
                     0x20u, 0x40u, 0x80u,   // $8005 JSR !$8040
                     0xDBu});               // $8008 STP
  put(rom, 0x0040u, {0xA9u, 0x04u,          // $8040 LDA #$04
                     0x8Du, 0x01u, 0x43u,   // $8042 STA !$4301  BBAD0 = $04: OAMDATA
                     0x60u});               // $8045 RTS
  const std::string bank0 = bankFile(disassemble(rom), "bank_00.asm");
  EXPECT_NE(bank0.find("\n; routine reset: 4 lines, 9 bytes\n"
                       ";   reaches Display; through Oam, DmaChannel\n"
                       ";   calls sub_008040; called by none\n"
                       "reset:\n"),
            std::string::npos) << bank0;
  EXPECT_NE(bank0.find("\n; routine sub_008040: 3 lines, 6 bytes\n"
                       ";   reaches Oam, DmaChannel; through none\n"
                       ";   calls none; called by reset\n"
                       "sub_008040:\n"),
            std::string::npos) << bank0;
  EXPECT_NE(bank0.find("INIDISP  EQU $2100\nBBAD0    EQU $4301\n"), std::string::npos) << bank0;
}

TEST(Render, AnAddressReadTwoWaysIsRenderedAsTheFirstReading) {
  std::vector<std::uint8_t> rom = loRomImage(1);
  put(rom, 0x0000u, {0x18u, 0xFBu,          // $8000 CLC ; XCE
                     0xC2u, 0x20u,          // $8002 REP #$20      m=16
                     0x20u, 0x20u, 0x80u,   // $8004 JSR !$8020    read with m=16 first
                     0xE2u, 0x20u,          // $8007 SEP #$20      m=8
                     0x20u, 0x20u, 0x80u,   // $8009 JSR !$8020    read with m=8 second
                     0xDBu});               // $800C STP
  put(rom, 0x0020u, {0xA9u, 0x01u, 0x00u,   // $8020 LDA #$0001 under m=16; LDA #$01 / BRK under m=8
                     0x60u});               // $8023 RTS
  const CartridgeDisassembly d = disassemble(rom);
  const std::string bank0 = bankFile(d, "bank_00.asm");
  EXPECT_NE(bank0.find("; warning: $00:8020 is reached with e=0 m=16 x=8 and with e=0 m=8 x=8\n"),
            std::string::npos) << bank0;
  EXPECT_NE(bank0.find("        LDA #$0001                      ; $00:8020  A9 01 00  3\n"),
            std::string::npos) << bank0;
  EXPECT_EQ(bank0.find("LDA #$01 "), std::string::npos);
}

TEST(Render, ARunOfDataIsWrittenAsTheFrameworkWritesIt) {
  std::vector<std::uint8_t> rom = loRomImage(2);
  put(rom, 0x0000u, {0xDBu});                       // STP: nothing reaches bank $01
  put(rom, 0x8000u, {'H', 'e', 'l', 'l', 'o', 0x00u, 0xA9u, 0x12u, 0xFFu});
  const CartridgeDisassembly d = disassemble(rom);
  // A copy, not a reference: a reference bound past a temporary argument is what
  // one compiler's dangling-reference check refuses.
  const RegionListing bank1 = regionNamed(d, "bank_01.asm");
  EXPECT_EQ(disasm::renderRegion(bank1, d), disasm::render(bank1.listing));
}

TEST(Render, EveryExampleTreeAssemblesBackToItsImage) {
  for (const examples::Example& example : examples::examples()) {
    const std::vector<std::uint8_t> rom = example.build();
    const bool uploads = example.name == "uploading" || example.name == "straddling_upload";
    const CartridgeDisassembly d = disassemble(rom, uploads);
    const disasm::VerifyReport report = verifyInMemory(d, rom);
    EXPECT_TRUE(report.identical()) << example.name << "\n" << disasm::renderReport(report);
    EXPECT_EQ(report.image, rom) << example.name;
  }
}

}  // namespace
}  // namespace snaggletooth::ir
