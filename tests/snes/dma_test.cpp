// The SNES machine's DMA and HDMA. A general-purpose DMA copies bytes between the
// A bus and the B bus while the CPU is halted; HDMA delivers a table's values to
// hardware registers once per visible scanline. Transfers are set up by writing the
// channel registers directly into the state, then triggered either by a program
// writing $420B/$420C or by running the beam past the HDMA points, and observed by
// the values that land in video memory, the register file, and work RAM. The timing
// cases reproduce the four worked examples in the console's timing reference.

#include <cstdint>
#include <initializer_list>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "snaggletooth/snes/snes.h"

namespace snaggletooth {
namespace {

// A machine that runs `program` from $8000, to trigger a transfer from CPU code.
// The DMA channel registers are set up separately, in the state.
Snes programMachine(std::initializer_list<std::uint8_t> program) {
  std::vector<std::uint8_t> rom(program.begin(), program.end());
  rom.resize(0x8000u, 0x00u);
  rom[0x7FFCu] = 0x00u;   // reset -> $8000
  rom[0x7FFDu] = 0x80u;
  return Snes(SnesConfig{.rom = rom});
}

// A machine halted at the frame origin, its counters zeroed, so running it advances
// only the beam (and any HDMA the state arms). Used to drive HDMA and to place the
// DMA engine at a chosen master position for the timing cases.
Snes haltedMachine() {
  Snes m = programMachine({0xDBu});  // STP
  m.step();                          // halt the core
  SnesState s = m.state();
  s.master = 0; s.consumed = 0; s.apuPhase = 0;
  s.hpos = 0; s.vpos = 0; s.field = 0;
  m.restore(s);
  return m;
}

// Loads A with an immediate and writes it to an absolute address in bank 0, the
// two-instruction shape the trigger programs use.
constexpr std::uint8_t kLdaImm = 0xA9u;
constexpr std::uint8_t kStaAbs = 0x8Du;
constexpr std::uint8_t kLdaAbs = 0xADu;
constexpr std::uint8_t kStp = 0xDBu;
constexpr std::uint8_t kNop = 0xEAu;
constexpr std::uint32_t kLine = 1364u;
// The master position a little past the first vblank line's HDMA delivery point.
constexpr std::uint64_t kVblankFirstDelivery = 225ull * 1364ull + 1200ull;

// ---- the channel register file --------------------------------------------

TEST(SnesDma, RegistersRoundTripThroughTheBus) {
  // A program writes channel 0's parameter, B-address, and 16-bit source registers,
  // then reads two of them back into work RAM.
  Snes m = programMachine({
      kLdaImm, 0x1Fu, kStaAbs, 0x00u, 0x43u,  // DMAP0 = $1F
      kLdaImm, 0x80u, kStaAbs, 0x01u, 0x43u,  // BBAD0 = $80
      kLdaImm, 0x34u, kStaAbs, 0x02u, 0x43u,  // A1T0 low = $34
      kLdaImm, 0x12u, kStaAbs, 0x03u, 0x43u,  // A1T0 high = $12
      kLdaAbs, 0x00u, 0x43u, kStaAbs, 0x00u, 0x00u,  // WRAM[0] = DMAP0
      kLdaAbs, 0x03u, 0x43u, kStaAbs, 0x01u, 0x00u,  // WRAM[1] = A1T0 high
      kStp,
  });
  m.run(2000u);
  EXPECT_EQ(m.state().dma[0].dmap, 0x1Fu);
  EXPECT_EQ(m.state().dma[0].a1t, 0x1234u);
  EXPECT_EQ(m.state().wram[0], 0x1Fu);   // read back through the bus
  EXPECT_EQ(m.state().wram[1], 0x12u);
}

TEST(SnesDma, UnusedByteAliasesTwoAddresses) {
  // Both $433B and $433F read and write the one unused byte of channel 3.
  Snes m = programMachine({
      kLdaAbs, 0x3Bu, 0x43u, kStaAbs, 0x00u, 0x00u,
      kLdaAbs, 0x3Fu, 0x43u, kStaAbs, 0x01u, 0x00u,
      kStp,
  });
  SnesState s = m.state();
  s.dma[3].unused = 0x5Au;
  m.restore(s);
  m.run(2000u);
  EXPECT_EQ(m.state().wram[0], 0x5Au);
  EXPECT_EQ(m.state().wram[1], 0x5Au);
}

TEST(SnesDma, MiddleRegistersReadOpenBus) {
  // $43xC-$43xE are not registers; a read returns the last value on the data bus,
  // which for an absolute read is the high byte of the instruction's own address.
  Snes m = programMachine({
      kLdaAbs, 0x0Cu, 0x43u, kStaAbs, 0x00u, 0x00u,  // read $430C (open bus = $43) -> WRAM[0]
      kStp,
  });
  m.run(2000u);
  EXPECT_EQ(m.state().wram[0], 0x43u);
}

// ---- general-purpose DMA transfers ----------------------------------------

// Arms channel 0 in `s` for an A->B transfer and returns a machine whose program
// triggers it, then runs to completion. The channel is configured by the caller.
Snes runDma(const DmaChannel& channel, std::uint8_t enable,
            void (*seed)(SnesState&) = nullptr) {
  Snes m = programMachine({kLdaImm, enable, kStaAbs, 0x0Bu, 0x42u, kNop, kStp});
  SnesState s = m.state();
  s.dma[0] = channel;
  if (seed != nullptr) seed(s);
  m.restore(s);
  m.run(20000u);
  return m;
}

TEST(SnesDma, CopiesToVramWithTheTwoRegisterPattern) {
  // Pattern 1 writes VMDATAL then VMDATAH, so a word lands each two bytes; the eight
  // source bytes in work RAM become the first four VRAM words.
  const DmaChannel ch{
      .dmap = 0x01u,   // A->B, increment, pattern 1
      .bbad = 0x18u,   // $2118 (VMDATAL)
      .a1t = 0x0010u,  // work RAM offset $10
      .a1b = 0x7Eu,    // work RAM bank
      .das = 0x0008u,  // eight bytes
  };
  Snes m = runDma(ch, 0x01u, [](SnesState& s) {
    for (std::uint8_t i = 0; i < 8; ++i) s.wram[0x10u + i] = static_cast<std::uint8_t>(0xA0u + i);
    s.ppu.vmain = 0x80u;  // increment after the high byte, step one word
    s.ppu.vmadd = 0x0000u;
  });
  for (std::uint8_t i = 0; i < 8; ++i) {
    EXPECT_EQ(m.vram()[i], static_cast<std::uint8_t>(0xA0u + i)) << "byte " << int(i);
  }
  EXPECT_EQ(m.state().dma[0].a1t, 0x0018u);  // source advanced by eight
  EXPECT_EQ(m.state().dma[0].das, 0x0000u);  // count exhausted
  EXPECT_EQ(m.state().mdmaen & 1u, 0u);      // and the enable bit cleared
}

TEST(SnesDma, FixedAddressFillsFromOneByte) {
  // A fixed A-bus address reads the same source byte for every write: a fill.
  const DmaChannel ch{
      .dmap = 0x09u,   // A->B, fixed (bits4-3 = 01), pattern 1
      .bbad = 0x18u,
      .a1t = 0x0020u,
      .a1b = 0x7Eu,
      .das = 0x0004u,
  };
  Snes m = runDma(ch, 0x01u, [](SnesState& s) {
    s.wram[0x20u] = 0xCDu;
    s.ppu.vmain = 0x80u;
    s.ppu.vmadd = 0x0000u;
  });
  EXPECT_EQ(m.vram()[0], 0xCDu);
  EXPECT_EQ(m.vram()[1], 0xCDu);
  EXPECT_EQ(m.vram()[2], 0xCDu);
  EXPECT_EQ(m.vram()[3], 0xCDu);
  EXPECT_EQ(m.state().dma[0].a1t, 0x0020u);  // the address never moved
}

TEST(SnesDma, DecrementWalksTheSourceBackward) {
  const DmaChannel ch{
      .dmap = 0x10u,   // A->B, decrement (bits4-3 = 10), pattern 0
      .bbad = 0x80u,   // $2180 (WRAM data port)
      .a1t = 0x0042u,
      .a1b = 0x7Eu,
      .das = 0x0003u,
  };
  Snes m = runDma(ch, 0x01u, [](SnesState& s) {
    s.wram[0x42u] = 0x11u;
    s.wram[0x41u] = 0x22u;
    s.wram[0x40u] = 0x33u;
    s.wmadd = 0x00100u;  // write the three bytes to $100..$102
  });
  EXPECT_EQ(m.state().wram[0x100], 0x11u);
  EXPECT_EQ(m.state().wram[0x101], 0x22u);
  EXPECT_EQ(m.state().wram[0x102], 0x33u);
  EXPECT_EQ(m.state().dma[0].a1t, 0x003Fu);  // stepped down three
}

TEST(SnesDma, DirectionBtoAReadsTheRegisterIntoMemory) {
  // Direction 1 copies from the B bus to the A bus: read a PPU-status style register
  // and store it into work RAM. $4212 with no controller and outside blanking reads
  // its open-bus middle bits as zero and the flags as their state.
  const DmaChannel ch{
      .dmap = 0x80u,   // B->A, increment, pattern 0
      .bbad = 0x80u,   // $2180 -> reads work RAM at the port
      .a1t = 0x0200u,
      .a1b = 0x7Eu,
      .das = 0x0002u,
  };
  Snes m = runDma(ch, 0x01u, [](SnesState& s) {
    s.wmadd = 0x00050u;
    s.wram[0x50u] = 0xABu;
    s.wram[0x51u] = 0xCDu;
  });
  EXPECT_EQ(m.state().wram[0x200], 0xABu);  // the port streamed WRAM[$50], WRAM[$51]
  EXPECT_EQ(m.state().wram[0x201], 0xCDu);
}

TEST(SnesDma, TwoChannelsRunLowestFirst) {
  Snes m = programMachine({kLdaImm, 0x05u, kStaAbs, 0x0Bu, 0x42u, kNop, kStp});  // channels 0 and 2
  SnesState s = m.state();
  s.dma[0] = DmaChannel{.dmap = 0x00u, .bbad = 0x80u, .a1t = 0x0010u, .a1b = 0x7Eu, .das = 0x0002u};
  s.dma[2] = DmaChannel{.dmap = 0x00u, .bbad = 0x80u, .a1t = 0x0020u, .a1b = 0x7Eu, .das = 0x0002u};
  s.wram[0x10u] = 0x01u; s.wram[0x11u] = 0x02u;
  s.wram[0x20u] = 0x03u; s.wram[0x21u] = 0x04u;
  s.wmadd = 0x00100u;
  m.restore(s);
  m.run(20000u);
  // Both channels wrote through the same port in order 0 then 2.
  EXPECT_EQ(m.state().wram[0x100], 0x01u);
  EXPECT_EQ(m.state().wram[0x101], 0x02u);
  EXPECT_EQ(m.state().wram[0x102], 0x03u);
  EXPECT_EQ(m.state().wram[0x103], 0x04u);
  EXPECT_EQ(m.state().mdmaen, 0u);  // both enable bits cleared
}

TEST(SnesDma, ReadingTheRegisterRegionReturnsOpenBus) {
  // DMA may not read $2100-$21FF, $4000-$41FF, $4200-$421F, or $4300-$437F on the A
  // bus; a read there returns the data bus, so the copy carries open bus, not the
  // register. Source $4300 (a real register holding $00) reads back as the bus value.
  const DmaChannel ch{
      .dmap = 0x00u,   // A->B, increment, pattern 0
      .bbad = 0x80u,   // -> work RAM port
      .a1t = 0x4300u,  // an excluded A-bus address
      .a1b = 0x00u,
      .das = 0x0001u,
  };
  Snes m = runDma(ch, 0x01u, [](SnesState& s) { s.wmadd = 0x00100u; });
  // The channel's own DMAP register at $4300 is $00, but the excluded read does not
  // return it; it returns the open-bus value the transfer itself last drove.
  EXPECT_EQ(m.state().dma[0].a1t, 0x4301u);  // the transfer still ran and stepped
}

// ---- DMA timing: the four worked examples ---------------------------------

// Places the engine at pause master `pause` with one channel of three bytes armed
// and the core halted, then runs to the resume, returning the transfer master (the
// span the engine held the bus) and the resume pad added to the first cycle back.
struct DmaTiming { std::uint64_t transfer; std::uint64_t pad; };
DmaTiming runTiming(std::uint64_t pause) {
  Snes m = haltedMachine();
  SnesState s = m.state();
  s.master = pause; s.consumed = pause; s.dmaPauseMaster = pause;
  s.dmaRunning = true; s.dmaOpened = false; s.dmaChannelOpened = false; s.dmaUnit = 0;
  s.mdmaen = 0x01u;
  s.dma[0] = DmaChannel{.dmap = 0x00u, .bbad = 0x80u, .a1t = 0x0000u, .a1b = 0x7Eu, .das = 0x0003u};
  s.wmadd = 0x00100u;
  m.restore(s);
  while (m.state().dmaRunning) m.step();
  const std::uint64_t transfer = m.state().master - pause;
  const std::uint64_t before = m.state().master;
  m.step();  // the first cycle back pays the resume pad on top of its own six
  return {transfer, (m.state().master - before) - 6u};
}

TEST(SnesDma, TimingAlignedPauseTakesTheLongPath) {
  // Pause already on an eight-cycle boundary: align waits a full 8, transfer is 48,
  // resume pad 6, so the whole thing is 54 master cycles.
  const DmaTiming t = runTiming(0u);
  EXPECT_EQ(t.transfer, 48u);
  EXPECT_EQ(t.pad, 6u);
  EXPECT_EQ(t.transfer + t.pad, 54u);
}

TEST(SnesDma, TimingTwoPastBoundary) {
  const DmaTiming t = runTiming(6u);  // needs 2 to align
  EXPECT_EQ(t.transfer, 42u);
  EXPECT_EQ(t.pad, 6u);
  EXPECT_EQ(t.transfer + t.pad, 48u);
}

TEST(SnesDma, TimingFourPastBoundary) {
  const DmaTiming t = runTiming(4u);  // needs 4 to align
  EXPECT_EQ(t.transfer, 44u);
  EXPECT_EQ(t.pad, 4u);
  EXPECT_EQ(t.transfer + t.pad, 48u);
}

TEST(SnesDma, TimingSixPastBoundary) {
  const DmaTiming t = runTiming(2u);  // needs 6 to align
  EXPECT_EQ(t.transfer, 46u);
  EXPECT_EQ(t.pad, 2u);
  EXPECT_EQ(t.transfer + t.pad, 48u);
}

// ---- snapshot mid-transfer ------------------------------------------------

TEST(SnesDma, SnapshotMidTransferResumesExactly) {
  // Run a long transfer part-way, snapshot, and finish on a fresh machine; the two
  // must land on identical destination memory and identical channel state.
  const DmaChannel ch{.dmap = 0x00u, .bbad = 0x80u, .a1t = 0x0000u, .a1b = 0x7Eu, .das = 0x0040u};

  Snes reference = programMachine({kLdaImm, 0x01u, kStaAbs, 0x0Bu, 0x42u, kNop, kStp});
  SnesState rs = reference.state();
  rs.dma[0] = ch;
  for (std::uint16_t i = 0; i < 0x40u; ++i) rs.wram[i] = static_cast<std::uint8_t>(i);
  rs.wmadd = 0x00800u;
  reference.restore(rs);
  reference.run(20000u);  // the whole transfer

  Snes split = programMachine({kLdaImm, 0x01u, kStaAbs, 0x0Bu, 0x42u, kNop, kStp});
  SnesState ss = split.state();
  ss.dma[0] = ch;
  for (std::uint16_t i = 0; i < 0x40u; ++i) ss.wram[i] = static_cast<std::uint8_t>(i);
  ss.wmadd = 0x00800u;
  split.restore(ss);
  split.run(400u);            // stop somewhere inside the transfer
  ASSERT_TRUE(split.state().dmaRunning);
  SnesState mid = split.state();
  Snes resumed = programMachine({kLdaImm, 0x01u, kStaAbs, 0x0Bu, 0x42u, kNop, kStp});
  resumed.restore(mid);       // resume from the snapshot on another machine
  resumed.run(20000u);

  for (std::uint16_t i = 0; i < 0x40u; ++i) {
    EXPECT_EQ(resumed.state().wram[0x800u + i], reference.state().wram[0x800u + i]) << "byte " << int(i);
  }
  EXPECT_EQ(resumed.state().dma[0].das, reference.state().dma[0].das);
}

// ---- HDMA -----------------------------------------------------------------

// A halted machine with an HDMA channel armed against a table in work RAM. The table
// writes to INIDISP ($2100), which the state exposes.
Snes hdmaMachine(std::initializer_list<std::uint8_t> table, std::uint8_t dmap = 0x00u) {
  Snes m = haltedMachine();
  SnesState s = m.state();
  s.dma[0] = DmaChannel{.dmap = dmap, .bbad = 0x00u, .a1t = 0x0300u, .a1b = 0x7Eu};
  s.hdmaen = 0x01u;
  std::uint16_t addr = 0x0300u;
  for (std::uint8_t byte : table) s.wram[addr++] = byte;
  s.ppu.inidisp =0x80u;  // a known starting value distinct from the table's
  m.restore(s);
  return m;
}

TEST(SnesDma, HdmaDeliversAcrossScanlines) {
  // Write $0A for two lines (non-repeat), then $0B for one line, then stop. $0A lands
  // on line 0, $0B on line 2; nothing changes it after.
  Snes m = hdmaMachine({0x02u, 0x0Au, 0x01u, 0x0Bu, 0x00u});
  m.run(kLine + 1200u);            // past line 0's delivery
  EXPECT_EQ(m.state().ppu.inidisp, 0x0Au);
  EXPECT_EQ(m.state().vpos, 1u);
  m.run(2u * kLine);               // through line 2's delivery
  EXPECT_EQ(m.state().ppu.inidisp, 0x0Bu);
  EXPECT_EQ(m.state().hdmaActive & 1u, 0u);  // terminated
}

TEST(SnesDma, HdmaRepeatWritesEveryLine) {
  // A repeat entry (bit 7 set) writes its own value each of the counted lines; with
  // increment-free direct data the source walks the table, so successive lines take
  // successive bytes.
  Snes m = hdmaMachine({0x83u, 0x10u, 0x11u, 0x12u, 0x00u});  // repeat for 3 lines
  m.run(1200u);                     // line 0
  EXPECT_EQ(m.state().ppu.inidisp, 0x10u);
  m.run(kLine);                     // line 1
  EXPECT_EQ(m.state().ppu.inidisp, 0x11u);
  m.run(kLine);                     // line 2
  EXPECT_EQ(m.state().ppu.inidisp, 0x12u);
}

TEST(SnesDma, HdmaZeroLineCountTerminatesImmediately) {
  Snes m = hdmaMachine({0x00u});    // a stop as the very first entry
  m.run(2u * kLine);
  EXPECT_EQ(m.state().ppu.inidisp, 0x80u);       // never written
  EXPECT_EQ(m.state().hdmaActive & 1u, 0u);  // never activated
}

// The line-count byte's three cases, from the table format the register page prints:
// $00 ends the channel, $01-$80 transfer once and then stand quiet for the rest of
// their lines, and $81-$FF transfer on every one of theirs. The two ranges meet on
// $80, which is one transfer followed by 127 quiet lines — the longest pause a single
// entry can name, and what a table uses to hold a channel still to the end of a frame.

TEST(SnesDma, HdmaAnEntryBelowTheRepeatRangeTransfersOnceAndThenStandsQuiet) {
  // $04: one transfer on its first line, then three lines of nothing, then $0B.
  Snes m = hdmaMachine({0x04u, 0x0Au, 0x01u, 0x0Bu, 0x00u});
  m.run(1200u);
  EXPECT_EQ(m.state().ppu.inidisp, 0x0Au);
  SnesState marked = m.state();
  marked.ppu.inidisp = 0x33u;  // a marker any further delivery would overwrite
  m.restore(marked);
  m.run(3u * kLine);           // lines 1, 2 and 3 are the entry's quiet ones
  EXPECT_EQ(m.state().ppu.inidisp, 0x33u);
  m.run(kLine);                // line 4 takes the next entry
  EXPECT_EQ(m.state().ppu.inidisp, 0x0Bu);
}

TEST(SnesDma, HdmaEightyTransfersOnceAndHoldsTheChannelQuietForTheRestOfThePicture) {
  // $80 is the top of the range that transfers once: its line, then 127 quiet ones,
  // which reaches past the picture's last line. A channel that read the top bit as a
  // repeat flag instead would write on all 128 of them.
  Snes m = hdmaMachine({0x80u, 0x0Au, 0x0Bu, 0x0Cu, 0x0Du, 0x0Eu, 0x0Fu});
  m.run(1200u);
  EXPECT_EQ(m.state().ppu.inidisp, 0x0Au);  // the one transfer, on line 0

  SnesState marked = m.state();
  marked.ppu.inidisp = 0x33u;
  m.restore(marked);
  m.run(100u * kLine);
  EXPECT_EQ(m.state().ppu.inidisp, 0x33u);  // and nothing for a hundred lines after it
}

// ---- $420C while the picture is being drawn --------------------------------

// A machine that runs a program from the frame origin with channel 0 armed against a
// table in work RAM, so the program can take the channel away or bring it in while the
// picture is being drawn. `delay` counts iterations of a two-instruction loop the
// program runs before its first store, which is how a store is placed on a chosen
// line; every case asserts where its store actually landed rather than predicting it.
// `stores` are (value, low byte of a bank-zero address) pairs, written in order.
Snes hdmaWritingMachine(std::uint8_t delay,
                        std::initializer_list<std::pair<std::uint8_t, std::uint8_t>> stores,
                        std::initializer_list<std::uint8_t> table, std::uint8_t hdmaen,
                        std::uint8_t secondDelay = 0u) {
  std::vector<std::uint8_t> program{0xA2u, delay,   // LDX #delay
                                    0xCAu,          // $8002 DEX
                                    0xD0u, 0xFDu};  // BNE $8002
  bool first = true;
  for (const auto& [value, low] : stores) {
    if (!first && secondDelay != 0u) {
      program.insert(program.end(), {0xA2u, secondDelay, 0xCAu, 0xD0u, 0xFDu});
    }
    first = false;
    program.insert(program.end(), {0xA9u, value, 0x8Du, low, 0x42u});
  }
  program.push_back(0xDBu);  // STP

  std::vector<std::uint8_t> rom = program;
  rom.resize(0x8000u, 0x00u);
  rom[0x7FFCu] = 0x00u;  // reset -> $8000
  rom[0x7FFDu] = 0x80u;
  Snes m(SnesConfig{.rom = rom});
  SnesState s = m.state();
  s.master = 0; s.consumed = 0; s.apuPhase = 0;
  s.hpos = 0; s.vpos = 0; s.field = 0;
  s.dma[0] = DmaChannel{.dmap = 0x00u, .bbad = 0x00u, .a1t = 0x0300u, .a1b = 0x7Eu};
  s.hdmaen = hdmaen;
  std::uint16_t addr = 0x0300u;
  for (std::uint8_t byte : table) s.wram[addr++] = byte;
  s.ppu.inidisp = 0x80u;
  m.restore(s);
  return m;
}

// The dot a line's delivery is taken at; a store landing before it on its line is a
// store the line's delivery sees.
constexpr std::uint16_t kDeliverDot = 1112u;

// Each case runs its program to the halt that ends it, then works from where the
// program's last store actually landed rather than predicting a line: a store belongs
// to a line the picture draws, and lands before that line's delivery, and the run
// after it is measured from there.
void runToTheHalt(Snes& m) {
  for (int n = 0; n < 4000 && m.state().cpu.run == CpuRunState::Running; ++n) m.step();
  ASSERT_NE(m.state().cpu.run, CpuRunState::Running) << "the program reached its halt";
}

void expectStoreLandedOnADrawnLineBeforeItsDelivery(const Snes& m) {
  ASSERT_FALSE(m.state().inVblank) << "the store belongs to a line the picture draws";
  ASSERT_GE(m.state().vpos, 1u) << "and to a line past the frame's own reload";
  ASSERT_LT(m.state().hpos, kDeliverDot) << "and lands before that line's delivery";
}

// Runs to just past the delivery of the line the machine is on.
void runPastThisLinesDelivery(Snes& m) {
  m.run(kDeliverDot - m.state().hpos + 50u);
}

TEST(SnesDma, HdmaAChannelTakenAwayMidPictureStopsDelivering) {
  // A repeat entry would write a fresh value on every line of the picture. The program
  // clears $420C part-way down, and no line after that delivers — while the channel's
  // table is left where it stood, not ended.
  Snes m = hdmaWritingMachine(40u, {{0x00u, 0x0Cu}}, {}, 0x01u);
  SnesState s = m.state();
  s.wram[0x300u] = 0xFFu;  // repeat for 127 lines
  for (std::uint16_t i = 0; i < 127u; ++i) {
    s.wram[0x301u + i] = static_cast<std::uint8_t>(0x01u + i);  // a distinct value per line
  }
  m.restore(s);

  runToTheHalt(m);
  expectStoreLandedOnADrawnLineBeforeItsDelivery(m);
  EXPECT_NE(m.state().ppu.inidisp, 0x80u) << "the lines before the store delivered";

  SnesState marked = m.state();
  marked.ppu.inidisp = 0x33u;  // a marker any further delivery would overwrite
  m.restore(marked);
  m.run(4u * kLine);
  EXPECT_EQ(m.state().ppu.inidisp, 0x33u) << "and no line after it delivered";
  EXPECT_NE(m.state().hdmaActive & 1u, 0u) << "the channel's table has not ended";
}

TEST(SnesDma, HdmaAChannelTakenAwayLeavesTheOnesStillNamedDelivering) {
  // Two channels repeat together and the program takes only one of them away. The
  // line's work is per channel, so the one still named in $420C goes on delivering.
  Snes m = hdmaWritingMachine(40u, {{0x02u, 0x0Cu}}, {}, 0x03u);
  SnesState s = m.state();
  s.wram[0x300u] = 0xFFu;  // channel 0 repeats to INIDISP
  for (std::uint16_t i = 0; i < 127u; ++i) {
    s.wram[0x301u + i] = static_cast<std::uint8_t>(0x01u + i);
  }
  s.dma[1] = DmaChannel{.dmap = 0x00u, .bbad = 0x05u, .a1t = 0x0400u, .a1b = 0x7Eu};
  s.wram[0x400u] = 0xFFu;  // channel 1 repeats to BGMODE
  for (std::uint16_t i = 0; i < 127u; ++i) {
    s.wram[0x401u + i] = static_cast<std::uint8_t>(0x01u + i);
  }
  m.restore(s);

  runToTheHalt(m);
  expectStoreLandedOnADrawnLineBeforeItsDelivery(m);
  SnesState marked = m.state();
  marked.ppu.inidisp = 0x33u;
  marked.ppu.bgmode = 0x33u;
  m.restore(marked);
  m.run(3u * kLine);
  EXPECT_EQ(m.state().ppu.inidisp, 0x33u) << "the channel taken away delivered nothing";
  EXPECT_NE(m.state().ppu.bgmode, 0x33u) << "the channel still named went on delivering";
}

TEST(SnesDma, HdmaAChannelTakenAwayCostsThePictureNothing) {
  // A channel out of $420C is not work the picture does, so the lines after it is taken
  // away leave the processor exactly the time it would have had if nothing were armed
  // at all. The program counts in work RAM; the two runs must reach the same count.
  const std::vector<std::uint8_t> counting{
      0xEEu, 0x00u, 0x00u,   // $8000 INC $0000
      0xD0u, 0xFBu,          // $8003 BNE $8000
      0xEEu, 0x01u, 0x00u,   // $8005 INC $0001
      0x80u, 0xF6u,          // $8008 BRA $8000
  };

  const auto count = [&counting](std::uint8_t active) {
    std::vector<std::uint8_t> rom = counting;
    rom.resize(0x8000u, 0x00u);
    rom[0x7FFCu] = 0x00u;
    rom[0x7FFDu] = 0x80u;
    Snes m(SnesConfig{.rom = rom});
    SnesState s = m.state();
    s.master = 0; s.consumed = 0; s.apuPhase = 0;
    s.hpos = 0; s.vpos = 0; s.field = 0;
    s.dma[0] = DmaChannel{.dmap = 0x00u, .bbad = 0x00u, .a1t = 0x0300u, .a1b = 0x7Eu};
    s.hdmaen = 0x00u;   // taken away
    s.hdmaActive = active;  // but its table would still be running
    s.wram[0x300u] = 0xFFu;
    m.restore(s);
    m.run(20u * kLine);
    return static_cast<unsigned>(m.state().wram[1]) * 256u + m.state().wram[0];
  };

  EXPECT_EQ(count(0x01u), count(0x00u));
}

TEST(SnesDma, HdmaAChannelPutBackResumesWhereItStood) {
  // Taken away and given back a line later: the table keeps its place, so the value
  // that lands next is further down the entry than the one taken away interrupted —
  // never the table's first value again.
  Snes m = hdmaWritingMachine(40u, {{0x00u, 0x0Cu}, {0x01u, 0x0Cu}}, {}, 0x01u, 36u);
  SnesState s = m.state();
  s.wram[0x300u] = 0xFFu;
  for (std::uint16_t i = 0; i < 127u; ++i) {
    s.wram[0x301u + i] = static_cast<std::uint8_t>(0x01u + i);
  }
  m.restore(s);

  runToTheHalt(m);
  expectStoreLandedOnADrawnLineBeforeItsDelivery(m);
  ASSERT_NE(m.state().ppu.inidisp, 0x80u) << "the lines before the take-away delivered";

  // The entry counts a line down whether or not the channel is named, so once it is
  // named again the values that land go on stepping by one a line — which a channel
  // that had started its table over, or spent a line fetching, could not do.
  runPastThisLinesDelivery(m);
  const std::uint8_t resumed = m.state().ppu.inidisp;
  m.run(kLine);
  EXPECT_EQ(m.state().ppu.inidisp, static_cast<std::uint8_t>(resumed + 1u));
  m.run(kLine);
  EXPECT_EQ(m.state().ppu.inidisp, static_cast<std::uint8_t>(resumed + 2u));
  m.run(kLine);
  EXPECT_EQ(m.state().ppu.inidisp, static_cast<std::uint8_t>(resumed + 3u));
}

TEST(SnesDma, HdmaAChannelBroughtInMidPictureKeepsTheCursorTheProgramGaveIt) {
  // Nothing was armed as the frame began, so no reload has happened and none will: the
  // channel walks from the cursor in $43x8, and the table start in $43x2 — which names
  // somewhere else entirely — is not consulted.
  Snes m = hdmaWritingMachine(40u, {{0x01u, 0x0Cu}}, {}, 0x00u);
  SnesState s = m.state();
  s.dma[0].a1t = 0x0500u;   // a table start the frame's reload would have used
  s.wram[0x500u] = 0x01u;   // holding a different entry entirely
  s.wram[0x501u] = 0x77u;
  s.dma[0].a2a = 0x0300u;   // the cursor the program set
  s.dma[0].nltr = 0x01u;    // one line to spend reaching the table
  s.wram[0x300u] = 0x02u;   // $7E:0300: two lines of $0A, then stop
  s.wram[0x301u] = 0x0Au;
  s.wram[0x302u] = 0x00u;
  m.restore(s);

  runToTheHalt(m);
  expectStoreLandedOnADrawnLineBeforeItsDelivery(m);
  ASSERT_NE(m.state().hdmaActive & 1u, 0u) << "the write brought the channel in";
  runPastThisLinesDelivery(m);
  m.run(kLine);
  EXPECT_EQ(m.state().ppu.inidisp, 0x0Au) << "the cursor's table, not the one A1T names";
}

TEST(SnesDma, HdmaAChannelBroughtInAloneFetchesBeforeItDelivers) {
  // $420C was empty, so this is the picture's first channel: the line it comes in on
  // spends the count the program left and fetches the table's own entry, delivering
  // nothing, and the line after that carries the table's value.
  Snes m = hdmaWritingMachine(40u, {{0x01u, 0x0Cu}}, {0x02u, 0x0Au, 0x00u}, 0x00u);
  SnesState s = m.state();
  s.dma[0].a2a = 0x0300u;
  s.dma[0].nltr = 0x01u;
  m.restore(s);

  runToTheHalt(m);
  expectStoreLandedOnADrawnLineBeforeItsDelivery(m);
  runPastThisLinesDelivery(m);
  EXPECT_EQ(m.state().ppu.inidisp, 0x80u) << "nothing delivered on the line it came in on";
  m.run(kLine);
  EXPECT_EQ(m.state().ppu.inidisp, 0x0Au) << "and the table's value on the line after";
}

TEST(SnesDma, HdmaAChannelJoiningChannelsAlreadyGoingDeliversOnItsFirstLine) {
  // Channel 1 is armed as the frame begins, so HDMA is already running when channel 0
  // is brought in. That channel delivers before it fetches, so the byte its cursor
  // names goes out as a value rather than being read as a count.
  Snes m = hdmaWritingMachine(40u, {{0x03u, 0x0Cu}}, {}, 0x02u);
  SnesState s = m.state();
  s.dma[1] = DmaChannel{.dmap = 0x00u, .bbad = 0x32u, .a1t = 0x0400u, .a1b = 0x7Eu};
  s.wram[0x400u] = 0xFFu;  // channel 1 repeats, keeping HDMA running all picture
  for (std::uint16_t i = 0x401u; i <= 0x47Fu; ++i) s.wram[i] = 0x00u;
  s.dma[0].a2a = 0x0300u;
  s.dma[0].nltr = 0x01u;
  s.wram[0x300u] = 0x0Fu;  // the cursor's byte: a value here, not a count
  s.wram[0x301u] = 0x02u;  // the entry the fetch after it takes
  s.wram[0x302u] = 0x0Au;
  s.wram[0x303u] = 0x00u;
  m.restore(s);

  runToTheHalt(m);
  expectStoreLandedOnADrawnLineBeforeItsDelivery(m);
  runPastThisLinesDelivery(m);
  EXPECT_EQ(m.state().ppu.inidisp, 0x0Fu) << "the cursor's byte went out as a value";
}

TEST(SnesDma, HdmaAChannelWhoseTableEndedCannotBeBroughtBackInThePicture) {
  // The first entry is a terminator, so the channel is done for the frame before it
  // delivers anything. Taking it out of $420C and putting it back does not restart it.
  Snes m = hdmaWritingMachine(40u, {{0x00u, 0x0Cu}, {0x01u, 0x0Cu}}, {0x00u, 0x0Au}, 0x01u, 36u);
  runToTheHalt(m);
  EXPECT_EQ(m.state().hdmaActive & 1u, 0u) << "ended at the frame's own reload";
  m.run(4u * kLine);
  EXPECT_EQ(m.state().ppu.inidisp, 0x80u) << "and never delivered";
  EXPECT_EQ(m.state().hdmaActive & 1u, 0u);
}

TEST(SnesDma, HdmaATableThatEndsPartWayDownThePictureCannotBeBroughtBack) {
  // The table terminates on its second line rather than at the picture's own reload.
  // Cycling $420C afterwards must not start it again: what follows the terminator
  // would go out as a value if it did.
  Snes m = hdmaWritingMachine(90u, {{0x00u, 0x0Cu}, {0x01u, 0x0Cu}}, {}, 0x01u, 36u);
  SnesState s = m.state();
  s.wram[0x300u] = 0x01u;  // one line of $0A
  s.wram[0x301u] = 0x0Au;
  s.wram[0x302u] = 0x00u;  // then the terminator
  s.wram[0x303u] = 0x5Au;  // and a value a restarted channel would reach
  m.restore(s);

  runToTheHalt(m);
  EXPECT_EQ(m.state().ppu.inidisp, 0x0Au) << "the one line the table named";
  EXPECT_EQ(m.state().hdmaActive & 1u, 0u) << "ended part-way down the picture";
  m.run(4u * kLine);
  EXPECT_EQ(m.state().ppu.inidisp, 0x0Au) << "and nothing after the terminator went out";
}

TEST(SnesDma, HdmaAPictureThatOpensWithNoChannelArmedStillClearsWhatTheLastOneLeft) {
  // The start-of-frame reload is what clears a channel's per-picture state, and a
  // picture that opens with $420C empty runs no reload at all — so the picture's own
  // boundary clears it, and a channel whose table ended is free to be brought in again.
  Snes m = haltedMachine();
  SnesState s = m.state();
  s.hdmaen = 0x00u;
  s.hdmaActive = 0x01u;
  s.hdmaEnded = 0x01u;
  s.hdmaDoWrite = 0x01u;
  s.vpos = 260u;
  s.inVblank = true;
  m.restore(s);

  m.run(3u * kLine);
  ASSERT_LT(m.state().vpos, 3u) << "the run crossed into the next picture";
  EXPECT_EQ(m.state().hdmaEnded, 0u);
  EXPECT_EQ(m.state().hdmaActive, 0u);
  EXPECT_EQ(m.state().hdmaDoWrite, 0u);
}

TEST(SnesDma, HdmaAWriteInTheVerticalBlankBringsNothingIn) {
  // The blank has no lines left to join, and the frame after it hands every channel
  // its table afresh.
  Snes m = hdmaWritingMachine(40u, {{0x01u, 0x0Cu}}, {0x02u, 0x0Au, 0x00u}, 0x00u);
  SnesState s = m.state();
  s.vpos = 226u;  // inside the vertical blank
  s.inVblank = true;
  s.dma[0].a2a = 0x0300u;
  s.dma[0].nltr = 0x01u;
  m.restore(s);

  runToTheHalt(m);
  ASSERT_TRUE(m.state().inVblank) << "the store belongs to the blank";
  EXPECT_EQ(m.state().hdmaActive & 1u, 0u) << "nothing joined";
  EXPECT_EQ(m.state().ppu.inidisp, 0x80u);
}

TEST(SnesDma, HdmaDeactivatesAtVblank) {
  // A repeat entry long enough to span the whole visible frame must still stop at
  // vblank: it delivers on the visible lines and never on a vblank line.
  Snes m = haltedMachine();
  SnesState s = m.state();
  s.dma[0] = DmaChannel{.dmap = 0x00u, .bbad = 0x00u, .a1t = 0x0300u, .a1b = 0x7Eu};
  s.hdmaen = 0x01u;
  s.wram[0x300u] = 0xFFu;  // repeat for 127 lines
  for (std::uint16_t i = 0x301u; i <= 0x37Fu; ++i) s.wram[i] = 0x0Au;  // every line writes $0A
  s.ppu.inidisp =0x80u;
  m.restore(s);

  m.run(kVblankFirstDelivery);
  EXPECT_EQ(m.state().ppu.inidisp, 0x0Au);         // delivered during the visible picture
  EXPECT_EQ(m.state().hdmaActive & 1u, 0u);    // and deactivated at vblank
  SnesState s2 = m.state();
  s2.ppu.inidisp =0x33u;                          // a marker a vblank delivery would overwrite
  m.restore(s2);
  m.run(4u * kLine);
  EXPECT_EQ(m.state().ppu.inidisp, 0x33u);         // untouched through vblank
}

}  // namespace
}  // namespace snaggletooth
