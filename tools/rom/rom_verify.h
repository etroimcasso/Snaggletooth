#pragma once

// Verification of a source tree against the cartridge image it describes: the
// manifest read, every file assembled under its chip's dialect, each range a
// bank file emits placed at the image offset its address reads from, each block
// of the sound program placed at the offset the manifest recorded, and the whole
// compared with the image byte for byte.
//
// The answer is the one question a source tree has to settle — whether it
// rebuilds the image it came from — so the report says which file differs,
// where, and what nobody produced, and the tree is identical only when every
// file assembled, every byte was produced exactly once, and none differ.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "assembler/assembler.h"
#include "rom/rom_disasm.h"

namespace snaggletooth::disasm {

// A run of bytes a file produced that differs from the image: the file, where
// the run was placed, and the first byte in it that differs.
struct VerifyMismatch {
  std::string file;
  Address address = 0;               // the first address of the run, as the file placed it
  unsigned addressBits = 24;         // how the address prints: 24 for a bank file, 16 for the sound file
  std::size_t romOffset = 0;         // where the run lands in the image
  std::size_t length = 0;            // bytes in the run
  std::size_t firstDifference = 0;   // the image offset of the first byte that differs
};

// One file of the tree, verified.
struct VerifiedFile {
  std::string file;
  std::string chip;                            // "65816" or "SPC700"
  std::vector<assembler::Diagnostic> errors;   // what stopped its assembly; empty when it assembled
  std::string problem;                         // what could not be done at all; empty when it could
  std::size_t runs = 0;                        // ranges compared, or placed blocks for the sound file
  std::size_t bytes = 0;                       // bytes compared
  std::size_t differing = 0;                   // of them, the bytes that differ
};

// What verifying a tree found.
struct VerifyReport {
  std::vector<VerifiedFile> files;
  std::vector<VerifyMismatch> mismatches;
  std::vector<std::uint8_t> image;   // the image the tree assembles to; a byte nobody produced is zero
  std::size_t compared = 0;          // bytes the tree produced and compared
  std::size_t differing = 0;         // of them, the bytes that differ
  std::size_t unplaced = 0;          // image bytes no file produced
  std::size_t placedTwice = 0;       // image bytes two files produced
  std::string error;                 // what stopped the run before any file was read

  // Whether the tree assembles to the image: every file assembled, every byte
  // produced exactly once, none differing.
  [[nodiscard]] bool identical() const;
};

// The text of one of the tree's files, by the path the manifest names, or
// nothing when it cannot be read.
using SourceReader = std::function<std::optional<std::string>(const std::string& file)>;

// Verifies the tree `manifest` describes against `rom`, reading each file through
// `read`. The manifest must name a map and the image it was written for; a
// manifest written for another image is refused, as the disassembler refuses it.
[[nodiscard]] VerifyReport verifyProject(const ManifestInput& manifest,
                                         std::span<const std::uint8_t> rom,
                                         const SourceReader& read);

// Reads `project.manifest` under `directory` and verifies the tree it describes,
// with every file read from the same directory.
[[nodiscard]] VerifyReport verifyTree(const std::filesystem::path& directory,
                                      std::span<const std::uint8_t> rom);

// The report as the command prints it: one line per file, one per mismatching
// run, then the totals and the verdict.
[[nodiscard]] std::string renderReport(const VerifyReport& report);

}  // namespace snaggletooth::disasm
