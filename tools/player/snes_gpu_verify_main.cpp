// snes_gpu_verify — the GPU PPU held to the machine's own frame over a whole run,
// with no window: one cartridge run for a number of seconds under its input script,
// as the sweep runs it, on a machine given the player's GPU device with a frame
// observer and a timeline observer both set, and at every timeline report — made
// after the device has drawn the frame — the texture read back and compared with
// the frame the machine drew in the same cycle, a frame 512 wide column for column,
// one 256 wide position for position. One tab-separated line:
//
//   image, the FNV-1a digest of every frame as the sweep computes it, the frames,
//   the timelines, the frames whose pixels differ from the texture's, the frames
//   whose width, height or parity differ from the timeline's, the frames with no
//   timeline in their cycle, for the first frame whose pixels differ its index,
//   line and position or `-`, the median nanoseconds the stepping thread spent in
//   the device's draw() a frame, uploading, dispatching and submitting, and the
//   most of any frame, the median and the largest nanoseconds from a frame's
//   draw() to the device finishing it, and the median packed frame in bytes.
//
// With `frames`, the digest folds only the first that many frames, as the sweep
// harness's does, and the frame count is still every frame reported; `--ppu-thread`
// builds the machine with the picture thread on. `--pass-times` submits each
// frame's upload and each of its three passes alone and times each to its fence
// (SdlGpuDevice::setTimesPasses), and appends ten columns: the median and the
// largest nanoseconds of the upload, the sprite pass, the tile pass and the pixel
// pass, then the register events and the palette events of the frame whose device
// time was the largest. Under it the device columns carry a fence round trip for
// each part, so they apportion a frame's time and are not its cost. A machine that cannot open a device
// prints `<image>	NO GPU	<reason>` and exits 0.
//
//   snes_gpu_verify <image> <seconds> <input-dir|-> [frames] [--ppu-thread] [--pass-times]

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include "SDL3/SDL.h"
#include "SDL3/SDL_main.h"
#include "player/sdl_gpu_device.h"
#include "rom/input_script.h"
#include "snaggletooth/snes/cartridge.h"
#include "snaggletooth/snes/snes.h"

using namespace snaggletooth;

namespace {

struct Verify final : FrameObserver, PpuTimelineObserver {
  const Snes* machine = nullptr;
  player::SdlGpuDevice* device = nullptr;
  std::uint64_t value = 1469598103934665603ull;
  unsigned folded = 0xFFFFFFFFu;  // the frames the digest folds, from the first
  unsigned frames = 0, timelines = 0, pixelMismatches = 0, shapeMismatches = 0, unmatched = 0;
  std::string first = "-";
  std::vector<std::uint8_t> pixels;
  unsigned width = 0, height = 0;
  std::uint8_t field = 0;
  std::uint64_t at = 0;
  bool pending = false;
  std::vector<std::uint8_t> gpu;
  std::vector<std::uint64_t> nanos;
  std::vector<std::uint64_t> gpuNanos;
  std::vector<std::size_t> packed;
  bool timesPasses = false;
  std::vector<std::uint64_t> uploadNanos, spriteNanos, tileNanos, pixelNanos;
  std::uint64_t worstNanos = 0;
  std::size_t worstRegisterEvents = 0, worstPaletteEvents = 0;

  void frame(const VideoFrame& picture) override {
    if (pending) ++unmatched;
    ++frames;
    if (frames <= folded) {
      for (const std::uint8_t byte : picture.pixels) {
        value ^= byte;
        value *= 1099511628211ull;
      }
      value ^= picture.width;
      value *= 1099511628211ull;
    }
    pixels.assign(picture.pixels.begin(), picture.pixels.end());
    width = picture.width;
    height = picture.height;
    field = picture.field;
    at = machine->state().master;
    pending = true;
  }
  void frame(const PpuTimeline& t) override {
    ++timelines;
    nanos.push_back(device->lastDrawNanos());
    gpuNanos.push_back(device->waitForDrawn());
    packed.push_back(t.packedSize());
    if (timesPasses) {
      uploadNanos.push_back(device->uploadNanos());
      spriteNanos.push_back(device->passNanos(gpu::Pass::Sprites));
      tileNanos.push_back(device->passNanos(gpu::Pass::Tiles));
      pixelNanos.push_back(device->passNanos(gpu::Pass::Pixels));
      if (gpuNanos.back() >= worstNanos) {
        worstNanos = gpuNanos.back();
        worstPaletteEvents = static_cast<std::size_t>(
            std::count_if(t.events.begin(), t.events.end(), [](const PpuTimeline::Event& event) {
              return event.field() == PpuTimeline::Field::PaletteWord;
            }));
        worstRegisterEvents = t.events.size() - worstPaletteEvents;
      }
    }
    if (!pending || at != machine->state().master) return;
    pending = false;
    if (t.width != width || t.height != height || t.field != field) {
      ++shapeMismatches;
      return;
    }
    if (!device->readBack(t.width, t.height, gpu)) {
      ++pixelMismatches;
      if (first == "-") first = std::to_string(frames - 1u) + ":read-back";
      return;
    }
    for (unsigned row = t.firstLine - 1u; row < height; ++row) {
      for (unsigned x = 0u; x < width; ++x) {
        const std::size_t index = (static_cast<std::size_t>(row) * width + x) * 4u;
        if (!std::equal(pixels.begin() + static_cast<std::ptrdiff_t>(index),
                        pixels.begin() + static_cast<std::ptrdiff_t>(index + 4u),
                        gpu.begin() + static_cast<std::ptrdiff_t>(index))) {
          if (first == "-") {
            first = std::to_string(frames - 1u) + ":" + std::to_string(row + 1u) + ":" + std::to_string(x);
          }
          ++pixelMismatches;
          return;
        }
      }
    }
  }
};

template <typename T>
T median(std::vector<T> values) {
  if (values.empty()) return T{};
  std::sort(values.begin(), values.end());
  return values[values.size() / 2u];
}

template <typename T>
T largest(const std::vector<T>& values) {
  return values.empty() ? T{} : *std::max_element(values.begin(), values.end());
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 4 || argc > 7) {
    std::fprintf(stderr,
                 "usage: snes_gpu_verify <image> <seconds> <input-dir|-> [frames] [--ppu-thread] [--pass-times]\n");
    return 2;
  }
  bool pictureThread = false;
  bool timesPasses = false;
  unsigned folded = 0xFFFFFFFFu;
  for (int at = 4; at < argc; ++at) {
    const std::string arg = argv[at];
    if (arg == "--ppu-thread") {
      pictureThread = true;
    } else if (arg == "--pass-times") {
      timesPasses = true;
    } else {
      folded = static_cast<unsigned>(std::stoul(arg));
    }
  }
  const std::filesystem::path imagePath = argv[1];
  const std::string name = imagePath.filename().string();
  std::ifstream in(imagePath, std::ios::binary);
  if (!in) {
    std::printf("%s\tUNREADABLE\n", name.c_str());
    return 0;
  }
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  std::vector<std::uint8_t> rom(text.begin(), text.end());
  if (readCopierHeader(rom).has_value()) {
    rom.erase(rom.begin(), rom.begin() + static_cast<std::ptrdiff_t>(kCopierHeaderBytes));
  }

  disasm::InputScript input;
  if (std::string(argv[3]) != "-") {
    const std::filesystem::path scriptPath = disasm::scriptFor(argv[3], imagePath);
    std::ifstream script(scriptPath);
    if (script) {
      const std::string body((std::istreambuf_iterator<char>(script)), std::istreambuf_iterator<char>());
      std::string error;
      if (const std::optional<disasm::InputScript> parsed = disasm::parseInputScript(body, error)) input = *parsed;
    }
  }

  SDL_SetMainReady();
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    std::printf("%s\tNO GPU\t%s\n", name.c_str(), SDL_GetError());
    return 0;
  }
  const struct Shutdown {
    ~Shutdown() { SDL_Quit(); }
  } shutdown;
  player::SdlGpuDevice device;
  {
    std::string error;
    if (!device.open(error)) {
      std::printf("%s\tNO GPU\t%s\n", name.c_str(), error.c_str());
      return 0;
    }
  }
  device.setPlacement(gpu::FieldPlacement::Field);
  device.setTimesPasses(timesPasses);

  try {
    SnesConfig config{.rom = rom};
    config.ppuThread = pictureThread;
    config.gpuPpu = &device;
    Snes machine(config);
    Verify verify;
    verify.folded = folded;
    verify.machine = &machine;
    verify.device = &device;
    verify.timesPasses = timesPasses;
    machine.setFrameObserver(&verify);
    machine.setPpuTimelineObserver(&verify);

    constexpr std::uint64_t kLineMaster = 1364u;
    const std::uint64_t budget = std::stoull(argv[2]) * 21477272ull;
    std::uint64_t spent = 0;
    std::uint32_t padFrame = 0;
    machine.setJoypad(JoypadPort::One, input.padAt(JoypadPort::One, 0));
    machine.setJoypad(JoypadPort::Two, input.padAt(JoypadPort::Two, 0));
    while (spent < budget) {
      const std::uint16_t lineBefore = machine.state().vpos;
      spent += machine.run(kLineMaster);
      if (machine.state().vpos < lineBefore) {
        ++padFrame;
        machine.setJoypad(JoypadPort::One, input.padAt(JoypadPort::One, padFrame));
        machine.setJoypad(JoypadPort::Two, input.padAt(JoypadPort::Two, padFrame));
      }
    }
    if (verify.pending) ++verify.unmatched;
    std::printf("%s\t%016llX\t%u\t%u\t%u\t%u\t%u\t%s\t%llu\t%llu\t%llu\t%llu\t%llu", name.c_str(),
                static_cast<unsigned long long>(verify.value), verify.frames, verify.timelines, verify.pixelMismatches,
                verify.shapeMismatches, verify.unmatched, verify.first.c_str(),
                static_cast<unsigned long long>(median(verify.nanos)),
                static_cast<unsigned long long>(device.drawNanosMost()),
                static_cast<unsigned long long>(median(verify.gpuNanos)),
                static_cast<unsigned long long>(largest(verify.gpuNanos)),
                static_cast<unsigned long long>(median(verify.packed)));
    if (timesPasses) {
      for (const std::vector<std::uint64_t>* part :
           {&verify.uploadNanos, &verify.spriteNanos, &verify.tileNanos, &verify.pixelNanos}) {
        std::printf("\t%llu\t%llu", static_cast<unsigned long long>(median(*part)),
                    static_cast<unsigned long long>(largest(*part)));
      }
      std::printf("\t%zu\t%zu", verify.worstRegisterEvents, verify.worstPaletteEvents);
    }
    std::printf("\n");
  } catch (const std::exception& e) {
    std::printf("%s\tFAILED\t%s\n", name.c_str(), e.what());
  }
  return 0;
}
