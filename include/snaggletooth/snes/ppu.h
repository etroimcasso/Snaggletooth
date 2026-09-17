#pragma once

// The PPU: its register file, the three video memories behind it, and the
// picture it resolves from them a pixel at a time.
//
// The picture processor sits on the B bus at $2100-$213F. Its registers are
// written by the CPU and the transfer engines and read back through the few
// ports that read at all; the register file, VRAM, the palette and the sprite
// table are the whole of what a program can put into the chip, and they are
// kept exactly as the hardware keeps them — the write-twice latches, the
// address flip-flops, the counter latch, the two open-bus values the chip's
// halves remember, and the windows in which each memory can be reached.
//
// A pixel is resolved from that state as it stands at the pixel's own dot, so a
// program that writes a register mid-line changes the rest of the line. Every
// mode draws the backgrounds it has, at the depths it gives them, and the
// sprites, in that mode's own priority order, over the backdrop. In modes 2, 4
// and 6 BG3's tilemap is not drawn: it is a table of offsets the other
// backgrounds are read through, a tile column at a time. Mode 7's one background
// is a field of packed pixels read through its matrix rather than a tilemap of
// characters, and SETINI bit 6 makes a second layer of the same pixels.
//
// Modes 5 and 6 draw a line in half-pixels, two to each of its 256 positions: the
// sub screen's pixel on the left half and the main screen's on the right. Their
// tiles are two characters wide across eight positions, the even pixels of a
// tile showing on the sub screen and the odd ones on the main. $2133 bit 3 draws
// any other mode's line the same way, from tiles one character wide.
//
// Sprites are the one part of the picture not resolved at the dot that shows
// them. A line's sprites are found and gathered during the line before it, in
// two passes the chip runs across that line's dots, and what those passes leave
// is part of the chip's state.
//
// The state is a plain value, PpuState, held once inside the machine's state
// and nowhere else, so a snapshot of the machine carries the PPU whole and a
// restore puts it back mid-latch. Ppu is the chip's behaviour applied to that
// value: the machine builds one over its state for the length of a call and
// never keeps it.
//
// What the chip is told about the machine at an access is PpuInputs — its
// input pins: where the beam is, the frame parity, the two blank signals, the
// clock rate and the level of the counter-latch line. It learns nothing else.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace snaggletooth {

// The two lines vertical blank can begin on. The picture ends on line 224, or on
// line 239 when SETINI bit 2 asks for the taller one; vblank runs from its start
// line to the end of the frame, and line 0 is the end of vblank.
constexpr std::uint16_t kVblankStartLine = 225u;
constexpr std::uint16_t kOverscanVblankStartLine = 240u;

// The sprites OAM describes, the positions a line of picture holds, and the
// half-pixels a line drawn in half-pixels holds, two to a position.
constexpr unsigned kSprites = 128u;
constexpr std::uint16_t kPictureWidth = 256u;
constexpr std::uint16_t kHiresWidth = 512u;

// What the chip can afford on one line: the sprites Range keeps, and the 8x8
// tiles Time loads from them. A sprite past the first is dropped and a tile past
// the second is not loaded, and each raises its own flag in $213E.
constexpr unsigned kSpritesPerLine = 32u;
constexpr unsigned kSpriteTilesPerLine = 34u;

// The palette word a sprite claims sits at 128 or above, so 0 is a word no
// sprite can name; a position holds this priority where no sprite claimed it,
// one past the three the hardware has.
constexpr std::uint8_t kNoSprite = 4u;

// What the chip's two sprite passes leave behind: the line they gathered, and
// the pass over the line after it that is still running.
//
// Range walks OAM across a line's visible dots, two dots a sprite, and keeps
// the first kSpritesPerLine the next line crosses. Time then runs in the
// horizontal blank that follows and draws those sprites into the line buffer,
// back to front, so the sprite nearest the front keeps every position it
// claims — up to kSpriteTilesPerLine tiles, after which it loads nothing more.
// The two passes run in opposite directions along the sprites Range kept.
struct SpriteLine {
  // The line buffer Time filled, and the line it was filled for. A position
  // holds the palette word the sprite claiming it named and that sprite's own
  // priority against the backgrounds, or kNoSprite where none claimed it.
  std::array<std::uint8_t, kPictureWidth> word{};
  std::array<std::uint8_t, kPictureWidth> priority{};
  std::uint16_t line = 0;

  // The pass in flight: the sprites Range has kept so far, in the order it found
  // them, how far along OAM it has walked, and the sprite it began at — which
  // $2103 bit 7 moves, and which the walk counts from and wraps around.
  std::array<std::uint8_t, kSpritesPerLine> inRange{};
  std::uint8_t found = 0;
  std::uint8_t scanned = 0;
  std::uint8_t first = 0;

  [[nodiscard]] bool operator==(const SpriteLine&) const noexcept = default;
};

// The machine as the PPU sees it at one access.
struct PpuInputs {
  std::uint16_t hdot = 0;   // the beam's dot within the line (0..340), by the dot map's two long dots
  std::uint16_t vpos = 0;   // the beam's line
  std::uint8_t field = 0;   // the frame parity, toggled every frame
  bool vblank = false;      // the vertical-blank signal: raised at the start line and held to the frame's end
  bool hblank = false;      // the horizontal-blank signal: raised at H=274 and lowered at H=1, on every line
  bool pal = false;         // the clock-rate pin: 50 Hz when set
  bool extLatch = true;     // the counter-latch line's level, WRIO bit 7 (high when nothing pulls it)
  bool lateHalf = false;    // the dot's second half: the last two of a four-cycle dot's master cycles, the last three of a six-cycle dot's
};

// The PPU as a value: the register file, the latches, and the three memories.
// A register that is stored as written keeps the name the hardware gives it.
struct PpuState {
  std::array<std::uint8_t, 65536> vram{};  // 64 KB video RAM (32K words)
  std::array<std::uint8_t, 512> cgram{};   // 512 B palette RAM (256 words)
  std::array<std::uint8_t, 544> oam{};     // 512 B of sprite entries and the 32 B of high bits

  // What Range and Time have made of that sprite table for the line being drawn.
  SpriteLine sprites{};

  // ---- display ----------------------------------------------------------------
  std::uint8_t inidisp = 0x80;  // $2100: bit7 forced blank (set at power-on), bits3-0 brightness
  std::uint8_t objsel = 0;      // $2101: sprite sizes, name select and name base
  std::uint8_t bgmode = 0x0F;   // $2105: the screen mode, BG3's priority and the tile sizes (power-on: mode 7, all set)
  std::uint8_t mosaic = 0;      // $2106: the mosaic size and the layers it applies to
  std::uint8_t bg1sc = 0;       // $2107: BG1 screen base and size
  std::uint8_t bg2sc = 0;       // $2108: BG2 screen base and size
  std::uint8_t bg3sc = 0;       // $2109: BG3 screen base and size
  std::uint8_t bg4sc = 0;       // $210A: BG4 screen base and size
  std::uint8_t bg12nba = 0;     // $210B: BG1/BG2 character base
  std::uint8_t bg34nba = 0;     // $210C: BG3/BG4 character base
  std::uint8_t setini = 0;      // $2133: interlace, overscan, hires, EXTBG, external sync

  // ---- scroll -----------------------------------------------------------------
  // Each is written twice, low byte then high, through one latch the eight share:
  // a horizontal offset keeps three bits of its own previous high byte in place of
  // the latch's low three, a vertical offset takes the latch whole. The value is
  // kept as the two writes assembled it; the offset is its low ten bits.
  std::uint16_t bg1hofs = 0;  // $210D
  std::uint16_t bg1vofs = 0;  // $210E
  std::uint16_t bg2hofs = 0;  // $210F
  std::uint16_t bg2vofs = 0;  // $2110
  std::uint16_t bg3hofs = 0;  // $2111
  std::uint16_t bg3vofs = 0;  // $2112
  std::uint16_t bg4hofs = 0;  // $2113
  std::uint16_t bg4vofs = 0;  // $2114
  std::uint8_t bgLatch = 0;   // the byte the last scroll write left for the next

  // ---- Mode 7 -----------------------------------------------------------------
  // The matrix, the centre and the Mode 7 scroll are written twice through a
  // second shared latch, low byte then high, and kept as the two writes assembled
  // them. A write to $210D or $210E updates both BG1's offset (through the scroll
  // latch) and Mode 7's (through this one).
  std::uint8_t m7sel = 0;      // $211A: screen-over, the fill, the flips
  std::uint16_t m7a = 0xFFFF;  // $211B: matrix A, 8.8 fixed point, signed
  std::uint16_t m7b = 0xFFFF;  // $211C: matrix B
  std::uint16_t m7c = 0;       // $211D: matrix C
  std::uint16_t m7d = 0;       // $211E: matrix D
  std::uint16_t m7x = 0;       // $211F: the centre's X, signed in its low 13 bits
  std::uint16_t m7y = 0;       // $2120: the centre's Y, the same
  std::uint16_t m7hofs = 0;    // $210D: Mode 7 horizontal scroll, the same
  std::uint16_t m7vofs = 0;    // $210E: Mode 7 vertical scroll, the same
  std::uint8_t m7Latch = 0;    // the byte the last Mode 7 write left for the next
  std::uint8_t m7bByte = 0xFF; // the last byte written to $211C: the multiplier of the product at $2134-$2136

  // ---- the VRAM port ----------------------------------------------------------
  std::uint8_t vmain = 0x0F;    // $2115: increment mode (bit7), address translation (bits3-2), step (bits1-0)
  std::uint16_t vmadd = 0;      // $2116/$2117: the VRAM word address
  std::uint16_t vramLatch = 0;  // the 16-bit read-prefetch register behind $2139/$213A

  // ---- the palette port -------------------------------------------------------
  std::uint8_t cgadd = 0;       // $2121: the CGRAM word address
  bool cgLatchHigh = false;     // the $2122/$213B low/high access flip-flop (false = low byte next)
  std::uint8_t cgLatch = 0;     // the low byte held between the two halves of a CGRAM write

  // ---- the sprite-table port --------------------------------------------------
  std::uint16_t oamadd = 0;     // $2102/$2103 as written: the 9-bit reload value, the priority-rotation bit above it
  std::uint16_t oamAddress = 0; // the 10-bit OAM byte address the port is at: the reload value doubled on a write to $2102/$2103 and at the start of vblank, stepped by every access
  std::uint8_t oamLatch = 0;    // the low byte held between the two halves of a write below $200

  // ---- windows ----------------------------------------------------------------
  std::uint8_t w12sel = 0;   // $2123: window enables and inversions for BG1 and BG2
  std::uint8_t w34sel = 0;   // $2124: the same for BG3 and BG4
  std::uint8_t wobjsel = 0;  // $2125: the same for sprites and the colour window
  std::uint8_t wh0 = 0;      // $2126: window 1's left edge
  std::uint8_t wh1 = 0;      // $2127: window 1's right edge
  std::uint8_t wh2 = 0;      // $2128: window 2's left edge
  std::uint8_t wh3 = 0;      // $2129: window 2's right edge
  std::uint8_t wbglog = 0;   // $212A: how the two windows combine, per background
  std::uint8_t wobjlog = 0;  // $212B: the same for sprites and the colour window
  std::uint8_t tm = 0;       // $212C: main-screen layer enables
  std::uint8_t ts = 0;       // $212D: sub-screen layer enables
  std::uint8_t tmw = 0;      // $212E: which main-screen layers the windows mask
  std::uint8_t tsw = 0;      // $212F: which sub-screen layers the windows mask

  // ---- colour math ------------------------------------------------------------
  std::uint8_t cgwsel = 0;     // $2130: the colour window's regions, the addend, direct colour
  std::uint8_t cgadsub = 0;    // $2131: add or subtract, half, and the layers it applies to
  std::uint8_t fixedRed = 0;   // $2132: the fixed colour's red, five bits
  std::uint8_t fixedGreen = 0; // $2132: its green
  std::uint8_t fixedBlue = 0;  // $2132: its blue

  // ---- the counter latch ------------------------------------------------------
  // A read of $2137, or the latch line falling, captures the beam's dot and line
  // here. Each counter reads out in two halves through its own flip-flop; a read
  // of $213F clears the flag and both flip-flops.
  std::uint16_t ophct = 0x01FF;  // $213C: the dot latched, nine bits
  std::uint16_t opvct = 0x01FF;  // $213D: the line latched, nine bits
  bool ophctHigh = false;        // $213C's flip-flop: the high half is next
  bool opvctHigh = false;        // $213D's flip-flop
  bool countersLatched = false;  // $213F bit 6: new values have been latched since the last read of $213F

  // ---- mosaic -----------------------------------------------------------------
  // The vertical phase of the mosaic blocks: the picture line the current row of
  // blocks began on, and the size $2106 held when it began. A line's mosaiced pixels
  // are read from that first line; a row of blocks runs for the size it began with,
  // and the size in the register is read again only as the next row begins.
  std::uint16_t mosaicBlockLine = 1;  // the line the current row of blocks began on
  std::uint8_t mosaicBlockSize = 0;   // $2106 bits 7-4 as that row began

  // ---- the half-pixel line -----------------------------------------------------
  // What the last main-screen pixel of the line decided, which the sub screen's
  // half-pixel one position to its right is drawn under: whether $2130 blacked it,
  // whether it took math, which way and with which addend, whether the result was
  // halved, and its colour before the math. Carried a position at a time along
  // every line and read by the left half of a line drawn in half-pixels alone.
  struct MainDecision {
    bool present = false;       // a main pixel has been drawn on this line
    bool clipped = false;       // $2130 bits 7-6 replaced it with black
    std::uint8_t addend = 0;    // 0 no math, 1 the fixed colour, 2 the sub screen's pixel
    bool subtract = false;      // the math subtracted
    bool halved = false;        // the result was halved
    std::uint16_t colour = 0;   // its colour before the math, five bits a channel
    [[nodiscard]] bool operator==(const MainDecision&) const noexcept = default;
  };
  MainDecision lastMain{};

  // ---- status -----------------------------------------------------------------
  // Raised by the two passes whether or not $212C shows the sprites at all, and
  // cleared as the next picture begins.
  bool rangeOver = false;  // $213E bit 6: Range met more sprites than it can keep
  bool timeOver = false;   // $213E bit 7: Time met more tiles than it can load

  // ---- the chip's two open-bus values -----------------------------------------
  // Each half of the chip remembers the last byte read through its own ports and
  // answers with it where a read has nothing to say: the first half on a read of
  // any write-only register in its groups and in bit 4 of $213E, the second in the
  // top bit of a palette read's second half, the top seven bits of a counter's
  // second half, and bit 5 of $213F.
  std::uint8_t ppu1Bus = 0;  // the last byte read from $2134-$2136, $2138-$213A, $213E
  std::uint8_t ppu2Bus = 0;  // the last byte read from $213B-$213D, $213F

  // The product at $2134-$2136: the signed 16-bit matrix A times the signed byte
  // last written to $211C, a signed 24-bit result, ready as soon as either is
  // written.
  [[nodiscard]] std::int32_t multiplyResult() const noexcept;

  [[nodiscard]] bool forcedBlank() const noexcept { return (inidisp & 0x80u) != 0u; }

  // The two signals SETINI gives the 5A22: bit 0 asks for an interlaced frame,
  // bit 2 for the taller picture, which moves vertical blank's start line to
  // kOverscanVblankStartLine.
  [[nodiscard]] bool interlace() const noexcept { return (setini & 0x01u) != 0u; }
  [[nodiscard]] bool overscan() const noexcept { return (setini & 0x04u) != 0u; }

  // The line vertical blank begins on with SETINI as it stands.
  [[nodiscard]] std::uint16_t vblankStartLine() const noexcept {
    return overscan() ? kOverscanVblankStartLine : kVblankStartLine;
  }

  // Two are the same when every register, latch and counter is, and every byte of
  // the three memories — the whole of what a program put into the chip, in one
  // comparison.
  [[nodiscard]] bool operator==(const PpuState&) const noexcept = default;
};

// The chip's behaviour over one PpuState. Built by the machine over its own
// state for one call; it holds a reference and is never kept.
class Ppu {
 public:
  explicit Ppu(PpuState& state) noexcept : s_(state) {}

  // A read of $2100-$213F: the byte the chip drove, or nothing, which the machine
  // answers with the CPU's own open bus. A register that reads at all is read
  // with its side effects — a counter's flip-flop, the prefetch, the address
  // step — and a write-only register in the first half's groups answers with
  // that half's open-bus value.
  [[nodiscard]] std::optional<std::uint8_t> read(std::uint16_t offset, const PpuInputs& in);

  // A write to $2100-$213F, and where it landed when it reached a video memory:
  // the VRAM word address for a write to $2118 or $2119, the palette word for a
  // write to $2122 on both halves, the OAM byte for a write to $2104 on both
  // halves. Absent on every other write, and on one the chip ignored because
  // the memory was not reachable when it was made.
  [[nodiscard]] std::optional<std::uint16_t> write(std::uint16_t offset, std::uint8_t value,
                                                    const PpuInputs& in);

  // The counter-latch line falling: the beam's dot and line are captured and
  // the flag raised. The machine calls it when WRIO bit 7 goes from 1 to 0.
  void latchCounters(const PpuInputs& in) noexcept;

  // The beam reaching dot 10 of vertical blank's first line: with the screen on,
  // the OAM address returns to the reload value.
  void beginVblank() noexcept;

  // The beam reaching the frame's first line: with the screen on, the two sprite
  // overflow flags clear. They were set by the last picture the chip drew, so a
  // frame the chip spent in forced blank leaves them as they were.
  void beginFrame() noexcept;

  // Range examining the next sprite, for the line after the one now running.
  // The machine calls it at that sprite's own dot — two dots a sprite from the
  // picture's first — so a write to $2101 landing mid-line reaches the sprites
  // whose dots have not gone by and no others. A sprite is kept when the line
  // crosses it and any part of it stands at or right of the picture's left
  // edge, up to kSpritesPerLine of them; the sprite that would be one too many
  // raises $213E bit 6 at its own dot. The chip is not examining OAM while it
  // renders nothing, so forced blank walks nowhere.
  void rangeSprite(std::uint16_t line) noexcept;

  // Time drawing the sprites Range kept into the line buffer, in the horizontal
  // blank before that line begins: from the last sprite Range kept back towards
  // the first, so the sprite nearest the front holds every position it claims,
  // and its priority is the one the backgrounds answer. It loads
  // kSpriteTilesPerLine 8x8 tiles standing on the picture and no more, counting
  // each sprite's left to right; the tile that would be one too many raises
  // $213E bit 7. Forced blank gathers nothing and leaves the buffer for a line
  // no picture will ask about. `field` is the parity of the frame the line
  // belongs to, which picks the rows of a sprite drawn at half height.
  void timeSprites(std::uint16_t line, std::uint8_t field) noexcept;

  // The line boundary handing Range a fresh pass over OAM for `line`, and with
  // it the sprite the walk begins at: sprite 0, or the one $2103 bit 7 and the
  // sprite-table port's own address name between them.
  void beginRange(std::uint16_t line) noexcept;

  // The beam beginning a line: the mosaic's vertical counter. The picture's first
  // line begins a row of blocks at the size $2106 holds; each later line begins a
  // new row when the current one has run for the size it began with, and is one
  // more line of the current row otherwise.
  void beginLine(std::uint16_t line) noexcept;

  // What the chip's converter drives at picture position (x, y), four bytes a
  // pixel — red, green, blue, then 255 — with x across a line's 256 positions and
  // y the beam's line, 1 for the picture's first.
  //
  // A position is drawn in half-pixels in modes 5 and 6, and in any other mode
  // while $2133 bit 3 is set, read at the dot like every register. Its right half
  // is the main screen's pixel under colour math. Its left half is the sub screen's
  // front-most pixel, or colour 0 where the sub screen shows nothing, drawn under
  // what the main pixel one position to its left decided: black where $2130 blacked
  // that pixel, then the same math — none, the fixed colour, or that pixel's own
  // colour before its math where its addend was the sub screen — halved where it
  // was halved. The left half at position 0 has no main pixel to its left and takes
  // neither. Any other position shows one pixel, the main screen's under colour
  // math, in both halves. Every position records its main pixel's decision in the
  // state, so a line resumed from a snapshot draws on as the unbroken line would. The main screen's
  // pixel is the one its enabled backgrounds and the line's sprites name at that
  // dot, taken in the mode's priority order, or the backdrop where none of them
  // shows. Everything is scaled by INIDISP's brightness; forced blank and
  // brightness zero are black on both halves.
  struct Dot {
    std::array<std::uint8_t, 4> left;   // the sub screen's half, or the pixel itself
    std::array<std::uint8_t, 4> right;  // the main screen's half, or the pixel itself
    bool hires;                          // the line is drawn in half-pixels here
  };
  //
  // `in` is the beam: its line is y, and its parity is the field. With $2133 bit 0
  // set, modes 5 and 6 read their tilemaps in half-lines, line y of field F reading
  // half-line 2y + F; with bit 1 set, in any mode, the line's sprites were gathered
  // at half height.
  [[nodiscard]] Dot dot(std::uint16_t x, const PpuInputs& in) noexcept;

  // What dot() records at picture position x of the beam's line, and nothing else:
  // the main pixel's decision, carried to the next position whether or not the
  // picture is drawn.
  void decide(std::uint16_t x, const PpuInputs& in) noexcept;

 private:
  // One background as its own registers describe it, with what the mode makes of
  // it: its depth, and where in the palette its colours are read. A character
  // takes eight bytes for each of its bitplanes.
  struct Background {
    std::uint8_t screen;         // its $2107-$210A: the map's base and the map's size
    std::uint8_t characterBase;  // its nibble of $210B or $210C, counting 8 KB blocks
    std::uint16_t horizontal;    // its horizontal offset register
    std::uint16_t vertical;      // its vertical offset register
    bool large;                  // its bit of $2105: 16x16 blocks rather than 8x8 tiles
    bool hires;                  // its mode's tiles are two characters wide and cover eight
                                 // positions, whatever `large` says of their width — modes 5
                                 // and 6; `large` then chooses the height alone
    unsigned planes;             // its bitplanes, and so its colours: 1 << planes
    unsigned paletteStride;      // the words one step of its tile's palette field moves,
                                 // or none where the mode gives it no palette field
    unsigned wordBase;           // the first palette word its own colours begin at
    std::uint16_t offsetBit;     // the bit of a BG3 entry that applies its offset to this
                                 // background — $2000 for BG1, $4000 for BG2 — or 0 where the
                                 // mode gives it no offset table
  };

  // A background's tiles in positions: eight across in modes 5 and 6 and wherever
  // `large` is clear, sixteen otherwise; sixteen down where `large` is set and
  // eight otherwise.
  [[nodiscard]] static unsigned tileWidth(const Background& background) noexcept;
  [[nodiscard]] static unsigned tileHeight(const Background& background) noexcept;

  // The two offsets a background is read with at one picture column.
  struct Offsets {
    std::uint16_t horizontal;
    std::uint16_t vertical;
  };

  // The tilemap entry a background holds at one of its own positions: the base its
  // screen register names, the row and column within one 32x32 screen, and the
  // terms that carry a wide or tall map into its further screens. A tile is eight
  // or sixteen positions across and eight or sixteen down, as `hires` and `large`
  // say.
  [[nodiscard]] std::uint16_t entryAt(const Background& background, unsigned bgX,
                                      unsigned bgY) const noexcept;

  // The offsets a background is read with at picture column x: its own two
  // registers' low ten bits, or where the mode gives it an offset table, what BG3's
  // tilemap says for the tile column x falls in. The first tile column takes the
  // registers whatever the table holds; tile T after it reads BG3's tile T - 1 from
  // BG3's own coarse scroll, in the row BG3's vertical offset names and the row
  // eight lines below it, whatever the line — or in mode 4 the first of those alone,
  // bit 15 saying which axis it is. An entry whose bit for this background is set
  // replaces the coarse horizontal offset, keeping the register's low three bits,
  // or the vertical offset whole.
  [[nodiscard]] Offsets offsetsFor(const Background& background, std::uint16_t x) const noexcept;

  // What a background shows at a picture position: the palette word its tile's
  // pixel names, the tile's own priority bit, which decides where the pixel sits
  // in the mode's order, and — where the pixel was read as a colour rather than
  // as an index — that colour, which no palette word names.
  struct Shown {
    std::uint8_t word;
    bool priority;
    std::optional<std::uint16_t> direct;
  };

  // What a background shows at a picture position, or nothing where its tile's
  // pixel is colour 0, which every palette treats as transparent. `half` is which
  // half of the position a two-character tile is read for: the left, its even
  // pixel, or the right, its odd one. A tile one character wide has one pixel to
  // the position and ignores it.
  [[nodiscard]] std::optional<Shown> sample(const Background& background, std::uint16_t x,
                                            std::uint16_t y, bool half) const noexcept;

  // One sprite as its OAM record describes it: its position, the first of its
  // characters, its attribute byte, and the size its own flag chose from the
  // pair $2101 names. X is nine bits read as signed, so a sprite can hang off
  // the left edge; Y is eight, and its wrap is what brings a tall sprite's
  // lower part in at the top of the picture.
  struct Sprite {
    int x;
    std::uint8_t y;
    std::uint8_t tile;
    std::uint8_t attributes;  // vhoopppN
    unsigned width;
    unsigned height;
  };

  // The record OAM holds for a sprite, the high table's two bits included.
  [[nodiscard]] Sprite spriteAt(std::uint8_t index) const noexcept;

  // The sprite the walk for `line` begins at, which is also the sprite in front
  // of every other. $2103 bit 7 clear leaves it sprite 0; set, it is the sprite
  // the port's own address stands in, and where that address is parked on the
  // last byte of a record the line the pass is matching is added to it.
  [[nodiscard]] std::uint8_t firstSprite(std::uint16_t line) const noexcept;

  // The byte address in VRAM of the character holding the sprite's pixel at
  // column and row, both counted from its top left after flipping. A sprite's
  // tiles wrap inside the 16x16 character table, each nibble of the number on
  // its own, which is not how a background's 16x16 block runs on.
  [[nodiscard]] std::size_t spriteCharacter(const Sprite& sprite, unsigned column,
                                            unsigned row) const noexcept;

  // The six things a window can be enabled for. Each keeps four bits of a window
  // selector — two enables and two inversions — and two bits of a logic register,
  // and the first five have a bit of this value in the two mask registers. The
  // colour window has none: it feeds colour math and no layer's visibility.
  enum class Layer : unsigned { Bg1 = 0u, Bg2 = 1u, Bg3 = 2u, Bg4 = 3u, Object = 4u, Colour = 5u };

  // Whether the windows cover a picture position for one layer. Each window is the
  // span its two edges name, both ends inclusive and empty where the left edge
  // stands past the right, taken as written or inverted as the layer's own bits
  // direct. Where the layer enables both, they are combined by the logic its two
  // bits of $212A or $212B name; where it enables one, that window is the answer;
  // where it enables neither, nothing is covered.
  [[nodiscard]] bool windowCovers(Layer layer, std::uint16_t x) const noexcept;

  // Whether a layer shows nothing at a picture position on the screen whose mask
  // register this is: the windows cover the position and that register names the
  // layer. The backdrop has no bit in either register and is never masked.
  [[nodiscard]] bool masked(Layer layer, std::uint8_t maskRegister,
                            std::uint16_t x) const noexcept;

  // Which of the two screens a resolution is for. They differ in the register
  // that puts layers on them and the register that masks those layers, and in
  // nothing else: the same order decides both.
  enum class Screen : unsigned { Main, Sub };

  // What a screen shows at a picture position — the palette word and the layer it
  // came from, which is what decides whether colour math reaches it. Nothing at
  // all is that screen's backdrop: palette word 0 on the main screen; on the sub
  // screen, the fixed colour as colour math's addend and palette word 0 as the left
  // half of a line drawn in half-pixels.
  struct Resolved {
    std::uint8_t word;
    Layer layer;
    std::optional<std::uint16_t> direct;
  };

  // One place in a mode's priority chart: a background's tiles at one priority,
  // or a sprite at one of its four. The chart is read front to back, and the
  // first place holding anything at a position is the pixel.
  struct Place {
    Layer layer;
    unsigned priority;
  };

  // The chart the mode $2105 names keeps, front to back — and for Mode 1 the
  // chart $2105 bit 3 exchanges it for, which is Mode 1's alone.
  [[nodiscard]] std::span<const Place> order() const noexcept;

  // How the mode reads one of the four backgrounds — its depth, its palette and
  // whether it is read through an offset table — or nothing where the mode does not
  // have that background at all, BG3 in modes 2, 4 and 6 among them. Mode 7's
  // background is not one of these: it is the field, read by sampleField.
  [[nodiscard]] std::optional<Background> background(Layer layer) const noexcept;

  // A position in Mode 7's field, in 1/256 pixel: the pixel is bits 8-10 of each
  // coordinate, the map entry bits 11-17, and anything above them is a position
  // outside the field.
  struct FieldPoint {
    std::int32_t x;
    std::int32_t y;
  };

  // The field position the matrix maps a picture position to, from the
  // registers as they stand: the two flips applied to the picture position, the
  // scroll less the centre clipped to ten bits with its sign, each product of a
  // matrix term with that clipped offset or with the line truncated to a
  // multiple of sixty-four, and the per-pixel products whole.
  [[nodiscard]] FieldPoint fieldPoint(std::uint16_t x, std::uint16_t line) const noexcept;

  // The field's pixel at a field position, by $211A's screen-over bits: the
  // field wrapping at 1024, or outside it nothing, or outside it character 0's
  // pixel at the position's low three bits. The pixel byte as the memory holds
  // it, zero included.
  [[nodiscard]] std::uint8_t fieldPixel(FieldPoint at) const noexcept;

  // What Mode 7 shows at a picture position for BG1, or for the BG2 SETINI bit 6
  // makes of the same pixel — its bit 7 the priority, its low seven bits the word,
  // and never read as a colour. Nothing where the pixel is zero or the position
  // is outside a field that shows nothing there. Whether BG2 exists at all is the
  // chart's to say: without the bit, Mode 7's order has no place for it.
  [[nodiscard]] std::optional<Shown> sampleField(Layer layer, std::uint16_t x,
                                                 std::uint16_t line) const noexcept;

  // Where a layer is read from at a picture position under mosaic: the position
  // itself, or the top-left corner of the block it stands in — the column taken
  // back to a multiple of the block's width, counted from the picture's left edge
  // at the size $2106 holds at this dot, and the line taken back to the one the
  // current row of blocks began on. Each background has its own enable bit, except
  // the second Mode 7 layer, which reads bit 0 as its vertical enable and bit 1 as
  // its horizontal one. Mode 7's blocks stand in the picture, so the matrix reads
  // their corners.
  //
  // In modes 5 and 6 a block is counted in half-pixels, twice its size wide, from
  // the line's first half-pixel, so its corner is always a left half and a size of
  // 0 already covers a right half with its left one. Under $2133 bit 3 a block is
  // counted in positions as on any other line.
  //
  // The line a tilemap is read at is the beam's, except in modes 5 and 6 with
  // $2133 bit 0 set, where it is a half-line: 2 x line + field, or for a block
  // 2 x its corner line in either field, which is the even field's half-line.
  struct Position {
    std::uint16_t x;
    std::uint16_t line;
    bool half;  // which half of the position a two-character tile is read for
  };
  [[nodiscard]] Position mosaicPosition(Layer layer, std::uint16_t x, const PpuInputs& in,
                                        bool half) const noexcept;

  // How far a line is into the current row of mosaic blocks: 0 on the row's first.
  [[nodiscard]] std::uint16_t mosaicIndex(std::uint16_t line) const noexcept;

  // Whether the chip is drawing a Mode 7 picture at an access: mode 7, forced
  // blank off, and a line before vertical blank's start — every dot of such a
  // line, horizontal blank included.
  [[nodiscard]] bool drawingModeSeven(const PpuInputs& in) const noexcept;

  // What $2134-$2136 hold at a dot of a Mode 7 picture: two products a dot on the
  // chip's own schedule, each with its low three bits dropped — the offset and
  // line products in the line's first three dots, then matrix A times the column
  // in a dot's first half and matrix C times it in the second. The line term is
  // the line less BG1's mosaic index where BG1 is mosaiced, flipped after.
  [[nodiscard]] std::int32_t multiplierWhileDrawing(const PpuInputs& in) const noexcept;

  // The front-most pixel of one screen, by the order its mode keeps, each layer
  // taken only where that screen enables it and the windows leave it there. `half`
  // is which half of the position a two-character tile is read for; sprites and
  // windows stand on whole positions and never read it.
  [[nodiscard]] std::optional<Resolved> resolve(Screen screen, std::uint16_t x,
                                                const PpuInputs& in, bool half) const noexcept;

  // The main screen's pixel at a picture position under colour math, the colour
  // it had before the math, and what it decided: `half` as resolve takes it, the
  // sub screen's addend being read for the left half.
  struct MainPixel {
    std::uint16_t colour;
    PpuState::MainDecision decision;
  };
  [[nodiscard]] MainPixel mainPixel(std::uint16_t x, const PpuInputs& in,
                                    bool half) const noexcept;

  // A colour taken through colour math: added to or subtracted from `addend` five
  // bits a channel, halved first where asked, and held to the range a channel has.
  [[nodiscard]] static std::uint16_t combine(std::uint16_t colour, std::uint16_t addend,
                                             bool subtract, bool halve) noexcept;

  // Whether a picture position is drawn in half-pixels: modes 5 and 6, and any
  // mode while $2133 bit 3 is set.
  [[nodiscard]] bool hiresAt() const noexcept;

  // Whether one of $2130's two-bit regions covers a picture position: 0 nowhere,
  // 1 outside the colour window, 2 inside it, 3 everywhere. The colour window is
  // the sixth thing a window can be enabled for and feeds these two fields alone.
  [[nodiscard]] bool regionCovers(unsigned region, std::uint16_t x) const noexcept;

  // The 15-bit colour a palette word names, and the one $2132 holds.
  [[nodiscard]] std::uint16_t paletteColour(std::uint8_t word) const noexcept;
  [[nodiscard]] std::uint16_t fixedColour() const noexcept;

  // The registers one of the four backgrounds reads, and the shape of the mode's
  // tiles: its screen register, its character-base nibble, its two offsets, its
  // tile-size bit, and whether the mode's tiles are two characters wide. What else
  // the mode makes of it is added by background().
  [[nodiscard]] Background registersOf(Layer layer) const noexcept;

  // The converter's four bytes for one 15-bit palette word at the brightness
  // INIDISP holds.
  [[nodiscard]] std::array<std::uint8_t, 4> convert(std::uint16_t colour) const noexcept;

  // Whether vertical blank is open to the memories, and whether the taller picture
  // was asked for after the blank had already begun — which shuts them again until
  // the line that picture ends on.
  [[nodiscard]] bool inVblankWindow(const PpuInputs& in) const noexcept;
  [[nodiscard]] bool overscanLate(const PpuInputs& in) const noexcept;

  // Whether each memory can be reached now. VRAM and the sprite table only in
  // vertical blank or forced blank; the palette in horizontal blank too.
  [[nodiscard]] bool vramReachable(const PpuInputs& in) const noexcept;
  [[nodiscard]] bool oamReachable(const PpuInputs& in) const noexcept;
  [[nodiscard]] bool cgramReachable(const PpuInputs& in) const noexcept;

  // The VRAM word the address currently reaches, after any $2115 translation.
  [[nodiscard]] std::uint16_t vramWordAddress() const noexcept;
  // The 16-bit word at that address, the value the prefetch register takes.
  [[nodiscard]] std::uint16_t readVramWord() const noexcept;
  // Advances the VRAM word address by the step $2115 selects, after a low- or
  // high-byte access as the increment mode directs.
  void stepVramAddress(bool highByte) noexcept;
  // Reinitialises the OAM address from the reload value.
  void reloadOamAddress() noexcept;

  // A scroll register's write through the shared scroll latch.
  void writeScroll(std::uint16_t& horizontal, std::uint16_t& vertical, bool isVertical,
                   std::uint8_t value) noexcept;
  // A Mode 7 register's write through the shared Mode 7 latch.
  void writeMode7(std::uint16_t& reg, std::uint8_t value) noexcept;

  PpuState& s_;
};

}  // namespace snaggletooth
