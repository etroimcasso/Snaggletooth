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

}  // namespace

std::int32_t PpuState::multiplyResult() const noexcept {
  const std::int32_t a = static_cast<std::int16_t>(m7a);
  const std::int32_t b = static_cast<std::int8_t>(m7bByte);
  return a * b;
}

// ---- the memories' windows ---------------------------------------------------

bool Ppu::vramReachable(const PpuInputs& in) const noexcept { return in.vblank || s_.forcedBlank(); }
bool Ppu::oamReachable(const PpuInputs& in) const noexcept { return in.vblank || s_.forcedBlank(); }
bool Ppu::cgramReachable(const PpuInputs& in) const noexcept {
  return in.vblank || in.hblank || s_.forcedBlank();
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
      // The read also clears the flag and resets both counters' flip-flops.
      s_.countersLatched = false;
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
      // address then, as it would have at the line's start with the screen on.
      if (released && in.vpos == kVblankStartLine) reloadOamAddress();
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
