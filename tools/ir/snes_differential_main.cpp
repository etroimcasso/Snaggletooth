// snes_differential — replays a cartridge's recorded run beside the interpreter
// and reports where the lifted program and the machine disagree.
//
//   snes_differential <directory> <image> -o <report> [--seconds N]
//                                                     [--input <script> | --input-dir <directory>]
//                                                     [--quiet]
//
// Reads the directory's `program.snagir` — the program `snes_disasm` wrote, in
// the grammar `docs/snagir.md` gives — and its `apu.snagir` where the tree has
// one, and runs the machine on the image for `--seconds` of the master clock
// (sixty by default) with the interpreters beside it, held to every access,
// every register and every cycle on both CPUs. The image must be the one the
// file is a program of: its size is checked against the file's own `image`
// line before anything runs. `--input` replays a recorded run into the
// controller ports, exactly as `snes_disasm --input` does, so the same run is
// checked that produced the tree; `--input-dir` finds the run named for the
// image under that directory, or its `default.snaginput`, as `snes_disasm --input-dir` does, and leaves the
// ports empty when there is none.
//
// The report is written under `-o`: `summary.txt` (what was checked and how
// much, the sound CPU's lines after the main CPU's), `divergences.txt` (each
// disagreement with its step, node, effect and the two values; a sound CPU's
// names its site `apu $XXXX`), `forms.txt` (how many times each instruction
// form ran under each mode, and each sound-CPU form as `apu MOV A,dp`),
// `constructs.txt` (how many times each named construct was exercised, zero
// where the run never reached one), `unlifted.txt` (the addresses the run
// executed that the tree has no instruction for, as the tree places them, the
// sound CPU's as `apu $XXXX`), and `patched.txt` (the audio addresses where
// the bytes the sound CPU fetched are not the sound file's node's). One line
// on standard output sums it up.
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
    const std::filesystem::path script = snaggletooth::disasm::scriptFor(inputDir, imagePath);
    if (std::filesystem::is_regular_file(script)) {
      inputPath = script.string();
      std::cout << "replaying " << script.string() << "\n";
    } else {
      std::cout << "no recorded run at " << script.string() << " and no default.snaginput beside it; the ports stay empty\n";
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
  snaggletooth::ir::Program program = parsed->program;
  const snaggletooth::ir::ProgramCounts counts = snaggletooth::ir::countProgram(*parsed);

  // The sound program, where the tree has one: its nodes join the program's,
  // in the audio unit's own space.
  snaggletooth::ir::ProgramCounts soundCounts;
  const std::filesystem::path soundPath = std::filesystem::path(directory) / "apu.snagir";
  if (std::filesystem::is_regular_file(soundPath)) {
    const std::string soundText = readText(soundPath.string(), ok);
    if (!ok) {
      std::cerr << "cannot open " << soundPath.string() << "\n";
      return 2;
    }
    const std::optional<snaggletooth::ir::Parsed> parsedSound =
        snaggletooth::ir::parseProgram(soundText, error);
    if (!parsedSound) {
      std::cerr << soundPath.string() << ": " << error << "\n";
      return 2;
    }
    program.spc700 = parsedSound->program.spc700;
    soundCounts = snaggletooth::ir::countProgram(*parsedSound);
  }

  snaggletooth::disasm::InputScript input;
  if (!inputPath.empty()) {
    const std::string text = readText(inputPath, ok);
    if (!ok) {
      std::cerr << "cannot open " << inputPath << "\n";
      return 2;
    }
    const auto parsedScript = snaggletooth::disasm::parseInputScript(text, error);
    if (!parsedScript) {
      std::cerr << inputPath << ": " << error << "\n";
      return 2;
    }
    input = *parsedScript;
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
      << "\ndivergences " << report.divergences.size()
      << "\nsound code lines " << soundCounts.codeLines << "\nsound nodes " << soundCounts.nodes
      << "\nsound instructions checked " << report.spc700Instructions
      << "\nsound CPU cycles checked " << report.spc700Cycles
      << "\nsound instructions with no node " << report.spc700Unlifted << " at "
      << report.spc700UnliftedSites.size() << " addresses"
      << "\nsound instructions with other bytes than the file's " << report.spc700Patched << " at "
      << report.spc700PatchedSites.size() << " addresses\n";
  }
  std::size_t soundDivergences = 0;
  {
    std::ofstream f(out / "divergences.txt");
    for (const snaggletooth::ir::Divergence& dv : report.divergences) {
      if (dv.processor == snaggletooth::ir::Processor::Spc700) {
        ++soundDivergences;
        f << "step " << dv.instruction << "  apu " << hex(dv.site, 4) << "  " << dv.name
          << (dv.effect ? "  effect " + std::to_string(*dv.effect) : std::string()) << "  "
          << dv.what << ": machine " << hex(dv.expected, 1) << " interpreter "
          << hex(dv.actual, 1) << "\n";
        continue;
      }
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
    for (const auto& [form, count] : report.spc700Forms) f << count << "\tapu " << form << "\n";
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
    for (const snaggletooth::ir::Address a : report.spc700UnliftedSites) f << "apu " << hex(a, 4) << "\n";
  }
  {
    std::ofstream f(out / "patched.txt");
    for (const snaggletooth::ir::Address a : report.spc700PatchedSites) f << "apu " << hex(a, 4) << "\n";
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
            << report.divergences.size() << " divergences; sound: "
            << report.spc700Instructions << " instructions, " << report.spc700Cycles
            << " cycles, " << report.spc700Unlifted << " unlifted at "
            << report.spc700UnliftedSites.size() << " addresses, " << report.spc700Patched
            << " with other bytes at " << report.spc700PatchedSites.size() << " addresses, "
            << report.spc700Forms.size() << " forms, " << soundDivergences << " divergences\n";
  return report.divergences.empty() ? 0 : 1;
}
