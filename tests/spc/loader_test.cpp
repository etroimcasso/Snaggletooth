// The .spc loader: format validation, verbatim field mapping, register recovery
// from the RAM image, the conditional extra-RAM overwrite, and the seeding of every
// piece of machine state the format does not carry.
//
// Every assertion is traced to the .spc format spec (v0.30/v0.31) and the loader's
// seeding law. Images are authored inline from our own bytes — no fixtures, no
// copyrighted data.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "snaggletooth/apu/apu.h"
#include "snaggletooth/apu/dsp.h"
#include "spc/spc_loader.h"

namespace {

using snaggletooth::EnvPhase;
using snaggletooth::spc::parseSpc;

// Image field offsets the tests poke.
constexpr std::size_t kPc = 0x25, kA = 0x27, kX = 0x28, kY = 0x29, kPsw = 0x2A,
                      kSp = 0x2B, kRam = 0x100, kDsp = 0x10100, kExtra = 0x101C0;

// A valid, machine-complete .spc image (66,048 bytes): the fixed magic and marker,
// and zeroed CPU registers, RAM, and DSP registers. Tests set the bytes they care
// about (RAM byte b lives at kRam + b, DSP register r at kDsp + r).
std::vector<std::uint8_t> baseImage() {
  std::vector<std::uint8_t> img(0x10200, 0);
  constexpr std::string_view magic = "SNES-SPC700 Sound File Data v0.30";
  for (std::size_t i = 0; i < magic.size(); ++i)
    img[i] = static_cast<std::uint8_t>(magic[i]);
  img[0x21] = 26;
  img[0x22] = 26;
  return img;
}

// ── Format validation ───────────────────────────────────────────────────────

TEST(SpcLoader, RejectsShortFileWithASizeReason) {
  std::vector<std::uint8_t> img = baseImage();
  img.resize(0x10200 - 1);  // one byte short of machine-complete
  snaggletooth::spc::SpcLoad load = parseSpc(img);
  EXPECT_FALSE(load.state.has_value());
  EXPECT_NE(load.error.find("66048"), std::string::npos);
}

TEST(SpcLoader, RejectsBadMagic) {
  std::vector<std::uint8_t> img = baseImage();
  img[4] = 'X';  // corrupt the fixed prefix
  snaggletooth::spc::SpcLoad load = parseSpc(img);
  EXPECT_FALSE(load.state.has_value());
  EXPECT_NE(load.error.find("magic"), std::string::npos);
}

TEST(SpcLoader, RejectsMissingMarker) {
  std::vector<std::uint8_t> img = baseImage();
  img[0x21] = 0;  // not 26
  snaggletooth::spc::SpcLoad load = parseSpc(img);
  EXPECT_FALSE(load.state.has_value());
}

TEST(SpcLoader, AcceptsAMinimalValidImage) {
  snaggletooth::spc::SpcLoad load = parseSpc(baseImage());
  ASSERT_TRUE(load.state.has_value());
  EXPECT_TRUE(load.error.empty());
}

TEST(SpcLoader, IgnoresTheTwoVersionDigitsAfterTheV) {
  std::vector<std::uint8_t> img = baseImage();
  img[0x1D] = '9';  // the '0' of "v0.30" — a version digit, not validated
  img[0x20] = '9';  // the trailing '0'
  EXPECT_TRUE(parseSpc(img).state.has_value());
}

// ── Verbatim field mapping ──────────────────────────────────────────────────

TEST(SpcLoader, MapsCpuRegistersWithLittleEndianPc) {
  std::vector<std::uint8_t> img = baseImage();
  img[kPc] = 0x34;
  img[kPc + 1] = 0x12;  // PC = 0x1234, little-endian
  img[kA] = 0xAA;
  img[kX] = 0xBB;
  img[kY] = 0xCC;
  img[kPsw] = 0xDD;
  img[kSp] = 0xEE;
  const snaggletooth::ApuState st = parseSpc(img).state.value();
  EXPECT_EQ(st.cpu.pc, 0x1234);
  EXPECT_EQ(st.cpu.a, 0xAA);
  EXPECT_EQ(st.cpu.x, 0xBB);
  EXPECT_EQ(st.cpu.y, 0xCC);
  EXPECT_EQ(st.cpu.psw, 0xDD);
  EXPECT_EQ(st.cpu.sp, 0xEE);
}

TEST(SpcLoader, CopiesRamVerbatimIncludingTheOverlayBytes) {
  std::vector<std::uint8_t> img = baseImage();
  img[kRam + 0x1234] = 0xAB;  // an ordinary RAM byte
  img[kRam + 0x00F3] = 0xCD;  // a byte under the $F0-$FF overlay
  const snaggletooth::ApuState st = parseSpc(img).state.value();
  EXPECT_EQ(st.ram[0x1234], 0xAB);
  EXPECT_EQ(st.ram[0x00F3], 0xCD);
}

TEST(SpcLoader, CopiesDspRegistersVerbatim) {
  std::vector<std::uint8_t> img = baseImage();
  img[kDsp + 0x05] = 0x5A;
  img[kDsp + 0x7F] = 0xA5;
  const snaggletooth::ApuState st = parseSpc(img).state.value();
  EXPECT_EQ(st.dsp.regs[0x05], 0x5A);
  EXPECT_EQ(st.dsp.regs[0x7F], 0xA5);
}

// ── Register recovery from the RAM image ────────────────────────────────────

TEST(SpcLoader, RecoversControlAndDspAddrFromRam) {
  std::vector<std::uint8_t> img = baseImage();
  img[kRam + 0xF1] = 0x30;  // CONTROL
  img[kRam + 0xF2] = 0x40;  // DSPADDR
  const snaggletooth::ApuState st = parseSpc(img).state.value();
  EXPECT_EQ(st.control, 0x30);
  EXPECT_EQ(st.dspAddr, 0x40);
}

TEST(SpcLoader, RecoversTimerTargetsAndMasksTheFourBitOutputs) {
  std::vector<std::uint8_t> img = baseImage();
  img[kRam + 0xFA] = 0x11;  // T0 target
  img[kRam + 0xFB] = 0x22;  // T1 target
  img[kRam + 0xFC] = 0x33;  // T2 target
  img[kRam + 0xFD] = 0xF7;  // T0 output — only the low nibble is readable
  img[kRam + 0xFE] = 0xF3;
  img[kRam + 0xFF] = 0xF9;
  const snaggletooth::ApuState st = parseSpc(img).state.value();
  EXPECT_EQ(st.timers[0].target, 0x11);
  EXPECT_EQ(st.timers[1].target, 0x22);
  EXPECT_EQ(st.timers[2].target, 0x33);
  EXPECT_EQ(st.timers[0].stage3, 0x07);
  EXPECT_EQ(st.timers[1].stage3, 0x03);
  EXPECT_EQ(st.timers[2].stage3, 0x09);
}

TEST(SpcLoader, SeedsBothPortFacesFromTheRamImage) {
  std::vector<std::uint8_t> img = baseImage();
  img[kRam + 0xF4] = 0xA0;
  img[kRam + 0xF5] = 0xB1;
  img[kRam + 0xF6] = 0xC2;
  img[kRam + 0xF7] = 0xD3;
  const snaggletooth::ApuState st = parseSpc(img).state.value();
  const std::array<std::uint8_t, 4> expected{0xA0, 0xB1, 0xC2, 0xD3};
  EXPECT_EQ(st.inputPorts, expected);
  EXPECT_EQ(st.outputPorts, expected);
}

// ── Extra RAM ───────────────────────────────────────────────────────────────

TEST(SpcLoader, ExtraRamOverwritesHighRamWhenControlBit7IsSet) {
  std::vector<std::uint8_t> img = baseImage();
  img[kRam + 0xF1] = 0x80;      // CONTROL bit 7 set
  img[kRam + 0xFFC0] = 0x22;    // the RAM image's byte (should lose)
  img[kExtra + 0x00] = 0x11;    // the extra-RAM byte (should win)
  img[kExtra + 0x3F] = 0x99;    // last extra-RAM byte -> $FFFF
  const snaggletooth::ApuState st = parseSpc(img).state.value();
  EXPECT_EQ(st.ram[0xFFC0], 0x11);
  EXPECT_EQ(st.ram[0xFFFF], 0x99);
}

TEST(SpcLoader, ExtraRamIsIgnoredWhenControlBit7IsClear) {
  std::vector<std::uint8_t> img = baseImage();
  img[kRam + 0xF1] = 0x00;      // CONTROL bit 7 clear
  img[kRam + 0xFFC0] = 0x22;    // the RAM image's byte (should be kept)
  img[kExtra + 0x00] = 0x11;    // junk extra RAM (should be ignored)
  const snaggletooth::ApuState st = parseSpc(img).state.value();
  EXPECT_EQ(st.ram[0xFFC0], 0x22);
}

// ── Seeding law: state the format does not carry ────────────────────────────

TEST(SpcLoader, SeedsDspInternalsToPowerOnShape) {
  const snaggletooth::ApuState st = parseSpc(baseImage()).state.value();
  EXPECT_EQ(st.dsp.noiseLevel, -0x4000);
  EXPECT_EQ(st.dsp.globalCounter, 0);
  EXPECT_EQ(st.dsp.sampleIndex, 0u);
  EXPECT_EQ(st.dsp.internalKon, 0);
  EXPECT_EQ(st.dsp.echoIndex, 0);
  EXPECT_EQ(st.dsp.echoLength, 0);
  for (const snaggletooth::VoiceState& v : st.dsp.voices) {
    EXPECT_EQ(v.phase, EnvPhase::Release);
    EXPECT_EQ(v.envelope, 0);
  }
}

TEST(SpcLoader, SeedsTimerStage2AndDividerToZero) {
  std::vector<std::uint8_t> img = baseImage();
  img[kRam + 0xFA] = 0x40;  // a non-zero target must not seed a non-zero stage-2
  const snaggletooth::ApuState st = parseSpc(img).state.value();
  EXPECT_EQ(st.divider, 0);
  for (const snaggletooth::TimerState& t : st.timers) EXPECT_EQ(t.stage2, 0);
}

TEST(SpcLoader, LeavesTheCpuRunStateRunning) {
  const snaggletooth::ApuState st = parseSpc(baseImage()).state.value();
  EXPECT_EQ(st.cpu.run, snaggletooth::RunState::Running);
}

TEST(SpcLoader, DoesNotInjectAKeyOnFromTheDumpedKonRegister) {
  // A dumped KON byte is a readback value, not a command to replay: the loader
  // never keys a voice on, so every voice stays at its power-on Release.
  std::vector<std::uint8_t> img = baseImage();
  img[kDsp + 0x4C] = 0xFF;  // KON with every voice bit set
  const snaggletooth::ApuState st = parseSpc(img).state.value();
  for (const snaggletooth::VoiceState& v : st.dsp.voices) {
    EXPECT_EQ(v.phase, EnvPhase::Release);
    EXPECT_EQ(v.envelope, 0);
    EXPECT_EQ(v.konDelay, 0);
  }
}

TEST(SpcLoader, LeavesTheTestRegisterAtZero) {
  // TEST is write-only and unrecoverable; the format does not carry it, so it stays
  // at the benign zero regardless of the RAM byte under it.
  std::vector<std::uint8_t> img = baseImage();
  img[kRam + 0xF0] = 0x99;
  const snaggletooth::ApuState st = parseSpc(img).state.value();
  EXPECT_EQ(st.test, 0);
}

}  // namespace
