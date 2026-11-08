// The machine's instruction observer: a host told every boundary step() returns
// at, from step(), from run() and from the calls into the guest. Each case runs a
// real program and compares what the observer was told against what a stepped
// machine returns: the register file at each boundary, the master cycles between
// two of them, and where the program counter stood.
//
// The first cases pin the boundaries themselves: one report per step(), the same
// sequence from run() a budget at a time as from step() over a cartridge that
// takes NMIs, waits, runs a general-purpose transfer and an HDMA table each frame,
// a report for every idle cycle of a halted core, an HDMA event between
// instructions reported with the program counter where it was, and a refresh
// counted into the boundary after it. The next pin where `before` is marked: when
// the observer is set, after a restore, after a reset and after the register file
// is written. The last pin what the observer may not do and may not change: the
// machine's state and frames are the same with it set, the boundaries are the same
// under either thread, a call into the guest from inside a report is refused, a
// call from outside one reports its own boundaries, and a move carries it.

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "snaggletooth/snes/snes.h"
#include "snaggletooth/snes/video_frame.h"

namespace snaggletooth {
namespace {

constexpr std::uint8_t kLdaImm = 0xA9u;
constexpr std::uint8_t kLdaAbs = 0xADu;
constexpr std::uint8_t kStaAbs = 0x8Du;
constexpr std::uint8_t kIncAbs = 0xEEu;
constexpr std::uint8_t kJmpAbs = 0x4Cu;
constexpr std::uint8_t kBra = 0x80u;
constexpr std::uint8_t kRts = 0x60u;
constexpr std::uint8_t kRti = 0x40u;
constexpr std::uint8_t kNop = 0xEAu;
constexpr std::uint8_t kWai = 0xCBu;
constexpr std::uint8_t kStp = 0xDBu;

constexpr std::uint64_t kFrameMaster = 262ull * 1364ull;  // one NTSC frame, a little under

// A boundary as the observer was told it, and the machine as state() read inside
// the call.
struct Told {
  Cpu65816State before{};
  Cpu65816State after{};
  std::uint32_t cycles = 0;
  std::uint64_t master = 0;
  bool stateIsAfter = false;
};

bool sameBoundary(const Told& a, const Told& b) {
  return a.before == b.before && a.after == b.after && a.cycles == b.cycles &&
         a.master == b.master;
}

// An observer that keeps every boundary it is told, reading the machine it is set
// on inside the call.
struct Recorder final : InstructionObserver {
  const Snes* machine = nullptr;
  std::vector<Told> told;
  void instruction(const Cpu65816State& before, const Cpu65816State& after,
                   std::uint32_t masterCycles) override {
    Told t;
    t.before = before;
    t.after = after;
    t.cycles = masterCycles;
    t.master = machine->state().master;
    t.stateIsAfter = machine->state().cpu == after;
    told.push_back(t);
  }
};

// An observer that keeps nothing.
struct Nothing final : InstructionObserver {
  void instruction(const Cpu65816State&, const Cpu65816State&, std::uint32_t) override {}
};

// A program assembled from $8000 of a one-bank LoROM image, the bytes appended in
// order.
struct Code {
  std::vector<std::uint8_t> bytes;
  [[nodiscard]] std::uint16_t here() const {
    return static_cast<std::uint16_t>(0x8000u + bytes.size());
  }
  void op(std::initializer_list<std::uint8_t> code) {
    bytes.insert(bytes.end(), code.begin(), code.end());
  }
  // LDA #value ; STA address
  void store(std::uint8_t value, std::uint16_t address) {
    op({kLdaImm, value, kStaAbs, static_cast<std::uint8_t>(address & 0xFFu),
        static_cast<std::uint8_t>(address >> 8)});
  }
  // BRA to `target`, which lies within reach.
  void branchTo(std::uint16_t target) {
    const int offset = static_cast<int>(target) - static_cast<int>(here() + 2u);
    op({kBra, static_cast<std::uint8_t>(offset & 0xFF)});
  }
};

// The image `program` runs from: reset at $8000, the emulation-mode NMI vector at
// $8100, and `at`'s bytes placed at each offset from $8000 it names.
std::vector<std::uint8_t> image(const Code& program,
                                std::initializer_list<std::pair<std::uint16_t, std::vector<std::uint8_t>>> at = {}) {
  std::vector<std::uint8_t> rom = program.bytes;
  rom.resize(0x8000u, 0x00u);
  for (const auto& [address, bytes] : at) {
    std::size_t to = address - 0x8000u;
    for (const std::uint8_t byte : bytes) rom[to++] = byte;
  }
  rom[0x7FFAu] = 0x00u;  // NMI -> $8100
  rom[0x7FFBu] = 0x81u;
  rom[0x7FFCu] = 0x00u;  // reset -> $8000
  rom[0x7FFDu] = 0x80u;
  return rom;
}

// The cartridge every sequence case runs. Each frame it starts a general-purpose
// transfer of 64 bytes from $00:9000 into the VRAM port, waits for the NMI, counts
// the frame at $0010, and goes round; its NMI handler counts at $0011. HDMA channel
// 0 writes COLDATA ($2132) through two repeat entries of 15 lines each from
// $00:9100. The CPU stays in emulation mode, so the NMI takes the vector at $FFFA.
std::vector<std::uint8_t> busyImage() {
  Code c;
  c.store(0x01u, 0x4310u);  // DMAP1: two registers, A to B
  c.store(0x18u, 0x4311u);  // BBAD1: $2118
  c.store(0x90u, 0x4313u);  // A1T1 high: $90
  c.store(0x00u, 0x4314u);  // A1B1: bank $00
  c.store(0x00u, 0x4300u);  // DMAP0: one register
  c.store(0x32u, 0x4301u);  // BBAD0: $2132
  c.store(0x00u, 0x4302u);  // A1T0: $9100
  c.store(0x91u, 0x4303u);
  c.store(0x00u, 0x4304u);  // A1B0: bank $00
  c.store(0x01u, 0x420Cu);  // HDMAEN: channel 0
  c.store(0x80u, 0x4200u);  // NMITIMEN: the NMI on
  const std::uint16_t loop = c.here();
  c.store(0x00u, 0x4312u);  // A1T1 low: $00, back to $9000
  c.store(0x40u, 0x4315u);  // DAS1: 64 bytes
  c.store(0x00u, 0x4316u);
  c.store(0x02u, 0x420Bu);  // MDMAEN: channel 1
  c.op({kWai});
  c.op({kIncAbs, 0x10u, 0x00u});
  c.branchTo(loop);

  std::vector<std::uint8_t> handler = {kIncAbs, 0x11u, 0x00u, kLdaAbs, 0x10u, 0x42u, kRti};
  std::vector<std::uint8_t> data(0x40u);
  for (std::size_t i = 0; i < data.size(); ++i) data[i] = static_cast<std::uint8_t>(i * 3u);
  std::vector<std::uint8_t> table;
  for (int entry = 0; entry < 2; ++entry) {
    table.push_back(0x8Fu);  // repeat, 15 lines, a byte each
    for (int line = 0; line < 15; ++line) table.push_back(static_cast<std::uint8_t>(0x20 + line));
  }
  table.push_back(0x00u);
  return image(c, {{0x8100u, handler}, {0x9000u, data}, {0x9100u, table}});
}

// 256 NOPs from $8000 and a jump back: every instruction moves the program
// counter, so a boundary that leaves it where it was is not an instruction's.
std::vector<std::uint8_t> nopSledImage() {
  Code c;
  for (int i = 0; i < 256; ++i) c.op({kNop});
  c.op({kJmpAbs, 0x00u, 0x80u});
  return image(c);
}

// A program that stops the core at once.
std::vector<std::uint8_t> stoppedImage() {
  Code c;
  c.op({kStp});
  return image(c);
}

// Steps `m` `count` times with `r` set, keeping each call's return and the
// register file it returned at.
struct Stepped {
  std::vector<std::uint32_t> returns;
  std::vector<Cpu65816State> files;
};
Stepped stepWith(Snes& m, std::size_t count) {
  Stepped s;
  for (std::size_t i = 0; i < count; ++i) {
    s.returns.push_back(m.step());
    s.files.push_back(m.state().cpu);
  }
  return s;
}

// Runs a machine over `rom` built as `config` says, a budget of `chunk` master
// cycles at a time until `total` have run, and answers what its observer was told.
std::vector<Told> runTold(SnesConfig config, std::uint64_t total, std::uint64_t chunk) {
  Snes m{config};
  Recorder r;
  r.machine = &m;
  m.setInstructionObserver(&r);
  for (std::uint64_t spent = 0; spent < total; spent += chunk) m.run(chunk);
  return std::move(r.told);
}

// Steps a machine over `rom` built as `config` says until it has returned
// `count` times, and answers what its observer was told.
std::vector<Told> stepTold(SnesConfig config, std::size_t count) {
  Snes m{config};
  Recorder r;
  r.machine = &m;
  m.setInstructionObserver(&r);
  for (std::size_t i = 0; i < count; ++i) m.step();
  return std::move(r.told);
}

// The first place two sequences of boundaries differ, or their shorter length.
std::size_t firstDifference(const std::vector<Told>& a, const std::vector<Told>& b) {
  const std::size_t n = a.size() < b.size() ? a.size() : b.size();
  for (std::size_t i = 0; i < n; ++i) {
    if (!sameBoundary(a[i], b[i])) return i;
  }
  return n;
}

// ---- the boundaries ---------------------------------------------------------

TEST(SnesInstructionObserver, EveryStepReportsItsBoundary) {
  const std::vector<std::uint8_t> rom = busyImage();
  Snes m{SnesConfig{.rom = rom}};
  Recorder r;
  r.machine = &m;
  EXPECT_EQ(m.instructionObserver(), nullptr);
  m.setInstructionObserver(&r);
  EXPECT_EQ(m.instructionObserver(), &r);
  const Cpu65816State start = m.state().cpu;
  const Stepped s = stepWith(m, 20000u);
  ASSERT_EQ(r.told.size(), s.returns.size());
  for (std::size_t i = 0; i < r.told.size(); ++i) {
    ASSERT_EQ(r.told[i].cycles, s.returns[i]) << "report " << i;
    ASSERT_EQ(r.told[i].after, s.files[i]) << "report " << i;
    ASSERT_EQ(r.told[i].before, i == 0 ? start : s.files[i - 1]) << "report " << i;
    ASSERT_TRUE(r.told[i].stateIsAfter) << "report " << i;
  }
  m.setInstructionObserver(nullptr);
  m.step();
  EXPECT_EQ(r.told.size(), s.returns.size()) << "no observer, no report";
}

TEST(SnesInstructionObserver, RunReportsTheBoundariesStepReturnsAt) {
  const std::vector<std::uint8_t> rom = busyImage();
  const std::vector<Told> run = runTold(SnesConfig{.rom = rom}, 3u * kFrameMaster, 1000u);
  const std::vector<Told> step = stepTold(SnesConfig{.rom = rom}, run.size());
  ASSERT_EQ(step.size(), run.size());
  EXPECT_EQ(firstDifference(run, step), run.size());

  // The cartridge did what the sequence is meant to cover: it waited, took the
  // NMI, ran its transfers and its HDMA table. The table's events land while the
  // core waits: a report of the halted core that is neither an idle cycle (6) nor
  // an idle cycle carrying a refresh (46) is an HDMA event.
  std::size_t waiting = 0, nmis = 0, transfers = 0, hdma = 0;
  for (const Told& t : run) {
    if (t.after.run == CpuRunState::Waiting) ++waiting;
    if (t.after.pc == 0x8100u) ++nmis;
    if (t.cycles > 64u * 8u) ++transfers;
    if (t.before.run == CpuRunState::Waiting && t.cycles != 6u && t.cycles != 6u + 40u) ++hdma;
  }
  EXPECT_GT(waiting, 0u);
  EXPECT_GE(nmis, 2u);
  EXPECT_GE(transfers, 2u);
  EXPECT_GT(hdma, 0u);
}

TEST(SnesInstructionObserver, AHaltedCoreReportsEveryIdleCycle) {
  const std::vector<std::uint8_t> rom = stoppedImage();
  Snes m{SnesConfig{.rom = rom}};
  m.step();  // STP
  ASSERT_EQ(m.state().cpu.run, CpuRunState::Stopped);
  Recorder r;
  r.machine = &m;
  m.setInstructionObserver(&r);
  const std::uint64_t from = m.state().master;
  m.run(4u * 1364u);
  ASSERT_GT(r.told.size(), 0u);
  std::uint64_t sum = 0;
  for (const Told& t : r.told) {
    EXPECT_EQ(t.after.run, CpuRunState::Stopped);
    EXPECT_EQ(t.after, t.before);
    sum += t.cycles;
  }
  EXPECT_EQ(sum, r.told.back().master - from);

  // A stepped twin returns exactly where the run reported.
  Snes twin{SnesConfig{.rom = rom}};
  twin.step();
  for (std::size_t i = 0; i < r.told.size(); ++i) {
    ASSERT_EQ(twin.step(), r.told[i].cycles) << "idle cycle " << i;
  }
}

// A register file a host writes as halted part-way through an instruction: step()
// returns after each idle cycle all the same, and the report comes at each one.
TEST(SnesInstructionObserver, AFileWrittenHaltedInsideAnInstructionReportsEveryIdleCycle) {
  const std::vector<std::uint8_t> rom = nopSledImage();
  Snes m{SnesConfig{.rom = rom}};
  Snes twin{SnesConfig{.rom = rom}};
  m.step();
  twin.step();
  Cpu65816State file = m.state().cpu;
  file.run = CpuRunState::Waiting;
  file.ir = kNop;
  file.tcu = 1;
  m.setCpuState(file);
  twin.setCpuState(file);
  Recorder r;
  r.machine = &m;
  m.setInstructionObserver(&r);
  m.run(2000u);
  ASSERT_GT(r.told.size(), 100u);
  for (std::size_t i = 0; i < r.told.size(); ++i) {
    ASSERT_EQ(twin.step(), r.told[i].cycles) << "idle cycle " << i;
    ASSERT_EQ(r.told[i].after.run, CpuRunState::Waiting);
  }
}

// The same file with an NMI pending: the first idle cycle wakes the core part-way
// through its instruction, and step() returns after that cycle all the same, as it
// returns after every idle cycle; the report comes there too, then at the end of the
// instruction the core goes on with.
TEST(SnesInstructionObserver, ACoreWokenInsideAnInstructionReportsTheCycleThatWokeIt) {
  const std::vector<std::uint8_t> rom = nopSledImage();
  Snes m{SnesConfig{.rom = rom}};
  Snes twin{SnesConfig{.rom = rom}};
  m.step();
  twin.step();
  Cpu65816State file = m.state().cpu;
  file.run = CpuRunState::Waiting;
  file.ir = kNop;
  file.tcu = 1;
  file.nmiPending = true;
  m.setCpuState(file);
  twin.setCpuState(file);
  Recorder r;
  r.machine = &m;
  m.setInstructionObserver(&r);
  m.run(200u);
  ASSERT_GE(r.told.size(), 2u);
  EXPECT_EQ(r.told[0].before.run, CpuRunState::Waiting);
  EXPECT_EQ(r.told[0].after.run, CpuRunState::Running) << "the first report is the cycle that woke the core";
  for (std::size_t i = 0; i < r.told.size(); ++i) {
    ASSERT_EQ(twin.step(), r.told[i].cycles) << "report " << i;
    ASSERT_EQ(twin.state().cpu, r.told[i].after) << "report " << i;
  }
}

// The NOP sled with HDMA channel 0 armed against a table in work RAM, as the
// transfer tests arm it, and a bus observer beside the instruction observer: every
// access since the last report, by who made it.
struct Sources final : BusObserver {
  std::vector<AccessSource> since;
  void access(const BusAccess& a) override { since.push_back(a.source); }
  void internal(std::uint32_t, std::optional<CycleKind>) override {}
};
struct HdmaBoundaries final : InstructionObserver {
  Sources* bus = nullptr;
  std::size_t betweenInstructions = 0;
  std::size_t notOnlyHdma = 0;
  void instruction(const Cpu65816State& before, const Cpu65816State& after,
                   std::uint32_t masterCycles) override {
    if (before.pc == after.pc && before.pbr == after.pbr) {
      ++betweenInstructions;
      bool onlyHdma = masterCycles > 0u && !bus->since.empty();
      for (const AccessSource source : bus->since) onlyHdma = onlyHdma && source == AccessSource::Hdma;
      if (!onlyHdma) ++notOnlyHdma;
    }
    bus->since.clear();
  }
};

TEST(SnesInstructionObserver, AnHdmaEventBetweenInstructionsIsABoundaryWithTheProgramCounterWhereItWas) {
  const std::vector<std::uint8_t> rom = nopSledImage();
  Snes m{SnesConfig{.rom = rom}};
  SnesState s = m.state();
  s.dma[0] = DmaChannel{.dmap = 0x00u, .bbad = 0x32u, .a1t = 0x0300u, .a1b = 0x7Eu};
  s.hdmaen = 0x01u;
  std::uint16_t at = 0x0300u;
  for (int entry = 0; entry < 2; ++entry) {
    s.wram[at++] = 0x8Fu;
    for (int line = 0; line < 15; ++line) s.wram[at++] = static_cast<std::uint8_t>(line);
  }
  s.wram[at] = 0x00u;
  m.restore(s);
  Sources bus;
  HdmaBoundaries boundaries;
  boundaries.bus = &bus;
  m.setObserver(&bus);
  m.setInstructionObserver(&boundaries);
  m.run(2u * kFrameMaster);
  EXPECT_GT(boundaries.betweenInstructions, 0u);
  EXPECT_EQ(boundaries.notOnlyHdma, 0u) << "a boundary leaving the program counter where it was is an HDMA event";
}

TEST(SnesInstructionObserver, ARefreshIsNotABoundaryAndItsCyclesCountTowardTheNext) {
  const std::vector<std::uint8_t> rom = nopSledImage();
  Snes m{SnesConfig{.rom = rom}};
  Recorder r;
  r.machine = &m;
  m.step();
  const std::uint64_t from = m.state().master;
  m.setInstructionObserver(&r);
  m.run(kFrameMaster);
  ASSERT_GT(r.told.size(), 0u);
  std::uint64_t sum = 0;
  std::size_t carryingARefresh = 0;
  for (const Told& t : r.told) {
    EXPECT_FALSE(t.before.pc == t.after.pc) << "no boundary of its own for a refresh";
    sum += t.cycles;
    // A NOP is 14 master cycles from the cartridge and the JMP 24; a report past
    // those carries a refresh's 40.
    if (t.cycles == 14u + 40u || t.cycles == 24u + 40u) {
      ++carryingARefresh;
    } else {
      EXPECT_TRUE(t.cycles == 14u || t.cycles == 24u) << t.cycles;
    }
  }
  EXPECT_EQ(sum, r.told.back().master - from) << "every master cycle lands in a report";
  EXPECT_EQ(carryingARefresh, 262u) << "one refresh a line";
}

// ---- where `before` is marked -----------------------------------------------

TEST(SnesInstructionObserver, BeforeIsMarkedWhenTheObserverIsSet) {
  const std::vector<std::uint8_t> rom = busyImage();
  Snes m{SnesConfig{.rom = rom}};
  m.run(777u);  // stops part-way through an instruction
  Recorder r;
  r.machine = &m;
  const Cpu65816State set = m.state().cpu;
  const std::uint64_t setAt = m.state().master;
  m.setInstructionObserver(&r);
  m.step();
  ASSERT_EQ(r.told.size(), 1u);
  EXPECT_EQ(r.told[0].before, set);
  EXPECT_EQ(r.told[0].cycles, r.told[0].master - setAt);
}

TEST(SnesInstructionObserver, BeforeIsMarkedAfterARestore) {
  const std::vector<std::uint8_t> rom = busyImage();
  Snes m{SnesConfig{.rom = rom}};
  m.run(5000u);
  const SnesState saved = m.state();
  Recorder r;
  r.machine = &m;
  m.setInstructionObserver(&r);
  m.run(20000u);
  ASSERT_TRUE(m.restore(saved));
  EXPECT_EQ(m.instructionObserver(), &r) << "restore() leaves the observer in place";
  r.told.clear();
  m.step();
  ASSERT_EQ(r.told.size(), 1u);
  EXPECT_EQ(r.told[0].before, saved.cpu);
  EXPECT_EQ(r.told[0].cycles, r.told[0].master - saved.master);
}

TEST(SnesInstructionObserver, BeforeIsMarkedAfterAReset) {
  const std::vector<std::uint8_t> rom = busyImage();
  Snes m{SnesConfig{.rom = rom}};
  Recorder r;
  r.machine = &m;
  m.setInstructionObserver(&r);
  m.run(30000u);
  m.reset();
  EXPECT_EQ(m.instructionObserver(), &r) << "reset() leaves the observer in place";
  const Cpu65816State afterReset = m.state().cpu;
  r.told.clear();
  m.step();
  ASSERT_EQ(r.told.size(), 1u);
  EXPECT_EQ(r.told[0].before, afterReset);
  EXPECT_EQ(r.told[0].cycles, r.told[0].master) << "the master count starts again at the reset";
}

TEST(SnesInstructionObserver, BeforeIsMarkedAfterTheRegisterFileIsWritten) {
  const std::vector<std::uint8_t> rom = busyImage();
  Snes m{SnesConfig{.rom = rom}};
  Recorder r;
  r.machine = &m;
  m.setInstructionObserver(&r);
  m.run(3000u);
  m.step();  // to a boundary
  Cpu65816State file = m.state().cpu;
  file.a = 0x1234u;
  file.x = 0x0056u;
  const std::uint64_t writtenAt = m.state().master;
  m.setCpuState(file);
  r.told.clear();
  m.step();
  ASSERT_EQ(r.told.size(), 1u);
  EXPECT_EQ(r.told[0].before, file);
  EXPECT_EQ(r.told[0].cycles, r.told[0].master - writtenAt);
}

// ---- what the observer changes ----------------------------------------------

// A frame observer keeping every frame's pixels.
struct Frames final : FrameObserver {
  std::vector<std::vector<std::uint8_t>> frames;
  void frame(const VideoFrame& f) override { frames.emplace_back(f.pixels.begin(), f.pixels.end()); }
};

TEST(SnesInstructionObserver, AnObserverMovesNothing) {
  const std::vector<std::uint8_t> rom = busyImage();
  Snes plain{SnesConfig{.rom = rom}};
  Snes observed{SnesConfig{.rom = rom}};
  Frames plainFrames, observedFrames;
  plain.setFrameObserver(&plainFrames);
  observed.setFrameObserver(&observedFrames);
  Nothing nothing;
  observed.setInstructionObserver(&nothing);
  for (std::uint64_t spent = 0; spent < 3u * kFrameMaster; spent += 4096u) {
    plain.run(4096u);
    observed.run(4096u);
    ASSERT_EQ(plain.state().asBytes(), observed.state().asBytes()) << "at master " << spent;
  }
  ASSERT_GE(plainFrames.frames.size(), 2u);
  EXPECT_EQ(plainFrames.frames, observedFrames.frames);
  EXPECT_EQ(plain.state().wram[0x11], observed.state().wram[0x11]);
  EXPECT_GE(int{observed.state().wram[0x11]}, 2) << "the NMI ran";
}

TEST(SnesInstructionObserver, TheBoundariesAreTheSameUnderTheThreads) {
  const std::vector<std::uint8_t> rom = busyImage();
  const std::vector<Told> plain = runTold(SnesConfig{.rom = rom}, 3u * kFrameMaster, 1000u);
  ASSERT_GT(plain.size(), 0u);
  for (int threads = 1; threads <= 3; ++threads) {
    SnesConfig config{.rom = rom};
    config.apuThread = (threads & 1) != 0;
    config.ppuThread = (threads & 2) != 0;
    const std::vector<Told> run = runTold(config, 3u * kFrameMaster, 1000u);
    ASSERT_EQ(run.size(), plain.size()) << "threads " << threads;
    EXPECT_EQ(firstDifference(run, plain), plain.size()) << "run, threads " << threads;
    const std::vector<Told> step = stepTold(config, plain.size());
    EXPECT_EQ(firstDifference(step, plain), plain.size()) << "step, threads " << threads;
  }
}

TEST(SnesInstructionObserver, AnObserverClearedOnAThreadedMachineLeavesItsStepsAsTheyWere) {
  const std::vector<std::uint8_t> rom = busyImage();
  Snes plain{SnesConfig{.rom = rom}};
  for (int i = 0; i < 30000; ++i) plain.step();
  for (int threads = 1; threads <= 3; ++threads) {
    SnesConfig config{.rom = rom};
    config.apuThread = (threads & 1) != 0;
    config.ppuThread = (threads & 2) != 0;
    Snes m{config};
    Nothing nothing;
    m.setInstructionObserver(&nothing);
    m.step();
    m.setInstructionObserver(nullptr);
    for (int i = 1; i < 30000; ++i) m.step();
    EXPECT_EQ(m.state().asBytes(), plain.state().asBytes()) << "threads " << threads;
  }
}

// An observer that tries both calls into the guest from inside its call.
struct Caller final : InstructionObserver {
  Snes* machine = nullptr;
  std::size_t calls = 0;
  std::size_t accepted = 0;
  void instruction(const Cpu65816State&, const Cpu65816State&, std::uint32_t) override {
    ++calls;
    if (machine->callInContext(0x008100u, Standin::Near, 100u)) ++accepted;
    if (machine->callOnStack(0x008100u, 0x01F0u, Standin::Near, 100u)) ++accepted;
  }
};

// A program that runs NOPs from $8000 and goes round, with LDA #$22 ; RTS at $8100.
std::vector<std::uint8_t> callerImage() {
  Code c;
  for (int i = 0; i < 16; ++i) c.op({kNop});
  c.branchTo(0x8000u);
  return image(c, {{0x8100u, {kLdaImm, 0x22u, kRts}}});
}

TEST(SnesInstructionObserver, ACallIntoTheGuestFromInsideAReportIsRefused) {
  const std::vector<std::uint8_t> rom = callerImage();
  Snes m{SnesConfig{.rom = rom}};
  Caller caller;
  caller.machine = &m;
  m.setInstructionObserver(&caller);
  m.run(2000u);
  ASSERT_GT(caller.calls, 0u);
  EXPECT_EQ(caller.accepted, 0u);
  m.setInstructionObserver(nullptr);
  EXPECT_TRUE(m.callInContext(0x008100u, Standin::Near, 100u)) << "the same call, outside a report";
}

TEST(SnesInstructionObserver, TheCallsIntoTheGuestReportTheirBoundaries) {
  const std::vector<std::uint8_t> rom = callerImage();
  Snes m{SnesConfig{.rom = rom}};
  Recorder r;
  r.machine = &m;
  m.setInstructionObserver(&r);
  m.run(295u);  // part-way through a NOP: its opcode fetch ends at 296
  ASSERT_FALSE(r.told.empty());
  ASSERT_NE(r.told.back().master, m.state().master) << "the budget stopped inside an instruction";
  const std::size_t beforeCall = r.told.size();
  ASSERT_TRUE(m.callInContext(0x008100u, Standin::Near, 100u));
  const Cpu65816State guest = m.state().cpu;
  // The guest's instruction finished, then the routine's two.
  ASSERT_EQ(r.told.size(), beforeCall + 3u);
  const Told& finished = r.told[beforeCall];
  const Told& load = r.told[beforeCall + 1u];
  const Told& back = r.told[beforeCall + 2u];
  EXPECT_EQ(finished.after.pbr, guest.pbr);
  EXPECT_EQ(finished.after.pc, guest.pc) << "the guest resumes where its instruction ended";
  EXPECT_EQ(int{load.after.pc}, 0x8102);
  EXPECT_EQ(load.after.a & 0xFFu, 0x22u);
  EXPECT_EQ(back.after.pc, guest.pc) << "the return lands on the guest's program counter";
  EXPECT_EQ(back.after.s, guest.s);
  // The next report counts from the guest's file, put back.
  m.step();
  ASSERT_EQ(r.told.size(), beforeCall + 4u);
  EXPECT_EQ(r.told.back().before, guest);
}

TEST(SnesInstructionObserver, ACallOnAStackOfTheHostsOwnReportsItsBoundaries) {
  const std::vector<std::uint8_t> rom = callerImage();
  Snes m{SnesConfig{.rom = rom}};
  Recorder r;
  r.machine = &m;
  m.setInstructionObserver(&r);
  m.run(295u);  // part-way through a NOP: its opcode fetch ends at 296
  ASSERT_FALSE(r.told.empty());
  ASSERT_NE(r.told.back().master, m.state().master) << "the budget stopped inside an instruction";
  const std::size_t beforeCall = r.told.size();
  ASSERT_TRUE(m.callOnStack(0x008100u, 0x01F0u, Standin::Near, 100u));
  // The guest's instruction finished, then the routine's two, on the host's stack.
  ASSERT_EQ(r.told.size(), beforeCall + 3u);
  const Told& finished = r.told[beforeCall];
  const Told& load = r.told[beforeCall + 1u];
  const Told& back = r.told[beforeCall + 2u];
  EXPECT_EQ(int{load.after.pc}, 0x8102);
  EXPECT_EQ(int{load.after.s}, 0x01EE) << "the landing pushed on the host's stack";
  EXPECT_EQ(back.after.pc, finished.after.pc) << "the return lands where the guest's instruction ended";
  EXPECT_EQ(int{back.after.s}, 0x01F0);
  EXPECT_EQ(back.after, m.state().cpu) << "nothing is put back";
}

// A frame observer that throws at the first frame it is handed.
struct ThrowsOnce final : FrameObserver {
  bool thrown = false;
  void frame(const VideoFrame&) override {
    if (!thrown) {
      thrown = true;
      throw 1;
    }
  }
};

TEST(SnesInstructionObserver, AStepAReportThrewFromIsFollowedByReportedSteps) {
  const std::vector<std::uint8_t> rom = busyImage();
  for (int threads = 0; threads <= 3; ++threads) {
    SnesConfig config{.rom = rom};
    config.apuThread = (threads & 1) != 0;
    config.ppuThread = (threads & 2) != 0;
    Snes m{config};
    ThrowsOnce frames;
    m.setFrameObserver(&frames);
    Recorder r;
    r.machine = &m;
    m.setInstructionObserver(&r);
    bool caught = false;
    for (int i = 0; i < 200000 && !caught; ++i) {
      try {
        m.step();
      } catch (int) {
        caught = true;
      }
    }
    ASSERT_TRUE(caught) << "threads " << threads;
    const std::size_t before = r.told.size();
    m.step();
    m.step();
    EXPECT_EQ(r.told.size(), before + 2u) << "threads " << threads;
    EXPECT_EQ(r.told.back().after, m.state().cpu) << "threads " << threads;
  }
}

TEST(SnesInstructionObserver, AMoveCarriesTheObserver) {
  const std::vector<std::uint8_t> rom = busyImage();
  Snes first{SnesConfig{.rom = rom}};
  Recorder r;
  r.machine = &first;
  first.setInstructionObserver(&r);
  first.step();
  const Cpu65816State last = first.state().cpu;
  Snes moved{std::move(first)};
  EXPECT_EQ(moved.instructionObserver(), &r);
  EXPECT_EQ(first.instructionObserver(), nullptr);  // NOLINT(bugprone-use-after-move)
  r.machine = &moved;
  moved.step();
  ASSERT_EQ(r.told.size(), 2u);
  EXPECT_EQ(r.told[1].before, last);
  EXPECT_EQ(r.told[1].after, moved.state().cpu);
}

}  // namespace
}  // namespace snaggletooth
