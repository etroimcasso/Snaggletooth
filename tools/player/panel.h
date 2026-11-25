#pragma once

// What the panel over a stopped game says and what it lets the person change.
//
// `panelOf` turns one look at the disassembly into the panel's lines;
// `drawPanel` draws them with Dear ImGui and answers what the person did, which
// the player hands to the `LiveDisassembly`:
//
//   const player::PanelModel model = player::panelOf(live.status(), "Cmd+P");
//   const player::PanelActions did = player::drawPanel(model, edits);
//   if (did.toggle) live.togglePause();
//   if (did.pause) live.setPauseSettings(*did.pause);
//   if (did.limits) live.setLimits(did.limits->memoryBytes, did.limits->diskBytes);
//
// The panel is up while the game is stopped and while the disassembly closes.
// It has four headings: a stop at the buffer's ceilings, a stop with RAM alone
// past its ceiling, a stop by hand, and the close. Its lines, in this order,
// each present only when the status has its figure:
//
//   The buffer holds 6.0 GB of 11.0 GB: 1.0 GB of 1.0 GB in RAM, 5.0 GB of 10.0 GB in files.
//   Draining at 610 MB a second.
//   The game resumes at 5.5 GB, in about 1 s.
//   Empty in about 10 s.
//   The last stretch of play filled the buffer at 120 MB a second: from 5.5 GB the game plays about 46 s before it stops again.
//
// A stop with RAM past its ceiling opens with `The buffer holds 1.2 GB of RAM,
// past its 1.0 GB ceiling, while its files are written.`, resumes at the RAM
// level (`The game resumes at 500 MB of RAM.`), and leaves out the last line,
// which is the ceilings' own. Under `hold` and by hand the game resumes when the
// person resumes it, and the third line says so. The close says what it has
// left (`12.3 GB to play, then the last tree.`) and that it ends by itself, and
// carries no control.
//
// The controls, under the lines: a bar of what the buffer holds over its
// ceilings with a mark at the resume level, `Resume at` (a slider, 0 to 100
// percent), `Stay stopped until I resume` (`PauseSettings::hold`), `RAM (GB)`
// and `Files (GB)` with `Apply` (the two ceilings, read by `parseGigabytes`), and
// `Resume` with the pause chord beside it. A stop with RAM past its ceiling
// shows the buffer's RAM over its own ceiling on the bar and marks the RAM level.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "player/live_disasm.h"
#include "player/pause.h"

namespace snaggletooth::player {

struct PanelModel {
  std::string heading;             // "Stopped: the buffer is at its ceilings", "Stopped by hand", "Finishing the disassembly"
  std::vector<std::string> lines;  // what the buffer holds, the rates, the times
  float fill = 0.0f;               // what the buffer holds over its ceilings, 0 to 1
  float resumeAt = 0.0f;           // the resume level over the ceilings, where the bar marks it
  bool controls = true;            // false while closing: nothing is left to choose
  bool canResume = false;          // the resume button answers
  PauseSettings pause;
  std::uint64_t memoryBytes = 0;
  std::uint64_t diskBytes = 0;
  std::string chord;               // the pause chord as the person presses it; empty: none is bound
};

// The panel for `status`. A game that runs and is not closing has an empty
// heading and no lines; the player draws no panel then.
//
// `canResume` is the resume the run grants (`PauseControl::update`), read from
// what the status carries: a stop by hand always; a stop at the ceilings once
// the buffer holds the resume level or less, or once it stands under its
// ceilings with RAM at or under its own; a stop with RAM past its ceiling once
// RAM stands at its level (`BufferedRun::Progress::memoryLevel`), the buffer is
// empty, or the same second case holds. It is never true for a resume the run
// refuses.
[[nodiscard]] PanelModel panelOf(const LiveStatus& status, std::string_view chord);

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

// Draws the panel inside the Dear ImGui frame the caller began, centered over
// the display, and answers what the person did. The window is not movable, not
// resizable, and has no title bar or close box. A size field that
// `parseGigabytes` does not read says `A size in GB, such as 1 or 0.5` under it,
// and `Apply` answers `limits` only when both read. `layout`, where given, is
// filled with each control's rectangle.
[[nodiscard]] PanelActions drawPanel(const PanelModel& model, PanelEdits& edits, PanelLayout* layout = nullptr);

// A size as the two fields show it: GB with as many decimals as it needs, which
// `parseGigabytes` reads back to the same bytes: `1`, `0.5`, `1.25`.
[[nodiscard]] std::string gigabytesText(std::uint64_t bytes);

}  // namespace snaggletooth::player
