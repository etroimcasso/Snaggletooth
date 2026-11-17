#pragma once

// Whether a game played beside a buffer runs, and what the player says of the
// buffer while it does not.
//
// A `PauseControl` is asked after every run of the machine and on every pass of
// a stop. The game stops when the buffer stands at its ceilings, and when the
// person stops it; a stop at the ceilings ends when the buffer has drained to
// the resume level, or when the person resumes, and under `hold` only then.
//
//   player::PauseControl pause{{.resumePercent = 50, .hold = false}};
//   if (pause.update(run.atLimit(), progress.held, ceiling) != player::Stopped::No) {
//     // the host stops running the machine and asks again a tenth of a second on
//   }
//
// `BufferRate` gives the rate the buffer fills or drains at, and the three text
// functions write a size and a time as the player says them and read a size a
// flag is handed. Nothing here starts a thread, allocates while it runs, or
// knows the run it is told about.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace snaggletooth::player {

// Why the game is stopped, if it is.
enum class Stopped : std::uint8_t {
  No,
  AtLimit,  // the buffer reached its ceilings and has not drained to the resume level
  ByHand,   // the person stopped it
};

// How a stop at the ceilings ends.
struct PauseSettings {
  unsigned resumePercent = 50;  // it ends when the buffer holds this share of its ceilings or less; over 100 is 100
  bool hold = false;            // it ends only when the person resumes
  [[nodiscard]] bool operator==(const PauseSettings&) const = default;
};

// The transitions, at each `update`, where `level` is `resumeLevel(ceiling)`
// and *asked* is a `toggle()` since the last `update`:
//
//   No       asked                                  -> ByHand
//   No       not asked, at the ceilings             -> AtLimit
//   AtLimit  asked, not at the ceilings or at/under level -> No
//   AtLimit  asked, otherwise                       -> AtLimit, the ask dropped
//   AtLimit  not asked, not `hold`, at/under level  -> No
//   ByHand   asked, at the ceilings and over level  -> AtLimit
//   ByHand   asked, otherwise                       -> No
//
// and the state stays as it is otherwise. A resume at or under the level is
// granted whether or not the buffer is still at its ceilings: ceilings smaller
// than a block are at the ceiling always, and the game then runs one call,
// stops, and resumes once that call's records are played. Two toggles between
// two updates are one ask and then none.
class PauseControl {
 public:
  explicit PauseControl(PauseSettings settings = {}) noexcept : settings_(settings) {}
  [[nodiscard]] PauseSettings settings() const noexcept { return settings_; }
  void set(PauseSettings settings) noexcept { settings_ = settings; }
  // The person's chord or button: a running game stops by hand; a stopped one
  // is asked to resume, which the next `update` grants or refuses.
  void toggle() noexcept { asked_ = !asked_; }
  // One look at the buffer: `atLimit` as the run answers it, `held` its bytes,
  // `ceiling` its two ceilings together. Answers the state from here on.
  [[nodiscard]] Stopped update(bool atLimit, std::uint64_t held, std::uint64_t ceiling) noexcept;
  [[nodiscard]] Stopped state() const noexcept { return state_; }
  // The bytes a stop at the ceilings drains to: `ceiling` times the resume
  // percent over a hundred, rounded down.
  [[nodiscard]] std::uint64_t resumeLevel(std::uint64_t ceiling) const noexcept;

 private:
  PauseSettings settings_;
  Stopped state_ = Stopped::No;
  bool asked_ = false;
};

// The rate a buffer fills or drains at, from what it held over the last five
// seconds of looks.
//
//   player::BufferRate drain;
//   drain.sample(now, progress.held);
//   if (const std::optional<double> seconds = drain.secondsTo(level)) say(player::durationText(*seconds));
class BufferRate {
 public:
  // A look: `nanos` on any steady clock, `held` the buffer's bytes then.
  void sample(std::uint64_t nanos, std::uint64_t held) noexcept;
  void clear() noexcept { count_ = 0; }
  // Bytes a second between the oldest look kept and the newest, positive while
  // the buffer fills; nothing until two looks stand a fifth of a second apart.
  [[nodiscard]] std::optional<double> bytesPerSecond() const noexcept;
  // Seconds until what the newest look held falls to `level` at that rate;
  // nothing while there is no rate or the buffer is not draining, 0 at or under
  // the level.
  [[nodiscard]] std::optional<double> secondsTo(std::uint64_t level) const noexcept;

  // The most looks kept. A look that comes less than a sixty-fourth of five
  // seconds after the look before the newest takes the newest's place, so the
  // looks kept span the last five seconds however often they come.
  static constexpr std::size_t kLooks = 64;

 private:
  struct Look {
    std::uint64_t nanos = 0;
    std::uint64_t held = 0;
  };
  [[nodiscard]] const Look& at(std::size_t i) const noexcept { return looks_[(first_ + i) % kLooks]; }
  std::array<Look, kLooks> looks_{};
  std::size_t first_ = 0;
  std::size_t count_ = 0;
};

// A size as the player says it: `41.2 GB` from a billion bytes up, one decimal,
// rounded down; `512 MB` under it, whole megabytes of a million bytes, rounded
// down.
[[nodiscard]] std::string sizeText(std::uint64_t bytes);
// A time as the player says it: `40 s`, `1 min 40 s`, `2 h 5 min`; rounded up
// to a whole second, and to a whole minute from an hour up.
[[nodiscard]] std::string durationText(double seconds);
// A size a flag is handed, in GB of a billion bytes with up to nine decimals,
// as bytes, exact: `64`, `0.5`, `1.25`. Nothing for anything else.
[[nodiscard]] std::optional<std::uint64_t> parseGigabytes(std::string_view text);

}  // namespace snaggletooth::player
