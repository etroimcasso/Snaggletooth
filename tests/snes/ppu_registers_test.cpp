// The PPU's register file beyond the memory ports: the two write-twice latches
// the scroll and Mode 7 registers share, the product at $2134-$2136, the H/V
// counter latch under the I/O port's top bit, the two status registers, the
// three open-bus values a read can answer with, and the windows in which VRAM,
// the sprite table and the palette can be reached. Every expectation is
// computed by hand from the register page; the cartridges are assembled inline.

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "snaggletooth/snes/snes.h"

namespace snaggletooth {
namespace {

constexpr std::uint8_t kLdaImm = 0xA9u;
constexpr std::uint8_t kLdaAbs = 0xADu;
constexpr std::uint8_t kStaAbs = 0x8Du;
constexpr std::uint8_t kStzAbs = 0x9Cu;
constexpr std::uint8_t kNop = 0xEAu;
constexpr std::uint8_t kStp = 0xDBu;

// A cartridge that runs `program` from $8000 and ends in STP.
std::vector<std::uint8_t> cartridge(std::vector<std::uint8_t> program) {
  program.resize(0x8000u, 0x00u);
  program[0x7FFCu] = 0x00u;  // reset -> $8000
  program[0x7FFDu] = 0x80u;
  return program;
}

// Runs `program` from power-on to its STP and returns the settled machine.
Snes run(std::vector<std::uint8_t> program, Region region = Region::Ntsc) {
  const std::vector<std::uint8_t> rom = cartridge(std::move(program));
  Snes m(SnesConfig{.rom = rom, .region = region});
  while (m.state().cpu.run == CpuRunState::Running) m.step();
  return m;
}

// Runs `program` with the beam placed at (vpos, hpos) before its first
// instruction, and the I/O port as `wrio`, to its STP. A short program stays
// where it was placed: a load-and-store pair costs 46 master cycles.
Snes runAt(std::vector<std::uint8_t> program, std::uint16_t vpos, std::uint16_t hpos,
           std::uint8_t wrio = 0xFFu, BusObserver* observer = nullptr) {
  const std::vector<std::uint8_t> rom = cartridge(std::move(program));
  Snes m(SnesConfig{.rom = rom});
  SnesState s = m.state();
  s.vpos = vpos;
  s.hpos = hpos;
  s.wrio = wrio;
  // Vertical blank is a latched fact rather than a comparison, so a beam placed by
  // hand carries it: a machine on a line past the start line is one whose blank began
  // there, which is what the placement means.
  s.inVblank = vpos >= s.ppu.vblankStartLine();
  if (s.inVblank) s.vblankBeginLine = s.ppu.vblankStartLine();
  m.restore(s);
  m.setObserver(observer);
  while (m.state().cpu.run == CpuRunState::Running) m.step();
  return m;
}

// Where every write to `port` landed, in order.
struct Landings final : BusObserver {
  explicit Landings(std::uint16_t port) : port_(port) {}
  std::vector<std::optional<std::uint16_t>> landed;
  void access(const BusAccess& a) override {
    if (a.write && (a.address & 0xFFFFu) == port_) landed.push_back(a.landed);
  }
  void internal(std::uint32_t, std::optional<CycleKind>) override {}
  std::uint16_t port_;
};

// LDA #v ; STA $21xx
std::vector<std::uint8_t> store(std::uint8_t low, std::uint8_t value) {
  return {kLdaImm, value, kStaAbs, low, 0x21u};
}
// LDA $21xx ; STA $00nn
std::vector<std::uint8_t> load(std::uint8_t low, std::uint8_t wram) {
  return {kLdaAbs, low, 0x21u, kStaAbs, wram, 0x00u};
}
std::vector<std::uint8_t> join(std::initializer_list<std::vector<std::uint8_t>> parts) {
  std::vector<std::uint8_t> out;
  for (const std::vector<std::uint8_t>& p : parts) out.insert(out.end(), p.begin(), p.end());
  out.push_back(kStp);
  return out;
}

constexpr std::uint16_t kPictureLine = 100u;
constexpr std::uint16_t kPictureDot = 200u;   // master cycles into the line, inside the active span
constexpr std::uint16_t kHblankStart = 1120u; // past the active span's end at 1112
constexpr std::uint16_t kVblankLine = 230u;

// ---- the scroll registers and their shared latch ---------------------------------

TEST(SnesPpuRegisters, ScrollRegistersAssembleThroughOneSharedLatch) {
  // BG2HOFS <- $34, $12: the second write takes the latch's $34 with its low three
  // bits replaced by the register's own bits 8-10, which the first write set to 4.
  // BG2VOFS <- $78, $56: its first write takes the latch whole, and the latch still
  // holds the $12 the horizontal register left.
  Snes m = run(join({store(0x0Fu, 0x34u), store(0x0Fu, 0x12u), store(0x10u, 0x78u), store(0x10u, 0x56u)}));
  EXPECT_EQ(m.state().ppu.bg2hofs, 0x1234u);
  EXPECT_EQ(m.state().ppu.bg2vofs, 0x5678u);
}

TEST(SnesPpuRegisters, AScrollWriteOutOfOrderTakesTheOtherRegistersByte) {
  // BG1HOFS low $34, then BG1VOFS low $78, whose write takes the latch's $34 as
  // its own low byte, then BG1HOFS high $12, which finds $78 in the latch.
  Snes m = run(join({store(0x0Du, 0x34u), store(0x0Eu, 0x78u), store(0x0Du, 0x12u)}));
  EXPECT_EQ(m.state().ppu.bg1vofs, 0x7834u);
  EXPECT_EQ(m.state().ppu.bg1hofs, 0x127Cu);  // ($12 << 8) | ($78 & ~7) | (($3400 >> 8) & 7)
}

TEST(SnesPpuRegisters, TheBg1OffsetPortsUpdateMode7ThroughTheirOwnLatch) {
  // M7A <- $AB leaves $AB in the Mode 7 latch and nothing in the scroll latch; the
  // one write to $210D then reads the two latches apart.
  Snes m = run(join({store(0x1Bu, 0xABu), store(0x0Du, 0x34u)}));
  EXPECT_EQ(m.state().ppu.m7hofs, 0x34ABu);
  EXPECT_EQ(m.state().ppu.bg1hofs, 0x3400u);
}

// ---- Mode 7 and the product -------------------------------------------------------

TEST(SnesPpuRegisters, Mode7RegistersAssembleThroughTheirSharedLatch) {
  // M7A <- $23, $01; M7B <- $FE, whose byte is also the latch M7C's write takes.
  Snes m = run(join({store(0x1Bu, 0x23u), store(0x1Bu, 0x01u), store(0x1Cu, 0xFEu), store(0x1Du, 0x11u)}));
  EXPECT_EQ(m.state().ppu.m7a, 0x0123u);
  EXPECT_EQ(m.state().ppu.m7b, 0xFE01u);
  EXPECT_EQ(m.state().ppu.m7c, 0x11FEu);
  EXPECT_EQ(m.state().ppu.m7bByte, 0xFEu);
}

TEST(SnesPpuRegisters, TheProductIsMatrixATimesTheLastByteOfMatrixB) {
  // $0123 * -2 = -582 = $FFFDBA, read back low, middle, high.
  Snes m = run(join({store(0x1Bu, 0x23u), store(0x1Bu, 0x01u), store(0x1Cu, 0xFEu),
                     load(0x34u, 0x50u), load(0x35u, 0x51u), load(0x36u, 0x52u)}));
  EXPECT_EQ(m.state().wram[0x50], 0xBAu);
  EXPECT_EQ(m.state().wram[0x51], 0xFDu);
  EXPECT_EQ(m.state().wram[0x52], 0xFFu);
}

TEST(SnesPpuRegisters, TheProductCarriesItsSignIntoTheHighByte) {
  // $8000 (-32768) * $7F (127) = -4161536 = $C08000 in 24 bits.
  Snes m = run(join({store(0x1Bu, 0x00u), store(0x1Bu, 0x80u), store(0x1Cu, 0x7Fu),
                     load(0x34u, 0x50u), load(0x35u, 0x51u), load(0x36u, 0x52u)}));
  EXPECT_EQ(m.state().wram[0x50], 0x00u);
  EXPECT_EQ(m.state().wram[0x51], 0x80u);
  EXPECT_EQ(m.state().wram[0x52], 0xC0u);
}

TEST(SnesPpuRegisters, TheProductAtPowerOnIsOne) {
  // Both operands power on at -1.
  Snes m = run(join({load(0x34u, 0x50u), load(0x35u, 0x51u), load(0x36u, 0x52u)}));
  EXPECT_EQ(m.state().ppu.m7a, 0xFFFFu);
  EXPECT_EQ(m.state().ppu.m7bByte, 0xFFu);
  EXPECT_EQ(m.state().wram[0x50], 0x01u);
  EXPECT_EQ(m.state().wram[0x51], 0x00u);
  EXPECT_EQ(m.state().wram[0x52], 0x00u);
}

TEST(SnesPpuRegisters, ATransferReadsTheProductOntoTheABus) {
  // Channel 0, B to A, the four-register pattern from $2134, three bytes into work
  // RAM at $7E:0100 — the way a program fills memory from the multiplier.
  std::vector<std::uint8_t> program = join({store(0x1Bu, 0x23u), store(0x1Bu, 0x01u), store(0x1Cu, 0xFEu),
                                            {kLdaImm, 0x01u, kStaAbs, 0x0Bu, 0x42u, kNop}});
  const std::vector<std::uint8_t> rom = cartridge(std::move(program));
  Snes m(SnesConfig{.rom = rom});
  SnesState s = m.state();
  s.dma[0] = DmaChannel{.dmap = 0x84u, .bbad = 0x34u, .a1t = 0x0100u, .a1b = 0x7Eu, .das = 0x0003u};
  m.restore(s);
  while (m.state().cpu.run == CpuRunState::Running) m.step();
  EXPECT_EQ(m.state().wram[0x100], 0xBAu);
  EXPECT_EQ(m.state().wram[0x101], 0xFDu);
  EXPECT_EQ(m.state().wram[0x102], 0xFFu);
}

// ---- the counter latch --------------------------------------------------------------

// The beam after the first instruction's data cycle, when that instruction is a
// four-cycle absolute access placed at master cycle 400 of a line: three slow
// fetches from the cartridge (8 each) and the fast access itself (6) tick 30
// cycles before the access resolves, so it sees master cycle 430 — dot 107.
constexpr std::uint16_t kLatchPlacement = 400u;
constexpr std::uint8_t kLatchedDot = 107u;

TEST(SnesPpuRegisters, ReadingTheSoftwareLatchCapturesTheBeamAndTheCountersReadOutInHalves) {
  Snes m = runAt(join({{kLdaAbs, 0x37u, 0x21u},
                       load(0x3Cu, 0x50u), load(0x3Cu, 0x51u),
                       load(0x3Du, 0x52u), load(0x3Du, 0x53u),
                       load(0x3Fu, 0x54u), load(0x3Fu, 0x55u)}),
                 kPictureLine, kLatchPlacement);
  EXPECT_EQ(m.state().wram[0x50], kLatchedDot);
  // The second half: the ninth bit under the second half's open bus, which is the
  // byte the first half just answered with.
  EXPECT_EQ(m.state().wram[0x51], kLatchedDot & 0xFEu);
  EXPECT_EQ(m.state().wram[0x52], kPictureLine);
  EXPECT_EQ(m.state().wram[0x53], kPictureLine & 0xFEu);
  // STAT78: no field, the latch flag, open bus bit 5 from the $64 just read, NTSC,
  // version 3 — then the same read with the flag cleared and its own $63 on the bus.
  EXPECT_EQ(m.state().wram[0x54], 0x63u);
  EXPECT_EQ(m.state().wram[0x55], 0x23u);
}

TEST(SnesPpuRegisters, ReadingStat78ResetsBothCountersHalves) {
  // One half of OPHCT read, then STAT78, then OPHCT again: the low half again.
  Snes m = runAt(join({{kLdaAbs, 0x37u, 0x21u}, load(0x3Cu, 0x50u), load(0x3Fu, 0x51u), load(0x3Cu, 0x52u)}),
                 kPictureLine, kLatchPlacement);
  EXPECT_EQ(m.state().wram[0x50], kLatchedDot);
  EXPECT_EQ(m.state().wram[0x52], kLatchedDot);
}

TEST(SnesPpuRegisters, TheSoftwareLatchDoesNothingWhileTheLatchLineIsLow) {
  Snes m = runAt(join({{kLdaAbs, 0x37u, 0x21u}, load(0x3Cu, 0x50u), load(0x3Cu, 0x51u), load(0x3Fu, 0x52u)}),
                 kPictureLine, kLatchPlacement, /*wrio=*/0x7Fu);
  EXPECT_EQ(m.state().ppu.ophct, 0x01FFu);  // the power-on value stands
  EXPECT_EQ(m.state().wram[0x50], 0xFFu);
  EXPECT_EQ(m.state().wram[0x51], 0xFFu);   // bit 0 set, the rest the $FF just read
  EXPECT_EQ(m.state().wram[0x52] & 0x40u, 0u);
}

TEST(SnesPpuRegisters, TheLatchLineFallingCapturesTheBeam) {
  // STZ $4201 takes bit 7 from 1 to 0 at its write cycle, placed like the read above.
  Snes m = runAt(join({{kStzAbs, 0x01u, 0x42u}, load(0x3Cu, 0x50u), load(0x3Du, 0x51u), load(0x3Fu, 0x52u)}),
                 kPictureLine, kLatchPlacement);
  EXPECT_EQ(m.state().wram[0x50], kLatchedDot);
  EXPECT_EQ(m.state().wram[0x51], kPictureLine);
  EXPECT_NE(m.state().wram[0x52] & 0x40u, 0u);
}

TEST(SnesPpuRegisters, TheIoPortReadsBackAsWritten) {
  Snes m = run(join({{kLdaImm, 0x5Au, kStaAbs, 0x01u, 0x42u, kLdaAbs, 0x13u, 0x42u, kStaAbs, 0x50u, 0x00u}}));
  EXPECT_EQ(m.state().wrio, 0x5Au);
  EXPECT_EQ(m.state().wram[0x50], 0x5Au);
}

// ---- the status registers -----------------------------------------------------------

TEST(SnesPpuRegisters, Stat77CarriesTheFirstHalfsOpenBusAndVersion) {
  // Before any first-half read: version 1 alone. After MPYL reads $10 (M7A $0010
  // times M7B $01), bit 4 follows that byte.
  Snes m = run(join({load(0x3Eu, 0x50u), store(0x1Bu, 0x10u), store(0x1Bu, 0x00u), store(0x1Cu, 0x01u),
                     load(0x34u, 0x51u), load(0x3Eu, 0x52u)}));
  EXPECT_EQ(m.state().wram[0x50], 0x01u);
  EXPECT_EQ(m.state().wram[0x51], 0x10u);
  EXPECT_EQ(m.state().wram[0x52], 0x11u);
}

TEST(SnesPpuRegisters, Stat78CarriesTheClockRateAndVersion) {
  // Bit 7 is the frame parity, which the machine's first frame takes at H = 1 of its
  // first line: the console leaves reset with the flag clear, so that frame is the
  // pair's second and the bit reads set through it.
  Snes ntsc = run(join({load(0x3Fu, 0x50u)}));
  EXPECT_EQ(ntsc.state().wram[0x50], 0x83u);
  Snes pal = run(join({load(0x3Fu, 0x50u)}), Region::Pal);
  EXPECT_EQ(pal.state().wram[0x50], 0x93u);
}

TEST(SnesPpuRegisters, Stat78CarriesTheFrameParity) {
  Snes m = run(join({load(0x3Fu, 0x50u)}));
  SnesState s = m.state();
  s.field = 1u;
  s.cpu = Snes(SnesConfig{.rom = cartridge(join({load(0x3Fu, 0x50u)}))}).state().cpu;  // back to the start
  m.restore(s);
  while (m.state().cpu.run == CpuRunState::Running) m.step();
  EXPECT_EQ(m.state().wram[0x50], 0x83u);
}

// ---- the three open-bus values ------------------------------------------------------

TEST(SnesPpuRegisters, AWriteOnlyRegisterInTheFirstHalfsGroupsReadsItsOpenBus) {
  // MPYL reads $01 at power-on; $2104 then answers with it. INIDISP answers with
  // the CPU's own bus, the operand byte $21 the instruction just fetched.
  Snes m = run(join({load(0x34u, 0x50u), load(0x04u, 0x51u), load(0x00u, 0x52u)}));
  EXPECT_EQ(m.state().wram[0x51], 0x01u);
  EXPECT_EQ(m.state().wram[0x52], 0x21u);
}

TEST(SnesPpuRegisters, ThePaletteReadsSecondHalfCarriesTheSecondHalfsOpenBusInItsTopBit) {
  // Word 0 = $7F00: the low half reads $00, so the high half's top bit is clear.
  // Word 1 = $7F80: the low half reads $80, so the high half's top bit is set.
  Snes m = run(join({store(0x21u, 0x00u), store(0x22u, 0x00u), store(0x22u, 0x7Fu),
                     store(0x22u, 0x80u), store(0x22u, 0x7Fu),
                     store(0x21u, 0x00u), load(0x3Bu, 0x50u), load(0x3Bu, 0x51u),
                     load(0x3Bu, 0x52u), load(0x3Bu, 0x53u)}));
  EXPECT_EQ(m.state().wram[0x50], 0x00u);
  EXPECT_EQ(m.state().wram[0x51], 0x7Fu);
  EXPECT_EQ(m.state().wram[0x52], 0x80u);
  EXPECT_EQ(m.state().wram[0x53], 0xFFu);
}

// ---- the windows in which the memories can be reached --------------------------------

// The screen turned on, then one VRAM word written at $0010 through the port.
std::vector<std::uint8_t> vramWordWithScreenOn() {
  return join({store(0x00u, 0x0Fu), store(0x15u, 0x80u), store(0x16u, 0x10u), store(0x17u, 0x00u),
               store(0x18u, 0x34u), store(0x19u, 0x12u)});
}

TEST(SnesPpuRegisters, AVramWriteInsideThePictureIsIgnoredAndStepsTheAddress) {
  Landings landings(0x2118u);
  Snes m = runAt(vramWordWithScreenOn(), kPictureLine, kPictureDot, 0xFFu, &landings);
  EXPECT_EQ(m.vram()[0x20], 0x00u);
  EXPECT_EQ(m.vram()[0x21], 0x00u);
  EXPECT_EQ(m.state().ppu.vmadd, 0x0011u);
  ASSERT_EQ(landings.landed.size(), 1u);
  EXPECT_FALSE(landings.landed[0].has_value());
}

TEST(SnesPpuRegisters, AVramWriteInVerticalBlankLands) {
  Landings landings(0x2118u);
  Snes m = runAt(vramWordWithScreenOn(), kVblankLine, kPictureDot, 0xFFu, &landings);
  EXPECT_EQ(m.vram()[0x20], 0x34u);
  EXPECT_EQ(m.vram()[0x21], 0x12u);
  ASSERT_EQ(landings.landed.size(), 1u);
  EXPECT_EQ(landings.landed[0], std::optional<std::uint16_t>{0x0010u});
}

TEST(SnesPpuRegisters, AVramWriteInForcedBlankLandsOnAVisibleLine) {
  Snes m = runAt(join({store(0x15u, 0x80u), store(0x16u, 0x10u), store(0x17u, 0x00u),
                       store(0x18u, 0x34u), store(0x19u, 0x12u)}),
                 kPictureLine, kPictureDot);
  EXPECT_EQ(m.vram()[0x20], 0x34u);
  EXPECT_EQ(m.vram()[0x21], 0x12u);
}

TEST(SnesPpuRegisters, LineZeroIsNotVerticalBlankForTheMemories) {
  Snes m = runAt(vramWordWithScreenOn(), 0u, kPictureDot);
  EXPECT_EQ(m.vram()[0x20], 0x00u);
}

TEST(SnesPpuRegisters, AVramReadInsideThePictureDoesNotRefillThePrefetch) {
  // Two words written in forced blank, the prefetch loaded with the first, the
  // screen turned on, then three reads: the register never reloads, so the
  // first word's low byte comes back every time and the address steps regardless.
  Snes m = runAt(join({store(0x15u, 0x80u), store(0x16u, 0x00u), store(0x17u, 0x00u),
                       store(0x18u, 0x34u), store(0x19u, 0x12u), store(0x18u, 0x78u), store(0x19u, 0x56u),
                       store(0x15u, 0x00u), store(0x16u, 0x00u), store(0x17u, 0x00u),
                       store(0x00u, 0x0Fu),
                       load(0x39u, 0x50u), load(0x39u, 0x51u), load(0x39u, 0x52u)}),
                 kPictureLine, kPictureDot);
  EXPECT_EQ(m.state().wram[0x50], 0x34u);
  EXPECT_EQ(m.state().wram[0x51], 0x34u);
  EXPECT_EQ(m.state().wram[0x52], 0x34u);  // in blank this read would be word 1's $78
  EXPECT_EQ(m.state().ppu.vmadd, 0x0003u);
}

TEST(SnesPpuRegisters, AnOamWriteInsideThePictureIsIgnoredAndStepsTheAddress) {
  Landings landings(0x2104u);
  Snes m = runAt(join({store(0x00u, 0x0Fu), store(0x02u, 0x02u), store(0x03u, 0x00u),
                       store(0x04u, 0x34u), store(0x04u, 0x12u)}),
                 kPictureLine, kPictureDot, 0xFFu, &landings);
  EXPECT_EQ(m.oam()[4], 0x00u);
  EXPECT_EQ(m.oam()[5], 0x00u);
  EXPECT_EQ(m.state().ppu.oamAddress, 6u);
  ASSERT_EQ(landings.landed.size(), 2u);
  EXPECT_FALSE(landings.landed[0].has_value());
  EXPECT_FALSE(landings.landed[1].has_value());
}

TEST(SnesPpuRegisters, AnOamReadInsideThePictureAnswersWithTheFirstHalfsOpenBus) {
  // Byte 0 = $CD written in forced blank; MPYL read for a known open bus of $01;
  // the screen on; the read at address 0 answers $01 and the address still steps.
  Snes m = runAt(join({store(0x02u, 0x00u), store(0x03u, 0x00u), store(0x04u, 0xCDu), store(0x04u, 0xABu),
                       load(0x34u, 0x60u), store(0x00u, 0x0Fu), store(0x02u, 0x00u),
                       load(0x38u, 0x50u)}),
                 kPictureLine, kPictureDot);
  EXPECT_EQ(m.oam()[0], 0xCDu);
  EXPECT_EQ(m.state().wram[0x50], 0x01u);
  EXPECT_EQ(m.state().ppu.oamAddress, 1u);
}

TEST(SnesPpuRegisters, APaletteWriteInHorizontalBlankLands) {
  Landings landings(0x2122u);
  Snes m = runAt(join({store(0x00u, 0x0Fu), store(0x21u, 0x10u), store(0x22u, 0x34u), store(0x22u, 0x12u)}),
                 kPictureLine, kHblankStart, 0xFFu, &landings);
  EXPECT_EQ(m.cgram()[0x20], 0x34u);
  EXPECT_EQ(m.cgram()[0x21], 0x12u);
  ASSERT_EQ(landings.landed.size(), 2u);
  EXPECT_EQ(landings.landed[1], std::optional<std::uint16_t>{0x0010u});
}

TEST(SnesPpuRegisters, APaletteWriteInsideThePictureIsIgnoredAndStepsTheAddress) {
  Landings landings(0x2122u);
  Snes m = runAt(join({store(0x00u, 0x0Fu), store(0x21u, 0x10u), store(0x22u, 0x34u), store(0x22u, 0x12u)}),
                 kPictureLine, kPictureDot, 0xFFu, &landings);
  EXPECT_EQ(m.cgram()[0x20], 0x00u);
  EXPECT_EQ(m.cgram()[0x21], 0x00u);
  EXPECT_EQ(m.state().ppu.cgadd, 0x11u);
  EXPECT_FALSE(m.state().ppu.cgLatchHigh);
  ASSERT_EQ(landings.landed.size(), 2u);
  EXPECT_FALSE(landings.landed[1].has_value());
}

TEST(SnesPpuRegisters, APaletteReadInsideThePictureAnswersWithTheSecondHalfsOpenBus) {
  // Word 0 = $1234 written in forced blank; STAT78 read for a known open bus of
  // $03; the screen on; the read answers $03.
  Snes m = runAt(join({store(0x21u, 0x00u), store(0x22u, 0x34u), store(0x22u, 0x12u),
                       load(0x3Fu, 0x60u), store(0x00u, 0x0Fu), store(0x21u, 0x00u),
                       load(0x3Bu, 0x50u)}),
                 kPictureLine, kPictureDot);
  EXPECT_EQ(m.cgram()[0], 0x34u);
  EXPECT_EQ(m.state().wram[0x50], 0x03u);
  EXPECT_TRUE(m.state().ppu.cgLatchHigh);
}

// ---- the fixed colour and the plain registers ------------------------------------------

TEST(SnesPpuRegisters, TheFixedColourTakesEachChannelItsWriteSelects) {
  Snes m = run(join({store(0x32u, 0x3Fu), store(0x32u, 0x4Au), store(0x32u, 0x95u)}));
  EXPECT_EQ(m.state().ppu.fixedRed, 0x1Fu);
  EXPECT_EQ(m.state().ppu.fixedGreen, 0x0Au);
  EXPECT_EQ(m.state().ppu.fixedBlue, 0x15u);
  Snes all = run(join({store(0x32u, 0xE3u)}));
  EXPECT_EQ(all.state().ppu.fixedRed, 0x03u);
  EXPECT_EQ(all.state().ppu.fixedGreen, 0x03u);
  EXPECT_EQ(all.state().ppu.fixedBlue, 0x03u);
}

TEST(SnesPpuRegisters, EveryPlainRegisterStoresWhatIsWritten) {
  struct Plain {
    std::uint8_t low;
    std::uint8_t PpuState::*field;
  };
  const Plain plain[] = {
      {0x01u, &PpuState::objsel},  {0x05u, &PpuState::bgmode},  {0x06u, &PpuState::mosaic},
      {0x07u, &PpuState::bg1sc},   {0x08u, &PpuState::bg2sc},   {0x09u, &PpuState::bg3sc},
      {0x0Au, &PpuState::bg4sc},   {0x0Bu, &PpuState::bg12nba}, {0x0Cu, &PpuState::bg34nba},
      {0x15u, &PpuState::vmain},   {0x1Au, &PpuState::m7sel},   {0x23u, &PpuState::w12sel},
      {0x24u, &PpuState::w34sel},  {0x25u, &PpuState::wobjsel}, {0x26u, &PpuState::wh0},
      {0x27u, &PpuState::wh1},     {0x28u, &PpuState::wh2},     {0x29u, &PpuState::wh3},
      {0x2Au, &PpuState::wbglog},  {0x2Bu, &PpuState::wobjlog}, {0x2Cu, &PpuState::tm},
      {0x2Du, &PpuState::ts},      {0x2Eu, &PpuState::tmw},     {0x2Fu, &PpuState::tsw},
      {0x30u, &PpuState::cgwsel},  {0x31u, &PpuState::cgadsub}, {0x33u, &PpuState::setini},
  };
  std::vector<std::uint8_t> program;
  for (const Plain& p : plain) {
    const std::vector<std::uint8_t> one = store(p.low, static_cast<std::uint8_t>(p.low ^ 0xA5u));
    program.insert(program.end(), one.begin(), one.end());
  }
  program.push_back(kStp);
  Snes m = run(program);
  for (const Plain& p : plain) {
    EXPECT_EQ(int{m.state().ppu.*p.field}, int{static_cast<std::uint8_t>(p.low ^ 0xA5u)}) << "$21" << std::hex << int{p.low};
  }
}

// ---- the state as a value ---------------------------------------------------------------

TEST(SnesPpuRegisters, ASnapshotCarriesAHalfWrittenRegister) {
  // The low byte of BG2HOFS written on one machine; its state restored into
  // another, whose program writes the high byte.
  Snes first = run(join({store(0x0Fu, 0x34u)}));
  const std::vector<std::uint8_t> rom = cartridge(join({store(0x0Fu, 0x12u)}));
  Snes second(SnesConfig{.rom = rom});
  SnesState s = first.state();
  s.cpu = second.state().cpu;
  second.restore(s);
  while (second.state().cpu.run == CpuRunState::Running) second.step();
  EXPECT_EQ(second.state().ppu.bg2hofs, 0x1234u);
}

TEST(SnesPpuRegisters, PowerOnValuesAreTheConsoles) {
  Snes m = run({kStp});
  const PpuState& p = m.state().ppu;
  EXPECT_EQ(p.inidisp, 0x80u);
  EXPECT_EQ(p.bgmode, 0x0Fu);
  EXPECT_EQ(p.vmain, 0x0Fu);
  EXPECT_EQ(p.m7a, 0xFFFFu);
  EXPECT_EQ(p.m7b, 0xFFFFu);
  EXPECT_EQ(p.setini, 0x00u);
  EXPECT_EQ(p.ophct, 0x01FFu);
  EXPECT_EQ(p.opvct, 0x01FFu);
  EXPECT_FALSE(p.countersLatched);
  EXPECT_EQ(m.state().wrio, 0xFFu);
}

}  // namespace
}  // namespace snaggletooth
