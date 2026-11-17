// Whether a game played beside a buffer runs: every transition the pause
// control makes, the resume level, the rate a buffer fills or drains at, and
// the sizes and times the player says and reads.

#include <cstdint>
#include <optional>
#include <string>

#include "gtest/gtest.h"
#include "player/pause.h"

namespace snaggletooth::player {
namespace {

constexpr std::uint64_t kCeiling = 1'000'000'000u;  // a ceiling of 1 GB: the resume level at 50 is 500 MB
constexpr std::uint64_t kOver = 600'000'000u;       // over that level
constexpr std::uint64_t kUnder = 400'000'000u;      // under it
constexpr std::uint64_t kSecond = 1'000'000'000u;   // nanoseconds

// A control brought to `state` under `settings`.
PauseControl at(Stopped state, PauseSettings settings = {}) {
  PauseControl pause{settings};
  if (state == Stopped::AtLimit) {
    EXPECT_EQ(pause.update(true, kCeiling, kCeiling), Stopped::AtLimit);
  } else if (state == Stopped::ByHand) {
    pause.toggle();
    EXPECT_EQ(pause.update(false, 0u, kCeiling), Stopped::ByHand);
  }
  return pause;
}

}  // namespace

TEST(PlayerPause, ARunningGameAskedStopsByHand) {
  PauseControl pause = at(Stopped::No);
  pause.toggle();
  EXPECT_EQ(pause.update(true, kCeiling, kCeiling), Stopped::ByHand) << "the ask is taken before the ceilings";
  EXPECT_EQ(pause.state(), Stopped::ByHand);
}

TEST(PlayerPause, ARunningGameAtTheCeilingsStops) {
  PauseControl pause = at(Stopped::No);
  EXPECT_EQ(pause.update(true, kOver, kCeiling), Stopped::AtLimit);
}

TEST(PlayerPause, ARunningGameUnderTheCeilingsRuns) {
  PauseControl pause = at(Stopped::No);
  EXPECT_EQ(pause.update(false, kOver, kCeiling), Stopped::No);
  EXPECT_EQ(pause.update(false, kCeiling, kCeiling), Stopped::No) << "what it holds is not the test: the run says";
}

TEST(PlayerPause, AStopAtTheCeilingsAskedResumesOnceThereIsRoom) {
  PauseControl under = at(Stopped::AtLimit);
  under.toggle();
  EXPECT_EQ(under.update(true, kUnder, kCeiling), Stopped::No) << "at or under the level";
  PauseControl room = at(Stopped::AtLimit);
  room.toggle();
  EXPECT_EQ(room.update(false, kOver, kCeiling), Stopped::No) << "no longer at the ceilings";
}

TEST(PlayerPause, AStopAtTheCeilingsAskedWithNoRoomDropsTheAsk) {
  PauseControl pause = at(Stopped::AtLimit);
  pause.toggle();
  EXPECT_EQ(pause.update(true, kOver, kCeiling), Stopped::AtLimit);
  EXPECT_EQ(pause.update(true, kUnder, kCeiling), Stopped::No) << "the ask was dropped, and the drain resumes it";
}

TEST(PlayerPause, AStopAtTheCeilingsResumesAtTheLevel) {
  PauseControl pause = at(Stopped::AtLimit);
  EXPECT_EQ(pause.update(true, 500'000'000u, kCeiling), Stopped::No) << "at the level";
}

TEST(PlayerPause, AStopAtTheCeilingsStaysOverTheLevelOrUnderHold) {
  PauseControl over = at(Stopped::AtLimit);
  EXPECT_EQ(over.update(false, 500'000'001u, kCeiling), Stopped::AtLimit) << "a byte over the level";
  PauseControl held = at(Stopped::AtLimit, {.resumePercent = 50, .hold = true});
  EXPECT_EQ(held.update(false, 0u, kCeiling), Stopped::AtLimit) << "under hold, only an ask resumes";
  held.toggle();
  EXPECT_EQ(held.update(false, 0u, kCeiling), Stopped::No);
}

TEST(PlayerPause, AStopByHandAskedAtTheCeilingsOverTheLevelBecomesAStopAtThem) {
  PauseControl pause = at(Stopped::ByHand);
  pause.toggle();
  EXPECT_EQ(pause.update(true, kOver, kCeiling), Stopped::AtLimit);
}

TEST(PlayerPause, AStopByHandAskedOtherwiseResumes) {
  PauseControl notAtLimit = at(Stopped::ByHand);
  notAtLimit.toggle();
  EXPECT_EQ(notAtLimit.update(false, kOver, kCeiling), Stopped::No);
  PauseControl underLevel = at(Stopped::ByHand);
  underLevel.toggle();
  EXPECT_EQ(underLevel.update(true, kUnder, kCeiling), Stopped::No);
}

TEST(PlayerPause, AStopByHandNotAskedStays) {
  PauseControl pause = at(Stopped::ByHand);
  EXPECT_EQ(pause.update(false, 0u, kCeiling), Stopped::ByHand);
  EXPECT_EQ(pause.update(true, kCeiling, kCeiling), Stopped::ByHand);
}

TEST(PlayerPause, TwoTogglesBetweenTwoLooksAreNone) {
  PauseControl pause = at(Stopped::No);
  pause.toggle();
  pause.toggle();
  EXPECT_EQ(pause.update(false, 0u, kCeiling), Stopped::No);
}

TEST(PlayerPause, TheResumeLevelIsTheCeilingsShareRoundedDown) {
  EXPECT_EQ(PauseControl({.resumePercent = 0, .hold = false}).resumeLevel(kCeiling), 0u);
  EXPECT_EQ(PauseControl({.resumePercent = 50, .hold = false}).resumeLevel(kCeiling), 500'000'000u);
  EXPECT_EQ(PauseControl({.resumePercent = 100, .hold = false}).resumeLevel(kCeiling), kCeiling);
  EXPECT_EQ(PauseControl({.resumePercent = 250, .hold = false}).resumeLevel(kCeiling), kCeiling) << "over 100 is 100";
  EXPECT_EQ(PauseControl({.resumePercent = 33, .hold = false}).resumeLevel(199u), 65u) << "65.67, rounded down";
  const std::uint64_t most = ~std::uint64_t{0};
  EXPECT_EQ(PauseControl({.resumePercent = 50, .hold = false}).resumeLevel(most), most / 2u)
      << "with no overflow in between";
}

TEST(PlayerPause, SettingsSetDuringAStopTakeEffectAtTheNextLook) {
  PauseControl pause = at(Stopped::AtLimit);
  EXPECT_EQ(pause.update(true, kOver, kCeiling), Stopped::AtLimit);
  pause.set({.resumePercent = 0, .hold = false});
  EXPECT_EQ(pause.settings(), (PauseSettings{.resumePercent = 0, .hold = false}));
  EXPECT_EQ(pause.update(true, 1u, kCeiling), Stopped::AtLimit) << "at 0 it waits for the buffer to empty";
  EXPECT_EQ(pause.update(true, 0u, kCeiling), Stopped::No);
}

TEST(PlayerPause, CeilingsUnderABlockResumeWhenEmptyAndStopAgain) {
  // A ceiling the open blocks alone pass: at the ceilings with nothing held.
  PauseControl pause{{.resumePercent = 50, .hold = false}};
  EXPECT_EQ(pause.update(true, 0u, 1'000'000u), Stopped::AtLimit);
  EXPECT_EQ(pause.update(true, 0u, 1'000'000u), Stopped::No) << "nothing held is under any level";
  EXPECT_EQ(pause.update(true, 0u, 1'000'000u), Stopped::AtLimit) << "and the next look stops it again";
}

TEST(PlayerPause, ABufferRateNeedsTwoLooksAFifthOfASecondApart) {
  BufferRate rate;
  EXPECT_FALSE(rate.bytesPerSecond().has_value()) << "no look";
  EXPECT_FALSE(rate.secondsTo(0u).has_value());
  rate.sample(10u * kSecond, 1'000u);
  EXPECT_FALSE(rate.bytesPerSecond().has_value()) << "one look";
  rate.sample(10u * kSecond + kSecond / 10u, 2'000u);
  EXPECT_FALSE(rate.bytesPerSecond().has_value()) << "a tenth of a second apart";
  rate.sample(10u * kSecond + kSecond / 5u, 3'000u);
  ASSERT_TRUE(rate.bytesPerSecond().has_value());
  EXPECT_DOUBLE_EQ(*rate.bytesPerSecond(), 10'000.0) << "2,000 bytes over a fifth of a second, filling";
  rate.clear();
  EXPECT_FALSE(rate.bytesPerSecond().has_value());
}

TEST(PlayerPause, ABufferRateDrainsAndForgetsLooksOlderThanFiveSeconds) {
  BufferRate rate;
  // 10 MB a second down from 100 MB, a look every tenth of a second for eight seconds.
  for (std::uint64_t tenth = 0; tenth <= 80u; ++tenth) rate.sample(tenth * kSecond / 10u, 100'000'000u - tenth * 1'000'000u);
  ASSERT_TRUE(rate.bytesPerSecond().has_value());
  EXPECT_NEAR(*rate.bytesPerSecond(), -10'000'000.0, 1.0);
  // A jump in the first three seconds would show if they were kept.
  BufferRate jumped;
  jumped.sample(0u, 1'000'000'000u);
  for (std::uint64_t tenth = 30; tenth <= 80u; ++tenth) jumped.sample(tenth * kSecond / 10u, 100'000'000u - tenth * 1'000'000u);
  ASSERT_TRUE(jumped.bytesPerSecond().has_value());
  EXPECT_NEAR(*jumped.bytesPerSecond(), -10'000'000.0, 1.0) << "the look at 0 s is gone";
  // Held at 20 MB on the last look: 2 s to empty, 1 s to 10 MB, 0 at or under.
  EXPECT_NEAR(*rate.secondsTo(0u), 2.0, 1e-9);
  EXPECT_NEAR(*rate.secondsTo(10'000'000u), 1.0, 1e-9);
  EXPECT_EQ(rate.secondsTo(20'000'000u), std::optional<double>(0.0)) << "at the level";
  EXPECT_EQ(rate.secondsTo(30'000'000u), std::optional<double>(0.0)) << "under it";
  BufferRate filling;
  filling.sample(0u, 0u);
  filling.sample(kSecond, 5'000'000u);
  EXPECT_FALSE(filling.secondsTo(1'000'000u).has_value()) << "a filling buffer never falls to a level";
}

TEST(PlayerPause, SizesAndTimesAreSaidAsThePlayerSaysThem) {
  EXPECT_EQ(sizeText(0u), "0 MB");
  EXPECT_EQ(sizeText(999'999'999u), "999 MB");
  EXPECT_EQ(sizeText(1'000'000'000u), "1.0 GB");
  EXPECT_EQ(sizeText(41'299'999'999u), "41.2 GB");
  EXPECT_EQ(sizeText(65'000'000'000u), "65.0 GB");
  EXPECT_EQ(durationText(0.0), "0 s");
  EXPECT_EQ(durationText(39.2), "40 s");
  EXPECT_EQ(durationText(59.0), "59 s");
  EXPECT_EQ(durationText(59.5), "1 min 0 s");
  EXPECT_EQ(durationText(100.0), "1 min 40 s");
  EXPECT_EQ(durationText(3599.2), "1 h 0 min");
  EXPECT_EQ(durationText(7500.0), "2 h 5 min");
  EXPECT_EQ(durationText(7501.0), "2 h 6 min") << "a minute rounded up from an hour on";
}

TEST(PlayerPause, ASizeAFlagIsHandedIsReadExactly) {
  EXPECT_EQ(parseGigabytes("64"), std::optional<std::uint64_t>(64'000'000'000u));
  EXPECT_EQ(parseGigabytes("0"), std::optional<std::uint64_t>(0u));
  EXPECT_EQ(parseGigabytes("0.5"), std::optional<std::uint64_t>(500'000'000u));
  EXPECT_EQ(parseGigabytes("1.25"), std::optional<std::uint64_t>(1'250'000'000u));
  EXPECT_EQ(parseGigabytes("1.000000001"), std::optional<std::uint64_t>(1'000'000'001u));
  for (const char* refused : {"-1", "1.", ".5", "1e3", "1 GB", "1.0000000001", "", "99999999999999999999"}) {
    EXPECT_FALSE(parseGigabytes(refused).has_value()) << "'" << refused << "'";
  }
}

}  // namespace snaggletooth::player
