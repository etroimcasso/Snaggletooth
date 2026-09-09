// Progress — what the disassembler and the differential tell a caller as they
// run, and what the printer writes from it (`rom/progress.h`).
//
// Held here: the run reports its stage first with nothing spent and last with
// the whole budget, in order and never backwards, every tenth of a second of
// the master clock between; the disassembly's stages come in their order; the
// boot reports both boots and what each took; the replay reports short of the budget when the
// cartridge stopped; and the printer refreshes one line in place on a terminal
// and writes one line per ten seconds off one.

#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "examples/example_cartridges.h"
#include "gtest/gtest.h"
#include "ir/cpu65816_lift.h"
#include "ir/ir_differential.h"
#include "rom/progress.h"
#include "rom/rom_disasm.h"
#include "rom/rom_observe.h"

namespace snaggletooth::disasm {
namespace {

using examples::mixedImage;
using examples::uploadingImage;

constexpr std::uint64_t kFrame = 357'954u;  // one NTSC frame of the master clock, roughly

struct Report {
  std::string stage;
  std::uint64_t spent = 0;
  std::uint64_t budget = 0;
};

// A sink that keeps every report.
ProgressSink keeping(std::vector<Report>& out) {
  return [&out](const Progress& p) {
    out.push_back(Report{std::string(p.stage), p.spent, p.budget});
  };
}

// Every report of one stage: the first with nothing spent, the last with the
// budget spent — or the step that carried the run past it, never more — the
// spent never falling, and consecutive reports never more than a tick apart.
void expectMeasured(const std::vector<Report>& reports, const std::string& stage, std::uint64_t budget) {
  constexpr std::uint64_t kLongestStep = 1024;  // master cycles; an instruction is far fewer
  std::vector<const Report*> of;
  for (const Report& r : reports) {
    if (r.stage == stage) of.push_back(&r);
  }
  ASSERT_GE(of.size(), 2u) << stage;
  EXPECT_EQ(of.front()->spent, 0u) << stage;
  EXPECT_GE(of.back()->spent, budget) << stage;
  EXPECT_LT(of.back()->spent - budget, kLongestStep) << stage;
  for (const Report* r : of) EXPECT_EQ(r->budget, budget) << stage;
  for (std::size_t i = 1; i < of.size(); ++i) {
    EXPECT_GE(of[i]->spent, of[i - 1]->spent) << stage;
    EXPECT_LE(of[i]->spent - of[i - 1]->spent, kProgressTick + 2u * kFrame) << stage << " at " << i;
  }
  // Every tick between the first and the last is reported.
  EXPECT_GE(of.size(), budget / kProgressTick) << stage;
}

// ---- the run ----------------------------------------------------------------------

TEST(Progress, TheRunReportsEveryTenthOfASecondFromNothingToTheBudget) {
  std::vector<Report> reports;
  std::vector<std::string> notes;
  const std::uint64_t budget = 3u * kProgressTick + kFrame;
  const std::vector<std::uint8_t> rom = mixedImage();
  static_cast<void>(observeRun(rom, budget, InputScript{}, notes, keeping(reports)));
  expectMeasured(reports, "running the cartridge", budget);
  for (const Report& r : reports) EXPECT_EQ(r.stage, "running the cartridge");
}

TEST(Progress, TheRunReportsNothingWithoutASink) {
  std::vector<std::string> notes;
  const std::vector<std::uint8_t> rom = mixedImage();
  const RunObservation with = observeRun(rom, 2u * kFrame, InputScript{}, notes);
  EXPECT_GT(with.instructions, 0u);
}

// ---- the disassembly ------------------------------------------------------------------

TEST(Progress, TheDisassemblyReportsItsStagesInOrder) {
  std::vector<Report> reports;
  const std::vector<std::uint8_t> rom = mixedImage();
  CartridgeRequest request;
  request.rom = rom;
  request.captureSound = false;
  request.observeRun = true;
  request.runMasterCycles = kProgressTick + kFrame;
  request.progress = keeping(reports);
  static_cast<void>(disassembleCartridge(request));
  std::vector<std::string> stages;
  for (const Report& r : reports) {
    if (stages.empty() || stages.back() != r.stage) stages.push_back(r.stage);
  }
  const std::vector<std::string> expected = {"running the cartridge", "tracing", "proving what every path reaches"};
  EXPECT_EQ(stages, expected);
  expectMeasured(reports, "running the cartridge", request.runMasterCycles);
  for (const Report& r : reports) {
    if (r.stage != "running the cartridge") {
      EXPECT_EQ(r.spent, 0u) << r.stage;
      EXPECT_EQ(r.budget, 0u) << r.stage;
    }
  }
}

TEST(Progress, TheBootReportsBothBootsAndWhatEachTook) {
  std::vector<Report> reports;
  std::string reason;
  const std::uint64_t budget = 2u * kProgressMasterPerSecond;
  const std::vector<std::uint8_t> rom = uploadingImage();
  const std::optional<UploadCapture> capture = captureUpload(rom, budget, reason, keeping(reports));
  ASSERT_TRUE(capture.has_value()) << reason;
  // Two boots, each beginning at nothing spent and ending where the program
  // started — early, on this cartridge — never at the bound.
  std::vector<std::size_t> starts;
  for (std::size_t i = 0; i < reports.size(); ++i) {
    EXPECT_EQ(reports[i].stage, "booting the sound program");
    EXPECT_EQ(reports[i].budget, budget);
    if (reports[i].spent == 0u) starts.push_back(i);
  }
  ASSERT_EQ(starts.size(), 2u);
  ASSERT_GE(reports.size(), 4u);
  const Report& firstEnd = reports[starts[1] - 1u];
  const Report& secondEnd = reports.back();
  EXPECT_GT(firstEnd.spent, 0u);
  EXPECT_LT(firstEnd.spent, budget);
  EXPECT_GT(secondEnd.spent, 0u);
  EXPECT_LT(secondEnd.spent, budget);
  for (std::size_t i = 1; i < reports.size(); ++i) {
    if (i == starts[1]) continue;
    EXPECT_GE(reports[i].spent, reports[i - 1].spent) << i;
  }
}

TEST(Progress, ABootThatNeverStartsTheProgramReportsItsWholeBudget) {
  std::vector<Report> reports;
  std::string reason;
  const std::uint64_t budget = 3u * kProgressTick;
  const std::vector<std::uint8_t> rom = mixedImage();  // sends no sound program
  const std::optional<UploadCapture> capture = captureUpload(rom, budget, reason, keeping(reports));
  EXPECT_FALSE(capture.has_value());
  ASSERT_GE(reports.size(), 2u);
  EXPECT_EQ(reports.front().spent, 0u);
  EXPECT_GE(reports.back().spent, budget);
}

// ---- the replay --------------------------------------------------------------------

TEST(Progress, TheReplayReportsShortOfTheBudgetWhenTheCartridgeStops) {
  const std::vector<std::uint8_t> rom = mixedImage();
  CartridgeRequest request;
  request.rom = rom;
  request.captureSound = false;
  const CartridgeDisassembly d = disassembleCartridge(request);
  std::vector<Report> reports;
  ir::Replay replay;
  replay.rom = rom;
  replay.masterCycles = 60u * kProgressMasterPerSecond;
  replay.progress = keeping(reports);
  const ir::DifferentialReport report = ir::differential(d.program, replay);
  ASSERT_TRUE(report.stopped);
  ASSERT_GE(reports.size(), 2u);
  EXPECT_EQ(reports.front().spent, 0u);
  EXPECT_EQ(reports.back().spent, report.masterCycles);
  EXPECT_LT(reports.back().spent, replay.masterCycles);
  for (const Report& r : reports) {
    EXPECT_EQ(r.stage, "replaying the run");
    EXPECT_EQ(r.budget, replay.masterCycles);
  }
  for (std::size_t i = 1; i < reports.size(); ++i) EXPECT_GE(reports[i].spent, reports[i - 1].spent);
}

// ---- the printer -------------------------------------------------------------------

TEST(Progress, ThePrinterRefreshesOneLineInPlaceOnATerminal) {
  std::ostringstream out;
  ProgressPrinter printer(out, true);
  const std::uint64_t budget = 60u * kProgressMasterPerSecond;
  printer(Progress{.stage = "running the cartridge", .spent = 0, .budget = budget});
  printer(Progress{.stage = "running the cartridge", .spent = 235u * kProgressMasterPerSecond / 10u, .budget = budget});
  printer(Progress{.stage = "tracing", .spent = 0, .budget = 0});
  printer.stage("writing out");
  printer.finish();
  EXPECT_EQ(out.str(),
            "\rrunning the cartridge: 0.0 of 60.0 s"
            "\rrunning the cartridge: 23.5 of 60.0 s\n"
            "tracing\n"
            "writing out\n");
}

TEST(Progress, ThePrinterWritesALinePerTenSecondsOffATerminal) {
  std::ostringstream out;
  ProgressPrinter printer(out, false);
  const std::uint64_t budget = 25u * kProgressMasterPerSecond;
  for (std::uint64_t tenth = 0; tenth <= 250u; ++tenth) {
    printer(Progress{.stage = "replaying the run", .spent = tenth * kProgressMasterPerSecond / 10u, .budget = budget});
  }
  printer.finish();
  EXPECT_EQ(out.str(),
            "replaying the run: 0.0 of 25.0 s\n"
            "replaying the run: 10.0 of 25.0 s\n"
            "replaying the run: 20.0 of 25.0 s\n"
            "replaying the run: 25.0 of 25.0 s\n");
}

TEST(Progress, ThePrinterEndsALineWhenItsCountStartsOver) {
  std::ostringstream out;
  ProgressPrinter printer(out, true);
  const std::uint64_t budget = 2u * kProgressMasterPerSecond;
  printer(Progress{.stage = "booting the sound program", .spent = 0, .budget = budget});
  printer(Progress{.stage = "booting the sound program", .spent = 3u * kProgressTick, .budget = budget});
  printer(Progress{.stage = "booting the sound program", .spent = 0, .budget = budget});
  printer(Progress{.stage = "booting the sound program", .spent = 3u * kProgressTick, .budget = budget});
  printer(Progress{.stage = "tracing", .spent = 0, .budget = 0});
  EXPECT_EQ(out.str(),
            "\rbooting the sound program: 0.0 of 2.0 s"
            "\rbooting the sound program: 0.3 of 2.0 s\n"
            "\rbooting the sound program: 0.0 of 2.0 s"
            "\rbooting the sound program: 0.3 of 2.0 s\n"
            "tracing\n");

  std::ostringstream piped;
  ProgressPrinter log(piped, false);
  log(Progress{.stage = "booting the sound program", .spent = 0, .budget = budget});
  log(Progress{.stage = "booting the sound program", .spent = 3u * kProgressTick, .budget = budget});
  log(Progress{.stage = "booting the sound program", .spent = 0, .budget = budget});
  log(Progress{.stage = "booting the sound program", .spent = budget, .budget = budget});
  EXPECT_EQ(piped.str(),
            "booting the sound program: 0.0 of 2.0 s\n"
            "booting the sound program: 0.0 of 2.0 s\n"
            "booting the sound program: 2.0 of 2.0 s\n");
}

}  // namespace
}  // namespace snaggletooth::disasm
