// The audio thread: a machine built with SnesConfig::apuThread on runs the audio
// machine's cycles on a thread of its own, and one built with it off, the default,
// runs them inside the console's cycles. The cases hold the two equal — the whole
// state and every audio frame — at the return of every call, over a cartridge that
// talks to the audio machine through its ports on every frame: a CPU store and a DMA
// to port 0, and a poll of port 0 until the audio CPU echoes the byte back. They pin
// the faces that meet the thread: a port read, an engine's write, snapshot and
// restore, the move, the reset line, the audio observer's thread and the refusal of a
// call from it, a report that throws and what a report reads, a peek from a bus
// observer, a guest call between calls, the thread's park and wake and its end with
// the machine, and a burst of port writes the thread is behind on.

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "snaggletooth/snes/snes.h"

namespace snaggletooth {
namespace {

// One NTSC frame in master cycles, and a quarter of one.
constexpr std::uint64_t kFrame = 357366u;
constexpr std::uint64_t kQuarterFrame = kFrame / 4u;

// The work-RAM bytes the handshake cartridge keeps.
constexpr std::size_t kIndex = 0x10u;   // the byte index the program sends next
constexpr std::size_t kFrames = 0x12u;  // frames counted by the NMI handler
constexpr std::size_t kEcho = 0x14u;    // the last byte the program read from port 0

// A one-bank LoROM image, run in emulation mode, from `program` at $8000 with
// `nmi` at $8100.
std::vector<std::uint8_t> image(std::initializer_list<std::uint8_t> program,
                                std::initializer_list<std::uint8_t> nmi) {
  std::vector<std::uint8_t> rom(0x8000u, 0x00u);
  std::size_t at = 0x0000u;
  for (const std::uint8_t byte : program) rom[at++] = byte;
  at = 0x0100u;
  for (const std::uint8_t byte : nmi) rom[at++] = byte;
  rom[0x7FFAu] = 0x00u;  // emulation-mode NMI -> $8100
  rom[0x7FFBu] = 0x81u;
  rom[0x7FFCu] = 0x00u;  // reset -> $8000
  rom[0x7FFDu] = 0x80u;
  return rom;
}

// The handshake cartridge. It waits for the upload stub's ready byte, starts a
// transfer to audio RAM $0200, and then sends bytes for ever: the data byte to
// port 1, the index to port 0 — a CPU store for an even index, a DMA of a
// four-byte work-RAM buffer holding the index for an odd one — and a poll of
// port 0 until the stub echoes the index, each read kept in work RAM.
//
//   $8000  LDA #$80 ; STA $4200           NMI on
//   $8005  LDA $2140 ; CMP #$AA ; BNE     wait for the stub's ready byte
//   $800C  LDA #$00 ; STA $4300 ; STA $4303 ; STA $4304 ; STA $4306
//          LDA #$40 ; STA $4301           channel 0: one register, $2140, from $00:00xx
//   $801F  LDA #$01 ; STA $2141           a transfer, not a run
//          LDA #$00 ; STA $2142 ; LDA #$02 ; STA $2143    to $0200
//          LDA #$CC ; STA $2140           the kick
//   $8033  CMP $2140 ; BNE $8033          wait for its echo
//   $8038  STZ $10                        the index starts at zero
//   $803A  LDA $10 ; EOR #$5A ; ORA #$01 ; STA $2141      the data byte, never zero
//   $8043  LDA $10 ; LSR A ; BCS $804F    an odd index goes by DMA
//   $8048  LDA $10 ; STA $2140 ; BRA $8068
//   $804F  LDA $10 ; STA $20 ; STA $21 ; STA $22 ; STA $23
//          LDA #$20 ; STA $4302 ; LDA #$04 ; STA $4305    the buffer, four bytes
//          LDA #$01 ; STA $420B           the transfer
//   $8068  LDA $2140 ; STA $14 ; CMP $10 ; BNE $8068      poll for the echo
//   $8071  INC $10 ; BRA $803A
//
//   $8100  PHA ; LDA $4210 ; INC $12 ; PLA ; RTI          count the frame
std::vector<std::uint8_t> handshake() {
  return image(
      {
          0xA9, 0x80, 0x8D, 0x00, 0x42,                    // $8000
          0xAD, 0x40, 0x21, 0xC9, 0xAA, 0xD0, 0xF9,        // $8005
          0xA9, 0x00, 0x8D, 0x00, 0x43, 0x8D, 0x03, 0x43,  // $800C
          0x8D, 0x04, 0x43, 0x8D, 0x06, 0x43,              // $8014
          0xA9, 0x40, 0x8D, 0x01, 0x43,                    // $801A
          0xA9, 0x01, 0x8D, 0x41, 0x21,                    // $801F
          0xA9, 0x00, 0x8D, 0x42, 0x21,                    // $8024
          0xA9, 0x02, 0x8D, 0x43, 0x21,                    // $8029
          0xA9, 0xCC, 0x8D, 0x40, 0x21,                    // $802E
          0xCD, 0x40, 0x21, 0xD0, 0xFB,                    // $8033
          0x64, 0x10,                                      // $8038
          0xA5, 0x10, 0x49, 0x5A, 0x09, 0x01, 0x8D, 0x41, 0x21,  // $803A
          0xA5, 0x10, 0x4A, 0xB0, 0x07,                    // $8043
          0xA5, 0x10, 0x8D, 0x40, 0x21, 0x80, 0x19,        // $8048
          0xA5, 0x10, 0x85, 0x20, 0x85, 0x21, 0x85, 0x22, 0x85, 0x23,  // $804F
          0xA9, 0x20, 0x8D, 0x02, 0x43,                    // $8059
          0xA9, 0x04, 0x8D, 0x05, 0x43,                    // $805E
          0xA9, 0x01, 0x8D, 0x0B, 0x42,                    // $8063
          0xAD, 0x40, 0x21, 0x85, 0x14, 0xC5, 0x10, 0xD0, 0xF7,  // $8068
          0xE6, 0x10, 0x80, 0xC5,                          // $8071
      },
      {0x48, 0xAD, 0x10, 0x42, 0xE6, 0x12, 0x68, 0x40});
}

// A cartridge that stores to port 0 on every pass and never reads a port:
//   $8000  STA $2140 ; INC A ; AND #$7F ; BRA $8000
// The mask keeps the byte off $CC, so the stub never starts an upload.
std::vector<std::uint8_t> portWriter() {
  return image({0x8D, 0x40, 0x21, 0x1A, 0x29, 0x7F, 0x80, 0xF8}, {0x40});
}

// The port writer with a routine of the host's to call at $8200:
//   $8200  LDA $2140 ; STA $30 ; RTS     read port 0 into work RAM
constexpr std::uint32_t kPortRoutine = 0x008200u;
constexpr std::size_t kRoutineRead = 0x30u;
std::vector<std::uint8_t> portWriterWithRoutine() {
  std::vector<std::uint8_t> rom = portWriter();
  const std::array<std::uint8_t, 6> routine = {0xAD, 0x40, 0x21, 0x85, 0x30, 0x60};
  for (std::size_t i = 0; i < routine.size(); ++i) rom[0x0200u + i] = routine[i];
  return rom;
}

// NOP ; BRA $8000.
std::vector<std::uint8_t> loop() { return image({0xEA, 0x80, 0xFD}, {0x40}); }

// An audio program in place of the upload stub, at $0200: it reads port 0 for ever
// and keeps the sum of every byte it read at $0020, so a write that lands at another
// audio cycle changes the sum.
//   $0200  MOV A,$F4 ; CLRC ; ADC A,$20 ; MOV $20,A ; BRA $0200
constexpr std::uint16_t kReadSum = 0x0020u;
void loadPortReader(Snes& machine) {
  SnesState s = machine.state();
  const std::array<std::uint8_t, 9> program = {0xE4, 0xF4, 0x60, 0x84, 0x20, 0xC4, 0x20, 0x2F, 0xF7};
  for (std::size_t i = 0; i < program.size(); ++i) s.apu.ram[0x0200u + i] = program[i];
  s.apu.cpu.pc = 0x0200u;
  s.apu.cpu.psw = 0x00u;  // the direct page at $00xx, where $F4 is the port
  machine.restore(s);
}

SnesConfig config(const std::vector<std::uint8_t>& rom, bool threaded) {
  return SnesConfig{
      .rom = rom,
      .region = Region::Ntsc,
      .iplStub = true,
      .map = std::nullopt,
      .saveRamBytes = std::nullopt,
      .bootRom = std::nullopt,
      .apuThread = threaded,
  };
}

// The same cartridge on a machine with the thread off and one with it on.
struct Twin {
  Snes lockstep;
  Snes threaded;
};

Twin twin(const std::vector<std::uint8_t>& rom) {
  return Twin{.lockstep = Snes(config(rom, false)), .threaded = Snes(config(rom, true))};
}

// The whole state and every audio frame produced since the last look, equal.
::testing::AssertionResult equal(Snes& lockstep, Snes& threaded) {
  if (!(lockstep.state() == threaded.state())) {
    return ::testing::AssertionFailure()
           << "the states differ: master " << lockstep.state().master << " and "
           << threaded.state().master;
  }
  if (lockstep.takeFrames() != threaded.takeFrames()) {
    return ::testing::AssertionFailure() << "the audio frames differ";
  }
  return ::testing::AssertionSuccess();
}

// The byte the stub stores for index `index`.
std::uint8_t dataByte(std::uint8_t index) {
  return static_cast<std::uint8_t>((index ^ 0x5Au) | 0x01u);
}

TEST(SnesApuThread, AThreadedMachineAndALockstepMachineAreEqualAtEveryRunsReturn) {
  Twin t = twin(handshake());
  for (int call = 0; call < 240; ++call) {
    t.lockstep.run(kQuarterFrame);
    t.threaded.run(kQuarterFrame);
    ASSERT_TRUE(equal(t.lockstep, t.threaded)) << "after run " << call;
  }
  EXPECT_GE(t.threaded.state().wram[kFrames], 59u) << "the NMI handler counted the frames";
  // The bytes the program sent landed in audio RAM, the odd ones through the DMA.
  for (std::uint8_t index = 0; index < 8u; ++index) {
    EXPECT_EQ(t.threaded.peekApu(static_cast<std::uint16_t>(0x0200u + index)), dataByte(index))
        << "byte " << static_cast<int>(index);
  }
}

TEST(SnesApuThread, AThreadedMachineAndALockstepMachineAreEqualAtEveryStepsReturn) {
  Twin t = twin(handshake());
  for (int call = 0; call < 20000; ++call) {
    t.lockstep.step();
    t.threaded.step();
    ASSERT_TRUE(equal(t.lockstep, t.threaded)) << "after step " << call;
  }
  t.lockstep.run(kQuarterFrame);
  t.threaded.run(kQuarterFrame);
  ASSERT_TRUE(equal(t.lockstep, t.threaded)) << "after the run";
  for (int call = 0; call < 2000; ++call) {
    t.lockstep.step();
    t.threaded.step();
    ASSERT_TRUE(equal(t.lockstep, t.threaded)) << "after step " << call << " past the run";
  }
}

TEST(SnesApuThread, APortReadMidBurstAnswersTheLockstepByte) {
  // Step by step through the first three echoes: the program's reads of port 0 land
  // in work RAM, and every one lands at the same cycle on both machines.
  Twin t = twin(handshake());
  int steps = 0;
  while (t.lockstep.state().wram[kIndex] != 3u) {
    ASSERT_LT(++steps, 200000) << "the handshake never reached the third echo";
    t.lockstep.step();
    t.threaded.step();
    ASSERT_TRUE(equal(t.lockstep, t.threaded)) << "after step " << steps;
  }
  EXPECT_EQ(t.threaded.state().wram[kIndex], 3u);
  EXPECT_EQ(t.threaded.state().wram[kEcho], 2u) << "the read that ended the third poll";
  EXPECT_EQ(t.threaded.state().wram[kEcho], t.lockstep.state().wram[kEcho]);
  EXPECT_EQ(t.threaded.state().master, t.lockstep.state().master);
}

TEST(SnesApuThread, AnEngineWriteToThePortLandsAtTheSameCycle) {
  // Index 1 goes to port 0 by DMA. Step until its echo has come back, then run to
  // the end of the frame the transfer ran in.
  Twin t = twin(handshake());
  int steps = 0;
  while (t.lockstep.state().wram[kIndex] != 2u) {
    ASSERT_LT(++steps, 200000) << "the handshake never reached the transfer's echo";
    t.lockstep.step();
    t.threaded.step();
    ASSERT_TRUE(equal(t.lockstep, t.threaded)) << "after step " << steps;
  }
  EXPECT_EQ(t.threaded.state().apu.inputPorts[0], 1u) << "the transfer's byte, in the port";
  EXPECT_EQ(t.threaded.state().apu.outputPorts[0], 1u) << "and its echo";
  t.lockstep.run(kFrame);
  t.threaded.run(kFrame);
  EXPECT_TRUE(equal(t.lockstep, t.threaded));
  EXPECT_EQ(t.threaded.peekApu(0x0201u), dataByte(1u));
}

TEST(SnesApuThread, ASnapshotOfAThreadedMachineRestoresIntoALockstepOneAndBack) {
  Twin t = twin(handshake());
  for (int call = 0; call < 10; ++call) {
    t.lockstep.run(kQuarterFrame);
    t.threaded.run(kQuarterFrame);
  }
  t.lockstep.run(1001u);  // mid-instruction
  t.threaded.run(1001u);
  ASSERT_TRUE(equal(t.lockstep, t.threaded));
  const SnesState fromThreaded = t.threaded.state();
  const SnesState fromLockstep = t.lockstep.state();
  for (int call = 0; call < 7; ++call) {
    t.lockstep.run(kQuarterFrame);
    t.threaded.run(kQuarterFrame);
  }
  t.lockstep.restore(fromThreaded);
  t.threaded.restore(fromLockstep);
  ASSERT_TRUE(equal(t.lockstep, t.threaded)) << "after the swap";
  for (int call = 0; call < 20; ++call) {
    t.lockstep.run(kQuarterFrame);
    t.threaded.run(kQuarterFrame);
    ASSERT_TRUE(equal(t.lockstep, t.threaded)) << "after run " << call << " past the restore";
  }
}

TEST(SnesApuThread, AMovedThreadedMachineRunsOn) {
  Twin t = twin(handshake());
  for (int call = 0; call < 10; ++call) {
    t.lockstep.run(kQuarterFrame);
    t.threaded.run(kQuarterFrame);
  }
  t.lockstep.run(777u);
  t.threaded.run(777u);
  Snes moved(std::move(t.threaded));
  ASSERT_TRUE(equal(t.lockstep, moved)) << "after the move";
  for (int call = 0; call < 20; ++call) {
    t.lockstep.run(kQuarterFrame);
    moved.run(kQuarterFrame);
    ASSERT_TRUE(equal(t.lockstep, moved)) << "after run " << call << " past the move";
  }
}

TEST(SnesApuThread, AResetMidRunLeavesBothMachinesEqual) {
  Twin t = twin(handshake());
  for (int call = 0; call < 10; ++call) {
    t.lockstep.run(kQuarterFrame);
    t.threaded.run(kQuarterFrame);
  }
  t.lockstep.run(555u);
  t.threaded.run(555u);
  t.lockstep.reset();
  t.threaded.reset();
  ASSERT_TRUE(equal(t.lockstep, t.threaded)) << "after the reset";
  for (int call = 0; call < 20; ++call) {
    t.lockstep.run(kQuarterFrame);
    t.threaded.run(kQuarterFrame);
    ASSERT_TRUE(equal(t.lockstep, t.threaded)) << "after run " << call << " past the reset";
  }
}

// Every report the audio machine's observer is told, folded into one value in
// order, and the thread each came on.
struct ThreadRecorder final : ApuObserver {
  std::optional<std::thread::id> first;
  bool oneThread = true;
  std::uint64_t digest = 1469598103934665603ull;
  std::size_t reports = 0;
  void note(std::thread::id id, std::uint64_t word) {
    if (!first.has_value()) first = id;
    if (*first != id) oneThread = false;
    ++reports;
    digest = (digest ^ word) * 1099511628211ull;
  }
  void access(std::uint16_t address, std::uint8_t value, bool write) override {
    note(std::this_thread::get_id(),
         (std::uint64_t{address} << 16) | (std::uint64_t{value} << 8) | (write ? 1u : 0u));
  }
  void instruction(const Spc700State&, const Spc700State& after, std::uint32_t cycles) override {
    note(std::this_thread::get_id(), (std::uint64_t{after.pc} << 32) | cycles);
  }
};

TEST(SnesApuThread, TheAudioObserverIsToldOnTheAudioThread) {
  Twin t = twin(handshake());
  ThreadRecorder onLockstep;
  ThreadRecorder onThreaded;
  t.lockstep.setApuObserver(&onLockstep);
  t.threaded.setApuObserver(&onThreaded);
  for (int call = 0; call < 8; ++call) {
    t.lockstep.run(kQuarterFrame);
    t.threaded.run(kQuarterFrame);
  }
  ASSERT_TRUE(equal(t.lockstep, t.threaded));
  ASSERT_TRUE(onLockstep.first.has_value());
  ASSERT_TRUE(onThreaded.first.has_value());
  EXPECT_EQ(*onLockstep.first, std::this_thread::get_id()) << "the lockstep machine reports inline";
  EXPECT_NE(*onThreaded.first, std::this_thread::get_id()) << "the threaded one on its own thread";
  EXPECT_TRUE(onLockstep.oneThread);
  EXPECT_TRUE(onThreaded.oneThread);
  EXPECT_EQ(onThreaded.reports, onLockstep.reports);
  EXPECT_EQ(onThreaded.digest, onLockstep.digest) << "the same reports in the same order";
}

struct Thrown {};

// A frame observer that throws on the second frame it is told.
struct ThrowsOnSecond final : FrameObserver {
  int frames = 0;
  void frame(const VideoFrame&) override {
    if (++frames == 2) throw Thrown{};
  }
};

TEST(SnesApuThread, AReportThatThrowsLeavesBothMachinesAtOneTime) {
  Twin t = twin(handshake());
  ThrowsOnSecond onLockstep;
  ThrowsOnSecond onThreaded;
  t.lockstep.setFrameObserver(&onLockstep);
  t.threaded.setFrameObserver(&onThreaded);
  EXPECT_THROW(t.lockstep.run(3u * kFrame), Thrown);
  EXPECT_THROW(t.threaded.run(3u * kFrame), Thrown);
  ASSERT_TRUE(equal(t.lockstep, t.threaded)) << "where the throw left them";
  t.lockstep.run(kFrame);
  t.threaded.run(kFrame);
  EXPECT_TRUE(equal(t.lockstep, t.threaded)) << "after the run that finished the budget";
}

TEST(SnesApuThread, TheThreadIsOffByDefaultAndTheObserverIsToldOnTheCallingThread) {
  const std::vector<std::uint8_t> rom = handshake();
  SnesConfig defaults{};
  EXPECT_FALSE(defaults.apuThread);
  defaults.rom = rom;
  Snes machine(defaults);
  ThreadRecorder told;
  machine.setApuObserver(&told);
  machine.run(kQuarterFrame);
  ASSERT_TRUE(told.first.has_value());
  EXPECT_EQ(*told.first, std::this_thread::get_id());
  EXPECT_TRUE(told.oneThread);
}

TEST(SnesApuThread, AMachineBuiltWithTheThreadOffPacesTheApuByTheRatio) {
  // The interleave's pin: one NTSC denominator of master cycles is exactly the
  // numerator of audio cycles, and 176 sample frames.
  Twin t = twin(loop());
  t.lockstep.run(118125u);
  t.threaded.run(118125u);
  EXPECT_EQ(t.lockstep.state().apu.divider, 5632u);
  EXPECT_EQ(t.threaded.state().apu.divider, t.lockstep.state().apu.divider);
  EXPECT_EQ(t.lockstep.takeFrames().size(), 176u);
  EXPECT_EQ(t.threaded.takeFrames().size(), 176u);
}

TEST(SnesApuThread, TheMachineStopsItsThreadWhenItGoes) {
  const std::vector<std::uint8_t> rom = handshake();
  {
    Snes never(config(rom, true));  // no call: no thread to stop
  }
  {
    Snes spinning(config(rom, true));
    spinning.run(kFrame);  // destroyed while its thread spins after the call
  }
  {
    Snes parked(config(rom, true));
    parked.run(kFrame);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));  // past the spin grace
    parked.run(kQuarterFrame);  // a call wakes it
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }  // destroyed while its thread is parked
  SUCCEED() << "every machine's thread ended with it";
}

// An audio observer that holds the audio thread up at its first report.
struct Stall final : ApuObserver {
  bool stalled = false;
  std::size_t reports = 0;
  void access(std::uint16_t, std::uint8_t, bool) override { ++reports; }
  void instruction(const Spc700State&, const Spc700State&, std::uint32_t) override {
    ++reports;
    if (stalled) return;
    stalled = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
  }
};

TEST(SnesApuThread, ThePortWriteRingNeverLosesAWriteUnderBackpressure) {
  // The program stores to port 0 thousands of times a frame and never reads one,
  // and the audio CPU sums every byte it reads there. With the audio thread held up
  // at its first report, the console runs far enough ahead to fill the ring and
  // wait on it; every write still lands at its own audio cycle.
  Twin t = twin(portWriter());
  loadPortReader(t.lockstep);
  loadPortReader(t.threaded);
  Stall onLockstep;
  Stall onThreaded;
  t.lockstep.setApuObserver(&onLockstep);
  t.threaded.setApuObserver(&onThreaded);
  t.lockstep.run(kFrame);
  t.threaded.run(kFrame);
  EXPECT_TRUE(equal(t.lockstep, t.threaded));
  EXPECT_EQ(onThreaded.reports, onLockstep.reports);
  EXPECT_EQ(t.threaded.peekApu(kReadSum), t.lockstep.peekApu(kReadSum));
}

// What a frame report reads of the audio machine, frame by frame.
struct AudioAtReport final : FrameObserver {
  Snes* machine = nullptr;
  std::vector<std::uint16_t> dividers;
  std::vector<std::uint8_t> sums;
  void frame(const VideoFrame&) override {
    dividers.push_back(machine->state().apu.divider);
    sums.push_back(machine->state().apu.ram[kReadSum]);
  }
};

TEST(SnesApuThread, AFrameReportReadsTheAudioMachineAtTheReportsCycle) {
  Twin t = twin(portWriter());
  loadPortReader(t.lockstep);
  loadPortReader(t.threaded);
  AudioAtReport onLockstep;
  AudioAtReport onThreaded;
  onLockstep.machine = &t.lockstep;
  onThreaded.machine = &t.threaded;
  t.lockstep.setFrameObserver(&onLockstep);
  t.threaded.setFrameObserver(&onThreaded);
  t.lockstep.run(8u * kFrame);
  t.threaded.run(8u * kFrame);
  EXPECT_TRUE(equal(t.lockstep, t.threaded));
  ASSERT_GE(onLockstep.dividers.size(), 7u);
  EXPECT_EQ(onThreaded.dividers, onLockstep.dividers);
  EXPECT_EQ(onThreaded.sums, onLockstep.sums);
}

// What a bus observer peeks of the audio machine at every write to port 0.
struct PeekAtPortWrite final : BusObserver {
  Snes* machine = nullptr;
  std::vector<std::uint8_t> sums;
  void access(const BusAccess& access) override {
    if (access.write && (access.address & 0xFFFFu) == 0x2140u) sums.push_back(machine->peekApu(kReadSum));
  }
  void internal(std::uint32_t, std::optional<CycleKind>) override {}
};

TEST(SnesApuThread, ABusObserverPeeksTheAudioMachineAtTheAccessesCycle) {
  Twin t = twin(portWriter());
  loadPortReader(t.lockstep);
  loadPortReader(t.threaded);
  PeekAtPortWrite onLockstep;
  PeekAtPortWrite onThreaded;
  onLockstep.machine = &t.lockstep;
  onThreaded.machine = &t.threaded;
  t.lockstep.setObserver(&onLockstep);
  t.threaded.setObserver(&onThreaded);
  t.lockstep.run(2u * kFrame);
  t.threaded.run(2u * kFrame);
  EXPECT_TRUE(equal(t.lockstep, t.threaded));
  ASSERT_GT(onLockstep.sums.size(), 1000u);
  EXPECT_EQ(onThreaded.sums, onLockstep.sums);
}

// An audio observer that calls into the guest from every report it is told.
struct CallsFromTheAudioMachine final : ApuObserver {
  Snes* machine = nullptr;
  std::size_t calls = 0;
  std::size_t ran = 0;
  void call() {
    ++calls;
    if (machine->callInContext(kPortRoutine, Standin::Near, 100)) ++ran;
    if (machine->callOnStack(kPortRoutine, 0x01F0u, Standin::Near, 100)) ++ran;
  }
  void access(std::uint16_t, std::uint8_t, bool) override { call(); }
  void instruction(const Spc700State&, const Spc700State&, std::uint32_t) override { call(); }
};

TEST(SnesApuThread, ACallFromTheAudioThreadIsRefused) {
  // The reports come on the audio thread through every call, the stretch where the
  // console waits for the thread at the call's return included.
  Twin t = twin(portWriterWithRoutine());
  CallsFromTheAudioMachine onLockstep;
  CallsFromTheAudioMachine onThreaded;
  onLockstep.machine = &t.lockstep;
  onThreaded.machine = &t.threaded;
  t.lockstep.setApuObserver(&onLockstep);
  t.threaded.setApuObserver(&onThreaded);
  for (int call = 0; call < 16; ++call) {
    t.lockstep.run(kQuarterFrame);
    t.threaded.run(kQuarterFrame);
    ASSERT_TRUE(equal(t.lockstep, t.threaded)) << "after run " << call;
  }
  EXPECT_GT(onThreaded.calls, 0u);
  EXPECT_EQ(onThreaded.calls, onLockstep.calls);
  EXPECT_EQ(onThreaded.ran, 0u);
  EXPECT_EQ(onLockstep.ran, 0u);
}

TEST(SnesApuThread, AGuestCallBetweenCallsWakesAParkedThreadAndJoinsIt) {
  // The routine reads port 0, so the call waits for the audio machine; the thread
  // has parked since the last call.
  Twin t = twin(portWriterWithRoutine());
  loadPortReader(t.lockstep);
  loadPortReader(t.threaded);
  t.lockstep.run(kFrame);
  t.threaded.run(kFrame);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));  // past the spin grace
  EXPECT_TRUE(t.lockstep.callInContext(kPortRoutine, Standin::Near, 100));
  EXPECT_TRUE(t.threaded.callInContext(kPortRoutine, Standin::Near, 100));
  ASSERT_TRUE(equal(t.lockstep, t.threaded)) << "after the call in context";
  EXPECT_EQ(t.threaded.state().wram[kRoutineRead], t.lockstep.state().wram[kRoutineRead]);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_TRUE(t.lockstep.callOnStack(kPortRoutine, 0x01C0u, Standin::Near, 100));
  EXPECT_TRUE(t.threaded.callOnStack(kPortRoutine, 0x01C0u, Standin::Near, 100));
  EXPECT_TRUE(equal(t.lockstep, t.threaded)) << "after the call on a stack";
}

TEST(SnesApuThread, AThreadParkingAsACallBeginsIsWokenByIt) {
  // Calls a line long, each after a pause of its own length: across the run the
  // pauses pass the spin grace at every point of its end, so some call begins just
  // as the thread parks. A thread left parked through a call never returns from it.
  Twin t = twin(portWriter());
  loadPortReader(t.lockstep);
  loadPortReader(t.threaded);
  for (int call = 0; call < 20000; ++call) {
    t.lockstep.run(1364u);
    t.threaded.run(1364u);
    const auto pause = std::chrono::microseconds(call % 400);
    const auto until = std::chrono::steady_clock::now() + pause;
    while (std::chrono::steady_clock::now() < until) {
    }
  }
  EXPECT_TRUE(equal(t.lockstep, t.threaded));
}

}  // namespace
}  // namespace snaggletooth
