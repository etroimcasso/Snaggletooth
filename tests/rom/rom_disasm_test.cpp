// A whole cartridge disassembled into a source tree. The images are the
// hand-built cartridges of `examples/example_cartridges.h` — a LoROM header, a few
// banks of 65816 code that call and jump across them, and one whose reset code
// speaks the audio upload protocol — so each case pins one rule about how the
// tree is traced, split, matched to the image, and written.

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "examples/example_cartridges.h"
#include "gtest/gtest.h"
#include "rom/rom_disasm.h"

namespace snaggletooth::disasm {
namespace {

using examples::kUploadedProgram;
using examples::kUploadedTable;
using examples::liftingImage;
using examples::loRomImage;
using examples::ramCodeImage;
using examples::wrappingImage;
using examples::put;
using examples::threeBankImage;
using examples::uploadingImage;

CartridgeDisassembly disassembleWithoutSound(std::span<const std::uint8_t> rom) {
  CartridgeRequest request;
  request.rom = rom;
  request.captureSound = false;
  return disassembleCartridge(request);
}

const RegionListing& regionNamed(const CartridgeDisassembly& d, const std::string& file) {
  for (const RegionListing& region : d.regions) {
    if (region.region.file == file) return region;
  }
  ADD_FAILURE() << "no region " << file;
  return d.regions.front();
}

const Line* codeLineAt(const Listing& listing, Address address) {
  for (const Line& line : listing.lines) {
    if (line.isCode && line.address == address) return &line;
  }
  return nullptr;
}

}  // namespace

// ---- the file split ---------------------------------------------------------

TEST(RomDisasm, OneRegionPerBankUnderLoRom) {
  const std::vector<SourceRegion> regions = bankRegions(CartridgeMap::LoRom, 512u * 1024u);
  ASSERT_EQ(regions.size(), 16u);
  EXPECT_EQ(regions[0].file, "bank_00.asm");
  EXPECT_EQ(regions[0].first, 0x008000u);
  EXPECT_EQ(regions[0].last, 0x00FFFFu);
  EXPECT_EQ(regions[15].file, "bank_0F.asm");
  EXPECT_EQ(regions[15].first, 0x0F8000u);
  EXPECT_EQ(regions[15].last, 0x0FFFFFu);
}

TEST(RomDisasm, OneRegionPerBankUnderHiRom) {
  const std::vector<SourceRegion> regions = bankRegions(CartridgeMap::HiRom, 1024u * 1024u);
  ASSERT_EQ(regions.size(), 16u);
  EXPECT_EQ(regions[0].file, "bank_C0.asm");
  EXPECT_EQ(regions[0].first, 0xC00000u);
  EXPECT_EQ(regions[0].last, 0xC0FFFFu);
  EXPECT_EQ(regions[15].file, "bank_CF.asm");
}

TEST(RomDisasm, ExHiRomSecondHalfLandsInBank40Onward) {
  const std::vector<SourceRegion> regions = bankRegions(CartridgeMap::ExHiRom, 6u * 1024u * 1024u);
  ASSERT_EQ(regions.size(), 96u);
  EXPECT_EQ(regions[63].file, "bank_FF.asm");
  EXPECT_EQ(regions[64].file, "bank_40.asm");
  EXPECT_EQ(regions[64].first, 0x400000u);
  EXPECT_EQ(regions[95].first, 0x5F0000u);
}

// ---- the trace across banks -------------------------------------------------

TEST(RomDisasm, TheResetVectorNamesItsHandler) {
  const std::vector<std::uint8_t> rom = threeBankImage();
  const CartridgeDisassembly d = disassembleWithoutSound(rom);
  ASSERT_EQ(d.regions.size(), 3u);
  ASSERT_FALSE(d.entries.empty());
  EXPECT_EQ(d.entries[0].name, "reset");
  EXPECT_EQ(d.entries[0].address, 0x008000u);
  EXPECT_EQ(d.entries[0].mode, Cpu65816Mode::reset());
  const Listing& bank0 = regionNamed(d, "bank_00.asm").listing;
  ASSERT_TRUE(bank0.labels.count(0x008000u));
  EXPECT_EQ(bank0.labels.at(0x008000u), "reset");
  EXPECT_TRUE(d.notes.empty());
}

TEST(RomDisasm, ACallIntoAnotherBankTracesThatBank) {
  const std::vector<std::uint8_t> rom = threeBankImage();
  const CartridgeDisassembly d = disassembleWithoutSound(rom);
  const Listing& bank1 = regionNamed(d, "bank_01.asm").listing;
  const Line* lda = codeLineAt(bank1, 0x018000u);
  ASSERT_NE(lda, nullptr);
  EXPECT_EQ(lda->instruction.text, "LDA #$1234");
  ASSERT_TRUE(bank1.labels.count(0x018000u));
  EXPECT_EQ(bank1.labels.at(0x018000u), "sub_018000");
}

TEST(RomDisasm, TheModeCrossesWithTheCall) {
  // REP #$30 in bank $00 is what makes the immediate in bank $01 three bytes.
  const std::vector<std::uint8_t> rom = threeBankImage();
  const CartridgeDisassembly d = disassembleWithoutSound(rom);
  const Line* lda = codeLineAt(regionNamed(d, "bank_01.asm").listing, 0x018000u);
  ASSERT_NE(lda, nullptr);
  EXPECT_EQ(lda->instruction.length, 3u);
  // SEP #$20 in bank $02 narrows it again; the index stays sixteen.
  const Line* narrow = codeLineAt(regionNamed(d, "bank_02.asm").listing, 0x028002u);
  ASSERT_NE(narrow, nullptr);
  EXPECT_EQ(narrow->instruction.text, "LDA #$12");
  EXPECT_EQ(narrow->instruction.length, 2u);
  EXPECT_EQ(modeOf(narrow->context), Cpu65816Mode::native(true, false));
}

TEST(RomDisasm, ACallBackIntoAnEarlierBankTracesItAgain) {
  // Bank $00 is traced before bank $01 names $00:8100; the trace goes round
  // again until no bank has a new entry.
  const std::vector<std::uint8_t> rom = threeBankImage();
  const CartridgeDisassembly d = disassembleWithoutSound(rom);
  const Listing& bank0 = regionNamed(d, "bank_00.asm").listing;
  const Line* nop = codeLineAt(bank0, 0x008100u);
  ASSERT_NE(nop, nullptr);
  EXPECT_EQ(nop->instruction.text, "NOP");
  EXPECT_EQ(bank0.labels.at(0x008100u), "sub_008100");
}

TEST(RomDisasm, AJumpThroughAMirrorLandsInTheHomeBank) {
  const std::vector<std::uint8_t> rom = threeBankImage();
  const CartridgeDisassembly d = disassembleWithoutSound(rom);
  const Listing& bank2 = regionNamed(d, "bank_02.asm").listing;
  const Line* sep = codeLineAt(bank2, 0x028000u);
  ASSERT_NE(sep, nullptr);
  EXPECT_EQ(sep->instruction.text, "SEP #$20");
  ASSERT_TRUE(bank2.labels.count(0x028000u));
  EXPECT_EQ(bank2.labels.at(0x028000u), "loc_028000");
}

TEST(RomDisasm, TheStopsNameWhereTheBytesRunOut) {
  const std::vector<std::uint8_t> rom = threeBankImage();
  const CartridgeDisassembly d = disassembleWithoutSound(rom);
  ASSERT_EQ(d.stops.size(), 2u);
  EXPECT_EQ(d.stops[0].address, 0x028004u);
  EXPECT_NE(d.stops[0].reason.find("JSL $7E:2000"), std::string::npos);
  EXPECT_NE(d.stops[0].reason.find("work RAM"), std::string::npos);
  EXPECT_EQ(d.stops[1].address, 0x028008u);
  EXPECT_NE(d.stops[1].reason.find("JMP (!$8100,X)"), std::string::npos);
  EXPECT_NE(d.stops[1].reason.find("computed at run time"), std::string::npos);
  // The bytes past the table jump were never reached, so they are data.
  EXPECT_EQ(codeLineAt(regionNamed(d, "bank_02.asm").listing, 0x028100u), nullptr);
}

TEST(RomDisasm, AnEntryAPersonAddsIsTracedUnderItsMode) {
  const std::vector<std::uint8_t> rom = threeBankImage();
  CartridgeRequest request;
  request.rom = rom;
  request.captureSound = false;
  request.entries.push_back(TraceEntry{.address = 0x028100u,
                                       .mode = Cpu65816Mode::native(true, false),
                                       .name = "table_target"});
  const CartridgeDisassembly d = disassembleCartridge(request);
  const Listing& bank2 = regionNamed(d, "bank_02.asm").listing;
  const Line* nop = codeLineAt(bank2, 0x028100u);
  ASSERT_NE(nop, nullptr);
  EXPECT_EQ(nop->instruction.text, "NOP");
  EXPECT_EQ(modeOf(nop->context), Cpu65816Mode::native(true, false));
  EXPECT_EQ(bank2.labels.at(0x028100u), "table_target");
  ASSERT_EQ(d.entries.size(), 2u);
  EXPECT_EQ(d.entries[1].name, "table_target");
  // The BRK after it goes through a vector, which is an entry already, not a stop.
  const Line* brk = codeLineAt(bank2, 0x028101u);
  ASSERT_NE(brk, nullptr);
  EXPECT_EQ(brk->instruction.text, "BRK #$01");
  EXPECT_EQ(d.stops.size(), 2u);
}

// A vector whose name is a mnemonic — `cop`, `brk` — cannot label its handler,
// so the label carries `_handler`; every other vector's name is its label. A
// person's entry with such a name is renamed the same way, and told.
TEST(RomDisasm, AVectorNamedLikeAMnemonicIsLabelledAsAHandler) {
  const std::vector<std::uint8_t> rom = examples::copVectorImage();
  const CartridgeDisassembly d = disassembleWithoutSound(rom);
  ASSERT_EQ(d.entries.size(), 3u);
  EXPECT_EQ(d.entries[0].name, "reset");
  EXPECT_EQ(d.entries[1].name, "cop_handler");  // the emulation set first
  EXPECT_EQ(d.entries[2].name, "brk_native");
  const Listing& bank0 = regionNamed(d, "bank_00.asm").listing;
  EXPECT_EQ(bank0.labels.at(0x008010u), "cop_handler");
  EXPECT_EQ(bank0.labels.at(0x008012u), "brk_native");
  EXPECT_NE(renderManifest(d).find("entry    $00:8010 cop_handler e=1 m=8 x=8\n"), std::string::npos);
  EXPECT_NE(renderManifest(d).find("entry    $00:8012 brk_native e=0 m=? x=?\n"), std::string::npos);
  EXPECT_NE(renderRegion(regionNamed(d, "bank_00.asm"), d).find("\ncop_handler:\n"), std::string::npos);
  EXPECT_TRUE(d.notes.empty());

  CartridgeRequest request;
  request.rom = rom;
  request.captureSound = false;
  request.entries.push_back(
      TraceEntry{.address = 0x008012u, .mode = Cpu65816Mode::reset(), .name = "brk"});
  const CartridgeDisassembly named = disassembleCartridge(request);
  ASSERT_EQ(named.notes.size(), 1u);
  EXPECT_NE(named.notes[0].find("entry brk at $00:8012 is labelled brk_handler"), std::string::npos);
  EXPECT_EQ(named.entries.back().name, "brk_handler");
}

// The CPU takes an emulation-mode vector only with the emulation flag set, which
// fixes both widths at eight; a native vector is taken with whatever widths the
// interrupted code had. So an immediate in the emulation NMI handler reads, and
// the same instruction in the native NMI handler is reported.
TEST(RomDisasm, AnEmulationVectorIsTracedInEmulationMode) {
  std::vector<std::uint8_t> rom = loRomImage(1);
  const std::size_t site = 0x7FC0u;
  rom[site + 0x3A] = 0x10u;  // nmi (emulation) -> $8010
  rom[site + 0x3B] = 0x80u;
  rom[site + 0x2A] = 0x20u;  // nmi (native) -> $8020
  rom[site + 0x2B] = 0x80u;
  put(rom, 0x0000u, {0xDBu});                      // $8000 STP
  put(rom, 0x0010u, {0xA9u, 0x80u, 0x40u});        // $8010 LDA #$80 / RTI
  put(rom, 0x0020u, {0xA9u, 0x80u, 0x40u});        // $8020 LDA #$80 / RTI
  const CartridgeDisassembly d = disassembleWithoutSound(rom);

  ASSERT_EQ(d.entries.size(), 3u);
  EXPECT_EQ(d.entries[1].name, "nmi");
  EXPECT_EQ(d.entries[1].mode, Cpu65816Mode::reset());
  EXPECT_EQ(d.entries[2].name, "nmi_native");
  EXPECT_EQ(d.entries[2].mode, Cpu65816Mode::nativeUnknown());

  const Listing& bank0 = regionNamed(d, "bank_00.asm").listing;
  const Line* emulation = codeLineAt(bank0, 0x008010u);
  ASSERT_NE(emulation, nullptr);
  EXPECT_EQ(emulation->instruction.text, "LDA #$80");
  EXPECT_EQ(emulation->instruction.length, 2u);
  EXPECT_NE(codeLineAt(bank0, 0x008012u), nullptr) << "the handler's RTI was reached";
  EXPECT_EQ(codeLineAt(bank0, 0x008020u), nullptr) << "an immediate under an unknown width is not read";
  ASSERT_EQ(bank0.warnings.size(), 1u);
  EXPECT_NE(bank0.warnings[0].find("$00:8020"), std::string::npos);

  const std::string manifest = renderManifest(d);
  EXPECT_NE(manifest.find("entry    $00:8010 nmi e=1 m=8 x=8\n"), std::string::npos) << manifest;
  EXPECT_NE(manifest.find("entry    $00:8020 nmi_native e=0 m=? x=?\n"), std::string::npos) << manifest;
}

TEST(RomDisasm, AnEntryOutsideTheImageIsANote) {
  const std::vector<std::uint8_t> rom = threeBankImage();
  CartridgeRequest request;
  request.rom = rom;
  request.captureSound = false;
  request.entries.push_back(
      TraceEntry{.address = 0x7E2000u, .mode = Cpu65816Mode::reset(), .name = "ram"});
  const CartridgeDisassembly d = disassembleCartridge(request);
  ASSERT_EQ(d.notes.size(), 1u);
  EXPECT_NE(d.notes[0].find("ram at $7E:2000 is not in the image"), std::string::npos);
  EXPECT_EQ(d.entries.size(), 1u);
}

TEST(RomDisasm, ARegionSplitFromTheRequestIsTheOneWritten) {
  const std::vector<std::uint8_t> rom = threeBankImage();
  CartridgeRequest request;
  request.rom = rom;
  request.captureSound = false;
  request.regions = {SourceRegion{.file = "init.asm", .first = 0x008000u, .last = 0x0080FFu},
                     SourceRegion{.file = "bank_01.asm", .first = 0x018000u, .last = 0x01FFFFu}};
  const CartridgeDisassembly d = disassembleCartridge(request);
  ASSERT_EQ(d.regions.size(), 2u);
  EXPECT_EQ(d.regions[0].region.file, "init.asm");
  EXPECT_NE(codeLineAt(d.regions[0].listing, 0x008000u), nullptr);
  // Bank $02 has no file and neither does $00:8100, so the jump and the call to
  // them are stops, and bank $02's bytes go unplaced.
  ASSERT_EQ(d.stops.size(), 2u);
  EXPECT_EQ(d.stops[0].address, 0x008008u);
  EXPECT_NE(d.stops[0].reason.find("lies in no region"), std::string::npos);
  EXPECT_EQ(d.stops[1].address, 0x018003u);
  EXPECT_NE(d.stops[1].reason.find("$00:8100 lies in no region"), std::string::npos);
  const Placement placement = placeBytes(d);
  EXPECT_EQ(placement.unplaced, rom.size() - 0x100u - 0x8000u);
}

TEST(RomDisasm, ANonContiguousRegionIsLeftOutWithANote) {
  const std::vector<std::uint8_t> rom = threeBankImage();
  CartridgeRequest request;
  request.rom = rom;
  request.captureSound = false;
  request.regions = {SourceRegion{.file = "low.asm", .first = 0x000000u, .last = 0x00FFFFu}};
  const CartridgeDisassembly d = disassembleCartridge(request);
  EXPECT_TRUE(d.regions.empty());
  ASSERT_FALSE(d.notes.empty());
  EXPECT_NE(d.notes[0].find("low.asm"), std::string::npos);
  EXPECT_NE(d.notes[0].find("consecutive"), std::string::npos);
}

TEST(RomDisasm, AnImageWithNoHeaderIsANote) {
  const std::vector<std::uint8_t> rom(0x100u, 0u);
  const CartridgeDisassembly d = disassembleWithoutSound(rom);
  EXPECT_TRUE(d.regions.empty());
  ASSERT_EQ(d.notes.size(), 1u);
  EXPECT_NE(d.notes[0].find("header"), std::string::npos);
}

// ---- the sound program ------------------------------------------------------

TEST(RomDisasm, TheBootUploadIsCapturedFromTheMachine) {
  const std::vector<std::uint8_t> rom = uploadingImage();
  std::string reason;
  const std::optional<UploadCapture> capture = captureUpload(rom, 2u * 21'477'272u, reason);
  ASSERT_TRUE(capture.has_value()) << reason;
  EXPECT_EQ(capture->entry, 0x0200u);
  // The program and the table land end to end, and are told apart by where the
  // image holds them.
  ASSERT_EQ(capture->blocks.size(), 2u);
  EXPECT_EQ(capture->blocks[0].apuAddress, 0x0200u);
  EXPECT_EQ(capture->blocks[0].bytes, kUploadedProgram);
  ASSERT_TRUE(capture->blocks[0].romOffset.has_value());
  EXPECT_EQ(*capture->blocks[0].romOffset, 0xA0u);
  EXPECT_EQ(capture->blocks[1].apuAddress, 0x0218u);
  EXPECT_EQ(capture->blocks[1].bytes, kUploadedTable);
  ASSERT_TRUE(capture->blocks[1].romOffset.has_value());
  EXPECT_EQ(*capture->blocks[1].romOffset, 0x100u);
}

TEST(RomDisasm, ABlockTheImageHoldsTwiceIsNotPlaced) {
  std::vector<std::uint8_t> rom = uploadingImage();
  std::copy(kUploadedProgram.begin(), kUploadedProgram.end(), rom.begin() + 0x1000);
  std::string reason;
  const std::optional<UploadCapture> capture = captureUpload(rom, 2u * 21'477'272u, reason);
  ASSERT_TRUE(capture.has_value()) << reason;
  ASSERT_EQ(capture->blocks.size(), 2u);
  EXPECT_EQ(capture->blocks[0].bytes, kUploadedProgram);
  EXPECT_FALSE(capture->blocks[0].romOffset.has_value());
  EXPECT_EQ(capture->blocks[1].bytes, kUploadedTable);  // the table after it is still placed
  ASSERT_TRUE(capture->blocks[1].romOffset.has_value());
  EXPECT_EQ(*capture->blocks[1].romOffset, 0x100u);
  // The bank keeps both copies, and the tree is still complete.
  CartridgeRequest request;
  request.rom = rom;
  const CartridgeDisassembly d = disassembleCartridge(request);
  ASSERT_TRUE(d.sound.has_value());
  EXPECT_NE(renderRegion(regionNamed(d, "bank_00.asm"), d).find("$E8,$5A,$C5"), std::string::npos);
  EXPECT_NE(renderManifest(d).find("block    apu/driver.asm $0200 24 unplaced\n"), std::string::npos);
  const Placement placement = placeBytes(d);
  EXPECT_EQ(placement.unplaced, 0u);
  EXPECT_EQ(placement.placedTwice, 0u);
  EXPECT_EQ(placement.image, rom);
}


TEST(RomDisasm, ACartridgeThatNeverUploadsIsReported) {
  std::vector<std::uint8_t> rom = loRomImage(1);
  put(rom, 0x0000u, {0xDBu});  // STP
  std::string reason;
  const std::optional<UploadCapture> capture = captureUpload(rom, 200'000u, reason);
  EXPECT_FALSE(capture.has_value());
  EXPECT_NE(reason.find("did not leave the upload stub"), std::string::npos);
}

TEST(RomDisasm, TheSoundProgramIsTracedFromItsEntry) {
  const std::vector<std::uint8_t> rom = uploadingImage();
  CartridgeRequest request;
  request.rom = rom;
  const CartridgeDisassembly d = disassembleCartridge(request);
  ASSERT_TRUE(d.sound.has_value());
  EXPECT_EQ(d.sound->file, "apu/driver.asm");
  const Listing& listing = d.sound->listing;
  EXPECT_EQ(listing.addressBits, 16u);
  // Twenty instructions, then the table the trace never reached, as data.
  ASSERT_EQ(listing.lines.size(), 21u);
  EXPECT_EQ(listing.lines[0].instruction.text, "MOV A,#$5A");
  EXPECT_EQ(listing.lines[1].instruction.text, "MOV !$0250,A");
  EXPECT_EQ(listing.lines[2].instruction.text, "MOV A,#$00");
  EXPECT_EQ(listing.lines[3].instruction.text, "NOP");
  EXPECT_EQ(listing.lines[19].instruction.text, "STOP");
  EXPECT_FALSE(listing.lines[20].isCode);
  EXPECT_EQ(listing.lines[20].address, 0x0218u);
  EXPECT_EQ(listing.lines[20].data, kUploadedTable);
  EXPECT_EQ(listing.labels.at(0x0200u), "entry");
}

TEST(RomDisasm, TheBankLeavesTheUploadedBytesToTheSoundProgram) {
  const std::vector<std::uint8_t> rom = uploadingImage();
  CartridgeRequest request;
  request.rom = rom;
  const CartridgeDisassembly d = disassembleCartridge(request);
  ASSERT_TRUE(d.sound.has_value());
  const std::string bank0 = renderRegion(regionNamed(d, "bank_00.asm"), d);
  EXPECT_NE(bank0.find("        ORG $00:8000\n"), std::string::npos);
  EXPECT_NE(bank0.find("; ---- $00:80A0-$00:80B7: the sound program, see apu/driver.asm\n"),
            std::string::npos);
  EXPECT_NE(bank0.find("        ORG $00:80B8\n"), std::string::npos);  // the piece after it
  EXPECT_NE(bank0.find("; ---- $00:8100-$00:8113: the sound program, see apu/driver.asm\n"),
            std::string::npos);
  EXPECT_NE(bank0.find("        ORG $00:8114\n"), std::string::npos);
  EXPECT_EQ(bank0.find("$E8,$5A,$C5"), std::string::npos);  // the program's bytes are not here
  EXPECT_EQ(bank0.find("$9C,$3D,$71"), std::string::npos);  // nor the table's
  const std::string sound = renderSoundProgram(*d.sound);
  EXPECT_NE(sound.find("        ORG $0200\n"), std::string::npos);
  EXPECT_NE(sound.find("; $0200: 24 bytes, read from image offset $0000A0\n"), std::string::npos);
  EXPECT_NE(sound.find("; $0218: 20 bytes, read from image offset $000100\n"), std::string::npos);
  EXPECT_NE(sound.find("MOV !$0250,A"), std::string::npos);
  EXPECT_NE(sound.find("DB $9C,$3D"), std::string::npos);
}

TEST(RomDisasm, EveryImageByteIsPlacedOnce) {
  const std::vector<std::uint8_t> rom = uploadingImage();
  CartridgeRequest request;
  request.rom = rom;
  const CartridgeDisassembly d = disassembleCartridge(request);
  ASSERT_TRUE(d.sound.has_value());
  const Placement placement = placeBytes(d);
  EXPECT_EQ(placement.unplaced, 0u);
  EXPECT_EQ(placement.placedTwice, 0u);
  EXPECT_EQ(placement.image, rom);
}

TEST(RomDisasm, ABankNothingReachesIsWrittenAsData) {
  std::vector<std::uint8_t> rom = loRomImage(2);
  put(rom, 0x0000u, {0xDBu});         // STP: nothing reaches bank $01
  put(rom, 0x8000u, {0xA9u, 0x12u});  // bytes that would read as code if traced
  const CartridgeDisassembly d = disassembleWithoutSound(rom);
  ASSERT_EQ(d.regions.size(), 2u);
  const Listing& bank1 = regionNamed(d, "bank_01.asm").listing;
  ASSERT_EQ(bank1.lines.size(), 1u);
  EXPECT_FALSE(bank1.lines[0].isCode);
  EXPECT_EQ(bank1.lines[0].address, 0x018000u);
  EXPECT_EQ(bank1.lines[0].data.size(), 0x8000u);
  EXPECT_EQ(bank1.addressBits, 24u);
  const std::string text = renderRegion(regionNamed(d, "bank_01.asm"), d);
  EXPECT_NE(text.find("        ORG $01:8000\n"), std::string::npos);
  EXPECT_NE(text.find("DB $A9,$12"), std::string::npos);
  const Placement placement = placeBytes(d);
  EXPECT_EQ(placement.unplaced, 0u);
  EXPECT_EQ(placement.image, rom);
}

TEST(RomDisasm, WithoutTheSoundProgramTheBankKeepsEveryByte) {
  const std::vector<std::uint8_t> rom = uploadingImage();
  const CartridgeDisassembly d = disassembleWithoutSound(rom);
  EXPECT_FALSE(d.sound.has_value());
  const std::string bank0 = renderRegion(regionNamed(d, "bank_00.asm"), d);
  EXPECT_NE(bank0.find("$E8,$5A,$C5"), std::string::npos);
  EXPECT_NE(bank0.find("$9C,$3D,$71"), std::string::npos);
  const Placement placement = placeBytes(d);
  EXPECT_EQ(placement.unplaced, 0u);
  EXPECT_EQ(placement.image, rom);
}

// ---- the manifest -----------------------------------------------------------

TEST(RomDisasm, TheManifestReadsBackItsEntriesAndFiles) {
  const std::vector<std::uint8_t> rom = threeBankImage();
  CartridgeRequest request;
  request.rom = rom;
  request.captureSound = false;
  request.entries.push_back(TraceEntry{.address = 0x028100u,
                                       .mode = Cpu65816Mode::native(true, false),
                                       .name = "table_target"});
  const CartridgeDisassembly d = disassembleCartridge(request);
  const std::string text = renderManifest(d);
  EXPECT_NE(text.find("map      LoROM\n"), std::string::npos);
  EXPECT_NE(text.find("image    98304\n"), std::string::npos);
  EXPECT_NE(text.find("file     bank_01.asm 65816 $01:8000 $01:FFFF\n"), std::string::npos);
  EXPECT_NE(text.find("entry    $00:8000 reset e=1 m=8 x=8\n"), std::string::npos);
  EXPECT_NE(text.find("entry    $02:8100 table_target e=0 m=8 x=16\n"), std::string::npos);
  EXPECT_NE(text.find("stop     $02:8008 `JMP (!$8100,X)`"), std::string::npos);

  std::string error;
  const std::optional<ManifestInput> input = parseManifest(text, error);
  ASSERT_TRUE(input.has_value()) << error;
  ASSERT_EQ(input->regions.size(), 3u);
  EXPECT_EQ(input->regions[2].file, "bank_02.asm");
  EXPECT_EQ(input->regions[2].first, 0x028000u);
  EXPECT_EQ(input->regions[2].last, 0x02FFFFu);
  ASSERT_EQ(input->entries.size(), 2u);
  EXPECT_EQ(input->entries[0].name, "reset");
  EXPECT_EQ(input->entries[0].mode, Cpu65816Mode::reset());
  EXPECT_EQ(input->entries[1].address, 0x028100u);
  EXPECT_EQ(input->entries[1].mode, Cpu65816Mode::native(true, false));
}

TEST(RomDisasm, AnInterruptEntryReadsBackWithUnknownWidths) {
  std::string error;
  const std::optional<ManifestInput> input =
      parseManifest("entry $00:816A nmi e=0 m=? x=?\n", error);
  ASSERT_TRUE(input.has_value()) << error;
  ASSERT_EQ(input->entries.size(), 1u);
  EXPECT_EQ(input->entries[0].mode, Cpu65816Mode::nativeUnknown());
}

TEST(RomDisasm, ReadingTheManifestBackDoesNotDoubleTheVectors) {
  const std::vector<std::uint8_t> rom = threeBankImage();
  const CartridgeDisassembly first = disassembleWithoutSound(rom);
  std::string error;
  const std::optional<ManifestInput> input = parseManifest(renderManifest(first), error);
  ASSERT_TRUE(input.has_value()) << error;
  CartridgeRequest request;
  request.rom = rom;
  request.captureSound = false;
  request.entries = input->entries;
  request.regions = input->regions;
  const CartridgeDisassembly second = disassembleCartridge(request);
  EXPECT_EQ(second.entries.size(), first.entries.size());
  EXPECT_EQ(renderManifest(second), renderManifest(first));
}

TEST(RomDisasm, TheManifestNamesTheImageItWasWrittenFor) {
  const std::vector<std::uint8_t> rom = threeBankImage();
  const CartridgeDisassembly d = disassembleWithoutSound(rom);
  std::string error;
  const std::optional<ManifestInput> input = parseManifest(renderManifest(d), error);
  ASSERT_TRUE(input.has_value()) << error;
  ASSERT_TRUE(input->imageBytes.has_value());
  EXPECT_EQ(*input->imageBytes, rom.size());
  ASSERT_TRUE(input->checksum.has_value());
  EXPECT_EQ(*input->checksum, 0xEDCBu);
  EXPECT_EQ(manifestMismatch(*input, rom), "");

  const std::vector<std::uint8_t> other = uploadingImage();
  EXPECT_NE(manifestMismatch(*input, other).find("98304 bytes"), std::string::npos);
  std::vector<std::uint8_t> changed = rom;
  changed[0x7FDEu] = 0xCCu;  // the checksum's low byte
  EXPECT_NE(manifestMismatch(*input, changed).find("checksum $EDCB"), std::string::npos);
  EXPECT_NE(manifestMismatch(*input, changed).find("$EDCC"), std::string::npos);
}

// ---- what a run landed on, and what it saw ---------------------------------------

namespace {

constexpr std::uint64_t kRunFrame = 357'954u;  // one NTSC frame of the master clock, roughly

CartridgeDisassembly disassembleRamCode(bool run, std::vector<TraceEntry> entries = {}) {
  static const std::vector<std::uint8_t> rom = ramCodeImage();
  CartridgeRequest request;
  request.rom = rom;
  request.captureSound = false;
  request.observeRun = run;
  request.runMasterCycles = kRunFrame;
  request.entries = std::move(entries);
  return disassembleCartridge(request);
}

}  // namespace

TEST(RomLanding, TheManifestCarriesTheLandingsAndTheValuesSeenAndReadsTheLandingsBack) {
  const CartridgeDisassembly d = disassembleRamCode(true);
  const std::string manifest = renderManifest(d);
  EXPECT_NE(manifest.find("ran      $00:803A loc_00803A e=1 m=8 x=8 from $00:8039\n"), std::string::npos)
      << manifest;
  EXPECT_NE(manifest.find("ran      $00:804D loc_00804D e=0 m=16 x=16 from $00:804C\n"), std::string::npos)
      << manifest;
  EXPECT_NE(manifest.find("ran      $00:802C loc_00802C e=1 m=8 x=8 from $7E:2001\n"), std::string::npos)
      << manifest;
  EXPECT_NE(manifest.find("seen     $00:802C D=$4300|$4310 DBR=$00\n"), std::string::npos) << manifest;
  EXPECT_NE(manifest.find("seen     $00:803F D=$4320 DBR=$00\n"), std::string::npos) << manifest;

  std::string error;
  const std::optional<ManifestInput> input = parseManifest(manifest, error);
  ASSERT_TRUE(input.has_value()) << error;
  ASSERT_EQ(input->ran.size(), 3u);
  for (std::size_t i = 0; i < 3; ++i) {
    EXPECT_TRUE(sameLanding(input->ran[i], d.ran[i])) << i;
    EXPECT_EQ(input->ran[i].name, d.ran[i].name);
  }
  EXPECT_TRUE(parseManifest("seen $00:802C D=$4300|$4310 DBR=$00\n", error).has_value())
      << "a seen line is read past, never read back";
  EXPECT_FALSE(parseManifest("ran $00:802C loc_00802C e=1 m=8 x=8 at $7E:2001\n", error).has_value());
  EXPECT_NE(error.find("`from`"), std::string::npos);
  EXPECT_FALSE(parseManifest("ran $00:802C loc_00802C e=1 m=8 from $7E:2001\n", error).has_value());
  EXPECT_FALSE(parseManifest("ran $802C loc_00802C e=1 m=8 x=8 from $7E:2001\n", error).has_value());
}

TEST(RomLanding, TheTraceStartsFromWhereTheRunLanded) {
  const CartridgeDisassembly without = disassembleRamCode(false);
  const Listing& before = regionNamed(without, "bank_00.asm").listing;
  EXPECT_EQ(codeLineAt(before, 0x00802Cu), nullptr) << "after an RTL the bytes name no path";
  EXPECT_EQ(codeLineAt(before, 0x00803Au), nullptr);
  EXPECT_EQ(codeLineAt(before, 0x00804Du), nullptr);

  const CartridgeDisassembly with = disassembleRamCode(true);
  const Listing& after = regionNamed(with, "bank_00.asm").listing;
  ASSERT_NE(codeLineAt(after, 0x00802Cu), nullptr);
  ASSERT_NE(codeLineAt(after, 0x00803Au), nullptr);
  ASSERT_NE(codeLineAt(after, 0x00804Du), nullptr);
  EXPECT_EQ(after.labels.at(0x00802Cu), "loc_00802C");
  EXPECT_EQ(after.labels.at(0x00803Au), "loc_00803A");
  EXPECT_EQ(after.labels.at(0x00804Du), "loc_00804D");
  EXPECT_EQ(codeLineAt(after, 0x00804Du)->instruction.text, "STP");
}

TEST(RomLanding, AnEarlierRunsLandingsAreTracedFromWithoutRunning) {
  const CartridgeDisassembly ran = disassembleRamCode(true);
  std::string error;
  const std::optional<ManifestInput> input = parseManifest(renderManifest(ran), error);
  ASSERT_TRUE(input.has_value()) << error;
  static const std::vector<std::uint8_t> rom = ramCodeImage();
  CartridgeRequest again;
  again.rom = rom;
  again.captureSound = false;
  again.observeRun = false;
  again.ran = input->ran;
  const CartridgeDisassembly replayed = disassembleCartridge(again);
  ASSERT_EQ(replayed.ran.size(), 3u);
  EXPECT_NE(codeLineAt(regionNamed(replayed, "bank_00.asm").listing, 0x00802Cu), nullptr);
  EXPECT_TRUE(replayed.seen.empty());
  // The same landings, once: the run's again and the manifest's are one set.
  CartridgeRequest merged = again;
  merged.observeRun = true;
  merged.runMasterCycles = kRunFrame;
  EXPECT_EQ(disassembleCartridge(merged).ran.size(), 3u);
}

TEST(RomLanding, APersonsEntryNamesTheLanding) {
  const CartridgeDisassembly d = disassembleRamCode(
      true, {TraceEntry{.address = 0x00802Cu, .mode = Cpu65816Mode::reset(), .name = "after_the_routine"}});
  const std::string manifest = renderManifest(d);
  EXPECT_NE(manifest.find("ran      $00:802C after_the_routine e=1 m=8 x=8 from $7E:2001\n"),
            std::string::npos)
      << manifest;
  EXPECT_EQ(manifest.find("loc_00802C"), std::string::npos);
}

namespace {

// The rendered line whose instruction text begins with `head`.
std::string lineOf(const std::string& file, const std::string& head) {
  const std::size_t at = file.find("        " + head);
  if (at == std::string::npos) return {};
  return file.substr(at, file.find('\n', at) - at);
}

}  // namespace

TEST(RomLanding, ADirectPageOperandUnderTheOneDirectRegisterTheRunSawNamesItsRegister) {
  const CartridgeDisassembly with = disassembleRamCode(true);
  const std::string file = renderRegion(regionNamed(with, "bank_00.asm"), with);
  const std::string one = lineOf(file, "STA $01 ");
  EXPECT_NE(one.find("BBAD2 (run)"), std::string::npos) << one;
  // Two direct registers seen: the run names nothing, since either could be it.
  const std::string two = lineOf(file, "STA $00 ");
  EXPECT_FALSE(two.empty()) << file;
  EXPECT_EQ(two.find("(run)"), std::string::npos) << two;
  EXPECT_EQ(two.find("DMAP"), std::string::npos) << two;
  // An indexed form: the run does not know X here, and names nothing.
  const std::string indexed = lineOf(file, "STA $03,X");
  EXPECT_FALSE(indexed.empty()) << file;
  EXPECT_EQ(indexed.find("(run)"), std::string::npos) << indexed;
  // The paths prove the direct register at $801D: the proven name, and not the run's.
  const std::string proven = lineOf(file, "STA $04 ");
  EXPECT_NE(proven.find("A1B0"), std::string::npos) << proven;
  EXPECT_EQ(proven.find("(run)"), std::string::npos) << proven;

  const CartridgeDisassembly without = disassembleRamCode(false);
  EXPECT_EQ(renderRegion(regionNamed(without, "bank_00.asm"), without).find("(run)"), std::string::npos);
}

TEST(RomDisasm, AManifestLineThatDoesNotParseNamesItsLine) {
  std::string error;
  EXPECT_FALSE(parseManifest("; fine\nimage 10\nentry $8000 reset e=1 m=8 x=8\n", error).has_value());
  EXPECT_NE(error.find("line 3:"), std::string::npos);
  EXPECT_NE(error.find("$BB:XXXX"), std::string::npos);
  EXPECT_FALSE(parseManifest("file a.asm 65816 $00:8000 $01:8000\n", error).has_value());
  EXPECT_NE(error.find("one bank"), std::string::npos);
  EXPECT_FALSE(parseManifest("region a.asm\n", error).has_value());
  EXPECT_NE(error.find("not a manifest line"), std::string::npos);
  EXPECT_TRUE(parseManifest("stop $00:8000 anything at all ; and a comment\nnote free text\n", error)
                  .has_value());
}

// ---- the tree on disk -------------------------------------------------------

TEST(RomDisasm, TheProjectIsWrittenAsOneFilePerRegionPlusTheManifest) {
  const std::vector<std::uint8_t> rom = uploadingImage();
  CartridgeRequest request;
  request.rom = rom;
  const CartridgeDisassembly d = disassembleCartridge(request);
  ASSERT_TRUE(d.sound.has_value());
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() / "snaggletooth-rom-disasm-test";
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::string error;
  ASSERT_TRUE(writeProject(d, dir, error)) << error;
  EXPECT_TRUE(std::filesystem::is_regular_file(dir / "project.manifest"));
  EXPECT_TRUE(std::filesystem::is_regular_file(dir / "bank_00.asm"));
  EXPECT_TRUE(std::filesystem::is_regular_file(dir / "apu" / "driver.asm"));
  // Read and close the file before the directory goes: an open file cannot be
  // removed on every platform.
  std::string bank0;
  {
    std::ifstream in(dir / "bank_00.asm", std::ios::binary);
    bank0.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  }
  EXPECT_EQ(bank0, renderRegion(regionNamed(d, "bank_00.asm"), d));
  std::filesystem::remove_all(dir, ec);
}

// ---- the assets ---------------------------------------------------------------
//
// The lifting cartridge sends the image's bytes to the hardware every way the
// rules have a case for; a run of three frames sees all of it, the HDMA table
// included.

namespace {

constexpr std::uint64_t kFrame = 357'954u;  // one NTSC frame of the master clock, roughly

CartridgeDisassembly lifted(std::span<const std::uint8_t> rom, CartridgeRequest request = {}) {
  request.rom = rom;
  request.captureSound = false;
  request.observeRun = true;
  request.runMasterCycles = 3u * kFrame;
  return disassembleCartridge(request);
}

const AssetFile* assetNamed(const CartridgeDisassembly& d, const std::string& file) {
  for (const AssetFile& asset : d.assets) {
    if (asset.file == file) return &asset;
  }
  return nullptr;
}

bool anyNote(const CartridgeDisassembly& d, const std::string& text) {
  return std::any_of(d.notes.begin(), d.notes.end(),
                     [&](const std::string& note) { return note.find(text) != std::string::npos; });
}

}  // namespace

TEST(RomAssets, EveryLiftedRangeIsAFileUnderTheDirectoryOfItsMemory) {
  const std::vector<std::uint8_t> rom = liftingImage();
  const CartridgeDisassembly d = lifted(rom);
  ASSERT_EQ(d.assets.size(), 10u) << renderManifest(d);
  struct Expected {
    const char* file;
    RegisterClass cls;
    MovedKind kind;
    Address first;
    std::size_t bytes;
  };
  // The tenth is the shadow's: the transfer from `$01:FFF0` wraps into
  // `$01:0000`, which is work RAM holding the bytes the port copied there from
  // `$9900`, so those sixteen are lifted as the source they were staged from.
  const Expected expected[] = {
      {"vram/00_9000.bin", RegisterClass::Vram, MovedKind::Dma, 0x009000u, 80},
      {"cgram/00_9200.bin", RegisterClass::Cgram, MovedKind::Dma, 0x009200u, 16},
      {"oam/00_9300.bin", RegisterClass::Oam, MovedKind::Dma, 0x009300u, 544},
      {"apu/00_9600.bin", RegisterClass::Apu, MovedKind::Dma, 0x009600u, 8},
      {"hdma/00_9700.bin", RegisterClass::Cgram, MovedKind::Table, 0x009700u, 7},
      {"hdma/00_9710.bin", RegisterClass::Cgram, MovedKind::Indirect, 0x009710u, 2},
      {"hdma/00_9712.bin", RegisterClass::Cgram, MovedKind::Indirect, 0x009712u, 2},
      {"vram/00_9900.bin", RegisterClass::Vram, MovedKind::Staged, 0x009900u, 16},
      {"vram/01_8000.bin", RegisterClass::Vram, MovedKind::Dma, 0x018000u, 32},
      {"vram/01_FFF0.bin", RegisterClass::Vram, MovedKind::Dma, 0x01FFF0u, 16},
  };
  for (std::size_t i = 0; i < 10; ++i) {
    const AssetFile& asset = d.assets[i];
    EXPECT_EQ(asset.file, expected[i].file);
    EXPECT_EQ(asset.classes, std::vector<RegisterClass>{expected[i].cls}) << asset.file;
    EXPECT_EQ(asset.kind, expected[i].kind) << asset.file;
    EXPECT_EQ(asset.first, expected[i].first) << asset.file;
    EXPECT_EQ(asset.bytes.size(), expected[i].bytes) << asset.file;
    // The bytes are the image's at the offset the address reads from.
    const std::optional<std::size_t> offset = romOffset(CartridgeMap::LoRom, asset.first, rom.size());
    ASSERT_TRUE(offset.has_value());
    EXPECT_EQ(asset.romOffset, *offset);
    EXPECT_TRUE(std::equal(asset.bytes.begin(), asset.bytes.end(), rom.begin() + static_cast<std::ptrdiff_t>(*offset)))
        << asset.file;
  }
}

TEST(RomAssets, RangesThatShareBytesAreOneFileAndRangesThatTouchAreTwo) {
  const CartridgeDisassembly d = lifted(liftingImage());
  // 64 from $9000, 16 from $9010 inside it, 32 from $9030 across its end: one
  // file of 80.
  const AssetFile* tileset = assetNamed(d, "vram/00_9000.bin");
  ASSERT_NE(tileset, nullptr);
  EXPECT_EQ(tileset->bytes.size(), 80u);
  EXPECT_EQ(assetNamed(d, "vram/00_9010.bin"), nullptr);
  EXPECT_EQ(assetNamed(d, "vram/00_9030.bin"), nullptr);
  // The two blocks the indirect table points at end to end: two files.
  EXPECT_NE(assetNamed(d, "hdma/00_9710.bin"), nullptr);
  EXPECT_NE(assetNamed(d, "hdma/00_9712.bin"), nullptr);
}

TEST(RomAssets, ARangeReadDownwardIsLiftedInImageOrder) {
  const CartridgeDisassembly d = lifted(liftingImage());
  const AssetFile* palette = assetNamed(d, "cgram/00_9200.bin");
  ASSERT_NE(palette, nullptr);
  ASSERT_EQ(palette->bytes.size(), 16u);
  for (std::size_t i = 0; i < 16; ++i) {
    EXPECT_EQ(palette->bytes[i], 0xE0u + i);
  }
}

TEST(RomAssets, ARefusedRangeIsNotedAndStaysInItsBank) {
  const CartridgeDisassembly d = lifted(liftingImage());
  // Sent to VRAM and then to CGRAM.
  EXPECT_EQ(assetNamed(d, "vram/00_9800.bin"), nullptr);
  EXPECT_EQ(assetNamed(d, "cgram/00_9800.bin"), nullptr);
  EXPECT_TRUE(anyNote(d, "the bytes at $00:9800 were sent to VMDATAL and CGDATA; not lifted"));
  // The reset routine's own bytes.
  EXPECT_EQ(assetNamed(d, "vram/00_8000.bin"), nullptr);
  EXPECT_TRUE(anyNote(d, "$00:8000-$00:800F overlaps an instruction the trace decoded; not lifted"));
  const std::string bank0 = renderRegion(regionNamed(d, "bank_00.asm"), d);
  // The bytes stay as DB rows: the row that begins four bytes before them.
  EXPECT_NE(bank0.find("        DB $00,$00,$00,$00,$60,$61,$62,$63  ; $00:97FC  |"), std::string::npos) << bank0;
  EXPECT_EQ(bank0.find("INCBIN \"vram/00_9800.bin\""), std::string::npos);
  EXPECT_EQ(bank0.find("INCBIN \"cgram/00_9800.bin\""), std::string::npos);
}

TEST(RomAssets, WhatIsNotAnAssetIsLeftWithoutAWord) {
  const CartridgeDisassembly d = lifted(liftingImage());
  // A fill from one byte, and a read from a register back into the image,
  // which takes nothing. The copy into work RAM through the port is not an
  // asset for being copied; it is lifted only because the wrapped transfer
  // later sent sixteen of its bytes on to VRAM, and as that source.
  for (const AssetFile& asset : d.assets) {
    if (asset.first == 0x009900u) {
      EXPECT_EQ(asset.kind, MovedKind::Staged);
    }
    EXPECT_NE(asset.first, 0x009A00u);
    EXPECT_NE(asset.first, 0x009B00u);
  }
  EXPECT_FALSE(anyNote(d, "$00:9900"));
  EXPECT_FALSE(anyNote(d, "$00:9A00"));
  EXPECT_FALSE(anyNote(d, "$00:9B00"));
}

TEST(RomAssets, TheSameBytesSentToTwoRegistersOfOneClassAreRefused) {
  const CartridgeDisassembly d = lifted(liftingImage());
  EXPECT_EQ(assetNamed(d, "vram/00_9C00.bin"), nullptr);
  EXPECT_TRUE(anyNote(d, "the bytes at $00:9C00 were sent to VMDATAL and VMDATAH; not lifted"));
}

TEST(RomAssets, ARangeOverABlockOfTheSoundProgramIsRefused) {
  // The uploading cartridge's first block is 24 bytes at image offset $A0,
  // which is $00:80A0; a range laid over it, as an earlier run might have
  // recorded one, is not lifted.
  const std::vector<std::uint8_t> rom = uploadingImage();
  CartridgeRequest request;
  request.rom = rom;
  request.captureSound = true;
  request.observeRun = false;
  MovedRange over{.site = 0x008000u,
                  .channel = 0,
                  .toRegister = true,
                  .registerAddress = 0x002118u,
                  .registerName = "VMDATAL",
                  .registerClass = RegisterClass::Vram,
                  .memory = 0x008098u,
                  .step = MovedStep::Increment,
                  .bytes = 16,
                  .kind = MovedKind::Dma,
                  .times = 1};
  request.moved = {over};
  const CartridgeDisassembly d = disassembleCartridge(request);
  ASSERT_TRUE(d.sound.has_value());
  EXPECT_TRUE(d.assets.empty());
  EXPECT_TRUE(anyNote(d, "$00:8098-$00:80A7 overlaps a block of the sound program; not lifted"));
}

TEST(RomAssets, AHiRomTransferThatWrapsItsBankIsLiftedAsItsPieces) {
  const std::vector<std::uint8_t> rom = wrappingImage();
  CartridgeRequest request;
  request.rom = rom;
  request.captureSound = false;
  request.observeRun = true;
  request.runMasterCycles = kFrame;
  const CartridgeDisassembly d = disassembleCartridge(request);
  ASSERT_EQ(d.moved.size(), 1u);
  EXPECT_EQ(d.moved.front().memory, 0xC1FFF0u);
  EXPECT_EQ(d.moved.front().bytes, 32u);
  ASSERT_EQ(d.assets.size(), 2u) << renderManifest(d);
  EXPECT_EQ(d.assets[0].file, "vram/C1_0000.bin");
  EXPECT_EQ(d.assets[0].first, 0xC10000u);
  EXPECT_EQ(d.assets[0].bytes.size(), 16u);
  EXPECT_EQ(d.assets[0].bytes.front(), 0xB0u);
  EXPECT_EQ(d.assets[1].file, "vram/C1_FFF0.bin");
  EXPECT_EQ(d.assets[1].first, 0xC1FFF0u);
  EXPECT_EQ(d.assets[1].bytes.front(), 0xA0u);
  EXPECT_FALSE(anyNote(d, "not the image")) << "both pieces are the image";
  const std::string bank1 = renderRegion(regionNamed(d, "bank_C1.asm"), d);
  EXPECT_NE(bank1.find("        ORG $C1:0000\n\n; ---- $C1:0000-$C1:000F: 16 bytes a transfer carried to VMDATAL, in vram/C1_0000.bin\n"
                       "        INCBIN \"vram/C1_0000.bin\"\n"), std::string::npos) << bank1;
  EXPECT_NE(bank1.find("        INCBIN \"vram/C1_FFF0.bin\"\n"), std::string::npos);
  const Placement placement = placeBytes(d);
  EXPECT_EQ(placement.unplaced, 0u);
  EXPECT_EQ(placement.placedTwice, 0u);
}

TEST(RomAssets, ATransferThatLeavesTheImageIsLiftedAsFarAsItWasInIt) {
  const CartridgeDisassembly d = lifted(liftingImage());
  const AssetFile* edge = assetNamed(d, "vram/01_FFF0.bin");
  ASSERT_NE(edge, nullptr);
  EXPECT_EQ(edge->bytes.size(), 16u);
  EXPECT_TRUE(anyNote(d, "memory $01:FFF0 bytes 32: 16 of its bytes are not the image and are not lifted"));
}

TEST(RomAssets, TheBankFileIncludesEachFileWhereItsBytesWere) {
  const CartridgeDisassembly d = lifted(liftingImage());
  const std::string bank0 = renderRegion(regionNamed(d, "bank_00.asm"), d);
  EXPECT_NE(bank0.find("\n; ---- $00:9000-$00:904F: 80 bytes a transfer carried to VMDATAL, in vram/00_9000.bin\n"
                       "        INCBIN \"vram/00_9000.bin\"\n"),
            std::string::npos) << bank0;
  EXPECT_NE(bank0.find("; ---- $00:9700-$00:9706: an HDMA table walked to CGADD, in hdma/00_9700.bin\n"
                       "        INCBIN \"hdma/00_9700.bin\"\n"),
            std::string::npos);
  EXPECT_NE(bank0.find("; ---- $00:9710-$00:9711: a block an HDMA entry pointed at, sent to CGADD, in hdma/00_9710.bin\n"),
            std::string::npos);
  EXPECT_EQ(bank0.find("; $00:9000  |"), std::string::npos) << "the lifted bytes are no longer DB rows";
  EXPECT_EQ(bank0.find("; $00:9040  |"), std::string::npos);
  EXPECT_NE(bank0.find("; $00:9050  |"), std::string::npos) << "the byte after the file is";
  // An INCBIN continues the range: one ORG per bank still.
  EXPECT_EQ(std::count(bank0.begin(), bank0.end(), '\n') > 0, true);
  std::size_t orgs = 0;
  for (std::size_t at = bank0.find("        ORG "); at != std::string::npos; at = bank0.find("        ORG ", at + 1)) ++orgs;
  EXPECT_EQ(orgs, 1u);
  // A file at the very end of a bank is included after the last data row.
  const std::string bank1 = renderRegion(regionNamed(d, "bank_01.asm"), d);
  const std::size_t include = bank1.find("        INCBIN \"vram/01_FFF0.bin\"\n");
  ASSERT_NE(include, std::string::npos);
  EXPECT_EQ(bank1.find("        DB ", include), std::string::npos);
}

TEST(RomAssets, EveryByteIsPlacedOnce) {
  const CartridgeDisassembly d = lifted(liftingImage());
  const Placement placement = placeBytes(d);
  EXPECT_EQ(placement.unplaced, 0u);
  EXPECT_EQ(placement.placedTwice, 0u);
  EXPECT_EQ(placement.image, liftingImage());
}

TEST(RomAssets, TheAssetLineIsWrittenAndReadBack) {
  const CartridgeDisassembly d = lifted(liftingImage());
  const std::string manifest = renderManifest(d);
  EXPECT_NE(manifest.find("asset    vram/00_9000.bin Vram as dma from $00:9000 bytes 80\n"), std::string::npos);
  EXPECT_NE(manifest.find("asset    hdma/00_9712.bin Cgram as indirect from $00:9712 bytes 2\n"), std::string::npos);
  std::string error;
  const std::optional<ManifestInput> input = parseManifest(manifest, error);
  ASSERT_TRUE(input.has_value()) << error;
  ASSERT_EQ(input->assets.size(), 10u);
  EXPECT_EQ(input->assets[0].file, "vram/00_9000.bin");
  EXPECT_EQ(input->assets[0].first, 0x009000u);
  EXPECT_EQ(input->assets[0].bytes, 80u);

  EXPECT_FALSE(parseManifest("asset vram/x.bin Vram dma from $00:9000 bytes 80\n", error).has_value());
  EXPECT_NE(error.find("an asset is a path"), std::string::npos);
  EXPECT_FALSE(parseManifest("asset vram/x.bin Tiles as dma from $00:9000 bytes 80\n", error).has_value());
  EXPECT_NE(error.find("not a register class"), std::string::npos);
  EXPECT_FALSE(parseManifest("asset vram/x.bin Vram as copy from $00:9000 bytes 80\n", error).has_value());
  EXPECT_NE(error.find("not dma, table, indirect, stream, staged or proven"), std::string::npos);
  EXPECT_FALSE(parseManifest("asset vram/x.bin Vram as dma from $9000 bytes 80\n", error).has_value());
  EXPECT_NE(error.find("$BB:XXXX"), std::string::npos);
  EXPECT_FALSE(parseManifest("asset vram/x.bin Vram as dma from $00:9000 bytes 0\n", error).has_value());
  EXPECT_NE(error.find("not a byte count"), std::string::npos);
}

TEST(RomAssets, APersonsPathSurvivesARunAndAnOrphanIsDropped) {
  CartridgeRequest request;
  request.assets = {ManifestAsset{.file = "vram/tiles.bin", .first = 0x009000u, .bytes = 80,
                                  .classes = {RegisterClass::Vram}, .kind = MovedKind::Dma},
                    ManifestAsset{.file = "vram/nowhere.bin", .first = 0x00C000u, .bytes = 5,
                                  .classes = {RegisterClass::Vram}, .kind = MovedKind::Dma},
                    ManifestAsset{.file = "vram/short.bin", .first = 0x009000u, .bytes = 64,
                                  .classes = {RegisterClass::Vram}, .kind = MovedKind::Dma}};
  const CartridgeDisassembly d = lifted(liftingImage(), request);
  EXPECT_NE(assetNamed(d, "vram/tiles.bin"), nullptr);
  EXPECT_EQ(assetNamed(d, "vram/00_9000.bin"), nullptr);
  EXPECT_NE(renderRegion(regionNamed(d, "bank_00.asm"), d).find("        INCBIN \"vram/tiles.bin\"\n"),
            std::string::npos);
  EXPECT_NE(renderManifest(d).find("asset    vram/tiles.bin Vram as dma from $00:9000 bytes 80\n"),
            std::string::npos);
  EXPECT_TRUE(anyNote(d, "asset vram/nowhere.bin at $00:C000 names no range this run lifted; dropped"));
  // The same first byte with another length is another range, not this file.
  EXPECT_EQ(assetNamed(d, "vram/short.bin"), nullptr);
  EXPECT_TRUE(anyNote(d, "asset vram/short.bin at $00:9000 names no range this run lifted; dropped"));
}

TEST(RomAssets, ATreeWithoutARunLiftsWhatItReadBack) {
  const std::vector<std::uint8_t> rom = liftingImage();
  const CartridgeDisassembly first = lifted(rom);
  CartridgeRequest request;
  request.rom = rom;
  request.captureSound = false;
  request.observeRun = false;
  request.moved = first.moved;
  // The shadow's file is kept by its own line; the rest follow from `moved`.
  std::string error;
  const std::optional<ManifestInput> input = parseManifest(renderManifest(first), error);
  ASSERT_TRUE(input.has_value()) << error;
  request.assets = input->assets;
  const CartridgeDisassembly again = disassembleCartridge(request);
  ASSERT_EQ(again.assets.size(), first.assets.size());
  for (std::size_t i = 0; i < first.assets.size(); ++i) {
    EXPECT_EQ(again.assets[i].file, first.assets[i].file);
    EXPECT_EQ(again.assets[i].bytes, first.assets[i].bytes);
  }
  EXPECT_EQ(renderRegion(regionNamed(again, "bank_00.asm"), again),
            renderRegion(regionNamed(first, "bank_00.asm"), first));
}

// ---- what the shadow lifts ---------------------------------------------------
//
// The staging cartridge builds ranges in work RAM and sends them; the cases pin
// that each is lifted as its source, that a computed range is not, that a
// stream is lifted as the run its carrier read, that a buffer the CPU carries
// out is lifted as its source, that a source built into data for two classes
// is one file under `staged/`, that the lines survive a pass without a run,
// and that the tree still assembles.

namespace {

using examples::stagingImage;

CartridgeDisassembly stagedLift(CartridgeRequest request = {}) {
  static const std::vector<std::uint8_t> rom = stagingImage();
  request.rom = rom;
  request.captureSound = false;
  request.observeRun = true;
  request.runMasterCycles = 4u * kFrame;
  return disassembleCartridge(request);
}

}  // namespace

TEST(RomAssets, AStagedRangeIsLiftedAsItsSourceWhole) {
  const CartridgeDisassembly d = stagedLift();
  ASSERT_EQ(d.assets.size(), 7u) << renderManifest(d);
  // The decoder's stream: the counts, the values and the terminator, eleven bytes.
  const AssetFile* stream = assetNamed(d, "vram/00_9000.bin");
  ASSERT_NE(stream, nullptr);
  EXPECT_EQ(stream->kind, MovedKind::Staged);
  EXPECT_EQ(stream->classes, std::vector<RegisterClass>{RegisterClass::Vram});
  EXPECT_EQ(stream->first, 0x009000u);
  EXPECT_EQ(stream->bytes, (std::vector<std::uint8_t>{0x08u, 0x11u, 0x08u, 0x22u, 0x04u, 0x33u, 0x04u, 0x44u,
                                                      0x08u, 0x55u, 0x00u}));
  // The copied block, and the two bytes the port stores took from it: one file.
  const AssetFile* copy = assetNamed(d, "vram/00_9100.bin");
  ASSERT_NE(copy, nullptr);
  EXPECT_EQ(copy->bytes.size(), 32u);
  EXPECT_EQ(copy->kind, MovedKind::Staged);
  // The bytes the engine carried in through the port and out again: the
  // engine's, and exact.
  const AssetFile* port = assetNamed(d, "vram/00_9300.bin");
  ASSERT_NE(port, nullptr);
  EXPECT_EQ(port->bytes.size(), 32u);
  EXPECT_EQ(port->kind, MovedKind::Staged);
  EXPECT_NE(renderManifest(d).find("staged   vram/00_9300.bin at $7E:0400 bytes 32 to Vram by none exact\n"),
            std::string::npos);
}

TEST(RomAssets, AStagedTableIsPlacedWhereItsUseWent) {
  // A table copied into work RAM and walked by HDMA to a display register: the
  // source is placed as a table is, under `hdma/`, and named `staged`.
  const CartridgeDisassembly d = stagedLift();
  const AssetFile* table = assetNamed(d, "hdma/00_9400.bin");
  ASSERT_NE(table, nullptr);
  EXPECT_EQ(table->kind, MovedKind::Staged);
  EXPECT_EQ(table->classes, std::vector<RegisterClass>{RegisterClass::Display});
  EXPECT_EQ(table->bytes, (std::vector<std::uint8_t>{0x01u, 0x0Fu, 0x00u}));
  EXPECT_NE(renderManifest(d).find("asset    hdma/00_9400.bin Display as staged from $00:9400 bytes 3\n"),
            std::string::npos);
}

TEST(RomAssets, AComputedRangeIsNotLifted) {
  const CartridgeDisassembly d = stagedLift();
  for (const AssetFile& asset : d.assets) {
    EXPECT_EQ(asset.file.rfind("oam/", 0), std::string::npos) << asset.file;
  }
  // And says nothing of it: no note, since nothing was refused.
  EXPECT_FALSE(anyNote(d, "$7F:0300"));
}

TEST(RomAssets, AStreamIsLiftedAsTheRunItsInvocationRead) {
  // The loop carried sixteen bytes and read the end mark after them: the file
  // is the seventeen, and the `streamed` line still says sixteen.
  const CartridgeDisassembly d = stagedLift();
  const AssetFile* palette = assetNamed(d, "cgram/00_9200.bin");
  ASSERT_NE(palette, nullptr);
  EXPECT_EQ(palette->kind, MovedKind::Stream);
  EXPECT_EQ(palette->classes, std::vector<RegisterClass>{RegisterClass::Cgram});
  EXPECT_EQ(palette->first, 0x009200u);
  EXPECT_EQ(palette->bytes.size(), 17u);
  EXPECT_EQ(palette->bytes[0], 0xE0u);
  EXPECT_EQ(palette->bytes[16], 0xFFu);
  EXPECT_NE(renderManifest(d).find("asset    cgram/00_9200.bin Cgram as stream from $00:9200 bytes 17\n"),
            std::string::npos);
  EXPECT_NE(renderManifest(d).find("streamed $00:818F $00:2122 CGDATA Cgram from $00:9200 bytes 16 times 1 at $00-$07 in palette\n"),
            std::string::npos);
  EXPECT_NE(renderRegion(regionNamed(d, "bank_00.asm"), d)
                .find("; ---- $00:9200-$00:9210: 17 bytes a routine carried Cgram data from, in cgram/00_9200.bin\n"
                      "        INCBIN \"cgram/00_9200.bin\"\n"),
            std::string::npos);
}

TEST(RomAssets, ABufferTheCpuCarriesOutIsLiftedAsItsSourceNotAsAStream) {
  const CartridgeDisassembly d = stagedLift();
  const AssetFile* buffer = assetNamed(d, "vram/00_9600.bin");
  ASSERT_NE(buffer, nullptr);
  EXPECT_EQ(buffer->kind, MovedKind::Staged);
  EXPECT_EQ(buffer->classes, std::vector<RegisterClass>{RegisterClass::Vram});
  EXPECT_EQ(buffer->bytes.size(), 16u);
  EXPECT_EQ(buffer->bytes[0], 0xB0u);
  for (const AssetFile& asset : d.assets) {
    EXPECT_FALSE(asset.first == 0x009600u && asset.kind == MovedKind::Stream) << asset.file;
  }
  EXPECT_TRUE(d.notes.empty()) << d.notes.front();  // and nothing was refused in its place
  EXPECT_NE(renderManifest(d).find("asset    vram/00_9600.bin Vram as staged from $00:9600 bytes 16\n"),
            std::string::npos);
}

TEST(RomAssets, ASourceSentToTwoClassesIsOneFileUnderStaged) {
  const CartridgeDisassembly d = stagedLift();
  const AssetFile* both = assetNamed(d, "staged/00_9500.bin");
  ASSERT_NE(both, nullptr);
  EXPECT_EQ(both->kind, MovedKind::Staged);
  EXPECT_EQ(both->classes, (std::vector<RegisterClass>{RegisterClass::Vram, RegisterClass::Cgram}));
  EXPECT_EQ(both->first, 0x009500u);
  EXPECT_EQ(both->bytes.size(), 8u);
  EXPECT_EQ(assetNamed(d, "vram/00_9500.bin"), nullptr);
  EXPECT_EQ(assetNamed(d, "cgram/00_9500.bin"), nullptr);
  EXPECT_FALSE(anyNote(d, "$00:9500"));
  EXPECT_NE(renderManifest(d).find("asset    staged/00_9500.bin Vram+Cgram as staged from $00:9500 bytes 8\n"),
            std::string::npos);
  EXPECT_NE(renderRegion(regionNamed(d, "bank_00.asm"), d)
                .find("; ---- $00:9500-$00:9507: 8 bytes a routine built Vram and Cgram data from, in staged/00_9500.bin\n"
                      "        INCBIN \"staged/00_9500.bin\"\n"),
            std::string::npos);
}

TEST(RomAssets, TheStagedAndStreamLinesAreReadBackAndKeptWithoutARun) {
  const CartridgeDisassembly ran = stagedLift();
  std::string error;
  const std::optional<ManifestInput> input = parseManifest(renderManifest(ran), error);
  ASSERT_TRUE(input.has_value()) << error;
  ASSERT_EQ(input->assets.size(), 7u);
  EXPECT_EQ(input->assets[0].kind, MovedKind::Staged);
  EXPECT_EQ(input->assets[0].classes, std::vector<RegisterClass>{RegisterClass::Vram});
  EXPECT_EQ(input->assets[2].kind, MovedKind::Stream);
  EXPECT_EQ(input->assets[2].classes, std::vector<RegisterClass>{RegisterClass::Cgram});
  EXPECT_EQ(input->assets[4].classes, std::vector<RegisterClass>{RegisterClass::Display});
  EXPECT_EQ(input->assets[5].classes, (std::vector<RegisterClass>{RegisterClass::Vram, RegisterClass::Cgram}));
  EXPECT_EQ(input->assets[5].file, "staged/00_9500.bin");
  EXPECT_FALSE(parseManifest("asset staged/x.bin Vram+Nowhere as staged from $00:9500 bytes 8\n", error).has_value());
  EXPECT_NE(error.find("Nowhere"), std::string::npos);

  CartridgeRequest again;
  static const std::vector<std::uint8_t> rom = stagingImage();
  again.rom = rom;
  again.captureSound = false;
  again.observeRun = false;
  again.moved = input->moved;
  again.assets = input->assets;
  const CartridgeDisassembly kept = disassembleCartridge(again);
  ASSERT_EQ(kept.assets.size(), 7u) << renderManifest(kept);
  for (std::size_t i = 0; i < 7; ++i) {
    EXPECT_EQ(kept.assets[i].file, ran.assets[i].file);
    EXPECT_EQ(kept.assets[i].kind, ran.assets[i].kind);
    EXPECT_EQ(kept.assets[i].bytes, ran.assets[i].bytes);
  }
  // The bank file reads the same either way: the comment names the class,
  // which the line keeps, not the register, which only the run knows.
  EXPECT_EQ(renderRegion(regionNamed(kept, "bank_00.asm"), kept),
            renderRegion(regionNamed(ran, "bank_00.asm"), ran));
  EXPECT_FALSE(anyNote(kept, "names no range this run lifted"));
}

TEST(RomAssets, AStagedFileTheNextRunLiftsWiderIsTheWiderFile) {
  // A person's line for a staged file is read back; a run that lifts a wider
  // file over the same bytes names the wider file, and no note is owed.
  CartridgeRequest request;
  request.assets = {ManifestAsset{.file = "vram/two.bin", .first = 0x009100u, .bytes = 2,
                                  .classes = {RegisterClass::Vram}, .kind = MovedKind::Staged}};
  const CartridgeDisassembly d = stagedLift(request);
  EXPECT_EQ(assetNamed(d, "vram/two.bin"), nullptr);
  EXPECT_NE(assetNamed(d, "vram/00_9100.bin"), nullptr);
  EXPECT_FALSE(anyNote(d, "vram/two.bin"));
}

TEST(RomAssets, TheStagedTreeStillAssemblesToItsImage) {
  const CartridgeDisassembly d = stagedLift();
  const Placement placement = placeBytes(d);
  EXPECT_EQ(placement.unplaced, 0u);
  EXPECT_EQ(placement.placedTwice, 0u);
  EXPECT_EQ(placement.image, stagingImage());
}

// ---- where the files landed --------------------------------------------------
//
// The landing cartridge uploads to every video memory and sets the bases
// afterwards; the cases pin the `landed` line as written, the directory a
// VRAM file takes from its landings, a staged file placed by the landing of
// the range built from it, the widened `streamed` line, that a name and a
// tree without a run keep the path, and that the tree still assembles.

namespace {

using examples::landingImage;

CartridgeDisassembly landingLift(CartridgeRequest request = {}) {
  static const std::vector<std::uint8_t> rom = landingImage();
  request.rom = rom;
  request.captureSound = false;
  request.observeRun = true;
  request.runMasterCycles = 8u * kFrame;
  return disassembleCartridge(request);
}

}  // namespace

TEST(RomLandedFiles, TheLandedLineIsWrittenBesideItsMovedLineAndIsAKnownKind) {
  const CartridgeDisassembly d = landingLift();
  const std::string manifest = renderManifest(d);
  EXPECT_NE(manifest.find("landed   $00:8034 channel 0 memory $00:9000 bytes 64 as dma at $3000-$301F in tiles1+tiles2 times 1\n"),
            std::string::npos) << manifest;
  EXPECT_NE(manifest.find("landed   $00:8129 channel 0 memory $00:9500 bytes 32 as dma at $10-$1F in palette times 1\n"),
            std::string::npos);
  EXPECT_NE(manifest.find("landed   $00:8430 channel 0 memory $00:A000 bytes 544 as dma at $000-$21F in oam times 2\n"),
            std::string::npos);
  EXPECT_NE(manifest.find("landed   $00:8430 channel 0 memory $00:A000 bytes 544 as dma at $010-$21F in oam times 1\n"),
            std::string::npos);
  EXPECT_NE(manifest.find("landed   $00:8401 channel 0 memory $00:9A00 bytes 64 as dma at $0100-$011F in unshown times 1\n"),
            std::string::npos);
  EXPECT_NE(manifest.find("landed   $00:80CA channel 0 memory $00:9300 bytes 32 as dma at $5000-$500F in none times 1\n"),
            std::string::npos);
  // The copy into work RAM through the port lands in no memory, and has no line.
  EXPECT_EQ(manifest.find("landed   $00:8200"), std::string::npos);
  std::string error;
  const std::optional<ManifestInput> input = parseManifest(manifest, error);
  ASSERT_TRUE(input.has_value()) << error;
}

TEST(RomLandedFiles, AVramFileTakesTheDirectoryItsLandingsName) {
  const CartridgeDisassembly d = landingLift();
  std::vector<std::string> files;
  for (const AssetFile& asset : d.assets) files.push_back(asset.file);
  EXPECT_EQ(files, (std::vector<std::string>{
                       "tiles/00_9000.bin",   // the name base BG1 and BG2 share
                       "maps/00_9100.bin",    // BG1's screen
                       "vram/00_9200.bin",    // across a screen and the name bases
                       "vram/00_9300.bin",    // no base reaches it
                       "tiles/00_9400.bin",   // the sprite tiles
                       "cgram/00_9500.bin",   // the palette
                       "maps/00_9700.bin",    // BG2's screen, after the flip
                       "vram/00_9800.bin",    // under Mode 7
                       "tiles/00_9900.bin",   // the rotated words, in the name bases
                       "vram/00_9A00.bin",    // no frame drew it
                       "maps/00_9B00.bin",    // staged through work RAM into BG1's screen
                       "maps/00_9D00.bin",    // BG2's screen through its wrap round the end of VRAM
                       "oam/00_A000.bin",     // the sprite table
                   }))
      << renderManifest(d);
}

TEST(RomLandedFiles, AStagedFileIsPlacedByWhereTheRangeBuiltFromItLanded) {
  const CartridgeDisassembly d = landingLift();
  const AssetFile* staged = assetNamed(d, "maps/00_9B00.bin");
  ASSERT_NE(staged, nullptr);
  EXPECT_EQ(staged->kind, MovedKind::Staged);
  EXPECT_EQ(staged->bytes.size(), 32u);
  EXPECT_NE(renderManifest(d).find("asset    maps/00_9B00.bin Vram as staged from $00:9B00 bytes 32\n"),
            std::string::npos);
}

TEST(RomLandedFiles, TheBankFileIncludesAFileByThePathItsLandingsGaveIt) {
  const CartridgeDisassembly d = landingLift();
  const std::string bank = renderRegion(regionNamed(d, "bank_00.asm"), d);
  EXPECT_NE(bank.find("        INCBIN \"tiles/00_9000.bin\"\n"), std::string::npos);
  EXPECT_NE(bank.find("        INCBIN \"maps/00_9100.bin\"\n"), std::string::npos);
  EXPECT_NE(bank.find("        INCBIN \"vram/00_9300.bin\"\n"), std::string::npos);
}

TEST(RomLandedFiles, ANameAndATreeWithoutARunKeepThePath) {
  const CartridgeDisassembly ran = landingLift();
  std::string error;
  const std::optional<ManifestInput> input = parseManifest(renderManifest(ran), error);
  ASSERT_TRUE(input.has_value()) << error;
  CartridgeRequest again;
  static const std::vector<std::uint8_t> rom = landingImage();
  again.rom = rom;
  again.captureSound = false;
  again.observeRun = false;
  again.moved = input->moved;
  again.assets = input->assets;
  again.assets[0].file = "tiles/font.bin";  // a person's name for the tileset
  const CartridgeDisassembly kept = disassembleCartridge(again);
  ASSERT_EQ(kept.assets.size(), ran.assets.size()) << renderManifest(kept);
  EXPECT_EQ(kept.assets[0].file, "tiles/font.bin");
  for (std::size_t i = 1; i < ran.assets.size(); ++i) EXPECT_EQ(kept.assets[i].file, ran.assets[i].file);
  EXPECT_TRUE(kept.landed.empty()) << "nothing is read back";
  EXPECT_FALSE(anyNote(kept, "names no range this run lifted"));
}

TEST(RomLandedFiles, TheStreamedLineCarriesTheLanding) {
  const CartridgeDisassembly d = stagedLift();
  const std::string manifest = renderManifest(d);
  EXPECT_NE(manifest.find("streamed $00:818F $00:2122 CGDATA Cgram from $00:9200 bytes 16 times 1 at $00-$07 in palette\n"),
            std::string::npos) << manifest;
  EXPECT_NE(manifest.find("streamed $00:84C8 $00:2118 VMDATAL Vram from $7F:0800 bytes 16 times 1 at $"),
            std::string::npos) << manifest;
  EXPECT_NE(manifest.find(" in unshown\n"), std::string::npos);
}

TEST(RomLandedFiles, TheLandingTreeStillAssemblesToItsImage) {
  const CartridgeDisassembly d = landingLift();
  const Placement placement = placeBytes(d);
  EXPECT_EQ(placement.unplaced, 0u);
  EXPECT_EQ(placement.placedTwice, 0u);
  EXPECT_EQ(placement.image, landingImage());
}

TEST(RomAssets, TheInstructionsTextDoesNotChange) {
  const std::vector<std::uint8_t> rom = liftingImage();
  const CartridgeDisassembly with = lifted(rom);
  const CartridgeDisassembly without = disassembleWithoutSound(rom);
  // Without a run the code's own word lifts what it proves: the six files the
  // sixteen starts declare, less the fill, the copy, the read-back and the
  // refusals.
  EXPECT_EQ(without.assets.size(), 6u) << renderManifest(without);
  for (const AssetFile& asset : without.assets) {
    EXPECT_EQ(asset.kind, MovedKind::Proven) << asset.file;
  }
  const std::string lifted0 = renderRegion(regionNamed(with, "bank_00.asm"), with);
  const std::string plain0 = renderRegion(regionNamed(without, "bank_00.asm"), without);
  // The reset routine runs from $8000 to the first data run; that whole prefix
  // is identical, and only what follows — the cuts — differs.
  const std::size_t liftedData = lifted0.find("\n; ---- ");
  const std::size_t plainData = plain0.find("\n; ---- ");
  ASSERT_NE(liftedData, std::string::npos);
  ASSERT_EQ(liftedData, plainData);
  EXPECT_EQ(lifted0.substr(0, liftedData), plain0.substr(0, plainData));
  EXPECT_NE(lifted0, plain0);
}

// ---- what the code proves ------------------------------------------------------
//
// The declaring cartridge starts channel 0 four times from one stretch of code
// and, behind a button the run never presses, sets five more channels up whole;
// the cases pin that a transfer the code proves whole is lifted as a file of
// kind `proven`, that a fill, an unproven source and a table are not, that a
// proven piece and the run's piece of the same bytes are one file of the run's
// kind, that the run confirms every proven transfer it took and a
// contradiction is noted, and that the line is read back for its path.

namespace {

using examples::declaringImage;

CartridgeDisassembly declared(CartridgeRequest request = {}, bool run = true) {
  static const std::vector<std::uint8_t> rom = declaringImage();
  request.rom = rom;
  request.captureSound = false;
  request.observeRun = run;
  request.runMasterCycles = 3u * kFrame;
  return disassembleCartridge(request);
}

}  // namespace

TEST(RomAssets, AProvenTransferIsLiftedAsItsFile) {
  const CartridgeDisassembly d = declared();
  ASSERT_EQ(d.assets.size(), 5u) << renderManifest(d);
  struct Expected {
    const char* file;
    RegisterClass cls;
    MovedKind kind;
    Address first;
    std::size_t bytes;
  };
  // The 48 bytes at `$9100` are proven by channel 1's routine, and the run
  // sent sixteen of them on channel 7: one file, the run's kind.
  const Expected expected[] = {
      {"vram/00_9000.bin", RegisterClass::Vram, MovedKind::Dma, 0x009000u, 32},
      {"vram/00_9040.bin", RegisterClass::Vram, MovedKind::Dma, 0x009040u, 16},
      {"vram/00_9100.bin", RegisterClass::Vram, MovedKind::Dma, 0x009100u, 48},
      {"cgram/00_9200.bin", RegisterClass::Cgram, MovedKind::Dma, 0x009200u, 16},
      {"oam/00_9300.bin", RegisterClass::Oam, MovedKind::Proven, 0x009300u, 544},
  };
  const std::vector<std::uint8_t> rom = declaringImage();
  for (std::size_t i = 0; i < 5; ++i) {
    const AssetFile& asset = d.assets[i];
    EXPECT_EQ(asset.file, expected[i].file);
    EXPECT_EQ(asset.classes, std::vector<RegisterClass>{expected[i].cls}) << asset.file;
    EXPECT_EQ(asset.kind, expected[i].kind) << asset.file;
    EXPECT_EQ(asset.first, expected[i].first) << asset.file;
    EXPECT_EQ(asset.bytes.size(), expected[i].bytes) << asset.file;
    const std::optional<std::size_t> offset = romOffset(CartridgeMap::LoRom, asset.first, rom.size());
    ASSERT_TRUE(offset.has_value());
    EXPECT_TRUE(std::equal(asset.bytes.begin(), asset.bytes.end(), rom.begin() + static_cast<std::ptrdiff_t>(*offset)))
        << asset.file;
  }
  // The run confirmed the five it took, so nothing is noted.
  EXPECT_TRUE(d.notes.empty()) << d.notes.front();
  const std::string manifest = renderManifest(d);
  EXPECT_NE(manifest.find("asset    oam/00_9300.bin Oam as proven from $00:9300 bytes 544\n"), std::string::npos)
      << manifest;
  EXPECT_NE(manifest.find("asset    vram/00_9100.bin Vram as dma from $00:9100 bytes 48\n"), std::string::npos)
      << "the proven piece joins the run's, and the file is the run's kind";
  EXPECT_NE(manifest.find("asset    vram/00_9000.bin Vram as dma from $00:9000 bytes 32\n"), std::string::npos);
  const std::string bank0 = renderRegion(regionNamed(d, "bank_00.asm"), d);
  EXPECT_NE(bank0.find("\n; ---- $00:9300-$00:951F: 544 bytes a transfer the code sets up to carry to OAMDATA, in oam/00_9300.bin\n"
                       "        INCBIN \"oam/00_9300.bin\"\n"),
            std::string::npos)
      << bank0;
  EXPECT_NE(bank0.find("\n; ---- $00:9100-$00:912F: 48 bytes a transfer carried to VMDATAL, in vram/00_9100.bin\n"),
            std::string::npos)
      << bank0;
}

TEST(RomAssets, WhatTheCodeDoesNotProveWholeIsNotLifted) {
  const CartridgeDisassembly d = declared();
  for (const AssetFile& asset : d.assets) {
    EXPECT_NE(asset.first, 0x009080u) << "a transfer set up whole and never started";
    EXPECT_NE(asset.first, 0x009600u) << "a fill";
    EXPECT_NE(asset.first, 0x009700u) << "a table, whose count is not a length";
    EXPECT_NE(asset.first, 0x009800u) << "a source the code does not prove";
  }
  EXPECT_TRUE(d.notes.empty());
}

TEST(RomAssets, WithoutARunEveryProvenTransferIsLifted) {
  const CartridgeDisassembly d = declared({}, false);
  ASSERT_EQ(d.assets.size(), 5u) << renderManifest(d);
  for (const AssetFile& asset : d.assets) {
    EXPECT_EQ(asset.kind, MovedKind::Proven) << asset.file;
  }
  // Channel 7's sixteen bytes lie inside channel 1's forty-eight: one file.
  const AssetFile* block = assetNamed(d, "vram/00_9100.bin");
  ASSERT_NE(block, nullptr);
  EXPECT_EQ(block->bytes.size(), 48u);
  EXPECT_EQ(assetNamed(d, "vram/00_9120.bin"), nullptr);
  const AssetFile* palette = assetNamed(d, "cgram/00_9200.bin");
  ASSERT_NE(palette, nullptr);
  ASSERT_EQ(palette->bytes.size(), 16u);
  EXPECT_EQ(palette->bytes[0], 0xE0u) << "read downward from $920F, lifted in image order";
  EXPECT_TRUE(d.notes.empty());
}

TEST(RomAssets, AProvenTransferTheRunContradictsIsNotedAndTheRunsRangeIsLifted) {
  // A range read back as an earlier run's record: the same start, a shorter
  // count. The code's word and the run's stand beside each other, and the
  // run's is the file — sixteen bytes, not the thirty-two the proof would add.
  CartridgeRequest request;
  request.moved = {MovedRange{.site = 0x008025u,
                              .channel = 0,
                              .toRegister = true,
                              .registerAddress = 0x002118u,
                              .registerName = "VMDATAL",
                              .registerClass = RegisterClass::Vram,
                              .memory = 0x009000u,
                              .step = MovedStep::Increment,
                              .bytes = 16,
                              .kind = MovedKind::Dma,
                              .times = 1}};
  const CartridgeDisassembly d = declared(request, false);
  const AssetFile* tileset = assetNamed(d, "vram/00_9000.bin");
  ASSERT_NE(tileset, nullptr);
  EXPECT_EQ(tileset->kind, MovedKind::Dma);
  EXPECT_EQ(tileset->bytes.size(), 16u);
  EXPECT_TRUE(anyNote(d, "dma $00:8007 channel 0: the code proves $00:9000 increment bytes 32; "
                         "the run moved $00:9000 increment bytes 16"))
      << (d.notes.empty() ? std::string("no notes") : d.notes.front());
  EXPECT_EQ(assetNamed(d, "vram/00_9040.bin")->kind, MovedKind::Proven) << "the others are untouched";
}

TEST(RomAssets, AProvenFilesPathSurvivesAndAnOrphanIsDropped) {
  CartridgeRequest request;
  request.assets = {ManifestAsset{.file = "oam/sprites.bin", .first = 0x009300u, .bytes = 544,
                                  .classes = {RegisterClass::Oam}, .kind = MovedKind::Proven},
                    ManifestAsset{.file = "oam/gone.bin", .first = 0x009500u, .bytes = 5,
                                  .classes = {RegisterClass::Oam}, .kind = MovedKind::Proven}};
  const CartridgeDisassembly d = declared(request);
  EXPECT_NE(assetNamed(d, "oam/sprites.bin"), nullptr);
  EXPECT_EQ(assetNamed(d, "oam/00_9300.bin"), nullptr);
  EXPECT_NE(renderRegion(regionNamed(d, "bank_00.asm"), d).find("        INCBIN \"oam/sprites.bin\"\n"),
            std::string::npos);
  EXPECT_TRUE(anyNote(d, "asset oam/gone.bin at $00:9500 names no range this run lifted; dropped"));
  std::string error;
  const std::optional<ManifestInput> input = parseManifest(renderManifest(d), error);
  ASSERT_TRUE(input.has_value()) << error;
  const auto sprites = std::find_if(input->assets.begin(), input->assets.end(),
                                    [](const ManifestAsset& a) { return a.file == "oam/sprites.bin"; });
  ASSERT_NE(sprites, input->assets.end());
  EXPECT_EQ(sprites->kind, MovedKind::Proven);
}

TEST(RomAssets, TheProvenTreeStillAssemblesToItsImage) {
  const CartridgeDisassembly d = declared();
  const Placement placement = placeBytes(d);
  EXPECT_EQ(placement.unplaced, 0u);
  EXPECT_EQ(placement.placedTwice, 0u);
  EXPECT_EQ(placement.image, declaringImage());
}

}  // namespace snaggletooth::disasm
