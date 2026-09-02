// rom_render — boots a cartridge on the SNES machine, runs it for a requested
// duration, and writes the DSP's 32 kHz stereo output as a WAV file.
//
//   rom_render <in.smc> (--seconds N | --samples N) -o <out.wav> [--quiet]
//
// A cartridge drives its sound driver from the 65816: song changes, effects and
// per-frame parameter writes all reach the audio unit, so a render carries the
// command stream a standalone .spc image does not have. Rendering starts at power-on
// and runs forward verbatim — no warm-up, no skip, no fade — so the opening seconds
// are whatever the game does before it starts its driver, silence included.
//
// A copier header, the 512 bytes some dumps carry ahead of the image, is dropped
// when the file length says one is present. Progress goes to stdout a second at a
// time unless --quiet is given; the WAV goes to the named file.

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "snaggletooth/snes/snes.h"
#include "spc/wav_writer.h"

namespace {

constexpr std::uint32_t kSampleRate = 32000;

// One second of the master clock the machine counts in.
constexpr std::uint64_t kMasterPerSecond = 21477272ull;

[[noreturn]] void usage(const char* prog) {
  std::cerr << "usage: " << prog
            << " <in.smc> (--seconds N | --samples N) -o <out.wav> [--quiet]\n";
  std::exit(2);
}

}  // namespace

int main(int argc, char** argv) {
  std::string inPath;
  std::string outPath;
  std::uint64_t samples = 0;
  bool haveDuration = false;
  bool quiet = false;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    auto next = [&](const char* name) -> std::string {
      if (i + 1 >= argc) {
        std::cerr << name << " needs a value\n";
        usage(argv[0]);
      }
      return argv[++i];
    };
    if (arg == "--seconds") {
      samples = std::stoull(next("--seconds")) * kSampleRate;
      haveDuration = true;
    } else if (arg == "--samples") {
      samples = std::stoull(next("--samples"));
      haveDuration = true;
    } else if (arg == "--quiet") {
      quiet = true;
    } else if (arg == "-o") {
      outPath = next("-o");
    } else if (inPath.empty()) {
      inPath = arg;
    } else {
      usage(argv[0]);
    }
  }
  if (inPath.empty() || outPath.empty() || !haveDuration) usage(argv[0]);

  std::ifstream in(inPath, std::ios::binary);
  if (!in) {
    std::cerr << "cannot open " << inPath << "\n";
    return 1;
  }
  std::vector<std::uint8_t> rom((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
  if (rom.size() % 1024 == 512) rom.erase(rom.begin(), rom.begin() + 512);
  if (rom.empty()) {
    std::cerr << inPath << " holds no cartridge image\n";
    return 1;
  }

  snaggletooth::Snes machine{snaggletooth::SnesConfig{.rom = rom}};
  std::vector<snaggletooth::StereoFrame> frames;
  frames.reserve(samples);

  // Run a second at a time so a long render can report as it goes; the machine
  // carries any overshoot between calls, so the total is the same either way.
  const std::uint64_t seconds = (samples + kSampleRate - 1) / kSampleRate;
  for (std::uint64_t s = 0; s < seconds && frames.size() < samples; ++s) {
    machine.run(kMasterPerSecond);
    const std::vector<snaggletooth::StereoFrame> got = machine.takeFrames();
    frames.insert(frames.end(), got.begin(), got.end());
    if (quiet) continue;
    std::size_t sounding = 0;
    for (int v = 0; v < 8; ++v) {
      if (machine.state().apu.dsp.voices[v].envelope) ++sounding;
    }
    std::size_t nonzero = 0;
    for (const snaggletooth::StereoFrame& f : got) {
      if (f.left || f.right) ++nonzero;
    }
    std::cout << "  " << (s + 1) << "s  nonzero=" << nonzero << "  voices=" << sounding << "\n"
              << std::flush;
  }
  if (frames.size() > samples) frames.resize(samples);

  const std::vector<std::uint8_t> wav = snaggletooth::spc::writeWav(frames, kSampleRate);
  std::ofstream out(outPath, std::ios::binary);
  if (!out) {
    std::cerr << "cannot write " << outPath << "\n";
    return 1;
  }
  out.write(reinterpret_cast<const char*>(wav.data()), static_cast<std::streamsize>(wav.size()));
  if (!quiet) std::cout << frames.size() << " frames -> " << outPath << "\n";
  return 0;
}
