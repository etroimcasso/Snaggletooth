// snes_render — writes a tree's bank files from its program file.
//
//   snes_render <directory>
//
// Reads the directory's `program.snagir` and `project.manifest` and writes one
// 65816 source file per region the program file names, rendered from the
// program in the file: the instructions from their nodes, the data runs and the
// labels from the file's records, and the names, the routine comments and the
// `INCBIN` lines from the manifest's facts. It reads no image and runs nothing;
// its library cannot trace or lift, so the files it writes come from the
// program file and from nowhere else. Standard output names how many files
// were written.
//
// The exit status is 0 when every file was written, 1 when the tree does not
// read or a file cannot be written, 2 on a bad argument.

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

#include "rom/rom_render.h"

namespace {

[[noreturn]] void usage(const char* prog) {
  std::cerr << "usage: " << prog << " <directory>\n"
               "  writes the bank files from the directory's program.snagir and project.manifest\n";
  std::exit(2);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) usage(argv[0]);
  const std::filesystem::path directory(argv[1]);
  std::size_t rendered = 0;
  std::string error;
  if (!snaggletooth::disasm::renderTree(directory, rendered, error)) {
    std::cerr << error << "\n";
    return 1;
  }
  std::cout << rendered << " files rendered from " << (directory / "program.snagir").string() << "\n";
  return 0;
}
