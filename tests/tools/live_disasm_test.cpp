// The cartridge disassembled beside the game as it is played, driven as the
// player drives it: the closing tree held file for file to the tree the
// disassembly writes from the same run handed back, however the game stopped
// on the way — at the ceilings at any resume level, under hold, by hand, with
// the ceilings changed — and with trees made in the middle, a reset, a second
// session, and either machine thread; the directory refused for another image;
// the two commands asked for after each tree; the close answering a request
// and ending only when its work is done; the status through a stop; the
// title's parts, its pages and the close's progress in it; and the buffer's
// directory gone at the close.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "../rom/run_test_support.h"
#include "examples/example_cartridges.h"
#include "gtest/gtest.h"
#include "player/live_disasm.h"
#include "rom/rom_disasm.h"
#include "rom/rom_observe.h"
#include "rom/rom_render.h"
#include "rom/rom_verify.h"
#include "snaggletooth/snes/snes.h"
#include "snaggletooth/snes/video_frame.h"

namespace snaggletooth::player {
namespace {

constexpr std::uint64_t kFrame = 357'954u;  // one NTSC frame of the master clock, roughly
constexpr std::uint64_t kNever = ~std::uint64_t{0} / 2u;  // a tree interval no case reaches
constexpr unsigned kStuckPasses = 20'000;                 // a stop's passes, a millisecond and more each
constexpr std::size_t kWide = 1000;                       // a title's room that holds every part
const std::string kDash = " \xE2\x80\x94 ";               // the dash between two of a title's parts, as UTF-8

std::uint64_t nowNanos() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

// A directory of this case's own, empty at the start of it.
class PlayerLiveDisassembly : public ::testing::Test {
 protected:
  void SetUp() override {
    const ::testing::TestInfo* const which = ::testing::UnitTest::GetInstance()->current_test_info();
    root_ = std::filesystem::temp_directory_path() /
            ("snaggletooth-live-" + std::string(which == nullptr ? "case" : which->name()));
    std::filesystem::remove_all(root_);
    std::filesystem::create_directories(root_);
  }
  void TearDown() override { std::filesystem::remove_all(root_); }
  std::filesystem::path root_;
};

// A player's pads: the next frame's presented at every frame the PPU finishes.
struct FramePads final : FrameObserver {
  Snes& machine;
  const disasm::InputScript& input;
  std::uint32_t count = 0;
  FramePads(Snes& m, const disasm::InputScript& i) : machine(m), input(i) {
    disasm::detail::presentPads(machine, input, 0);
  }
  void frame(const VideoFrame&) override { disasm::detail::presentPads(machine, input, ++count); }
};

SnesConfig configFor(std::span<const std::uint8_t> rom, bool audioThread, bool pictureThread) {
  SnesConfig config;
  config.rom = rom;
  config.apuThread = audioThread;
  config.ppuThread = pictureThread;
  return config;
}

// Every file under `directory`, by its path there.
std::map<std::string, std::string> filesUnder(const std::filesystem::path& directory) {
  std::map<std::string, std::string> files;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(directory)) {
    if (!entry.is_regular_file()) continue;
    std::ifstream in(entry.path(), std::ios::binary);
    files[std::filesystem::relative(entry.path(), directory).generic_string()] =
        std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
  return files;
}

void expectSameTree(const std::filesystem::path& a, const std::filesystem::path& b, const std::string& what) {
  const std::map<std::string, std::string> left = filesUnder(a);
  const std::map<std::string, std::string> right = filesUnder(b);
  ASSERT_FALSE(left.empty()) << what;
  std::vector<std::string> leftNames;
  std::vector<std::string> rightNames;
  for (const auto& [name, bytes] : left) leftNames.push_back(name);
  for (const auto& [name, bytes] : right) rightNames.push_back(name);
  ASSERT_EQ(leftNames, rightNames) << what;
  for (const auto& [name, bytes] : left) EXPECT_TRUE(bytes == right.at(name)) << what << ": " << name << " differs";
}

// How a case drives the run, on top of E1-10's loop.
struct Plan {
  std::uint64_t cycles = 12u * kFrame;
  bool audioThread = false;
  bool pictureThread = false;
  std::uint64_t resetAt = 0;  // master cycles; 0: no reset
  // After each run of the machine, before `afterRun`.
  std::function<void(LiveDisassembly&, std::uint64_t master)> afterEachRun;
  // On each pass of a stop, before `whileStopped`; `pass` counts from 0.
  std::function<void(LiveDisassembly&, Stopped why, unsigned pass)> eachStopPass;
  // Once a stop has ended, before the machine runs again.
  std::function<void(LiveDisassembly&)> afterStop;
  // On each pass of the close.
  std::function<void(LiveDisassembly&)> eachClosePass;
  // Once the close is done, with the class still standing.
  std::function<void(LiveDisassembly&)> afterClose;
  // Past `cycles`, the game plays on, a quarter frame every 4 ms, while this
  // answers true, for five minutes at most.
  std::function<bool(LiveDisassembly&)> playOn;
};

struct Lines {
  std::mutex mutex;
  std::vector<std::string> said;
  std::vector<std::string> always;
  [[nodiscard]] std::size_t count(const std::string& prefix) {
    std::lock_guard lock(mutex);
    std::size_t n = 0;
    for (const std::string& line : said) n += line.rfind(prefix, 0) == 0u ? 1u : 0u;
    for (const std::string& line : always) n += line.rfind(prefix, 0) == 0u ? 1u : 0u;
    return n;
  }
};

struct Played {
  std::optional<TreeSummary> closing;
  std::string error;
  std::size_t trees = 0;
  unsigned stopsAtLimit = 0;
  unsigned stopsByHand = 0;
};

LiveHooks hooksInto(Lines& lines, std::function<std::optional<int>(const std::vector<std::string>&, bool)> run = {}) {
  return LiveHooks{.say = [&lines](const std::string& line) {
                     std::lock_guard lock(lines.mutex);
                     lines.said.push_back(line);
                   },
                   .always = [&lines](const std::string& line) {
                     std::lock_guard lock(lines.mutex);
                     lines.always.push_back(line);
                   },
                   .run = std::move(run)};
}

// E1-10's loop over `rom`, into `directory`.
Played play(std::span<const std::uint8_t> rom, const std::filesystem::path& directory, LiveOptions options,
            LiveHooks hooks, const Plan& plan) {
  const disasm::InputScript script = disasm::exampleScript();
  Snes machine{configFor(rom, plan.audioThread, plan.pictureThread)};
  FramePads pads{machine, script};
  machine.setFrameObserver(&pads);
  Played out;
  {
    LiveDisassembly live{machine, rom, directory, std::move(options), std::move(hooks)};
    live.start();
    bool reset = false;
    const auto runOnce = [&] {
      machine.run(kFrame / 4u);
      if (plan.afterEachRun) plan.afterEachRun(live, machine.state().master);
      Stopped why = live.afterRun(nowNanos());
      if (why == Stopped::AtLimit) ++out.stopsAtLimit;
      if (why == Stopped::ByHand) ++out.stopsByHand;
      for (unsigned pass = 0; why != Stopped::No; ++pass) {
        if (plan.eachStopPass) plan.eachStopPass(live, why, pass);
        // A stop no case ends by itself in this long fails the case, and is
        // resumed so the case finishes.
        if (pass >= kStuckPasses) {
          if (pass == kStuckPasses) ADD_FAILURE() << "the game stayed stopped for " << kStuckPasses << " passes";
          live.togglePause();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const Stopped was = why;
        why = live.whileStopped(nowNanos());
        if (was != Stopped::AtLimit && why == Stopped::AtLimit) ++out.stopsAtLimit;
        if (why == Stopped::No && plan.afterStop) plan.afterStop(live);
      }
      if (plan.resetAt != 0u && !reset && machine.state().master >= plan.resetAt) {
        reset = true;
        live.reset();
      }
    };
    while (machine.state().master < plan.cycles) runOnce();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(5);
    while (plan.playOn && plan.playOn(live) && std::chrono::steady_clock::now() < deadline) {
      runOnce();
      std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
    live.beginClose();
    while (!live.closed()) {
      if (plan.eachClosePass) plan.eachClosePass(live);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    out.closing = live.closingTree(out.error);
    out.trees = live.status().trees;
    if (plan.afterClose) plan.afterClose(live);
  }
  machine.setFrameObserver(nullptr);
  return out;
}

// The tree the disassembly writes from the same run, made by an `ObservedRun`
// over the same loop and handed back as a host's run.
void referenceTree(std::span<const std::uint8_t> rom, const std::filesystem::path& directory, const Plan& plan) {
  const disasm::InputScript script = disasm::exampleScript();
  Snes machine{configFor(rom, plan.audioThread, plan.pictureThread)};
  FramePads pads{machine, script};
  machine.setFrameObserver(&pads);
  disasm::ObservedRun run{machine, rom};
  bool reset = false;
  while (machine.state().master < plan.cycles) {
    machine.run(kFrame / 4u);
    if (plan.resetAt != 0u && !reset && machine.state().master >= plan.resetAt) {
      reset = true;
      run.reset();
    }
  }
  std::vector<std::string> notes;
  const disasm::RunObservation observation = run.finish(notes);
  machine.setFrameObserver(nullptr);
  bool found = false;
  std::string error;
  std::optional<disasm::CartridgeRequest> request = disasm::treeRequest(rom, directory, found, error);
  ASSERT_TRUE(request.has_value()) << error;
  request->observed = &observation;
  request->observedNotes = notes;
  request->captureSound = true;
  const disasm::CartridgeDisassembly disassembly = disasm::disassembleCartridge(*request);
  ASSERT_TRUE(disasm::writeProject(disassembly, directory, error)) << error;
}

LiveOptions quiet(std::uint64_t memoryBytes = 1'000'000'000, std::uint64_t diskBytes = 64'000'000'000,
                  PauseSettings pause = {}) {
  return LiveOptions{.bufferDirectory = {},
                     .memoryBytes = memoryBytes,
                     .diskBytes = diskBytes,
                     .threads = 2,
                     .pause = pause,
                     .commands = {},
                     .image = {},
                     .treeIntervalNanos = kNever};
}

// The two commands, as files a test's hook stands in for.
std::filesystem::path commandsIn(const std::filesystem::path& directory, bool render, bool verify) {
  std::filesystem::create_directories(directory);
#if defined(_WIN32)
  const char* const suffix = ".exe";
#else
  const char* const suffix = "";
#endif
  if (render) std::ofstream(directory / (std::string("snes_render") + suffix)) << "";
  if (verify) std::ofstream(directory / (std::string("snes_verify") + suffix)) << "";
  return directory;
}

}  // namespace

TEST_F(PlayerLiveDisassembly, TheClosingTreeIsTheTreeOfTheSameRunHandedBack) {
  for (const std::vector<std::uint8_t>& rom : {examples::movingImage(), examples::drawingImage()}) {
    const Plan plan;
    std::filesystem::remove_all(root_ / "live");
    std::filesystem::remove_all(root_ / "reference");
    Lines lines;
    const Played played = play(rom, root_ / "live", quiet(), hooksInto(lines), plan);
    ASSERT_TRUE(played.closing.has_value()) << played.error;
    referenceTree(rom, root_ / "reference", plan);
    expectSameTree(root_ / "live", root_ / "reference", "the closing tree");
    EXPECT_EQ(played.trees, 1u) << "the closing tree alone";
    EXPECT_EQ(lines.count("tree: "), 1u);
    EXPECT_GT(played.closing->instructions, 0u);
    EXPECT_EQ(played.closing->placed, played.closing->imageBytes) << "every byte placed";
  }
}

TEST_F(PlayerLiveDisassembly, StopsAtTheCeilingsAtAnyResumeLevelLoseNothing) {
  const std::vector<std::uint8_t> rom = examples::movingImage();
  const Plan plan;
  referenceTree(rom, root_ / "reference", plan);
  for (const unsigned level : {0u, 50u, 100u}) {
    const std::string what = "resume at " + std::to_string(level);
    std::filesystem::remove_all(root_ / "live");
    Lines lines;
    const Played played = play(rom, root_ / "live", quiet(3'000'000, 0, {.resumePercent = level, .hold = false}),
                               hooksInto(lines), plan);
    ASSERT_TRUE(played.closing.has_value()) << what << ": " << played.error;
    EXPECT_GT(played.stopsAtLimit, 0u) << what;
    EXPECT_GT(lines.count("stopped: the disassembly's buffer holds "), 0u) << what;
    EXPECT_GT(lines.count("resumed after "), 0u) << what;
    expectSameTree(root_ / "live", root_ / "reference", what);
  }
}

TEST_F(PlayerLiveDisassembly, AStopUnderHoldLastsUntilTheToggle) {
  const std::vector<std::uint8_t> rom = examples::movingImage();
  const Plan reference;
  referenceTree(rom, root_ / "reference", reference);
  Plan plan;
  unsigned begun = 0;
  unsigned reachedTwenty = 0;
  std::chrono::steady_clock::time_point firstBegan;
  plan.eachStopPass = [&](LiveDisassembly& live, Stopped why, unsigned pass) {
    if (why != Stopped::AtLimit) return;
    if (pass == 0u && ++begun == 1u) firstBegan = std::chrono::steady_clock::now();
    if (pass == 20u) ++reachedTwenty;
    // The buffer drains under the level long before twenty passes of waiting;
    // under hold only the toggle resumes, asked until it is granted. The first
    // stop is held past the two seconds after which a stop says its drain.
    const bool firstHeld = begun == 1u && std::chrono::steady_clock::now() - firstBegan < std::chrono::milliseconds(2500);
    if (pass >= 20u && !firstHeld) live.togglePause();
  };
  Lines lines;
  const Played played = play(rom, root_ / "live", quiet(8'000'000, 0, {.resumePercent = 50, .hold = true}),
                             hooksInto(lines), plan);
  ASSERT_TRUE(played.closing.has_value()) << played.error;
  EXPECT_GT(begun, 0u);
  EXPECT_EQ(reachedTwenty, begun) << "every stop lasted to the toggle";
  EXPECT_EQ(lines.count("draining at "), 1u) << "a stop past two seconds says its drain, once";
  EXPECT_GT(lines.count("stopped: the disassembly's buffer holds "), 0u);
  {
    std::lock_guard lock(lines.mutex);
    const auto held = std::find_if(lines.said.begin(), lines.said.end(), [](const std::string& line) {
      return line.rfind("stopped: ", 0) == 0u;
    });
    ASSERT_NE(held, lines.said.end());
    EXPECT_NE(held->find("; the pause chord resumes it"), std::string::npos) << *held;
  }
  expectSameTree(root_ / "live", root_ / "reference", "under hold");
}

TEST_F(PlayerLiveDisassembly, AStopByHandLastsUntilTheResumeByHand) {
  const std::vector<std::uint8_t> rom = examples::movingImage();
  const Plan reference;
  referenceTree(rom, root_ / "reference", reference);
  Plan plan;
  bool toggled = false;
  unsigned longest = 0;
  plan.afterEachRun = [&](LiveDisassembly& live, std::uint64_t master) {
    if (!toggled && master >= 4u * kFrame) {
      toggled = true;
      live.togglePause();
    }
  };
  plan.eachStopPass = [&](LiveDisassembly& live, Stopped why, unsigned pass) {
    ASSERT_EQ(why, Stopped::ByHand);
    longest = std::max(longest, pass);
    if (pass == 10u) live.togglePause();
  };
  Lines lines;
  const Played played = play(rom, root_ / "live", quiet(), hooksInto(lines), plan);
  ASSERT_TRUE(played.closing.has_value()) << played.error;
  EXPECT_EQ(played.stopsByHand, 1u);
  EXPECT_EQ(longest, 10u);
  EXPECT_EQ(lines.count("stopped by hand: the buffer holds "), 1u);
  EXPECT_EQ(lines.count("resumed after "), 1u);
  expectSameTree(root_ / "live", root_ / "reference", "a stop by hand");
}

TEST_F(PlayerLiveDisassembly, AResumeAskedAtTheCeilingsIsRefusedAndSaid) {
  const std::vector<std::uint8_t> rom = examples::movingImage();
  Plan plan;
  unsigned asks = 0;
  plan.eachStopPass = [&](LiveDisassembly& live, Stopped why, unsigned pass) {
    if (why == Stopped::AtLimit && pass == 0u) {
      ++asks;
      live.togglePause();
    }
  };
  Lines lines;
  // Ceilings under the blocks two open streams are charged: at the ceilings
  // throughout, so an ask while any byte is held over a 0 level is refused,
  // and one that finds the buffer empty is granted.
  const Played played =
      play(rom, root_ / "live", quiet(3'000'000, 0, {.resumePercent = 0, .hold = false}), hooksInto(lines), plan);
  ASSERT_TRUE(played.closing.has_value()) << played.error;
  EXPECT_GT(asks, 0u);
  EXPECT_GT(lines.count("the buffer is at its ceilings; the game resumes at 0 MB"), 0u);
  EXPECT_LE(lines.count("the buffer is at its ceilings; the game resumes at 0 MB"), asks);
}

TEST_F(PlayerLiveDisassembly, LargerCeilingsResumeAStopAndSmallerOnesStopPlay) {
  const std::vector<std::uint8_t> rom = examples::movingImage();
  const Plan reference;
  referenceTree(rom, root_ / "reference", reference);
  Plan plan;
  bool raised = false;
  bool lowered = false;
  unsigned raisedStopPasses = 0;  // the passes of the stop the larger ceilings were set in
  unsigned stopsAfterLowered = 0;
  plan.eachStopPass = [&](LiveDisassembly& live, Stopped why, unsigned pass) {
    if (!raised && why == Stopped::AtLimit && pass == 0u) {
      // At 8 MB the stop comes past 4.85 MB held, over the 4 MB level; at
      // 1 GB the level is 500 MB, and the next look resumes the game.
      raised = true;
      live.setLimits(1'000'000'000, 0);
    }
    if (raised && !lowered) ++raisedStopPasses;
    if (lowered && pass == 0u) ++stopsAfterLowered;
  };
  plan.afterEachRun = [&](LiveDisassembly& live, std::uint64_t master) {
    if (raised && !lowered && master >= 8u * kFrame) {
      lowered = true;
      live.setLimits(3'000'000, 0);
    }
  };
  Lines lines;
  const Played played = play(rom, root_ / "live", quiet(8'000'000, 0), hooksInto(lines), plan);
  ASSERT_TRUE(played.closing.has_value()) << played.error;
  ASSERT_TRUE(raised);
  ASSERT_TRUE(lowered);
  EXPECT_EQ(raisedStopPasses, 1u) << "the larger ceilings resumed the stop at its next look";
  EXPECT_GT(stopsAfterLowered, 0u) << "the smaller ceilings stopped play";
  expectSameTree(root_ / "live", root_ / "reference", "ceilings changed");
}

TEST_F(PlayerLiveDisassembly, TreesInTheMiddleAssembleBackAndLeaveTheClosingTreeAsItWas) {
  const std::vector<std::uint8_t> rom = examples::movingImage();
  const Plan reference;
  referenceTree(rom, root_ / "reference", reference);
  std::mutex mutex;
  std::size_t verified = 0;
  std::size_t identical = 0;
  // The two commands, run in this process: the tree rendered, then held to the image.
  const auto run = [&](const std::vector<std::string>& arguments, bool) -> std::optional<int> {
    const std::filesystem::path directory = arguments.at(1);
    if (arguments.size() == 2u) {
      std::size_t rendered = 0;
      std::string error;
      return disasm::renderTree(directory, rendered, error) ? 0 : 1;
    }
    const disasm::VerifyReport report = disasm::verifyTree(directory, rom);
    std::lock_guard lock(mutex);
    ++verified;
    if (report.identical()) ++identical;
    return report.identical() ? 0 : 1;
  };
  LiveOptions options = quiet();
  options.commands = commandsIn(root_ / "commands", true, true);
  options.treeIntervalNanos = 0;
  // The game held still, after its first frame and its sixth, until a tree is
  // written from what it has played: the run is the reference's length.
  Plan plan;
  std::size_t waitedFor = 0;
  plan.afterEachRun = [&](LiveDisassembly& live, std::uint64_t master) {
    const std::size_t want = master >= 6u * kFrame ? 2u : master >= kFrame ? 1u : 0u;
    if (want <= waitedFor) return;
    waitedFor = want;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(2);
    while (live.status().trees < want && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  };
  Lines lines;
  const Played played = play(rom, root_ / "live", options, hooksInto(lines, run), plan);
  ASSERT_TRUE(played.closing.has_value()) << played.error;
  EXPECT_GE(played.trees, 3u) << "two in the middle and the closing tree";
  EXPECT_EQ(verified, played.trees) << "each tree verified";
  EXPECT_EQ(identical, verified) << "and each assembles back to the image";
  // The rendered bank files are the reference's after rendering it too.
  std::size_t rendered = 0;
  std::string error;
  ASSERT_TRUE(disasm::renderTree(root_ / "reference", rendered, error)) << error;
  expectSameTree(root_ / "live", root_ / "reference", "after trees in the middle");
}

TEST_F(PlayerLiveDisassembly, ATreeFromTheFirstFrameLeavesTheClosingTreeAsItWouldBe) {
  // The drawing cartridge's tree after its first quarter frame lifts its VRAM
  // as bytes the run has not seen drawn; the closing tree, from the whole run,
  // writes them as tiles and maps. The session's trees read the directory as it
  // stood when the session began, so the first names nothing for the last.
  const std::vector<std::uint8_t> rom = examples::drawingImage();
  const Plan reference;
  referenceTree(rom, root_ / "reference", reference);
  LiveOptions options = quiet();
  options.treeIntervalNanos = 0;
  Plan plan;
  bool waited = false;
  plan.afterEachRun = [&](LiveDisassembly& live, std::uint64_t) {
    if (waited) return;
    waited = true;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(2);
    while (live.status().trees == 0u && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  };
  Lines lines;
  const Played played = play(rom, root_ / "live", options, hooksInto(lines), plan);
  ASSERT_TRUE(played.closing.has_value()) << played.error;
  ASSERT_GE(played.trees, 2u) << "a tree from the first frame, and the closing tree";
  // Every file of the reference, byte for byte; a file the first tree wrote
  // under a name the closing tree does not use stays beside them, unnamed by
  // the manifest.
  const std::map<std::string, std::string> live = filesUnder(root_ / "live");
  const std::map<std::string, std::string> expected = filesUnder(root_ / "reference");
  ASSERT_FALSE(expected.empty());
  for (const auto& [name, bytes] : expected) {
    ASSERT_EQ(live.count(name), 1u) << name << " is missing";
    EXPECT_TRUE(live.at(name) == bytes) << name << " differs";
  }
  const std::string manifest = live.at("project.snagifest");
  for (const auto& [name, bytes] : live) {
    if (expected.count(name) != 0u) continue;
    EXPECT_EQ(manifest.find(name), std::string::npos) << name << " is left from the first tree, and named";
  }
}

TEST_F(PlayerLiveDisassembly, RamPastItsCeilingStopsTheGameUntilItHasDrained) {
  // No RAM and 64 MB of files: once a block is handed, RAM holds more than the
  // blocks being written and one more, with the two ceilings together far off.
  const std::vector<std::uint8_t> rom = examples::movingImage();
  const Plan reference;
  referenceTree(rom, root_ / "reference", reference);
  Plan plan;
  unsigned seen = 0;
  bool forRam = false;  // the current stop is one for RAM
  // With no RAM to drain to, a stop for RAM lasts until the buffer holds nothing.
  plan.afterStop = [&](LiveDisassembly& live) {
    if (std::exchange(forRam, false)) EXPECT_EQ(live.status().buffer.held, 0u) << "a stop for RAM ended early";
  };
  plan.eachStopPass = [&](LiveDisassembly& live, Stopped why, unsigned pass) {
    if (why != Stopped::AtLimit || pass != 0u) return;
    const LiveStatus s = live.status();
    if (!s.memoryStop) return;
    forRam = true;
    ++seen;
    EXPECT_FALSE(s.secondsToResume.has_value()) << "a stop for RAM counts to no level of the two ceilings";
    EXPECT_EQ(live.statusText().rfind("stopped, ", 0), 0u) << live.statusText();
    EXPECT_NE(live.statusText().find(" of 0 MB RAM, resumes at 0 MB"), std::string::npos) << live.statusText();
  };
  Lines lines;
  const Played played = play(rom, root_ / "live", quiet(0, 64'000'000), hooksInto(lines), plan);
  ASSERT_TRUE(played.closing.has_value()) << played.error;
  EXPECT_GT(seen, 0u) << "the game stopped for RAM";
  EXPECT_GT(lines.count("stopped: the disassembly's buffer holds "), 0u);
  {
    std::lock_guard lock(lines.mutex);
    std::size_t forRam = 0;
    for (const std::string& line : lines.said) {
      if (line.rfind("stopped: ", 0) != 0u) continue;
      EXPECT_NE(line.find(" of RAM, past its 0 MB ceiling, while its files are written; the game resumes at 0 MB of RAM"),
                std::string::npos)
          << line;
      ++forRam;
    }
    EXPECT_GT(forRam, 0u);
  }
  expectSameTree(root_ / "live", root_ / "reference", "stops for RAM");
}

TEST_F(PlayerLiveDisassembly, TheTitleSaysTheBuffersSizesAndTurnsWhileTheWorkMoves) {
  constexpr std::uint64_t kSecond = 1'000'000'000u;
  // The ten frames of the spinner, as UTF-8.
  const std::array<std::string, 10> frames = {"\xE2\xA0\x8B", "\xE2\xA0\x99", "\xE2\xA0\xB9", "\xE2\xA0\xB8",
                                              "\xE2\xA0\xBC", "\xE2\xA0\xB4", "\xE2\xA0\xA6", "\xE2\xA0\xA7",
                                              "\xE2\xA0\x87", "\xE2\xA0\x8F"};
  LiveStatus s;
  s.buffer.memoryUsed = 412'000'000u;
  s.buffer.memoryBytes = 1'000'000'000u;
  s.buffer.diskUsed = 0u;
  s.buffer.diskBytes = 64'000'000'000u;
  s.buffer.playedMaster = 1000u;
  LiveTitle title;
  const std::string sizes = "RAM 412 MB of 1.0 GB" + kDash + "files 0 MB of 64.0 GB ";
  EXPECT_EQ(title.text("", s, 0u, kWide), sizes + frames[0]) << "the first frame";
  EXPECT_EQ(title.text("", s, kSecond / 2u, kWide), sizes + frames[0]) << "nothing moved: the spinner stands";
  s.buffer.playedMaster = 2000u;
  EXPECT_EQ(title.text("", s, kSecond, kWide), sizes + frames[1]) << "the threads played on";
  s.buffer.held = 5u;
  EXPECT_EQ(title.text("", s, kSecond, kWide), sizes + frames[2]) << "the buffer moved";
  s.trees = 1u;
  EXPECT_EQ(title.text("", s, kSecond, kWide), sizes + frames[3]) << "a tree was written";
  s.work = Work::Render;
  EXPECT_EQ(title.text("", s, kSecond, kWide), sizes + frames[4]) << "a command is running over the tree";
  EXPECT_EQ(title.text("", s, kSecond, kWide), sizes + frames[5]) << "and goes on turning while it runs";
  s.work = Work::Watching;
  s.buffer.memoryUsed = 999'000'000u;
  s.buffer.diskUsed = 2'500'000'000u;
  EXPECT_EQ(title.text("", s, kSecond + kSecond / 2u, kWide), sizes + frames[5]) << "the sizes stand for two seconds";
  EXPECT_EQ(title.text("", s, 2u * kSecond, kWide),
            "RAM 999 MB of 1.0 GB" + kDash + "files 2.5 GB of 64.0 GB " + frames[5])
      << "and are taken again after them";
  EXPECT_EQ(title.text("60.1 fps", s, 2u * kSecond, kWide),
            "60.1 fps" + kDash + "RAM 999 MB of 1.0 GB" + kDash + "files 2.5 GB of 64.0 GB " + frames[5])
      << "the lead comes first";
  for (unsigned step = 6; step < 12; ++step) {
    ++s.buffer.playedMaster;
    const std::string text = title.text("", s, 2u * kSecond, kWide);
    EXPECT_EQ(text.substr(text.size() - 3u), frames[step % 10u]) << "step " << step;
  }
}

TEST_F(PlayerLiveDisassembly, TheTitleIsAPageAtATimeWhereTheRoomDoesNotHoldIt) {
  constexpr std::uint64_t kSecond = 1'000'000'000u;
  const std::string dot = " \xE2\xA0\x8B";  // nothing moves here, so the spinner stands at its first frame
  LiveStatus s;
  s.buffer.memoryUsed = 412'000'000u;
  s.buffer.memoryBytes = 1'000'000'000u;
  s.buffer.diskBytes = 64'000'000'000u;
  const std::string lead = "60.1 fps";            // 8 characters
  const std::string ram = "RAM 412 MB of 1.0 GB";  // 20
  const std::string files = "files 0 MB of 64.0 GB";  // 21
  // The three, two dashes of three and the spinner's two: 57 characters.
  {
    LiveTitle title;
    const std::string whole = lead + kDash + ram + kDash + files + dot;
    EXPECT_EQ(title.text(lead, s, 0u, 57u), whole) << "room for all of it";
    EXPECT_EQ(title.text(lead, s, 4u * kSecond, 57u), whole) << "one page is every page";
    EXPECT_EQ(title.text(lead, s, 8u * kSecond, 57u), whole);
  }
  {
    LiveTitle title;
    EXPECT_EQ(title.text(lead, s, 0u, 56u), lead + kDash + ram + dot) << "one character short: the files wait";
    EXPECT_EQ(title.text(lead, s, 4u * kSecond - 1u, 56u), lead + kDash + ram + dot) << "a page lasts four seconds";
    EXPECT_EQ(title.text(lead, s, 4u * kSecond, 56u), files + dot) << "then the next";
    EXPECT_EQ(title.text(lead, s, 8u * kSecond - 1u, 56u), files + dot);
    EXPECT_EQ(title.text(lead, s, 8u * kSecond, 56u), lead + kDash + ram + dot) << "the first follows the last";
  }
  {
    // The lead, a dash, the RAM and the spinner: 33 characters.
    LiveTitle fits;
    EXPECT_EQ(fits.text(lead, s, 0u, 33u), lead + kDash + ram + dot);
    LiveTitle title;
    EXPECT_EQ(title.text(lead, s, 0u, 32u), lead + dot) << "one character short of two parts";
    EXPECT_EQ(title.text(lead, s, 4u * kSecond, 32u), ram + dot);
    EXPECT_EQ(title.text(lead, s, 8u * kSecond, 32u), files + dot);
    EXPECT_EQ(title.text(lead, s, 12u * kSecond, 32u), lead + dot);
  }
  {
    LiveTitle title;
    EXPECT_EQ(title.text(lead, s, 0u, 0u), lead + dot) << "a page holds one part however little room there is";
    EXPECT_EQ(title.text(lead, s, 4u * kSecond, 0u), ram + dot);
    EXPECT_EQ(title.text(lead, s, 8u * kSecond, 0u), files + dot);
    EXPECT_EQ(title.text("", s, 9u * kSecond, 0u), ram + dot) << "a lead gone from the last page leaves the first";
  }
  {
    LiveTitle title;
    EXPECT_EQ(title.text("", s, 0u, 0u), ram + dot) << "no lead: the RAM is the first page";
    EXPECT_EQ(title.text("", s, 4u * kSecond, 0u), files + dot);
    EXPECT_EQ(title.text("", s, 8u * kSecond, 0u), ram + dot);
  }
  {
    // Eight characters of two bytes each take the room of eight.
    const std::string accented = "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9";
    LiveTitle title;
    EXPECT_EQ(title.text(accented, s, 0u, 33u), accented + kDash + ram + dot) << "the room is counted in characters";
  }
}

TEST_F(PlayerLiveDisassembly, TheTitleShowsTheLeadAtOnceWhenWhatItSaysChanges) {
  constexpr std::uint64_t kSecond = 1'000'000'000u;
  // The text without its spinner, which turns as the buffer moves.
  const auto page = [](const std::string& text) { return text.substr(0, text.size() - 4u); };
  LiveStatus s;
  s.buffer.memoryUsed = 412'000'000u;
  s.buffer.memoryBytes = 1'000'000'000u;
  s.buffer.diskBytes = 64'000'000'000u;
  const std::string ram = "RAM 412 MB of 1.0 GB";
  LiveTitle title;
  // No room: a part a page.
  EXPECT_EQ(page(title.text("60.1 fps", s, 0u, 0u)), "60.1 fps");
  EXPECT_EQ(page(title.text("60.1 fps", s, 4u * kSecond, 0u)), ram);
  s.stopped = Stopped::AtLimit;
  EXPECT_EQ(page(title.text("stopped", s, 5u * kSecond, 0u)), "stopped") << "the game stopped";
  EXPECT_EQ(page(title.text("stopped", s, 9u * kSecond - 1u, 0u)), "stopped") << "and its page lasts its four seconds";
  EXPECT_EQ(page(title.text("stopped", s, 9u * kSecond, 0u)), ram);
  s.stopped = Stopped::ByHand;
  EXPECT_EQ(page(title.text("by hand", s, 10u * kSecond, 0u)), "by hand") << "the stop became one by hand";
  EXPECT_EQ(page(title.text("by hand", s, 14u * kSecond, 0u)), ram);
  s.stopped = Stopped::No;
  EXPECT_EQ(page(title.text("59.9 fps", s, 15u * kSecond, 0u)), "59.9 fps") << "the game resumed";
  EXPECT_EQ(page(title.text("60.0 fps", s, 16u * kSecond, 0u)), "60.0 fps") << "a lead that only reads differently";
  EXPECT_EQ(page(title.text("60.0 fps", s, 19u * kSecond, 0u)), ram) << "leaves the page its four seconds";
  s.closing = true;
  s.buffer.held = 1'000'000'000u;
  EXPECT_EQ(page(title.text("", s, 20u * kSecond, 0u)), "finishing: 0 MB of 1.0 GB played, 0 %") << "the close began";
  EXPECT_EQ(page(title.text("", s, 24u * kSecond, 0u)), ram);
  s.buffer.held = 500'000'000u;
  EXPECT_EQ(page(title.text("", s, 25u * kSecond, 0u)), ram) << "the buffer draining is the same stage";
  s.work = Work::Tree;
  EXPECT_EQ(page(title.text("", s, 26u * kSecond, 0u)), ram) << "and so is a tree from the middle of the session";
  s.work = Work::Watching;
  s.buffer.held = 0u;
  EXPECT_EQ(page(title.text("", s, 27u * kSecond, 0u)), "finishing: 1.0 GB of 1.0 GB played, 100 %")
      << "the buffer is played";
  static_cast<void>(title.text("", s, 31u * kSecond, 0u));
  s.work = Work::Tree;
  EXPECT_EQ(page(title.text("", s, 32u * kSecond, 0u)), "finishing: writing the last tree");
  static_cast<void>(title.text("", s, 36u * kSecond, 0u));
  s.work = Work::Render;
  EXPECT_EQ(page(title.text("", s, 37u * kSecond, 0u)), "finishing: snes_render");
  static_cast<void>(title.text("", s, 41u * kSecond, 0u));
  s.work = Work::Verify;
  EXPECT_EQ(page(title.text("", s, 42u * kSecond, 0u)), "finishing: snes_verify");
  static_cast<void>(title.text("", s, 46u * kSecond, 0u));
  s.work = Work::Done;
  EXPECT_EQ(page(title.text("", s, 47u * kSecond, 0u)), "finishing: closing the buffer");
}

TEST_F(PlayerLiveDisassembly, TheClosesProgressCountsFromTheMostTheBufferHeld) {
  // What the title says first, with room for all of it.
  const auto lead = [](const std::string& text) { return text.substr(0, text.find(kDash)); };
  LiveStatus s;
  s.buffer.memoryBytes = 1'000'000'000u;
  s.buffer.diskBytes = 64'000'000'000u;
  LiveTitle title;
  // What the buffer held while the game ran is no part of the close.
  s.buffer.held = 50'000'000'000u;
  EXPECT_EQ(lead(title.text("60.1 fps", s, 0u, kWide)), "60.1 fps");
  s.closing = true;
  s.buffer.held = 12'400'000'000u;
  EXPECT_EQ(lead(title.text("60.1 fps", s, 1u, kWide)), "finishing: 0 MB of 12.4 GB played, 0 %")
      << "the lead handed is not read while the close works";
  s.buffer.held = 9'200'000'000u;
  s.secondsToEmpty = 100.0;
  EXPECT_EQ(lead(title.text("", s, 2u, kWide)), "finishing: 3.2 GB of 12.4 GB played, 25 %, about 1 min 40 s left");
  s.buffer.held = 12'500'000'000u;
  EXPECT_EQ(lead(title.text("", s, 3u, kWide)), "finishing: 0 MB of 12.5 GB played, 0 %, about 1 min 40 s left")
      << "the buffer took its last blocks after the close began";
  s.buffer.held = 6'250'000'000u;
  s.secondsToEmpty.reset();
  EXPECT_EQ(lead(title.text("", s, 4u, kWide)), "finishing: 6.2 GB of 12.5 GB played, 50 %") << "no rate yet: no time";
  s.buffer.held = 0u;
  s.secondsToEmpty = 0.0;
  EXPECT_EQ(lead(title.text("", s, 5u, kWide)), "finishing: 12.5 GB of 12.5 GB played, 100 %")
      << "nothing is left to time";
  s.work = Work::Tree;
  EXPECT_EQ(lead(title.text("", s, 6u, kWide)), "finishing: writing the last tree");
  s.work = Work::Render;
  EXPECT_EQ(lead(title.text("", s, 7u, kWide)), "finishing: snes_render");
  s.work = Work::Verify;
  EXPECT_EQ(lead(title.text("", s, 8u, kWide)), "finishing: snes_verify");
  s.work = Work::Done;
  EXPECT_EQ(lead(title.text("", s, 9u, kWide)), "finishing: closing the buffer");

  // A tree from the middle of the session, still being made and checked while
  // the buffer is played: the buffer is what the close has left.
  LiveTitle middle;
  LiveStatus m;
  m.closing = true;
  m.buffer.held = 1'000'000'000u;
  m.work = Work::Tree;
  EXPECT_EQ(lead(middle.text("", m, 0u, kWide)), "finishing: 0 MB of 1.0 GB played, 0 %");
  m.buffer.held = 500'000'000u;
  m.work = Work::Render;
  EXPECT_EQ(lead(middle.text("", m, 1u, kWide)), "finishing: 500 MB of 1.0 GB played, 50 %");
  m.work = Work::Verify;
  EXPECT_EQ(lead(middle.text("", m, 2u, kWide)), "finishing: 500 MB of 1.0 GB played, 50 %");

  // A close with nothing in the buffer.
  LiveTitle empty;
  LiveStatus e;
  e.closing = true;
  EXPECT_EQ(lead(empty.text("", e, 0u, kWide)), "finishing: nothing left to play");
}

TEST_F(PlayerLiveDisassembly, TheTitlesRoomIsItsBarInCharactersLessTheNameAndItsDash) {
  EXPECT_EQ(titleCharacters(""), 0u);
  EXPECT_EQ(titleCharacters("Game"), 4u);
  // An e with an acute accent, a dash and a braille dot: two bytes, three and three.
  EXPECT_EQ(titleCharacters("\xC3\xA9\xE2\x80\x94\xE2\xA0\x8B"), 3u);
  EXPECT_EQ(titleRoom(700.0, 7.0, "Game"), 93u) << "a hundred characters, less the name's four and the dash's three";
  EXPECT_EQ(titleRoom(699.0, 7.0, "Game"), 92u) << "a part of a character is none";
  EXPECT_EQ(titleRoom(800.0, 8.0, "Game"), 93u);
  EXPECT_EQ(titleRoom(700.0, 7.0, "\xC3\xA9"), 96u) << "the name is counted in characters";
  EXPECT_EQ(titleRoom(77.0, 7.0, "Game"), 4u);
  EXPECT_EQ(titleRoom(49.0, 7.0, "Game"), 0u) << "the name and its dash fill the bar";
  EXPECT_EQ(titleRoom(42.0, 7.0, "Game"), 0u) << "and overflow it";
  EXPECT_EQ(titleRoom(0.0, 7.0, "Game"), 0u);
  EXPECT_EQ(titleRoom(-700.0, 7.0, ""), 0u);
  EXPECT_EQ(titleRoom(700.0, 0.0, ""), 0u);
  EXPECT_EQ(titleRoom(700.0, -7.0, ""), 0u);
  EXPECT_EQ(titleRoom(1e30, 1.0, ""), 999'999'997u) << "a width past any window's is a billion characters";
}

TEST_F(PlayerLiveDisassembly, ANewTreeBeginsOnlyWhenTheRunHasFoundMore) {
  // A cartridge that spins in one place from its first instruction.
  std::vector<std::uint8_t> idle = examples::loRomImage(1);
  examples::put(idle, 0x0000u, {0x80u, 0xFEu});  // $8000 BRA $8000
  LiveOptions options = quiet();
  options.treeIntervalNanos = 20'000'000;  // a look every 20 ms
  Plan plan;
  plan.cycles = 30u * kFrame;
  std::size_t treesWhileIdle = 0;
  plan.afterEachRun = [&](LiveDisassembly& live, std::uint64_t master) {
    if (master >= 20u * kFrame && treesWhileIdle == 0u) {
      // Let the thread look several times with nothing new found.
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
      while (live.status().trees == 0u && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
      // A look every 20 ms for a second, and none begins a tree.
      const auto idleUntil = std::chrono::steady_clock::now() + std::chrono::seconds(1);
      while (std::chrono::steady_clock::now() < idleUntil) {
        EXPECT_EQ(live.status().work, Work::Watching) << "a look with nothing new found began a tree";
        if (live.status().work != Work::Watching) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      treesWhileIdle = live.status().trees;
    }
  };
  Lines lines;
  const Played played = play(idle, root_ / "live", options, hooksInto(lines), plan);
  ASSERT_TRUE(played.closing.has_value()) << played.error;
  EXPECT_EQ(treesWhileIdle, 1u) << "the looks after the first found nothing new";

  // The moving cartridge finds more frame after frame: a tree in the middle,
  // then another once more is found.
  std::filesystem::remove_all(root_ / "live");
  Plan moving;
  moving.cycles = 2u * kFrame;
  moving.playOn = [](LiveDisassembly& live) { return live.status().trees < 2u; };
  LiveOptions often = quiet();
  often.treeIntervalNanos = 0;
  const Played more = play(examples::movingImage(), root_ / "live", often, hooksInto(lines), moving);
  ASSERT_TRUE(more.closing.has_value()) << more.error;
  EXPECT_GE(more.trees, 3u);
}

TEST_F(PlayerLiveDisassembly, AResetInTheMiddleIsTheReferencesReset) {
  const std::vector<std::uint8_t> rom = disasm::resettingImage();
  Plan plan;
  plan.cycles = 6u * kFrame;
  plan.resetAt = 3u * kFrame;
  referenceTree(rom, root_ / "reference", plan);
  Lines lines;
  const Played played = play(rom, root_ / "live", quiet(), hooksInto(lines), plan);
  ASSERT_TRUE(played.closing.has_value()) << played.error;
  expectSameTree(root_ / "live", root_ / "reference", "a reset in the middle");
}

TEST_F(PlayerLiveDisassembly, ASecondSessionKeepsWhatTheFirstReached) {
  // The dispatching cartridge jumps through a table its run reads.
  const std::vector<std::uint8_t> rom = examples::dispatchingImage();
  Lines lines;
  const Played first = play(rom, root_ / "live", quiet(), hooksInto(lines), Plan{});
  ASSERT_TRUE(first.closing.has_value()) << first.error;
  const auto reachedIn = [&](const std::filesystem::path& directory) {
    std::ifstream in(directory / "project.snagifest", std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::string error;
    const std::optional<disasm::ManifestInput> manifest = disasm::parseManifest(text, error);
    EXPECT_TRUE(manifest.has_value()) << error;
    std::vector<disasm::Address> targets;
    if (manifest) {
      for (const disasm::ReachedTarget& r : manifest->reached) targets.push_back(r.target);
    }
    return targets;
  };
  const std::vector<disasm::Address> before = reachedIn(root_ / "live");
  ASSERT_FALSE(before.empty()) << "the first session reached indirect targets";
  bool found = false;
  std::size_t entries = 0;
  std::size_t files = 0;
  std::string error;
  ASSERT_TRUE(LiveDisassembly::readable(rom, root_ / "live", found, entries, files, error)) << error;
  EXPECT_TRUE(found);
  EXPECT_GT(files, 0u);
  // A shorter second session, under the same script.
  Plan shorter;
  shorter.cycles = 2u * kFrame;
  const Played second = play(rom, root_ / "live", quiet(), hooksInto(lines), shorter);
  ASSERT_TRUE(second.closing.has_value()) << second.error;
  const std::vector<disasm::Address> after = reachedIn(root_ / "live");
  for (const disasm::Address target : before) {
    EXPECT_NE(std::find(after.begin(), after.end(), target), after.end()) << "kept: " << target;
  }
}

TEST_F(PlayerLiveDisassembly, ADirectoryWrittenForAnotherImageIsRefused) {
  Lines lines;
  Plan plan;
  plan.cycles = kFrame;
  const Played played = play(examples::movingImage(), root_ / "live", quiet(), hooksInto(lines), plan);
  ASSERT_TRUE(played.closing.has_value()) << played.error;
  bool found = false;
  std::size_t entries = 0;
  std::size_t files = 0;
  std::string error;
  // An image of three banks, where the tree was written for one of one bank.
  EXPECT_FALSE(LiveDisassembly::readable(examples::threeBankImage(), root_ / "live", found, entries, files, error));
  EXPECT_TRUE(found);
  EXPECT_NE(error.find((root_ / "live" / "project.snagifest").string()), std::string::npos) << error;
  EXPECT_TRUE(LiveDisassembly::readable(examples::threeBankImage(), root_ / "empty", found, entries, files, error));
  EXPECT_FALSE(found) << "a directory with no manifest takes any image";
}

TEST_F(PlayerLiveDisassembly, TheTwoCommandsRunAfterEachTreeLoweredWhileTheGamePlays) {
  const std::vector<std::uint8_t> rom = examples::movingImage();
  struct Asked {
    std::vector<std::string> arguments;
    bool lowered = false;
  };
  std::mutex mutex;
  std::vector<Asked> asked;
  const auto run = [&](const std::vector<std::string>& arguments, bool lowered) -> std::optional<int> {
    std::lock_guard lock(mutex);
    asked.push_back(Asked{.arguments = arguments, .lowered = lowered});
    return 0;
  };
  LiveOptions options = quiet();
  options.commands = commandsIn(root_ / "commands", true, true);
  options.image = root_ / "game.sfc";
  options.treeIntervalNanos = 0;
  Plan plan;
  plan.cycles = 2u * kFrame;
  // Playing until a tree in the middle has had both its commands run.
  plan.playOn = [&](LiveDisassembly&) {
    std::lock_guard lock(mutex);
    return asked.size() < 2u;
  };
  Lines lines;
  const Played played = play(rom, root_ / "live", options, hooksInto(lines, run), plan);
  ASSERT_TRUE(played.closing.has_value()) << played.error;
  ASSERT_EQ(asked.size(), 2u * played.trees);
  ASSERT_GE(played.trees, 2u);
  const std::string live = (root_ / "live").string();
  for (std::size_t i = 0; i < asked.size(); i += 2u) {
    EXPECT_NE(asked[i].arguments.at(0).find("snes_render"), std::string::npos);
    EXPECT_EQ(asked[i].arguments, (std::vector<std::string>{asked[i].arguments.at(0), live}));
    EXPECT_NE(asked[i + 1u].arguments.at(0).find("snes_verify"), std::string::npos);
    EXPECT_EQ(asked[i + 1u].arguments,
              (std::vector<std::string>{asked[i + 1u].arguments.at(0), live, options.image.string()}));
  }
  EXPECT_TRUE(asked.front().lowered) << "a tree in the middle, the game playing";
  EXPECT_FALSE(asked.back().lowered) << "the closing tree's";
}

TEST_F(PlayerLiveDisassembly, AnAbsentCommandIsSaidOnceAndAFailingOneEachTime) {
  const std::vector<std::uint8_t> rom = examples::movingImage();
  std::atomic<unsigned> runs{0};
  const auto run = [&](const std::vector<std::string>&, bool) -> std::optional<int> {
    ++runs;
    return 3;
  };
  LiveOptions options = quiet();
  options.commands = commandsIn(root_ / "commands", false, true);  // snes_render is not there
  options.image = root_ / "game.sfc";
  options.treeIntervalNanos = 0;
  Plan plan;
  plan.cycles = 2u * kFrame;
  plan.playOn = [&](LiveDisassembly&) { return runs.load() < 2u; };
  Lines lines;
  const Played played = play(rom, root_ / "live", options, hooksInto(lines, run), plan);
  ASSERT_TRUE(played.closing.has_value()) << played.error;
  EXPECT_GE(played.trees, 3u);
  EXPECT_EQ(runs.load(), played.trees) << "snes_verify alone ran, after each tree";
  EXPECT_EQ(lines.count("snes_render is not beside the player; run it over the tree by hand: snes_render " +
                        (root_ / "live").string()),
            1u);
  EXPECT_EQ(lines.count("snes_verify exited 3"), played.trees);
}

TEST_F(PlayerLiveDisassembly, ARequestToCloseDuringTheCloseIsAnsweredAndEndsNothing) {
  const std::vector<std::uint8_t> rom = examples::movingImage();
  std::atomic<bool> release{false};
  std::atomic<bool> inside{false};
  const auto run = [&](const std::vector<std::string>& arguments, bool) -> std::optional<int> {
    if (arguments.size() == 2u) {
      inside = true;
      while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return 0;
  };
  LiveOptions options = quiet();
  options.commands = commandsIn(root_ / "commands", true, true);
  Plan plan;
  plan.cycles = 2u * kFrame;
  unsigned answered = 0;
  plan.eachClosePass = [&](LiveDisassembly& live) {
    if (inside && answered < 2u) {
      ++answered;
      live.closeAsked("kill -9 4242");
      EXPECT_FALSE(live.closed()) << "a request ends nothing";
      EXPECT_EQ(live.statusText(), "finishing the disassembly, snes_render is running");
      if (answered == 2u) release = true;
    }
  };
  Lines lines;
  const Played played = play(rom, root_ / "live", options, hooksInto(lines, run), plan);
  ASSERT_TRUE(played.closing.has_value()) << played.error;
  EXPECT_EQ(answered, 2u);
  EXPECT_EQ(lines.count("still finishing the disassembly: snes_render is running. It ends by itself when that is "
                        "done; kill -9 4242 ends it without the last tree"),
            2u);
}

TEST_F(PlayerLiveDisassembly, TheStatusSaysWhereTheStopStands) {
  const std::vector<std::uint8_t> rom = examples::movingImage();
  Plan plan;
  bool seen = false;
  bool raisedSeen = false;
  bool loweredAgain = false;
  // Within a wait of `done`.
  const auto soon = [&](auto done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!done() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return done();
  };
  plan.afterStop = [&](LiveDisassembly& live) {
    if (!seen || loweredAgain) return;
    loweredAgain = true;
    // The pool's two, the shadow and sound threads and the two streams', lowered again.
    EXPECT_TRUE(soon([&] { return live.status().buffer.threadsLowered == 6u; })) << "the run lowered once the game runs";
  };
  plan.eachStopPass = [&](LiveDisassembly& live, Stopped why, unsigned pass) {
    if (seen || why != Stopped::AtLimit) return;
    if (!raisedSeen) {
      raisedSeen = true;
      EXPECT_TRUE(soon([&] { return live.status().buffer.threadsLowered == 0u; })) << "the run raised while stopped";
    }
    const LiveStatus s = live.status();
    EXPECT_EQ(s.stopped, Stopped::AtLimit);
    EXPECT_FALSE(s.closing);
    EXPECT_EQ(s.ceiling, 3'000'000u);
    EXPECT_EQ(s.resumeLevel, 1'500'000u);
    EXPECT_FALSE(s.secondsToResume.has_value()) << "under hold the stop has no end to count to";
    EXPECT_EQ(s.pause, (PauseSettings{.resumePercent = 50, .hold = true}));
    EXPECT_EQ(s.buffer.threads, 2u);
    EXPECT_EQ(live.statusText().rfind("stopped, ", 0), 0u) << live.statusText();
    EXPECT_NE(live.statusText().find(", the pause chord resumes"), std::string::npos) << live.statusText();
    if (pass >= 5u) {
      seen = true;
      live.setPauseSettings({.resumePercent = 50, .hold = false});
      const LiveStatus free = live.status();
      EXPECT_FALSE(free.pause.hold);
      ASSERT_TRUE(free.secondsToResume.has_value() || free.buffer.held > free.resumeLevel);
      live.togglePause();
    }
  };
  plan.afterEachRun = [&](LiveDisassembly& live, std::uint64_t) {
    if (live.status().stopped == Stopped::No) {
      EXPECT_EQ(live.statusText(), "") << "nothing to say while the game runs";
    }
  };
  Lines lines;
  const Played played = play(rom, root_ / "live", quiet(3'000'000, 0, {.resumePercent = 50, .hold = true}),
                             hooksInto(lines), plan);
  ASSERT_TRUE(played.closing.has_value()) << played.error;
  EXPECT_TRUE(seen);
}

TEST_F(PlayerLiveDisassembly, TheBuffersDirectoryGoesAtTheClose) {
  const std::vector<std::uint8_t> rom = examples::movingImage();
  Plan plan;
  bool there = false;
  plan.afterEachRun = [&](LiveDisassembly&, std::uint64_t) {
    there = there || std::filesystem::exists(root_ / "live" / "buffer");
  };
  bool goneAtClose = false;
  plan.afterClose = [&](LiveDisassembly&) { goneAtClose = !std::filesystem::exists(root_ / "live" / "buffer"); };
  Lines lines;
  const Played played = play(rom, root_ / "live", quiet(), hooksInto(lines), plan);
  ASSERT_TRUE(played.closing.has_value()) << played.error;
  EXPECT_TRUE(there) << "the buffer is under the tree while the game plays";
  EXPECT_TRUE(goneAtClose) << "the close removes it, before the class goes";
}

TEST_F(PlayerLiveDisassembly, TheTreeIsTheSameUnderEitherMachineThreadAndBoth) {
  const std::vector<std::uint8_t> rom = examples::drawingImage();
  referenceTree(rom, root_ / "reference", Plan{});
  for (const bool audioThread : {false, true}) {
    for (const bool pictureThread : {false, true}) {
      if (!audioThread && !pictureThread) continue;
      const std::string what = std::string("audio thread ") + (audioThread ? "on" : "off") + ", picture thread " +
                               (pictureThread ? "on" : "off");
      std::filesystem::remove_all(root_ / "live");
      Plan plan;
      plan.audioThread = audioThread;
      plan.pictureThread = pictureThread;
      Lines lines;
      const Played played = play(rom, root_ / "live", quiet(), hooksInto(lines), plan);
      ASSERT_TRUE(played.closing.has_value()) << what << ": " << played.error;
      expectSameTree(root_ / "live", root_ / "reference", what);
    }
  }
}

}  // namespace snaggletooth::player
