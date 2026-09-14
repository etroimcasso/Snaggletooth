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

// The character data's shape: four bitplanes of eight rows for a 16-colour tile, so
// 32 bytes a character, with planes 2 and 3 the sixteen bytes after planes 0 and 1.
constexpr std::size_t kCharacterBytes = 32u;
constexpr std::size_t kHighPlanes = 16u;

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

std::optional<std::uint8_t> Ppu::sampleBg1(std::uint16_t x, std::uint16_t line) const noexcept {
  // Where the position falls in the background. The offsets are the low ten bits of
  // the two scroll registers, and the display never falls outside the background:
  // the masks below wrap it at its own size, whatever that size is.
  const bool large = (s_.bgmode & 0x10u) != 0u;  // $2105 bit 4: BG1 in 16x16 blocks
  const unsigned side = large ? 16u : 8u;
  const unsigned bgX = x + (s_.bg1hofs & 0x03FFu);
  const unsigned bgY = line + (s_.bg1vofs & 0x03FFu);
  const unsigned tileX = bgX / side;
  const unsigned tileY = bgY / side;

  // The tilemap word for that tile: the base $2107 names, the row and column within
  // one 32x32 screen, and the terms that carry a wide or tall map into its further
  // screens, which follow the first at $800 bytes each.
  //
  // The base counts whole screens: a 32x32 screen is $400 words, so the six bits of
  // $2107 step the map in $400-word units and reach every 2 KB boundary of the
  // memory.
  const bool wide = (s_.bg1sc & 0x01u) != 0u;
  const bool tall = (s_.bg1sc & 0x02u) != 0u;
  unsigned word = (static_cast<unsigned>(s_.bg1sc >> 2) << 10) + ((tileY & 0x1Fu) << 5) +
                  (tileX & 0x1Fu);
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

  // The character the tile names, under the base $210B holds for BG1. Planes 0 and 1
  // are the low and high bytes of the row's word and planes 2 and 3 the same word
  // sixteen bytes on; the leftmost pixel of a row is bit 7.
  const std::size_t character =
      ((static_cast<std::size_t>(s_.bg12nba & 0x0Fu) << 13) + tile * kCharacterBytes) & 0xFFFFu;
  const std::size_t row = (character + inY * 2u) & 0xFFFFu;
  const unsigned bit = 7u - inX;
  const unsigned index = ((s_.vram[row] >> bit) & 1u) |
                         (((s_.vram[(row + 1u) & 0xFFFFu] >> bit) & 1u) << 1) |
                         (((s_.vram[(row + kHighPlanes) & 0xFFFFu] >> bit) & 1u) << 2) |
                         (((s_.vram[(row + kHighPlanes + 1u) & 0xFFFFu] >> bit) & 1u) << 3);
  if (index == 0u) return std::nullopt;  // colour 0 of any palette is transparent

  // A 16-colour background's palette is sixteen words on from the one before it.
  const unsigned palette = (entry >> 10) & 0x07u;
  return static_cast<std::uint8_t>(palette * 16u + index);
}

std::array<std::uint8_t, 4> Ppu::pixel(std::uint16_t x, std::uint16_t line) const noexcept {
  // Forced blank drives black, whatever the memories hold.
  if (s_.forcedBlank()) return {0u, 0u, 0u, 255u};

  // The first layer the main screen enables with something to show here, and the
  // backdrop — palette word 0 — under all of them. BG1 of Mode 1 is the layer this
  // chip draws; a layer enabled only on the sub screen shows nowhere, and the modes
  // whose backgrounds are not 16 colours show their backdrop.
  std::uint8_t word = 0u;
  if ((s_.bgmode & 0x07u) == 1u && (s_.tm & 0x01u) != 0u) {
    if (const std::optional<std::uint8_t> bg1 = sampleBg1(x, line)) word = *bg1;
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
