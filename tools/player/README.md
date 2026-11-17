# The player

`snes_player` runs a cartridge in a window, with its sound and your controller, and records the same run when asked to, and disassembles the cartridge as you play it when asked to.

```
snes_player <image> [--out <directory>] [--seconds N] [--scale N]
                    [--input <script> | --input-dir <directory>] [--config <file>]
                    [--region ntsc|pal] [--vsync on|off|auto] [--unthrottled]
                    [--interlace weave|bob|off] [--apu-thread] [--ppu-thread] [--ppu-verify]
                    [--gpu-ppu] [--gpu-verify] [--mute] [--quiet]
                    [--disasm <directory>] [--disasm-buffer-memory <GB>] [--disasm-buffer-disk <GB>]
                    [--disasm-buffer-dir <directory>] [--disasm-threads <n>]
                    [--disasm-resume-at <percent>]
snes_player --default-config
snes_player --user-files
snes_gpu_verify <image> <seconds> <input-dir|-> [frames] [--ppu-thread] [--pass-times]
```

The window shows the picture the machine draws, frame by frame, at the rate the run is held to — a frame drawn in half-pixels, 512 wide, fills the same window as a 256-wide one, each half-pixel half a scaled pixel, and an interlaced run is woven into its full-height picture (see below), with the rate it achieves in its title so what the run costs is visible while it runs. It closes when the window is closed or when `--seconds` of the master clock have been spent; nothing else stops it. A run started with `--disasm` closes when its disassembly is finished ([Disassembling as you play](#disassembling-as-you-play)).

The machine runs at the rate the cartridge's own country byte asks for, so a 50 Hz cartridge boots rather than refusing, and is paced at 50 Hz. `--region` sets it by hand for an image that does not say, or one patched to run at the other rate while still declaring the first.

## The rate it runs at

A television took whatever the console sent it. A fixed-refresh panel does not, and the two rates are not the same number: the 60 Hz console draws 60.0988 frames a second and a 60 Hz panel refreshes 60.000 times, so a run paced to the console alone puts a frame on the wrong side of a refresh about once every ten seconds — shown twice, or torn — for as long as it runs, with nothing to pull the phase back once something has moved it.

So the run is held to the panel instead, wherever the panel is within one part in a hundred of the console: the window waits for a refresh and every refresh carries one new frame. The first thing a run says is which of the two rates it took —

```
held to the display at 60.000 Hz
the console's own 60.099 Hz
```

**A held run is a sixth of a per cent slow by the wall clock, and exact in every other sense.** Every cycle is emulated as it always was and a recording of a held run is byte-identical to a recording of an unheld one; what changes is the rate at which frames are asked for. A panel at another rate entirely — 50 Hz, 144 Hz — is not one a run can be held to, so the run keeps the console's own rate there and the beat comes back with it.

`--vsync` names the arrangement outright: `on` holds the run to whatever the panel reports, `off` keeps the console's rate and hands every present straight back, and `auto`, the default, decides by the rule above.

`--unthrottled` turns the pacing off: no frame waits for its deadline and vsync is off, so the machine runs as fast as the host allows. The window is drawn no faster than the panel refreshes, because a present past that rate waits for the display — on macOS the Metal layer hands out one drawable per refresh whether or not the present is asked to wait — and that wait would hold the run to the display's rate, the very thing the flag turns off. So each refresh shows the frame current at that moment, the frames between are emulated and counted and not drawn, and `--out` keeps every one of them. The run's first line says so —

```
unthrottled: no frame pacing, no vsync
```

— and the mean rate is printed at exit, as for every run.

## The audio unit on its own thread

`--apu-thread` runs the audio unit on a thread of the machine's own, beside the one that steps the console ([The audio thread](../../docs/snes-machine.md#the-audio-thread)). It is for a host whose cores are each too slow to run the whole machine at speed: the thread that steps the console does less work per frame, the audio unit's work moves to a second core, and the waits where the two threads meet add to it. The process as a whole uses a little more CPU in total, and each thread has less to do, so each core carries a lighter load and a slower device keeps up by running the work in parallel. Turn on [`--ppu-thread`](#the-picture-on-its-own-thread) first, which takes the larger share of the work off the thread that steps the console, and add `--apu-thread` only where `--ppu-thread` alone does not hold the console's frame rate; on a host that already runs the console at speed, leave it off. The picture and the sound are the same bytes either way, and the run says which way it took —

```
the audio unit on its own thread
```

## The picture on its own thread

`--ppu-thread` draws the picture on a thread of the machine's own, beside the one that steps the console ([The picture thread](../../docs/snes-machine.md#the-picture-thread)). It is for the same host as `--apu-thread` and is the one to turn on first; `--apu-thread` joins it only where it alone does not hold the console's frame rate. With it on the thread that steps the console keeps the chip's registers, its memories, the beam and the sprite passes, and the drawing moves to another core. The process as a whole uses a little more CPU in total, and each thread has less to do, so each core carries a lighter load and a slower device keeps up by running the work in parallel. The pixels are the same either way, each frame is whole before the window shows it, and the run says which way it took —

```
the picture on its own thread
```

## The lines it draws

The machine draws a picture line only when something the line reads has changed since the frame before — a register the picture reads, the sprites on the line, a page of video memory or a group of palette words the line reads, or a write landing inside the line — and takes the line from that frame otherwise ([A line drawn again only when it changes](../../docs/snes-machine.md#a-line-drawn-again-only-when-it-changes)). The picture is the same either way. The run's last line says how many lines it took and how many it drew —

```
lines: 76188 taken from the frame before, 23436 drawn (76 % taken)
```

`--ppu-verify` draws every line and compares it with what the frame before would have stood in with, and the last line adds the count of lines that disagreed, which is 0 on a machine that reuses exactly. It costs the reuse, so it is for proving a run, not for playing one; the run says so —

```
every line drawn and checked against the frame before
```

## The picture on the GPU

`--gpu-ppu` hands the machine a GPU device and the picture is drawn there, by the core's own compute shader from each frame's timeline ([gpu-ppu.md](../../docs/gpu-ppu.md)): every pixel the one the dot renderer draws, and nothing drawn on the CPU. The window shows the texture the shader leaves, scaled by a whole number as before; an interlaced run is placed in it the way `--interlace` asks. The pads, the sound, the pacing, `--out` and the rest of the run are as they are without the flag. The run says which shader format and driver the device took and how the window is presented — at the display's refresh where the run is held to the display, and handed straight back where it is not —

```
the picture drawn on the GPU (metallib, metal), presented vsync
```

and the last lines add what the GPU cost: the microseconds the thread that steps the console spent uploading, dispatching and submitting a frame, mean, least and most —

```
gpu: 29 µs a frame to upload and draw (22–1096)
```

`--gpu-verify` turns the GPU on and keeps the CPU drawing too: every frame is drawn both ways and compared pixel for pixel, a 512-wide frame column for column and a 256-wide one position for position, and the last line says how many frames differed and where the first difference stood, as `frame:line:x` —

```
gpu-verify: 3606 frames, 0 differed, 0 shapes differed, first at -
```

It costs the CPU's drawing and a read-back a frame, so it is for proving a run, not for playing one; the run says so. A shape that differs is a frame whose width, height or parity the two sides disagree on.

`snes_gpu_verify` is the same comparison with no window, for a whole library: one cartridge run for N seconds under its input script, as the sweep harness runs it, and one tab-separated row — the image, the frame digest, the frames, the timelines, the frames whose pixels differed, the frames whose shape differed, the frames with no timeline in their cycle, the first difference as `frame:line:x` or `-`, the median and the largest nanoseconds the stepping thread spent on a frame's upload and dispatch, the median and the largest nanoseconds from that entry until the device had finished drawing the frame, and the median packed frame in bytes. With `frames` the digest folds only the first that many frames, as the sweep harness's does; `--ppu-thread` draws on the picture thread. A machine with no GPU device prints `<image> NO GPU <reason>` and exits 0.

`--pass-times` divides a frame's device time between its parts: the device submits the upload and each of the three passes on a command buffer of its own and waits for each on a fence, and the row gains ten columns — the median and the largest nanoseconds of the upload, the sprite pass, the tile pass and the pixel pass, then the register events and the palette events of the frame whose device time was the largest. Each part carries a fence's round trip of its own, so under the flag the device columns say where a frame's time goes, not what the frame costs; the row without the flag says that.

The device is the player's own code over SDL's GPU API, `sdl_gpu_device.h`: it is what the core's `gpu::Device` interface looks like implemented, with the present and the read-back a host adds for itself, and the GPU suite (`snaggletooth_gpu_tests`) opens it with no window. It records a frame's upload and its three passes on one command buffer and submits it once. Once its three pipelines are made it clears the texture and runs the three passes over a frame header that draws nothing, waited for, inside the machine's constructor: a GPU's first use of a texture and of each pipeline costs milliseconds, and paid there the first frame costs what any frame does.

## Interlacing

A cartridge that turns interlace on (SETINI bit 0) draws one field a frame — half the picture's lines, the two fields alternating — and the SNES's signal wove them into one full-height picture. The player does the same, and `--interlace` names how:

| Mode | What it shows |
|---|---|
| `weave` | the two fields interleaved into the full-height picture the console drew — 448 lines, or 478 with overscan — field F's line at row `2i+F`. Sharp; it combs where the two fields differ under motion. The **default**. |
| `bob` | each field on its own, its lines doubled to full height, a new picture every field. No combing; a shimmer where the fields differ. |
| `off` | the field as the machine made it, half height — what the player did before this. |

A run the cartridge did **not** interlace draws a whole picture a frame and is shown unchanged whatever the mode; there is nothing to weave. The window's size does not change — the full-height picture fills the same area, at double the vertical resolution, the way 480i filled the same screen as 240p. `--out` keeps whatever is shown, so a recording of an interlaced run is the woven picture.

The sound goes to the default playback device as the machine makes it, 32 kHz stereo. The device is told the rate the run *delivers* at rather than the rate the chip makes: a held run makes its samples that same sixth of a per cent slower, and a device consuming 32,000 a second would run dry every few minutes. A run on the console's own rate is handed the chip's own 32 kHz and nothing resamples it. `--mute` opens no device at all, and a machine that has none runs silent and says why. Where the queued sound runs more than a quarter of a second ahead of the speakers a chunk is left out rather than added to the delay: the device consumes at its own crystal, so the two still drift apart slowly over a long run.

## Playing it

The keyboard and every gamepad plugged in drive the ports. Each pad is taken as it arrives, given the lowest port free, and named on the way in — the family it belongs to and what the system calls it — so you can see which controller went where. Pulling one out empties its port at once, and the next pad plugged in takes the port that came free. A third pad is reported and left idle.

A pad is read by **where its buttons sit** rather than by the letters printed on them, so every family plays the same way round: the button under your thumb at the south position is B, east is A, west is Y, north is X. The keyboard's default puts the four face buttons at the same positions — `Z` is B, `X` is A, `A` is Y, `S` is X, with `Q` and `W` for L and R, `Return` for Start, `Backspace` for Select, and the arrows for the d-pad.

The keyboard and a pad on one port add together, so a hand on each is one player.

Command and `R` on a Mac, Control and `R` everywhere else, is the console's reset button. The cartridge starts again from its reset vector with work RAM and its save as they were, the way it does on the console. A run replayed from a script or recorded to one leaves the button alone, since a script has no word for it.

Command and `P` on a Mac, Control and `P` everywhere else, is the pause chord: under `--disasm` it stops the game and resumes it ([When the buffer reaches its ceilings](#when-the-buffer-reaches-its-ceilings)).

`--config <file>` hands the tool a mapping of your own, and `--default-config` prints the one it ships with so you have something to copy. The whole form is [pad-config.md](../../docs/pad-config.md).

With no `--config`, a mapping kept at `config/input/default.snagpad` where your files go is what the run takes, and the built-in one when there is none. The tool says which it took. `--user-files` prints that directory and exits — see [user-files.md](../../docs/user-files.md).

## Keeping a save

A cartridge whose header declares a battery keeps what it writes, in `sram/<cartridge>.srm` beside your configuration: the file [bsnes, snes9x and Mesen](../../docs/user-files.md#a-cartridges-save) read and write for the same cartridge. It is read in before the cartridge's first instruction and written as the machine reports the save changed, with whatever the last moments left written as the run closes. A file whose size is not the one the cartridge declares is left exactly as it is and that run keeps nothing, so a save belonging to something else is never overwritten; the tool says so when it starts. A disk that refuses is reported once and the run carries on.

The window asks to be raised when it opens. A window opened from a terminal does not take the keyboard on every platform, and a run you cannot press a button on is not a run — if the keys are going to the terminal instead, click the window once.

A run driven by a recorded script leaves the keyboard and the pads alone: `--input` names a script and `--input-dir` a directory of them, from which the one named for the image is replayed, or the directory's `default.snaginput` when the image has none — the same [input scripts](../../docs/input-script.md) the cartridge disassembler replays. Two runs of one cartridge are then the same run, which is what makes two recordings worth setting beside each other, and the tool says so at the start rather than leaving you pressing buttons at a replay.

## Recording it

`--out` writes what the run produced into a directory, named after the image. Without it the run keeps none of it — the sound goes to the speakers and is let go rather than held for a file nobody asked for:

| File | What it holds |
|---|---|
| `<image>.avi` | every frame exactly as the machine drove it, uncompressed, at the largest shape the run produced (`../video/README.md`) |
| `<image>.csv` | a row a frame: wall and emulation time in nanoseconds, master cycles, dots drawn, the rate instantaneous and mean, and under `--gpu-ppu` the nanoseconds the frame's upload and dispatch took, 0 otherwise |
| `<image>.wav` | the sound the run produced, 32 kHz stereo — the chip's own rate, whatever the panel showing it runs at |
| `<image>.snaginput` | the buttons, as a script that replays the run — under the name every replay looks for, so `--input-dir` on this directory, here or in the toolkit, replays the run |

They come together — one run, one set of evidence. The script is written whatever drove the run, so a run you played by hand replays exactly, and one driven by a script is written back in the same canonical form.

## Disassembling as you play

`--disasm <directory>` disassembles the cartridge while you play it, into a tree in that directory: the tree [`snes_disasm`](../../docs/snes-disassembler.md) writes, made from the run you are playing. The game waits for none of it. The thread that steps the console copies what the machine reports into a buffer and does nothing else for the disassembly; the disassembly's own threads work through the buffer, checking every instruction of both CPUs and following where each byte came from; and one more makes the tree, one tree after another, each from everything the run has seen by then. After each tree the player runs `snes_render` and then `snes_verify` from its own directory, so the directory holds source files that assemble back to the image while you are still playing. When the run closes, the last tree is made from the whole session.

```
snes_player drawing.smc --disasm drawing-tree
```

The flag turns `--apu-thread` and `--ppu-thread` on, so the thread that steps the console carries the copying and little else. The run says what it started, and each tree as it is written —

```
disassembling into drawing-tree on 8 threads; the buffer takes up to 1.0 GB of RAM and 10.0 GB of files under drawing-tree/buffer
tree: 1 files, 430 instructions, 3 entries, 0 stops, 32768 of 32768 bytes placed, 0 notes -> drawing-tree
```

A second session over the same directory reads the tree that is there and adds what it reaches ([Running the cartridge](../../docs/snes-disassembler.md#running-the-cartridge)). A directory whose tree was written for another image refuses the run before the window opens. A tree made while you play counts each sighting as of that tree; the closing tree's counts are the session's.

### The buffer

The buffer holds what the disassembly's threads have not reached: in RAM first, then in files. You set the ceiling on each.

| Flag | What it sets | Default |
|---|---|---|
| `--disasm-buffer-memory <GB>` | the most RAM the buffer takes | 1 |
| `--disasm-buffer-disk <GB>` | the most its files hold | 10 |
| `--disasm-buffer-dir <directory>` | where the files go | `buffer` under the tree's directory |
| `--disasm-threads <n>` | the threads that check the instructions | two fewer than the computer's cores, at least 1 |
| `--disasm-resume-at <percent>` | how far the buffer drains before a stopped game resumes | 50 |

A size is in GB of 1,000,000,000 bytes and takes a fraction: `0.5` is 500,000,000 bytes. Each file is removed as it is read back, and the directory when the run closes. On a computer whose threads keep up with the game the buffer stays in RAM; on a slower one it grows through the files while you play and drains while you do not. The files never hold more than their ceiling. Where the files are written more slowly than the game fills the buffer, RAM holds the blocks the files have not taken yet; once that passes the RAM ceiling by more than the blocks being written and one more while the files have room, the game stops until RAM has drained to `--disasm-resume-at` percent of its own ceiling, or the buffer holds nothing, and the run says so —

```
stopped: the disassembly's buffer holds 503 MB of RAM, past its 500 MB ceiling, while its files are written; the game resumes at 250 MB of RAM
```

### When the buffer reaches its ceilings

Nothing the run copies is dropped. When the buffer holds both of its ceilings the game stops: the window asks for attention, its title says what the buffer holds and when the game resumes, and the run says — here a run started with `--disasm-buffer-memory 2 --disasm-buffer-disk 0` —

```
stopped: the disassembly's buffer holds 1.9 GB of 2.0 GB (2.0 GB of RAM, 0 MB of files); the game resumes at 1.0 GB
resumed after 1 s
```

A stop that lasts past two seconds also says, once, how fast the buffer is draining and about how long it has to the resume level: `draining at <n> MB a second: about <time> to <level>`.

While the game runs, one of the disassembly's threads works at the game's own scheduling class — the one that plays the main CPU's records in order, which the rest wait on — and the others work under it. While the game is stopped they all work at the game's class, so a stop drains faster than play does. The game resumes when the buffer has drained to `--disasm-resume-at` percent of its ceilings: a lower level is a longer stop and a longer stretch of play after it, and `0` waits for the buffer to empty.

The pause chord stops the game by hand and resumes it, so a stop can be taken when it suits you and not when the buffer calls it. A resume asked for while the buffer stands at its ceilings is refused, and the run says where the game resumes. A stop runs no cycle of the machine: a recording, the sound, a script and `--seconds` do not see it.

The ceilings are where the game stops. The call into the machine that reaches them is finished first — the player cannot interrupt one — and what it copies past them stays in RAM, under a megabyte for an ordinary quarter frame.

### Closing

A run with `--disasm` closes when its work is done. Every request to close — the window's close button, Command-Q, Ctrl-C, a `kill` — stops the game and starts the close: the buffer is worked through to its end, the last tree written, and the two commands run. The window stays up and its title says what is left ([The window's title](#the-windows-title)). A save and an `--out` recording are written before the close begins. A second request ends nothing and is answered —

```
still finishing the disassembly: 462 MB to play, then the last tree. It ends by itself when that is done; kill -9 47432 ends it without the last tree
```

When the close is done the run says what the session cost the buffer: the most it held beside its two ceilings, how much went through its files, and how often and how long the game stopped —

```
disassembly: the buffer held at most 412 MB against 1.0 GB of RAM and 10.0 GB of files; 0 MB went through its files; the game never stopped
```

A session whose buffer stays under its RAM ceiling writes nothing to the disk; one that reads `2.6 GB went through its files; the game stopped 2 times for 1 min 40 s` fell behind the game by that much.

A forced kill — `kill -9`, Force Quit, Task Manager's End Task — is the one thing that ends it early, and it can leave the tree's files part-written. On Windows, closing the console window, logging off and shutting down end the process a few seconds later whatever it does; the player says so when one arrives.

### The window's title

Under `--disasm` the title says three things after the game's name, in this order: the rate — or, while the game is stopped, why and when it resumes — then the buffer's RAM against its ceiling, then its files against theirs.

```
drawing — 60.1 fps — RAM 412 MB of 1.0 GB — files 0 MB of 10.0 GB ⠹
```

The dot at the end turns while the disassembly is working — its threads playing the buffer, the buffer filling or draining, a tree being written or checked — and stands still when none of that changes, so a disassembly that has stalled shows as a dot that has stopped. The two sizes are read every two seconds.

A window too narrow for all three shows them a page at a time: as many in order as fit, each page for four seconds, the first again after the last. The game's name stays in place and the dot ends every page. Whether a title fits is an estimate from the window's width and a width for each character, since no window system says how wide it draws a title. The first page is shown at once when the game stops or resumes, when the close begins, and at each stage of the close.

While the run closes, the first of the three is the close's progress:

```
drawing — finishing: 3.2 GB of 12.4 GB played, 25 %, about 1 min 40 s left — RAM 1.0 GB of 1.0 GB — files 8.2 GB of 10.0 GB ⠼
```

It counts from the most the buffer has held since the close began, and a close with nothing in the buffer says `finishing: nothing left to play`. The time is the buffer's alone, at the rate it is draining.

The last tree and the two commands follow, and the title names each as it runs. The tree names the step it is at — `tracing`, `proving what every path reaches`, `lifting the files`, `drawing the previews`, `writing the files` — and how long it has left:

```
drawing — finishing: writing the last tree, proving what every path reaches, about 40 s left
drawing — finishing: writing the last tree, drawing the previews, 2 min 10 s
drawing — finishing: snes_render, 12 s
```

The time left is what the last tree the session wrote spent in the step this one is in and in each step after it. The last tree is made from more of the run than that one was, so it tends to run past the figure; once a step has run longer than it did in that tree, and in a session that wrote no tree before its close, the title says how long the stage has run, as it does for `snes_render`, `snes_verify` and `closing the buffer`.

A tree being made when the close is asked for is given up at its next step, since the last tree is made from the whole run after it, and the two commands of one already written are not begun; the run says `quit during a mid-session tree: skipped it, the final tree covers the whole session`.

## What it is built on

The library draws on the CPU only while a frame observer is set (`snaggletooth/snes/video_frame.h`), so the tool sets one, or under `--gpu-ppu` hands the machine its GPU device and sets a timeline observer instead, which is told of each frame once the device has drawn it. The library hands each finished frame over as the frame's vertical blank begins, and the frame goes to the window, the recording and the table in turn before the machine runs on. The controllers are read after that frame's wait rather than before it and presented there, ahead of the frame's auto-read, so what you are holding is sampled immediately ahead of the moment the console latches it, and the script records it as that frame's line ([input-script.md §2](../../docs/input-script.md#2-frames)). The sound is taken from the machine between frames, a quarter of a frame at a time, and handed straight to the device.

The mapping is `snaggletooth_player_pads`, which holds no reference to any windowing library and opens no device: it turns a controller's reported state into the `Joypad` the machine takes. The rate arithmetic in `display.h` is the same kind of thing — it is handed what a panel reports and answers with the rate to pace by, when each frame of that pacing is owed, and the rate to open a device at, so all of it is checked by the suite. Every call that touches a real device is in this tool.

The player, `snes_gpu_verify` and the GPU suite are the targets that link SDL. They are built when `SNAGGLETOOTH_BUILD_PLAYER` and `SNAGGLETOOTH_BUILD_TOOLS` are both on — the default for a top-level build — and takes SDL from a parent build that already defines `SDL3::SDL3`, or from the pinned submodule at `third_party/sdl`, built statically with the subsystems a window, a speaker and a gamepad do not need switched off. The library itself never links SDL.

**Run a `Release` build.** A `Debug` build of the machine is several times slower, and the player built that way falls well short of the console's rate, so the rate in the window's title reads as a slow emulator when it is only the build. A Debug player says so before anything else when it starts. These commands build the player as `Release` on every platform:

```
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --target snes_player
```

The player is then `build/snes_player`, or `build\Release\snes_player.exe` on Windows. On Windows, `--config Release` is the flag that matters: without it the build is `Debug`, and the player lands in `build\Debug\`. [docs/build-and-consume.md](../../docs/build-and-consume.md#building-a-release-build) says which flag each generator reads.
