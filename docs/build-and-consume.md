# Build & consume

How to build Snaggletooth, what targets it exposes, and how a program of your own links the
library and drives a machine.

## Contents

- [Requirements](#requirements)
- [Targets](#targets)
- [Build modes](#build-modes)
  - [Build options](#build-options)
- [Consuming the library](#consuming-the-library)
  - [What the library is](#what-the-library-is)
  - [Driving the console](#driving-the-console)
  - [Driving the audio unit alone](#driving-the-audio-unit-alone)
  - [Linking a tool library](#linking-a-tool-library)
- [Versioning](#versioning)
- [Dependencies](#dependencies)
- [The test data](#the-test-data)
- [License](#license)

## Requirements

- CMake 3.24 or later
- A C++20 compiler. The suite runs on every push on GCC (Linux x64 and ARM64), Apple Clang
  (macOS ARM64) and MSVC from Visual Studio 2022 (Windows x64 and ARM64), each with warnings as
  errors on the project's own targets.
- Git. SDL3 is a submodule, needed by the player alone; clone with `--recurse-submodules` when
  you want the player, and plainly when you do not.

```sh
git clone --recurse-submodules <repo-url>
cd Snaggletooth
cmake -B build
cmake --build build
ctest --test-dir build
```

A build is optimized by default: when nothing supplies `CMAKE_BUILD_TYPE`, a top-level configure
sets it to `Release`. Multi-config generators choose at build time as they always do
(`cmake --build build --config Release`).

## Targets

| Target | Alias | Purpose |
|---|---|---|
| `snaggletooth` | `snaggletooth::snaggletooth` | The library: both machines, both CPU cores, the S-DSP, the PPU and the cartridge functions. This is what a program of your own links. |
| `snaggletooth_spc`, `snaggletooth_video`, `snaggletooth_disasm`, `snaggletooth_assembler`, `snaggletooth_formats`, `snaggletooth_spc700`, `snaggletooth_cpu65816`, `snaggletooth_ir`, `snaggletooth_ir_lockstep`, `snaggletooth_ir_provenance`, `snaggletooth_ir_differential`, `snaggletooth_rom_render`, `snaggletooth_rom`, `snaggletooth_player_pads` | — | The tool libraries. Each command-line tool is a thin `main` over one of them, and a program that wants a tool's capability links the library rather than running the tool. [tools/README.md](../tools/README.md) lists which library carries what. |
| `snes_disasm`, `snes_render`, `snes_verify`, `snes_lift`, `snes_differential`, `snes_examples`, `cpu65816_disasm`, `cpu65816_asm`, `spc700_disasm`, `spc700_asm`, `rom_render`, `spc_render` | — | The command-line tools. They land in the build directory's root. |
| `snes_player` | — | The player: a cartridge in a window, driven by a keyboard, a controller or an input script, recorded as it runs. The one target that links SDL. |
| `snaggletooth_tests` | — | The suite, one GoogleTest binary registered with `ctest`. |

The library ships as source. Everything public is in the `snaggletooth` namespace under
[`include/snaggletooth/`](../include/snaggletooth): the console in `snes/`, the audio unit in
`apu/`, the main CPU core in `cpu/`, and `version.h`. Nothing under `src/` or `tools/` is a public
header.

## Build modes

One source tree supports three configurations:

1. **Standalone.** Configure the repository as the top-level project, as above. This builds the
   library, the tool libraries and command-line tools, the player when SDL is present, and the
   suite. It is the configuration the suite runs in on every push.
2. **As a subproject.** A program of your own adds the repository with `add_subdirectory` and
   links `snaggletooth::snaggletooth`. The suite, the tools and the player are off in this
   configuration, so a parent's `ctest` shows only the parent's tests, no GoogleTest is fetched,
   and SDL is not configured. The parent's warning settings are its own: warnings-as-errors is
   set on Snaggletooth's targets only.
3. **As a subproject, with the tools.** The subproject above with `SNAGGLETOOTH_BUILD_TOOLS` on:
   the parent links a tool library — the cartridge disassembler, the assemblers, the codecs —
   without the suite. The player follows only when `SNAGGLETOOTH_BUILD_PLAYER` is on as well.

### Build options

| Option | Default | Effect |
|---|---|---|
| `SNAGGLETOOTH_BUILD_TESTS` | ON when top-level, OFF as a subproject | Build the suite. Fetches GoogleTest 1.14.0 at configure time. |
| `SNAGGLETOOTH_BUILD_TOOLS` | ON when top-level, OFF as a subproject | Build the tool libraries and the command-line tools. The suite needs the tool libraries, so it builds them whether or not this is on. |
| `SNAGGLETOOTH_BUILD_PLAYER` | ON when top-level, OFF as a subproject | Build the player. Takes effect only when the tools are built too. A build that already defines `SDL3::SDL3` supplies it; otherwise the submodule under `third_party/sdl` is built statically, with the subsystems a window does not need switched off. |
| `SNAGGLETOOTH_SPC700_VECTORS` | empty | See [The test data](#the-test-data). |
| `SNAGGLETOOTH_65816_VECTORS` | empty | See [The test data](#the-test-data). |
| `SNAGGLETOOTH_BLARGG_ROMS` | empty | See [The test data](#the-test-data). |
| `SNAGGLETOOTH_PPU_ROMS` | empty | See [The test data](#the-test-data). |
| `SNAGGLETOOTH_BOOT_ROM` | empty | See [The test data](#the-test-data). |

Set any of them at configure time, e.g. `cmake -B build -DSNAGGLETOOTH_BUILD_PLAYER=OFF`. When
`ccache` is installed and Snaggletooth is top-level, compilation routes through it.

## Consuming the library

Vendor the repository into your tree — a git submodule at a path of your choosing works — add
it as a subdirectory, and link the library target:

```cmake
add_subdirectory(third_party/snaggletooth)
target_link_libraries(your_program PRIVATE snaggletooth::snaggletooth)
```

That is the whole of the build side. The library's include directory and its C++20 requirement
travel with the target. No option needs setting: the suite, the tools and the player are off in a
parent build, and the library itself links nothing but the standard library — no SDL, no PNG
decoder, no test framework.

### What the library is

The library is the console and its parts as values a program holds and drives. A machine is
built from a configuration value, advanced by a cycle budget or one instruction at a time, and
snapshotted and restored whole. What it produces reaches you through observers you set — stereo
frames, each finished picture, every bus access, each change to the save window — and what you
hand it is a controller state, a snapshot, or a place to write. It opens no window, no audio
device and no file, and it keeps no clock: the program that links it owns time, I/O and the
files, and decides what the machine's output becomes. The same machine run twice from the same
state under the same inputs produces the same bytes, whether or not a host is reaching into it.

The pages under [docs/](README.md) describe each surface in full. The two that a program linking
the library reads first are [snes-machine.md](snes-machine.md) for the console and
[apu-machine.md](apu-machine.md) for the audio unit on its own.

### Driving the console

```cpp
#include "snaggletooth/snes/snes.h"

#include <cstdint>
#include <vector>

using namespace snaggletooth;

std::vector<std::uint8_t> cartridge = /* a cartridge image, without a copier's 512 bytes ahead of it */;
Snes machine(SnesConfig{.rom = cartridge, .region = Region::Ntsc});

// One NTSC frame of master cycles, then the audio the frame produced.
machine.run(357'368);
std::vector<StereoFrame> sound = machine.takeFrames();
```

The image is copied in, so the span it comes from need not outlive the call. Which map the
cartridge uses is read from its header; `SnesConfig::map` names one outright, and
`SnesConfig::saveRamBytes` overrides the header's save size. A cartridge dump that carries a
copier header is not stripped for you: `readCopierHeader` in
[snes-cartridge.md](snes-cartridge.md) says whether one is there and how long it is.

A picture arrives through a `FrameObserver` set with `setFrameObserver`, each finished frame as
it completes; a controller is plugged into a port, or pulled, with `setJoypad`, its buttons given
whole; a snapshot is `state()`, a reference to the machine's own value, and goes back with
`restore()`. [snes-machine.md](snes-machine.md) covers each of these, the
save observer that tells you when the battery save changed, and the host face — reading and
writing any place the machine holds, answering an access before it takes effect, and calling a
routine the cartridge holds.

### Driving the audio unit alone

The audio unit is a machine of its own, with no console around it:

```cpp
#include "snaggletooth/apu/apu.h"

snaggletooth::Apu apu;

const std::uint8_t program[] = {0xE8, 0x2A};  // MOV A,#$2A
apu.loadRam(0x0200, program);
apu.setPc(0x0200);
std::uint32_t cycles = apu.step();  // 2
```

An `.spc` dump becomes a running machine through the loader in the `snaggletooth_spc` tool
library ([spc-rendering.md](spc-rendering.md)); the library alone carries the machine, not the
file format.

### Linking a tool library

A program that wants a tool's capability in process — disassembling a cartridge, assembling
source, reading a tile sheet back into bytes — turns the tools on and links the library the tool
is a `main` over:

```cmake
set(SNAGGLETOOTH_BUILD_TOOLS ON CACHE BOOL "" FORCE)
add_subdirectory(third_party/snaggletooth)
target_link_libraries(your_program PRIVATE snaggletooth_rom)   # the cartridge disassembler and verifier
```

The tool libraries' headers are under [`tools/`](../tools) beside their sources, on the include
path each target carries. Which library carries which capability, and the page that documents
it, is the table in [tools/README.md](../tools/README.md). The tool libraries are not held to the
same stability as the public headers: their surfaces are documented in place and each page's
"Stability" section says how settled it is.

## Versioning

`snaggletooth::version()`, declared in `snaggletooth/version.h`, returns the library's version as
a `std::string_view` in `"major.minor.patch"` form — `"0.0.1"` today. The view refers to static
storage and is valid for the life of the process. It is an identity a program can log or display
and carries no behavior. The status of each component sits in the
[project README](../README.md#what-is-built), which is where "what does this build do" is
answered.

## Dependencies

- **[SDL3](https://github.com/libsdl-org/SDL)** — the window, the audio device and the
  controllers of the player, and nothing else. A submodule at `third_party/sdl/`, pinned to
  release 3.4.16, built statically with `SDL_SHARED` off and the camera, haptic, power and
  dialog subsystems off. A parent build that already defines `SDL3::SDL3` supplies its own and
  the submodule is left unconfigured. The library never links it. zlib license.
- **[lodepng](https://github.com/lvandeve/lodepng)** — the PNG encoder and decoder behind the
  tile-sheet codec in the tool libraries. Vendored as single-file source at
  `third_party/lodepng/`, built as its own static target with a system include so its warnings
  are its own, and linked privately into `snaggletooth_formats`; no lodepng symbol reaches a
  header of this project. The library never links it. zlib license.
- **[GoogleTest](https://github.com/google/googletest)** — fetched at configure time, version
  1.14.0, only when `SNAGGLETOOTH_BUILD_TESTS` is on. Never part of the library or the tools.

A program that links the library alone ships the library's own objects and the standard library.
There is no runtime dependency to install.

## The test data

Three bodies of third-party reference data and two kinds of third-party image are never
vendored. The tests that use them register and skip, with a visible reason, until they are pointed
at a local copy; `ctest` then reports them as skipped rather than passed.

| Option | Points at |
|---|---|
| `SNAGGLETOOTH_SPC700_VECTORS` | the SingleStepTests SPC700 `v1` directory |
| `SNAGGLETOOTH_65816_VECTORS` | the SingleStepTests 65816 `v1` directory |
| `SNAGGLETOOTH_BLARGG_ROMS` | a directory holding the four Blargg SPC test ROMs, searched recursively |
| `SNAGGLETOOTH_PPU_ROMS` | a directory holding the PPU test cartridges, searched recursively |
| `SNAGGLETOOTH_BOOT_ROM` | a 64-byte audio boot ROM you hold your own dump of |

```sh
cmake -B build -DSNAGGLETOOTH_65816_VECTORS=/path/to/65816/v1 \
               -DSNAGGLETOOTH_SPC700_VECTORS=/path/to/spc700/v1 \
               -DSNAGGLETOOTH_BLARGG_ROMS=/path/to/blargg
```

Without a boot ROM the acceptance tests boot on the built-in upload program and assert its
outcome. Setting `SNAGGLETOOTH_REQUIRE_65816_VECTORS`, `SNAGGLETOOTH_REQUIRE_VECTORS`,
`SNAGGLETOOTH_REQUIRE_BLARGG_ROMS` or `SNAGGLETOOTH_REQUIRE_PPU_ROMS` in the environment
`ctest` runs under turns the matching skip into a failure, which is how a build that is meant to
have the data is kept from passing without it.

## License

Snaggletooth is MIT licensed — see [LICENSE](../LICENSE) at the repository root. Vendored
dependencies retain their own licenses.
