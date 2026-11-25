#pragma once

// What the panel over a stopped game says and what it lets the person change.
//
// `panelOf` turns one look at the disassembly into the panel's parts;
// `drawPanel` draws them with Dear ImGui as a sheet over the dimmed picture and
// answers what the person did, which the player hands to the `LiveDisassembly`:
//
//   player::stylePanel();                     // once, after the context is made
//   player::usePlatformFont();
//   ...
//   const player::LiveStatus status = live.status();
//   if (status.closing) closeMost = std::max(closeMost, status.buffer.held);
//   const player::PanelModel model = player::panelOf(status, "Cmd+P", closeMost);
//   const player::PanelActions did = player::drawPanel(model, edits);
//   if (did.toggle) live.togglePause();
//   if (did.pause) live.setPauseSettings(*did.pause);
//   if (did.limits) live.setLimits(did.limits->memoryBytes, did.limits->diskBytes);
//
// The panel is up while the game is stopped and while the disassembly closes.
// From the top: the heading, in the color of the kind of stop; under it when the
// game resumes; a meter of what the buffer holds against its ceilings, with a
// mark at the resume level, its figure beside it and its parts under it; a line
// for each rate the status has; then the controls, in two sections — `Resume at`
// (a slider, 0 to 100 percent) and `Stay stopped until I resume`
// (`PauseSettings::hold`) under "When the buffer is full", and the RAM and files
// ceilings in GB with `Apply` under "Buffer ceilings" — and last, on the right,
// `Resume` with the pause chord beside it. For a stop at the ceilings:
//
//   Stopped: the buffer is at its ceilings
//   The game resumes at 5.5 GB, in about 1 s.
//   [meter]  6.0 GB of 11.0 GB
//   RAM 1.0 GB of 1.0 GB · files 5.0 GB of 10.0 GB
//   Draining at 610 MB a second; empty in about 10 s.
//   The last stretch of play filled it at 120 MB a second: from 5.5 GB the game plays about 46 s before it stops again.
//
// A stop with RAM past its ceiling meters RAM against its own ceiling and its
// level (`RAM 1.2 GB of 1.0 GB`, `Its files are being written: 300 MB of 10.0
// GB.`, `The game resumes at 400 MB of RAM.`) and has no fill line, which is the
// ceilings'. Under `hold` and by hand the game resumes when the person resumes
// it.
//
// The close's meter is the close's own progress and carries no mark and no
// control. While the buffer holds anything it is what the close has played of
// the most the buffer held once the close began (`1.2 GB of 3.2 GB played`), the
// drain under it. Once the buffer is empty the stage has no known end, so the
// meter is a stripe that moves while it works, and the line names the stage as
// the window's title does: `Reading the run records: decompressing
// runs/0009.snagrun.zst (5.5 MB).`, `Writing the last tree: proving what every
// path reaches.` with `about 40 s left` beside the meter where the session's
// last tree gives a time, `Running snes_render over the tree.`, `Running
// snes_verify over the tree.`, `Closing the buffer.` The last line is always
// `The application will exit when done.` A rate is said in whole megabytes a second from
// 10 up, in tenths under that, rounded down (`0.2 MB a second`), and as `under
// 0.1 MB a second` below a tenth.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "player/live_disasm.h"
#include "player/pause.h"

namespace snaggletooth::player {

// The kind of panel, which sets its heading's color.
enum class PanelKind : std::uint8_t {
  None,            // the game runs: no panel
  AtCeilings,      // stopped: the buffer is at its ceilings
  RamPastCeiling,  // stopped: the buffer's RAM is past its ceiling
  ByHand,          // stopped by the pause chord or the panel
  Closing,         // the disassembly is finishing
};

struct PanelModel {
  PanelKind kind = PanelKind::None;
  std::string heading;             // "Stopped: the buffer is at its ceilings", "Stopped by hand", "Finishing the disassembly"
  std::string summary;             // when the game resumes, or what the close has left
  std::string meter;               // beside the meter: "6.0 GB of 11.0 GB"
  std::string parts;               // under the meter: "RAM 1.0 GB of 1.0 GB · files 5.0 GB of 10.0 GB"; may be empty
  std::vector<std::string> notes;  // the rates and the times, a sentence each
  float fill = 0.0f;               // what the meter holds over its ceiling, 0 to 1
  float resumeAt = 0.0f;           // the resume level over that ceiling, where the meter marks it
  bool mark = true;                // false while closing: the meter has no resume level
  bool indeterminate = false;      // the meter moves with no fill: a stage of the close with no known end
  bool controls = true;            // false while closing: nothing is left to choose
  bool canResume = false;          // the resume button answers
  PauseSettings pause;
  std::uint64_t memoryBytes = 0;
  std::uint64_t diskBytes = 0;
  std::string chord;               // the pause chord as the person presses it; empty: none is bound
};

// The panel for `status`. `closeMost` is the most the buffer has held since the
// close began, which the caller keeps from one look to the next; it is read only
// while closing. A game that runs and is not closing has the kind `None`, an
// empty heading and nothing else; the player draws no panel then.
//
// `canResume` is the resume the run grants (`PauseControl::update`), read from
// what the status carries: a stop by hand always; a stop at the ceilings once
// the buffer holds the resume level or less, or once it stands under its
// ceilings with RAM at or under its own; a stop with RAM past its ceiling once
// RAM stands at its level (`BufferedRun::Progress::memoryLevel`), the buffer is
// empty, or the same second case holds. It is never true for a resume the run
// refuses.
[[nodiscard]] PanelModel panelOf(const LiveStatus& status, std::string_view chord, std::uint64_t closeMost = 0);

struct PanelLimits {
  std::uint64_t memoryBytes = 0;
  std::uint64_t diskBytes = 0;
};

// What the person did in one drawing of the panel.
struct PanelActions {
  bool toggle = false;                 // the resume button
  std::optional<PauseSettings> pause;  // the level or the hold changed
  std::optional<PanelLimits> limits;   // new ceilings applied
};

// The text the person is typing into the two size fields, kept by the caller
// between drawings.
struct PanelEdits {
  std::string memory;   // GB, as `parseGigabytes` reads it
  std::string disk;
  bool seeded = false;  // false: the fields take the model's ceilings at the next drawing
};

// A control's rectangle in Dear ImGui's display coordinates; all zero for one
// the drawing did not draw.
struct PanelRect {
  float left = 0.0f;
  float top = 0.0f;
  float right = 0.0f;
  float bottom = 0.0f;
  [[nodiscard]] bool drawn() const noexcept { return right > left; }
};

// Where `drawPanel` drew each control, for a caller that places the mouse on one.
struct PanelLayout {
  PanelRect bar;
  PanelRect resumeAt;
  PanelRect hold;
  PanelRect memory;
  PanelRect disk;
  PanelRect apply;
  PanelRect resume;
};

// Dear ImGui's style for the panel — rounded corners, the spacing, the colors of
// the sheet and its controls — set on the current context. Called once, after
// the context is made and before its sizes are scaled.
void stylePanel();

// The platform's UI font added to the current context's atlas and made its
// default, at 15 points: San Francisco on macOS, Segoe UI on Windows, and on
// Linux DejaVu Sans or Noto Sans where the system has one. False, with Dear
// ImGui's embedded scalable font in its place, where none of them is on the
// system or the file does not load. Called once, before the first frame.
bool usePlatformFont();

// Draws the panel inside the Dear ImGui frame the caller began — the picture
// dimmed, the sheet centered over it — and answers what the person did. The
// sheet is not movable, not resizable, and has no title bar or close box. A
// size field that `parseGigabytes` does not read says `A size in GB, such as 1
// or 0.5` under it; `Apply` answers `limits` only when both fields read and one
// differs from the model's ceiling, and is dimmed otherwise. `Resume` is dimmed
// unless `canResume`. `layout`, where given, is filled with each control's
// rectangle.
[[nodiscard]] PanelActions drawPanel(const PanelModel& model, PanelEdits& edits, PanelLayout* layout = nullptr);

// How many times Dear ImGui's own size the panel's text and spacing are drawn
// in a window `windowScale` times the console's picture: a third of the scale,
// and never less than 1, so the player's default `--scale 3` draws them at their
// own size and `--scale 6` at twice it.
[[nodiscard]] float panelTextScale(unsigned windowScale) noexcept;

// A size as the two fields show it: GB with as many decimals as it needs, which
// `parseGigabytes` reads back to the same bytes: `1`, `0.5`, `1.25`.
[[nodiscard]] std::string gigabytesText(std::uint64_t bytes);

}  // namespace snaggletooth::player
