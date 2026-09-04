// snes_disasm — disassembles a whole cartridge into a source tree.
//
//   snes_disasm <image> -o <directory> [--no-sound] [--boot-seconds N] [--no-run] [--run-seconds N]
//
// The tree is one source file per bank, the sound program the cartridge uploads
// at boot as a file of its own, and `project.manifest`, which says where every
// file's bytes land in the image, where the trace began, and where it stopped.
// The trace starts at the handlers the cartridge header names and follows
// control flow across banks; bytes execution cannot reach are written as data.
//
// When the directory already holds a manifest, its `entry` and `file` lines are
// read first: an entry a person added is traced with the vectors, and the file
// split it names is the one written. That is how the trace is carried past a
// jump table or a computed pointer — the manifest's `stop` lines say where.
//
// The sound program is found by booting the cartridge on the machine and reading
// what reached the audio unit before its program started. --no-sound skips the
// boot; --boot-seconds bounds it (fifteen seconds of the master clock by default).
//
// A copier header, the 512 bytes some dumps carry ahead of the image, is dropped
// when the file length says one is present.

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "rom/rom_disasm.h"

namespace {

constexpr std::uint64_t kMasterPerSecond = 21'477'272ull;

[[noreturn]] void usage(const char* prog) {
  std::cerr << "usage: " << prog << " <image> -o <directory> [--no-sound] [--boot-seconds N] [--no-run] [--run-seconds N]\n"
               "  the directory's project.manifest, when present, supplies entries and the file split\n";
  std::exit(2);
}

bool readFile(const std::filesystem::path& path, std::string& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  out.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  std::string imagePath;
  std::string outDir;
  bool sound = true;
  std::uint64_t bootSeconds = 15;
  bool run = true;
  std::uint64_t runSeconds = 60;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto next = [&](const char* name) -> std::string {
      if (i + 1 >= argc) {
        std::cerr << name << " needs a value\n";
        usage(argv[0]);
      }
      return argv[++i];
    };
    if (arg == "-o") {
      outDir = next("-o");
    } else if (arg == "--no-run") {
      run = false;
    } else if (arg == "--run-seconds") {
      try {
        runSeconds = std::stoull(next("--run-seconds"));
      } catch (const std::exception&) {
        std::cerr << "--run-seconds needs a number\n";
        usage(argv[0]);
      }
    } else if (arg == "--no-sound") {
      sound = false;
    } else if (arg == "--boot-seconds") {
      try {
        bootSeconds = std::stoull(next("--boot-seconds"));
      } catch (const std::exception&) {
        std::cerr << "--boot-seconds needs a number\n";
        usage(argv[0]);
      }
    } else if (imagePath.empty()) {
      imagePath = arg;
    } else {
      usage(argv[0]);
    }
  }
  if (imagePath.empty() || outDir.empty()) usage(argv[0]);

  std::string bytes;
  if (!readFile(imagePath, bytes)) {
    std::cerr << "cannot open " << imagePath << "\n";
    return 1;
  }
  std::vector<std::uint8_t> rom(bytes.begin(), bytes.end());
  if (rom.size() % 1024 == 512) rom.erase(rom.begin(), rom.begin() + 512);
  if (rom.empty()) {
    std::cerr << imagePath << " holds no cartridge image\n";
    return 1;
  }

  snaggletooth::disasm::CartridgeRequest request;
  request.rom = rom;
  request.captureSound = sound;
  request.observeRun = run;
  request.runMasterCycles = runSeconds * 21'477'272u;
  request.bootMasterCycles = bootSeconds * kMasterPerSecond;

  const std::filesystem::path directory(outDir);
  const std::filesystem::path manifestPath = directory / "project.manifest";
  std::string manifestText;
  if (readFile(manifestPath, manifestText)) {
    std::string error;
    const std::optional<snaggletooth::disasm::ManifestInput> input =
        snaggletooth::disasm::parseManifest(manifestText, error);
    if (!input) {
      std::cerr << manifestPath.string() << ": " << error << "\n";
      return 1;
    }
    const std::string mismatch = snaggletooth::disasm::manifestMismatch(*input, rom);
    if (!mismatch.empty()) {
      std::cerr << manifestPath.string() << ": " << mismatch << "\n";
      return 1;
    }
    request.entries = input->entries;
    request.regions = input->regions;
    request.reached = input->reached;
    std::cout << "read " << input->entries.size() << " entries and " << input->regions.size()
              << " files from " << manifestPath.string() << "\n";
  }

  const snaggletooth::disasm::CartridgeDisassembly disassembly =
      snaggletooth::disasm::disassembleCartridge(request);
  std::string error;
  if (!snaggletooth::disasm::writeProject(disassembly, directory, error)) {
    std::cerr << error << "\n";
    return 1;
  }

  std::size_t instructions = 0;
  for (const snaggletooth::disasm::RegionListing& region : disassembly.regions) {
    for (const snaggletooth::disasm::Line& line : region.listing.lines) {
      if (line.isCode) ++instructions;
    }
  }
  const snaggletooth::disasm::Placement placement = snaggletooth::disasm::placeBytes(disassembly);
  std::cout << disassembly.regions.size() << " files, " << instructions << " instructions, "
            << disassembly.entries.size() << " entries, " << disassembly.stops.size() << " stops\n";
  if (disassembly.sound) {
    std::size_t placed = 0;
    for (const snaggletooth::disasm::UploadBlock& block : disassembly.sound->capture.blocks) {
      if (block.romOffset) ++placed;
    }
    std::cout << "sound program: entry " << snaggletooth::disasm::formatAddress(disassembly.sound->capture.entry, 16)
              << ", " << disassembly.sound->capture.blocks.size() << " blocks, " << placed
              << " matched to the image\n";
  }
  for (const std::string& note : disassembly.notes) std::cout << "note: " << note << "\n";
  std::cout << (placement.image.size() - placement.unplaced) << " of " << placement.image.size()
            << " bytes placed";
  if (placement.placedTwice != 0) std::cout << ", " << placement.placedTwice << " placed twice";
  std::cout << " -> " << directory.string() << "\n";
  return 0;
}
