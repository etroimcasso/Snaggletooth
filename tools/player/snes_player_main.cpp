// snes_player — runs a cartridge in a window, with its sound, and records the run
// when asked to.
//
//   snes_player <image> [--out <directory>] [--seconds N] [--scale N]
//                       [--input <script> | --input-dir <directory>] [--mute] [--quiet]
//
// The window shows the picture the machine draws, frame by frame, at the console's
// own rate, with that rate in its title so what the run costs is visible while it
// runs, and the DSP's own 32 kHz stereo goes to the default playback device as the
// machine produces it. It closes when the window is closed or when --seconds of the
// master clock have been spent; nothing else stops it. --mute leaves the sound where
// it is made, and a machine with no playback device runs silent and says so.
//
// The buttons come from a recorded run rather than the keyboard: --input names a
// script and --input-dir a directory of them, from which the one named for the image
// is replayed, or the directory's default.snaginput when the image has none
// (`rom/input_script.h`). Two runs of one cartridge are then the same run, which is
// what makes two recordings worth setting beside each other.
//
// --out writes what the run produced into a directory, named after the image: an
// uncompressed AVI of every frame exactly as the machine drove it, a table of what
// each frame cost, and the sound as a WAV. They come together — one run, one set of
// evidence — and without it the run keeps none of them, so nothing accumulates
// behind a run nobody asked to record.
//
// The machine runs at the NTSC clock rate.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "SDL3/SDL.h"
#include "SDL3/SDL_main.h"  // SDL_SetMainReady, which the tool calls to keep its own main()
#include "rom/input_script.h"
#include "snaggletooth/apu/dsp.h"
#include "snaggletooth/snes/cartridge.h"
#include "snaggletooth/snes/snes.h"
#include "snaggletooth/snes/video_frame.h"
#include "spc/wav_writer.h"
#include "video/avi_writer.h"

namespace {

using snaggletooth::Snes;
using snaggletooth::VideoFrame;

// The NTSC master clock, and the master cycles an NTSC frame takes on average: 262
// lines of 1364 cycles, less the four that one line of every second frame drops. The
// console's frame rate is the first over the second, which is not a whole number of
// frames a second and is carried as the ratio it is.
constexpr std::uint32_t kNtscMaster = 236250000u;   // 236,250,000 / 11 Hz, times eleven
constexpr std::uint32_t kNtscFrameElevenths = 3931026u;  // 11 * 357366 master cycles
constexpr std::uint64_t kMasterPerSecond = 21477272ull;

// How much the machine is run between drains of its sound: a quarter of a frame, so
// the queue stays short without the loop spinning.
constexpr std::uint64_t kChunkMaster = 89341ull;

constexpr std::uint64_t kNanosPerSecond = 1000000000ull;

// The nanosecond the Nth frame is due at, counted from the run's start. The frame
// count and the exact frame interval are multiplied apart rather than together: the
// whole product passes 64 bits at frame 4693, a little over a minute in, and a
// deadline that has wrapped is behind the clock, so the run stops waiting between
// frames and sprints until the wrap climbs back past it. Splitting the interval into
// its whole nanoseconds and the fraction left over keeps every deadline exact and
// leaves room for a run of a thousand billion frames.
constexpr std::uint64_t kFrameWholeNanos = kNanosPerSecond * kNtscFrameElevenths / kNtscMaster;
constexpr std::uint64_t kFrameRemainder = kNanosPerSecond * kNtscFrameElevenths % kNtscMaster;
[[nodiscard]] constexpr std::uint64_t frameDueAt(std::uint64_t frames) {
  return frames * kFrameWholeNanos + frames * kFrameRemainder / kNtscMaster;
}

// Two deadlines worked out by hand from 1,000,000,000 * 3,931,026 / 236,250,000:
// the first frame's, and one past the point a single product passes 64 bits, so a
// rearrangement that loses the fraction or wraps again does not compile.
static_assert(frameDueAt(1u) == 16639263ull);
static_assert(frameDueAt(4693u) == 78088063568ull);

// The DSP's own rate and shape, which the playback device is asked for directly so
// nothing resamples what the machine made.
constexpr int kSampleRate = 32000;
constexpr int kChannels = 2;
constexpr int kBytesPerSample = kChannels * static_cast<int>(sizeof(std::int16_t));

// How far the sound is allowed to run ahead of the speakers before a chunk is left
// out: a quarter of a second, which is latency a person notices. The machine is
// paced to the console's frame interval and the device consumes at its own crystal,
// so the two drift apart slowly; dropping a chunk costs a tenth of a frame of sound
// and puts the queue back where it belongs.
constexpr int kQueueBoundBytes = kSampleRate * kBytesPerSample / 4;

// The rate a frame's interval works out to, in thousandths of a frame a second, so a
// rate is reported exactly without leaving the integers.
[[nodiscard]] std::uint64_t milliFps(std::uint64_t nanos, std::uint64_t frames) {
  if (nanos == 0u) return 0u;
  return frames * 1000u * kNanosPerSecond / nanos;
}

[[nodiscard]] std::string rateText(std::uint64_t milli) {
  char text[32];
  std::snprintf(text, sizeof(text), "%llu.%03llu", static_cast<unsigned long long>(milli / 1000u),
                static_cast<unsigned long long>(milli % 1000u));
  return text;
}

bool readFile(const std::filesystem::path& path, std::string& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  out.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return true;
}

[[noreturn]] void usage(const char* program) {
  std::cerr << "usage: " << program
            << " <image> [--out <directory>] [--seconds N] [--scale N]"
               " [--input <script> | --input-dir <directory>] [--mute] [--quiet]\n";
  std::exit(2);
}

// The default playback device, taking the DSP's frames as they are made. A tool
// that could not open one still runs — the machine is the point and the picture is
// still there — so every method is safe on a sink that never opened.
class Sound {
 public:
  ~Sound() {
    if (stream_ != nullptr) SDL_DestroyAudioStream(stream_);
  }

  Sound(const Sound&) = delete;
  Sound& operator=(const Sound&) = delete;
  Sound() = default;

  // Opens the device at the DSP's own rate and starts it. Says why it could not
  // rather than failing the run.
  void open() {
    const SDL_AudioSpec spec{.format = SDL_AUDIO_S16, .channels = kChannels, .freq = kSampleRate};
    stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (stream_ == nullptr) {
      std::cerr << "no sound: " << SDL_GetError() << "\n";
      return;
    }
    SDL_ResumeAudioStreamDevice(stream_);
  }

  // The frames the machine has just made, handed to the device — unless the sound
  // already queued has run far enough ahead of the speakers that keeping this chunk
  // would only add to the delay.
  void put(const std::vector<snaggletooth::StereoFrame>& frames) {
    if (stream_ == nullptr || frames.empty()) return;
    if (SDL_GetAudioStreamQueued(stream_) > kQueueBoundBytes) return;
    SDL_PutAudioStreamData(stream_, frames.data(),
                           static_cast<int>(frames.size() * sizeof(snaggletooth::StereoFrame)));
  }

 private:
  SDL_AudioStream* stream_ = nullptr;
};

// The window, the recording and the table, all fed by the frames the machine
// finishes. It is the machine's frame observer, so every one of them arrives here as
// the beam wraps, and the run is held to the console's rate before the next begins.
class Player final : public snaggletooth::FrameObserver {
 public:
  Player(Snes& machine, const snaggletooth::disasm::InputScript& script, unsigned scale,
         const std::string& title)
      : machine_(machine), script_(script), scale_(scale), title_(title) {}

  ~Player() override {
    if (texture_ != nullptr) SDL_DestroyTexture(texture_);
    if (renderer_ != nullptr) SDL_DestroyRenderer(renderer_);
    if (window_ != nullptr) SDL_DestroyWindow(window_);
  }

  Player(const Player&) = delete;
  Player& operator=(const Player&) = delete;

  // Opens the window at the picture's own size times the scale. The picture can be
  // either height, so the texture is as tall as the taller one and each frame draws
  // the rows it has.
  [[nodiscard]] bool open() {
    if (!SDL_CreateWindowAndRenderer(title_.c_str(), static_cast<int>(kWidth * scale_),
                                     static_cast<int>(kShortHeight * scale_), 0, &window_,
                                     &renderer_)) {
      std::cerr << "cannot open a window: " << SDL_GetError() << "\n";
      return false;
    }
    texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING,
                                 static_cast<int>(kWidth), static_cast<int>(kTallHeight));
    if (texture_ == nullptr) {
      std::cerr << "cannot make a texture: " << SDL_GetError() << "\n";
      return false;
    }
    SDL_SetTextureScaleMode(texture_, SDL_SCALEMODE_NEAREST);
    started_ = SDL_GetTicksNS();
    lastFrame_ = started_;
    resumed_ = started_;
    return true;
  }

  // Records what the run draws and what each frame costs, into `directory` under
  // `stem`. Without it the run is the window alone. A file holds one shape of
  // picture, and the picture can be either height, so the recording is opened on the
  // first frame — the one that says which height this run is.
  void record(const std::filesystem::path& directory, const std::string& stem) {
    recordAt_ = directory / (stem + ".avi");
    table_.open(directory / (stem + ".csv"));
    if (table_) {
      table_ << "frame,wall_ns,emulated_ns,master,dots,milli_fps,mean_milli_fps\n";
    } else {
      std::cerr << "cannot write the table; the run goes on without one\n";
    }
  }

  [[nodiscard]] bool closed() const noexcept { return closed_; }
  [[nodiscard]] std::uint64_t frames() const noexcept { return frames_; }
  [[nodiscard]] std::uint64_t meanMilliFps() const {
    return milliFps(lastFrame_ - started_, frames_);
  }

  void frame(const VideoFrame& picture) override {
    const std::uint64_t emulated = SDL_GetTicksNS() - resumed_;
    ++frames_;

    show(picture);
    if (!recordAt_.empty() && !recording_) {
      recording_ = std::make_unique<snaggletooth::video::AviRecording>(
          recordAt_, picture.width, picture.height,
          snaggletooth::video::FrameRate{.rate = kNtscMaster, .scale = kNtscFrameElevenths});
      if (!recording_->open()) {
        std::cerr << "cannot write the recording; the run goes on without one\n";
        recordAt_.clear();
        recording_.reset();
      }
    }
    if (recording_) recording_->add(picture);
    presentPads();  // the frame beginning now takes what the script holds for it
    pump();

    // The console's own rate: each frame is held until its share of the run has
    // passed, so a machine that emulates faster than the console runs is watchable.
    const std::uint64_t deadline = started_ + frameDueAt(frames_);
    const std::uint64_t before = SDL_GetTicksNS();
    if (before < deadline) SDL_DelayNS(deadline - before);

    const std::uint64_t now = SDL_GetTicksNS();
    if (table_) {
      table_ << frames_ << ',' << (now - lastFrame_) << ',' << emulated << ','
             << machine_.state().master << ',' << (picture.width * picture.height) << ','
             << milliFps(now - lastFrame_, 1u) << ',' << milliFps(now - started_, frames_) << '\n';
    }
    lastFrame_ = now;
    resumed_ = now;
    if ((frames_ % kTitleFrames) == 0u) {
      const std::string text = title_ + " — " + rateText(milliFps(now - started_, frames_)) + " fps";
      SDL_SetWindowTitle(window_, text.c_str());
    }
  }

  void finish() {
    if (recording_) recording_->finish();
    if (table_) table_.close();
  }

 private:
  static constexpr unsigned kWidth = 256u;
  static constexpr unsigned kShortHeight = 224u;
  static constexpr unsigned kTallHeight = 239u;
  static constexpr std::uint64_t kTitleFrames = 30u;

  void show(const VideoFrame& picture) {
    const SDL_Rect rows{0, 0, static_cast<int>(picture.width), static_cast<int>(picture.height)};
    SDL_UpdateTexture(texture_, &rows, picture.pixels.data(),
                      static_cast<int>(picture.width * 4u));
    const SDL_FRect source{0.0f, 0.0f, static_cast<float>(picture.width),
                           static_cast<float>(picture.height)};
    SDL_RenderClear(renderer_);
    SDL_RenderTexture(renderer_, texture_, &source, nullptr);
    SDL_RenderPresent(renderer_);
  }

  void presentPads() {
    const std::uint32_t at = static_cast<std::uint32_t>(frames_);
    machine_.setJoypad(snaggletooth::JoypadPort::One,
                       script_.padAt(snaggletooth::JoypadPort::One, at));
    machine_.setJoypad(snaggletooth::JoypadPort::Two,
                       script_.padAt(snaggletooth::JoypadPort::Two, at));
  }

  void pump() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
        closed_ = true;
      }
    }
  }

  Snes& machine_;
  const snaggletooth::disasm::InputScript& script_;
  unsigned scale_;
  std::string title_;
  SDL_Window* window_ = nullptr;
  SDL_Renderer* renderer_ = nullptr;
  SDL_Texture* texture_ = nullptr;
  std::filesystem::path recordAt_;  // where the recording goes, once its height is known
  std::unique_ptr<snaggletooth::video::AviRecording> recording_;
  std::ofstream table_;
  std::uint64_t frames_ = 0;
  std::uint64_t started_ = 0;
  std::uint64_t lastFrame_ = 0;
  std::uint64_t resumed_ = 0;
  bool closed_ = false;
};

}  // namespace

int main(int argc, char** argv) {
  std::string imagePath;
  std::string outDir;
  std::string inputPath;
  std::string inputDir;
  std::uint64_t seconds = 0;
  unsigned scale = 3u;
  bool quiet = false;
  bool mute = false;

  for (int at = 1; at < argc; ++at) {
    const std::string arg = argv[at];
    auto next = [&](const char* name) -> std::string {
      if (at + 1 >= argc) {
        std::cerr << name << " needs a value\n";
        usage(argv[0]);
      }
      return argv[++at];
    };
    if (arg == "--out") {
      outDir = next("--out");
    } else if (arg == "--input") {
      inputPath = next("--input");
    } else if (arg == "--input-dir") {
      inputDir = next("--input-dir");
    } else if (arg == "--seconds") {
      try {
        seconds = std::stoull(next("--seconds"));
      } catch (const std::exception&) {
        std::cerr << "--seconds needs a number\n";
        usage(argv[0]);
      }
    } else if (arg == "--scale") {
      try {
        scale = static_cast<unsigned>(std::stoul(next("--scale")));
      } catch (const std::exception&) {
        std::cerr << "--scale needs a number\n";
        usage(argv[0]);
      }
    } else if (arg == "--mute") {
      mute = true;
    } else if (arg == "--quiet") {
      quiet = true;
    } else if (imagePath.empty()) {
      imagePath = arg;
    } else {
      usage(argv[0]);
    }
  }
  if (imagePath.empty()) usage(argv[0]);
  if (scale == 0u) scale = 1u;
  if (!inputPath.empty() && !inputDir.empty()) {
    std::cerr << "--input and --input-dir both name a recorded run; give one\n";
    return 2;
  }

  if (!inputDir.empty()) {
    const std::filesystem::path found = snaggletooth::disasm::scriptFor(inputDir, imagePath);
    if (std::filesystem::is_regular_file(found)) {
      inputPath = found.string();
      if (!quiet) std::cerr << "replaying " << found.string() << "\n";
    } else if (!quiet) {
      std::cerr << "no recorded run at " << found.string()
                << " and no default.snaginput beside it; the ports stay empty\n";
    }
  }

  snaggletooth::disasm::InputScript script;
  if (!inputPath.empty()) {
    std::string text;
    if (!readFile(inputPath, text)) {
      std::cerr << "cannot open " << inputPath << "\n";
      return 1;
    }
    std::string error;
    const std::optional<snaggletooth::disasm::InputScript> parsed =
        snaggletooth::disasm::parseInputScript(text, error);
    if (!parsed) {
      std::cerr << inputPath << ": " << error << "\n";
      return 1;
    }
    script = *parsed;
  }

  std::string bytes;
  if (!readFile(imagePath, bytes)) {
    std::cerr << "cannot open " << imagePath << "\n";
    return 1;
  }
  std::vector<std::uint8_t> rom(bytes.begin(), bytes.end());
  if (const std::optional<snaggletooth::CopierHeader> copier = snaggletooth::readCopierHeader(rom)) {
    rom.erase(rom.begin(),
              rom.begin() + static_cast<std::ptrdiff_t>(snaggletooth::kCopierHeaderBytes));
    if (!quiet) {
      std::cerr << "dropped " << snaggletooth::describeCopierHeader(*copier, rom.size()) << "\n";
    }
  }

  SDL_SetMainReady();
  // The audio subsystem is asked for only where the sound is wanted, so a muted run
  // opens no device at all.
  const SDL_InitFlags subsystems = SDL_INIT_VIDEO | (mute ? 0u : SDL_INIT_AUDIO);
  if (!SDL_Init(subsystems)) {
    std::cerr << "cannot start SDL: " << SDL_GetError() << "\n";
    return 1;
  }
  // SDL is shut down by this, and it is declared before everything that holds
  // something SDL gave out — the window, its renderer and texture, the audio
  // stream — so those are all destroyed first, on every path out of main. An audio
  // stream destroyed after the shutdown reaches a device the shutdown has already
  // freed, which ends the run in a segmentation fault however it was closed.
  const struct Shutdown {
    ~Shutdown() { SDL_Quit(); }
  } shutdown;

  Snes machine(snaggletooth::SnesConfig{.rom = rom});
  const std::string stem = std::filesystem::path(imagePath).stem().string();
  Player player(machine, script, scale, stem);
  if (!player.open()) return 1;
  if (!outDir.empty()) {
    std::filesystem::create_directories(outDir);
    player.record(outDir, stem);
  }
  machine.setFrameObserver(&player);
  machine.setJoypad(snaggletooth::JoypadPort::One,
                    script.padAt(snaggletooth::JoypadPort::One, 0u));
  machine.setJoypad(snaggletooth::JoypadPort::Two,
                    script.padAt(snaggletooth::JoypadPort::Two, 0u));

  Sound speakers;
  if (!mute) speakers.open();

  // The sound is kept only where it is going to be written out; a run nobody asked
  // to record hands each chunk to the speakers and lets it go.
  const bool recording = !outDir.empty();
  const std::uint64_t bound = seconds * kMasterPerSecond;
  std::vector<snaggletooth::StereoFrame> sound;
  while (!player.closed() && (bound == 0u || machine.state().master < bound)) {
    machine.run(kChunkMaster);
    const std::vector<snaggletooth::StereoFrame> produced = machine.takeFrames();
    speakers.put(produced);
    if (recording) sound.insert(sound.end(), produced.begin(), produced.end());
  }
  machine.setFrameObserver(nullptr);
  player.finish();

  if (recording) {
    const std::vector<std::uint8_t> wav =
        snaggletooth::spc::writeWav(sound, static_cast<std::uint32_t>(kSampleRate));
    const std::filesystem::path path = std::filesystem::path(outDir) / (stem + ".wav");
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(wav.data()), static_cast<std::streamsize>(wav.size()));
  }
  if (!quiet) {
    std::cerr << player.frames() << " frames at " << rateText(player.meanMilliFps()) << " fps\n";
  }
  return 0;
}
