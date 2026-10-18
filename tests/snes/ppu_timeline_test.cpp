// The picture as a timeline: a machine with a PpuTimelineObserver set hands every
// finished frame over as the registers, memories and writes it is drawn from, and
// PpuTimeline::draw draws the frame from that alone through the chip's own dot code.
// The oracle in every case is the machine's own picture: the frame a frame observer
// is handed is the frame draw() makes of the timeline, pixel for pixel. The cases pin
// where each part of the timeline is taken — the registers at a line's first read
// dot, an event at the first dot a write reaches, the early read of $2100, the
// palette and the sprite table as versions, VRAM's base and its writes inside the
// picture, Time's inputs as a line begins — and the report's terms: its cycle, its
// order beside the frame report, a throw, a restore part-way down a frame, the
// picture thread, and a machine that records changing nothing it does.
//
// The scene: Mode 1, BG1 and BG2 over distinct tilemaps, eight sprites, the backdrop
// rewritten by HDMA on every line, BG1's scroll written in the picture every frame,
// color math and BG2's mosaic switched by the frame, and an interrupt at a named dot
// of a named line for the writes that must land inside a line.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "snaggletooth/snes/ppu_timeline.h"
#include "snaggletooth/snes/snes.h"

namespace snaggletooth {
namespace {

// One NTSC frame in master cycles, and a quarter of one.
constexpr std::uint64_t kFrame = 357366u;
constexpr std::uint64_t kQuarterFrame = kFrame / 4u;

// Where the scene cartridge keeps its pieces in bank $00, and the work-RAM byte its
// NMI handler counts frames in.
constexpr std::uint16_t kBgTiles = 0x8400u;       // the program at $8000 runs to here; four 4bpp tiles
constexpr std::uint16_t kSpriteTiles = 0x8480u;   // four more, for the sprites
constexpr std::uint16_t kBg1Map = 0x8800u;        // one 32x32 screen, 2048 bytes
constexpr std::uint16_t kBg2Map = 0x9000u;
constexpr std::uint16_t kPalette = 0x9800u;       // 256 words
constexpr std::uint16_t kOamTable = 0x9A00u;      // 544 bytes
constexpr std::uint16_t kNmiHandler = 0xA200u;
constexpr std::uint16_t kIrqHandler = 0xA500u;
constexpr std::uint16_t kHdmaTable = 0xB000u;     // 901 bytes
constexpr std::uint16_t kHdmaConstant = 0xB400u;  // the same shape, one color
constexpr std::uint16_t kVramSource = 0xB800u;    // 64 bytes a transfer writes into VRAM
constexpr std::uint8_t kFrameCounter = 0x10u;

// The video memory the scene builds: the backgrounds' characters at byte 0, the
// sprites' at byte $4000 (name base 1), BG1's map at byte $1000 and BG2's at $1800.
// The transfer into VRAM lands on the solid background character, word $10.
constexpr std::uint16_t kSolidCharacterWord = 0x0010u;

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

// LDA $10 ; STA $abs: the frame counter stored.
Bytes storeCounter(std::uint16_t address) {
  return {0xA5u, kFrameCounter, 0x8Du, static_cast<std::uint8_t>(address & 0xFFu),
          static_cast<std::uint8_t>(address >> 8)};
}

// A general-purpose DMA on channel `channel` of `size` bytes from bank $00 address
// `source` to $2100 + `port` under transfer pattern `pattern`, started at once. A
// size of 0 is 65536 bytes.
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

// Four channels of 65536 bytes each from bank $00 to BG1's horizontal scroll
// register, all four started with one write.
Bytes burstDma() {
  Bytes out;
  for (std::uint8_t channel = 0u; channel < 4u; ++channel) {
    const auto reg = [channel](std::uint8_t index) {
      return static_cast<std::uint16_t>(0x4300u | (channel << 4) | index);
    };
    out = joined({out, store(reg(0u), 0x00u), store(reg(1u), 0x0Du), clear(reg(2u)), clear(reg(3u)),
                  clear(reg(4u)), clear(reg(5u)), clear(reg(6u))});
  }
  return joined({out, store(0x420Bu, 0x0Fu)});
}

// The VRAM port pointed at the solid character and 64 bytes written through it.
Bytes vramTransfer() {
  return joined({store(0x2116u, static_cast<std::uint8_t>(kSolidCharacterWord & 0xFFu)),
                 store(0x2117u, static_cast<std::uint8_t>(kSolidCharacterWord >> 8)),
                 dma(3u, 0x01u, 0x18u, kVramSource, 64u)});
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

// The sprite table: eight sprites across the picture at the four characters, every
// palette and priority, the even ones 16x16, and the rest at the origin on the blank
// character.
Bytes oamTable() {
  Bytes out(544u, 0u);
  for (unsigned sprite = 0u; sprite < 8u; ++sprite) {
    out[sprite * 4u] = static_cast<std::uint8_t>(20u + sprite * 28u);
    out[sprite * 4u + 1u] = static_cast<std::uint8_t>(30u + sprite * 20u);
    out[sprite * 4u + 2u] = static_cast<std::uint8_t>(sprite & 3u);
    out[sprite * 4u + 3u] = static_cast<std::uint8_t>(((sprite & 7u) << 1) | (((sprite >> 1) & 3u) << 4) |
                                                      ((sprite & 1u) << 6));
    if ((sprite & 1u) == 0u) out[512u + sprite / 4u] |= static_cast<std::uint8_t>(2u << ((sprite % 4u) * 2u));
  }
  return out;
}

// The HDMA table: for each of 224 lines, the palette address 0 twice and the
// backdrop's color, under transfer pattern 3 ($2121, $2121, $2122, $2122). The color
// of entry n is n * $0421, or one color throughout.
Bytes hdmaTable(std::optional<unsigned> constant) {
  Bytes out;
  const auto lines = [&](unsigned from, unsigned count) {
    out.push_back(static_cast<std::uint8_t>(0x80u | count));
    for (unsigned line = from; line < from + count; ++line) {
      const unsigned color = constant.has_value() ? *constant : (line * 0x0421u) & 0x7FFFu;
      out.push_back(0u);
      out.push_back(0u);
      out.push_back(static_cast<std::uint8_t>(color & 0xFFu));
      out.push_back(static_cast<std::uint8_t>(color >> 8));
    }
  };
  lines(0u, 127u);
  lines(127u, 97u);
  out.push_back(0u);
  return out;
}

// The color the HDMA table's entry n writes.
std::uint16_t hdmaColor(unsigned entry) { return static_cast<std::uint16_t>((entry * 0x0421u) & 0x7FFFu); }

// The 64 bytes the transfer writes into VRAM.
Bytes vramSource() {
  Bytes out;
  for (unsigned at = 0u; at < 64u; ++at) out.push_back(static_cast<std::uint8_t>(at * 37u + 5u));
  return out;
}

// What a case changes in the scene: the handler's blank-time part, after the count
// and the switches; its picture-time part, after the scroll write; an interrupt at dot
// `htime` of line `vtime` running `irqBody`; a still scene, with no HDMA, no scroll
// write and no switching; and the HDMA table writing one color on every line.
struct Variant {
  Bytes inBlank{};
  Bytes inPicture{};
  Bytes irqBody{};
  std::uint16_t htime = 0;
  std::uint16_t vtime = 0;
  bool irq = false;
  bool still = false;
  bool constantHdma = false;
};

// The scene's NMI handler: the flag read, the frame counted; for a scene that is not
// still, HDMA kept on, color math on every layer with halving for 32 frames in 64,
// BG2 under 4x4 mosaic blocks on every other frame; the variant's blank-time part; and
// for a scene that is not still, a wait for the picture to begin and a line more, and
// BG1's scroll written from the counter in the picture; the variant's picture-time
// part.
Bytes handler(const Variant& variant) {
  Bytes switches;
  Bytes picture;
  if (!variant.still) {
    switches = joined({store(0x420Cu, 0x02u),                          // HDMAEN = channel 1
                       {0xA5u, kFrameCounter, 0x29u, 0x20u, 0xF0u, 0x04u},  // LDA $10 ; AND #$20 ; BEQ +4
                       {0xA9u, 0x3Fu, 0x80u, 0x02u},                   // LDA #$3F ; BRA +2
                       {0xA9u, 0x00u},                                 // LDA #$00
                       {0x8Du, 0x31u, 0x21u},                          // STA $2131
                       {0xA5u, kFrameCounter, 0x29u, 0x01u, 0xF0u, 0x04u},  // LDA $10 ; AND #$01 ; BEQ +4
                       {0xA9u, 0x32u, 0x80u, 0x02u},                   // LDA #$32 ; BRA +2
                       {0xA9u, 0x00u},                                 // LDA #$00
                       {0x8Du, 0x06u, 0x21u}});                        // STA $2106
    picture = joined({awaitPicture(),
                      {0xA2u, 0x30u, 0xCAu, 0xD0u, 0xFDu},  // LDX #$30 ; DEX ; BNE
                      storeCounter(0x210Du),
                      clear(0x210Du),
                      variant.inPicture});
  } else if (!variant.inPicture.empty()) {
    picture = joined({awaitPicture(), {0xA2u, 0x30u, 0xCAu, 0xD0u, 0xFDu}, variant.inPicture});
  }
  return joined({{0x48u, 0xDAu},          // PHA ; PHX
                 {0xADu, 0x10u, 0x42u},   // LDA $4210
                 {0xE6u, kFrameCounter},  // INC $10
                 switches,
                 variant.inBlank,
                 picture,
                 {0xFAu, 0x68u, 0x40u}});  // PLX ; PLA ; RTI
}

// The interrupt handler: PHA ; body ; LDA $4211 ; PLA ; RTI.
Bytes irqHandler(const Variant& variant) {
  return joined({{0x48u}, variant.irqBody, {0xADu, 0x11u, 0x42u}, {0x68u, 0x40u}});
}

// The scene's setup under forced blank: the characters, the two maps, the palette
// and the sprite table loaded by DMA, Mode 1 with both backgrounds and the sprites on
// the main screen, the HDMA channel pointed at its table, and the interrupt's
// position where the variant has one.
Bytes setup(const Variant& variant) {
  const std::uint16_t table = variant.constantHdma ? kHdmaConstant : kHdmaTable;
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
                      store(0x2107u, 0x08u), store(0x2108u, 0x0Cu),
                      clear(0x210Bu),
                      store(0x2101u, 0x01u),
                      store(0x212Cu, 0x13u), clear(0x212Du),
                      store(0x4310u, 0x03u), store(0x4311u, 0x21u),
                      store(0x4312u, static_cast<std::uint8_t>(table & 0xFFu)),
                      store(0x4313u, static_cast<std::uint8_t>(table >> 8)), clear(0x4314u),
                      variant.still ? clear(0x420Cu) : store(0x420Cu, 0x02u)});
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
  return joined({awaitVblank(), store(0x2100u, 0x0Fu), store(0x4200u, variant.irq ? 0xB0u : 0x80u),
                 {0x58u, 0xCBu, 0x80u, 0xFDu}});  // CLI ; WAI ; BRA -3
}

// A one-bank LoROM image: `program` at $8000, the data at its places, the two
// handlers and the vectors.
Bytes image(const Bytes& program, const Variant& variant) {
  Bytes rom(0x8000u, 0x00u);
  const auto place = [&rom](std::uint16_t address, const Bytes& bytes) {
    std::copy(bytes.begin(), bytes.end(), rom.begin() + (address - 0x8000u));
  };
  const Bytes nmi = handler(variant);
  const Bytes irq = irqHandler(variant);
  if (program.size() > kBgTiles - 0x8000u) throw std::length_error("the program runs into the data");
  if (nmi.size() > kIrqHandler - kNmiHandler) throw std::length_error("the handler runs into the interrupt's");
  if (irq.size() > kHdmaTable - kIrqHandler) throw std::length_error("the interrupt runs into the tables");
  place(0x8000u, program);
  place(kNmiHandler, nmi);
  place(kIrqHandler, irq);
  place(kBgTiles, characterSet(0u));
  place(kSpriteTiles, characterSet(1u));
  place(kBg1Map, tilemap(0u));
  place(kBg2Map, tilemap(1u));
  place(kPalette, palette());
  place(kOamTable, oamTable());
  place(kHdmaTable, hdmaTable(std::nullopt));
  place(kHdmaConstant, hdmaTable(0x1234u));
  place(kVramSource, vramSource());
  rom[0x7FFAu] = static_cast<std::uint8_t>(kNmiHandler & 0xFFu);  // emulation-mode NMI
  rom[0x7FFBu] = static_cast<std::uint8_t>(kNmiHandler >> 8);
  rom[0x7FFCu] = 0x00u;  // reset -> $8000
  rom[0x7FFDu] = 0x80u;
  rom[0x7FFEu] = static_cast<std::uint8_t>(kIrqHandler & 0xFFu);  // emulation-mode IRQ
  rom[0x7FFFu] = static_cast<std::uint8_t>(kIrqHandler >> 8);
  return rom;
}

// The scene: SEI, the setup, and the run.
Bytes scene(const Variant& variant = {}) {
  return image(joined({{0x78u}, setup(variant), go(variant)}), variant);
}

// The scene with a write to $2100 in the picture every frame, made through a long
// store whose bank byte $8F is on the bus ahead of the value, so the chip's early
// read of the register draws one dot under $8F.
Bytes earlyRedraw() { return scene(Variant{.inPicture = {0xA9u, 0x0Fu, 0x8Fu, 0x00u, 0x21u, 0x8Fu}}); }

// The scene lit, then a burst: four channels write 65536 bytes each to BG1's
// horizontal scroll register while the picture is drawn, then forced blank and the
// scene loaded again.
Bytes burst() {
  const Variant variant{};
  return image(joined({{0x78u}, setup(variant), awaitVblank(), store(0x2100u, 0x0Fu), awaitPicture(),
                       burstDma(), setup(variant), go(variant)}),
               variant);
}

SnesConfig config(const Bytes& rom, bool pictureThread = false) {
  return SnesConfig{
      .rom = rom,
      .region = Region::Ntsc,
      .iplStub = true,
      .map = std::nullopt,
      .saveRamBytes = std::nullopt,
      .bootRom = std::nullopt,
      .apuThread = false,
      .ppuThread = pictureThread,
      .ppuVerify = false,
  };
}

// A finished frame, kept whole.
struct Frame {
  unsigned width = 0;
  unsigned height = 0;
  std::uint8_t field = 0;
  Bytes pixels{};
  [[nodiscard]] bool operator==(const Frame&) const = default;
};

// A timeline, kept whole: the frame's facts and a copy of every span.
struct Kept {
  unsigned height = 0;
  unsigned width = 0;
  std::uint8_t field = 0;
  unsigned firstLine = 0;
  std::vector<PpuTimeline::Line> lines{};
  std::vector<PpuTimeline::Event> events{};
  std::vector<PpuTimeline::VramWrite> vramWrites{};
  Bytes vram{};
  Bytes palettes{};
  Bytes oams{};
  Bytes spriteBuffers{};
  std::uint8_t counter = 0;  // the handler's count as the report was made
  std::uint64_t master = 0;  // the machine's master counter there

  // The timeline again, over this copy's storage.
  [[nodiscard]] PpuTimeline view() const {
    PpuTimeline timeline;
    timeline.height = height;
    timeline.width = width;
    timeline.field = field;
    timeline.firstLine = firstLine;
    timeline.lines = lines;
    timeline.events = events;
    timeline.vramWrites = vramWrites;
    timeline.vram = vram;
    timeline.palettes = palettes;
    timeline.oams = oams;
    timeline.spriteBuffers = spriteBuffers;
    return timeline;
  }
  // The two the same, every word and every byte.
  [[nodiscard]] bool sameAs(const Kept& other) const {
    const auto words = [](const std::vector<PpuTimeline::Line>& a, const std::vector<PpuTimeline::Line>& b) {
      return std::equal(a.begin(), a.end(), b.begin(), b.end(),
                        [](const PpuTimeline::Line& x, const PpuTimeline::Line& y) { return x.words == y.words; });
    };
    const auto pairs = [](const auto& a, const auto& b) {
      return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](const auto& x, const auto& y) {
        return x.at == y.at && x.change == y.change;
      });
    };
    return height == other.height && width == other.width && field == other.field &&
           firstLine == other.firstLine && words(lines, other.lines) && pairs(events, other.events) &&
           pairs(vramWrites, other.vramWrites) && vram == other.vram && palettes == other.palettes &&
           oams == other.oams && spriteBuffers == other.spriteBuffers;
  }
};

Kept keep(const PpuTimeline& timeline) {
  return Kept{.height = timeline.height,
              .width = timeline.width,
              .field = timeline.field,
              .firstLine = timeline.firstLine,
              .lines = {timeline.lines.begin(), timeline.lines.end()},
              .events = {timeline.events.begin(), timeline.events.end()},
              .vramWrites = {timeline.vramWrites.begin(), timeline.vramWrites.end()},
              .vram = {timeline.vram.begin(), timeline.vram.end()},
              .palettes = {timeline.palettes.begin(), timeline.palettes.end()},
              .oams = {timeline.oams.begin(), timeline.oams.end()},
              .spriteBuffers = {timeline.spriteBuffers.begin(), timeline.spriteBuffers.end()},
              .counter = 0,
              .master = 0};
}

// Every frame and every timeline handed over, in the order the reports came, with
// what each report found of the machine.
struct Recorder final : FrameObserver, PpuTimelineObserver {
  const Snes* machine = nullptr;
  std::vector<Frame> frames;
  std::vector<std::uint64_t> frameMasters;
  std::vector<Kept> timelines;
  std::vector<char> order;  // 'f' for a frame report, 't' for a timeline's
  int throwOnTimeline = 0;  // the timeline report that throws, counted from 1; 0 for none
  bool keepVram = false;    // keep VRAM as each timeline report finds it
  std::vector<Bytes> vramAtReport;

  void frame(const VideoFrame& picture) override {
    frames.push_back(Frame{.width = picture.width,
                           .height = picture.height,
                           .field = picture.field,
                           .pixels = Bytes(picture.pixels.begin(), picture.pixels.end())});
    frameMasters.push_back(machine->state().master);
    order.push_back('f');
  }
  void frame(const PpuTimeline& timeline) override {
    Kept kept = keep(timeline);
    kept.counter = machine->state().wram[kFrameCounter];
    kept.master = machine->state().master;
    timelines.push_back(std::move(kept));
    order.push_back('t');
    if (keepVram) vramAtReport.emplace_back(machine->state().ppu.vram.begin(), machine->state().ppu.vram.end());
    if (throwOnTimeline != 0 && static_cast<int>(timelines.size()) == throwOnTimeline) throw std::runtime_error("thrown");
  }
};

// A machine with both observers set on one recorder.
struct Watched {
  explicit Watched(const Bytes& rom, bool pictureThread = false, bool frames = true)
      : machine(config(rom, pictureThread)) {
    recorder.machine = &machine;
    if (frames) machine.setFrameObserver(&recorder);
    machine.setPpuTimelineObserver(&recorder);
  }
  Watched(const Watched&) = delete;
  Watched& operator=(const Watched&) = delete;
  Snes machine;
  Recorder recorder;
};

// The rows draw() makes of a timeline against a frame's pixels: each recorded row of
// a frame 512 wide column for column, of one 256 wide each pixel against both halves
// of its position. A pixel that differs is named.
::testing::AssertionResult drawsTheFrame(const PpuTimeline& timeline, const Frame& frame) {
  Bytes drawn(PpuTimeline::kDrawBytes, 0xA5u);
  timeline.draw(drawn);
  if (timeline.lines.empty()) return ::testing::AssertionFailure() << "no line recorded";
  for (unsigned row = timeline.firstLine - 1u; row < frame.height; ++row) {
    for (unsigned x = 0u; x < frame.width; ++x) {
      const std::size_t from = (static_cast<std::size_t>(row) * frame.width + x) * 4u;
      const std::size_t column = frame.width == 512u ? x : x * 2u;
      for (unsigned half = 0u; half < (frame.width == 512u ? 1u : 2u); ++half) {
        const std::size_t at = (static_cast<std::size_t>(row) * kHiresWidth + column + half) * 4u;
        if (!std::equal(drawn.begin() + static_cast<std::ptrdiff_t>(at),
                        drawn.begin() + static_cast<std::ptrdiff_t>(at + 4u),
                        frame.pixels.begin() + static_cast<std::ptrdiff_t>(from))) {
          return ::testing::AssertionFailure() << "line " << row + 1u << " position " << x << " half " << half
                                               << " differs";
        }
      }
    }
  }
  return ::testing::AssertionSuccess();
}

// Every report of `recorder` drawn: each timeline against the frame of the same
// report, and the frame's facts against the timeline's.
::testing::AssertionResult everyFrameDrawn(const Recorder& recorder, std::size_t from = 0u) {
  if (recorder.frames.size() != recorder.timelines.size()) {
    return ::testing::AssertionFailure() << recorder.frames.size() << " frames and " << recorder.timelines.size()
                                         << " timelines";
  }
  for (std::size_t i = from; i < recorder.frames.size(); ++i) {
    const Frame& frame = recorder.frames[i];
    const Kept& kept = recorder.timelines[i];
    if (kept.height != frame.height || kept.width != frame.width || kept.field != frame.field) {
      return ::testing::AssertionFailure() << "report " << i << ": the timeline is " << kept.width << "x"
                                           << kept.height << " field " << unsigned{kept.field} << ", the frame "
                                           << frame.width << "x" << frame.height << " field "
                                           << unsigned{frame.field};
    }
    const ::testing::AssertionResult drawn = drawsTheFrame(kept.view(), frame);
    if (!drawn) return ::testing::AssertionFailure() << "report " << i << ": " << drawn.message();
  }
  return ::testing::AssertionSuccess();
}

// Where writes to one register landed on picture lines, in master cycles into the
// line, with the register's values as each write left them.
struct WriteSites final : BusObserver {
  const Snes* machine = nullptr;
  std::uint16_t watched = 0x210Du;
  struct Site {
    std::uint16_t vpos = 0;
    std::uint16_t hpos = 0;
    std::uint16_t bg1hofs = 0;
    std::uint16_t m7hofs = 0;
    std::uint8_t counter = 0;
  };
  std::vector<Site> sites;
  void access(const BusAccess& a) override {
    if (!a.write || (a.address & 0xFFFFu) != watched) return;
    const SnesState& s = machine->state();
    if (s.vpos == 0u || s.inVblank) return;
    sites.push_back(Site{.vpos = s.vpos,
                         .hpos = s.hpos,
                         .bg1hofs = s.ppu.bg1hofs,
                         .m7hofs = s.ppu.m7hofs,
                         .counter = s.wram[kFrameCounter]});
  }
  void internal(std::uint32_t, std::optional<CycleKind>) override {}
};

// The events a timeline holds for one line, and for one field across the frame.
std::vector<PpuTimeline::Event> eventsOf(const Kept& kept, unsigned line) {
  std::vector<PpuTimeline::Event> out;
  for (const PpuTimeline::Event& event : kept.events) {
    if (event.line() == line) out.push_back(event);
  }
  return out;
}
unsigned countOf(const Kept& kept, PpuTimeline::Field field) {
  return static_cast<unsigned>(std::count_if(kept.events.begin(), kept.events.end(),
                                             [field](const PpuTimeline::Event& e) { return e.field() == field; }));
}

// The recorded line for picture line `line`.
const PpuTimeline::Line& lineOf(const Kept& kept, unsigned line) {
  for (const PpuTimeline::Line& recorded : kept.lines) {
    if (recorded.lineNumber() == line) return recorded;
  }
  throw std::out_of_range("the line was not recorded");
}

// The palette word `word` of a line's palette version.
std::uint16_t paletteWordOf(const Kept& kept, const PpuTimeline::Line& line, unsigned word) {
  const std::size_t at = line.paletteVersion() * PpuTimeline::kPaletteBytes + word * 2u;
  return static_cast<std::uint16_t>(kept.palettes[at] | (kept.palettes[at + 1u] << 8));
}

TEST(SnesPpuTimeline, TheTimelineDrawsTheFrameTheChipDrew) {
  Watched t(scene());
  for (int call = 0; call < 16; ++call) t.machine.run(kFrame);
  ASSERT_GE(t.recorder.timelines.size(), 15u);
  EXPECT_TRUE(everyFrameDrawn(t.recorder));
  // The scene's picture changes from frame to frame: color math, mosaic and the scroll.
  EXPECT_GT(t.recorder.timelines.back().events.size(), 0u);
  EXPECT_EQ(t.recorder.timelines.back().firstLine, 1u);
  EXPECT_EQ(t.recorder.timelines.back().lines.size(), 224u);
}

TEST(SnesPpuTimeline, TheFrameIsAsWideAsItsDotsDrawnInHalfPixels) {
  for (const bool pictureThread : {false, true}) {
    SCOPED_TRACE(::testing::Message() << (pictureThread ? "the picture thread" : "the calling thread"));
    {
      // $2133 bit 3 set part-way along the last picture line, by a write the line's
      // dots read, and cleared in vertical blank: every lit frame is 512 wide.
      Watched t(scene(Variant{.inBlank = clear(0x2133u),
                              .irqBody = store(0x2133u, 0x08u),
                              .htime = 100u,
                              .vtime = 224u,
                              .irq = true}),
                pictureThread);
      for (int call = 0; call < 6; ++call) t.machine.run(kFrame);
      const Kept& kept = t.recorder.timelines.back();
      EXPECT_EQ(kept.width, 512u);
      EXPECT_EQ(lineOf(kept, 224u).value(PpuTimeline::Field::Setini) & 0x08u, 0u) << "the line begins narrow";
      EXPECT_EQ(countOf(kept, PpuTimeline::Field::Setini), 1u);
      EXPECT_TRUE(everyFrameDrawn(t.recorder));
    }
    {
      // The same write in the last picture line's horizontal blank reaches no picture
      // dot: every frame is 256 wide.
      Watched t(scene(Variant{.inBlank = clear(0x2133u),
                              .irqBody = store(0x2133u, 0x08u),
                              .htime = 262u,
                              .vtime = 224u,
                              .irq = true}),
                pictureThread);
      for (int call = 0; call < 6; ++call) t.machine.run(kFrame);
      EXPECT_EQ(t.recorder.timelines.back().width, 256u);
      EXPECT_TRUE(everyFrameDrawn(t.recorder));
    }
  }
}

TEST(SnesPpuTimeline, TheTimelineIsTheSameOnEitherThread) {
  Watched lockstep(scene(), false);
  Watched threaded(scene(), true);
  for (int call = 0; call < 16; ++call) {
    lockstep.machine.run(kQuarterFrame);
    threaded.machine.run(kQuarterFrame);
    ASSERT_TRUE(lockstep.machine.state() == threaded.machine.state()) << "after run " << call;
  }
  for (int call = 0; call < 12; ++call) {
    lockstep.machine.run(kFrame);
    threaded.machine.run(kFrame);
  }
  ASSERT_EQ(lockstep.recorder.timelines.size(), threaded.recorder.timelines.size());
  ASSERT_GE(threaded.recorder.timelines.size(), 14u);
  for (std::size_t i = 0; i < threaded.recorder.timelines.size(); ++i) {
    EXPECT_TRUE(lockstep.recorder.timelines[i].sameAs(threaded.recorder.timelines[i])) << "report " << i;
  }
  EXPECT_TRUE(everyFrameDrawn(threaded.recorder));
}

TEST(SnesPpuTimeline, AWriteInHorizontalBlankIsTheNextLinesBaseState) {
  // The interrupt on line 100 writes $212C in the line's horizontal blank, every
  // frame, and the handler writes it back in vertical blank: line 101 begins with the
  // new value and line 100 with the old, and no event in the frame names $212C.
  for (const bool pictureThread : {false, true}) {
    SCOPED_TRACE(::testing::Message() << (pictureThread ? "the picture thread" : "the calling thread"));
    Watched t(scene(Variant{.inBlank = store(0x212Cu, 0x13u),
                            .irqBody = store(0x212Cu, 0x11u),
                            .htime = 262u,
                            .vtime = 100u,
                            .irq = true}),
              pictureThread);
    WriteSites sites;
    sites.machine = &t.machine;
    sites.watched = 0x212Cu;
    t.machine.setObserver(&sites);
    for (int call = 0; call < 6; ++call) t.machine.run(kFrame);
    ASSERT_FALSE(sites.sites.empty());
    EXPECT_EQ(sites.sites.back().vpos, 100u);
    EXPECT_GE(sites.sites.back().hpos / 4u + 1u, 278u) << "the write reaches past the last picture dot";
    const Kept& kept = t.recorder.timelines.back();
    EXPECT_EQ(lineOf(kept, 100u).value(PpuTimeline::Field::Tm), 0x13u);
    EXPECT_EQ(lineOf(kept, 101u).value(PpuTimeline::Field::Tm), 0x11u);
    EXPECT_EQ(lineOf(kept, 224u).value(PpuTimeline::Field::Tm), 0x11u);
    EXPECT_EQ(countOf(kept, PpuTimeline::Field::Tm), 0u);
    EXPECT_TRUE(everyFrameDrawn(t.recorder));
  }
}

TEST(SnesPpuTimeline, AWriteInsideTheLineIsAnEventAtTheDotItReaches) {
  // The scene's two writes to $210D in the picture: each is two events at the first
  // dot it reaches, BG1's offset and Mode 7's, holding the words the write assembled,
  // and the line's words 3 and 10 hold the words before the first of them.
  Watched t(scene());
  WriteSites sites;
  sites.machine = &t.machine;
  t.machine.setObserver(&sites);
  for (int call = 0; call < 8; ++call) t.machine.run(kFrame);
  const Kept& kept = t.recorder.timelines.back();
  std::vector<WriteSites::Site> frameSites;
  std::optional<WriteSites::Site> before;
  for (const WriteSites::Site& site : sites.sites) {
    if (site.counter == kept.counter) frameSites.push_back(site);
    if (site.counter + 1u == kept.counter) before = site;
  }
  ASSERT_EQ(frameSites.size(), 2u);
  ASSERT_TRUE(before.has_value());
  const unsigned line = frameSites[0].vpos;
  ASSERT_EQ(frameSites[1].vpos, line);
  const std::vector<PpuTimeline::Event> events = eventsOf(kept, line);
  ASSERT_EQ(events.size(), 4u);
  for (unsigned write = 0u; write < 2u; ++write) {
    SCOPED_TRACE(::testing::Message() << "write " << write);
    const unsigned dot = frameSites[write].hpos / 4u + 1u;
    ASSERT_GE(dot, kFirstTileReadDot);
    ASSERT_LE(dot, kLastPictureDot);
    const PpuTimeline::Event& bg = events[write * 2u];
    const PpuTimeline::Event& mode7 = events[write * 2u + 1u];
    EXPECT_EQ(bg.line(), line);
    EXPECT_EQ(bg.dot(), dot);
    EXPECT_EQ(bg.field(), PpuTimeline::Field::Bg1hofs);
    EXPECT_EQ(bg.value(), frameSites[write].bg1hofs);
    EXPECT_EQ(mode7.dot(), dot);
    EXPECT_EQ(mode7.field(), PpuTimeline::Field::M7hofs);
    EXPECT_EQ(mode7.value(), frameSites[write].m7hofs);
  }
  EXPECT_NE(frameSites[0].bg1hofs, before->bg1hofs) << "the first write changes the offset";
  EXPECT_EQ(lineOf(kept, line).value(PpuTimeline::Field::Bg1hofs), before->bg1hofs);
  EXPECT_EQ(lineOf(kept, line).value(PpuTimeline::Field::M7hofs), before->m7hofs);
  EXPECT_EQ(lineOf(kept, line + 1u).value(PpuTimeline::Field::Bg1hofs), frameSites[1].bg1hofs);
  EXPECT_EQ(lineOf(kept, line).firstEvent() + lineOf(kept, line).eventCount(),
            lineOf(kept, line + 1u).firstEvent());
  EXPECT_TRUE(everyFrameDrawn(t.recorder));
}

TEST(SnesPpuTimeline, AnInidispWriteInsideTheLineIsTwoEvents) {
  // The long store to $2100 in the picture: the bus byte $8F at the write cycle's last
  // dot, then $0F at the next, and the frame drawn from them is the frame drawn.
  for (const bool pictureThread : {false, true}) {
    SCOPED_TRACE(::testing::Message() << (pictureThread ? "the picture thread" : "the calling thread"));
    Watched t(earlyRedraw(), pictureThread);
    WriteSites sites;
    sites.machine = &t.machine;
    sites.watched = 0x2100u;
    t.machine.setObserver(&sites);
    for (int call = 0; call < 8; ++call) t.machine.run(kFrame);
    const Kept& kept = t.recorder.timelines.back();
    std::optional<WriteSites::Site> site;
    for (const WriteSites::Site& at : sites.sites) {
      if (at.counter == kept.counter) site = at;
    }
    ASSERT_TRUE(site.has_value());
    const unsigned dot = site->hpos / 4u;
    ASSERT_GE(dot, kFirstTileReadDot);
    ASSERT_LT(dot, kLastPictureDot);
    std::vector<PpuTimeline::Event> inidisp;
    for (const PpuTimeline::Event& event : eventsOf(kept, site->vpos)) {
      if (event.field() == PpuTimeline::Field::Inidisp) inidisp.push_back(event);
    }
    ASSERT_EQ(inidisp.size(), 2u);
    EXPECT_EQ(inidisp[0].dot(), dot);
    EXPECT_EQ(inidisp[0].value(), 0x8Fu);
    EXPECT_EQ(inidisp[1].dot(), dot + 1u);
    EXPECT_EQ(inidisp[1].value(), 0x0Fu);
    EXPECT_TRUE(everyFrameDrawn(t.recorder));
  }
}

TEST(SnesPpuTimeline, APaletteWriteBeforeTheLineVersionsThePalette) {
  // HDMA writes the backdrop in every line's horizontal blank: each line's version
  // holds the color the table gives the entry for the line before it, and a version
  // is made for every line. A table writing one color on every line makes one.
  {
    Watched t(scene());
    for (int call = 0; call < 6; ++call) t.machine.run(kFrame);
    const Kept& kept = t.recorder.timelines.back();
    for (unsigned line = 1u; line <= 224u; ++line) {
      SCOPED_TRACE(::testing::Message() << "line " << line);
      EXPECT_EQ(paletteWordOf(kept, lineOf(kept, line), 0u), hdmaColor(line - 1u));
    }
    EXPECT_EQ(kept.palettes.size(), 224u * PpuTimeline::kPaletteBytes);
    EXPECT_TRUE(everyFrameDrawn(t.recorder));
  }
  {
    Watched t(scene(Variant{.constantHdma = true}));
    for (int call = 0; call < 6; ++call) t.machine.run(kFrame);
    const Kept& kept = t.recorder.timelines.back();
    EXPECT_EQ(kept.palettes.size(), PpuTimeline::kPaletteBytes);
    EXPECT_EQ(paletteWordOf(kept, lineOf(kept, 120u), 0u), 0x1234u);
    EXPECT_TRUE(everyFrameDrawn(t.recorder));
  }
}

TEST(SnesPpuTimeline, APaletteWriteInsideThePictureIsAnEvent) {
  // On line 100 the interrupt sets forced blank, writes palette word 50 with the frame
  // count, and lets the screen go: one PaletteWord event at the dot the write's second
  // half reaches, the next line's version holding the word, and the frame drawn from
  // it the frame drawn.
  const Bytes body = joined({store(0x2100u, 0x80u), store(0x2121u, 50u), storeCounter(0x2122u),
                             clear(0x2122u), store(0x2100u, 0x0Fu)});
  for (const bool pictureThread : {false, true}) {
    SCOPED_TRACE(::testing::Message() << (pictureThread ? "the picture thread" : "the calling thread"));
    Watched t(scene(Variant{.irqBody = body, .htime = 90u, .vtime = 100u, .irq = true}), pictureThread);
    WriteSites sites;
    sites.machine = &t.machine;
    sites.watched = 0x2122u;
    t.machine.setObserver(&sites);
    for (int call = 0; call < 8; ++call) t.machine.run(kFrame);
    const Kept& kept = t.recorder.timelines.back();
    // The interrupt's two writes, apart from HDMA's in the line's horizontal blank.
    std::vector<WriteSites::Site> frameSites;
    for (const WriteSites::Site& site : sites.sites) {
      if (site.counter == kept.counter && site.vpos == 100u && site.hpos / 4u + 1u <= kLastPictureDot) {
        frameSites.push_back(site);
      }
    }
    ASSERT_EQ(frameSites.size(), 2u);
    ASSERT_EQ(countOf(kept, PpuTimeline::Field::PaletteWord), 1u);
    const auto event = std::find_if(kept.events.begin(), kept.events.end(), [](const PpuTimeline::Event& e) {
      return e.field() == PpuTimeline::Field::PaletteWord;
    });
    EXPECT_EQ(event->line(), 100u);
    EXPECT_EQ(event->dot(), frameSites[1].hpos / 4u + 1u);
    EXPECT_EQ(event->paletteIndex(), 50u);
    EXPECT_EQ(event->value(), kept.counter);
    EXPECT_EQ(paletteWordOf(kept, lineOf(kept, 101u), 50u), kept.counter);
    EXPECT_NE(paletteWordOf(kept, lineOf(kept, 100u), 50u), kept.counter);
    EXPECT_TRUE(everyFrameDrawn(t.recorder));
  }
}

// The sprites Range has kept for the next line, read where Range's pass over the line
// before it is done and Time has not run: in that line's horizontal blank.
struct RangeKept {
  std::array<std::uint8_t, kSpritesPerLine> inRange{};
  std::uint8_t found = 0;
};
RangeKept rangeFor(Snes& machine, unsigned line) {
  while (!(machine.state().vpos == line - 1u && machine.state().hpos >= 1200u)) machine.run(4u);
  return RangeKept{.inRange = machine.state().ppu.sprites.inRange, .found = machine.state().ppu.sprites.found};
}

TEST(SnesPpuTimeline, TheSpriteInputsAreTimesOwn) {
  {
    // The interrupt writes $2101 inside line 100 and the handler writes it back in
    // vertical blank: line 100's Time read the old value, line 101's the new.
    Watched t(scene(Variant{.inBlank = store(0x2101u, 0x01u),
                            .irqBody = store(0x2101u, 0x02u),
                            .htime = 90u,
                            .vtime = 100u,
                            .irq = true}));
    for (int call = 0; call < 6; ++call) t.machine.run(kFrame);
    const Kept& kept = t.recorder.timelines.back();
    EXPECT_EQ(lineOf(kept, 100u).spriteObjsel(), 0x01u);
    EXPECT_EQ(lineOf(kept, 101u).spriteObjsel(), 0x02u);
    EXPECT_EQ(lineOf(kept, 100u).spriteSource(), PpuTimeline::SpriteSource::Gathered);
    EXPECT_TRUE(everyFrameDrawn(t.recorder));
  }
  {
    // A line's kept sprites are the ones Range kept across the line before, on both
    // threads.
    for (const bool pictureThread : {false, true}) {
      SCOPED_TRACE(::testing::Message() << (pictureThread ? "the picture thread" : "the calling thread"));
      Watched t(scene(), pictureThread);
      for (int call = 0; call < 5; ++call) t.machine.run(kFrame);
      const RangeKept kept = rangeFor(t.machine, 71u);
      t.machine.run(kFrame);
      const PpuTimeline::Line& line = lineOf(t.recorder.timelines.back(), 71u);
      ASSERT_GE(kept.found, 1u) << "a sprite crosses line 71";
      EXPECT_EQ(line.spriteFound(), kept.found);
      for (unsigned nth = 0u; nth < kSpritesPerLine; ++nth) EXPECT_EQ(line.inRange(nth), kept.inRange[nth]);
      EXPECT_TRUE(everyFrameDrawn(t.recorder));
    }
  }
  {
    // The sprite table rewritten under forced blank on line 100 is a second version
    // in that frame; the scene without it has one.
    const Bytes body = joined({store(0x2100u, 0x80u), store(0x2102u, 14u), clear(0x2103u),
                               storeCounter(0x2104u), store(0x2104u, 40u), store(0x2100u, 0x0Fu)});
    Watched t(scene(Variant{.irqBody = body, .htime = 90u, .vtime = 100u, .irq = true}));
    for (int call = 0; call < 6; ++call) t.machine.run(kFrame);
    const Kept& kept = t.recorder.timelines.back();
    EXPECT_EQ(kept.oams.size(), 2u * PpuTimeline::kOamBytes);
    EXPECT_EQ(lineOf(kept, 100u).oamVersion(), 0u);
    EXPECT_EQ(lineOf(kept, 101u).oamVersion(), 1u);
    EXPECT_TRUE(everyFrameDrawn(t.recorder));
    Watched plain(scene());
    for (int call = 0; call < 6; ++call) plain.machine.run(kFrame);
    EXPECT_EQ(plain.recorder.timelines.back().oams.size(), PpuTimeline::kOamBytes);
  }
}

TEST(SnesPpuTimeline, ALineWhoseTimeGatheredNothingShowsNoSprite) {
  // The interrupt sets forced blank in line 75's horizontal blank and lets the screen
  // go part-way along line 76: Time gathers nothing as line 76 begins, the buffer it
  // leaves is line 75's, and the line's lit dots show no sprite where sprite 2 stands;
  // line 77's Time gathers again.
  const Bytes body = joined({{0xDAu},                                    // PHX
                             store(0x2100u, 0x80u),
                             {0xA2u, 0x06u, 0xCAu, 0xD0u, 0xFDu},         // LDX #$06 ; DEX ; BNE
                             store(0x2100u, 0x0Fu),
                             {0xFAu}});                                   // PLX
  for (const bool pictureThread : {false, true}) {
    SCOPED_TRACE(::testing::Message() << (pictureThread ? "the picture thread" : "the calling thread"));
    Watched t(scene(Variant{.irqBody = body, .htime = 262u, .vtime = 75u, .irq = true}), pictureThread);
    WriteSites sites;
    sites.machine = &t.machine;
    sites.watched = 0x2100u;
    t.machine.setObserver(&sites);
    for (int call = 0; call < 6; ++call) t.machine.run(kFrame);
    const Kept& kept = t.recorder.timelines.back();
    std::vector<WriteSites::Site> frameSites;
    for (const WriteSites::Site& site : sites.sites) {
      if (site.counter == kept.counter) frameSites.push_back(site);
    }
    ASSERT_EQ(frameSites.size(), 2u);
    EXPECT_EQ(frameSites[0].vpos, 75u);
    EXPECT_GE(frameSites[0].hpos / 4u + 1u, 278u) << "forced blank before line 76 begins: " << frameSites[0].hpos;
    EXPECT_EQ(frameSites[1].vpos, 76u);
    EXPECT_LT(frameSites[1].hpos / 4u, kFirstPictureDot + 76u)
        << "the screen lit before sprite 2's first position: " << frameSites[1].hpos;
    EXPECT_EQ(lineOf(kept, 76u).spriteSource(), PpuTimeline::SpriteSource::None);
    EXPECT_EQ(lineOf(kept, 76u).value(PpuTimeline::Field::Inidisp), 0x80u);
    EXPECT_EQ(lineOf(kept, 77u).spriteSource(), PpuTimeline::SpriteSource::Gathered);
    EXPECT_TRUE(everyFrameDrawn(t.recorder));
  }
}

TEST(SnesPpuTimeline, ALineRestoredBeforeItsFirstReadDotCarriesItsSprites) {
  // A state taken as line 80 begins, after Time has gathered its sprites and before
  // its first read dot, restored into a watched machine: the line is recorded, and the
  // buffer it is drawn with, which no Time of the recording gathered, is carried whole.
  Snes source(config(scene()));
  for (int call = 0; call < 5; ++call) source.run(kFrame);
  while (source.state().vpos != 80u) source.run(2u);
  ASSERT_LE(source.state().hpos, 8u) << "before the line's first read dot";
  ASSERT_EQ(source.state().ppu.sprites.line, 80u);
  const SnesState state = source.state();
  Watched t(scene());
  t.machine.run(kFrame);
  t.recorder.frames.clear();
  t.recorder.timelines.clear();
  ASSERT_TRUE(t.machine.restore(state));
  t.machine.run(kFrame * 3u / 4u);
  ASSERT_EQ(t.recorder.timelines.size(), 1u);
  const Kept& kept = t.recorder.timelines.front();
  EXPECT_EQ(kept.firstLine, 80u);
  const PpuTimeline::Line& line = lineOf(kept, 80u);
  EXPECT_EQ(line.spriteSource(), PpuTimeline::SpriteSource::Carried);
  EXPECT_EQ(line.spriteBuffer(), 0u);
  ASSERT_EQ(kept.spriteBuffers.size(), PpuTimeline::kSpriteBufferBytes);
  EXPECT_TRUE(std::equal(state.ppu.sprites.word.begin(), state.ppu.sprites.word.end(), kept.spriteBuffers.begin()));
  EXPECT_EQ(lineOf(kept, 81u).spriteSource(), PpuTimeline::SpriteSource::Gathered);
  EXPECT_TRUE(drawsTheFrame(kept.view(), t.recorder.frames.front()));
}

TEST(SnesPpuTimeline, AVramWriteInsideTheFrameIsLoggedAtItsPosition) {
  for (const bool pictureThread : {false, true}) {
    SCOPED_TRACE(::testing::Message() << (pictureThread ? "the picture thread" : "the calling thread"));
    // Forced blank on line 100 and 64 bytes into the solid character there: every
    // byte with its position, VRAM before them as the report before left it, and the
    // frame drawn from them the frame drawn.
    const Bytes body = joined({store(0x2100u, 0x80u), vramTransfer(), store(0x2100u, 0x0Fu)});
    Watched t(scene(Variant{.irqBody = body, .htime = 60u, .vtime = 100u, .irq = true}), pictureThread);
    t.recorder.keepVram = true;
    for (int call = 0; call < 6; ++call) t.machine.run(kFrame);
    const std::vector<Bytes>& vramAtReport = t.recorder.vramAtReport;
    const Bytes source = vramSource();
    const std::size_t last = t.recorder.timelines.size() - 1u;
    for (std::size_t report = last - 2u; report <= last; ++report) {
      SCOPED_TRACE(::testing::Message() << "report " << report);
      const Kept& kept = t.recorder.timelines[report];
      ASSERT_EQ(kept.vramWrites.size(), 64u);
      for (unsigned byte = 0u; byte < 64u; ++byte) {
        const PpuTimeline::VramWrite& write = kept.vramWrites[byte];
        EXPECT_EQ(write.address(), kSolidCharacterWord * 2u + byte);
        EXPECT_EQ(write.value(), source[byte]);
        EXPECT_GE(write.line(), 100u);
        EXPECT_LE(write.line(), 101u);
        if (byte > 0u) {
          const PpuTimeline::VramWrite& previous = kept.vramWrites[byte - 1u];
          EXPECT_TRUE(write.line() > previous.line() ||
                      (write.line() == previous.line() && write.dot() > previous.dot()));
        }
      }
    }
    EXPECT_TRUE(everyFrameDrawn(t.recorder));
    // Each lit frame finds VRAM as the report before left it: nothing writes it
    // between a report and the frame's transfer.
    for (std::size_t report = last - 2u; report <= last; ++report) {
      SCOPED_TRACE(::testing::Message() << "report " << report);
      EXPECT_TRUE(t.recorder.timelines[report].vram == vramAtReport[report - 1u]);
    }
  }
  {
    // The same transfer in vertical blank: no write is the frame's, and the base
    // holds the bytes.
    Watched t(scene(Variant{.inBlank = vramTransfer()}));
    for (int call = 0; call < 6; ++call) t.machine.run(kFrame);
    const Kept& kept = t.recorder.timelines.back();
    EXPECT_TRUE(kept.vramWrites.empty());
    const Bytes source = vramSource();
    EXPECT_TRUE(std::equal(source.begin(), source.end(),
                           kept.vram.begin() + static_cast<std::ptrdiff_t>(kSolidCharacterWord * 2u)));
    EXPECT_TRUE(everyFrameDrawn(t.recorder));
  }
}

TEST(SnesPpuTimeline, TheReportIsMadeWithTheFrameReport) {
  {
    // Both reports in one cycle, the timeline's after the frame's, at the cycle the
    // blank begins.
    Watched t(scene());
    struct Blank final : PpuTimelineObserver {
      const Snes* machine = nullptr;
      std::vector<std::pair<std::uint16_t, std::uint16_t>> lines;  // vpos and vblankBeginLine at each report
      Recorder* recorder = nullptr;
      void frame(const PpuTimeline& timeline) override {
        lines.emplace_back(machine->state().vpos, machine->state().vblankBeginLine);
        recorder->frame(timeline);
      }
    } blank;
    blank.machine = &t.machine;
    blank.recorder = &t.recorder;
    t.machine.setPpuTimelineObserver(&blank);
    for (int call = 0; call < 6; ++call) t.machine.run(kFrame);
    ASSERT_GE(t.recorder.timelines.size(), 5u);
    ASSERT_EQ(t.recorder.frames.size(), t.recorder.timelines.size());
    for (std::size_t i = 0; i < t.recorder.order.size(); ++i) {
      EXPECT_EQ(t.recorder.order[i], i % 2u == 0u ? 'f' : 't') << "report " << i;
    }
    for (std::size_t i = 0; i < t.recorder.timelines.size(); ++i) {
      EXPECT_EQ(t.recorder.timelines[i].master, t.recorder.frameMasters[i]) << "report " << i;
      EXPECT_EQ(blank.lines[i].first, blank.lines[i].second);
      EXPECT_EQ(blank.lines[i].first, 225u);
    }
  }
  {
    // With no frame observer the timeline's comes alone, at the same cycles.
    Watched both(scene());
    Watched alone(scene(), false, /*frames=*/false);
    for (int call = 0; call < 6; ++call) {
      both.machine.run(kFrame);
      alone.machine.run(kFrame);
    }
    ASSERT_EQ(alone.recorder.timelines.size(), both.recorder.timelines.size());
    EXPECT_TRUE(alone.recorder.frames.empty());
    for (std::size_t i = 0; i < alone.recorder.timelines.size(); ++i) {
      EXPECT_EQ(alone.recorder.timelines[i].master, both.recorder.timelines[i].master);
      EXPECT_TRUE(alone.recorder.timelines[i].sameAs(both.recorder.timelines[i])) << "report " << i;
    }
  }
  {
    // A report that throws leaves the machine at the cycle's end, and the next run()
    // finishes the budget the throw interrupted.
    Watched thrown(scene(), false, /*frames=*/false);
    Watched straight(scene(), false, /*frames=*/false);
    thrown.recorder.throwOnTimeline = 2;
    EXPECT_THROW(thrown.machine.run(3u * kFrame), std::runtime_error);
    EXPECT_EQ(thrown.machine.state().vpos, thrown.machine.state().vblankBeginLine);
    thrown.machine.run(kFrame);
    straight.machine.run(4u * kFrame);
    EXPECT_TRUE(thrown.machine.state() == straight.machine.state());
  }
}

TEST(SnesPpuTimeline, ARestoreMidPictureRecordsFromTheBeam) {
  // A state taken at line 100, dot 40, restored into a watched machine: the frame's
  // timeline begins at line 101, and draw() leaves the rows above it as they were.
  Snes source(config(scene()));
  for (int call = 0; call < 5; ++call) source.run(kFrame);
  while (!(source.state().vpos == 100u && source.state().hpos >= 160u)) source.run(4u);
  ASSERT_EQ(source.state().vpos, 100u);
  const SnesState state = source.state();
  // The watched machine is restored part-way down a frame it has recorded lines of.
  Watched t(scene());
  t.machine.run(kFrame + kFrame / 2u);
  t.recorder.frames.clear();
  t.recorder.timelines.clear();
  ASSERT_TRUE(t.machine.restore(state));
  t.machine.run(kFrame / 2u);
  ASSERT_EQ(t.recorder.timelines.size(), 1u);
  const Kept& kept = t.recorder.timelines.front();
  EXPECT_EQ(kept.firstLine, 101u);
  EXPECT_EQ(kept.lines.size(), kept.height - 100u);
  Bytes drawn(PpuTimeline::kDrawBytes, 0xA5u);
  kept.view().draw(drawn);
  const std::size_t row = std::size_t{kHiresWidth} * 4u;
  EXPECT_TRUE(std::all_of(drawn.begin(), drawn.begin() + static_cast<std::ptrdiff_t>(100u * row),
                          [](std::uint8_t byte) { return byte == 0xA5u; }));
  EXPECT_FALSE(std::all_of(drawn.begin() + static_cast<std::ptrdiff_t>(100u * row),
                           drawn.begin() + static_cast<std::ptrdiff_t>(101u * row),
                           [](std::uint8_t byte) { return byte == 0xA5u; }));
  EXPECT_TRUE(drawsTheFrame(kept.view(), t.recorder.frames.front()));
}

TEST(SnesPpuTimeline, AnEventOnALineNotRecordedIsDropped) {
  // The interrupt writes $212C inside line 100 and again inside line 101. A state
  // taken at line 100, dot 40, before the first write, restored into a watched
  // machine: line 100 is not recorded and its event is dropped, and line 101's event
  // stands with the line.
  const Bytes body = joined({{0xDAu},                                     // PHX
                             store(0x212Cu, 0x11u),
                             {0xA2u, 0x28u, 0xCAu, 0xD0u, 0xFDu},         // LDX #$28 ; DEX ; BNE
                             store(0x212Cu, 0x13u),
                             {0xFAu}});                                   // PLX
  const Bytes rom = scene(Variant{.irqBody = body, .htime = 100u, .vtime = 100u, .irq = true});
  Snes source(config(rom));
  for (int call = 0; call < 5; ++call) source.run(kFrame);
  while (!(source.state().vpos == 100u && source.state().hpos >= 160u)) source.run(4u);
  ASSERT_EQ(source.state().vpos, 100u);
  const SnesState state = source.state();
  Watched t(rom);
  WriteSites sites;
  sites.machine = &t.machine;
  sites.watched = 0x212Cu;
  t.machine.setObserver(&sites);
  t.machine.run(kFrame + kFrame / 2u);
  t.recorder.frames.clear();
  t.recorder.timelines.clear();
  sites.sites.clear();
  ASSERT_TRUE(t.machine.restore(state));
  t.machine.run(kFrame / 2u);
  ASSERT_EQ(t.recorder.timelines.size(), 1u);
  ASSERT_EQ(sites.sites.size(), 2u);
  EXPECT_EQ(sites.sites[0].vpos, 100u);
  EXPECT_GT(sites.sites[0].hpos, 160u);
  EXPECT_EQ(sites.sites[1].vpos, 101u);
  EXPECT_LE(sites.sites[1].hpos / 4u + 1u, kLastPictureDot);
  const Kept& kept = t.recorder.timelines.front();
  EXPECT_EQ(kept.firstLine, 101u);
  ASSERT_FALSE(kept.events.empty());
  for (const PpuTimeline::Event& event : kept.events) EXPECT_GE(event.line(), 101u);
  EXPECT_EQ(countOf(kept, PpuTimeline::Field::Tm), 1u);
  const std::vector<PpuTimeline::Event> line101 = eventsOf(kept, 101u);
  ASSERT_EQ(line101.size(), 1u);
  EXPECT_EQ(lineOf(kept, 101u).firstEvent(), 0u);
  EXPECT_EQ(lineOf(kept, 101u).eventCount(), 1u);
  EXPECT_TRUE(drawsTheFrame(kept.view(), t.recorder.frames.front()));
}

TEST(SnesPpuTimeline, AVramWriteInAHorizontalBlankReachesTheNextLinesSprites) {
  // The handler points the VRAM port at the sprites' solid character in vertical
  // blank. In line 130's horizontal blank the interrupt sets forced blank, writes the
  // first row's two low planes of the character with the frame count, and lets the
  // screen go before line 131 begins: Time gathers sprite 5, which stands from line 131
  // on that character, from the row as written.
  const Bytes blank = joined({store(0x2116u, 0x10u), store(0x2117u, 0x20u)});
  const Bytes body = joined({store(0x2100u, 0x80u), storeCounter(0x2118u), store(0x2119u, 0x55u),
                             store(0x2100u, 0x0Fu)});
  for (const bool pictureThread : {false, true}) {
    SCOPED_TRACE(::testing::Message() << (pictureThread ? "the picture thread" : "the calling thread"));
    Watched t(scene(Variant{.inBlank = blank, .irqBody = body, .htime = 243u, .vtime = 130u, .irq = true}),
              pictureThread);
    WriteSites sites;
    sites.machine = &t.machine;
    sites.watched = 0x2100u;
    t.machine.setObserver(&sites);
    for (int call = 0; call < 6; ++call) t.machine.run(kFrame);
    const Kept& kept = t.recorder.timelines.back();
    std::vector<WriteSites::Site> frameSites;
    for (const WriteSites::Site& site : sites.sites) {
      if (site.counter == kept.counter) frameSites.push_back(site);
    }
    ASSERT_EQ(frameSites.size(), 2u);
    EXPECT_EQ(frameSites[0].vpos, 130u);
    EXPECT_GE(frameSites[0].hpos / 4u + 1u, 278u) << frameSites[0].hpos;
    EXPECT_EQ(frameSites[1].vpos, 130u) << "the screen lit before line 131 begins: " << frameSites[0].hpos
                                        << " then " << frameSites[1].hpos;
    ASSERT_EQ(kept.vramWrites.size(), 2u);
    EXPECT_EQ(kept.vramWrites[0].line(), 130u);
    EXPECT_GT(kept.vramWrites[0].dot(), kLastPictureDot) << "past the line's last picture dot";
    EXPECT_EQ(kept.vramWrites[1].line(), 130u);
    EXPECT_EQ(kept.vramWrites[0].address(), 0x4020u);
    EXPECT_EQ(kept.vramWrites[0].value(), kept.counter);
    EXPECT_EQ(lineOf(kept, 131u).spriteSource(), PpuTimeline::SpriteSource::Gathered);
    EXPECT_TRUE(everyFrameDrawn(t.recorder));
  }
}

TEST(SnesPpuTimeline, RecordingChangesNothingTheMachineDoes) {
  for (const bool pictureThread : {false, true}) {
    SCOPED_TRACE(::testing::Message() << (pictureThread ? "the picture thread" : "the calling thread"));
    Watched recording(scene(), pictureThread);
    Snes plain(config(scene(), pictureThread));
    Recorder plainFrames;
    plainFrames.machine = &plain;
    plain.setFrameObserver(&plainFrames);
    for (int call = 0; call < 64; ++call) {
      recording.machine.run(kQuarterFrame);
      plain.run(kQuarterFrame);
      ASSERT_TRUE(recording.machine.state() == plain.state()) << "after run " << call;
      ASSERT_TRUE(recording.machine.takeFrames() == plain.takeFrames()) << "after run " << call;
      ASSERT_TRUE(recording.machine.lineReuse() == plain.lineReuse()) << "after run " << call;
    }
    ASSERT_GE(plainFrames.frames.size(), 15u);
    EXPECT_TRUE(recording.recorder.frames == plainFrames.frames);
  }
}

// FNV-1a over a 32-bit value's four bytes from the low one up, so every platform
// folds the same bytes.
void fold(std::uint64_t& digest, std::uint32_t value) {
  for (unsigned byte = 0u; byte < 4u; ++byte) {
    digest = (digest ^ ((value >> (8u * byte)) & 0xFFu)) * 1099511628211ull;
  }
}

TEST(SnesPpuTimeline, TheLayoutIsFixed) {
  EXPECT_EQ(sizeof(PpuTimeline::Line), 128u);
  EXPECT_EQ(sizeof(PpuTimeline::Event), 8u);
  EXPECT_EQ(sizeof(PpuTimeline::VramWrite), 8u);

  // A line built word by word and read back field by field.
  PpuTimeline::Line line{};
  line.words[0] = 0x0F | 0x01u << 8 | 0x32u << 16 | 0x04u << 24;
  line.words[3] = 0x0123u | 0x0456u << 16;
  line.words[10] = 0x1FFFu | 0x0800u << 16;
  line.words[14] = 0x15u | 0x3Fu << 8 | 0x1Fu << 16 | 0x0Au << 24;
  line.words[16] = 1u | 0x02u << 8 | 0x02u << 16 | 3u << 24;
  line.words[17] = 0x41u | 7u << 16;
  line.words[18] = 2u;
  line.words[19] = 5u;
  line.words[20] = 4u;
  line.words[21] = 99u;
  line.words[22] = 2u;
  line.words[23] = 6u;
  line.words[24] = 0x11u | 0x22u << 8 | 0x33u << 16;
  line.words[31] = 0x7Fu << 24;
  EXPECT_EQ(line.value(PpuTimeline::Field::Inidisp), 0x0Fu);
  EXPECT_EQ(line.value(PpuTimeline::Field::Bgmode), 0x01u);
  EXPECT_EQ(line.value(PpuTimeline::Field::Mosaic), 0x32u);
  EXPECT_EQ(line.value(PpuTimeline::Field::Setini), 0x04u);
  EXPECT_EQ(line.value(PpuTimeline::Field::Bg1hofs), 0x0123u);
  EXPECT_EQ(line.value(PpuTimeline::Field::Bg1vofs), 0x0456u);
  EXPECT_EQ(line.value(PpuTimeline::Field::M7hofs), 0x1FFFu);
  EXPECT_EQ(line.value(PpuTimeline::Field::M7vofs), 0x0800u);
  EXPECT_EQ(line.value(PpuTimeline::Field::Tsw), 0x15u);
  EXPECT_EQ(line.value(PpuTimeline::Field::Cgadsub), 0x3Fu);
  EXPECT_EQ(line.value(PpuTimeline::Field::FixedRed), 0x1Fu);
  EXPECT_EQ(line.value(PpuTimeline::Field::FixedGreen), 0x0Au);
  EXPECT_EQ(line.value(PpuTimeline::Field::PaletteWord), 0u);
  EXPECT_EQ(line.field(), 1u);
  EXPECT_EQ(line.spriteObjsel(), 0x02u);
  EXPECT_EQ(line.spriteSetini(), 0x02u);
  EXPECT_EQ(line.spriteFound(), 3u);
  EXPECT_EQ(line.spriteFirst(), 0x41u);
  EXPECT_EQ(line.paletteVersion(), 7u);
  EXPECT_EQ(line.oamVersion(), 2u);
  EXPECT_EQ(line.firstEvent(), 5u);
  EXPECT_EQ(line.eventCount(), 4u);
  EXPECT_EQ(line.lineNumber(), 99u);
  EXPECT_EQ(line.spriteSource(), PpuTimeline::SpriteSource::Carried);
  EXPECT_EQ(line.spriteBuffer(), 6u);
  EXPECT_EQ(line.inRange(0u), 0x11u);
  EXPECT_EQ(line.inRange(2u), 0x33u);
  EXPECT_EQ(line.inRange(3u), 0u);
  EXPECT_EQ(line.inRange(31u), 0x7Fu);
  const PpuTimeline::Event event{.at = 100u | 37u << 16, .change = 42u | 9u << 8 | 0x7C1Fu << 16};
  EXPECT_EQ(event.line(), 100u);
  EXPECT_EQ(event.dot(), 37u);
  EXPECT_EQ(event.field(), PpuTimeline::Field::FixedRed);
  EXPECT_EQ(event.paletteIndex(), 9u);
  EXPECT_EQ(event.value(), 0x7C1Fu);
  const PpuTimeline::VramWrite write{.at = 12u | 300u << 16, .change = 0xBEEFu | 0xA5u << 16};
  EXPECT_EQ(write.line(), 12u);
  EXPECT_EQ(write.dot(), 300u);
  EXPECT_EQ(write.address(), 0xBEEFu);
  EXPECT_EQ(write.value(), 0xA5u);

  // A filled frame's buffers, pinned.
  Watched t(scene());
  for (int call = 0; call < 10; ++call) t.machine.run(kFrame);
  const Kept& kept = t.recorder.timelines.back();
  std::uint64_t digest = 1469598103934665603ull;
  fold(digest, kept.height);
  fold(digest, kept.width);
  fold(digest, kept.field);
  fold(digest, kept.firstLine);
  for (const PpuTimeline::Line& recorded : kept.lines) {
    for (const std::uint32_t word : recorded.words) fold(digest, word);
  }
  for (const PpuTimeline::Event& e : kept.events) {
    fold(digest, e.at);
    fold(digest, e.change);
  }
  for (const PpuTimeline::VramWrite& w : kept.vramWrites) {
    fold(digest, w.at);
    fold(digest, w.change);
  }
  for (const Bytes* span : {&kept.vram, &kept.palettes, &kept.oams, &kept.spriteBuffers}) {
    fold(digest, static_cast<std::uint32_t>(span->size()));
    for (const std::uint8_t byte : *span) digest = (digest ^ byte) * 1099511628211ull;
  }
  EXPECT_EQ(digest, 0x5B5DBFE7ECD22BDEull);
}

// Where the burst's writes reached, on picture lines, counted between reports, and
// whether anything but $210D was written to the chip inside a picture line's dots
// then.
struct ScrollWrites final : BusObserver, PpuTimelineObserver {
  const Snes* machine = nullptr;
  Recorder* recorder = nullptr;
  unsigned reaching = 0;  // writes to $210D reaching dots 3 to 277 of a picture line since the last report
  bool other = false;
  std::vector<unsigned> perFrame;
  std::vector<bool> otherPerFrame;
  void access(const BusAccess& a) override {
    const std::uint32_t offset = a.address & 0xFFFFu;
    if (!a.write || offset < 0x2100u || offset > 0x213Fu) return;
    const SnesState& s = machine->state();
    if (s.vpos == 0u || s.inVblank) return;
    const unsigned dot = s.hpos / 4u + 1u;
    if (dot + 1u < kFirstTileReadDot || dot > kLastPictureDot + 1u) return;
    if (offset != 0x210Du) {
      other = true;
    } else if (dot >= kFirstTileReadDot && dot <= kLastPictureDot) {
      ++reaching;
    }
  }
  void internal(std::uint32_t, std::optional<CycleKind>) override {}
  void frame(const PpuTimeline& timeline) override {
    perFrame.push_back(reaching);
    otherPerFrame.push_back(other);
    reaching = 0;
    other = false;
    recorder->frame(timeline);
  }
};

TEST(SnesPpuTimeline, AFrameOfManyEventsLosesNone) {
  for (const bool pictureThread : {false, true}) {
    SCOPED_TRACE(::testing::Message() << (pictureThread ? "the picture thread" : "the calling thread"));
    Watched t(burst(), pictureThread);
    ScrollWrites writes;
    writes.machine = &t.machine;
    writes.recorder = &t.recorder;
    t.machine.setObserver(&writes);
    t.machine.setPpuTimelineObserver(&writes);
    t.machine.run(12u * kFrame);
    ASSERT_EQ(writes.perFrame.size(), t.recorder.timelines.size());
    unsigned burstFrames = 0;
    for (std::size_t i = 0; i < writes.perFrame.size(); ++i) {
      if (writes.otherPerFrame[i]) continue;  // the setup's writes make events of their own
      EXPECT_EQ(t.recorder.timelines[i].events.size(), 2u * writes.perFrame[i]) << "report " << i;
      if (writes.perFrame[i] >= 20000u) ++burstFrames;
    }
    EXPECT_GE(burstFrames, 3u) << "frames of the burst";
    EXPECT_TRUE(everyFrameDrawn(t.recorder));
  }
}

TEST(SnesPpuTimeline, ALineTheFrameBeforeStandsInForIsRecordedWhole) {
  // A still scene: every line is taken from the frame before, and every line is
  // recorded all the same, as it was the frame before.
  for (const bool pictureThread : {false, true}) {
    SCOPED_TRACE(::testing::Message() << (pictureThread ? "the picture thread" : "the calling thread"));
    Watched t(scene(Variant{.still = true}), pictureThread);
    struct Counts final : PpuTimelineObserver {
      Snes* machine = nullptr;
      Recorder* recorder = nullptr;
      std::vector<Snes::LineReuse> counts;
      void frame(const PpuTimeline& timeline) override {
        counts.push_back(machine->lineReuse());
        recorder->frame(timeline);
      }
    } counts;
    counts.machine = &t.machine;
    counts.recorder = &t.recorder;
    t.machine.setPpuTimelineObserver(&counts);
    for (int call = 0; call < 6; ++call) t.machine.run(kFrame);
    ASSERT_GE(t.recorder.timelines.size(), 5u);
    // The parity turns every frame and word 16 carries it, so the frame two before is
    // the one this frame's lines are equal to.
    const std::size_t last = t.recorder.timelines.size() - 1u;
    EXPECT_EQ(counts.counts[last].reused, t.recorder.timelines[last].height);
    EXPECT_EQ(t.recorder.timelines[last].lines.size(), t.recorder.timelines[last].height);
    EXPECT_TRUE(t.recorder.timelines[last].sameAs(t.recorder.timelines[last - 2u]));
    EXPECT_TRUE(t.recorder.timelines[last].events.empty());
    EXPECT_TRUE(everyFrameDrawn(t.recorder, 1u));
  }
}

}  // namespace
}  // namespace snaggletooth
