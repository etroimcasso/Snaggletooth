#pragma once

// The input configuration: one plain-text file that IS the mapping.
//
// It is a Unix configuration file — `#` comments, `[section]` headers, `key = value`
// lines — and it says which keys and which places on a pad hold each of the
// machine's twelve buttons, and which port a controller goes to. The tool ships one
// as its default and a person hands it another with a flag; the form is
// `docs/pad-config.md`.
//
// Parsing is device-free, like everything else beside it. A key is kept as the name
// the file gave, because what a key is called belongs to whoever owns the keyboard;
// the application turns those names into its own ids when it loads the file. A pad
// source is resolved here, into this tool's own vocabulary.

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "player/pads.h"

namespace snaggletooth::player {

// A `[gamepad]` section, or a `[gamepad <family>]` one. A family's section carries
// only the keys it names and takes the rest from the plain section, so giving one
// family a different port is one line rather than a second whole mapping.
struct GamepadSection {
  std::optional<PortChoice> port;
  std::array<std::vector<PadSource>, kButtonCount> sources{};
  std::array<bool, kButtonCount> given{};
};

// The `[keyboard]` section. Its keys stay as names until the application resolves
// them.
struct KeyboardSection {
  std::optional<PortChoice> port;
  std::array<std::vector<std::string>, kButtonCount> keys{};
  std::array<bool, kButtonCount> given{};
};

// The keys held beside a hotkey's own: `cmd` is the Command key on a Mac and the
// Windows or Super key elsewhere, `alt` is Option on a Mac. Left and right are one.
//
// `cmdorctrl` is the modifier a platform's own shortcuts use — Command on a Mac,
// Control everywhere else — and is exactly one of the two on any machine, never
// both. It is a word of the file's only: resolving a chord turns it into the one
// the keyboard in front of the person has.
enum KeyModifier : std::uint8_t {
  kModifierCmd = 1u << 0,
  kModifierCtrl = 1u << 1,
  kModifierAlt = 1u << 2,
  kModifierShift = 1u << 3,
  kModifierCmdOrCtrl = 1u << 4,
};

// Whose keyboard a chord is resolved for, which is what `cmdorctrl` asks.
enum class KeyboardKind : std::uint8_t { Mac, Other };

// The keyboard of the platform this was compiled for.
[[nodiscard]] constexpr KeyboardKind thisKeyboard() noexcept {
#if defined(__APPLE__)
  return KeyboardKind::Mac;
#else
  return KeyboardKind::Other;
#endif
}

// One way to press a hotkey, as the file wrote it: `cmd+r` is the modifier and the
// key's name, which stays a name until the application resolves it.
struct ChordName {
  std::uint8_t modifiers = 0;
  std::string key;

  [[nodiscard]] bool operator==(const ChordName&) const = default;
};

// The `[hotkeys]` section: the chords that work the console itself rather than a
// pad in one of its ports.
struct HotkeySection {
  std::vector<ChordName> reset;  // the console's reset button
  bool resetGiven = false;
};

// A parsed configuration.
struct PadConfig {
  KeyboardSection keyboard;
  GamepadSection gamepad;
  std::map<PadFamily, GamepadSection> families;
  HotkeySection hotkeys;
  bool hasKeyboard = false;
  bool hasGamepad = false;
};

// Reads a configuration. Nothing, with `error` naming the line and what is wrong
// with it, when a line does not read: an unknown section or key, a key given twice,
// a button with no source, a phrase that names no place on a pad, a port that is
// not `auto`, `1` or `2`, two sections pinning one port, or a key under `[hotkeys]`
// that is not a hotkey. A file that does not read refuses the run before the window
// opens, the way a script that does not read does.
[[nodiscard]] std::optional<PadConfig> parsePadConfig(std::string_view text, std::string& error);

// Fills in whichever of the two plain sections `config` does not have from
// `defaults`, and any hotkey it does not name. These are the places the shipped
// default reaches into a person's own file, so a one-section file is worth writing:
// a file that says only what its author wanted changed still has a keyboard, still
// has a pad, and still has a reset button.
void fillFrom(PadConfig& config, const PadConfig& defaults);

// The mapping a pad gets: the plain `[gamepad]` section with this family's own
// section laid over it key by key, and every label word answered by the letters
// this pad prints — a letter it does not print binds nothing and raises the map's
// own flag, so the tool can say so once rather than leave a button quietly dead.
[[nodiscard]] GamepadMap resolve(const PadConfig& config, PadFamily family,
                                 const FaceLabels& labels);

// The keyboard's mapping, with each key name turned into the application's own id
// by `idFor` — the one thing about a keyboard this library cannot know. Nothing,
// with `error` naming the key, for a name the application does not know.
[[nodiscard]] std::optional<KeyboardMap> resolveKeyboard(
    const PadConfig& config,
    const std::function<std::optional<KeyId>(std::string_view)>& idFor,
    std::string& error);

// One way to press a hotkey, with its key as the application's own id.
struct Chord {
  std::uint8_t modifiers = 0;
  KeyId key = 0;

  [[nodiscard]] bool operator==(const Chord&) const = default;
};

// The hotkeys as the application's own key ids.
struct HotkeyMap {
  std::vector<Chord> reset;
};

// Whether pressing `key` with exactly `modifiers` held is one of `chords`. The
// modifiers match whole: `cmd+r` is not pressed by `r`, nor by `cmd+shift+r`.
[[nodiscard]] bool pressed(std::span<const Chord> chords, KeyId key, std::uint8_t modifiers) noexcept;

// The hotkeys resolved the way the keyboard's keys are, with `cmdorctrl` turned
// into Command for a Mac's keyboard and Control for any other. Nothing, with
// `error` naming the key, for a name the application does not know, or for a key
// with no modifier that `keyboard` already gives to a button — one press would do
// both.
[[nodiscard]] std::optional<HotkeyMap> resolveHotkeys(
    const PadConfig& config, const KeyboardMap& keyboard, KeyboardKind kind,
    const std::function<std::optional<KeyId>(std::string_view)>& idFor, std::string& error);

}  // namespace snaggletooth::player
