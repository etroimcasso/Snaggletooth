// spc_render — loads a .spc image, runs the machine for a requested duration, and
// writes the DSP's 32 kHz stereo output as a WAV file.
//
//   spc_render <in.spc> (--seconds N | --samples N) -o <out.wav>
//
// Rendering is from sample zero, verbatim: no warm-up, no skip, no fade — exactly N
// samples from the restored state. N frames costs run(N*32) machine cycles; the
// final instruction may overshoot a frame boundary, and any frames past N are
// truncated here so the output is exactly N. Aligning the stream against any
// reference is left to the caller.

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "snaggletooth/apu/apu.h"
#include "spc_loader.h"
#include "wav_writer.h"

namespace {

constexpr std::uint32_t kSampleRate = 32000;

[[noreturn]] void usage(const char* prog) {
  std::cerr << "usage: " << prog
            << " <in.spc> (--seconds N | --samples N) -o <out.wav>\n";
  std::exit(2);
}

}  // namespace

int main(int argc, char** argv) {
  std::string inPath;
  std::string outPath;
  std::uint64_t samples = 0;
  bool haveDuration = false;

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
  const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                        std::istreambuf_iterator<char>());

  snaggletooth::spc::SpcLoad load = snaggletooth::spc::parseSpc(bytes);
  if (!load.state) {
    std::cerr << "cannot load " << inPath << ": " << load.error << "\n";
    return 1;
  }

  snaggletooth::Apu apu{*load.state};
  apu.run(samples * 32);
  std::vector<snaggletooth::StereoFrame> frames = apu.takeFrames();
  if (frames.size() > samples) frames.resize(samples);

  const std::vector<std::uint8_t> wav =
      snaggletooth::spc::writeWav(frames, kSampleRate);
  std::ofstream out(outPath, std::ios::binary);
  if (!out) {
    std::cerr << "cannot write " << outPath << "\n";
    return 1;
  }
  out.write(reinterpret_cast<const char*>(wav.data()),
            static_cast<std::streamsize>(wav.size()));
  return 0;
}
