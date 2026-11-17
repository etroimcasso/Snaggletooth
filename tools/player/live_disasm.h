#pragma once

// The cartridge disassembled beside the game as it is played.
//
// A `LiveDisassembly` binds a `disasm::BufferedRun` (`rom/rom_buffered_run.h`)
// to the machine a host runs, built to keep the host's call in flight at the
// buffer's ceilings. The host's thread copies each report into the buffer; the
// run's threads work through it; and a thread of this class's own makes the
// tree, one tree after another: each from the directory's request as it stood
// when the session began (`disasm::treeRequest`, read once at construction),
// handed the run's observation, disassembled, written (`disasm::writeProject`),
// and followed by `snes_render` and then `snes_verify`, which the host runs. It
// looks at the run again five seconds after a tree is done, and begins the next
// when the run has found something that tree does not hold.
//
// The trees of one session never read each other, so the closing tree is the
// one the directory takes from the whole run. A lifted file a tree in the
// middle wrote under a name the closing tree does not use stays in the
// directory beside it; the closing tree's manifest does not name it.
//
// The game stops when the buffer stands at its ceilings, when its RAM stands
// past its own ceiling because the files are written more slowly than the game
// fills the buffer (`disasm::BufferedRun::memoryOver`), and when the person
// stops it (`pause.h`). A stop of the first kind ends at the resume level of the
// two ceilings together; one of the second, when RAM has drained to the resume
// level of its own ceiling or nothing is held. The host asks after every run of
// its machine, and while the answer is a stop it runs nothing and asks again:
//
//   player::LiveDisassembly live{machine, rom, "tree", {}, hooks};
//   live.start();
//   while (!closing) {
//     machine.run(quarterFrame);
//     for (player::Stopped why = live.afterRun(clock()); why != player::Stopped::No && !closing;
//          why = live.whileStopped(clock())) {
//       wait();  // the host's own tenth of a second: its events, its window
//     }
//   }
//   live.beginClose();
//   while (!live.closed()) wait();
//   std::string error;
//   const std::optional<player::TreeSummary> last = live.closingTree(error);
//
// `beginClose()` ends the run: everything buffered is played, the last tree is
// made from the whole of it, and the two commands run, all on the tree's
// thread, while the host keeps its window alive. The host makes no call into
// the machine or the run after it. Nothing here calls into a windowing library
// or starts a process: what the class says goes through `LiveHooks`, and a
// command is run by the host's hook.
//
// The tree's thread works at `disasm::ThreadClass::Lowered` while the game runs
// and at `Normal` while it is stopped or closing, as the run's threads do.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "player/pause.h"
#include "rom/rom_buffered_run.h"
#include "snaggletooth/snes/snes.h"

namespace snaggletooth::player {

// What one tree holds, counted as `snes_disasm` counts its own.
struct TreeSummary {
  std::size_t files = 0;
  std::size_t instructions = 0;
  std::size_t entries = 0;
  std::size_t stops = 0;
  std::size_t placed = 0;
  std::size_t imageBytes = 0;
  std::size_t notes = 0;
  std::uint64_t playedMaster = 0;  // the master cycles the run's threads had played when the tree's observation was taken
};

struct LiveOptions {
  std::filesystem::path bufferDirectory;      // where the buffer's files go; empty: `buffer` under the tree's directory
  std::uint64_t memoryBytes = 1'000'000'000;  // the buffer's RAM ceiling
  std::uint64_t diskBytes = 64'000'000'000;   // the buffer's files' ceiling
  unsigned threads = 0;                       // `BufferedRunOptions::threads`
  PauseSettings pause;
  std::filesystem::path commands;             // the directory `snes_render` and `snes_verify` are in; empty: neither is run
  std::filesystem::path image;                // the image's path, as `snes_verify` is handed it
  std::uint64_t treeIntervalNanos = 5'000'000'000;  // the least time from one tree's end to the next look at the run, a year at most
};

struct LiveHooks {
  // A line about the run's ordinary course; the host drops it under `--quiet`.
  std::function<void(const std::string&)> say;
  // A line the person must see whatever the host was told: a failure, and the
  // answer to a request to close while the close is working.
  std::function<void(const std::string&)> always;
  // Runs a command to its end and answers its exit status, or nothing when it
  // could not be started. `lowered` asks for a class under the host's thread's.
  std::function<std::optional<int>(const std::vector<std::string>& arguments, bool lowered)> run;
};

// What the tree's thread is doing.
enum class Work : std::uint8_t { Watching, Tree, Render, Verify, Done };

// One look at the disassembly, for a title, a line or a panel.
struct LiveStatus {
  disasm::BufferedRun::Progress buffer;
  bool atLimit = false;                           // the buffer stands at its ceilings
  Stopped stopped = Stopped::No;
  bool memoryStop = false;                        // a stop at the ceilings that is RAM alone past its ceiling
  bool closing = false;
  Work work = Work::Watching;
  std::size_t trees = 0;                          // trees written so far
  std::uint64_t ceiling = 0;                      // the two ceilings together
  std::uint64_t resumeLevel = 0;
  PauseSettings pause;
  std::optional<double> fillBytesPerSecond;       // over the last stretch of play
  std::optional<double> drainBytesPerSecond;      // over the last five seconds of this stop or close, as a positive rate
  std::optional<double> secondsToResume;          // to the resume level, while stopped at the ceilings without `hold`
  std::optional<double> secondsToEmpty;
  std::optional<double> secondsOfPlayAfter;       // from the resume level to the ceilings at the fill rate
};

// The characters of `text` as a title shows them: its UTF-8 code points.
[[nodiscard]] std::size_t titleCharacters(std::string_view text) noexcept;
// The characters a title has room for after `name` and the dash that follows
// it, estimated: `points` of title bar at `characterPoints` a character, less
// the name's characters and the dash's three. 0 where the name fills the bar,
// and where either width is not above zero.
[[nodiscard]] std::size_t titleRoom(double points, double characterPoints, std::string_view name) noexcept;

// What the window's title says after the game's name while a run disassembles
// as it plays. It has three parts, in this order: the lead, the buffer's RAM
// against its ceiling, and its files against theirs. The lead is the host's
// while the game runs or is stopped — its rate, the stop's phrase — and the
// close's progress while the disassembly closes:
//
//   finishing: 3.2 GB of 12.4 GB played, 25 %, about 1 min 40 s left
//   finishing: writing the last tree
//   finishing: snes_render
//   finishing: snes_verify
//   finishing: closing the buffer
//
// The first counts from the most the buffer has held since the close began; the
// time is the buffer's alone, at the rate it is draining, and the tree and the
// two commands follow it.
//
// Where `room` characters hold the three parts, the text is all of them, joined
// by dashes. Where they do not, it is a page: as many parts in order as `room`
// holds, one at least, each page shown for four seconds and the first following
// the last. The lead's page is shown at once when the game stops or resumes,
// when the close begins, and at each stage of the close.
//
// Every text ends in a spinner that turns one step a call while the disassembly
// is working — its threads playing the buffer, the buffer moving, a tree being
// made or checked — and stands still when none of that changes, so a stall
// shows as a spinner that has stopped. The two sizes are taken every two
// seconds.
//
//   player::LiveTitle title;
//   const std::size_t room = player::titleRoom(barPoints, 7.0, game);
//   const std::string text = game + " — " + title.text("60.1 fps", live.status(), nowNanos, room);
//   // "Game — 60.1 fps — RAM 412 MB of 1.0 GB — files 0 MB of 64.0 GB", then a braille dot
class LiveTitle {
 public:
  static constexpr std::uint64_t kRefreshNanos = 2'000'000'000;  // how often the two sizes are taken again
  static constexpr std::uint64_t kPageNanos = 4'000'000'000;     // how long a page is shown
  // The text at `nanos` on the host's steady clock, for a title with `room`
  // characters after the game's name. `lead` is left out when it is empty, and
  // is not read while the disassembly closes.
  [[nodiscard]] std::string text(std::string_view lead, const LiveStatus& status, std::uint64_t nanos,
                                 std::size_t room);

 private:
  [[nodiscard]] std::string closeText(const LiveStatus& status) const;

  std::string ram_;
  std::string files_;
  std::uint64_t sizesAt_ = 0;
  bool begun_ = false;
  std::uint64_t played_ = 0;
  std::uint64_t held_ = 0;
  std::size_t trees_ = 0;
  unsigned step_ = 0;
  std::size_t first_ = 0;        // the first part of the page shown
  std::uint64_t pageAt_ = 0;     // when that page was first shown
  unsigned saying_ = 0;          // what the lead was about at the last call
  std::uint64_t closeMost_ = 0;  // the most the buffer has held since the close began
};

class LiveDisassembly {
 public:
  // Whether `directory` can take a tree of `rom`: `disasm::treeRequest` made and
  // let go. False with `error` set for a manifest that does not read or was
  // written for another image; `found` says whether a manifest is there, and
  // `entries` and `files` what it holds.
  [[nodiscard]] static bool readable(std::span<const std::uint8_t> rom, const std::filesystem::path& directory,
                                     bool& found, std::size_t& entries, std::size_t& files, std::string& error);

  // Binds the run, throwing as `disasm::BufferedRun`'s constructor does. `rom`
  // and `machine` outlive this.
  LiveDisassembly(Snes& machine, std::span<const std::uint8_t> rom, std::filesystem::path directory,
                  LiveOptions options, LiveHooks hooks);
  // `beginClose()` and the wait for `closed()`, where the host did neither.
  ~LiveDisassembly();
  LiveDisassembly(const LiveDisassembly&) = delete;
  LiveDisassembly& operator=(const LiveDisassembly&) = delete;

  // Starts the tree's thread. Called once.
  void start();

  // The host's thread, between two runs of its machine. `afterRun` puts the
  // whole machine into the buffer when the beam has wrapped since the last
  // call, then asks the pause control; `whileStopped` asks it again during a
  // stop. `nanos` is the host's steady clock. Both answer whether the game is
  // stopped; the run's threads change class as the answer changes.
  [[nodiscard]] Stopped afterRun(std::uint64_t nanos);
  [[nodiscard]] Stopped whileStopped(std::uint64_t nanos);
  // The person's chord or button (`PauseControl::toggle`).
  void togglePause();
  // The console's reset line, through the run (`disasm::BufferedRun::reset`).
  void reset();
  [[nodiscard]] PauseSettings pauseSettings() const;
  void setPauseSettings(PauseSettings settings);
  // New ceilings for the buffer, for the rest of the session.
  void setLimits(std::uint64_t memoryBytes, std::uint64_t diskBytes);

  // Any thread.
  [[nodiscard]] LiveStatus status() const;
  // The status as one phrase, for a title or a line: what the buffer holds
  // against its ceilings and when the game resumes, or what the close has left.
  // Empty while the game runs and nothing is closing.
  [[nodiscard]] std::string statusText() const;

  // Ends the run on the tree's thread: the observers are cleared and the run's
  // threads raised here, on the host's thread, and the rest follows there.
  // Called once, after the host's last run of its machine.
  void beginClose();
  [[nodiscard]] bool closed() const noexcept;
  // A request to close while the close works: says through `always` what is
  // left and that `howToEndIt` ends the process without the last tree.
  void closeAsked(const std::string& howToEndIt);
  // The last tree's summary once `closed()`, or nothing with `error` set.
  [[nodiscard]] std::optional<TreeSummary> closingTree(std::string& error) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace snaggletooth::player
