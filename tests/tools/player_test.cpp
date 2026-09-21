// The player's input: the pad vocabulary, the mapping a configuration resolves to,
// which port a controller takes, and the run recorded as a script. None of it opens
// a device — a pad's state is a value handed in, and the letters a pad prints are
// reported by whoever owns it — so every case here runs on every platform.

#include <algorithm>
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
#include "player/display.h"
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

// The twelve key names the shipped configuration gives to buttons and the three
// the hotkey cases use, numbered as an application that owns a keyboard would
// number them. The names themselves are whoever owns the keyboard's business; what
// this pins is that each of the twelve reaches exactly one button.
const std::map<std::string, KeyId>& keyNumbers() {
  static const std::map<std::string, KeyId> kKeys{
      {"z", 0},   {"x", 1},    {"a", 2},          {"s", 3},   {"q", 4},    {"w", 5},
      {"return", 6}, {"backspace", 7}, {"up", 8}, {"down", 9}, {"left", 10}, {"right", 11},
      {"r", 12}, {"f9", 13}, {"f10", 14},
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
      {"q", "l"},   {"w", "r"},           {"return", "start"}, {"backspace", "select"},
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

// What the file on disk says and what was built into the binary from it, compared —
// which is what says the build read the file it claims to have read, and would catch
// a stale copy, a truncated read or the wrong file entirely.
//
// The comparison is of CONTENT, with line endings set aside, because the endings
// belong to whoever checked the file out rather than to the file: a checkout that
// converts them leaves the build and the repository disagreeing about nothing, on
// one platform, over a difference no reader of the configuration can observe.
// `.gitattributes` asks for the file verbatim so a fresh clone is consistent, but
// this case does not lean on that having been honoured.
TEST(PlayerConfig, TheDefaultConfigParsesAndIsTheEmbeddedText) {
  const auto lines = [](std::string text) {
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    return text;
  };
  const std::string onDisk = [] {
    std::string text;
    std::ifstream in(std::string(SNAGGLETOOTH_SOURCE_DIR) + "/tools/player/default.snagpad",
                     std::ios::binary);
    EXPECT_TRUE(in.good()) << "the shipped configuration is missing";
    text.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return text;
  }();
  EXPECT_FALSE(onDisk.empty());
  EXPECT_EQ(lines(onDisk), lines(std::string(kDefaultPadConfig)))
      << "the file and the built-in default say the same thing";
  std::string error;
  EXPECT_TRUE(parsePadConfig(onDisk, error).has_value()) << error;
  EXPECT_TRUE(parsePadConfig(kDefaultPadConfig, error).has_value()) << error;
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
  EXPECT_NE(refusal("[hotkeys]\nquit = escape\n").find("not a hotkey"), std::string::npos);
  EXPECT_NE(refusal("[hotkeys]\nreset =\n").find("given nothing"), std::string::npos);
  EXPECT_NE(refusal("[hotkeys]\nreset = f5\nreset = f6\n").find("given twice"), std::string::npos);
  EXPECT_NE(refusal("[hotkeys]\nreset = f5,\n").find("empty key"), std::string::npos);
  EXPECT_NE(refusal("[gamepad]\nb = south\n").find("does not say what holds"), std::string::npos)
      << "a plain section is the whole mapping, not a patch";
  EXPECT_NE(refusal(whole + whole).find("given twice"), std::string::npos);
  EXPECT_NE(refusal("b = south\n").find("before any section"), std::string::npos);
  EXPECT_NE(refusal("[gamepad]\nnonsense\n").find("`[section]` header or `key = value`"),
            std::string::npos);
  EXPECT_EQ(refusal("[mouse]\n").find("line 1"), 0u) << "the error names the line";

  const std::string pinned =
      "[keyboard]\nport = 2\nb = z\na = x\ny = a\nx = s\nl = q\nr = w\n"
      "start = return\nselect = backspace\nup = up\ndown = down\nleft = left\nright = right\n"
      "[gamepad sony]\nport = 2\n";
  EXPECT_NE(refusal(pinned).find("both pinned to port 2"), std::string::npos);
}

TEST(PlayerConfig, AKeyTheKeyboardDoesNotHaveIsRefusedWhenItIsResolved) {
  PadConfig config = parsed(
      "[keyboard]\nport = 1\nb = nonesuch\na = x\ny = a\nx = s\nl = q\nr = w\n"
      "start = return\nselect = backspace\nup = up\ndown = down\nleft = left\nright = right\n");
  fillFrom(config, theDefault());
  std::string error;
  EXPECT_FALSE(resolveKeyboard(config, numberFor, error).has_value());
  EXPECT_NE(error.find("nonesuch"), std::string::npos) << error;
}

// ---- the hotkeys -------------------------------------------------------------------

TEST(PlayerConfig, TheShippedResetChordIsCommandOnAMacAndControlElsewhereAndNeverBoth) {
  std::string error;
  const std::optional<KeyboardMap> keys = resolveKeyboard(theDefault(), numberFor, error);
  ASSERT_TRUE(keys.has_value()) << error;
  const KeyId r = keyNumbers().at("r");

  const std::optional<HotkeyMap> mac =
      resolveHotkeys(theDefault(), *keys, KeyboardKind::Mac, numberFor, error);
  ASSERT_TRUE(mac.has_value()) << error;
  const std::vector<Chord> command = {Chord{.modifiers = kModifierCmd, .key = r}};
  EXPECT_EQ(mac->reset, command);
  EXPECT_TRUE(pressed(mac->reset, r, kModifierCmd));
  EXPECT_FALSE(pressed(mac->reset, r, kModifierCtrl)) << "Control and R is not a Mac's reset";

  const std::optional<HotkeyMap> other =
      resolveHotkeys(theDefault(), *keys, KeyboardKind::Other, numberFor, error);
  ASSERT_TRUE(other.has_value()) << error;
  const std::vector<Chord> control = {Chord{.modifiers = kModifierCtrl, .key = r}};
  EXPECT_EQ(other->reset, control);
  EXPECT_TRUE(pressed(other->reset, r, kModifierCtrl));
  EXPECT_FALSE(pressed(other->reset, r, kModifierCmd)) << "nor the Windows key and R anyone else's";
}

TEST(PlayerConfig, CmdOrCtrlKeepsTheModifiersWrittenBesideIt) {
  PadConfig config = parsed("[hotkeys]\nreset = shift+CmdOrCtrl+r, cmd+f9\n");
  fillFrom(config, theDefault());
  std::string error;
  const std::optional<KeyboardMap> keys = resolveKeyboard(config, numberFor, error);
  ASSERT_TRUE(keys.has_value()) << error;
  const std::optional<HotkeyMap> other =
      resolveHotkeys(config, *keys, KeyboardKind::Other, numberFor, error);
  ASSERT_TRUE(other.has_value()) << error;
  const std::vector<Chord> resolved = {
      Chord{.modifiers = static_cast<std::uint8_t>(kModifierShift | kModifierCtrl),
            .key = keyNumbers().at("r")},
      Chord{.modifiers = kModifierCmd, .key = keyNumbers().at("f9")}};  // a plain `cmd` stays Command
  EXPECT_EQ(other->reset, resolved);
}

TEST(PlayerConfig, AChordIsPressedByExactlyItsModifiers) {
  const KeyId r = keyNumbers().at("r");
  const std::vector<Chord> chords = {Chord{.modifiers = kModifierCmd, .key = r},
                                     Chord{.modifiers = kModifierCtrl, .key = r}};
  EXPECT_TRUE(pressed(chords, r, kModifierCmd));
  EXPECT_TRUE(pressed(chords, r, kModifierCtrl));
  EXPECT_FALSE(pressed(chords, r, 0u)) << "the key alone is not the chord";
  EXPECT_FALSE(pressed(chords, r, static_cast<std::uint8_t>(kModifierCmd | kModifierShift)))
      << "nor is the chord with another modifier held";
  EXPECT_FALSE(pressed(chords, keyNumbers().at("z"), kModifierCmd)) << "nor another key";
}

TEST(PlayerConfig, AChordReadsItsModifiersInAnyCaseAndOrder) {
  const PadConfig config = parsed("[hotkeys]\nreset = Shift + CTRL+f9, f10, alt+cmd+r\n");
  const std::vector<ChordName> read = {
      ChordName{.modifiers = static_cast<std::uint8_t>(kModifierShift | kModifierCtrl), .key = "f9"},
      ChordName{.modifiers = 0u, .key = "f10"},
      ChordName{.modifiers = static_cast<std::uint8_t>(kModifierAlt | kModifierCmd), .key = "r"}};
  EXPECT_EQ(config.hotkeys.reset, read);

  // A word that is not a modifier begins the key's name, so a key named with a `+`
  // of its own still reads, and a modifier nobody has is a key nobody has.
  const PadConfig keypad = parsed("[hotkeys]\nreset = ctrl+keypad +\n");
  const std::vector<ChordName> plus = {ChordName{.modifiers = kModifierCtrl, .key = "keypad +"}};
  EXPECT_EQ(keypad.hotkeys.reset, plus);
  EXPECT_EQ(parsed("[hotkeys]\nreset = hyper+r\n").hotkeys.reset[0].key, "hyper+r");
}

TEST(PlayerConfig, AHotkeyAFileDoesNotNameComesFromTheDefault) {
  // A file written before the section had a key carries it empty, and still has a
  // reset button; one that names the key has its own, and as many as it lists.
  PadConfig silent = parsed("[hotkeys]\n");
  EXPECT_TRUE(silent.hotkeys.reset.empty());
  fillFrom(silent, theDefault());
  EXPECT_EQ(silent.hotkeys.reset, theDefault().hotkeys.reset);
  EXPECT_EQ(silent.hotkeys.reset.size(), 1u);

  PadConfig named = parsed("[HotKeys]\nReset = f9\n");
  fillFrom(named, theDefault());
  const std::vector<ChordName> own = {ChordName{.modifiers = 0u, .key = "f9"}};
  EXPECT_EQ(named.hotkeys.reset, own);
}

TEST(PlayerConfig, AHotkeyIsRefusedWhenTheKeyboardDoesNotHaveItOrAButtonAlreadyDoes) {
  PadConfig unknown = parsed("[hotkeys]\nreset = cmd+nonesuch\n");
  fillFrom(unknown, theDefault());
  std::string error;
  const std::optional<KeyboardMap> keys = resolveKeyboard(unknown, numberFor, error);
  ASSERT_TRUE(keys.has_value()) << error;
  EXPECT_FALSE(resolveHotkeys(unknown, *keys, KeyboardKind::Mac, numberFor, error).has_value());
  EXPECT_NE(error.find("nonesuch"), std::string::npos) << error;

  // `z` holds B in the shipped keyboard section; alone, one press would do both.
  // Inside a chord it is a different press, and stands.
  PadConfig shared = parsed("[hotkeys]\nreset = z\n");
  fillFrom(shared, theDefault());
  error.clear();
  EXPECT_FALSE(resolveHotkeys(shared, *keys, KeyboardKind::Mac, numberFor, error).has_value());
  EXPECT_NE(error.find("`z` is the reset hotkey and holds b"), std::string::npos) << error;

  PadConfig chorded = parsed("[hotkeys]\nreset = ctrl+z\n");
  fillFrom(chorded, theDefault());
  EXPECT_TRUE(resolveHotkeys(chorded, *keys, KeyboardKind::Mac, numberFor, error).has_value()) << error;
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

// ---- the display -------------------------------------------------------------------

// The rates the four cases below are written against: the console's own, and the
// panels a run meets. 60.000 is the panel every fixed-refresh display of that family
// reports; 59.94 is the one a television-rate mode reports, as the ratio it is.
constexpr FrameRate kNtsc = consoleFrameRate(consoleClock(Region::Ntsc));
constexpr FrameRate kPal = consoleFrameRate(consoleClock(Region::Pal));
constexpr FrameRate kPanel60 = {.numerator = 60u, .denominator = 1u};
constexpr FrameRate kPanel5994 = {.numerator = 60000u, .denominator = 1001u};
constexpr FrameRate kPanel50 = {.numerator = 50u, .denominator = 1u};
constexpr FrameRate kPanel75 = {.numerator = 75u, .denominator = 1u};
constexpr FrameRate kPanel144 = {.numerator = 144u, .denominator = 1u};

TEST(PlayerDisplay, ADisplayCloseToTheConsoleIsLockedTo) {
  EXPECT_TRUE(locksTo(kNtsc, kPanel60)) << "60.000 against the console's 60.0988";
  EXPECT_TRUE(locksTo(kNtsc, kPanel5994)) << "59.94 against the console's 60.0988";
  EXPECT_TRUE(locksTo(kNtsc, kNtsc)) << "a panel at the console's own rate";
  EXPECT_FALSE(locksTo(kNtsc, kPanel50));
  EXPECT_FALSE(locksTo(kNtsc, kPanel75));
  EXPECT_FALSE(locksTo(kNtsc, kPanel144));
  // A 50 Hz machine takes the other half of the same line: the panel it lines up with
  // is the one a 60 Hz machine does not.
  EXPECT_TRUE(locksTo(kPal, kPanel50));
  EXPECT_FALSE(locksTo(kPal, kPanel60));
  // A panel nobody could read a rate from is never locked to, whichever way round.
  EXPECT_FALSE(locksTo(kNtsc, FrameRate{}));
}

TEST(PlayerDisplay, TheLockedRateIsTheDisplaysOwn) {
  const Pacing pacing = paceRun(kNtsc, kPanel60, Vsync::Auto);
  EXPECT_TRUE(pacing.locked);
  EXPECT_EQ(pacing.rate.numerator, kPanel60.numerator);
  EXPECT_EQ(pacing.rate.denominator, kPanel60.denominator);
  // A second of a 60.000 Hz panel is sixty refreshes, so the first frame of a run held
  // to it is owed a sixtieth of a second in and not the console's own interval.
  EXPECT_EQ(FramePace(pacing.rate).owedAt(1u), 16666666ull);
  EXPECT_EQ(FrameDeadline(Region::Ntsc).owedAt(1u), 16639263ull) << "the console's own";
  // And it stays the panel's rate over a run: an hour of it is 216,000 refreshes.
  EXPECT_EQ(FramePace(pacing.rate).owedAt(216000u), 3600000000000ull) << "an hour, exactly";
}

TEST(PlayerDisplay, TheSampleRateFollowsTheLockedRate) {
  // 32000 x 60.000 / 60.0988: the machine makes 32,000 samples a second of its own
  // time, and a second of its own time now takes 60.0988/60.000 seconds of the
  // panel's, so the device is told the stream arrives this much slower.
  const Pacing pacing = paceRun(kNtsc, kPanel60, Vsync::Auto);
  EXPECT_EQ(pacedSampleRate(32000, kNtsc, pacing.rate), 31947);
  // The other direction is the same arithmetic: a panel faster than the console.
  EXPECT_EQ(pacedSampleRate(32000, kNtsc, FrameRate{.numerator = 61u, .denominator = 1u}), 32480);
  // A run held to the console's own rate is handed the rate the machine makes.
  EXPECT_EQ(pacedSampleRate(32000, kNtsc, kNtsc), 32000)
      << "a run held to the console's own rate takes the rate the machine makes";
  EXPECT_EQ(pacedSampleRate(32000, kPal, kPal), 32000);
}

TEST(PlayerDisplay, AnUnlockableDisplayKeepsTheConsolesRate) {
  const Pacing pacing = paceRun(kPal, kPanel60, Vsync::Auto);
  EXPECT_FALSE(pacing.locked);
  EXPECT_EQ(pacing.rate.numerator, kPal.numerator);
  EXPECT_EQ(pacing.rate.denominator, kPal.denominator);
  EXPECT_EQ(FramePace(pacing.rate).owedAt(1u), FrameDeadline(Region::Pal).owedAt(1u));
  EXPECT_EQ(pacedSampleRate(32000, kPal, pacing.rate), 32000) << "and the sound is unchanged";
  // A panel that could not be read leaves the run on the console's rate as well.
  EXPECT_FALSE(paceRun(kNtsc, FrameRate{}, Vsync::Auto).locked);
  EXPECT_FALSE(paceRun(kNtsc, FrameRate{}, Vsync::On).locked);
  // The flag names the arrangement rather than asking for one: a panel the band would
  // refuse is held to when a run asks for it, and one the band would take is not when
  // a run asks it not to be.
  EXPECT_TRUE(paceRun(kNtsc, kPanel144, Vsync::On).locked);
  EXPECT_EQ(paceRun(kNtsc, kPanel144, Vsync::On).rate.numerator, kPanel144.numerator);
  EXPECT_FALSE(paceRun(kNtsc, kPanel60, Vsync::Off).locked);
  EXPECT_EQ(paceRun(kNtsc, kPanel60, Vsync::Off).rate.numerator, kNtsc.numerator);
}

}  // namespace snaggletooth::player
