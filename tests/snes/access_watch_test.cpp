// The console's access watch: a host is told, before a watched access takes
// effect, that the program is reading or writing a place it armed, and answers
// what happens — let it through, prevent it, or substitute a byte. Each case
// drives a real access (a CPU instruction, a transfer, the work-RAM port) and
// reads back the byte the program saw or the byte that landed — never "the
// symbol exists".
//
// A watched place is a byte, not a bus address: the mirror cases arm a byte
// through one of its addresses and hear it through the others — the low 8 KB
// of work RAM from bank $7E, a system bank and the work-RAM port; a register
// from bank $00 and bank $80; an image byte from every bank the map repeats it
// in; a save byte across its window. The classification walk holds the
// machine's own physical() to the read path over every one of the 16,777,216
// bus addresses on three cartridge maps, and to the cartridge functions the
// bus's page table is built from, on images of two chips, of a size no page
// divides, and with no save. The source cases hold that the watcher
// is told which part of the machine made the access; the kind cases that it is
// told what the cycle was for; the cycle cases that it is told which cycle of
// the instruction the access is, in the order the chip spends them — pinned on
// the machine for the shapes that matter to a host answering per byte, and held
// to the SingleStepTests recordings for every opcode in both modes through the
// core's cycle index, which is what the machine reports. The wrinkle case pins
// a register read whose side effect has happened by the time the watch is
// told. The last cases pin the machine that watches nothing: with a watcher set
// and no place armed it runs a fixed program to a fixed budget and lands
// byte-identical to a plain machine, and the watch survives a move.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "../cpu65816/vector_harness.h"
#include "snaggletooth/snes/snes.h"

#ifndef SNAGGLETOOTH_65816_VECTORS
#define SNAGGLETOOTH_65816_VECTORS ""
#endif

namespace snaggletooth {
namespace {

// One access the watcher was told about.
struct Told {
  std::uint32_t address = 0;
  std::uint8_t value = 0;
  AccessSource source = AccessSource::Cpu;
  CycleKind kind = CycleKind::DataRead;
  std::uint8_t cycle = 0;
  bool write = false;
};

// A watcher that records every access it is told and answers each with a fixed
// verdict the test sets. proceed() by default.
struct Watcher final : AccessWatcher {
  std::vector<Told> told;
  AccessAnswer verdict = AccessAnswer::proceed();

  AccessAnswer read(std::uint32_t address, std::uint8_t value, AccessSource source,
                    CycleKind kind, std::uint8_t cycle) override {
    told.push_back({address, value, source, kind, cycle, false});
    return verdict;
  }
  AccessAnswer write(std::uint32_t address, std::uint8_t value, AccessSource source,
                     CycleKind kind, std::uint8_t cycle) override {
    told.push_back({address, value, source, kind, cycle, true});
    return verdict;
  }

  [[nodiscard]] std::size_t reads() const {
    std::size_t n = 0;
    for (const Told& t : told)
      if (!t.write) ++n;
    return n;
  }
  [[nodiscard]] std::size_t writes() const {
    std::size_t n = 0;
    for (const Told& t : told)
      if (t.write) ++n;
    return n;
  }
  [[nodiscard]] const Told* firstFrom(AccessSource source) const {
    for (const Told& t : told)
      if (t.source == source) return &t;
    return nullptr;
  }
  [[nodiscard]] const Told* firstAt(std::uint32_t address) const {
    for (const Told& t : told)
      if (t.address == address) return &t;
    return nullptr;
  }
};

constexpr std::uint8_t kLdaImm = 0xA9u;
constexpr std::uint8_t kLdaAbs = 0xADu;
constexpr std::uint8_t kLdaLong = 0xAFu;
constexpr std::uint8_t kStaAbs = 0x8Du;
constexpr std::uint8_t kIncAbs = 0xEEu;
constexpr std::uint8_t kPha = 0x48u;
constexpr std::uint8_t kClc = 0x18u;
constexpr std::uint8_t kXce = 0xFBu;
constexpr std::uint8_t kRep = 0xC2u;
constexpr std::uint8_t kNop = 0xEAu;
constexpr std::uint8_t kStp = 0xDBu;
constexpr std::uint8_t kBra = 0x80u;

// A machine running `program` from $8000 in a one-bank LoROM image.
Snes programMachine(std::initializer_list<std::uint8_t> program) {
  std::vector<std::uint8_t> rom(program.begin(), program.end());
  rom.resize(0x8000u, 0x00u);
  rom[0x7FFCu] = 0x00u;  // reset -> $8000
  rom[0x7FFDu] = 0x80u;
  return Snes(SnesConfig{.rom = rom});
}

// Runs the three instructions that put the core in native mode with a 16-bit
// accumulator and index registers: CLC, XCE, REP #$30. Programs that test a
// 16-bit access begin with them.
void toNative16(Snes& m) {
  m.step();  // CLC
  m.step();  // XCE
  m.step();  // REP #$30
}

// ---- nothing armed: the watcher is not told, and the machine is unchanged ----

TEST(SnesAccessWatch, NothingArmedIsNotTold) {
  Snes m = programMachine({kLdaImm, 0x77u, kStaAbs, 0x20u, 0x00u, kStp});
  Watcher w;
  m.setAccessWatcher(&w);
  EXPECT_EQ(m.accessWatcher(), &w);
  m.step();  // LDA #$77
  m.step();  // STA !$0020 -- a real read (the fetch) and write, but no place is armed
  EXPECT_EQ(w.told.size(), 0u);
  EXPECT_EQ(m.state().wram[0x20], 0x77u);  // the write landed as it always would
}

// ---- reads: proceed, substitute, veto ----------------------------------------

TEST(SnesAccessWatch, AProceededReadDeliversTheMachinesByte) {
  Snes m = programMachine({kLdaAbs, 0x10u, 0x00u, kStp});  // LDA !$0010
  SnesState s = m.state();
  s.wram[0x10] = 0x5Au;
  m.restore(s);
  Watcher w;
  m.setAccessWatcher(&w);
  m.watchAccess(0x000010u, 1, /*onRead=*/true, /*onWrite=*/false);
  m.step();  // LDA !$0010
  ASSERT_EQ(w.reads(), 1u);
  const Told& t = *w.firstFrom(AccessSource::Cpu);
  EXPECT_EQ(t.address, 0x000010u);
  EXPECT_EQ(int{t.value}, 0x5A) << "the byte the machine would have answered";
  EXPECT_FALSE(t.write);
  EXPECT_EQ(static_cast<int>(m.cpuState().a & 0xFFu), 0x5A) << "and the CPU received it unchanged";
}

TEST(SnesAccessWatch, AnInsteadReadDeliversTheSubstitute) {
  Snes m = programMachine({kLdaAbs, 0x10u, 0x00u, kStp});
  SnesState s = m.state();
  s.wram[0x10] = 0x5Au;
  m.restore(s);
  Watcher w;
  w.verdict = AccessAnswer::instead(0x99u);
  m.setAccessWatcher(&w);
  m.watchAccess(0x000010u, 1, true, false);
  m.step();
  EXPECT_EQ(static_cast<int>(m.cpuState().a & 0xFFu), 0x99) << "the CPU received the substitute";
  EXPECT_EQ(m.state().wram[0x10], 0x5Au) << "the memory the watch stood in front of is untouched";
}

TEST(SnesAccessWatch, AVetoedReadStillDeliversTheMachinesByte) {
  // A read cannot be prevented: a veto answers the byte the access would have.
  Snes m = programMachine({kLdaAbs, 0x10u, 0x00u, kStp});
  SnesState s = m.state();
  s.wram[0x10] = 0x5Au;
  m.restore(s);
  Watcher w;
  w.verdict = AccessAnswer::veto();
  m.setAccessWatcher(&w);
  m.watchAccess(0x000010u, 1, true, false);
  m.step();
  EXPECT_EQ(static_cast<int>(m.cpuState().a & 0xFFu), 0x5A);
}

// ---- writes: proceed, substitute, veto ---------------------------------------

TEST(SnesAccessWatch, AProceededWriteStoresTheValue) {
  Snes m = programMachine({kLdaImm, 0x77u, kStaAbs, 0x20u, 0x00u, kStp});
  Watcher w;
  m.setAccessWatcher(&w);
  m.watchAccess(0x000020u, 1, false, /*onWrite=*/true);
  m.step();  // LDA #$77
  m.step();  // STA !$0020
  ASSERT_EQ(w.writes(), 1u);
  const Told& t = *w.firstFrom(AccessSource::Cpu);
  EXPECT_EQ(t.address, 0x000020u);
  EXPECT_EQ(int{t.value}, 0x77);
  EXPECT_TRUE(t.write);
  EXPECT_EQ(m.state().wram[0x20], 0x77u);
}

TEST(SnesAccessWatch, AnInsteadWriteStoresTheSubstitute) {
  Snes m = programMachine({kLdaImm, 0x77u, kStaAbs, 0x20u, 0x00u, kStp});
  Watcher w;
  w.verdict = AccessAnswer::instead(0x33u);
  m.setAccessWatcher(&w);
  m.watchAccess(0x000020u, 1, false, true);
  m.step();
  m.step();
  EXPECT_EQ(m.state().wram[0x20], 0x33u) << "the substitute landed, not the driven value";
}

TEST(SnesAccessWatch, AVetoedWriteStoresNothing) {
  Snes m = programMachine({kLdaImm, 0x77u, kStaAbs, 0x20u, 0x00u, kStp});
  SnesState s = m.state();
  s.wram[0x20] = 0xC5u;  // a byte already there
  m.restore(s);
  Watcher w;
  w.verdict = AccessAnswer::veto();
  m.setAccessWatcher(&w);
  m.watchAccess(0x000020u, 1, false, true);
  m.step();
  m.step();
  EXPECT_EQ(m.state().wram[0x20], 0xC5u) << "the write was prevented; the byte stands";
}

// ---- the two directions are armed independently ------------------------------

TEST(SnesAccessWatch, ReadAndWriteAreArmedIndependently) {
  Snes m = programMachine({kLdaImm, 0x77u, kStaAbs, 0x20u, 0x00u, kStp});
  Watcher w;
  m.setAccessWatcher(&w);
  m.watchAccess(0x000020u, 1, /*onRead=*/true, /*onWrite=*/false);  // reads only
  m.step();  // LDA #$77
  m.step();  // STA !$0020 -- a write to the armed place, but only reads are armed
  EXPECT_EQ(w.writes(), 0u) << "a write is not told when only reads are armed";
  EXPECT_EQ(m.state().wram[0x20], 0x77u);
}

// ---- the watcher is told which part of the machine made the access ----------

TEST(SnesAccessWatch, ACpuAccessIsToldWithSourceCpu) {
  Snes m = programMachine({kLdaAbs, 0x10u, 0x00u, kStp});
  Watcher w;
  m.setAccessWatcher(&w);
  m.watchAccess(0x000010u, 1, true, false);
  m.step();
  ASSERT_NE(w.firstFrom(AccessSource::Cpu), nullptr);
  EXPECT_EQ(w.firstFrom(AccessSource::Cpu)->address, 0x000010u);
}

TEST(SnesAccessWatch, ATransferAccessIsToldWithSourceDmaAsADataReadOfItsUnit) {
  // A general-purpose transfer of one byte reads $00:9000 (A bus) and writes it
  // through $2180 (B bus). Arming the read side hears the engine's read: a plain
  // data read, the byte at position 0 of the channel's pattern.
  Snes m = programMachine({kLdaImm, 0x01u, kStaAbs, 0x0Bu, 0x42u, kNop, kStp});  // MDMAEN=1
  SnesState s = m.state();
  s.dma[0].dmap = 0x00u;    // A->B, increment
  s.dma[0].bbad = 0x80u;    // $2180
  s.dma[0].a1t = 0x9000u;
  s.dma[0].a1b = 0x00u;
  s.dma[0].das = 0x0001u;   // one byte
  s.wmadd = 0x001000u;      // the transfer's byte lands here, in work RAM
  m.restore(s);
  m.poke(0x009000u, 0xA0u);  // the byte the transfer reads, in the cartridge image
  Watcher w;
  m.setAccessWatcher(&w);
  m.watchAccess(0x009000u, 1, /*onRead=*/true, false);
  m.run(20000u);
  const Told* t = w.firstFrom(AccessSource::Dma);
  ASSERT_NE(t, nullptr);
  EXPECT_EQ(t->address, 0x009000u);
  EXPECT_FALSE(t->write);
  EXPECT_EQ(t->kind, CycleKind::DataRead);
  EXPECT_EQ(int{t->cycle}, 0);
}

TEST(SnesAccessWatch, ATwoRegisterTransferTellsEachByteItsPositionInThePattern) {
  // Pattern 1 writes two bytes to $2118 then $2119: the second byte of each pair
  // is position 1 of the pattern, on its read and on its write alike.
  Snes m = programMachine({kLdaImm, 0x01u, kStaAbs, 0x0Bu, 0x42u, kNop, kStp});  // MDMAEN=1
  SnesState s = m.state();
  s.dma[0].dmap = 0x01u;    // A->B, increment, pattern 1 (two registers)
  s.dma[0].bbad = 0x18u;    // $2118
  s.dma[0].a1t = 0x9000u;
  s.dma[0].a1b = 0x00u;
  s.dma[0].das = 0x0002u;   // one pair
  m.restore(s);
  Watcher w;
  m.setAccessWatcher(&w);
  m.watchAccess(0x009000u, 2, /*onRead=*/true, false);
  m.watchAccess(0x002118u, 2, false, /*onWrite=*/true);
  m.run(20000u);
  const Told* second = w.firstAt(0x009001u);
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(int{second->cycle}, 1) << "the second byte of the pair on its read";
  const Told* high = w.firstAt(0x002119u);
  ASSERT_NE(high, nullptr);
  EXPECT_TRUE(high->write);
  EXPECT_EQ(int{high->cycle}, 1) << "and on its write";
  EXPECT_EQ(int{w.firstAt(0x009000u)->cycle}, 0);
  EXPECT_EQ(int{w.firstAt(0x002118u)->cycle}, 0);
}

TEST(SnesAccessWatch, AnHdmaAccessIsToldWithSourceHdmaAndATableReadCarriesZero) {
  // Channel 0 delivers a one-line direct table to $2100; arming the table's count
  // byte hears the HDMA engine's read at initialisation, cycle 0 for a count.
  Snes m = programMachine({kNop, kBra, 0xFDu});  // NOP ; BRA back
  SnesState s = m.state();
  s.hdmaen = 0x01u;
  s.dma[0].dmap = 0x00u;
  s.dma[0].bbad = 0x00u;   // $2100
  s.dma[0].a1t = 0x0100u;  // the table at $7E:0100
  s.dma[0].a1b = 0x7Eu;
  s.wram[0x100] = 0x01u;   // one line
  s.wram[0x101] = 0x0Fu;   // the value
  s.wram[0x102] = 0x00u;   // stop
  m.restore(s);
  Watcher w;
  m.setAccessWatcher(&w);
  m.watchAccess(0x7E0100u, 1, /*onRead=*/true, false);
  m.run(2u * 1364u);
  const Told* t = w.firstFrom(AccessSource::Hdma);
  ASSERT_NE(t, nullptr);
  EXPECT_EQ(t->address, 0x7E0100u);
  EXPECT_EQ(int{t->value}, 0x01);
  EXPECT_EQ(t->kind, CycleKind::DataRead);
  EXPECT_EQ(int{t->cycle}, 0);
}

TEST(SnesAccessWatch, TheWorkRamPortsOwnAccessIsToldWithSourceWramPortAndTheDrivingCycle) {
  // The CPU reads $2180 on cycle 3 of LDA !abs; the port reads work RAM at its
  // own 24-bit address. That read is the work-RAM port's, and arming it hears
  // the port, not the CPU, carrying the cycle of the access that drove it.
  Snes m = programMachine({kLdaAbs, 0x80u, 0x21u, kStp});  // LDA !$2180
  SnesState s = m.state();
  s.wmadd = 0x000010u;
  s.wram[0x10] = 0x3Cu;
  m.restore(s);
  Watcher w;
  m.setAccessWatcher(&w);
  m.watchAccess(0x7E0010u, 1, /*onRead=*/true, false);
  m.step();
  const Told* t = w.firstFrom(AccessSource::WramPort);
  ASSERT_NE(t, nullptr);
  EXPECT_EQ(t->address, 0x7E0010u);
  EXPECT_EQ(int{t->value}, 0x3C);
  EXPECT_EQ(t->kind, CycleKind::DataRead);
  EXPECT_EQ(int{t->cycle}, 3);
}

TEST(SnesAccessWatch, AWorkRamPortReadCanBeSubstituted) {
  // A substitute at the port's own read changes the byte $2180 delivers to the CPU.
  Snes m = programMachine({kLdaAbs, 0x80u, 0x21u, kStp});  // LDA !$2180
  SnesState s = m.state();
  s.wmadd = 0x000010u;
  s.wram[0x10] = 0x3Cu;
  m.restore(s);
  Watcher w;
  w.verdict = AccessAnswer::instead(0xE7u);
  m.setAccessWatcher(&w);
  m.watchAccess(0x7E0010u, 1, true, false);
  m.step();
  EXPECT_EQ(static_cast<int>(m.cpuState().a & 0xFFu), 0xE7);
  EXPECT_EQ(m.state().wram[0x10], 0x3Cu) << "the RAM beneath is untouched";
}

// ---- a watch is on the byte, whichever address reaches it -------------------

TEST(SnesAccessWatch, ALowWorkRamByteArmedInBank7EIsHeardThroughEveryAlias) {
  // $7E:0010 is the same byte as $00:0010 and $BF:0010, and the byte the port
  // reads at wmadd = $000010. Armed once through bank $7E, all four are told.
  Snes m = programMachine({kLdaAbs, 0x10u, 0x00u,          // LDA !$0010   (bank $00)
                           kLdaLong, 0x10u, 0x00u, 0xBFu,  // LDA >$BF0010
                           kLdaLong, 0x10u, 0x00u, 0x7Eu,  // LDA >$7E0010
                           kLdaAbs, 0x80u, 0x21u,          // LDA !$2180   (the port)
                           kStp});
  SnesState s = m.state();
  s.wram[0x10] = 0x3Cu;
  s.wmadd = 0x000010u;
  m.restore(s);
  Watcher w;
  m.setAccessWatcher(&w);
  m.watchAccess(0x7E0010u, 1, /*onRead=*/true, false);
  for (int i = 0; i < 4; ++i) m.step();
  ASSERT_EQ(w.reads(), 4u);
  ASSERT_NE(w.firstAt(0x000010u), nullptr) << "through the bank-$00 mirror";
  ASSERT_NE(w.firstAt(0xBF0010u), nullptr) << "through the bank-$BF mirror";
  ASSERT_NE(w.firstAt(0x7E0010u), nullptr) << "through bank $7E itself, and the port's own read";
  EXPECT_EQ(w.firstAt(0x000010u)->source, AccessSource::Cpu);
  EXPECT_EQ(int{w.firstAt(0xBF0010u)->value}, 0x3C);
  EXPECT_NE(w.firstFrom(AccessSource::WramPort), nullptr);
}

TEST(SnesAccessWatch, ABankArmedThroughAMirrorIsTheSameByteAsBank7E) {
  // The other way round: armed through $00:0010, an access through $7E:0010 is
  // told, and a substitute there reaches the program.
  Snes m = programMachine({kLdaLong, 0x10u, 0x00u, 0x7Eu, kStp});  // LDA >$7E0010
  Watcher w;
  w.verdict = AccessAnswer::instead(0x42u);
  m.setAccessWatcher(&w);
  m.watchAccess(0x000010u, 1, true, false);
  m.step();
  ASSERT_EQ(w.reads(), 1u);
  EXPECT_EQ(w.told[0].address, 0x7E0010u) << "told the address the access drove";
  EXPECT_EQ(static_cast<int>(m.cpuState().a & 0xFFu), 0x42);
}

TEST(SnesAccessWatch, ARegisterArmedOnceIsHeardFromBank00AndBank80) {
  // $2140 is the same audio port at its offset in every system bank.
  Snes m = programMachine({kLdaAbs, 0x40u, 0x21u,          // LDA !$2140
                           kLdaLong, 0x40u, 0x21u, 0x80u,  // LDA >$802140
                           kStp});
  Watcher w;
  m.setAccessWatcher(&w);
  m.watchAccess(0x002140u, 1, /*onRead=*/true, false);
  m.step();
  m.step();
  EXPECT_EQ(w.reads(), 2u);
  EXPECT_NE(w.firstAt(0x002140u), nullptr);
  EXPECT_NE(w.firstAt(0x802140u), nullptr);
}

TEST(SnesAccessWatch, AnImageByteArmedOnceIsHeardFromEveryBankTheMapRepeatsItIn) {
  // Image offset $0100 of a 32 KB LoROM image reads at $00:8100, $80:8100 and,
  // the image repeating across the cartridge banks, at $40:8100.
  Snes m = programMachine({kLdaLong, 0x00u, 0x81u, 0x80u,  // LDA >$808100
                           kLdaLong, 0x00u, 0x81u, 0x40u,  // LDA >$408100
                           kStp});
  Watcher w;
  m.setAccessWatcher(&w);
  m.watchAccess(0x008100u, 1, /*onRead=*/true, false);
  m.step();
  m.step();
  EXPECT_EQ(w.reads(), 2u);
  EXPECT_NE(w.firstAt(0x808100u), nullptr);
  EXPECT_NE(w.firstAt(0x408100u), nullptr);
}

TEST(SnesAccessWatch, ASaveByteArmedOnceIsHeardAcrossItsWindow) {
  // An 8 KB LoROM save repeats across $70-$7D and $F0-$FF, and within each
  // bank's lower half: $70:0010, $71:2010 and $F0:0010 are one byte.
  std::vector<std::uint8_t> rom(0x8000u, 0x00u);
  const std::uint8_t program[] = {kLdaLong, 0x10u, 0x00u, 0x70u,   // LDA >$700010
                                  kLdaLong, 0x10u, 0x20u, 0x71u,   // LDA >$712010
                                  kLdaLong, 0x10u, 0x00u, 0xF0u,   // LDA >$F00010
                                  kStp};
  for (std::size_t i = 0; i < sizeof program; ++i) rom[i] = program[i];
  rom[0x7FFCu] = 0x00u;
  rom[0x7FFDu] = 0x80u;
  Snes m(SnesConfig{.rom = rom, .map = CartridgeMap::LoRom, .saveRamBytes = 0x2000u});
  Watcher w;
  m.setAccessWatcher(&w);
  m.watchAccess(0x700010u, 1, /*onRead=*/true, false);
  for (int i = 0; i < 3; ++i) m.step();
  EXPECT_EQ(w.reads(), 3u);
  EXPECT_NE(w.firstAt(0x700010u), nullptr);
  EXPECT_NE(w.firstAt(0x712010u), nullptr);
  EXPECT_NE(w.firstAt(0xF00010u), nullptr);
}

TEST(SnesAccessWatch, TheLowWorkRamAliasesShareOneKey) {
  // Every byte of the low 8 KB: its bank-$7E address and its address in each
  // of the 128 system banks classify the same.
  Snes m = programMachine({kStp});
  std::size_t apart = 0;
  for (std::uint32_t offset = 0; offset < 0x2000u; ++offset) {
    const Snes::Physical key = m.physical(0x7E0000u | offset);
    if (key.space != Snes::Space::WorkRam || key.index != offset) ++apart;
    for (std::uint32_t bank = 0; bank < 0x100u; ++bank) {
      const bool systemBank = bank <= 0x3Fu || (bank >= 0x80u && bank <= 0xBFu);
      if (!systemBank) continue;
      if (!(m.physical((bank << 16) | offset) == key)) ++apart;
    }
  }
  EXPECT_EQ(apart, 0u);
}

// The classification walk: physical() against the read path over every bus
// address. peek is the read path without a cycle, so a byte physical() places in
// a memory is checked against peek's byte at that place; a place physical() says
// is a register or open bus is one peek refuses; and every register window is
// the one the bus dispatches on. A machine's memories are filled with patterns
// that differ across their whole index, so an index off by any amount reads a
// different byte.
std::uint8_t patternAt(std::uint32_t index, std::uint8_t salt) {
  return static_cast<std::uint8_t>((index * 0x9Eu + (index >> 8) * 0x37u + (index >> 16) * 0x5Bu) ^
                                   salt);
}

struct WalkMachine {
  Snes machine;
  std::vector<std::uint8_t> image;  // the pattern the image carries
  std::size_t saveBytes;
};

WalkMachine walkMachine(CartridgeMap map, std::size_t imageBytes, std::size_t saveBytes) {
  std::vector<std::uint8_t> rom(imageBytes);
  for (std::size_t i = 0; i < rom.size(); ++i) rom[i] = patternAt(static_cast<std::uint32_t>(i), 0xA5u);
  Snes m(SnesConfig{.rom = rom, .map = map, .saveRamBytes = saveBytes});
  SnesState s = m.state();
  for (std::size_t i = 0; i < s.wram.size(); ++i) s.wram[i] = patternAt(static_cast<std::uint32_t>(i), 0x3Cu);
  for (std::size_t i = 0; i < s.sram.size(); ++i) s.sram[i] = patternAt(static_cast<std::uint32_t>(i), 0xC3u);
  m.restore(s);
  return WalkMachine{std::move(m), std::move(rom), saveBytes};
}

// Whether a system-bank offset is one the bus routes to a register: the same
// windows the machine's guide documents, stated here in their own right.
bool documentedRegister(std::uint16_t offset) {
  return (offset >= 0x2100u && offset <= 0x213Fu) ||  // the PPU
         (offset >= 0x2140u && offset <= 0x217Fu) ||  // the audio ports
         (offset >= 0x2180u && offset <= 0x2183u) ||  // the work-RAM port
         offset == 0x4016u || offset == 0x4017u ||    // the serial controller ports
         (offset >= 0x4200u && offset <= 0x421Fu) ||  // the CPU's registers
         (offset >= 0x4300u && offset <= 0x437Fu);    // the transfer engines'
}

// Walks every address and returns the first that disagrees, with why, or nothing.
std::optional<std::string> walk(const WalkMachine& w) {
  const Snes& m = w.machine;
  for (std::uint64_t a = 0; a < 0x1000000u; ++a) {
    const std::uint32_t address = static_cast<std::uint32_t>(a);
    const Snes::Physical p = m.physical(address);
    const std::optional<std::uint8_t> read = m.peek(address);
    const std::uint8_t bank = static_cast<std::uint8_t>(address >> 16);
    const std::uint16_t offset = static_cast<std::uint16_t>(address & 0xFFFFu);
    const bool systemBank = bank <= 0x3Fu || (bank >= 0x80u && bank <= 0xBFu);
    char why[96];
    switch (p.space) {
      case Snes::Space::WorkRam:
        if (p.index >= 0x20000u || !read.has_value() || *read != patternAt(p.index, 0x3Cu)) {
          std::snprintf(why, sizeof why, "$%06X: work RAM index %u disagrees with peek", address, p.index);
          return std::string(why);
        }
        break;
      case Snes::Space::Register:
        if (!systemBank || !documentedRegister(offset) || p.index != offset || read.has_value()) {
          std::snprintf(why, sizeof why, "$%06X: register index %u is not a register window", address, p.index);
          return std::string(why);
        }
        break;
      case Snes::Space::SaveRam:
        if (p.index >= w.saveBytes || !read.has_value() || *read != patternAt(p.index, 0xC3u)) {
          std::snprintf(why, sizeof why, "$%06X: save index %u disagrees with peek", address, p.index);
          return std::string(why);
        }
        break;
      case Snes::Space::CartridgeRom:
        if (p.index >= w.image.size() || !read.has_value() || *read != w.image[p.index]) {
          std::snprintf(why, sizeof why, "$%06X: image index %u disagrees with peek", address, p.index);
          return std::string(why);
        }
        break;
      case Snes::Space::OpenBus:
        if (p.index != address || read.has_value() || (systemBank && documentedRegister(offset))) {
          std::snprintf(why, sizeof why, "$%06X: open bus, but peek answers or a register lives here", address);
          return std::string(why);
        }
        break;
    }
  }
  return std::nullopt;
}

TEST(SnesAccessWatch, PhysicalAgreesWithTheReadPathOverEveryAddressUnderLoRom) {
  const WalkMachine w = walkMachine(CartridgeMap::LoRom, 0x10000u, 0x2000u);
  const std::optional<std::string> miss = walk(w);
  EXPECT_FALSE(miss.has_value()) << *miss;
}

TEST(SnesAccessWatch, PhysicalAgreesWithTheReadPathOverEveryAddressUnderHiRom) {
  const WalkMachine w = walkMachine(CartridgeMap::HiRom, 0x10000u, 0x2000u);
  const std::optional<std::string> miss = walk(w);
  EXPECT_FALSE(miss.has_value()) << *miss;
}

TEST(SnesAccessWatch, PhysicalAgreesWithTheReadPathOverEveryAddressUnderExHiRom) {
  const WalkMachine w = walkMachine(CartridgeMap::ExHiRom, 0x10000u, 0x2000u);
  const std::optional<std::string> miss = walk(w);
  EXPECT_FALSE(miss.has_value()) << *miss;
}

TEST(SnesAccessWatch, TwoAddressesThatReadOneByteClassifyTheSame) {
  // The other half of the walk: for a work-RAM, save or image byte, the
  // machine's own canonical address for it classifies identically, so arming
  // through either reaches one bit.
  const WalkMachine w = walkMachine(CartridgeMap::LoRom, 0x10000u, 0x2000u);
  const Snes& m = w.machine;
  std::size_t apart = 0;
  for (std::uint64_t a = 0; a < 0x1000000u; ++a) {
    const std::uint32_t address = static_cast<std::uint32_t>(a);
    const Snes::Physical p = m.physical(address);
    std::optional<std::uint32_t> canonical;
    if (p.space == Snes::Space::WorkRam) canonical = 0x7E0000u + p.index;
    if (p.space == Snes::Space::SaveRam) canonical = 0x700000u + p.index;  // LoROM's window begins at $70:0000
    if (p.space == Snes::Space::CartridgeRom) canonical = romAddress(CartridgeMap::LoRom, p.index);
    if (canonical.has_value() && !(m.physical(*canonical) == p)) ++apart;
  }
  EXPECT_EQ(apart, 0u);
}

// The table against the functions it is built from. For every address
// physical() puts on the image, the index is the one romOffset answers — the
// upper half's, in LoROM's save window on a cartridge with no save, where the
// board reads what the upper half reads — and for every address it puts on the
// save, the offset saveRamOffset answers reduced to the save's size. The walk
// above holds the table to the read path, which reads the same table; this
// holds it to its source at every address.
std::optional<std::string> walkAgainstTheCartridgeFunctions(const WalkMachine& w,
                                                            CartridgeMap map) {
  const Snes& m = w.machine;
  for (std::uint64_t a = 0; a < 0x1000000u; ++a) {
    const std::uint32_t address = static_cast<std::uint32_t>(a);
    const Snes::Physical p = m.physical(address);
    char why[96];
    if (p.space == Snes::Space::CartridgeRom) {
      std::optional<std::size_t> expected = romOffset(map, address, w.image.size());
      if (!expected.has_value() && map == CartridgeMap::LoRom &&
          saveRamOffset(map, address).has_value()) {
        expected = romOffset(map, address | 0x8000u, w.image.size());
      }
      if (!expected.has_value() || *expected != p.index) {
        std::snprintf(why, sizeof why, "$%06X: image index %u is not romOffset's", address, p.index);
        return std::string(why);
      }
    } else if (p.space == Snes::Space::SaveRam) {
      const std::optional<std::size_t> linear = saveRamOffset(map, address);
      if (!linear.has_value() || w.saveBytes == 0 || *linear % w.saveBytes != p.index) {
        std::snprintf(why, sizeof why, "$%06X: save index %u is not saveRamOffset's", address, p.index);
        return std::string(why);
      }
    }
  }
  return std::nullopt;
}

// Both walks on one machine: the read path, then the cartridge functions.
void walkBothWays(const WalkMachine& w, CartridgeMap map) {
  const std::optional<std::string> miss = walk(w);
  EXPECT_FALSE(miss.has_value()) << *miss;
  const std::optional<std::string> apart = walkAgainstTheCartridgeFunctions(w, map);
  EXPECT_FALSE(apart.has_value()) << *apart;
}

TEST(SnesAccessWatch, TheWalkHoldsOnAThreeMegabyteImage) {
  // A 3 MB image is a 2 MB chip and a 1 MB chip: the megabyte above it repeats
  // the second chip, so the fold lands on every page above 3 MB of the map's
  // layout and crosses pages as it goes.
  walkBothWays(walkMachine(CartridgeMap::LoRom, 3u * 1024u * 1024u, 0x2000u), CartridgeMap::LoRom);
}

TEST(SnesAccessWatch, TheWalkHoldsOnAnImageThatIsNotAMultipleOfEightKilobytes) {
  // 11 KB is chips of 8 KB, 2 KB and 1 KB: the two small chips repeat inside a
  // page, so every image page is found through romOffset at each access.
  walkBothWays(walkMachine(CartridgeMap::LoRom, 0x2C00u, 0x2000u), CartridgeMap::LoRom);
  walkBothWays(walkMachine(CartridgeMap::HiRom, 0x2C00u, 0x2000u), CartridgeMap::HiRom);
}

TEST(SnesAccessWatch, TheWalkHoldsWithNoSaveAndWithASaveSmallerThanAPage) {
  // With no save, LoROM's window reads the image its upper halves read and
  // HiROM's reads open bus; a 2 KB save repeats four times inside each page of
  // its window.
  walkBothWays(walkMachine(CartridgeMap::LoRom, 0x10000u, 0u), CartridgeMap::LoRom);
  walkBothWays(walkMachine(CartridgeMap::LoRom, 0x10000u, 0x800u), CartridgeMap::LoRom);
  walkBothWays(walkMachine(CartridgeMap::HiRom, 0x10000u, 0u), CartridgeMap::HiRom);
}

// ---- the watcher is told what the cycle was for ------------------------------

TEST(SnesAccessWatch, AnOpcodeFetchAndADataReadOfOneAddressAreToldApart) {
  // LDA !$8000 at $8000 reads its own opcode byte as data: the fetch is told as
  // OpcodeFetch on cycle 0 and the load as DataRead on cycle 3.
  Snes m = programMachine({kLdaAbs, 0x00u, 0x80u, kStp});
  Watcher w;
  m.setAccessWatcher(&w);
  m.watchAccess(0x008000u, 1, /*onRead=*/true, false);
  m.step();
  ASSERT_EQ(w.reads(), 2u);
  EXPECT_EQ(w.told[0].kind, CycleKind::OpcodeFetch);
  EXPECT_EQ(int{w.told[0].cycle}, 0);
  EXPECT_EQ(w.told[1].kind, CycleKind::DataRead);
  EXPECT_EQ(int{w.told[1].cycle}, 3);
  EXPECT_EQ(int{w.told[1].value}, int{kLdaAbs}) << "the program read its own opcode";
}

TEST(SnesAccessWatch, AnInterruptsVectorReadsAreToldAsVectorReads) {
  // NMI enabled, the frame's vertical blank takes the CPU through its interrupt
  // sequence: in emulation mode the two vector bytes at $FFFA-$FFFB are read on
  // cycles 5 and 6, low byte first.
  Snes m = programMachine({kLdaImm, 0x80u, kStaAbs, 0x00u, 0x42u,  // LDA #$80 ; STA $4200
                           kNop, kBra, 0xFDu});                   // loop
  m.poke(0x00FFFAu, 0x00u);  // the NMI vector -> $8000
  m.poke(0x00FFFBu, 0x80u);
  Watcher w;
  m.setAccessWatcher(&w);
  m.watchAccess(0x00FFFAu, 2, /*onRead=*/true, false);
  m.run(262u * 1364u);  // a frame: the NMI lands at vertical blank
  ASSERT_GE(w.reads(), 2u);
  EXPECT_EQ(w.told[0].address, 0x00FFFAu);
  EXPECT_EQ(w.told[0].kind, CycleKind::VectorRead);
  EXPECT_EQ(int{w.told[0].cycle}, 5);
  EXPECT_EQ(w.told[1].address, 0x00FFFBu);
  EXPECT_EQ(w.told[1].kind, CycleKind::VectorRead);
  EXPECT_EQ(int{w.told[1].cycle}, 6);
}

// ---- the watcher is told which cycle of the instruction the access is --------

TEST(SnesAccessWatch, ASixteenBitLoadReadsLowThenHighOnConsecutiveCycles) {
  Snes m = programMachine({kClc, kXce, kRep, 0x30u, kLdaAbs, 0x10u, 0x00u, kStp});
  toNative16(m);
  Watcher w;
  m.setAccessWatcher(&w);
  m.watchAccess(0x000010u, 2, /*onRead=*/true, false);
  m.step();  // LDA !$0010, 16-bit
  ASSERT_EQ(w.reads(), 2u);
  EXPECT_EQ(w.told[0].address, 0x000010u);
  EXPECT_EQ(int{w.told[0].cycle}, 3);
  EXPECT_EQ(w.told[1].address, 0x000011u);
  EXPECT_EQ(int{w.told[1].cycle}, 4);
  EXPECT_EQ(w.told[0].kind, CycleKind::DataRead);
}

TEST(SnesAccessWatch, ASixteenBitPushWritesTheHighByteFirst) {
  // PHA with a 16-bit accumulator: the high byte at S on cycle 2, the low byte
  // at S-1 on cycle 3. S is $01FF at power-on.
  Snes m = programMachine({kClc, kXce, kRep, 0x30u, kLdaImm, 0x34u, 0x12u, kPha, kStp});
  toNative16(m);
  m.step();  // LDA #$1234
  Watcher w;
  m.setAccessWatcher(&w);
  m.watchAccess(0x0001FEu, 2, false, /*onWrite=*/true);
  m.step();  // PHA
  ASSERT_EQ(w.writes(), 2u);
  EXPECT_EQ(w.told[0].address, 0x0001FFu);
  EXPECT_EQ(int{w.told[0].value}, 0x12);
  EXPECT_EQ(int{w.told[0].cycle}, 2);
  EXPECT_EQ(w.told[1].address, 0x0001FEu);
  EXPECT_EQ(int{w.told[1].value}, 0x34);
  EXPECT_EQ(int{w.told[1].cycle}, 3);
}

TEST(SnesAccessWatch, ANativeSixteenBitReadModifyWriteWritesBackHighThenLow) {
  // INC !$0010 with a 16-bit accumulator: reads low (3) then high (4), the
  // modify cycle (5) reaches no byte, then writes high (6) then low (7).
  Snes m = programMachine({kClc, kXce, kRep, 0x30u, kIncAbs, 0x10u, 0x00u, kStp});
  toNative16(m);
  SnesState s = m.state();
  s.wram[0x10] = 0xFFu;
  s.wram[0x11] = 0x00u;
  m.restore(s);
  Watcher w;
  m.setAccessWatcher(&w);
  m.watchAccess(0x000010u, 2, /*onRead=*/true, /*onWrite=*/true);
  m.step();  // INC !$0010
  ASSERT_EQ(w.told.size(), 4u);
  EXPECT_EQ(w.told[0].address, 0x000010u);
  EXPECT_EQ(w.told[0].kind, CycleKind::RmwRead);
  EXPECT_EQ(int{w.told[0].cycle}, 3);
  EXPECT_EQ(w.told[1].address, 0x000011u);
  EXPECT_EQ(int{w.told[1].cycle}, 4);
  EXPECT_EQ(w.told[2].address, 0x000011u) << "the high byte is written back first";
  EXPECT_TRUE(w.told[2].write);
  EXPECT_EQ(w.told[2].kind, CycleKind::RmwWrite);
  EXPECT_EQ(int{w.told[2].value}, 0x01);
  EXPECT_EQ(int{w.told[2].cycle}, 6);
  EXPECT_EQ(w.told[3].address, 0x000010u);
  EXPECT_EQ(int{w.told[3].value}, 0x00);
  EXPECT_EQ(int{w.told[3].cycle}, 7);
}

TEST(SnesAccessWatch, AnEmulationModeReadModifyWriteWritesItsAddressTwice) {
  // INC !$0010 in emulation mode: the read (3), then the byte just read written
  // back (4, RmwModifyWrite), then the new byte (5, RmwWrite).
  Snes m = programMachine({kIncAbs, 0x10u, 0x00u, kStp});
  SnesState s = m.state();
  s.wram[0x10] = 0x41u;
  m.restore(s);
  Watcher w;
  m.setAccessWatcher(&w);
  m.watchAccess(0x000010u, 1, /*onRead=*/true, /*onWrite=*/true);
  m.step();
  ASSERT_EQ(w.told.size(), 3u);
  EXPECT_EQ(w.told[0].kind, CycleKind::RmwRead);
  EXPECT_EQ(int{w.told[0].cycle}, 3);
  EXPECT_TRUE(w.told[1].write);
  EXPECT_EQ(w.told[1].kind, CycleKind::RmwModifyWrite);
  EXPECT_EQ(int{w.told[1].value}, 0x41) << "the old byte, written back before the new one";
  EXPECT_EQ(int{w.told[1].cycle}, 4);
  EXPECT_EQ(w.told[2].kind, CycleKind::RmwWrite);
  EXPECT_EQ(int{w.told[2].value}, 0x42);
  EXPECT_EQ(int{w.told[2].cycle}, 5);
  EXPECT_EQ(m.state().wram[0x10], 0x42u);
}

TEST(SnesAccessWatch, VetoingOneByteOfATwoByteStoreTearsIt) {
  // STA !$0010 16-bit writes low (3) then high (4). Vetoing the high byte alone
  // leaves the low byte stored and the high byte as it was.
  struct VetoHigh final : AccessWatcher {
    AccessAnswer read(std::uint32_t, std::uint8_t, AccessSource, CycleKind, std::uint8_t) override {
      return AccessAnswer::proceed();
    }
    AccessAnswer write(std::uint32_t address, std::uint8_t, AccessSource, CycleKind,
                       std::uint8_t) override {
      return address == 0x000011u ? AccessAnswer::veto() : AccessAnswer::proceed();
    }
  };
  Snes m = programMachine({kClc, kXce, kRep, 0x30u, kLdaImm, 0x34u, 0x12u, kStaAbs, 0x10u, 0x00u, kStp});
  toNative16(m);
  m.step();  // LDA #$1234
  SnesState s = m.state();
  s.wram[0x11] = 0xEEu;
  m.restore(s);
  VetoHigh v;
  m.setAccessWatcher(&v);
  m.watchAccess(0x000010u, 2, false, /*onWrite=*/true);
  m.step();  // STA !$0010
  EXPECT_EQ(m.state().wram[0x10], 0x34u);
  EXPECT_EQ(m.state().wram[0x11], 0xEEu) << "torn: the high byte never landed";
}

// The recording oracle: for every opcode in both modes, the core's cycle index
// at each bus access — which the machine passes to the watcher — is the
// position of that cycle in the SingleStepTests recording of the instruction,
// counted from its opcode fetch. A block move runs one byte per seven cycles
// and begins each byte as a fresh instruction, so its index wraps at seven.

std::string vectorsDir() { return SNAGGLETOOTH_65816_VECTORS; }

struct VectorParam {
  std::uint8_t opcode;
  char mode;  // 'n' native, 'e' emulated
};

std::string opcodeFile(const VectorParam& param) {
  char name[16];
  std::snprintf(name, sizeof name, "/%02x.%c.json", param.opcode, param.mode);
  return vectorsDir() + name;
}

std::size_t caseCap() {
  const char* raw = std::getenv("SNAGGLETOOTH_65816_CASE_CAP");
  if (raw == nullptr) return 0;
  long v = std::strtol(raw, nullptr, 10);
  return v > 0 ? static_cast<std::size_t>(v) : 0;
}

bool blockMove(std::uint8_t opcode) { return opcode == 0x54 || opcode == 0x44; }

// A bus that notes, for every cycle the core narrates, the core's cycle index
// as the access happens — the value the machine's watch is told.
struct CycleIndexBus {
  std::unordered_map<std::uint32_t, std::uint8_t> mem;
  const Cpu65816* cpu = nullptr;
  std::vector<std::uint8_t> indexAtCycle;  // one entry per narrated cycle

  std::uint8_t read(std::uint32_t address, CycleKind) {
    indexAtCycle.push_back(cpu->state().tcu);
    const auto it = mem.find(address & 0xFFFFFFu);
    return it == mem.end() ? std::uint8_t{0} : it->second;
  }
  void write(std::uint32_t address, std::uint8_t value, CycleKind) {
    indexAtCycle.push_back(cpu->state().tcu);
    mem[address & 0xFFFFFFu] = value;
  }
  void internal(std::uint32_t) { indexAtCycle.push_back(cpu->state().tcu); }
  void internal(std::uint32_t, CycleKind) { indexAtCycle.push_back(cpu->state().tcu); }
};

Cpu65816State stateOf(const cpu_vectors::RegState& r) {
  return Cpu65816State{.pc = r.pc, .s = r.s, .a = r.a, .x = r.x, .y = r.y, .d = r.d,
                       .p = r.p, .dbr = r.dbr, .pbr = r.pbr, .e = r.e};
}

class AccessWatchCycles : public ::testing::TestWithParam<VectorParam> {};

TEST_P(AccessWatchCycles, TheCycleToldIsTheRecordingsPosition) {
  const VectorParam param = GetParam();
  if (vectorsDir().empty()) {
    if (std::getenv("SNAGGLETOOTH_REQUIRE_65816_VECTORS") != nullptr) {
      FAIL() << "SNAGGLETOOTH_65816_VECTORS is empty but "
                "SNAGGLETOOTH_REQUIRE_65816_VECTORS demands the oracle — configure "
                "with -DSNAGGLETOOTH_65816_VECTORS pointing at the SingleStepTests "
                "65816 'v1' directory.";
    }
    GTEST_SKIP() << "SNAGGLETOOTH_65816_VECTORS is unset — point it at the "
                    "SingleStepTests 65816 'v1' directory to run the vectors.";
  }

  const std::string path = opcodeFile(param);
  const auto text = cpu_vectors::readFile(path);
  ASSERT_TRUE(text.has_value()) << "cannot open vector file: " << path;
  const auto cases = cpu_vectors::parseVectorFile(*text);
  ASSERT_FALSE(cases.empty()) << "no cases in " << path;

  const std::size_t cap = caseCap();
  std::size_t ran = 0;
  for (const cpu_vectors::VectorCase& c : cases) {
    if (cap != 0 && ran >= cap) break;
    ++ran;
    CycleIndexBus bus;
    for (const auto& [address, value] : c.initial.ram) bus.mem[address] = value;
    Cpu65816 cpu(stateOf(c.initial));
    bus.cpu = &cpu;
    std::size_t mismatches = 0;
    std::size_t firstMismatch = 0;
    for (std::size_t i = 0; i < c.cycles.size(); ++i) {
      const std::size_t narrated = bus.indexAtCycle.size();
      cpu.stepCycle(bus);
      if (bus.indexAtCycle.size() == narrated) continue;  // a halted cycle drives nothing
      const std::size_t expected = blockMove(param.opcode) ? i % 7u : i;
      if (bus.indexAtCycle.back() != expected) {
        if (mismatches == 0) firstMismatch = i;
        ++mismatches;
      }
    }
    EXPECT_EQ(mismatches, 0u) << c.name << " (first at cycle " << firstMismatch << ")";
  }
}

std::vector<VectorParam> everyOpcode() {
  std::vector<VectorParam> params;
  for (unsigned opcode = 0; opcode < 256; ++opcode) {
    params.push_back({static_cast<std::uint8_t>(opcode), 'n'});
    params.push_back({static_cast<std::uint8_t>(opcode), 'e'});
  }
  return params;
}

INSTANTIATE_TEST_SUITE_P(EveryOpcode, AccessWatchCycles, ::testing::ValuesIn(everyOpcode()),
                         [](const ::testing::TestParamInfo<VectorParam>& info) {
                           char label[16];
                           std::snprintf(label, sizeof label, "op%02X_%c", info.param.opcode,
                                         info.param.mode);
                           return std::string(label);
                         });

// ---- a read with a side effect has had it when the watch is told -------------

TEST(SnesAccessWatch, AReadOf4210HasClearedTheNmiFlagWhateverTheAnswer) {
  // Two reads of $4210 with the flag set and a veto on both: the first is told
  // the flag and the program receives it; the second finds it cleared, because
  // the read acknowledged it before the watch was told and nothing undoes that.
  Snes m = programMachine({kLdaAbs, 0x10u, 0x42u, kLdaAbs, 0x10u, 0x42u, kStp});
  SnesState s = m.state();
  s.vblankNmi = true;
  m.restore(s);
  Watcher w;
  w.verdict = AccessAnswer::veto();
  m.setAccessWatcher(&w);
  m.watchAccess(0x004210u, 1, /*onRead=*/true, false);
  m.step();
  EXPECT_EQ(static_cast<int>(m.cpuState().a & 0x80u), 0x80) << "the program received the flag";
  m.step();
  ASSERT_EQ(w.reads(), 2u);
  EXPECT_EQ(static_cast<int>(w.told[0].value & 0x80u), 0x80);
  EXPECT_EQ(static_cast<int>(w.told[1].value & 0x80u), 0x00)
      << "the first read cleared it; the veto could not";
  EXPECT_FALSE(m.state().vblankNmi);
}

TEST(SnesAccessWatch, ASubstitutedReadOf4210StillClearsTheNmiFlag) {
  Snes m = programMachine({kLdaAbs, 0x10u, 0x42u, kStp});
  SnesState s = m.state();
  s.vblankNmi = true;
  m.restore(s);
  Watcher w;
  w.verdict = AccessAnswer::instead(0x00u);
  m.setAccessWatcher(&w);
  m.watchAccess(0x004210u, 1, true, false);
  m.step();
  EXPECT_EQ(static_cast<int>(m.cpuState().a & 0xFFu), 0x00) << "the program received the substitute";
  EXPECT_FALSE(m.state().vblankNmi) << "and the flag cleared all the same";
}

// ---- arming shapes: spans, idempotence, aliases, disarming ------------------

TEST(SnesAccessWatch, ASpanArmsEveryByteInIt) {
  Snes m = programMachine({kLdaAbs, 0x12u, 0x00u, kStp});  // LDA !$0012 (inside the span)
  SnesState s = m.state();
  s.wram[0x12] = 0x44u;
  m.restore(s);
  Watcher w;
  m.setAccessWatcher(&w);
  m.watchAccess(0x000010u, 4, /*onRead=*/true, false);  // $0010-$0013
  m.step();
  ASSERT_EQ(w.reads(), 1u);
  EXPECT_EQ(w.firstFrom(AccessSource::Cpu)->address, 0x000012u);
}

TEST(SnesAccessWatch, AByteOutsideTheSpanIsNotArmed) {
  Snes m = programMachine({kLdaAbs, 0x14u, 0x00u, kStp});  // LDA !$0014 (past the span)
  SnesState s = m.state();
  s.wram[0x14] = 0x44u;
  m.restore(s);
  Watcher w;
  m.setAccessWatcher(&w);
  m.watchAccess(0x000010u, 4, true, false);  // $0010-$0013 only
  m.step();
  EXPECT_EQ(w.reads(), 0u);
}

TEST(SnesAccessWatch, ArmingTwiceIsIdempotentSoOneDisarmClearsIt) {
  Snes m = programMachine({kLdaAbs, 0x10u, 0x00u, kStp});
  SnesState s = m.state();
  s.wram[0x10] = 0x5Au;
  m.restore(s);
  Watcher w;
  m.setAccessWatcher(&w);
  m.watchAccess(0x000010u, 1, true, false);
  m.watchAccess(0x000010u, 1, true, false);  // arming again does nothing
  m.unwatchAccess(0x000010u, 1, true, false);
  m.step();
  EXPECT_EQ(w.reads(), 0u) << "one disarm cleared the place a double-arm left armed once";
}

TEST(SnesAccessWatch, ArmingThroughTwoAliasesIsOneArmAndDisarmingThroughAThirdClearsIt) {
  Snes m = programMachine({kLdaAbs, 0x80u, 0x21u, kStp});  // LDA !$2180: the port reads $7E:0010
  SnesState s = m.state();
  s.wmadd = 0x000010u;
  m.restore(s);
  Watcher w;
  m.setAccessWatcher(&w);
  m.watchAccess(0x7E0010u, 1, true, false);
  m.watchAccess(0x000010u, 1, true, false);    // the same byte: nothing more is armed
  m.unwatchAccess(0xBF0010u, 1, true, false);  // and one disarm through a third alias clears it
  m.step();
  EXPECT_EQ(w.reads(), 0u);
}

TEST(SnesAccessWatch, ADisarmedMachineArmsAgainAndIsToldAgain) {
  // Disarming the last byte and arming another: the watch works from a fresh
  // table exactly as from the first.
  Snes m = programMachine({kLdaAbs, 0x10u, 0x00u, kLdaAbs, 0x20u, 0x00u, kStp});
  SnesState s = m.state();
  s.wram[0x10] = 0x11u;
  s.wram[0x20] = 0x22u;
  m.restore(s);
  Watcher w;
  m.setAccessWatcher(&w);
  m.watchAccess(0x000010u, 1, true, false);
  m.unwatchAccess(0x7E0010u, 1, true, false);  // the last byte, through an alias
  m.step();                                     // LDA !$0010: not told
  m.watchAccess(0x000020u, 1, true, false);
  m.step();                                     // LDA !$0020: told
  ASSERT_EQ(w.reads(), 1u);
  EXPECT_EQ(w.told[0].address, 0x000020u);
  EXPECT_EQ(int{w.told[0].value}, 0x22);
}

TEST(SnesAccessWatch, ArmingARegisterAloneLeavesAnImageFetchUntold) {
  // With only a register armed, the program's fetches from the image and its
  // loads from work RAM go untold; the register alone is heard.
  Snes m = programMachine({kLdaAbs, 0x10u, 0x00u, kLdaAbs, 0x40u, 0x21u, kStp});
  Watcher w;
  m.setAccessWatcher(&w);
  m.watchAccess(0x002140u, 1, true, false);
  m.step();
  m.step();
  ASSERT_EQ(w.reads(), 1u);
  EXPECT_EQ(w.told[0].address, 0x002140u);
}

// ---- the machine that watches nothing behaves exactly as it does today -------

TEST(SnesAccessWatch, WatcherSetNothingArmedRunsByteIdenticalToAPlainRun) {
  Snes a = programMachine({kLdaImm, 0x77u, kStaAbs, 0x20u, 0x00u, kNop, kBra, 0xFCu});  // store, then loop
  Snes b = programMachine({kLdaImm, 0x77u, kStaAbs, 0x20u, 0x00u, kNop, kBra, 0xFCu});
  Watcher w;
  a.setAccessWatcher(&w);  // set, but nothing armed
  a.run(200000u);
  b.run(200000u);
  EXPECT_EQ(w.told.size(), 0u);
  EXPECT_TRUE(a.state() == b.state());
  EXPECT_EQ(a.takeFrames(), b.takeFrames());
}

TEST(SnesAccessWatch, AWatchedRunSpendsTheSameCyclesAsAPlainRun) {
  // A watch answered on every access changes nothing about time: the same
  // program run to the same budget lands on the same master count, the same
  // beam position and the same audio, whatever the watcher answered.
  Snes a = programMachine({kLdaImm, 0x77u, kStaAbs, 0x20u, 0x00u, kNop, kBra, 0xFCu});
  Snes b = programMachine({kLdaImm, 0x77u, kStaAbs, 0x20u, 0x00u, kNop, kBra, 0xFCu});
  Watcher w;
  w.verdict = AccessAnswer::instead(0x77u);  // the same byte the program stores
  a.setAccessWatcher(&w);
  a.watchAccess(0x000020u, 1, true, true);
  a.run(200000u);
  b.run(200000u);
  EXPECT_GT(w.told.size(), 0u);
  EXPECT_TRUE(a.state() == b.state());
  EXPECT_EQ(a.takeFrames(), b.takeFrames());
}

TEST(SnesAccessWatch, APlaceAndAnInstructionArmedButNeverReachedRunByteIdenticalToNothingArmed) {
  // One work-RAM byte and one image instruction armed, neither of which the
  // loop reaches: the machine classifies every access through the same table
  // as the plain machine reads its bus through, and lands on the same state and
  // the same audio to the same budget.
  Snes a = programMachine({kLdaImm, 0x77u, kStaAbs, 0x20u, 0x00u, kNop, kBra, 0xF8u});  // store, then loop
  Snes b = programMachine({kLdaImm, 0x77u, kStaAbs, 0x20u, 0x00u, kNop, kBra, 0xF8u});
  Watcher w;
  a.setAccessWatcher(&w);
  a.watchAccess(0x7E1F00u, 1, true, true);
  a.watchInstruction(0x00FF00u, Standin::None);
  a.run(200000u);
  b.run(200000u);
  EXPECT_EQ(w.told.size(), 0u);
  EXPECT_TRUE(a.state() == b.state());
  EXPECT_EQ(a.takeFrames(), b.takeFrames());
}

TEST(SnesAccessWatch, TheWatchSurvivesAMove) {
  Snes m = programMachine({kLdaAbs, 0x10u, 0x00u, kStp});
  SnesState s = m.state();
  s.wram[0x10] = 0x5Au;
  m.restore(s);
  Watcher w;
  w.verdict = AccessAnswer::instead(0x88u);
  m.setAccessWatcher(&w);
  m.watchAccess(0x000010u, 1, true, false);
  Snes moved = std::move(m);
  moved.step();  // the armed read still reaches the watcher on the moved machine
  EXPECT_EQ(static_cast<int>(moved.cpuState().a & 0xFFu), 0x88);
}

}  // namespace
}  // namespace snaggletooth
