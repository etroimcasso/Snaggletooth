#pragma once

// Progress: what a long step is doing, told to the caller as it goes.
//
// The cartridge disassembler and the differential each run the machine for
// tens of seconds of the master clock, which is about as long in wall time.
// A caller that wants to show that something is happening hands either a
// `ProgressSink`; the library calls it as each stage begins and, for a stage
// that runs the machine, every tenth of a second of the master clock with the
// cycles spent against the budget. The library prints nothing. The
// `ProgressPrinter` below is the printer the command-line tools use over it,
// kept here so a front end can use the same one or write its own.

#include <cstdint>
#include <functional>
#include <ostream>
#include <string>
#include <string_view>

namespace snaggletooth::disasm {

// One report: the stage by name, and for a stage that runs the machine the
// master cycles spent so far against the budget it was given. A stage with no
// measure reports zero for both.
struct Progress {
  std::string_view stage;
  std::uint64_t spent = 0;
  std::uint64_t budget = 0;
};

using ProgressSink = std::function<void(const Progress&)>;

// The master clock's rate, for a report read as seconds.
inline constexpr std::uint64_t kProgressMasterPerSecond = 21'477'272u;

// How often a stage that runs the machine reports: every tenth of a second of
// the master clock, and once more when it ends.
inline constexpr std::uint64_t kProgressTick = kProgressMasterPerSecond / 10u;

// Writes progress to a stream as the tools do. On a terminal a stage with a
// measure is one line refreshed in place — `running the cartridge: 23.5 of
// 60.0 s` — and ended with a newline when the next stage begins, when its
// count starts over (the second of two boots), or when `finish` is called;
// off a terminal, where a refresh would pile up in a log, the line is written
// when the stage begins, each time the seconds spent cross a multiple of ten,
// and when the stage reaches its budget. A stage with no measure is one line
// either way.
class ProgressPrinter {
 public:
  ProgressPrinter(std::ostream& out, bool terminal) : out_(out), terminal_(terminal) {}

  void operator()(const Progress& progress) {
    const bool newStage = progress.stage != stage_ || (measured_ && progress.spent < lastSpent_);
    if (newStage) {
      finish();
      stage_ = std::string(progress.stage);
      lastTens_ = 0;
      measured_ = progress.budget != 0;
      if (!measured_) {
        out_ << stage_ << "\n";
        out_.flush();
        return;
      }
    }
    if (!measured_) return;
    const std::uint64_t tens = progress.spent / (10u * kProgressMasterPerSecond);
    const bool ended = progress.spent >= progress.budget;
    if (terminal_) {
      out_ << "\r" << line(progress);
      out_.flush();
      open_ = true;
    } else if (newStage || tens != lastTens_ || ended) {
      out_ << line(progress) << "\n";
      out_.flush();
    }
    lastTens_ = tens;
    lastSpent_ = progress.spent;
  }

  // A stage of the tool's own, with no measure: `writing …`.
  void stage(std::string_view name) { (*this)(Progress{.stage = name, .spent = 0, .budget = 0}); }

  // Ends a line refreshed in place, if one is open.
  void finish() {
    if (open_) {
      out_ << "\n";
      out_.flush();
      open_ = false;
    }
    measured_ = false;
    stage_.clear();
  }

 private:
  static std::string seconds(std::uint64_t cycles) {
    const std::uint64_t tenths = (cycles * 10u + kProgressMasterPerSecond / 2u) / kProgressMasterPerSecond;
    return std::to_string(tenths / 10u) + "." + std::to_string(tenths % 10u);
  }

  std::string line(const Progress& progress) const {
    return stage_ + ": " + seconds(progress.spent) + " of " + seconds(progress.budget) + " s";
  }

  std::ostream& out_;
  bool terminal_;
  std::string stage_;
  std::uint64_t lastTens_ = 0;
  std::uint64_t lastSpent_ = 0;
  bool measured_ = false;
  bool open_ = false;
};

}  // namespace snaggletooth::disasm
