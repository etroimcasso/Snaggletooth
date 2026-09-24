// The audio machine's whole host face on one running sound program, in its
// own 16-bit vocabulary: a host reaching in, driving the DSP from outside,
// answering an access, standing in for a routine and calling two of its own,
// all at once, while a voice plays. The program reads the host's first port,
// keeps a running sum of what it read, posts the sum to its second output port,
// calls a routine and copies a RAM byte, round and round. The host keys a
// voice through the DSP's own register file, sets its pitch from the trace,
// arms the RAM byte's read, the port's write and the routine, and calls two
// routines of its own from inside the watch on the routine.
//
// The first cases pin what each face does to the running program, read back
// from its RAM, its ports and the sound it makes. The determinism cases pin the
// machine's defining property with the host in the loop: the same state and
// the same trace — the same port bytes and the same pitch, tick by tick, and
// the same host — give the same bytes, the whole machine state and every audio
// frame; a different trace gives different bytes, so the comparison can fail.
// The last cases pin what the host's objects are: not part of the state, so a
// snapshot carries none of them, and a machine armed and disarmed again runs
// exactly as one never armed.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "snaggletooth/apu/apu.h"

namespace {

using snaggletooth::AccessAnswer;
using snaggletooth::Apu;
using snaggletooth::ApuAccessWatcher;
using snaggletooth::ApuInstructionWatcher;
using snaggletooth::ApuStandin;
using snaggletooth::ApuState;
using snaggletooth::Spc700State;
using snaggletooth::StereoFrame;

// One tick of the trace in machine cycles: 64 samples.
constexpr std::uint64_t kTick = 2048u;

constexpr std::uint16_t kEntry = 0x0300u;
constexpr std::uint16_t kStoodIn = 0x0400u;   // the routine the host stands in for
constexpr std::uint16_t kCounted = 0x0500u;   // INC !$0258 ; RET, called in context
constexpr std::uint16_t kDoubled = 0x0510u;   // ASL A ; MOV !$025A,A ; RET, called on a stack
constexpr std::uint8_t kHostStack = 0xC0u;    // the host's own stack for callOnStack
constexpr std::uint8_t kAnswered = 0x42u;     // what the host answers the program's read with
constexpr std::uint8_t kPoked = 0x07u;        // what the host pokes into that byte

// The sound program, at $0300:
//
//   $0300  MOV A,$F4 ; MOV !$0250,A      the host's first port byte
//          CLRC ; ADC A,!$0254           the running sum of every byte read
//          MOV !$0254,A ; MOV !$00F5,A   the sum to the second output port
//   $030F  CALL !$0400                   the routine the host stands in for
//   $0312  MOV A,!$0260 ; MOV !$0252,A   copy the byte the host answers for
//   $0318  BRA $0300
//
//   $0400  MOV A,#$EE ; MOV !$0256,A ; RET   marks $0256 when it runs
//   $0500  INC !$0258 ; RET
//   $0510  ASL A ; MOV !$025A,A ; RET
//
// And a looped one-block BRR square wave at $0700, its directory entry at
// $0600, for the voice the host keys.
Apu machine() {
  Apu apu;
  const auto put = [&apu](std::uint16_t address, std::initializer_list<std::uint8_t> bytes) {
    for (std::uint8_t byte : bytes) apu.poke(address++, byte);
  };
  put(0x0300u, {0xE4u, 0xF4u, 0xC5u, 0x50u, 0x02u, 0x60u, 0x85u, 0x54u, 0x02u, 0xC5u, 0x54u,
                0x02u, 0xC5u, 0xF5u, 0x00u, 0x3Fu, 0x00u, 0x04u, 0xE5u, 0x60u, 0x02u, 0xC5u,
                0x52u, 0x02u, 0x2Fu, 0xE6u});
  put(0x0400u, {0xE8u, 0xEEu, 0xC5u, 0x56u, 0x02u, 0x6Fu});
  put(0x0500u, {0xACu, 0x58u, 0x02u, 0x6Fu});
  put(0x0510u, {0x1Cu, 0xC5u, 0x5Au, 0x02u, 0x6Fu});
  put(0x0600u, {0x00u, 0x07u, 0x00u, 0x07u});  // start $0700, loop $0700
  put(0x0700u, {0xC3u, 0x77u, 0x77u, 0x77u, 0x77u, 0x99u, 0x99u, 0x99u, 0x99u});
  apu.poke(0x0260u, kPoked);
  apu.setPc(kEntry);
  return apu;
}

// Keys voice 0 on the square wave through the DSP's register file, at full
// volume under a fixed gain, the amplifier unmuted and echo writes off.
void keyVoice(Apu& apu) {
  apu.writeDspRegister(0x00u, 0x7Fu);  // V0VOLL
  apu.writeDspRegister(0x01u, 0x7Fu);  // V0VOLR
  apu.writeDspRegister(0x02u, 0x00u);  // V0PITCHL
  apu.writeDspRegister(0x03u, 0x10u);  // V0PITCHH
  apu.writeDspRegister(0x04u, 0x00u);  // V0SRCN
  apu.writeDspRegister(0x05u, 0x00u);  // V0ADSR1: the gain register rules
  apu.writeDspRegister(0x07u, 0x7Fu);  // V0GAIN: direct, full
  apu.writeDspRegister(0x0Cu, 0x7Fu);  // MVOLL
  apu.writeDspRegister(0x1Cu, 0x7Fu);  // MVOLR
  apu.writeDspRegister(0x5Du, 0x06u);  // DIR: the directory at $0600
  apu.writeDspRegister(0x6Cu, 0x20u);  // FLG: unmuted, echo writes off
  apu.writeDspRegister(0x4Cu, 0x01u);  // KON: voice 0
}

// The host: answers the program's read of $0260 with its own byte, vetoes every
// write to the second output port, stands in for the routine at $0400 and,
// each time the program reaches it, calls $0500 in the program's context and
// $0510 on a stack of its own, reading that routine's A before putting the
// program's file back.
struct Host final : ApuAccessWatcher, ApuInstructionWatcher {
  Apu* apu = nullptr;
  std::size_t reads = 0;
  std::size_t portWrites = 0;
  std::vector<std::uint16_t> reachedAt;
  std::vector<bool> inContext;
  std::vector<bool> onStack;
  std::vector<std::uint8_t> onStackA;

  AccessAnswer read(std::uint16_t, std::uint8_t, std::uint8_t) override {
    ++reads;
    return AccessAnswer::instead(kAnswered);
  }
  AccessAnswer write(std::uint16_t, std::uint8_t, std::uint8_t) override {
    ++portWrites;
    return AccessAnswer::veto();
  }
  void reached(std::uint16_t address) override {
    reachedAt.push_back(address);
    inContext.push_back(apu->callInContext(kCounted, ApuStandin::Return, 64));
    const Spc700State program = apu->cpuState();
    Spc700State frame = program;
    frame.a = 0x05u;
    apu->setCpuState(frame);
    onStack.push_back(apu->callOnStack(kDoubled, kHostStack, ApuStandin::Return, 64));
    onStackA.push_back(apu->cpuState().a);
    apu->setCpuState(program);
  }
};

void attach(Apu& apu, Host& host) {
  host.apu = &apu;
  apu.setAccessWatcher(&host);
  apu.setInstructionWatcher(&host);
  apu.watchAccess(0x0260u, 1, true, false);
  apu.watchAccess(0x00F5u, 1, false, true);
  apu.watchInstruction(kStoodIn, ApuStandin::Return);
}

// One tick of the trace: the byte the host posts to the first port and the
// voice's pitch, high byte.
struct Tick {
  std::uint8_t port = 0;
  std::uint8_t pitch = 0;
};

std::vector<Tick> trace(std::size_t ticks) {
  std::vector<Tick> out(ticks);
  for (std::size_t i = 0; i < ticks; ++i) {
    out[i].port = static_cast<std::uint8_t>(0x10u + i * 3u);
    out[i].pitch = static_cast<std::uint8_t>(0x08u + (i % 5u) * 2u);
  }
  return out;
}

// Plays `ticks`: each posts its port byte, sets the pitch through the DSP's
// register file, runs one tick and drains into a buffer of the caller's own.
std::vector<StereoFrame> play(Apu& apu, std::span<const Tick> ticks) {
  std::vector<StereoFrame> heard;
  std::array<StereoFrame, 32> buffer{};
  for (const Tick& tick : ticks) {
    apu.writePort(0, tick.port);
    apu.writeDspRegister(0x03u, tick.pitch);
    apu.run(kTick);
    for (std::size_t n = apu.takeFrames(buffer); n != 0; n = apu.takeFrames(buffer)) {
      heard.insert(heard.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(n));
    }
  }
  return heard;
}

bool sounds(const std::vector<StereoFrame>& frames) {
  return std::any_of(frames.begin(), frames.end(),
                     [](const StereoFrame& f) { return f.left != 0 || f.right != 0; });
}

// ---- every face on the running program -------------------------------------

TEST(ApuHostSurface, ReachingInReadsAndWritesTheRunningProgram) {
  Apu apu = machine();
  Host host;
  attach(apu, host);
  keyVoice(apu);
  const std::vector<StereoFrame> heard = play(apu, trace(24));
  // The voice the host keyed through the DSP's register file is playing, at
  // the pitch the trace set last.
  EXPECT_TRUE(sounds(heard));
  // The calls' cycles are the machine's own and run on top of each tick's
  // budget, so the sound runs longer than the ticks alone would make it.
  EXPECT_GT(heard.size(), 24u * kTick / 32u);
  EXPECT_EQ(apu.readDspRegister(0x03u), trace(24).back().pitch);
  // The poke stands: the host's answer changed what the program read, never
  // what RAM holds.
  EXPECT_EQ(apu.peek(0x0260u), kPoked);
  EXPECT_EQ(apu.peek(kEntry), 0xE4u) << "the program, read in place";
  EXPECT_TRUE(apu.addressable(0xFF00u, 0x100u));
  EXPECT_FALSE(apu.addressable(0xFF00u, 0x101u));
  // The first port holds what the host posted last; the program read it.
  EXPECT_EQ(apu.state().ram[0x0250u], trace(24).back().port);
}

TEST(ApuHostSurface, AnAnswerChangesWhatTheProgramReadsAndStores) {
  Apu hosted = machine();
  Apu plain = machine();
  Host host;
  attach(hosted, host);
  const std::vector<Tick> ticks = trace(24);
  play(hosted, ticks);
  play(plain, ticks);
  EXPECT_EQ(hosted.state().ram[0x0252u], kAnswered);
  EXPECT_EQ(plain.state().ram[0x0252u], kPoked);
  EXPECT_GT(host.reads, 0u);
  // Every write to the second output port was vetoed: it still holds the
  // power-on ready byte, where the plain machine's holds the sum.
  EXPECT_GT(host.portWrites, 0u);
  EXPECT_EQ(hosted.readPort(1), 0xBBu);
  EXPECT_EQ(plain.readPort(1), plain.state().ram[0x0254u]);
}

TEST(ApuHostSurface, TheStoodInRoutineNeverRunsAndTheHostsCallsDo) {
  Apu hosted = machine();
  Apu plain = machine();
  Host host;
  attach(hosted, host);
  play(hosted, trace(24));
  play(plain, trace(24));
  ASSERT_GE(host.reachedAt.size(), 10u);
  for (std::uint16_t address : host.reachedAt) EXPECT_EQ(address, kStoodIn);
  EXPECT_EQ(hosted.state().ram[0x0256u], 0x00u);
  EXPECT_EQ(plain.state().ram[0x0256u], 0xEEu);
  // One call in context per reach — the counter wraps at 256.
  EXPECT_EQ(hosted.state().ram[0x0258u], static_cast<std::uint8_t>(host.reachedAt.size()));
  for (bool returned : host.inContext) EXPECT_TRUE(returned);
  for (bool returned : host.onStack) EXPECT_TRUE(returned);
  for (std::uint8_t a : host.onStackA) EXPECT_EQ(a, 0x0Au);
  EXPECT_EQ(hosted.state().ram[0x025Au], 0x0Au);
  // The program carried on unaware: its stack is where the plain machine's is.
  EXPECT_EQ(hosted.cpuState().sp, plain.cpuState().sp);
}

// ---- same state + same trace => same bytes ------------------------------------

TEST(ApuHostSurface, SameStateAndSameTraceGiveTheSameBytesWithTheHostInTheLoop) {
  const std::vector<Tick> ticks = trace(32);
  const std::span<const Tick> head(ticks.data(), 7);
  const std::span<const Tick> rest(ticks.data() + 7, ticks.size() - 7);

  Apu a = machine();
  Host hostA;
  attach(a, hostA);
  keyVoice(a);
  play(a, head);
  const ApuState snapshot = a.state();  // mid-run, wherever the seventh tick stopped
  const std::size_t toldBefore = hostA.reachedAt.size();

  Apu b = machine();
  Host hostB;
  attach(b, hostB);
  b.restore(snapshot);

  const std::vector<StereoFrame> heardA = play(a, rest);
  const std::vector<StereoFrame> heardB = play(b, rest);
  EXPECT_TRUE(a.state() == b.state());
  EXPECT_EQ(heardA, heardB);
  EXPECT_TRUE(sounds(heardA)) << "a comparison of silence would prove nothing";
  EXPECT_EQ(hostA.reachedAt.size() - toldBefore, hostB.reachedAt.size());
  EXPECT_FALSE(hostB.reachedAt.empty());
}

TEST(ApuHostSurface, SameTraceFromPowerOnGivesTheSameBytes) {
  const std::vector<Tick> ticks = trace(32);
  Apu a = machine();
  Apu b = machine();
  Host hostA;
  Host hostB;
  attach(a, hostA);
  attach(b, hostB);
  keyVoice(a);
  keyVoice(b);
  const std::vector<StereoFrame> heardA = play(a, ticks);
  const std::vector<StereoFrame> heardB = play(b, ticks);
  EXPECT_TRUE(a.state() == b.state());
  EXPECT_EQ(heardA, heardB);
  EXPECT_TRUE(sounds(heardA));
  EXPECT_EQ(hostA.reachedAt, hostB.reachedAt);
  EXPECT_EQ(hostA.reads, hostB.reads);
  EXPECT_EQ(hostA.portWrites, hostB.portWrites);
}

TEST(ApuHostSurface, ADifferentTraceGivesDifferentBytes) {
  // One tick's port byte and pitch changed: the running sum carries the byte
  // to the end, and the pitch changes the sound from that tick on.
  const std::vector<Tick> ticks = trace(32);
  std::vector<Tick> changed = ticks;
  changed[12].port = static_cast<std::uint8_t>(changed[12].port + 1u);
  changed[12].pitch = static_cast<std::uint8_t>(changed[12].pitch + 1u);
  Apu a = machine();
  Apu b = machine();
  Host hostA;
  Host hostB;
  attach(a, hostA);
  attach(b, hostB);
  keyVoice(a);
  keyVoice(b);
  const std::vector<StereoFrame> heardA = play(a, ticks);
  const std::vector<StereoFrame> heardB = play(b, changed);
  EXPECT_FALSE(a.state() == b.state());
  EXPECT_NE(a.state().ram[0x0254u], b.state().ram[0x0254u]);
  EXPECT_NE(heardA, heardB);
}

// ---- the host's objects are the host's -----------------------------------------

TEST(ApuHostSurface, ASnapshotCarriesNoneOfTheHostsObjects) {
  const std::vector<Tick> ticks = trace(24);
  Apu hosted = machine();
  Host host;
  attach(hosted, host);
  play(hosted, std::span<const Tick>(ticks.data(), 6));
  Apu bare = machine();
  bare.restore(hosted.state());
  play(bare, std::span<const Tick>(ticks.data() + 6, ticks.size() - 6));
  EXPECT_EQ(bare.state().ram[0x0256u], 0xEEu);
  EXPECT_EQ(bare.state().ram[0x0252u], kPoked);
  EXPECT_EQ(bare.readPort(1), bare.state().ram[0x0254u]);
  EXPECT_EQ(bare.accessWatcher(), nullptr);
  EXPECT_EQ(bare.instructionWatcher(), nullptr);
}

TEST(ApuHostSurface, AMachineArmedAndDisarmedRunsExactlyAsOneNeverArmed) {
  const std::vector<Tick> ticks = trace(32);
  Apu armed = machine();
  Apu plain = machine();
  Host host;
  attach(armed, host);
  armed.unwatchAccess(0x0260u, 1, true, false);
  armed.unwatchAccess(0x00F5u, 1, false, true);
  armed.unwatchInstruction(kStoodIn);
  keyVoice(armed);
  keyVoice(plain);
  const std::vector<StereoFrame> heardArmed = play(armed, ticks);
  const std::vector<StereoFrame> heardPlain = play(plain, ticks);
  EXPECT_TRUE(host.reachedAt.empty());
  EXPECT_EQ(host.reads + host.portWrites, 0u);
  EXPECT_TRUE(armed.state() == plain.state());
  EXPECT_EQ(heardArmed, heardPlain);
  EXPECT_TRUE(sounds(heardArmed));
}

}  // namespace
