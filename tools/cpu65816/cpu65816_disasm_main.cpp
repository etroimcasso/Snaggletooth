// cpu65816_disasm — disassembles 65816 code from a raw image.
//
//   cpu65816_disasm <image> [--base ADDR] [--entry ADDR]... [--offset N] [--length N]
//                           [--native [--a16] [--x16] | --native --widths-unknown]
//                           [--prior <image> [--prior-offset N]] [-o <out.txt>]
//
// The image is raw bytes: one bank of a cartridge carved out with --offset and
// --length, a block copied into work RAM, or a whole mapped image. --base is the
// 24-bit address the first byte occupies, so every address in the listing is the
// address the code actually runs at.
//
// Disassembly follows control flow from each --entry, so bytes execution cannot
// reach are printed as data rather than decoded as instructions. Without --entry
// the trace starts at --base.
//
// Every entry starts in the mode the flags give: emulation mode with both widths
// eight unless --native is given, and then eight-bit widths unless --a16 or
// --x16 widens one. That is the mode the reset vector starts in by default. An
// interrupt handler's widths are whatever the interrupted code had, which the
// image cannot say: --widths-unknown starts it that way, and the trace reads
// what it can and stops at the first operand whose width it would have to guess.
//
// --prior names the same region before the code ran. Every byte that differs is
// called out on its line, which is what makes a self-modifying program's patched
// slots visible instead of reading as though they had always held those bytes.
//
// Numbers are decimal, or hexadecimal with a 0x or $ prefix.

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "cpu65816_disasm.h"

namespace {

[[noreturn]] void usage(const char* prog) {
  std::cerr << "usage: " << prog
            << " <image> [--base ADDR] [--entry ADDR]... [--offset N] [--length N]\n"
               "                        [--native [--a16] [--x16] | --native --widths-unknown]\n"
               "                        [--prior <image> [--prior-offset N]] [-o <out>]\n";
  std::exit(2);
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

bool readFile(const std::string& path, std::vector<std::uint8_t>& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  out.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return true;
}

// Narrows a whole file to the requested window, reporting a window that does not
// fit rather than silently clamping it.
bool carve(std::vector<std::uint8_t>& bytes, std::uint32_t offset, std::uint32_t length,
           const char* what) {
  if (offset > bytes.size()) {
    std::cerr << what << " offset " << offset << " is past the end of the file ("
              << bytes.size() << " bytes)\n";
    return false;
  }
  bytes.erase(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(offset));
  if (length != 0) {
    if (length > bytes.size()) {
      std::cerr << what << " length " << length << " runs past the end of the file ("
                << bytes.size() << " bytes available)\n";
      return false;
    }
    bytes.resize(length);
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  std::string imagePath;
  std::string priorPath;
  std::string outPath;
  std::uint32_t base = 0;
  std::uint32_t offset = 0;
  std::uint32_t length = 0;
  std::uint32_t priorOffset = 0;
  bool native = false;
  bool a16 = false;
  bool x16 = false;
  bool widthsUnknown = false;
  std::vector<snaggletooth::disasm::Address> entries;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto next = [&](const char* name) -> std::string {
      if (i + 1 >= argc) {
        std::cerr << name << " needs a value\n";
        usage(argv[0]);
      }
      return argv[++i];
    };
    auto number = [&](const char* name) -> std::uint32_t {
      const std::string text = next(name);
      std::uint32_t value = 0;
      if (!parseNumber(text, value)) {
        std::cerr << name << ": " << text << " is not a number\n";
        usage(argv[0]);
      }
      return value;
    };

    if (arg == "--base") {
      base = number("--base");
    } else if (arg == "--entry") {
      entries.push_back(number("--entry"));
    } else if (arg == "--offset") {
      offset = number("--offset");
    } else if (arg == "--length") {
      length = number("--length");
    } else if (arg == "--native") {
      native = true;
    } else if (arg == "--a16") {
      a16 = true;
    } else if (arg == "--x16") {
      x16 = true;
    } else if (arg == "--widths-unknown") {
      widthsUnknown = true;
    } else if (arg == "--prior") {
      priorPath = next("--prior");
    } else if (arg == "--prior-offset") {
      priorOffset = number("--prior-offset");
    } else if (arg == "-o") {
      outPath = next("-o");
    } else if (imagePath.empty()) {
      imagePath = arg;
    } else {
      usage(argv[0]);
    }
  }
  if (imagePath.empty()) usage(argv[0]);
  if (base > 0xFFFFFF) {
    std::cerr << "--base must be a 24-bit address\n";
    return 1;
  }
  if ((a16 || x16 || widthsUnknown) && !native) {
    std::cerr << "--a16, --x16 and --widths-unknown need --native: emulation mode holds both "
                 "widths at eight\n";
    return 1;
  }
  if (widthsUnknown && (a16 || x16)) {
    std::cerr << "--widths-unknown names neither width; drop --a16 and --x16\n";
    return 1;
  }
  for (snaggletooth::disasm::Address entry : entries) {
    if (entry > 0xFFFFFF) {
      std::cerr << "--entry must be a 24-bit address\n";
      return 1;
    }
  }

  std::vector<std::uint8_t> image;
  if (!readFile(imagePath, image)) {
    std::cerr << "cannot open " << imagePath << "\n";
    return 1;
  }
  if (!carve(image, offset, length, "--offset/--length")) return 1;
  if (image.empty()) {
    std::cerr << "nothing to disassemble\n";
    return 1;
  }
  if (base + image.size() > 0x1000000u) {
    std::cerr << "the image does not fit in the address space at --base\n";
    return 1;
  }

  std::vector<std::uint8_t> prior;
  if (!priorPath.empty()) {
    if (!readFile(priorPath, prior)) {
      std::cerr << "cannot open " << priorPath << "\n";
      return 1;
    }
    if (!carve(prior, priorOffset, static_cast<std::uint32_t>(image.size()), "--prior"))
      return 1;
  }

  snaggletooth::disasm::Request request;
  request.image = image;
  request.base = base;
  request.entries = entries;
  request.priorImage = prior;
  snaggletooth::disasm::Cpu65816Mode mode = snaggletooth::disasm::Cpu65816Mode::reset();
  if (widthsUnknown) {
    mode = snaggletooth::disasm::Cpu65816Mode::nativeUnknown();
  } else if (native) {
    mode = snaggletooth::disasm::Cpu65816Mode::native(!a16, !x16);
  }
  request.context = snaggletooth::disasm::contextOf(mode);

  const std::string text = snaggletooth::disasm::render(
      snaggletooth::disasm::trace(snaggletooth::disasm::cpu65816Backend(), request));

  if (outPath.empty()) {
    std::cout << text;
    return 0;
  }
  std::ofstream out(outPath);
  if (!out) {
    std::cerr << "cannot write " << outPath << "\n";
    return 1;
  }
  out << text;
  return 0;
}
