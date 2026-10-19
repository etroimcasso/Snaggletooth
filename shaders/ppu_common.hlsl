// The picture processor's dot path on the GPU, the part the three passes share:
// the packed frame, the registers as they stand at a dot, VRAM as of a dot, what the
// mode makes of each background, the tile reads, Mode 7's field, Time's sprite walk,
// the windows and the charts. `ppu_sprites.comp.hlsl`, `ppu_tiles.comp.hlsl` and
// `ppu_pixels.comp.hlsl` include it and each draws one stage of a frame, the way the
// chip does the work: the sprites of a line once as the line begins, each tile
// column's reads once at their dots, and every pixel from those.
//
// The frame is PpuTimeline's packed form (`snaggletooth/snes/ppu_timeline.h`,
// `docs/gpu-ppu.md`): a header of offsets, then the lines, the events, VRAM, the
// palette and sprite-table versions, the carried sprite buffers, and the VRAM writes
// in position order and again sorted by address behind a page table and a line
// table. The registers at a dot are the line's base words with the line's events up
// to that dot applied; the byte at a VRAM address as of a dot is found through the
// sorted writes. Nothing is carried from thread to thread but through the scratch
// buffer the passes hand each other (`gpu_ppu.h` gives its layout).
//
// The frame is bound at t0 of space 0 in every pass; the passes declare the rest.

ByteAddressBuffer frame : register(t0, space0);

// ---- the packed frame ----------------------------------------------------------

uint word(uint index) { return frame.Load(index * 4u); }
uint byteAt(uint offset) { return (frame.Load(offset & ~3u) >> (8u * (offset & 3u))) & 0xFFu; }

// The header, read once a thread.
static uint gHeight, gWidth, gFirstLine, gField;
static uint gLinesAt, gEventsAt, gVramAt, gPalettesAt, gOamsAt, gBuffersAt;
static uint gWritesAt, gWriteCount, gSortedAt, gPagesAt, gLineTableAt;

// The thread's line: where its 32 words stand, its number, and the ordinals of the
// frame's VRAM writes that stand before it and before the line after it. The words
// are read from the frame as they are needed, where every thread of the line finds
// them in the cache, rather than copied into an array a thread keeps in its own
// memory.
static uint gLineAt;
static uint gLine;
static uint gWritesBeforeLine;
static uint gWritesBeforeNext;

uint lineWord(uint index) { return word(gLineAt + index); }

void readHeader() {
  gHeight = word(0u);
  gWidth = word(1u);
  gFirstLine = word(2u);
  gField = word(3u);
  gLinesAt = word(4u);
  gEventsAt = word(6u);
  gVramAt = word(8u);
  gPalettesAt = word(9u);
  gOamsAt = word(11u);
  gBuffersAt = word(13u);
  gWritesAt = word(15u);
  gWriteCount = word(16u);
  gSortedAt = word(17u);
  gPagesAt = word(18u);
  gLineTableAt = word(19u);
}

void readLine(uint lineIndex) {
  gLineAt = gLinesAt + lineIndex * 32u;
  gLine = lineWord(21u);
  gWritesBeforeLine = word(gLineTableAt + gLine);
  gWritesBeforeNext = gLine + 1u < 240u ? word(gLineTableAt + gLine + 1u) : gWriteCount;
}

// ---- the registers at a dot ----------------------------------------------------

// A Field's place in a line's words 0 to 15: the word, the bit it begins at and its
// width, in PpuTimeline::Field's order. A switch rather than a table: a constant
// table the shader indexes is built in every thread's own memory by the Metal
// compiler, where a switch folds to its constant for a constant Field and to a
// select for an event's.
uint3 placeOf(uint field) {
  switch (field) {
    case 0u: return uint3(0u, 0u, 8u);
    case 1u: return uint3(0u, 8u, 8u);
    case 2u: return uint3(0u, 16u, 8u);
    case 3u: return uint3(0u, 24u, 8u);
    case 4u: return uint3(1u, 0u, 8u);
    case 5u: return uint3(1u, 8u, 8u);
    case 6u: return uint3(1u, 16u, 8u);
    case 7u: return uint3(1u, 24u, 8u);
    case 8u: return uint3(2u, 0u, 8u);
    case 9u: return uint3(2u, 8u, 8u);
    case 10u: return uint3(2u, 16u, 8u);
    case 11u: return uint3(2u, 24u, 8u);
    case 12u: return uint3(3u, 0u, 16u);
    case 13u: return uint3(3u, 16u, 16u);
    case 14u: return uint3(4u, 0u, 16u);
    case 15u: return uint3(4u, 16u, 16u);
    case 16u: return uint3(5u, 0u, 16u);
    case 17u: return uint3(5u, 16u, 16u);
    case 18u: return uint3(6u, 0u, 16u);
    case 19u: return uint3(6u, 16u, 16u);
    case 20u: return uint3(7u, 0u, 16u);
    case 21u: return uint3(7u, 16u, 16u);
    case 22u: return uint3(8u, 0u, 16u);
    case 23u: return uint3(8u, 16u, 16u);
    case 24u: return uint3(9u, 0u, 16u);
    case 25u: return uint3(9u, 16u, 16u);
    case 26u: return uint3(10u, 0u, 16u);
    case 27u: return uint3(10u, 16u, 16u);
    case 28u: return uint3(11u, 0u, 8u);
    case 29u: return uint3(11u, 8u, 8u);
    case 30u: return uint3(11u, 16u, 8u);
    case 31u: return uint3(11u, 24u, 8u);
    case 32u: return uint3(12u, 0u, 8u);
    case 33u: return uint3(12u, 8u, 8u);
    case 34u: return uint3(12u, 16u, 8u);
    case 35u: return uint3(12u, 24u, 8u);
    case 36u: return uint3(13u, 0u, 8u);
    case 37u: return uint3(13u, 8u, 8u);
    case 38u: return uint3(13u, 16u, 8u);
    case 39u: return uint3(13u, 24u, 8u);
    case 40u: return uint3(14u, 0u, 8u);
    case 41u: return uint3(14u, 8u, 8u);
    case 42u: return uint3(14u, 16u, 8u);
    case 43u: return uint3(14u, 24u, 8u);
    case 44u: return uint3(15u, 0u, 8u);
    default: return uint3(0u, 0u, 0u);
  }
}

static const uint F_INIDISP = 0u, F_BGMODE = 1u, F_MOSAIC = 2u, F_SETINI = 3u;
static const uint F_BG1SC = 4u, F_BG2SC = 5u, F_BG3SC = 6u, F_BG4SC = 7u;
static const uint F_BG12NBA = 8u, F_BG34NBA = 9u, F_M7SEL = 10u, F_CGWSEL = 11u;
static const uint F_BG1HOFS = 12u, F_BG1VOFS = 13u;  // then BG2's, BG3's and BG4's pairs
static const uint F_M7A = 20u, F_M7B = 21u, F_M7C = 22u, F_M7D = 23u;
static const uint F_M7X = 24u, F_M7Y = 25u, F_M7HOFS = 26u, F_M7VOFS = 27u;
static const uint F_W12SEL = 28u, F_W34SEL = 29u, F_WOBJSEL = 30u, F_WBGLOG = 31u;
static const uint F_WOBJLOG = 32u, F_WH0 = 33u, F_WH1 = 34u, F_WH2 = 35u;
static const uint F_WH3 = 36u, F_TM = 37u, F_TS = 38u, F_TMW = 39u;
static const uint F_TSW = 40u, F_CGADSUB = 41u, F_FIXEDRED = 42u, F_FIXEDGREEN = 43u;
static const uint F_FIXEDBLUE = 44u, F_PALETTEWORD = 45u;

// The sixteen register words as named scalars, so none is ever an array a thread
// keeps in its own memory.
struct Regs {
  uint w0, w1, w2, w3, w4, w5, w6, w7, w8, w9, w10, w11, w12, w13, w14, w15;
};

// A word of the registers by a number that may not be a constant, read and
// written through a switch.
uint regWord(Regs r, uint index) {
  switch (index) {
    case 0u: return r.w0; case 1u: return r.w1; case 2u: return r.w2; case 3u: return r.w3;
    case 4u: return r.w4; case 5u: return r.w5; case 6u: return r.w6; case 7u: return r.w7;
    case 8u: return r.w8; case 9u: return r.w9; case 10u: return r.w10; case 11u: return r.w11;
    case 12u: return r.w12; case 13u: return r.w13; case 14u: return r.w14; default: return r.w15;
  }
}
void setRegWord(inout Regs r, uint index, uint value) {
  switch (index) {
    case 0u: r.w0 = value; break; case 1u: r.w1 = value; break; case 2u: r.w2 = value; break;
    case 3u: r.w3 = value; break; case 4u: r.w4 = value; break; case 5u: r.w5 = value; break;
    case 6u: r.w6 = value; break; case 7u: r.w7 = value; break; case 8u: r.w8 = value; break;
    case 9u: r.w9 = value; break; case 10u: r.w10 = value; break; case 11u: r.w11 = value; break;
    case 12u: r.w12 = value; break; case 13u: r.w13 = value; break; case 14u: r.w14 = value; break;
    default: r.w15 = value; break;
  }
}

// A register's value by Field; every caller names the Field as a constant, so
// the place folds to a constant and the word is a register.
uint reg(Regs r, uint field) {
  uint3 place = placeOf(field);
  uint mask = (1u << place.z) - 1u;
  return (regWord(r, place.x) >> place.y) & mask;
}

// A background's screen register and its two offsets, the background numbered.
uint bgScreen(Regs r, uint layer) {
  switch (layer) {
    case 0u: return reg(r, F_BG1SC); case 1u: return reg(r, F_BG2SC); case 2u: return reg(r, F_BG3SC); default: return reg(r, F_BG4SC);
  }
}
uint bgHofs(Regs r, uint layer) {
  switch (layer) {
    case 0u: return reg(r, F_BG1HOFS); case 1u: return reg(r, F_BG1HOFS + 2u); case 2u: return reg(r, F_BG1HOFS + 4u); default: return reg(r, F_BG1HOFS + 6u);
  }
}
uint bgVofs(Regs r, uint layer) {
  switch (layer) {
    case 0u: return reg(r, F_BG1VOFS); case 1u: return reg(r, F_BG1VOFS + 2u); case 2u: return reg(r, F_BG1VOFS + 4u); default: return reg(r, F_BG1VOFS + 6u);
  }
}

// The registers the picture reads as they stood at `dot` of the thread's line: the
// line's base words, then every event of the line placed at or before the dot, in
// order. A palette word changes no register.
Regs regsAt(uint dot) {
  Regs r;
  r.w0 = lineWord(0u); r.w1 = lineWord(1u); r.w2 = lineWord(2u); r.w3 = lineWord(3u);
  r.w4 = lineWord(4u); r.w5 = lineWord(5u); r.w6 = lineWord(6u); r.w7 = lineWord(7u);
  r.w8 = lineWord(8u); r.w9 = lineWord(9u); r.w10 = lineWord(10u); r.w11 = lineWord(11u);
  r.w12 = lineWord(12u); r.w13 = lineWord(13u); r.w14 = lineWord(14u); r.w15 = lineWord(15u);
  uint first = lineWord(19u);
  uint count = lineWord(20u);
  for (uint e = 0u; e < count; ++e) {
    uint at = word(gEventsAt + (first + e) * 2u);
    if ((at >> 16) > dot) break;
    uint change = word(gEventsAt + (first + e) * 2u + 1u);
    uint field = change & 0xFFu;
    if (field >= F_PALETTEWORD) continue;
    uint3 place = placeOf(field);
    uint mask = (1u << place.z) - 1u;
    setRegWord(r, place.x, (regWord(r, place.x) & ~(mask << place.y)) | (((change >> 16) & mask) << place.y));
  }
  return r;
}

bool forcedBlank(Regs r) { return (reg(r, F_INIDISP) & 0x80u) != 0u; }
uint modeOf(Regs r) { return reg(r, F_BGMODE) & 7u; }

// The 15-bit color at palette word `index` as it stood at `dot`: the line's
// version, then the last palette write of the line placed at or before the dot.
uint paletteColor(uint dot, uint index) {
  uint version = lineWord(17) >> 16;
  uint pair = word(gPalettesAt + version * 128u + index / 2u);
  uint color = (index & 1u) != 0u ? (pair >> 16) : (pair & 0xFFFFu);
  uint first = lineWord(19);
  uint count = lineWord(20);
  for (uint e = 0u; e < count; ++e) {
    uint at = word(gEventsAt + (first + e) * 2u);
    if ((at >> 16) > dot) break;
    uint change = word(gEventsAt + (first + e) * 2u + 1u);
    if ((change & 0xFFu) == F_PALETTEWORD && ((change >> 8) & 0xFFu) == index) color = change >> 16;
  }
  return color & 0x7FFFu;
}

// ---- VRAM at a dot ---------------------------------------------------------------

// The ordinal past the last of the line's writes placed at or before `dot`: the
// line's first ordinal, stepped past its own writes up to the dot.
uint writesUpTo(uint dot) {
  uint lo = gWritesBeforeLine;
  uint hi = gWritesBeforeNext;
  while (lo < hi) {
    uint mid = (lo + hi) / 2u;
    uint at = word(gWritesAt + mid * 2u);
    if ((at >> 16) <= dot) lo = mid + 1u; else hi = mid;
  }
  return lo;
}

// The byte at `address` once the writes with an ordinal below `upTo` have landed:
// the last of the address's writes in the sorted list below that ordinal, or VRAM's
// own byte.
uint vramByteBefore(uint upTo, uint address) {
  address &= 0xFFFFu;
  uint page = address >> 8;
  uint lo = word(gPagesAt + page);
  uint start = lo;
  uint hi = word(gPagesAt + page + 1u);
  uint target = (address << 16) | upTo;
  while (lo < hi) {
    uint mid = (lo + hi) / 2u;
    uint entry = word(gSortedAt + mid * 2u);
    uint key = ((entry & 0xFFFFu) << 16) | (entry >> 16);
    if (key < target) lo = mid + 1u; else hi = mid;
  }
  if (lo > start) {
    uint entry = word(gSortedAt + (lo - 1u) * 2u);
    if ((entry & 0xFFFFu) == address) return word(gSortedAt + (lo - 1u) * 2u + 1u) & 0xFFu;
  }
  return byteAt(gVramAt * 4u + address);
}

uint vramByteAt(uint dot, uint address) { return vramByteBefore(writesUpTo(dot), address); }
uint vramByteAtLineStart(uint address) { return vramByteBefore(gWritesBeforeLine, address); }

// ---- what the mode makes of a background ----------------------------------------

// A background as its registers and the mode describe it: its screen register, its
// character base, its tile size, whether its tiles are two characters wide, its
// depth and palette terms, and the bit of a BG3 entry that offsets it. A depth of 0
// is a background the mode does not draw from a tilemap.
struct Background {
  uint screen;
  uint characterBase;
  bool large;
  bool hires;
  uint planes;
  uint paletteStride;
  uint wordBase;
  uint offsetBit;
};

Background registersOf(Regs r, uint layer) {
  Background b;
  b.screen = bgScreen(r, layer);
  uint bases = layer < 2u ? reg(r, F_BG12NBA) : reg(r, F_BG34NBA);
  b.characterBase = (layer & 1u) != 0u ? (bases >> 4) : (bases & 0x0Fu);
  b.large = (reg(r, F_BGMODE) & (0x10u << layer)) != 0u;
  uint mode = modeOf(r);
  b.hires = mode == 5u || mode == 6u;
  b.planes = 0u;
  b.paletteStride = 0u;
  b.wordBase = 0u;
  b.offsetBit = 0u;
  return b;
}

Background describedAt(Regs r, uint layer) {
  Background b = registersOf(r, layer);
  uint mode = modeOf(r);
  uint planes = 0u, stride = 0u, base = 0u;
  if (mode == 0u) { planes = 2u; stride = 4u; base = layer * 32u; }
  else if (mode == 1u) { if (layer < 2u) { planes = 4u; stride = 16u; } else if (layer == 2u) { planes = 2u; stride = 4u; } }
  else if (mode == 2u) { if (layer < 2u) { planes = 4u; stride = 16u; } }
  else if (mode == 3u) { if (layer == 0u) { planes = 8u; } else if (layer == 1u) { planes = 4u; stride = 16u; } }
  else if (mode == 4u) { if (layer == 0u) { planes = 8u; } else if (layer == 1u) { planes = 2u; stride = 4u; } }
  else if (mode == 5u) { if (layer == 0u) { planes = 4u; stride = 16u; } else if (layer == 1u) { planes = 2u; stride = 4u; } }
  else if (mode == 6u) { if (layer == 0u) { planes = 4u; stride = 16u; } }
  b.planes = planes;
  b.paletteStride = stride;
  b.wordBase = base;
  bool offsetTable = mode == 2u || mode == 4u || mode == 6u;
  b.offsetBit = offsetTable && planes != 0u ? (0x2000u << layer) : 0u;
  return b;
}

uint tileWidth(Background b) { return (b.hires || !b.large) ? 8u : 16u; }
uint tileHeight(Background b) { return b.large ? 16u : 8u; }

// ---- mosaic and the line a tilemap is read at -----------------------------------

uint mosaicIndex(uint lineNo) {
  uint blockLine = lineWord(15) >> 16;
  return lineNo < blockLine ? 0u : lineNo - blockLine;
}

// The line a layer's tilemap is read at: the beam's own, or the line the current
// row of mosaic blocks began on, and in modes 5 and 6 under interlace a half-line.
uint readLineOf(Regs r, uint layer) {
  uint mode = modeOf(r);
  uint mosaic = reg(r, F_MOSAIC);
  bool down = (mode == 7u && layer == 1u) ? (mosaic & 1u) != 0u : ((mosaic >> layer) & 1u) != 0u;
  bool halfLines = (mode == 5u || mode == 6u) && (reg(r, F_SETINI) & 1u) != 0u;
  uint corner = down ? gLine - mosaicIndex(gLine) : gLine;
  uint at = corner;
  if (halfLines) at = down ? 2u * corner : 2u * gLine + (gField & 1u);
  return at;
}

// The column a layer is read from at a position under mosaic, and which half of the
// position a two-character tile is read for.
void mosaicColumn(Regs r, uint layer, uint x, bool oddHalf, out uint column, out bool columnHalf) {
  uint mode = modeOf(r);
  uint mosaic = reg(r, F_MOSAIC);
  bool across = (mode == 7u && layer == 1u) ? (mosaic & 2u) != 0u : ((mosaic >> layer) & 1u) != 0u;
  uint width = (mosaic >> 4) + 1u;
  if (!across) { column = x; columnHalf = oddHalf; return; }
  if (mode == 5u || mode == 6u) {
    uint halfPixel = 2u * x + (oddHalf ? 1u : 0u);
    uint blockCorner = halfPixel - halfPixel % (2u * width);
    column = blockCorner / 2u;
    columnHalf = false;
    return;
  }
  column = x - x % width;
  columnHalf = oddHalf;
}

// ---- the tile reads ---------------------------------------------------------------

// A tilemap entry at a background's own position, from VRAM once the writes with an
// ordinal below `upTo` have landed.
uint entryAt(uint upTo, Background b, uint bgX, uint bgY) {
  uint tileX = bgX / tileWidth(b);
  uint tileY = bgY / tileHeight(b);
  bool wide = (b.screen & 1u) != 0u;
  bool tall = (b.screen & 2u) != 0u;
  uint w = ((b.screen >> 2) << 10) + ((tileY & 0x1Fu) << 5) + (tileX & 0x1Fu);
  if (tall) w += (tileY & 0x20u) << (wide ? 6u : 5u);
  if (wide) w += (tileX & 0x20u) << 5;
  uint at = (w << 1) & 0xFFFFu;
  return vramByteBefore(upTo, at) | (vramByteBefore(upTo, (at + 1u) & 0xFFFFu) << 8);
}

// The offsets a background's entry read takes at position x: its own registers'
// low ten bits, or through BG3's offset table where the mode gives it one.
void offsetsFor(Regs r, uint upTo, Background b, uint layer, uint x, out uint horizontal, out uint vertical) {
  uint h = bgHofs(r, layer);
  uint v = bgVofs(r, layer);
  horizontal = h & 0x3FFu;
  vertical = v & 0x3FFu;
  if (b.offsetBit == 0u) return;
  uint fine = horizontal & 7u;
  uint column = x + fine;
  if (column < 8u) return;
  Background table = registersOf(r, 2u);
  uint tableH = bgHofs(r, 2u);
  uint tableV = bgVofs(r, 2u);
  uint tableX = (column & ~7u) - 8u + (tableH & 0x3F8u);
  uint tableY = tableV & 0x3FFu;
  uint entryH = entryAt(upTo, table, tableX, tableY);
  uint entryV = 0u;
  if (modeOf(r) == 4u) {
    if ((entryH & 0x8000u) != 0u) { entryV = entryH; entryH = 0u; }
  } else {
    entryV = entryAt(upTo, table, tableX, tableY + 8u);
  }
  if ((entryH & b.offsetBit) != 0u) horizontal = (entryH & 0x3F8u) | fine;
  if ((entryV & b.offsetBit) != 0u) vertical = entryV & 0x3FFu;
}

// What a background's entry read of one tile column stored.
struct Tile {
  uint entry;
  uint bgX;
  uint bgY;
  uint planes;
  bool large;
  bool hires;
};

Tile entryRead(uint layer, uint column) {
  Tile t;
  t.entry = 0u; t.bgX = 0u; t.bgY = 0u; t.planes = 0u; t.large = false; t.hires = false;
  uint dot = column * 8u + 3u;
  Regs r = regsAt(dot);
  if (forcedBlank(r) || modeOf(r) == 7u) return t;
  Background b = describedAt(r, layer);
  if (b.planes == 0u) return t;
  uint x = column * 8u;
  uint lineNo = readLineOf(r, layer);
  uint upTo = writesUpTo(dot);
  uint horizontal, vertical;
  offsetsFor(r, upTo, b, layer, x, horizontal, vertical);
  t.bgX = (x + (horizontal & ~7u)) & 0xFFFFu;
  t.bgY = (lineNo + vertical) & 0xFFFFu;
  t.entry = entryAt(upTo, b, t.bgX, t.bgY);
  t.planes = b.planes;
  t.large = b.large;
  t.hires = b.hires;
  return t;
}

// One character's row as eight color indices, a byte each in a word pair — pixel k
// in byte k, pixels 0 to 3 in the first word — from the row's plane bytes, read
// once each from VRAM once the writes below `upTo` have landed, under the
// horizontal flip. A character takes eight bytes a plane, the planes in pairs
// sixteen bytes apart.
uint2 rowIndices(uint upTo, uint at, uint planes, bool flipX) {
  uint2 packed = uint2(0u, 0u);
  [loop] for (uint plane = 0u; plane < planes; ++plane) {
    uint bits = vramByteBefore(upTo, (at + (plane / 2u) * 16u + (plane % 2u)) & 0xFFFFu);
    for (uint within = 0u; within < 4u; ++within) {
      uint columnInRow = flipX ? 7u - within : within;
      packed.x |= ((bits >> (7u - columnInRow)) & 1u) << (8u * within + plane);
      uint columnInRow2 = flipX ? 3u - within : 4u + within;
      packed.y |= ((bits >> (7u - columnInRow2)) & 1u) << (8u * within + plane);
    }
  }
  return packed;
}

// The color indices of a column's tile, from the character read at dot 8E + 7: the
// character base as it stands then, the row under the vertical flip, the character
// of a 16x16 block's half or of a two-character tile's pair, and the columns under
// the horizontal flip. `first` holds the eight indices of the tile's character, or
// of the first of a two-character tile's pair, and `second` the second's, zero for
// any other tile; both zero under forced blank at that dot, or where the entry read
// stored nothing.
void characterRows(uint layer, uint column, Tile t, out uint2 first, out uint2 second) {
  first = uint2(0u, 0u);
  second = uint2(0u, 0u);
  uint dot = column * 8u + 7u;
  Regs r = regsAt(dot);
  if (forcedBlank(r) || t.planes == 0u) return;
  bool flipX = (t.entry & 0x4000u) != 0u;
  bool flipY = (t.entry & 0x8000u) != 0u;
  uint bases = layer < 2u ? reg(r, F_BG12NBA) : reg(r, F_BG34NBA);
  uint characterBase = (layer & 1u) != 0u ? (bases >> 4) : (bases & 0x0Fu);
  uint height = t.large ? 16u : 8u;
  uint inY = t.bgY % height;
  if (flipY) inY = height - 1u - inY;
  uint tile = t.entry & 0x3FFu;
  if (inY >= 8u) { tile += 16u; inY -= 8u; }
  uint firstCharacter;
  uint secondCharacter;
  if (t.hires) {
    firstCharacter = flipX ? tile + 1u : tile;
    secondCharacter = flipX ? tile : tile + 1u;
  } else {
    bool rightHalf = (t.bgX % 16u) >= 8u;
    bool half = t.large && (rightHalf != flipX);
    firstCharacter = half ? tile + 1u : tile;
    secondCharacter = firstCharacter;
  }
  uint upTo = writesUpTo(dot);
  uint base = characterBase << 13;
  first = rowIndices(upTo, (base + (firstCharacter & 0x3FFu) * 8u * t.planes + inY * 2u) & 0xFFFFu, t.planes, flipX);
  if (t.hires) {
    second = rowIndices(upTo, (base + (secondCharacter & 0x3FFu) * 8u * t.planes + inY * 2u) & 0xFFFFu, t.planes, flipX);
  }
}

// The fine horizontal scroll column E's positions are drawn with, read at dot
// 8E + 22; the thirty-third column reads none.
uint fineOf(uint layer, uint column) {
  if (column >= 32u) return 0u;
  Regs r = regsAt(column * 8u + 22u);
  return bgHofs(r, layer) & 7u;
}

// What a layer shows at a position: the palette word, the tile's priority bit, and
// the color itself where the pixel was read as one.
struct Shown {
  bool present;
  uint word;
  bool priority;
  bool direct;
  uint color;
};

Shown nothingShown() {
  Shown s;
  s.present = false; s.word = 0u; s.priority = false; s.direct = false; s.color = 0u;
  return s;
}

// A 256-color pixel read as a color: BBGGGRRR with the tile's three palette bits.
uint directColor(uint pixel, uint palette) {
  uint red = ((pixel & 7u) << 2) | ((palette & 1u) << 1);
  uint green = (((pixel >> 3) & 7u) << 2) | (palette & 2u);
  uint blue = (((pixel >> 6) & 3u) << 3) | (palette & 4u);
  return red | (green << 5) | (blue << 10);
}

// A tilemap background's pixel at position x, `half` naming which half of the
// position a two-character tile is read for, with the mode's palette terms and

// ---- Mode 7's field ---------------------------------------------------------------

int signed16(uint v) { return (int(v << 16)) >> 16; }
int signed13(uint v) {
  int low = int(v & 0x1FFFu);
  return (low & 0x1000) != 0 ? low - 0x2000 : low;
}
int clippedOffset(int difference) { return (difference & 0x2000) != 0 ? (difference | ~0x3FF) : (difference & 0x3FF); }
int truncated(int product) { return product & ~63; }
int fieldInteger(int coordinate) { return coordinate >> 8; }
bool outsideField(int integer) { return (integer & ~0x3FF) != 0; }

// The field's pixel byte at a picture position and line, through the matrix as the
// registers stand at the pixel's dot and VRAM once the writes below `upTo` have landed.
uint fieldPixelAt(Regs r, uint upTo, uint x, uint lineNo) {
  uint m7sel = reg(r, F_M7SEL);
  int sx = int((m7sel & 1u) != 0u ? (x ^ 0xFFu) : x);
  int sy = int((m7sel & 2u) != 0u ? (lineNo ^ 0xFFu) : lineNo);
  int a = signed16(reg(r, F_M7A));
  int b = signed16(reg(r, F_M7B));
  int c = signed16(reg(r, F_M7C));
  int d = signed16(reg(r, F_M7D));
  int centerX = signed13(reg(r, F_M7X));
  int centerY = signed13(reg(r, F_M7Y));
  int ox = clippedOffset(signed13(reg(r, F_M7HOFS)) - centerX);
  int oy = clippedOffset(signed13(reg(r, F_M7VOFS)) - centerY);
  int fx = truncated(a * ox) + truncated(b * oy) + centerX * 256 + truncated(b * sy) + a * sx;
  int fy = truncated(c * ox) + truncated(d * oy) + centerY * 256 + truncated(d * sy) + c * sx;
  int fieldX = fieldInteger(fx);
  int fieldY = fieldInteger(fy);
  uint character = 0u;
  bool outside = outsideField(fieldX) || outsideField(fieldY);
  if ((m7sel & 0x80u) == 0u || !outside) {
    fieldX &= 0x3FF;
    fieldY &= 0x3FF;
    uint entryAddress = uint(((fieldY >> 3) << 7) | (fieldX >> 3)) << 1;
    character = vramByteBefore(upTo, entryAddress);
  } else if ((m7sel & 0x40u) == 0u) {
    return 0u;
  }
  uint pixelAddress = (uint((int(character) << 6) | ((fieldY & 7) << 3) | (fieldX & 7)) << 1) | 1u;
  return vramByteBefore(upTo, pixelAddress);
}

Shown sampleField(Regs r, uint upTo, uint layer, uint x, uint lineNo) {
  uint pixel = fieldPixelAt(r, upTo, x, lineNo);
  Shown s = nothingShown();
  if (layer == 1u) {
    uint w = pixel & 0x7Fu;
    if (w == 0u) return s;
    s.present = true; s.word = w; s.priority = (pixel & 0x80u) != 0u;
    return s;
  }
  if (pixel == 0u) return s;
  s.present = true;
  s.word = pixel;
  s.priority = false;
  if ((reg(r, F_CGWSEL) & 1u) != 0u) { s.direct = true; s.color = directColor(pixel, 0u); }
  return s;
}

// ---- the sprites -------------------------------------------------------------------

// The two sizes $2101 bits 7-5 name: small width, small height, large width, large height.
uint4 spriteSizes(uint pair) {
  switch (pair) {
    case 0u: return uint4(8u, 8u, 16u, 16u);
    case 1u: return uint4(8u, 8u, 32u, 32u);
    case 2u: return uint4(8u, 8u, 64u, 64u);
    case 3u: return uint4(16u, 16u, 32u, 32u);
    case 4u: return uint4(16u, 16u, 64u, 64u);
    case 5u: return uint4(32u, 32u, 64u, 64u);
    case 6u: return uint4(16u, 32u, 32u, 64u);
    default: return uint4(16u, 32u, 32u, 32u);
  }
}

// Which of a sprite's rows a line crosses, or 0xFFFFFFFF where it misses: the
// eight-bit difference, doubled with the parity added at half height; object
// interlace makes a 16x32 sprite 16x16.
uint rowCrossed(uint y, uint width, uint height, uint lineNo, bool halfHeight, uint field) {
  if (halfHeight && width == 16u && height == 32u) height = 16u;
  uint into = (lineNo - 1u - y) & 0xFFu;
  uint row = halfHeight ? 2u * into + (field & 1u) : into;
  return row >= height ? 0xFFFFFFFFu : row;
}

int countedX(int x) { return x == -256 ? 0 : x; }
uint flipRow(uint row, uint width) { return (row / width) * width + (width - 1u - row % width); }

// The sprite's palette word and priority at position x, or priority 4 and word 0
// where no sprite claims it: none on a line whose Time gathered nothing, the carried
// buffer's bytes on a carried line, and on a gathered line Time's own walk — from
// the last sprite Range kept back to the first, loading at most 34 on-picture tiles

// Time's walk over a gathered line, into `words` and `priorities`, 256 bytes each
// packed four to a word: from the last sprite Range kept back to the first, loading
// at most 34 on-picture tiles counted each sprite's left to right, each loaded
// tile's nonzero pixels written so a sprite nearer the front keeps every position
// it claims. A position no sprite claims holds word 0 and priority 4.
void timeSprites(inout uint words[64], inout uint priorities[64]) {
  for (uint i = 0u; i < 64u; ++i) { words[i] = 0u; priorities[i] = 0x04040404u; }
  uint objsel = (lineWord(16) >> 8) & 0xFFu;
  bool halfHeight = ((lineWord(16) >> 16) & 2u) != 0u;
  uint found = lineWord(16) >> 24;
  uint oamAt = gOamsAt * 4u + lineWord(18) * 544u;
  uint4 sizes = spriteSizes((objsel >> 5) & 7u);
  uint tiles = 0u;
  for (uint nth = found; nth > 0u; --nth) {
    uint index = (lineWord(24u + (nth - 1u) / 4u) >> (8u * ((nth - 1u) % 4u))) & 0xFFu;
    uint record = oamAt + index * 4u;
    uint high = byteAt(oamAt + 512u + (index >> 2));
    uint bits = (high >> ((index & 3u) * 2u)) & 3u;
    uint wide = byteAt(record) | ((bits & 1u) << 8);
    int spriteX = int(wide) - (wide >= 256u ? 512 : 0);
    uint spriteY = byteAt(record + 1u);
    uint tile = byteAt(record + 2u);
    uint attributes = byteAt(record + 3u);
    bool large = (bits & 2u) != 0u;
    uint width = large ? sizes.z : sizes.x;
    uint height = large ? sizes.w : sizes.y;
    uint crossed = rowCrossed(spriteY, width, height, gLine, halfHeight, gField);
    if (crossed == 0xFFFFFFFFu) continue;
    bool flipVertical = (attributes & 0x80u) != 0u;
    bool flipHorizontal = (attributes & 0x40u) != 0u;
    uint row = flipVertical ? flipRow(crossed, width) : crossed;
    uint priority = (attributes >> 4) & 3u;
    uint palette = (attributes >> 1) & 7u;
    int counted = countedX(spriteX);
    for (uint across = 0u; across < width; across += 8u) {
      int tileX = counted + int(across);
      if (tileX <= -8 || tileX >= 256) continue;
      if (tiles >= 34u) continue;
      ++tiles;
      // The character holding the tile, each nibble of the number wrapping on its
      // own, and its row's four plane bytes, read once for the eight columns.
      uint first = flipHorizontal ? width - 1u - across : across;
      uint number = ((tile + first / 8u) & 0x0Fu) | ((((tile >> 4) + row / 8u) & 0x0Fu) << 4);
      uint base = objsel & 7u;
      uint name = (objsel >> 3) & 3u;
      bool secondTable = (attributes & 1u) != 0u;
      uint characterWord = ((base << 13) + (number << 4) + (secondTable ? ((name + 1u) << 12) : 0u)) & 0x7FFFu;
      uint at = (characterWord * 2u + (row % 8u) * 2u) & 0xFFFFu;
      uint planeBytes[4];
      for (uint plane = 0u; plane < 4u; ++plane) {
        planeBytes[plane] = vramByteAtLineStart((at + (plane / 2u) * 16u + (plane % 2u)) & 0xFFFFu);
      }
      for (uint column = across; column < across + 8u; ++column) {
        int x = spriteX + int(column);
        if (x < 0 || x >= 256) continue;
        uint sourceColumn = flipHorizontal ? width - 1u - column : column;
        uint pixel = 0u;
        for (uint plane = 0u; plane < 4u; ++plane) pixel |= ((planeBytes[plane] >> (7u - (sourceColumn % 8u))) & 1u) << plane;
        if (pixel == 0u) continue;
        uint slot = uint(x) / 4u;
        uint shift = 8u * (uint(x) % 4u);
        words[slot] = (words[slot] & ~(0xFFu << shift)) | ((128u + palette * 16u + pixel) << shift);
        priorities[slot] = (priorities[slot] & ~(0xFFu << shift)) | (priority << shift);
      }
    }
  }
}

// ---- the windows --------------------------------------------------------------------

// Whether the windows cover a position for a layer, 0 to 3 the backgrounds, 4 the
// sprites and 5 the color window: each window the span its edges name, both ends
// in, inverted and enabled by the layer's bits, the two combined by its logic.
bool windowCovers(Regs r, uint layer, uint x) {
  uint selector = layer < 2u ? reg(r, F_W12SEL) : (layer < 4u ? reg(r, F_W34SEL) : reg(r, F_WOBJSEL));
  uint bits = (selector >> ((layer & 1u) * 4u)) & 0x0Fu;
  uint logic = layer < 4u ? reg(r, F_WBGLOG) : reg(r, F_WOBJLOG);
  uint op = (logic >> ((layer & 3u) * 2u)) & 3u;
  bool one = reg(r, F_WH0) <= x && x <= reg(r, F_WH1);
  bool two = reg(r, F_WH2) <= x && x <= reg(r, F_WH3);
  if ((bits & 1u) != 0u) one = !one;
  if ((bits & 4u) != 0u) two = !two;
  bool enable1 = (bits & 2u) != 0u;
  bool enable2 = (bits & 8u) != 0u;
  if (!enable1) return enable2 ? two : false;
  if (!enable2) return one;
  if (op == 0u) return one || two;
  if (op == 1u) return one && two;
  if (op == 2u) return one != two;
  return one == two;
}

bool masked(Regs r, uint layer, uint maskRegister, uint x) {
  return (maskRegister & (1u << layer)) != 0u && windowCovers(r, layer, x);
}

bool regionCovers(Regs r, uint region, uint x) {
  if (region == 1u) return !windowCovers(r, 5u, x);
  if (region == 2u) return windowCovers(r, 5u, x);
  return region == 3u;
}

// ---- the charts ---------------------------------------------------------------------

// Each mode's priority chart, front to back, a place being the layer in the low
// nibble and the priority above it; the sprites are layer 4. chartSpan gives where
// a chart's places begin and how many it has; chartPlace the place at an index.
uint2 chartSpan(uint chart) {
  switch (chart) {
    case 0u: return uint2(0u, 12u);   // Mode 0
    case 1u: return uint2(12u, 10u);  // Mode 1
    case 2u: return uint2(22u, 10u);  // Mode 1 with $2105 bit 3
    case 3u: return uint2(32u, 8u);   // Modes 2, 3, 4 and 5
    case 4u: return uint2(40u, 6u);   // Mode 6
    case 5u: return uint2(46u, 5u);   // Mode 7
    default: return uint2(51u, 7u);   // Mode 7 with SETINI bit 6
  }
}
uint chartPlace(uint index) {
  switch (index) {
    case 0u: return 0x34u;
    case 1u: return 0x10u;
    case 2u: return 0x11u;
    case 3u: return 0x24u;
    case 4u: return 0x00u;
    case 5u: return 0x01u;
    case 6u: return 0x14u;
    case 7u: return 0x12u;
    case 8u: return 0x13u;
    case 9u: return 0x04u;
    case 10u: return 0x02u;
    case 11u: return 0x03u;
    case 12u: return 0x34u;
    case 13u: return 0x10u;
    case 14u: return 0x11u;
    case 15u: return 0x24u;
    case 16u: return 0x00u;
    case 17u: return 0x01u;
    case 18u: return 0x14u;
    case 19u: return 0x12u;
    case 20u: return 0x04u;
    case 21u: return 0x02u;
    case 22u: return 0x12u;
    case 23u: return 0x34u;
    case 24u: return 0x10u;
    case 25u: return 0x11u;
    case 26u: return 0x24u;
    case 27u: return 0x00u;
    case 28u: return 0x01u;
    case 29u: return 0x14u;
    case 30u: return 0x04u;
    case 31u: return 0x02u;
    case 32u: return 0x34u;
    case 33u: return 0x10u;
    case 34u: return 0x24u;
    case 35u: return 0x11u;
    case 36u: return 0x14u;
    case 37u: return 0x00u;
    case 38u: return 0x04u;
    case 39u: return 0x01u;
    case 40u: return 0x34u;
    case 41u: return 0x10u;
    case 42u: return 0x24u;
    case 43u: return 0x14u;
    case 44u: return 0x00u;
    case 45u: return 0x04u;
    case 46u: return 0x34u;
    case 47u: return 0x24u;
    case 48u: return 0x14u;
    case 49u: return 0x00u;
    case 50u: return 0x04u;
    case 51u: return 0x34u;
    case 52u: return 0x24u;
    case 53u: return 0x11u;
    case 54u: return 0x14u;
    case 55u: return 0x00u;
    case 56u: return 0x04u;
    case 57u: return 0x01u;
    default: return 0u;
  }
}

uint chartOf(Regs r) {
  uint mode = modeOf(r);
  if (mode == 0u) return 0u;
  if (mode == 1u) return (reg(r, F_BGMODE) & 8u) != 0u ? 2u : 1u;
  if (mode <= 5u) return 3u;
  if (mode == 6u) return 4u;
  return (reg(r, F_SETINI) & 0x40u) != 0u ? 6u : 5u;
}

uint fixedColor(Regs r) { return reg(r, F_FIXEDRED) | (reg(r, F_FIXEDGREEN) << 5) | (reg(r, F_FIXEDBLUE) << 10); }

// A color through color math: the addend added or subtracted a channel at a time,
// halved first where asked, each channel held to five bits.
uint combine(uint color, uint addend, bool subtract, bool halve) {
  uint result = 0u;
  for (uint shift = 0u; shift < 15u; shift += 5u) {
    uint above = (color >> shift) & 0x1Fu;
    uint below = (addend >> shift) & 0x1Fu;
    uint value = subtract ? (above > below ? above - below : 0u) : above + below;
    if (halve) value >>= 1;
    result |= (value > 31u ? 31u : value) << shift;
  }
  return result;
}

// What the main screen's pixel decided: whether $2130 blacked it, its addend (0
// none, 1 the fixed color, 2 the sub screen's pixel), the direction, the halving,
// and its color before the math.
struct Decision {
  bool present;
  bool clipped;
  uint addend;
  bool subtract;
  bool halved;
  uint color;
};

// The main screen's pixel at position x under color math, and its decision. `sub`
// is the sub screen's pixel where the caller resolved it (`haveSub`), which the
// left half of a half-pixel position does; otherwise it is resolved here when the

bool hiresAt(Regs r) {
  uint mode = modeOf(r);
  return mode == 5u || mode == 6u || (reg(r, F_SETINI) & 8u) != 0u;
}

// The converter's bytes for a 15-bit color at INIDISP's brightness, as the texture
// holds them.
float4 convert(Regs r, uint color) {
  uint brightness = reg(r, F_INIDISP) & 0x0Fu;
  if (brightness == 0u) return float4(0.0, 0.0, 0.0, 1.0);
  uint red = ((color & 0x1Fu) * (brightness + 1u) * 255u + 248u) / 496u;
  uint green = (((color >> 5) & 0x1Fu) * (brightness + 1u) * 255u + 248u) / 496u;
  uint blue = (((color >> 10) & 0x1Fu) * (brightness + 1u) * 255u + 248u) / 496u;
  return float4(float(red), float(green), float(blue), 255.0) / 255.0;
}

// The decision the main pixel one position to the left made, read where the left
// half of a half-pixel position is drawn under it: none at position 0, none where
