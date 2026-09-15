# The player

`snes_player` runs a cartridge in a window, with its sound and your controller, and records the same
run when asked to.

```
snes_player <image> [--out <directory>] [--seconds N] [--scale N]
                    [--input <script> | --input-dir <directory>] [--config <file>]
                    [--region ntsc|pal] [--mute] [--quiet]
snes_player --default-config
```

The window shows the picture the machine draws, frame by frame, at the console's own rate, with
that rate in its title so what the run costs is visible while it runs. It closes when the window
is closed or when `--seconds` of the master clock have been spent; nothing else stops it.

The machine runs at the rate the cartridge's own country byte asks for, so a 50 Hz cartridge boots
rather than refusing, and is paced at 50 Hz. `--region` sets it by hand for an image that does not
say, or one patched to run at the other rate while still declaring the first.

The sound goes to the default playback device as the machine makes it, at the DSP's own 32 kHz
stereo, so nothing resamples what the chip produced. `--mute` opens no device at all, and a machine
that has none runs silent and says why. Where the queued sound runs more than a quarter of a second
ahead of the speakers a chunk is left out rather than added to the delay: the machine is paced to
the console's frame interval and the device consumes at its own crystal, so the two drift apart
over a long run.

## Playing it

The keyboard and every gamepad plugged in drive the ports. Each pad is taken as it arrives, given
the lowest port free, and named on the way in — the family it belongs to and what the system calls
it — so you can see which controller went where. Pulling one out empties its port at once, and the
next pad plugged in takes the port that came free. A third pad is reported and left idle.

A pad is read by **where its buttons sit** rather than by the letters printed on them, so every
family plays the same way round: the button under your thumb at the south position is B, east is A,
west is Y, north is X. The keyboard's default puts the four face buttons at the same positions —
`Z` is B, `X` is A, `A` is Y, `S` is X, with `Q` and `W` for L and R, `Return` for Start,
`Right Shift` for Select, and the arrows for the d-pad.

The keyboard and a pad on one port add together, so a hand on each is one player.

`--config <file>` hands the tool a mapping of your own, and `--default-config` prints the one it
ships with so you have something to copy. The whole form is
[pad-config.md](../../docs/pad-config.md).

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
| `<image>.avi` | every frame exactly as the machine drove it, uncompressed (`../video/README.md`) |
| `<image>.csv` | a row a frame: wall and emulation time in nanoseconds, master cycles, dots drawn, and the rate instantaneous and mean |
| `<image>.wav` | the sound the run produced, 32 kHz stereo |
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
opens no device: it turns a controller's reported state into the `Joypad` the machine takes. Every
call that touches a real device is in this tool.

This is the only target that links SDL. It is built when `SNAGGLETOOTH_BUILD_PLAYER` and
`SNAGGLETOOTH_BUILD_TOOLS` are both on — the default for a top-level build — and takes SDL from a
parent build that already defines `SDL3::SDL3`, or from the pinned submodule at `third_party/sdl`,
built statically with the subsystems a window, a speaker and a gamepad do not need switched off. The
library itself never links SDL.
