# Input script

An input script is a text file that says which buttons are held on which
controller port from which frame. The [cartridge disassembler](snes-disassembler.md)
replays one into the machine while it [runs the cartridge](snes-disassembler.md#running-the-cartridge),
so the run exercises what a player would — a title screen left behind, a menu
entered, a level begun — and the trace reaches the code those things run.

> **Status.** `snes_disasm --input <script>` replays a script through both of the
> machine's controller paths, the auto-read and the serial ports. Scripts are
> written by hand or exported from a recording; nothing here records one.

---

## Contents

- [1. Form](#1-form)
- [2. Frames](#2-frames)
- [3. Ports](#3-ports)
- [4. Buttons](#4-buttons)
- [5. Refusals](#5-refusals)
- [6. Library](#6-library)
- [7. Stability](#7-stability)
- [See also](#see-also)

## 1. Form

A script is UTF-8 text, one line per change:

```
frame <n> <port> <buttons>    ; comment
```

`frame` is the keyword every line begins with. `<n>` is the frame the line takes
effect on, `<port>` is `1` or `2`, and `<buttons>` is the set held from that
frame on — one or more button names, or `none`. Fields are separated by spaces or
tabs. A semicolon begins a comment that runs to the end of the line, and a blank
line is nothing.

A script that leaves a title screen with Start, walks a menu, and runs right
through a level with a jump every so often:

```
; through the title and one menu
frame 300 1 start
frame 310 1 none
frame 480 1 start
frame 490 1 none

; into the level
frame 720 1 a
frame 730 1 none
frame 900 1 right
frame 1080 1 right b
frame 1090 1 right
```

A line replaces what the port held: `frame 1080 1 right b` holds Right and B;
`frame 1090 1 right` releases B and keeps Right. Nothing is held between lines
that the last line did not name.

## 2. Frames

Frames are counted from power-on, the first being frame 0, and a frame begins as
the beam wraps to the top of the picture. A line's buttons are presented to the
machine as its frame begins, ahead of the vertical blank in which the console's
auto-read latches the pads — so a button named on frame `n` is what the program
reads on frame `n`, through either path.

Lines run in frame order. One frame may carry a line for each port.

## 3. Ports

A port the script names anywhere has a controller plugged in from power-on, with
nothing pressed until its first line. A port the script never names has no
controller, and a program reading it sees exactly what the machine reads with an
empty port: every bit zero. The distinction is the console's — a standard pad
answers a read past its sixteenth bit with ones, and an empty port with zeros —
so a program that checks for a controller sees one on a named port and none on an
unnamed one.

## 4. Buttons

The twelve buttons of a standard controller, in the order the pad shifts them
out:

```
b  y  select  start  up  down  left  right  a  x  l  r
```

Names are read in any case. `none` stands alone and means a pad with nothing
pressed. The identity bits a pad shifts out after its buttons are the standard
pad's, and are not scriptable.

## 5. Refusals

A script is read whole before the run starts, and a line that cannot be read
refuses the whole script, naming the line:

- a line that does not begin with `frame`, or lacks a frame, a port or a button word;
- a frame that is not a number, or is lower than the line before it;
- a port other than `1` or `2`, or a port given twice for one frame;
- a word that is not a button, a button named twice, or `none` beside a button.

```
$ snes_disasm cartridge.sfc -o cartridge --input play.txt
play.txt: line 4: `fire` is not a button; the buttons are b y select start up down left right a x l r, or none
```

## 6. Library

```cpp
#include "rom/input_script.h"

std::string error;
const std::optional<snaggletooth::disasm::InputScript> script =
    snaggletooth::disasm::parseInputScript(text, error);
if (!script) std::cerr << error << "\n";

request.input = *script;  // a CartridgeRequest; observeRun replays it
```

`parseInputScript` returns the `InputScript` — its `events` in frame order, each
an `InputEvent` of `frame`, `port` and the `Joypad` held — or nothing, with
`error` naming the line. `InputScript::padAt(port, frame)` is what a port holds
at a frame, and `names(port)` whether the port has a pad at all.
`observeRun(rom, masterCycles, script, notes)` in `rom/rom_observe.h` is the
replay; `CartridgeRequest::input` carries a script into `disassembleCartridge`.
The `Joypad` value and the ports are the [machine's](snes-machine.md#the-controller-ports).

The library target is `snaggletooth_rom`.

## 7. Stability

This document defines a published surface, held to the same rule as the
[assembly language](assembly-lexicon.md#8-stability): once a release replays a
script, a later release replays it to the same effect. New line kinds and new
words may be added; the meaning of a line this page describes does not change,
and nothing described here is removed.

## See also

- [Cartridge disassembler §Running the cartridge](snes-disassembler.md#running-the-cartridge)
  — the run a script is replayed into, and what it yields.
- [The SNES machine §The controller ports](snes-machine.md#the-controller-ports)
  — how the machine presents a pad to the program.
- [Project manifest](project-manifest.md) — the `reached` lines a run writes.
