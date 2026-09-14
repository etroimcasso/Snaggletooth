# The player

`snes_player` runs a cartridge in a window and records the same run.

```
snes_player <image> [--out <directory>] [--seconds N] [--scale N]
                    [--input <script> | --input-dir <directory>] [--quiet]
```

The window shows the picture the machine draws, frame by frame, at the console's own rate, with
that rate in its title so what the run costs is visible while it runs. It closes when the window
is closed or when `--seconds` of the master clock have been spent; nothing else stops it.

The buttons come from a recorded run rather than the keyboard: `--input` names a script and
`--input-dir` a directory of them, from which the one named for the image is replayed, or the
directory's `default.snaginput` when the image has none — the same [input scripts](../../docs/input-script.md)
the cartridge disassembler replays. Two runs of one cartridge are then the same run, which is what
makes two recordings worth setting beside each other.

`--out` writes what the run produced into a directory, named after the image:

| File | What it holds |
|---|---|
| `<image>.avi` | every frame exactly as the machine drove it, uncompressed (`../video/README.md`) |
| `<image>.csv` | a row a frame: wall and emulation time in nanoseconds, master cycles, dots drawn, and the rate instantaneous and mean |
| `<image>.wav` | the sound the run produced, 32 kHz stereo |

They come together — one run, one set of evidence.

## What it is built on

The library draws only while a frame observer is set (`snaggletooth/snes/video_frame.h`), so the
tool sets one, and each finished frame goes to the window, the recording and the table in turn
before the next frame is allowed to begin. The machine runs at the NTSC clock rate.

This is the only target that links SDL. It is built when `SNAGGLETOOTH_BUILD_PLAYER` and
`SNAGGLETOOTH_BUILD_TOOLS` are both on — the default for a top-level build — and takes SDL from a
parent build that already defines `SDL3::SDL3`, or from the pinned submodule at `third_party/sdl`,
built statically with the subsystems a window does not need switched off. The library itself never
links SDL.
