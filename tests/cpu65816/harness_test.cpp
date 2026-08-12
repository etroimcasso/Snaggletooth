#include "vector_harness.h"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

namespace {

using snaggletooth::cpu_vectors::parseVectorFile;
using snaggletooth::cpu_vectors::VectorCase;

// The pinned schema shape — one case, verbatim from the vectors' documented layout
// (a 65816 has the wider register set and 24-bit RAM addresses). The reader must
// recover every field it maps.
const char* kOneCase =
    R"([{
      "name": "00 n 1",
      "initial": { "pc": 58175, "s": 387, "p": 185, "a": 61134, "x": 207, "y": 195,
                   "dbr": 72, "d": 16223, "pbr": 156, "e": 0,
                   "ram": [[65510, 72], [10281792, 164]] },
      "final":   { "pc": 33352, "s": 383, "p": 181, "a": 61134, "x": 207, "y": 195,
                   "dbr": 72, "d": 16223, "pbr": 0, "e": 0,
                   "ram": [[65511, 130], [384, 185], [387, 156]] },
      "cycles":  [[10281791, 0, "dp-r-mx-"], [387, 156, "d--w-mx-"], [65510, 72, "d-vr-mx-"]]
    }])";

TEST(Cpu65816VectorHarness, ParsesEveryRegister) {
  const auto cases = parseVectorFile(kOneCase);
  ASSERT_EQ(cases.size(), 1u);
  const VectorCase& c = cases.front();
  EXPECT_EQ(c.name, "00 n 1");
  EXPECT_EQ(c.initial.pc, 58175);
  EXPECT_EQ(c.initial.s, 387);
  EXPECT_EQ(c.initial.p, 185);
  EXPECT_EQ(c.initial.a, 61134);
  EXPECT_EQ(c.initial.x, 207);
  EXPECT_EQ(c.initial.y, 195);
  EXPECT_EQ(c.initial.dbr, 72);
  EXPECT_EQ(c.initial.d, 16223);
  EXPECT_EQ(c.initial.pbr, 156);
  EXPECT_FALSE(c.initial.e);
  EXPECT_EQ(c.final_.pc, 33352);
  EXPECT_EQ(c.final_.pbr, 0);
}

TEST(Cpu65816VectorHarness, ParsesEmulationFlagTrue) {
  const auto cases = parseVectorFile(
      R"([{ "name": "e", "initial": { "e": 1, "ram": [] },
           "final": { "e": 1, "ram": [] }, "cycles": [] }])");
  EXPECT_TRUE(cases.front().initial.e);
}

TEST(Cpu65816VectorHarness, ParsesTwentyFourBitRamAddresses) {
  const auto cases = parseVectorFile(kOneCase);
  ASSERT_EQ(cases.front().initial.ram.size(), 2u);
  EXPECT_EQ(cases.front().initial.ram[1].first, 10281792u);
  EXPECT_EQ(cases.front().initial.ram[1].second, 164);
  ASSERT_EQ(cases.front().final_.ram.size(), 3u);
  EXPECT_EQ(cases.front().final_.ram[2].first, 387u);
  EXPECT_EQ(cases.front().final_.ram[2].second, 156);
}

TEST(Cpu65816VectorHarness, ReadsEveryCycleEntry) {
  const auto cases = parseVectorFile(kOneCase);
  const auto& cycles = cases.front().cycles;
  ASSERT_EQ(cycles.size(), 3u);
  ASSERT_TRUE(cycles[0].address.has_value());
  EXPECT_EQ(*cycles[0].address, 10281791u);
  ASSERT_TRUE(cycles[0].value.has_value());
  EXPECT_EQ(int{*cycles[0].value}, 0);
  EXPECT_EQ(cycles[0].signals, "dp-r-mx-");
  EXPECT_EQ(cycles[1].signals, "d--w-mx-");
  EXPECT_EQ(cycles[2].signals, "d-vr-mx-");
}

TEST(Cpu65816VectorHarness, ReadsACycleThatMovedNoByte) {
  // An internal cycle drives an address and reads nothing, so its value is null and
  // must come back absent rather than as a zero that looks like a real byte.
  const auto cases = parseVectorFile(
      R"([{ "name": "x", "initial": { "ram": [] }, "final": { "ram": [] },
           "cycles": [[4096, null, "---r-mx-"]] }])");
  const auto& cycles = cases.front().cycles;
  ASSERT_EQ(cycles.size(), 1u);
  ASSERT_TRUE(cycles[0].address.has_value());
  EXPECT_EQ(*cycles[0].address, 4096u);
  EXPECT_FALSE(cycles[0].value.has_value());
  EXPECT_EQ(cycles[0].signals, "---r-mx-");
}

TEST(Cpu65816VectorHarness, ReadsACycleThatDroveNoAddress) {
  // A halted cycle drives nothing at all: both fields are null.
  const auto cases = parseVectorFile(
      R"([{ "name": "x", "initial": { "ram": [] }, "final": { "ram": [] },
           "cycles": [[null, null, "--------"]] }])");
  const auto& cycles = cases.front().cycles;
  ASSERT_EQ(cycles.size(), 1u);
  EXPECT_FALSE(cycles[0].address.has_value());
  EXPECT_FALSE(cycles[0].value.has_value());
  EXPECT_EQ(cycles[0].signals, "--------");
}

// The pin string the harness derives for a cycle must be the one the vectors write
// for the same cycle — otherwise every per-cycle comparison is against a fiction.
TEST(Cpu65816VectorHarness, DerivesThePinStringForEachKindOfCycle) {
  using snaggletooth::CycleKind;
  using snaggletooth::cpu_vectors::internalSignals;
  using snaggletooth::cpu_vectors::signalsFor;
  // Native, both registers 16-bit: only the access characters are set.
  EXPECT_EQ(signalsFor(CycleKind::OpcodeFetch, false, false, false), "dp-r----");
  EXPECT_EQ(signalsFor(CycleKind::OperandFetch, false, false, false), "-p-r----");
  EXPECT_EQ(signalsFor(CycleKind::DataRead, false, false, false), "d--r----");
  EXPECT_EQ(signalsFor(CycleKind::DataWrite, false, false, false), "d--w----");
  EXPECT_EQ(signalsFor(CycleKind::RmwRead, false, false, false), "d--r---l");
  EXPECT_EQ(signalsFor(CycleKind::RmwWrite, false, false, false), "d--w---l");
  EXPECT_EQ(signalsFor(CycleKind::VectorRead, false, false, false), "d-vr----");
  EXPECT_EQ(internalSignals(false, false, false), "---r----");
  // The width characters report the processor, not the access.
  EXPECT_EQ(signalsFor(CycleKind::OpcodeFetch, false, true, false), "dp-r-m--");
  EXPECT_EQ(signalsFor(CycleKind::OpcodeFetch, false, false, true), "dp-r--x-");
  EXPECT_EQ(signalsFor(CycleKind::OpcodeFetch, true, true, true), "dp-remx-");
  EXPECT_EQ(internalSignals(true, true, true), "---remx-");
}

TEST(Cpu65816VectorHarness, ParsesMultipleCasesAndAWriteCase) {
  const char* twoCases =
      R"([
        { "name": "a", "initial": { "pc": 1, "ram": [] },
          "final": { "pc": 2, "ram": [] }, "cycles": [[1,0,"dp-r-mx-"]] },
        { "name": "b", "initial": { "pc": 10, "ram": [[16, 255], [17, 1]] },
          "final": { "pc": 12, "ram": [[16, 42]] },
          "cycles": [[10,0,"dp-r-mx-"],[11,0,"-p-r-mx-"],[16,42,"d--w-mx-"]] }
      ])";
  const auto cases = parseVectorFile(twoCases);
  ASSERT_EQ(cases.size(), 2u);
  EXPECT_EQ(cases[0].cycles.size(), 1u);
  EXPECT_EQ(cases[1].name, "b");
  ASSERT_EQ(cases[1].initial.ram.size(), 2u);
  EXPECT_EQ(cases[1].initial.ram[1].first, 17u);
  ASSERT_EQ(cases[1].final_.ram.size(), 1u);
  EXPECT_EQ(cases[1].final_.ram[0].second, 42);
  EXPECT_EQ(cases[1].cycles.size(), 3u);
}

TEST(Cpu65816VectorHarness, HandlesEmptyTopLevelArray) {
  EXPECT_TRUE(parseVectorFile("[]").empty());
}

TEST(Cpu65816VectorHarness, ThrowsOnMalformedInput) {
  EXPECT_THROW(parseVectorFile("[{ \"name\": }]"), std::runtime_error);
  EXPECT_THROW(parseVectorFile("not json"), std::runtime_error);
}

}  // namespace
