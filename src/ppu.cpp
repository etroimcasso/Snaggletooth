#include "snaggletooth/snes/ppu.h"

#include <cstddef>

namespace snaggletooth {
namespace {

// The write-only registers a read of which answers with the chip's first-half
// open bus rather than the CPU's: $2104-$2106, $2108-$210A, and the same
// offsets in the next two groups of sixteen.
[[nodiscard]] bool readsPpu1Bus(std::uint16_t offset) noexcept {
  if (offset >= 0x2130u) return false;
  const std::uint8_t low = static_cast<std::uint8_t>(offset & 0x0Fu);
  return (low >= 0x4u && low <= 0x6u) || (low >= 0x8u && low <= 0xAu);
}

// The byte the converter drives for one five-bit channel at brightness N: the
// exact rational value * (N + 1) * 255 / (31 * 16), rounded once, in integers.
constexpr unsigned kFullScale = 31u * 16u;
[[nodiscard]] std::uint8_t channelByte(unsigned value, unsigned brightness) noexcept {
  return static_cast<std::uint8_t>((value * (brightness + 1u) * 255u + kFullScale / 2u) /
                                   kFullScale);
}

// The character data's shape: eight rows of a bitplane fill eight words, so a pair
// of bitplanes takes sixteen bytes and the next pair begins sixteen bytes on.
constexpr std::size_t kPlanePairBytes = 16u;

// A sprite's character is always sixteen colours, so four bitplanes, and a
// character is eight pixels on a side.
constexpr unsigned kSpritePlanes = 4u;
constexpr unsigned kCharacterSide = 8u;

// The two sizes $2101 bits 7-5 name, the sprite's own flag choosing between
// them. The last two pairs are printed by both register documents and called
// undocumented by both.
struct SizePair {
  unsigned smallWidth;
  unsigned smallHeight;
  unsigned largeWidth;
  unsigned largeHeight;
};
constexpr std::array<SizePair, 8> kSpriteSizes{{
    {.smallWidth = 8u, .smallHeight = 8u, .largeWidth = 16u, .largeHeight = 16u},
    {.smallWidth = 8u, .smallHeight = 8u, .largeWidth = 32u, .largeHeight = 32u},
    {.smallWidth = 8u, .smallHeight = 8u, .largeWidth = 64u, .largeHeight = 64u},
    {.smallWidth = 16u, .smallHeight = 16u, .largeWidth = 32u, .largeHeight = 32u},
    {.smallWidth = 16u, .smallHeight = 16u, .largeWidth = 64u, .largeHeight = 64u},
    {.smallWidth = 32u, .smallHeight = 32u, .largeWidth = 64u, .largeHeight = 64u},
    {.smallWidth = 16u, .smallHeight = 32u, .largeWidth = 32u, .largeHeight = 64u},
    {.smallWidth = 16u, .smallHeight = 32u, .largeWidth = 32u, .largeHeight = 32u},
}};

// Which of a sprite's own rows a picture line crosses, or nothing where it
// misses. A sprite whose Y is N first draws on line N + 1, because the console
// renders a line it does not output; the subtraction is eight bits wide, which
// is what brings a tall sprite hung above the picture back in at the top. At
// half height the lines in are doubled after that subtraction and the field
// added, so each line shows one row of the pair it covers. A sprite's height is
// always even, so the field never changes which lines it stands on.
[[nodiscard]] std::optional<unsigned> rowCrossed(unsigned y, unsigned height, std::uint16_t line,
                                                 bool halfHeight, std::uint8_t field) noexcept {
  const unsigned into = (line - 1u - y) & 0xFFu;
  const unsigned row = halfHeight ? 2u * into + (field & 1u) : into;
  if (row >= height) return std::nullopt;
  return row;
}

// Where Range and Time count a sprite. Nine bits of X reach one position that is
// the picture's left edge a whole screen away, and the chip counts a sprite
// standing there as though it stood at the edge itself — while drawing it where
// its own X puts it, which is off the picture altogether.
[[nodiscard]] int countedX(int x) noexcept {
  return x == -static_cast<int>(kPictureWidth) ? 0 : x;
}

// A 256-colour background's pixel read as a colour rather than as a palette
// index: the pixel is BBGGGRRR and its tile's three palette bits are bgr, and
// each channel takes its own field shifted up with the tile's own bit under it.
// Every value it can make is even, and the low bits it cannot reach are zero.
[[nodiscard]] std::uint16_t directColour(unsigned pixel, unsigned palette) noexcept {
  const unsigned red = ((pixel & 0x07u) << 2) | ((palette & 0x01u) << 1);
  const unsigned green = (((pixel >> 3) & 0x07u) << 2) | (palette & 0x02u);
  const unsigned blue = (((pixel >> 6) & 0x03u) << 3) | (palette & 0x04u);
  return static_cast<std::uint16_t>(red | (green << 5) | (blue << 10));
}

// A Mode 7 register's thirteen-bit signed value: bit 12 is the sign.
[[nodiscard]] std::int32_t signed13(std::uint16_t reg) noexcept {
  const auto low = static_cast<std::int32_t>(reg & 0x1FFFu);
  return (low & 0x1000) != 0 ? low - 0x2000 : low;
}

// The scroll less the centre as the matrix takes it: the difference's low ten
// bits under the difference's own sign, so 1024 is 0 and -1025 is -1.
[[nodiscard]] std::int32_t clippedOffset(std::int32_t difference) noexcept {
  return (difference & 0x2000) != 0 ? (difference | ~0x3FF) : (difference & 0x3FF);
}

// A product of a matrix term with an offset or a line, with the low six bits the
// chip drops before it sums them.
[[nodiscard]] std::int32_t truncated(std::int32_t product) noexcept { return product & ~63; }

// A Mode 7 field coordinate's integer part; the field is 1024 pixels on a side
// and a position past it in either direction has bits above the tenth set.
[[nodiscard]] std::int32_t fieldInteger(std::int32_t coordinate) noexcept {
  return coordinate >> 8;
}
[[nodiscard]] bool outsideField(std::int32_t integer) noexcept {
  return (integer & ~0x3FF) != 0;
}

// The chip drops the low three bits of each product it puts on the multiplier's
// ports while Mode 7 draws, and the ports are twenty-four bits wide.
[[nodiscard]] std::int32_t scheduled(std::int32_t product) noexcept {
  const std::int32_t low24 = (product >> 3) & 0xFFFFFF;
  return (low24 & 0x800000) != 0 ? low24 - 0x1000000 : low24;
}

// The row a vertical flip shows in that row's place. A flip reverses the whole
// sprite rather than its characters — except that a rectangular sprite flips as
// though it were two square sprites stacked, so rows "01234567" become
// "32107654" and not "76543210".
[[nodiscard]] unsigned flipRow(unsigned row, unsigned width) noexcept {
  return (row / width) * width + (width - 1u - row % width);
}

}  // namespace

unsigned Ppu::tileWidth(const Background& background) noexcept {
  return background.hires || !background.large ? 8u : 16u;
}

unsigned Ppu::tileHeight(const Background& background) noexcept {
  return background.large ? 16u : 8u;
}

std::int32_t PpuState::multiplyResult() const noexcept {
  const std::int32_t a = static_cast<std::int16_t>(m7a);
  const std::int32_t b = static_cast<std::int8_t>(m7bByte);
  return a * b;
}

// ---- the memories' windows ---------------------------------------------------

bool Ppu::inVblankWindow(const PpuInputs& in) const noexcept {
  // Vertical blank opens the memories — except when the taller picture was asked for
  // after the blank had already begun. That resumes nothing the blank stopped, but the
  // chip holds its memories as though it were still drawing, to the line the taller
  // picture ends on.
  return in.vblank && !(overscanLate(in));
}

bool Ppu::overscanLate(const PpuInputs& in) const noexcept {
  return s_.overscan() && in.vpos < kOverscanVblankStartLine;
}

bool Ppu::vramReachable(const PpuInputs& in) const noexcept {
  return inVblankWindow(in) || s_.forcedBlank();
}
bool Ppu::oamReachable(const PpuInputs& in) const noexcept {
  return inVblankWindow(in) || s_.forcedBlank();
}
bool Ppu::cgramReachable(const PpuInputs& in) const noexcept {
  return inVblankWindow(in) || in.hblank || s_.forcedBlank();
}

// ---- the VRAM port -----------------------------------------------------------

std::uint16_t Ppu::vramWordAddress() const noexcept {
  // The address translation left-rotates the low 8, 9 or 10 bits of the word address
  // by three, so a bitmap laid out by increasing tile number reads back as rows.
  const std::uint16_t addr = s_.vmadd;
  switch ((s_.vmain >> 2) & 3u) {
    case 1: return static_cast<std::uint16_t>((addr & 0xFF00u) | ((addr << 3) & 0x00F8u) | ((addr >> 5) & 0x0007u));
    case 2: return static_cast<std::uint16_t>((addr & 0xFE00u) | ((addr << 3) & 0x01F8u) | ((addr >> 6) & 0x0007u));
    case 3: return static_cast<std::uint16_t>((addr & 0xFC00u) | ((addr << 3) & 0x03F8u) | ((addr >> 7) & 0x0007u));
    default: return addr;
  }
}

std::uint16_t Ppu::readVramWord() const noexcept {
  const std::uint16_t word = vramWordAddress();
  const std::size_t byte = static_cast<std::size_t>(word) << 1;
  return static_cast<std::uint16_t>(s_.vram[byte & 0xFFFFu] | (s_.vram[(byte + 1u) & 0xFFFFu] << 8));
}

void Ppu::stepVramAddress(bool highByte) noexcept {
  // The increment happens after the low or the high byte, whichever $2115 bit 7
  // selects — so an access to the other byte leaves the address alone.
  const bool incrementOnHigh = (s_.vmain & 0x80u) != 0u;
  if (highByte != incrementOnHigh) return;
  static constexpr std::uint16_t kStep[4] = {1u, 32u, 128u, 128u};
  s_.vmadd = static_cast<std::uint16_t>(s_.vmadd + kStep[s_.vmain & 3u]);
}

// ---- the sprite-table port ---------------------------------------------------

void Ppu::reloadOamAddress() noexcept {
  s_.oamAddress = static_cast<std::uint16_t>((s_.oamadd & 0x1FFu) << 1);
}

void Ppu::beginVblank() noexcept {
  // The PPU, done drawing, takes the OAM address back to the reload value — but
  // not in forced blank, when it was not drawing.
  if (!s_.forcedBlank()) reloadOamAddress();
}

// ---- the frame's start -------------------------------------------------------

void Ppu::beginFrame() noexcept {
  // The overflow flags belong to the picture the chip has just finished, and it
  // clears them as it starts the next one. In forced blank it drew nothing, so
  // whatever they hold stands.
  if (s_.forcedBlank()) return;
  s_.rangeOver = false;
  s_.timeOver = false;
}

// ---- the sprite passes -------------------------------------------------------

Ppu::Sprite Ppu::spriteAt(std::uint8_t index) const noexcept {
  // Four bytes in the low table — X's low eight bits, Y, the first character,
  // then vhoopppN — and two bits in the high table, which holds four sprites to
  // a byte from the low pair up: the ninth bit of X, then the size flag.
  const std::size_t at = static_cast<std::size_t>(index) * 4u;
  const std::uint8_t high = s_.oam[512u + (index >> 2)];
  const unsigned bits = (high >> ((index & 3u) * 2u)) & 3u;

  // Nine bits of X read as signed, so a sprite can stand off the left edge.
  const unsigned wide = s_.oam[at] | ((bits & 1u) << 8);
  const SizePair& sizes = kSpriteSizes[(s_.objsel >> 5) & 7u];
  const bool large = (bits & 2u) != 0u;
  return Sprite{
      .x = static_cast<int>(wide) - (wide >= 256u ? 512 : 0),
      .y = s_.oam[at + 1u],
      .tile = s_.oam[at + 2u],
      .attributes = s_.oam[at + 3u],
      .width = large ? sizes.largeWidth : sizes.smallWidth,
      .height = large ? sizes.largeHeight : sizes.smallHeight,
  };
}

std::size_t Ppu::spriteCharacter(const Sprite& sprite, unsigned column,
                                 unsigned row) const noexcept {
  // The number's low nibble is the character's column in the 16x16 table and
  // its high nibble the row, and each wraps inside its own nibble: a 16x16
  // sprite whose first character is $FF is made of $FF, $F0, $0F and $00.
  const unsigned number = ((sprite.tile + column / kCharacterSide) & 0x0Fu) |
                          ((((sprite.tile >> 4) + row / kCharacterSide) & 0x0Fu) << 4);

  // The word address: the base $2101 bits 2-0 name, the character sixteen words
  // on from the last, and the second table the gap the Name bits choose past the
  // first — a gap of none putting the second table immediately after it.
  const unsigned base = s_.objsel & 0x07u;
  const unsigned name = (s_.objsel >> 3) & 0x03u;
  const bool secondTable = (sprite.attributes & 0x01u) != 0u;
  const unsigned word =
      ((base << 13) + (number << 4) + (secondTable ? ((name + 1u) << 12) : 0u)) & 0x7FFFu;
  return static_cast<std::size_t>(word) * 2u;
}

std::uint8_t Ppu::firstSprite(std::uint16_t line) const noexcept {
  // Without $2103's top bit the walk begins at sprite 0, whatever the port is
  // doing.
  if ((s_.oamadd & 0x8000u) == 0u) return 0u;

  // With it, the sprite the port's own address stands in — the address counts
  // bytes and a record is four of them. Standing on the last byte of a record it
  // carries the line the pass is matching as well, which is the line before the
  // one the sprites it finds will draw on.
  unsigned sprite = static_cast<unsigned>(s_.oamAddress) >> 2;
  if ((s_.oamAddress & 0x03u) == 0x03u) sprite += static_cast<unsigned>(line) - 1u;
  return static_cast<std::uint8_t>(sprite & 0x7Fu);
}

void Ppu::beginRange(std::uint16_t line) noexcept {
  s_.sprites.scanned = 0u;
  s_.sprites.found = 0u;
  s_.sprites.first = firstSprite(line);
}

// ---- mosaic ------------------------------------------------------------------

void Ppu::beginLine(std::uint16_t line) noexcept {
  // The counter reloads for the picture's first line and counts every line after,
  // and a row of blocks runs for the size it began with before the register's size
  // is read again.
  const unsigned current = s_.mosaicBlockLine;
  const bool rowEnded = line < current || line - current > s_.mosaicBlockSize;
  if (line == 1u || rowEnded) {
    s_.mosaicBlockLine = line;
    s_.mosaicBlockSize = static_cast<std::uint8_t>(s_.mosaic >> 4);
  }
}

std::uint16_t Ppu::mosaicIndex(std::uint16_t line) const noexcept {
  const unsigned current = s_.mosaicBlockLine;
  return static_cast<std::uint16_t>(line < current ? 0u : line - current);
}

Ppu::Position Ppu::mosaicPosition(Layer layer, std::uint16_t x, const PpuInputs& in,
                                  bool half) const noexcept {
  const unsigned index = static_cast<unsigned>(layer);
  const std::uint8_t mode = s_.bgmode & 0x07u;
  const std::uint16_t line = in.vpos;
  bool across = ((s_.mosaic >> index) & 0x01u) != 0u;
  bool down = across;
  if (mode == 7u && layer == Layer::Bg2) {
    // The second Mode 7 layer's two axes each have a bit of their own.
    down = (s_.mosaic & 0x01u) != 0u;
    across = (s_.mosaic & 0x02u) != 0u;
  }
  const unsigned width = (s_.mosaic >> 4) + 1u;
  const bool halfLines = (mode == 5u || mode == 6u) && s_.interlace();
  const unsigned corner = down ? static_cast<unsigned>(line) - mosaicIndex(line) : line;
  unsigned readLine = corner;
  if (halfLines) readLine = down ? 2u * corner : 2u * static_cast<unsigned>(line) + (in.field & 1u);
  const auto read = static_cast<std::uint16_t>(readLine);
  if (!across) return Position{.x = x, .line = read, .half = half};
  if (mode == 5u || mode == 6u) {
    // Counted in half-pixels: the corner is a multiple of twice the size, which is
    // always a left half.
    const unsigned halfPixel = 2u * x + (half ? 1u : 0u);
    const unsigned blockCorner = halfPixel - halfPixel % (2u * width);
    return Position{.x = static_cast<std::uint16_t>(blockCorner / 2u), .line = read, .half = false};
  }
  return Position{.x = static_cast<std::uint16_t>(x - x % width), .line = read, .half = half};
}

void Ppu::rangeSprite(std::uint16_t line) noexcept {
  // A chip in forced blank is rendering nothing, so it walks no further along
  // OAM and the pass stands where the blank found it.
  if (s_.forcedBlank()) return;
  if (static_cast<unsigned>(s_.sprites.scanned) >= kSprites) return;

  // The sprite this dot belongs to: the walk counts from the sprite it began at
  // and wraps past the last, and reads $2101 as it stands at this dot.
  const auto index = static_cast<std::uint8_t>(
      (static_cast<unsigned>(s_.sprites.first) + s_.sprites.scanned) & 0x7Fu);
  ++s_.sprites.scanned;
  const Sprite sprite = spriteAt(index);

  const bool halfHeight = (s_.setini & 0x02u) != 0u;
  if (!rowCrossed(sprite.y, sprite.height, line, halfHeight, 0u).has_value()) return;
  if (countedX(sprite.x) <= -static_cast<int>(sprite.width)) {
    return;  // nothing of it stands on the picture
  }

  // Range keeps so many of them and no more; the sprite that is one too many
  // raises its flag here, at its own dot.
  if (static_cast<unsigned>(s_.sprites.found) >= kSpritesPerLine) {
    s_.rangeOver = true;
    return;
  }
  s_.sprites.inRange[s_.sprites.found] = index;
  ++s_.sprites.found;
}

void Ppu::timeSprites(std::uint16_t line, std::uint8_t field) noexcept {
  if (s_.forcedBlank()) return;
  const bool halfHeight = (s_.setini & 0x02u) != 0u;

  s_.sprites.line = line;
  s_.sprites.word.fill(0u);
  s_.sprites.priority.fill(kNoSprite);

  // From the last sprite Range found backwards, so the sprite nearest the front
  // writes last and keeps every position it claims — its priority with it, which
  // is why a sprite in front at priority 0 hides one behind it at priority 3 from
  // a background that shows above priority 0.
  unsigned tiles = 0u;
  for (unsigned nth = s_.sprites.found; nth-- > 0u;) {
    const Sprite sprite = spriteAt(s_.sprites.inRange[nth]);
    const std::optional<unsigned> crossed =
        rowCrossed(sprite.y, sprite.height, line, halfHeight, field);
    if (!crossed.has_value()) continue;

    const bool flipVertical = (sprite.attributes & 0x80u) != 0u;
    const bool flipHorizontal = (sprite.attributes & 0x40u) != 0u;
    const unsigned row = flipVertical ? flipRow(*crossed, sprite.width) : *crossed;
    const auto priority = static_cast<std::uint8_t>((sprite.attributes >> 4) & 0x03u);
    const unsigned palette = (sprite.attributes >> 1) & 0x07u;
    const int counted = countedX(sprite.x);

    // A sprite's row is loaded a tile at a time, left to right. Time has so many
    // tiles to spend on a line and spends them only on tiles that stand on the
    // picture; the tile that is one too many raises its flag and is not loaded,
    // and neither is anything behind it in the walk.
    for (unsigned across = 0u; across < sprite.width; across += kCharacterSide) {
      const int tileX = counted + static_cast<int>(across);
      if (tileX <= -static_cast<int>(kCharacterSide) ||
          tileX >= static_cast<int>(kPictureWidth)) {
        continue;
      }
      if (tiles >= kSpriteTilesPerLine) {
        s_.timeOver = true;
        continue;
      }
      ++tiles;

      for (unsigned column = across; column < across + kCharacterSide; ++column) {
        const int x = sprite.x + static_cast<int>(column);
        if (x < 0 || x >= static_cast<int>(kPictureWidth)) continue;

        // A horizontal flip reverses the whole sprite, so the leftmost position
        // takes the rightmost of its pixels.
        const unsigned source = flipHorizontal ? sprite.width - 1u - column : column;
        const std::size_t character = spriteCharacter(sprite, source, row);
        const std::size_t at = (character + (row % kCharacterSide) * 2u) & 0xFFFFu;
        const unsigned bit = kCharacterSide - 1u - source % kCharacterSide;
        unsigned index = 0u;
        for (unsigned plane = 0u; plane < kSpritePlanes; ++plane) {
          const std::size_t byteAt =
              (at + (plane / 2u) * kPlanePairBytes + (plane % 2u)) & 0xFFFFu;
          index |= ((s_.vram[byteAt] >> bit) & 1u) << plane;
        }
        if (index == 0u) continue;  // colour 0 of a sprite's palette is transparent too

        // Eight sixteen-colour palettes, beginning at CGRAM word 128.
        s_.sprites.word[static_cast<std::size_t>(x)] =
            static_cast<std::uint8_t>(128u + palette * 16u + index);
        s_.sprites.priority[static_cast<std::size_t>(x)] = priority;
      }
    }
  }
}

// ---- the picture -------------------------------------------------------------

std::array<std::uint8_t, 4> Ppu::convert(std::uint16_t colour) const noexcept {
  // A palette word is five bits a channel, blue then green then red from the top,
  // and the converter scales each by (N + 1) / 16 for INIDISP's brightness N. A
  // brightness of zero is the screen off, which is black whatever the word holds.
  const unsigned brightness = s_.inidisp & 0x0Fu;
  if (brightness == 0u) return {0u, 0u, 0u, 255u};
  return {channelByte(colour & 0x1Fu, brightness),
          channelByte((colour >> 5) & 0x1Fu, brightness),
          channelByte((colour >> 10) & 0x1Fu, brightness), 255u};
}

Ppu::Background Ppu::registersOf(Layer layer) const noexcept {
  // Each background keeps a screen register, a nibble of a character-base
  // register — BG1 and BG3 the low one, BG2 and BG4 the high one above it — a pair
  // of offsets, and one of $2105's four size bits, from bit 4 up in the order the
  // backgrounds are numbered. Modes 5 and 6 make every tilemap's tiles two
  // characters wide, BG3's offset table included.
  const unsigned index = static_cast<unsigned>(layer);
  static constexpr std::uint8_t PpuState::*kScreens[4] = {&PpuState::bg1sc, &PpuState::bg2sc,
                                                          &PpuState::bg3sc, &PpuState::bg4sc};
  const std::uint8_t bases = index < 2u ? s_.bg12nba : s_.bg34nba;
  const std::uint8_t mode = s_.bgmode & 0x07u;
  return Background{.layer = layer,
                    .screen = s_.*kScreens[index],
                    .characterBase = static_cast<std::uint8_t>((index & 1u) ? (bases >> 4)
                                                                            : (bases & 0x0Fu)),
                    .large = (s_.bgmode & (0x10u << index)) != 0u,
                    .hires = mode == 5u || mode == 6u,
                    .planes = 0u,
                    .paletteStride = 0u,
                    .wordBase = 0u,
                    .offsetBit = 0u};
}

Ppu::Offsets Ppu::scrollOf(Layer layer) const noexcept {
  // A background's two offset registers, read where they are asked for rather than
  // carried in its description: a scroll written part-way along a line moves the
  // positions after the write, which is how a transfer parallaxes a picture.
  const unsigned index = static_cast<unsigned>(layer);
  static constexpr std::uint16_t PpuState::*kHorizontal[4] = {
      &PpuState::bg1hofs, &PpuState::bg2hofs, &PpuState::bg3hofs, &PpuState::bg4hofs};
  static constexpr std::uint16_t PpuState::*kVertical[4] = {
      &PpuState::bg1vofs, &PpuState::bg2vofs, &PpuState::bg3vofs, &PpuState::bg4vofs};
  return Offsets{.horizontal = s_.*kHorizontal[index], .vertical = s_.*kVertical[index]};
}

const Ppu::Derived::Descriptions& Ppu::descriptions() const noexcept {
  // The four backgrounds and the chart as $2105-$210C and $2133 stand. Every write
  // to one of those drops this, so the first dot after such a write reads the
  // registers again and every dot until the next one reads what it found.
  Derived::Descriptions& kept = d_.descriptions;
  if (kept.valid) return kept;
  for (unsigned index = 0u; index < kept.registers.size(); ++index) {
    const auto layer = static_cast<Layer>(index);
    kept.registers[index] = registersOf(layer);
    kept.described[index] = background(layer);
  }
  kept.chart = order();
  kept.field = (s_.bgmode & 0x07u) == 7u;
  kept.valid = true;
  return kept;
}

std::optional<Ppu::Background> Ppu::background(Layer layer) const noexcept {
  // What the mode makes of a background: how deep its characters are, and where in
  // the palette its colours are read. A sixteen-colour background's palette field
  // steps sixteen words and a four-colour one's steps four; Mode 0 gives each of
  // its four thirty-two words of its own, one subset after another, which is the
  // only mode that offsets a background at all; and a 256-colour background has no
  // palette field, its pixel being the word itself.
  struct Depth {
    unsigned planes;
    unsigned paletteStride;
    unsigned wordBase;
  };
  static constexpr Depth kNone{.planes = 0u, .paletteStride = 0u, .wordBase = 0u};
  static constexpr Depth kFour{.planes = 2u, .paletteStride = 4u, .wordBase = 0u};
  static constexpr Depth kSixteen{.planes = 4u, .paletteStride = 16u, .wordBase = 0u};
  static constexpr Depth kFullPalette{.planes = 8u, .paletteStride = 0u, .wordBase = 0u};

  const unsigned index = static_cast<unsigned>(layer);
  Depth depth = kNone;
  switch (s_.bgmode & 0x07u) {
    case 0u:
      // Four four-colour backgrounds, each reading the thirty-two words after the
      // last one's.
      depth = Depth{.planes = kFour.planes,
                    .paletteStride = kFour.paletteStride,
                    .wordBase = index * 32u};
      break;
    case 1u:
      depth = index < 2u ? kSixteen : (index == 2u ? kFour : kNone);
      break;
    case 2u:
      // BG3's tilemap is the offset table, so the mode draws two backgrounds.
      depth = index < 2u ? kSixteen : kNone;
      break;
    case 3u:
      depth = index == 0u ? kFullPalette : (index == 1u ? kSixteen : kNone);
      break;
    case 4u:
      depth = index == 0u ? kFullPalette : (index == 1u ? kFour : kNone);
      break;
    case 5u:
      depth = index == 0u ? kSixteen : (index == 1u ? kFour : kNone);
      break;
    case 6u:
      // BG3's tilemap is the offset table, so the mode draws one background.
      depth = index == 0u ? kSixteen : kNone;
      break;
    case 7u:
      break;  // the field, which sampleField reads
  }
  if (depth.planes == 0u) return std::nullopt;

  // The three offset-per-tile modes read their backgrounds through BG3's table,
  // each background under its own bit of an entry.
  const std::uint8_t mode = s_.bgmode & 0x07u;
  const bool offsetTable = mode == 2u || mode == 4u || mode == 6u;

  Background described = registersOf(layer);
  described.planes = depth.planes;
  described.paletteStride = depth.paletteStride;
  described.wordBase = depth.wordBase;
  described.offsetBit = offsetTable ? static_cast<std::uint16_t>(0x2000u << index) : 0u;
  return described;
}

std::span<const Ppu::Place> Ppu::order() const noexcept {
  // Each mode's chart, front to back, exactly as the priority table gives it. A
  // background appears twice, once for each value of its tiles' priority bit; a
  // sprite appears four times, once for each of its own priorities.
  static constexpr Layer kBg1 = Layer::Bg1;
  static constexpr Layer kBg2 = Layer::Bg2;
  static constexpr Layer kBg3 = Layer::Bg3;
  static constexpr Layer kBg4 = Layer::Bg4;
  static constexpr Layer kObj = Layer::Object;
  static constexpr Place kModeZero[12] = {
      {kObj, 3u}, {kBg1, 1u}, {kBg2, 1u}, {kObj, 2u}, {kBg1, 0u}, {kBg2, 0u},
      {kObj, 1u}, {kBg3, 1u}, {kBg4, 1u}, {kObj, 0u}, {kBg3, 0u}, {kBg4, 0u}};
  static constexpr Place kModeOne[10] = {
      {kObj, 3u}, {kBg1, 1u}, {kBg2, 1u}, {kObj, 2u},  {kBg1, 0u},
      {kBg2, 0u}, {kObj, 1u}, {kBg3, 1u}, {kObj, 0u},  {kBg3, 0u}};
  // $2105 bit 3 takes BG3's high tiles out of their place and puts them in front
  // of everything. It is Mode 1's bit and names no place in any other chart.
  static constexpr Place kModeOneLifted[10] = {
      {kBg3, 1u}, {kObj, 3u}, {kBg1, 1u}, {kBg2, 1u}, {kObj, 2u},
      {kBg1, 0u}, {kBg2, 0u}, {kObj, 1u}, {kObj, 0u}, {kBg3, 0u}};
  static constexpr Place kModeThree[8] = {{kObj, 3u},  {kBg1, 1u}, {kObj, 2u}, {kBg2, 1u},
                                          {kObj, 1u},  {kBg1, 0u}, {kObj, 0u}, {kBg2, 0u}};
  // Mode 7's field has one place and no priority bit. SETINI bit 6 adds the second
  // layer it makes of the same pixels, whose priority is the pixel's own bit 7.
  static constexpr Place kModeSeven[5] = {{kObj, 3u}, {kObj, 2u}, {kObj, 1u}, {kBg1, 0u}, {kObj, 0u}};
  static constexpr Place kModeSevenExtended[7] = {{kObj, 3u}, {kObj, 2u}, {kBg2, 1u}, {kObj, 1u},
                                                  {kBg1, 0u}, {kObj, 0u}, {kBg2, 0u}};
  // Mode 6 is mode 3's chart with BG2's two places taken out.
  static constexpr Place kModeSix[6] = {{kObj, 3u}, {kBg1, 1u}, {kObj, 2u},
                                        {kObj, 1u}, {kBg1, 0u}, {kObj, 0u}};

  switch (s_.bgmode & 0x07u) {
    case 0u: return kModeZero;
    case 1u: return (s_.bgmode & 0x08u) != 0u ? std::span<const Place>(kModeOneLifted)
                                              : std::span<const Place>(kModeOne);
    // Modes 2, 4 and 5 keep mode 3's chart: two backgrounds, BG1 before BG2 at
    // each priority, and $2105 bit 3 naming no place.
    case 2u:
    case 3u:
    case 4u:
    case 5u: return kModeThree;
    case 6u: return kModeSix;
    default: break;
  }
  return (s_.setini & 0x40u) != 0u ? std::span<const Place>(kModeSevenExtended)
                                   : std::span<const Place>(kModeSeven);
}

std::uint16_t Ppu::entryAt(const Background& background, unsigned bgX,
                           unsigned bgY) const noexcept {
  const unsigned tileX = bgX / tileWidth(background);
  const unsigned tileY = bgY / tileHeight(background);

  // The base counts whole screens: a 32x32 screen is $400 words, so the six bits of
  // the register step the map in $400-word units and reach every 2 KB boundary of
  // the memory. A wide or tall map's further screens follow the first a screen
  // apart.
  const bool wide = (background.screen & 0x01u) != 0u;
  const bool tall = (background.screen & 0x02u) != 0u;
  unsigned word = (static_cast<unsigned>(background.screen >> 2) << 10) +
                  ((tileY & 0x1Fu) << 5) + (tileX & 0x1Fu);
  if (tall) word += (tileY & 0x20u) << (wide ? 6u : 5u);
  if (wide) word += (tileX & 0x20u) << 5;
  const std::size_t at = (static_cast<std::size_t>(word) << 1) & 0xFFFFu;
  return static_cast<std::uint16_t>(s_.vram[at] | (s_.vram[(at + 1u) & 0xFFFFu] << 8));
}

Ppu::Offsets Ppu::offsetsFor(const Background& background, std::uint16_t x) const noexcept {
  const Offsets scroll = scrollOf(background.layer);
  Offsets offsets{.horizontal = static_cast<std::uint16_t>(scroll.horizontal & 0x03FFu),
                  .vertical = static_cast<std::uint16_t>(scroll.vertical & 0x03FFu)};
  if (background.offsetBit == 0u) return offsets;

  // The background's own tile column at x. Its first, however little of it is on
  // the picture, reads the registers.
  const unsigned fine = offsets.horizontal & 0x07u;
  const unsigned column = x + fine;
  if (column < 8u) return offsets;

  // Tile T reads BG3's tile T - 1, counted from BG3's coarse scroll; the rows are
  // BG3's vertical offset and the row eight lines below it, and the line plays no
  // part. BG3's own tile size decides how many BG3 positions one entry covers.
  const Background& table = descriptions().registers[static_cast<unsigned>(Layer::Bg3)];
  const Offsets tableScroll = scrollOf(Layer::Bg3);
  const unsigned tableX = (column & ~7u) - 8u + (tableScroll.horizontal & 0x03F8u);
  const unsigned tableY = tableScroll.vertical & 0x03FFu;
  std::uint16_t horizontal = entryAt(table, tableX, tableY);
  std::uint16_t vertical = 0u;
  if ((s_.bgmode & 0x07u) == 4u) {
    // Mode 4 reads one entry, and bit 15 says which axis it is.
    if ((horizontal & 0x8000u) != 0u) {
      vertical = horizontal;
      horizontal = 0u;
    }
  } else {
    vertical = entryAt(table, tableX, tableY + 8u);
  }

  // A horizontal entry replaces the coarse scroll and keeps the register's fine
  // one, its own low three bits unread; a vertical entry replaces the register.
  if ((horizontal & background.offsetBit) != 0u) {
    offsets.horizontal = static_cast<std::uint16_t>((horizontal & 0x03F8u) | fine);
  }
  if ((vertical & background.offsetBit) != 0u) {
    offsets.vertical = static_cast<std::uint16_t>(vertical & 0x03FFu);
  }
  return offsets;
}

std::optional<Ppu::Shown> Ppu::sample(const Background& background, std::uint16_t x,
                                      std::uint16_t line, bool half) const noexcept {
  // Where the position falls in the background. The display never falls outside
  // the background: the tilemap lookup wraps it at the map's own size, whatever
  // that size is.
  const Offsets offsets = offsetsFor(background, x);
  const unsigned bgX = x + offsets.horizontal;
  const unsigned bgY = line + offsets.vertical;
  const std::uint16_t entry = entryAt(background, bgX, bgY);

  // The pixel within the tile. A two-character tile has sixteen pixels across its
  // eight positions: the position's left half reads the even one and its right
  // half the odd one.
  const unsigned width = tileWidth(background);
  const unsigned height = tileHeight(background);
  const unsigned across = background.hires ? 2u * width : width;
  unsigned inX = background.hires ? 2u * (bgX % width) + (half ? 1u : 0u) : bgX % width;
  unsigned inY = bgY % height;

  // The entry is vhopppcc cccccccc: the two flips, the tile's priority, its palette
  // and its number. A flip reverses the whole tile, a 16x16 block and a
  // two-character tile included.
  if ((entry & 0x4000u) != 0u) inX = across - 1u - inX;
  if ((entry & 0x8000u) != 0u) inY = height - 1u - inY;

  // A 16x16 block is Tile, Tile + 1, Tile + 16 and Tile + 17, and a two-character
  // tile is Tile and Tile + 1. The numbers run on rather than wrapping within the
  // block; only the ten-bit number itself wraps.
  unsigned tile = entry & 0x03FFu;
  if (inX >= 8u) {
    ++tile;
    inX -= 8u;
  }
  if (inY >= 8u) {
    tile += 16u;
    inY -= 8u;
  }
  tile &= 0x03FFu;

  // The character the tile names, under the base the layer's own nibble holds: eight
  // bytes a bitplane, so sixteen for a four-colour character and thirty-two for a
  // sixteen-colour one.
  const std::size_t character = ((static_cast<std::size_t>(background.characterBase) << 13) +
                                 tile * 8u * background.planes) &
                                0xFFFFu;
  const std::size_t row = (character + inY * 2u) & 0xFFFFu;

  // The planes, low bit first: 0 and 1 in the low and high bytes of the row's word,
  // then each further pair sixteen bytes on. The leftmost pixel of a row is bit 7.
  const unsigned bit = 7u - inX;
  unsigned index = 0u;
  for (unsigned plane = 0u; plane < background.planes; ++plane) {
    const std::size_t at =
        (row + (plane / 2u) * kPlanePairBytes + (plane % 2u)) & 0xFFFFu;
    index |= ((s_.vram[at] >> bit) & 1u) << plane;
  }
  if (index == 0u) return std::nullopt;  // colour 0 of any palette is transparent

  // The palette word: the first word this background's colours begin at, its
  // tile's palette field as many words on as that field steps, and the pixel. A
  // 256-colour background steps by none, so its pixel is the word.
  const unsigned palette = (entry >> 10) & 0x07u;
  const auto colourWord = static_cast<std::uint8_t>(
      background.wordBase + palette * background.paletteStride + index);
  const bool priority = (entry & 0x2000u) != 0u;

  // $2130 bit 0 reads a 256-colour background's pixel as a colour instead. The
  // condition is the depth, because the eight bits are what the composition is
  // made of — the modes that can do it are the modes with such a background. The
  // bit is read here, at the dot, like every other register.
  if (background.planes == 8u && (s_.cgwsel & 0x01u) != 0u) {
    return Shown{
        .word = colourWord, .priority = priority, .direct = directColour(index, palette)};
  }
  return Shown{.word = colourWord, .priority = priority, .direct = std::nullopt};
}

// ---- Mode 7's field ----------------------------------------------------------

Ppu::FieldPoint Ppu::fieldPoint(std::uint16_t x, std::uint16_t line) const noexcept {
  // The picture position, each axis XORed with $FF where its flip bit is set; the
  // line is the beam's own, 1 for the first line drawn.
  const std::int32_t sx = (s_.m7sel & 0x01u) != 0u ? (x ^ 0xFF) : x;
  const std::int32_t sy = (s_.m7sel & 0x02u) != 0u ? (line ^ 0xFF) : line;

  // The four matrix terms, 8.8 signed; the centre; and the scroll less the centre,
  // clipped. The centre is added back whole, as pixels.
  const std::int32_t a = static_cast<std::int16_t>(s_.m7a);
  const std::int32_t b = static_cast<std::int16_t>(s_.m7b);
  const std::int32_t c = static_cast<std::int16_t>(s_.m7c);
  const std::int32_t d = static_cast<std::int16_t>(s_.m7d);
  const std::int32_t centreX = signed13(s_.m7x);
  const std::int32_t centreY = signed13(s_.m7y);
  const std::int32_t ox = clippedOffset(signed13(s_.m7hofs) - centreX);
  const std::int32_t oy = clippedOffset(signed13(s_.m7vofs) - centreY);

  // Each product with the offset or the line loses its low six bits before the sum;
  // the product with the column is added whole, which is the same number the chip
  // reaches by stepping the matrix term along the line.
  return FieldPoint{
      .x = truncated(a * ox) + truncated(b * oy) + centreX * 256 + truncated(b * sy) + a * sx,
      .y = truncated(c * ox) + truncated(d * oy) + centreY * 256 + truncated(d * sy) + c * sx};
}

std::uint8_t Ppu::fieldPixel(FieldPoint at) const noexcept {
  // The pixel's integer position. $211A bit 7 clear wraps it into the field; set,
  // a position outside the field shows nothing or character 0, as bit 6 says.
  std::int32_t fieldX = fieldInteger(at.x);
  std::int32_t fieldY = fieldInteger(at.y);
  unsigned character = 0u;
  const bool outside = outsideField(fieldX) || outsideField(fieldY);
  if ((s_.m7sel & 0x80u) == 0u || !outside) {
    fieldX &= 0x3FF;
    fieldY &= 0x3FF;
    // The map is the low byte of the word the tile's row and column name, 128
    // entries a row.
    const std::size_t entryAt =
        static_cast<std::size_t>(((fieldY >> 3) << 7) | (fieldX >> 3)) << 1;
    character = s_.vram[entryAt];
  } else if ((s_.m7sel & 0x40u) == 0u) {
    return 0u;
  }
  // The character's pixel is the high byte of the word its row and column name,
  // sixty-four words a character.
  const std::size_t pixelAt =
      (static_cast<std::size_t>((character << 6) | ((fieldY & 7) << 3) | (fieldX & 7)) << 1) | 1u;
  return s_.vram[pixelAt];
}

std::optional<Ppu::Shown> Ppu::sampleField(Layer layer, std::uint16_t x,
                                           std::uint16_t line) const noexcept {
  const std::uint8_t pixel = fieldPixel(fieldPoint(x, line));
  if (layer == Layer::Bg2) {
    // The second layer: bit 7 is the pixel's priority and the low seven bits its
    // word, so it has 128 colours and never a composed one.
    const auto word = static_cast<std::uint8_t>(pixel & 0x7Fu);
    if (word == 0u) return std::nullopt;
    return Shown{.word = word, .priority = (pixel & 0x80u) != 0u, .direct = std::nullopt};
  }
  if (pixel == 0u) return std::nullopt;
  // The field's own layer: the byte is the word, or with $2130 bit 0 the colour
  // itself — with no tile bits, since this map has no palette field.
  if ((s_.cgwsel & 0x01u) != 0u) {
    return Shown{.word = pixel, .priority = false, .direct = directColour(pixel, 0u)};
  }
  return Shown{.word = pixel, .priority = false, .direct = std::nullopt};
}

bool Ppu::drawingModeSeven(const PpuInputs& in) const noexcept {
  return (s_.bgmode & 0x07u) == 7u && !s_.forcedBlank() && !in.vblank &&
         in.vpos < s_.vblankStartLine();
}

std::int32_t Ppu::multiplierWhileDrawing(const PpuInputs& in) const noexcept {
  const std::int32_t a = static_cast<std::int16_t>(s_.m7a);
  const std::int32_t b = static_cast<std::int16_t>(s_.m7b);
  const std::int32_t c = static_cast<std::int16_t>(s_.m7c);
  const std::int32_t d = static_cast<std::int16_t>(s_.m7d);
  const std::int32_t ox = clippedOffset(signed13(s_.m7hofs) - signed13(s_.m7x));
  const std::int32_t oy = clippedOffset(signed13(s_.m7vofs) - signed13(s_.m7y));
  // The line term is the line less BG1's mosaic index, under the vertical flip; the
  // column term is the dot less three, wrapped at 256, under the horizontal flip.
  const std::int32_t line =
      (s_.mosaic & 0x01u) != 0u ? in.vpos - mosaicIndex(in.vpos) : in.vpos;
  const std::int32_t sy = (s_.m7sel & 0x02u) != 0u ? (line ^ 0xFF) : line;
  const std::int32_t column = (in.hdot - 3) & 0xFF;
  const std::int32_t sx = (s_.m7sel & 0x01u) != 0u ? (column ^ 0xFF) : column;
  switch (in.hdot) {
    case 0u: return scheduled(in.lateHalf ? d * oy : a * ox);
    case 1u: return scheduled(in.lateHalf ? c * ox : b * oy);
    case 2u: return scheduled(in.lateHalf ? d * sy : b * sy);
    default: return scheduled(in.lateHalf ? c * sx : a * sx);
  }
}

bool Ppu::windowCovers(Layer layer, std::uint16_t x) const noexcept {
  // The three selectors hold a layer in each nibble — BG1, BG3 and OBJ in the low
  // one, BG2, BG4 and the colour window in the high — and the two logic registers
  // give every layer a pair of bits of its own, the four backgrounds filling one
  // register and the sprites and the colour window the low half of the other.
  const unsigned index = static_cast<unsigned>(layer);
  const std::uint8_t selector =
      index < 2u ? s_.w12sel : (index < 4u ? s_.w34sel : s_.wobjsel);
  const unsigned bits = (selector >> ((index & 1u) * 4u)) & 0x0Fu;
  const std::uint8_t logic = index < 4u ? s_.wbglog : s_.wobjlog;
  const unsigned op = (logic >> ((index & 3u) * 2u)) & 0x03u;

  // A window runs from its left edge to its right, both ends inclusive — so edges
  // that meet are one pixel wide, and a left edge past the right is a window with
  // no range at all. The layer's inversion bit replaces it with its inverse.
  const auto spans = [x](std::uint8_t left, std::uint8_t right) {
    return x >= left && x <= right;
  };
  const bool window1 = spans(s_.wh0, s_.wh1) != ((bits & 0x01u) != 0u);
  const bool window2 = spans(s_.wh2, s_.wh3) != ((bits & 0x04u) != 0u);

  const bool enable1 = (bits & 0x02u) != 0u;
  const bool enable2 = (bits & 0x08u) != 0u;
  if (!enable1) return enable2 && window2;
  if (!enable2) return window1;

  // Both enabled: the logic the layer's own pair of bits names. XNOR is the
  // inverse of XOR, which is the two agreeing.
  switch (op) {
    case 0u: return window1 || window2;   // OR
    case 1u: return window1 && window2;   // AND
    case 2u: return window1 != window2;   // XOR
    default: return window1 == window2;   // XNOR
  }
}

bool Ppu::masked(Layer layer, std::uint8_t maskRegister, std::uint16_t x) const noexcept {
  // The mask register's bits run BG1, BG2, BG3, BG4, OBJ, in the order the layers
  // are numbered, and it gates the mask rather than the window: a layer whose
  // windows are enabled but whose bit is clear is masked nowhere.
  const unsigned bit = 1u << static_cast<unsigned>(layer);
  return (maskRegister & bit) != 0u && windowCovers(layer, x);
}

std::optional<Ppu::Resolved> Ppu::resolve(Screen screen, std::uint16_t x,
                                          const PpuInputs& in, bool half) const noexcept {
  const std::uint16_t line = in.vpos;
  // The two screens differ in which register puts layers on them and which one
  // masks those layers. Everything below is the same for both.
  const std::uint8_t enables = screen == Screen::Main ? s_.tm : s_.ts;
  const std::uint8_t maskRegister = screen == Screen::Main ? s_.tmw : s_.tsw;

  // The backgrounds the mode has, each sampled once and only where the screen's
  // own register puts it there and the windows leave it: a layer this screen does
  // not enable shows nothing here, and one the windows mask shows nothing at this
  // dot, so the chart below falls through to whatever stands behind it.
  //
  // The window registers are read here, at the dot they shape, and nothing about
  // them is carried from one dot to the next — which is what lets a program move an
  // edge part-way along a line, and what lets a transfer shape a window down the
  // picture a line at a time.
  //
  // A mosaiced layer is read at its block's corner, and only there: the windows and
  // the screens above are taken at the dot itself, so a window can cut a block.
  std::array<std::optional<Shown>, 4> backgrounds{};
  const Derived::Descriptions& kept = descriptions();
  for (unsigned index = 0u; index < backgrounds.size(); ++index) {
    const auto layer = static_cast<Layer>(index);
    if ((enables & (1u << index)) == 0u || masked(layer, maskRegister, x)) continue;
    const Position read = mosaicPosition(layer, x, in, half);
    // Mode 7's layers are the field, read through the matrix; every other mode's
    // are tilemaps of characters.
    if (kept.field) {
      if (index < 2u) backgrounds[index] = sampleField(layer, read.x, read.line);
      continue;
    }
    if (const std::optional<Background>& described = kept.described[index]) {
      backgrounds[index] = sample(*described, read.x, read.line, read.half);
    }
  }

  // The sprite the line buffer holds here, where the screen's register puts sprites
  // on it and the windows leave them there. Only the topmost sprite reached the
  // buffer, so only its priority speaks to the backgrounds, and a masked sprite is
  // masked whole — whatever the buffer holds at this dot is not consulted. A line
  // Time did not gather shows none. One buffer serves both screens: the passes run
  // once a line, and which screen is asking changes nothing about what they found.
  std::uint8_t spriteWord = 0u;
  std::uint8_t spritePriority = kNoSprite;
  if ((enables & 0x10u) != 0u && s_.sprites.line == line &&
      !masked(Layer::Object, maskRegister, x)) {
    spriteWord = s_.sprites.word[x];
    spritePriority = s_.sprites.priority[x];
  }

  // Front to back by the mode's own chart, the first place holding anything here
  // being the pixel. The screen's backdrop is under them all, and is what nothing
  // here stands for.
  for (const Place& place : kept.chart) {
    if (place.layer == Layer::Object) {
      if (spritePriority == place.priority) {
        return Resolved{
            .word = spriteWord, .layer = Layer::Object, .direct = std::nullopt};
      }
      continue;
    }
    const std::optional<Shown>& shown = backgrounds[static_cast<unsigned>(place.layer)];
    if (shown.has_value() && shown->priority == (place.priority != 0u)) {
      return Resolved{.word = shown->word, .layer = place.layer, .direct = shown->direct};
    }
  }
  return std::nullopt;
}

bool Ppu::regionCovers(unsigned region, std::uint16_t x) const noexcept {
  // Each of $2130's two-bit fields names where it applies against the colour
  // window: nowhere, outside it, inside it, or everywhere. With neither of the
  // colour window's own windows enabled nothing is inside, so "outside" covers the
  // whole line and "inside" covers none of it, which falls out of the resolution
  // rather than being a case of its own.
  switch (region & 0x03u) {
    case 1u: return !windowCovers(Layer::Colour, x);
    case 2u: return windowCovers(Layer::Colour, x);
    case 3u: return true;
    default: return false;
  }
}

std::uint16_t Ppu::paletteColour(std::uint8_t word) const noexcept {
  const std::size_t at = static_cast<std::size_t>(word) << 1;
  return static_cast<std::uint16_t>(s_.cgram[at] | (s_.cgram[at + 1u] << 8));
}

std::uint16_t Ppu::fixedColour() const noexcept {
  // $2132 keeps its three channels apart, five bits each, in the order a colour
  // word holds them.
  return static_cast<std::uint16_t>(s_.fixedRed | (s_.fixedGreen << 5) |
                                    (s_.fixedBlue << 10));
}

bool Ppu::hiresAt() const noexcept {
  const std::uint8_t mode = s_.bgmode & 0x07u;
  return mode == 5u || mode == 6u || (s_.setini & 0x08u) != 0u;
}

std::uint16_t Ppu::combine(std::uint16_t colour, std::uint16_t addend, bool subtract,
                           bool halve) noexcept {
  // The halving comes before the hold and is observable there: two full channels
  // added and halved are full, not half.
  const auto channel = [&](unsigned shift) {
    const unsigned above = (colour >> shift) & 0x1Fu;
    const unsigned below = (addend >> shift) & 0x1Fu;
    unsigned value = subtract ? (above > below ? above - below : 0u) : above + below;
    if (halve) value >>= 1;
    return static_cast<std::uint16_t>(value > 31u ? 31u : value);
  };
  return static_cast<std::uint16_t>(channel(0u) | (channel(5u) << 5) | (channel(10u) << 10));
}

void Ppu::decide(std::uint16_t x, const PpuInputs& in) noexcept {
  if (s_.forcedBlank()) {
    s_.lastMain.present = false;
    return;
  }
  s_.lastMain = mainPixel(x, in, hiresAt()).decision;
}

Ppu::Dot Ppu::dot(std::uint16_t x, const PpuInputs& in) noexcept {
  const bool hires = hiresAt();
  if (x == 0u) s_.lastMain.present = false;

  // Forced blank drives black, whatever the memories hold, and draws no main pixel.
  if (s_.forcedBlank()) {
    static constexpr std::array<std::uint8_t, 4> kBlack{0u, 0u, 0u, 255u};
    s_.lastMain.present = false;
    return Dot{.left = kBlack, .right = kBlack, .hires = hires};
  }

  const MainPixel main = mainPixel(x, in, hires);
  if (!hires) {
    s_.lastMain = main.decision;
    const std::array<std::uint8_t, 4> colour = convert(main.colour);
    return Dot{.left = colour, .right = colour, .hires = false};
  }

  // The left half: the sub screen's own front-most pixel, or colour 0 where it shows
  // nothing, drawn under the decision the main pixel to its left made.
  const std::optional<Resolved> sub = resolve(Screen::Sub, x, in, false);
  std::uint16_t left = paletteColour(0u);
  if (sub.has_value()) left = sub->direct.has_value() ? *sub->direct : paletteColour(sub->word);
  const PpuState::MainDecision& before = s_.lastMain;
  if (before.present) {
    if (before.clipped) left = 0u;
    if (before.addend != 0u) {
      const std::uint16_t addend = before.addend == 1u ? fixedColour() : before.colour;
      left = combine(left, addend, before.subtract, before.halved);
    }
  }
  const std::array<std::uint8_t, 4> leftBytes = convert(left);
  s_.lastMain = main.decision;
  return Dot{.left = leftBytes, .right = convert(main.colour), .hires = true};
}

Ppu::MainPixel Ppu::mainPixel(std::uint16_t x, const PpuInputs& in, bool half) const noexcept {
  // The front-most pixel of the main screen, and the colour it stands for — unless
  // $2130's upper region covers this position, which replaces that colour with
  // black before any arithmetic and is remembered, because a pixel clipped this way
  // is not halved afterwards.
  //
  // A pixel read as a colour rather than as an index names no palette word, so it
  // carries its own colour and is taken in place of one — on either screen, because
  // direct colour is how the character data was read and not a main-screen effect.
  const std::optional<Resolved> main = resolve(Screen::Main, x, in, half);
  const bool clipped = regionCovers((s_.cgwsel >> 6) & 0x03u, x);
  const auto colourOf = [this](const std::optional<Resolved>& shown) {
    if (!shown.has_value()) return paletteColour(0u);
    return shown->direct.has_value() ? *shown->direct : paletteColour(shown->word);
  };
  const std::uint16_t mainColour = clipped ? 0u : colourOf(main);
  PpuState::MainDecision decision{.present = true,
                                  .clipped = clipped,
                                  .addend = 0u,
                                  .subtract = false,
                                  .halved = false,
                                  .colour = mainColour};

  // Whether this pixel takes math: $2131 keeps a bit for each of the six things the
  // main screen can show, the backdrop included, and $2130's lower region can
  // prevent it over part of the line. A sprite is the exception the layer bit alone
  // does not cover — only palettes 4 to 7 take math, which is a word of 192 or
  // above, and a sprite from any palette below that never does whatever bit 4 says.
  const unsigned layerBit = main.has_value() ? static_cast<unsigned>(main->layer) : 5u;
  const bool spriteRefuses =
      main.has_value() && main->layer == Layer::Object && main->word < 192u;
  const bool maths = (s_.cgadsub & (1u << layerBit)) != 0u && !spriteRefuses &&
                     !regionCovers((s_.cgwsel >> 4) & 0x03u, x);
  if (!maths) return MainPixel{.colour = mainColour, .decision = decision};

  // The addend. $2130 bit 1 clear names the fixed colour, and half applies to it as
  // bit 6 asks; set, it names the front-most pixel of the sub screen — and where
  // the sub screen shows nothing there, its backdrop is the fixed colour and half
  // is not applied. The two paths reach the same colour by different arithmetic,
  // which is why the exception is worth stating rather than folding together.
  const bool fromSubScreen = (s_.cgwsel & 0x02u) != 0u;
  std::uint16_t addend = fixedColour();
  bool subBackdrop = true;
  if (fromSubScreen) {
    const std::optional<Resolved> sub = resolve(Screen::Sub, x, in, false);
    subBackdrop = !sub.has_value();
    if (sub.has_value()) addend = colourOf(sub);
  }

  const bool subtract = (s_.cgadsub & 0x80u) != 0u;
  const bool halve =
      (s_.cgadsub & 0x40u) != 0u && !clipped && !(fromSubScreen && subBackdrop);
  decision.addend = fromSubScreen && !subBackdrop ? 2u : 1u;
  decision.subtract = subtract;
  decision.halved = halve;
  return MainPixel{.colour = combine(mainColour, addend, subtract, halve), .decision = decision};
}

// ---- the write-twice latches -------------------------------------------------

void Ppu::writeScroll(std::uint16_t& horizontal, std::uint16_t& vertical, bool isVertical,
                      std::uint8_t value) noexcept {
  // A horizontal offset keeps its own high byte's low three bits where the latch
  // would put its low three; a vertical offset takes the latch whole. Then the
  // byte just written is the latch for the next.
  if (isVertical) {
    vertical = static_cast<std::uint16_t>((value << 8) | s_.bgLatch);
  } else {
    horizontal = static_cast<std::uint16_t>((value << 8) | (s_.bgLatch & ~7u) | ((horizontal >> 8) & 7u));
  }
  s_.bgLatch = value;
}

void Ppu::writeMode7(std::uint16_t& reg, std::uint8_t value) noexcept {
  reg = static_cast<std::uint16_t>((value << 8) | s_.m7Latch);
  s_.m7Latch = value;
}

// ---- the counter latch -------------------------------------------------------

void Ppu::latchCounters(const PpuInputs& in) noexcept {
  s_.ophct = static_cast<std::uint16_t>(in.hdot & 0x01FFu);
  s_.opvct = static_cast<std::uint16_t>(in.vpos & 0x01FFu);
  s_.countersLatched = true;
}

// ---- reads -------------------------------------------------------------------

std::optional<std::uint8_t> Ppu::read(std::uint16_t offset, const PpuInputs& in) {
  // The three ports hold the plain product — matrix A times the byte last written
  // to $211C — except while the chip is drawing a Mode 7 picture, when they hold
  // what its own multiplier is doing at this dot.
  const auto product = [this, &in] {
    return drawingModeSeven(in) ? multiplierWhileDrawing(in) : s_.multiplyResult();
  };
  switch (offset) {
    case 0x2134: {  // MPYL
      const std::uint8_t v = static_cast<std::uint8_t>(product() & 0xFF);
      s_.ppu1Bus = v;
      return v;
    }
    case 0x2135: {  // MPYM
      const std::uint8_t v = static_cast<std::uint8_t>((product() >> 8) & 0xFF);
      s_.ppu1Bus = v;
      return v;
    }
    case 0x2136: {  // MPYH
      const std::uint8_t v = static_cast<std::uint8_t>((product() >> 16) & 0xFF);
      s_.ppu1Bus = v;
      return v;
    }
    case 0x2137:  // SLHV: the software latch, while the latch line is high; the value read is the CPU's open bus
      if (in.extLatch) latchCounters(in);
      return std::nullopt;
    case 0x2138: {  // RDOAM: the byte at the OAM address, which then steps
      const std::uint16_t at = s_.oamAddress & 0x3FFu;
      s_.oamAddress = static_cast<std::uint16_t>((at + 1u) & 0x3FFu);
      if (!oamReachable(in)) return s_.ppu1Bus;  // the table is the chip's now; the address stepped all the same
      const std::size_t index = at >= 0x200u ? 0x200u | (at & 0x1Fu) : at;
      s_.ppu1Bus = s_.oam[index];
      return s_.ppu1Bus;
    }
    case 0x2139: {  // RDVRAML: the low byte of the prefetch register
      const std::uint8_t v = static_cast<std::uint8_t>(s_.vramLatch & 0xFFu);
      if ((s_.vmain & 0x80u) == 0u) {
        // Prefetch from the OLD address, THEN increment — the documented glitch.
        // Outside the window the memory is not read and the register stands.
        if (vramReachable(in)) s_.vramLatch = readVramWord();
        stepVramAddress(/*highByte=*/false);
      }
      s_.ppu1Bus = v;
      return v;
    }
    case 0x213A: {  // RDVRAMH: the high byte of the prefetch register
      const std::uint8_t v = static_cast<std::uint8_t>(s_.vramLatch >> 8);
      if ((s_.vmain & 0x80u) != 0u) {
        if (vramReachable(in)) s_.vramLatch = readVramWord();
        stepVramAddress(/*highByte=*/true);
      }
      s_.ppu1Bus = v;
      return v;
    }
    case 0x213B: {  // RDCGRAM: two reads make a word; the high byte's top bit is the second half's open bus
      const bool reachable = cgramReachable(in);
      const std::uint16_t byte = static_cast<std::uint16_t>(s_.cgadd) << 1;
      std::uint8_t v;
      if (!s_.cgLatchHigh) {
        v = reachable ? s_.cgram[byte & 0x1FFu] : s_.ppu2Bus;
        s_.cgLatchHigh = true;
      } else {
        v = reachable ? static_cast<std::uint8_t>((s_.cgram[(byte + 1u) & 0x1FFu] & 0x7Fu) | (s_.ppu2Bus & 0x80u))
                      : s_.ppu2Bus;
        s_.cgadd = static_cast<std::uint8_t>(s_.cgadd + 1u);
        s_.cgLatchHigh = false;
      }
      s_.ppu2Bus = v;
      return v;
    }
    case 0x213C: {  // OPHCT: the latched dot, low byte then the ninth bit under open bus
      std::uint8_t v;
      if (!s_.ophctHigh) {
        v = static_cast<std::uint8_t>(s_.ophct & 0xFFu);
      } else {
        v = static_cast<std::uint8_t>(((s_.ophct >> 8) & 1u) | (s_.ppu2Bus & 0xFEu));
      }
      s_.ophctHigh = !s_.ophctHigh;
      s_.ppu2Bus = v;
      return v;
    }
    case 0x213D: {  // OPVCT: the latched line, the same way through its own flip-flop
      std::uint8_t v;
      if (!s_.opvctHigh) {
        v = static_cast<std::uint8_t>(s_.opvct & 0xFFu);
      } else {
        v = static_cast<std::uint8_t>(((s_.opvct >> 8) & 1u) | (s_.ppu2Bus & 0xFEu));
      }
      s_.opvctHigh = !s_.opvctHigh;
      s_.ppu2Bus = v;
      return v;
    }
    case 0x213E: {  // STAT77: the overflow flags, master, open bus, the first half's version
      const std::uint8_t v = static_cast<std::uint8_t>(
          (s_.timeOver ? 0x80u : 0x00u) | (s_.rangeOver ? 0x40u : 0x00u) | (s_.ppu1Bus & 0x10u) | 0x01u);
      s_.ppu1Bus = v;
      return v;
    }
    case 0x213F: {  // STAT78: the field, the latch flag, open bus, the clock rate, the second half's version
      const std::uint8_t v = static_cast<std::uint8_t>(
          ((in.field & 1u) << 7) | (s_.countersLatched ? 0x40u : 0x00u) | (s_.ppu2Bus & 0x20u) |
          (in.pal ? 0x10u : 0x00u) | 0x03u);
      // The read clears the latch flag, but only while the latch line is high. The
      // two counters' flip-flops it resets whatever that line is doing: that is a
      // side effect of the read itself.
      if (in.extLatch) s_.countersLatched = false;
      s_.ophctHigh = false;
      s_.opvctHigh = false;
      s_.ppu2Bus = v;
      return v;
    }
    default:
      break;
  }
  if (readsPpu1Bus(offset)) return s_.ppu1Bus;
  return std::nullopt;  // every other write-only register reads as the CPU's open bus
}

// ---- writes ------------------------------------------------------------------

std::optional<std::uint16_t> Ppu::write(std::uint16_t offset, std::uint8_t value, const PpuInputs& in) {
  // What each background is and where it stands in the chart the chip reads out of
  // $2105-$210C and $2133, so a write to one of those is what makes its answer
  // stale. $2106 stands inside that span and describes nothing, and dropping on it
  // costs one reading of eleven registers.
  if ((offset >= 0x2105u && offset <= 0x210Cu) || offset == 0x2133u) d_.dropDescriptions();

  switch (offset) {
    case 0x2100: {  // INIDISP: forced blank and brightness
      const bool released = s_.forcedBlank() && (value & 0x80u) == 0u;
      s_.inidisp = value;
      // Forced blank released on vblank's first line: the PPU reloads the OAM
      // address then, as it would have at dot 10 of that line with the screen on.
      // Which line that is, the chip reads from its own SETINI.
      if (released && in.vpos == s_.vblankStartLine()) reloadOamAddress();
      return std::nullopt;
    }
    case 0x2101: s_.objsel = value; return std::nullopt;
    case 0x2102:  // OAMADDL: the low eight bits of the reload value, and the address takes the whole value
      s_.oamadd = static_cast<std::uint16_t>((s_.oamadd & 0xFF00u) | value);
      reloadOamAddress();
      return std::nullopt;
    case 0x2103:  // OAMADDH: the ninth bit and the priority-rotation bit
      s_.oamadd = static_cast<std::uint16_t>((s_.oamadd & 0x00FFu) | (value << 8));
      reloadOamAddress();
      return std::nullopt;
    case 0x2104: {  // OAMDATA: a word through the latch below $200, a byte above it, mirrored past $21F
      const std::uint16_t at = s_.oamAddress & 0x3FFu;
      s_.oamAddress = static_cast<std::uint16_t>((at + 1u) & 0x3FFu);
      if (!oamReachable(in)) return std::nullopt;  // the table is the chip's now; the address stepped all the same
      if (at >= 0x200u) {
        const std::size_t index = 0x200u | (at & 0x1Fu);
        s_.oam[index] = value;
        return static_cast<std::uint16_t>(index);
      }
      if ((at & 1u) == 0u) {
        s_.oamLatch = value;
        return at;
      }
      s_.oam[at - 1u] = s_.oamLatch;
      s_.oam[at] = value;
      return at;
    }
    case 0x2105: s_.bgmode = value; return std::nullopt;
    case 0x2106: s_.mosaic = value; return std::nullopt;
    case 0x2107: s_.bg1sc = value; return std::nullopt;
    case 0x2108: s_.bg2sc = value; return std::nullopt;
    case 0x2109: s_.bg3sc = value; return std::nullopt;
    case 0x210A: s_.bg4sc = value; return std::nullopt;
    case 0x210B: s_.bg12nba = value; return std::nullopt;
    case 0x210C: s_.bg34nba = value; return std::nullopt;
    case 0x210D:  // BG1HOFS and M7HOFS, each through its own latch
      writeMode7(s_.m7hofs, value);
      writeScroll(s_.bg1hofs, s_.bg1vofs, /*isVertical=*/false, value);
      return std::nullopt;
    case 0x210E:  // BG1VOFS and M7VOFS
      writeMode7(s_.m7vofs, value);
      writeScroll(s_.bg1hofs, s_.bg1vofs, /*isVertical=*/true, value);
      return std::nullopt;
    case 0x210F: writeScroll(s_.bg2hofs, s_.bg2vofs, false, value); return std::nullopt;
    case 0x2110: writeScroll(s_.bg2hofs, s_.bg2vofs, true, value); return std::nullopt;
    case 0x2111: writeScroll(s_.bg3hofs, s_.bg3vofs, false, value); return std::nullopt;
    case 0x2112: writeScroll(s_.bg3hofs, s_.bg3vofs, true, value); return std::nullopt;
    case 0x2113: writeScroll(s_.bg4hofs, s_.bg4vofs, false, value); return std::nullopt;
    case 0x2114: writeScroll(s_.bg4hofs, s_.bg4vofs, true, value); return std::nullopt;
    case 0x2115: s_.vmain = value; return std::nullopt;  // increment mode and address translation
    case 0x2116:
      s_.vmadd = static_cast<std::uint16_t>((s_.vmadd & 0xFF00u) | value);
      if (vramReachable(in)) s_.vramLatch = readVramWord();  // changing the address prefetches the new word
      return std::nullopt;
    case 0x2117:
      s_.vmadd = static_cast<std::uint16_t>((s_.vmadd & 0x00FFu) | (value << 8));
      if (vramReachable(in)) s_.vramLatch = readVramWord();
      return std::nullopt;
    case 0x2118: {  // VMDATAL: the low byte of the addressed word
      const std::uint16_t word = static_cast<std::uint16_t>(vramWordAddress() & 0x7FFFu);
      const bool reachable = vramReachable(in);
      if (reachable) s_.vram[static_cast<std::size_t>(word) << 1] = value;
      stepVramAddress(/*highByte=*/false);  // a write never prefetches, and the address steps either way
      if (!reachable) return std::nullopt;
      return word;
    }
    case 0x2119: {  // VMDATAH: the high byte of the addressed word
      const std::uint16_t word = static_cast<std::uint16_t>(vramWordAddress() & 0x7FFFu);
      const bool reachable = vramReachable(in);
      if (reachable) s_.vram[(static_cast<std::size_t>(word) << 1) + 1u] = value;
      stepVramAddress(/*highByte=*/true);
      if (!reachable) return std::nullopt;
      return word;
    }
    case 0x211A: s_.m7sel = value; return std::nullopt;
    case 0x211B: writeMode7(s_.m7a, value); return std::nullopt;
    case 0x211C:  // M7B, and every write is the multiplier's byte
      writeMode7(s_.m7b, value);
      s_.m7bByte = value;
      return std::nullopt;
    case 0x211D: writeMode7(s_.m7c, value); return std::nullopt;
    case 0x211E: writeMode7(s_.m7d, value); return std::nullopt;
    case 0x211F: writeMode7(s_.m7x, value); return std::nullopt;
    case 0x2120: writeMode7(s_.m7y, value); return std::nullopt;
    case 0x2121:  // CGADD: setting the address resets the low/high flip-flop
      s_.cgadd = value;
      s_.cgLatchHigh = false;
      return std::nullopt;
    case 0x2122: {  // CGDATA: the low byte is held, the high byte commits the word
      const bool reachable = cgramReachable(in);
      const std::uint8_t word = s_.cgadd;
      if (!s_.cgLatchHigh) {
        s_.cgLatch = value;
        s_.cgLatchHigh = true;
      } else {
        if (reachable) {
          const std::uint16_t byte = static_cast<std::uint16_t>(s_.cgadd) << 1;
          s_.cgram[byte & 0x1FFu] = s_.cgLatch;
          s_.cgram[(byte + 1u) & 0x1FFu] = static_cast<std::uint8_t>(value & 0x7Fu);
        }
        s_.cgadd = static_cast<std::uint8_t>(s_.cgadd + 1u);
        s_.cgLatchHigh = false;
      }
      if (!reachable) return std::nullopt;
      return word;
    }
    case 0x2123: s_.w12sel = value; return std::nullopt;
    case 0x2124: s_.w34sel = value; return std::nullopt;
    case 0x2125: s_.wobjsel = value; return std::nullopt;
    case 0x2126: s_.wh0 = value; return std::nullopt;
    case 0x2127: s_.wh1 = value; return std::nullopt;
    case 0x2128: s_.wh2 = value; return std::nullopt;
    case 0x2129: s_.wh3 = value; return std::nullopt;
    case 0x212A: s_.wbglog = value; return std::nullopt;
    case 0x212B: s_.wobjlog = value; return std::nullopt;
    case 0x212C: s_.tm = value; return std::nullopt;
    case 0x212D: s_.ts = value; return std::nullopt;
    case 0x212E: s_.tmw = value; return std::nullopt;
    case 0x212F: s_.tsw = value; return std::nullopt;
    case 0x2130: s_.cgwsel = value; return std::nullopt;
    case 0x2131: s_.cgadsub = value; return std::nullopt;
    case 0x2132: {  // COLDATA: the low five bits into each channel the top three select
      const std::uint8_t colour = static_cast<std::uint8_t>(value & 0x1Fu);
      if ((value & 0x20u) != 0u) s_.fixedRed = colour;
      if ((value & 0x40u) != 0u) s_.fixedGreen = colour;
      if ((value & 0x80u) != 0u) s_.fixedBlue = colour;
      return std::nullopt;
    }
    case 0x2133: s_.setini = value; return std::nullopt;
    default: return std::nullopt;  // the read-only ports ignore writes
  }
}

}  // namespace snaggletooth
