# Pad configuration

A pad configuration is a text file that says which keys and which places on a
controller hold each of the machine's twelve buttons, and which port a controller
goes to. [`snes_player`](../tools/player/README.md) ships with one and uses it
unless you hand it another.

> **Status.** `snes_player` reads the keyboard and every gamepad plugged in, and
> maps both through this file. The file's extension is `.snagpad`.

---

## Contents

- [1. Form](#1-form)
- [2. The keyboard](#2-the-keyboard)
- [3. The gamepad](#3-the-gamepad)
- [4. A family of its own](#4-a-family-of-its-own)
- [5. Ports](#5-ports)
- [6. Hotkeys](#6-hotkeys)
- [7. Refusals](#7-refusals)
- [8. Library](#8-library)
- [9. Stability](#9-stability)
- [See also](#see-also)

## 1. Form

A configuration is UTF-8 text in a file whose extension is `.snagpad`, in the
shape of a Unix configuration file:

```
# a comment, to the end of the line

[section]
key = value
key = value, second value
```

A `#` begins a comment, a blank line is nothing, and section headers and keys are
read in any case. A value is one or more entries separated by commas, and a button
with more than one is held when any of them is.

Start from the one the tool ships with:

```
$ snes_player --default-config > mine.snagpad
$ snes_player cartridge.sfc --config mine.snagpad
```

`--default-config` prints the built-in configuration and exits, opening no window.
What it prints is the file the tool was built from, so what you copy is what runs.

The twelve buttons are named as the machine names them, which is also how a
[recorded run](input-script.md) names them:

```
b  y  select  start  up  down  left  right  a  x  l  r
```

## 2. The keyboard

```
[keyboard]
port = 1
b = z
a = x
y = a
x = s
l = q
r = w
start = return
select = right shift
up = up
down = down
left = left
right = right
```

Each button names one or more keys. **Keys are named as the system names them** —
`Z`, `Return`, `Right Shift`, `Up` — because a key's name belongs to the keyboard
rather than to this tool, and one table of two hundred names is enough. A name the
keyboard does not have refuses the run and says which one.

The default puts the four face buttons where they sit on a pad rather than where
their letters fall on the keyboard: `Z` is south and so holds B, `X` is east and
holds A, `A` is west and holds Y, `S` is north and holds X.

The keyboard is always in play, so a person with a pad in one hand and a hand on
the keyboard is one player: both feed the port, and a button either of them holds
is held.

## 3. The gamepad

```
[gamepad]
port = auto
b = south
a = east
y = west
x = north
l = left shoulder
r = right shoulder
start = start
select = back
up = dpad up, left stick up
down = dpad down, left stick down
left = dpad left, left stick left
right = dpad right, left stick right
```

**A pad is read by where its buttons sit, not by what is printed on them.** The
letters move between families and the positions do not: the button under your
thumb at the south position is the machine's B on every pad, whichever letter that
pad prints there. One configuration therefore plays the same way round on every
controller, and no pad needs a section of its own to be usable.

The places a button may name:

| | |
|---|---|
| face positions | `south` `east` `west` `north` |
| d-pad | `dpad up` `dpad down` `dpad left` `dpad right` |
| shoulders | `left shoulder` `right shoulder` |
| triggers | `left trigger` `right trigger` |
| sticks | `left stick up` `left stick down` `left stick left` `left stick right`, and the same four for `right stick` |
| stick buttons | `left stick click` `right stick click` |
| the rest | `start` `back` `guide` |
| printed letters | `a` `b` `x` `y` — see below |

A stick counts as pointing somewhere once it is past half of its range, and a
trigger counts as held past half of its travel. Runs of spaces in a phrase read as
one, so `left  stick   up` is `left stick up`.

The four **printed letters** are the exception to reading a pad by position: `a`
means the button this pad prints an A on, wherever that is. They exist for the one
family whose letters are the machine's own, and resolve per pad — a pad that
prints something else on its face, a cross or a circle, prints no such letter, and
a button bound to one holds nothing. The tool says so once when the pad is opened
rather than leaving a button quietly dead.

## 4. A family of its own

A `[gamepad <family>]` section overrides the plain one **key by key** for that
family, and leaves every key it does not name. The families are `nintendo`,
`sony`, `xbox`, `gamecube`, `standard` and `unknown`.

The shipped configuration has exactly one, because exactly one pad needs it:

```
# The GameCube pad prints the same four letters the machine does, in the same
# places, so it is the one family read by its letters instead of its positions.
[gamepad gamecube]
a = a
b = b
x = x
y = y
l = left trigger
r = right trigger
```

A GameCube pad prints A at south, X at east, B at west and Y at north — so read by
position its face would be wrong, and read by its letters it is exactly a
controller for this machine. Its L and R are its analog triggers. Start, Select
and the d-pad are not named here, so they come from the section above.

That is also how you give one family a port of its own without repeating a whole
mapping:

```
[gamepad sony]
port = 2
```

## 5. Ports

`port` is `1`, `2` or `auto`. Under `[keyboard]` it is `1` or `2` — the keyboard is
on a port of its own and is never `auto`.

A pad set to `auto` takes the lowest port free when it is plugged in. The first pad
to arrive takes port 1, the next takes port 2, a third is reported and left idle,
and pulling one out frees its port for the next. A pad whose family pins a port
takes that port, even if it is the first to arrive.

A port with no controller on it is **empty**, which a program can tell from a
controller with nothing pressed — the machine answers an empty socket with zeros
and a pad with its own idle word.

## 6. Hotkeys

```
[hotkeys]
```

The section where later functions bind. **This release defines no key in it.** The
empty section is accepted so the shipped file can show where they go; a key in it
is refused rather than quietly ignored.

## 7. Refusals

A configuration is read whole before the window opens, and a line that cannot be
read refuses it, naming the line:

- a section that is not one of the above, or `[keyboard]` or `[gamepad]` twice;
- a key that is not one of the twelve buttons and is not `port`, or one given twice;
- a button given nothing to be held by;
- a phrase that names no place on a pad;
- a `port` that is not `auto`, `1` or `2` — or `auto` under `[keyboard]`;
- two sections pinned to the same port;
- any key under `[hotkeys]`;
- a key name the keyboard does not have.

A present `[keyboard]` or `[gamepad]` section says what holds **every one of the
twelve**. Those two sections are the whole mapping rather than a patch, so nothing
hidden fills a gap — a section that leaves a button unnamed is refused, naming it.
A `[gamepad <family>]` section is the patch, and says as little as it likes.

```
$ snes_player cartridge.sfc --config mine.snagpad
mine.snagpad: line 4: `elbow` is not a place on a pad
```

A file that leaves out `[keyboard]` or `[gamepad]` entirely takes that section from
the shipped default, so a file that changes one thing can be four lines long. That
is the one place the default reaches into a configuration of your own.

## 8. Library

```cpp
#include "player/pad_config.h"

std::string error;
const std::optional<snaggletooth::player::PadConfig> config =
    snaggletooth::player::parsePadConfig(text, error);
if (!config) std::cerr << error << "\n";

const snaggletooth::player::GamepadMap map =
    snaggletooth::player::resolve(*config, family, labels);
const snaggletooth::Joypad held = snaggletooth::player::padFromGamepad(input, map);
machine.setJoypad(snaggletooth::JoypadPort::One, held);
```

`resolve` takes the pad's family and the four letters it prints, and answers the
mapping for that pad. `padFromGamepad` turns a `PadInput` — the pad's state as the
application that owns the device reports it — into the `Joypad` the machine takes.
`padFromKeyboard` does the same for the keys held down, and `either` adds two
together for a port they share.

Nothing in the library opens a device, reads an event or touches a file: it holds
no reference to any windowing or input library, and an application supplies the
state. The machine's own `Button`, `Joypad` and `JoypadPort` are the
[machine's](snes-machine.md#the-controller-ports).

The library target is `snaggletooth_player_pads`.

## 9. Stability

This document defines a published surface, held to the same rule as the
[input script](input-script.md#7-stability): once a release reads a
configuration, a later release reads it to the same effect. New sections, new keys
and new phrases may be added; the meaning of a line this page describes does not
change, and nothing described here is removed.

## See also

- [Input script](input-script.md) — a recorded run, which the player writes and
  replays, and which names the same twelve buttons.
- [tools/player/README.md](../tools/player/README.md) — the player itself: what it
  shows, what it records, and what it takes on the command line.
- [The SNES machine §The controller ports](snes-machine.md#the-controller-ports)
  — how the machine presents a controller to the program.
