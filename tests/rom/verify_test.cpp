// Verification of a source tree against its image. Each case takes a tree the
// disassembler wrote for one of the hand-built cartridges, keeps it as it is or
// damages it in one way, and pins what the report says: identical, or which
// file differs where, what did not assemble, what nobody produced.

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "cartridge_fixtures.h"
#include "gtest/gtest.h"
#include "rom/rom_verify.h"

namespace snaggletooth::disasm {
namespace {

using fixtures::threeBankImage;
using fixtures::uploadingImage;

// A tree as text: the manifest and every file, by the path the manifest names.
struct Tree {
  std::string manifest;
  std::map<std::string, std::string> files;
};

Tree treeOf(const CartridgeDisassembly& d) {
  Tree tree;
  tree.manifest = renderManifest(d);
  for (const RegionListing& region : d.regions) {
    tree.files[region.region.file] = renderRegion(region, d);
  }
  if (d.sound) tree.files[d.sound->file] = renderSoundProgram(*d.sound);
  return tree;
}

Tree treeOf(std::span<const std::uint8_t> rom, bool sound) {
  CartridgeRequest request;
  request.rom = rom;
  request.captureSound = sound;
  return treeOf(disassembleCartridge(request));
}

VerifyReport verify(const Tree& tree, std::span<const std::uint8_t> rom) {
  std::string error;
  const std::optional<ManifestInput> manifest = parseManifest(tree.manifest, error);
  EXPECT_TRUE(manifest.has_value()) << error;
  if (!manifest) return VerifyReport{};
  return verifyProject(*manifest, rom, [&](const std::string& file) -> std::optional<std::string> {
    const auto found = tree.files.find(file);
    if (found == tree.files.end()) return std::nullopt;
    return found->second;
  });
}

// A copy, not a reference: a reference returned past a temporary argument is
// what one compiler's dangling-reference check refuses.
VerifiedFile fileNamed(const VerifyReport& report, const std::string& name) {
  for (const VerifiedFile& file : report.files) {
    if (file.file == name) return file;
  }
  ADD_FAILURE() << "no file " << name;
  return report.files.empty() ? VerifiedFile{} : report.files.front();
}

std::string replaceFirst(std::string text, const std::string& from, const std::string& to) {
  const std::size_t at = text.find(from);
  EXPECT_NE(at, std::string::npos) << from;
  if (at != std::string::npos) text.replace(at, from.size(), to);
  return text;
}

}  // namespace

// ---- the manifest, read in full ---------------------------------------------

TEST(RomVerify, TheManifestReadsBackTheMapAndTheSoundProgram) {
  const std::vector<std::uint8_t> rom = uploadingImage();
  const Tree tree = treeOf(rom, true);
  std::string error;
  const std::optional<ManifestInput> input = parseManifest(tree.manifest, error);
  ASSERT_TRUE(input.has_value()) << error;
  ASSERT_TRUE(input->map.has_value());
  EXPECT_EQ(*input->map, CartridgeMap::LoRom);
  ASSERT_TRUE(input->sound.has_value());
  EXPECT_EQ(input->sound->file, "apu/driver.asm");
  EXPECT_EQ(input->sound->entry, 0x0200u);
  ASSERT_EQ(input->sound->blocks.size(), 2u);
  EXPECT_EQ(input->sound->blocks[0].apuAddress, 0x0200u);
  EXPECT_EQ(input->sound->blocks[0].length, 24u);
  EXPECT_EQ(input->sound->blocks[0].romOffset.value_or(0), 0xA0u);
  EXPECT_EQ(input->sound->blocks[1].apuAddress, 0x0218u);
  EXPECT_EQ(input->sound->blocks[1].length, 20u);
  EXPECT_EQ(input->sound->blocks[1].romOffset.value_or(0), 0x100u);

  const std::optional<ManifestInput> unplaced = parseManifest(
      "map HiROM\nsound apu/driver.asm SPC700 entry $0500\nblock apu/driver.asm $0500 100 unplaced\n",
      error);
  ASSERT_TRUE(unplaced.has_value()) << error;
  EXPECT_EQ(*unplaced->map, CartridgeMap::HiRom);
  ASSERT_EQ(unplaced->sound->blocks.size(), 1u);
  EXPECT_FALSE(unplaced->sound->blocks[0].romOffset.has_value());
  EXPECT_EQ(unplaced->sound->blocks[0].length, 100u);

  EXPECT_FALSE(parseManifest("map SuperROM\n", error).has_value());
  EXPECT_NE(error.find("not a map"), std::string::npos);
  EXPECT_FALSE(parseManifest("sound a.asm SPC700 $0500\n", error).has_value());
  EXPECT_NE(error.find("entry"), std::string::npos);
  EXPECT_FALSE(parseManifest("block a.asm $0500 12 somewhere\n", error).has_value());
  EXPECT_NE(error.find("`at`"), std::string::npos);
  EXPECT_FALSE(
      parseManifest("sound a.asm SPC700 entry $0500\nblock b.asm $0500 12 at $000100\n", error)
          .has_value());
  EXPECT_NE(error.find("names a file the sound line does not"), std::string::npos);
  EXPECT_FALSE(parseManifest("block a.asm $0500 12 at $100\n", error).has_value());
  EXPECT_NE(error.find("$XXXXXX"), std::string::npos);
}

// ---- a tree as written ------------------------------------------------------

TEST(RomVerify, ATreeTheDisassemblerWroteAssemblesToItsImage) {
  const std::vector<std::uint8_t> rom = uploadingImage();
  const VerifyReport report = verify(treeOf(rom, true), rom);
  EXPECT_TRUE(report.error.empty()) << report.error;
  EXPECT_TRUE(report.identical()) << renderReport(report);
  EXPECT_EQ(report.compared, rom.size());
  EXPECT_EQ(report.differing, 0u);
  EXPECT_EQ(report.unplaced, 0u);
  EXPECT_EQ(report.placedTwice, 0u);
  EXPECT_EQ(report.image, rom);
  ASSERT_EQ(report.files.size(), 2u);
  // The bank file is three pieces around the two uploaded blocks; the sound
  // file is the two blocks.
  const VerifiedFile bank = fileNamed(report, "bank_00.asm");
  EXPECT_EQ(bank.chip, "65816");
  EXPECT_EQ(bank.runs, 3u);
  EXPECT_EQ(bank.bytes, 0x8000u - 24u - 20u);
  const VerifiedFile sound = fileNamed(report, "apu/driver.asm");
  EXPECT_EQ(sound.chip, "SPC700");
  EXPECT_EQ(sound.runs, 2u);
  EXPECT_EQ(sound.bytes, 44u);
}

TEST(RomVerify, ALabelAcrossAPieceOfTheBankFileAssembles) {
  // The jump before the uploaded blocks names the line after them, which is in
  // the file's last piece under its own ORG.
  const std::vector<std::uint8_t> rom = uploadingImage();
  const Tree tree = treeOf(rom, true);
  const std::string& bank = tree.files.at("bank_00.asm");
  EXPECT_NE(bank.find("        JMP !loc_008120 "), std::string::npos) << bank;
  EXPECT_NE(bank.find("        ORG $00:8114\n"), std::string::npos);
  EXPECT_NE(bank.find("\nloc_008120:\n"), std::string::npos);
  EXPECT_LT(bank.find("JMP !loc_008120"), bank.find("loc_008120:"));
  EXPECT_NE(bank.find("        BNE loc_008005 "), std::string::npos);
  EXPECT_TRUE(verify(tree, rom).identical());
}

TEST(RomVerify, ThreeBanksWithoutASoundProgramAssembleToTheImage) {
  const std::vector<std::uint8_t> rom = threeBankImage();
  const Tree tree = treeOf(rom, false);
  // A call into another file keeps its address: the lexicon has no symbol that
  // crosses a file.
  EXPECT_NE(tree.files.at("bank_00.asm").find("JSL $01:8000"), std::string::npos);
  const VerifyReport report = verify(tree, rom);
  EXPECT_TRUE(report.identical()) << renderReport(report);
  ASSERT_EQ(report.files.size(), 3u);
  for (const VerifiedFile& file : report.files) {
    EXPECT_EQ(file.runs, 1u) << file.file;
    EXPECT_EQ(file.bytes, 0x8000u) << file.file;
  }
  EXPECT_EQ(report.compared, rom.size());
}

TEST(RomVerify, AnUnplacedBlockIsTheBanksAndIsNotCompared) {
  std::vector<std::uint8_t> rom = uploadingImage();
  std::copy(fixtures::kUploadedProgram.begin(), fixtures::kUploadedProgram.end(),
            rom.begin() + 0x1000);
  const VerifyReport report = verify(treeOf(rom, true), rom);
  EXPECT_TRUE(report.identical()) << renderReport(report);
  EXPECT_EQ(fileNamed(report, "apu/driver.asm").runs, 1u);  // the table; the program stayed in the bank
  EXPECT_EQ(report.compared, rom.size());
}

TEST(RomVerify, AHandlerNamedLikeAMnemonicStillAssembles) {
  const std::vector<std::uint8_t> rom = fixtures::copVectorImage();
  const Tree tree = treeOf(rom, false);
  EXPECT_NE(tree.files.at("bank_00.asm").find("\ncop_handler:\n"), std::string::npos);
  const VerifyReport report = verify(tree, rom);
  EXPECT_TRUE(report.identical()) << renderReport(report);
  EXPECT_TRUE(fileNamed(report, "bank_00.asm").errors.empty());
}

// An instruction across the edge of two uploaded pieces is kept whole, since
// the two are one run of the program; an instruction that runs past the end of
// what was uploaded is not one, and the bytes of it that were uploaded are
// written as data. Either way every uploaded byte is in the file, and a branch
// to a line the file does not hold as an instruction stays an address.
TEST(RomVerify, AnInstructionAcrossTwoUploadedPiecesIsKeptWhole) {
  const std::vector<std::uint8_t> rom = fixtures::straddlingUploadImage();
  const Tree tree = treeOf(rom, true);
  const std::string& sound = tree.files.at("apu/driver.asm");
  EXPECT_NE(sound.find("; $0200: 24 bytes, read from image offset $0000A0\n"), std::string::npos) << sound;
  EXPECT_NE(sound.find("; $0218: 20 bytes, read from image offset $000100\n"), std::string::npos);
  EXPECT_NE(sound.find("        MOV A,#$9C "), std::string::npos) << sound;
  EXPECT_NE(sound.find("; $0217  E8 9C"), std::string::npos) << sound;
  EXPECT_EQ(sound.find("        ORG $0218\n"), std::string::npos) << "one piece, no cut at the edge";
  EXPECT_NE(sound.find("        BRA $022B "), std::string::npos) << sound;
  EXPECT_NE(sound.find("        DB $E8 "), std::string::npos) << sound;
  EXPECT_NE(sound.find("; $022B  |"), std::string::npos) << sound;
  const VerifyReport report = verify(tree, rom);
  EXPECT_TRUE(report.identical()) << renderReport(report);
  EXPECT_EQ(fileNamed(report, "apu/driver.asm").runs, 2u);
  EXPECT_EQ(report.compared, rom.size());
}

// ---- what a damaged tree reports --------------------------------------------

TEST(RomVerify, AnEditedInstructionIsReportedWhereItDiffers) {
  const std::vector<std::uint8_t> rom = uploadingImage();
  Tree tree = treeOf(rom, true);
  tree.files["bank_00.asm"] = replaceFirst(tree.files["bank_00.asm"], "        SEI ", "        CLI ");
  const VerifyReport report = verify(tree, rom);
  EXPECT_FALSE(report.identical());
  EXPECT_EQ(report.differing, 1u);
  EXPECT_EQ(report.unplaced, 0u);
  EXPECT_EQ(fileNamed(report, "bank_00.asm").differing, 1u);
  ASSERT_EQ(report.mismatches.size(), 1u);
  EXPECT_EQ(report.mismatches[0].file, "bank_00.asm");
  EXPECT_EQ(report.mismatches[0].address, 0x008000u);
  EXPECT_EQ(report.mismatches[0].romOffset, 0u);
  EXPECT_EQ(report.mismatches[0].length, 0xA0u);
  EXPECT_EQ(report.mismatches[0].firstDifference, 0u);
  EXPECT_EQ(report.image[0], 0x58u);  // CLI, where the image holds SEI
  const std::string text = renderReport(report);
  EXPECT_NE(text.find("bank_00.asm $00:8000-$00:809F at $000000: first difference at $000000"),
            std::string::npos) << text;
  EXPECT_NE(text.find("1 differ"), std::string::npos);
  EXPECT_NE(text.find("does not assemble to the image"), std::string::npos);
}

TEST(RomVerify, AnEditedSoundProgramIsReportedAtItsBlock) {
  const std::vector<std::uint8_t> rom = uploadingImage();
  Tree tree = treeOf(rom, true);
  tree.files["apu/driver.asm"] =
      replaceFirst(tree.files["apu/driver.asm"], "MOV A,#$5A", "MOV A,#$5B");
  const VerifyReport report = verify(tree, rom);
  EXPECT_FALSE(report.identical());
  ASSERT_EQ(report.mismatches.size(), 1u);
  EXPECT_EQ(report.mismatches[0].file, "apu/driver.asm");
  EXPECT_EQ(report.mismatches[0].address, 0x0200u);
  EXPECT_EQ(report.mismatches[0].addressBits, 16u);
  EXPECT_EQ(report.mismatches[0].romOffset, 0xA0u);
  EXPECT_EQ(report.mismatches[0].firstDifference, 0xA1u);
  EXPECT_NE(renderReport(report).find("apu/driver.asm $0200-$0217 at $0000A0"), std::string::npos);
}

TEST(RomVerify, AFileThatDoesNotAssembleIsReportedWithItsLine) {
  const std::vector<std::uint8_t> rom = threeBankImage();
  Tree tree = treeOf(rom, false);
  tree.files["bank_01.asm"] += "        BOGUS\n";
  const VerifyReport report = verify(tree, rom);
  EXPECT_FALSE(report.identical());
  const VerifiedFile file = fileNamed(report, "bank_01.asm");
  ASSERT_EQ(file.errors.size(), 1u);
  EXPECT_EQ(file.errors[0].file, "bank_01.asm");
  EXPECT_NE(file.errors[0].message.find("BOGUS"), std::string::npos);
  EXPECT_EQ(file.runs, 0u);
  // Nothing of that bank was produced; the other two banks still were.
  EXPECT_EQ(report.unplaced, 0x8000u);
  EXPECT_EQ(report.compared, 0x10000u);
  const std::string text = renderReport(report);
  EXPECT_NE(text.find("bank_01.asm: 1 error, not assembled"), std::string::npos) << text;
  EXPECT_NE(text.find("bank_01.asm:"), std::string::npos);
  EXPECT_NE(text.find("32768 produced by no file"), std::string::npos);
}

TEST(RomVerify, AMissingFileIsReported) {
  const std::vector<std::uint8_t> rom = threeBankImage();
  Tree tree = treeOf(rom, false);
  tree.files.erase("bank_02.asm");
  const VerifyReport report = verify(tree, rom);
  EXPECT_FALSE(report.identical());
  EXPECT_EQ(fileNamed(report, "bank_02.asm").problem, "cannot read bank_02.asm");
  EXPECT_EQ(report.unplaced, 0x8000u);
  EXPECT_NE(renderReport(report).find("bank_02.asm: cannot read bank_02.asm"), std::string::npos);
}

TEST(RomVerify, ASplitThatLeavesABankOutIsNotIdentical) {
  const std::vector<std::uint8_t> rom = threeBankImage();
  CartridgeRequest request;
  request.rom = rom;
  request.captureSound = false;
  request.regions = {SourceRegion{.file = "bank_00.asm", .first = 0x008000u, .last = 0x00FFFFu}};
  const VerifyReport report = verify(treeOf(disassembleCartridge(request)), rom);
  EXPECT_FALSE(report.identical());
  EXPECT_EQ(report.differing, 0u);
  EXPECT_EQ(report.unplaced, 0x10000u);
  EXPECT_EQ(report.compared, 0x8000u);
}

TEST(RomVerify, AByteTwoFilesProduceIsReported) {
  const std::vector<std::uint8_t> rom = threeBankImage();
  CartridgeRequest request;
  request.rom = rom;
  request.captureSound = false;
  request.regions = {SourceRegion{.file = "init.asm", .first = 0x008000u, .last = 0x0080FFu},
                     SourceRegion{.file = "bank_00.asm", .first = 0x008000u, .last = 0x00FFFFu},
                     SourceRegion{.file = "bank_01.asm", .first = 0x018000u, .last = 0x01FFFFu},
                     SourceRegion{.file = "bank_02.asm", .first = 0x028000u, .last = 0x02FFFFu}};
  const VerifyReport report = verify(treeOf(disassembleCartridge(request)), rom);
  EXPECT_FALSE(report.identical());
  EXPECT_EQ(report.differing, 0u);
  EXPECT_EQ(report.unplaced, 0u);
  EXPECT_EQ(report.placedTwice, 0x100u);
  EXPECT_NE(renderReport(report).find("256 produced twice"), std::string::npos);
}

TEST(RomVerify, ABlockTheSoundFileDoesNotEmitIsReported) {
  const std::vector<std::uint8_t> rom = uploadingImage();
  Tree tree = treeOf(rom, true);
  tree.files["apu/driver.asm"] = "        ORG $0218\n        DB $9C\n";
  const VerifyReport report = verify(tree, rom);
  EXPECT_FALSE(report.identical());
  const VerifiedFile sound = fileNamed(report, "apu/driver.asm");
  EXPECT_TRUE(sound.errors.empty());
  EXPECT_EQ(sound.runs, 0u);
  EXPECT_NE(sound.problem.find("not all emitted by the file"), std::string::npos) << sound.problem;
  EXPECT_EQ(report.unplaced, 44u);
}

TEST(RomVerify, AManifestForAnotherImageIsRefused) {
  const std::vector<std::uint8_t> rom = threeBankImage();
  const Tree tree = treeOf(rom, false);
  const VerifyReport report = verify(tree, uploadingImage());
  EXPECT_NE(report.error.find("written for an image of 98304 bytes"), std::string::npos)
      << report.error;
  EXPECT_FALSE(report.identical());
  EXPECT_TRUE(report.files.empty());
  EXPECT_EQ(renderReport(report), report.error + "\n");
}

TEST(RomVerify, AManifestWithoutAMapIsRefused) {
  const std::vector<std::uint8_t> rom = threeBankImage();
  std::string error;
  const std::optional<ManifestInput> manifest =
      parseManifest("file bank_00.asm 65816 $00:8000 $00:FFFF\n", error);
  ASSERT_TRUE(manifest.has_value()) << error;
  const VerifyReport report =
      verifyProject(*manifest, rom, [](const std::string&) { return std::optional<std::string>(); });
  EXPECT_EQ(report.error, "the manifest names no map");
  EXPECT_FALSE(report.identical());
}

// ---- the report, and the tree on disk ---------------------------------------

TEST(RomVerify, TheReportNamesEveryFileAndTheVerdict) {
  const std::vector<std::uint8_t> rom = uploadingImage();
  const std::string text = renderReport(verify(treeOf(rom, true), rom));
  EXPECT_NE(text.find("bank_00.asm: 3 ranges, 32724 bytes, identical\n"), std::string::npos) << text;
  EXPECT_NE(text.find("apu/driver.asm: 2 blocks, 44 bytes, identical\n"), std::string::npos) << text;
  EXPECT_NE(text.find("32768 of 32768 bytes compared, 0 differ\n"), std::string::npos) << text;
  EXPECT_NE(text.find("the tree assembles to the image\n"), std::string::npos) << text;
  EXPECT_EQ(text.find("produced by no file"), std::string::npos);
}

TEST(RomVerify, TheTreeOnDiskVerifies) {
  const std::vector<std::uint8_t> rom = uploadingImage();
  CartridgeRequest request;
  request.rom = rom;
  const CartridgeDisassembly d = disassembleCartridge(request);
  const std::filesystem::path dir = std::filesystem::temp_directory_path() / "snaggletooth-verify-test";
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::string error;
  ASSERT_TRUE(writeProject(d, dir, error)) << error;

  const VerifyReport report = verifyTree(dir, rom);
  EXPECT_TRUE(report.identical()) << renderReport(report);
  EXPECT_EQ(report.image, rom);
  // Another image is refused; a directory without a manifest is reported.
  EXPECT_NE(verifyTree(dir, threeBankImage()).error.find("written for"), std::string::npos);
  EXPECT_NE(verifyTree(dir / "missing", rom).error.find("cannot read"), std::string::npos);
  std::filesystem::remove_all(dir, ec);
}

}  // namespace snaggletooth::disasm
