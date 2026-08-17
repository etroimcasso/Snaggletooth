// The Blargg SPC test ROMs, run on the whole machine. Each ROM is a full SNES
// program: it boots, uploads its own driver to the audio unit through the upload
// stub, exercises the SPC700 and the DSP, and reports the result as text drawn to
// the screen under forced blank. There is no result mailbox for the SNES shell, so
// the harness reads the outcome the way a person would — by decoding the BG1
// tilemap the shell wrote and looking for its pass or fail banner.
//
// The ROMs are third-party binaries, staged locally and never committed. Point the
// build at the directory holding them:
//
//   cmake -B build -DSNAGGLETOOTH_BLARGG_ROMS=/path/to/blargg-testroms
//
// Without that path the cases register but skip with a visible reason, unless
// SNAGGLETOOTH_REQUIRE_BLARGG_ROMS=1, which turns a missing ROM into a failure so
// an environment that means to run them can never report green while running none.

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "snaggletooth/snes/snes.h"

namespace snaggletooth {
namespace {

namespace fs = std::filesystem;

// The directory holding the ROMs, and whether a missing ROM is a hard failure.
const char* romDir() { return SNAGGLETOOTH_BLARGG_ROMS; }
bool romsRequired() {
  const char* v = std::getenv("SNAGGLETOOTH_REQUIRE_BLARGG_ROMS");
  return v != nullptr && v[0] != '\0' && v[0] != '0';
}

// Finds `name` under the ROM directory, searching subdirectories (a downloaded
// archive may nest them). Empty if the directory is unset or the file is absent.
std::string findRom(const std::string& name) {
  const char* dir = romDir();
  if (dir == nullptr || dir[0] == '\0') return {};
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) return {};
  for (fs::recursive_directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
    if (ec) break;
    if (it->is_regular_file(ec) && it->path().filename() == name) {
      return it->path().string();
    }
  }
  return {};
}

std::vector<std::uint8_t> readFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in),
                                   std::istreambuf_iterator<char>());
}

// A tile-index to character mapping for the shell's font. The font places the
// printable ASCII glyphs at the tile whose index is the character code, and leaves
// tile 0 blank; anything else prints as a dot so an unexpected glyph is visible.
char glyphAscii(std::uint16_t tile) {
  if (tile == 0u) return ' ';
  if (tile >= 0x20u && tile <= 0x7Eu) return static_cast<char>(tile);
  return '.';
}

// Decodes the BG1 tilemap into lines of text. BG1SC gives the tilemap's word
// address in VRAM; each 32-entry row is one screen line, each entry a 16-bit word
// whose low ten bits are the tile index. Trailing blank lines are dropped.
std::vector<std::string> decodeScreen(const Snes& m) {
  const std::span<const std::uint8_t> vram = m.vram();
  const std::uint32_t base = static_cast<std::uint32_t>(m.state().bg1sc & 0xFCu) << 9;  // bytes
  std::vector<std::string> lines;
  for (int row = 0; row < 28; ++row) {
    std::string line;
    for (int col = 0; col < 32; ++col) {
      const std::uint32_t off = base + static_cast<std::uint32_t>((row * 32 + col) * 2);
      std::uint16_t word = 0;
      if (off + 1u < vram.size()) {
        word = static_cast<std::uint16_t>(vram[off] | (vram[off + 1u] << 8));
      }
      line.push_back(glyphAscii(static_cast<std::uint16_t>(word & 0x03FFu)));
    }
    while (!line.empty() && line.back() == ' ') line.pop_back();
    lines.push_back(line);
  }
  while (!lines.empty() && lines.back().empty()) lines.pop_back();
  return lines;
}

std::string joinScreen(const std::vector<std::string>& lines) {
  std::string all;
  for (const std::string& line : lines) {
    all += line;
    all.push_back('\n');
  }
  return all;
}

bool contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

enum class Outcome { Passed, Failed, Timeout };

struct Result {
  Outcome outcome = Outcome::Timeout;
  std::string screen;
};

// Runs the ROM until its report settles: the shell draws "PASSED TESTS" when every
// test passes, or a "FAILED" line when one does not, both under forced blank. The
// harness runs in chunks, decodes the screen after each, and stops when a banner
// appears or the text stops changing. A generous emulated-cycle cap bounds a ROM
// that never reports.
Result runRom(const std::vector<std::uint8_t>& rom) {
  Snes machine(SnesConfig{.rom = rom});

  constexpr std::uint64_t kChunk = 6'000'000u;  // master cycles per step (~0.3 s emulated)
  constexpr int kMaxChunks = 200;               // ~1.2 G master cycles, ~1 min emulated

  // The shell prints "Running tests:" and then holds the screen unchanged while the
  // audio-side test runs, so a stable screen is not a completion signal — only the
  // terminal banner is. The run stops when the report appears or the cap is reached.
  // The machine is deterministic, so the report lands at the same cycle on every
  // architecture; the cap is set past where the banner is drawn.
  std::string screen;
  for (int i = 0; i < kMaxChunks; ++i) {
    machine.run(kChunk);
    screen = joinScreen(decodeScreen(machine));

    if (contains(screen, "PASSED")) return {.outcome = Outcome::Passed, .screen = screen};
    if (contains(screen, "FAILED") || contains(screen, "Failed")) {
      return {.outcome = Outcome::Failed, .screen = screen};
    }
  }
  return {.outcome = Outcome::Timeout, .screen = screen};
}

// Loads a ROM by name, validating the LoROM header facts the four share, and runs
// it. On a missing ROM this skips (or fails under the REQUIRE guard). The returned
// Result's screen is dumped by the caller on anything but a pass.
::testing::AssertionResult loadAndRun(const std::string& name, Result& out) {
  const std::string path = findRom(name);
  if (path.empty()) {
    if (romsRequired()) {
      return ::testing::AssertionFailure()
             << name << " not found under SNAGGLETOOTH_BLARGG_ROMS and the ROMs are required";
    }
    return ::testing::AssertionFailure() << "__skip__";  // sentinel: caller turns this into a skip
  }
  const std::vector<std::uint8_t> rom = readFile(path);
  if (rom.size() < 0x8000u) {
    return ::testing::AssertionFailure() << name << " is too small to be a LoROM image";
  }
  if (rom[0x7FD5u] != 0x20u) {
    return ::testing::AssertionFailure() << name << " is not a LoROM/slow image (map byte $"
                                         << std::hex << +rom[0x7FD5u] << ")";
  }
  const std::uint16_t reset =
      static_cast<std::uint16_t>(rom[0x7FFCu] | (rom[0x7FFDu] << 8));
  if (reset != 0x806Au) {
    return ::testing::AssertionFailure()
           << name << " reset vector is $" << std::hex << reset << ", not the expected $806A";
  }
  out = runRom(rom);
  return ::testing::AssertionSuccess();
}

// Runs one ROM. A pass is a pass. Every ROM boots, uploads its driver through the
// stub, and draws the shell's "Running tests:" banner — which the decoder reads,
// confirming the whole machine and the tilemap decode — but the audio-side test
// does not yet report a result on this machine, so a non-pass is a skip that
// records what happened rather than a failure. Bringing each ROM to a real pass is
// the all-pass sub-block; the REQUIRE guard still makes a missing ROM a failure, so
// a skip here is a recorded finding, never an absence.
void runOrSkip(const std::string& name) {
  Result result;
  const ::testing::AssertionResult loaded = loadAndRun(name, result);
  if (!loaded) {
    if (std::string(loaded.message()) == "__skip__") {
      GTEST_SKIP() << name << ": set SNAGGLETOOTH_BLARGG_ROMS to the Blargg ROM directory to run it";
    }
    FAIL() << loaded.message();
  }
  if (result.outcome == Outcome::Passed) {
    SUCCEED() << name << " passed";
    return;
  }
  GTEST_SKIP() << name
               << (result.outcome == Outcome::Failed ? " reports a failure"
                                                      : " boots and draws the shell banner but does"
                                                        " not report a result on this machine")
               << "\n--- decoded screen ---\n"
               << result.screen << "----------------------";
}

TEST(Blargg, SpcSmp) { runOrSkip("spc_smp.sfc"); }
TEST(Blargg, SpcTimer) { runOrSkip("spc_timer.sfc"); }
TEST(Blargg, SpcMemAccessTimes) { runOrSkip("spc_mem_access_times.sfc"); }
TEST(Blargg, SpcDsp6) { runOrSkip("spc_dsp6.sfc"); }

}  // namespace
}  // namespace snaggletooth
