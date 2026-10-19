# The GPU PPU

The picture drawn on a GPU by the core's own compute shader, from each frame's timeline, every
pixel the one the dot renderer draws. A host hands the machine a device and the machine draws
every finished frame on it; the host presents the texture the shader leaves. The shader, the
layout of what it reads and the dispatch are the core's; the device, the texture and the
presentation are the host's. The core links no GPU API.

## Contents

- [What it is](#what-it-is)
- [Switching it on](#switching-it-on)
- [The device](#the-device)
- [The three passes](#the-three-passes)
- [The packed frame](#the-packed-frame)
- [The scratch buffer](#the-scratch-buffer)
- [The texture and the row mapping](#the-texture-and-the-row-mapping)
- [Compiling the shader](#compiling-the-shader)
- [Holding it to the dot renderer](#holding-it-to-the-dot-renderer)
- [Gotchas](#gotchas)
- [See also](#see-also)

## What it is

The machine's dot renderer draws the picture on the CPU, a dot at a time inside the cycles that
pass it ([ppu.md](ppu.md)). The GPU PPU draws the same picture on a GPU from the frame's timeline
— the registers the picture reads as each line's first read dot found them, every write a line's
dots read at the dot it reaches, the palette and sprite table as each line read them, and VRAM
with every write that landed inside the picture ([ppu.md §The picture as a timeline](ppu.md#the-picture-as-a-timeline)).
Every rule the dot renderer applies, the shader applies at the same dot from the same inputs, so
the texture it leaves is the frame the chip drew, half-pixel for half-pixel.

It is the core's third way of drawing the picture, beside the calling thread and the picture
thread, and it is its own option: `SnesConfig::gpuPpu` takes a device, and the machine draws on
it whether or not either thread is on. The chip's state, the timeline and everything a program can
observe are the same whichever way the picture is drawn.

## Switching it on

```cpp
#include "snaggletooth/gpu/gpu_ppu.h"
#include "snaggletooth/snes/snes.h"
using namespace snaggletooth;

MyDevice device;  // a gpu::Device over the host's GPU API, below
Snes machine(SnesConfig{.rom = rom, .gpuPpu = &device});
machine.run(cycles);  // every finished frame is drawn on the device before run() returns
```

With a device set the machine records every frame's timeline and, at the end of the cycle in
which the frame's vertical blank begins, packs it into the device's buffer and has the device
draw it, before a timeline observer, if one is set, is told of the frame, and before the save
observer. The call reaches the device on the thread the machine is stepped on, from inside
`step()` or `run()`. The machine draws pixels on the CPU only while a frame observer is set, so a
host that sets none and takes the picture from the device has the whole of the drawing on the
GPU.

The constructor throws `std::runtime_error` when the device takes no shader format the library
was built in, when the device cannot make a pipeline, or when the library was built with
`SNAGGLETOOTH_GPU_PPU` off and carries no shader. The device is the host's object, not part of
the state: a snapshot does not carry it and `restore()` leaves it in place.

## The device

`gpu::Device` is what a host implements, over whatever API it has — four virtual functions:

```cpp
class Device {
 public:
  virtual ~Device() = default;
  virtual ShaderFormat formats() const = 0;
  virtual bool makePipeline(Pass pass, const ShaderBytecode& code, ShaderFormat format, std::string& error) = 0;
  virtual std::span<std::uint8_t> frameBytes(std::size_t bytes) = 0;
  virtual void draw(const Frame& frame) = 0;
};
```

- `formats()` answers the formats the device takes — `ShaderFormat::Spirv`, `Dxil` and
  `Metallib`, as bits — and the core picks the shader bytes with `selectShader`.
- `makePipeline()` is called once a pass from the machine's constructor with the bytes and
  their format; `passLayout(pass)` says what the pass binds, in SDL's compute convention: the
  read-only storage buffers, then the read-write storage textures and buffers, then the uniform
  buffers, and the threads a group has. Every pass reads the frame as its one read-only buffer
  and the scratch buffer as a read-write one; `Pass::Pixels` alone writes the texture and reads
  the uniform.
- `frameBytes(bytes)` answers a span of the asked length the core packs the frame into: the
  device's upload buffer, mapped and grown to fit. The core writes exactly that many bytes.
- `draw(frame)` uploads what was packed, dispatches the three passes in order with
  `groupsFor(pass, frame)` groups each — the frame buffer bound read-only, a scratch buffer of
  `kScratchBytes` bound read-write, and for `Pixels` the texture and the uniform — and submits.

The player's device over SDL's GPU API, `tools/player/sdl_gpu_device.h`, is a complete
implementation to read beside this page, with the present and the read-back a host adds for
itself.

## The three passes

The work is divided as the chip divides it, each pass a compute pipeline of its own
(`shaders/ppu_sprites.comp.hlsl`, `ppu_tiles.comp.hlsl`, `ppu_pixels.comp.hlsl`, over the
shared `ppu_common.hlsl`):

| Pass | One thread a | Does |
|---|---|---|
| `Sprites` | recorded line | Time's walk over the line's inputs, into the line's sprite buffer: from the last sprite Range kept back to the first, at most thirty-four on-picture tiles, the last nonzero pixel found winning. A line whose Time gathered nothing gets no sprite; a carried line gets its carried buffer. |
| `Tiles` | tile column, background, line | The column's three reads as the chip makes them — the tilemap entry at dot 8E + 3, the character rows at 8E + 7 and the fine scroll at 8E + 22 — each under the registers and VRAM as they stand at that dot, into the column's record. |
| `Pixels` | position, row | The position's pixel from the registers at its own dot, 22 + x, and the records: the two screens resolved by the mode's chart, the windows, color math, Mode 7's field through its matrix, mosaic, the half-pixel line, and the converter. |

Each pass's threads are `kGroupSide` (8) across on each axis it spans; `groupsFor` gives the
groups: `Sprites` one group of lines, `Tiles` 5 × the lines × 4, `Pixels` 32 × the rows.

## The packed frame

`PpuTimeline::pack()` writes the frame as one buffer a shader reads as 32-bit words
(`PpuTimeline::packedSize()` bytes). It begins with `kHeaderWords` (32) words, every offset in
words from the buffer's start and every count in entries:

| Word | Holds |
|---|---|
| 0–3 | `height`, `width`, `firstLine`, `field` |
| 4, 5 | the lines' offset and count — 32 words each, `lines` as they stand |
| 6, 7 | the events' offset and count — 2 words each, `events` as they stand |
| 8 | VRAM's offset — 16384 words, the 64 KB as it stands |
| 9, 10 | the palettes' offset and version count — 128 words each |
| 11, 12 | the sprite tables' offset and version count — 136 words each |
| 13, 14 | the sprite buffers' offset and count — 128 words each |
| 15, 16 | the VRAM writes' offset and count — 2 words each, in position order |
| 17 | the same writes sorted by address, then by ordinal — 2 words each: `address \| ordinal << 16`, then the byte; the ordinal is the write's index in the position-ordered list |
| 18 | the page table's offset — 257 words: word `p` the index in the sorted list where the writes to addresses `p × 256` to `p × 256 + 255` begin, word 256 the list's length |
| 19 | the line table's offset — 240 words: word `L` the number of writes whose line is below `L`, so the ordinal of line `L`'s first write |
| 20–31 | zero |

Every section begins on a word boundary. A reader finds the byte at an address as of a line and
dot by counting the writes placed at or before that point — the line table's entry for the line,
stepped past the line's own writes at or before the dot — and taking, from the address's run in
the sorted list, the last write whose ordinal is below that count, or VRAM's byte where there is
none. `pack` writes nothing and answers 0 for a span shorter than `packedSize()`.

## The scratch buffer

The passes hand each other `kScratchBytes` (1,136,640) of device memory the host binds
read-write to all three: each recorded line's sprite buffer, 512 bytes — the palette word at
each of the 256 positions, then the priority at each, `kNoSprite` where no sprite claims it — at
the line's index times 512; then each line's tile records, one 32-byte record a background a
column, 33 columns, at `kTileRecordsAt + ((line × 4 + layer) × 33 + column) × 32`. A record's
first word is the tilemap entry in its low half, the depth in bits 16 to 23, the tile-size bit
at 24, the two-character bit at 25 and the fine scroll from bit 26; its second is the background
position, X then Y; its next four are the sixteen color indices a byte each; its last two are
zero.

## The texture and the row mapping

The texture is `kTextureWidth` (512) by `kTextureHeight` (478) texels of four floats, red, green,
blue and 1, each channel the converter's byte over 255; a host reading it back rounds each
channel to its byte. A frame 512 wide puts position `x`'s left half at column `2x` and its right
at `2x + 1`; a frame 256 wide puts the position's one pixel at column `x`.

The uniform is a `RowMap`: picture line `L` lands on row `(L − 1) × step + base`, and on the row
after it too where `copies` is 2. `rowMapFor(placement, interlaced, field)` gives the three
placements of an interlaced run's fields — `Weave`, each field at the rows its parity owns, the
full-height picture the console's signal carried; `Bob`, each field doubled to full height on its
own; `Field`, the field as the machine drew it — and `(1, 0, 1)` for a run the cartridge did not
interlace. The texture persists between frames, so a weave keeps the other parity's rows; a host
clears it when the picture's shape changes. `rowsUnder(rows, height)` is how many rows a frame
fills under a map.

## Compiling the shader

The shader is HLSL and is compiled at build time for the platform's format: `metallib` on Apple
systems through glslang, spirv-cross and the Metal toolchain; `dxil` on Windows through dxc;
`spirv` elsewhere through glslang. `shaders/SnaggletoothShaders.cmake` finds the tools and adds
the compile to a target, and `shaders/gen_shader.cmake` is the compile itself; the library's own
build compiles the three passes into itself under `SNAGGLETOOTH_GPU_PPU`
([build-and-consume.md §Build options](build-and-consume.md#build-options)). `gpu::ppuShader(pass)`
is the bytes the build made, in the platform's slot of a `ShaderVariants` and empty in the other
two, and `gpu::selectShader(formats, variants)` picks the slot a device takes.

A host that builds the library another way includes the module and compiles the shaders itself:

```cmake
include(shaders/SnaggletoothShaders.cmake)
snaggletooth_find_shader_tools()
snaggletooth_compile_shader(my_target STEM ppu_pixels.comp SRC shaders/ppu_pixels.comp.hlsl
                            STAGE comp INCLUDE shaders UNIFORMS 1 READONLY_BUFFERS 1 READWRITE_TEXTURES 1)
```

The header each compile writes, `snaggletooth/gpu/shaders/<stem>.h`, holds the bytes and one
`ShaderVariants` named after the stem. The counts are what the pass declares; the Metal compile
needs them because SDL's Metal convention keeps uniform, read-only and read-write buffers in one
index space where SPIR-V keeps them in three sets.

## Holding it to the dot renderer

The dot renderer is the reference. `PpuTimeline::draw()` draws a frame from its timeline through
the chip's own dot code, and the texture is held to it row for row: a 512-wide frame column for
column, a 256-wide frame each position against both of `draw()`'s equal halves. The suite's GPU
cases (`tests/gpu/gpu_ppu_test.cpp`) hold every mode, Mode 7 each way, mosaic, the offset table,
the windows and the color regions, color math, the half-pixel line, interlace and the taller
picture, events inside a line, VRAM written inside a frame, the sprite walk and its budgets,
carried and empty lines and both widths to `draw()` and to the machine's own frame of the same
cycle; `snes_player --gpu-verify` and `snes_gpu_verify` hold a whole run of a cartridge to the
machine's frames the same way ([tools/player/README.md](../tools/player/README.md)).

## Gotchas

- **The device is called from inside the machine.** `draw()` runs on the thread that stepped the
  machine, inside the report that ends the frame; a host presents afterwards, from a timeline
  observer or after the call returns, never from inside `draw()` on another thread.
- **The texture keeps the last frame.** A frame 256 wide leaves columns 256 to 511 as they were,
  and a field leaves the other parity's rows; clear the texture when the shape changes.
- **The float texture is exact by rounding.** Each channel is the byte over 255 as a 32-bit float;
  reading it back takes `round(v × 255)`. A blit to an 8-bit swapchain rounds the same way.
- **A frame recorded from part-way down** (`firstLine` above 1) draws its rows from there; the
  rows above hold what the texture held.

## See also

- [ppu.md §The picture as a timeline](ppu.md#the-picture-as-a-timeline) — what a frame's timeline
  holds, the records' words.
- [snes-machine.md §Building a machine](snes-machine.md#building-a-machine) — the option beside
  the two threads.
- [tools/player/README.md](../tools/player/README.md) — `--gpu-ppu`, `--gpu-verify`,
  `snes_gpu_verify` and the player's SDL device.
- [build-and-consume.md](build-and-consume.md) — the shader tools and the build option.
