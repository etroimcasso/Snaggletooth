// The GPU PPU's first pass: Time's sprite walk, once a recorded line. One thread a
// line writes the line's sprite buffer into the scratch buffer: Time's own walk over
// a gathered line's inputs, nothing for a line whose Time gathered nothing, and the
// carried buffer for a carried line.
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

[numthreads(8, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  readHeader();
  uint lineIndex = id.x;
  if (lineIndex + gFirstLine > gHeight) return;
  readLine(lineIndex);
  uint words[64];
  uint priorities[64];
  uint source = lineWord(22);
  if (source == 1u) {
    for (uint i = 0u; i < 64u; ++i) { words[i] = 0u; priorities[i] = 0x04040404u; }
  } else if (source == 2u) {
    uint at = gBuffersAt + lineWord(23) * 128u;
    for (uint i = 0u; i < 64u; ++i) { words[i] = word(at + i); priorities[i] = word(at + 64u + i); }
  } else {
    timeSprites(words, priorities);
  }
  uint slot = spriteSlot(lineIndex);
  for (uint i = 0u; i < 64u; ++i) {
    scratch.Store(slot + i * 4u, words[i]);
    scratch.Store(slot + 256u + i * 4u, priorities[i]);
  }
}
