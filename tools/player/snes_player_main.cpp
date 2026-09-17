// snes_player — runs a cartridge in a window, with its sound and your controller,
// and records the run when asked to.
//
//   snes_player <image> [--out <directory>] [--seconds N] [--scale N]
//               [--input <script> | --input-dir <directory>] [--config <file>]
//               [--region ntsc|pal] [--vsync on|off|auto] [--mute] [--quiet]
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
//
// The run is then held to the panel it is shown on, where the panel is within one part
// in a hundred of the console: the window waits for a refresh and each refresh carries
// one new frame, so no frame is shown twice and none is torn. A 60 Hz console on a
// 60.000 Hz panel runs a sixth of a per cent slow by the wall clock — every cycle is
// still emulated exactly, and the sound is opened a sixth of a per cent slower so it
// stays with the picture. A panel at another rate entirely leaves the run on the
// console's own, and --vsync names either arrangement outright. Which one a run took is
// the first thing it says.

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
#include "player/display.h"
#include "player/pad_config.h"
#include "player/pads.h"
#include "player/user_files.h"
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

// The DSP's own rate and shape, which the playback device is asked for directly so
// nothing resamples what the machine made.
constexpr int kSampleRate = 32000;
constexpr int kChannels = 2;
constexpr int kBytesPerSample = kChannels * static_cast<int>(sizeof(std::int16_t));

// The rate a frame's interval works out to, in thousandths of a frame a second, so a
// rate is reported exactly without leaving the integers.
[[nodiscard]] std::uint64_t milliFps(std::uint64_t nanos, std::uint64_t frames) {
  if (nanos == 0u) return 0u;
  return frames * 1000u * player::kNanosPerSecond / nanos;
}

// A rate the tool holds as a ratio, in the same thousandths a measured one is
// reported in, so the rate a run is held to and the rate it achieves read alike.
[[nodiscard]] std::uint64_t milliFps(player::FrameRate rate) {
  if (!player::isRate(rate)) return 0u;
  return (rate.numerator * 1000u + rate.denominator / 2u) / rate.denominator;
}

[[nodiscard]] std::string rateText(std::uint64_t milli) {
  char text[32];
  std::snprintf(text, sizeof(text), "%llu.%03llu", static_cast<unsigned long long>(milli / 1000u),
                static_cast<unsigned long long>(milli % 1000u));
  return text;
}

// Where this tool keeps a person's files. The platform is asked — the one call
// that decides it, on every OS, with no path of ours written down anywhere — and
// the environment replaces the answer whole for anyone who wants their files
// somewhere else.
[[nodiscard]] std::optional<std::filesystem::path> userFilesRoot(std::string& error) {
  // The windowing library's own reader rather than the standard one: what the C
  // library offers here is deprecated on one of the platforms this builds for, and
  // this is a tool that already has a portable answer linked into it.
  if (const char* elsewhere = SDL_getenv("SNAGGLETOOTH_USER_FILES");
      elsewhere != nullptr && *elsewhere != '\0') {
    return std::filesystem::path(elsewhere);
  }
  // An empty organisation puts the directory at the top of the platform's per-user
  // data location rather than under a company of ours, which is what was asked for.
  char* const answered = SDL_GetPrefPath("", "Snaggletooth");
  if (answered == nullptr) {
    error = SDL_GetError();
    return std::nullopt;
  }
  std::filesystem::path root(answered);
  SDL_free(answered);
  return root;
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
               " [--region ntsc|pal] [--vsync on|off|auto] [--mute] [--quiet]\n       "
            << program << " --default-config\n       " << program << " --user-files\n";
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

  // Opens the device and starts it, telling it the rate the run delivers samples at
  // — the DSP's own where the run is held to the console, a shade under it where the
  // run is held to a slower panel. Says why it could not rather than failing the run.
  void open(int rate) {
    // How far the sound may run ahead of the speakers before a chunk is left out: a
    // quarter of a second, which is latency a person notices. The machine is paced to a
    // frame interval and the device consumes at its own crystal, so the two drift apart
    // slowly; dropping a chunk costs a tenth of a frame of sound and puts the queue back
    // where it belongs.
    bound_ = rate * kBytesPerSample / 4;
    const SDL_AudioSpec spec{.format = SDL_AUDIO_S16, .channels = kChannels, .freq = rate};
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
    if (SDL_GetAudioStreamQueued(stream_) > bound_) return;
    SDL_PutAudioStreamData(stream_, frames.data(),
                           static_cast<int>(frames.size() * sizeof(snaggletooth::StereoFrame)));
  }

 private:
  SDL_AudioStream* stream_ = nullptr;
  int bound_ = 0;
};

// The window, the recording and the table, all fed by the frames the machine
// finishes. It is the machine's frame observer, so every one of them arrives here as
// the beam wraps, and the run is held to the console's rate before the next begins.
class Player final : public snaggletooth::FrameObserver {
 public:
  Player(Snes& machine, snaggletooth::Region region, const snaggletooth::disasm::InputScript& script,
         bool scripted, Devices& devices, unsigned scale, player::Vsync vsync,
         const std::string& title)
      : machine_(machine),
        deadline_(region),
        clock_(snaggletooth::consoleClock(region)),
        script_(script),
        scripted_(scripted),
        devices_(devices),
        scale_(scale),
        vsync_(vsync),
        pacing_{.rate = player::consoleFrameRate(clock_), .locked = false},
        title_(title) {}

  ~Player() override {
    if (texture_ != nullptr) SDL_DestroyTexture(texture_);
    if (renderer_ != nullptr) SDL_DestroyRenderer(renderer_);
    if (window_ != nullptr) SDL_DestroyWindow(window_);
  }

  Player(const Player&) = delete;
  Player& operator=(const Player&) = delete;

  // Opens the window at the picture's own size times the scale. The picture can be
  // either height and either width, so the texture is as large as the largest and
  // each frame draws the part it has, stretched to the same window: a frame drawn
  // in half-pixels fills it with each half-pixel half a scaled pixel wide.
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
                                 static_cast<int>(kTextureWidth), static_cast<int>(kTallHeight));
    if (texture_ == nullptr) {
      std::cerr << "cannot make a texture: " << SDL_GetError() << "\n";
      return false;
    }
    SDL_SetTextureScaleMode(texture_, SDL_SCALEMODE_NEAREST);
    holdToPanel();
    started_ = SDL_GetTicksNS();
    lastFrame_ = started_;
    resumed_ = started_;
    firstFrameEnd_ = started_;
    titleAt_ = started_;
    return true;
  }

  // Records what the run draws and what each frame costs, into `directory` under
  // `stem`. Without it the run is the window alone. The recording is opened on the
  // first frame, at that frame's shape; a run whose picture changes shape is laid out
  // at its largest as the recording closes.
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

  // Which rate the run is held to, and whether that rate is the panel's.
  [[nodiscard]] player::Pacing pacing() const noexcept { return pacing_; }

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

    // Each frame is held until its share of the run has passed, so a machine that
    // emulates faster than the console runs is watchable. The share is the panel's
    // where the run is held to one and the console's where it is not, and it bounds the
    // rate whether or not the present waits — a panel that will not wait, or a
    // compositor that hands back a present without one, then costs the run its phase
    // rather than its speed.
    const std::uint64_t due = started_ + owedAt(frames_);
    const std::uint64_t before = SDL_GetTicksNS();
    if (before < due) {
      SDL_DelayNS(due - before);
    } else if (before - due > kForgivenDebt * interval()) {
      // The run lost real time — the window went away, the machine was descheduled —
      // and it is not getting it back. The origin moves forward by what was lost, so
      // the frames after this one are owed from here. Without that, every deadline is
      // still counted from an origin the run cannot reach any more, every one of them
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
  static constexpr unsigned kTextureWidth = 512u;
  static constexpr unsigned kShortHeight = 224u;
  static constexpr unsigned kTallHeight = 239u;
  static constexpr std::uint64_t kTitleFrames = 30u;
  // How far behind the run may fall before its lateness is written off rather than
  // chased. Under this a single short frame settles it, which keeps the run's own
  // average rate honest; over it, the time is gone and pretending otherwise is what
  // makes a moment's interruption into seconds of the game at the wrong speed.
  static constexpr std::uint64_t kForgivenDebt = 2u;

  // What the run is held to, decided once the window is up and the panel it opened on
  // can be asked what it runs at. A panel within a hundredth of the console's rate
  // carries one frame per refresh, so the run takes the panel's rate and the present is
  // asked to wait for one; any other panel, or one that reports no rate at all, leaves
  // the run on the console's own with the present handed straight back.
  void holdToPanel() {
    const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window_));
    player::FrameRate panel;
    if (mode != nullptr && mode->refresh_rate_numerator > 0 &&
        mode->refresh_rate_denominator > 0) {
      panel = player::FrameRate{
          .numerator = static_cast<std::uint64_t>(mode->refresh_rate_numerator),
          .denominator = static_cast<std::uint64_t>(mode->refresh_rate_denominator)};
    }
    pacing_ = player::paceRun(player::consoleFrameRate(clock_), panel, vsync_);
    if (!pacing_.locked) return;
    panel_.emplace(pacing_.rate);
    if (!SDL_SetRenderVSync(renderer_, 1)) {
      // The rate still holds — the frame interval is the panel's either way — and only
      // the phase within a refresh is lost, so the run goes on and says what it lost.
      std::cerr << "the picture will not wait for a refresh: " << SDL_GetError() << "\n";
    }
  }

  // When the Nth frame is owed, and what one frame's interval is: the panel's where the
  // run is held to one, the console's where it is not.
  [[nodiscard]] std::uint64_t owedAt(std::uint64_t frames) const {
    return panel_ ? panel_->owedAt(frames) : deadline_.owedAt(frames);
  }

  [[nodiscard]] std::uint64_t interval() const {
    return panel_ ? panel_->wholeNanos() : deadline_.wholeNanos();
  }

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
  player::Vsync vsync_;
  player::Pacing pacing_;                   // the rate the run is held to
  std::optional<player::FramePace> panel_;  // and its deadlines, where that is the panel's
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
  player::Vsync vsync = player::Vsync::Auto;
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
    } else if (arg == "--user-files") {
      // The one way to learn the path without knowing the platform's rule. No
      // image, no window, nothing opened.
      std::string error;
      const std::optional<std::filesystem::path> root = userFilesRoot(error);
      if (!root) {
        std::cerr << "the platform did not say where a user's files go: " << error << "\n";
        return 1;
      }
      std::cout << root->string() << "\n";
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
    } else if (arg == "--vsync") {
      const std::string choice = next("--vsync");
      if (choice == "on") {
        vsync = player::Vsync::On;
      } else if (choice == "off") {
        vsync = player::Vsync::Off;
      } else if (choice == "auto") {
        vsync = player::Vsync::Auto;
      } else {
        std::cerr << "--vsync is on, off or auto\n";
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
  // Where a person's own files are kept. A platform that will not say is not fatal:
  // the run goes ahead on the built-in configuration and keeps no save, which is
  // said plainly rather than discovered when a save does not appear.
  std::string filesError;
  const std::optional<std::filesystem::path> filesRoot = userFilesRoot(filesError);
  if (!filesRoot && !quiet) {
    std::cerr << "no user files: " << filesError
              << " — the run keeps nothing and takes the built-in configuration\n";
  }

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
    // A mapping of a person's own, kept where their files go, is what the tool runs
    // with when it is handed no --config: the flag first, then that file, then the
    // one built in.
    const std::optional<player::UserFiles> files =
        filesRoot ? std::optional<player::UserFiles>(player::UserFiles(*filesRoot)) : std::nullopt;
    configPath = player::chooseConfigPath(configPath, files ? &*files : nullptr);
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

  // A cartridge with a battery keeps what it writes. The file goes in with the rest
  // of a person's files, under the name other emulators read, and it is put into the
  // machine before the first instruction — after that the machine says when it
  // changed and this writes it.
  std::optional<player::CartridgeSave> save;
  if (filesRoot && !machine.state().sram.empty()) {
    save.emplace(player::UserFiles(*filesRoot), player::UserFiles::saveNameFor(imagePath));
    snaggletooth::SnesState state = machine.state();
    std::size_t found = 0;
    std::size_t declared = 0;
    switch (save->loadInto(state.sram, found, declared)) {
      case player::CartridgeSave::Load::Loaded:
        machine.restore(state);
        if (!quiet) std::cerr << "save: " << save->name() << "\n";
        break;
      case player::CartridgeSave::Load::NoFile:
        if (!quiet) std::cerr << "save: " << save->name() << ", new\n";
        break;
      case player::CartridgeSave::Load::WrongSize:
        // Someone else's save, or a broken one. The cartridge boots on its own
        // power-on RAM and this run keeps nothing, so nothing of theirs is lost.
        std::cerr << "save: " << save->name() << " holds " << found << " bytes and this cartridge "
                  << "keeps " << declared << "; it is left alone and this run keeps nothing\n";
        save.reset();
        break;
    }
  }
  if (save) machine.setSaveObserver(&*save);

  const std::string stem = std::filesystem::path(imagePath).stem().string();
  Player player(machine, region, script, scripted, devices, scale, vsync, stem);
  if (!player.open()) return 1;
  // Which of the two arrangements the run took, before anything else it does: a run
  // that feels wrong is asked this first, and nobody should have to guess at it.
  const snaggletooth::player::Pacing pacing = player.pacing();
  if (!quiet) {
    std::cerr << (pacing.locked ? "held to the display at " : "the console's own ")
              << rateText(milliFps(pacing.rate)) << " Hz\n";
  }
  if (!outDir.empty()) {
    std::filesystem::create_directories(outDir);
    player.record(outDir, stem);
  }
  machine.setFrameObserver(&player);
  player.beginRun();

  Sound speakers;
  // The device is told the rate the run delivers at rather than the rate the DSP makes:
  // a run held to a panel slower than the console makes its samples that fraction
  // slower too, and a device consuming the DSP's own rate would run dry every few
  // minutes. What is written to a file is untouched — a recording is the machine's
  // output at its own rate, whatever the panel showing it runs at.
  if (!mute) {
    speakers.open(snaggletooth::player::pacedSampleRate(
        kSampleRate, snaggletooth::player::consoleFrameRate(clock), pacing.rate));
  }

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

  // Whatever the last frames left in the save window, on its way out: the machine
  // reports at a frame's end and a window closes whenever a person closes it, so
  // the run settles the file itself rather than losing the moments after the last
  // report. It writes nothing when nothing moved.
  if (save) {
    machine.setSaveObserver(nullptr);
    save->changed(machine.state().sram);
    if (!save->error().empty()) {
      std::cerr << "save: " << save->error() << "\n";
    } else if (!quiet && save->wrote()) {
      std::cerr << "save: kept in " << save->name() << "\n";
    }
  }

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
