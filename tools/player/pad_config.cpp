#include "player/pad_config.h"

#include <cstddef>

namespace snaggletooth::player {
namespace {

std::string_view trimmed(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
    text.remove_suffix(1);
  }
  return text;
}

std::string lowered(std::string_view text) {
  std::string out(text);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return out;
}

// The comma-separated values of one line, each trimmed. An empty piece is kept, so
// a trailing comma is caught rather than quietly dropped.
std::vector<std::string_view> pieces(std::string_view value) {
  std::vector<std::string_view> out;
  std::size_t start = 0;
  while (true) {
    const std::size_t comma = value.find(',', start);
    out.push_back(trimmed(value.substr(
        start, comma == std::string_view::npos ? std::string_view::npos : comma - start)));
    if (comma == std::string_view::npos) break;
    start = comma + 1;
  }
  return out;
}

std::optional<PortChoice> portFromName(std::string_view name) {
  if (name == "auto") return PortChoice::Auto;
  if (name == "1") return PortChoice::One;
  if (name == "2") return PortChoice::Two;
  return std::nullopt;
}

// Which section a header names. A header this does not know refuses the file.
enum class SectionKind : std::uint8_t { None, Keyboard, Gamepad, Family, Hotkeys };

}  // namespace

std::optional<PadConfig> parsePadConfig(std::string_view text, std::string& error) {
  PadConfig config;
  SectionKind kind = SectionKind::None;
  PadFamily family = PadFamily::Unknown;
  bool seenHotkeys = false;
  std::size_t lineNumber = 0;
  std::size_t start = 0;

  while (start <= text.size()) {
    const std::size_t end = text.find('\n', start);
    std::string_view line =
        text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
    start = end == std::string_view::npos ? text.size() + 1 : end + 1;
    ++lineNumber;

    const std::size_t comment = line.find('#');
    if (comment != std::string_view::npos) line = line.substr(0, comment);
    line = trimmed(line);
    if (line.empty()) continue;

    const auto fail = [&](const std::string& why) {
      error = "line " + std::to_string(lineNumber) + ": " + why;
      return std::optional<PadConfig>{};
    };

    if (line.front() == '[') {
      if (line.back() != ']') return fail("a section header ends with `]`");
      const std::string header = lowered(trimmed(line.substr(1, line.size() - 2)));
      if (header == "keyboard") {
        if (config.hasKeyboard) return fail("`[keyboard]` is given twice");
        config.hasKeyboard = true;
        kind = SectionKind::Keyboard;
      } else if (header == "gamepad") {
        if (config.hasGamepad) return fail("`[gamepad]` is given twice");
        config.hasGamepad = true;
        kind = SectionKind::Gamepad;
      } else if (header == "hotkeys") {
        if (seenHotkeys) return fail("`[hotkeys]` is given twice");
        seenHotkeys = true;
        kind = SectionKind::Hotkeys;
      } else if (header.rfind("gamepad ", 0) == 0) {
        const std::optional<PadFamily> named = padFamilyFromName(header.substr(8));
        if (!named) return fail("`" + header.substr(8) + "` is not a pad family");
        if (config.families.count(*named) != 0) {
          return fail("`[gamepad " + std::string(padFamilyName(*named)) + "]` is given twice");
        }
        family = *named;
        config.families[family] = GamepadSection{};
        kind = SectionKind::Family;
      } else {
        return fail("`" + header + "` is not a section of this file");
      }
      continue;
    }

    const std::size_t equals = line.find('=');
    if (equals == std::string_view::npos) {
      return fail("a line is either a `[section]` header or `key = value`");
    }
    const std::string key = lowered(trimmed(line.substr(0, equals)));
    const std::string_view value = trimmed(line.substr(equals + 1));

    if (kind == SectionKind::None) return fail("`" + key + "` is before any section");
    if (kind == SectionKind::Hotkeys) {
      return fail("no hotkey is defined in this release, so `" + key + "` names nothing");
    }
    if (value.empty()) return fail("`" + key + "` is given nothing to be held by");

    GamepadSection* pad = kind == SectionKind::Gamepad    ? &config.gamepad
                          : kind == SectionKind::Family   ? &config.families[family]
                                                          : nullptr;

    if (key == "port") {
      const std::optional<PortChoice> choice = portFromName(lowered(value));
      if (!choice) return fail("`" + std::string(value) + "` is not a port; they are auto, 1 and 2");
      if (kind == SectionKind::Keyboard) {
        if (*choice == PortChoice::Auto) {
          return fail("the keyboard is on a port of its own; it is 1 or 2, never auto");
        }
        if (config.keyboard.port.has_value()) return fail("`port` is given twice");
        config.keyboard.port = *choice;
      } else {
        if (pad->port.has_value()) return fail("`port` is given twice");
        pad->port = *choice;
      }
      continue;
    }

    const std::optional<Button> button = buttonFromName(key);
    if (!button) {
      return fail("`" + key + "` is not a button of the machine, and not `port`");
    }
    const std::size_t at = static_cast<std::size_t>(*button);

    if (kind == SectionKind::Keyboard) {
      if (config.keyboard.given[at]) return fail("`" + key + "` is given twice");
      config.keyboard.given[at] = true;
      for (const std::string_view piece : pieces(value)) {
        if (piece.empty()) return fail("`" + key + "` names an empty key");
        config.keyboard.keys[at].emplace_back(piece);
      }
      continue;
    }

    if (pad->given[at]) return fail("`" + key + "` is given twice");
    pad->given[at] = true;
    for (const std::string_view piece : pieces(value)) {
      const std::optional<PadSource> source = padSourceFromName(piece);
      if (!source) {
        return fail("`" + std::string(piece) + "` is not a place on a pad");
      }
      pad->sources[at].push_back(*source);
    }
  }

  // A plain section is the whole mapping rather than a patch, so it says what holds
  // every one of the twelve. A family's section is the patch, and says what it likes.
  const auto complete = [&](const std::array<bool, kButtonCount>& given,
                            std::string_view section) {
    for (const Button button : buttons()) {
      if (given[static_cast<std::size_t>(button)]) continue;
      error = std::string(section) + " does not say what holds " + std::string(buttonName(button));
      return false;
    }
    return true;
  };
  if (config.hasKeyboard && !complete(config.keyboard.given, "`[keyboard]`")) return std::nullopt;
  if (config.hasGamepad && !complete(config.gamepad.given, "`[gamepad]`")) return std::nullopt;

  // Two controllers pinned to one port would fight over it, so the file says so
  // rather than whichever arrives second being silently left idle.
  std::optional<PortChoice> pinnedByKeyboard = config.keyboard.port;
  for (const auto& [named, section] : config.families) {
    if (!section.port.has_value() || *section.port == PortChoice::Auto) continue;
    if (pinnedByKeyboard.has_value() && *section.port == *pinnedByKeyboard) {
      error = "`[gamepad " + std::string(padFamilyName(named)) +
              "]` and `[keyboard]` are both pinned to port " +
              (*section.port == PortChoice::One ? "1" : "2");
      return std::nullopt;
    }
    for (const auto& [other, otherSection] : config.families) {
      if (other == named || !otherSection.port.has_value()) continue;
      if (*otherSection.port == *section.port) {
        error = "`[gamepad " + std::string(padFamilyName(named)) + "]` and `[gamepad " +
                std::string(padFamilyName(other)) + "]` are both pinned to port " +
                (*section.port == PortChoice::One ? "1" : "2");
        return std::nullopt;
      }
    }
  }

  return config;
}

void fillFrom(PadConfig& config, const PadConfig& defaults) {
  if (!config.hasKeyboard) {
    config.keyboard = defaults.keyboard;
    config.hasKeyboard = defaults.hasKeyboard;
  }
  if (!config.hasGamepad) {
    config.gamepad = defaults.gamepad;
    config.hasGamepad = defaults.hasGamepad;
  }
}

GamepadMap resolve(const PadConfig& config, PadFamily family, const FaceLabels& labels) {
  GamepadMap map;
  const auto found = config.families.find(family);
  const GamepadSection* own = found != config.families.end() ? &found->second : nullptr;

  map.port = config.gamepad.port.value_or(PortChoice::Auto);
  if (own != nullptr && own->port.has_value()) map.port = *own->port;

  for (const Button button : buttons()) {
    const std::size_t at = static_cast<std::size_t>(button);
    const std::vector<PadSource>& from =
        (own != nullptr && own->given[at]) ? own->sources[at] : config.gamepad.sources[at];
    for (const PadSource& source : from) {
      if (source.kind != PadSource::Kind::Label) {
        map.sources[at].push_back(source);
        continue;
      }
      const std::optional<PadButton> where = labels.position(source.label);
      if (!where) {
        map.unprintedLabel = true;  // this pad prints no such letter; nothing is bound
        continue;
      }
      map.sources[at].push_back(
          PadSource{.kind = PadSource::Kind::Button,
                    .button = *where,
                    .axis = PadAxis::LeftX,
                    .label = FaceLabel::None});
    }
  }
  return map;
}

std::optional<KeyboardMap> resolveKeyboard(
    const PadConfig& config,
    const std::function<std::optional<KeyId>(std::string_view)>& idFor,
    std::string& error) {
  KeyboardMap map;
  map.port = config.keyboard.port.value_or(PortChoice::One) == PortChoice::Two ? JoypadPort::Two
                                                                               : JoypadPort::One;
  for (const Button button : buttons()) {
    const std::size_t at = static_cast<std::size_t>(button);
    for (const std::string& name : config.keyboard.keys[at]) {
      const std::optional<KeyId> id = idFor(name);
      if (!id) {
        error = "`" + name + "` is not a key this keyboard has";
        return std::nullopt;
      }
      map.keys[at].push_back(*id);
    }
  }
  return map;
}

}  // namespace snaggletooth::player
