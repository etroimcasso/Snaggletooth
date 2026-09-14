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
// is what brings a tall sprite hung above the picture back in at the top.
[[nodiscard]] std::optional<unsigned> rowCrossed(unsigned y, unsigned height,
                                                 std::uint16_t line) noexcept {
  const unsigned row = (line - 1u - y) & 0xFFu;
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

// The row a vertical flip shows in that row's place. A flip reverses the whole
// sprite rather than its characters — except that a rectangular sprite flips as
// though it were two square sprites stacked, so rows "01234567" become
// "32107654" and not "76543210".
[[nodiscard]] unsigned flipRow(unsigned row, unsigned width) noexcept {
  return (row / width) * width + (width - 1u - row % width);
}

}  // namespace

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

  if (!rowCrossed(sprite.y, sprite.height, line).has_value()) return;
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

void Ppu::timeSprites(std::uint16_t line) noexcept {
  if (s_.forcedBlank()) return;

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
    const std::optional<unsigned> crossed = rowCrossed(sprite.y, sprite.height, line);
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

Ppu::Background Ppu::mode1Bg1() const noexcept {
  return Background{.screen = s_.bg1sc,
                    .characterBase = static_cast<std::uint8_t>(s_.bg12nba & 0x0Fu),
                    .horizontal = s_.bg1hofs,
                    .vertical = s_.bg1vofs,
                    .large = (s_.bgmode & 0x10u) != 0u,
                    .planes = 4u};
}

Ppu::Background Ppu::mode1Bg2() const noexcept {
  // $210B keeps BG2's character base in its high nibble, above BG1's.
  return Background{.screen = s_.bg2sc,
                    .characterBase = static_cast<std::uint8_t>(s_.bg12nba >> 4),
                    .horizontal = s_.bg2hofs,
                    .vertical = s_.bg2vofs,
                    .large = (s_.bgmode & 0x20u) != 0u,
                    .planes = 4u};
}

Ppu::Background Ppu::mode1Bg3() const noexcept {
  // Mode 1's third background is four colours, so two bitplanes.
  return Background{.screen = s_.bg3sc,
                    .characterBase = static_cast<std::uint8_t>(s_.bg34nba & 0x0Fu),
                    .horizontal = s_.bg3hofs,
                    .vertical = s_.bg3vofs,
                    .large = (s_.bgmode & 0x40u) != 0u,
                    .planes = 2u};
}

std::optional<Ppu::Shown> Ppu::sample(const Background& background, std::uint16_t x,
                                      std::uint16_t line) const noexcept {
  // Where the position falls in the background. The offsets are the low ten bits of
  // its two scroll registers, and the display never falls outside the background:
  // the masks below wrap it at its own size, whatever that size is.
  const unsigned side = background.large ? 16u : 8u;
  const unsigned bgX = x + (background.horizontal & 0x03FFu);
  const unsigned bgY = line + (background.vertical & 0x03FFu);
  const unsigned tileX = bgX / side;
  const unsigned tileY = bgY / side;

  // The tilemap word for that tile: the base its screen register names, the row and
  // column within one 32x32 screen, and the terms that carry a wide or tall map into
  // its further screens, which follow the first at $800 bytes each.
  //
  // The base counts whole screens: a 32x32 screen is $400 words, so the six bits of
  // the register step the map in $400-word units and reach every 2 KB boundary of
  // the memory.
  const bool wide = (background.screen & 0x01u) != 0u;
  const bool tall = (background.screen & 0x02u) != 0u;
  unsigned word = (static_cast<unsigned>(background.screen >> 2) << 10) +
                  ((tileY & 0x1Fu) << 5) + (tileX & 0x1Fu);
  if (tall) word += (tileY & 0x20u) << (wide ? 6u : 5u);
  if (wide) word += (tileX & 0x20u) << 5;
  const std::size_t entryAt = (static_cast<std::size_t>(word) << 1) & 0xFFFFu;
  const std::uint16_t entry =
      static_cast<std::uint16_t>(s_.vram[entryAt] | (s_.vram[(entryAt + 1u) & 0xFFFFu] << 8));

  // The entry is vhopppcc cccccccc: the two flips, the tile's priority, its palette
  // and its number. A flip reverses the whole tile, a 16x16 block included.
  unsigned inX = bgX % side;
  unsigned inY = bgY % side;
  if ((entry & 0x4000u) != 0u) inX = side - 1u - inX;
  if ((entry & 0x8000u) != 0u) inY = side - 1u - inY;

  // A 16x16 block is Tile, Tile + 1, Tile + 16 and Tile + 17. The numbers run on
  // rather than wrapping within the block; only the ten-bit number itself wraps.
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

  // A background's palette is as many words on as it has colours, and Mode 1 gives
  // none of the three a starting palette of its own.
  const unsigned palette = (entry >> 10) & 0x07u;
  return Shown{.word = static_cast<std::uint8_t>(palette * (1u << background.planes) + index),
               .priority = (entry & 0x2000u) != 0u};
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

std::array<std::uint8_t, 4> Ppu::pixel(std::uint16_t x, std::uint16_t line) const noexcept {
  // Forced blank drives black, whatever the memories hold.
  if (s_.forcedBlank()) return {0u, 0u, 0u, 255u};

  // Mode 1 is the mode this chip draws; the others show their backdrop. Each of its
  // three backgrounds is sampled once, and only where $212C puts it on the main
  // screen and the windows leave it there: a layer enabled on the sub screen alone
  // shows nowhere, and one the windows mask shows nothing at this dot, so the chart
  // below falls through to whatever stands behind it.
  //
  // The window registers are read here, at the dot they shape, and nothing about
  // them is carried from one dot to the next — which is what lets a program move an
  // edge part-way along a line, and what lets a transfer shape a window down the
  // picture a line at a time.
  std::optional<Shown> bg1;
  std::optional<Shown> bg2;
  std::optional<Shown> bg3;
  if ((s_.bgmode & 0x07u) == 1u) {
    if ((s_.tm & 0x01u) != 0u && !masked(Layer::Bg1, s_.tmw, x)) bg1 = sample(mode1Bg1(), x, line);
    if ((s_.tm & 0x02u) != 0u && !masked(Layer::Bg2, s_.tmw, x)) bg2 = sample(mode1Bg2(), x, line);
    if ((s_.tm & 0x04u) != 0u && !masked(Layer::Bg3, s_.tmw, x)) bg3 = sample(mode1Bg3(), x, line);
  }

  // The sprite the line buffer holds here, where $212C puts sprites on the main
  // screen and the windows leave them there. Only the topmost sprite reached the
  // buffer, so only its priority speaks to the backgrounds, and a masked sprite is
  // masked whole — whatever the buffer holds at this dot is not consulted. A line
  // Time did not gather shows none.
  std::uint8_t spriteWord = 0u;
  std::uint8_t spritePriority = kNoSprite;
  if ((s_.tm & 0x10u) != 0u && s_.sprites.line == line && (s_.bgmode & 0x07u) == 1u &&
      !masked(Layer::Object, s_.tmw, x)) {
    spriteWord = s_.sprites.word[x];
    spritePriority = s_.sprites.priority[x];
  }

  // Front to back, by the chart Mode 1 keeps: a sprite at priority 3, BG1 and BG2
  // at tile priority 1, a sprite at 2, the same two backgrounds at tile priority 0,
  // a sprite at 1, BG3's high-priority tiles, a sprite at 0, then BG3's low —
  // except that $2105 bit 3 lifts BG3's high-priority tiles in front of everything,
  // taking them out of the place they otherwise hold. The backdrop, palette word 0,
  // is under them all.
  const auto shows = [](const std::optional<Shown>& background, bool priority) {
    return background.has_value() && background->priority == priority;
  };
  const auto sprite = [spritePriority](std::uint8_t priority) {
    return spritePriority == priority;
  };
  const bool bg3InFront = (s_.bgmode & 0x08u) != 0u;
  std::uint8_t word = 0u;
  if (bg3InFront && shows(bg3, true)) {
    word = bg3->word;
  } else if (sprite(3u)) {
    word = spriteWord;
  } else if (shows(bg1, true)) {
    word = bg1->word;
  } else if (shows(bg2, true)) {
    word = bg2->word;
  } else if (sprite(2u)) {
    word = spriteWord;
  } else if (shows(bg1, false)) {
    word = bg1->word;
  } else if (shows(bg2, false)) {
    word = bg2->word;
  } else if (sprite(1u)) {
    word = spriteWord;
  } else if (!bg3InFront && shows(bg3, true)) {
    word = bg3->word;
  } else if (sprite(0u)) {
    word = spriteWord;
  } else if (shows(bg3, false)) {
    word = bg3->word;
  }

  const std::size_t at = static_cast<std::size_t>(word) << 1;
  return convert(static_cast<std::uint16_t>(s_.cgram[at] | (s_.cgram[at + 1u] << 8)));
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
  switch (offset) {
    case 0x2134: {  // MPYL
      const std::uint8_t v = static_cast<std::uint8_t>(s_.multiplyResult() & 0xFF);
      s_.ppu1Bus = v;
      return v;
    }
    case 0x2135: {  // MPYM
      const std::uint8_t v = static_cast<std::uint8_t>((s_.multiplyResult() >> 8) & 0xFF);
      s_.ppu1Bus = v;
      return v;
    }
    case 0x2136: {  // MPYH
      const std::uint8_t v = static_cast<std::uint8_t>((s_.multiplyResult() >> 16) & 0xFF);
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
