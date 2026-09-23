// The console's access watch: a host is told, before a watched access takes
// effect, that the program is reading or writing a place it armed, and answers
// what happens — let it through, prevent it, or substitute a byte. Each case
// drives a real access (a CPU instruction, a transfer, the work-RAM port) and
// reads back the byte the program saw or the byte that landed — never "the
// symbol exists". The source cases hold that the watcher is told which part of
// the machine made the access: the CPU, either transfer engine, and the
// work-RAM port. The last cases pin the machine that watches nothing: with a
// watcher set and no place armed it runs a fixed program to a fixed budget and
// lands byte-identical to a plain machine, and the watch survives a move.

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

// One access the watcher was told about.
struct Told {
  std::uint32_t address = 0;
  std::uint8_t value = 0;
  AccessSource source = AccessSource::Cpu;
  bool write = false;
};

// A watcher that records every access it is told and answers each with a fixed
// verdict the test sets. proceed() by default.
struct Watcher final : Snes::AccessWatcher {
  std::vector<Told> told;
  Snes::AccessAnswer verdict = Snes::AccessAnswer::proceed();

  Snes::AccessAnswer read(std::uint32_t address, std::uint8_t value, AccessSource source) override {
    told.push_back({address, value, source, false});
    return verdict;
  }
  Snes::AccessAnswer write(std::uint32_t address, std::uint8_t value, AccessSource source) override {
    told.push_back({address, value, source, true});
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
};

constexpr std::uint8_t kLdaImm = 0xA9u;
constexpr std::uint8_t kLdaAbs = 0xADu;
constexpr std::uint8_t kStaAbs = 0x8Du;
constexpr std::uint8_t kNop = 0xEAu;
constexpr std::uint8_t kStp = 0xDBu;

// A machine running `program` from $8000 in a one-bank LoROM image.
Snes programMachine(std::initializer_list<std::uint8_t> program) {
  std::vector<std::uint8_t> rom(program.begin(), program.end());
  rom.resize(0x8000u, 0x00u);
  rom[0x7FFCu] = 0x00u;  // reset -> $8000
  rom[0x7FFDu] = 0x80u;
  return Snes(SnesConfig{.rom = rom});
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
  w.verdict = Snes::AccessAnswer::instead(0x99u);
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
  w.verdict = Snes::AccessAnswer::veto();
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
  w.verdict = Snes::AccessAnswer::instead(0x33u);
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
  w.verdict = Snes::AccessAnswer::veto();
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

TEST(SnesAccessWatch, ATransferAccessIsToldWithSourceDma) {
  // A general-purpose transfer of one byte reads $00:9000 (A bus) and writes it
  // through $2180 (B bus). Arming the read side hears the engine's read.
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
}

TEST(SnesAccessWatch, AnHdmaAccessIsToldWithSourceHdma) {
  // Channel 0 delivers a one-line direct table to $2100; arming the table's count
  // byte hears the HDMA engine's read at initialisation.
  Snes m = programMachine({kNop, 0x80u, 0xFDu});  // NOP ; BRA back
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
}

TEST(SnesAccessWatch, TheWorkRamPortsOwnAccessIsToldWithSourceWramPort) {
  // The CPU reads $2180; the port reads work RAM at its own 24-bit address. That
  // read is the work-RAM port's, and arming it hears the port, not the CPU.
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
}

TEST(SnesAccessWatch, AWorkRamPortReadCanBeSubstituted) {
  // A substitute at the port's own read changes the byte $2180 delivers to the CPU.
  Snes m = programMachine({kLdaAbs, 0x80u, 0x21u, kStp});  // LDA !$2180
  SnesState s = m.state();
  s.wmadd = 0x000010u;
  s.wram[0x10] = 0x3Cu;
  m.restore(s);
  Watcher w;
  w.verdict = Snes::AccessAnswer::instead(0xE7u);
  m.setAccessWatcher(&w);
  m.watchAccess(0x7E0010u, 1, true, false);
  m.step();
  EXPECT_EQ(static_cast<int>(m.cpuState().a & 0xFFu), 0xE7);
  EXPECT_EQ(m.state().wram[0x10], 0x3Cu) << "the RAM beneath is untouched";
}

// ---- arming shapes: spans, idempotence, banks --------------------------------

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

TEST(SnesAccessWatch, TheTableTellsBanksApartByTheirBankByte) {
  // The port reads $7E:0010; arming the same offset in bank $00 must not fire on
  // it. The two live in different bank slots of the two-level table.
  Snes m = programMachine({kLdaAbs, 0x80u, 0x21u, kStp});  // LDA !$2180
  SnesState s = m.state();
  s.wmadd = 0x000010u;  // the port reads $7E:0010
  s.wram[0x10] = 0x3Cu;
  m.restore(s);
  Watcher w;
  m.setAccessWatcher(&w);
  m.watchAccess(0x000010u, 1, /*onRead=*/true, false);  // bank $00, offset $0010 -- a different bank
  m.step();
  EXPECT_EQ(w.reads(), 0u) << "a bank-$00 arm does not fire on a bank-$7E access at the same offset";
}

// ---- the machine that watches nothing behaves exactly as it does today -------

TEST(SnesAccessWatch, WatcherSetNothingArmedRunsByteIdenticalToAPlainRun) {
  Snes a = programMachine({kLdaImm, 0x77u, kStaAbs, 0x20u, 0x00u, kNop, 0x80u, 0xFCu});  // store, then loop
  Snes b = programMachine({kLdaImm, 0x77u, kStaAbs, 0x20u, 0x00u, kNop, 0x80u, 0xFCu});
  Watcher w;
  a.setAccessWatcher(&w);  // set, but nothing armed
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
  w.verdict = Snes::AccessAnswer::instead(0x88u);
  m.setAccessWatcher(&w);
  m.watchAccess(0x000010u, 1, true, false);
  Snes moved = std::move(m);
  moved.step();  // the armed read still reaches the watcher on the moved machine
  EXPECT_EQ(static_cast<int>(moved.cpuState().a & 0xFFu), 0x88);
}

}  // namespace
}  // namespace snaggletooth
