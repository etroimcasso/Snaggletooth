// The panel over a stopped game: every line `panelOf` writes for each kind of
// stop and for the close, the resume it grants, and the drawing read and
// clicked in a Dear ImGui context with no backend.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "imgui.h"
#include "player/live_disasm.h"
#include "player/panel.h"
#include "player/pause.h"

namespace snaggletooth::player {
namespace {

constexpr std::uint64_t kGigabyte = 1'000'000'000u;
constexpr std::uint64_t kMegabyte = 1'000'000u;

// A stop at the ceilings: 6.0 GB held of 1.0 GB of RAM and 10.0 GB of files, the
// resume level at half of the two, 5.5 GB.
LiveStatus atTheCeilings() {
  LiveStatus status;
  status.buffer.held = 6u * kGigabyte;
  status.buffer.memoryUsed = kGigabyte;
  status.buffer.memoryBytes = kGigabyte;
  status.buffer.memoryLevel = 500u * kMegabyte;
  status.buffer.diskUsed = 5u * kGigabyte;
  status.buffer.diskBytes = 10u * kGigabyte;
  status.atLimit = true;
  status.stopped = Stopped::AtLimit;
  status.ceiling = 11u * kGigabyte;
  status.resumeLevel = 5'500'000'000u;
  status.pause = PauseSettings{.resumePercent = 50, .hold = false};
  return status;
}

// The same buffer with its drain, its fill and the times they give.
LiveStatus atTheCeilingsWithRates() {
  LiveStatus status = atTheCeilings();
  status.drainBytesPerSecond = 610.0 * kMegabyte;
  status.secondsToResume = 0.8;
  status.secondsToEmpty = 9.8;
  status.fillBytesPerSecond = 120.0 * kMegabyte;
  status.secondsOfPlayAfter = 45.8;
  return status;
}

// A stop with RAM alone past its ceiling: 1.2 GB of RAM against 1.0 GB, the
// files with room, the RAM level the run reports 400 MB — a share of the RAM
// ceiling other than the ceilings' resume level, so the two cannot be mistaken.
LiveStatus ramPastItsCeiling() {
  LiveStatus status = atTheCeilings();
  status.buffer.held = 1'500'000'000u;
  status.buffer.memoryUsed = 1'200'000'000u;
  status.buffer.memoryLevel = 400u * kMegabyte;
  status.buffer.diskUsed = 300u * kMegabyte;
  status.atLimit = false;
  status.memoryStop = true;
  return status;
}

LiveStatus closingAt(Work work) {
  LiveStatus status = atTheCeilingsWithRates();
  status.stopped = Stopped::No;
  status.closing = true;
  status.work = work;
  status.buffer.held = 12'300'000'000u;
  return status;
}

TEST(PlayerPanel, AStopAtTheCeilingsWithItsRatesSaysEveryPart) {
  const PanelModel model = panelOf(atTheCeilingsWithRates(), "Cmd+P");
  EXPECT_EQ(model.kind, PanelKind::AtCeilings);
  EXPECT_EQ(model.heading, "Stopped: the buffer is at its ceilings");
  EXPECT_EQ(model.summary, "The game resumes at 5.5 GB, in about 1 s.");
  EXPECT_EQ(model.meter, "6.0 GB of 11.0 GB");
  EXPECT_EQ(model.parts, "RAM 1.0 GB of 1.0 GB · files 5.0 GB of 10.0 GB");
  const std::vector<std::string> notes{
      "Draining at 610 MB a second; empty in about 10 s.",
      "The last stretch of play filled it at 120 MB a second: from 5.5 GB the game plays about 46 s before it "
      "stops again.",
  };
  EXPECT_EQ(model.notes, notes);
  EXPECT_FLOAT_EQ(model.fill, 6.0f / 11.0f);
  EXPECT_FLOAT_EQ(model.resumeAt, 0.5f);
  EXPECT_TRUE(model.mark);
  EXPECT_TRUE(model.controls);
  EXPECT_FALSE(model.canResume);
  EXPECT_EQ(model.chord, "Cmd+P");
  EXPECT_EQ(model.memoryBytes, kGigabyte);
  EXPECT_EQ(model.diskBytes, 10u * kGigabyte);
  EXPECT_EQ(model.pause, (PauseSettings{.resumePercent = 50, .hold = false}));
}

TEST(PlayerPanel, AStopAtTheCeilingsBeforeAnyRateSaysTheLevelAlone) {
  // A rate of nothing is no rate: the buffer neither drains nor filled.
  LiveStatus status = atTheCeilings();
  status.drainBytesPerSecond = 0.0;
  status.fillBytesPerSecond = 0.0;
  status.secondsOfPlayAfter = 10.0;
  const PanelModel model = panelOf(status, "Cmd+P");
  EXPECT_EQ(model.summary, "The game resumes at 5.5 GB.");
  EXPECT_TRUE(model.notes.empty());
}

TEST(PlayerPanel, TheDrainIsSaidByWhatTheStatusHasOfIt) {
  LiveStatus status = atTheCeilings();
  status.drainBytesPerSecond = 610.0 * kMegabyte;
  EXPECT_EQ(panelOf(status, "").notes, (std::vector<std::string>{"Draining at 610 MB a second."}));
  status.drainBytesPerSecond.reset();
  status.secondsToEmpty = 9.8;
  EXPECT_EQ(panelOf(status, "").notes, (std::vector<std::string>{"Empty in about 10 s."}));
}

TEST(PlayerPanel, UnderHoldTheGameResumesWhenThePersonResumesIt) {
  LiveStatus status = atTheCeilingsWithRates();
  status.pause.hold = true;
  status.secondsToResume.reset();
  const PanelModel model = panelOf(status, "Cmd+P");
  EXPECT_EQ(model.summary, "The game resumes when you resume it.");
  EXPECT_EQ(model.notes.size(), 2u);
  EXPECT_TRUE(model.pause.hold);
}

TEST(PlayerPanel, AStopByHandResumesAtTheButtonWhateverTheBufferHolds) {
  LiveStatus status = atTheCeilings();
  status.stopped = Stopped::ByHand;
  const PanelModel model = panelOf(status, "Ctrl+P");
  EXPECT_EQ(model.kind, PanelKind::ByHand);
  EXPECT_EQ(model.heading, "Stopped by hand");
  EXPECT_EQ(model.summary, "The game resumes when you resume it.");
  EXPECT_EQ(model.meter, "6.0 GB of 11.0 GB");
  EXPECT_TRUE(model.notes.empty());
  EXPECT_TRUE(model.canResume);
}

TEST(PlayerPanel, AnEmptyBufferHasNoTimeToEmpty) {
  LiveStatus status = atTheCeilings();
  status.stopped = Stopped::ByHand;
  status.atLimit = false;
  status.buffer.held = 0u;
  status.buffer.memoryUsed = 0u;
  status.buffer.diskUsed = 0u;
  status.secondsToEmpty = 0.0;
  const PanelModel model = panelOf(status, "Cmd+P");
  EXPECT_EQ(model.meter, "0 MB of 11.0 GB");
  EXPECT_EQ(model.parts, "RAM 0 MB of 1.0 GB · files 0 MB of 10.0 GB");
  EXPECT_TRUE(model.notes.empty());
}

TEST(PlayerPanel, ACeilingOfNothingIsFullOnceAnythingIsHeld) {
  LiveStatus status = atTheCeilings();
  status.ceiling = 0u;
  status.resumeLevel = 0u;
  EXPECT_FLOAT_EQ(panelOf(status, "").fill, 1.0f);
  EXPECT_FLOAT_EQ(panelOf(status, "").resumeAt, 0.0f);
  status.buffer.held = 0u;
  EXPECT_FLOAT_EQ(panelOf(status, "").fill, 0.0f);
}

TEST(PlayerPanel, AStopWithRamPastItsCeilingSaysRamAgainstItsOwnCeiling) {
  LiveStatus status = ramPastItsCeiling();
  status.drainBytesPerSecond = 300.0 * kMegabyte;
  status.secondsToEmpty = 5.0;
  status.fillBytesPerSecond = 120.0 * kMegabyte;
  status.secondsOfPlayAfter = 45.8;
  const PanelModel model = panelOf(status, "Cmd+P");
  EXPECT_EQ(model.kind, PanelKind::RamPastCeiling);
  EXPECT_EQ(model.heading, "Stopped: the buffer's RAM is past its ceiling");
  EXPECT_EQ(model.summary, "The game resumes at 400 MB of RAM.");
  EXPECT_EQ(model.meter, "RAM 1.2 GB of 1.0 GB");
  EXPECT_EQ(model.parts, "Its files are being written: 300 MB of 10.0 GB.");
  EXPECT_EQ(model.notes, (std::vector<std::string>{"Draining at 300 MB a second; empty in about 5 s."}));
  EXPECT_FLOAT_EQ(model.fill, 1.0f);
  EXPECT_FLOAT_EQ(model.resumeAt, 0.4f);
  EXPECT_FALSE(model.canResume);
}

TEST(PlayerPanel, WhileTheBufferHoldsAnythingTheCloseMetersWhatItHasPlayed) {
  // 12.3 GB left of the 16.4 GB the buffer held once the close began: 4.1 GB
  // played. A tree from the middle of the session running over it is not the
  // close's stage.
  for (const Work work : {Work::Watching, Work::Tree, Work::Records}) {
    const PanelModel model = panelOf(closingAt(work), "Cmd+P", 16'400'000'000u);
    EXPECT_EQ(model.kind, PanelKind::Closing);
    EXPECT_EQ(model.heading, "Finishing the disassembly");
    EXPECT_EQ(model.summary, "Playing what the buffer holds, then the last tree.");
    EXPECT_EQ(model.meter, "4.1 GB of 16.4 GB played");
    EXPECT_FLOAT_EQ(model.fill, 0.25f);
    EXPECT_FALSE(model.indeterminate);
    EXPECT_EQ(model.parts, "Draining at 610 MB a second; empty in about 10 s.");
    EXPECT_EQ(model.notes, (std::vector<std::string>{"The application will exit when done."}));
    EXPECT_FALSE(model.mark);
    EXPECT_FALSE(model.controls);
    EXPECT_FALSE(model.canResume);
  }
  // The most held is never less than what the buffer holds now.
  EXPECT_EQ(panelOf(closingAt(Work::Watching), "", 0u).meter, "0 MB of 12.3 GB played");
  LiveStatus empty = closingAt(Work::Watching);
  empty.buffer.held = 0u;
  const PanelModel nothing = panelOf(empty, "", 0u);
  EXPECT_EQ(nothing.meter, "nothing left to play");
  EXPECT_FLOAT_EQ(nothing.fill, 1.0f);
}

TEST(PlayerPanel, OnceTheBufferIsEmptyTheCloseNamesItsStageAndItsMeterMoves) {
  const std::vector<std::pair<Work, std::string>> said{
      {Work::Records, "Reading the run records: decompressing runs/0009.snagrun.zst (5.5 MB)."},
      {Work::Tree, "Writing the last tree: proving what every path reaches."},
      {Work::Render, "Running snes_render over the tree."},
      {Work::Verify, "Running snes_verify over the tree."},
      {Work::Done, "Closing the buffer."},
  };
  for (const auto& [work, summary] : said) {
    LiveStatus status = closingAt(work);
    status.buffer.held = 0u;
    status.step = work == Work::Records ? "decompressing runs/0009.snagrun.zst (5.5 MB)"
                                        : "proving what every path reaches";
    const PanelModel model = panelOf(status, "Cmd+P", 16'400'000'000u);
    EXPECT_EQ(model.summary, summary);
    EXPECT_TRUE(model.indeterminate);
    EXPECT_TRUE(model.meter.empty());
    EXPECT_TRUE(model.parts.empty());
    EXPECT_EQ(model.notes, (std::vector<std::string>{"The application will exit when done."}));
    EXPECT_FALSE(model.controls);
  }
  // The last tree's time left, where the session's last tree gives one.
  LiveStatus tree = closingAt(Work::Tree);
  tree.buffer.held = 0u;
  tree.treeSecondsLeft = 39.2;
  EXPECT_EQ(panelOf(tree, "").meter, "about 40 s left");
  EXPECT_EQ(panelOf(tree, "").summary, "Writing the last tree.");
  tree.treeSecondsLeft = 0.0;
  EXPECT_TRUE(panelOf(tree, "").meter.empty());
}

TEST(PlayerPanel, ARunningGameHasNoPanel) {
  LiveStatus status = atTheCeilingsWithRates();
  status.stopped = Stopped::No;
  status.atLimit = false;
  const PanelModel model = panelOf(status, "Cmd+P");
  EXPECT_EQ(model.kind, PanelKind::None);
  EXPECT_TRUE(model.heading.empty());
  EXPECT_TRUE(model.summary.empty());
  EXPECT_TRUE(model.meter.empty());
  EXPECT_TRUE(model.parts.empty());
  EXPECT_FLOAT_EQ(model.fill, 0.0f);
  EXPECT_TRUE(model.notes.empty());
  EXPECT_FALSE(model.controls);
  EXPECT_FALSE(model.canResume);
}

TEST(PlayerPanel, ResumeIsOfferedOnlyWhereTheRunGrantsIt) {
  // At the ceilings: over the level, refused; at the level, granted.
  LiveStatus status = atTheCeilings();
  EXPECT_FALSE(panelOf(status, "").canResume);
  status.buffer.held = status.resumeLevel;
  EXPECT_TRUE(panelOf(status, "").canResume);
  // Under the ceilings again, over the level: granted while RAM is at or under its ceiling.
  status = atTheCeilings();
  status.atLimit = false;
  EXPECT_TRUE(panelOf(status, "").canResume);
  status.buffer.memoryUsed = kGigabyte + 1u;
  EXPECT_FALSE(panelOf(status, "").canResume);

  // RAM past its ceiling: refused until RAM is at its level, the buffer is empty,
  // or RAM is back under its ceiling with the two ceilings not reached.
  status = ramPastItsCeiling();
  EXPECT_FALSE(panelOf(status, "").canResume);
  status.buffer.memoryUsed = status.buffer.memoryLevel;
  EXPECT_TRUE(panelOf(status, "").canResume);
  status = ramPastItsCeiling();
  status.buffer.held = 0u;
  EXPECT_TRUE(panelOf(status, "").canResume);
  // The run's ceilings and its RAM stop are read at different moments, so a RAM
  // stop can be read with the ceilings reached: then the RAM level alone grants it.
  status = ramPastItsCeiling();
  status.atLimit = true;
  status.buffer.memoryUsed = status.buffer.memoryLevel;
  EXPECT_TRUE(panelOf(status, "").canResume);
  status.buffer.memoryUsed = status.buffer.memoryLevel + 1u;
  EXPECT_FALSE(panelOf(status, "").canResume);
  status = ramPastItsCeiling();
  status.buffer.memoryUsed = 800u * kMegabyte;
  EXPECT_TRUE(panelOf(status, "").canResume);
  status.atLimit = true;
  EXPECT_FALSE(panelOf(status, "").canResume);
}

TEST(PlayerPanel, ARateUnderTenMegabytesASecondIsSaidInTenths) {
  LiveStatus status = atTheCeilingsWithRates();
  status.drainBytesPerSecond = 9'990'000.0;
  status.fillBytesPerSecond = 230'000.0;
  PanelModel model = panelOf(status, "Cmd+P");
  ASSERT_EQ(model.notes.size(), 2u);
  EXPECT_EQ(model.notes[0], "Draining at 9.9 MB a second; empty in about 10 s.");
  EXPECT_EQ(model.notes[1],
            "The last stretch of play filled it at 0.2 MB a second: from 5.5 GB the game plays about 46 s before it "
            "stops again.");
  status.drainBytesPerSecond = 10'000'000.0;
  status.fillBytesPerSecond = 99'000.0;
  model = panelOf(status, "Cmd+P");
  ASSERT_EQ(model.notes.size(), 2u);
  EXPECT_EQ(model.notes[0], "Draining at 10 MB a second; empty in about 10 s.");
  EXPECT_EQ(model.notes[1],
            "The last stretch of play filled it at under 0.1 MB a second: from 5.5 GB the game plays about 46 s "
            "before it stops again.");
}

TEST(PlayerPanel, ThePanelGrowsWithTheWindowPastTheDefaultScale) {
  EXPECT_FLOAT_EQ(panelTextScale(1u), 1.0f);
  EXPECT_FLOAT_EQ(panelTextScale(3u), 1.0f);
  EXPECT_FLOAT_EQ(panelTextScale(4u), 4.0f / 3.0f);
  EXPECT_FLOAT_EQ(panelTextScale(6u), 2.0f);
  EXPECT_FLOAT_EQ(panelTextScale(9u), 3.0f);
}

TEST(PlayerPanel, AFieldsSizeReadsBackToTheSameBytes) {
  EXPECT_EQ(gigabytesText(0u), "0");
  EXPECT_EQ(gigabytesText(kGigabyte), "1");
  EXPECT_EQ(gigabytesText(500u * kMegabyte), "0.5");
  EXPECT_EQ(gigabytesText(1'250'000'000u), "1.25");
  EXPECT_EQ(gigabytesText(10'000'000'001u), "10.000000001");
  for (const std::uint64_t bytes : {std::uint64_t{0u}, std::uint64_t{1u}, 500u * kMegabyte, 64u * kGigabyte,
                                    std::uint64_t{1'234'567'891u}}) {
    EXPECT_EQ(parseGigabytes(gigabytesText(bytes)), bytes);
  }
}

// A Dear ImGui context with no backend: the panel's style, an 800 by 600
// display and Dear ImGui's own font's atlas built, which is all the core needs
// to lay out and draw.
class Context {
 public:
  Context() : context_(ImGui::CreateContext()) {
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.DisplaySize = ImVec2(800.0f, 600.0f);
    io.DeltaTime = 1.0f / 60.0f;
    stylePanel();
    io.Fonts->Build();
  }
  ~Context() { ImGui::DestroyContext(context_); }
  Context(const Context&) = delete;
  Context& operator=(const Context&) = delete;

  // One frame with the panel in it.
  PanelActions frame(const PanelModel& model, PanelEdits& edits, PanelLayout* layout = nullptr) {
    ImGui::NewFrame();
    const PanelActions did = drawPanel(model, edits, layout);
    ImGui::Render();
    return did;
  }

  // The mouse placed on the middle of `rect`, pressed and let go over three
  // frames, and what the panel answered across them.
  PanelActions click(const PanelModel& model, PanelEdits& edits, const PanelRect& rect) {
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent((rect.left + rect.right) / 2.0f, (rect.top + rect.bottom) / 2.0f);
    PanelActions all = frame(model, edits);
    io.AddMouseButtonEvent(0, true);
    merge(all, frame(model, edits));
    io.AddMouseButtonEvent(0, false);
    merge(all, frame(model, edits));
    return all;
  }

  // Where each control lands once the window has taken its size.
  PanelLayout layoutOf(const PanelModel& model, PanelEdits& edits) {
    PanelLayout layout;
    for (int i = 0; i < 3; ++i) static_cast<void>(frame(model, edits, &layout));
    return layout;
  }

 private:
  static void merge(PanelActions& into, const PanelActions& from) {
    into.toggle = into.toggle || from.toggle;
    if (from.pause) into.pause = from.pause;
    if (from.limits) into.limits = from.limits;
  }

  ImGuiContext* context_;
};

TEST(PlayerPanel, TheDrawingHoldsThePanelAndEveryControl) {
  Context context;
  PanelEdits edits;
  const PanelModel model = panelOf(atTheCeilingsWithRates(), "Cmd+P");
  const PanelLayout layout = context.layoutOf(model, edits);
  EXPECT_GT(ImGui::GetDrawData()->TotalVtxCount, 0);
  for (const PanelRect& rect : {layout.bar, layout.resumeAt, layout.hold, layout.memory, layout.disk, layout.apply,
                                layout.resume}) {
    EXPECT_TRUE(rect.drawn());
  }
  // The fields take the model's ceilings at the first drawing.
  EXPECT_TRUE(edits.seeded);
  EXPECT_EQ(edits.memory, "1");
  EXPECT_EQ(edits.disk, "10");
}

TEST(PlayerPanel, TheResumeButtonAnswersOnlyWhereResumeIsGranted) {
  Context context;
  PanelEdits edits;
  PanelModel model = panelOf(atTheCeilings(), "Cmd+P");
  ASSERT_FALSE(model.canResume);
  PanelLayout layout = context.layoutOf(model, edits);
  EXPECT_FALSE(context.click(model, edits, layout.resume).toggle);

  model.canResume = true;
  layout = context.layoutOf(model, edits);
  const PanelActions did = context.click(model, edits, layout.resume);
  EXPECT_TRUE(did.toggle);
  EXPECT_FALSE(did.pause);
  EXPECT_FALSE(did.limits);
}

TEST(PlayerPanel, TheHoldBoxAnswersTheSettingsWithTheHoldFlipped) {
  Context context;
  PanelEdits edits;
  const PanelModel model = panelOf(atTheCeilings(), "Cmd+P");
  const PanelLayout layout = context.layoutOf(model, edits);
  const PanelActions did = context.click(model, edits, layout.hold);
  ASSERT_TRUE(did.pause);
  EXPECT_EQ(*did.pause, (PauseSettings{.resumePercent = 50, .hold = true}));
  EXPECT_FALSE(did.toggle);
}

TEST(PlayerPanel, TheSliderAnswersTheLevelWhereItIsClicked) {
  Context context;
  PanelEdits edits;
  const PanelModel model = panelOf(atTheCeilings(), "Cmd+P");
  const PanelLayout layout = context.layoutOf(model, edits);
  // The slider's frame is the item's left part, before its label: its far left is 0 %.
  PanelRect left = layout.resumeAt;
  left.right = left.left + 2.0f;
  const PanelActions did = context.click(model, edits, left);
  ASSERT_TRUE(did.pause);
  EXPECT_EQ(did.pause->resumePercent, 0u);
  EXPECT_FALSE(did.pause->hold);
}

TEST(PlayerPanel, ApplyAnswersTheTwoSizesExactAndNothingForASizeThatDoesNotRead) {
  Context context;
  PanelEdits edits{.memory = "2", .disk = "0.5", .seeded = true};
  const PanelModel model = panelOf(atTheCeilings(), "Cmd+P");
  PanelLayout layout = context.layoutOf(model, edits);
  PanelActions did = context.click(model, edits, layout.apply);
  ASSERT_TRUE(did.limits);
  EXPECT_EQ(did.limits->memoryBytes, 2u * kGigabyte);
  EXPECT_EQ(did.limits->diskBytes, 500u * kMegabyte);

  edits = PanelEdits{.memory = "abc", .disk = "0.5", .seeded = true};
  layout = context.layoutOf(model, edits);
  did = context.click(model, edits, layout.apply);
  EXPECT_FALSE(did.limits);
  EXPECT_EQ(edits.memory, "abc");

  edits = PanelEdits{.memory = "2", .disk = "0.5.1", .seeded = true};
  layout = context.layoutOf(model, edits);
  did = context.click(model, edits, layout.apply);
  EXPECT_FALSE(did.limits);

  // The ceilings as they stand: nothing to apply.
  edits = PanelEdits{.memory = "1", .disk = "10", .seeded = true};
  layout = context.layoutOf(model, edits);
  did = context.click(model, edits, layout.apply);
  EXPECT_FALSE(did.limits);
}

// Every vertex the frame drew, as positions in order.
std::vector<float> drawnVertices() {
  std::vector<float> at;
  const ImDrawData* data = ImGui::GetDrawData();
  for (const ImDrawList* list : data->CmdLists) {
    for (const ImDrawVert& vertex : list->VtxBuffer) {
      at.push_back(vertex.pos.x);
      at.push_back(vertex.pos.y);
    }
  }
  return at;
}

TEST(PlayerPanel, AStageOfTheCloseWithNoKnownEndMovesItsMeterWithTheClock) {
  Context context;
  PanelEdits edits;
  LiveStatus status = closingAt(Work::Records);
  status.buffer.held = 0u;
  const PanelModel model = panelOf(status, "Cmd+P");
  ASSERT_TRUE(model.indeterminate);
  static_cast<void>(context.layoutOf(model, edits));
  const std::vector<float> before = drawnVertices();
  ImGui::GetIO().DeltaTime = 0.4f;
  static_cast<void>(context.frame(model, edits));
  EXPECT_NE(drawnVertices(), before);
  // A meter with a fill stands still.
  const PanelModel steady = panelOf(closingAt(Work::Watching), "Cmd+P", 16'400'000'000u);
  static_cast<void>(context.layoutOf(steady, edits));
  const std::vector<float> still = drawnVertices();
  static_cast<void>(context.frame(steady, edits));
  EXPECT_EQ(drawnVertices(), still);
}

TEST(PlayerPanel, WhileClosingTheTextAndTheMeterAreDrawnAndNoControl) {
  Context context;
  PanelEdits edits;
  const PanelModel model = panelOf(closingAt(Work::Tree), "Cmd+P");
  const PanelLayout layout = context.layoutOf(model, edits);
  EXPECT_GT(ImGui::GetDrawData()->TotalVtxCount, 0);
  EXPECT_TRUE(layout.bar.drawn());
  for (const PanelRect& rect : {layout.resumeAt, layout.hold, layout.memory, layout.disk, layout.apply,
                                layout.resume}) {
    EXPECT_FALSE(rect.drawn());
  }
}

}  // namespace
}  // namespace snaggletooth::player
