// The audio machine's access watch, in its own 16-bit vocabulary: a host is
// told, before a watched access takes effect, that the sound CPU is reading or
// writing a place it armed, and answers what happens — let it through, prevent
// it, or substitute a byte. The sound CPU is the only thing on this bus, so the
// watcher is told an address and no source. Each case drives a real SPC700
// instruction and reads back the byte the CPU saw or the byte that landed. The
// last cases pin the machine that watches nothing: with a watcher set and no
// place armed it runs a fixed program to a fixed budget and lands byte-identical
// to a plain machine, and the watch survives a move.

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "snaggletooth/apu/apu.h"

namespace {

using snaggletooth::Apu;
using snaggletooth::StereoFrame;

constexpr std::uint8_t kMovAImm = 0xE8u;  // MOV A,#imm
constexpr std::uint8_t kMovAAbs = 0xE5u;  // MOV A,!abs   (opcode, abs low, abs high)
constexpr std::uint8_t kMovAbsA = 0xC5u;  // MOV !abs,A
constexpr std::uint8_t kBra = 0x2Fu;      // BRA rel

// One access the watcher was told about.
struct Told {
  std::uint16_t address = 0;
  std::uint8_t value = 0;
  bool write = false;
};

// A watcher that records every access it is told and answers each with a fixed
// verdict the test sets. proceed() by default.
struct Watcher final : Apu::AccessWatcher {
  std::vector<Told> told;
  Apu::AccessAnswer verdict = Apu::AccessAnswer::proceed();

  Apu::AccessAnswer read(std::uint16_t address, std::uint8_t value) override {
    told.push_back({address, value, false});
    return verdict;
  }
  Apu::AccessAnswer write(std::uint16_t address, std::uint8_t value) override {
    told.push_back({address, value, true});
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
  [[nodiscard]] const Told* firstWriteTo(std::uint16_t address) const {
    for (const Told& t : told)
      if (t.write && t.address == address) return &t;
    return nullptr;
  }
  [[nodiscard]] const Told* firstReadOf(std::uint16_t address) const {
    for (const Told& t : told)
      if (!t.write && t.address == address) return &t;
    return nullptr;
  }
};

// A machine holding `code` at $0300 with the CPU pointed there, not yet stepped.
Apu loaded(std::initializer_list<std::uint8_t> code, std::uint16_t at = 0x0300u) {
  Apu apu;
  std::uint16_t a = at;
  for (std::uint8_t byte : code) apu.writeRam(a++, byte);
  apu.setPc(at);
  return apu;
}

// ---- nothing armed: the watcher is not told, and the machine is unchanged ----

TEST(ApuAccessWatch, NothingArmedIsNotTold) {
  Apu apu = loaded({kMovAImm, 0x77u, kMovAbsA, 0x50u, 0x02u});  // MOV A,#$77 ; MOV $0250,A
  Watcher w;
  apu.setAccessWatcher(&w);
  EXPECT_EQ(apu.accessWatcher(), &w);
  apu.step();  // MOV A,#$77
  apu.step();  // MOV $0250,A -- real reads (fetches) and a write, but nothing is armed
  EXPECT_EQ(w.told.size(), 0u);
  EXPECT_EQ(apu.readRam(0x0250u), 0x77u);  // the write landed as it always would
}

// ---- reads: proceed, substitute, veto ----------------------------------------

TEST(ApuAccessWatch, AProceededReadDeliversTheMachinesByte) {
  Apu apu = loaded({kMovAAbs, 0x50u, 0x02u});  // MOV A,$0250
  apu.writeRam(0x0250u, 0x5Au);
  Watcher w;
  apu.setAccessWatcher(&w);
  apu.watchAccess(0x0250u, 1, /*onRead=*/true, /*onWrite=*/false);
  apu.step();
  const Told* t = w.firstReadOf(0x0250u);
  ASSERT_NE(t, nullptr);
  EXPECT_EQ(int{t->value}, 0x5A) << "the byte the machine would have answered";
  EXPECT_EQ(int{apu.cpuState().a}, 0x5A) << "and the CPU received it unchanged";
}

TEST(ApuAccessWatch, AnInsteadReadDeliversTheSubstitute) {
  Apu apu = loaded({kMovAAbs, 0x50u, 0x02u});
  apu.writeRam(0x0250u, 0x5Au);
  Watcher w;
  w.verdict = Apu::AccessAnswer::instead(0x99u);
  apu.setAccessWatcher(&w);
  apu.watchAccess(0x0250u, 1, true, false);
  apu.step();
  EXPECT_EQ(int{apu.cpuState().a}, 0x99) << "the CPU received the substitute";
  EXPECT_EQ(apu.readRam(0x0250u), 0x5Au) << "the RAM the watch stood in front of is untouched";
}

TEST(ApuAccessWatch, AVetoedReadStillDeliversTheMachinesByte) {
  Apu apu = loaded({kMovAAbs, 0x50u, 0x02u});
  apu.writeRam(0x0250u, 0x5Au);
  Watcher w;
  w.verdict = Apu::AccessAnswer::veto();
  apu.setAccessWatcher(&w);
  apu.watchAccess(0x0250u, 1, true, false);
  apu.step();
  EXPECT_EQ(int{apu.cpuState().a}, 0x5A);
}

// ---- writes: proceed, substitute, veto ---------------------------------------

TEST(ApuAccessWatch, AProceededWriteStoresTheValue) {
  Apu apu = loaded({kMovAImm, 0x77u, kMovAbsA, 0x50u, 0x02u});
  Watcher w;
  apu.setAccessWatcher(&w);
  apu.watchAccess(0x0250u, 1, false, /*onWrite=*/true);
  apu.step();  // MOV A,#$77
  apu.step();  // MOV $0250,A
  const Told* t = w.firstWriteTo(0x0250u);
  ASSERT_NE(t, nullptr);
  EXPECT_EQ(int{t->value}, 0x77);
  EXPECT_EQ(apu.readRam(0x0250u), 0x77u);
}

TEST(ApuAccessWatch, AnInsteadWriteStoresTheSubstitute) {
  Apu apu = loaded({kMovAImm, 0x77u, kMovAbsA, 0x50u, 0x02u});
  Watcher w;
  w.verdict = Apu::AccessAnswer::instead(0x33u);
  apu.setAccessWatcher(&w);
  apu.watchAccess(0x0250u, 1, false, true);
  apu.step();
  apu.step();
  EXPECT_EQ(apu.readRam(0x0250u), 0x33u) << "the substitute landed, not the driven value";
}

TEST(ApuAccessWatch, AVetoedWriteStoresNothing) {
  Apu apu = loaded({kMovAImm, 0x77u, kMovAbsA, 0x50u, 0x02u});
  apu.writeRam(0x0250u, 0xC5u);  // a byte already there
  Watcher w;
  w.verdict = Apu::AccessAnswer::veto();
  apu.setAccessWatcher(&w);
  apu.watchAccess(0x0250u, 1, false, true);
  apu.step();
  apu.step();
  EXPECT_EQ(apu.readRam(0x0250u), 0xC5u) << "the write was prevented; the byte stands";
}

// ---- the two directions are armed independently ------------------------------

TEST(ApuAccessWatch, ReadAndWriteAreArmedIndependently) {
  Apu apu = loaded({kMovAImm, 0x77u, kMovAbsA, 0x50u, 0x02u});
  Watcher w;
  apu.setAccessWatcher(&w);
  apu.watchAccess(0x0250u, 1, /*onRead=*/true, /*onWrite=*/false);  // reads only
  apu.step();  // MOV A,#$77
  apu.step();  // MOV $0250,A -- a write to the armed place, but only reads are armed
  EXPECT_EQ(w.writes(), 0u);
  EXPECT_EQ(apu.readRam(0x0250u), 0x77u);
}

// ---- arming shapes: spans, idempotence ---------------------------------------

TEST(ApuAccessWatch, ASpanArmsEveryByteInIt) {
  Apu apu = loaded({kMovAAbs, 0x52u, 0x02u});  // MOV A,$0252 (inside the span)
  apu.writeRam(0x0252u, 0x44u);
  Watcher w;
  apu.setAccessWatcher(&w);
  apu.watchAccess(0x0250u, 4, /*onRead=*/true, false);  // $0250-$0253
  apu.step();
  ASSERT_NE(w.firstReadOf(0x0252u), nullptr);
}

TEST(ApuAccessWatch, AByteOutsideTheSpanIsNotArmed) {
  Apu apu = loaded({kMovAAbs, 0x54u, 0x02u});  // MOV A,$0254 (past the span)
  apu.writeRam(0x0254u, 0x44u);
  Watcher w;
  apu.setAccessWatcher(&w);
  apu.watchAccess(0x0250u, 4, true, false);  // $0250-$0253 only
  apu.step();
  EXPECT_EQ(w.firstReadOf(0x0254u), nullptr);
}

TEST(ApuAccessWatch, ArmingTwiceIsIdempotentSoOneDisarmClearsIt) {
  Apu apu = loaded({kMovAAbs, 0x50u, 0x02u});
  apu.writeRam(0x0250u, 0x5Au);
  Watcher w;
  apu.setAccessWatcher(&w);
  apu.watchAccess(0x0250u, 1, true, false);
  apu.watchAccess(0x0250u, 1, true, false);  // again does nothing
  apu.unwatchAccess(0x0250u, 1, true, false);
  apu.step();
  EXPECT_EQ(w.reads(), 0u) << "one disarm cleared the place a double-arm left armed once";
}

// ---- the machine that watches nothing behaves exactly as it does today -------

TEST(ApuAccessWatch, WatcherSetNothingArmedRunsByteIdenticalToAPlainRun) {
  // MOV A,#$77 ; MOV $0250,A ; BRA back -- a loop that reads and writes.
  const std::initializer_list<std::uint8_t> code = {kMovAImm, 0x77u, kMovAbsA,
                                                     0x50u,    0x02u, kBra, 0xF9u};
  Apu a = loaded(code);
  Apu b = loaded(code);
  Watcher w;
  a.setAccessWatcher(&w);  // set, but nothing armed
  a.run(50000u);
  b.run(50000u);
  EXPECT_EQ(w.told.size(), 0u);
  EXPECT_TRUE(a.state() == b.state());
  EXPECT_EQ(a.takeFrames(), b.takeFrames());
}

TEST(ApuAccessWatch, TheWatchSurvivesAMove) {
  Apu apu = loaded({kMovAAbs, 0x50u, 0x02u});
  apu.writeRam(0x0250u, 0x5Au);
  Watcher w;
  w.verdict = Apu::AccessAnswer::instead(0x88u);
  apu.setAccessWatcher(&w);
  apu.watchAccess(0x0250u, 1, true, false);
  Apu moved = std::move(apu);
  moved.step();  // the armed read still reaches the watcher on the moved machine
  EXPECT_EQ(int{moved.cpuState().a}, 0x88);
}

}  // namespace
