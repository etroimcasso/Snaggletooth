// snes_player — runs a cartridge in a window, with its sound and your controller,
// and records the run when asked to.
//
//   snes_player <image> [--out <directory>] [--seconds N] [--scale N]
//               [--input <script> | --input-dir <directory>] [--config <file>]
//               [--region ntsc|pal] [--mute] [--quiet]
//   snes_player --default-config
//
// The window shows the picture the machine draws, frame by frame, at the console's
// own rate, with that rate in its title so what the run costs is visible while it
// runs, and the DSP's own 32 kHz stereo goes to the default playback device as the
// machine produces it. It closes when the window is closed or when --seconds of the
// master clock have been spent; nothing else stops it. --mute leaves the sound where
// it is made, and a machine with no playback device runs silent and says so.
//
// The buttons come from the keyboard and from whatever gamepads are plugged in, each
// pad taken as it arrives and given the lowest port free. A pad is read by where its
// buttons SIT rather than by the letters printed on them, so every family plays the
// same way round; `--config` hands the tool a mapping of your own and
// `--default-config` prints the one it ships with. The whole form is
// `docs/pad-config.md`.
//
// A run driven by a recorded script instead is a run the keyboard and the pads leave
// alone: --input names a script and --input-dir a directory of them, from which the
// one named for the image is replayed, or the directory's default.snaginput when the
// image has none (`rom/input_script.h`). Two runs of one cartridge are then the same
// run, which is what makes two recordings worth setting beside each other.
//
// --out writes what the run produced into a directory, named after the image: an
// uncompressed AVI of every frame exactly as the machine drove it, a table of what
// each frame cost, the sound as a WAV, and the buttons as a script that replays the
// run. They come together — one run, one set of evidence — and without it the run
// keeps none of them, so nothing accumulates behind a run nobody asked to record.
//
// The machine runs at the rate the cartridge's own country byte asks for, so a 50 Hz
// cartridge boots and is paced at 50 Hz; --region overrides a cartridge that does not
// say.

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
#include <utility>
#include <vector>

#include "SDL3/SDL.h"
#include "SDL3/SDL_main.h"  // SDL_SetMainReady, which the tool calls to keep its own main()
#include "player/default_snagpad.h"
#include "player/pad_config.h"
#include "player/pads.h"
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
namespace player = snaggletooth::player;

constexpr std::uint64_t kNanosPerSecond = 1000000000ull;

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
               " [--input <script> | --input-dir <directory>] [--config <file>]"
               " [--region ntsc|pal] [--mute] [--quiet]\n       "
            << program << " --default-config\n";
  std::exit(2);
}

// ---- the devices -------------------------------------------------------------------
// Every call into the windowing library that reads an input is in this class. What it
// hands out is a controller per port, which is what the machine takes.

[[nodiscard]] player::PadFamily familyOf(SDL_GamepadType type) {
  switch (type) {
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
      return player::PadFamily::Nintendo;
    case SDL_GAMEPAD_TYPE_PS3:
    case SDL_GAMEPAD_TYPE_PS4:
    case SDL_GAMEPAD_TYPE_PS5:
      return player::PadFamily::Sony;
    case SDL_GAMEPAD_TYPE_XBOX360:
    case SDL_GAMEPAD_TYPE_XBOXONE:
      return player::PadFamily::Xbox;
    case SDL_GAMEPAD_TYPE_GAMECUBE:
      return player::PadFamily::GameCube;
    case SDL_GAMEPAD_TYPE_STANDARD:
      return player::PadFamily::Standard;
    default:
      return player::PadFamily::Unknown;
  }
}

// The four letters this pad prints at its four face positions. A pad that prints
// something else there — a cross, a circle — prints none of these, which is an
// ordinary answer: it is read by its positions instead.
[[nodiscard]] player::FaceLabels labelsOf(SDL_Gamepad* pad) {
  const auto letter = [pad](SDL_GamepadButton at) {
    switch (SDL_GetGamepadButtonLabel(pad, at)) {
      case SDL_GAMEPAD_BUTTON_LABEL_A: return player::FaceLabel::A;
      case SDL_GAMEPAD_BUTTON_LABEL_B: return player::FaceLabel::B;
      case SDL_GAMEPAD_BUTTON_LABEL_X: return player::FaceLabel::X;
      case SDL_GAMEPAD_BUTTON_LABEL_Y: return player::FaceLabel::Y;
      default: return player::FaceLabel::None;
    }
  };
  return player::FaceLabels{{letter(SDL_GAMEPAD_BUTTON_SOUTH), letter(SDL_GAMEPAD_BUTTON_EAST),
                             letter(SDL_GAMEPAD_BUTTON_WEST), letter(SDL_GAMEPAD_BUTTON_NORTH)}};
}

[[nodiscard]] player::PadInput readPad(SDL_Gamepad* pad) {
  constexpr std::pair<player::PadButton, SDL_GamepadButton> kButtons[] = {
      {player::PadButton::South, SDL_GAMEPAD_BUTTON_SOUTH},
      {player::PadButton::East, SDL_GAMEPAD_BUTTON_EAST},
      {player::PadButton::West, SDL_GAMEPAD_BUTTON_WEST},
      {player::PadButton::North, SDL_GAMEPAD_BUTTON_NORTH},
      {player::PadButton::DpadUp, SDL_GAMEPAD_BUTTON_DPAD_UP},
      {player::PadButton::DpadDown, SDL_GAMEPAD_BUTTON_DPAD_DOWN},
      {player::PadButton::DpadLeft, SDL_GAMEPAD_BUTTON_DPAD_LEFT},
      {player::PadButton::DpadRight, SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
      {player::PadButton::LeftShoulder, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},
      {player::PadButton::RightShoulder, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
      {player::PadButton::Start, SDL_GAMEPAD_BUTTON_START},
      {player::PadButton::Back, SDL_GAMEPAD_BUTTON_BACK},
      {player::PadButton::Guide, SDL_GAMEPAD_BUTTON_GUIDE},
      {player::PadButton::LeftStickClick, SDL_GAMEPAD_BUTTON_LEFT_STICK},
      {player::PadButton::RightStickClick, SDL_GAMEPAD_BUTTON_RIGHT_STICK},
  };
  constexpr std::pair<player::PadAxis, SDL_GamepadAxis> kAxes[] = {
      {player::PadAxis::LeftX, SDL_GAMEPAD_AXIS_LEFTX},
      {player::PadAxis::LeftY, SDL_GAMEPAD_AXIS_LEFTY},
      {player::PadAxis::RightX, SDL_GAMEPAD_AXIS_RIGHTX},
      {player::PadAxis::RightY, SDL_GAMEPAD_AXIS_RIGHTY},
      {player::PadAxis::LeftTrigger, SDL_GAMEPAD_AXIS_LEFT_TRIGGER},
      {player::PadAxis::RightTrigger, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER},
  };

  player::PadInput input;
  for (const auto& [ours, theirs] : kButtons) input.press(ours, SDL_GetGamepadButton(pad, theirs));
  for (const auto& [ours, theirs] : kAxes) {
    input.move(ours, static_cast<float>(SDL_GetGamepadAxis(pad, theirs)) / 32767.0f);
  }
  return input;
}

class Devices {
 public:
  ~Devices() {
    for (const Pad& pad : pads_) SDL_CloseGamepad(pad.handle);
  }

  Devices() = default;
  Devices(const Devices&) = delete;
  Devices& operator=(const Devices&) = delete;

  // Resolves the configuration against this keyboard. The key names are the only
  // thing about a keyboard the mapping cannot know, so they are looked up here.
  [[nodiscard]] bool configure(player::PadConfig config, bool quiet, std::string& error) {
    config_ = std::move(config);
    const std::optional<player::KeyboardMap> keys = player::resolveKeyboard(
        config_,
        [](std::string_view name) -> std::optional<player::KeyId> {
          const SDL_Scancode code = SDL_GetScancodeFromName(std::string(name).c_str());
          if (code == SDL_SCANCODE_UNKNOWN) return std::nullopt;
          return static_cast<player::KeyId>(code);
        },
        error);
    if (!keys) return false;
    keyboard_ = *keys;
    quiet_ = quiet;
    return true;
  }

  void opened(SDL_JoystickID id) {
    SDL_Gamepad* handle = SDL_OpenGamepad(id);
    if (handle == nullptr) {
      std::cerr << "a controller could not be opened: " << SDL_GetError() << "\n";
      return;
    }
    const player::PadFamily family = familyOf(SDL_GetGamepadType(handle));
    const player::GamepadMap map = player::resolve(config_, family, labelsOf(handle));
    const std::optional<snaggletooth::JoypadPort> port = ports_.plug(static_cast<int>(id), map.port);
    const char* name = SDL_GetGamepadName(handle);
    if (!quiet_) {
      std::cerr << (port ? (*port == snaggletooth::JoypadPort::One ? "port 1: " : "port 2: ")
                         : "no free port: ")
                << "a " << player::padFamilyName(family) << " pad"
                << (name != nullptr ? std::string(", ") + name : std::string()) << "\n";
      if (map.unprintedLabel) {
        std::cerr << "  its configuration names a letter this pad does not print;"
                     " nothing is held by it\n";
      }
    }
    if (!port) {
      SDL_CloseGamepad(handle);
      return;
    }
    pads_.push_back(Pad{.id = id, .handle = handle, .map = map, .port = *port});
  }

  void closed(SDL_JoystickID id) {
    for (std::size_t i = 0; i < pads_.size(); ++i) {
      if (pads_[i].id != id) continue;
      if (!quiet_) {
        std::cerr << "port " << (pads_[i].port == snaggletooth::JoypadPort::One ? 1 : 2)
                  << ": the pad was pulled out\n";
      }
      SDL_CloseGamepad(pads_[i].handle);
      pads_.erase(pads_.begin() + static_cast<std::ptrdiff_t>(i));
      break;
    }
    ports_.unplug(static_cast<int>(id));
  }

  // What each port holds now: the keyboard on its own port, every pad on the port it
  // took, and a port with neither left empty — which a program reads as an empty
  // socket rather than as a controller holding nothing.
  [[nodiscard]] std::optional<snaggletooth::Joypad> port(snaggletooth::JoypadPort which) {
    std::optional<snaggletooth::Joypad> held;
    if (keyboard_.port == which) held = keysDown();
    for (const Pad& pad : pads_) {
      if (pad.port != which) continue;
      const snaggletooth::Joypad from = player::padFromGamepad(readPad(pad.handle), pad.map);
      held = held ? player::either(*held, from) : from;
    }
    return held;
  }

 private:
  struct Pad {
    SDL_JoystickID id;
    SDL_Gamepad* handle;
    player::GamepadMap map;
    snaggletooth::JoypadPort port;
  };

  [[nodiscard]] snaggletooth::Joypad keysDown() const {
    int count = 0;
    const bool* state = SDL_GetKeyboardState(&count);
    std::vector<player::KeyId> down;
    for (int code = 0; code < count; ++code) {
      if (state[code]) down.push_back(static_cast<player::KeyId>(code));
    }
    return player::padFromKeyboard(down, keyboard_);
  }

  player::PadConfig config_;
  player::KeyboardMap keyboard_;
  player::PortAssignment ports_;
  std::vector<Pad> pads_;
  bool quiet_ = false;
};

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
  Player(Snes& machine, snaggletooth::Region region, const snaggletooth::disasm::InputScript& script,
         bool scripted, Devices& devices, unsigned scale, const std::string& title)
      : machine_(machine),
        deadline_(region),
        clock_(snaggletooth::consoleClock(region)),
        script_(script),
        scripted_(scripted),
        devices_(devices),
        scale_(scale),
        title_(title) {}

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
    // A window opened from a terminal does not take the keyboard on every platform,
    // and a run nobody can press a button on is not a run. Asking for it is one call
    // and costs nothing where it was already focused.
    SDL_RaiseWindow(window_);
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
    firstFrameEnd_ = started_;
    titleAt_ = started_;
    return true;
  }

  // Records what the run draws and what each frame costs, into `directory` under
  // `stem`. Without it the run is the window alone. A file holds one shape of
  // picture, and the picture can be either height, so the recording is opened on the
  // first frame — the one that says which height this run is.
  void record(const std::filesystem::path& directory, const std::string& stem) {
    recordAt_ = directory / (stem + ".avi");
    scriptAt_ = directory / (stem + ".snaginput");
    table_.open(directory / (stem + ".csv"));
    if (table_) {
      table_ << "frame,wall_ns,emulated_ns,master,dots,milli_fps,mean_milli_fps\n";
    } else {
      std::cerr << "cannot write the table; the run goes on without one\n";
    }
  }

  [[nodiscard]] bool closed() const noexcept { return closed_; }
  [[nodiscard]] std::uint64_t frames() const noexcept { return frames_; }
  // The rate the run actually held, measured from the end of the first frame: that
  // first one carries opening a window, a device and a cartridge, and a mean that
  // includes it describes the startup rather than the run.
  [[nodiscard]] std::uint64_t meanMilliFps() const {
    if (frames_ < 2u) return 0u;
    return milliFps(lastFrame_ - firstFrameEnd_, frames_ - 1u);
  }

  void frame(const VideoFrame& picture) override {
    const std::uint64_t emulated = SDL_GetTicksNS() - resumed_;
    ++frames_;

    show(picture);
    if (!recordAt_.empty() && !recording_) {
      recording_ = std::make_unique<snaggletooth::video::AviRecording>(
          recordAt_, picture.width, picture.height,
          snaggletooth::video::FrameRate{
              .rate = static_cast<std::uint32_t>(clock_.hertzNumerator),
              .scale = static_cast<std::uint32_t>(clock_.masterCyclesPerFrame *
                                                  clock_.hertzDenominator)});
      if (!recording_->open()) {
        std::cerr << "cannot write the recording; the run goes on without one\n";
        recordAt_.clear();
        recording_.reset();
      }
    }
    if (recording_) recording_->add(picture);

    // The console's own rate: each frame is held until its share of the run has
    // passed, so a machine that emulates faster than the console runs is watchable.
    const std::uint64_t due = started_ + deadline_.owedAt(frames_);
    const std::uint64_t before = SDL_GetTicksNS();
    if (before < due) {
      SDL_DelayNS(due - before);
    } else if (before - due > kForgivenDebt * deadline_.wholeNanos()) {
      // The run lost real time — the window went away, the machine was descheduled —
      // and it is not getting it back. The origin moves forward by what was lost, so
      // the frames after this one are owed from here. Without that, every deadline is
      // still counted from an origin the run can no longer reach, every one of them
      // has already gone by, and the machine runs flat out until it catches a schedule
      // that stopped being true — a second of lost time replayed as a second of the
      // game at several times its speed. A run that lost time stays late.
      started_ += before - due;
    }

    // The controllers are read after that wait rather than before it. The frame that
    // latches what is read begins the moment this returns, so a sample taken ahead of
    // the wait would already be a whole frame old by the time the machine could see
    // it — a frame of delay bought for nothing.
    pump();         // the plugs, the pulls and the keys, before anything reads them
    presentPads(static_cast<std::uint32_t>(frames_));  // the frame beginning now takes what is held for it

    const std::uint64_t now = SDL_GetTicksNS();
    if (table_) {
      table_ << frames_ << ',' << (now - lastFrame_) << ',' << emulated << ','
             << machine_.state().master << ',' << (picture.width * picture.height) << ','
             << milliFps(now - lastFrame_, 1u) << ',' << milliFps(now - started_, frames_) << '\n';
    }
    lastFrame_ = now;
    resumed_ = now;
    if (frames_ == 1u) firstFrameEnd_ = now;
    // The rate over the frames since this was last written, rather than over the whole
    // run: a mean since the run began takes half a minute to approach the truth and
    // reads as though the machine were speeding up, which is not what a rate in a
    // window's title is read as.
    if ((frames_ % kTitleFrames) == 0u) {
      const std::string text =
          title_ + " — " + rateText(milliFps(now - titleAt_, frames_ - titleFrames_)) + " fps";
      SDL_SetWindowTitle(window_, text.c_str());
      titleAt_ = now;
      titleFrames_ = frames_;
    }
  }

  // Frame zero: the pads present at the start arrive as events on the first pump, so
  // the run knows what is plugged in before the machine takes its first step — and the
  // recorded script says so, rather than opening a frame later than the run it records.
  void beginRun() {
    pump();
    presentPads(0u);
  }

  void finish() {
    if (recording_) recording_->finish();
    if (table_) table_.close();
    if (scriptAt_.empty()) return;
    std::ofstream out(scriptAt_, std::ios::binary);
    const std::string text = snaggletooth::disasm::writeInputScript(recorder_.script());
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
  }

 private:
  static constexpr unsigned kWidth = 256u;
  static constexpr unsigned kShortHeight = 224u;
  static constexpr unsigned kTallHeight = 239u;
  static constexpr std::uint64_t kTitleFrames = 30u;
  // How far behind the run may fall before its lateness is written off rather than
  // chased. Under this a single short frame settles it, which keeps the run's own
  // average rate honest; over it, the time is gone and pretending otherwise is what
  // makes a moment's interruption into seconds of the game at the wrong speed.
  static constexpr std::uint64_t kForgivenDebt = 2u;

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

  void presentPads(std::uint32_t at) {
    const std::optional<snaggletooth::Joypad> one =
        scripted_ ? script_.padAt(snaggletooth::JoypadPort::One, at)
                  : devices_.port(snaggletooth::JoypadPort::One);
    const std::optional<snaggletooth::Joypad> two =
        scripted_ ? script_.padAt(snaggletooth::JoypadPort::Two, at)
                  : devices_.port(snaggletooth::JoypadPort::Two);
    machine_.setJoypad(snaggletooth::JoypadPort::One, one);
    machine_.setJoypad(snaggletooth::JoypadPort::Two, two);
    if (!scriptAt_.empty()) recorder_.frame(at, one, two);
  }

  void pump() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
        closed_ = true;
      } else if (event.type == SDL_EVENT_GAMEPAD_ADDED) {
        devices_.opened(event.gdevice.which);
      } else if (event.type == SDL_EVENT_GAMEPAD_REMOVED) {
        devices_.closed(event.gdevice.which);
      }
    }
  }

  Snes& machine_;
  snaggletooth::FrameDeadline deadline_;
  snaggletooth::ConsoleClock clock_;
  const snaggletooth::disasm::InputScript& script_;
  bool scripted_;
  Devices& devices_;
  player::PadRecorder recorder_;
  unsigned scale_;
  std::string title_;
  SDL_Window* window_ = nullptr;
  SDL_Renderer* renderer_ = nullptr;
  SDL_Texture* texture_ = nullptr;
  std::filesystem::path recordAt_;  // where the recording goes, once its height is known
  std::filesystem::path scriptAt_;  // where the run's own script goes
  std::unique_ptr<snaggletooth::video::AviRecording> recording_;
  std::ofstream table_;
  std::uint64_t frames_ = 0;
  std::uint64_t started_ = 0;
  std::uint64_t lastFrame_ = 0;
  std::uint64_t resumed_ = 0;
  std::uint64_t firstFrameEnd_ = 0;  // where the run proper begins, past the startup frame
  std::uint64_t titleAt_ = 0;        // when the title last took a rate
  std::uint64_t titleFrames_ = 0;    // and how many frames had run by then
  bool closed_ = false;
};

}  // namespace

int main(int argc, char** argv) {
  std::string imagePath;
  std::string outDir;
  std::string inputPath;
  std::string inputDir;
  std::string configPath;
  std::string regionName;
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
    if (arg == "--default-config") {
      std::cout << player::kDefaultPadConfig;
      return 0;
    } else if (arg == "--out") {
      outDir = next("--out");
    } else if (arg == "--input") {
      inputPath = next("--input");
    } else if (arg == "--input-dir") {
      inputDir = next("--input-dir");
    } else if (arg == "--config") {
      configPath = next("--config");
    } else if (arg == "--region") {
      regionName = next("--region");
      if (regionName != "ntsc" && regionName != "pal") {
        std::cerr << "--region is ntsc or pal\n";
        usage(argv[0]);
      }
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
    } else if (!quiet) {
      std::cerr << "no recorded run at " << found.string()
                << " and no default.snaginput beside it; the keyboard and the pads drive it\n";
    }
  }

  snaggletooth::disasm::InputScript script;
  const bool scripted = !inputPath.empty();
  if (scripted) {
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
    if (!quiet) {
      std::cerr << "replaying " << inputPath
                << "; the keyboard and the gamepads are idle\n";
    }
  }

  // The configuration is read before the window opens, so one that does not read
  // refuses the run rather than being discovered after a device is up.
  player::PadConfig padConfig;
  {
    std::string defaults;
    std::string error;
    const std::optional<player::PadConfig> shipped =
        player::parsePadConfig(player::kDefaultPadConfig, error);
    if (!shipped) {
      std::cerr << "the built-in configuration does not read: " << error << "\n";
      return 1;
    }
    if (configPath.empty()) {
      padConfig = *shipped;
      if (!quiet && !scripted) std::cerr << "input: the built-in configuration\n";
    } else {
      std::string text;
      if (!readFile(configPath, text)) {
        std::cerr << "cannot open " << configPath << "\n";
        return 1;
      }
      const std::optional<player::PadConfig> theirs = player::parsePadConfig(text, error);
      if (!theirs) {
        std::cerr << configPath << ": " << error << "\n";
        return 1;
      }
      padConfig = *theirs;
      player::fillFrom(padConfig, *shipped);
      if (!quiet && !scripted) std::cerr << "input: " << configPath << "\n";
    }
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

  // The cartridge says which console it was made for, and a machine of the other
  // region is one its own code refuses to run on.
  snaggletooth::Region region = snaggletooth::Region::Ntsc;
  const char* why = "the default";
  if (const std::optional<snaggletooth::CartridgeHeader> header =
          snaggletooth::parseCartridgeHeader(rom)) {
    if (header->video == snaggletooth::VideoStandard::Pal) {
      region = snaggletooth::Region::Pal;
      why = "the cartridge";
    } else if (header->video == snaggletooth::VideoStandard::Ntsc) {
      why = "the cartridge";
    }
  }
  if (!regionName.empty()) {
    region = regionName == "pal" ? snaggletooth::Region::Pal : snaggletooth::Region::Ntsc;
    why = "--region";
  }
  const snaggletooth::ConsoleClock clock = snaggletooth::consoleClock(region);
  if (!quiet) {
    std::cerr << (region == snaggletooth::Region::Pal ? "a 50 Hz machine" : "a 60 Hz machine")
              << ", by " << why << "\n";
  }

  SDL_SetMainReady();
  // The audio subsystem is asked for only where the sound is wanted, so a muted run
  // opens no device at all; the gamepad subsystem only where a run is live, so a
  // replay touches no controller.
  const SDL_InitFlags subsystems = SDL_INIT_VIDEO | (mute ? 0u : SDL_INIT_AUDIO) |
                                   (scripted ? 0u : SDL_INIT_GAMEPAD);
  if (!SDL_Init(subsystems)) {
    std::cerr << "cannot start SDL: " << SDL_GetError() << "\n";
    return 1;
  }
  // SDL is shut down by this, and it is declared before everything that holds
  // something SDL gave out — the window, its renderer and texture, the audio
  // stream, the gamepads — so those are all destroyed first, on every path out of
  // main. An audio stream destroyed after the shutdown reaches a device the shutdown
  // has already freed, which ends the run in a segmentation fault however it was
  // closed.
  const struct Shutdown {
    ~Shutdown() { SDL_Quit(); }
  } shutdown;

  Devices devices;
  {
    std::string error;
    if (!devices.configure(padConfig, quiet, error)) {
      std::cerr << (configPath.empty() ? "the built-in configuration" : configPath) << ": " << error
                << "\n";
      return 1;
    }
  }

  Snes machine(snaggletooth::SnesConfig{.rom = rom, .region = region});
  const std::string stem = std::filesystem::path(imagePath).stem().string();
  Player player(machine, region, script, scripted, devices, scale, stem);
  if (!player.open()) return 1;
  if (!outDir.empty()) {
    std::filesystem::create_directories(outDir);
    player.record(outDir, stem);
  }
  machine.setFrameObserver(&player);
  player.beginRun();

  Sound speakers;
  if (!mute) speakers.open();

  // How much the machine is run between drains of its sound: a quarter of a frame,
  // so the queue stays short without the loop spinning.
  const std::uint64_t chunk = clock.masterCyclesPerFrame / 4u;
  const std::uint64_t masterPerSecond = clock.hertzNumerator / clock.hertzDenominator;

  // The sound is kept only where it is going to be written out; a run nobody asked
  // to record hands each chunk to the speakers and lets it go.
  const bool recording = !outDir.empty();
  const std::uint64_t bound = seconds * masterPerSecond;
  std::vector<snaggletooth::StereoFrame> sound;
  while (!player.closed() && (bound == 0u || machine.state().master < bound)) {
    machine.run(chunk);
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
