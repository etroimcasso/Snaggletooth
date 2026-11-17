#include "rom/rom_buffered_run.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "rom/rom_buffer.h"
#include "rom/rom_observe_internal.h"
#include "snaggletooth/snes/snes.h"

namespace snaggletooth::disasm {
namespace {

using namespace detail;

constexpr std::size_t kNoOffset = std::numeric_limits<std::size_t>::max();

// What a pool thread answers for a block of the CPU stream: one check a
// boundary, and the shadow's calls every step made, in order.
struct CpuChecks {
  std::vector<Checked> steps;
  ir::Tape tape;
};

// For a block of the sound stream: one check a boundary, and the keys of the
// nodes the checks lifted.
struct SoundChecks {
  std::vector<SoundChecked> steps;
  std::vector<Spc700Check::NodeKey> lifted;
};

// Where a stream's check stands at a block's first record: the file at the last
// boundary before it, the step's ordinal, and the records of the step under
// way, which began in an earlier block: a CPU access or a cycle with no access
// on the CPU stream, a sound-CPU access on the sound stream.
struct CpuStart {
  Cpu65816State file;
  std::uint64_t ordinal = 0;
  std::vector<std::uint8_t> prefix;
};
struct SoundStart {
  Spc700State file;
  ApuWindow window;
  std::uint64_t ordinal = 0;
  std::vector<std::uint8_t> prefix;
};

// Copies the records from `from` that a check reads into `prefix`.
void collect(std::span<const std::uint8_t> block, std::size_t from, bool sound, std::vector<std::uint8_t>& prefix) {
  RecordReader r{block, from};
  while (r.more()) {
    const std::size_t start = r.at;
    const RecordKind kind = r.kind();
    r.skip(kind);
    bool kept = false;
    if (sound) {
      kept = kind == RecordKind::SoundAccess;
    } else if (kind == RecordKind::Internal) {
      kept = true;
    } else if (kind == RecordKind::Access) {
      BusAccess access;
      std::memcpy(&access, block.data() + start + 1, sizeof access);
      kept = access.source == AccessSource::Cpu;
    }
    if (kept) prefix.insert(prefix.end(), block.begin() + static_cast<std::ptrdiff_t>(start), block.begin() + static_cast<std::ptrdiff_t>(r.at));
  }
}

// Where the CPU stream's check stands after `block`.
void advance(std::span<const std::uint8_t> block, CpuStart& at) {
  std::size_t tail = kNoOffset;
  for (RecordReader r{block}; r.more();) {
    const RecordKind kind = r.kind();
    if (kind == RecordKind::Boundary) {
      at.file = r.payload<BoundaryRecord>().after;
      ++at.ordinal;
      tail = r.at;
    } else if (kind == RecordKind::Reset) {
      at.file = r.payload<ResetRecord>().after;
      tail = r.at;
    } else {
      r.skip(kind);
    }
  }
  if (tail != kNoOffset) at.prefix.clear();
  collect(block, tail == kNoOffset ? 0u : tail, false, at.prefix);
}

// Where the sound stream's check stands after `block`.
void advance(std::span<const std::uint8_t> block, SoundStart& at) {
  std::size_t tail = kNoOffset;
  for (RecordReader r{block}; r.more();) {
    const RecordKind kind = r.kind();
    if (kind == RecordKind::SoundStep) {
      const SoundStepRecord step = r.payload<SoundStepRecord>();
      at.file = step.after;
      at.window = step.window;
      ++at.ordinal;
      tail = r.at;
    } else if (kind == RecordKind::SoundReset) {
      const SoundResetRecord reset = r.payload<SoundResetRecord>();
      at.file = reset.after;
      at.window = reset.window;
      tail = r.at;
    } else {
      r.skip(kind);
    }
  }
  if (tail != kNoOffset) at.prefix.clear();
  collect(block, tail == kNoOffset ? 0u : tail, true, at.prefix);
}

// What one pool thread keeps from one block to the next: a check of each CPU's,
// with its cache of nodes, and the window the sound check reads its bytes through.
struct PoolChecks {
  std::unique_ptr<SnesState> held;
  ApuWindow window;
  StepCheck cpu;
  Spc700Check sound;
  ir::StepObserver observed;

  PoolChecks(const CartridgeBoard& board, std::size_t imageBytes, const SnesState& start, const ApuWindow& startWindow)
      : held(std::make_unique<SnesState>(start)),
        window(startWindow),
        cpu{board, imageBytes, start.cpu, nullptr},
        sound{MachineView{*held, window}, start.apu.cpu} {}

  // The CPU stream's block, checked from where its check stands.
  CpuChecks check(std::span<const std::uint8_t> block, const CpuStart& start) {
    CpuChecks out;
    cpu.interpreter.shadow = &out.tape;
    cpu.interpreter.registers = ir::registersOf(start.file);
    observed.clear();
    Cpu65816State last = start.file;
    std::uint64_t ordinal = start.ordinal;
    const auto read = [&](std::span<const std::uint8_t> records) {
      for (RecordReader r{records}; r.more();) {
        const RecordKind kind = r.kind();
        switch (kind) {
          case RecordKind::Access: observed.access(r.payload<BusAccess>()); break;
          case RecordKind::Internal: {
            const InternalRecord internal = r.payload<InternalRecord>();
            observed.internal(internal.address, internal.has ? std::optional<CycleKind>(internal.kind) : std::nullopt);
            break;
          }
          case RecordKind::Boundary: {
            const BoundaryRecord boundary = r.payload<BoundaryRecord>();
            const std::size_t calls = out.tape.entries.size();
            Checked c = cpu.step(ordinal++, last, boundary.after, observed);
            c.calls = static_cast<std::uint32_t>(out.tape.entries.size() - calls);
            out.steps.push_back(std::move(c));
            observed.clear();
            last = boundary.after;
            break;
          }
          case RecordKind::Reset: {
            last = r.payload<ResetRecord>().after;
            cpu.interpreter.registers = ir::registersOf(last);
            cpu.interpreter.release();
            observed.clear();
            break;
          }
          default: r.skip(kind); break;
        }
      }
    };
    read(start.prefix);
    read(block);
    cpu.interpreter.shadow = nullptr;
    return out;
  }

  // The sound stream's block, checked from where its check stands.
  SoundChecks check(std::span<const std::uint8_t> block, const SoundStart& start) {
    SoundChecks out;
    window = start.window;
    sound.steps = start.ordinal;
    sound.restart(start.file);
    const auto read = [&](std::span<const std::uint8_t> records) {
      for (RecordReader r{records}; r.more();) {
        const RecordKind kind = r.kind();
        switch (kind) {
          case RecordKind::SoundAccess: {
            const SoundAccessRecord access = r.payload<SoundAccessRecord>();
            sound.access(access.address, access.value, access.write);
            break;
          }
          case RecordKind::SoundStep: {
            const SoundStepRecord step = r.payload<SoundStepRecord>();
            window = step.window;
            out.steps.push_back(sound.step(step.before, step.after, step.cycles));
            break;
          }
          case RecordKind::SoundReset: {
            const SoundResetRecord reset = r.payload<SoundResetRecord>();
            window = reset.window;
            sound.restart(reset.after);
            break;
          }
          default: r.skip(kind); break;
        }
      }
    };
    read(start.prefix);
    read(block);
    out.lifted = std::move(sound.lifted);
    sound.lifted.clear();
    return out;
  }
};

// The threads that check blocks: each takes the oldest block queued, of either
// stream, and answers its checks through a future. Each works at the class the
// budget asks for, set as it takes a task and when `nudge()` wakes it.
class Pool {
 public:
  Pool(unsigned count, StreamBudget& budget, const CartridgeBoard& board, std::size_t imageBytes,
       const SnesState& start, const ApuWindow& window) {
    for (unsigned i = 0; i < count; ++i) {
      threads_.emplace_back([this, &budget, &board, imageBytes, &start, window] {
        ThreadClassKeeper keeper{budget, ThreadClass::Lowered};
        keeper.keep();
        const auto checks = std::make_unique<PoolChecks>(board, imageBytes, start, window);
        {
          std::lock_guard lock(mutex_);
          ++ready_;
        }
        built_.notify_all();
        run(*checks, keeper);
      });
    }
    // Every thread builds its checks from `start` before the run's own state moves on.
    std::unique_lock waiting(mutex_);
    built_.wait(waiting, [&] { return ready_ == threads_.size(); });
  }
  ~Pool() {
    {
      std::lock_guard lock(mutex_);
      stopping_ = true;
    }
    queued_.notify_all();
    for (std::thread& thread : threads_) thread.join();
  }
  Pool(const Pool&) = delete;
  Pool& operator=(const Pool&) = delete;

  [[nodiscard]] std::size_t size() const noexcept { return threads_.size(); }

  template <class Checks, class Start>
  std::future<Checks> check(std::span<const std::uint8_t> block, Start start) {
    auto promise = std::make_shared<std::promise<Checks>>();
    std::future<Checks> answer = promise->get_future();
    {
      std::lock_guard lock(mutex_);
      tasks_.emplace_back([promise, block, start = std::move(start)](PoolChecks& checks) {
        try {
          promise->set_value(checks.check(block, start));
        } catch (...) {
          promise->set_exception(std::current_exception());
        }
      });
    }
    queued_.notify_one();
    return answer;
  }

  // Wakes every thread waiting for a task, so each takes the class the budget
  // now asks for.
  void nudge() {
    { std::lock_guard lock(mutex_); }
    queued_.notify_all();
  }

 private:
  void run(PoolChecks& checks, ThreadClassKeeper& keeper) {
    for (;;) {
      std::function<void(PoolChecks&)> task;
      {
        std::unique_lock lock(mutex_);
        queued_.wait(lock, [&] { return stopping_ || !tasks_.empty() || keeper.due(); });
        if (tasks_.empty() && stopping_) return;
        if (!tasks_.empty()) {
          task = std::move(tasks_.front());
          tasks_.pop_front();
        }
      }
      keeper.keep();
      if (task) task(checks);
    }
  }

  std::mutex mutex_;
  std::condition_variable queued_;
  std::condition_variable built_;
  std::deque<std::function<void(PoolChecks&)>> tasks_;
  std::size_t ready_ = 0;
  bool stopping_ = false;
  std::vector<std::thread> threads_;
};

unsigned poolSize(unsigned asked) {
  if (asked != 0u) return asked;
  const unsigned cores = std::thread::hardware_concurrency();
  return cores > 3u ? cores - 2u : 1u;
}

// The bytes at the sound CPU's program counter on each side of a step.
ApuWindow windowAt(const Snes& machine, std::uint16_t before, std::uint16_t after) {
  ApuWindow window;
  window.pc[0] = before;
  window.pc[1] = after;
  for (std::size_t side = 0; side < 2u; ++side) {
    for (std::uint16_t i = 0; i < 3u; ++i) {
      window.bytes[side][i] = machine.peekApu(static_cast<std::uint16_t>(window.pc[side] + i));
    }
  }
  return window;
}

}  // namespace

struct BufferedRun::Impl final : BusObserver, InstructionObserver, ApuObserver {
  using Core = ObservedRun::Impl;

  Snes& machine;
  std::span<const std::uint8_t> rom;
  std::filesystem::path directory;
  std::size_t keyframeBytes = 0;
  std::thread::id hostThread = std::this_thread::get_id();

  // The host's side: the streams it writes, the budget they share, and what its
  // observers keep.
  StreamBudget budget;
  std::unique_ptr<Stream> cpuStream;
  std::unique_ptr<Stream> soundStream;
  std::atomic<bool> fullFlag{false};
  std::uint16_t lastLine = 0;
  std::uint32_t frames = 0;
  std::uint64_t hostSpent = 0;
  std::atomic<std::uint64_t> hostMaster{0};
  bool stopped = false;

  // The threads' side. The shadow thread writes the kept state's CPU part and
  // the sound thread its audio part.
  std::unique_ptr<SnesState> kept;
  ApuWindow window;
  std::unique_ptr<Core> core;
  std::unique_ptr<SnesState> keyframeState;  // the shadow thread's, a keyframe read back
  std::unique_ptr<Pool> pool;
  unsigned poolThreads = 0;
  std::atomic<std::uint64_t> playedMaster{0};
  std::atomic<std::uint64_t> keyframesDiffered{0};
  bool keyframeNoted = false;

  // A request for the observation, and the threads' answers.
  std::mutex requestOrder;  // one request at a time
  mutable std::mutex mutex;
  std::condition_variable wake;      // the two threads wait here
  std::condition_variable answered;  // the requester waits here
  std::atomic<bool> requested{false};
  bool requestOpen = false;
  std::uint64_t cpuTarget = 0;
  std::uint64_t soundTarget = 0;
  bool cpuAnswered = false;
  bool soundAnswered = false;
  bool cpuGone = false;    // the shadow thread has returned
  bool soundGone = false;  // the sound thread has returned
  RunObservation cpuPart;
  std::vector<std::string> cpuNotes;
  RunObservation soundPart;
  std::vector<std::string> soundNotes;
  bool stopping = false;
  std::string failure;  // the first a thread of the run met

  std::thread shadowThread;
  std::thread soundThread;

  Impl(Snes& m, std::span<const std::uint8_t> image, std::filesystem::path where, const BufferedRunOptions& options)
      : machine(m),
        rom(image),
        directory(std::move(where)),
        budget(options.memoryBytes, options.diskBytes, options.atLimit) {
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error || !std::filesystem::is_directory(directory)) {
      throw std::runtime_error("the run's buffer directory " + directory.string() + " cannot be created");
    }
    cpuStream = std::make_unique<Stream>(directory / "cpu", budget);
    soundStream = std::make_unique<Stream>(directory / "sound", budget);
    const SnesState& state = machine.state();
    keyframeBytes = state.byteSize();
    lastLine = state.vpos;
    kept = std::make_unique<SnesState>(state);
    window = windowAt(machine, state.apu.cpu.pc, state.apu.cpu.pc);
    core = std::make_unique<Core>(MachineView{*kept, window}, rom);
    keyframeState = std::make_unique<SnesState>(state);
    poolThreads = poolSize(options.threads);
    pool = std::make_unique<Pool>(poolThreads, budget, core->board, rom.size(), state, window);
    shadowThread = std::thread([this] { shadowBody(); });
    soundThread = std::thread([this] { soundBody(); });
    machine.setObserver(this);
    machine.setApuObserver(this);
    machine.setInstructionObserver(this);
  }

  ~Impl() {
    try {
      stop();
    } catch (...) {
    }
  }

  // ---- the host's thread: copying ----

  void markFull() noexcept { fullFlag.store(true, std::memory_order_relaxed); }
  [[nodiscard]] bool isFull() const noexcept { return fullFlag.load(std::memory_order_relaxed); }

  void access(const BusAccess& a) override {
    if (isFull()) return;
    bool written = false;
    if (a.source == AccessSource::Dma || a.source == AccessSource::Hdma) {
      const DmaChannel& channel = machine.state().dma[a.channel];
      written = putRecord(*cpuStream, RecordKind::EngineAccess,
                          EngineAccessRecord{.access = a, .dmap = channel.dmap, .bbad = channel.bbad});
    } else {
      written = putRecord(*cpuStream, RecordKind::Access, a);
    }
    if (!written) markFull();
  }

  void internal(std::uint32_t address, std::optional<CycleKind> kind) override {
    if (isFull()) return;
    const InternalRecord record{.address = address,
                                .has = kind.has_value(),
                                .kind = kind.value_or(CycleKind::DataRead)};
    if (!putRecord(*cpuStream, RecordKind::Internal, record)) markFull();
  }

  void instruction(const Cpu65816State&, const Cpu65816State& after, std::uint32_t masterCycles) override {
    const SnesState& state = machine.state();
    hostSpent += masterCycles;
    hostMaster.store(hostSpent, std::memory_order_relaxed);
    const bool wrapped = state.vpos < lastLine;
    lastLine = state.vpos;
    if (wrapped) ++frames;
    if (isFull()) return;
    if (wrapped) {
      const PpuState& ppu = state.ppu;
      FrameStartRecord frame{.inidisp = ppu.inidisp,
                             .objsel = ppu.objsel,
                             .bgmode = ppu.bgmode,
                             .bg1sc = ppu.bg1sc,
                             .bg2sc = ppu.bg2sc,
                             .bg3sc = ppu.bg3sc,
                             .bg4sc = ppu.bg4sc,
                             .bg12nba = ppu.bg12nba,
                             .bg34nba = ppu.bg34nba,
                             .palette = ppu.cgram};
      if (!putRecord(*cpuStream, RecordKind::FrameStart, frame)) {
        markFull();
        return;
      }
    }
    const BoundaryRecord boundary{.after = after, .masterCycles = masterCycles, .vpos = state.vpos};
    if (!putRecord(*cpuStream, RecordKind::Boundary, boundary)) markFull();
  }

  void access(std::uint16_t address, std::uint8_t value, bool write) override {
    if (isFull()) return;
    if (write && address == AudioLockstep::kDspDataPort && value != 0u &&
        machine.state().apu.dspAddr == AudioLockstep::kDspKon) {
      std::uint8_t* const at = putBytes(*soundStream, RecordKind::KeyOnMemory, AudioLockstep::kAudioMemory);
      if (at == nullptr) {
        markFull();
        return;
      }
      std::memcpy(at, machine.state().apu.ram.data(), AudioLockstep::kAudioMemory);
    }
    const SoundAccessRecord record{.address = address, .value = value, .write = write};
    if (!putRecord(*soundStream, RecordKind::SoundAccess, record)) markFull();
  }

  void instruction(const Spc700State& before, const Spc700State& after, std::uint32_t cycles) override {
    if (isFull()) return;
    const SoundStepRecord record{.before = before,
                                 .after = after,
                                 .cycles = cycles,
                                 .window = windowAt(machine, before.pc, after.pc)};
    if (!putRecord(*soundStream, RecordKind::SoundStep, record)) markFull();
  }

  void keyframe() {
    if (stopped) throw std::logic_error("a keyframe for a run that is finished");
    if (isFull()) return;
    const SnesState& state = machine.state();
    const KeyframeHeader header{.master = state.master,
                                .frame = frames,
                                .bytes = static_cast<std::uint32_t>(keyframeBytes)};
    std::uint8_t* const at = putRecord(*cpuStream, RecordKind::Keyframe, header, keyframeBytes);
    if (at == nullptr) {
      markFull();
      return;
    }
    static_cast<void>(state.asBytes(std::span<std::uint8_t>(at, keyframeBytes)));
  }

  void reset() {
    if (stopped) throw std::logic_error("a reset for a run that is finished");
    machine.reset();
    const SnesState& state = machine.state();
    lastLine = state.vpos;
    if (isFull()) return;
    if (!putRecord(*cpuStream, RecordKind::Reset, ResetRecord{.after = state.cpu, .vpos = state.vpos})) {
      markFull();
      return;
    }
    SoundResetRecord sound{.after = state.apu.cpu,
                           .dspAddr = state.apu.dspAddr,
                           .registers = {},
                           .window = windowAt(machine, state.apu.cpu.pc, state.apu.cpu.pc)};
    std::copy_n(state.apu.dsp.regs.begin(), sound.registers.size(), sound.registers.begin());
    if (!putRecord(*soundStream, RecordKind::SoundReset, sound)) markFull();
  }

  void setLimits(std::uint64_t memoryBytes, std::uint64_t diskBytes) {
    if (stopped) throw std::logic_error("new ceilings for a run that is finished");
    budget.setLimits(memoryBytes, diskBytes);
  }

  void hostStopped(bool stoppedHere) {
    if (stopped) throw std::logic_error("the host's stop told to a run that is finished");
    budget.setHostStopped(stoppedHere);
    pool->nudge();
  }

  // The observers are cleared, both streams end, and every thread of the run is
  // joined, once; the threads work at the class a thread starts with meanwhile.
  void stop() {
    if (stopped) return;
    stopped = true;
    budget.setHostStopped(true);
    pool->nudge();
    machine.setObserver(nullptr);
    machine.setApuObserver(nullptr);
    machine.setInstructionObserver(nullptr);
    static_cast<void>(putRecord(*cpuStream, RecordKind::End));
    static_cast<void>(putRecord(*soundStream, RecordKind::SoundEnd));
    cpuStream->end();
    soundStream->end();
    {
      std::lock_guard lock(mutex);
      stopping = true;
    }
    wake.notify_all();
    shadowThread.join();
    soundThread.join();
    pool.reset();
  }

  RunObservation finish(std::vector<std::string>& notes) {
    if (stopped) throw std::logic_error("a run is finished once");
    stop();
    RunObservation observation;
    core->finish(observation);
    core->notesInto(notes);
    noteFailure(notes);
    return observation;
  }

  // A failure, said last: the observation holds what was played before it.
  void noteFailure(std::vector<std::string>& notes) const {
    std::lock_guard lock(mutex);
    if (!failure.empty()) notes.push_back("run: " + failure + "; the observation holds what was played before it");
  }

  [[nodiscard]] std::string firstFailure() const {
    std::lock_guard lock(mutex);
    return failure;
  }

  RunObservation observation(std::vector<std::string>& notes) {
    std::lock_guard one(requestOrder);
    if (stopped) throw std::logic_error("an observation of a run that is finished");
    if (std::this_thread::get_id() == hostThread) {
      cpuStream->flush();
      soundStream->flush();
    }
    std::unique_lock lock(mutex);
    cpuTarget = cpuStream->handed();
    soundTarget = soundStream->handed();
    cpuAnswered = false;
    soundAnswered = false;
    requestOpen = true;
    requested.store(true, std::memory_order_release);
    wake.notify_all();
    answered.wait(lock, [&] { return (cpuAnswered || cpuGone) && (soundAnswered || soundGone); });
    requestOpen = false;
    requested.store(false, std::memory_order_release);
    // A thread that has returned answers nothing more; its part is read here,
    // where nothing else now touches it.
    if (!cpuAnswered) {
      cpuPart = RunObservation{};
      core->assembleCpu(cpuPart);
      cpuNotes = core->notes;
    }
    if (!soundAnswered) {
      soundPart = RunObservation{};
      core->assembleSound(soundPart);
      soundNotes = core->audio.notes;
    }
    RunObservation out = std::move(cpuPart);
    out.spc700Instructions = soundPart.spc700Instructions;
    out.spc700Nodes = soundPart.spc700Nodes;
    out.spc700Divergences = soundPart.spc700Divergences;
    out.samples = std::move(soundPart.samples);
    notes.insert(notes.end(), cpuNotes.begin(), cpuNotes.end());
    notes.insert(notes.end(), soundNotes.begin(), soundNotes.end());
    if (!failure.empty()) notes.push_back("run: " + failure + "; the observation holds what was played before it");
    return out;
  }

  // ---- the run's threads ----

  void failed(const std::string& what) {
    std::lock_guard lock(mutex);
    if (failure.empty()) failure = "the buffer could not be played: " + what;
    answered.notify_all();
  }

  // Waits a little for more of the buffer, or a request this thread has not
  // answered, or the end.
  void idle(const bool& answeredHere) {
    std::unique_lock lock(mutex);
    wake.wait_for(lock, std::chrono::microseconds(500),
                  [&] { return stopping || (requestOpen && !answeredHere); });
  }

  // A stream's thread: takes the blocks that are ready, up to one more than the
  // pool's threads, has each checked, and plays them in order. A block the
  // stream cannot hand over ends the taking, and the blocks already taken are
  // still played; a block that does not check or play ends the playing. From
  // then on the thread keeps the run as it stood, and answers every request
  // with it until the run stops.
  template <class Checks, class Start, class Play, class Answer>
  void body(Stream& stream, Start start, Play play, Answer answer, const bool& answeredHere) {
    ThreadClassKeeper keeper{budget, ThreadClass::Lowered};
    struct InFlight {
      std::span<const std::uint8_t> block;
      std::future<Checks> checks;
    };
    std::deque<InFlight> inflight;
    const std::size_t lookahead = pool->size() + 1u;
    std::uint64_t played = 0;
    bool ended = false;
    bool taking = true;
    bool broken = false;
    for (;;) {
      keeper.keep();
      if (taking && !broken) {
        try {
          while (inflight.size() < lookahead) {
            const std::optional<std::span<const std::uint8_t>> next = stream.poll(ended);
            if (!next) break;
            inflight.push_back(InFlight{.block = *next, .checks = pool->check<Checks>(*next, start)});
            advance(*next, start);
          }
        } catch (const std::exception& e) {
          taking = false;
          failed(e.what());
        }
      }
      if (!inflight.empty()) {
        // The pool reads a block until its checks are answered, so each is
        // waited for before its block is returned.
        InFlight& front = inflight.front();
        try {
          Checks answered = front.checks.get();
          if (!broken) play(front.block, answered);
        } catch (const std::exception& e) {
          if (!broken) failed(e.what());
          broken = true;
        }
        inflight.pop_front();
        stream.returned();
        if (!broken) ++played;
        answer(played, broken);
        continue;
      }
      const bool stopped = broken || !taking;
      answer(played, stopped);
      if (ended && !stopped) return;
      idle(answeredHere);
      if (stopped && isStopping()) return;
    }
  }

  [[nodiscard]] bool isStopping() {
    std::lock_guard lock(mutex);
    return stopping;
  }

  // A thread of the run has returned.
  void gone(bool& flag) {
    std::lock_guard lock(mutex);
    flag = true;
    answered.notify_all();
  }

  void shadowBody() {
    try {
      CpuStart start{.file = kept->cpu, .ordinal = 0, .prefix = {}};
      Cpu65816State playedFile = kept->cpu;
      body<CpuChecks>(
          *cpuStream, std::move(start),
          [&](std::span<const std::uint8_t> block, const CpuChecks& checks) { playCpu(block, checks, playedFile); },
          [&](std::uint64_t played, bool broken) { answerCpu(played, broken); }, cpuAnswered);
    } catch (const std::exception& e) {
      failed(e.what());
    }
    gone(cpuGone);
  }

  void soundBody() {
    try {
      SoundStart start{.file = kept->apu.cpu, .window = window, .ordinal = 0, .prefix = {}};
      body<SoundChecks>(
          *soundStream, std::move(start),
          [&](std::span<const std::uint8_t> block, SoundChecks& checks) { playSound(block, checks); },
          [&](std::uint64_t played, bool broken) { answerSound(played, broken); }, soundAnswered);
    } catch (const std::exception& e) {
      failed(e.what());
    }
    gone(soundGone);
  }

  // A write's byte, kept where the run reads work RAM.
  void keepWrite(const BusAccess& a) {
    if (!a.write) return;
    if (const std::optional<std::size_t> index = ir::Provenance::workRamIndex(a.address)) kept->wram[*index] = a.value;
  }

  void playCpu(std::span<const std::uint8_t> block, const CpuChecks& checks, Cpu65816State& playedFile) {
    std::size_t step = 0;
    std::size_t tapeAt = 0;
    for (RecordReader r{block}; r.more();) {
      const RecordKind kind = r.kind();
      switch (kind) {
        case RecordKind::Access: {
          const BusAccess a = r.payload<BusAccess>();
          keepWrite(a);
          core->recorder.access(a);
          break;
        }
        case RecordKind::EngineAccess: {
          const EngineAccessRecord e = r.payload<EngineAccessRecord>();
          kept->dma[e.access.channel].dmap = e.dmap;
          kept->dma[e.access.channel].bbad = e.bbad;
          keepWrite(e.access);
          core->recorder.access(e.access);
          break;
        }
        case RecordKind::Internal: {
          const InternalRecord internal = r.payload<InternalRecord>();
          core->recorder.internal(internal.address,
                                  internal.has ? std::optional<CycleKind>(internal.kind) : std::nullopt);
          break;
        }
        case RecordKind::FrameStart: {
          const FrameStartRecord frame = r.payload<FrameStartRecord>();
          PpuState& ppu = kept->ppu;
          ppu.inidisp = frame.inidisp;
          ppu.objsel = frame.objsel;
          ppu.bgmode = frame.bgmode;
          ppu.bg1sc = frame.bg1sc;
          ppu.bg2sc = frame.bg2sc;
          ppu.bg3sc = frame.bg3sc;
          ppu.bg4sc = frame.bg4sc;
          ppu.bg12nba = frame.bg12nba;
          ppu.bg34nba = frame.bg34nba;
          ppu.cgram = frame.palette;
          break;
        }
        case RecordKind::Boundary: {
          const BoundaryRecord boundary = r.payload<BoundaryRecord>();
          kept->cpu = boundary.after;
          kept->vpos = boundary.vpos;
          if (step >= checks.steps.size()) throw std::logic_error("a boundary the pool did not check");
          const Checked& c = checks.steps[step++];
          core->boundary(playedFile, boundary.after, boundary.masterCycles, &c, &checks.tape, tapeAt);
          tapeAt += c.calls;
          playedFile = boundary.after;
          playedMaster.store(core->spent, std::memory_order_relaxed);
          break;
        }
        case RecordKind::Keyframe: {
          const KeyframeHeader header = r.payload<KeyframeHeader>();
          const std::span<const std::uint8_t> bytes = r.bytes(header.bytes);
          if (!SnesState::fromBytes(bytes, *keyframeState)) throw std::logic_error("a keyframe that does not read back");
          if (keyframeState->wram != kept->wram) {
            keyframesDiffered.fetch_add(1u, std::memory_order_relaxed);
            if (!keyframeNoted) {
              keyframeNoted = true;
              core->notes.push_back("run: the kept work RAM differs from the machine's at frame " +
                                    std::to_string(header.frame));
            }
          }
          break;
        }
        case RecordKind::Reset: {
          const ResetRecord reset = r.payload<ResetRecord>();
          kept->cpu = reset.after;
          kept->vpos = reset.vpos;
          core->afterReset();
          playedFile = reset.after;
          break;
        }
        default: r.skip(kind); break;
      }
    }
  }

  void playSound(std::span<const std::uint8_t> block, SoundChecks& checks) {
    std::size_t step = 0;
    AudioLockstep& audio = core->audio;
    for (RecordReader r{block}; r.more();) {
      const RecordKind kind = r.kind();
      switch (kind) {
        case RecordKind::SoundAccess: {
          const SoundAccessRecord access = r.payload<SoundAccessRecord>();
          audio.wrote(access.address, access.value, access.write);
          break;
        }
        case RecordKind::KeyOnMemory: {
          const std::span<const std::uint8_t> ram = r.bytes(AudioLockstep::kAudioMemory);
          std::copy(ram.begin(), ram.end(), kept->apu.ram.begin());
          break;
        }
        case RecordKind::SoundStep: {
          const SoundStepRecord record = r.payload<SoundStepRecord>();
          window = record.window;
          if (step >= checks.steps.size()) throw std::logic_error("a sound-CPU boundary the pool did not check");
          audio.follow(checks.steps[step++]);
          break;
        }
        case RecordKind::SoundReset: {
          const SoundResetRecord reset = r.payload<SoundResetRecord>();
          kept->apu.cpu = reset.after;
          kept->apu.dspAddr = reset.dspAddr;
          std::copy(reset.registers.begin(), reset.registers.end(), kept->apu.dsp.regs.begin());
          window = reset.window;
          audio.restartDsp();
          break;
        }
        default: r.skip(kind); break;
      }
    }
    audio.took(checks.lifted);
  }

  // A request is answered once the thread has played every block handed over
  // when it was made, or at once when the thread has stopped playing.
  void answerCpu(std::uint64_t played, bool broken) {
    if (!requested.load(std::memory_order_acquire)) return;
    std::lock_guard lock(mutex);
    if (!requestOpen || cpuAnswered || (!broken && played < cpuTarget)) return;
    cpuPart = RunObservation{};
    core->assembleCpu(cpuPart);
    cpuNotes = core->notes;
    cpuAnswered = true;
    answered.notify_all();
  }

  void answerSound(std::uint64_t played, bool broken) {
    if (!requested.load(std::memory_order_acquire)) return;
    std::lock_guard lock(mutex);
    if (!requestOpen || soundAnswered || (!broken && played < soundTarget)) return;
    soundPart = RunObservation{};
    core->assembleSound(soundPart);
    soundNotes = core->audio.notes;
    soundAnswered = true;
    answered.notify_all();
  }
};

BufferedRun::BufferedRun(Snes& machine, std::span<const std::uint8_t> rom, std::filesystem::path directory,
                         BufferedRunOptions options) {
  if (machine.state().master != 0) {
    throw std::logic_error("a run binds to a machine at power-on, and this one has run");
  }
  if (machine.observer() != nullptr || machine.apuObserver() != nullptr || machine.instructionObserver() != nullptr) {
    throw std::logic_error("a run sets the machine's bus, audio and instruction observers, and one is already set");
  }
  if (!parseCartridgeHeader(rom)) {
    throw std::logic_error("the image is too small to hold a cartridge header at any site");
  }
  impl_ = std::make_unique<Impl>(machine, rom, std::move(directory), options);
}

BufferedRun::~BufferedRun() {
  const std::filesystem::path directory = impl_->directory;
  impl_.reset();
  std::error_code ignored;
  if (std::filesystem::is_empty(directory, ignored)) std::filesystem::remove(directory, ignored);
}

void BufferedRun::keyframe() { impl_->keyframe(); }

void BufferedRun::reset() { impl_->reset(); }

std::uint32_t BufferedRun::frame() const noexcept { return impl_->frames; }

bool BufferedRun::full() const noexcept { return impl_->isFull(); }

bool BufferedRun::atLimit() const noexcept { return impl_->budget.atLimit(); }

bool BufferedRun::memoryOver() const noexcept { return impl_->budget.memoryOver(); }

void BufferedRun::setLimits(std::uint64_t memoryBytes, std::uint64_t diskBytes) {
  impl_->setLimits(memoryBytes, diskBytes);
}

void BufferedRun::hostStopped(bool stopped) { impl_->hostStopped(stopped); }

BufferedRun::Progress BufferedRun::progress() const noexcept {
  const StreamBudget& budget = impl_->budget;
  return Progress{.playedMaster = impl_->playedMaster.load(std::memory_order_relaxed),
                  .hostMaster = impl_->hostMaster.load(std::memory_order_relaxed),
                  .held = budget.held(),
                  .memoryUsed = budget.memoryUsed(),
                  .memoryBytes = budget.memoryBytes(),
                  .diskUsed = budget.diskUsed(),
                  .diskBytes = budget.diskBytes(),
                  .diskWritten = impl_->cpuStream->diskWritten() + impl_->soundStream->diskWritten(),
                  .keyframesDiffered = impl_->keyframesDiffered.load(std::memory_order_relaxed),
                  .threads = impl_->poolThreads,
                  .threadsLowered = budget.threadsLowered()};
}

RunObservation BufferedRun::observation(std::vector<std::string>& notes) { return impl_->observation(notes); }

RunObservation BufferedRun::finish(std::vector<std::string>& notes) { return impl_->finish(notes); }

std::string BufferedRun::failure() const { return impl_->firstFailure(); }

}  // namespace snaggletooth::disasm
