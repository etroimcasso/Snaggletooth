// snes_verify — verifies a source tree against the cartridge image it came from.
//
//   snes_verify <directory> <image> [-o <rebuilt>]
//
// Reads the directory's `project.manifest`, assembles every file it names —
// the bank files under the 65816 dialect, the sound program under the SPC700
// dialect — places each range and each placed block at the image offset the
// manifest gives, and compares the whole with the image byte for byte. The
// report names every file, every run that differs with its first differing
// byte, the bytes no file produced, and the verdict. `-o` writes the image the
// tree assembled to, whatever the verdict.
//
// The exit status is 0 only when the tree assembles to the image: every file
// assembled, every byte produced exactly once, none differing. A manifest
// written for another image is refused.
//
// A copier header, the 512 bytes some dumps carry ahead of the image, is dropped
// when the file length says one is present.

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "rom/rom_verify.h"

namespace {

[[noreturn]] void usage(const char* prog) {
  std::cerr << "usage: " << prog << " <directory> <image> [-o <rebuilt>]\n"
               "  assembles the tree under the directory and compares it with the image\n";
  std::exit(2);
}

}  // namespace

int main(int argc, char** argv) {
  std::string directory;
  std::string imagePath;
  std::string outPath;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "-o") {
      if (i + 1 >= argc) {
        std::cerr << "-o needs a value\n";
        usage(argv[0]);
      }
      outPath = argv[++i];
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
      return 1;
    }
    rom.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  }
  if (rom.size() % 1024 == 512) rom.erase(rom.begin(), rom.begin() + 512);
  if (rom.empty()) {
    std::cerr << imagePath << " holds no cartridge image\n";
    return 1;
  }

  const snaggletooth::disasm::VerifyReport report =
      snaggletooth::disasm::verifyTree(std::filesystem::path(directory), rom);
  std::cout << snaggletooth::disasm::renderReport(report);

  if (!outPath.empty() && report.error.empty()) {
    std::ofstream out(outPath, std::ios::binary);
    if (!out) {
      std::cerr << "cannot write " << outPath << "\n";
      return 1;
    }
    out.write(reinterpret_cast<const char*>(report.image.data()),
              static_cast<std::streamsize>(report.image.size()));
    if (!out) {
      std::cerr << "cannot write " << outPath << "\n";
      return 1;
    }
    std::cout << "wrote " << report.image.size() << " bytes to " << outPath << "\n";
  }
  return report.identical() ? 0 : 1;
}
