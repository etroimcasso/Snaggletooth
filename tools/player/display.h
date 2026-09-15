#pragma once

// Holding a run to the panel it is shown on.
//
// The console drove a television, and a television took whatever it was sent. A
// fixed-refresh panel does not: it refreshes on its own schedule, and the console's
// rate and the panel's are two different numbers — 60.0988 frames a second against
// 60.000. A run paced to the console alone therefore lands a frame on the wrong side
// of a refresh about once every ten seconds, for as long as it runs, and nothing
// pulls that phase back once something has shifted it.
//
// What is here decides which of the two rates a run is held to, when each of its
// frames is owed, and what the choice costs the sound. It opens nothing, reads no
// clock and names no windowing library: an application that owns a window reports
// what its panel runs at and these functions answer with values, so every one of them
// is checkable wherever the suite runs.
//
// The machine is untouched either way. A run held to a panel emulates every cycle
// exactly as an unheld one does — what changes is only the wall-clock rate its frames
// are asked for, and the sound rate that follows from it.

#include <cstdint>
#include <numeric>

#include "snaggletooth/snes/snes.h"

namespace snaggletooth::player {

inline constexpr std::uint64_t kNanosPerSecond = 1000000000ull;

// A rate as the exact ratio it is: `numerator / denominator` frames a second. Neither
// the console's rate nor a panel's is a whole number of frames a second, and a run is
// held to a fraction of a nanosecond over hours, so the ratio is carried rather than a
// figure divided out of it. A rate with a zero in it is no rate at all — a panel
// nobody could read one from — and is never held to.
struct FrameRate {
  std::uint64_t numerator = 0;
  std::uint64_t denominator = 0;
};

[[nodiscard]] constexpr bool isRate(FrameRate rate) noexcept {
  return rate.numerator != 0u && rate.denominator != 0u;
}

// The rate a console of this region draws frames at: its master clock over the master
// cycles one frame takes.
[[nodiscard]] constexpr FrameRate consoleFrameRate(const ConsoleClock& clock) noexcept {
  return FrameRate{.numerator = clock.hertzNumerator,
                   .denominator = clock.hertzDenominator * clock.masterCyclesPerFrame};
}

// How close a panel's rate must be to the console's before a run is held to the panel
// instead: one part in a hundred. A 60.000 Hz panel and a 60 Hz console are a sixth of
// a per cent apart, which one refresh a frame absorbs; a 50 Hz panel, or a 144 Hz one,
// is a different rate entirely and holding a run to it would change the speed the game
// is played at.
inline constexpr std::uint64_t kLockParts = 100u;

// Whether a run shown on a panel of this rate is held to the panel rather than to the
// console.
[[nodiscard]] constexpr bool locksTo(FrameRate console, FrameRate panel) noexcept {
  if (!isRate(console) || !isRate(panel)) return false;
  const std::uint64_t theirs = panel.numerator * console.denominator;
  const std::uint64_t ours = console.numerator * panel.denominator;
  const std::uint64_t apart = theirs > ours ? theirs - ours : ours - theirs;
  return apart * kLockParts <= ours;
}

// When each frame of a run at a given rate is owed, in nanoseconds from the run's
// start. Like the console's own deadline it reads no clock and does not wait — a host
// asks when the next frame is due and waits however it waits — and it splits the
// interval into whole nanoseconds and the fraction left over rather than multiplying
// the product out, so a deadline stays exact past the point a single product passes 64
// bits. The rate it is built from must be a real one.
class FramePace {
 public:
  explicit constexpr FramePace(FrameRate rate) noexcept
      : divisor_(rate.numerator),
        whole_(kNanosPerSecond * rate.denominator / rate.numerator),
        remainder_(kNanosPerSecond * rate.denominator % rate.numerator) {}

  // The nanosecond the Nth frame of the run is owed at.
  [[nodiscard]] constexpr std::uint64_t owedAt(std::uint64_t frames) const noexcept {
    return frames * whole_ + frames * remainder_ / divisor_;
  }

  // One frame's interval, in whole nanoseconds.
  [[nodiscard]] constexpr std::uint64_t wholeNanos() const noexcept { return whole_; }

 private:
  std::uint64_t divisor_;
  std::uint64_t whole_;
  std::uint64_t remainder_;
};

// A 60.000 Hz panel's frame, worked out by hand; and the console's own rate through
// this, which is the machine's own deadline to the nanosecond — the two are one
// arithmetic and a rearrangement that parted them would not compile.
static_assert(FramePace(FrameRate{.numerator = 60u, .denominator = 1u}).owedAt(1u) == 16666666ull);
static_assert(FramePace(consoleFrameRate(consoleClock(Region::Ntsc))).owedAt(4693u) ==
              FrameDeadline(Region::Ntsc).owedAt(4693u));
static_assert(FramePace(consoleFrameRate(consoleClock(Region::Pal))).owedAt(4693u) ==
              FrameDeadline(Region::Pal).owedAt(4693u));

// Dividing a ratio's two halves by what they share.
constexpr void reduce(std::uint64_t& top, std::uint64_t& bottom) noexcept {
  const std::uint64_t common = std::gcd(top, bottom);
  if (common == 0u) return;
  top /= common;
  bottom /= common;
}

// The rate to open a playback device's stream at, for a run held to `paced` on a
// machine that makes `made` samples a second of its own time. A run held to a panel a
// sixth of a per cent slower than the console makes its samples that fraction slower
// too, and a device consuming exactly `made` a second would run dry every few minutes;
// telling the device the stream arrives at this rate instead keeps the sound on the
// same clock as the picture. Rounded to the nearest whole rate, which is what a device
// is asked in.
[[nodiscard]] constexpr int pacedSampleRate(int made, FrameRate console, FrameRate paced) noexcept {
  if (!isRate(console) || !isRate(paced) || made <= 0) return made;
  // `made` x paced / console, with the ratio divided down before any of it is
  // multiplied out. Both halves of the console's rate are eight-figure numbers, so a
  // run held to the console's own rate multiplies one of them by the other and passes
  // 64 bits; reduced first, that same run is the ratio one to one.
  std::uint64_t theirs = paced.numerator;
  std::uint64_t ours = console.denominator;
  std::uint64_t perTheirs = paced.denominator;
  std::uint64_t perOurs = console.numerator;
  reduce(theirs, perOurs);
  reduce(ours, perTheirs);
  reduce(theirs, perTheirs);
  reduce(ours, perOurs);
  const std::uint64_t top = static_cast<std::uint64_t>(made) * theirs * ours;
  const std::uint64_t bottom = perTheirs * perOurs;
  return static_cast<int>((top + bottom / 2u) / bottom);
}

// What a run was asked for. `Auto` holds the run to the panel where the two rates are
// close enough; `On` and `Off` name the arrangement outright.
enum class Vsync : std::uint8_t { Auto, On, Off };

// Which rate a run takes, and whether that rate is the panel's — which is also whether
// the present is asked to wait for a refresh.
struct Pacing {
  FrameRate rate;
  bool locked = false;
};

[[nodiscard]] constexpr Pacing paceRun(FrameRate console, FrameRate panel,
                                       Vsync choice) noexcept {
  const bool locked = choice == Vsync::Off  ? false
                      : choice == Vsync::On ? isRate(panel)
                                            : locksTo(console, panel);
  return Pacing{.rate = locked ? panel : console, .locked = locked};
}

}  // namespace snaggletooth::player
