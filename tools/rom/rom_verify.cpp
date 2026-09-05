#include "rom/rom_verify.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>

#include "cpu65816/cpu65816_asm.h"
#include "spc700/spc700_asm.h"

namespace snaggletooth::disasm {
namespace {

std::string hex(std::uint32_t value, int digits) {
  char buffer[12];
  std::snprintf(buffer, sizeof buffer, "%0*X", digits, static_cast<unsigned>(value));
  return buffer;
}

std::string offsetText(std::size_t offset) {
  return "$" + hex(static_cast<std::uint32_t>(offset), 6);
}

// The image being rebuilt from the tree, and what each byte's placement found.
struct Rebuild {
  std::span<const std::uint8_t> rom;
  std::vector<std::uint8_t> image;
  std::vector<std::uint8_t> count;  // how many files produced each byte, capped at two

  explicit Rebuild(std::span<const std::uint8_t> original)
      : rom(original), image(original.size(), 0u), count(original.size(), 0u) {}

  // Places one byte; whether it matches the image.
  bool place(std::size_t offset, std::uint8_t byte) {
    image[offset] = byte;
    if (count[offset] < 2) ++count[offset];
    return rom[offset] == byte;
  }
};

// Places a run of bytes whose first lands at `offset`, consecutive from there,
// and records it in `file` and, when a byte differs, in `mismatches`.
void placeRun(Rebuild& rebuild, const std::string& fileName, Address address, unsigned addressBits,
              std::size_t offset, std::span<const std::uint8_t> bytes, VerifiedFile& file,
              std::vector<VerifyMismatch>& mismatches) {
  std::optional<std::size_t> firstDifference;
  std::size_t differing = 0;
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    if (!rebuild.place(offset + i, bytes[i])) {
      ++differing;
      if (!firstDifference) firstDifference = offset + i;
    }
  }
  ++file.runs;
  file.bytes += bytes.size();
  file.differing += differing;
  if (firstDifference) {
    mismatches.push_back(VerifyMismatch{.file = fileName,
                                        .address = address,
                                        .addressBits = addressBits,
                                        .romOffset = offset,
                                        .length = bytes.size(),
                                        .firstDifference = *firstDifference});
  }
}

// A bank file: every range it emits, placed at the image offset its address
// reads from under the map. A range must read consecutive image bytes, which is
// what one `ORG` over a bank's window gives; a range whose bytes do not is
// reported and not placed.
void verifyBankFile(Rebuild& rebuild, CartridgeMap map, const SourceRegion& region,
                    const SourceReader& read, VerifyReport& report) {
  VerifiedFile file{.file = region.file,
                    .chip = "65816",
                    .errors = {},
                    .problem = {},
                    .runs = 0,
                    .bytes = 0,
                    .differing = 0};
  const std::optional<std::string> source = read(region.file);
  if (!source) {
    file.problem = "cannot read " + region.file;
    report.files.push_back(std::move(file));
    return;
  }
  const assembler::Assembly assembly = assembler::assembleCpu65816(*source, region.file, read);
  file.errors = assembly.errors;
  if (!assembly.ok()) {
    report.files.push_back(std::move(file));
    return;
  }
  for (const assembler::Range& range : assembly.ranges) {
    const std::optional<std::size_t> start = romOffset(map, range.start, rebuild.rom.size());
    bool consecutive = start.has_value();
    for (std::size_t i = 1; consecutive && i < range.bytes.size(); ++i) {
      const std::optional<std::size_t> offset =
          romOffset(map, range.start + static_cast<Address>(i), rebuild.rom.size());
      consecutive = offset && *offset == *start + i;
    }
    if (!consecutive) {
      const Address last = range.start + static_cast<Address>(range.bytes.size()) - 1u;
      file.problem = formatAddress(range.start, 24) + "-" + formatAddress(last, 24) +
                     " does not read consecutive image bytes; not placed";
      continue;
    }
    placeRun(rebuild, region.file, range.start, 24, *start, range.bytes, file, report.mismatches);
  }
  report.files.push_back(std::move(file));
}

// The sound program's file: assembled into the audio CPU's 64 KB, then each
// block the manifest placed in the image compared there. An unplaced block is
// the bank's, and is not compared here. A placed block the file does not cover
// whole is reported rather than compared against the fill.
void verifySoundFile(Rebuild& rebuild, const ManifestSound& sound, const SourceReader& read,
                     VerifyReport& report) {
  VerifiedFile file{.file = sound.file,
                    .chip = "SPC700",
                    .errors = {},
                    .problem = {},
                    .runs = 0,
                    .bytes = 0,
                    .differing = 0};
  const std::optional<std::string> source = read(sound.file);
  if (!source) {
    file.problem = "cannot read " + sound.file;
    report.files.push_back(std::move(file));
    return;
  }
  const assembler::Assembly assembly = assembler::assembleSpc700(*source, sound.file, read);
  file.errors = assembly.errors;
  if (!assembly.ok()) {
    report.files.push_back(std::move(file));
    return;
  }
  constexpr std::size_t kAudioMemory = 65536u;
  const std::optional<std::vector<std::uint8_t>> memory = assembler::image(assembly, 0, kAudioMemory);
  auto covered = [&](std::size_t first, std::size_t length) {
    for (const assembler::Range& range : assembly.ranges) {
      if (range.start <= first && first + length <= range.start + range.bytes.size()) return true;
    }
    return false;
  };
  for (const ManifestBlock& block : sound.blocks) {
    if (!block.romOffset) continue;
    const std::size_t first = block.apuAddress;
    if (!memory || first + block.length > kAudioMemory ||
        *block.romOffset + block.length > rebuild.rom.size()) {
      file.problem = "block " + formatAddress(block.apuAddress, 16) + " lies outside the image";
      continue;
    }
    if (!covered(first, block.length)) {
      file.problem = "block " + formatAddress(block.apuAddress, 16) + " (" +
                     std::to_string(block.length) + " bytes) is not all emitted by the file";
      continue;
    }
    const std::span<const std::uint8_t> bytes(memory->data() + first, block.length);
    placeRun(rebuild, sound.file, block.apuAddress, 16, *block.romOffset, bytes, file,
             report.mismatches);
  }
  report.files.push_back(std::move(file));
}

}  // namespace

bool VerifyReport::identical() const {
  if (!error.empty() || differing != 0 || unplaced != 0 || placedTwice != 0) return false;
  return std::all_of(files.begin(), files.end(), [](const VerifiedFile& file) {
    return file.errors.empty() && file.problem.empty();
  });
}

VerifyReport verifyProject(const ManifestInput& manifest, std::span<const std::uint8_t> rom,
                           const SourceReader& read) {
  VerifyReport report;
  if (!manifest.map) {
    report.error = "the manifest names no map";
    return report;
  }
  report.error = manifestMismatch(manifest, rom);
  if (!report.error.empty()) return report;

  Rebuild rebuild(rom);
  for (const SourceRegion& region : manifest.regions) {
    verifyBankFile(rebuild, *manifest.map, region, read, report);
  }
  if (manifest.sound) verifySoundFile(rebuild, *manifest.sound, read, report);

  for (const std::uint8_t c : rebuild.count) {
    if (c == 0) ++report.unplaced;
    if (c > 1) ++report.placedTwice;
  }
  for (const VerifiedFile& file : report.files) {
    report.compared += file.bytes;
    report.differing += file.differing;
  }
  report.image = std::move(rebuild.image);
  return report;
}

VerifyReport verifyTree(const std::filesystem::path& directory, std::span<const std::uint8_t> rom) {
  auto read = [&](const std::string& file) -> std::optional<std::string> {
    std::ifstream in(directory / file, std::ios::binary);
    if (!in) return std::nullopt;
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  };
  const std::optional<std::string> text = read("project.manifest");
  VerifyReport report;
  if (!text) {
    report.error = "cannot read " + (directory / "project.manifest").string();
    return report;
  }
  std::string error;
  const std::optional<ManifestInput> manifest = parseManifest(*text, error);
  if (!manifest) {
    report.error = (directory / "project.manifest").string() + ": " + error;
    return report;
  }
  return verifyProject(*manifest, rom, read);
}

std::string renderReport(const VerifyReport& report) {
  if (!report.error.empty()) return report.error + "\n";
  std::string out;
  for (const VerifiedFile& file : report.files) {
    out += file.file + ": ";
    if (!file.errors.empty()) {
      out += std::to_string(file.errors.size()) + (file.errors.size() == 1 ? " error" : " errors") +
             ", not assembled\n";
      for (const assembler::Diagnostic& diagnostic : file.errors) {
        out += "  " + diagnostic.file + ":" + std::to_string(diagnostic.line) + ": " +
               diagnostic.message + "\n";
      }
      continue;
    }
    if (!file.problem.empty() && file.runs == 0) {
      out += file.problem + "\n";
      continue;
    }
    out += std::to_string(file.runs) + (file.chip == "SPC700" ? " block" : " range") +
           (file.runs == 1 ? "" : "s") + ", " + std::to_string(file.bytes) + " bytes";
    out += file.differing == 0 ? ", identical" : ", " + std::to_string(file.differing) + " differ";
    if (!file.problem.empty()) out += "; " + file.problem;
    out += "\n";
  }
  for (const VerifyMismatch& mismatch : report.mismatches) {
    const Address last = mismatch.address + static_cast<Address>(mismatch.length) - 1u;
    out += "  " + mismatch.file + " " + formatAddress(mismatch.address, mismatch.addressBits) +
           "-" + formatAddress(last, mismatch.addressBits) + " at " + offsetText(mismatch.romOffset) +
           ": first difference at " + offsetText(mismatch.firstDifference) + "\n";
  }
  out += std::to_string(report.compared) + " of " + std::to_string(report.image.size()) +
         " bytes compared, " + std::to_string(report.differing) + " differ";
  if (report.unplaced != 0) out += ", " + std::to_string(report.unplaced) + " produced by no file";
  if (report.placedTwice != 0) out += ", " + std::to_string(report.placedTwice) + " produced twice";
  out += "\n";
  out += report.identical() ? "the tree assembles to the image\n"
                            : "the tree does not assemble to the image\n";
  return out;
}

}  // namespace snaggletooth::disasm
