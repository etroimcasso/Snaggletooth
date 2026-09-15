// The player's input: the pad vocabulary, the mapping a configuration resolves to,
// which port a controller takes, and the run recorded as a script. None of it opens
// a device — a pad's state is a value handed in, and the letters a pad prints are
// reported by whoever owns it — so every case here runs on every platform.

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "player/default_snagpad.h"
#include "player/pad_config.h"
#include "player/pads.h"
#include "rom/input_script.h"

namespace snaggletooth::player {
namespace {

using disasm::InputScript;
using disasm::parseInputScript;
using disasm::writeInputScript;

PadConfig parsed(std::string_view text) {
  std::string error;
  const std::optional<PadConfig> config = parsePadConfig(text, error);
  EXPECT_TRUE(config.has_value()) << error;
  return config.value_or(PadConfig{});
}

std::string refusal(std::string_view text) {
  std::string error;
  const std::optional<PadConfig> config = parsePadConfig(text, error);
  EXPECT_FALSE(config.has_value()) << "accepted: " << text;
  return error;
}

PadConfig theDefault() { return parsed(kDefaultPadConfig); }

// The twelve key names the shipped configuration uses, numbered as an application
// that owns a keyboard would number them. The names themselves are whoever owns the
// keyboard's business; what this pins is that each one reaches exactly one button.
const std::map<std::string, KeyId>& keyNumbers() {
  static const std::map<std::string, KeyId> kKeys{
      {"z", 0},   {"x", 1},    {"a", 2},          {"s", 3},   {"q", 4},    {"w", 5},
      {"return", 6}, {"right shift", 7}, {"up", 8}, {"down", 9}, {"left", 10}, {"right", 11},
  };
  return kKeys;
}

std::optional<KeyId> numberFor(std::string_view name) {
  const auto found = keyNumbers().find(std::string(name));
  if (found == keyNumbers().end()) return std::nullopt;
  return found->second;
}

KeyboardMap defaultKeyboard() {
  std::string error;
  const std::optional<KeyboardMap> map = resolveKeyboard(theDefault(), numberFor, error);
  EXPECT_TRUE(map.has_value()) << error;
  return map.value_or(KeyboardMap{});
}

// A pad holding exactly one position.
PadInput holding(PadButton button) {
  PadInput input;
  input.press(button, true);
  return input;
}

// Which buttons a pad holds, as a sorted list of names, so a failure reads.
std::string held(const Joypad& pad) {
  std::string out;
  for (const Button button : buttons()) {
    if (!pad.holds(button)) continue;
    if (!out.empty()) out.push_back(' ');
    out.append(buttonName(button));
  }
  return out.empty() ? "nothing" : out;
}

}  // namespace

// ---- the families ------------------------------------------------------------------

TEST(PlayerPads, EveryFamilyHasAConfigSectionName) {
  ASSERT_EQ(padFamilies().size(), 6u);
  for (const PadFamily family : padFamilies()) {
    const std::string_view name = padFamilyName(family);
    EXPECT_FALSE(name.empty());
    EXPECT_EQ(padFamilyFromName(name), family) << name;
    EXPECT_EQ(padFamilyFromName(std::string(name) + "  "), family) << "spaces around it";
  }
  // Each family by name, spelled out. The round trip above cannot see two families
  // swapping names — it reads the same table both ways, so a swap stays consistent
  // with itself — and the name is what a person writes in a section header.
  EXPECT_EQ(padFamilyName(PadFamily::Nintendo), "nintendo");
  EXPECT_EQ(padFamilyName(PadFamily::Sony), "sony");
  EXPECT_EQ(padFamilyName(PadFamily::Xbox), "xbox");
  EXPECT_EQ(padFamilyName(PadFamily::GameCube), "gamecube");
  EXPECT_EQ(padFamilyName(PadFamily::Standard), "standard");
  EXPECT_EQ(padFamilyName(PadFamily::Unknown), "unknown");
  EXPECT_FALSE(padFamilyFromName("dreamcast").has_value());
  EXPECT_FALSE(padFamilyFromName("").has_value());
}

// ---- the mapping -------------------------------------------------------------------

TEST(PlayerPads, AGamepadMapsByPositionOnEveryFamilyButOne) {
  const PadConfig config = theDefault();
  for (const PadFamily family :
       {PadFamily::Nintendo, PadFamily::Sony, PadFamily::Xbox, PadFamily::Standard,
        PadFamily::Unknown}) {
    const GamepadMap map = resolve(config, family, kAbxy);
    EXPECT_FALSE(map.unprintedLabel) << padFamilyName(family);
    EXPECT_EQ(held(padFromGamepad(holding(PadButton::South), map)), "b") << padFamilyName(family);
    EXPECT_EQ(held(padFromGamepad(holding(PadButton::East), map)), "a") << padFamilyName(family);
    EXPECT_EQ(held(padFromGamepad(holding(PadButton::West), map)), "y") << padFamilyName(family);
    EXPECT_EQ(held(padFromGamepad(holding(PadButton::North), map)), "x") << padFamilyName(family);
    EXPECT_EQ(held(padFromGamepad(holding(PadButton::LeftShoulder), map)), "l");
    EXPECT_EQ(held(padFromGamepad(holding(PadButton::RightShoulder), map)), "r");
    EXPECT_EQ(held(padFromGamepad(holding(PadButton::Start), map)), "start");
    EXPECT_EQ(held(padFromGamepad(holding(PadButton::Back), map)), "select");
    EXPECT_EQ(held(padFromGamepad(holding(PadButton::DpadUp), map)), "up");
    EXPECT_EQ(held(padFromGamepad(holding(PadButton::DpadDown), map)), "down");
    EXPECT_EQ(held(padFromGamepad(holding(PadButton::DpadLeft), map)), "left");
    EXPECT_EQ(held(padFromGamepad(holding(PadButton::DpadRight), map)), "right");
    EXPECT_EQ(held(padFromGamepad(holding(PadButton::Guide), map)), "nothing")
        << "nothing is bound to it";
    EXPECT_EQ(held(padFromGamepad(holding(PadButton::LeftStickClick), map)), "nothing");
  }
}

TEST(PlayerPads, TheGameCubeMapsByItsLabels) {
  // The pad prints A at south, X at east, B at west and Y at north, so reading it by
  // its letters is not the same as reading it by its positions — which is the whole
  // reason it has a section of its own.
  const GamepadMap map = resolve(theDefault(), PadFamily::GameCube, kAxby);
  EXPECT_FALSE(map.unprintedLabel);
  EXPECT_EQ(held(padFromGamepad(holding(PadButton::South), map)), "a") << "the button printed A";
  EXPECT_EQ(held(padFromGamepad(holding(PadButton::West), map)), "b") << "the button printed B";
  EXPECT_EQ(held(padFromGamepad(holding(PadButton::East), map)), "x") << "the button printed X";
  EXPECT_EQ(held(padFromGamepad(holding(PadButton::North), map)), "y") << "the button printed Y";

  PadInput triggers;
  triggers.move(PadAxis::LeftTrigger, 1.0f);
  EXPECT_EQ(held(padFromGamepad(triggers, map)), "l");
  triggers.move(PadAxis::LeftTrigger, 0.0f);
  triggers.move(PadAxis::RightTrigger, 1.0f);
  EXPECT_EQ(held(padFromGamepad(triggers, map)), "r");
  EXPECT_EQ(held(padFromGamepad(holding(PadButton::LeftShoulder), map)), "nothing")
      << "its shoulders are not its L and R";

  // What the section does not name it takes from the one above it.
  EXPECT_EQ(held(padFromGamepad(holding(PadButton::Start), map)), "start");
  EXPECT_EQ(held(padFromGamepad(holding(PadButton::Back), map)), "select");
  EXPECT_EQ(held(padFromGamepad(holding(PadButton::DpadLeft), map)), "left");
}

TEST(PlayerPads, ALabelWordOnAPadWithoutLabelsIsNothing) {
  const GamepadMap map = resolve(theDefault(), PadFamily::GameCube, kNoLabels);
  EXPECT_TRUE(map.unprintedLabel) << "the tool says so once rather than leaving them dead";
  for (const PadButton face :
       {PadButton::South, PadButton::East, PadButton::West, PadButton::North}) {
    EXPECT_EQ(held(padFromGamepad(holding(face), map)), "nothing");
  }
}

TEST(PlayerPads, TheLeftStickPastTheThresholdIsADirection) {
  const GamepadMap map = resolve(theDefault(), PadFamily::Standard, kAbxy);
  PadInput input;
  input.move(PadAxis::LeftX, 0.51f);
  EXPECT_EQ(held(padFromGamepad(input, map)), "right");
  input.move(PadAxis::LeftX, 0.49f);
  EXPECT_EQ(held(padFromGamepad(input, map)), "nothing") << "short of the threshold";
  input.move(PadAxis::LeftX, -0.51f);
  EXPECT_EQ(held(padFromGamepad(input, map)), "left");

  // The two axes answer independently, and a stick adds to the d-pad rather than
  // replacing it.
  input.move(PadAxis::LeftY, -0.51f);
  EXPECT_EQ(held(padFromGamepad(input, map)), "up left") << "Y runs positive downward";
  input.move(PadAxis::LeftX, 0.0f);
  input.move(PadAxis::LeftY, 0.51f);
  PadInput both = input;
  both.press(PadButton::DpadRight, true);
  EXPECT_EQ(held(padFromGamepad(both, map)), "down right");
}

// ---- the keyboard ------------------------------------------------------------------

TEST(PlayerPads, TheKeyboardDefaultMapsTheSnesLayoutByPosition) {
  const KeyboardMap map = defaultKeyboard();
  EXPECT_EQ(map.port, JoypadPort::One);
  const std::map<std::string, std::string> expected{
      {"z", "b"},   {"x", "a"},           {"a", "y"},    {"s", "x"},
      {"q", "l"},   {"w", "r"},           {"return", "start"}, {"right shift", "select"},
      {"up", "up"}, {"down", "down"},     {"left", "left"},    {"right", "right"},
  };
  for (const auto& [name, button] : expected) {
    const KeyId key = keyNumbers().at(name);
    const KeyId one[1] = {key};
    EXPECT_EQ(held(padFromKeyboard(one, map)), button) << name;
  }
}

TEST(PlayerPads, KeysHeldTogetherAreAllPressed) {
  const KeyboardMap map = defaultKeyboard();
  const KeyId down[3] = {keyNumbers().at("right"), keyNumbers().at("z"),
                         keyNumbers().at("return")};
  EXPECT_EQ(held(padFromKeyboard(down, map)), "b start right");
}

TEST(PlayerPads, TheKeyboardAndAPadShareAPort) {
  const KeyboardMap keys = defaultKeyboard();
  const GamepadMap pad = resolve(theDefault(), PadFamily::Standard, kAbxy);
  const KeyId down[1] = {keyNumbers().at("z")};
  const Joypad together =
      either(padFromKeyboard(down, keys), padFromGamepad(holding(PadButton::East), pad));
  EXPECT_EQ(held(together), "b a") << "a hand on each is one player";
}

// ---- the ports ---------------------------------------------------------------------

TEST(PlayerPads, ThePortsFillAsPadsArePluggedIn) {
  PortAssignment ports;
  EXPECT_EQ(ports.plug(10, PortChoice::Auto), JoypadPort::One);
  EXPECT_EQ(ports.plug(20, PortChoice::Auto), JoypadPort::Two);
  EXPECT_FALSE(ports.plug(30, PortChoice::Auto).has_value()) << "a third is left idle";
  EXPECT_EQ(ports.on(JoypadPort::One), 10);
  EXPECT_EQ(ports.on(JoypadPort::Two), 20);

  ports.unplug(20);
  EXPECT_FALSE(ports.on(JoypadPort::Two).has_value());
  EXPECT_EQ(ports.plug(30, PortChoice::Auto), JoypadPort::Two)
      << "the next takes the port that came free, not the one still held";
  EXPECT_EQ(ports.on(JoypadPort::One), 10);
}

TEST(PlayerPads, APinnedPortIsTakenBeforeAnAutoOne) {
  PortAssignment ports;
  EXPECT_EQ(ports.plug(10, PortChoice::Two), JoypadPort::Two) << "even as the first in";
  EXPECT_EQ(ports.plug(20, PortChoice::Auto), JoypadPort::One);
  EXPECT_FALSE(ports.plug(30, PortChoice::Two).has_value()) << "its port is taken";
  EXPECT_EQ(ports.plug(10, PortChoice::Two), JoypadPort::Two) << "one already in keeps its port";
}

// ---- the configuration -------------------------------------------------------------

TEST(PlayerConfig, TheDefaultConfigParsesAndIsTheEmbeddedText) {
  const std::string onDisk = [] {
    std::string text;
    std::ifstream in(std::string(SNAGGLETOOTH_SOURCE_DIR) + "/tools/player/default.snagpad",
                     std::ios::binary);
    EXPECT_TRUE(in.good()) << "the shipped configuration is missing";
    text.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return text;
  }();
  EXPECT_EQ(onDisk, std::string(kDefaultPadConfig))
      << "the file and the built-in default are one thing";
  std::string error;
  EXPECT_TRUE(parsePadConfig(onDisk, error).has_value()) << error;
}

TEST(PlayerConfig, AConfigReadsSectionsCommentsAndLists) {
  const PadConfig config = parsed(
      "# a comment\n"
      "\n"
      "[GamePad]          # read in any case\n"
      "port = AUTO\n"
      "b = South\n"
      "a = east\n"
      "y = west\n"
      "x = north\n"
      "l = left shoulder\n"
      "r = right shoulder\n"
      "start = start\n"
      "select = back\n"
      "up = dpad up,  left  stick  up\n"
      "down = dpad down\n"
      "left = dpad left\n"
      "right = dpad right\n");
  EXPECT_TRUE(config.hasGamepad);
  EXPECT_FALSE(config.hasKeyboard);
  const GamepadMap map = resolve(config, PadFamily::Standard, kAbxy);
  EXPECT_EQ(map.port, PortChoice::Auto);
  EXPECT_EQ(map.sourcesFor(Button::Up).size(), 2u) << "a comma-separated list, spaces collapsed";
  PadInput stick;
  stick.move(PadAxis::LeftY, -1.0f);
  EXPECT_EQ(held(padFromGamepad(stick, map)), "up");
}

TEST(PlayerConfig, AFamilySectionOverridesKeyByKey) {
  PadConfig config = parsed(
      "[gamepad sony]\n"
      "port = 2\n"
      "b = east\n");
  fillFrom(config, theDefault());

  const GamepadMap sony = resolve(config, PadFamily::Sony, kAbxy);
  EXPECT_EQ(sony.port, PortChoice::Two);
  EXPECT_EQ(held(padFromGamepad(holding(PadButton::East), sony)), "b a")
      << "east now holds B as well, and still holds A from the section above";
  EXPECT_EQ(held(padFromGamepad(holding(PadButton::South), sony)), "nothing")
      << "b was replaced, not added to";
  EXPECT_EQ(held(padFromGamepad(holding(PadButton::North), sony)), "x") << "every other key stands";

  const GamepadMap xbox = resolve(config, PadFamily::Xbox, kAbxy);
  EXPECT_EQ(xbox.port, PortChoice::Auto);
  EXPECT_EQ(held(padFromGamepad(holding(PadButton::South), xbox)), "b") << "untouched by it";
}

TEST(PlayerConfig, AConfigOmittingASectionTakesTheDefaults) {
  PadConfig config = parsed("[gamepad sony]\nport = 2\n");
  EXPECT_FALSE(config.hasKeyboard);
  EXPECT_FALSE(config.hasGamepad);
  fillFrom(config, theDefault());
  EXPECT_TRUE(config.hasKeyboard);
  EXPECT_TRUE(config.hasGamepad);

  const GamepadMap map = resolve(config, PadFamily::Sony, kAbxy);
  EXPECT_EQ(map.port, PortChoice::Two);
  EXPECT_EQ(held(padFromGamepad(holding(PadButton::South), map)), "b");
  std::string error;
  const std::optional<KeyboardMap> keys = resolveKeyboard(config, numberFor, error);
  ASSERT_TRUE(keys.has_value()) << error;
  const KeyId one[1] = {keyNumbers().at("z")};
  EXPECT_EQ(held(padFromKeyboard(one, *keys)), "b");
}

TEST(PlayerConfig, AConfigRefusesWhatItCannotRead) {
  const std::string whole =
      "[gamepad]\nport = auto\nb = south\na = east\ny = west\nx = north\n"
      "l = left shoulder\nr = right shoulder\nstart = start\nselect = back\n"
      "up = dpad up\ndown = dpad down\nleft = dpad left\nright = dpad right\n";

  EXPECT_NE(refusal("[mouse]\n").find("not a section"), std::string::npos);
  EXPECT_NE(refusal("[gamepad dreamcast]\n").find("not a pad family"), std::string::npos);
  EXPECT_NE(refusal(whole + "trigger = south\n").find("not a button of the machine"),
            std::string::npos);
  EXPECT_NE(refusal(whole + "b = east\n").find("given twice"), std::string::npos);
  EXPECT_NE(refusal("[gamepad]\nb =\n").find("given nothing"), std::string::npos);
  EXPECT_NE(refusal("[gamepad]\nb = elbow\n").find("not a place on a pad"), std::string::npos);
  EXPECT_NE(refusal("[gamepad]\nport = 3\n").find("not a port"), std::string::npos);
  EXPECT_NE(refusal("[keyboard]\nport = auto\n").find("never auto"), std::string::npos);
  EXPECT_NE(refusal("[hotkeys]\nquit = escape\n").find("no hotkey is defined"), std::string::npos);
  EXPECT_NE(refusal("[gamepad]\nb = south\n").find("does not say what holds"), std::string::npos)
      << "a plain section is the whole mapping, not a patch";
  EXPECT_NE(refusal(whole + whole).find("given twice"), std::string::npos);
  EXPECT_NE(refusal("b = south\n").find("before any section"), std::string::npos);
  EXPECT_NE(refusal("[gamepad]\nnonsense\n").find("`[section]` header or `key = value`"),
            std::string::npos);
  EXPECT_EQ(refusal("[mouse]\n").find("line 1"), 0u) << "the error names the line";

  const std::string pinned =
      "[keyboard]\nport = 2\nb = z\na = x\ny = a\nx = s\nl = q\nr = w\n"
      "start = return\nselect = right shift\nup = up\ndown = down\nleft = left\nright = right\n"
      "[gamepad sony]\nport = 2\n";
  EXPECT_NE(refusal(pinned).find("both pinned to port 2"), std::string::npos);
}

TEST(PlayerConfig, AKeyTheKeyboardDoesNotHaveIsRefusedWhenItIsResolved) {
  PadConfig config = parsed(
      "[keyboard]\nport = 1\nb = nonesuch\na = x\ny = a\nx = s\nl = q\nr = w\n"
      "start = return\nselect = right shift\nup = up\ndown = down\nleft = left\nright = right\n");
  fillFrom(config, theDefault());
  std::string error;
  EXPECT_FALSE(resolveKeyboard(config, numberFor, error).has_value());
  EXPECT_NE(error.find("nonesuch"), std::string::npos) << error;
}

// ---- the recording -----------------------------------------------------------------

TEST(PlayerRecorder, ARecorderPlugsEveryPortAtFrameZero) {
  PadRecorder recorder;
  recorder.frame(0u, Joypad{}, std::nullopt);
  EXPECT_EQ(writeInputScript(recorder.script()), "frame 0 1 none\nframe 0 2 unplugged\n");
}

TEST(PlayerRecorder, ARecorderWritesOnlyChanges) {
  PadRecorder recorder;
  const Joypad right{.right = true};
  recorder.frame(0u, Joypad{}, std::nullopt);
  recorder.frame(1u, Joypad{}, std::nullopt);
  recorder.frame(2u, Joypad{}, std::nullopt);
  EXPECT_EQ(recorder.script().events.size(), 2u) << "nothing changed";
  recorder.frame(3u, right, std::nullopt);
  recorder.frame(4u, right, std::nullopt);
  EXPECT_EQ(recorder.script().events.size(), 3u);
  recorder.frame(5u, right, Joypad{});          // a pad arrives on port two
  recorder.frame(6u, std::nullopt, Joypad{});   // and the first is pulled out
  EXPECT_EQ(writeInputScript(recorder.script()),
            "frame 0 1 none\n"
            "frame 0 2 unplugged\n"
            "frame 3 1 right\n"
            "frame 5 2 none\n"
            "frame 6 1 unplugged\n");
}

TEST(PlayerRecorder, ARecordedRunReplaysItself) {
  const std::vector<std::pair<std::optional<Joypad>, std::optional<Joypad>>> run{
      {Joypad{}, std::nullopt},
      {Joypad{.b = true}, std::nullopt},
      {Joypad{.b = true, .right = true}, std::nullopt},
      {Joypad{.right = true}, Joypad{}},
      {Joypad{.right = true}, Joypad{.start = true}},
      {std::nullopt, Joypad{.start = true}},
      {std::nullopt, std::nullopt},
  };
  PadRecorder recorder;
  for (std::uint32_t at = 0; at < run.size(); ++at) {
    recorder.frame(at, run[at].first, run[at].second);
  }
  const std::string text = writeInputScript(recorder.script());
  std::string error;
  const std::optional<InputScript> replay = parseInputScript(text, error);
  ASSERT_TRUE(replay.has_value()) << error << "\n" << text;
  for (std::uint32_t at = 0; at < run.size(); ++at) {
    EXPECT_EQ(replay->padAt(JoypadPort::One, at), run[at].first) << "port 1 at frame " << at;
    EXPECT_EQ(replay->padAt(JoypadPort::Two, at), run[at].second) << "port 2 at frame " << at;
  }
  EXPECT_EQ(writeInputScript(*replay), text) << "writing the parse back is the same text";
}

}  // namespace snaggletooth::player
