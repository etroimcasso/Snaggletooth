#include "vector_harness.h"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

namespace {

using snaggletooth::test::parseVectorFile;
using snaggletooth::test::VectorCase;

// The pinned schema shape — one NOP-style case, verbatim from the vectors'
// documented layout. The reader must recover every field it maps.
const char* kOneCase =
    R"([{
      "name": "00 0000",
      "initial": { "pc": 30256, "a": 56, "x": 78, "y": 127, "sp": 236, "psw": 145,
                   "ram": [[30256, 0]] },
      "final":   { "pc": 30257, "a": 56, "x": 78, "y": 127, "sp": 236, "psw": 145,
                   "ram": [[30256, 0]] },
      "cycles":  [[30256, 0, "read"], [30257, null, "read"]]
    }])";

TEST(VectorHarness, ParsesRegistersAndName) {
  const auto cases = parseVectorFile(kOneCase);
  ASSERT_EQ(cases.size(), 1u);
  const VectorCase& c = cases.front();
  EXPECT_EQ(c.name, "00 0000");
  EXPECT_EQ(c.initial.pc, 30256);
  EXPECT_EQ(c.initial.a, 56);
  EXPECT_EQ(c.initial.x, 78);
  EXPECT_EQ(c.initial.y, 127);
  EXPECT_EQ(c.initial.sp, 236);
  EXPECT_EQ(c.initial.psw, 145);
  EXPECT_EQ(c.final_.pc, 30257);
}

TEST(VectorHarness, ParsesSparseRamPairs) {
  const auto cases = parseVectorFile(kOneCase);
  ASSERT_EQ(cases.front().initial.ram.size(), 1u);
  EXPECT_EQ(cases.front().initial.ram[0].first, 30256);
  EXPECT_EQ(cases.front().initial.ram[0].second, 0);
}

TEST(VectorHarness, CountsCycleEntriesIncludingNullValues) {
  // The reader asserts only the length of the cycle list; the null-valued dummy
  // read must still count.
  EXPECT_EQ(parseVectorFile(kOneCase).front().cycles, 2u);
}

TEST(VectorHarness, ParsesMultipleCasesAndAWriteCase) {
  const char* twoCases =
      R"([
        { "name": "a", "initial": { "pc": 1, "ram": [] },
          "final": { "pc": 2, "ram": [] }, "cycles": [[1,0,"read"]] },
        { "name": "b", "initial": { "pc": 10, "ram": [[16, 255], [17, 1]] },
          "final": { "pc": 12, "ram": [[16, 42]] },
          "cycles": [[10,0,"read"],[11,0,"read"],[16,42,"write"]] }
      ])";
  const auto cases = parseVectorFile(twoCases);
  ASSERT_EQ(cases.size(), 2u);
  EXPECT_EQ(cases[0].cycles, 1u);
  EXPECT_EQ(cases[1].name, "b");
  ASSERT_EQ(cases[1].initial.ram.size(), 2u);
  EXPECT_EQ(cases[1].initial.ram[1].first, 17);
  ASSERT_EQ(cases[1].final_.ram.size(), 1u);
  EXPECT_EQ(cases[1].final_.ram[0].second, 42);
  EXPECT_EQ(cases[1].cycles, 3u);
}

TEST(VectorHarness, HandlesEmptyTopLevelArray) {
  EXPECT_TRUE(parseVectorFile("[]").empty());
}

TEST(VectorHarness, ThrowsOnMalformedInput) {
  EXPECT_THROW(parseVectorFile("[{ \"name\": }]"), std::runtime_error);
  EXPECT_THROW(parseVectorFile("not json"), std::runtime_error);
}

}  // namespace
