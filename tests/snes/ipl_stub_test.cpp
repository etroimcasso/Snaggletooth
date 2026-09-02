// The APU upload stub: the original SPC700 program the machine seeds at power-on,
// exercised through the audio machine's host face the way the main CPU drives it.
// A scripted uploader writes input ports 0-3 and reads back output port 0, running
// the documented handshake — post the ready bytes, set a destination, stream bytes
// with a per-byte acknowledgement, and start the loaded program.

#include <cstdint>
#include <vector>

#include "gtest/gtest.h"
#include "snaggletooth/apu/apu.h"
#include "snes_ipl_stub.h"

namespace snaggletooth {
namespace {

// An audio machine seeded with the upload stub, in the state the console powers on
// with: the stub in RAM, the ports clear, the CPU at the stub entry.
Apu stubbedApu() {
  ApuState state = Apu().state();  // sane TEST/CONTROL/timer/DSP fields
  seedIplStub(state);
  return Apu(state);
}

// Runs the machine until output port 0 reads `expect`, or a generous cycle budget
// elapses. The stub echoes input port 0 to output port 0 to acknowledge each step,
// so this is how the uploader waits for an acknowledgement.
bool waitPort0(Apu& apu, std::uint8_t expect) {
  for (int i = 0; i < 8000; ++i) {
    if (apu.readPort(0) == expect) return true;
    apu.run(16);
  }
  return apu.readPort(0) == expect;
}

// The main-CPU side of the handshake. Tracks the last value written to port 0 so a
// block command can satisfy the "at least two higher than the previous value" rule.
struct Uploader {
  Apu& apu;
  std::uint8_t lastPort0 = 0xAAu;  // the ready byte the console reads before the first command

  // Points the stub at `dest` and either begins a transfer (run == false) or starts
  // the program already loaded there (run == true). Returns whether it was acked.
  bool command(std::uint16_t dest, bool run) {
    apu.writePort(2, static_cast<std::uint8_t>(dest & 0xFFu));
    apu.writePort(3, static_cast<std::uint8_t>(dest >> 8));
    apu.writePort(1, run ? 0x00u : 0x01u);  // zero starts the program, nonzero sets an address
    const std::uint8_t cmd = lastPort0 == 0xAAu
                                 ? 0xCCu  // the first command is $CC by the protocol
                                 : static_cast<std::uint8_t>(lastPort0 + 2u);
    apu.writePort(0, cmd);
    lastPort0 = cmd;
    return waitPort0(apu, cmd);
  }

  // Sends one byte with its running index (low byte) and waits for the echo. The
  // stub acknowledges as it accepts the byte and stores it a moment later, so a
  // short settle lets the store land before the next step reads memory.
  bool byte(std::uint8_t index, std::uint8_t value) {
    apu.writePort(1, value);
    apu.writePort(0, index);
    lastPort0 = index;
    if (!waitPort0(apu, index)) return false;
    apu.run(64);
    return true;
  }

  // Streams a whole block to `dest`, one byte per acknowledgement.
  bool block(std::uint16_t dest, const std::vector<std::uint8_t>& data) {
    if (!command(dest, /*run=*/false)) return false;
    for (std::size_t i = 0; i < data.size(); ++i) {
      if (!byte(static_cast<std::uint8_t>(i & 0xFFu), data[i])) return false;
    }
    return true;
  }
};

// ---- ready ----------------------------------------------------------------

TEST(IplStub, PostsReadyBytes) {
  Apu apu = stubbedApu();
  ASSERT_TRUE(waitPort0(apu, 0xAAu));    // output port 0 -> $AA
  EXPECT_EQ(apu.readPort(1), 0xBBu);     // output port 1 -> $BB
}

// ---- the kick -------------------------------------------------------------

TEST(IplStub, AcknowledgesTheKick) {
  Apu apu = stubbedApu();
  ASSERT_TRUE(waitPort0(apu, 0xAAu));
  Uploader up{.apu = apu};
  EXPECT_TRUE(up.command(0x0200u, /*run=*/false));  // the stub echoes $CC
}

// ---- a single block -------------------------------------------------------

TEST(IplStub, StreamsABlockIntoRam) {
  Apu apu = stubbedApu();
  ASSERT_TRUE(waitPort0(apu, 0xAAu));
  Uploader up{.apu = apu};
  const std::vector<std::uint8_t> data = {0x11u, 0x22u, 0x33u, 0x44u, 0x55u};
  ASSERT_TRUE(up.block(0x0200u, data));
  for (std::size_t i = 0; i < data.size(); ++i) {
    EXPECT_EQ(apu.readRam(static_cast<std::uint16_t>(0x0200u + i)), data[i]);
  }
}

// ---- jump to entry --------------------------------------------------------

TEST(IplStub, RunsTheUploadedProgram) {
  Apu apu = stubbedApu();
  ASSERT_TRUE(waitPort0(apu, 0xAAu));
  Uploader up{.apu = apu};
  // A program that writes a sentinel to $0250 and stops. Its own store is the
  // observable proof the stub jumped to it.
  const std::vector<std::uint8_t> program = {
      0xE8u, 0x5Au,        // MOV A,#$5A
      0xC5u, 0x50u, 0x02u, // MOV !$0250,A
      0xFFu,               // STOP
  };
  ASSERT_TRUE(up.block(0x0200u, program));
  ASSERT_TRUE(up.command(0x0200u, /*run=*/true));  // start it
  for (int i = 0; i < 200 && apu.state().cpu.run == RunState::Running; ++i) apu.run(16);
  EXPECT_EQ(apu.readRam(0x0250u), 0x5Au);
  EXPECT_EQ(apu.state().cpu.run, RunState::Stopped);
}

// ---- a change of address --------------------------------------------------

TEST(IplStub, ChainsBlocksToDifferentAddresses) {
  Apu apu = stubbedApu();
  ASSERT_TRUE(waitPort0(apu, 0xAAu));
  Uploader up{.apu = apu};
  ASSERT_TRUE(up.block(0x0300u, {0xDEu, 0xADu}));
  ASSERT_TRUE(up.block(0x0400u, {0xBEu, 0xEFu}));  // a new address, mid-session
  EXPECT_EQ(apu.readRam(0x0300u), 0xDEu);
  EXPECT_EQ(apu.readRam(0x0301u), 0xADu);
  EXPECT_EQ(apu.readRam(0x0400u), 0xBEu);
  EXPECT_EQ(apu.readRam(0x0401u), 0xEFu);
}

// ---- a block that crosses a page boundary ---------------------------------

TEST(IplStub, CarriesTheDestinationAcrossAPage) {
  Apu apu = stubbedApu();
  ASSERT_TRUE(waitPort0(apu, 0xAAu));
  Uploader up{.apu = apu};
  // 260 bytes: the running index wraps at 256, so the stub must carry the
  // destination's high byte to keep placing bytes past the page boundary.
  std::vector<std::uint8_t> data(260u);
  for (std::size_t i = 0; i < data.size(); ++i) {
    data[i] = static_cast<std::uint8_t>(0x80u + i);  // a value that varies past the wrap
  }
  ASSERT_TRUE(up.block(0x0250u, data));
  EXPECT_EQ(apu.readRam(0x0250u), data[0]);          // first byte
  EXPECT_EQ(apu.readRam(0x034Fu), data[255]);        // last byte before the wrap
  EXPECT_EQ(apu.readRam(0x0350u), data[256]);        // first byte after it
  EXPECT_EQ(apu.readRam(0x0353u), data[259]);        // last byte
}

// ---- the acknowledgement gate ---------------------------------------------

TEST(IplStub, DoesNotReprocessAStalePort) {
  Apu apu = stubbedApu();
  ASSERT_TRUE(waitPort0(apu, 0xAAu));
  Uploader up{.apu = apu};
  ASSERT_TRUE(up.command(0x0200u, /*run=*/false));
  ASSERT_TRUE(up.byte(0x00u, 0x99u));  // store $99 at $0200, index 0
  // Leave the ports untouched and let the machine run: the stub waits for port 0
  // to change and must not store the same byte again at the next index.
  for (int i = 0; i < 400; ++i) apu.run(16);
  EXPECT_EQ(apu.readRam(0x0200u), 0x99u);
  EXPECT_EQ(apu.readRam(0x0201u), 0x00u);  // nothing was written at the next index
}

}  // namespace
}  // namespace snaggletooth
