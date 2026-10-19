#pragma once

// The picture as a timeline: everything one frame's pixels are made of, laid out
// for a host that draws the frame itself, on a GPU one thread a pixel, rather than
// taking the pixels the machine draws.
//
// A host sets a PpuTimelineObserver on the machine (Snes::setPpuTimelineObserver).
// At the end of the cycle in which a frame's vertical blank begins, it is handed
// that frame's PpuTimeline:
//
//   - for every picture line, the registers the picture reads as they stood at the
//     line's first read dot, and the inputs the chip's sprite pass Time read as the
//     line began;
//   - every write to a register a line's dots read, at the first dot it reaches;
//   - the palette and the sprite table as each line read them, as versions a line
//     names;
//   - VRAM as it stood before the frame's first write inside its picture lines, and
//     every such write at its position.
//
// Nothing a pixel of the frame is made of is anywhere else, so the frame drawn from
// the timeline alone is the frame the chip drew. draw() draws it that way, through
// the chip's own dot code, into a host's buffer: it is the reference a host's own
// drawing is held to.
//
// Every record is a whole number of 32-bit words, with no padding, no pointer and
// no field narrower than a word, so a host uploads each span's bytes as they stand
// and a shader reads them as 32-bit words.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "snaggletooth/snes/ppu.h"

namespace snaggletooth {

struct PpuTimeline {
  // The sizes of the byte spans' entries, and of the buffer draw() fills: 512
  // half-pixels across, as many rows as the taller picture has, four bytes each.
  static constexpr std::size_t kVramBytes = 65536u;
  static constexpr std::size_t kPaletteBytes = 512u;
  static constexpr std::size_t kOamBytes = 544u;
  static constexpr std::size_t kSpriteBufferBytes = 2u * kPictureWidth;
  static constexpr std::size_t kDrawRows = kOverscanVblankStartLine - 1u;
  static constexpr std::size_t kDrawBytes = std::size_t{kHiresWidth} * kDrawRows * 4u;

  // A register the picture reads, named by the PpuState field that holds it, in
  // the order a Line's words 0 to 15 carry them; then a palette word. A write to
  // $2132 changes one of the three fixed-color channels for each of its top three
  // bits that is set, and a write to $210D or $210E changes both BG1's offset and
  // Mode 7's.
  enum class Field : std::uint32_t {
    Inidisp, Bgmode, Mosaic, Setini,
    Bg1sc, Bg2sc, Bg3sc, Bg4sc,
    Bg12nba, Bg34nba, M7sel, Cgwsel,
    Bg1hofs, Bg1vofs, Bg2hofs, Bg2vofs, Bg3hofs, Bg3vofs, Bg4hofs, Bg4vofs,
    M7a, M7b, M7c, M7d, M7x, M7y, M7hofs, M7vofs,
    W12sel, W34sel, Wobjsel, Wbglog,
    Wobjlog, Wh0, Wh1, Wh2,
    Wh3, Tm, Ts, Tmw,
    Tsw, Cgadsub, FixedRed, FixedGreen,
    FixedBlue,
    PaletteWord,
  };

  // Where a line's sprites come from.
  enum class SpriteSource : std::uint32_t {
    // Time gathered them as the line began, from the line's own inputs: the sprite
    // table version, $2101, $2133 bit 1, the parity, and the sprites the Range pass
    // kept across the line before. A reader runs Time over those.
    Gathered = 0u,
    // Time gathered nothing as the line began, the screen being forced blank then,
    // and the buffer it left is another line's: the line shows no sprite.
    None = 1u,
    // The buffer the line is drawn with is not one Time gathered in this frame: an
    // earlier frame's for this line, left by a Time that gathered nothing, or the
    // buffer a restore put in place. It is carried whole in `spriteBuffers`.
    Carried = 2u,
  };

  // One picture line, 32 words.
  //
  //   word  holds
  //   0     $2100 | $2105 << 8 | $2106 << 16 | $2133 << 24
  //   1     $2107 | $2108 << 8 | $2109 << 16 | $210A << 24
  //   2     $210B | $210C << 8 | $211A << 16 | $2130 << 24
  //   3     BG1's horizontal offset | its vertical offset << 16
  //   4     BG2's, the same
  //   5     BG3's
  //   6     BG4's
  //   7     Mode 7's matrix A | B << 16
  //   8     C | D << 16
  //   9     the center's X | its Y << 16
  //   10    Mode 7's horizontal offset | its vertical offset << 16
  //   11    $2123 | $2124 << 8 | $2125 << 16 | $212A << 24
  //   12    $212B | $2126 << 8 | $2127 << 16 | $2128 << 24
  //   13    $2129 | $212C << 8 | $212D << 16 | $212E << 24
  //   14    $212F | $2131 << 8 | fixed red << 16 | fixed green << 24
  //   15    fixed blue | the mosaic row's size << 8 | the line the row began on << 16
  //   16    the parity | $2101 << 8 | $2133 << 16 | the sprites Range kept << 24
  //   17    the sprite Range's walk began at | the palette version << 16
  //   18    the sprite table version
  //   19    the line's first event, an index into `events`
  //   20    how many events the line has
  //   21    the picture line, 1 to 239
  //   22    the SpriteSource
  //   23    the line's entry in `spriteBuffers` where word 22 is Carried, 0 otherwise
  //   24-31 the sprites Range kept, four to a word, the first in word 24's low byte
  //
  // Words 0 to 15 are each register exactly as PpuState holds it as the line's
  // first read dot is reached, a scroll or Mode 7 word as the two writes assembled
  // it: the chip masks what it reads, so nothing is masked here. The parity, the
  // palette version and the sprite table version hold for every line. $2101, $2133,
  // the count and the first sprite in words 16 and 17, and words 24 to 31, are what
  // Time read as the line began, which a write between the line's start and its
  // first read dot leaves apart from words 0 to 15; under any source but Gathered
  // they are 0. Word 23 is 0 on every line but a Carried one.
  struct Line {
    std::array<std::uint32_t, 32> words{};

    [[nodiscard]] std::uint32_t value(Field field) const noexcept;  // a register's value, words 0-15
    [[nodiscard]] std::uint32_t field() const noexcept { return words[16] & 0xFFu; }
    [[nodiscard]] std::uint32_t spriteObjsel() const noexcept { return (words[16] >> 8) & 0xFFu; }
    [[nodiscard]] std::uint32_t spriteSetini() const noexcept { return (words[16] >> 16) & 0xFFu; }
    [[nodiscard]] std::uint32_t spriteFound() const noexcept { return words[16] >> 24; }
    [[nodiscard]] std::uint32_t spriteFirst() const noexcept { return words[17] & 0xFFFFu; }
    [[nodiscard]] std::uint32_t paletteVersion() const noexcept { return words[17] >> 16; }
    [[nodiscard]] std::uint32_t oamVersion() const noexcept { return words[18]; }
    [[nodiscard]] std::uint32_t firstEvent() const noexcept { return words[19]; }
    [[nodiscard]] std::uint32_t eventCount() const noexcept { return words[20]; }
    [[nodiscard]] std::uint32_t lineNumber() const noexcept { return words[21]; }
    [[nodiscard]] SpriteSource spriteSource() const noexcept { return static_cast<SpriteSource>(words[22]); }
    [[nodiscard]] std::uint32_t spriteBuffer() const noexcept { return words[23]; }
    [[nodiscard]] std::uint32_t inRange(unsigned nth) const noexcept {
      return (words[24u + nth / 4u] >> (8u * (nth % 4u))) & 0xFFu;
    }
  };

  // A write that changes a register a line's dots read, or a palette word, inside
  // the line's dots 3 to 277. `at` is the picture line in the low half and the first
  // dot the write reaches in the high: the dot after its cycle ends, every dot up to
  // and including that cycle's last having been drawn under what it replaces. The
  // early read of $2100 makes two events at successive dots: the byte the data bus
  // held before the write, at the write cycle's last dot, then the written value at
  // the next. `change` is the Field in the low byte and the value it holds after the
  // write in the high half: a byte register's value in the low eight bits, a scroll
  // or Mode 7 word whole. A PaletteWord's change holds the word's index in bits 8 to
  // 15 and its fifteen-bit color in the high half.
  struct Event {
    std::uint32_t at = 0;
    std::uint32_t change = 0;

    [[nodiscard]] std::uint32_t line() const noexcept { return at & 0xFFFFu; }
    [[nodiscard]] std::uint32_t dot() const noexcept { return at >> 16; }
    [[nodiscard]] Field field() const noexcept { return static_cast<Field>(change & 0xFFu); }
    [[nodiscard]] std::uint32_t value() const noexcept { return change >> 16; }
    [[nodiscard]] std::uint32_t paletteIndex() const noexcept { return (change >> 8) & 0xFFu; }
  };

  // A write that reached VRAM inside the frame's picture lines, horizontal blanks
  // included, which happens only under forced blank. `at` is placed as an event's
  // is; `change` is the byte's address in the 64 KB in the low half and the byte in
  // the high. A write that stores the byte already there is a write all the same.
  struct VramWrite {
    std::uint32_t at = 0;
    std::uint32_t change = 0;

    [[nodiscard]] std::uint32_t line() const noexcept { return at & 0xFFFFu; }
    [[nodiscard]] std::uint32_t dot() const noexcept { return at >> 16; }
    [[nodiscard]] std::uint32_t address() const noexcept { return change & 0xFFFFu; }
    [[nodiscard]] std::uint32_t value() const noexcept { return change >> 16; }
  };

  static_assert(sizeof(Line) == 128u, "a line is 32 words");
  static_assert(sizeof(Event) == 8u, "an event is two words");
  static_assert(sizeof(VramWrite) == 8u, "a VRAM write is two words");

  // The frame: as many picture lines as the frame's VideoFrame holds, 512 wide when
  // a dot of a recorded line was drawn in half-pixels and 256 otherwise, and the
  // parity it ran under. `firstLine` is the first line recorded: 1 for a frame
  // recorded whole, or the line after the beam's where a restore, a reset or the
  // observer being set came part-way down the frame, the beam standing past that
  // line's first read dot, so `lines` holds height - firstLine + 1 lines.
  unsigned height = 0;
  unsigned width = 0;
  std::uint8_t field = 0;
  unsigned firstLine = 0;

  // The records, each in position order: a line's events stand together at its
  // firstEvent.
  std::span<const Line> lines;
  std::span<const Event> events;
  std::span<const VramWrite> vramWrites;

  // VRAM, kVramBytes, as it stood before the first of `vramWrites` landed, or as
  // the picture lines left it where none did. The palette's versions, kPaletteBytes
  // each, the first being the palette as the first recorded line's first read dot
  // found it and each after it the palette a later line's first read dot found
  // changed. The sprite table's, kOamBytes each, the same at Time's run as each line
  // began. And the buffers a Carried line is drawn with, kSpriteBufferBytes each:
  // the palette word at each of the line's 256 positions, then the priority at
  // each, kNoSprite where no sprite claims it.
  std::span<const std::uint8_t> vram;
  std::span<const std::uint8_t> palettes;
  std::span<const std::uint8_t> oams;
  std::span<const std::uint8_t> spriteBuffers;

  // The frame drawn from the timeline alone by the chip's own dot code, four bytes
  // a half-pixel — red, green, blue, then 255 — into `pixels`, kDrawBytes long: row
  // `line - 1` for picture line `line`, 512 half-pixels a row, position x's left
  // half at column 2x and its right half at 2x + 1, a position drawn whole filling
  // both with its one pixel. Rows before firstLine - 1 and from `height` on are left
  // as they were, as the machine leaves the rows its raster holds for them. A span
  // shorter than kDrawBytes is left as it was. A VideoFrame 512 wide is these rows
  // column for column; one 256 wide is them with each pixel taken once from its
  // position's two equal halves.
  void draw(std::span<std::uint8_t> pixels) const;
};

// What a host that draws the picture itself is told: the timeline of every frame
// the machine finishes. It arrives on the thread the machine is stepped on, from
// inside step() or run(), at the end of the cycle in which the frame's vertical
// blank begins, after the frame report and before the save report. The spans are
// the machine's own storage and are valid for the call. It may throw, on the terms
// SaveObserver::changed states (`snes.h`).
class PpuTimelineObserver {
 public:
  virtual ~PpuTimelineObserver() = default;
  virtual void frame(const PpuTimeline& timeline) = 0;
};

}  // namespace snaggletooth
