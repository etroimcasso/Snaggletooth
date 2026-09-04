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
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "snaggletooth/snes/snes.h"

namespace snaggletooth::disasm {

// One line of a script: from `frame` on, `port` holds exactly the buttons in
// `pad`.
struct InputEvent {
  std::uint32_t frame = 0;
  JoypadPort port = JoypadPort::One;
  Joypad pad;
};

// A parsed script: its events in frame order, each port at most once per frame.
struct InputScript {
  std::vector<InputEvent> events;

  // Whether the script names `port` anywhere — that is, whether the port has a pad.
  [[nodiscard]] bool names(JoypadPort port) const noexcept;

  // What `port` holds at `frame`: the last line at or before it, nothing pressed
  // before the first, and no pad at all on a port the script never names.
  [[nodiscard]] std::optional<Joypad> padAt(JoypadPort port, std::uint32_t frame) const noexcept;
};

// Reads a script. Nothing, with `error` naming the line and what is wrong with
// it, when a line does not parse: a frame out of order, a port named twice on
// one frame, a button named twice, a word that is not a button.
[[nodiscard]] std::optional<InputScript> parseInputScript(std::string_view text, std::string& error);

}  // namespace snaggletooth::disasm
