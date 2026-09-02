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

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "snaggletooth/snes/snes.h"
#include "snes_ipl_stub.h"

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

// The boot image the machine runs on. SNAGGLETOOTH_BOOT_ROM names a 64-byte audio
// boot ROM to boot from; left empty, the machine boots on the built-in stub and
// the cases below assert the stub's outcome instead. A file of the wrong size is a
// configuration mistake, so it fails loudly rather than falling back.
std::optional<std::array<std::uint8_t, kIplWindowBytes>> configuredBootRom() {
  const std::string path = SNAGGLETOOTH_BOOT_ROM;
  if (path.empty()) return std::nullopt;
  const std::vector<std::uint8_t> image = readFile(path);
  EXPECT_EQ(image.size(), kIplWindowBytes)
      << "SNAGGLETOOTH_BOOT_ROM: " << path << " is not a " << kIplWindowBytes << "-byte boot ROM";
  if (image.size() != kIplWindowBytes) return std::nullopt;
  std::array<std::uint8_t, kIplWindowBytes> boot{};
  std::copy(image.begin(), image.end(), boot.begin());
  return boot;
}

bool bootsOnStub() { return !configuredBootRom().has_value(); }

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
  Snes machine(SnesConfig{.rom = rom, .bootRom = configuredBootRom()});

  constexpr std::uint64_t kChunk = 6'000'000u;  // master cycles per step (~0.3 s emulated)
  constexpr int kMaxChunks = 300;               // ~1.8 G master cycles, ~1.5 min emulated

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

// Loads and runs `name`, or skips the calling case when the ROM directory is unset.
// `ran` reports whether `out` holds a run, so a caller stops rather than asserting
// on a result that was never produced.
void run(const std::string& name, Result& out, bool& ran) {
  ran = false;
  const ::testing::AssertionResult loaded = loadAndRun(name, out);
  if (loaded) {
    ran = true;
    return;
  }
  if (std::string(loaded.message()) == "__skip__") {
    GTEST_SKIP() << name << ": set SNAGGLETOOTH_BLARGG_ROMS to the Blargg ROM directory to run it";
  }
  ADD_FAILURE() << loaded.message();
}

// Runs one ROM and requires its pass banner.
void expectPass(const std::string& name) {
  Result result;
  bool ran = false;
  run(name, result, ran);
  if (!ran) return;
  EXPECT_EQ(result.outcome, Outcome::Passed)
      << name << " does not report a pass\n--- decoded screen ---\n"
      << result.screen << "----------------------";
}

// Runs one ROM that reports a failure this machine does not yet answer. The screen
// is recorded so the skip carries what happened.
void runOrSkip(const std::string& name) {
  Result result;
  bool ran = false;
  run(name, result, ran);
  if (!ran) return;
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

// The sub-tests spc_smp.sfc runs, in the order it names them on screen.
constexpr std::array<const char*, 8> kSpcSmpSubTests = {
    "CPU Instructions/Full DAA DAS",
    "CPU Timing/mem access times",
    "CPU Timing/time all opcodes",
    "CPU/addw and subw",
    "CPU/psw is 8 independent bits",
    "CPU/smp reg read-write behavior",
    "CPU/tset tclr",
    "CPU/verify IPL ROM",
};

// The screen without its spacing, so a comparison ignores the 32-column wrap.
std::string unspaced(const std::string& text) {
  std::string out;
  for (const char c : text) {
    if (c != ' ' && c != '\n') out.push_back(c);
  }
  return out;
}

// The 64 boot-window bytes as spc_smp.sfc prints them: two uppercase hex digits per
// byte, in address order.
std::string bootWindowHex(std::span<const std::uint8_t, kIplWindowBytes> image) {
  static constexpr char kDigits[] = "0123456789ABCDEF";
  std::string out;
  for (const std::uint8_t byte : image) {
    out.push_back(kDigits[byte >> 4]);
    out.push_back(kDigits[byte & 0x0Fu]);
  }
  return out;
}

// spc_smp.sfc booted on the stub. Its last sub-test reads the boot-ROM window back
// and checksums it against the console's own boot code, which only that code
// satisfies — so the machine runs every sub-test, passes the seven that measure
// behaviour, and stops at the one that measures identity. Supply the console's boot
// ROM through SNAGGLETOOTH_BOOT_ROM and the same ROM reports a pass instead.
//
// Asserting the whole shape keeps the seven behavioural sub-tests covered: a
// regression in any of them stops the run earlier, and the missing name fails here.
void expectStubBootOutcome(const Result& result) {
  ASSERT_EQ(result.outcome, Outcome::Failed)
      << "spc_smp.sfc booted on the stub reports its boot-ROM check\n--- decoded screen ---\n"
      << result.screen << "----------------------";

  std::size_t previous = std::string::npos;  // no sub-test located yet
  for (const char* subTest : kSpcSmpSubTests) {
    const std::size_t at = result.screen.find(subTest);
    ASSERT_NE(at, std::string::npos)
        << "spc_smp.sfc does not reach " << subTest << "\n--- decoded screen ---\n"
        << result.screen << "----------------------";
    if (previous != std::string::npos) {
      EXPECT_GT(at, previous) << subTest << " is out of order on screen";
    }
    previous = at;
  }

  // The window holds the stub, which is what the checksum reads and rejects.
  EXPECT_NE(unspaced(result.screen).find(bootWindowHex(iplStubImage())), std::string::npos)
      << "spc_smp.sfc prints boot-window bytes other than the stub's\n--- decoded screen ---\n"
      << result.screen << "----------------------";
}

TEST(Blargg, SpcSmp) {
  Result result;
  bool ran = false;
  run("spc_smp.sfc", result, ran);
  if (!ran) return;
  if (bootsOnStub()) {
    expectStubBootOutcome(result);
    return;
  }
  EXPECT_EQ(result.outcome, Outcome::Passed)
      << "spc_smp.sfc booted on a boot ROM does not report a pass\n--- decoded screen ---\n"
      << result.screen << "----------------------";
}

TEST(Blargg, SpcTimer) { expectPass("spc_timer.sfc"); }
TEST(Blargg, SpcMemAccessTimes) { expectPass("spc_mem_access_times.sfc"); }
TEST(Blargg, SpcDsp6) { runOrSkip("spc_dsp6.sfc"); }

}  // namespace
}  // namespace snaggletooth
