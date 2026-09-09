// snes_differential — replays a cartridge's recorded run beside the interpreter
// and reports where the lifted program and the machine disagree.
//
//   snes_differential <directory> <image> -o <report> [--seconds N]
//                                                     [--input <script> | --input-dir <directory>]
//                                                     [--quiet]
//
// Reads the directory's `program.snagir` — the program `snes_disasm` wrote, in
// the grammar `docs/snagir.md` gives — and runs the machine on the image for
// `--seconds` of the master clock (sixty by default) with the interpreter
// beside it, held to every access, every register and every cycle. The image
// must be the one the file is a program of: its size is checked against the
// file's own `image` line before anything runs. `--input`
// replays a recorded run into the controller ports, exactly as `snes_disasm
// --input` does, so the same run is checked that produced the tree;
// `--input-dir` finds the run named for the image under that directory, as
// `snes_disasm --input-dir` does, and leaves the ports empty when there is none.
//
// The report is written under `-o`: `summary.txt` (what was checked and how
// much), `divergences.txt` (each disagreement with its step, node, effect and
// the two values), `forms.txt` (how many times each instruction form ran under
// each mode), `constructs.txt` (how many times each named construct was
// exercised, zero where the run never reached one), and `unlifted.txt` (the
// addresses the run executed that the tree has no instruction for, as the tree
// places them). One line on standard output sums it up.
//
// The exit status is 0 when the run diverged nowhere, 1 when it did, 2 on a bad
// argument, an unreadable input, or a program file the reader refuses, which is
// named with its line.
//
// A copier's header ahead of the image is dropped, and the report says which
// copier wrote it and what it declares.
//
// The replay's advance is written to standard error as it goes, the seconds of
// the master clock spent against `--seconds`, refreshed in place on a terminal
// and one line per ten seconds otherwise. --quiet turns it off; the report and
// the line on standard output are the same either way.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include "ir/ir.h"
#include "ir/ir_differential.h"
#include "ir/ir_text.h"
#include "rom/input_script.h"
#include "rom/progress.h"
#include "snaggletooth/snes/cartridge.h"

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace {

[[noreturn]] void usage(const char* prog) {
  std::cerr << "usage: " << prog
            << " <directory> <image> -o <report> [--seconds N] [--input <script> | --input-dir <directory>]"
               " [--quiet]\n"
               "  replays the tree's recorded run on the machine beside the interpreter;\n"
               "  --quiet keeps the progress off standard error\n";
  std::exit(2);
}

// Whether standard error is a terminal, where a progress line is refreshed in place.
bool errorIsTerminal() {
#ifdef _WIN32
  return _isatty(_fileno(stderr)) != 0;
#else
  return isatty(2) != 0;
#endif
}

std::string readText(const std::string& path, bool& ok) {
  std::ifstream in(path);
  ok = static_cast<bool>(in);
  return std::string((std::istreambuf_iterator<char>(in)), {});
}

std::string hex(std::uint32_t v, int width) {
  char b[16];
  std::snprintf(b, sizeof b, "$%0*X", width, v);
  return b;
}

}  // namespace

int main(int argc, char** argv) {
  std::string directory;
  std::string imagePath;
  std::string outPath;
  std::string inputPath;
  std::string inputDir;
  double seconds = 60.0;
  bool quiet = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--quiet") {
      quiet = true;
    } else if (arg == "-o" || arg == "--seconds" || arg == "--input" || arg == "--input-dir") {
      if (i + 1 >= argc) {
        std::cerr << arg << " needs a value\n";
        usage(argv[0]);
      }
      const std::string value = argv[++i];
      if (arg == "-o") {
        outPath = value;
      } else if (arg == "--input") {
        inputPath = value;
      } else if (arg == "--input-dir") {
        inputDir = value;
      } else {
        seconds = std::strtod(value.c_str(), nullptr);
        if (seconds <= 0.0) {
          std::cerr << "--seconds needs a positive number\n";
          usage(argv[0]);
        }
      }
    } else if (directory.empty()) {
      directory = arg;
    } else if (imagePath.empty()) {
      imagePath = arg;
    } else {
      usage(argv[0]);
    }
  }
  if (directory.empty() || imagePath.empty() || outPath.empty()) usage(argv[0]);
  if (!inputPath.empty() && !inputDir.empty()) {
    std::cerr << "--input and --input-dir both name a recorded run; give one\n";
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
    std::cout << "dropped " << snaggletooth::describeCopierHeader(*copier, rom.size()) << "\n";
  }
  if (rom.empty()) {
    std::cerr << imagePath << " holds no cartridge image\n";
    return 2;
  }

  // The tree's program, read from the file `snes_disasm` wrote; the image must
  // be the one the file says it is a program of.
  bool ok = false;
  const std::filesystem::path programPath = std::filesystem::path(directory) / "program.snagir";
  const std::string programText = readText(programPath.string(), ok);
  if (!ok) {
    std::cerr << "cannot open " << programPath.string() << "\n";
    return 2;
  }
  std::string error;
  const std::optional<snaggletooth::ir::Parsed> parsed =
      snaggletooth::ir::parseProgram(programText, error);
  if (!parsed) {
    std::cerr << programPath.string() << ": " << error << "\n";
    return 2;
  }
  if (parsed->file.imageBytes != rom.size()) {
    std::cerr << programPath.string() << " is a program of an image of " << parsed->file.imageBytes
              << " bytes; " << imagePath << " holds " << rom.size() << "\n";
    return 2;
  }
  const snaggletooth::ir::Program& program = parsed->program;
  const snaggletooth::ir::ProgramCounts counts = snaggletooth::ir::countProgram(*parsed);

  snaggletooth::disasm::InputScript input;
  if (!inputPath.empty()) {
    const std::string text = readText(inputPath, ok);
    if (!ok) {
      std::cerr << "cannot open " << inputPath << "\n";
      return 2;
    }
    const auto parsed = snaggletooth::disasm::parseInputScript(text, error);
    if (!parsed) {
      std::cerr << inputPath << ": " << error << "\n";
      return 2;
    }
    input = *parsed;
  }

  snaggletooth::ir::Replay replay;
  replay.rom = rom;
  replay.masterCycles = static_cast<std::uint64_t>(seconds * 21'477'272.0);
  replay.input = input;
  replay.divergenceLimit = 200;
  snaggletooth::disasm::ProgressPrinter printer(std::cerr, errorIsTerminal());
  if (!quiet) replay.progress = std::ref(printer);
  const snaggletooth::ir::DifferentialReport report =
      snaggletooth::ir::differential(program, replay);
  printer.finish();

  std::error_code ec;
  std::filesystem::create_directories(outPath, ec);
  const std::filesystem::path out = outPath;
  {
    std::ofstream f(out / "summary.txt");
    if (!f) {
      std::cerr << "cannot write under " << outPath << "\n";
      return 2;
    }
    f << "code lines " << counts.codeLines << "\nnodes " << counts.nodes
      << "\nmaster cycles run " << report.masterCycles
      << "\ninstructions checked " << report.instructions
      << "\nhardware interrupts checked " << report.interrupts
      << "\nCPU cycles checked " << report.cpuCycles
      << "\nsteps held by a transfer " << report.heldSteps
      << "\nhalted cycles " << report.haltedCycles
      << "\nwaits released " << report.releases
      << "\ninstructions with no node " << report.unlifted << " at "
      << report.unliftedSites.size() << " addresses"
      << "\nstopped " << (report.stopped ? "yes" : "no")
      << "\ndivergences " << report.divergences.size() << "\n";
  }
  {
    std::ofstream f(out / "divergences.txt");
    for (const snaggletooth::ir::Divergence& dv : report.divergences) {
      f << "step " << dv.instruction << "  " << hex(dv.site, 6) << "  " << dv.name
        << (dv.mode.emulation ? "  e=1" : "  e=0")
        << (dv.effect ? "  effect " + std::to_string(*dv.effect) : std::string()) << "  "
        << dv.what << ": machine " << hex(dv.expected, 1) << " interpreter "
        << hex(dv.actual, 1) << "\n";
    }
  }
  {
    std::ofstream f(out / "forms.txt");
    for (const auto& [form, count] : report.forms) f << count << "\t" << form << "\n";
  }
  {
    std::ofstream f(out / "constructs.txt");
    f << "# how many times the run exercised each construct; zero means it rests on the "
         "vector proof alone\n";
    for (const auto& [name, count] : report.constructs) f << count << "\t" << name << "\n";
  }
  {
    std::ofstream f(out / "unlifted.txt");
    for (const snaggletooth::ir::Address a : report.unliftedSites) f << hex(a, 6) << "\n";
  }

  std::size_t unexercised = 0;
  for (const auto& [name, count] : report.constructs) unexercised += count == 0 ? 1u : 0u;
  std::cout << (report.divergences.empty() ? "OK " : "BAD") << ": " << report.instructions
            << " instructions, " << report.interrupts << " interrupts, " << report.cpuCycles
            << " CPU cycles, " << report.heldSteps << " held, " << report.unlifted
            << " unlifted at " << report.unliftedSites.size() << " addresses, "
            << report.forms.size() << " forms, " << unexercised << " of "
            << report.constructs.size() << " constructs unexercised, "
            << (report.stopped ? "stopped" : "ran out the budget") << ", "
            << report.divergences.size() << " divergences\n";
  return report.divergences.empty() ? 0 : 1;
}
