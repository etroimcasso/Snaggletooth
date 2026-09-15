#pragma once

// Turning what a person is holding into what the machine's controller ports hold.
//
// The controller itself is the machine's — `Button`, `Joypad` and `JoypadPort` are
// the console's own, declared in `snaggletooth/snes/snes.h`, and an application
// embedding the core speaks them without any of this. What is here is the other
// side: the modern hardware a person actually has in their hands, and the mapping
// from one to the other. It answers with the `Joypad` value `Snes::setJoypad` takes,
// so this layer composes or it disappears.
//
// Nothing here opens a device, reads an event or touches a file. An application that
// owns a keyboard and some gamepads reports their state; these functions do the rest.
// An application that already knows which pad is player one and what its buttons
// mean links none of it and loses nothing.
//
// A press on a pad is named by WHERE IT SITS rather than by the letter printed on
// it, because the letter moves between families and the position does not: the
// button under the thumb's south position is the SNES's B on every pad ever made,
// whatever its face says. The one family addressed by its letters is the GameCube,
// whose four letters are the SNES's four letters — and it is addressed that way by
// naming the letters a pad prints, which the application reports, so no table here
// can drift from a pad.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "rom/input_script.h"
#include "snaggletooth/snes/snes.h"

namespace snaggletooth::player {

// Where a press sits on a modern pad. The four face positions are compass points
// because that is what they are — the letters printed over them differ by family and
// are handled separately, below.
enum class PadButton : std::uint8_t {
  South, East, West, North,
  DpadUp, DpadDown, DpadLeft, DpadRight,
  LeftShoulder, RightShoulder,
  Start, Back, Guide,
  LeftStickClick, RightStickClick,
};
inline constexpr std::size_t kPadButtonCount = 15;

// A pad's two sticks and two triggers. A stick runs from -1 to 1 with its Y positive
// DOWNWARD, which is how every pad reports one; a trigger runs from 0 to 1.
enum class PadAxis : std::uint8_t {
  LeftX, LeftY, RightX, RightY, LeftTrigger, RightTrigger,
};
inline constexpr std::size_t kPadAxisCount = 6;

// How far a stick leaves its centre before it counts as pointing somewhere. A stick
// is an analog control answering a question with two answers, so the line is drawn
// once here rather than per button.
inline constexpr float kStickThreshold = 0.5f;

// The letter printed on a face button. `None` is a pad that prints nothing there,
// which is an ordinary answer rather than an error.
enum class FaceLabel : std::uint8_t { None, A, B, X, Y };

// The four letters a pad prints at its four face positions, in the order south,
// east, west, north. The application that owns the pad reports them — no table here
// decides what a pad prints, so nothing here can be wrong about one.
struct FaceLabels {
  std::array<FaceLabel, 4> at{FaceLabel::None, FaceLabel::None, FaceLabel::None, FaceLabel::None};

  // Which position carries a letter, or nothing where the pad prints it nowhere.
  [[nodiscard]] std::optional<PadButton> position(FaceLabel label) const noexcept;

  [[nodiscard]] bool operator==(const FaceLabels&) const noexcept = default;
};

// The three face layouts a pad has, as values to write a configuration against and to
// hand to these functions in a test. A running tool reads the letters off the pad in
// front of it instead.
inline constexpr FaceLabels kAbxy{{FaceLabel::A, FaceLabel::B, FaceLabel::X, FaceLabel::Y}};
inline constexpr FaceLabels kAxby{{FaceLabel::A, FaceLabel::X, FaceLabel::B, FaceLabel::Y}};
inline constexpr FaceLabels kBayx{{FaceLabel::B, FaceLabel::A, FaceLabel::Y, FaceLabel::X}};
inline constexpr FaceLabels kNoLabels{};

// One pad's state for one frame, as the application that owns the device reports it.
struct PadInput {
  std::array<bool, kPadButtonCount> pressed{};
  std::array<float, kPadAxisCount> axis{};

  void press(PadButton button, bool down) noexcept;
  void move(PadAxis which, float position) noexcept;

  [[nodiscard]] bool down(PadButton button) const noexcept;
  [[nodiscard]] float at(PadAxis which) const noexcept;
};

// A key, as the application that owns the keyboard numbers them. Nothing here reads
// the number: a key is compared to the keys a configuration named and to nothing
// else, so whatever numbering the application already has is the right one.
using KeyId = int;

// Where a press on a pad comes from.
struct PadSource {
  enum class Kind : std::uint8_t {
    Button,     // a position on the pad
    AxisHigh,   // an axis past the threshold in its positive direction
    AxisLow,    // an axis past the threshold in its negative direction
    Label,      // the button a pad prints this letter on, whichever position that is
  };

  Kind kind = Kind::Button;
  PadButton button = PadButton::South;
  PadAxis axis = PadAxis::LeftX;
  FaceLabel label = FaceLabel::None;

  [[nodiscard]] bool operator==(const PadSource&) const noexcept = default;
};

// What a pad source is called in a configuration file — `south`, `left stick up`,
// `right trigger` — and the source a phrase stands for, or nothing for a phrase that
// names none. Read in any case, with runs of spaces read as one.
[[nodiscard]] std::string padSourceName(const PadSource& source);
[[nodiscard]] std::optional<PadSource> padSourceFromName(std::string_view name);

// Which port a controller is put on. `Auto` takes the lowest port free when the
// controller arrives.
enum class PortChoice : std::uint8_t { Auto, One, Two };

// The pad families a configuration can give a section of its own. The family decides
// which section applies and what the tool reports when a pad is opened; it decides
// nothing about which button is which, because the positions answer that and the
// printed letters answer the rest.
enum class PadFamily : std::uint8_t { Nintendo, Sony, Xbox, GameCube, Standard, Unknown };

// Every family, and what a family is called in a section header and in a report.
[[nodiscard]] std::span<const PadFamily> padFamilies() noexcept;
[[nodiscard]] std::string_view padFamilyName(PadFamily family) noexcept;
[[nodiscard]] std::optional<PadFamily> padFamilyFromName(std::string_view name);

// A pad's mapping, resolved for one pad: what each of the console's twelve buttons
// is held by, and where the pad goes.
struct GamepadMap {
  std::array<std::vector<PadSource>, kButtonCount> sources{};
  PortChoice port = PortChoice::Auto;

  // Whether the configuration named a letter this pad does not print, so nothing is
  // bound to it. The tool says so once when the pad is opened rather than leaving a
  // button quietly dead.
  bool unprintedLabel = false;

  [[nodiscard]] std::span<const PadSource> sourcesFor(Button button) const noexcept;
};

// The keyboard's mapping: what each of the twelve buttons is held by, and which port
// the keyboard is on.
struct KeyboardMap {
  std::array<std::vector<KeyId>, kButtonCount> keys{};
  JoypadPort port = JoypadPort::One;

  [[nodiscard]] std::span<const KeyId> keysFor(Button button) const noexcept;
};

// The twelve buttons a pad in this state holds under this mapping. A button with more
// than one source is held when any of them is.
[[nodiscard]] Joypad padFromGamepad(const PadInput& input, const GamepadMap& map);

// The twelve buttons the keys held down hold under this mapping.
[[nodiscard]] Joypad padFromKeyboard(std::span<const KeyId> down, const KeyboardMap& map);

// Every button either of them holds. A person with a pad in hand and a hand on the
// keyboard is one player, so the two sources on a port are added rather than one
// replacing the other.
[[nodiscard]] Joypad either(const Joypad& a, const Joypad& b) noexcept;

// Which port each controller takes as controllers arrive and leave. The first to
// arrive takes the lowest port free, a controller that pins a port takes that one,
// and a controller with nowhere to go is reported and left idle. Pulling one out
// frees its port for the next to arrive.
//
// It is a value with no device in it: an application drives it from whatever its own
// device layer tells it, and one that assigns ports its own way ignores it.
class PortAssignment {
 public:
  // Puts a controller in. Answers the port it took, or nothing when both are full.
  // A controller already in keeps the port it has.
  [[nodiscard]] std::optional<JoypadPort> plug(int controller, PortChoice wanted);

  // Takes a controller out, freeing whatever port it held. A controller that is not
  // in does nothing.
  void unplug(int controller);

  // Which controller is on a port, or nothing where the port is empty.
  [[nodiscard]] std::optional<int> on(JoypadPort port) const noexcept;

 private:
  std::array<std::optional<int>, 2> ports_{};
};

// The run as a script, so what a person did once is replayed exactly afterwards.
//
// It is told what each port holds every frame and keeps a line only where that
// changed from the frame before — a button, a controller arriving, or a controller
// leaving. Both ports are written at frame 0 whatever they hold, so a replay's ports
// are the live run's ports from power-on and a program that looks for a controller
// before one arrived sees on replay what it saw live.
class PadRecorder {
 public:
  void frame(std::uint32_t at, const std::optional<Joypad>& one, const std::optional<Joypad>& two);

  [[nodiscard]] const disasm::InputScript& script() const noexcept { return script_; }

 private:
  void port(std::uint32_t at, JoypadPort which, const std::optional<Joypad>& held);

  disasm::InputScript script_;
  std::array<std::optional<Joypad>, 2> last_{};
  bool started_ = false;
};

}  // namespace snaggletooth::player
