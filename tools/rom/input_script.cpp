#include "rom/input_script.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <string>
#include <vector>

namespace snaggletooth::disasm {
namespace {

std::string lower(std::string_view word) {
  std::string out(word);
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

// The words of a line up to its comment, split on spaces and tabs.
std::vector<std::string> words(std::string_view line) {
  const std::size_t comment = line.find(';');
  if (comment != std::string_view::npos) line = line.substr(0, comment);
  std::vector<std::string> out;
  std::string word;
  for (const char c : line) {
    if (c == ' ' || c == '\t' || c == '\r') {
      if (!word.empty()) out.push_back(word);
      word.clear();
    } else {
      word.push_back(c);
    }
  }
  if (!word.empty()) out.push_back(word);
  return out;
}

// Sets the named button on `pad`; false for a word that names none. The names are
// the machine's own, so a script, a configuration file and a program that prints a
// button all spell the twelve the same way.
bool press(Joypad& pad, const std::string& name, bool& already) {
  const std::optional<Button> button = buttonFromName(name);
  if (!button) return false;
  already = pad.holds(*button);
  pad.hold(*button, true);
  return true;
}

// The twelve, spaced, for the refusal that names what a button word may be.
std::string buttonList() {
  std::string out;
  for (const Button button : snaggletooth::buttons()) {
    if (!out.empty()) out.push_back(' ');
    out.append(buttonName(button));
  }
  return out;
}

}  // namespace

bool InputScript::names(JoypadPort port) const noexcept {
  return std::any_of(events.begin(), events.end(),
                     [port](const InputEvent& e) { return e.port == port; });
}

std::optional<Joypad> InputScript::padAt(JoypadPort port, std::uint32_t frame) const noexcept {
  const InputEvent* first = nullptr;     // the port's own first line
  const InputEvent* governing = nullptr; // its last line at or before the frame
  for (const InputEvent& e : events) {
    if (e.port != port) continue;
    if (first == nullptr) first = &e;
    if (e.frame > frame) break;
    governing = &e;
  }
  if (first == nullptr) return std::nullopt;  // a port the script never names
  // Before its first line the port holds nothing pressed — and has a controller
  // exactly when that first line does, so a run whose second pad arrived part-way
  // through replays with the port empty until it did.
  const InputEvent& said = governing != nullptr ? *governing : *first;
  if (!said.plugged) return std::nullopt;
  return governing != nullptr ? governing->pad : Joypad{};
}

std::string writeInputScript(const InputScript& script) {
  std::string out;
  for (const InputEvent& e : script.events) {
    out += "frame ";
    out += std::to_string(e.frame);
    out += e.port == JoypadPort::One ? " 1 " : " 2 ";
    if (!e.plugged) {
      out += "unplugged";
    } else {
      std::size_t written = 0;
      for (const Button button : snaggletooth::buttons()) {
        if (!e.pad.holds(button)) continue;
        if (written++ != 0) out.push_back(' ');
        out.append(buttonName(button));
      }
      if (written == 0) out += "none";
    }
    out.push_back('\n');
  }
  return out;
}

std::optional<InputScript> parseInputScript(std::string_view text, std::string& error) {
  InputScript script;
  std::size_t lineNumber = 0;
  std::size_t start = 0;
  while (start <= text.size()) {
    const std::size_t end = text.find('\n', start);
    const std::string_view line =
        text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
    start = end == std::string_view::npos ? text.size() + 1 : end + 1;
    ++lineNumber;

    const std::vector<std::string> field = words(line);
    if (field.empty()) continue;
    const auto fail = [&](const std::string& why) {
      error = "line " + std::to_string(lineNumber) + ": " + why;
      return std::optional<InputScript>{};
    };

    if (lower(field[0]) != "frame") return fail("a line begins with `frame`, not `" + field[0] + "`");
    if (field.size() < 4) return fail("`frame <n> <port> <buttons>` needs a frame, a port and at least one button word");

    std::uint32_t frame = 0;
    const std::string& number = field[1];
    const std::from_chars_result parsed =
        std::from_chars(number.data(), number.data() + number.size(), frame);
    if (parsed.ec != std::errc{} || parsed.ptr != number.data() + number.size()) {
      return fail("`" + number + "` is not a frame number");
    }
    if (!script.events.empty() && frame < script.events.back().frame) {
      return fail("frame " + number + " comes after frame " +
                  std::to_string(script.events.back().frame) + "; lines run in frame order");
    }

    JoypadPort port;
    if (field[2] == "1") port = JoypadPort::One;
    else if (field[2] == "2") port = JoypadPort::Two;
    else return fail("`" + field[2] + "` is not a port; the ports are 1 and 2");
    for (const InputEvent& e : script.events) {
      if (e.frame == frame && e.port == port) {
        return fail("port " + field[2] + " is already given for frame " + number);
      }
    }

    Joypad pad;
    bool plugged = true;
    if (field.size() == 4 && lower(field[3]) == "none") {
      // a controller with nothing pressed
    } else if (field.size() == 4 && lower(field[3]) == "unplugged") {
      plugged = false;  // no controller in the port from this frame
    } else {
      for (std::size_t i = 3; i < field.size(); ++i) {
        const std::string name = lower(field[i]);
        bool already = false;
        if (name == "none") return fail("`none` stands alone; it is not a button");
        if (name == "unplugged") return fail("`unplugged` stands alone; it is not a button");
        if (!press(pad, name, already)) {
          return fail("`" + field[i] + "` is not a button; the buttons are " + buttonList() +
                      ", or none");
        }
        if (already) return fail("`" + field[i] + "` is named twice");
      }
    }
    script.events.push_back(
        InputEvent{.frame = frame, .port = port, .plugged = plugged, .pad = pad});
  }
  return script;
}

std::filesystem::path scriptPathFor(const std::filesystem::path& directory,
                                    const std::filesystem::path& image) {
  std::string name = image.stem().string();
  for (char& c : name) {
    if (c == ' ') c = '_';
  }
  return directory / (name + ".snaginput");
}

std::filesystem::path scriptFor(const std::filesystem::path& directory,
                                const std::filesystem::path& image) {
  const std::filesystem::path own = scriptPathFor(directory, image);
  if (std::filesystem::is_regular_file(own)) return own;
  const std::filesystem::path fallback = directory / "default.snaginput";
  if (std::filesystem::is_regular_file(fallback)) return fallback;
  return own;
}

}  // namespace snaggletooth::disasm
