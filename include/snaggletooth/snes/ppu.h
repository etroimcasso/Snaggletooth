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
// program that writes a register mid-line changes the rest of the line. Mode 1
// is the mode the chip draws: its three backgrounds and the sprites, in the
// priority order $2105 names, over the backdrop.
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

namespace snaggletooth {

// The two lines vertical blank can begin on. The picture ends on line 224, or on
// line 239 when SETINI bit 2 asks for the taller one; vblank runs from its start
// line to the end of the frame, and line 0 is the end of vblank.
constexpr std::uint16_t kVblankStartLine = 225u;
constexpr std::uint16_t kOverscanVblankStartLine = 240u;

// The sprites OAM describes, and the pixels a line of picture holds.
constexpr unsigned kSprites = 128u;
constexpr std::uint16_t kPictureWidth = 256u;

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
  // no picture will ask about.
  void timeSprites(std::uint16_t line) noexcept;

  // The line boundary handing Range a fresh pass over OAM for `line`, and with
  // it the sprite the walk begins at: sprite 0, or the one $2103 bit 7 and the
  // sprite-table port's own address name between them.
  void beginRange(std::uint16_t line) noexcept;

  // The four bytes the chip's converter drives at picture position (x, y): red,
  // green, blue, then 255. x runs across a line's 256 pixels and y down the
  // picture's lines from its first. The colour is the one the main screen's
  // enabled backgrounds and the line's sprites name at that dot, taken in the
  // mode's priority order, or the backdrop where none of them shows, scaled by
  // INIDISP's brightness. Forced blank and brightness zero are black.
  [[nodiscard]] std::array<std::uint8_t, 4> pixel(std::uint16_t x,
                                                  std::uint16_t y) const noexcept;

 private:
  // One background as its own registers describe it. The mode fixes the depth: a
  // sixteen-colour background is four bitplanes and a four-colour one is two, and
  // a character takes eight bytes for each.
  struct Background {
    std::uint8_t screen;         // its $2107-$210A: the map's base and the map's size
    std::uint8_t characterBase;  // its nibble of $210B or $210C, counting 8 KB blocks
    std::uint16_t horizontal;    // its horizontal offset register
    std::uint16_t vertical;      // its vertical offset register
    bool large;                  // its bit of $2105: 16x16 blocks rather than 8x8 tiles
    unsigned planes;             // its bitplanes, and so its colours: 1 << planes
  };

  // What a background shows at a picture position: the palette word its tile's
  // pixel names, and the tile's own priority bit, which decides where the pixel
  // sits in the mode's order.
  struct Shown {
    std::uint8_t word;
    bool priority;
  };

  // What a background shows at a picture position, or nothing where its tile's
  // pixel is colour 0, which every palette treats as transparent.
  [[nodiscard]] std::optional<Shown> sample(const Background& background, std::uint16_t x,
                                            std::uint16_t y) const noexcept;

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
  // all is that screen's backdrop: palette word 0 on the main screen, and the
  // fixed colour on the sub screen, which has no word of its own.
  struct Resolved {
    std::uint8_t word;
    Layer layer;
  };

  // The front-most pixel of one screen, by the order Mode 1 keeps, each layer
  // taken only where that screen enables it and the windows leave it there.
  [[nodiscard]] std::optional<Resolved> resolve(Screen screen, std::uint16_t x,
                                                std::uint16_t line) const noexcept;

  // Whether one of $2130's two-bit regions covers a picture position: 0 nowhere,
  // 1 outside the colour window, 2 inside it, 3 everywhere. The colour window is
  // the sixth thing a window can be enabled for and feeds these two fields alone.
  [[nodiscard]] bool regionCovers(unsigned region, std::uint16_t x) const noexcept;

  // The 15-bit colour a palette word names, and the one $2132 holds.
  [[nodiscard]] std::uint16_t paletteColour(std::uint8_t word) const noexcept;
  [[nodiscard]] std::uint16_t fixedColour() const noexcept;

  // The three backgrounds Mode 1 draws, each with the registers it reads.
  [[nodiscard]] Background mode1Bg1() const noexcept;
  [[nodiscard]] Background mode1Bg2() const noexcept;
  [[nodiscard]] Background mode1Bg3() const noexcept;

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
