#pragma once

// The GPU PPU: the picture drawn on a GPU from each frame's timeline by the core's
// own compute shader, every pixel the one the dot renderer draws. A host switches
// it on by handing SnesConfig::gpuPpu a Device — its own implementation of the few
// operations the dispatch needs, over whatever GPU API it has — and the machine
// then packs every finished frame's timeline into the device's buffer and has it
// drawn, at the end of the cycle in which the frame's vertical blank begins, on
// the thread the machine is stepped on. The shader, the packed layout and the
// dispatch are the core's; the device, the texture it holds and how the picture
// is presented are the host's. The core links no GPU API.
//
// The shader is three passes, the work divided as the chip divides it
// (`shaders/ppu_sprites.comp.hlsl`, `ppu_tiles.comp.hlsl`, `ppu_pixels.comp.hlsl`,
// over `ppu_common.hlsl`): Sprites runs Time's walk once a recorded line into a
// scratch buffer; Tiles reads each tile column of each background once, at the
// dots the chip reads it, into the same buffer; Pixels draws every position from
// those records and the registers at its own dot into the texture. Each pass is a
// compute pipeline of its own; passLayout() gives each one's bindings, and a host
// dispatches the three in order for every frame, the scratch buffer bound
// read-write to all three.
//
// What a Device implements:
//
//   formats()       the formats its device takes, as ShaderFormat bits;
//   makePipeline()  a compute pipeline for one pass from the bytes the core selects
//                   for one of those formats, bound as passLayout(pass) says, with
//                   kGroupSide by kGroupSide by 1 threads a group for Tiles and
//                   Pixels and kGroupSide by 1 by 1 for Sprites;
//   frameBytes()    a span of the asked length the core packs the frame into — the
//                   device's upload buffer, mapped and grown to fit;
//   draw()          the packed frame uploaded, then the three passes dispatched in
//                   order with groupsFor(pass, frame) groups each, the frame buffer
//                   bound read-only, a scratch buffer of kScratchBytes bound
//                   read-write, and for Pixels the texture and the uniform; then
//                   the work submitted.
//
// The uniform is the row mapping, a RowMap: picture line L lands on texture row
// (L - 1) * step + base, and on the row after it too where `copies` is 2, which is
// how a host places an interlaced field in the woven picture; rowMapFor gives the
// three placements. The texture persists between frames, so a weave keeps the
// other parity's rows. The texture's texel at column c of a row is the half-pixel
// the dot renderer draws there (`PpuTimeline::draw`), each channel the byte over
// 255: a host reading the texture back rounds each channel to its byte.
//
// The format the shader's bytes come in is the build's: `shaders/gen_shader.cmake`
// compiles each pass for the platform the library is built on, and ppuShader() is
// those bytes. A library built with SNAGGLETOOTH_GPU_PPU off has no bytes, and a
// machine given a device throws from its constructor.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include "snaggletooth/gpu/shader_format.h"

namespace snaggletooth::gpu {

// The texture the shader draws into: 512 half-pixels across, and as many rows as a
// woven picture of the taller field has, twice 239.
inline constexpr unsigned kTextureWidth = 512u;
inline constexpr unsigned kTextureHeight = 478u;
// The threads a dispatch group has on each axis it spans, as the shaders declare.
inline constexpr unsigned kGroupSide = 8u;
// The uniform buffer's length: a RowMap's three words and one of padding.
inline constexpr std::size_t kUniformBytes = 16u;

// The scratch buffer the passes hand each other: each recorded line's sprite
// buffer, 512 bytes — the palette word at each of the 256 positions, then the
// priority at each — at the line's index times 512, for up to 240 lines; then each
// line's tile records, one 32-byte record a background a column, 33 columns, at
// kTileRecordsAt + ((line * 4 + layer) * 33 + column) * 32. A record's first word
// is the tilemap entry in its low half, the depth in bits 16 to 23, the tile-size
// bit at 24, the two-character bit at 25 and the fine scroll from bit 26; its
// second is the background position, X then Y; its next four are the sixteen
// color indices a byte each; its last two are zero.
inline constexpr std::size_t kSpriteLineBytes = 512u;
inline constexpr std::size_t kTileRecordBytes = 32u;
inline constexpr std::size_t kTileRecordsAt = 240u * kSpriteLineBytes;
inline constexpr std::size_t kScratchBytes = kTileRecordsAt + 240u * 4u * 33u * kTileRecordBytes;

// The three passes, in the order they are dispatched.
enum class Pass : std::uint8_t { Sprites, Tiles, Pixels };

// What each pass binds, in SDL's compute convention: read-only storage buffers,
// then read-write storage textures and buffers, then uniform buffers. Every pass
// reads the frame as its one read-only buffer and the scratch buffer as a
// read-write one; Pixels alone writes the texture and reads the uniform.
struct PassLayout {
  unsigned readOnlyBuffers;
  unsigned readWriteTextures;
  unsigned readWriteBuffers;
  unsigned uniformBuffers;
  unsigned threadsX;
  unsigned threadsY;
};
[[nodiscard]] constexpr PassLayout passLayout(Pass pass) noexcept {
  switch (pass) {
    case Pass::Sprites: return PassLayout{1u, 0u, 1u, 0u, kGroupSide, 1u};
    case Pass::Tiles: return PassLayout{1u, 0u, 1u, 0u, kGroupSide, kGroupSide};
    case Pass::Pixels: break;
  }
  return PassLayout{1u, 1u, 1u, 1u, kGroupSide, kGroupSide};
}

// Where a frame's picture lines land in the texture's rows.
struct RowMap {
  std::uint32_t step = 1;
  std::uint32_t base = 0;
  std::uint32_t copies = 1;
  [[nodiscard]] bool operator==(const RowMap&) const noexcept = default;
};

// How an interlaced run's fields are placed: Weave interleaves the two parities
// into the full-height picture, each field at the rows its parity owns; Bob doubles
// each field to full height on its own; Field shows the field as the machine drew
// it, half height. A run the cartridge did not interlace is placed line for line
// whatever the placement.
enum class FieldPlacement : std::uint8_t { Weave, Bob, Field };

[[nodiscard]] constexpr RowMap rowMapFor(FieldPlacement placement, bool interlaced,
                                         std::uint8_t field) noexcept {
  if (!interlaced || placement == FieldPlacement::Field) return RowMap{1u, 0u, 1u};
  if (placement == FieldPlacement::Weave) return RowMap{2u, static_cast<std::uint32_t>(field & 1u), 1u};
  return RowMap{2u, 0u, 2u};
}

// The rows the texture holds a frame of `height` picture lines in under a map.
[[nodiscard]] constexpr unsigned rowsUnder(RowMap rows, unsigned height) noexcept {
  return rows.step == 1u ? height : 2u * height;
}

// The uniform's words as the shader reads them.
struct Uniform {
  std::uint32_t rowStep;
  std::uint32_t rowBase;
  std::uint32_t rowCopies;
  std::uint32_t pad;
};
[[nodiscard]] constexpr Uniform uniformFor(RowMap rows) noexcept {
  return Uniform{rows.step, rows.base, rows.copies, 0u};
}

// A frame the core has packed into the device's buffer and asks it to draw: its
// shape, whether the cartridge has interlace on, and the packed length.
struct Frame {
  unsigned width = 0;        // 256 or 512 columns
  unsigned height = 0;       // the picture lines, 224 or 239
  unsigned firstLine = 0;    // the first line recorded; rows above it are not drawn
  std::uint8_t field = 0;    // the parity the frame ran under
  bool interlaced = false;   // $2133 bit 0 as the frame was reported
  std::size_t bytes = 0;     // the packed frame's length, from the span's start
};

// The groups a pass's dispatch takes for a frame: Sprites one thread a recorded
// line, Tiles one a column a line a background, Pixels one a position a row.
struct Groups {
  unsigned x;
  unsigned y;
  unsigned z;
};
[[nodiscard]] constexpr Groups groupsFor(Pass pass, const Frame& frame) noexcept {
  const unsigned lines = frame.height + 1u - frame.firstLine;
  const unsigned lineGroups = (lines + kGroupSide - 1u) / kGroupSide;
  switch (pass) {
    case Pass::Sprites: return Groups{lineGroups, 1u, 1u};
    case Pass::Tiles: return Groups{(33u + kGroupSide - 1u) / kGroupSide, lineGroups, 4u};
    case Pass::Pixels: break;
  }
  return Groups{256u / kGroupSide, (frame.height + kGroupSide - 1u) / kGroupSide, 1u};
}

// The device a host hands the machine. Every call is made on the thread the
// machine is stepped on, from inside step() or run(); makePipeline once a pass,
// from the machine's constructor, then frameBytes and draw once a frame, in that
// order.
class Device {
 public:
  virtual ~Device() = default;

  [[nodiscard]] virtual ShaderFormat formats() const = 0;
  virtual bool makePipeline(Pass pass, const ShaderBytecode& code, ShaderFormat format, std::string& error) = 0;
  [[nodiscard]] virtual std::span<std::uint8_t> frameBytes(std::size_t bytes) = 0;
  virtual void draw(const Frame& frame) = 0;
};

// A pass's bytes as the build compiled them, in the slot of the platform's format
// and empty in the other two; three empty slots on a library built with
// SNAGGLETOOTH_GPU_PPU off.
[[nodiscard]] const ShaderVariants& ppuShader(Pass pass) noexcept;

}  // namespace snaggletooth::gpu
