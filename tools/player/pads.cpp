#include "player/pads.h"

#include <algorithm>
#include <cstddef>

namespace snaggletooth::player {
namespace {

constexpr PadSource fromButton(PadButton button) {
  return PadSource{.kind = PadSource::Kind::Button,
                   .button = button,
                   .axis = PadAxis::LeftX,
                   .label = FaceLabel::None};
}

constexpr PadSource fromAxisHigh(PadAxis axis) {
  return PadSource{.kind = PadSource::Kind::AxisHigh,
                   .button = PadButton::South,
                   .axis = axis,
                   .label = FaceLabel::None};
}

constexpr PadSource fromAxisLow(PadAxis axis) {
  return PadSource{.kind = PadSource::Kind::AxisLow,
                   .button = PadButton::South,
                   .axis = axis,
                   .label = FaceLabel::None};
}

constexpr PadSource fromLabel(FaceLabel label) {
  return PadSource{.kind = PadSource::Kind::Label,
                   .button = PadButton::South,
                   .axis = PadAxis::LeftX,
                   .label = label};
}

struct NamedSource {
  std::string_view name;
  PadSource source;
};

// Every phrase a configuration file may name a press by. The four face positions
// are compass points and the four letters are letters, which is the distinction the
// whole vocabulary exists to keep: a pad library's own strings call the south
// position "a", and that is exactly the confusion this avoids. A stick's Y runs
// positive downward, so its up is the negative direction.
constexpr NamedSource kSources[] = {
    {"south", fromButton(PadButton::South)},
    {"east", fromButton(PadButton::East)},
    {"west", fromButton(PadButton::West)},
    {"north", fromButton(PadButton::North)},
    {"dpad up", fromButton(PadButton::DpadUp)},
    {"dpad down", fromButton(PadButton::DpadDown)},
    {"dpad left", fromButton(PadButton::DpadLeft)},
    {"dpad right", fromButton(PadButton::DpadRight)},
    {"left shoulder", fromButton(PadButton::LeftShoulder)},
    {"right shoulder", fromButton(PadButton::RightShoulder)},
    {"start", fromButton(PadButton::Start)},
    {"back", fromButton(PadButton::Back)},
    {"guide", fromButton(PadButton::Guide)},
    {"left stick click", fromButton(PadButton::LeftStickClick)},
    {"right stick click", fromButton(PadButton::RightStickClick)},
    {"left trigger", fromAxisHigh(PadAxis::LeftTrigger)},
    {"right trigger", fromAxisHigh(PadAxis::RightTrigger)},
    {"left stick up", fromAxisLow(PadAxis::LeftY)},
    {"left stick down", fromAxisHigh(PadAxis::LeftY)},
    {"left stick left", fromAxisLow(PadAxis::LeftX)},
    {"left stick right", fromAxisHigh(PadAxis::LeftX)},
    {"right stick up", fromAxisLow(PadAxis::RightY)},
    {"right stick down", fromAxisHigh(PadAxis::RightY)},
    {"right stick left", fromAxisLow(PadAxis::RightX)},
    {"right stick right", fromAxisHigh(PadAxis::RightX)},
    {"a", fromLabel(FaceLabel::A)},
    {"b", fromLabel(FaceLabel::B)},
    {"x", fromLabel(FaceLabel::X)},
    {"y", fromLabel(FaceLabel::Y)},
};

constexpr PadFamily kFamilies[] = {
    PadFamily::Nintendo, PadFamily::Sony,     PadFamily::Xbox,
    PadFamily::GameCube, PadFamily::Standard, PadFamily::Unknown,
};

constexpr std::string_view kFamilyNames[] = {
    "nintendo", "sony", "xbox", "gamecube", "standard", "unknown",
};

// A phrase as the tables spell it: lower case, no leading or trailing spaces, and
// runs of spaces and tabs read as one, so `Left  Stick   Up` is `left stick up`.
std::string normalized(std::string_view text) {
  std::string out;
  bool pending = false;
  for (const char c : text) {
    if (c == ' ' || c == '\t') {
      pending = !out.empty();
      continue;
    }
    if (pending) {
      out.push_back(' ');
      pending = false;
    }
    out.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c);
  }
  return out;
}

bool sourceHeld(const PadInput& input, const PadSource& source) {
  switch (source.kind) {
    case PadSource::Kind::Button:
      return input.down(source.button);
    case PadSource::Kind::AxisHigh:
      return input.at(source.axis) > kStickThreshold;
    case PadSource::Kind::AxisLow:
      return input.at(source.axis) < -kStickThreshold;
    case PadSource::Kind::Label:
      // A resolved mapping holds no label: resolving one against the letters a pad
      // prints turns it into the position that carries it, or drops it.
      return false;
  }
  return false;
}

}  // namespace

std::optional<PadButton> FaceLabels::position(FaceLabel label) const noexcept {
  if (label == FaceLabel::None) return std::nullopt;
  constexpr PadButton kFace[4] = {PadButton::South, PadButton::East, PadButton::West,
                                  PadButton::North};
  for (std::size_t i = 0; i < at.size(); ++i) {
    if (at[i] == label) return kFace[i];
  }
  return std::nullopt;
}

void PadInput::press(PadButton button, bool down) noexcept {
  pressed[static_cast<std::size_t>(button)] = down;
}

void PadInput::move(PadAxis which, float position) noexcept {
  axis[static_cast<std::size_t>(which)] = position;
}

bool PadInput::down(PadButton button) const noexcept {
  return pressed[static_cast<std::size_t>(button)];
}

float PadInput::at(PadAxis which) const noexcept {
  return axis[static_cast<std::size_t>(which)];
}

std::string padSourceName(const PadSource& source) {
  for (const NamedSource& named : kSources) {
    if (named.source == source) return std::string(named.name);
  }
  return std::string();
}

std::optional<PadSource> padSourceFromName(std::string_view name) {
  const std::string wanted = normalized(name);
  for (const NamedSource& named : kSources) {
    if (named.name == wanted) return named.source;
  }
  return std::nullopt;
}

std::span<const PadFamily> padFamilies() noexcept { return kFamilies; }

std::string_view padFamilyName(PadFamily family) noexcept {
  return kFamilyNames[static_cast<std::size_t>(family)];
}

std::optional<PadFamily> padFamilyFromName(std::string_view name) {
  const std::string wanted = normalized(name);
  for (std::size_t i = 0; i < std::size(kFamilyNames); ++i) {
    if (kFamilyNames[i] == wanted) return kFamilies[i];
  }
  return std::nullopt;
}

std::span<const PadSource> GamepadMap::sourcesFor(Button button) const noexcept {
  return sources[static_cast<std::size_t>(button)];
}

std::span<const KeyId> KeyboardMap::keysFor(Button button) const noexcept {
  return keys[static_cast<std::size_t>(button)];
}

Joypad padFromGamepad(const PadInput& input, const GamepadMap& map) {
  Joypad pad;
  for (const Button button : buttons()) {
    for (const PadSource& source : map.sourcesFor(button)) {
      if (sourceHeld(input, source)) {
        pad.hold(button, true);
        break;
      }
    }
  }
  return pad;
}

Joypad padFromKeyboard(std::span<const KeyId> down, const KeyboardMap& map) {
  Joypad pad;
  for (const Button button : buttons()) {
    for (const KeyId key : map.keysFor(button)) {
      if (std::find(down.begin(), down.end(), key) != down.end()) {
        pad.hold(button, true);
        break;
      }
    }
  }
  return pad;
}

Joypad either(const Joypad& a, const Joypad& b) noexcept {
  Joypad both;
  for (const Button button : buttons()) {
    both.hold(button, a.holds(button) || b.holds(button));
  }
  return both;
}

std::optional<JoypadPort> PortAssignment::plug(int controller, PortChoice wanted) {
  for (std::size_t i = 0; i < ports_.size(); ++i) {
    if (ports_[i] == controller) return static_cast<JoypadPort>(i);
  }
  const auto take = [&](std::size_t i) -> std::optional<JoypadPort> {
    if (ports_[i].has_value()) return std::nullopt;  // pinned to a port already taken
    ports_[i] = controller;
    return static_cast<JoypadPort>(i);
  };
  if (wanted == PortChoice::One) return take(0);
  if (wanted == PortChoice::Two) return take(1);
  for (std::size_t i = 0; i < ports_.size(); ++i) {
    if (!ports_[i].has_value()) return take(i);
  }
  return std::nullopt;  // both ports are full; the controller is left idle
}

void PortAssignment::unplug(int controller) {
  for (std::optional<int>& port : ports_) {
    if (port == controller) port.reset();
  }
}

std::optional<int> PortAssignment::on(JoypadPort port) const noexcept {
  return ports_[static_cast<std::size_t>(port)];
}

void PadRecorder::frame(std::uint32_t at, const std::optional<Joypad>& one,
                        const std::optional<Joypad>& two) {
  if (!started_) {
    // Both ports are written whatever they hold, so a replay's ports are the live
    // run's ports from power-on rather than from the first button pressed.
    started_ = true;
    port(at, JoypadPort::One, one);
    port(at, JoypadPort::Two, two);
    last_[0] = one;
    last_[1] = two;
    return;
  }
  if (one != last_[0]) {
    port(at, JoypadPort::One, one);
    last_[0] = one;
  }
  if (two != last_[1]) {
    port(at, JoypadPort::Two, two);
    last_[1] = two;
  }
}

void PadRecorder::port(std::uint32_t at, JoypadPort which, const std::optional<Joypad>& held) {
  script_.events.push_back(disasm::InputEvent{.frame = at,
                                              .port = which,
                                              .plugged = held.has_value(),
                                              .pad = held.value_or(Joypad{})});
}

}  // namespace snaggletooth::player
