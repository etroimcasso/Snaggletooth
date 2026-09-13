#pragma once

// The PPU's register file and the three video memories behind it.
//
// The picture processor sits on the B bus at $2100-$213F. Its registers are
// written by the CPU and the transfer engines and read back through the few
// ports that read at all; the register file, VRAM, the palette and the sprite
// table are the whole of what a program can put into the chip. Nothing here
// draws: this is the state a renderer reads, kept exactly as the hardware
// keeps it — the write-twice latches, the address flip-flops, the counter
// latch, the two open-bus values the chip's halves remember, and the windows
// in which each memory can be reached.
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
#include <cstdint>
#include <optional>

namespace snaggletooth {

// The first line of vertical blank. The picture ends on line 224; vblank runs
// from this line to the end of the frame, and line 0 is the end of vblank.
constexpr std::uint16_t kVblankStartLine = 225u;

// The machine as the PPU sees it at one access.
struct PpuInputs {
  std::uint16_t hdot = 0;   // the beam's dot within the line (0..339)
  std::uint16_t vpos = 0;   // the beam's line
  std::uint8_t field = 0;   // the frame parity, toggled every frame
  bool vblank = false;      // the vertical-blank signal: line kVblankStartLine to the frame's end
  bool hblank = false;      // the horizontal-blank signal: outside the line's active span
  bool pal = false;         // the clock-rate pin: 50 Hz when set
  bool extLatch = true;     // the counter-latch line's level, WRIO bit 7 (high when nothing pulls it)
};

// The PPU as a value: the register file, the latches, and the three memories.
// A register that is stored as written keeps the name the hardware gives it.
struct PpuState {
  std::array<std::uint8_t, 65536> vram{};  // 64 KB video RAM (32K words)
  std::array<std::uint8_t, 512> cgram{};   // 512 B palette RAM (256 words)
  std::array<std::uint8_t, 544> oam{};     // 512 B of sprite entries and the 32 B of high bits

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
  bool rangeOver = false;  // $213E bit 6: more than 32 sprites on a line
  bool timeOver = false;   // $213E bit 7: more than 34 sprite slivers on a line

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

  // The beam reaching the first line of vertical blank: with the screen on, the
  // OAM address returns to the reload value.
  void beginVblank() noexcept;

 private:
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
