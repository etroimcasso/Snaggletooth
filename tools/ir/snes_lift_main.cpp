// snes_lift — prints a tree's program, whole or one source file's regions.
//
//   snes_lift <directory> [--apu] [-o <file.snagir>] [--file <name>]
//
// Reads the directory's `program.snagir`, the program file `snes_disasm`
// wrote in the grammar `docs/snagir.md` gives — or, with `--apu`, its
// `apu.snagir`, the sound program's — and writes what it counted — the
// regions, the code lines (one per address a node stands at), the nodes (an
// address two paths read two ways is two), how many nodes select a width by
// the live flag, how many carry a hardware register's name, how many were
// lifted from patched bytes, and the effects — then the program file, written
// again from what was read. `-o` writes the file there instead, and standard
// output keeps the summary; `--file` limits both to the regions written to one
// source file. It reads no image and runs nothing.
//
// The exit status is 0 when the file was read, 2 on a bad argument, a file that
// cannot be opened, or one the reader refuses, which is named with its line.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>

#include "ir/ir_text.h"

namespace {

[[noreturn]] void usage(const char* prog) {
  std::cerr << "usage: " << prog << " <directory> [--apu] [-o <file.snagir>] [--file <name>]\n"
               "  prints the directory's program.snagir — or, with --apu, its apu.snagir — with a\n"
               "  summary, whole or one source file's regions\n";
  std::exit(2);
}

}  // namespace

int main(int argc, char** argv) {
  std::string directory;
  std::string outPath;
  std::string onlyFile;
  bool apu = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "-o" || arg == "--file") {
      if (i + 1 >= argc) {
        std::cerr << arg << " needs a value\n";
        usage(argv[0]);
      }
      (arg == "-o" ? outPath : onlyFile) = argv[++i];
    } else if (arg == "--apu") {
      apu = true;
    } else if (directory.empty()) {
      directory = arg;
    } else {
      usage(argv[0]);
    }
  }
  if (directory.empty()) usage(argv[0]);

  const std::filesystem::path programPath =
      std::filesystem::path(directory) / (apu ? "apu.snagir" : "program.snagir");
  std::string text;
  {
    std::ifstream in(programPath, std::ios::binary);
    if (!in) {
      std::cerr << "cannot open " << programPath.string() << "\n";
      return 2;
    }
    text.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  }
  std::string error;
  std::optional<snaggletooth::ir::Parsed> parsed = snaggletooth::ir::parseProgram(text, error);
  if (!parsed) {
    std::cerr << programPath.string() << ": " << error << "\n";
    return 2;
  }
  if (!onlyFile.empty()) {
    parsed = snaggletooth::ir::selectFile(*parsed, onlyFile);
    if (!parsed) {
      std::cerr << "the program file names no file " << onlyFile << "\n";
      return 2;
    }
  }

  std::ofstream file;
  if (!outPath.empty()) {
    file.open(outPath, std::ios::binary);
    if (!file) {
      std::cerr << "cannot write " << outPath << "\n";
      return 2;
    }
  }

  const snaggletooth::ir::ProgramCounts counts = snaggletooth::ir::countProgram(*parsed);
  std::cout << "regions " << counts.regions << "\ncode lines " << counts.codeLines
            << "\nnodes " << counts.nodes
            << "\nnodes selecting a width by the live flag " << counts.liveWidth
            << "\nnodes naming a hardware register " << counts.named
            << "\nnodes lifted from patched bytes " << counts.patched << "\neffects " << counts.effects
            << "\n";
  const std::string rendered = snaggletooth::ir::renderProgram(parsed->program, parsed->file);
  if (outPath.empty()) {
    std::cout << "\n" << rendered;
  } else {
    file << rendered;
    if (!file) {
      std::cerr << "cannot write " << outPath << "\n";
      return 2;
    }
  }
  return 0;
}
