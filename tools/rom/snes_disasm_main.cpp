// snes_disasm — disassembles a whole cartridge into a source tree.
//
//   snes_disasm <image> -o <directory> [--no-sound] [--boot-seconds N]
//                                      [--no-run] [--run-seconds N]
//                                      [--input <script> | --input-dir <directory>]
//
// The tree is `program.snagir`, the whole program in the intermediate
// representation; `project.manifest`, which says where every file's bytes land
// in the image, where the trace began, and where it stopped; the files lifted
// out of the banks; one source file per bank, rendered from the program file
// read back; and the sound program the cartridge uploads at boot as a file of
// its own. The trace starts at the handlers the cartridge header names and
// follows control flow across banks; bytes execution cannot reach are written
// as data.
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
// The cartridge is also run, stepped, so the destinations its indirect jumps take
// and the landings its instructions do not name become entries. --no-run skips
// it; --run-seconds bounds it (sixty by default); --input replays a recorded run
// into the controller ports while it goes, so the run reaches what a player
// would; --input-dir names a directory of recorded runs, and the one named for
// the image (`rom/input_script.h`, `scriptPathFor`) is replayed when it is
// there and the ports stay empty when it is not.
//
// A copier's header ahead of the image is dropped, and the report says which
// copier wrote it and what it declares.

#include <cstddef>
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
#include "snaggletooth/snes/cartridge.h"

namespace {

constexpr std::uint64_t kMasterPerSecond = 21'477'272ull;

[[noreturn]] void usage(const char* prog) {
  std::cerr << "usage: " << prog
            << " <image> -o <directory> [--no-sound] [--boot-seconds N] [--no-run] [--run-seconds N]"
               " [--input <script> | --input-dir <directory>]\n"
               "  the directory's project.manifest, when present, supplies entries and the file split\n"
               "  --input replays a recorded run into the controller ports while the cartridge runs;\n"
               "  --input-dir replays the run named for the image under that directory, if there is one\n";
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
  std::string inputPath;
  std::string inputDir;

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
    } else if (arg == "--input") {
      inputPath = next("--input");
    } else if (arg == "--input-dir") {
      inputDir = next("--input-dir");
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
  if (!inputPath.empty() && !inputDir.empty()) {
    std::cerr << "--input and --input-dir both name a recorded run; give one\n";
    return 2;
  }
  if ((!inputPath.empty() || !inputDir.empty()) && !run) {
    std::cerr << "--input replays a run; it has nothing to do under --no-run\n";
    return 2;
  }
  if (!inputDir.empty()) {
    const std::filesystem::path script = snaggletooth::disasm::scriptPathFor(inputDir, imagePath);
    if (std::filesystem::is_regular_file(script)) {
      inputPath = script.string();
      std::cout << "replaying " << script.string() << "\n";
    } else {
      std::cout << "no recorded run at " << script.string() << "; the ports stay empty\n";
    }
  }

  snaggletooth::disasm::InputScript script;
  if (!inputPath.empty()) {
    std::string text;
    if (!readFile(inputPath, text)) {
      std::cerr << "cannot open " << inputPath << "\n";
      return 1;
    }
    std::string error;
    const std::optional<snaggletooth::disasm::InputScript> parsed =
        snaggletooth::disasm::parseInputScript(text, error);
    if (!parsed) {
      std::cerr << inputPath << ": " << error << "\n";
      return 1;
    }
    script = *parsed;
  }

  std::string bytes;
  if (!readFile(imagePath, bytes)) {
    std::cerr << "cannot open " << imagePath << "\n";
    return 1;
  }
  std::vector<std::uint8_t> rom(bytes.begin(), bytes.end());
  if (const std::optional<snaggletooth::CopierHeader> copier = snaggletooth::readCopierHeader(rom)) {
    rom.erase(rom.begin(), rom.begin() + static_cast<std::ptrdiff_t>(snaggletooth::kCopierHeaderBytes));
    std::cout << "dropped " << snaggletooth::describeCopierHeader(*copier, rom.size()) << "\n";
  }
  if (rom.empty()) {
    std::cerr << imagePath << " holds no cartridge image\n";
    return 1;
  }

  snaggletooth::disasm::CartridgeRequest request;
  request.rom = rom;
  request.captureSound = sound;
  request.observeRun = run;
  request.runMasterCycles = runSeconds * kMasterPerSecond;
  request.input = script;
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
    request.ran = input->ran;
    request.moved = input->moved;
    request.assets = input->assets;
    request.derived = input->derived;
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
  std::cout << "program.snagir: " << disassembly.program.nodes.size() << " nodes\n";
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
