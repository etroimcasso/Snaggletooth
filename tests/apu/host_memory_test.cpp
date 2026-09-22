// The audio machine's host memory face, in its own 16-bit vocabulary: reaching
// into RAM by address (peek / poke / addressable), the DSP register file and the
// sound CPU's $00F0-$00FF overlay reached by name, the allocation-free contract
// that a register write does not advance the sample slot, and the CPU register
// file read and written whole. Each case reads a value back or steps the machine
// and reads the result.
//
// The DSP-write cases hold the contract that a host write and the sound program's
// own DSPDATA write are one thing: written from the same fresh state, they leave
// the DSP register file byte-identical, acknowledge for ENDX included.

#include <array>
#include <cstdint>
#include <initializer_list>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "snaggletooth/apu/apu.h"

namespace {

using snaggletooth::Apu;
using snaggletooth::ApuState;
using snaggletooth::Spc700State;
using snaggletooth::StereoFrame;

// The sixteen overlay registers by index 0-15 ($00F0-$00FF).
constexpr std::uint8_t kDspAddr = 2;   // $F2
constexpr std::uint8_t kDspData = 3;   // $F3
constexpr std::uint8_t kT0Out = 13;    // $FD

// Loads `code` at $0300, points the CPU there, and runs `instructions` of it.
Apu program(std::initializer_list<std::uint8_t> code, int instructions) {
  Apu apu;
  std::uint16_t addr = 0x0300u;
  for (std::uint8_t byte : code) apu.writeRam(addr++, byte);
  apu.setPc(0x0300u);
  for (int i = 0; i < instructions; ++i) apu.step();
  return apu;
}

// ---- reaching into RAM ----------------------------------------------------

TEST(ApuHostMemory, PokeWritesRamAndPeekReadsItBack) {
  Apu apu;
  EXPECT_TRUE(apu.poke(0x0250u, 0x5Cu));
  EXPECT_EQ(apu.peek(0x0250u), 0x5Cu);
  EXPECT_EQ(apu.readRam(0x0250u), 0x5Cu);
}

TEST(ApuHostMemory, PokeAndPeekReachTheRamBeneathTheRegisterOverlay) {
  Apu apu;
  apu.poke(0x00F2u, 0x77u);            // the RAM beneath DSPADDR, not the register
  EXPECT_EQ(apu.peek(0x00F2u), 0x77u);
  EXPECT_EQ(apu.readRam(0x00F2u), 0x77u);
  EXPECT_EQ(apu.state().dspAddr, 0x00u);  // the register itself did not move
}

TEST(ApuHostMemory, AddressableCoversTheWholeRamAndAnswersWhetherASpanFits) {
  Apu apu;
  EXPECT_TRUE(apu.addressable(0x0000u, 1u));
  EXPECT_TRUE(apu.addressable(0xFFFFu, 1u));   // the last byte
  EXPECT_TRUE(apu.addressable(0x0000u, 0u));   // a zero-length span
  EXPECT_FALSE(apu.addressable(0xFFFFu, 2u));  // runs one byte past the end
}

// ---- the DSP register file ------------------------------------------------

TEST(ApuHostMemory, DspRegisterWriteEqualsTheDspDataPath) {
  Apu host;
  host.writeDspRegister(0x0Cu, 0x55u);  // MVOLL, a plain register

  Apu viaData;
  viaData.writeOverlayRegister(kDspAddr, 0x0Cu);  // DSPADDR := $0C
  viaData.writeOverlayRegister(kDspData, 0x55u);  // DSPDATA := $55 -> dsp[$0C]

  EXPECT_EQ(host.state().dsp, viaData.state().dsp);          // one and the same write
  EXPECT_EQ(host.readDspRegister(0x0Cu), 0x55u);
  EXPECT_EQ(viaData.readDspRegister(0x0Cu), 0x55u);
}

TEST(ApuHostMemory, DspRegisterWriteAcknowledgesEndxLikeTheDspDataPath) {
  Apu host;
  host.writeDspRegister(0x7Cu, 0xFFu);  // ENDX: any write acknowledges every end flag

  Apu viaData;
  viaData.writeOverlayRegister(kDspAddr, 0x7Cu);
  viaData.writeOverlayRegister(kDspData, 0xFFu);

  EXPECT_EQ(host.state().dsp, viaData.state().dsp);  // the acknowledge is the same act
  EXPECT_EQ(host.readDspRegister(0x7Cu), 0x00u);     // and cleared the flags
}

TEST(ApuHostMemory, DspRegisterWriteMatchesARealProgramWrite) {
  Apu host;
  host.writeDspRegister(0x0Cu, 0x55u);
  // MOV $F2,#$0C ; MOV $F3,#$55  -> DSPADDR then DSPDATA, the program's own path.
  Apu prog = program({0x8Fu, 0x0Cu, 0xF2u, 0x8Fu, 0x55u, 0xF3u}, 2);
  EXPECT_EQ(prog.readDspRegister(0x0Cu), 0x55u);
  EXPECT_EQ(host.readDspRegister(0x0Cu), prog.readDspRegister(0x0Cu));
}

TEST(ApuHostMemory, ReadDspRegisterMasksTheIndexLikeDspData) {
  Apu apu;
  apu.writeDspRegister(0x10u, 0x33u);
  EXPECT_EQ(apu.readDspRegister(0x10u), 0x33u);
  EXPECT_EQ(apu.readDspRegister(0x90u), 0x33u);  // the top bit mirrors $10
}

TEST(ApuHostMemory, DspRegisterWriteAboveTheFileIsIgnored) {
  Apu apu;
  apu.writeDspRegister(0x00u, 0x11u);
  apu.writeDspRegister(0x80u, 0xFFu);            // above $7F: ignored, as DSPDATA ignores it
  EXPECT_EQ(apu.readDspRegister(0x00u), 0x11u);  // register $00 is untouched
}

// ---- the $00F0-$00FF overlay ----------------------------------------------

TEST(ApuHostMemory, OverlayRegisterWriteLandsInRamAndTakesEffect) {
  Apu apu;
  apu.writeOverlayRegister(kDspAddr, 0x55u);  // DSPADDR
  EXPECT_EQ(apu.state().dspAddr, 0x55u);      // the register's effect
  EXPECT_EQ(apu.readRam(0x00F2u), 0x55u);     // and the byte lands in the RAM beneath
}

TEST(ApuHostMemory, OverlayRegisterReadClearsATimerOutput) {
  ApuState st;
  st.timers[0].stage3 = 0x0Au;  // a pending T0 output
  Apu apu;
  apu.restore(st);
  EXPECT_EQ(apu.readOverlayRegister(kT0Out), 0x0Au);  // $FD returns the output
  EXPECT_EQ(apu.readOverlayRegister(kT0Out), 0x00u);  // reading it cleared it
}

// ---- the CPU register file ------------------------------------------------

TEST(ApuHostMemory, SetCpuStateIsLiveOnTheNextCycle) {
  Apu apu;
  apu.writeRam(0x0400u, 0x00u);  // NOP
  Spc700State s = apu.cpuState();
  s.pc = 0x0400u;
  apu.setCpuState(s);
  EXPECT_EQ(apu.cpuState().pc, 0x0400u);
  apu.step();  // runs the NOP
  EXPECT_EQ(apu.cpuState().pc, 0x0401u);  // ran from the written program counter
}

TEST(ApuHostMemory, SetCpuStateKeepsPendingFrames) {
  Apu apu;
  apu.run(64u);  // two DSP samples land in the queue
  ASSERT_FALSE(apu.takeFrames().empty());
  apu.run(64u);
  Spc700State s = apu.cpuState();
  apu.setCpuState(s);  // reloads the core without discarding output, unlike restore()
  EXPECT_FALSE(apu.takeFrames().empty());
}

}  // namespace
