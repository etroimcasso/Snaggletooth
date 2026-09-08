// snes_lift — lifts a cartridge's source tree into the intermediate
// representation and writes it as a program file.
//
//   snes_lift <directory> <image> [-o <file.snagir>] [--file <name>]
//
// Reads the directory's `project.manifest`, traces the image as the manifest
// directs — its entries, its file split, the targets earlier runs saw — and
// lifts every 65816 region into nodes. Standard output opens with what was
// lifted: the regions, the code lines, the nodes (an address two paths read two
// ways is two), how many nodes select a width by the live flag, how many carry
// a hardware register's name, and the effects. Then the program file, in the
// grammar `docs/snagir.md` gives: the image, each region with its labels, its
// data runs and its nodes in address order, and the interrupt sequences. `-o`
// writes the file there instead, and standard output keeps the summary;
// `--file` limits both to one region's file.
//
// The exit status is 0 when the tree lifted, 2 on a bad argument or an
// unreadable input.
//
// A copier's header ahead of the image is dropped, and a line on standard error
// says which copier wrote it and what it declares.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include "ir/cpu65816_lift.h"
#include "ir/ir.h"
#include "ir/ir_text.h"
#include "rom/rom_disasm.h"
#include "snaggletooth/snes/cartridge.h"

namespace {

[[noreturn]] void usage(const char* prog) {
  std::cerr << "usage: " << prog << " <directory> <image> [-o <file.snagir>] [--file <name>]\n"
               "  lifts the tree's 65816 code into the intermediate representation and writes the program file\n";
  std::exit(2);
}

std::string mapName(snaggletooth::CartridgeMap map) {
  switch (map) {
    case snaggletooth::CartridgeMap::LoRom: return "LoROM";
    case snaggletooth::CartridgeMap::HiRom: return "HiROM";
    case snaggletooth::CartridgeMap::ExHiRom: return "ExHiROM";
  }
  return "LoROM";
}

}  // namespace

int main(int argc, char** argv) {
  std::string directory;
  std::string imagePath;
  std::string outPath;
  std::string onlyFile;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "-o" || arg == "--file") {
      if (i + 1 >= argc) {
        std::cerr << arg << " needs a value\n";
        usage(argv[0]);
      }
      (arg == "-o" ? outPath : onlyFile) = argv[++i];
    } else if (directory.empty()) {
      directory = arg;
    } else if (imagePath.empty()) {
      imagePath = arg;
    } else {
      usage(argv[0]);
    }
  }
  if (directory.empty() || imagePath.empty()) usage(argv[0]);

  std::vector<std::uint8_t> rom;
  {
    std::ifstream in(imagePath, std::ios::binary);
    if (!in) {
      std::cerr << "cannot open " << imagePath << "\n";
      return 2;
    }
    rom.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  }
  if (const std::optional<snaggletooth::CopierHeader> copier = snaggletooth::readCopierHeader(rom)) {
    rom.erase(rom.begin(), rom.begin() + static_cast<std::ptrdiff_t>(snaggletooth::kCopierHeaderBytes));
    std::cerr << "dropped " << snaggletooth::describeCopierHeader(*copier, rom.size()) << "\n";
  }
  if (rom.empty()) {
    std::cerr << imagePath << " holds no cartridge image\n";
    return 2;
  }

  std::string manifestText;
  {
    std::ifstream in(std::filesystem::path(directory) / "project.manifest");
    if (!in) {
      std::cerr << "cannot open " << directory << "/project.manifest\n";
      return 2;
    }
    manifestText.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  }
  std::string error;
  const auto manifest = snaggletooth::disasm::parseManifest(manifestText, error);
  if (!manifest) {
    std::cerr << "project.manifest: " << error << "\n";
    return 2;
  }
  const std::string mismatch = snaggletooth::disasm::manifestMismatch(*manifest, rom);
  if (!mismatch.empty()) {
    std::cerr << mismatch << "\n";
    return 2;
  }

  snaggletooth::disasm::CartridgeRequest request;
  request.rom = rom;
  request.entries = manifest->entries;
  request.regions = manifest->regions;
  request.reached = manifest->reached;
  request.ran = manifest->ran;
  request.captureSound = false;
  request.observeRun = false;
  const snaggletooth::disasm::CartridgeDisassembly d =
      snaggletooth::disasm::disassembleCartridge(request);

  std::ofstream file;
  if (!outPath.empty()) {
    file.open(outPath, std::ios::binary);
    if (!file) {
      std::cerr << "cannot write " << outPath << "\n";
      return 2;
    }
  }

  // Every region lifted with its image — both readings of a two-way address —
  // into one program, the nodes in address order, and the file's own records
  // beside it: the regions with their labels, their data runs and their
  // warnings.
  snaggletooth::ir::Program program;
  snaggletooth::ir::ProgramFile programFile;
  programFile.imageBytes = d.imageBytes;
  programFile.map = mapName(d.header.map);
  std::size_t codeLines = 0;
  std::size_t liveWidth = 0;
  std::size_t named = 0;
  std::size_t patched = 0;
  std::size_t effects = 0;
  bool matched = onlyFile.empty();
  for (const snaggletooth::disasm::RegionListing& region : d.regions) {
    if (!onlyFile.empty() && region.region.file != onlyFile) continue;
    matched = true;
    std::vector<std::uint8_t> image;
    snaggletooth::ir::ProgramRegion out;
    out.file = region.region.file;
    out.first = region.region.first;
    out.last = region.region.last;
    out.warnings = region.listing.warnings;
    for (const auto& [address, name] : region.listing.labels) out.labels.push_back({address, name});
    for (const snaggletooth::disasm::Line& line : region.listing.lines) {
      const std::vector<std::uint8_t>& bytes = line.isCode ? line.instruction.bytes : line.data;
      image.insert(image.end(), bytes.begin(), bytes.end());
      if (line.isCode) {
        ++codeLines;
      } else if (!line.data.empty()) {
        out.data.push_back({line.address, line.data});
      }
    }
    programFile.regions.push_back(std::move(out));
    snaggletooth::ir::Program one =
        snaggletooth::ir::lift65816(region.listing, image, region.region.first);
    for (const snaggletooth::ir::Node& node : one.nodes) {
      if (!node.mode.accumulatorKnown || !node.mode.indexKnown) ++liveWidth;
      if (!node.registerName.empty()) ++named;
      if (node.patched) ++patched;
      effects += node.effects.size();
    }
    program.nodes.insert(program.nodes.end(), one.nodes.begin(), one.nodes.end());
    program.nmi = one.nmi;
    program.irq = one.irq;
  }
  if (!matched) {
    std::cerr << "the manifest names no file " << onlyFile << "\n";
    return 2;
  }
  std::stable_sort(program.nodes.begin(), program.nodes.end(),
                   [](const snaggletooth::ir::Node& a, const snaggletooth::ir::Node& b) {
                     return a.instruction.address < b.instruction.address;
                   });

  std::cout << "regions " << programFile.regions.size() << "\ncode lines " << codeLines
            << "\nnodes " << program.nodes.size()
            << "\nnodes selecting a width by the live flag " << liveWidth
            << "\nnodes naming a hardware register " << named
            << "\nnodes lifted from patched bytes " << patched << "\neffects " << effects << "\n";
  const std::string text = snaggletooth::ir::renderProgram(program, programFile);
  if (outPath.empty()) {
    std::cout << "\n" << text;
  } else {
    file << text;
    if (!file) {
      std::cerr << "cannot write " << outPath << "\n";
      return 2;
    }
  }
  return 0;
}
