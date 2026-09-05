#include "assembler/asm_main.h"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

namespace snaggletooth::assembler {
namespace {

void usage(const char* prog, std::string_view chip) {
  std::cerr << "usage: " << prog
            << " <source.asm> -o <out.bin> [--base ADDR] [--size N] [--fill BYTE]\n"
               "assembles " << chip << " source into a flat image\n";
}

// Parses a number written in decimal, or in hexadecimal behind 0x or $.
bool parseNumber(const std::string& text, std::uint32_t& out) {
  try {
    if (text.size() > 1 && text[0] == '$') {
      out = static_cast<std::uint32_t>(std::stoul(text.substr(1), nullptr, 16));
    } else if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
      out = static_cast<std::uint32_t>(std::stoul(text.substr(2), nullptr, 16));
    } else {
      out = static_cast<std::uint32_t>(std::stoul(text, nullptr, 10));
    }
  } catch (const std::exception&) {
    return false;
  }
  return true;
}

}  // namespace

int assemblerMain(int argc, char** argv, Dialect& dialect) {
  std::string sourcePath;
  std::string outPath;
  std::optional<std::uint32_t> base;
  std::optional<std::uint32_t> size;
  std::uint32_t fill = 0;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto next = [&](const char* name) -> std::optional<std::string> {
      if (i + 1 >= argc) {
        std::cerr << name << " needs a value\n";
        return std::nullopt;
      }
      return std::string(argv[++i]);
    };
    auto number = [&](const char* name) -> std::optional<std::uint32_t> {
      const std::optional<std::string> text = next(name);
      if (!text) return std::nullopt;
      std::uint32_t value = 0;
      if (!parseNumber(*text, value)) {
        std::cerr << name << ": " << *text << " is not a number\n";
        return std::nullopt;
      }
      return value;
    };

    if (arg == "--base") {
      base = number("--base");
      if (!base) return 2;
    } else if (arg == "--size") {
      size = number("--size");
      if (!size) return 2;
    } else if (arg == "--fill") {
      const std::optional<std::uint32_t> value = number("--fill");
      if (!value) return 2;
      if (*value > 0xFFu) {
        std::cerr << "--fill is one byte\n";
        return 2;
      }
      fill = *value;
    } else if (arg == "-o") {
      const std::optional<std::string> path = next("-o");
      if (!path) return 2;
      outPath = *path;
    } else if (sourcePath.empty() && !arg.empty() && arg.front() != '-') {
      sourcePath = arg;
    } else {
      usage(argv[0], dialect.name());
      return 2;
    }
  }
  if (sourcePath.empty() || outPath.empty()) {
    usage(argv[0], dialect.name());
    return 2;
  }
  if (base && !fits(*base, dialect.addressBits())) {
    std::cerr << "--base must be a " << dialect.addressBits() << "-bit address\n";
    return 2;
  }

  std::ifstream in(sourcePath, std::ios::binary);
  if (!in) {
    std::cerr << "cannot open " << sourcePath << "\n";
    return 1;
  }
  const std::string source((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

  // An INCBIN's path arrives joined with the source's own directory, so it is
  // read exactly as the source was.
  const Reader reader = [](const std::string& path) -> std::optional<std::string> {
    std::ifstream file(path, std::ios::binary);
    if (!file) return std::nullopt;
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  };
  const Assembly assembly = assemble(dialect, source, sourcePath, reader);
  if (!assembly.ok()) {
    for (const Diagnostic& error : assembly.errors) {
      std::cerr << error.file << ":" << error.line << ": " << error.message << "\n";
    }
    std::cerr << assembly.errors.size() << (assembly.errors.size() == 1 ? " error" : " errors")
              << "; nothing written\n";
    return 1;
  }
  if (assembly.ranges.empty()) {
    std::cerr << "the source emits no bytes; nothing written\n";
    return 1;
  }

  const Address first = assembly.ranges.front().start;
  const Range& last = assembly.ranges.back();
  const Address end = last.start + static_cast<Address>(last.bytes.size());
  const Address imageBase = base.value_or(first);
  const std::size_t imageSize = size ? static_cast<std::size_t>(*size)
                                     : static_cast<std::size_t>(end - imageBase);
  const std::optional<std::vector<std::uint8_t>> bytes =
      image(assembly, imageBase, imageSize, static_cast<std::uint8_t>(fill));
  if (!bytes) {
    std::cerr << "the source places bytes outside the image window " << hex(imageBase, 4)
              << "-" << hex(imageBase + static_cast<Address>(imageSize) - 1u, 4)
              << "; nothing written\n";
    return 1;
  }

  std::ofstream out(outPath, std::ios::binary);
  if (!out) {
    std::cerr << "cannot write " << outPath << "\n";
    return 1;
  }
  out.write(reinterpret_cast<const char*>(bytes->data()), static_cast<std::streamsize>(bytes->size()));
  if (!out) {
    std::cerr << "cannot write " << outPath << "\n";
    return 1;
  }

  const unsigned digits = dialect.addressBits() / 4;
  for (const Range& range : assembly.ranges) {
    std::cout << "  " << hex(range.start, digits) << "-"
              << hex(range.start + static_cast<Address>(range.bytes.size()) - 1u, digits) << "  "
              << range.bytes.size() << " bytes\n";
  }
  std::cout << "wrote " << bytes->size() << " bytes to " << outPath << "\n";
  return 0;
}

}  // namespace snaggletooth::assembler
