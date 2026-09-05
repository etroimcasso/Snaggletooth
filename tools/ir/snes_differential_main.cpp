// snes_differential — replays a cartridge's recorded run beside the interpreter
// and reports where the lifted program and the machine disagree.
//
//   snes_differential <directory> <image> -o <report> [--seconds N] [--input <script>]
//
// Reads the directory's `project.manifest`, traces the image as the manifest
// directs — its entries, its file split, the targets earlier runs saw — lifts
// every 65816 region into the intermediate representation, and runs the machine
// for `--seconds` of the master clock (sixty by default) with the interpreter
// beside it, held to every access, every register and every cycle. `--input`
// replays a recorded run into the controller ports, exactly as `snes_disasm
// --input` does, so the same run is checked that produced the tree.
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
// argument or an unreadable input.
//
// A copier header, the 512 bytes some dumps carry ahead of the image, is dropped
// when the file length says one is present.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "ir/cpu65816_lift.h"
#include "ir/ir.h"
#include "ir/ir_differential.h"
#include "rom/input_script.h"
#include "rom/rom_disasm.h"

namespace {

[[noreturn]] void usage(const char* prog) {
  std::cerr << "usage: " << prog
            << " <directory> <image> -o <report> [--seconds N] [--input <script>]\n"
               "  replays the tree's recorded run on the machine beside the interpreter\n";
  std::exit(2);
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

// The tree's 65816 regions lifted into one program, the nodes in address order.
snaggletooth::ir::Program liftTree(const snaggletooth::disasm::CartridgeDisassembly& d,
                                   std::size_t& codeLines) {
  snaggletooth::ir::Program program;
  for (const snaggletooth::disasm::RegionListing& region : d.regions) {
    std::vector<std::uint8_t> image;
    for (const snaggletooth::disasm::Line& line : region.listing.lines) {
      const std::vector<std::uint8_t>& bytes = line.isCode ? line.instruction.bytes : line.data;
      image.insert(image.end(), bytes.begin(), bytes.end());
      if (line.isCode) ++codeLines;
    }
    snaggletooth::ir::Program one =
        snaggletooth::ir::lift65816(region.listing, image, region.region.first);
    program.nodes.insert(program.nodes.end(), one.nodes.begin(), one.nodes.end());
    program.nmi = one.nmi;
    program.irq = one.irq;
  }
  std::stable_sort(program.nodes.begin(), program.nodes.end(),
                   [](const snaggletooth::ir::Node& a, const snaggletooth::ir::Node& b) {
                     return a.instruction.address < b.instruction.address;
                   });
  return program;
}

}  // namespace

int main(int argc, char** argv) {
  std::string directory;
  std::string imagePath;
  std::string outPath;
  std::string inputPath;
  double seconds = 60.0;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "-o" || arg == "--seconds" || arg == "--input") {
      if (i + 1 >= argc) {
        std::cerr << arg << " needs a value\n";
        usage(argv[0]);
      }
      const std::string value = argv[++i];
      if (arg == "-o") {
        outPath = value;
      } else if (arg == "--input") {
        inputPath = value;
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

  bool ok = false;
  const std::string manifestText =
      readText((std::filesystem::path(directory) / "project.manifest").string(), ok);
  if (!ok) {
    std::cerr << "cannot open " << directory << "/project.manifest\n";
    return 2;
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

  // The tree as the manifest directs it, traced without the machine run and
  // without the sound capture: the entries and the reached targets are already
  // in the manifest, and the sound program is the audio CPU's.
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
  std::size_t codeLines = 0;
  const snaggletooth::ir::Program program = liftTree(d, codeLines);

  snaggletooth::ir::Replay replay;
  replay.rom = rom;
  replay.masterCycles = static_cast<std::uint64_t>(seconds * 21'477'272.0);
  replay.input = input;
  replay.divergenceLimit = 200;
  const snaggletooth::ir::DifferentialReport report =
      snaggletooth::ir::differential(program, replay);

  std::error_code ec;
  std::filesystem::create_directories(outPath, ec);
  const std::filesystem::path out = outPath;
  {
    std::ofstream f(out / "summary.txt");
    if (!f) {
      std::cerr << "cannot write under " << outPath << "\n";
      return 2;
    }
    f << "code lines " << codeLines << "\nnodes " << program.nodes.size()
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
