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

// Sets the named button on `pad`; false for a word that names none.
bool press(Joypad& pad, const std::string& name, bool& already) {
  bool* button = nullptr;
  if (name == "b") button = &pad.b;
  else if (name == "y") button = &pad.y;
  else if (name == "select") button = &pad.select;
  else if (name == "start") button = &pad.start;
  else if (name == "up") button = &pad.up;
  else if (name == "down") button = &pad.down;
  else if (name == "left") button = &pad.left;
  else if (name == "right") button = &pad.right;
  else if (name == "a") button = &pad.a;
  else if (name == "x") button = &pad.x;
  else if (name == "l") button = &pad.l;
  else if (name == "r") button = &pad.r;
  if (button == nullptr) return false;
  already = *button;
  *button = true;
  return true;
}

}  // namespace

bool InputScript::names(JoypadPort port) const noexcept {
  return std::any_of(events.begin(), events.end(),
                     [port](const InputEvent& e) { return e.port == port; });
}

std::optional<Joypad> InputScript::padAt(JoypadPort port, std::uint32_t frame) const noexcept {
  if (!names(port)) return std::nullopt;
  Joypad held{};  // nothing pressed until the first line
  for (const InputEvent& e : events) {
    if (e.frame > frame) break;
    if (e.port == port) held = e.pad;
  }
  return held;
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
    if (field.size() == 4 && lower(field[3]) == "none") {
      // nothing pressed
    } else {
      for (std::size_t i = 3; i < field.size(); ++i) {
        const std::string name = lower(field[i]);
        bool already = false;
        if (name == "none") return fail("`none` stands alone; it is not a button");
        if (!press(pad, name, already)) {
          return fail("`" + field[i] + "` is not a button; the buttons are b y select start up down left right a x l r, or none");
        }
        if (already) return fail("`" + field[i] + "` is named twice");
      }
    }
    script.events.push_back(InputEvent{.frame = frame, .port = port, .pad = pad});
  }
  return script;
}

}  // namespace snaggletooth::disasm
