// snes_player — runs a cartridge in a window, with its sound and your controller,
// and records the run when asked to.
//
//   snes_player <image> [--out <directory>] [--seconds N] [--scale N]
//               [--input <script> | --input-dir <directory>] [--config <file>]
//               [--region ntsc|pal] [--vsync on|off|auto] [--unthrottled]
//               [--interlace weave|bob|off] [--apu-thread] [--ppu-thread] [--ppu-verify]
//               [--gpu-ppu] [--gpu-verify] [--mute] [--quiet]
//               [--disasm <directory>] [--disasm-buffer-memory <GB>] [--disasm-buffer-disk <GB>]
//               [--disasm-buffer-dir <directory>] [--disasm-threads <n>]
//               [--disasm-resume-at <percent>]
//   snes_player --default-config
//
// The window shows the picture the machine draws, frame by frame, at the console's
// own rate, with that rate in its title so what the run costs is visible while it
// runs, and the DSP's own 32 kHz stereo goes to the default playback device as the
// machine produces it. It closes when the window is closed or when --seconds of the
// master clock have been spent; nothing else stops it. A run started with --disasm
// closes when its disassembly is finished, as below. --mute leaves the sound where
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
// the first thing it says — after a warning, printed only by a Debug build, that the
// rates it reports are the build's rather than the emulator's.
//
// --unthrottled turns the pacing off: no frame waits for its deadline and vsync is
// off, so the machine runs as fast as the host allows. The window is still drawn no
// faster than its panel refreshes, because a present past that rate waits for the
// display on macOS, which would hold the run to it: each refresh shows the frame
// current at that moment, the frames between go by undrawn, and --out keeps every one.
// Its first line says so, and the mean rate is printed at exit as for every run.
//
// --apu-thread builds the machine with SnesConfig::apuThread on: the audio unit runs
// on a thread of the machine's own, beside the one that steps the console, for a host
// whose cores are each too slow to run the whole machine at speed. --ppu-thread builds
// it with SnesConfig::ppuThread on: the picture is drawn on a thread of the machine's
// own the same way. --ppu-thread is the one to turn on first; --apu-thread joins it
// only where it alone does not hold the console's frame rate. The picture and the
// sound are the same bytes either way; the work moves to another core, and the run's
// first lines say so.
//
// The machine draws a picture line only when something it reads has changed since
// the frame before, and takes it from that frame otherwise; the run's last line says
// how many lines it took and how many it drew. --ppu-verify builds the machine with
// SnesConfig::ppuVerify on: every line is drawn and compared with what the frame
// before would have stood in with, and the last line adds the count of disagreements,
// which is 0 on a machine that reuses exactly. It costs the reuse, so it is for
// proving a run and never for playing one.
//
// A cartridge that turns interlace on draws one field a frame, and the player weaves
// the two into the full-height picture the console's signal carried. --interlace names
// how: weave, the two fields interleaved for the sharp full-height picture the console
// drew, and the default; bob, each field on its own doubled to full height, no combing
// under motion; off, the field as the machine made it, half height. A run the cartridge
// did not interlace is untouched whatever the mode, and --out keeps whatever is shown.
//
// --gpu-ppu hands the machine a GPU device (SnesConfig::gpuPpu): the picture is drawn
// on the GPU by the core's own compute shader from each frame's timeline, every pixel
// the one the dot renderer draws, and the window shows the texture the shader left.
// The machine draws no pixel on the CPU then; the pads, the sound, the pacing, the
// recording and the rest of the run are as they are without it. The run's first lines
// say which shader format and driver the device took and how the window is presented.
// --gpu-verify turns it on and keeps the CPU drawing too, so every frame is drawn both
// ways and compared pixel for pixel, the last line saying how many differed; it is
// for proving a run, not for playing one. The player's device over SDL's GPU API is
// `sdl_gpu_device.h`; `docs/gpu-ppu.md` is the GPU PPU itself.
//
// --disasm <directory> disassembles the cartridge while it is played, into a tree
// there (`live_disasm.h`), and turns --apu-thread and --ppu-thread on. The thread
// that steps the console copies what the machine reports into a buffer; the
// disassembly's own threads work through it, and one more writes the tree, one
// tree after another, and runs snes_render and then snes_verify from the
// player's own directory over each. When the buffer reaches the ceilings the
// --disasm-buffer-* flags set, the game stops until it has drained to the
// --disasm-resume-at level (`pause.h`); the pause chord stops the game and
// resumes it by hand. A request to close — the window's, a signal's — starts the
// close, which plays the buffer to its end, writes the last tree and runs the two
// commands with the window still up; further requests are answered and end
// nothing (`close_requests.h`). Without --disasm none of it exists in the process.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/resource.h>
#include <unistd.h>
#endif

#include "SDL3/SDL.h"
#include "SDL3/SDL_main.h"  // SDL_SetMainReady, which the tool calls to keep its own main()
#include "player/close_requests.h"
#include "player/default_snagpad.h"
#include "player/display.h"
#include "player/interlace.h"
#include "player/live_disasm.h"
#include "player/pad_config.h"
#include "player/pads.h"
#include "player/pause.h"
#include "player/sdl_gpu_device.h"
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

// Whether this player was compiled without optimization. Every optimized build type
// defines NDEBUG and Debug does not, so its absence is how a Debug build is told
// apart. A Debug build runs the machine several times slower, and its frame rates say
// nothing about the emulator's own speed.
#ifdef NDEBUG
constexpr bool kDebugBuild = false;
#else
constexpr bool kDebugBuild = true;
#endif

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

// ---- the disassembly's commands ----------------------------------------------------

// A process the player started, put under the player's own thread's class: the
// lowest nice value on macOS and Linux, below-normal on Windows.
void lowerProcess(Sint64 pid) {
#if defined(_WIN32)
  const HANDLE process = OpenProcess(PROCESS_SET_INFORMATION, FALSE, static_cast<DWORD>(pid));
  if (process == nullptr) return;
  SetPriorityClass(process, BELOW_NORMAL_PRIORITY_CLASS);
  CloseHandle(process);
#else
  setpriority(PRIO_PROCESS, static_cast<id_t>(pid), 19);
#endif
}

// The disassembly's hook for its two commands: the command started with the
// player's own standard output and error, lowered when asked, and waited for.
// Nothing when it could not be started.
std::optional<int> runCommand(const std::vector<std::string>& arguments, bool lowered) {
  std::vector<const char*> argv;
  argv.reserve(arguments.size() + 1u);
  for (const std::string& argument : arguments) argv.push_back(argument.c_str());
  argv.push_back(nullptr);
  SDL_Process* const process = SDL_CreateProcess(argv.data(), false);
  if (process == nullptr) return std::nullopt;
  if (lowered) {
    const Sint64 pid = SDL_GetNumberProperty(SDL_GetProcessProperties(process), SDL_PROP_PROCESS_PID_NUMBER, 0);
    if (pid > 0) lowerProcess(pid);
  }
  int status = 0;
  const bool waited = SDL_WaitProcess(process, true, &status);
  SDL_DestroyProcess(process);
  if (!waited) return std::nullopt;
  return status;
}

// What ends a process the close refuses to end, as the close's answer names it.
std::string howToEndThePlayer() {
#if defined(_WIN32)
  return "Task Manager's End Task";
#else
  return "kill -9 " + std::to_string(getpid());
#endif
}

// The options, then what was wrong with the command line, last, where it is read.
[[noreturn]] void usage(const char* program, const std::string& why) {
  std::cerr
      << "usage: " << program << " <image> [options]\n"
         "\n"
         "  --out <directory>           record the run there, named after the game: a video of\n"
         "                              every frame, the sound, a timing table, and a script\n"
         "                              that replays it\n"
         "  --seconds N                 quit after N seconds of game time\n"
         "  --scale N                   how big the window is, in multiples of the picture\n"
         "                              (default 3)\n"
         "  --input <script>            play back a recorded run instead of reading the controls\n"
         "  --input-dir <directory>     the same, using the script in there named after the game\n"
         "                              (or default.snaginput if there isn't one)\n"
         "  --config <file>             use your own controller mapping (--default-config shows\n"
         "                              the format)\n"
         "  --region ntsc|pal           pick the console's region when the game doesn't say\n"
         "  --vsync on|off|auto         sync to the display, run at the console's own rate, or let\n"
         "                              the player choose based on your display (default auto)\n"
         "  --unthrottled               run as fast as the machine can go, with no pacing or vsync;\n"
         "                              the window shows the frame current at each refresh\n"
         "  --interlace weave|bob|off   how to show interlaced games (default weave)\n"
         "  --apu-thread                run the sound chip on its own thread; only where\n"
         "                              --ppu-thread alone cannot hold full speed\n"
         "  --ppu-thread                draw the picture on its own thread\n"
         "  --ppu-verify                draw every line and check it against the frame before;\n"
         "                              for proving a run, not for playing one\n"
         "  --gpu-ppu                   draw the picture on the GPU with the core's own shader\n"
         "  --gpu-verify                draw every frame on the GPU and on the CPU and compare\n"
         "                              them; for proving a run, not for playing one\n"
         "  --disasm <directory>        disassemble the game as you play it, into a tree there; turns\n"
         "                              --apu-thread and --ppu-thread on\n"
         "  --disasm-buffer-memory <GB> the most RAM the disassembly's buffer takes (default 1)\n"
         "  --disasm-buffer-disk <GB>   the most its files hold (default 10)\n"
         "  --disasm-buffer-dir <directory>\n"
         "                              the directory those files go in, used as given (default:\n"
         "                              buffer under the --disasm directory)\n"
         "  --disasm-threads <n>        the threads that check the game's instructions (default: two\n"
         "                              fewer than this computer's cores)\n"
         "  --disasm-resume-at <percent>\n"
         "                              when the buffer fills and the game stops, how far it drains\n"
         "                              before the game resumes (default 50)\n"
         "  --mute                      don't play the sound\n"
         "  --quiet                     don't print the run's status lines\n"
         "  --default-config            print the built-in controller mapping and exit\n"
         "  --user-files                print where the player keeps its files and exit\n"
         "\n"
      << program << ": " << why << "\n";
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
    const auto idFor = [](std::string_view name) -> std::optional<player::KeyId> {
      const SDL_Scancode code = SDL_GetScancodeFromName(std::string(name).c_str());
      if (code == SDL_SCANCODE_UNKNOWN) return std::nullopt;
      return static_cast<player::KeyId>(code);
    };
    const std::optional<player::KeyboardMap> keys = player::resolveKeyboard(config_, idFor, error);
    if (!keys) return false;
    const std::optional<player::HotkeyMap> hotkeys =
        player::resolveHotkeys(config_, *keys, player::thisKeyboard(), idFor, error);
    if (!hotkeys) return false;
    keyboard_ = *keys;
    hotkeys_ = *hotkeys;
    quiet_ = quiet;
    return true;
  }

  // Whether `key`, pressed with `held`, is a chord the configuration gives to the
  // console's reset button, or to the pause chord. Left and right of a modifier are
  // one, and the locks are no part of a chord.
  [[nodiscard]] bool isReset(player::KeyId key, SDL_Keymod held) const {
    return player::pressed(hotkeys_.reset, key, modifiersOf(held));
  }
  [[nodiscard]] bool isPause(player::KeyId key, SDL_Keymod held) const {
    return player::pressed(hotkeys_.pause, key, modifiersOf(held));
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

  [[nodiscard]] static std::uint8_t modifiersOf(SDL_Keymod held) {
    unsigned modifiers = 0;
    if ((held & SDL_KMOD_GUI) != 0) modifiers |= player::kModifierCmd;
    if ((held & SDL_KMOD_CTRL) != 0) modifiers |= player::kModifierCtrl;
    if ((held & SDL_KMOD_ALT) != 0) modifiers |= player::kModifierAlt;
    if ((held & SDL_KMOD_SHIFT) != 0) modifiers |= player::kModifierShift;
    return static_cast<std::uint8_t>(modifiers);
  }

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
  player::HotkeyMap hotkeys_;
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
// Under --gpu-ppu it is the machine's timeline observer instead, told of each frame
// once the device has drawn it, and the window shows the device's texture; under
// --gpu-verify it is both, and compares the two pictures of each frame.
class Player final : public snaggletooth::FrameObserver, public snaggletooth::PpuTimelineObserver {
 public:
  Player(Snes& machine, snaggletooth::Region region, const snaggletooth::disasm::InputScript& script,
         bool scripted, Devices& devices, unsigned scale, player::Vsync vsync, bool unthrottled,
         player::Interlace interlace, player::SdlGpuDevice* gpu, bool gpuVerify, const std::string& title)
      : machine_(machine),
        deadline_(region),
        clock_(snaggletooth::consoleClock(region)),
        script_(script),
        scripted_(scripted),
        devices_(devices),
        scale_(scale),
        vsync_(vsync),
        unthrottled_(unthrottled),
        weaver_(interlace),
        interlace_(interlace),
        gpu_(gpu),
        gpuVerify_(gpuVerify),
        pacing_{.rate = player::consoleFrameRate(clock_), .locked = false},
        title_(title) {}

  ~Player() override {
    if (texture_ != nullptr) SDL_DestroyTexture(texture_);
    if (renderer_ != nullptr) SDL_DestroyRenderer(renderer_);
    if (gpu_ != nullptr) gpu_->releaseWindow();
    if (window_ != nullptr) SDL_DestroyWindow(window_);
  }

  Player(const Player&) = delete;
  Player& operator=(const Player&) = delete;

  // Opens the window at a progressive frame's size times the scale; `show` resizes it to
  // each frame's own shape as frames arrive, so the first interlaced or hi-res frame grows
  // the window to fit. The texture is as large as the largest picture and each frame fills
  // the part it has; the window is always the frame's own pixels times the scale, so the
  // present scales by a whole number and never combs. Under --gpu-ppu the window is the
  // GPU device's, claimed for its presents, and there is no renderer or texture here.
  [[nodiscard]] bool open() {
    const player::WindowSize initial = player::windowSize(kWidth, kShortHeight, scale_);
    if (gpu_ != nullptr) {
      window_ = SDL_CreateWindow(title_.c_str(), static_cast<int>(initial.width), static_cast<int>(initial.height), 0);
      if (window_ == nullptr) {
        std::cerr << "cannot open a window: " << SDL_GetError() << "\n";
        return false;
      }
    } else if (!SDL_CreateWindowAndRenderer(title_.c_str(), static_cast<int>(initial.width),
                                            static_cast<int>(initial.height), 0, &window_, &renderer_)) {
      std::cerr << "cannot open a window: " << SDL_GetError() << "\n";
      return false;
    }
    windowSize_ = initial;
    // A window opened from a terminal does not take the keyboard on every platform,
    // and a run nobody can press a button on is not a run. Asking for it is one call
    // and costs nothing where it was already focused.
    SDL_RaiseWindow(window_);
    if (gpu_ == nullptr) {
      // Tall enough for a woven interlaced frame, which is twice a field's height; a
      // progressive frame uses the top rows and leaves the rest.
      texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING,
                                   static_cast<int>(kTextureWidth), static_cast<int>(kTextureHeight));
      if (texture_ == nullptr) {
        std::cerr << "cannot make a texture: " << SDL_GetError() << "\n";
        return false;
      }
      SDL_SetTextureScaleMode(texture_, SDL_SCALEMODE_NEAREST);
    }
    if (unthrottled_) {
      // Vsync off explicitly, since the SDL_RENDER_VSYNC hint can open a renderer with
      // it on; and the window presented no faster than the panel refreshes, because on
      // macOS the Metal layer hands out a drawable once per refresh whether or not the
      // present is asked to wait for one, so a present past that rate blocks until the
      // next drawable and the run is held to the display by the very wait it asked not
      // to make. The frames between presents are emulated, recorded and counted, and
      // not drawn (`unthrottledShowRate`).
      if (renderer_ != nullptr) SDL_SetRenderVSync(renderer_, 0);
      showEvery_ = player::FramePace(player::unthrottledShowRate(
                                         player::consoleFrameRate(clock_), panelRate()))
                       .wholeNanos();
    } else {
      holdToPanel();
    }
    if (gpu_ != nullptr) {
      std::string error;
      if (!gpu_->claim(window_, pacing_.locked && !unthrottled_, error)) {
        std::cerr << "the GPU device cannot present to the window: " << error << "\n";
        return false;
      }
    }
    started_ = SDL_GetTicksNS();
    lastFrame_ = started_;
    resumed_ = started_;
    firstFrameEnd_ = started_;
    titleAt_ = started_;
    return true;
  }

  // Records what the run draws and what each frame costs, into `directory` under
  // `stem`, and the run's buttons to `script`. Without it the run is the window alone.
  // The recording is opened on the first frame, at that frame's shape; a run whose
  // picture changes shape is laid out at its largest as the recording closes.
  void record(const std::filesystem::path& directory, const std::string& stem,
              const std::filesystem::path& script) {
    recordAt_ = directory / (stem + ".avi");
    scriptAt_ = script;
    table_.open(directory / (stem + ".csv"));
    if (table_) {
      table_ << "frame,wall_ns,emulated_ns,master,dots,milli_fps,mean_milli_fps,gpu_ns\n";
    } else {
      std::cerr << "cannot write the table; the run goes on without one\n";
    }
  }

  // Which rate the run is held to, and whether that rate is the panel's.
  [[nodiscard]] player::Pacing pacing() const noexcept { return pacing_; }

  [[nodiscard]] bool closed() const noexcept { return closed_; }

  // Whether the reset button was pressed since this last said so.
  [[nodiscard]] bool takeReset() noexcept { return std::exchange(resetAsked_, false); }
  // Whether the pause chord was pressed since this last said so.
  [[nodiscard]] bool takePause() noexcept { return std::exchange(pauseAsked_, false); }
  // The requests to close the window system made since this last said so: the
  // window's close button, Command-Q, the dock.
  [[nodiscard]] unsigned takeCloseAsks() noexcept { return std::exchange(closeAsks_, 0u); }

  // Asks the window system to draw the eye to the window until it has the focus.
  void attention() { SDL_FlashWindow(window_, SDL_FLASH_UNTIL_FOCUSED); }

  // Everything the title says after the game's name, asked for each time the
  // title is set, with what the title would say there by itself (the rate, or a
  // stop's phrase), the steady clock, and the characters the title has room for
  // after the name (`titleRoom`). None unless a run disassembles as it plays.
  void setTitleExtra(std::function<std::string(const std::string& lead, std::uint64_t nanos, std::size_t room)> extra) {
    titleExtra_ = std::move(extra);
  }

  // One pass of a stop or of the close, while the machine runs nothing: the events,
  // the last picture presented again where the window was exposed, and the title
  // set to `status` beside the game's name when it changed and half a second has
  // passed since it was last set. The first pass after `held` sets it at once.
  void stoppedPass(const std::string& status) {
    pump();
    if (std::exchange(exposed_, false)) {
      if (gpu_ != nullptr) {
        if (lastShownWidth_ != 0u) static_cast<void>(gpu_->present(lastShownWidth_, lastShownRows_));
      } else {
        SDL_RenderClear(renderer_);
        SDL_RenderTexture(renderer_, texture_, &lastSource_, nullptr);
        SDL_RenderPresent(renderer_);
      }
    }
    const std::uint64_t now = SDL_GetTicksNS();
    if (now - stoppedTitleAt_ >= player::kNanosPerSecond / 2u) {
      std::string text = title_;
      if (titleExtra_) {
        text += " — " + titleExtra_(status, now, titleRoom());
      } else if (!status.empty()) {
        text += " — " + status;
      }
      if (text != stoppedTitle_) SDL_SetWindowTitle(window_, text.c_str());
      stoppedTitle_ = text;
      stoppedTitleAt_ = now;
    }
  }

  // A stop of `nanos` is over: the pacing, the table's two times and the mean rate
  // are moved past it, so none of them carries it. A stop runs no cycle, so the
  // machine's own time is untouched.
  void held(std::uint64_t nanos) {
    started_ += nanos;
    lastFrame_ += nanos;
    resumed_ += nanos;
    titleAt_ += nanos;
    if (frames_ >= 1u) heldNanos_ += nanos;
    stoppedTitle_.clear();
    stoppedTitleAt_ = 0;
  }
  [[nodiscard]] std::uint64_t frames() const noexcept { return frames_; }
  // The picture lines over the run, as the machine counted them in each frame
  // report: taken from the frame before, drawn, and the drawn lines that disagreed
  // with the record under the verification mode.
  [[nodiscard]] std::uint64_t linesReused() const noexcept { return linesReused_; }
  [[nodiscard]] std::uint64_t linesDrawn() const noexcept { return linesDrawn_; }
  [[nodiscard]] std::uint64_t mismatches() const noexcept { return mismatches_; }
  // Under --gpu-verify: the frames compared, the frames whose pixels differed, the
  // frames whose shape differed, and the first pixel that differed as frame:line:x.
  [[nodiscard]] std::uint64_t gpuCompared() const noexcept { return gpuCompared_; }
  [[nodiscard]] std::uint64_t gpuDiffered() const noexcept { return gpuDiffered_; }
  [[nodiscard]] std::uint64_t gpuShapesDiffered() const noexcept { return gpuShapesDiffered_; }
  [[nodiscard]] const std::string& gpuFirstDifference() const noexcept { return gpuFirst_; }
  // The rate the run actually held, measured from the end of the first frame: that
  // first one carries opening a window, a device and a cartridge, and a mean that
  // includes it describes the startup rather than the run. The game's stops are not
  // part of it.
  [[nodiscard]] std::uint64_t meanMilliFps() const {
    if (frames_ < 2u) return 0u;
    return milliFps(lastFrame_ - firstFrameEnd_ - heldNanos_, frames_ - 1u);
  }

  void frame(const VideoFrame& picture) override {
    if (gpu_ != nullptr) {
      // Under --gpu-verify the machine's own frame is kept until the timeline report of
      // the same cycle, where the device's picture is compared with it.
      verifyPixels_.assign(picture.pixels.begin(), picture.pixels.end());
      verifyWidth_ = picture.width;
      verifyHeight_ = picture.height;
      verifyField_ = picture.field;
      verifyAt_ = machine_.state().master;
      verifyPending_ = true;
      return;
    }
    const std::uint64_t emulated = SDL_GetTicksNS() - resumed_;
    // A cartridge running interlaced hands over one field a frame; the weaver composes
    // the two into the full-height picture the console's signal carried, the way the
    // run asked for. A progressive run, or --interlace off, passes straight through.
    const bool interlaced = machine_.state().ppu.interlace();
    const player::Shown shown = weaver_.present(picture.pixels.data(), picture.width,
                                                picture.height, picture.field, interlaced);
    // An unthrottled run draws the window only once the panel's interval has passed
    // since it was last drawn; every other run draws every frame.
    const std::uint64_t at = SDL_GetTicksNS();
    if (!unthrottled_ || at - lastShown_ >= showEvery_) {
      show(shown);
      lastShown_ = at;
    }
    endFrame(shown.pixels, shown.width, shown.height, picture.field, emulated, 0u);
  }

  // Under --gpu-ppu: the frame the device has just drawn, shown from its texture. Under
  // --gpu-verify the texture, drawn line for line, is first compared with the machine's
  // frame of the same cycle, and then drawn again under the run's field placement.
  void frame(const snaggletooth::PpuTimeline& timeline) override {
    const std::uint64_t emulated = SDL_GetTicksNS() - resumed_;
    if (gpuVerify_) verify(timeline);
    const snaggletooth::gpu::RowMap rows = gpu_->lastRows();
    const unsigned shownRows = snaggletooth::gpu::rowsUnder(rows, timeline.height);
    const player::WindowSize want = player::windowSize(timeline.width, shownRows, scale_);
    if (want != windowSize_) {
      SDL_SetWindowSize(window_, static_cast<int>(want.width), static_cast<int>(want.height));
      windowSize_ = want;
    }
    const std::uint64_t at = SDL_GetTicksNS();
    if (!unthrottled_ || at - lastShown_ >= showEvery_) {
      if (!gpu_->present(timeline.width, shownRows) && !presentRefused_) {
        std::cerr << "the GPU device cannot present: " << SDL_GetError() << "\n";
        presentRefused_ = true;
      }
      lastShown_ = at;
      lastShownWidth_ = timeline.width;
      lastShownRows_ = shownRows;
    }
    // A recording takes the shown picture back from the texture.
    const std::uint8_t* pixels = nullptr;
    if (!recordAt_.empty() && gpu_->readBack(timeline.width, shownRows, shownPixels_)) pixels = shownPixels_.data();
    endFrame(pixels, timeline.width, shownRows, timeline.field, emulated, gpu_->lastDrawNanos());
  }

  // The frame-end work every frame does, whichever side drew it: the line counts, the
  // recording, the wait for the frame's share of the run, the pads, the table and the
  // title. `pixels` is the shown picture, `width` by `height`, or null where there is
  // nothing to record.
  void endFrame(const std::uint8_t* pixels, unsigned width, unsigned height, std::uint8_t field,
                std::uint64_t emulated, std::uint64_t gpuNanos) {
    ++frames_;
    // The finished frame's line counts, valid inside the report.
    const snaggletooth::Snes::LineReuse lines = machine_.lineReuse();
    linesReused_ += lines.reused;
    linesDrawn_ += lines.drawn;
    mismatches_ += lines.mismatches;

    // The recording keeps every frame as the window shows it, woven and all, the ones an
    // unthrottled run leaves undrawn included: what --out keeps is the machine's output.
    const snaggletooth::VideoFrame kept{
        .pixels = std::span<const std::uint8_t>(pixels, pixels == nullptr ? 0u : static_cast<std::size_t>(width) * height * 4u),
        .width = width,
        .height = height,
        .field = field};
    if (!recordAt_.empty() && !recording_ && pixels != nullptr) {
      recording_ = std::make_unique<snaggletooth::video::AviRecording>(
          recordAt_, kept.width, kept.height,
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
    if (recording_ && pixels != nullptr) recording_->add(kept);

    // Each frame is held until its share of the run has passed, so a machine that
    // emulates faster than the console runs is watchable. The share is the panel's
    // where the run is held to one and the console's where it is not, and it bounds the
    // rate whether or not the present waits — a panel that will not wait, or a
    // compositor that hands back a present without one, then costs the run its phase
    // rather than its speed. An unthrottled run skips the wait, and the next frame
    // begins as soon as this one is done.
    if (!unthrottled_) {
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
    }

    // The controllers are read after that wait rather than before it. The report stands
    // at the start of the vertical blank of frame `frames_ - 1`, whose auto-read has not
    // begun, so what is read here is what that frame's auto-read latches the moment this
    // returns; a sample taken ahead of the wait would already be a frame's wait old by
    // then — a frame of delay bought for nothing.
    pump();  // the plugs, the pulls and the keys, before anything reads them
    presentPads(static_cast<std::uint32_t>(frames_ - 1u));  // the frame whose blank begins now takes what is held for it

    const std::uint64_t now = SDL_GetTicksNS();
    if (table_) {
      table_ << frames_ << ',' << (now - lastFrame_) << ',' << emulated << ','
             << machine_.state().master << ',' << (width * height) << ','
             << milliFps(now - lastFrame_, 1u) << ',' << milliFps(now - started_, frames_) << ','
             << gpuNanos << '\n';
    }
    lastFrame_ = now;
    resumed_ = now;
    if (frames_ == 1u) firstFrameEnd_ = now;
    // The rate over the frames since this was last written, rather than over the whole
    // run: a mean since the run began takes half a minute to approach the truth and
    // reads as though the machine were speeding up, which is not what a rate in a
    // window's title is read as.
    if ((frames_ % kTitleFrames) == 0u) {
      const std::string rate = rateText(milliFps(now - titleAt_, frames_ - titleFrames_)) + " fps";
      const std::string text = title_ + " — " + (titleExtra_ ? titleExtra_(rate, now, titleRoom()) : rate);
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
  // The texture holds the largest thing shown: a woven interlaced frame, twice the
  // taller field's height.
  static constexpr unsigned kTextureHeight = 2u * kTallHeight;
  static constexpr std::uint64_t kTitleFrames = 30u;

  // What a title bar keeps for the window's buttons and its margins, and the width
  // of a character of its text, in points. Both are estimates: no window system
  // says how wide a title's text is drawn.
#if defined(__APPLE__)
  static constexpr double kTitleButtonsPoints = 80.0;
  static constexpr double kTitleCharacterPoints = 7.0;
#elif defined(_WIN32)
  static constexpr double kTitleButtonsPoints = 180.0;
  static constexpr double kTitleCharacterPoints = 7.0;
#else
  static constexpr double kTitleButtonsPoints = 160.0;
  static constexpr double kTitleCharacterPoints = 8.0;
#endif

  // The characters the title has room for after the game's name, from the window's
  // width in points: its width in window coordinates, times the pixels a coordinate
  // spans, over the scale the display draws text at.
  [[nodiscard]] std::size_t titleRoom() const {
    int width = 0;
    int height = 0;
    if (!SDL_GetWindowSize(window_, &width, &height)) return 0;
    const float density = SDL_GetWindowPixelDensity(window_);
    const float scale = SDL_GetWindowDisplayScale(window_);
    const double points =
        density > 0.0f && scale > 0.0f ? static_cast<double>(width) * density / scale : static_cast<double>(width);
    return player::titleRoom(points - kTitleButtonsPoints, kTitleCharacterPoints, title_);
  }
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
    pacing_ = player::paceRun(player::consoleFrameRate(clock_), panelRate(), vsync_);
    if (!pacing_.locked) return;
    panel_.emplace(pacing_.rate);
    if (renderer_ == nullptr) return;  // the GPU device's swapchain waits for the refresh instead
    if (!SDL_SetRenderVSync(renderer_, 1)) {
      // The rate still holds — the frame interval is the panel's either way — and only
      // the phase within a refresh is lost, so the run goes on and says what it lost.
      std::cerr << "the picture will not wait for a refresh: " << SDL_GetError() << "\n";
    }
  }

  // The rate the panel the window is on reports, as the exact ratio; no rate at all
  // where the panel reports none.
  [[nodiscard]] player::FrameRate panelRate() const {
    const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window_));
    if (mode == nullptr || mode->refresh_rate_numerator <= 0 ||
        mode->refresh_rate_denominator <= 0) {
      return player::FrameRate{};
    }
    return player::FrameRate{
        .numerator = static_cast<std::uint64_t>(mode->refresh_rate_numerator),
        .denominator = static_cast<std::uint64_t>(mode->refresh_rate_denominator)};
  }

  // When the Nth frame is owed, and what one frame's interval is: the panel's where the
  // run is held to one, the console's where it is not.
  [[nodiscard]] std::uint64_t owedAt(std::uint64_t frames) const {
    return panel_ ? panel_->owedAt(frames) : deadline_.owedAt(frames);
  }

  [[nodiscard]] std::uint64_t interval() const {
    return panel_ ? panel_->wholeNanos() : deadline_.wholeNanos();
  }

  void show(const player::Shown& picture) {
    // The window is this frame's own pixels times the scale, so a nearest-neighbour present
    // makes each source pixel a scale-by-scale block. A frame of a new shape — interlace
    // turned on, hi-res entered — resizes the window to fit before it is shown.
    const player::WindowSize want = player::windowSize(picture.width, picture.height, scale_);
    if (want != windowSize_) {
      SDL_SetWindowSize(window_, static_cast<int>(want.width), static_cast<int>(want.height));
      windowSize_ = want;
    }
    const SDL_Rect rows{0, 0, static_cast<int>(picture.width), static_cast<int>(picture.height)};
    SDL_UpdateTexture(texture_, &rows, picture.pixels, static_cast<int>(picture.width * 4u));
    const SDL_FRect source{0.0f, 0.0f, static_cast<float>(picture.width),
                           static_cast<float>(picture.height)};
    SDL_RenderClear(renderer_);
    SDL_RenderTexture(renderer_, texture_, &source, nullptr);
    SDL_RenderPresent(renderer_);
    lastSource_ = source;
  }

  // Under --gpu-verify: the device's texture, drawn line for line, against the machine's
  // frame of the same cycle — a 512-wide frame column for column, a 256-wide one position
  // for position — then the texture drawn again under the run's field placement.
  void verify(const snaggletooth::PpuTimeline& timeline) {
    if (!verifyPending_ || verifyAt_ != machine_.state().master) return;
    verifyPending_ = false;
    ++gpuCompared_;
    if (verifyWidth_ != timeline.width || verifyHeight_ != timeline.height || verifyField_ != timeline.field) {
      ++gpuShapesDiffered_;
      if (gpuFirst_.empty()) gpuFirst_ = std::to_string(frames_) + ":shape";
    } else if (!gpu_->readBack(timeline.width, timeline.height, verifyGpu_)) {
      ++gpuDiffered_;
      if (gpuFirst_.empty()) gpuFirst_ = std::to_string(frames_) + ":read-back";
    } else {
      bool differed = false;
      for (unsigned row = timeline.firstLine - 1u; row < timeline.height && !differed; ++row) {
        for (unsigned x = 0u; x < timeline.width; ++x) {
          const std::size_t at = (static_cast<std::size_t>(row) * timeline.width + x) * 4u;
          if (!std::equal(verifyPixels_.begin() + static_cast<std::ptrdiff_t>(at),
                          verifyPixels_.begin() + static_cast<std::ptrdiff_t>(at + 4u),
                          verifyGpu_.begin() + static_cast<std::ptrdiff_t>(at))) {
            differed = true;
            if (gpuFirst_.empty()) {
              gpuFirst_ = std::to_string(frames_) + ":" + std::to_string(row + 1u) + ":" + std::to_string(x);
            }
            break;
          }
        }
      }
      if (differed) ++gpuDiffered_;
    }
    const bool interlaced = machine_.state().ppu.interlace();
    if (interlaced && interlace_ != player::Interlace::Off) {
      gpu_->redraw(snaggletooth::gpu::rowMapFor(placementOf(interlace_), true, timeline.field));
    }
  }

  [[nodiscard]] static snaggletooth::gpu::FieldPlacement placementOf(player::Interlace mode) noexcept {
    switch (mode) {
      case player::Interlace::Weave: return snaggletooth::gpu::FieldPlacement::Weave;
      case player::Interlace::Bob: return snaggletooth::gpu::FieldPlacement::Bob;
      default: return snaggletooth::gpu::FieldPlacement::Field;
    }
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
        ++closeAsks_;
      } else if (event.type == SDL_EVENT_WINDOW_EXPOSED) {
        exposed_ = true;
      } else if (event.type == SDL_EVENT_GAMEPAD_ADDED) {
        devices_.opened(event.gdevice.which);
      } else if (event.type == SDL_EVENT_GAMEPAD_REMOVED) {
        devices_.closed(event.gdevice.which);
      } else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
        const player::KeyId key = static_cast<player::KeyId>(event.key.scancode);
        if (devices_.isReset(key, event.key.mod)) pressReset();
        if (devices_.isPause(key, event.key.mod)) pauseAsked_ = true;
      }
    }
  }

  // The reset button. A run that is read from a script or written to one has no
  // word for it — a script is pads and frames — so there the button is left alone,
  // and the run says so the first time it is pressed.
  void pressReset() {
    if (!scripted_ && scriptAt_.empty()) {
      resetAsked_ = true;
      return;
    }
    if (!resetRefused_) {
      std::cerr << "reset: a recorded run has no word for the reset button; it is left alone\n";
    }
    resetRefused_ = true;
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
  bool unthrottled_;                        // no frame deadline and no vsync; the window drawn at the panel's rate
  std::uint64_t showEvery_ = 0;             // an unthrottled run's least interval between presents, ns
  std::uint64_t lastShown_ = 0;             // when the window was last drawn
  player::FieldWeaver weaver_;              // composes interlaced fields for the screen
  player::Interlace interlace_;             // how an interlaced run's fields are shown
  player::SdlGpuDevice* gpu_;               // the GPU PPU's device under --gpu-ppu, else null
  bool gpuVerify_;                          // every frame drawn both ways and compared
  std::vector<std::uint8_t> shownPixels_;   // the device's texture read back for the recording
  std::vector<std::uint8_t> verifyPixels_;  // the machine's frame kept for the comparison
  std::vector<std::uint8_t> verifyGpu_;     // the device's texture read back for it
  unsigned verifyWidth_ = 0;
  unsigned verifyHeight_ = 0;
  std::uint8_t verifyField_ = 0;
  std::uint64_t verifyAt_ = 0;              // the master cycle the kept frame was reported at
  bool verifyPending_ = false;
  std::uint64_t gpuCompared_ = 0;
  std::uint64_t gpuDiffered_ = 0;
  std::uint64_t gpuShapesDiffered_ = 0;
  std::string gpuFirst_;
  bool presentRefused_ = false;             // the device refused a present and the run said so
  player::Pacing pacing_;                   // the rate the run is held to
  std::optional<player::FramePace> panel_;  // and its deadlines, where that is the panel's
  std::string title_;
  SDL_Window* window_ = nullptr;
  SDL_Renderer* renderer_ = nullptr;
  SDL_Texture* texture_ = nullptr;
  player::WindowSize windowSize_{};  // the window's current pixels, resized when the frame's shape changes
  std::filesystem::path recordAt_;  // where the recording goes, once its height is known
  std::filesystem::path scriptAt_;  // where the run's own script goes
  std::unique_ptr<snaggletooth::video::AviRecording> recording_;
  std::ofstream table_;
  std::uint64_t frames_ = 0;
  std::uint64_t linesReused_ = 0;  // picture lines taken from the frame before, over the run
  std::uint64_t linesDrawn_ = 0;   // and drawn dot by dot
  std::uint64_t mismatches_ = 0;   // under the verification mode, drawn lines that disagreed with the record
  std::uint64_t started_ = 0;
  std::uint64_t lastFrame_ = 0;
  std::uint64_t resumed_ = 0;
  std::uint64_t firstFrameEnd_ = 0;  // where the run proper begins, past the startup frame
  std::uint64_t titleAt_ = 0;        // when the title last took a rate
  std::uint64_t titleFrames_ = 0;    // and how many frames had run by then
  bool resetAsked_ = false;    // the reset button was pressed since the machine last took it
  bool resetRefused_ = false;  // and a recorded run has already said it leaves it alone
  bool pauseAsked_ = false;    // the pause chord was pressed since the loop last took it
  bool closed_ = false;
  unsigned closeAsks_ = 0;     // the window system's requests to close, since the close's loop last took them
  bool exposed_ = false;       // the window was exposed since a stop's pass last presented the picture again
  SDL_FRect lastSource_{};     // the texture's part the window last showed
  unsigned lastShownWidth_ = 0;  // under --gpu-ppu, what the device last presented
  unsigned lastShownRows_ = 0;
  // Everything after the game's name in the title, under --disasm.
  std::function<std::string(const std::string&, std::uint64_t, std::size_t)> titleExtra_;
  std::string stoppedTitle_;        // the title last set during a stop or the close
  std::uint64_t stoppedTitleAt_ = 0;
  std::uint64_t heldNanos_ = 0;     // the game's stops, past the first frame
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
  bool unthrottled = false;
  bool apuThread = false;
  bool ppuThread = false;
  bool ppuVerify = false;
  bool gpuPpu = false;
  bool gpuVerify = false;
  player::Interlace interlace = player::Interlace::Weave;
  std::uint64_t seconds = 0;
  unsigned scale = 3u;
  bool quiet = false;
  bool mute = false;
  std::string disasmDir;
  player::LiveOptions liveOptions;
  // The flag of the five beside --disasm that was given first, which needs it.
  std::string needsDisasm;
  std::string needsDisasmThing;

  for (int at = 1; at < argc; ++at) {
    const std::string arg = argv[at];
    auto next = [&](const char* name) -> std::string {
      if (at + 1 >= argc) {
        usage(argv[0], std::string(name) + " needs a value");
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
        usage(argv[0], "--region is ntsc or pal, not `" + regionName + "`");
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
        usage(argv[0], "--vsync is on, off or auto, not `" + choice + "`");
      }
    } else if (arg == "--seconds") {
      try {
        seconds = std::stoull(next("--seconds"));
      } catch (const std::exception&) {
        usage(argv[0], "--seconds needs a number");
      }
    } else if (arg == "--scale") {
      try {
        scale = static_cast<unsigned>(std::stoul(next("--scale")));
      } catch (const std::exception&) {
        usage(argv[0], "--scale needs a number");
      }
    } else if (arg == "--unthrottled") {
      unthrottled = true;
    } else if (arg == "--apu-thread") {
      apuThread = true;
    } else if (arg == "--ppu-thread") {
      ppuThread = true;
    } else if (arg == "--ppu-verify") {
      ppuVerify = true;
    } else if (arg == "--gpu-ppu") {
      gpuPpu = true;
    } else if (arg == "--gpu-verify") {
      gpuPpu = true;
      gpuVerify = true;
    } else if (arg == "--interlace") {
      const std::optional<player::Interlace> mode = player::parseInterlace(next("--interlace"));
      if (!mode) {
        usage(argv[0], "--interlace is weave, bob or off");
      }
      interlace = *mode;
    } else if (arg == "--mute") {
      mute = true;
    } else if (arg == "--quiet") {
      quiet = true;
    } else if (arg == "--disasm") {
      disasmDir = next("--disasm");
    } else if (arg == "--disasm-buffer-memory" || arg == "--disasm-buffer-disk") {
      const bool memory = arg == "--disasm-buffer-memory";
      const std::optional<std::uint64_t> size = player::parseGigabytes(next(arg.c_str()));
      if (!size) {
        usage(argv[0], arg + " needs a size in GB, such as 1 or 0.5");
      }
      (memory ? liveOptions.memoryBytes : liveOptions.diskBytes) = *size;
      if (needsDisasm.empty()) {
        needsDisasm = arg;
        needsDisasmThing = memory ? "buffer's RAM ceiling" : "buffer's files' ceiling";
      }
    } else if (arg == "--disasm-buffer-dir") {
      liveOptions.bufferDirectory = next("--disasm-buffer-dir");
      if (needsDisasm.empty()) {
        needsDisasm = arg;
        needsDisasmThing = "buffer's directory";
      }
    } else if (arg == "--disasm-threads") {
      const std::string text = next("--disasm-threads");
      unsigned threads = 0;
      if (!text.empty() && text.size() <= 4u &&
          std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; })) {
        threads = static_cast<unsigned>(std::stoul(text));
      }
      if (threads == 0u) {
        usage(argv[0], "--disasm-threads needs a number, 1 or more");
      }
      liveOptions.threads = threads;
      if (needsDisasm.empty()) {
        needsDisasm = arg;
        needsDisasmThing = "threads";
      }
    } else if (arg == "--disasm-resume-at") {
      const std::string text = next("--disasm-resume-at");
      unsigned percent = 101;
      if (!text.empty() && text.size() <= 3u &&
          std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; })) {
        percent = static_cast<unsigned>(std::stoul(text));
      }
      if (percent > 100u) {
        usage(argv[0], "--disasm-resume-at is a percent from 0 to 100");
      }
      liveOptions.pause.resumePercent = percent;
      if (needsDisasm.empty()) {
        needsDisasm = arg;
        needsDisasmThing = "resume level";
      }
    } else if (imagePath.empty()) {
      imagePath = arg;
    } else if (!arg.empty() && arg.front() == '-') {
      usage(argv[0], "`" + arg + "` is not an option");
    } else {
      usage(argv[0], "`" + arg + "` is a second image after `" + imagePath +
                         "`; give one, and put a path with spaces in quotes");
    }
  }
  if (imagePath.empty()) usage(argv[0], "no image: give the cartridge to play");
  if (scale == 0u) scale = 1u;
  if (!needsDisasm.empty() && disasmDir.empty()) {
    std::cerr << needsDisasm << " sets the disassembly's " << needsDisasmThing
              << "; it has nothing to do without --disasm\n";
    return 2;
  }
  // A disassembly takes every thread it can use, the machine's two included.
  if (!disasmDir.empty()) {
    apuThread = true;
    ppuThread = true;
  }
  // The first thing a run says, and said under --quiet too: every rate the run
  // reports after it is the build's, not the emulator's.
  if constexpr (kDebugBuild) {
    std::cerr << "this player is a Debug build: the machine runs several times slower than a "
                 "Release build, so expect frame rates below the console's. Build with "
                 "--config Release (docs/build-and-consume.md).\n";
  }
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
  // Under --disasm the requests to close from outside the window are counted in
  // place of SDL's own handling, which would end the loop on the first; the close
  // answers each one.
  std::optional<player::CloseRequests> requests;
  if (!disasmDir.empty()) requests.emplace();

  Devices devices;
  {
    std::string error;
    if (!devices.configure(padConfig, quiet, error)) {
      std::cerr << (configPath.empty() ? "the built-in configuration" : configPath) << ": " << error
                << "\n";
      return 1;
    }
  }

  // The GPU PPU's device, opened before the machine that takes it and outliving the
  // machine and the player; under --gpu-verify the fields are placed line for line so
  // the texture compares with the machine's frame, and woven again for the window.
  player::SdlGpuDevice gpuDevice;
  if (gpuPpu) {
    std::string error;
    if (!gpuDevice.open(error)) {
      std::cerr << "no GPU device: " << error << "\n";
      return 1;
    }
    gpuDevice.setPlacement(gpuVerify ? snaggletooth::gpu::FieldPlacement::Field
                                     : (interlace == player::Interlace::Weave ? snaggletooth::gpu::FieldPlacement::Weave
                                        : interlace == player::Interlace::Bob ? snaggletooth::gpu::FieldPlacement::Bob
                                                                              : snaggletooth::gpu::FieldPlacement::Field));
  }

  snaggletooth::SnesConfig machineConfig{.rom = rom, .region = region};
  machineConfig.apuThread = apuThread;
  machineConfig.ppuThread = ppuThread;
  machineConfig.ppuVerify = ppuVerify;
  machineConfig.gpuPpu = gpuPpu ? &gpuDevice : nullptr;
  std::optional<Snes> built;
  try {
    built.emplace(machineConfig);
  } catch (const std::exception& e) {
    std::cerr << "the machine cannot be built: " << e.what() << "\n";
    return 1;
  }
  Snes& machine = *built;

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

  // The disassembly beside the game, bound to the machine at power-on, before the
  // window opens: a directory whose tree was written for another image refuses the
  // run here.
  const auto say = [quiet](const std::string& line) {
    if (!quiet) std::cerr << (line + "\n");
  };
  const auto always = [](const std::string& line) { std::cerr << (line + "\n"); };
  std::optional<player::LiveDisassembly> live;
  if (!disasmDir.empty()) {
    bool found = false;
    std::size_t entries = 0;
    std::size_t files = 0;
    std::string error;
    if (!player::LiveDisassembly::readable(rom, disasmDir, found, entries, files, error)) {
      std::cerr << error << "\n";
      return 1;
    }
    const char* const base = SDL_GetBasePath();
    liveOptions.commands = base != nullptr ? std::filesystem::path(base) : std::filesystem::path();
    liveOptions.image = imagePath;
    const std::filesystem::path buffer =
        liveOptions.bufferDirectory.empty() ? std::filesystem::path(disasmDir) / "buffer" : liveOptions.bufferDirectory;
    try {
      live.emplace(machine, rom, disasmDir, liveOptions,
                   player::LiveHooks{.say = say, .always = always, .run = runCommand, .step = {}});
    } catch (const std::exception& e) {
      std::cerr << "the disassembly cannot be bound: " << e.what() << "\n";
      return 1;
    }
    say("disassembling into " + disasmDir + " on " + std::to_string(live->status().buffer.threads) +
        " threads; the buffer takes up to " + player::sizeText(liveOptions.memoryBytes) + " of RAM and " +
        player::sizeText(liveOptions.diskBytes) + " of files under " + buffer.string());
    if (found) {
      say("read " + std::to_string(entries) + " entries and " + std::to_string(files) + " files from " +
          (std::filesystem::path(disasmDir) / "project.snagifest").string());
    }
    live->start();
  }

  const std::string stem = std::filesystem::path(imagePath).stem().string();
  Player player(machine, region, script, scripted, devices, scale, vsync, unthrottled, interlace,
                gpuPpu ? &gpuDevice : nullptr, gpuVerify, stem);
  if (!player.open()) return 1;
  // Under --disasm the title also carries the buffer's RAM and files, the close's
  // progress, and a spinner that turns while the work moves, a page at a time where
  // the window is too narrow for all of it.
  if (live) {
    player.setTitleExtra(
        [&live, title = player::LiveTitle{}](const std::string& lead, std::uint64_t now, std::size_t room) mutable {
          return title.text(lead, live->status(), now, room);
        });
  }
  // Which of the two arrangements the run took, before anything else it does: a run
  // that feels wrong is asked this first, and nobody should have to guess at it.
  const snaggletooth::player::Pacing pacing = player.pacing();
  if (!quiet) {
    if (unthrottled) {
      std::cerr << "unthrottled: no frame pacing, no vsync\n";
    } else {
      std::cerr << (pacing.locked ? "held to the display at " : "the console's own ")
                << rateText(milliFps(pacing.rate)) << " Hz\n";
    }
    if (apuThread) std::cerr << "the audio unit on its own thread\n";
    if (ppuThread) std::cerr << "the picture on its own thread\n";
    if (ppuVerify) std::cerr << "every line drawn and checked against the frame before\n";
    if (gpuPpu) {
      std::cerr << "the picture drawn on the GPU (" << gpuDevice.formatName() << ", " << gpuDevice.driverName()
                << "), presented " << gpuDevice.presentModeName() << "\n";
    }
    if (gpuVerify) std::cerr << "every frame drawn on the GPU and on the CPU and compared\n";
  }
  if (!outDir.empty()) {
    std::filesystem::create_directories(outDir);
    // The script takes the name every replay looks for, so the directory replays the run.
    player.record(outDir, stem, snaggletooth::disasm::scriptPathFor(outDir, imagePath));
  }
  // The frame observer draws the picture on the CPU, so a run on the GPU sets none
  // unless it is verifying; the timeline observer is how the GPU run is told its frames.
  if (!gpuPpu || gpuVerify) machine.setFrameObserver(&player);
  if (gpuPpu) machine.setPpuTimelineObserver(&player);
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
  // A reset starts the machine's master counter again, so what ran before each one
  // is kept here and the run's length is the two together.
  std::uint64_t beforeResets = 0;
  // A request to close from outside the window, counted under --disasm, is taken
  // once and kept, so the one that ends the loop is the one that starts the close.
  bool closeRequested = false;
  const auto closeAsked = [&] {
    if (!closeRequested && requests && requests->take() != 0u) closeRequested = true;
    return closeRequested;
  };
  bool pauseRefused = false;  // a run that disassembles nothing has said what the pause chord does
  while (!player.closed() && !closeAsked() && (bound == 0u || beforeResets + machine.state().master < bound)) {
    machine.run(chunk);
    const std::vector<snaggletooth::StereoFrame> produced = machine.takeFrames();
    speakers.put(produced);
    if (recording) sound.insert(sound.end(), produced.begin(), produced.end());
    // The game stops here, between two runs of the machine, as the reset is taken:
    // at the buffer's ceilings, or by the pause chord. A stop runs no cycle.
    if (live) {
      if (player.takePause()) live->togglePause();
      player::Stopped stop = live->afterRun(SDL_GetTicksNS());
      if (stop != player::Stopped::No) {
        const std::uint64_t began = SDL_GetTicksNS();
        if (stop == player::Stopped::AtLimit) player.attention();
        while (stop != player::Stopped::No && !player.closed() && !closeAsked()) {
          player.stoppedPass(live->statusText());
          if (player.takePause()) live->togglePause();
          SDL_DelayNS(20'000'000u);
          stop = live->whileStopped(SDL_GetTicksNS());
        }
        player.held(SDL_GetTicksNS() - began);
      }
    } else if (player.takePause() && !std::exchange(pauseRefused, true)) {
      std::cerr << "pause: this run disassembles nothing, so there is nothing to wait for; the game goes on\n";
    }
    // The button is taken here, between two runs of the machine: the events are
    // pumped from inside one, where the machine is part-way through a cycle.
    if (player.takeReset()) {
      beforeResets += machine.state().master;
      if (live) {
        live->reset();
      } else {
        machine.reset();
      }
    }
  }
  machine.setFrameObserver(nullptr);
  machine.setPpuTimelineObserver(nullptr);
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

  // The close of a run that disassembles as it plays: the buffer played to its
  // end, the last tree written and the two commands run, the window up and
  // answering every request to close until the work is done.
  bool treeWritten = true;
  if (live) {
    say("finishing the disassembly: " + player::sizeText(live->status().buffer.held) + " to play, then the last tree");
    live->beginClose();
    // The window's request that ended the loop is the one that started the close.
    static_cast<void>(player.takeCloseAsks());
    const std::string how = howToEndThePlayer();
    bool systemSaid = false;
    while (!live->closed()) {
      // The title says the close's progress itself (`player::LiveTitle`).
      player.stoppedPass({});
      unsigned asks = player.takeCloseAsks() + (requests ? requests->take() : 0u);
      for (; asks != 0u; --asks) live->closeAsked(how);
      if (requests && requests->systemIsClosing() && !std::exchange(systemSaid, true)) {
        always("Windows is closing the player and ends it in a few seconds whatever it does; the tree holds what "
               "was written before");
      }
      SDL_DelayNS(20'000'000u);
    }
    if (requests) requests->done();
    std::string error;
    if (!live->closingTree(error)) {
      always("the tree was not written: " + error);
      treeWritten = false;
    }
    // What the session cost the buffer, so a run that leaves RAM for the files shows.
    say("disassembly: " + player::sessionText(live->status()));
  }

  if (!quiet) {
    std::cerr << player.frames() << " frames at " << rateText(player.meanMilliFps()) << " fps\n";
    // How the picture lines were drawn over the run: taken from the frame before or
    // drawn, and under --ppu-verify how many drawn lines disagreed with the record.
    const std::uint64_t lines = player.linesReused() + player.linesDrawn();
    std::cerr << "lines: " << player.linesReused() << " taken from the frame before, " << player.linesDrawn()
              << " drawn";
    if (lines != 0u) std::cerr << " (" << (player.linesReused() * 1000u / lines + 5u) / 10u << " % taken)";
    if (ppuVerify) std::cerr << ", " << player.mismatches() << " mismatches";
    std::cerr << "\n";
    // What the GPU cost a frame, and under --gpu-verify what the comparison found.
    if (gpuPpu && gpuDevice.drawCount() != 0u) {
      std::cerr << "gpu: " << (gpuDevice.drawNanos() / gpuDevice.drawCount() + 500u) / 1000u
                << " µs a frame to upload and draw (" << (gpuDevice.drawNanosLeast() + 500u) / 1000u << "–"
                << (gpuDevice.drawNanosMost() + 500u) / 1000u << ")\n";
    }
    if (gpuVerify) {
      std::cerr << "gpu-verify: " << player.gpuCompared() << " frames, " << player.gpuDiffered() << " differed, "
                << player.gpuShapesDiffered() << " shapes differed, first at "
                << (player.gpuFirstDifference().empty() ? "-" : player.gpuFirstDifference()) << "\n";
    }
  }
  return treeWritten ? 0 : 1;
}
