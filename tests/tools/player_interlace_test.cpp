// The field weaver: how the player composes the machine's half-height fields
// into the interlaced picture the console's signal carried. Pure logic, no
// device — a field goes in and the pixels to show come out.

#include <cstdint>
#include <vector>

#include "gtest/gtest.h"
#include "player/interlace.h"

namespace {

using snaggletooth::player::FieldWeaver;
using snaggletooth::player::Interlace;
using snaggletooth::player::parseInterlace;
using snaggletooth::player::Shown;

// A solid field of one grey level, `w` by `h`, RGBA with alpha 255.
std::vector<std::uint8_t> solid(unsigned w, unsigned h, std::uint8_t value) {
  std::vector<std::uint8_t> f(static_cast<std::size_t>(w) * h * 4u);
  for (std::size_t i = 0; i < f.size(); i += 4u) {
    f[i] = value;
    f[i + 1] = value;
    f[i + 2] = value;
    f[i + 3] = 255u;
  }
  return f;
}

// The red byte of one row of a Shown.
std::uint8_t rowValue(const Shown& s, unsigned row) {
  return s.pixels[static_cast<std::size_t>(row) * s.width * 4u];
}

TEST(FieldWeaver, AProgressiveRunIsShownUnchangedWhateverTheMode) {
  const std::vector<std::uint8_t> field = solid(4u, 3u, 40u);
  for (const Interlace mode : {Interlace::Weave, Interlace::Bob, Interlace::Off}) {
    FieldWeaver weaver(mode);
    const Shown s = weaver.present(field.data(), 4u, 3u, /*field=*/0u, /*interlaced=*/false);
    EXPECT_EQ(s.pixels, field.data());  // the field itself, not a copy
    EXPECT_EQ(s.width, 4u);
    EXPECT_EQ(s.height, 3u);
  }
}

TEST(FieldWeaver, OffShowsTheFieldAsItArrivesEvenWhenInterlaced) {
  const std::vector<std::uint8_t> field = solid(4u, 3u, 40u);
  FieldWeaver weaver(Interlace::Off);
  const Shown s = weaver.present(field.data(), 4u, 3u, /*field=*/1u, /*interlaced=*/true);
  EXPECT_EQ(s.pixels, field.data());
  EXPECT_EQ(s.height, 3u);
}

TEST(FieldWeaver, WeavePutsEachFieldsLinesAtRowTwiceIPlusItsParity) {
  const std::vector<std::uint8_t> even = solid(2u, 2u, 10u);
  const std::vector<std::uint8_t> odd = solid(2u, 2u, 20u);
  FieldWeaver weaver(Interlace::Weave);

  const Shown a = weaver.present(even.data(), 2u, 2u, /*field=*/0u, /*interlaced=*/true);
  EXPECT_EQ(a.width, 2u);
  EXPECT_EQ(a.height, 4u);  // twice the field's height
  EXPECT_EQ(rowValue(a, 0u), 10u);  // field 0 -> rows 0, 2
  EXPECT_EQ(rowValue(a, 2u), 10u);

  const Shown b = weaver.present(odd.data(), 2u, 2u, /*field=*/1u, /*interlaced=*/true);
  EXPECT_EQ(rowValue(b, 0u), 10u);  // the even field is HELD
  EXPECT_EQ(rowValue(b, 2u), 10u);
  EXPECT_EQ(rowValue(b, 1u), 20u);  // field 1 -> rows 1, 3
  EXPECT_EQ(rowValue(b, 3u), 20u);
}

TEST(FieldWeaver, BobFillsTheGapFromTheFieldItself) {
  const std::vector<std::uint8_t> field = solid(2u, 2u, 30u);
  FieldWeaver weaver(Interlace::Bob);
  const Shown s = weaver.present(field.data(), 2u, 2u, /*field=*/0u, /*interlaced=*/true);
  EXPECT_EQ(s.height, 4u);
  for (unsigned row = 0; row < 4u; ++row) EXPECT_EQ(rowValue(s, row), 30u);  // every row this field
}

TEST(FieldWeaver, AShapeChangeRebuildsAndDropsTheHeldParity) {
  const std::vector<std::uint8_t> field0 = solid(2u, 2u, 10u);
  FieldWeaver weaver(Interlace::Weave);
  (void)weaver.present(field0.data(), 2u, 2u, 0u, true);  // writes the even rows of a 2x4 picture

  const std::vector<std::uint8_t> taller = solid(2u, 3u, 50u);
  const Shown s = weaver.present(taller.data(), 2u, 3u, 1u, true);
  EXPECT_EQ(s.height, 6u);
  EXPECT_EQ(rowValue(s, 0u), 0u);   // the old even field is gone, the buffer was cleared
  EXPECT_EQ(rowValue(s, 1u), 50u);  // the new odd field is in place
}

TEST(FieldWeaver, ParseInterlaceNamesTheThreeModesAndNothingElse) {
  EXPECT_EQ(parseInterlace("weave"), Interlace::Weave);
  EXPECT_EQ(parseInterlace("bob"), Interlace::Bob);
  EXPECT_EQ(parseInterlace("off"), Interlace::Off);
  EXPECT_FALSE(parseInterlace("weaved").has_value());
  EXPECT_FALSE(parseInterlace("").has_value());
  EXPECT_FALSE(parseInterlace("Weave").has_value());  // the names are lower-case
}

}  // namespace
