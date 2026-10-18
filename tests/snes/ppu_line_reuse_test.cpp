// A picture line taken from the frame before: the machine records what each picture
// line reads and what it produces, and when the next frame's same line reads the
// same — the registers the picture reads, the sprites Time gathered for it, the
// video-memory pages and palette groups its reads touch, the field where the picture
// reads it — it takes the line from the record instead of drawing it, unless a write
// the line's dots read lands inside it, where it is drawn up to the beam and on to
// its end. The oracle in every case is two machines run side by side with the same
// budgets: one that reuses, and one built with SnesConfig::ppuVerify on, which draws
// every line and compares it with what the frame before recorded. The two are held
// equal at every return — the whole state, every frame's pixels and size — and the
// verifying machine's count of disagreements is held at zero; the counts of how the
// lines were drawn are read from the machine that reuses, through Snes::lineReuse().
//
// The scene: Mode 1, BG1 and BG2 over distinct tilemaps, BG1 under 4-line mosaic
// blocks, four 8x8 sprites on a palette of their own, an NMI handler counting frames
// in work RAM and making each case's writes in the vertical blank of a named frame,
// two HDMA channels the cases enable, and an interrupt at a named dot of a named
// line for the writes and reads that must land inside a line.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "snaggletooth/snes/snes.h"

namespace snaggletooth {
namespace {

// One NTSC frame in master cycles.
constexpr std::uint64_t kFrame = 357366u;
constexpr unsigned kLines = 224u;

// Where the scene cartridge keeps its pieces in bank $00, and the work-RAM byte its
// NMI handler counts frames in.
constexpr std::uint16_t kBgTiles = 0x8400u;      // four 4bpp tiles, 128 bytes
constexpr std::uint16_t kSpriteTiles = 0x8480u;  // four more, for the sprites
constexpr std::uint16_t kBg1Map = 0x8800u;       // one 32x32 screen, 2048 bytes
constexpr std::uint16_t kBg2Map = 0x9000u;
constexpr std::uint16_t kPalette = 0x9800u;      // 256 words
constexpr std::uint16_t kOamTable = 0x9A00u;     // 544 bytes
constexpr std::uint16_t kHdmaMosaic = 0x9D00u;   // channel 1's table, to $2106
constexpr std::uint16_t kHdmaHires = 0x9D40u;    // channel 2's table, to $2133
constexpr std::uint16_t kZeros = 0x9E00u;        // 256 bytes of $00, a transfer's source
constexpr std::uint16_t kNmiHandler = 0xA200u;
constexpr std::uint16_t kIrqHandler = 0xA400u;
constexpr std::uint8_t kFrameCounter = 0x10u;

// The video memory layout the scene builds: the backgrounds' characters at byte 0,
// the sprites' at byte $4000 (name base 1), BG1's map at byte $1000 (base 2, the 1 KB
// pages 4 and 5) and BG2's at byte $1800 (base 3, pages 6 and 7).
constexpr std::uint16_t kBg1MapWord = 0x0800u;

// The four sprites: 8x8, on palette 1 (words 144 to 159, group 9), at the front.
constexpr std::array<std::uint8_t, 4> kSpriteY{30u, 80u, 130u, 180u};
constexpr std::uint8_t kSpriteAttributes = 0x32u;  // priority 3, palette 1, tile high bit clear
constexpr std::uint8_t kSpritePaletteWord = 146u;  // color 2 of palette 1: the solid tile's color

using Bytes = std::vector<std::uint8_t>;

Bytes joined(std::initializer_list<Bytes> parts) {
  Bytes out;
  for (const Bytes& part : parts) out.insert(out.end(), part.begin(), part.end());
  return out;
}

// LDA #v ; STA $abs
Bytes store(std::uint16_t address, std::uint8_t value) {
  return {0xA9u, value, 0x8Du, static_cast<std::uint8_t>(address & 0xFFu),
          static_cast<std::uint8_t>(address >> 8)};
}

// STZ $abs
Bytes clear(std::uint16_t address) {
  return {0x9Cu, static_cast<std::uint8_t>(address & 0xFFu), static_cast<std::uint8_t>(address >> 8)};
}

// `body`, run when the frame counter holds `frame`: LDA $10 ; CMP #frame ; BNE past.
Bytes onFrame(std::uint8_t frame, const Bytes& body) {
  if (body.size() > 127u) throw std::length_error("a frame's body is too long for the branch");
  return joined({{0xA5u, kFrameCounter, 0xC9u, frame, 0xD0u, static_cast<std::uint8_t>(body.size())}, body});
}

// `body`, run while the frame counter is below `frame`: LDA $10 ; CMP #frame ; BCS past.
Bytes beforeFrame(std::uint8_t frame, const Bytes& body) {
  if (body.size() > 127u) throw std::length_error("a frame's body is too long for the branch");
  return joined({{0xA5u, kFrameCounter, 0xC9u, frame, 0xB0u, static_cast<std::uint8_t>(body.size())}, body});
}

// A general-purpose DMA on channel `channel` of `size` bytes from bank $00 address
// `source` to $2100 + `port` under transfer pattern `pattern`, started at once.
Bytes dma(std::uint8_t channel, std::uint8_t pattern, std::uint8_t port, std::uint16_t source,
          std::uint16_t size) {
  const auto reg = [channel](std::uint8_t index) {
    return static_cast<std::uint16_t>(0x4300u | (channel << 4) | index);
  };
  return joined({store(reg(0u), pattern), store(reg(1u), port),
                 store(reg(2u), static_cast<std::uint8_t>(source & 0xFFu)),
                 store(reg(3u), static_cast<std::uint8_t>(source >> 8)), clear(reg(4u)),
                 store(reg(5u), static_cast<std::uint8_t>(size & 0xFFu)),
                 store(reg(6u), static_cast<std::uint8_t>(size >> 8)),
                 store(0x420Bu, static_cast<std::uint8_t>(1u << channel))});
}

// An HDMA channel pointed at its table: direct, one byte a line, to $2100 + `port`.
Bytes hdmaChannel(std::uint8_t channel, std::uint8_t port, std::uint16_t table) {
  const auto reg = [channel](std::uint8_t index) {
    return static_cast<std::uint16_t>(0x4300u | (channel << 4) | index);
  };
  return joined({clear(reg(0u)), store(reg(1u), port), store(reg(2u), static_cast<std::uint8_t>(table & 0xFFu)),
                 store(reg(3u), static_cast<std::uint8_t>(table >> 8)), clear(reg(4u))});
}

// Waits for vertical blank to begin, or to end: LDA $4212 ; BPL / BMI back.
Bytes awaitVblank() { return {0xADu, 0x12u, 0x42u, 0x10u, 0xFBu}; }
Bytes awaitPicture() { return {0xADu, 0x12u, 0x42u, 0x30u, 0xFBu}; }

// A 4bpp character from eight rows of eight color indices.
Bytes character(const std::array<std::array<std::uint8_t, 8>, 8>& rows) {
  Bytes out(32u, 0u);
  for (unsigned row = 0u; row < 8u; ++row) {
    for (unsigned plane = 0u; plane < 4u; ++plane) {
      std::uint8_t bits = 0u;
      for (unsigned column = 0u; column < 8u; ++column) {
        bits = static_cast<std::uint8_t>(bits | (((rows[row][column] >> plane) & 1u) << (7u - column)));
      }
      out[(plane / 2u) * 16u + row * 2u + (plane % 2u)] = bits;
    }
  }
  return out;
}

// Four characters: blank, solid, a checker of two colors, and a diagonal through the
// palette with transparent spots.
Bytes characterSet(unsigned seed) {
  std::array<std::array<std::uint8_t, 8>, 8> rows{};
  Bytes out;
  for (unsigned tile = 0u; tile < 4u; ++tile) {
    for (unsigned row = 0u; row < 8u; ++row) {
      for (unsigned column = 0u; column < 8u; ++column) {
        unsigned color = 0u;
        if (tile == 1u) color = 1u + seed;
        if (tile == 2u) color = ((row + column) & 1u) != 0u ? 2u : 3u;
        if (tile == 3u) color = (row * 3u + column + seed) & 0x0Fu;
        rows[row][column] = static_cast<std::uint8_t>(color);
      }
    }
    out = joined({out, character(rows)});
  }
  return out;
}

// A 32x32 tilemap whose entries run through the four tiles, the eight palettes, the
// priority bit and both flips.
Bytes tilemap(unsigned seed) {
  Bytes out;
  for (unsigned index = 0u; index < 1024u; ++index) {
    const unsigned i = index + seed;
    const unsigned tile = (i >> seed) & 3u;
    const unsigned palette = (i >> (2u + seed)) & 7u;
    const unsigned priority = (i >> (5u + seed)) & 1u;
    const unsigned flips = (i >> 7u) & 3u;
    const unsigned entry = tile | (palette << 10) | (priority << 13) | (flips << 14);
    out.push_back(static_cast<std::uint8_t>(entry & 0xFFu));
    out.push_back(static_cast<std::uint8_t>(entry >> 8));
  }
  return out;
}

// 256 palette words, none of them zero.
Bytes palette() {
  Bytes out;
  for (unsigned word = 0u; word < 256u; ++word) {
    const unsigned color = word == 0u ? 0x0C41u : (((word * 0x1D3u) ^ (word << 3)) & 0x7FFFu) | 0x0421u;
    out.push_back(static_cast<std::uint8_t>(color & 0xFFu));
    out.push_back(static_cast<std::uint8_t>(color >> 8));
  }
  return out;
}

// The sprite table: four 8x8 sprites on the solid character, one above the other
// down the picture at X = 40, 80, 120 and 160, and the rest at the origin on the
// blank character.
Bytes oamTable() {
  Bytes out(544u, 0u);
  for (unsigned sprite = 0u; sprite < 4u; ++sprite) {
    out[sprite * 4u] = static_cast<std::uint8_t>(40u + sprite * 40u);
    out[sprite * 4u + 1u] = kSpriteY[sprite];
    out[sprite * 4u + 2u] = 1u;
    out[sprite * 4u + 3u] = kSpriteAttributes;
  }
  return out;
}

// Channel 1's table: $2106 held at $31 (BG1 under 4-line blocks) through line 51,
// $51 written on line 52, so the row beginning on line 53 is six lines tall, and $31
// written back on line 53, so the rows after it are four lines tall again from line
// 59 — two lines later than every frame without it.
Bytes mosaicTable() {
  return {0x34u, 0x31u, 0x01u, 0x51u, 0x01u, 0x31u, 0x00u};
}

// Channel 2's table: $2133 clear through line 99, bit 3 set on line 100, so lines
// 101 to 111 are drawn in half-pixels, and clear again from line 111's write on.
Bytes hiresTable() {
  return {0x64u, 0x00u, 0x0Bu, 0x08u, 0x71u, 0x00u, 0x00u};
}

// What a case adds to the scene: the handler's blank-time part, after the count; its
// picture-time part, run once the picture has begun and a line has gone by; and an
// interrupt at dot `htime` of line `vtime` whose handler runs `irqBody`.
struct Variant {
  Bytes inBlank{};
  Bytes inPicture{};
  Bytes irqBody{};
  std::uint16_t htime = 0;
  std::uint16_t vtime = 0;
  bool irq = false;
};

// The NMI handler: the flag read, the frame counted, the variant's blank-time part,
// and where it has a picture-time part, a wait for the picture to begin, a line's
// delay and that part.
Bytes handler(const Variant& variant) {
  Bytes picture;
  if (!variant.inPicture.empty()) {
    picture = joined({awaitPicture(),
                      {0xA2u, 0x30u, 0xCAu, 0xD0u, 0xFDu},  // LDX #$30 ; DEX ; BNE
                      variant.inPicture});
  }
  return joined({{0x48u, 0xDAu},         // PHA ; PHX
                 {0xADu, 0x10u, 0x42u},  // LDA $4210
                 {0xE6u, kFrameCounter}, // INC $10
                 variant.inBlank,
                 picture,
                 {0xFAu, 0x68u, 0x40u}});  // PLX ; PLA ; RTI
}

// The interrupt handler: the variant's body, then the flag read: PHA ; body ; LDA
// $4211 ; PLA ; RTI.
Bytes irqHandler(const Variant& variant) {
  return joined({{0x48u}, variant.irqBody, {0xADu, 0x11u, 0x42u}, {0x68u, 0x40u}});
}

// The scene's setup under forced blank: the characters, the two maps, the palette
// and the sprite table loaded by DMA, Mode 1 with both backgrounds and the sprites on
// the main screen, BG1 under mosaic, the two HDMA channels pointed at their tables
// and left off, and the interrupt's position where the variant has one.
Bytes setup(const Variant& variant) {
  Bytes out = joined({store(0x2100u, 0x80u),
                      store(0x2115u, 0x80u),
                      clear(0x2116u), clear(0x2117u),
                      dma(0u, 0x01u, 0x18u, kBgTiles, 128u),
                      clear(0x2116u), store(0x2117u, 0x20u),
                      dma(0u, 0x01u, 0x18u, kSpriteTiles, 128u),
                      clear(0x2116u), store(0x2117u, 0x08u),
                      dma(0u, 0x01u, 0x18u, kBg1Map, 2048u),
                      clear(0x2116u), store(0x2117u, 0x0Cu),
                      dma(0u, 0x01u, 0x18u, kBg2Map, 2048u),
                      clear(0x2121u),
                      dma(0u, 0x00u, 0x22u, kPalette, 512u),
                      clear(0x2102u), clear(0x2103u),
                      dma(0u, 0x00u, 0x04u, kOamTable, 544u),
                      store(0x2105u, 0x01u),
                      store(0x2106u, 0x31u),
                      store(0x2107u, 0x08u), store(0x2108u, 0x0Cu),
                      clear(0x210Bu),
                      store(0x2101u, 0x01u),
                      store(0x212Cu, 0x13u), clear(0x212Du),
                      hdmaChannel(1u, 0x06u, kHdmaMosaic),
                      hdmaChannel(2u, 0x33u, kHdmaHires),
                      clear(0x420Cu)});
  if (variant.irq) {
    out = joined({out, store(0x4207u, static_cast<std::uint8_t>(variant.htime & 0xFFu)),
                  store(0x4208u, static_cast<std::uint8_t>(variant.htime >> 8)),
                  store(0x4209u, static_cast<std::uint8_t>(variant.vtime & 0xFFu)),
                  store(0x420Au, static_cast<std::uint8_t>(variant.vtime >> 8))});
  }
  return out;
}

// The scene lit and running: forced blank released at the next vertical blank, NMI
// on, the H/V interrupt on where the variant has one, interrupts enabled, and the
// main loop waiting for the next one.
Bytes go(const Variant& variant) {
  return joined({awaitVblank(), store(0x2100u, 0x0Fu),
                 store(0x4200u, variant.irq ? 0xB0u : 0x80u),
                 {0x58u, 0xCBu, 0x80u, 0xFDu}});  // CLI ; WAI ; BRA -3
}

// A one-bank LoROM image: the program at $8000, the data at its places, the two
// handlers and the vectors.
Bytes image(const Variant& variant) {
  Bytes rom(0x8000u, 0x00u);
  const auto place = [&rom](std::uint16_t address, const Bytes& bytes) {
    std::copy(bytes.begin(), bytes.end(), rom.begin() + (address - 0x8000u));
  };
  const Bytes program = joined({{0x78u}, setup(variant), go(variant)});
  const Bytes nmi = handler(variant);
  const Bytes irq = irqHandler(variant);
  if (program.size() > kBgTiles - 0x8000u) throw std::length_error("the program runs into the data");
  if (nmi.size() > kIrqHandler - kNmiHandler) throw std::length_error("the handler runs into the interrupt's");
  place(0x8000u, program);
  place(kNmiHandler, nmi);
  place(kIrqHandler, irq);
  place(kBgTiles, characterSet(0u));
  place(kSpriteTiles, characterSet(1u));
  place(kBg1Map, tilemap(0u));
  place(kBg2Map, tilemap(1u));
  place(kPalette, palette());
  place(kOamTable, oamTable());
  place(kHdmaMosaic, mosaicTable());
  place(kHdmaHires, hiresTable());
  rom[0x7FFAu] = static_cast<std::uint8_t>(kNmiHandler & 0xFFu);  // emulation-mode NMI
  rom[0x7FFBu] = static_cast<std::uint8_t>(kNmiHandler >> 8);
  rom[0x7FFCu] = 0x00u;  // reset -> $8000
  rom[0x7FFDu] = 0x80u;
  rom[0x7FFEu] = static_cast<std::uint8_t>(kIrqHandler & 0xFFu);  // emulation-mode IRQ
  rom[0x7FFFu] = static_cast<std::uint8_t>(kIrqHandler >> 8);
  return rom;
}

Bytes scene(const Variant& variant = {}) { return image(variant); }

SnesConfig config(const Bytes& rom, bool verify, bool pictureThread) {
  return SnesConfig{
      .rom = rom,
      .region = Region::Ntsc,
      .iplStub = true,
      .map = std::nullopt,
      .saveRamBytes = std::nullopt,
      .bootRom = std::nullopt,
      .apuThread = false,
      .ppuThread = pictureThread,
      .ppuVerify = verify,
  };
}

// A finished frame, kept whole, with the frame counter the handler had reached when
// it was handed over and the machine's counts for it.
struct Frame {
  unsigned width = 0;
  unsigned height = 0;
  std::uint8_t field = 0;
  Bytes pixels;
  [[nodiscard]] bool operator==(const Frame&) const = default;
};
struct Report {
  std::uint8_t counter = 0;
  Snes::LineReuse counts{};
  PpuState::MainDecision lastMain{};  // the decision the picture's last dot left, as the report found it
  Frame frame;
};

// Every frame handed over, tagged as above; the machine's counts are read inside the
// report, where they are the finished frame's.
struct Reports final : FrameObserver {
  Snes* machine = nullptr;
  std::vector<Report> reports;
  void frame(const VideoFrame& picture) override {
    reports.push_back(Report{.counter = machine->state().wram[kFrameCounter],
                             .counts = machine->lineReuse(),
                             .lastMain = machine->state().ppu.lastMain,
                             .frame = Frame{.width = picture.width,
                                            .height = picture.height,
                                            .field = picture.field,
                                            .pixels = Bytes(picture.pixels.begin(), picture.pixels.end())}});
  }
  // The report of the picture drawn after the vertical blank in which the handler
  // counted `counter`. A report is made as the next blank begins, before the handler
  // counts again, so a write the handler makes at count N is in the picture of
  // report N. The handler counts 1 in the blank that lights the screen, so the last
  // report at count 0 is the last blank picture, report 1 the first lit one and 2
  // the first a still scene takes whole. Every report before that blank reads 0, and
  // the last of them is the one answered.
  [[nodiscard]] const Report& at(std::uint8_t counter) const {
    for (auto report = reports.rbegin(); report != reports.rend(); ++report) {
      if (report->counter == counter) return *report;
    }
    throw std::out_of_range("no report with that count");
  }
  [[nodiscard]] bool has(std::uint8_t counter) const {
    return std::any_of(reports.begin(), reports.end(),
                       [counter](const Report& r) { return r.counter == counter; });
  }
};

// The same cartridge on the machine that reuses and the one that verifies, run with
// the same budgets and held equal at every return.
struct Pair {
  explicit Pair(const Bytes& rom, bool watch = true, bool pictureThread = false)
      : reusing(config(rom, false, pictureThread)), verifying(config(rom, true, pictureThread)) {
    reusingReports.machine = &reusing;
    verifyingReports.machine = &verifying;
    if (watch) {
      reusing.setFrameObserver(&reusingReports);
      verifying.setFrameObserver(&verifyingReports);
    }
  }
  Pair(const Pair&) = delete;
  Pair& operator=(const Pair&) = delete;
  Snes reusing;
  Snes verifying;
  Reports reusingReports;
  Reports verifyingReports;

  // The whole state equal, every frame since the last look equal, and the verifying
  // machine in agreement with its own record on every line.
  [[nodiscard]] ::testing::AssertionResult same() {
    if (!(reusing.state() == verifying.state())) {
      return ::testing::AssertionFailure() << "the states differ at master " << reusing.state().master;
    }
    const std::vector<Report>& a = reusingReports.reports;
    const std::vector<Report>& b = verifyingReports.reports;
    if (a.size() != b.size()) {
      return ::testing::AssertionFailure() << a.size() << " frames against " << b.size();
    }
    for (std::size_t i = checked; i < a.size(); ++i) {
      if (!(a[i].frame == b[i].frame)) {
        return ::testing::AssertionFailure() << "frame " << i << " (count " << unsigned{a[i].counter}
                                             << ") differs: " << a[i].frame.width << "x" << a[i].frame.height
                                             << " against " << b[i].frame.width << "x" << b[i].frame.height;
      }
      if (b[i].counts.mismatches != 0u) {
        return ::testing::AssertionFailure() << "the verifying machine found " << b[i].counts.mismatches
                                             << " mismatched lines in frame " << i;
      }
      if (a[i].counts.reused + a[i].counts.drawn != kLines && a[i].counter != 0u) {
        return ::testing::AssertionFailure() << "frame " << i << " counts " << a[i].counts.reused << " + "
                                             << a[i].counts.drawn << " lines";
      }
      const Snes::LineReuse& c = a[i].counts;
      if (c.firstDrawn + c.registers + c.sprites + c.vram + c.cgram + c.midLine + c.returned != c.drawn) {
        return ::testing::AssertionFailure() << "frame " << i << ": the reasons do not sum to the lines drawn";
      }
    }
    checked = a.size();
    return ::testing::AssertionSuccess();
  }
  std::size_t checked = 0;
  // The reports so far dropped, after a restore or a reset starts the count again.
  void forget() {
    reusingReports.reports.clear();
    verifyingReports.reports.clear();
    checked = 0;
  }

  void run(std::uint64_t budget) {
    reusing.run(budget);
    verifying.run(budget);
  }
  // Whole frames until both machines have reported count `counter`, holding them
  // equal after every frame.
  [[nodiscard]] ::testing::AssertionResult runTo(std::uint8_t counter, int atMost = 40) {
    for (int frame = 0; frame < atMost; ++frame) {
      run(kFrame);
      const ::testing::AssertionResult equal = same();
      if (!equal) return equal;
      if (reusingReports.has(counter) && verifyingReports.has(counter)) return ::testing::AssertionSuccess();
    }
    return ::testing::AssertionFailure() << "count " << unsigned{counter} << " not reached";
  }
  [[nodiscard]] const Snes::LineReuse& counts(std::uint8_t counter) const {
    return reusingReports.at(counter).counts;
  }
};

// A frame's counts, for a failure message.
std::string describe(const Snes::LineReuse& c) {
  return "reused " + std::to_string(c.reused) + " drawn " + std::to_string(c.drawn) + " (firstDrawn " +
         std::to_string(c.firstDrawn) + " registers " + std::to_string(c.registers) + " sprites " +
         std::to_string(c.sprites) + " vram " + std::to_string(c.vram) + " cgram " + std::to_string(c.cgram) +
         " midLine " + std::to_string(c.midLine) + " returned " + std::to_string(c.returned) + ") mismatches " +
         std::to_string(c.mismatches);
}

// Whether a frame holds a pixel that is not black.
bool lit(const Frame& frame) {
  for (std::size_t at = 0; at + 3u < frame.pixels.size(); at += 4u) {
    if (frame.pixels[at] != 0u || frame.pixels[at + 1u] != 0u || frame.pixels[at + 2u] != 0u) return true;
  }
  return false;
}

TEST(SnesLineReuse, ASecondFrameOfAStillPictureIsTakenWholeFromTheFirst) {
  // The handler counts 1 in the vertical blank that lights the screen, so the first
  // lit picture is report 1 and the first that can be taken whole is report 2.
  Pair t(scene());
  ASSERT_TRUE(t.runTo(3));
  const Report& first = t.reusingReports.reports.front();
  EXPECT_EQ(first.counts.drawn, kLines) << "the very first picture has nothing to take";
  EXPECT_EQ(first.counts.reused, 0u);
  EXPECT_FALSE(lit(t.reusingReports.at(0).frame));
  EXPECT_TRUE(lit(t.reusingReports.at(1).frame)) << "the scene is lit from the first counted frame";
  // The screen came on, so every line's key changed; the one line the setup wrote
  // the chip inside of, in the blank frame, was never recorded and is drawn for that.
  EXPECT_EQ(t.counts(1).drawn, kLines) << describe(t.counts(1));
  EXPECT_EQ(t.counts(1).registers + t.counts(1).firstDrawn, kLines) << describe(t.counts(1));
  EXPECT_GE(t.counts(1).registers, kLines - 1u);
  EXPECT_EQ(t.counts(2).reused, kLines) << describe(t.counts(2));
  EXPECT_EQ(t.counts(2).drawn, 0u);
  EXPECT_EQ(t.reusingReports.at(2).frame.pixels, t.reusingReports.at(1).frame.pixels);
  EXPECT_EQ(t.reusingReports.at(2).frame.width, t.reusingReports.at(1).frame.width);
  EXPECT_EQ(t.counts(3).reused, kLines);
}

// A write made once, in the vertical blank before frame 3's picture, to one register
// the key carries: frame 3 draws every line and frame 4 takes every line again.
struct RegisterWrite {
  std::uint16_t address;
  std::uint8_t value;
  bool scrollOnly;
};

TEST(SnesLineReuse, AChangedRegisterDrawsEveryLineOfTheNextFrame) {
  const std::array<RegisterWrite, 22> writes{{
      {0x2100u, 0x0Eu, false}, {0x2105u, 0x09u, false}, {0x2106u, 0x32u, false}, {0x2107u, 0x0Cu, false},
      {0x2108u, 0x08u, false}, {0x210Bu, 0x11u, false}, {0x210Du, 0x05u, true},  {0x2111u, 0x05u, true},
      {0x211Bu, 0x12u, false}, {0x211Fu, 0x34u, false}, {0x2123u, 0x03u, false}, {0x2126u, 0x40u, false},
      {0x2129u, 0x80u, false}, {0x212Au, 0x05u, false}, {0x212Cu, 0x12u, false}, {0x212Du, 0x01u, false},
      {0x212Eu, 0x01u, false}, {0x212Fu, 0x01u, false}, {0x2130u, 0x02u, false}, {0x2131u, 0x3Fu, false},
      {0x2132u, 0x3Fu, false}, {0x2133u, 0x08u, false},
  }};
  for (const RegisterWrite& write : writes) {
    SCOPED_TRACE(::testing::Message() << "register $" << std::hex << write.address);
    Pair t(scene(Variant{.inBlank = onFrame(3u, store(write.address, write.value))}));
    ASSERT_TRUE(t.runTo(4));
    EXPECT_EQ(t.counts(2).reused, kLines) << "before the write";
    EXPECT_EQ(t.counts(3).registers, kLines) << "the frame after the write";
    EXPECT_EQ(t.counts(3).scrollOnly, write.scrollOnly ? kLines : 0u);
    EXPECT_EQ(t.counts(4).reused, kLines) << "the frame after that";
  }
}

TEST(SnesLineReuse, AMosaicRowBeginningOnAnotherLineDrawsTheLinesItReaches) {
  // Channel 1 on for frame 3's picture alone: its row beginning on line 53 runs six
  // lines, so every row after it begins two lines later than the frame before's, and
  // lines 53 to 224 are drawn; frame 4, back on the frame-2 phase, draws the same 172
  // against frame 3's records; frame 5 takes every line again.
  Pair t(scene(Variant{.inBlank = joined({onFrame(3u, store(0x420Cu, 0x02u)), onFrame(4u, clear(0x420Cu))})}));
  ASSERT_TRUE(t.runTo(5));
  EXPECT_EQ(t.counts(2).reused, kLines);
  EXPECT_EQ(t.counts(3).registers, 172u);
  EXPECT_EQ(t.counts(3).reused, 52u);
  EXPECT_EQ(t.counts(4).registers, 172u);
  EXPECT_EQ(t.counts(4).reused, 52u);
  EXPECT_EQ(t.counts(5).reused, kLines);
}

TEST(SnesLineReuse, AVramWriteDrawsTheLinesThatReadItsPage) {
  // One word of BG1's map, in page 4, rewritten before frame 3: BG1's blocks whose
  // corner line falls in map rows 0 to 15 read the page, which is lines 1 to 128;
  // then a word of page 32, which no line reads, before frame 5, and the first word
  // again, unchanged, before frame 7. (BG3 is not shown but is read, over a map the
  // characters' bytes make, so its reads reach pages the scene never filled, up to
  // page 15.)
  const Bytes write = joined({store(0x2115u, 0x80u), store(0x2116u, static_cast<std::uint8_t>(kBg1MapWord & 0xFFu)),
                              store(0x2117u, static_cast<std::uint8_t>(kBg1MapWord >> 8)), store(0x2118u, 0xFFu),
                              store(0x2119u, 0x03u)});
  const Bytes unread = joined({store(0x2115u, 0x80u), clear(0x2116u), store(0x2117u, 0x40u), store(0x2118u, 0x55u)});
  Pair t(scene(Variant{.inBlank = joined({onFrame(3u, write), onFrame(5u, unread), onFrame(7u, write)})}));
  ASSERT_TRUE(t.runTo(7));
  EXPECT_EQ(t.counts(2).reused, kLines);
  EXPECT_EQ(t.counts(3).vram, 128u);
  EXPECT_EQ(t.counts(3).reused, 96u);
  EXPECT_EQ(t.counts(4).reused, kLines);
  EXPECT_EQ(t.counts(5).reused, kLines) << "a page no line reads";
  EXPECT_EQ(t.counts(7).reused, kLines) << "the same word written through the port again changes no byte";
}

TEST(SnesLineReuse, APaletteWriteDrawsTheLinesThatReadItsGroup) {
  // The sprites' color, word 146 of group 9, rewritten before frame 3: the eight lines
  // each of the four sprites stands on read it; then word 0, in group 0, which every
  // line reads through the backgrounds' palette 0, before frame 5.
  const Bytes sprites = joined({store(0x2121u, kSpritePaletteWord), store(0x2122u, 0x1Fu), store(0x2122u, 0x00u)});
  const Bytes backdrop = joined({clear(0x2121u), store(0x2122u, 0x00u), store(0x2122u, 0x7Cu)});
  Pair t(scene(Variant{.inBlank = joined({onFrame(3u, sprites), onFrame(5u, backdrop)})}));
  ASSERT_TRUE(t.runTo(5));
  EXPECT_EQ(t.counts(2).reused, kLines);
  EXPECT_EQ(t.counts(3).cgram, 32u);
  EXPECT_EQ(t.counts(3).reused, kLines - 32u);
  EXPECT_EQ(t.counts(4).reused, kLines);
  EXPECT_EQ(t.counts(5).cgram, kLines);
}

TEST(SnesLineReuse, AMovedSpriteDrawsTheLinesItCrossesAndCrossed) {
  // Sprite 0's Y from 30 to 50 before frame 3: lines 31 to 38 lose it and 51 to 58 gain
  // it.
  const Bytes move = joined({clear(0x2102u), clear(0x2103u), store(0x2104u, 40u), store(0x2104u, 50u)});
  Pair t(scene(Variant{.inBlank = onFrame(3u, move)}));
  ASSERT_TRUE(t.runTo(4));
  EXPECT_EQ(t.counts(2).reused, kLines);
  EXPECT_EQ(t.counts(3).sprites, 16u);
  EXPECT_EQ(t.counts(3).reused, kLines - 16u);
  EXPECT_EQ(t.counts(4).reused, kLines);
}

TEST(SnesLineReuse, ASpritesPriorityBitsAloneDrawItsLines) {
  // Sprite 0's priority from 3 to 0 before frame 3, its word unchanged: the buffer's
  // priority differs on its eight lines, and the sprite falls behind BG1's tiles.
  const Bytes lower = joined({store(0x2102u, 1u), clear(0x2103u), store(0x2104u, 1u), store(0x2104u, 0x02u)});
  Pair t(scene(Variant{.inBlank = onFrame(3u, lower)}));
  ASSERT_TRUE(t.runTo(4));
  EXPECT_EQ(t.counts(3).sprites, 8u);
  EXPECT_EQ(t.counts(3).reused, kLines - 8u);
  EXPECT_EQ(t.counts(4).reused, kLines);
}

TEST(SnesLineReuse, AWriteInsideTheLineDrawsItAndItIsNotKeptForTheNextFrame) {
  // A transfer of 64 bytes to BG1's scroll register begins a line into each picture
  // through frame 3's: its writes land inside one or two lines' picture dots, and
  // leave the scroll where it was. Those lines are drawn and the rest taken; once
  // the transfers stop, the lines' records from before them stand and every line is
  // taken again.
  Pair t(scene(Variant{.inPicture = beforeFrame(4u, dma(3u, 0x00u, 0x0Du, kZeros, 64u))}));
  ASSERT_TRUE(t.runTo(5));
  EXPECT_GE(t.counts(1).midLine, 1u);
  for (std::uint8_t counter = 2u; counter <= 3u; ++counter) {
    SCOPED_TRACE(::testing::Message() << "count " << unsigned{counter});
    const Snes::LineReuse& c = t.counts(counter);
    EXPECT_GE(c.midLine, 1u);
    EXPECT_LE(c.midLine, 2u);
    EXPECT_EQ(c.drawn, c.midLine);
    EXPECT_EQ(c.reused, kLines - c.midLine);
  }
  // The transfers' lines were never recorded lit: frame 4 draws them against the
  // blank frame's record, and frame 5 takes every line.
  EXPECT_EQ(t.counts(4).drawn, t.counts(4).registers);
  EXPECT_LE(t.counts(4).drawn, 2u);
  EXPECT_EQ(t.counts(5).reused, kLines);
}

// Where the interrupt's first write to the watched register landed on the verifying
// machine, in master cycles into the line, and the line: the first such write inside
// a picture line once the handler has counted, which leaves out the setup's own.
struct WriteSite final : BusObserver {
  const Snes* machine = nullptr;
  std::uint16_t watched = 0x2100u;
  std::optional<std::uint16_t> hpos;
  std::uint16_t vpos = 0;
  void access(const BusAccess& a) override {
    if (!a.write || (a.address & 0xFFFFu) != watched || hpos.has_value()) return;
    if (machine->state().vpos == 0u || machine->state().inVblank) return;
    if (machine->state().wram[kFrameCounter] == 0u) return;
    hpos = machine->state().hpos;
    vpos = machine->state().vpos;
  }
  void internal(std::uint32_t, std::optional<CycleKind>) override {}
};

TEST(SnesLineReuse, AWriteBeforeTheFirstReadLeavesTheLineReusable) {
  // The interrupt on line 100 writes $2100 with the value it holds. Timed to land
  // before the line's first read dot, the write changes nothing the key sees and
  // every line is taken; timed to land inside the picture dots, its line is drawn
  // every frame.
  {
    Pair t(scene(Variant{.irqBody = store(0x2100u, 0x0Fu), .htime = 307u, .vtime = 100u, .irq = true}));
    WriteSite site;
    site.machine = &t.verifying;
    t.verifying.setObserver(&site);
    ASSERT_TRUE(t.runTo(3));
    ASSERT_TRUE(site.hpos.has_value());
    EXPECT_LT(*site.hpos, 12u) << "the write lands before the line's first read dot, on line " << site.vpos;
    EXPECT_EQ(t.counts(2).reused, kLines);
    EXPECT_EQ(t.counts(3).reused, kLines);
  }
  {
    Pair t(scene(Variant{.irqBody = store(0x2100u, 0x0Fu), .htime = 316u, .vtime = 100u, .irq = true}));
    WriteSite site;
    site.machine = &t.verifying;
    t.verifying.setObserver(&site);
    ASSERT_TRUE(t.runTo(3));
    ASSERT_TRUE(site.hpos.has_value());
    EXPECT_GE(*site.hpos, 12u) << "the write lands inside the picture dots, on line " << site.vpos;
    EXPECT_LT(*site.hpos, 1112u);
    EXPECT_EQ(t.counts(2).midLine, 1u);
    EXPECT_EQ(t.counts(2).reused, kLines - 1u);
    EXPECT_EQ(t.counts(3).midLine, 1u);
  }
}

TEST(SnesLineReuse, AReadInsideTheLineLeavesItReused) {
  // The interrupt reads $213F and $2137 inside line 100 every frame: a read moves
  // nothing a pixel reads, and every line is taken.
  Pair t(scene(Variant{.irqBody = {0xADu, 0x3Fu, 0x21u, 0xADu, 0x37u, 0x21u}, .htime = 100u, .vtime = 100u, .irq = true}));
  ASSERT_TRUE(t.runTo(3));
  EXPECT_EQ(t.counts(2).reused, kLines);
  EXPECT_EQ(t.counts(3).reused, kLines);
}

TEST(SnesLineReuse, APaletteWriteInALinesHorizontalBlankDrawsThatLineTheNextFrame) {
  // The interrupt on line 84, one of sprite 1's lines, writes the sprites' color in
  // the line's horizontal blank, once, in the first lit picture. The line's pixels
  // were drawn before the write and its record is stamped with the count at its
  // first read dot, so the next frame draws it, with every other line that read the
  // color before the write: sprite 0's eight and sprite 1's 81 to 83, twelve in all;
  // the lines after it read the new color already and are taken.
  const Bytes write = onFrame(1u, joined({store(0x2121u, kSpritePaletteWord), store(0x2122u, 0x1Fu),
                                          store(0x2122u, 0x00u)}));
  Pair t(scene(Variant{.irqBody = write, .htime = 250u, .vtime = 84u, .irq = true}));
  WriteSite site;
  site.machine = &t.verifying;
  site.watched = 0x2122u;
  t.verifying.setObserver(&site);
  ASSERT_TRUE(t.runTo(3));
  ASSERT_TRUE(site.hpos.has_value());
  EXPECT_EQ(site.vpos, 84u);
  EXPECT_GE(*site.hpos, 1112u) << "the write lands in the line's horizontal blank";
  EXPECT_EQ(t.counts(2).cgram, 12u) << describe(t.counts(2));
  EXPECT_EQ(t.counts(2).reused, kLines - 12u);
  EXPECT_EQ(t.counts(3).reused, kLines);
}

TEST(SnesLineReuse, ABlankLineHandsOnTheDecisionItWasGiven) {
  // The screen forced blank before frame 3, and in frame 4's picture the interrupt on
  // line 100 writes $2100 with the blank value, timed so the write's cycle ends in
  // dot 277: the chip reads the new value a dot early, so that dot is drawn lit,
  // under the byte the bus held before the write, and leaves a decision on the
  // line. A blank line clears only the `present` bit of the last main decision and
  // keeps the rest, so lines 101 to 224 of frame 4 are handed that decision where
  // their records, made blank, hold the one the lit frames left, and are drawn;
  // line 100 is drawn for the write. Frame 5 hands the new decision to lines 1 to
  // 100, whose records hold the old one, and takes the rest; frame 6 takes every
  // line.
  Pair t(scene(Variant{.inBlank = onFrame(3u, store(0x2100u, 0x80u)),
                       .irqBody = onFrame(4u, store(0x2100u, 0x80u)),
                       .htime = 227u,
                       .vtime = 100u,
                       .irq = true}));
  WriteSite site;
  site.machine = &t.verifying;
  t.verifying.setObserver(&site);
  ASSERT_TRUE(t.runTo(6));
  ASSERT_TRUE(site.hpos.has_value());
  EXPECT_EQ(site.vpos, 100u);
  EXPECT_GE(*site.hpos, 1108u) << "the write's cycle ends inside dot 277";
  EXPECT_LT(*site.hpos, 1112u);
  EXPECT_EQ(t.counts(3).registers, kLines) << describe(t.counts(3));
  EXPECT_EQ(t.counts(4).midLine, 1u) << describe(t.counts(4));
  EXPECT_EQ(t.counts(4).registers, 124u) << describe(t.counts(4));
  EXPECT_EQ(t.counts(4).reused, 99u);
  EXPECT_EQ(t.counts(5).registers, 100u) << describe(t.counts(5));
  EXPECT_EQ(t.counts(5).reused, 124u);
  EXPECT_EQ(t.counts(6).reused, kLines) << describe(t.counts(6));
  EXPECT_NE(t.reusingReports.at(4).lastMain.colour, t.reusingReports.at(3).lastMain.colour)
      << "the dot drawn lit left another decision";
  EXPECT_FALSE(t.reusingReports.at(4).lastMain.present);
  EXPECT_FALSE(lit(t.reusingReports.at(5).frame));
}

TEST(SnesLineReuse, AWriteToInidispEndingInTheLastPictureDotDrawsTheLine) {
  // The interrupt on line 100 writes $2100 with the value it holds, once, in frame
  // 2's picture, timed so the write's cycle ends inside dot 277: the chip reads the
  // new value a dot early, so that dot is drawn under the byte the bus held before
  // the write, and the line is drawn for it; frame 3's line, without the write, is
  // taken from frame 1's record.
  Pair t(scene(Variant{.irqBody = onFrame(2u, store(0x2100u, 0x0Fu)), .htime = 227u, .vtime = 100u, .irq = true}));
  WriteSite site;
  site.machine = &t.verifying;
  t.verifying.setObserver(&site);
  ASSERT_TRUE(t.runTo(3));
  ASSERT_TRUE(site.hpos.has_value());
  EXPECT_EQ(site.vpos, 100u);
  EXPECT_GE(*site.hpos, 1108u) << "the write's cycle ends inside dot 277";
  EXPECT_LT(*site.hpos, 1112u);
  EXPECT_EQ(t.counts(2).midLine, 1u) << describe(t.counts(2));
  EXPECT_EQ(t.counts(2).reused, kLines - 1u);
  EXPECT_EQ(t.counts(3).reused, kLines) << describe(t.counts(3));
}

TEST(SnesLineReuse, AReturnInAHorizontalBlankAfterAWriteThereLeavesTheLineAsDrawn) {
  // Channel 2 writes $2133 in the horizontal blank of lines 100 to 111 every frame,
  // and the machines run in budgets four cycles short of a line, so one return falls
  // in each line and walks every position of a line in turn: a return falling in a
  // line's horizontal blank, after the write, has nothing of the line left to draw,
  // and a line the frame before stands in for is complete from the record there,
  // not drawn under the register the write changed. Those lines are taken; the
  // lines a return fell inside are drawn up to it.
  for (const bool pictureThread : {false, true}) {
    SCOPED_TRACE(::testing::Message() << (pictureThread ? "the picture thread" : "the calling thread"));
    Pair t(scene(Variant{.inBlank = onFrame(2u, store(0x420Cu, 0x04u))}), true, pictureThread);
    ASSERT_TRUE(t.runTo(3));
    EXPECT_EQ(t.counts(3).reused, kLines) << describe(t.counts(3));
    for (int call = 0; call < 12000; ++call) {
      t.run(1360u);
      if (call % 400 == 0) {
        ASSERT_TRUE(t.same()) << "after run " << call;
      }
    }
    ASSERT_TRUE(t.same());
    const Snes::LineReuse& c = t.reusingReports.reports.back().counts;
    EXPECT_GT(c.reused, 0u) << describe(c);
    EXPECT_GT(c.returned, 0u) << describe(c);
    EXPECT_EQ(c.reused + c.returned, kLines) << describe(c);
  }
}

TEST(SnesLineReuse, AReturnInsideTheLineDrawsItUpToTheBeam) {
  // The machines stepped an instruction at a time through two frames, then run with a
  // budget that ends inside a line, equal at every return: a line pending at a return
  // is drawn up to the beam, so the tile reads and the last decision a host reads
  // there are the beam's.
  Pair t(scene());
  ASSERT_TRUE(t.runTo(1));
  for (int call = 0; call < 30000; ++call) {
    t.reusing.step();
    t.verifying.step();
    if (call % 500 == 0) {
      ASSERT_TRUE(t.same()) << "after step " << call;
    }
  }
  ASSERT_TRUE(t.same());
  for (int call = 0; call < 800; ++call) {
    t.run(1001u);
    if (call % 50 == 0) {
      ASSERT_TRUE(t.same()) << "after run " << call;
    }
  }
  ASSERT_TRUE(t.same());
  // The lines a return fell inside were drawn and are counted so; the rest taken.
  const Snes::LineReuse& c = t.reusingReports.reports.back().counts;
  EXPECT_GT(c.returned, 0u);
  EXPECT_EQ(c.returned + c.reused, kLines);
}

TEST(SnesLineReuse, ARestoreDrawsEveryLineOfTheNextFrame) {
  Pair t(scene());
  ASSERT_TRUE(t.runTo(2));
  const SnesState saved = t.reusing.state();
  ASSERT_TRUE(t.runTo(4));
  t.reusing.restore(saved);
  t.verifying.restore(saved);
  t.forget();
  ASSERT_TRUE(t.same());
  // The count runs again from the snapshot's 3: the first picture after the restore
  // draws every line, the one after takes them.
  ASSERT_TRUE(t.runTo(4));
  EXPECT_EQ(t.counts(3).firstDrawn, kLines);
  EXPECT_EQ(t.counts(4).reused, kLines);
  // A reset keeps work RAM, so the count runs on from where it stood; the program
  // sets the scene up again under forced blank and lights it at the next blank,
  // where the handler counts once more. The first picture after the reset draws
  // every line, and the first lit one the handler counted is taken whole a frame
  // later.
  const std::uint8_t before = t.reusing.state().wram[kFrameCounter];
  t.reusing.reset();
  t.verifying.reset();
  t.forget();
  ASSERT_TRUE(t.same());
  ASSERT_TRUE(t.runTo(static_cast<std::uint8_t>(before + 2u), 60));
  EXPECT_EQ(t.reusingReports.reports.front().counts.drawn, kLines) << describe(t.reusingReports.reports.front().counts);
  EXPECT_EQ(t.reusingReports.reports.front().counts.reused, 0u);
  EXPECT_EQ(t.counts(static_cast<std::uint8_t>(before + 1u)).drawn, kLines);
  EXPECT_EQ(t.counts(static_cast<std::uint8_t>(before + 2u)).reused, kLines);
}

TEST(SnesLineReuse, AHostWriteToAVideoMemoryDrawsTheLinesThatReadIt) {
  Pair t(scene());
  ASSERT_TRUE(t.runTo(2));
  // BG1's map, page 4: lines 1 to 128 read it.
  t.reusing.writeVram(0x1000u, 0xFFu);
  t.verifying.writeVram(0x1000u, 0xFFu);
  ASSERT_TRUE(t.runTo(3));
  EXPECT_EQ(t.counts(3).vram, 128u);
  EXPECT_EQ(t.counts(3).reused, 96u);
  // The same byte again: nothing changed, every line taken.
  t.reusing.writeVram(0x1000u, 0xFFu);
  t.verifying.writeVram(0x1000u, 0xFFu);
  ASSERT_TRUE(t.runTo(4));
  EXPECT_EQ(t.counts(4).reused, kLines);
  // The sprites' color: their 32 lines.
  t.reusing.writeCgram(kSpritePaletteWord * 2u, 0x1Fu);
  t.verifying.writeCgram(kSpritePaletteWord * 2u, 0x1Fu);
  ASSERT_TRUE(t.runTo(5));
  EXPECT_EQ(t.counts(5).cgram, 32u);
  EXPECT_EQ(t.counts(5).reused, kLines - 32u);
  // Sprite 0's Y: the lines it leaves and the lines it reaches.
  t.reusing.writeOam(1u, 50u);
  t.verifying.writeOam(1u, 50u);
  ASSERT_TRUE(t.runTo(6));
  EXPECT_EQ(t.counts(6).sprites, 16u);
  EXPECT_EQ(t.counts(6).reused, kLines - 16u);
  ASSERT_TRUE(t.runTo(7));
  EXPECT_EQ(t.counts(7).reused, kLines);
}

TEST(SnesLineReuse, ALineTakenFromTheFrameBeforeIsCopiedAtTheFramesWidth) {
  // Channel 2 sets $2133 bit 3 on lines 101 to 111 of frame 3's picture, off again
  // for frame 4's, on again for frame 5's: the eleven lines are drawn each time and
  // the rest taken, into a frame 512 wide, then 256, then 512.
  Pair t(scene(Variant{.inBlank = joined({onFrame(3u, store(0x420Cu, 0x04u)), onFrame(4u, clear(0x420Cu)),
                                           onFrame(5u, store(0x420Cu, 0x04u))})}));
  ASSERT_TRUE(t.runTo(6));
  EXPECT_EQ(t.counts(2).reused, kLines);
  for (std::uint8_t counter = 3u; counter <= 5u; ++counter) {
    SCOPED_TRACE(::testing::Message() << "count " << unsigned{counter});
    EXPECT_EQ(t.counts(counter).registers, 11u);
    EXPECT_EQ(t.counts(counter).reused, kLines - 11u);
    EXPECT_EQ(t.reusingReports.at(counter).frame.width, counter == 4u ? 256u : 512u);
  }
  EXPECT_EQ(t.counts(6).reused, kLines);
  EXPECT_EQ(t.reusingReports.at(6).frame.width, 512u);
}

TEST(SnesLineReuse, AMachineNobodyWatchesTakesItsTileReadsAndItsDecisionFromTheFrameBefore) {
  Pair t(scene(), /*watch=*/false);
  for (int frame = 0; frame < 6; ++frame) {
    t.run(kFrame);
    ASSERT_TRUE(t.reusing.state() == t.verifying.state()) << "after frame " << frame;
  }
  const Snes::LineReuse c = t.reusing.lineReuse();
  EXPECT_EQ(c.reused, kLines);
  EXPECT_EQ(t.verifying.lineReuse().mismatches, 0u);
  EXPECT_TRUE(t.reusing.state().ppu.lastMain.present);
}

TEST(SnesLineReuse, AnObserverSetAfterAFrameDrawsTheNextFrameWhole) {
  // Unwatched for six frames, watched from the picture after them: the records made
  // unwatched hold no pixels, so the first watched picture draws every line; cleared
  // after the one that follows it, the record's pixels go unread and every line is
  // still taken. The six frames end at the next one's first line, so the first watched
  // picture is reported at the count the handler has reached.
  Pair t(scene(), /*watch=*/false);
  for (int frame = 0; frame < 6; ++frame) t.run(kFrame);
  ASSERT_TRUE(t.reusing.state() == t.verifying.state());
  ASSERT_GE(t.reusing.state().wram[kFrameCounter], 2u);
  t.reusing.setFrameObserver(&t.reusingReports);
  t.verifying.setFrameObserver(&t.verifyingReports);
  const std::uint8_t from = t.reusing.state().wram[kFrameCounter];
  ASSERT_TRUE(t.runTo(static_cast<std::uint8_t>(from + 1u)));
  EXPECT_EQ(t.counts(from).firstDrawn, kLines);
  EXPECT_EQ(t.counts(static_cast<std::uint8_t>(from + 1u)).reused, kLines);
  t.reusing.setFrameObserver(nullptr);
  t.verifying.setFrameObserver(nullptr);
  t.run(kFrame);
  t.run(kFrame);
  ASSERT_TRUE(t.reusing.state() == t.verifying.state());
  EXPECT_EQ(t.reusing.lineReuse().reused, kLines);
}

TEST(SnesLineReuse, TheThreadedMachineReusesAsTheLockstepMachineDoes) {
  {
    Pair t(scene(), /*watch=*/true, /*pictureThread=*/true);
    ASSERT_TRUE(t.runTo(3));
    EXPECT_EQ(t.counts(1).drawn, kLines);
    EXPECT_EQ(t.counts(2).reused, kLines);
    EXPECT_EQ(t.counts(3).reused, kLines);
  }
  {
    Pair t(scene(Variant{.inPicture = beforeFrame(4u, dma(3u, 0x00u, 0x0Du, kZeros, 64u))}), true, true);
    ASSERT_TRUE(t.runTo(5));
    for (std::uint8_t counter = 2u; counter <= 3u; ++counter) {
      EXPECT_GE(t.counts(counter).midLine, 1u) << "count " << unsigned{counter};
      EXPECT_EQ(t.counts(counter).drawn, t.counts(counter).midLine);
    }
    EXPECT_EQ(t.counts(5).reused, kLines);
  }
  {
    Pair t(scene(), true, true);
    ASSERT_TRUE(t.runTo(1));
    for (int call = 0; call < 800; ++call) {
      t.run(1001u);
      if (call % 50 == 0) {
        ASSERT_TRUE(t.same()) << "after run " << call;
      }
    }
    ASSERT_TRUE(t.same());
    const Snes::LineReuse& c = t.reusingReports.reports.back().counts;
    EXPECT_GT(c.returned, 0u);
    EXPECT_EQ(c.returned + c.reused, kLines);
  }
  {
    Pair t(scene(Variant{.inBlank = joined({onFrame(3u, store(0x420Cu, 0x04u)), onFrame(4u, clear(0x420Cu)),
                                             onFrame(5u, store(0x420Cu, 0x04u))})}),
           true, true);
    ASSERT_TRUE(t.runTo(6));
    for (std::uint8_t counter = 3u; counter <= 5u; ++counter) {
      EXPECT_EQ(t.counts(counter).registers, 11u) << "count " << unsigned{counter};
      EXPECT_EQ(t.reusingReports.at(counter).frame.width, counter == 4u ? 256u : 512u);
    }
  }
}

TEST(SnesLineReuse, TheCountsAreTheLastFinishedFrames) {
  Pair t(scene());
  ASSERT_TRUE(t.runTo(2));
  const Snes::LineReuse inReport = t.counts(2);
  EXPECT_EQ(inReport.reused, kLines);
  // A budget ending inside the next picture, past its first line: the counts are
  // still frame 2's.
  t.run(kFrame / 2u);
  ASSERT_GT(t.reusing.state().vpos, 0u);
  ASSERT_FALSE(t.reusing.state().inVblank);
  EXPECT_EQ(t.reusing.lineReuse(), inReport);
  ASSERT_TRUE(t.same());
}

TEST(SnesLineReuse, TheCountsRollAsTheBlankBegins) {
  // A register written before frame 3 makes frame 3's counts differ from frame 2's.
  // Frame 2's counts are the answer from its report, across the next frame's first
  // line and into its picture, and frame 3's from its own report on, on both
  // machines, with the picture drawn on either thread.
  for (const bool pictureThread : {false, true}) {
    SCOPED_TRACE(::testing::Message() << (pictureThread ? "the picture thread" : "the calling thread"));
    Pair t(scene(Variant{.inBlank = onFrame(3u, store(0x210Du, 0x05u))}), true, pictureThread);
    ASSERT_TRUE(t.runTo(2));
    ASSERT_NE(t.counts(2), t.reusingReports.at(1).counts);
    while (t.reusing.state().inVblank || t.reusing.state().vpos == 0u) t.run(kFrame / 8u);
    ASSERT_FALSE(t.reusingReports.has(3));
    EXPECT_EQ(t.reusing.lineReuse(), t.counts(2)) << "inside frame 3's picture";
    EXPECT_EQ(t.verifying.lineReuse(), t.verifyingReports.at(2).counts);
    while (!t.reusingReports.has(3)) t.run(kFrame / 8u);
    ASSERT_TRUE(t.same());
    EXPECT_EQ(t.counts(3).registers, kLines) << describe(t.counts(3));
    EXPECT_EQ(t.reusing.lineReuse(), t.counts(3)) << "from frame 3's report on";
    EXPECT_EQ(t.verifying.lineReuse(), t.verifyingReports.at(3).counts);
  }
}

}  // namespace
}  // namespace snaggletooth
