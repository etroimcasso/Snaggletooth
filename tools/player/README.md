# The player

`snes_player` runs a cartridge in a window, with its sound and your controller, and records the same
run when asked to.

```
snes_player <image> [--out <directory>] [--seconds N] [--scale N]
                    [--input <script> | --input-dir <directory>] [--config <file>]
                    [--region ntsc|pal] [--vsync on|off|auto] [--mute] [--quiet]
snes_player --default-config
snes_player --user-files
```

The window shows the picture the machine draws, frame by frame, at the rate the run is held to —
a frame drawn in half-pixels, 512 wide, fills the same window as a 256-wide one, each half-pixel half
a scaled pixel, and an interlaced run shows each field as it comes,
with the rate it achieves in its title so what the run costs is visible while it runs. It closes
when the window is closed or when `--seconds` of the master clock have been spent; nothing else
stops it.

The machine runs at the rate the cartridge's own country byte asks for, so a 50 Hz cartridge boots
rather than refusing, and is paced at 50 Hz. `--region` sets it by hand for an image that does not
say, or one patched to run at the other rate while still declaring the first.

## The rate it runs at

A television took whatever the console sent it. A fixed-refresh panel does not, and the two rates
are not the same number: the 60 Hz console draws 60.0988 frames a second and a 60 Hz panel refreshes
60.000 times, so a run paced to the console alone puts a frame on the wrong side of a refresh about
once every ten seconds — shown twice, or torn — for as long as it runs, with nothing to pull the
phase back once something has moved it.

So the run is held to the panel instead, wherever the panel is within one part in a hundred of the
console: the window waits for a refresh and every refresh carries one new frame. The first thing a
run says is which of the two rates it took —

```
held to the display at 60.000 Hz
the console's own 60.099 Hz
```

**A held run is a sixth of a per cent slow by the wall clock, and exact in every other sense.** Every
cycle is emulated as it always was and a recording of a held run is byte-identical to a recording of
an unheld one; what changes is the rate at which frames are asked for. A panel at another rate
entirely — 50 Hz, 144 Hz — is not one a run can be held to, so the run keeps the console's own rate
there and the beat comes back with it.

`--vsync` names the arrangement outright: `on` holds the run to whatever the panel reports, `off`
keeps the console's rate and hands every present straight back, and `auto`, the default, decides by
the rule above.

The sound goes to the default playback device as the machine makes it, 32 kHz stereo. The device is
told the rate the run *delivers* at rather than the rate the chip makes: a held run makes its
samples that same sixth of a per cent slower, and a device consuming 32,000 a second would run dry
every few minutes. A run on the console's own rate is handed the chip's own 32 kHz and nothing
resamples it. `--mute` opens no device at all, and a machine that has none runs silent and says why.
Where the queued sound runs more than a quarter of a second ahead of the speakers a chunk is left
out rather than added to the delay: the device consumes at its own crystal, so the two still drift
apart slowly over a long run.

## Playing it

The keyboard and every gamepad plugged in drive the ports. Each pad is taken as it arrives, given
the lowest port free, and named on the way in — the family it belongs to and what the system calls
it — so you can see which controller went where. Pulling one out empties its port at once, and the
next pad plugged in takes the port that came free. A third pad is reported and left idle.

A pad is read by **where its buttons sit** rather than by the letters printed on them, so every
family plays the same way round: the button under your thumb at the south position is B, east is A,
west is Y, north is X. The keyboard's default puts the four face buttons at the same positions —
`Z` is B, `X` is A, `A` is Y, `S` is X, with `Q` and `W` for L and R, `Return` for Start,
`Backspace` for Select, and the arrows for the d-pad.

The keyboard and a pad on one port add together, so a hand on each is one player.

Command and `R` on a Mac, Control and `R` everywhere else, is the console's reset button. The cartridge starts again from its reset vector with work RAM
and its save as they were, the way it does on the console. A run replayed from a script or recorded
to one leaves the button alone, since a script has no word for it.

`--config <file>` hands the tool a mapping of your own, and `--default-config` prints the one it
ships with so you have something to copy. The whole form is
[pad-config.md](../../docs/pad-config.md).

With no `--config`, a mapping kept at `config/input/default.snagpad` where your files go is what the
run takes, and the built-in one when there is none. The tool says which it took. `--user-files`
prints that directory and exits — see [user-files.md](../../docs/user-files.md).

## Keeping a save

A cartridge whose header declares a battery keeps what it writes, in `sram/<cartridge>.srm` beside
your configuration: the file [bsnes, snes9x and Mesen](../../docs/user-files.md#a-cartridges-save)
read and write for the same cartridge. It is read in before the cartridge's first instruction and
written as the machine reports the save changed, with whatever the last moments left written as the
run closes. A file whose size is not the one the cartridge declares is left exactly as it is and that
run keeps nothing, so a save belonging to something else is never overwritten; the tool says so when
it starts. A disk that refuses is reported once and the run carries on.

The window asks to be raised when it opens. A window opened from a terminal does not take the
keyboard on every platform, and a run you cannot press a button on is not a run — if the keys are
going to the terminal instead, click the window once.

A run driven by a recorded script leaves the keyboard and the pads alone: `--input` names a script
and `--input-dir` a directory of them, from which the one named for the image is replayed, or the
directory's `default.snaginput` when the image has none — the same
[input scripts](../../docs/input-script.md) the cartridge disassembler replays. Two runs of one
cartridge are then the same run, which is what makes two recordings worth setting beside each other,
and the tool says so at the start rather than leaving you pressing buttons at a replay.

## Recording it

`--out` writes what the run produced into a directory, named after the image. Without it the run
keeps none of it — the sound goes to the speakers and is let go rather than held for a file nobody
asked for:

| File | What it holds |
|---|---|
| `<image>.avi` | every frame exactly as the machine drove it, uncompressed, at the largest shape the run produced (`../video/README.md`) |
| `<image>.csv` | a row a frame: wall and emulation time in nanoseconds, master cycles, dots drawn, and the rate instantaneous and mean |
| `<image>.wav` | the sound the run produced, 32 kHz stereo — the chip's own rate, whatever the panel showing it runs at |
| `<image>.snaginput` | the buttons, as a script that replays the run |

They come together — one run, one set of evidence. The script is written whatever drove the run, so
a run you played by hand replays exactly, and one driven by a script is written back in the same
canonical form.

## What it is built on

The library draws only while a frame observer is set (`snaggletooth/snes/video_frame.h`), so the
tool sets one, and each finished frame goes to the window, the recording and the table in turn
before the next frame is allowed to begin. The controllers are read after that frame's wait rather
than before it, so what you are holding is sampled immediately ahead of the frame that latches it.
The sound is taken from the machine between frames, a quarter of a frame at a time, and handed
straight to the device.

The mapping is `snaggletooth_player_pads`, which holds no reference to any windowing library and
opens no device: it turns a controller's reported state into the `Joypad` the machine takes. The
rate arithmetic in `display.h` is the same kind of thing — it is handed what a panel reports and
answers with the rate to pace by, when each frame of that pacing is owed, and the rate to open a
device at, so all of it is checked by the suite. Every call that touches a real device is in this
tool.

This is the only target that links SDL. It is built when `SNAGGLETOOTH_BUILD_PLAYER` and
`SNAGGLETOOTH_BUILD_TOOLS` are both on — the default for a top-level build — and takes SDL from a
parent build that already defines `SDL3::SDL3`, or from the pinned submodule at `third_party/sdl`,
built statically with the subsystems a window, a speaker and a gamepad do not need switched off. The
library itself never links SDL.

**Run a `Release` build.** A `Debug` build of the machine is several times slower, and the player
built that way falls well short of the console's rate, so the rate in the window's title reads
as a slow emulator when it is only the build. A Debug player says so before anything else when
it starts. These commands build the player as `Release` on every platform:

```
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --target snes_player
```

The player is then `build/snes_player`, or `build\Release\snes_player.exe` on Windows. On
Windows, `--config Release` is the flag that matters: without it the build is `Debug`, and the
player lands in `build\Debug\`.
[docs/build-and-consume.md](../../docs/build-and-consume.md#building-a-release-build) says which
flag each generator reads.
