#include "player/live_disasm.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <process.h>
#include <windows.h>
#else
#include <pthread.h>
#endif

#include "rom/rom_disasm.h"
#include "rom/rom_observe.h"

namespace snaggletooth::player {

namespace {

// A thread with the stack a disassembly takes: it is written for a process's
// first thread, which has 8 MB on macOS and Linux, where a thread the process
// starts has 512 KB on macOS and 1 MB on Windows.
class TreeThread {
 public:
  static constexpr std::size_t kStackBytes = std::size_t{8} << 20;

  void start(std::function<void()> body) {
    body_ = std::move(body);
#if defined(_WIN32)
    handle_ = reinterpret_cast<HANDLE>(_beginthreadex(nullptr, static_cast<unsigned>(kStackBytes), &TreeThread::enter,
                                                      this, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr));
    if (handle_ == nullptr) throw std::runtime_error("the tree's thread could not be started");
#else
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, kStackBytes);
    const int failed = pthread_create(&thread_, &attributes, &TreeThread::enter, this);
    pthread_attr_destroy(&attributes);
    if (failed != 0) throw std::runtime_error("the tree's thread could not be started");
#endif
    started_ = true;
  }

  void join() {
    if (!started_) return;
    started_ = false;
#if defined(_WIN32)
    WaitForSingleObject(handle_, INFINITE);
    CloseHandle(handle_);
#else
    pthread_join(thread_, nullptr);
#endif
  }

 private:
#if defined(_WIN32)
  static unsigned __stdcall enter(void* self) {
    static_cast<TreeThread*>(self)->body_();
    return 0;
  }
  HANDLE handle_ = nullptr;
#else
  static void* enter(void* self) {
    static_cast<TreeThread*>(self)->body_();
    return nullptr;
  }
  pthread_t thread_{};
#endif
  std::function<void()> body_;
  bool started_ = false;
};

constexpr std::uint64_t kNanosPerSecond = 1'000'000'000u;
constexpr std::uint64_t kDrainLineNanos = 2u * kNanosPerSecond;  // when a stop at the ceilings says its drain
constexpr auto kCloseLook = std::chrono::milliseconds(100);      // how often the close looks at the drain
// The longest wait for the next look, so a wait added to the clock stays in its range.
constexpr std::uint64_t kLongestIntervalNanos = 365ull * 24u * 3600u * kNanosPerSecond;

std::uint64_t steadyNanos() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

// The counts of what a run has found, which the next tree begins on.
struct Found {
  std::size_t reached = 0;
  std::size_t moved = 0;
  std::size_t landed = 0;
  std::size_t walked = 0;
  std::size_t palettes = 0;
  std::size_t ran = 0;
  std::size_t seen = 0;
  std::size_t staged = 0;
  std::size_t streamed = 0;
  std::size_t samples = 0;
  std::size_t nodes = 0;
  std::size_t spc700Nodes = 0;
  std::size_t originSets = 0;
  std::uint64_t divergences = 0;
  std::uint64_t spc700Divergences = 0;
  [[nodiscard]] bool operator==(const Found&) const = default;
};

Found foundIn(const disasm::RunObservation& o) {
  return Found{.reached = o.reached.size(),
               .moved = o.moved.size(),
               .landed = o.landed.size(),
               .walked = o.walked.size(),
               .palettes = o.palettes.size(),
               .ran = o.ran.size(),
               .seen = o.seen.size(),
               .staged = o.staged.size(),
               .streamed = o.streamed.size(),
               .samples = o.samples.size(),
               .nodes = o.nodes,
               .spc700Nodes = o.spc700Nodes,
               .originSets = o.originSets,
               .divergences = o.divergences,
               .spc700Divergences = o.spc700Divergences};
}

TreeSummary summaryOf(const disasm::CartridgeDisassembly& disassembly, std::uint64_t playedMaster) {
  std::size_t instructions = 0;
  for (const disasm::RegionListing& region : disassembly.regions) {
    for (const disasm::Line& line : region.listing.lines) {
      if (line.isCode) ++instructions;
    }
  }
  const disasm::Placement placement = disasm::placeBytes(disassembly);
  return TreeSummary{.files = disassembly.regions.size(),
                     .instructions = instructions,
                     .entries = disassembly.entries.size(),
                     .stops = disassembly.stops.size(),
                     .placed = placement.image.size() - placement.unplaced,
                     .imageBytes = placement.image.size(),
                     .notes = disassembly.notes.size(),
                     .playedMaster = playedMaster};
}

// The two commands run over each tree, in order.
constexpr std::array<const char*, 2> kCommands = {"snes_render", "snes_verify"};

std::filesystem::path commandFile(const std::filesystem::path& directory, const char* name) {
#if defined(_WIN32)
  return directory / (std::string(name) + ".exe");
#else
  return directory / name;
#endif
}

}  // namespace

struct LiveDisassembly::Impl {
  Snes& machine;
  std::span<const std::uint8_t> rom;
  std::filesystem::path directory;
  LiveOptions options;
  LiveHooks hooks;
  std::unique_ptr<disasm::BufferedRun> run;

  // The host's thread's own; `drain` is the tree's thread's once the close begins.
  PauseControl pause;
  BufferRate fill;
  BufferRate drain;
  bool asked = false;  // a toggle since the last look, as the pause control counts it
  std::uint32_t frame = 0;
  std::uint64_t stopBegan = 0;
  bool drainSaid = false;
  bool memoryStop = false;  // the stop at the ceilings is RAM alone past its ceiling

  // Published for any thread.
  std::atomic<Stopped> stopped{Stopped::No};
  std::atomic<bool> memoryStopped{false};
  std::atomic<unsigned> resumePercent;
  std::atomic<bool> hold;
  std::atomic<bool> hasFill{false};
  std::atomic<double> fillRate{0.0};
  std::atomic<bool> hasDrain{false};
  std::atomic<double> drainRate{0.0};
  std::atomic<bool> closing{false};
  std::atomic<bool> closedFlag{false};

  // The tree's thread's, published under `mutex`, which it holds a few words at
  // a time and never across its work.
  mutable std::mutex mutex;
  std::condition_variable wake;
  bool started = false;
  bool closeRequested = false;
  Work work = Work::Watching;
  std::size_t trees = 0;
  std::optional<TreeSummary> closingSummary;
  std::string closingError;
  bool runGone = false;  // the run is let go at the close, its last progress kept

  // The directory's request as it stood when the session began, which every tree
  // of the session starts from, or why it could not be read.
  std::optional<disasm::CartridgeRequest> sessionRequest;
  std::string sessionError;
  disasm::BufferedRun::Progress lastProgress;
  std::array<bool, kCommands.size()> missingSaid{};
  std::array<bool, kCommands.size()> unstartableSaid{};
  bool fullSaid = false;
  TreeThread thread;

  Impl(Snes& m, std::span<const std::uint8_t> image, std::filesystem::path where, LiveOptions given, LiveHooks told)
      : machine(m),
        rom(image),
        directory(std::move(where)),
        options(std::move(given)),
        hooks(std::move(told)),
        pause(options.pause),
        resumePercent(options.pause.resumePercent),
        hold(options.pause.hold) {
    bool found = false;
    sessionRequest = disasm::treeRequest(rom, directory, found, sessionError);
    const std::filesystem::path buffer =
        options.bufferDirectory.empty() ? directory / "buffer" : options.bufferDirectory;
    run = std::make_unique<disasm::BufferedRun>(machine, rom, buffer,
                                                disasm::BufferedRunOptions{.memoryBytes = options.memoryBytes,
                                                                           .diskBytes = options.diskBytes,
                                                                           .threads = options.threads,
                                                                           .atLimit = disasm::AtLimit::Keep});
  }

  void say(const std::string& line) const {
    if (hooks.say) hooks.say(line);
  }
  void always(const std::string& line) const {
    if (hooks.always) hooks.always(line);
  }

  [[nodiscard]] PauseSettings settings() const {
    return PauseSettings{.resumePercent = resumePercent.load(std::memory_order_relaxed),
                         .hold = hold.load(std::memory_order_relaxed)};
  }

  // ---- the host's thread ----

  void publishFill() {
    const std::optional<double> rate = fill.bytesPerSecond();
    if (rate) fillRate.store(*rate, std::memory_order_relaxed);
    hasFill.store(rate.has_value(), std::memory_order_release);
  }

  void publishDrain() {
    const std::optional<double> rate = drain.bytesPerSecond();
    const bool draining = rate && *rate < 0.0;
    if (draining) drainRate.store(-*rate, std::memory_order_relaxed);
    hasDrain.store(draining, std::memory_order_release);
  }

  // Wakes the tree's thread to take its class again.
  void nudgeTree() {
    { std::lock_guard lock(mutex); }
    wake.notify_all();
  }

  Stopped look(std::uint64_t nanos, const disasm::BufferedRun::Progress& progress) {
    const std::uint64_t ceiling = progress.memoryBytes + progress.diskBytes;
    const bool atCeilings = run->atLimit();
    const bool ramOver = run->memoryOver();
    const Stopped before = pause.state();
    // A stop at the ceilings that begins with RAM alone past its ceiling ends
    // when RAM has drained to the resume level of its own ceiling, or the run
    // holds nothing; one with the two ceilings reached ends at their level.
    if (before != Stopped::AtLimit) {
      memoryStop = ramOver && !atCeilings;
    } else if (atCeilings) {
      memoryStop = false;
    }
    const std::uint64_t ramLevel = pause.resumeLevel(progress.memoryBytes);
    std::uint64_t heldForPause = progress.held;
    if (memoryStop && before == Stopped::AtLimit) {
      const bool drained = progress.memoryUsed <= ramLevel || progress.held == 0u;
      heldForPause = drained ? 0u : ~std::uint64_t{0};
    }
    const bool wasAsked = asked;
    asked = false;
    const Stopped now = pause.update(atCeilings || ramOver, heldForPause, ceiling);
    memoryStopped.store(now == Stopped::AtLimit && memoryStop, std::memory_order_release);
    if (now == before && !wasAsked) return now;
    const std::string level = memoryStop ? sizeText(ramLevel) + " of RAM" : sizeText(pause.resumeLevel(ceiling));
    // An ask that leaves the game stopped at the ceilings was a resume refused.
    if (wasAsked && now == Stopped::AtLimit) {
      say(memoryStop ? "the buffer's RAM is past its ceiling; the game resumes at " + level
                     : "the buffer is at its ceilings; the game resumes at " + level);
    }
    if (now == before) return now;
    stopped.store(now, std::memory_order_release);
    if (before == Stopped::No) {
      run->hostStopped(true);
      stopBegan = nanos;
      drain.clear();
      hasDrain.store(false, std::memory_order_release);
      drainSaid = false;
      nudgeTree();
      const std::string held = sizeText(progress.held) + " of " + sizeText(ceiling);
      const std::string resumes = pause.settings().hold ? "the pause chord resumes it" : "the game resumes at " + level;
      if (now == Stopped::ByHand) {
        say("stopped by hand: the buffer holds " + held);
      } else if (memoryStop) {
        say("stopped: the disassembly's buffer holds " + sizeText(progress.memoryUsed) + " of RAM, past its " +
            sizeText(progress.memoryBytes) + " ceiling, while its files are written; " + resumes);
      } else {
        say("stopped: the disassembly's buffer holds " + held + " (" + sizeText(progress.memoryBytes) + " of RAM, " +
            sizeText(progress.diskBytes) + " of files); " + resumes);
      }
    } else if (now == Stopped::No) {
      run->hostStopped(false);
      fill.clear();
      hasFill.store(false, std::memory_order_release);
      nudgeTree();
      say("resumed after " + durationText(static_cast<double>(nanos - stopBegan) / kNanosPerSecond));
    }
    return now;
  }

  Stopped afterRun(std::uint64_t nanos) {
    const bool wrapped = run->frame() != frame;
    if (wrapped) {
      frame = run->frame();
      run->keyframe();
    }
    const disasm::BufferedRun::Progress progress = run->progress();
    if (wrapped && pause.state() == Stopped::No) {
      fill.sample(nanos, progress.held);
      publishFill();
    }
    return look(nanos, progress);
  }

  Stopped whileStopped(std::uint64_t nanos) {
    const disasm::BufferedRun::Progress progress = run->progress();
    drain.sample(nanos, progress.held);
    publishDrain();
    if (!drainSaid && !memoryStop && pause.state() == Stopped::AtLimit && nanos - stopBegan >= kDrainLineNanos) {
      const std::optional<double> rate = drain.bytesPerSecond();
      const std::uint64_t level = pause.resumeLevel(progress.memoryBytes + progress.diskBytes);
      const std::optional<double> seconds = drain.secondsTo(level);
      if (rate && seconds && *rate < 0.0) {
        drainSaid = true;
        say("draining at " + std::to_string(static_cast<std::uint64_t>(-*rate / 1'000'000.0)) +
            " MB a second: about " + durationText(*seconds) + " to " + sizeText(level));
      }
    }
    return look(nanos, progress);
  }

  // ---- the tree's thread ----

  [[nodiscard]] bool lowered() const {
    return !closing.load(std::memory_order_acquire) && stopped.load(std::memory_order_acquire) == Stopped::No;
  }

  void keepClass() const {
    const disasm::ThreadClass wanted = lowered() ? disasm::ThreadClass::Lowered : disasm::ThreadClass::Normal;
    if (disasm::thisThreadClass() != wanted) disasm::setThisThreadClass(wanted);
  }

  void setWork(Work now) {
    std::lock_guard lock(mutex);
    work = now;
  }

  void noteFull() {
    if (!run->full()) return;
    {
      std::lock_guard lock(mutex);
      if (fullSaid) return;
      fullSaid = true;
    }
    always("the disassembly's buffer could not take a block: this computer is out of memory; the tree holds what "
           "was played before it");
  }

  void commands() {
    if (options.commands.empty()) return;
    for (std::size_t i = 0; i < kCommands.size(); ++i) {
      const char* const name = kCommands[i];
      const std::filesystem::path file = commandFile(options.commands, name);
      std::vector<std::string> arguments = {file.string(), directory.string()};
      if (i == 1u) arguments.push_back(options.image.string());
      std::string byHand = name;
      for (std::size_t a = 1; a < arguments.size(); ++a) byHand += " " + arguments[a];
      std::error_code ignored;
      if (!std::filesystem::exists(file, ignored)) {
        if (!missingSaid[i]) {
          missingSaid[i] = true;
          always(std::string(name) + " is not beside the player; run it over the tree by hand: " + byHand);
        }
        continue;
      }
      setWork(i == 0u ? Work::Render : Work::Verify);
      const std::optional<int> status = hooks.run ? hooks.run(arguments, lowered()) : std::nullopt;
      if (!status) {
        if (!unstartableSaid[i]) {
          unstartableSaid[i] = true;
          always(std::string(name) + " could not be started; run it over the tree by hand: " + byHand);
        }
      } else if (*status != 0) {
        always(std::string(name) + " exited " + std::to_string(*status));
      }
    }
  }

  // One tree from `observation`: the session's request, disassembled, written,
  // said, and the two commands run. False, said, when it was not written.
  bool makeTree(const disasm::RunObservation& observation, const std::vector<std::string>& notes,
                std::uint64_t playedMaster, bool last) {
    setWork(Work::Tree);
    std::string error = sessionError;
    try {
      std::optional<disasm::CartridgeRequest> request = sessionRequest;
      if (request) {
        request->observed = &observation;
        request->observedNotes = notes;
        request->captureSound = true;
        const disasm::CartridgeDisassembly disassembly = disasm::disassembleCartridge(*request);
        if (disasm::writeProject(disassembly, directory, error)) {
          const TreeSummary summary = summaryOf(disassembly, playedMaster);
          {
            std::lock_guard lock(mutex);
            ++trees;
            if (last) closingSummary = summary;
          }
          say("tree: " + std::to_string(summary.files) + " files, " + std::to_string(summary.instructions) +
              " instructions, " + std::to_string(summary.entries) + " entries, " + std::to_string(summary.stops) +
              " stops, " + std::to_string(summary.placed) + " of " + std::to_string(summary.imageBytes) +
              " bytes placed, " + std::to_string(summary.notes) + " notes -> " + directory.string());
          commands();
          setWork(Work::Watching);
          return true;
        }
      }
    } catch (const std::exception& e) {
      error = e.what();
    }
    always(error);
    {
      std::lock_guard lock(mutex);
      if (last) closingError = error;
    }
    setWork(Work::Watching);
    return false;
  }

  void close() {
    keepClass();
    try {
      // The run's threads, raised, play what the buffer holds; the drain is
      // looked at as they go.
      while (run->progress().held != 0u && run->failure().empty()) {
        drain.sample(steadyNanos(), run->progress().held);
        publishDrain();
        std::this_thread::sleep_for(kCloseLook);
      }
      std::vector<std::string> notes;
      const disasm::RunObservation observation = run->finish(notes);
      noteFull();
      const std::string failure = run->failure();
      if (!failure.empty()) {
        always("the disassembly stopped early: " + failure + "; the tree holds what was played before it");
      }
      static_cast<void>(makeTree(observation, notes, run->progress().playedMaster, true));
    } catch (const std::exception& e) {
      always(e.what());
      std::lock_guard lock(mutex);
      if (!closingSummary && closingError.empty()) closingError = e.what();
    }
    {
      std::lock_guard lock(mutex);
      lastProgress = run->progress();
      runGone = true;
      work = Work::Done;
    }
    // The buffer's files and its directory go with the run.
    run.reset();
    {
      std::lock_guard lock(mutex);
      closedFlag.store(true, std::memory_order_release);
    }
    wake.notify_all();
  }

  void treeBody() {
    std::optional<Found> last;
    for (;;) {
      {
        std::unique_lock lock(mutex);
        const auto deadline =
            std::chrono::steady_clock::now() +
            std::chrono::nanoseconds(static_cast<std::int64_t>(std::min(options.treeIntervalNanos, kLongestIntervalNanos)));
        while (!closeRequested) {
          keepClass();
          if (wake.wait_until(lock, deadline) == std::cv_status::timeout) break;
        }
        if (closeRequested) break;
      }
      keepClass();
      try {
        std::vector<std::string> notes;
        const disasm::RunObservation observation = run->observation(notes);
        const std::uint64_t playedMaster = run->progress().playedMaster;
        noteFull();
        const Found found = foundIn(observation);
        if (last && *last == found) continue;
        if (makeTree(observation, notes, playedMaster, false)) last = found;
      } catch (const std::exception& e) {
        always(e.what());
      }
    }
    close();
  }

  void start() {
    std::lock_guard lock(mutex);
    if (started) return;
    thread.start([this] { treeBody(); });
    started = true;
  }

  void beginClose() {
    if (closing.exchange(true, std::memory_order_acq_rel)) return;
    start();
    machine.setObserver(nullptr);
    machine.setApuObserver(nullptr);
    machine.setInstructionObserver(nullptr);
    run->hostStopped(true);
    {
      std::lock_guard lock(mutex);
      closeRequested = true;
    }
    wake.notify_all();
  }

  [[nodiscard]] std::string whatIsLeft(const LiveStatus& status) const {
    switch (status.work) {
      case Work::Tree: return "the last tree is being made";
      case Work::Render: return "snes_render is running";
      case Work::Verify: return "snes_verify is running";
      case Work::Watching:
      case Work::Done: break;
    }
    return sizeText(status.buffer.held) + " to play, then the last tree";
  }
};

std::size_t titleCharacters(std::string_view text) noexcept {
  std::size_t characters = 0;
  // Every byte but a continuation byte begins a code point.
  for (const char byte : text) characters += (static_cast<unsigned char>(byte) & 0xC0u) != 0x80u ? 1u : 0u;
  return characters;
}

std::size_t titleRoom(double points, double characterPoints, std::string_view name) noexcept {
  if (!(points > 0.0) || !(characterPoints > 0.0)) return 0;
  // Capped well inside the integer's range, where a width stops meaning anything.
  constexpr double kMost = 1e9;
  const double bar = points / characterPoints;
  const std::size_t characters = static_cast<std::size_t>(bar < kMost ? bar : kMost);
  const std::size_t taken = titleCharacters(name) + 3u;
  return characters > taken ? characters - taken : 0u;
}

std::string LiveTitle::closeText(const LiveStatus& status) const {
  const std::uint64_t held = status.buffer.held;
  // The last tree begins once the buffer is played, so a tree or a command
  // running over one while the buffer still holds anything is one from the
  // middle of the session, and the buffer is what the close has left.
  if (held == 0u) {
    switch (status.work) {
      case Work::Tree: return "finishing: writing the last tree";
      case Work::Render: return "finishing: snes_render";
      case Work::Verify: return "finishing: snes_verify";
      case Work::Done: return "finishing: closing the buffer";
      case Work::Watching: break;
    }
  }
  if (closeMost_ == 0u) return "finishing: nothing left to play";
  const std::uint64_t played = closeMost_ - held;
  // Rounded down. The product overflows where the buffer has held 184 petabytes.
  const std::uint64_t percent = played * 100u / closeMost_;
  std::string text = "finishing: " + sizeText(played) + " of " + sizeText(closeMost_) + " played, " +
                     std::to_string(percent) + " %";
  if (held != 0u && status.secondsToEmpty) text += ", about " + durationText(*status.secondsToEmpty) + " left";
  return text;
}

std::string LiveTitle::text(std::string_view lead, const LiveStatus& status, std::uint64_t nanos, std::size_t room) {
  // The ten braille frames of a turning dot and the dash between two parts, as
  // their UTF-8 bytes, which every compiler keeps exactly whatever its source
  // code page.
  static constexpr std::array<const char*, 10> kSpinner = {
      "\xE2\xA0\x8B", "\xE2\xA0\x99", "\xE2\xA0\xB9", "\xE2\xA0\xB8", "\xE2\xA0\xBC",
      "\xE2\xA0\xB4", "\xE2\xA0\xA6", "\xE2\xA0\xA7", "\xE2\xA0\x87", "\xE2\xA0\x8F"};
  static constexpr const char* kDash = " \xE2\x80\x94 ";
  constexpr std::size_t kDashCharacters = 3;
  constexpr std::size_t kSpinnerCharacters = 2;  // the space before it and the dot

  const bool moved = status.buffer.playedMaster != played_ || status.buffer.held != held_ || status.trees != trees_ ||
                     status.work != Work::Watching;
  if (begun_ && moved) step_ = (step_ + 1u) % kSpinner.size();
  played_ = status.buffer.playedMaster;
  held_ = status.buffer.held;
  trees_ = status.trees;
  if (!begun_ || nanos - sizesAt_ >= kRefreshNanos) {
    ram_ = "RAM " + sizeText(status.buffer.memoryUsed) + " of " + sizeText(status.buffer.memoryBytes);
    files_ = "files " + sizeText(status.buffer.diskUsed) + " of " + sizeText(status.buffer.diskBytes);
    sizesAt_ = nanos;
  }

  // What the lead is about: the game running, a stop of either kind, the
  // buffer being played at the close, or one of the close's stages after it.
  std::string closeLead;
  unsigned saying = static_cast<unsigned>(status.stopped);
  if (status.closing) {
    closeMost_ = std::max(closeMost_, status.buffer.held);
    closeLead = closeText(status);
    lead = closeLead;
    saying = status.buffer.held != 0u ? 3u : 4u + static_cast<unsigned>(status.work);
  }
  if (!begun_ || saying != saying_) {
    first_ = 0;
    pageAt_ = nanos;
  }
  saying_ = saying;
  begun_ = true;

  std::array<std::string_view, 3> parts;
  std::size_t count = 0;
  if (!lead.empty()) parts[count++] = lead;
  parts[count++] = ram_;
  parts[count++] = files_;
  // How many parts the page beginning at `from` holds: that one, and each one
  // after it the room still takes.
  const auto pageFrom = [&](std::size_t from) {
    std::size_t used = titleCharacters(parts[from]) + kSpinnerCharacters;
    std::size_t taken = 1;
    while (from + taken < count && used + kDashCharacters + titleCharacters(parts[from + taken]) <= room) {
      used += kDashCharacters + titleCharacters(parts[from + taken]);
      ++taken;
    }
    return taken;
  };
  if (first_ >= count) first_ = 0;
  if (nanos - pageAt_ >= kPageNanos) {
    first_ += pageFrom(first_);
    if (first_ >= count) first_ = 0;
    pageAt_ = nanos;
  }
  const std::size_t end = first_ + pageFrom(first_);
  std::string text;
  for (std::size_t i = first_; i < end; ++i) {
    if (i != first_) text += kDash;
    text += parts[i];
  }
  return text + " " + kSpinner[step_];
}

bool LiveDisassembly::readable(std::span<const std::uint8_t> rom, const std::filesystem::path& directory, bool& found,
                               std::size_t& entries, std::size_t& files, std::string& error) {
  found = false;
  entries = 0;
  files = 0;
  const std::optional<disasm::CartridgeRequest> request = disasm::treeRequest(rom, directory, found, error);
  if (!request) return false;
  entries = request->entries.size();
  files = request->regions.size();
  return true;
}

LiveDisassembly::LiveDisassembly(Snes& machine, std::span<const std::uint8_t> rom, std::filesystem::path directory,
                                 LiveOptions options, LiveHooks hooks)
    : impl_(std::make_unique<Impl>(machine, rom, std::move(directory), std::move(options), std::move(hooks))) {}

LiveDisassembly::~LiveDisassembly() {
  try {
    impl_->beginClose();
  } catch (...) {
  }
  {
    std::unique_lock lock(impl_->mutex);
    impl_->wake.wait(lock, [&] { return impl_->closedFlag.load(std::memory_order_acquire); });
  }
  impl_->thread.join();
}

void LiveDisassembly::start() { impl_->start(); }

Stopped LiveDisassembly::afterRun(std::uint64_t nanos) { return impl_->afterRun(nanos); }

Stopped LiveDisassembly::whileStopped(std::uint64_t nanos) { return impl_->whileStopped(nanos); }

void LiveDisassembly::togglePause() {
  impl_->pause.toggle();
  impl_->asked = !impl_->asked;
}

void LiveDisassembly::reset() { impl_->run->reset(); }

PauseSettings LiveDisassembly::pauseSettings() const { return impl_->settings(); }

void LiveDisassembly::setPauseSettings(PauseSettings settings) {
  impl_->pause.set(settings);
  impl_->resumePercent.store(settings.resumePercent, std::memory_order_relaxed);
  impl_->hold.store(settings.hold, std::memory_order_relaxed);
}

void LiveDisassembly::setLimits(std::uint64_t memoryBytes, std::uint64_t diskBytes) {
  impl_->run->setLimits(memoryBytes, diskBytes);
}

LiveStatus LiveDisassembly::status() const {
  const Impl& i = *impl_;
  LiveStatus s;
  {
    std::lock_guard lock(i.mutex);
    s.buffer = i.runGone ? i.lastProgress : i.run->progress();
    s.atLimit = !i.runGone && i.run->atLimit();
    s.work = i.work;
    s.trees = i.trees;
  }
  s.stopped = i.stopped.load(std::memory_order_acquire);
  s.memoryStop = s.stopped == Stopped::AtLimit && i.memoryStopped.load(std::memory_order_acquire);
  s.closing = i.closing.load(std::memory_order_acquire);
  s.ceiling = s.buffer.memoryBytes + s.buffer.diskBytes;
  s.pause = i.settings();
  s.resumeLevel = PauseControl{s.pause}.resumeLevel(s.ceiling);
  if (i.hasFill.load(std::memory_order_acquire)) s.fillBytesPerSecond = i.fillRate.load(std::memory_order_relaxed);
  const bool draining = s.stopped != Stopped::No || s.closing;
  if (draining && i.hasDrain.load(std::memory_order_acquire)) {
    s.drainBytesPerSecond = i.drainRate.load(std::memory_order_relaxed);
  }
  const std::uint64_t held = s.buffer.held;
  if (s.stopped == Stopped::AtLimit && !s.memoryStop && !s.pause.hold && !s.closing) {
    if (held <= s.resumeLevel) {
      s.secondsToResume = 0.0;
    } else if (s.drainBytesPerSecond && *s.drainBytesPerSecond > 0.0) {
      s.secondsToResume = static_cast<double>(held - s.resumeLevel) / *s.drainBytesPerSecond;
    }
  }
  if (draining) {
    if (held == 0u) {
      s.secondsToEmpty = 0.0;
    } else if (s.drainBytesPerSecond && *s.drainBytesPerSecond > 0.0) {
      s.secondsToEmpty = static_cast<double>(held) / *s.drainBytesPerSecond;
    }
  }
  if (s.fillBytesPerSecond && *s.fillBytesPerSecond > 0.0) {
    s.secondsOfPlayAfter = static_cast<double>(s.ceiling - s.resumeLevel) / *s.fillBytesPerSecond;
  }
  return s;
}

std::string LiveDisassembly::statusText() const {
  const LiveStatus s = status();
  if (s.closing) return "finishing the disassembly, " + impl_->whatIsLeft(s);
  const std::string held = sizeText(s.buffer.held) + " of " + sizeText(s.ceiling);
  switch (s.stopped) {
    case Stopped::No: return {};
    case Stopped::ByHand: return "stopped by hand, " + held;
    case Stopped::AtLimit: break;
  }
  if (s.memoryStop) {
    const std::string ram = "stopped, " + sizeText(s.buffer.memoryUsed) + " of " + sizeText(s.buffer.memoryBytes) + " RAM, ";
    if (s.pause.hold) return ram + "the pause chord resumes";
    return ram + "resumes at " + sizeText(PauseControl{s.pause}.resumeLevel(s.buffer.memoryBytes));
  }
  if (s.pause.hold) return "stopped, " + held + ", the pause chord resumes";
  if (s.secondsToResume) return "stopped, " + held + ", resumes in " + durationText(*s.secondsToResume);
  return "stopped, " + held + ", resumes at " + sizeText(s.resumeLevel);
}

void LiveDisassembly::beginClose() { impl_->beginClose(); }

bool LiveDisassembly::closed() const noexcept { return impl_->closedFlag.load(std::memory_order_acquire); }

void LiveDisassembly::closeAsked(const std::string& howToEndIt) {
  impl_->always("still finishing the disassembly: " + impl_->whatIsLeft(status()) +
                ". It ends by itself when that is done; " + howToEndIt + " ends it without the last tree");
}

std::optional<TreeSummary> LiveDisassembly::closingTree(std::string& error) const {
  std::lock_guard lock(impl_->mutex);
  if (impl_->closingSummary) return impl_->closingSummary;
  error = impl_->closingError.empty() ? "the disassembly has not closed" : impl_->closingError;
  return std::nullopt;
}

}  // namespace snaggletooth::player
