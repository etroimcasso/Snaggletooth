// The GPU PPU's third pass: every pixel of a frame, one thread a position of a
// recorded line, drawn from the registers at its own dot, the tile records and the
// sprite buffer the first two passes left, by the chip's own rules — the two
// screens resolved by the mode's chart, the windows, color math and the converter.
// A position on a line drawn in half-pixels is drawn after the position to its
// left, whose main-screen decision its left half is drawn under; one body of code
// draws both, in turn.
//
// The output is a texture of four floats a half-pixel, red, green, blue and 1,
// each channel the converter's byte over 255; a reader rounds each back to its
// byte. A frame 512 wide puts position x's left half at column 2x and its right at
// 2x + 1; a frame 256 wide puts the position's one pixel at column x. Picture line
// L lands on texture row (L - 1) * rowStep + rowBase, and on the row after it
// too where rowCopies is 2, which is how an interlaced field is placed in the
// woven picture.
//
// Bound as SDL's compute convention has it: the frame at t0 of space 0, the
// picture at u0 and the scratch buffer at u1 of space 1, and the present mapping
// at b0 of space 2.

#include "ppu_common.hlsl"

RWTexture2D<float4> picture : register(u0, space1);
RWByteAddressBuffer scratch : register(u1, space1);
cbuffer Present : register(b0, space2) {
  uint rowStep;
  uint rowBase;
  uint rowCopies;
  uint presentPad;
};

static uint gLineIndex;

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

// What a layer shows at a position, packed in one word: bit 0 present, bits 1 to 8
// the palette word, bit 9 the tile's priority, bit 10 a color read directly, bits
// 11 to 25 that color. Zero is nothing shown.
uint packShown(bool present, uint word, bool priority, bool direct, uint color) {
  if (!present) return 0u;
  return 1u | (word << 1) | ((priority ? 1u : 0u) << 9) | ((direct ? 1u : 0u) << 10) | (color << 11);
}

// A tilemap background's pixel at position x from its column's records, `oddHalf`
// naming which half of the position a two-character tile is read for, with the
// mode's palette terms and $2130's direct-color bit as they stand in `r`.
uint sampleBackground(Regs r, uint layer, uint x, bool oddHalf) {
  Background b = describedAt(r, layer);
  uint column = x / 8u;
  uint fine = (scratch.Load(tileRecord(gLineIndex, layer, column)) >> 26) & 7u;
  uint along = x % 8u + fine;
  uint readColumn = along < 8u ? column : column + 1u;
  uint inTile = along < 8u ? along : along - 8u;
  uint at = tileRecord(gLineIndex, layer, readColumn);
  uint head = scratch.Load(at);
  uint planes = (head >> 16) & 0xFFu;
  if (planes == 0u) return 0u;
  bool hires = ((head >> 25) & 1u) != 0u;
  uint p = hires ? 2u * inTile + (oddHalf ? 1u : 0u) : inTile;
  uint index = (scratch.Load(at + 8u + (p / 4u) * 4u) >> (8u * (p % 4u))) & 0xFFu;
  if (index == 0u) return 0u;
  uint entry = head & 0xFFFFu;
  uint palette = (entry >> 10) & 7u;
  uint word = (b.wordBase + palette * b.paletteStride + index) & 0xFFu;
  bool direct = planes == 8u && (reg(r, F_CGWSEL) & 1u) != 0u;
  return packShown(true, word, (entry & 0x2000u) != 0u, direct, direct ? directColor(index, palette) : 0u);
}

// The sprite buffer's word and priority at position x, as the first pass left them.
void spriteAt(uint x, out uint spriteWord, out uint spritePriority) {
  uint slot = spriteSlot(gLineIndex);
  spriteWord = (scratch.Load(slot + (x / 4u) * 4u) >> (8u * (x % 4u))) & 0xFFu;
  spritePriority = (scratch.Load(slot + 256u + (x / 4u) * 4u) >> (8u * (x % 4u))) & 0xFFu;
}

// Mode 7's field at a position for one of its two layers, as sampleField finds it.
uint sampleFieldPacked(Regs r, uint upTo, uint layer, uint x, uint lineNo) {
  Shown s = sampleField(r, upTo, layer, x, lineNo);
  return packShown(s.present, s.word, s.priority, s.direct, s.color);
}

// A layer's sample at a position for one screen: nothing where the screen does not
// enable the layer or the windows mask it there; else the field for Mode 7's two
// layers, or the tilemap background through the mosaic's column.
uint sampleFor(Regs r, uint upTo, uint layer, uint x, bool oddHalf, bool field, uint enables, uint maskRegister, uint covers) {
  if ((enables & (1u << layer)) == 0u) return 0u;
  if ((maskRegister & (1u << layer)) != 0u && (covers & (1u << layer)) != 0u) return 0u;
  uint column;
  bool columnHalf;
  mosaicColumn(r, layer, x, oddHalf, column, columnHalf);
  if (field) {
    if (layer >= 2u) return 0u;
    return sampleFieldPacked(r, upTo, layer, column, readLineOf(r, layer));
  }
  return sampleBackground(r, layer, column, columnHalf);
}

// A lane of a four-layer vector by a number that may not be a constant, read and
// written through a switch so the vector stays in registers.
uint lane(uint4 v, uint layer) {
  switch (layer) {
    case 0u: return v.x; case 1u: return v.y; case 2u: return v.z; default: return v.w;
  }
}
void setLane(inout uint4 v, uint layer, uint value) {
  switch (layer) {
    case 0u: v.x = value; break; case 1u: v.y = value; break; case 2u: v.z = value; break; default: v.w = value; break;
  }
}

// The front-most pixel of one screen by the mode's chart, from the four layers'
// samples for that screen and the sprite where the screen shows it: the packed
// sample of the layer the chart reaches first, with the sprite's as bit 0, its word
// in bits 1 to 8 and layer 4 in bits 26 to 28; a layer's own layer number stands in
// the same bits; zero where nothing stands and the screen shows its backdrop.
uint resolveFrom(uint chart, uint4 samples, bool spriteOn, uint spriteWord, uint spritePriority) {
  uint2 span = chartSpan(chart);
  [loop] for (uint i = 0u; i < span.y; ++i) {
    uint place = chartPlace(span.x + i);
    uint layer = place & 0xFu;
    uint priority = place >> 4;
    if (layer == 4u) {
      if (spriteOn && spritePriority == priority) return 1u | (spriteWord << 1) | (4u << 26);
      continue;
    }
    uint shown = lane(samples, layer);
    if (shown != 0u && (((shown >> 9) & 1u) != 0u) == (priority != 0u)) return shown | (layer << 26);
  }
  return 0u;
}

uint colorOfPacked(uint dot, uint shown) {
  if (shown == 0u) return paletteColor(dot, 0u);
  if (((shown >> 10) & 1u) != 0u) return (shown >> 11) & 0x7FFFu;
  return paletteColor(dot, (shown >> 1) & 0xFFu);
}

// One position drawn: `blank` under forced blank at its dot; else the main screen's
// pixel under color math as `mainColor` with its decision, and the sub screen's
// front-most color as `subColor` with whether the line is in half-pixels there.
void drawPosition(uint x, out bool blank, out bool hires, out uint mainColor, out uint subColor, out Decision decision) {
  uint dot = 22u + x;
  Regs r = regsAt(dot);
  blank = forcedBlank(r);
  hires = hiresAt(r);
  mainColor = 0u;
  subColor = 0u;
  decision.present = false; decision.clipped = false; decision.addend = 0u; decision.subtract = false; decision.halved = false; decision.color = 0u;
  if (blank) return;
  uint upTo = writesUpTo(dot);
  bool field = modeOf(r) == 7u;
  uint tm = reg(r, F_TM);
  uint ts = reg(r, F_TS);
  uint tmw = reg(r, F_TMW);
  uint tsw = reg(r, F_TSW);
  uint cgwsel = reg(r, F_CGWSEL);
  uint cgadsub = reg(r, F_CGADSUB);
  // The windows at this position, a bit a layer, the color window's at bit 5.
  uint covers = 0u;
  [loop] for (uint layer = 0u; layer < 6u; ++layer) {
    if (windowCovers(r, layer, x)) covers |= 1u << layer;
  }
  // The four layers for each screen: the sub screen reads the left half of a
  // two-character tile, the main screen the right where the line is in half-pixels.
  uint4 forMain = uint4(0u, 0u, 0u, 0u);
  uint4 forSub = uint4(0u, 0u, 0u, 0u);
  [loop] for (uint layer = 0u; layer < 4u; ++layer) {
    setLane(forSub, layer, sampleFor(r, upTo, layer, x, false, field, ts, tsw, covers));
    setLane(forMain, layer, sampleFor(r, upTo, layer, x, hires, field, tm, tmw, covers));
  }
  uint spriteWord;
  uint spritePriority;
  spriteAt(x, spriteWord, spritePriority);
  bool hasSprites = lineWord(22) != 1u;
  bool spriteMain = (tm & 0x10u) != 0u && hasSprites && !((tmw & 0x10u) != 0u && (covers & 0x10u) != 0u);
  bool spriteSub = (ts & 0x10u) != 0u && hasSprites && !((tsw & 0x10u) != 0u && (covers & 0x10u) != 0u);
  uint chart = chartOf(r);
  uint mainShown = resolveFrom(chart, forMain, spriteMain, spriteWord, spritePriority);
  uint subShown = resolveFrom(chart, forSub, spriteSub, spriteWord, spritePriority);
  subColor = colorOfPacked(dot, subShown);

  // The main pixel under color math: clipped to black by $2130's upper region
  // before any arithmetic; math where $2131 names the pixel's layer, except for a
  // sprite from a palette below 4, and $2130's lower region leaves it; the addend
  // the fixed color, or the sub screen's pixel — its backdrop being the fixed color
  // not halved.
  uint colorRegion = (cgwsel >> 6) & 3u;
  uint mathRegion = (cgwsel >> 4) & 3u;
  bool inColorWindow = (covers & 0x20u) != 0u;
  bool clipped = colorRegion == 3u || (colorRegion == 1u && !inColorWindow) || (colorRegion == 2u && inColorWindow);
  bool mathBarred = mathRegion == 3u || (mathRegion == 1u && !inColorWindow) || (mathRegion == 2u && inColorWindow);
  uint shownLayer = mainShown == 0u ? 5u : (mainShown >> 26) & 7u;
  uint before = clipped ? 0u : colorOfPacked(dot, mainShown);
  decision.present = true;
  decision.clipped = clipped;
  decision.color = before;
  bool spriteRefuses = mainShown != 0u && shownLayer == 4u && ((mainShown >> 1) & 0xFFu) < 192u;
  bool maths = (cgadsub & (1u << shownLayer)) != 0u && !spriteRefuses && !mathBarred;
  if (!maths) {
    mainColor = before;
    return;
  }
  bool fromSubScreen = (cgwsel & 2u) != 0u;
  bool subBackdrop = subShown == 0u;
  uint addend = fromSubScreen && !subBackdrop ? subColor : fixedColor(r);
  bool subtract = (cgadsub & 0x80u) != 0u;
  bool halve = (cgadsub & 0x40u) != 0u && !clipped && !(fromSubScreen && subBackdrop);
  decision.addend = (fromSubScreen && !subBackdrop) ? 2u : 1u;
  decision.subtract = subtract;
  decision.halved = halve;
  mainColor = combine(before, addend, subtract, halve);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  readHeader();
  uint x = id.x;
  uint lineNo = id.y + 1u;
  if (x >= 256u || lineNo < gFirstLine || lineNo > gHeight) return;
  gLineIndex = lineNo - gFirstLine;
  readLine(gLineIndex);
  // The position to the left first where this one is in half-pixels, for the
  // decision its left half is drawn under; then this position.
  Regs atDot = regsAt(22u + x);
  bool drawLeft = x != 0u && !forcedBlank(atDot) && hiresAt(atDot);
  Decision before;
  before.present = false; before.clipped = false; before.addend = 0u; before.subtract = false; before.halved = false; before.color = 0u;
  bool blank = false;
  bool hires = false;
  uint mainColor = 0u;
  uint subColor = 0u;
  Decision decision;
  [loop] for (uint pass = drawLeft ? 0u : 1u; pass < 2u; ++pass) {
    uint position = pass == 0u ? x - 1u : x;
    drawPosition(position, blank, hires, mainColor, subColor, decision);
    if (pass == 0u) before = decision;
  }
  float4 left;
  float4 right;
  if (blank) {
    left = float4(0.0, 0.0, 0.0, 1.0);
    right = left;
  } else if (!hires) {
    right = convert(atDot, mainColor);
    left = right;
  } else {
    // The left half: the sub screen's color under the decision the main pixel to the
    // left made — black where it was clipped, then its math with its addend, the fixed
    // color as it stands now or that pixel's own color.
    uint leftColor = subColor;
    if (before.present) {
      if (before.clipped) leftColor = 0u;
      if (before.addend != 0u) {
        uint addend = before.addend == 1u ? fixedColor(atDot) : before.color;
        leftColor = combine(leftColor, addend, before.subtract, before.halved);
      }
    }
    left = convert(atDot, leftColor);
    right = convert(atDot, mainColor);
  }
  uint row = (lineNo - 1u) * rowStep + rowBase;
  if (gWidth == 512u) {
    picture[uint2(2u * x, row)] = left;
    picture[uint2(2u * x + 1u, row)] = right;
    if (rowCopies == 2u) {
      picture[uint2(2u * x, row + 1u)] = left;
      picture[uint2(2u * x + 1u, row + 1u)] = right;
    }
  } else {
    picture[uint2(x, row)] = right;
    if (rowCopies == 2u) picture[uint2(x, row + 1u)] = right;
  }
}
