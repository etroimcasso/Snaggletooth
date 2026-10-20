// The GPU PPU's second pass: the tile reads, once a tile column a background a
// recorded line. One thread a column reads the column as the chip does — the
// tilemap entry at dot 8E + 3, the character rows at 8E + 7 and the fine scroll at
// 8E + 22, each under the registers and VRAM as they stand at that dot, the
// registers carried from one read's dot to the next over the line's register
// events — and writes the column's record into the scratch buffer for the pixels
// to draw from.
//
// Bound as SDL's compute convention has it: the frame at t0 of space 0 and the
// scratch buffer at u0 of space 1.

#include "ppu_common.hlsl"

RWByteAddressBuffer scratch : register(u0, space1);

// The scratch buffer the passes hand each other (`gpu_ppu.h` §The scratch buffer):
// each recorded line's sprite buffer, 512 bytes — the palette word at each of the
// 256 positions, then the priority at each — at the line's index times 512; then
// each line's tile records, one 32-byte record a layer a column, at
// kTileRecordsAt + ((line * 4 + layer) * 33 + column) * 32: the entry with the depth,
// the size, the width and the fine scroll above it, the background position, and the
// sixteen color indices.
static const uint kTileRecordsAt = 240u * 512u;
uint spriteSlot(uint lineIndex) { return lineIndex * 512u; }
uint tileRecord(uint lineIndex, uint layer, uint column) { return kTileRecordsAt + ((lineIndex * 4u + layer) * 33u + column) * 32u; }

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  readHeader();
  uint column = id.x;
  uint lineIndex = id.y;
  uint layer = id.z;
  if (column >= 33u || lineIndex + gFirstLine > gHeight || layer >= 4u) return;
  readLine(lineIndex);
  Regs r = lineRegs();
  uint cursor = gRegisterFirst;
  regsOn(r, cursor, column * 8u + 3u);
  Tile t = entryRead(r, layer, column);
  regsOn(r, cursor, column * 8u + 7u);
  uint2 first;
  uint2 second;
  characterRows(r, layer, column, t, first, second);
  regsOn(r, cursor, column * 8u + 22u);
  uint fine = fineOf(r, layer, column);
  uint at = tileRecord(lineIndex, layer, column);
  scratch.Store(at, (t.entry & 0xFFFFu) | (t.planes << 16) | ((t.large ? 1u : 0u) << 24) | ((t.hires ? 1u : 0u) << 25) | (fine << 26));
  scratch.Store(at + 4u, (t.bgX & 0xFFFFu) | ((t.bgY & 0xFFFFu) << 16));
  scratch.Store(at + 8u, first.x);
  scratch.Store(at + 12u, first.y);
  scratch.Store(at + 16u, second.x);
  scratch.Store(at + 20u, second.y);
  scratch.Store(at + 24u, 0u);
  scratch.Store(at + 28u, 0u);
}
