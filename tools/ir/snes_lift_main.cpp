// snes_lift — lifts a cartridge's source tree into the intermediate
// representation and writes it out for reading.
//
//   snes_lift <directory> <image> [-o <file.snagir>] [--file <name>]
//
// Reads the directory's `project.manifest`, traces the image as the manifest
// directs — its entries, its file split, the targets earlier runs saw — and
// lifts every 65816 region into nodes. The output opens with what was lifted:
// the regions, the code lines, the nodes (an address two paths read two ways is
// two), how many nodes select a width by the live flag, how many carry a
// hardware register's name, and the effects. Then every node, region by region,
// in address order: a header line with its address, mnemonic and operand, its
// mode and its measured costs, and under it each effect on its own line.
// `--file` limits the output to one region's file; `-o` writes it to a file
// rather than standard output. The text form carries the `.snagir` extension.
//
// The exit status is 0 when the tree lifted, 2 on a bad argument or an
// unreadable input.
//
// A copier header, the 512 bytes some dumps carry ahead of the image, is dropped
// when the file length says one is present.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "ir/cpu65816_lift.h"
#include "ir/ir.h"
#include "ir/ir_text.h"
#include "rom/rom_disasm.h"

namespace {

[[noreturn]] void usage(const char* prog) {
  std::cerr << "usage: " << prog << " <directory> <image> [-o <file.snagir>] [--file <name>]\n"
               "  lifts the tree's 65816 code into the intermediate representation and writes it out\n";
  std::exit(2);
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
  if (rom.size() % 1024 == 512) rom.erase(rom.begin(), rom.begin() + 512);
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
  request.captureSound = false;
  request.observeRun = false;
  const snaggletooth::disasm::CartridgeDisassembly d =
      snaggletooth::disasm::disassembleCartridge(request);

  std::ofstream file;
  if (!outPath.empty()) {
    file.open(outPath);
    if (!file) {
      std::cerr << "cannot write " << outPath << "\n";
      return 2;
    }
  }
  std::ostream& out = outPath.empty() ? std::cout : file;

  // Every region lifted first, so the summary can lead.
  struct Lifted {
    const snaggletooth::disasm::RegionListing* region;
    snaggletooth::ir::Program program;
  };
  std::vector<Lifted> lifted;
  std::size_t codeLines = 0;
  std::size_t nodes = 0;
  std::size_t liveWidth = 0;
  std::size_t named = 0;
  std::size_t patched = 0;
  std::size_t effects = 0;
  bool matched = onlyFile.empty();
  for (const snaggletooth::disasm::RegionListing& region : d.regions) {
    if (!onlyFile.empty() && region.region.file != onlyFile) continue;
    matched = true;
    std::vector<std::uint8_t> image;
    for (const snaggletooth::disasm::Line& line : region.listing.lines) {
      const std::vector<std::uint8_t>& bytes = line.isCode ? line.instruction.bytes : line.data;
      image.insert(image.end(), bytes.begin(), bytes.end());
      if (line.isCode) ++codeLines;
    }
    Lifted l{&region, snaggletooth::ir::lift65816(region.listing, image, region.region.first)};
    for (const snaggletooth::ir::Node& node : l.program.nodes) {
      ++nodes;
      if (!node.mode.accumulatorKnown || !node.mode.indexKnown) ++liveWidth;
      if (!node.registerName.empty()) ++named;
      if (node.patched) ++patched;
      effects += node.effects.size();
    }
    lifted.push_back(std::move(l));
  }
  if (!matched) {
    std::cerr << "the manifest names no file " << onlyFile << "\n";
    return 2;
  }

  out << "regions " << lifted.size() << "\ncode lines " << codeLines << "\nnodes " << nodes
      << "\nnodes selecting a width by the live flag " << liveWidth
      << "\nnodes naming a hardware register " << named
      << "\nnodes lifted from patched bytes " << patched << "\neffects " << effects << "\n";
  for (const Lifted& l : lifted) {
    out << "\n== " << l.region->region.file << "\n";
    for (const snaggletooth::ir::Node& node : l.program.nodes) out << "\n" << snaggletooth::ir::renderNode(node);
  }
  return 0;
}
