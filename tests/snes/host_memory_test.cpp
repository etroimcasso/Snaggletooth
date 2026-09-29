// The console's host memory face: reaching into any place the machine has by bus
// address (peek / poke / addressable), a register's value without its read's
// effects (peekRegister, peekApuRegister), the four memories the bus cannot name,
// and the CPU register file read and written whole. Each case reads a value back
// or steps the machine and reads the result — never "the symbol exists". The
// register cases hold each peeked byte against what the program's own LDA of the
// address loads from the same state, the open-bus bits against the data bus at
// the peek, and each read's effect against the state the peek leaves. The last
// two cases pin the machine that hosts nothing: it runs a fixed program to a fixed
// budget and lands on a byte-identical state and an identical audio frame sequence
// to another built the same way, and a moved machine reaches in identically.

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
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

// ---- peekRegister: a register's value, with nothing moved -----------------

// Puts `code` in work RAM at $00:0200 and points the CPU there. Reset leaves the
// CPU in emulation mode with the data bank at $00, so an LDA absolute there
// loads eight bits from bank $00.
void runFrom0200(Snes& m, std::initializer_list<std::uint8_t> code) {
  std::uint32_t at = 0x000200u;
  for (const std::uint8_t byte : code) m.poke(at++, byte);
  Cpu65816State c = m.cpuState();
  c.pc = 0x0200u;
  c.pbr = 0x00u;
  m.setCpuState(c);
}

// The program's LDA of $00:`address`, placed and pointed at, not yet run.
void loadFrom(Snes& m, std::uint16_t address) {
  runFrom0200(m, {0xADu, static_cast<std::uint8_t>(address & 0xFFu),
                  static_cast<std::uint8_t>(address >> 8)});
}

// The machine stepped to an instruction boundary inside vertical blank (lines
// 225-261), mid-line and clear of the blank's edges, so the cycles an LDA spends
// cross no blank edge and leave the field as it stands.
Snes parkedInVblank() {
  Snes m = machine();
  const auto parked = [&m] {
    const SnesState& s = m.state();
    return s.inVblank && s.vpos >= 230u && s.vpos < 250u && s.hpos >= 300u && s.hpos < 700u;
  };
  while (!parked()) m.step();
  return m;
}

// `parked` with every register given a byte of its own to answer: the PPU's
// memories, ports, counters and flags, its two open-bus values, the CPU's flags,
// the work-RAM port, the math results, a pad in each port part-way through its
// bits, channel 0's registers, and a data-bus byte unlike the PPU's two.
SnesState seeded(const SnesState& parked) {
  SnesState s = parked;
  for (std::size_t i = 0; i < s.ppu.vram.size(); ++i) s.ppu.vram[i] = static_cast<std::uint8_t>(i * 3u + 1u);
  for (std::size_t i = 0; i < s.ppu.cgram.size(); ++i) s.ppu.cgram[i] = static_cast<std::uint8_t>(i * 5u + 2u);
  for (std::size_t i = 0; i < s.ppu.oam.size(); ++i) s.ppu.oam[i] = static_cast<std::uint8_t>(i * 7u + 3u);
  s.ppu.m7a = 0x1234u;
  s.ppu.m7bByte = 0xA9u;
  s.ppu.vmain = 0x00u;  // the address steps after the low byte, by one word
  s.ppu.vmadd = 0x0456u;
  s.ppu.vramLatch = 0xBEEFu;
  s.ppu.cgadd = 0x21u;
  s.ppu.oamAddress = 0x0155u;
  s.ppu.ophct = 0x01A5u;
  s.ppu.opvct = 0x010Fu;
  s.ppu.countersLatched = true;
  s.ppu.timeOver = true;
  s.ppu.ppu1Bus = 0x5Au;
  s.ppu.ppu2Bus = 0xA5u;
  s.mdr = 0x3Cu;
  s.vblankNmi = true;
  s.timeup = true;
  s.wmadd = 0x01234u;
  s.wram[0x01234u] = 0x77u;
  s.rddiv = 0xABCDu;
  s.rdmpy = 0x1357u;
  for (std::size_t i = 0; i < s.joy.size(); ++i) s.joy[i] = static_cast<std::uint8_t>(0x11u * (i + 1u));
  Joypad pressed;
  pressed.b = true;
  pressed.a = true;
  s.pads[0] = pressed;
  s.pads[1] = Joypad{};
  s.joyLatch[0] = 0xA5A5u;
  s.joyLatch[1] = 0x8001u;
  s.joyClocks[0] = 3u;
  s.joyClocks[1] = 0u;
  DmaChannel& ch = s.dma[0];
  ch.dmap = 0x41u;
  ch.bbad = 0x18u;
  ch.a1t = 0x9876u;
  ch.a1b = 0x7Eu;
  ch.das = 0x0123u;
  ch.dasb = 0x45u;
  ch.a2a = 0xCDEFu;
  ch.nltr = 0x9Au;
  ch.unused = 0xE3u;
  return s;
}

// The offsets the register cases walk: the PPU's file, the four audio ports, the
// work-RAM port, the serial ports, the CPU's registers and channel 0's.
std::vector<std::uint16_t> walkedOffsets() {
  std::vector<std::uint16_t> offsets;
  const auto add = [&offsets](unsigned first, unsigned last) {
    for (unsigned o = first; o <= last; ++o) offsets.push_back(static_cast<std::uint16_t>(o));
  };
  add(0x2100u, 0x213Fu);
  add(0x2140u, 0x2143u);
  add(0x2180u, 0x2183u);
  add(0x4016u, 0x4017u);
  add(0x4200u, 0x421Fu);
  add(0x4300u, 0x430Fu);
  return offsets;
}

// The bits of a register's byte the read composes from the machine's state
// rather than the CPU's data bus: none where the read answers with the bus
// alone. The PPU's two open-bus values are the chip's state, so every bit of a
// PPU register that reads at all is here, and so is every bit of an offset that
// answers with the first half's value.
std::uint8_t stateBits(std::uint16_t offset) {
  if (offset <= 0x213Fu) {
    if (offset == 0x2137u) return 0x00u;
    if (offset >= 0x2134u) return 0xFFu;
    const unsigned low = offset & 0x0Fu;
    const bool firstHalfBus = offset < 0x2130u && ((low >= 0x4u && low <= 0x6u) || (low >= 0x8u && low <= 0xAu));
    if (firstHalfBus) return 0xFFu;
    return 0x00u;
  }
  if (offset <= 0x2143u) return 0xFFu;
  if (offset == 0x2180u) return 0xFFu;
  if (offset <= 0x2183u) return 0x00u;
  if (offset == 0x4016u) return 0x03u;
  if (offset == 0x4017u) return 0x1Fu;
  if (offset == 0x4210u) return 0x8Fu;
  if (offset == 0x4211u) return 0x80u;
  if (offset == 0x4212u) return 0xC1u;
  if (offset >= 0x4213u && offset <= 0x421Fu) return 0xFFu;
  if (offset >= 0x4300u && offset <= 0x430Fu) {
    const unsigned low = offset & 0x0Fu;
    if (low >= 0xCu && low <= 0xEu) return 0x00u;
    return 0xFFu;
  }
  return 0x00u;  // $4200-$420F
}

TEST(SnesHostMemory, PeekRegisterAnswersExactlyWherePhysicalSaysRegister) {
  Snes m = machine();
  for (const std::uint32_t bank : {0x000000u, 0x800000u}) {
    std::size_t registers = 0;
    for (std::uint32_t offset = 0; offset <= 0xFFFFu; ++offset) {
      const std::uint32_t address = bank | offset;
      const bool isRegister = m.physical(address).space == Snes::Space::Register;
      EXPECT_EQ(m.peekRegister(address).has_value(), isRegister) << std::hex << address;
      if (isRegister) ++registers;
    }
    // $2100-$2183, $4016-$4017, $4200-$421F and $4300-$437F; work RAM, the
    // offsets between the windows and the image answer nothing.
    EXPECT_EQ(registers, 0x84u + 0x2u + 0x20u + 0x80u);
  }
}

TEST(SnesHostMemory, PeekRegisterAnswersWhatTheProgramsLoadReadsAndMovesNothing) {
  Snes m = parkedInVblank();
  const SnesState firstHalf = seeded(m.state());
  // The flip-flops in their other phase, and the VRAM address stepping after the
  // high byte instead of the low.
  SnesState secondHalf = firstHalf;
  secondHalf.ppu.cgLatchHigh = true;
  secondHalf.ppu.ophctHigh = true;
  secondHalf.ppu.opvctHigh = true;
  secondHalf.ppu.vmain = 0x80u;
  const std::array<const SnesState*, 2> starts = {&firstHalf, &secondHalf};
  for (const SnesState* start : starts) {
    for (const std::uint16_t offset : walkedOffsets()) {
      m.restore(*start);
      loadFrom(m, offset);
      const SnesState before = m.state();
      const std::optional<std::uint8_t> peeked = m.peekRegister(offset);
      const std::optional<std::uint8_t> again = m.peekRegister(offset);
      ASSERT_TRUE(peeked.has_value()) << std::hex << offset;
      EXPECT_EQ(peeked, again) << std::hex << offset;
      EXPECT_TRUE(m.state() == before) << std::hex << offset;

      // The bits the bus supplies are the bus's byte at the moment of the peek —
      // never an LDA's, whose operand fetches put other bytes there first.
      const std::uint8_t own = stateBits(offset);
      const auto busBits = static_cast<std::uint8_t>(~own);
      EXPECT_EQ(*peeked & busBits, before.mdr & busBits) << std::hex << offset;

      // The register's own bits are the ones the program's load reads.
      if (own == 0x00u) continue;
      m.step();
      const auto loaded = static_cast<std::uint8_t>(m.cpuState().a & 0xFFu);
      EXPECT_EQ(*peeked & own, loaded & own) << std::hex << offset;
    }
  }
}

// Peeks `offset` from `start`, then runs the program's LDA of it: `field` stands
// through the peek, the data bus keeps its byte, and the load moves `field`.
template <typename Field>
void expectTheLoadAloneMoves(Snes& m, const SnesState& start, std::uint16_t offset, Field field) {
  m.restore(start);
  loadFrom(m, offset);
  const auto before = field(m.state());
  const std::uint8_t bus = m.state().mdr;
  EXPECT_TRUE(m.peekRegister(offset).has_value()) << std::hex << offset;
  EXPECT_EQ(field(m.state()), before) << std::hex << offset;
  EXPECT_EQ(m.state().mdr, bus) << std::hex << offset;
  m.step();
  EXPECT_NE(field(m.state()), before) << std::hex << offset;
}

TEST(SnesHostMemory, PeekRegisterLeavesEachEffectTheReadMakes) {
  Snes m = parkedInVblank();
  SnesState start = seeded(m.state());
  expectTheLoadAloneMoves(m, start, 0x4210u, [](const SnesState& s) { return s.vblankNmi; });
  expectTheLoadAloneMoves(m, start, 0x4211u, [](const SnesState& s) { return s.timeup; });
  expectTheLoadAloneMoves(m, start, 0x2138u, [](const SnesState& s) { return s.ppu.oamAddress; });
  expectTheLoadAloneMoves(m, start, 0x2139u, [](const SnesState& s) {
    return (static_cast<std::uint32_t>(s.ppu.vmadd) << 16) | s.ppu.vramLatch;
  });
  expectTheLoadAloneMoves(m, start, 0x213Cu, [](const SnesState& s) { return s.ppu.ophctHigh; });
  expectTheLoadAloneMoves(m, start, 0x213Du, [](const SnesState& s) { return s.ppu.opvctHigh; });
  expectTheLoadAloneMoves(m, start, 0x2180u, [](const SnesState& s) { return s.wmadd; });
  expectTheLoadAloneMoves(m, start, 0x4016u, [](const SnesState& s) { return s.joyClocks[0]; });

  // The latch through $2137 shows on a flag that stands clear; the latch line
  // ($4201 bit 7) is high, as reset leaves it.
  SnesState unlatched = start;
  unlatched.ppu.countersLatched = false;
  expectTheLoadAloneMoves(m, unlatched, 0x2137u, [](const SnesState& s) { return s.ppu.countersLatched; });

  // $213A steps and prefetches with the increment on the high byte.
  SnesState onHigh = start;
  onHigh.ppu.vmain = 0x80u;
  expectTheLoadAloneMoves(m, onHigh, 0x213Au, [](const SnesState& s) {
    return (static_cast<std::uint32_t>(s.ppu.vmadd) << 16) | s.ppu.vramLatch;
  });

  // $213B's second read moves the palette address and resets its flip-flop;
  // $213F clears the latch flag and resets both counters' flip-flops.
  SnesState secondHalf = start;
  secondHalf.ppu.cgLatchHigh = true;
  secondHalf.ppu.ophctHigh = true;
  secondHalf.ppu.opvctHigh = true;
  expectTheLoadAloneMoves(m, secondHalf, 0x213Bu, [](const SnesState& s) {
    return (static_cast<unsigned>(s.ppu.cgadd) << 1) | (s.ppu.cgLatchHigh ? 1u : 0u);
  });
  expectTheLoadAloneMoves(m, secondHalf, 0x213Fu, [](const SnesState& s) {
    return (s.ppu.countersLatched ? 4u : 0u) | (s.ppu.ophctHigh ? 2u : 0u) | (s.ppu.opvctHigh ? 1u : 0u);
  });
}

TEST(SnesHostMemory, PeekRegisterTakesTheOpenBusBitsFromTheByteTheBusLastCarried) {
  Snes m = machine();
  m.step();
  m.poke(0x000300u, 0x5Bu);
  loadFrom(m, 0x0300u);
  m.step();  // the load leaves $5B on the data bus
  ASSERT_EQ(m.state().mdr, 0x5Bu);
  const std::optional<std::uint8_t> rdnmi = m.peekRegister(0x004210u);
  ASSERT_TRUE(rdnmi.has_value());
  EXPECT_EQ(*rdnmi & 0x70u, 0x5Bu & 0x70u);                               // RDNMI's bits 6-4
  EXPECT_EQ(m.peekRegister(0x002100u), std::optional<std::uint8_t>{0x5Bu});  // INIDISP is write-only
  EXPECT_EQ(m.peekRegister(0x002200u), std::nullopt);                        // between the windows
}

TEST(SnesHostMemory, PeekApuRegisterReadsTheInputPortTheProgramWrote) {
  Snes m = machine();
  m.step();
  runFrom0200(m, {0xA9u, 0x6Du, 0x8Du, 0x40u, 0x21u});  // LDA #$6D ; STA $2140
  m.step();
  m.step();
  EXPECT_EQ(m.peekApuRegister(0x00F4u), std::optional<std::uint8_t>{0x6Du});
  EXPECT_EQ(m.peekApuRegister(0x0400u), std::nullopt);  // RAM is peekApu's
}

TEST(SnesHostMemory, PeekingEveryRegisterBetweenRunsLandsByteIdenticalToAPlainRun) {
  Snes a = machine();
  Snes b = machine();
  a.run(150000u);
  b.run(150000u);
  std::size_t answered = 0;
  for (std::uint32_t offset = 0; offset <= 0xFFFFu; ++offset) {
    if (a.peekRegister(offset).has_value()) ++answered;
  }
  EXPECT_EQ(answered, 0x84u + 0x2u + 0x20u + 0x80u);
  a.run(150000u);
  b.run(150000u);
  EXPECT_TRUE(a.state() == b.state());
  EXPECT_EQ(a.takeFrames(), b.takeFrames());
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

TEST(SnesHostMemory, WriteApuPortSetsTheInputLatchTheSpc700Reads) {
  Snes m = machine();
  const std::array<std::uint8_t, 4> outputBefore = m.state().apu.outputPorts;
  // Each port takes its value from the console's side, in the input latch the SPC700
  // reads at $F4 + index — the same latch a CPU store to $2140 + index sets.
  m.writeApuPort(0u, 0x12u);
  m.writeApuPort(1u, 0x34u);
  m.writeApuPort(2u, 0x56u);
  m.writeApuPort(3u, 0x78u);
  EXPECT_EQ(m.state().apu.inputPorts[0], 0x12u);
  EXPECT_EQ(m.state().apu.inputPorts[1], 0x34u);
  EXPECT_EQ(m.state().apu.inputPorts[2], 0x56u);
  EXPECT_EQ(m.state().apu.inputPorts[3], 0x78u);
  // The output latches the SPC700 wrote stand: writing one side never disturbs the other.
  for (std::size_t i = 0; i < 4; ++i) {
    EXPECT_EQ(m.state().apu.outputPorts[i], outputBefore[i]) << "output port " << i;
  }
  // An index past 3 wraps modulo 4, the way the bus mirrors the ports through $217F.
  m.writeApuPort(4u, 0x9Au);  // -> port 0
  m.writeApuPort(7u, 0xBCu);  // -> port 3
  EXPECT_EQ(m.state().apu.inputPorts[0], 0x9Au);
  EXPECT_EQ(m.state().apu.inputPorts[3], 0xBCu);
}

TEST(SnesHostMemory, WriteApuPortDrivesTheRunningSpc700) {
  // The 5A22's own way of talking to the sound CPU, done from the host's side. The
  // cartridge loops harmlessly while the APU boots its upload stub; the stub reads
  // input port 0 at $F4 and echoes it to output port 0 to acknowledge each step, so a
  // byte set with writeApuPort comes back through peekRegister once the running SPC700
  // has read it — the whole path, not just the latch.
  Snes m = machine();
  const auto ready = [&] {
    return m.peekRegister(0x2140u).value_or(0x00u) == 0xAAu &&
           m.peekRegister(0x2141u).value_or(0x00u) == 0xBBu;
  };
  bool posted = false;
  for (int i = 0; i < 4000 && !(posted = ready()); ++i) m.run(256u);
  ASSERT_TRUE(posted) << "the stub posts its ready bytes on the output ports";

  // The first command by the stub's protocol: point it at an address and kick port 0
  // with $CC, all through writeApuPort — the same stores the CPU would make to $2140-$2143.
  m.writeApuPort(2u, 0x00u);  // the address low byte, $0200
  m.writeApuPort(3u, 0x02u);
  m.writeApuPort(1u, 0x01u);  // nonzero sets an address rather than starting a program
  m.writeApuPort(0u, 0xCCu);

  bool echoed = false;
  for (int i = 0; i < 4000 && !(echoed = m.peekRegister(0x2140u).value_or(0x00u) == 0xCCu); ++i) {
    m.run(256u);
  }
  EXPECT_TRUE(echoed) << "the running SPC700 read the port writeApuPort set and echoed it";
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

TEST(SnesHostMemory, SetCpuStateKeepsQueuedAudioFramesAndTheRestOfTheMachine) {
  // Writing the register file touches the CPU alone: the audio frames queued
  // since the last drain are still there, and the machine runs on to the same
  // state as one whose registers were never written.
  Snes a = machine();
  Snes b = machine();
  a.run(100000u);  // enough for the audio machine to queue frames
  b.run(100000u);
  a.setCpuState(a.cpuState());  // the same registers, written back
  a.run(100000u);
  b.run(100000u);
  const std::vector<StereoFrame> fa = a.takeFrames();
  const std::vector<StereoFrame> fb = b.takeFrames();
  EXPECT_GT(fa.size(), 100u);
  EXPECT_EQ(fa, fb) << "no frame was dropped by the write";
  EXPECT_TRUE(a.state() == b.state());
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
