// The machine's beam: the dot map and its two six-cycle dots, the horizontal-blank
// flag's edges, the frame's shape under SETINI's interlace and overscan bits, the
// exact master offset of every per-line event — vertical blank, the NMI flag, the
// frame parity, the sprite table's reload, the overflow flags' clear and the H/V
// timer — and the memory refresh that pauses the CPU once a line.
// Nothing here draws. Every expectation is computed from the timing pages; the
// cartridges are assembled inline.

#include <cstdint>
#include <memory>
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
constexpr std::uint8_t kNop = 0xEAu;
constexpr std::uint8_t kStp = 0xDBu;
constexpr std::uint8_t kWai = 0xCBu;
constexpr std::uint8_t kCli = 0x58u;

constexpr std::uint8_t kRefreshMaster = 40u;  // the pause's length

constexpr std::uint32_t kLine = 1364u;       // a normal scanline
constexpr std::uint32_t kShortLine = 1360u;  // NTSC's line 240 on an odd field
constexpr std::uint32_t kLongLine = 1368u;   // PAL's line 311 on an odd interlaced field

// What each shape of access costs in master cycles. A fetch from the cartridge in
// bank 0 is eight cycles and a register is six, so a load or store of an absolute
// address is an opcode and two address bytes and then the access itself.
constexpr std::uint16_t kAbsoluteAccess = 8u + 8u + 8u + 6u;
constexpr std::uint16_t kImmediateLoad = 8u + 8u;
constexpr std::uint16_t kNopCost = 8u + 6u;

// The events this block places, as master-cycle offsets into their line.
constexpr std::uint16_t kHblankClear = 4u;     // H = 1
constexpr std::uint16_t kHblankSet = 1096u;    // H = 274
constexpr std::uint16_t kFieldToggle = 4u;     // H = 1 of line 0
constexpr std::uint16_t kNmiFlag = 2u;         // H = 0.5 of vertical blank's first line
constexpr std::uint16_t kOamReload = 40u;      // H = 10 of that line
constexpr std::uint16_t kFirstRefresh = 538u;  // into line 0 of the first frame

// A cartridge that runs `program` from $8000.
std::vector<std::uint8_t> cartridge(std::vector<std::uint8_t> program) {
  program.resize(0x8000u, 0x00u);
  program[0x7FFCu] = 0x00u;  // reset -> $8000
  program[0x7FFDu] = 0x80u;
  return program;
}

// Where a machine is placed before its program runs, and what it has been told.
// Every field is machine state, so a placement is a restore — except the clock
// rate, which is fixed at construction.
struct Placement {
  std::uint16_t vpos = 0;         // the line the beam is on
  std::uint16_t hpos = 0;         // master cycles into it
  Region region = Region::Ntsc;
  std::uint8_t setini = 0;        // $2133: bit 0 interlace, bit 2 overscan
  std::uint8_t field = 0;         // the frame parity
  bool inVblank = false;          // whether vertical blank has already begun this frame
  std::uint8_t inidisp = 0x0Fu;   // the screen on, which is what a drawing frame looks like
  std::uint8_t nmitimen = 0;      // $4200: the H/V timer's mode
  std::uint16_t htime = 0x01FFu;
  std::uint16_t vtime = 0x01FFu;
};

void apply(SnesState& s, const Placement& at) {
  s.hpos = at.hpos;
  s.vpos = at.vpos;
  s.field = at.field;
  s.inVblank = at.inVblank;
  if (at.inVblank) s.vblankBeginLine = at.vpos;  // a placed blank began where it is
  s.nmitimen = at.nmitimen;
  s.htime = at.htime;
  s.vtime = at.vtime;
  s.ppu.setini = at.setini;
  s.ppu.inidisp = at.inidisp;
}

// Runs `program` from the placement to its STP and returns the settled machine.
Snes placed(std::vector<std::uint8_t> program, const Placement& at,
            BusObserver* observer = nullptr) {
  const std::vector<std::uint8_t> rom = cartridge(std::move(program));
  Snes m(SnesConfig{.rom = rom, .region = at.region});
  SnesState s = m.state();
  apply(s, at);
  m.restore(s);
  m.setObserver(observer);
  while (m.state().cpu.run == CpuRunState::Running) m.step();
  return m;
}

// A machine whose core is halted at the frame origin, so run() advances it in
// six-cycle idle steps from the placement. The line's pause holds a halted core as it
// holds any other, and it is forty cycles — four past a multiple of six — so the idle
// grid past N pauses sits 4N cycles on from the budget's own.
Snes haltedAt(const Placement& at) {
  Snes m(SnesConfig{.rom = cartridge({kStp}), .region = at.region});
  m.step();  // execute the STP so the core is halted
  SnesState s = m.state();
  s.master = 0; s.consumed = 0; s.apuPhase = 0;
  apply(s, at);
  s.refreshAt = kFirstRefresh;
  m.restore(s);
  return m;
}

// A machine whose core is waiting on WAI at the frame origin, with an IRQ handler at
// $8100 that marks $0050 and stops. The wait begins before the rebase, so every cycle
// the placement runs is an idle one on its own six-cycle grid.
Snes waitingAt(const Placement& at) {
  std::vector<std::uint8_t> rom = cartridge({kCli, kWai});
  rom[0x7FFEu] = 0x00u;  // the emulation IRQ vector -> $8100
  rom[0x7FFFu] = 0x81u;
  const std::uint8_t handler[] = {kLdaImm, 0x5Au, kStaAbs, 0x50u, 0x00u, kStp};
  for (std::size_t i = 0; i < sizeof handler; ++i) rom[0x0100u + i] = handler[i];
  Snes m(SnesConfig{.rom = rom, .region = at.region});
  while (m.state().cpu.run == CpuRunState::Running) m.step();  // through CLI, into the wait
  SnesState s = m.state();
  s.master = 0; s.consumed = 0; s.apuPhase = 0;
  apply(s, at);
  s.refreshAt = kFirstRefresh;
  m.restore(s);
  return m;
}

// A machine whose program spins on a branch, so it keeps making bus cycles for as
// long as it is run.
Snes spinning() { return Snes(SnesConfig{.rom = cartridge({0x80u, 0xFEu})}); }

// LDA $xxxx ; STA $0050 ; STP, with `nops` padding the front. The read resolves
// kAbsoluteAccess cycles after the padding, and the byte it answered is at $0050.
std::vector<std::uint8_t> readProgram(std::uint16_t port, std::uint8_t nops = 0) {
  std::vector<std::uint8_t> p(nops, kNop);
  p.push_back(kLdaAbs);
  p.push_back(static_cast<std::uint8_t>(port & 0xFFu));
  p.push_back(static_cast<std::uint8_t>(port >> 8));
  p.push_back(kStaAbs);
  p.push_back(0x50u);
  p.push_back(0x00u);
  p.push_back(kStp);
  return p;
}

// LDA #value ; STA $xxxx ; STP. The write resolves kImmediateLoad + kAbsoluteAccess
// cycles in.
std::vector<std::uint8_t> writeProgram(std::uint16_t port, std::uint8_t value) {
  return {kLdaImm, value,
          kStaAbs, static_cast<std::uint8_t>(port & 0xFFu), static_cast<std::uint8_t>(port >> 8),
          kStp};
}

// The byte a read of `port` answers when it resolves exactly `master` cycles into
// the line the placement names, reached from within that line.
std::uint8_t readWithin(std::uint16_t port, const Placement& line, std::uint16_t master) {
  Placement at = line;
  at.hpos = static_cast<std::uint16_t>(master - kAbsoluteAccess);
  return placed(readProgram(port), at).state().wram[0x50];
}

// The same, reached from the line before it, so the beam crosses the line's start
// under the machine's own hand and every event that start carries has fired. `nops`
// pad the program when the target lies further in than one load reaches.
std::uint8_t readCrossing(std::uint16_t port, const Placement& line, std::uint16_t master,
                          std::uint8_t nops = 0) {
  Placement at = line;
  at.vpos = static_cast<std::uint16_t>(line.vpos - 1u);
  const std::uint16_t lead = static_cast<std::uint16_t>(kNopCost * nops + kAbsoluteAccess);
  at.hpos = static_cast<std::uint16_t>(kLine - lead + master);
  return placed(readProgram(port, nops), at).state().wram[0x50];
}

// The dot OPHCT latches when a read of $2137 resolves `master` cycles into the
// placement's line.
std::uint16_t latchedDot(const Placement& line, std::uint16_t master) {
  Placement at = line;
  at.hpos = static_cast<std::uint16_t>(master - kAbsoluteAccess);
  return placed({kLdaAbs, 0x37u, 0x21u, kStp}, at).state().ppu.ophct;
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

// How many cycles the machine reported, accesses and internal cycles together.
struct CycleCount final : BusObserver {
  std::uint32_t cycles = 0;
  void access(const BusAccess&) override { ++cycles; }
  void internal(std::uint32_t, std::optional<CycleKind>) override { ++cycles; }
};

constexpr std::uint16_t kPictureLine = 100u;

// ---- the dot map ---------------------------------------------------------------

TEST(SnesPpuBeam, TheDotMapPutsTwoSixCycleDotsLateInANormalLine) {
  // Dots 323 and 327 are six cycles wide and every other dot is four, so the line's
  // 1364 cycles carry 340 dots: 0-322 at 4, 323 at 6, 324-326 at 4, 327 at 6, and
  // 328-339 at 4 to the line's end.
  const Placement line{.vpos = kPictureLine};
  EXPECT_EQ(latchedDot(line, 1290u), 322u);
  EXPECT_EQ(latchedDot(line, 1292u), 323u);
  EXPECT_EQ(latchedDot(line, 1297u), 323u);  // the last cycle of the first long dot
  EXPECT_EQ(latchedDot(line, 1298u), 324u);
  EXPECT_EQ(latchedDot(line, 1309u), 326u);
  EXPECT_EQ(latchedDot(line, 1310u), 327u);
  EXPECT_EQ(latchedDot(line, 1315u), 327u);
  EXPECT_EQ(latchedDot(line, 1316u), 328u);
}

TEST(SnesPpuBeam, ANormalLineHasNoDot340) {
  // The counter's measured table gives dot 340 no cycles at all on a normal line:
  // its last four cycles are dot 339's.
  const Placement line{.vpos = kPictureLine};
  EXPECT_EQ(latchedDot(line, 1360u), 339u);
  EXPECT_EQ(latchedDot(line, 1363u), 339u);
}

TEST(SnesPpuBeam, TheShortLineIsThreeHundredFortyEvenDots) {
  // NTSC's line 240 on an odd field drops one dot, and the dots it keeps are all
  // four cycles: the two long dots belong to the lines that are 1364 cycles long.
  const Placement line{.vpos = 240u, .field = 1u};
  EXPECT_EQ(latchedDot(line, 1290u), 322u);
  EXPECT_EQ(latchedDot(line, 1292u), 323u);
  EXPECT_EQ(latchedDot(line, 1297u), 324u);
  EXPECT_EQ(latchedDot(line, 1310u), 327u);
  EXPECT_EQ(latchedDot(line, 1316u), 329u);
}

TEST(SnesPpuBeam, TheLongLineIsWhereDot340Exists) {
  // PAL's line 311 on an odd interlaced field runs 1368 cycles, and the four it
  // gains are dot 340's.
  const Placement line{.vpos = 311u, .region = Region::Pal, .setini = 0x01u, .field = 1u};
  EXPECT_EQ(latchedDot(line, 1363u), 339u);
  EXPECT_EQ(latchedDot(line, 1364u), 340u);
  EXPECT_EQ(latchedDot(line, 1367u), 340u);
}

// ---- the horizontal-blank flag -------------------------------------------------

TEST(SnesPpuBeam, HorizontalBlankIsRaisedAtHTwoHundredSeventyFourAndLoweredAtHOne) {
  const Placement line{.vpos = kPictureLine};
  EXPECT_EQ(readWithin(0x4212u, line, 1092u) & 0x40u, 0x00u);
  EXPECT_EQ(readWithin(0x4212u, line, kHblankSet) & 0x40u, 0x40u);
  EXPECT_EQ(readWithin(0x4212u, line, 1100u) & 0x40u, 0x40u);
  EXPECT_EQ(readCrossing(0x4212u, line, 0u) & 0x40u, 0x40u);  // still raised as the line starts
  EXPECT_EQ(readCrossing(0x4212u, line, kHblankClear) & 0x40u, 0x00u);
  EXPECT_EQ(readCrossing(0x4212u, line, 8u) & 0x40u, 0x00u);
}

TEST(SnesPpuBeam, HorizontalBlankTogglesOnEveryLineIncludingVerticalBlanksOwn) {
  const Placement line{.vpos = 230u, .inVblank = true};
  EXPECT_EQ(readWithin(0x4212u, line, 600u) & 0x40u, 0x00u);
  EXPECT_EQ(readWithin(0x4212u, line, 1200u) & 0x40u, 0x40u);
}

TEST(SnesPpuBeam, HorizontalBlankTogglesInForcedBlankToo) {
  const Placement line{.vpos = kPictureLine, .inidisp = 0x80u};
  EXPECT_EQ(readWithin(0x4212u, line, 600u) & 0x40u, 0x00u);
  EXPECT_EQ(readWithin(0x4212u, line, 1200u) & 0x40u, 0x40u);
}

TEST(SnesPpuBeam, ThePaletteWindowFollowsTheHorizontalBlankFlagsOwnEdges) {
  // The palette can be reached in horizontal blank, so where the flag's edges are is
  // where a mid-picture write to it lands: one resolving at master 1100 is inside the
  // blank and one at master 40 is not.
  const Placement line{.vpos = kPictureLine};

  Landings late(0x2122u);
  Placement at = line;
  at.hpos = static_cast<std::uint16_t>(1100u - kImmediateLoad - kAbsoluteAccess);
  placed(writeProgram(0x2122u, 0x12u), at, &late);
  ASSERT_EQ(late.landed.size(), 1u);
  EXPECT_TRUE(late.landed[0].has_value());

  Landings early(0x2122u);
  Placement before = line;
  before.vpos = static_cast<std::uint16_t>(line.vpos - 1u);
  before.hpos = static_cast<std::uint16_t>(kLine - kImmediateLoad - kAbsoluteAccess + 40u);
  placed(writeProgram(0x2122u, 0x12u), before, &early);
  ASSERT_EQ(early.landed.size(), 1u);
  EXPECT_FALSE(early.landed[0].has_value());
}

// ---- vertical blank and the NMI flag -------------------------------------------

TEST(SnesPpuBeam, TheNmiFlagFollowsTheVblankFlagByTwoMasterCycles) {
  // The vertical-blank signal is raised as the line starts; the NMI flag is raised at
  // H = 0.5, so a read resolving in between sees one without the other.
  const Placement line{.vpos = kVblankStartLine};
  EXPECT_EQ(readCrossing(0x4212u, line, 0u) & 0x80u, 0x80u);
  EXPECT_EQ(readCrossing(0x4210u, line, 0u) & 0x80u, 0x00u);
  EXPECT_EQ(readCrossing(0x4210u, line, 1u) & 0x80u, 0x00u);
  EXPECT_EQ(readCrossing(0x4210u, line, kNmiFlag) & 0x80u, 0x80u);
}

TEST(SnesPpuBeam, TheVblankFlagClearsAsTheFrameStarts) {
  const Placement first{.vpos = 0u, .inVblank = true};
  EXPECT_EQ(readCrossing(0x4212u, first, 8u) & 0x80u, 0x00u);
}

// ---- overscan ------------------------------------------------------------------

TEST(SnesPpuBeam, OverscanMovesVerticalBlanksStartToLineTwoHundredForty) {
  const Placement tall{.vpos = 230u, .setini = 0x04u};
  EXPECT_EQ(readCrossing(0x4212u, tall, 8u) & 0x80u, 0x00u);
  const Placement begun{.vpos = kOverscanVblankStartLine, .setini = 0x04u};
  EXPECT_EQ(readCrossing(0x4212u, begun, 8u) & 0x80u, 0x80u);
}

TEST(SnesPpuBeam, OverscanMovesTheNmiFlagWithTheStartLine) {
  const Placement tall{.vpos = 230u, .setini = 0x04u};
  EXPECT_EQ(readCrossing(0x4210u, tall, 8u) & 0x80u, 0x00u);
  const Placement begun{.vpos = kOverscanVblankStartLine, .setini = 0x04u};
  EXPECT_EQ(readCrossing(0x4210u, begun, kNmiFlag) & 0x80u, 0x80u);
}

TEST(SnesPpuBeam, ClearingOverscanBeforeLineTwoHundredFortyBeginsVblankAtTheNextLine) {
  // The machine asks again at the start of every line while vertical blank has not
  // begun, so the bit cleared during line 230 begins it as line 231 starts.
  Placement at{.vpos = 230u, .setini = 0x04u};
  at.hpos = 1300u;  // the SETINI write lands on line 230, the flag read on line 231
  std::vector<std::uint8_t> program = writeProgram(0x2133u, 0x00u);
  program.pop_back();  // drop the STP; the read follows
  const std::vector<std::uint8_t> read = readProgram(0x4212u);
  program.insert(program.end(), read.begin(), read.end());
  Snes m = placed(std::move(program), at);
  EXPECT_EQ(m.state().vpos, 231u);
  EXPECT_NE(m.state().wram[0x50] & 0x80u, 0x00u);
}

TEST(SnesPpuBeam, OverscanSetAfterVblankBeganHoldsTheVideoMemoryShut) {
  // The bit set too late resumes nothing, but the PPU keeps its memories as if the
  // picture were still running, to line 240.
  Landings landings(0x2118u);
  Placement at{.vpos = 232u, .setini = 0x04u, .inVblank = true};
  at.hpos = 600u;
  placed(writeProgram(0x2118u, 0x5Au), at, &landings);
  ASSERT_EQ(landings.landed.size(), 1u);
  EXPECT_FALSE(landings.landed[0].has_value());
}

TEST(SnesPpuBeam, TheVideoMemoryOpensAgainAtLineTwoHundredFortyWhenOverscanCameTooLate) {
  Landings landings(0x2118u);
  Placement at{.vpos = kOverscanVblankStartLine, .setini = 0x04u, .inVblank = true};
  at.hpos = 600u;
  placed(writeProgram(0x2118u, 0x5Au), at, &landings);
  ASSERT_EQ(landings.landed.size(), 1u);
  EXPECT_TRUE(landings.landed[0].has_value());
}

TEST(SnesPpuBeam, HdmaDeliversThroughTheTallerPicture) {
  // A repeat entry long enough to reach line 230 delivers there under overscan, and
  // the channels are still active: they deactivate when vertical blank begins, which
  // the taller picture puts at line 240.
  Snes m = haltedAt(Placement{.setini = 0x04u, .inidisp = 0x80u});
  SnesState s = m.state();
  s.dma[0] = DmaChannel{.dmap = 0x00u, .bbad = 0x21u, .a1t = 0x0300u, .a1b = 0x7Eu};
  s.hdmaen = 0x01u;
  s.wram[0x300u] = 0xFFu;  // repeat for 127 lines
  for (std::uint16_t i = 0; i < 127u; ++i) s.wram[0x301u + i] = 0x0Au;
  s.wram[0x380u] = 0xFFu;  // and 127 more
  for (std::uint16_t i = 0; i < 127u; ++i) s.wram[0x381u + i] = 0x0Bu;
  s.wram[0x381u + 103u] = 0x0Cu;  // the byte line 230 takes
  s.wram[0x400u] = 0x00u;
  s.ppu.cgadd = 0x00u;
  m.restore(s);

  m.run(230u * kLine + 1200u);
  EXPECT_EQ(m.state().vpos, 230u);
  EXPECT_EQ(m.state().ppu.cgadd, 0x0Cu);
  EXPECT_NE(m.state().hdmaActive & 1u, 0u);
}

TEST(SnesPpuBeam, HdmaStopsWhenTheTallerPicturesVblankBegins) {
  Snes m = haltedAt(Placement{.setini = 0x04u, .inidisp = 0x80u});
  SnesState s = m.state();
  s.dma[0] = DmaChannel{.dmap = 0x00u, .bbad = 0x21u, .a1t = 0x0300u, .a1b = 0x7Eu};
  s.hdmaen = 0x01u;
  s.wram[0x300u] = 0xFFu;
  for (std::uint16_t i = 0; i < 127u; ++i) s.wram[0x301u + i] = 0x0Au;
  s.wram[0x380u] = 0xFFu;
  for (std::uint16_t i = 0; i < 127u; ++i) s.wram[0x381u + i] = 0x0Bu;
  s.wram[0x400u] = 0x00u;
  m.restore(s);

  m.run(kOverscanVblankStartLine * kLine + 1200u);
  EXPECT_EQ(m.state().hdmaActive & 1u, 0u);
  SnesState marked = m.state();
  marked.ppu.cgadd = 0x33u;  // a marker a vblank delivery would overwrite
  m.restore(marked);
  m.run(2u * kLine);
  EXPECT_EQ(m.state().ppu.cgadd, 0x33u);
}

// ---- interlace -----------------------------------------------------------------

TEST(SnesPpuBeam, TheFrameParityTogglesAtHOneOfTheFirstLine) {
  // A read resolving before H = 1 sees the parity the frame just ended with, and one
  // after it sees the frame's own.
  const Placement first{.vpos = 0u, .field = 1u};
  EXPECT_EQ(readCrossing(0x213Fu, first, kFieldToggle - 2u) & 0x80u, 0x80u);
  EXPECT_EQ(readCrossing(0x213Fu, first, kFieldToggle + 2u) & 0x80u, 0x00u);
}

TEST(SnesPpuBeam, AnInterlacedFramePairRunsOneExtraLineAndNoShortOne) {
  // The parity toggles as each frame begins, so the first frame runs odd: no extra
  // line, and no short line either because interlace is asked for. The second runs
  // even and takes the extra line. Two frames are 262 + 263 whole lines.
  Snes m = haltedAt(Placement{.setini = 0x01u});
  m.run(262ull * kLine + 263ull * kLine);
  EXPECT_EQ(m.state().vpos, 0u);
  EXPECT_EQ(m.state().hpos, 0u);
  EXPECT_EQ(m.state().field, 0u);
}

TEST(SnesPpuBeam, AnInterlacedEvenFrameReachesLineTwoHundredSixtyTwo) {
  Snes m = haltedAt(Placement{.setini = 0x01u});
  m.run(262ull * kLine + 262ull * kLine + 8u);
  EXPECT_EQ(m.state().vpos, 262u);
}

TEST(SnesPpuBeam, TheExtraLineIsAVblankLineAndDeliversNoHdma) {
  Snes m = haltedAt(Placement{.setini = 0x01u, .inidisp = 0x80u});
  SnesState s = m.state();
  s.dma[0] = DmaChannel{.dmap = 0x00u, .bbad = 0x21u, .a1t = 0x0300u, .a1b = 0x7Eu};
  s.hdmaen = 0x01u;
  s.wram[0x300u] = 0xFFu;
  for (std::uint16_t i = 0; i < 127u; ++i) s.wram[0x301u + i] = 0x0Au;
  s.wram[0x380u] = 0x00u;
  m.restore(s);
  m.run(262ull * kLine + 262ull * kLine + 8u);
  ASSERT_EQ(m.state().vpos, 262u);
  SnesState marked = m.state();
  marked.ppu.cgadd = 0x44u;
  m.restore(marked);
  m.run(kLine);
  EXPECT_EQ(m.state().ppu.cgadd, 0x44u);
}

TEST(SnesPpuBeam, AnInterlacedPalFramePairTakesBothIrregularLines) {
  // PAL's odd field lengthens line 311 and its even field takes the extra line, so a
  // pair is 312 lines with one long among them and then 313 whole ones.
  Snes m = haltedAt(Placement{.region = Region::Pal, .setini = 0x01u});
  m.run(311ull * kLine + kLongLine + 313ull * kLine);
  EXPECT_EQ(m.state().vpos, 0u);
  // 625 lines are 625 pauses of forty; 625 * 40 is four past a multiple of six, so the
  // grid past them stands four cycles on from the budget's and the pair's own last
  // cycle is 852504, four behind where the run stops.
  EXPECT_EQ(m.state().hpos, 4u);
}

TEST(SnesPpuBeam, AnInterlacedPalEvenFrameReachesLineThreeHundredTwelve) {
  Snes m = haltedAt(Placement{.region = Region::Pal, .setini = 0x01u});
  m.run(311ull * kLine + kLongLine + 312ull * kLine + 8u);
  EXPECT_EQ(m.state().vpos, 312u);
}

TEST(SnesPpuBeam, WithoutInterlaceTheFramePairKeepsItsShortLine) {
  // The pins above measured against the shape the machine has without interlace: the
  // odd frame shortens line 240 and neither frame gains a line.
  Snes m = haltedAt(Placement{});
  m.run(261ull * kLine + kShortLine + 262ull * kLine);
  EXPECT_EQ(m.state().vpos, 0u);
  // 524 lines are 524 pauses; 524 * 40 is two past a multiple of six, so the pair's
  // own last cycle is 714732, two behind where the run stops.
  EXPECT_EQ(m.state().hpos, 2u);
}

// ---- the sprite table's reload --------------------------------------------------

TEST(SnesPpuBeam, TheOamAddressReloadsAtHTenOfVblanksFirstLine) {
  Snes m = haltedAt(Placement{});
  SnesState s = m.state();
  s.ppu.oamadd = 0x0010u;    // the reload value; doubled, the address becomes $20
  s.ppu.oamAddress = 0x0022u;
  m.restore(s);
  m.run(kVblankStartLine * kLine + 36u);
  EXPECT_EQ(m.state().vpos, kVblankStartLine);
  EXPECT_EQ(m.state().ppu.oamAddress, 0x0022u) << "before H = 10 the port is where the program left it";
  m.run(6u);                 // master 42, past the reload at 40
  EXPECT_EQ(m.state().ppu.oamAddress, 0x0020u);
}

TEST(SnesPpuBeam, TheOamReloadFollowsOverscansStartLine) {
  Snes m = haltedAt(Placement{.setini = 0x04u});
  SnesState s = m.state();
  s.ppu.oamadd = 0x0010u;
  s.ppu.oamAddress = 0x0022u;
  m.restore(s);
  m.run(kVblankStartLine * kLine + kOamReload + 2u);
  EXPECT_EQ(m.state().ppu.oamAddress, 0x0022u) << "line 225 is picture under overscan";
  m.run((kOverscanVblankStartLine - kVblankStartLine) * kLine);
  EXPECT_EQ(m.state().vpos, kOverscanVblankStartLine);
  EXPECT_EQ(m.state().ppu.oamAddress, 0x0020u);
}

// ---- the overflow flags --------------------------------------------------------

TEST(SnesPpuBeam, TheOverflowFlagsClearAsTheFrameStarts) {
  Snes m = haltedAt(Placement{.vpos = 261u, .hpos = 1358u});
  SnesState s = m.state();
  s.ppu.rangeOver = true;
  s.ppu.timeOver = true;
  m.restore(s);
  m.run(6u);  // across the frame's wrap
  ASSERT_EQ(m.state().vpos, 0u);
  EXPECT_FALSE(m.state().ppu.rangeOver);
  EXPECT_FALSE(m.state().ppu.timeOver);
}

TEST(SnesPpuBeam, TheOverflowFlagsSurviveAFrameOfForcedBlank) {
  Snes m = haltedAt(Placement{.vpos = 261u, .hpos = 1358u, .inidisp = 0x80u});
  SnesState s = m.state();
  s.ppu.rangeOver = true;
  s.ppu.timeOver = true;
  m.restore(s);
  m.run(6u);
  ASSERT_EQ(m.state().vpos, 0u);
  EXPECT_TRUE(m.state().ppu.rangeOver);
  EXPECT_TRUE(m.state().ppu.timeOver);
}

// ---- the H/V timer -------------------------------------------------------------

TEST(SnesPpuBeam, TheHorizontalTimerFiresFourteenCyclesPastFourTimesHtime) {
  Snes m = haltedAt(Placement{.nmitimen = 0x10u, .htime = 100u});
  m.run(408u);
  EXPECT_FALSE(m.state().timeup);
  m.run(6u);  // master 414 = 14 + 4 * 100
  EXPECT_TRUE(m.state().timeup);
}

TEST(SnesPpuBeam, TheVerticalTimerFiresTenCyclesIntoItsLine) {
  // With no H position to compare, the point is 1374 master cycles after the previous
  // line began — ten into a line that follows a normal one.
  Snes m = haltedAt(Placement{.nmitimen = 0x20u, .vtime = kPictureLine});
  m.run(kPictureLine * kLine + 4u);
  EXPECT_EQ(m.state().vpos, kPictureLine);
  EXPECT_FALSE(m.state().timeup);
  m.run(6u);  // master 14, the first cycle to reach the point at 10
  EXPECT_TRUE(m.state().timeup);
}

TEST(SnesPpuBeam, TheBothCompareTimerWithNoHPositionUsesTheSamePoint) {
  Snes m = haltedAt(Placement{.nmitimen = 0x30u, .htime = 0u, .vtime = kPictureLine});
  m.run(kPictureLine * kLine + 4u);
  EXPECT_FALSE(m.state().timeup);
  m.run(6u);
  EXPECT_TRUE(m.state().timeup);
}

TEST(SnesPpuBeam, TheTimersHZeroPointFollowsTheShortLinesLength) {
  // 1374 cycles after a 1360-cycle line began is fourteen into the line that follows
  // it, not the ten a line after a normal one gets. A halted core cannot tell the two
  // apart — its six-cycle grid steps over both at once — so this runs a live core whose
  // opcode fetch lands on master 12: past ten, short of fourteen, and the flag is down.
  const std::uint64_t lineStart = 240ull * kLine + kShortLine;
  Snes m(SnesConfig{.rom = cartridge(std::vector<std::uint8_t>(8u, kNop))});
  SnesState s = m.state();
  s.master = lineStart + 4u;
  s.consumed = s.master;
  s.hpos = 4u;
  s.vpos = 241u;
  s.field = 1u;
  s.previousLineMaster = static_cast<std::uint16_t>(kShortLine);
  s.nmitimen = 0x20u;  // mode 2: V = V, with no H to compare
  s.vtime = 241u;
  m.restore(s);

  m.run(8u);  // the opcode fetch, master 4 to 12
  ASSERT_EQ(m.state().hpos, 12u);
  EXPECT_FALSE(m.state().timeup);
  m.run(6u);  // its internal cycle, past fourteen
  EXPECT_TRUE(m.state().timeup);
}

TEST(SnesPpuBeam, EnablingTheTimerInTheCycleItsPointIsCrossedRaisesTheFlag) {
  // The crossing is noted as the cycle ticks and the flag is set at the cycle's end
  // under the mode the cycle leaves behind, so a write that arms the timer in that
  // very cycle is in time.
  Placement at{.htime = 100u};
  at.hpos = 370u;  // the write resolves at 416, its cycle spanning the point at 414
  Snes m = placed(writeProgram(0x4200u, 0x10u), at);
  EXPECT_TRUE(m.state().timeup);
}

TEST(SnesPpuBeam, ATimerCrossingDisarmedInItsOwnCycleDoesNotFireLater) {
  Placement at{.nmitimen = 0x10u, .htime = 100u};
  at.hpos = 370u;
  std::vector<std::uint8_t> program = writeProgram(0x4200u, 0x00u);
  program.pop_back();
  const std::vector<std::uint8_t> rearm = writeProgram(0x4200u, 0x10u);
  program.insert(program.end(), rearm.begin(), rearm.end());
  Snes m = placed(std::move(program), at);
  EXPECT_FALSE(m.state().timeup);
}

// ---- the memory refresh --------------------------------------------------------

TEST(SnesPpuBeam, TheRefreshHoldsTheCpuOffTheBusForFortyMasterCyclesALine) {
  // Forty NOPs are 560 master cycles of their own and cross the first line's refresh
  // point at 538, so they cost 600.
  std::vector<std::uint8_t> program(40u, kNop);
  program.push_back(kStp);
  Snes m(SnesConfig{.rom = cartridge(std::move(program))});
  std::uint64_t spent = 0;
  for (int i = 0; i < 40; ++i) spent += m.step();
  EXPECT_EQ(spent, 40ull * kNopCost + 40ull);
}

TEST(SnesPpuBeam, TheRefreshIsNeitherAnAccessNorAnInternalCycle) {
  std::vector<std::uint8_t> program(40u, kNop);
  program.push_back(kStp);
  CycleCount counted;
  Snes m(SnesConfig{.rom = cartridge(std::move(program))});
  m.setObserver(&counted);
  for (int i = 0; i < 40; ++i) m.step();
  EXPECT_EQ(counted.cycles, 80u);  // forty opcode fetches and forty internal cycles
}

TEST(SnesPpuBeam, NoCounterReadLandsInsideTheRefreshsWindow) {
  // The pause runs from master 538 to 578, so the first read after it latches dot 144
  // or later and the dots the window covers are never seen.
  std::vector<std::uint8_t> program(37u, kNop);
  program.push_back(kLdaAbs);
  program.push_back(0x37u);
  program.push_back(0x21u);
  program.push_back(kStp);
  Snes m(SnesConfig{.rom = cartridge(std::move(program))});
  while (m.state().cpu.run == CpuRunState::Running) m.step();
  EXPECT_GE(m.state().ppu.ophct, 144u);
}

TEST(SnesPpuBeam, TheRefreshPointWalksAnEightCycleGrid) {
  // The first pause is 538 into line 0 and each one after it is the point on the
  // eight-cycle grid nearest 536 into its own line.
  Snes m = haltedAt(Placement{});
  EXPECT_EQ(m.state().refreshAt, kFirstRefresh);
  m.run(kLine);
  ASSERT_EQ(m.state().vpos, 1u);
  EXPECT_EQ(m.state().refreshAt, kLine + 534u);
  m.run(kLine);
  ASSERT_EQ(m.state().vpos, 2u);
  EXPECT_EQ(m.state().refreshAt, 2ull * kLine + 538u);
}

TEST(SnesPpuBeam, TheShortLineRePhasesTheRefreshPoint) {
  // Line 240 is four cycles short, so the grid comes up in the same place two lines
  // running instead of alternating.
  Snes m = haltedAt(Placement{});
  m.run(240u * kLine);
  ASSERT_EQ(m.state().vpos, 240u);
  EXPECT_EQ(m.state().refreshAt, 240ull * kLine + 538u);
  m.run(kShortLine);
  ASSERT_EQ(m.state().vpos, 241u);
  EXPECT_EQ(m.state().refreshAt, 240ull * kLine + kShortLine + 538u);
}

TEST(SnesPpuBeam, AHaltedCoreIsPausedToo) {
  // The pause holds the core whatever it is doing — anomie-timing.txt 68 and
  // fullsnes.txt 27047 describe the CPU paused, and neither exempts a halted one.
  // Reading `refreshLeft` right after the point is what says the pause began: a pause
  // cycle costs the same six as an idle one, so the position alone would not.
  Snes m = haltedAt(Placement{});
  m.run(540u);  // past the first line's point at 538
  EXPECT_EQ(m.state().refreshLeft, kRefreshMaster);
  m.run(9u * kLine - 540u);
  EXPECT_EQ(m.state().vpos, 9u);
  // Nine lines are nine pauses of forty, and forty is four past a multiple of six, so
  // the grid past them stands 9 * 4 = 36 cycles on from the budget's — itself a
  // multiple of six, which is why this budget still lands on the line's first cycle.
  EXPECT_EQ(m.state().hpos, 0u);
  EXPECT_EQ(m.state().refreshLeft, 0u);
  EXPECT_EQ(m.state().refreshAt, 9ull * kLine + 534u);
}

TEST(SnesPpuBeam, AStoppedCoreSpendsThePauseLikeAnyOther) {
  // The pause is spent a fast cycle at a time for a stopped core as for a running one,
  // so a run still stops within one cycle of its budget and carries the rest as state.
  Snes m = haltedAt(Placement{});
  m.run(540u);
  EXPECT_NE(m.state().refreshLeft, 0u);
  EXPECT_GE(m.state().master, m.state().consumed);
  EXPECT_LT(m.state().master, m.state().consumed + 12u);
  m.run(20u);  // inside the pause: four of its six-cycle steps are spent, sixteen owed
  EXPECT_EQ(m.state().master, 564u);
  EXPECT_EQ(m.state().refreshLeft, 16u);
  m.run(3u * kLine - 560u);
  // Three pauses are 120 cycles, a whole multiple of six, so the grid returns to the
  // budget's and the total is exact.
  EXPECT_EQ(m.state().master, 3ull * kLine);
  EXPECT_EQ(m.state().vpos, 3u);
  EXPECT_EQ(m.state().hpos, 0u);
}

TEST(SnesPpuBeam, AWaitReleasedInsideAPauseWakesWhenThePauseEnds) {
  // A wait is released by the interrupt line the core samples at each idle cycle, and
  // a paused core makes none. With the H timer's point at 542 — fourteen past four
  // times 132 — the flag rises inside line 0's pause, which runs from the cycle
  // boundary at 540 to 580, and the core sees it at the first cycle after that.
  Snes m = waitingAt(Placement{.nmitimen = 0x10u, .htime = 132u});
  ASSERT_EQ(m.state().cpu.run, CpuRunState::Waiting);

  m.run(560u);  // master 564, sixteen cycles of the pause still owed
  EXPECT_NE(m.state().refreshLeft, 0u);
  EXPECT_TRUE(m.state().timeup) << "the point at 542 is passed inside the pause";
  EXPECT_EQ(m.state().cpu.run, CpuRunState::Waiting) << "the pause holds the wait";

  m.run(40u);  // master 604, past the pause's end at 580
  EXPECT_EQ(m.state().refreshLeft, 0u);
  EXPECT_NE(m.state().cpu.run, CpuRunState::Waiting);
  while (m.state().cpu.run != CpuRunState::Stopped) m.step();
  EXPECT_EQ(m.state().wram[0x50], 0x5Au) << "the wait vectors to its handler";

  // The control: the same machine with its point at 414, which no pause covers, wakes
  // on the cycle after the flag rises.
  Snes clear = waitingAt(Placement{.nmitimen = 0x10u, .htime = 100u});
  clear.run(480u);
  EXPECT_TRUE(clear.state().timeup);
  EXPECT_NE(clear.state().cpu.run, CpuRunState::Waiting);
}

TEST(SnesPpuBeam, RunMayStopInsideARefreshAndKeepsItsOvershootBound) {
  Snes m = spinning();
  m.run(540u);  // the cycle that reaches the point finishes, and the pause is owed
  EXPECT_NE(m.state().refreshLeft, 0u);
  EXPECT_GE(m.state().master, 540u);
  EXPECT_LT(m.state().master, 540u + 12u);

  // A budget that ends inside the pause itself: it is spent a fast cycle at a time, so
  // the run stops within one of the budget and carries the rest of the pause.
  m.run(20u);
  EXPECT_NE(m.state().refreshLeft, 0u);
  EXPECT_GE(m.state().master, m.state().consumed);
  EXPECT_LT(m.state().master, m.state().consumed + 12u);
}

TEST(SnesPpuBeam, ASnapshotTakenInsideARefreshFinishesItExactly) {
  Snes reference = spinning();
  reference.run(3000u);

  Snes split = spinning();
  split.run(540u);
  ASSERT_NE(split.state().refreshLeft, 0u);
  const SnesState mid = split.state();
  auto resumed = std::make_unique<Snes>(SnesConfig{.rom = cartridge({0x80u, 0xFEu})});
  resumed->restore(mid);
  resumed->run(3000u - 540u);

  EXPECT_EQ(resumed->state().master, reference.state().master);
  EXPECT_EQ(resumed->state().hpos, reference.state().hpos);
  EXPECT_EQ(resumed->state().vpos, reference.state().vpos);
  EXPECT_EQ(resumed->state().refreshAt, reference.state().refreshAt);
  EXPECT_EQ(resumed->state().refreshLeft, reference.state().refreshLeft);
  EXPECT_EQ(resumed->state().cpu.pc, reference.state().cpu.pc);
}

// ---- snapshot and restore ------------------------------------------------------

TEST(SnesPpuBeam, ASnapshotCarriesVerticalBlanksLatchedFactAndTheFrameParity) {
  Snes m = haltedAt(Placement{});
  m.run(kVblankStartLine * kLine + 600u);
  ASSERT_TRUE(m.state().inVblank);
  const std::uint8_t field = m.state().field;

  const SnesState snap = m.state();
  auto restored = std::make_unique<Snes>(SnesConfig{.rom = cartridge({kStp})});
  restored->restore(snap);
  EXPECT_TRUE(restored->state().inVblank);
  EXPECT_EQ(restored->state().field, field);
  EXPECT_EQ(restored->state().vblankBeginLine, kVblankStartLine);
}

}  // namespace
}  // namespace snaggletooth
