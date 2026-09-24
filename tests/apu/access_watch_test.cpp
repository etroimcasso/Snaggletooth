// The audio machine's access watch, in its own 16-bit vocabulary: a host is
// told, before a watched access takes effect, that the sound CPU is reading or
// writing a place it armed, and answers what happens — let it through, prevent
// it, or substitute a byte. The sound CPU is the only thing on this bus, so the
// watcher is told an address and no source, and the bus carries no kind, so an
// opcode fetch and a data read are told apart by the cycle alone. Each case
// drives a real SPC700 instruction and reads back the byte the CPU saw or the
// byte that landed. The cycle cases pin which cycle of the instruction each
// access is told as — a load's read, a store's read of its destination before
// its write, a word load's low and high bytes — and hold the number to the
// SingleStepTests recordings for every opcode through the core's cycle index,
// which is what the machine reports. The wrinkle case pins a timer output
// cleared by the time the watch is told. The last cases pin the machine that
// watches nothing: with a watcher set and no place armed it runs a fixed
// program to a fixed budget and lands byte-identical to a plain machine, and
// the watch survives a move.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "../spc700/vector_harness.h"
#include "snaggletooth/apu/apu.h"

#ifndef SNAGGLETOOTH_SPC700_VECTORS
#define SNAGGLETOOTH_SPC700_VECTORS ""
#endif

namespace {

using snaggletooth::AccessAnswer;
using snaggletooth::Apu;
using snaggletooth::ApuAccessWatcher;
using snaggletooth::ApuState;
using snaggletooth::Spc700;
using snaggletooth::Spc700State;
using snaggletooth::StereoFrame;

constexpr std::uint8_t kMovAImm = 0xE8u;  // MOV A,#imm
constexpr std::uint8_t kMovAAbs = 0xE5u;  // MOV A,!abs   (opcode, abs low, abs high)
constexpr std::uint8_t kMovAbsA = 0xC5u;  // MOV !abs,A
constexpr std::uint8_t kMovADp = 0xE4u;   // MOV A,dp
constexpr std::uint8_t kMovwYaDp = 0xBAu; // MOVW YA,dp
constexpr std::uint8_t kBra = 0x2Fu;      // BRA rel

// One access the watcher was told about.
struct Told {
  std::uint16_t address = 0;
  std::uint8_t value = 0;
  std::uint8_t cycle = 0;
  bool write = false;
};

// A watcher that records every access it is told and answers each with a fixed
// verdict the test sets. proceed() by default.
struct Watcher final : ApuAccessWatcher {
  std::vector<Told> told;
  AccessAnswer verdict = AccessAnswer::proceed();

  AccessAnswer read(std::uint16_t address, std::uint8_t value, std::uint8_t cycle) override {
    told.push_back({address, value, cycle, false});
    return verdict;
  }
  AccessAnswer write(std::uint16_t address, std::uint8_t value, std::uint8_t cycle) override {
    told.push_back({address, value, cycle, true});
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
  w.verdict = AccessAnswer::instead(0x99u);
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
  w.verdict = AccessAnswer::veto();
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
  w.verdict = AccessAnswer::instead(0x33u);
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
  w.verdict = AccessAnswer::veto();
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
  apu.step();  // MOV $0250,A -- a store reads its destination first, then writes it
  EXPECT_EQ(w.writes(), 0u);
  EXPECT_EQ(w.reads(), 1u) << "the store's own read of its destination is a read";
  EXPECT_EQ(apu.readRam(0x0250u), 0x77u);
}

// ---- the watcher is told which cycle of the instruction the access is --------

TEST(ApuAccessWatch, ALoadsReadIsCycleThreeAfterTheOpcodeAndTwoOperandBytes) {
  Apu apu = loaded({kMovAAbs, 0x50u, 0x02u});  // MOV A,!$0250
  Watcher w;
  apu.setAccessWatcher(&w);
  apu.watchAccess(0x0250u, 1, true, false);
  apu.step();
  ASSERT_EQ(w.reads(), 1u);
  EXPECT_EQ(int{w.told[0].cycle}, 3);
}

TEST(ApuAccessWatch, AnOpcodeFetchAndADataReadOfOneAddressAreToldApartByCycle) {
  // MOV A,!$0300 at $0300 reads its own opcode byte as data: cycle 0 is the
  // fetch, cycle 3 the load, at one address.
  Apu apu = loaded({kMovAAbs, 0x00u, 0x03u});
  Watcher w;
  apu.setAccessWatcher(&w);
  apu.watchAccess(0x0300u, 1, true, false);
  apu.step();
  ASSERT_EQ(w.reads(), 2u);
  EXPECT_EQ(int{w.told[0].cycle}, 0);
  EXPECT_EQ(int{w.told[1].cycle}, 3);
  EXPECT_EQ(int{w.told[1].value}, int{kMovAAbs}) << "the program read its own opcode";
}

TEST(ApuAccessWatch, AStoreReadsItsDestinationOnCycleThreeAndWritesOnFour) {
  Apu apu = loaded({kMovAImm, 0x77u, kMovAbsA, 0x50u, 0x02u});
  apu.step();  // MOV A,#$77
  Watcher w;
  apu.setAccessWatcher(&w);
  apu.watchAccess(0x0250u, 1, /*onRead=*/true, /*onWrite=*/true);
  apu.step();  // MOV !$0250,A
  ASSERT_EQ(w.told.size(), 2u);
  EXPECT_FALSE(w.told[0].write);
  EXPECT_EQ(int{w.told[0].cycle}, 3);
  EXPECT_TRUE(w.told[1].write);
  EXPECT_EQ(int{w.told[1].cycle}, 4);
  EXPECT_EQ(int{w.told[1].value}, 0x77);
}

TEST(ApuAccessWatch, AWordLoadReadsLowOnCycleTwoAndHighOnCycleFour) {
  // MOVW YA,dp spends a cycle inside the chip between its two bytes.
  Apu apu = loaded({kMovwYaDp, 0x50u});  // MOVW YA,$50
  apu.writeRam(0x0050u, 0x34u);
  apu.writeRam(0x0051u, 0x12u);
  Watcher w;
  apu.setAccessWatcher(&w);
  apu.watchAccess(0x0050u, 2, true, false);
  apu.step();
  ASSERT_EQ(w.reads(), 2u);
  EXPECT_EQ(w.told[0].address, 0x0050u);
  EXPECT_EQ(int{w.told[0].cycle}, 2);
  EXPECT_EQ(w.told[1].address, 0x0051u);
  EXPECT_EQ(int{w.told[1].cycle}, 4);
  EXPECT_EQ(int{apu.cpuState().a}, 0x34);
  EXPECT_EQ(int{apu.cpuState().y}, 0x12);
}

// The recording oracle: for every opcode, the core's cycle index at each bus
// access — which the machine passes to the watcher — is the position of that
// cycle in the SingleStepTests recording of the instruction, counted from its
// opcode fetch.

std::string vectorsDir() { return SNAGGLETOOTH_SPC700_VECTORS; }

std::string opcodeFile(std::uint8_t opcode) {
  char name[16];
  std::snprintf(name, sizeof name, "/%02x.json", opcode);
  return vectorsDir() + name;
}

std::size_t caseCap() {
  const char* raw = std::getenv("SNAGGLETOOTH_SPC700_CASE_CAP");
  if (raw == nullptr) return 0;
  long v = std::strtol(raw, nullptr, 10);
  return v > 0 ? static_cast<std::size_t>(v) : 0;
}

// A flat 64 KB bus that notes, for every cycle the core narrates, the core's
// cycle index as the access happens — the value the machine's watch is told.
struct CycleIndexBus {
  std::array<std::uint8_t, 65536> ram{};
  const Spc700* cpu = nullptr;
  std::vector<std::uint8_t> indexAtCycle;

  std::uint8_t read(std::uint16_t address) {
    indexAtCycle.push_back(cpu->state().tcu);
    return ram[address];
  }
  void write(std::uint16_t address, std::uint8_t value) {
    indexAtCycle.push_back(cpu->state().tcu);
    ram[address] = value;
  }
};

class ApuAccessWatchCycles : public ::testing::TestWithParam<std::uint8_t> {};

TEST_P(ApuAccessWatchCycles, TheCycleToldIsTheRecordingsPosition) {
  const std::uint8_t opcode = GetParam();
  if (vectorsDir().empty()) {
    if (std::getenv("SNAGGLETOOTH_REQUIRE_VECTORS") != nullptr) {
      FAIL() << "SNAGGLETOOTH_SPC700_VECTORS is empty but SNAGGLETOOTH_REQUIRE_VECTORS "
                "demands the oracle — configure with -DSNAGGLETOOTH_SPC700_VECTORS "
                "pointing at the SingleStepTests SPC700 'v1' directory.";
    }
    GTEST_SKIP() << "SNAGGLETOOTH_SPC700_VECTORS is unset — point it at the "
                    "SingleStepTests SPC700 'v1' directory to run the vectors.";
  }

  const std::string path = opcodeFile(opcode);
  const auto text = snaggletooth::test::readFile(path);
  ASSERT_TRUE(text.has_value()) << "cannot open vector file: " << path;
  const auto cases = snaggletooth::test::parseVectorFile(*text);
  ASSERT_FALSE(cases.empty()) << "no cases in " << path;

  const std::size_t cap = caseCap();
  std::size_t ran = 0;
  for (const snaggletooth::test::VectorCase& c : cases) {
    if (cap != 0 && ran >= cap) break;
    ++ran;
    CycleIndexBus bus;
    for (const auto& [address, value] : c.initial.ram) bus.ram[address] = value;
    Spc700 cpu(Spc700State{.pc = c.initial.pc, .a = c.initial.a, .x = c.initial.x,
                           .y = c.initial.y, .sp = c.initial.sp, .psw = c.initial.psw});
    bus.cpu = &cpu;
    std::size_t mismatches = 0;
    std::size_t firstMismatch = 0;
    for (std::size_t i = 0; i < c.cycles.size(); ++i) {
      const std::size_t narrated = bus.indexAtCycle.size();
      cpu.stepCycle(bus);
      if (bus.indexAtCycle.size() == narrated) continue;  // a wait cycle drives nothing
      if (bus.indexAtCycle.back() != i) {
        if (mismatches == 0) firstMismatch = i;
        ++mismatches;
      }
    }
    EXPECT_EQ(mismatches, 0u) << c.name << " (first at cycle " << firstMismatch << ")";
  }
}

std::vector<std::uint8_t> everyOpcode() {
  std::vector<std::uint8_t> params;
  for (unsigned opcode = 0; opcode < 256; ++opcode) params.push_back(static_cast<std::uint8_t>(opcode));
  return params;
}

INSTANTIATE_TEST_SUITE_P(EveryOpcode, ApuAccessWatchCycles, ::testing::ValuesIn(everyOpcode()),
                         [](const ::testing::TestParamInfo<std::uint8_t>& info) {
                           char label[8];
                           std::snprintf(label, sizeof label, "op%02X", info.param);
                           return std::string(label);
                         });

// ---- a read with a side effect has had it when the watch is told -------------

TEST(ApuAccessWatch, AReadOfATimerOutputHasClearedItWhateverTheAnswer) {
  // T0OUT holds 5; the program reads $FD twice with a veto on both. The first
  // read is told 5 and the CPU receives it; the second finds 0, because the
  // read cleared the output before the watch was told and nothing undoes that.
  Apu apu = loaded({kMovADp, 0xFDu, kMovADp, 0xFDu});  // MOV A,$FD ; MOV A,$FD
  ApuState s = apu.state();
  s.timers[0].stage3 = 5u;
  apu.restore(s);
  Watcher w;
  w.verdict = AccessAnswer::veto();
  apu.setAccessWatcher(&w);
  apu.watchAccess(0x00FDu, 1, /*onRead=*/true, false);
  apu.step();
  EXPECT_EQ(int{apu.cpuState().a}, 5) << "the program received the count";
  apu.step();
  ASSERT_EQ(w.reads(), 2u);
  EXPECT_EQ(int{w.told[0].value}, 5);
  EXPECT_EQ(int{w.told[1].value}, 0) << "the first read cleared it; the veto could not";
  EXPECT_EQ(int{apu.state().timers[0].stage3}, 0);
}

TEST(ApuAccessWatch, ASubstitutedReadOfATimerOutputStillClearsIt) {
  Apu apu = loaded({kMovADp, 0xFDu});
  ApuState s = apu.state();
  s.timers[0].stage3 = 5u;
  apu.restore(s);
  Watcher w;
  w.verdict = AccessAnswer::instead(0x0Fu);
  apu.setAccessWatcher(&w);
  apu.watchAccess(0x00FDu, 1, true, false);
  apu.step();
  EXPECT_EQ(int{apu.cpuState().a}, 0x0F) << "the program received the substitute";
  EXPECT_EQ(int{apu.state().timers[0].stage3}, 0) << "and the output cleared all the same";
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
  w.verdict = AccessAnswer::instead(0x88u);
  apu.setAccessWatcher(&w);
  apu.watchAccess(0x0250u, 1, true, false);
  Apu moved = std::move(apu);
  moved.step();  // the armed read still reaches the watcher on the moved machine
  EXPECT_EQ(int{moved.cpuState().a}, 0x88);
}

}  // namespace
