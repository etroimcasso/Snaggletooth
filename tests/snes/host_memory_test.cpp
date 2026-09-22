// The console's host memory face: reaching into any place the machine has by bus
// address (peek / poke / addressable), the four memories the bus cannot name, and
// the CPU register file read and written whole. Each case reads a value back or
// steps the machine and reads the result — never "the symbol exists". The last
// two cases pin the machine that hosts nothing: it runs a fixed program to a fixed
// budget and lands on a byte-identical state and an identical audio frame sequence
// to another built the same way, and a moved machine reaches in identically.

#include <array>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "snaggletooth/snes/snes.h"

namespace snaggletooth {
namespace {

// A LoROM cartridge that loops (NOP, BRA to itself) with a couple of distinctive
// image bytes and, on request, a save. rom[0] answers at bus $00:8000; rom[0x100]
// at $00:8100 (LoROM maps image offset O to bank O>>15, address $8000|(O&$7FFF)).
Snes machine(std::size_t saveBytes = 0) {
  std::vector<std::uint8_t> rom(0x8000u, 0x00u);
  rom[0x0000u] = 0xEAu;  // NOP at $8000
  rom[0x0001u] = 0x80u;  // BRA
  rom[0x0002u] = 0xFDu;  // -3 -> back to $8000
  rom[0x0100u] = 0x5Au;  // a distinctive ROM byte, at $00:8100
  rom[0x7FFCu] = 0x00u;  // reset vector -> $8000
  rom[0x7FFDu] = 0x80u;
  SnesConfig cfg{.rom = rom, .map = CartridgeMap::LoRom};
  if (saveBytes != 0) cfg.saveRamBytes = saveBytes;
  return Snes(cfg);
}

// ---- peek: what each place answers ----------------------------------------

TEST(SnesHostMemory, PeekReadsWorkRamAndItsSystemBankMirror) {
  Snes m = machine();
  m.poke(0x7E0100u, 0x42u);
  EXPECT_EQ(m.peek(0x7E0100u), std::optional<std::uint8_t>{0x42u});
  // $00:0100 is the low-8K work-RAM mirror of the same byte.
  EXPECT_EQ(m.peek(0x000100u), std::optional<std::uint8_t>{0x42u});
}

TEST(SnesHostMemory, PeekReadsCartridgeRom) {
  Snes m = machine();
  EXPECT_EQ(m.peek(0x008000u), std::optional<std::uint8_t>{0xEAu});  // rom[0]
  EXPECT_EQ(m.peek(0x008100u), std::optional<std::uint8_t>{0x5Au});  // rom[0x100]
}

TEST(SnesHostMemory, PeekReadsSaveRam) {
  Snes m = machine(0x2000u);  // 8 KB LoROM save at banks $70-$7D
  m.poke(0x700000u, 0x99u);
  EXPECT_EQ(m.peek(0x700000u), std::optional<std::uint8_t>{0x99u});
}

TEST(SnesHostMemory, PeekAnswersNothingForARegister) {
  Snes m = machine();
  EXPECT_EQ(m.peek(0x002100u), std::nullopt);  // INIDISP is a register, not memory
  EXPECT_EQ(m.peek(0x004200u), std::nullopt);  // NMITIMEN, likewise
}

TEST(SnesHostMemory, PeekAnswersNothingForAnUnmappedAddress) {
  Snes m = machine();
  // $00:6000-$7FFF is the expansion region; a plain cartridge maps nothing there.
  EXPECT_EQ(m.peek(0x006000u), std::nullopt);
}

// ---- poke: what each place accepts ----------------------------------------

TEST(SnesHostMemory, PokeWritesWorkRam) {
  Snes m = machine();
  EXPECT_TRUE(m.poke(0x7E1234u, 0xC3u));    // bank $7E, offset $1234 -> wram index $1234
  EXPECT_EQ(m.state().wram[0x1234u], 0xC3u);
  EXPECT_TRUE(m.poke(0x7F0001u, 0xD4u));    // bank $7F begins at wram index $10000
  EXPECT_EQ(m.state().wram[0x10001u], 0xD4u);
}

TEST(SnesHostMemory, PokeToRomChangesTheImageTheMachineReads) {
  Snes m = machine();
  EXPECT_TRUE(m.poke(0x008000u, 0x1Bu));
  EXPECT_EQ(m.peek(0x008000u), std::optional<std::uint8_t>{0x1Bu});  // the machine's own copy changed
}

TEST(SnesHostMemory, PokeWritesSaveRam) {
  Snes m = machine(0x2000u);
  EXPECT_TRUE(m.poke(0x700010u, 0x7Fu));
  EXPECT_EQ(m.state().sram[0x10u], 0x7Fu);
}

TEST(SnesHostMemory, PokeToARegisterIsRefusedAndChangesNothing) {
  Snes m = machine();
  const std::uint8_t inidisp = m.state().ppu.inidisp;
  EXPECT_FALSE(m.poke(0x002100u, 0x00u));
  EXPECT_EQ(m.state().ppu.inidisp, inidisp);  // the register did not move
}

// ---- addressable: peek's own answer ---------------------------------------

TEST(SnesHostMemory, AddressableAgreesWithPeek) {
  Snes m = machine(0x2000u);
  EXPECT_TRUE(m.addressable(0x008000u, 1u));   // ROM
  EXPECT_TRUE(m.addressable(0x7E0000u, 2u));   // work RAM
  EXPECT_TRUE(m.addressable(0x700000u, 1u));   // save
  EXPECT_FALSE(m.addressable(0x002100u, 1u));  // a register is not memory
  EXPECT_TRUE(m.addressable(0x008000u, 0u));   // a zero-length span is addressable
}

TEST(SnesHostMemory, AddressableIsFalseWhenAnyByteInTheSpanIsNotMemory) {
  Snes m = machine();
  // $00:1FFF is the last work-RAM byte; $00:2000 begins the register page.
  EXPECT_TRUE(m.addressable(0x001FFFu, 1u));
  EXPECT_FALSE(m.addressable(0x001FFFu, 2u));
}

// ---- the four memories the bus cannot name --------------------------------

TEST(SnesHostMemory, WriteVramLandsInVideoRam) {
  Snes m = machine();
  m.writeVram(0x1234u, 0xABu);
  EXPECT_EQ(m.state().ppu.vram[0x1234u], 0xABu);
  EXPECT_EQ(m.vram()[0x1234u], 0xABu);
}

TEST(SnesHostMemory, WriteCgramLandsAndIgnoresPastItsEnd) {
  Snes m = machine();
  m.writeCgram(0x1FFu, 0x7Eu);  // last palette byte
  EXPECT_EQ(m.state().ppu.cgram[0x1FFu], 0x7Eu);
  m.writeCgram(0x200u, 0x11u);  // past the 512-byte end: ignored, no crash
  EXPECT_EQ(m.cgram()[0x1FFu], 0x7Eu);
}

TEST(SnesHostMemory, WriteOamLandsAndIgnoresPastItsEnd) {
  Snes m = machine();
  m.writeOam(543u, 0x22u);  // last OAM byte (544 total)
  EXPECT_EQ(m.state().ppu.oam[543u], 0x22u);
  m.writeOam(544u, 0x33u);  // past the end: ignored, no crash
  EXPECT_EQ(m.oam()[543u], 0x22u);
}

TEST(SnesHostMemory, WriteApuRamReachesTheAudioMachine) {
  Snes m = machine();
  m.writeApuRam(0x0200u, 0x5Cu);
  EXPECT_EQ(m.state().apu.ram[0x0200u], 0x5Cu);
  EXPECT_EQ(m.peekApu(0x0200u), 0x5Cu);
}

// ---- the CPU register file ------------------------------------------------

TEST(SnesHostMemory, SetCpuStateIsLiveOnTheNextCycle) {
  Snes m = machine();
  m.step();  // advance the machine off the reset vector
  m.step();
  Cpu65816State s = m.cpuState();
  s.pc = 0x8000u;  // point it back at the NOP
  s.a = 0x00AAu;
  m.setCpuState(s);
  EXPECT_EQ(m.cpuState().pc, 0x8000u);  // the written registers stand
  EXPECT_EQ(m.cpuState().a, 0x00AAu);
  m.step();                             // runs the NOP at $8000
  EXPECT_EQ(m.cpuState().pc, 0x8001u);  // it ran from the written program counter
  EXPECT_EQ(m.cpuState().a, 0x00AAu);   // and a NOP left the accumulator alone
}

// ---- the machine that hosts nothing behaves exactly as it does today ------

TEST(SnesHostMemory, NothingReachedInLandsByteIdenticalToAPlainRun) {
  Snes a = machine();
  Snes b = machine();
  a.run(200000u);
  b.run(200000u);
  EXPECT_TRUE(a.state() == b.state());              // the whole state, to the byte
  EXPECT_EQ(a.takeFrames(), b.takeFrames());        // and the same audio frames
}

TEST(SnesHostMemory, TheMemoryFaceSurvivesAMove) {
  Snes m = machine();
  m.poke(0x7E0000u, 0x42u);
  Snes moved = std::move(m);
  EXPECT_EQ(moved.peek(0x7E0000u), std::optional<std::uint8_t>{0x42u});  // the poke came across
  EXPECT_TRUE(moved.poke(0x7E0001u, 0x43u));                             // and the face still works
  EXPECT_EQ(moved.peek(0x7E0001u), std::optional<std::uint8_t>{0x43u});
}

}  // namespace
}  // namespace snaggletooth
