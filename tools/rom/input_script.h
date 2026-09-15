#pragma once

// A recorded run: which buttons are held on which port from which frame, as a
// text file a person writes or a tool exports. The disassembler replays one
// while it runs the cartridge, so the run exercises what a player would — a
// title screen left behind, a menu entered, a level begun — and the trace
// reaches what the boot alone never does. The grammar is `docs/input-script.md`.
//
// A script names frames from power-on, the first being frame 0. A port the
// script names has a pad from power-on, with nothing pressed until its first
// line; a port it never names has no pad, and reads as no controller.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "snaggletooth/snes/snes.h"

namespace snaggletooth::disasm {

// One line of a script: from `frame` on, `port` holds exactly the buttons in
// `pad` — or has no controller in it at all, which is what `plugged` false says.
// `pad` is nothing pressed on a line that says the port is empty.
struct InputEvent {
  std::uint32_t frame = 0;
  JoypadPort port = JoypadPort::One;
  bool plugged = true;
  Joypad pad;
};

// A parsed script: its events in frame order, each port at most once per frame.
struct InputScript {
  std::vector<InputEvent> events;

  // Whether the script names `port` anywhere. A port it names is a port it says
  // something about, which may be that nothing is plugged into it.
  [[nodiscard]] bool names(JoypadPort port) const noexcept;

  // What `port` holds at `frame`: the last line at or before it, and before the
  // first line, nothing pressed — with a controller exactly when that first line
  // has one. A port the script never names has no controller at any frame.
  [[nodiscard]] std::optional<Joypad> padAt(JoypadPort port, std::uint32_t frame) const noexcept;
};

// A script as text, in the one form this writes: a `frame <n> <port> <buttons>`
// line per event in event order, buttons in the order the pad shifts them out,
// `none` for a controller with nothing pressed and `unplugged` for a port with no
// controller, lower case, single spaces, a newline after every line, no comments.
//
// Two laws hold over it, and the suite pins both: parsing what this writes
// presents the same controller on every port at every frame as the script it was
// given, and writing that parse back is byte for byte the same text.
[[nodiscard]] std::string writeInputScript(const InputScript& script);

// Reads a script. Nothing, with `error` naming the line and what is wrong with
// it, when a line does not parse: a frame out of order, a port named twice on
// one frame, a button named twice, a word that is not a button.
[[nodiscard]] std::optional<InputScript> parseInputScript(std::string_view text, std::string& error);

// The run a directory holds for `image`: the file `scriptPathFor` names when it
// exists, otherwise the directory's `default.snaginput` when that exists — the run
// every image without one of its own is played through — and otherwise the
// image's own path, which does not exist, so a caller finds no run to replay.
[[nodiscard]] std::filesystem::path scriptFor(const std::filesystem::path& directory,
                                              const std::filesystem::path& image);

// Where a directory of recorded runs keeps the one for `image`: the image's file
// name without its extension, each space an underscore, with `.snaginput` — so
// `Some Game (U).sfc` has `Some_Game_(U).snaginput`. The file need not exist.
[[nodiscard]] std::filesystem::path scriptPathFor(const std::filesystem::path& directory,
                                                  const std::filesystem::path& image);

}  // namespace snaggletooth::disasm
