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
#include <functional>
#include <map>
#include <optional>
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

// A parsed configuration.
struct PadConfig {
  KeyboardSection keyboard;
  GamepadSection gamepad;
  std::map<PadFamily, GamepadSection> families;
  bool hasKeyboard = false;
  bool hasGamepad = false;
};

// Reads a configuration. Nothing, with `error` naming the line and what is wrong
// with it, when a line does not read: an unknown section or key, a key given twice,
// a button with no source, a phrase that names no place on a pad, a port that is
// not `auto`, `1` or `2`, two sections pinning one port, or a key under `[hotkeys]`,
// which this release defines none of. A file that does not read refuses the run
// before the window opens, the way a script that does not read does.
[[nodiscard]] std::optional<PadConfig> parsePadConfig(std::string_view text, std::string& error);

// Fills in whichever of the two plain sections `config` does not have from
// `defaults`. It is the one place the shipped default reaches into a person's own
// file, so a one-section file is worth writing: a file that says only what its
// author wanted changed still has a keyboard and still has a pad.
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

}  // namespace snaggletooth::player
