// A whole cartridge disassembled into a source tree. The images here are built
// by hand: a LoROM header, a few banks of 65816 code that call and jump across
// them, and one cartridge whose reset code speaks the audio upload protocol, so
// each case pins one rule about how the tree is traced, split, matched to the
// image, and written.

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "rom/rom_disasm.h"

namespace snaggletooth::disasm {
namespace {

// A LoROM image of `banks` 32 KB banks whose header names reset at $8000 and
// leaves every other vector unused.
std::vector<std::uint8_t> loRomImage(std::size_t banks) {
  std::vector<std::uint8_t> rom(banks * 0x8000u, 0u);
  const std::size_t site = 0x7FC0u;
  for (std::size_t i = 0; i < 21; ++i) rom[site + i] = 'A';
  rom[site + 0x15] = 0x20u;  // the map-mode byte: LoROM
  rom[site + 0x1C] = 0x34u;  // a complement and checksum that agree
  rom[site + 0x1D] = 0x12u;
  rom[site + 0x1E] = 0xCBu;
  rom[site + 0x1F] = 0xEDu;
  rom[site + 0x3C] = 0x00u;  // reset -> $8000
  rom[site + 0x3D] = 0x80u;
  return rom;
}

void put(std::vector<std::uint8_t>& rom, std::size_t offset, std::initializer_list<std::uint8_t> bytes) {
  std::size_t i = offset;
  for (const std::uint8_t b : bytes) rom[i++] = b;
}

// Three banks. Reset switches to native mode with both widths sixteen, calls a
// routine in bank $01, and jumps to bank $02 through its mirror at $82. The
// bank-$01 routine calls back into bank $00, at an address only it reaches. The
// bank-$02 code narrows the accumulator, calls into work RAM, and ends in a
// jump through a table; the table's one target sits at $02:8100.
std::vector<std::uint8_t> threeBankImage() {
  std::vector<std::uint8_t> rom = loRomImage(3);
  put(rom, 0x0000u, {0x18u,                       // CLC
                     0xFBu,                       // XCE
                     0xC2u, 0x30u,                // REP #$30
                     0x22u, 0x00u, 0x80u, 0x01u,  // JSL $01:8000
                     0x5Cu, 0x00u, 0x80u, 0x82u});  // JML $82:8000
  put(rom, 0x0100u, {0xEAu,    // $00:8100 NOP   (reached only from bank $01)
                     0x6Bu});  //          RTL
  put(rom, 0x8000u, {0xA9u, 0x34u, 0x12u,          // LDA #$1234
                     0x22u, 0x00u, 0x81u, 0x00u,   // JSL $00:8100
                     0x6Bu});                      // RTL
  put(rom, 0x10000u, {0xE2u, 0x20u,                // SEP #$20
                      0xA9u, 0x12u,                // LDA #$12
                      0x22u, 0x00u, 0x20u, 0x7Eu,  // JSL $7E:2000
                      0x7Cu, 0x00u, 0x81u});       // JMP (!$8100,X)
  put(rom, 0x10100u, {0xEAu,          // NOP        (reached only through the table)
                      0x00u, 0x01u,   // BRK #$01   (continues at the BRK vector's handler)
                      0x6Bu});        // RTL
  return rom;
}

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

// The 24-byte program the uploading cartridge sends: three instructions, sixteen
// NOPs (whose opcode is zero, the value cleared audio memory already holds) and
// STOP. It sits in the image at $80A0.
const std::vector<std::uint8_t> kUploadedProgram = {
    0xE8u, 0x5Au,         // MOV A,#$5A
    0xC5u, 0x50u, 0x02u,  // MOV !$0250,A
    0xE8u, 0x00u,         // MOV A,#$00
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,  // NOP x16
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
    0xFFu,                // STOP
};

// The 20 bytes the same cartridge sends after it, to the address right after
// the program, from a table elsewhere in the image ($8100).
const std::vector<std::uint8_t> kUploadedTable = {
    0x9Cu, 0x3Du, 0x71u, 0xE2u, 0x58u, 0xA7u, 0x06u, 0xB4u, 0xC9u, 0x1Fu,
    0x63u, 0x8Eu, 0x2Bu, 0xF5u, 0x4Au, 0xD0u, 0x17u, 0x86u, 0xEBu, 0x39u,
};

// One bank whose reset code speaks the upload protocol: waits for the ready
// bytes, sends the program to $0200 one acknowledged byte at a time, sends the
// table to $0218 the same way, and starts the program. The two land end to end
// in audio memory but come from two places in the image.
std::vector<std::uint8_t> uploadingImage() {
  std::vector<std::uint8_t> rom = loRomImage(1);
  put(rom, 0x0000u, {
      0x78u,                     // 8000 SEI
      0x18u,                     // 8001 CLC
      0xFBu,                     // 8002 XCE
      0xE2u, 0x30u,              // 8003 SEP #$30
      0xADu, 0x40u, 0x21u,       // 8005 LDA $2140      ; ready?
      0xC9u, 0xAAu,              // 8008 CMP #$AA
      0xD0u, 0xF9u,              // 800A BNE $8005
      0xADu, 0x41u, 0x21u,       // 800C LDA $2141
      0xC9u, 0xBBu,              // 800F CMP #$BB
      0xD0u, 0xF2u,              // 8011 BNE $8005
      0xA9u, 0x00u,              // 8013 LDA #$00       ; destination $0200
      0x8Du, 0x42u, 0x21u,       // 8015 STA $2142
      0xA9u, 0x02u,              // 8018 LDA #$02
      0x8Du, 0x43u, 0x21u,       // 801A STA $2143
      0xA9u, 0x01u,              // 801D LDA #$01       ; a transfer, not a start
      0x8Du, 0x41u, 0x21u,       // 801F STA $2141
      0xA9u, 0xCCu,              // 8022 LDA #$CC       ; the kick
      0x8Du, 0x40u, 0x21u,       // 8024 STA $2140
      0xCDu, 0x40u, 0x21u,       // 8027 CMP $2140      ; acknowledged?
      0xD0u, 0xFBu,              // 802A BNE $8027
      0xA2u, 0x00u,              // 802C LDX #$00
      0xBDu, 0xA0u, 0x80u,       // 802E LDA $80A0,X    ; the program's next byte
      0x8Du, 0x41u, 0x21u,       // 8031 STA $2141
      0x8Au,                     // 8034 TXA            ; its index
      0x8Du, 0x40u, 0x21u,       // 8035 STA $2140
      0xCDu, 0x40u, 0x21u,       // 8038 CMP $2140      ; acknowledged?
      0xD0u, 0xFBu,              // 803B BNE $8038
      0xE8u,                     // 803D INX
      0xE0u, 0x18u,              // 803E CPX #$18
      0xD0u, 0xECu,              // 8040 BNE $802E
      0xA9u, 0x18u,              // 8042 LDA #$18       ; destination $0218
      0x8Du, 0x42u, 0x21u,       // 8044 STA $2142
      0xA9u, 0x02u,              // 8047 LDA #$02
      0x8Du, 0x43u, 0x21u,       // 8049 STA $2143
      0xA9u, 0x01u,              // 804C LDA #$01       ; a transfer
      0x8Du, 0x41u, 0x21u,       // 804E STA $2141
      0xA9u, 0x19u,              // 8051 LDA #$19       ; two past the last index
      0x8Du, 0x40u, 0x21u,       // 8053 STA $2140
      0xCDu, 0x40u, 0x21u,       // 8056 CMP $2140      ; acknowledged?
      0xD0u, 0xFBu,              // 8059 BNE $8056
      0xA2u, 0x00u,              // 805B LDX #$00
      0xBDu, 0x00u, 0x81u,       // 805D LDA $8100,X    ; the table's next byte
      0x8Du, 0x41u, 0x21u,       // 8060 STA $2141
      0x8Au,                     // 8063 TXA
      0x8Du, 0x40u, 0x21u,       // 8064 STA $2140
      0xCDu, 0x40u, 0x21u,       // 8067 CMP $2140
      0xD0u, 0xFBu,              // 806A BNE $8067
      0xE8u,                     // 806C INX
      0xE0u, 0x14u,              // 806D CPX #$14
      0xD0u, 0xECu,              // 806F BNE $805D
      0xA9u, 0x00u,              // 8071 LDA #$00       ; start at $0200
      0x8Du, 0x42u, 0x21u,       // 8073 STA $2142
      0xA9u, 0x02u,              // 8076 LDA #$02
      0x8Du, 0x43u, 0x21u,       // 8078 STA $2143
      0xA9u, 0x00u,              // 807B LDA #$00       ; zero starts the program
      0x8Du, 0x41u, 0x21u,       // 807D STA $2141
      0xA9u, 0x15u,              // 8080 LDA #$15       ; two past the last index
      0x8Du, 0x40u, 0x21u,       // 8082 STA $2140
      0xCDu, 0x40u, 0x21u,       // 8085 CMP $2140      ; acknowledged?
      0xD0u, 0xFBu,              // 8088 BNE $8085
      0xDBu,                     // 808A STP
  });
  std::copy(kUploadedProgram.begin(), kUploadedProgram.end(), rom.begin() + 0xA0);
  std::copy(kUploadedTable.begin(), kUploadedTable.end(), rom.begin() + 0x100);
  return rom;
}

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

}  // namespace
}  // namespace snaggletooth::disasm
