// The console's whole host face on one running cartridge: a host reaching in,
// answering an access, standing in for a routine and calling two of its own,
// all at once, while the cartridge runs frame after frame on its own interrupt
// and reads a pad. The cartridge waits for the vertical blank, whose handler
// acknowledges it, reads the pad, keeps a running sum of the pad bytes, sends
// the sum to the audio machine's first port and counts the frame; the main loop
// then calls a routine and copies a work-RAM byte. The host arms that byte's read,
// the port's write and the routine, and calls two routines of its own from
// inside the watch on the routine.
//
// The first cases pin what each face does to the running cartridge, read back
// from its memory, its registers and the audio machine's port. The
// determinism cases pin the machine's defining property with the host in the
// loop: the same state and the same trace — the same pads, frame by frame, and
// the same host — give the same bytes, the whole machine state and every audio
// frame; a different trace gives different bytes, so the comparison can fail.
// The last cases pin what the host's objects are: not part of the state, so a
// snapshot carries none of them, and a machine armed and disarmed again runs
// exactly as one never armed.

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <vector>

#include "gtest/gtest.h"
#include "snaggletooth/snes/snes.h"

namespace snaggletooth {
namespace {

// One NTSC frame in master cycles.
constexpr std::uint64_t kFrame = 357366u;

constexpr std::uint32_t kStoodIn = 0x008200u;   // the routine the host stands in for
constexpr std::uint32_t kCounted = 0x008300u;   // INC $0018 ; RTS, called in context
constexpr std::uint32_t kDoubled = 0x008310u;   // ASL A ; STA $001A ; RTS, called on a stack
constexpr std::uint16_t kHostStack = 0x01C0u;   // the host's own stack for callOnStack
constexpr std::uint8_t kAnswered = 0x42u;       // what the host answers the program's read with
constexpr std::uint8_t kPoked = 0x07u;          // what the host pokes into that byte

// The cartridge, a one-bank LoROM image run in emulation mode:
//
//   $8000  LDA #$81 ; STA $4200          NMI and the pad auto-read on
//   $8005  WAI                           wait for the vertical blank
//   $8006  JSR $8200                     the routine the host stands in for
//   $8009  LDA $0020 ; STA $0022         copy the byte the host answers for
//   $800F  BRA $8005
//
//   $8100  LDA $4210                     acknowledge the NMI
//          LDA $4218 ; STA $0010         the pad's A/X/L/R byte
//          CLC ; ADC $0016 ; STA $0016   the running sum of every pad byte
//          STA $2140                     the sum to the audio machine
//          INC $0012 ; RTI               count the frame
//
//   $8200  LDA #$EE ; STA $0014 ; RTS    marks $0014 when it runs
//   $8300  INC $0018 ; RTS
//   $8310  ASL A ; STA $001A ; RTS
std::vector<std::uint8_t> cartridge() {
  std::vector<std::uint8_t> rom(0x8000u, 0x00u);
  const auto put = [&rom](std::uint32_t address, std::initializer_list<std::uint8_t> bytes) {
    std::size_t at = address - 0x8000u;
    for (std::uint8_t byte : bytes) rom[at++] = byte;
  };
  put(0x8000u, {0xA9u, 0x81u, 0x8Du, 0x00u, 0x42u, 0xCBu, 0x20u, 0x00u, 0x82u, 0xADu, 0x20u,
                0x00u, 0x8Du, 0x22u, 0x00u, 0x80u, 0xF4u});
  put(0x8100u, {0xADu, 0x10u, 0x42u, 0xADu, 0x18u, 0x42u, 0x8Du, 0x10u, 0x00u, 0x18u, 0x6Du,
                0x16u, 0x00u, 0x8Du, 0x16u, 0x00u, 0x8Du, 0x40u, 0x21u, 0xEEu, 0x12u, 0x00u,
                0x40u});
  put(0x8200u, {0xA9u, 0xEEu, 0x8Du, 0x14u, 0x00u, 0x60u});
  put(0x8300u, {0xEEu, 0x18u, 0x00u, 0x60u});
  put(0x8310u, {0x0Au, 0x8Du, 0x1Au, 0x00u, 0x60u});
  rom[0x7FFAu] = 0x00u;  // emulation-mode NMI -> $8100
  rom[0x7FFBu] = 0x81u;
  rom[0x7FFCu] = 0x00u;  // reset -> $8000
  rom[0x7FFDu] = 0x80u;
  return rom;
}

Snes machine() {
  const std::vector<std::uint8_t> rom = cartridge();
  Snes m(SnesConfig{.rom = rom});
  m.poke(0x000020u, kPoked);
  return m;
}

// The host: answers the program's read of $0020 with its own byte, vetoes every
// write to the port, stands in for the routine at $8200 and, each time the
// program reaches it, calls $8300 in the guest's context and $8310 on a stack
// of its own, reading that routine's A before putting the guest's file back.
struct Host final : AccessWatcher, InstructionWatcher {
  Snes* m = nullptr;
  std::size_t reads = 0;
  std::size_t portWrites = 0;
  std::vector<std::uint32_t> reachedAt;
  std::vector<bool> inContext;
  std::vector<bool> onStack;
  std::vector<std::uint16_t> onStackA;

  AccessAnswer read(std::uint32_t, std::uint8_t, AccessSource, CycleKind, std::uint8_t) override {
    ++reads;
    return AccessAnswer::instead(kAnswered);
  }
  AccessAnswer write(std::uint32_t, std::uint8_t, AccessSource, CycleKind, std::uint8_t) override {
    ++portWrites;
    return AccessAnswer::veto();
  }
  void reached(std::uint32_t address) override {
    reachedAt.push_back(address);
    inContext.push_back(m->callInContext(kCounted, Standin::Near, 64));
    const Cpu65816State guest = m->cpuState();
    Cpu65816State frame = guest;
    frame.a = 0x0005u;
    m->setCpuState(frame);
    onStack.push_back(m->callOnStack(kDoubled, kHostStack, Standin::Near, 64));
    onStackA.push_back(m->cpuState().a);
    m->setCpuState(guest);
  }
};

// Sets the host on the machine and arms its three places: the work-RAM byte's
// read through the bank-$7E alias the program never uses, the port's write, and
// the routine.
void attach(Snes& m, Host& host) {
  host.m = &m;
  m.setAccessWatcher(&host);
  m.setInstructionWatcher(&host);
  m.watchAccess(0x7E0020u, 1, true, false);
  m.watchAccess(0x002140u, 1, false, true);
  m.watchInstruction(kStoodIn, Standin::Near);
}

// The pads, frame by frame: A on every third frame, X on every other.
std::vector<Joypad> trace(std::size_t frames) {
  std::vector<Joypad> pads(frames);
  for (std::size_t i = 0; i < frames; ++i) {
    pads[i].a = i % 3u == 0u;
    pads[i].x = i % 2u == 0u;
  }
  return pads;
}

// Plays `pads` one frame each, draining the audio frames into a buffer of the
// caller's own after every frame, and answers every frame drained.
std::vector<StereoFrame> play(Snes& m, std::span<const Joypad> pads) {
  std::vector<StereoFrame> heard;
  std::array<StereoFrame, 256> buffer{};
  for (const Joypad& pad : pads) {
    m.setJoypad(JoypadPort::One, pad);
    m.run(kFrame);
    for (std::size_t n = m.takeFrames(buffer); n != 0; n = m.takeFrames(buffer)) {
      heard.insert(heard.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(n));
    }
  }
  return heard;
}

// ---- every face on the running cartridge -------------------------------------

TEST(SnesHostSurface, ReachingInReadsAndWritesTheRunningCartridge) {
  Snes m = machine();
  Host host;
  attach(m, host);
  m.writeVram(0x1000u, 0xABu);
  m.writeCgram(0x0010u, 0xCDu);
  m.writeOam(0x0200u, 0xEFu);
  m.writeApuRam(0x0400u, 0x5Au);
  play(m, trace(12));
  // The frame counter the handler keeps is read where the program keeps it.
  ASSERT_TRUE(m.peek(0x000012u).has_value());
  EXPECT_EQ(*m.peek(0x000012u), m.state().wram[0x12]);
  EXPECT_GE(m.state().wram[0x12], 10u) << "the cartridge ran on its own interrupt";
  EXPECT_EQ(m.peek(0x7E0012u).value_or(0x00u), *m.peek(0x000012u)) << "one byte, two addresses";
  // The poke landed and stands: the host's answer changed what the program
  // read, never what memory holds.
  EXPECT_EQ(m.peek(0x000020u).value_or(0x00u), kPoked);
  EXPECT_EQ(m.peek(0x008000u).value_or(0x00u), 0xA9u) << "the image, read in place";
  EXPECT_FALSE(m.peek(0x002140u).has_value()) << "a register is not memory";
  EXPECT_TRUE(m.addressable(0x7E0000u, 0x20000u));
  EXPECT_FALSE(m.addressable(0x0021FFu, 2u));
  EXPECT_EQ(m.physical(0x000020u), m.physical(0x7E0020u));
  // The memories the bus cannot name keep what the host wrote; the cartridge
  // never touches them.
  EXPECT_EQ(m.vram()[0x1000u], 0xABu);
  EXPECT_EQ(m.cgram()[0x0010u], 0xCDu);
  EXPECT_EQ(m.oam()[0x0200u], 0xEFu);
  EXPECT_EQ(m.peekApu(0x0400u), 0x5Au);
}

TEST(SnesHostSurface, AnAnswerChangesWhatTheProgramReadsAndStores) {
  Snes hosted = machine();
  Snes plain = machine();
  Host host;
  attach(hosted, host);
  const std::vector<Joypad> pads = trace(12);
  play(hosted, pads);
  play(plain, pads);
  // The read of $0020 was answered with the host's byte, told through the
  // alias the host armed; the plain machine copied what memory holds.
  EXPECT_EQ(hosted.state().wram[0x22], kAnswered);
  EXPECT_EQ(plain.state().wram[0x22], kPoked);
  EXPECT_EQ(host.reads, host.reachedAt.size()) << "one read of $0020 per pass of the loop";
  // Every write to the port was vetoed: the audio machine never received the
  // sum, which the plain machine delivered.
  EXPECT_EQ(host.portWrites, hosted.state().wram[0x12]);
  EXPECT_EQ(hosted.state().apu.inputPorts[0], 0x00u);
  EXPECT_NE(plain.state().apu.inputPorts[0], 0x00u);
  EXPECT_EQ(plain.state().apu.inputPorts[0], plain.state().wram[0x16]);
  // The handler's own work is untouched by the host: the same pad sum on both.
  EXPECT_EQ(hosted.state().wram[0x16], plain.state().wram[0x16]);
  EXPECT_EQ(hosted.state().wram[0x10], plain.state().wram[0x10]);
}

TEST(SnesHostSurface, TheStoodInRoutineNeverRunsAndTheHostsCallsDo) {
  Snes hosted = machine();
  Snes plain = machine();
  Host host;
  attach(hosted, host);
  play(hosted, trace(12));
  play(plain, trace(12));
  ASSERT_GE(host.reachedAt.size(), 10u);
  for (std::uint32_t address : host.reachedAt) EXPECT_EQ(address, kStoodIn);
  // The stand-in answered the routine's first fetch with its return: the
  // marker the routine stores never landed, while the plain machine ran it.
  EXPECT_EQ(hosted.state().wram[0x14], 0x00u);
  EXPECT_EQ(plain.state().wram[0x14], 0xEEu);
  // The call in context ran once per reach, and each returned.
  EXPECT_EQ(hosted.state().wram[0x18], host.reachedAt.size());
  for (bool returned : host.inContext) EXPECT_TRUE(returned);
  // The call on the host's stack doubled the A the host gave it; the host read
  // the result from the file the routine left, then put the guest's back.
  for (bool returned : host.onStack) EXPECT_TRUE(returned);
  for (std::uint16_t a : host.onStackA) EXPECT_EQ(a & 0x00FFu, 0x0Au);
  EXPECT_EQ(hosted.state().wram[0x1A], 0x0Au);
  // The guest carried on unaware: its stack and its loop are where the plain
  // machine's are, parked on the WAI between frames.
  EXPECT_EQ(hosted.cpuState().s, plain.cpuState().s);
  EXPECT_EQ(hosted.cpuState().e, plain.cpuState().e);
  EXPECT_EQ(hosted.state().wram[0x12], plain.state().wram[0x12]) << "every frame's interrupt taken";
}

// ---- same state + same trace => same bytes ------------------------------------

TEST(SnesHostSurface, SameStateAndSameTraceGiveTheSameBytesWithTheHostInTheLoop) {
  const std::vector<Joypad> pads = trace(16);
  const std::span<const Joypad> head(pads.data(), 4);
  const std::span<const Joypad> rest(pads.data() + 4, pads.size() - 4);

  Snes a = machine();
  Host hostA;
  attach(a, hostA);
  play(a, head);
  const SnesState snapshot = a.state();  // mid-run, wherever the fourth frame stopped
  const std::size_t toldBefore = hostA.reachedAt.size();

  Snes b = machine();
  Host hostB;
  attach(b, hostB);
  b.restore(snapshot);

  const std::vector<StereoFrame> heardA = play(a, rest);
  const std::vector<StereoFrame> heardB = play(b, rest);
  EXPECT_TRUE(a.state() == b.state());
  EXPECT_EQ(heardA, heardB);
  EXPECT_FALSE(heardA.empty());
  // The host was told as often after the snapshot on both.
  EXPECT_EQ(hostA.reachedAt.size() - toldBefore, hostB.reachedAt.size());
  EXPECT_FALSE(hostB.reachedAt.empty());
}

TEST(SnesHostSurface, SameTraceFromPowerOnGivesTheSameBytes) {
  const std::vector<Joypad> pads = trace(16);
  Snes a = machine();
  Snes b = machine();
  Host hostA;
  Host hostB;
  attach(a, hostA);
  attach(b, hostB);
  const std::vector<StereoFrame> heardA = play(a, pads);
  const std::vector<StereoFrame> heardB = play(b, pads);
  EXPECT_TRUE(a.state() == b.state());
  EXPECT_EQ(heardA, heardB);
  EXPECT_EQ(hostA.reachedAt, hostB.reachedAt);
  EXPECT_EQ(hostA.reads, hostB.reads);
  EXPECT_EQ(hostA.portWrites, hostB.portWrites);
}

TEST(SnesHostSurface, ADifferentTraceGivesDifferentBytes) {
  // The same run with one frame's pad changed: the running sum carries the
  // difference to the end, so the comparison above can fail.
  std::vector<Joypad> pads = trace(16);
  std::vector<Joypad> changed = pads;
  changed[6].a = !changed[6].a;
  Snes a = machine();
  Snes b = machine();
  Host hostA;
  Host hostB;
  attach(a, hostA);
  attach(b, hostB);
  play(a, pads);
  play(b, changed);
  EXPECT_FALSE(a.state() == b.state());
  EXPECT_NE(a.state().wram[0x16], b.state().wram[0x16]);
}

// ---- the host's objects are the host's -----------------------------------------

TEST(SnesHostSurface, ASnapshotCarriesNoneOfTheHostsObjects) {
  // A snapshot of a hosted machine restored into a machine with no host runs
  // the stood-in routine and delivers to the port: the watches, the stand-in
  // and the watchers stayed with the machine that set them.
  const std::vector<Joypad> pads = trace(12);
  Snes hosted = machine();
  Host host;
  attach(hosted, host);
  play(hosted, std::span<const Joypad>(pads.data(), 4));
  Snes bare = machine();
  bare.restore(hosted.state());
  play(bare, std::span<const Joypad>(pads.data() + 4, pads.size() - 4));
  EXPECT_EQ(bare.state().wram[0x14], 0xEEu);
  EXPECT_EQ(bare.state().wram[0x22], kPoked);
  EXPECT_EQ(bare.state().apu.inputPorts[0], bare.state().wram[0x16]);
  EXPECT_EQ(bare.accessWatcher(), nullptr);
  EXPECT_EQ(bare.instructionWatcher(), nullptr);
}

TEST(SnesHostSurface, AMachineArmedAndDisarmedRunsExactlyAsOneNeverArmed) {
  const std::vector<Joypad> pads = trace(16);
  Snes armed = machine();
  Snes plain = machine();
  Host host;
  attach(armed, host);
  armed.unwatchAccess(0x000020u, 1, true, false);  // disarmed through the other alias
  armed.unwatchAccess(0x002140u, 1, false, true);
  armed.unwatchInstruction(kStoodIn);
  const std::vector<StereoFrame> heardArmed = play(armed, pads);
  const std::vector<StereoFrame> heardPlain = play(plain, pads);
  EXPECT_TRUE(host.reachedAt.empty());
  EXPECT_EQ(host.reads + host.portWrites, 0u);
  EXPECT_TRUE(armed.state() == plain.state());
  EXPECT_EQ(heardArmed, heardPlain);
}

}  // namespace
}  // namespace snaggletooth
