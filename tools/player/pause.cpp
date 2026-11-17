#include "player/pause.h"

#include <cmath>
#include <limits>

namespace snaggletooth::player {

namespace {

constexpr std::uint64_t kNanosPerSecond = 1'000'000'000u;
constexpr std::uint64_t kWindowNanos = 5u * kNanosPerSecond;
constexpr std::uint64_t kRateNanos = kNanosPerSecond / 5u;  // a fifth of a second
constexpr std::uint64_t kSpacingNanos = kWindowNanos / BufferRate::kLooks;

}  // namespace

Stopped PauseControl::update(bool atLimit, std::uint64_t held, std::uint64_t ceiling) noexcept {
  const bool asked = asked_;
  asked_ = false;
  const bool drained = held <= resumeLevel(ceiling);
  switch (state_) {
    case Stopped::No:
      if (asked) {
        state_ = Stopped::ByHand;
      } else if (atLimit) {
        state_ = Stopped::AtLimit;
      }
      break;
    case Stopped::AtLimit:
      if (asked) {
        if (!atLimit || drained) state_ = Stopped::No;
      } else if (!settings_.hold && drained) {
        state_ = Stopped::No;
      }
      break;
    case Stopped::ByHand:
      if (asked) state_ = atLimit && !drained ? Stopped::AtLimit : Stopped::No;
      break;
  }
  return state_;
}

std::uint64_t PauseControl::resumeLevel(std::uint64_t ceiling) const noexcept {
  const std::uint64_t percent = settings_.resumePercent > 100u ? 100u : settings_.resumePercent;
  // ceiling * percent / 100, exact and rounded down without the product's overflow.
  return ceiling / 100u * percent + ceiling % 100u * percent / 100u;
}

void BufferRate::sample(std::uint64_t nanos, std::uint64_t held) noexcept {
  const Look look{.nanos = nanos, .held = held};
  if (count_ >= 2u && nanos - at(count_ - 2u).nanos < kSpacingNanos) {
    looks_[(first_ + count_ - 1u) % kLooks] = look;
  } else if (count_ == kLooks) {
    looks_[first_] = look;
    first_ = (first_ + 1u) % kLooks;
  } else {
    looks_[(first_ + count_) % kLooks] = look;
    ++count_;
  }
  while (count_ > 1u && nanos - at(0).nanos > kWindowNanos) {
    first_ = (first_ + 1u) % kLooks;
    --count_;
  }
}

std::optional<double> BufferRate::bytesPerSecond() const noexcept {
  if (count_ < 2u) return std::nullopt;
  const Look& oldest = at(0);
  const Look& newest = at(count_ - 1u);
  const std::uint64_t span = newest.nanos - oldest.nanos;
  if (span < kRateNanos) return std::nullopt;
  const double bytes = static_cast<double>(newest.held) - static_cast<double>(oldest.held);
  return bytes * static_cast<double>(kNanosPerSecond) / static_cast<double>(span);
}

std::optional<double> BufferRate::secondsTo(std::uint64_t level) const noexcept {
  if (count_ == 0u) return std::nullopt;
  const std::uint64_t held = at(count_ - 1u).held;
  if (held <= level) return 0.0;
  const std::optional<double> rate = bytesPerSecond();
  if (!rate || *rate >= 0.0) return std::nullopt;
  return static_cast<double>(held - level) / -*rate;
}

std::string sizeText(std::uint64_t bytes) {
  if (bytes >= 1'000'000'000u) {
    const std::uint64_t tenths = bytes / 100'000'000u;
    return std::to_string(tenths / 10u) + "." + std::to_string(tenths % 10u) + " GB";
  }
  return std::to_string(bytes / 1'000'000u) + " MB";
}

std::string durationText(double seconds) {
  if (!(seconds > 0.0)) return "0 s";
  // Capped well inside the integer's range, where a time stops meaning anything.
  constexpr double kMost = 1e15;
  const std::uint64_t whole = static_cast<std::uint64_t>(std::ceil(seconds < kMost ? seconds : kMost));
  if (whole >= 3600u) {
    const std::uint64_t minutes = (whole + 59u) / 60u;
    return std::to_string(minutes / 60u) + " h " + std::to_string(minutes % 60u) + " min";
  }
  if (whole >= 60u) return std::to_string(whole / 60u) + " min " + std::to_string(whole % 60u) + " s";
  return std::to_string(whole) + " s";
}

std::optional<std::uint64_t> parseGigabytes(std::string_view text) {
  constexpr std::uint64_t kGigabyte = 1'000'000'000u;
  std::size_t at = 0;
  std::uint64_t whole = 0;
  while (at < text.size() && text[at] >= '0' && text[at] <= '9') {
    const std::uint64_t digit = static_cast<std::uint64_t>(text[at] - '0');
    if (whole > (std::numeric_limits<std::uint64_t>::max() - digit) / 10u) return std::nullopt;
    whole = whole * 10u + digit;
    ++at;
  }
  if (at == 0u) return std::nullopt;
  std::uint64_t fraction = 0;
  if (at < text.size()) {
    if (text[at] != '.') return std::nullopt;
    ++at;
    const std::size_t begins = at;
    while (at < text.size() && text[at] >= '0' && text[at] <= '9') {
      fraction = fraction * 10u + static_cast<std::uint64_t>(text[at] - '0');
      ++at;
    }
    const std::size_t decimals = at - begins;
    if (at != text.size() || decimals == 0u || decimals > 9u) return std::nullopt;
    for (std::size_t pad = decimals; pad < 9u; ++pad) fraction *= 10u;
  }
  if (whole > (std::numeric_limits<std::uint64_t>::max() - fraction) / kGigabyte) return std::nullopt;
  return whole * kGigabyte + fraction;
}

}  // namespace snaggletooth::player
