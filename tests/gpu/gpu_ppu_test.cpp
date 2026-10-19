// The GPU PPU: a machine given a device (SnesConfig::gpuPpu) has every finished
// frame drawn on the GPU by the core's compute shader from the frame's timeline, and
// the texture it leaves is the frame the dot renderer drew. The oracle in every case
// is twofold: PpuTimeline::draw() over the same timeline, row for row, and the
// machine's own frame report of the same cycle. The device is the player's, over
// SDL's GPU API; a machine with no device skips every case, visibly.
//
// The scenes are cartridges built here: a program that loads VRAM, the palette and
// the sprite table under forced blank, writes the registers a case names, turns the
// screen on, and runs a handler on every vertical blank with a frame counter, with
// an interrupt at a named dot of a named line for writes that must land inside a
// line. Each case names the mode, the memories and the writes it needs.

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

#include "SDL3/SDL.h"
#include "gtest/gtest.h"
#include "player/interlace.h"
#include "player/sdl_gpu_device.h"
#include "snaggletooth/snes/ppu_timeline.h"
#include "snaggletooth/snes/snes.h"

namespace snaggletooth {
namespace {

constexpr std::uint64_t kFrame = 357366u;
constexpr std::uint8_t kFrameCounter = 0x10u;

using Bytes = std::vector<std::uint8_t>;

Bytes joined(std::initializer_list<Bytes> parts) {
  Bytes out;
  for (const Bytes& part : parts) out.insert(out.end(), part.begin(), part.end());
  return out;
}

// LDA #v ; STA $abs
Bytes store(std::uint16_t address, std::uint8_t value) {
  return {0xA9u, value, 0x8Du, static_cast<std::uint8_t>(address & 0xFFu), static_cast<std::uint8_t>(address >> 8)};
}
// STZ $abs
Bytes clear(std::uint16_t address) {
  return {0x9Cu, static_cast<std::uint8_t>(address & 0xFFu), static_cast<std::uint8_t>(address >> 8)};
}
// LDA $10 ; STA $abs
Bytes storeCounter(std::uint16_t address) {
  return {0xA5u, kFrameCounter, 0x8Du, static_cast<std::uint8_t>(address & 0xFFu), static_cast<std::uint8_t>(address >> 8)};
}
// LDX #n ; DEX ; BNE : a short wait
Bytes wait(std::uint8_t n) { return {0xA2u, n, 0xCAu, 0xD0u, 0xFDu}; }
Bytes awaitVblank() { return {0xADu, 0x12u, 0x42u, 0x10u, 0xFBu}; }
Bytes awaitPicture() { return {0xADu, 0x12u, 0x42u, 0x30u, 0xFBu}; }

// A general-purpose DMA on channel 0 of `size` bytes from a 24-bit address to
// $2100 + `port` under `pattern`, started at once.
Bytes dma(std::uint8_t pattern, std::uint8_t port, std::uint32_t source, std::uint16_t size) {
  return joined({store(0x4300u, pattern), store(0x4301u, port), store(0x4302u, static_cast<std::uint8_t>(source & 0xFFu)),
                 store(0x4303u, static_cast<std::uint8_t>((source >> 8) & 0xFFu)),
                 store(0x4304u, static_cast<std::uint8_t>(source >> 16)), store(0x4305u, static_cast<std::uint8_t>(size & 0xFFu)),
                 store(0x4306u, static_cast<std::uint8_t>(size >> 8)), store(0x420Bu, 0x01u)});
}

// ---- the memories ------------------------------------------------------------------

// A character of `planes` bitplanes from eight rows of eight color indices.
Bytes character(unsigned planes, const std::array<std::array<std::uint8_t, 8>, 8>& rows) {
  Bytes out(8u * planes, 0u);
  for (unsigned row = 0u; row < 8u; ++row) {
    for (unsigned plane = 0u; plane < planes; ++plane) {
      std::uint8_t bits = 0u;
      for (unsigned column = 0u; column < 8u; ++column) {
        bits = static_cast<std::uint8_t>(bits | (((rows[row][column] >> plane) & 1u) << (7u - column)));
      }
      out[(plane / 2u) * 16u + row * 2u + (plane % 2u)] = bits;
    }
  }
  return out;
}

// `count` characters of `planes` planes: character 0 blank, 1 solid, 2 a checker,
// the rest a diagonal through the palette with transparent spots.
Bytes characterSet(unsigned planes, unsigned count, unsigned seed) {
  const unsigned colors = 1u << planes;
  std::array<std::array<std::uint8_t, 8>, 8> rows{};
  Bytes out;
  for (unsigned tile = 0u; tile < count; ++tile) {
    for (unsigned row = 0u; row < 8u; ++row) {
      for (unsigned column = 0u; column < 8u; ++column) {
        unsigned color = 0u;
        if (tile == 1u) color = (1u + seed) % colors;
        if (tile == 2u) color = ((row + column) & 1u) != 0u ? 2u % colors : 3u % colors;
        if (tile >= 3u) color = (row * 3u + column * 5u + tile * 7u + seed) % colors;
        rows[row][column] = static_cast<std::uint8_t>(color);
      }
    }
    out = joined({out, character(planes, rows)});
  }
  return out;
}

// A 32x32 tilemap through `tiles` characters, every palette, the priority bit and
// both flips.
Bytes tilemap(unsigned seed, unsigned tiles) {
  Bytes out;
  for (unsigned index = 0u; index < 1024u; ++index) {
    const unsigned i = index + seed;
    const unsigned tile = (i >> seed) % tiles;
    const unsigned palette = (i >> (2u + seed)) & 7u;
    const unsigned priority = (i >> (5u + seed)) & 1u;
    const unsigned flips = (i >> 7u) & 3u;
    const unsigned entry = tile | (palette << 10) | (priority << 13) | (flips << 14);
    out.push_back(static_cast<std::uint8_t>(entry & 0xFFu));
    out.push_back(static_cast<std::uint8_t>(entry >> 8));
  }
  return out;
}

// A 32x32 tilemap of one character, palette 0, no flip.
Bytes solidMap(unsigned tile) {
  Bytes out;
  for (unsigned index = 0u; index < 1024u; ++index) {
    out.push_back(static_cast<std::uint8_t>(tile & 0xFFu));
    out.push_back(static_cast<std::uint8_t>(tile >> 8));
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

// One sprite's record: X, Y, the character, the attributes, and the two high bits.
struct SpriteRecord {
  int x;
  std::uint8_t y;
  std::uint8_t tile;
  std::uint8_t attributes;  // vhoopppN
  bool large;
};

Bytes oamTable(const std::vector<SpriteRecord>& sprites) {
  Bytes out(544u, 0u);
  for (unsigned i = 0u; i < sprites.size() && i < 128u; ++i) {
    const SpriteRecord& s = sprites[i];
    const unsigned wide = static_cast<unsigned>(s.x) & 0x1FFu;
    out[i * 4u] = static_cast<std::uint8_t>(wide & 0xFFu);
    out[i * 4u + 1u] = s.y;
    out[i * 4u + 2u] = s.tile;
    out[i * 4u + 3u] = s.attributes;
    const unsigned bits = ((wide >> 8) & 1u) | (s.large ? 2u : 0u);
    out[512u + i / 4u] |= static_cast<std::uint8_t>(bits << ((i % 4u) * 2u));
  }
  return out;
}

// Eight sprites across the picture at the four characters, every palette and
// priority, the even ones large.
std::vector<SpriteRecord> eightSprites() {
  std::vector<SpriteRecord> out;
  for (unsigned sprite = 0u; sprite < 8u; ++sprite) {
    out.push_back(SpriteRecord{
        .x = static_cast<int>(20u + sprite * 28u),
        .y = static_cast<std::uint8_t>(30u + sprite * 20u),
        .tile = static_cast<std::uint8_t>(sprite & 3u),
        .attributes = static_cast<std::uint8_t>(((sprite & 7u) << 1) | (((sprite >> 1) & 3u) << 4) | ((sprite & 1u) << 6)),
        .large = (sprite & 1u) == 0u});
  }
  return out;
}

// An HDMA table writing the backdrop's color on every line under pattern 3
// ($2121, $2121, $2122, $2122): entry n's color is n * $0421.
Bytes backdropTable() {
  Bytes out;
  const auto lines = [&](unsigned from, unsigned count) {
    out.push_back(static_cast<std::uint8_t>(0x80u | count));
    for (unsigned line = from; line < from + count; ++line) {
      const unsigned color = (line * 0x0421u) & 0x7FFFu;
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

// An HDMA table writing one byte register a value a line, from `values`, under
// pattern 0.
Bytes byteTable(const std::vector<std::uint8_t>& values) {
  Bytes out;
  std::size_t at = 0u;
  while (at < values.size()) {
    const std::size_t count = std::min<std::size_t>(127u, values.size() - at);
    out.push_back(static_cast<std::uint8_t>(0x80u | count));
    for (std::size_t i = 0u; i < count; ++i) out.push_back(values[at + i]);
    at += count;
  }
  out.push_back(0u);
  return out;
}

// Mode 7's memory: the low byte of each word the map entry, the high byte a
// character's pixel. The map runs through the characters; a character's pixel is
// a pattern of its own with transparent spots and, in the top half of the
// characters, bit 7 set on some pixels.
Bytes fieldMemory() {
  Bytes out(65536u, 0u);
  for (unsigned word = 0u; word < 32768u; ++word) {
    const unsigned mapX = word & 0x7Fu;
    const unsigned mapY = word >> 7;
    out[word * 2u] = static_cast<std::uint8_t>((mapX * 3u + mapY * 5u + (mapX * mapY >> 4)) & 0xFFu);
    const unsigned character = word >> 6;
    const unsigned pixelY = (word >> 3) & 7u;
    const unsigned pixelX = word & 7u;
    unsigned pixel = (character * 11u + pixelY * 29u + pixelX * 37u) & 0x7Fu;
    if (((pixelX + pixelY) & 3u) == 0u) pixel = 0u;
    if (character >= 128u && ((pixelX ^ pixelY) & 1u) != 0u) pixel |= 0x80u;
    out[word * 2u + 1u] = static_cast<std::uint8_t>(pixel);
  }
  return out;
}

// ---- the cartridge ------------------------------------------------------------------

struct Block {
  std::uint16_t word;  // the VRAM word address the block is written at
  Bytes bytes;
};

// What a scene is: the memories, the registers written after them, the handler's
// parts and the interrupt.
struct Scene {
  std::vector<Block> vram;
  Bytes palette = ::snaggletooth::palette();
  Bytes oam = oamTable(eightSprites());
  std::vector<std::pair<std::uint16_t, std::uint8_t>> registers;
  // An HDMA channel 1 table under `hdmaPattern` to $2100 + `hdmaPort`, on every
  // frame, or none.
  Bytes hdmaTable;
  std::uint8_t hdmaPattern = 0;
  std::uint8_t hdmaPort = 0;
  // The handler's blank-time part; its picture-time part, after the picture begins
  // and a short wait; and an interrupt body at dot `htime` of line `vtime`.
  Bytes inBlank;
  Bytes inPicture;
  Bytes irqBody;
  std::uint16_t htime = 0;
  std::uint16_t vtime = 0;
  bool irq = false;
  // Bytes a scene's own transfers read, at bank 0 $B000.
  Bytes data;
};

// A one-bank LoROM image's data placed in the banks after the first: a 24-bit
// address for each block, and the image.
class Image {
 public:
  std::uint32_t place(const Bytes& bytes) {
    // Each block whole in one bank, so a transfer never crosses a bank's end.
    if (bytes.size() > 0x8000u) throw std::length_error("a block larger than a bank");
    if (next_ + bytes.size() > 0x8000u) {
      ++bank_;
      next_ = 0u;
    }
    const std::uint32_t at = (static_cast<std::uint32_t>(bank_) << 16) | (0x8000u + next_);
    rom_.resize(static_cast<std::size_t>(bank_ + 1u) * 0x8000u, 0u);
    std::copy(bytes.begin(), bytes.end(), rom_.begin() + static_cast<std::ptrdiff_t>(bank_ * 0x8000u + next_));
    next_ += static_cast<std::uint32_t>(bytes.size());
    return at;
  }
  void placeAt(std::uint16_t address, const Bytes& bytes) {
    if (rom_.size() < 0x8000u) rom_.resize(0x8000u, 0u);
    std::copy(bytes.begin(), bytes.end(), rom_.begin() + (address - 0x8000u));
  }
  Bytes rom_;

 private:
  unsigned bank_ = 1u;
  std::uint32_t next_ = 0u;
};

constexpr std::uint16_t kNmiHandler = 0xA200u;
constexpr std::uint16_t kIrqHandler = 0xA600u;
constexpr std::uint16_t kTables = 0xB000u;

Bytes image(const Scene& scene) {
  Image image;
  // The data, in the banks after the first.
  // A block larger than a bank goes in pieces, each a transfer of its own, the port's
  // address running on from one to the next.
  struct Piece {
    std::optional<std::uint16_t> word;  // the VRAM word the piece begins at, or the one the last piece reached
    std::uint32_t at;
    std::uint16_t size;
  };
  std::vector<Piece> pieces;
  for (const Block& block : scene.vram) {
    std::size_t from = 0u;
    while (from < block.bytes.size()) {
      const std::size_t size = std::min<std::size_t>(0x8000u, block.bytes.size() - from);
      const Bytes part(block.bytes.begin() + static_cast<std::ptrdiff_t>(from),
                       block.bytes.begin() + static_cast<std::ptrdiff_t>(from + size));
      pieces.push_back(Piece{.word = from == 0u ? std::optional<std::uint16_t>(block.word) : std::nullopt,
                             .at = image.place(part),
                             .size = static_cast<std::uint16_t>(size)});
      from += size;
    }
  }
  const std::uint32_t paletteAt = image.place(scene.palette);
  const std::uint32_t oamAt = image.place(scene.oam);
  const std::uint32_t tableAt = scene.hdmaTable.empty() ? 0u : image.place(scene.hdmaTable);

  // The setup under forced blank: the memories, the registers, the HDMA channel and
  // the interrupt.
  Bytes program = joined({{0x78u}, store(0x2100u, 0x80u), store(0x2115u, 0x80u)});
  for (const Piece& piece : pieces) {
    if (piece.word.has_value()) {
      program = joined({program, store(0x2116u, static_cast<std::uint8_t>(*piece.word & 0xFFu)),
                        store(0x2117u, static_cast<std::uint8_t>(*piece.word >> 8))});
    }
    program = joined({program, dma(0x01u, 0x18u, piece.at, piece.size)});
  }
  program = joined({program, clear(0x2121u), dma(0x00u, 0x22u, paletteAt, static_cast<std::uint16_t>(scene.palette.size())),
                    clear(0x2102u), clear(0x2103u), dma(0x00u, 0x04u, oamAt, static_cast<std::uint16_t>(scene.oam.size()))});
  for (const auto& [address, value] : scene.registers) program = joined({program, store(address, value)});
  if (!scene.hdmaTable.empty()) {
    program = joined({program, store(0x4310u, scene.hdmaPattern), store(0x4311u, scene.hdmaPort),
                      store(0x4312u, static_cast<std::uint8_t>(tableAt & 0xFFu)),
                      store(0x4313u, static_cast<std::uint8_t>((tableAt >> 8) & 0xFFu)),
                      store(0x4314u, static_cast<std::uint8_t>(tableAt >> 16)), store(0x420Cu, 0x02u)});
  }
  if (scene.irq) {
    program = joined({program, store(0x4207u, static_cast<std::uint8_t>(scene.htime & 0xFFu)),
                      store(0x4208u, static_cast<std::uint8_t>(scene.htime >> 8)),
                      store(0x4209u, static_cast<std::uint8_t>(scene.vtime & 0xFFu)),
                      store(0x420Au, static_cast<std::uint8_t>(scene.vtime >> 8))});
  }
  program = joined({program, awaitVblank(), store(0x2100u, 0x0Fu), store(0x4200u, scene.irq ? 0xB0u : 0x80u),
                    {0x58u, 0xCBu, 0x80u, 0xFDu}});  // CLI ; WAI ; BRA -3

  // The handler: the flag read, the frame counted, HDMA kept on, the blank-time
  // part, and the picture-time part after the picture begins.
  Bytes handler = joined({{0x48u, 0xDAu}, {0xADu, 0x10u, 0x42u}, {0xE6u, kFrameCounter}});
  if (!scene.hdmaTable.empty()) handler = joined({handler, store(0x420Cu, 0x02u)});
  handler = joined({handler, scene.inBlank});
  if (!scene.inPicture.empty()) handler = joined({handler, awaitPicture(), wait(0x30u), scene.inPicture});
  handler = joined({handler, {0xFAu, 0x68u, 0x40u}});
  const Bytes irq = joined({{0x48u}, scene.irqBody, {0xADu, 0x11u, 0x42u}, {0x68u, 0x40u}});

  if (program.size() > kNmiHandler - 0x8000u) throw std::length_error("the program runs into the handler");
  if (handler.size() > kIrqHandler - kNmiHandler) throw std::length_error("the handler runs into the interrupt's");
  if (irq.size() > kTables - kIrqHandler) throw std::length_error("the interrupt runs past its room");
  image.placeAt(0x8000u, program);
  image.placeAt(kNmiHandler, handler);
  image.placeAt(kIrqHandler, irq);
  if (!scene.data.empty()) image.placeAt(kTables, scene.data);
  Bytes& rom = image.rom_;
  rom[0x7FFAu] = static_cast<std::uint8_t>(kNmiHandler & 0xFFu);
  rom[0x7FFBu] = static_cast<std::uint8_t>(kNmiHandler >> 8);
  rom[0x7FFCu] = 0x00u;
  rom[0x7FFDu] = 0x80u;
  rom[0x7FFEu] = static_cast<std::uint8_t>(kIrqHandler & 0xFFu);
  rom[0x7FFFu] = static_cast<std::uint8_t>(kIrqHandler >> 8);
  return rom;
}

// ---- the scenes ---------------------------------------------------------------------

// The base: Mode 1, BG1 and BG2 over distinct maps of four 4bpp characters at
// byte 0, the sprites' characters at byte $4000 (name base 1), BG1's map at byte
// $1000 and BG2's at $1800, both backgrounds and the sprites on the main screen,
// the backdrop rewritten by HDMA every line, and the frame counter switching color
// math and BG2's mosaic.
Scene baseScene() {
  Scene scene;
  scene.vram = {{0x0000u, characterSet(4u, 4u, 0u)},
                {0x2000u, characterSet(4u, 4u, 1u)},
                {0x0800u, tilemap(0u, 4u)},
                {0x0C00u, tilemap(1u, 4u)}};
  scene.registers = {{0x2105u, 0x01u}, {0x2107u, 0x08u}, {0x2108u, 0x0Cu}, {0x210Bu, 0x00u},
                     {0x2101u, 0x01u}, {0x212Cu, 0x13u}, {0x212Du, 0x00u}};
  scene.hdmaTable = backdropTable();
  scene.hdmaPattern = 0x03u;
  scene.hdmaPort = 0x21u;
  scene.inBlank = joined({{0xA5u, kFrameCounter, 0x29u, 0x20u, 0xF0u, 0x04u},  // LDA $10 ; AND #$20 ; BEQ +4
                          {0xA9u, 0x3Fu, 0x80u, 0x02u},                        // LDA #$3F ; BRA +2
                          {0xA9u, 0x00u},                                      // LDA #$00
                          {0x8Du, 0x31u, 0x21u},                               // STA $2131
                          {0xA5u, kFrameCounter, 0x29u, 0x01u, 0xF0u, 0x04u},  // LDA $10 ; AND #$01 ; BEQ +4
                          {0xA9u, 0x32u, 0x80u, 0x02u},                        // LDA #$32 ; BRA +2
                          {0xA9u, 0x00u},                                      // LDA #$00
                          {0x8Du, 0x06u, 0x21u}});                             // STA $2106
  scene.inPicture = joined({storeCounter(0x210Du), clear(0x210Du)});
  return scene;
}

SnesConfig config(const Bytes& rom, gpu::Device* device) {
  SnesConfig c{.rom = rom};
  c.gpuPpu = device;
  return c;
}

// A finished frame, kept whole.
struct Frame {
  unsigned width = 0;
  unsigned height = 0;
  std::uint8_t field = 0;
  Bytes pixels;
};

// A timeline, kept whole.
struct Kept {
  unsigned height = 0;
  unsigned width = 0;
  std::uint8_t field = 0;
  unsigned firstLine = 0;
  std::vector<PpuTimeline::Line> lines;
  std::vector<PpuTimeline::Event> events;
  std::vector<PpuTimeline::VramWrite> vramWrites;
  Bytes vram, palettes, oams, spriteBuffers;
  [[nodiscard]] PpuTimeline view() const {
    PpuTimeline t;
    t.height = height;
    t.width = width;
    t.field = field;
    t.firstLine = firstLine;
    t.lines = lines;
    t.events = events;
    t.vramWrites = vramWrites;
    t.vram = vram;
    t.palettes = palettes;
    t.oams = oams;
    t.spriteBuffers = spriteBuffers;
    return t;
  }
};

Kept keep(const PpuTimeline& t) {
  return Kept{.height = t.height,
              .width = t.width,
              .field = t.field,
              .firstLine = t.firstLine,
              .lines = {t.lines.begin(), t.lines.end()},
              .events = {t.events.begin(), t.events.end()},
              .vramWrites = {t.vramWrites.begin(), t.vramWrites.end()},
              .vram = {t.vram.begin(), t.vram.end()},
              .palettes = {t.palettes.begin(), t.palettes.end()},
              .oams = {t.oams.begin(), t.oams.end()},
              .spriteBuffers = {t.spriteBuffers.begin(), t.spriteBuffers.end()}};
}

// The GPU's rows read back at each timeline report, when the device has just drawn
// the frame, held against PpuTimeline::draw()'s rows and against the machine's own
// frame of the same cycle: a 512-wide frame column for column, a 256-wide one each
// position against both of draw()'s halves. The first difference is named.
struct Checker final : FrameObserver, PpuTimelineObserver {
  Snes* machine = nullptr;
  player::SdlGpuDevice* device = nullptr;
  std::optional<Frame> kept;
  std::uint64_t frameAt = 0;
  unsigned reports = 0;
  unsigned checked = 0;
  unsigned gpuDiffers = 0;
  unsigned frameDiffers = 0;
  std::string first;
  std::vector<Kept> timelines;
  bool keepTimelines = false;
  Bytes drawn = Bytes(PpuTimeline::kDrawBytes);
  Bytes gpu;

  void frame(const VideoFrame& picture) override {
    kept = Frame{.width = picture.width,
                          .height = picture.height,
                          .field = picture.field,
                          .pixels = Bytes(picture.pixels.begin(), picture.pixels.end())};
    frameAt = machine->state().master;
  }
  void frame(const PpuTimeline& t) override {
    ++reports;
    if (keepTimelines) timelines.push_back(keep(t));
    if (!device->readBack(t.width, t.height, gpu)) {
      if (first.empty()) first = "report " + std::to_string(reports) + ": no read-back";
      ++gpuDiffers;
      return;
    }
    t.draw(drawn);
    ++checked;
    bool named = false;
    const auto note = [&](const std::string& what, unsigned row, unsigned x, const std::uint8_t* got,
                          const std::uint8_t* want) {
      if (!first.empty() || named) return;
      named = true;
      first = "report " + std::to_string(reports) + " line " + std::to_string(row + 1u) + " x " +
              std::to_string(x) + ": gpu " + std::to_string(got[0]) + "," + std::to_string(got[1]) + "," +
              std::to_string(got[2]) + " " + what + " " + std::to_string(want[0]) + "," + std::to_string(want[1]) +
              "," + std::to_string(want[2]);
    };
    bool gpuDiffered = false;
    for (unsigned row = t.firstLine - 1u; row < t.height; ++row) {
      for (unsigned x = 0u; x < t.width; ++x) {
        const std::uint8_t* got = gpu.data() + (static_cast<std::size_t>(row) * t.width + x) * 4u;
        const std::size_t column = t.width == kHiresWidth ? x : x * 2u;
        for (unsigned half = 0u; half < (t.width == kHiresWidth ? 1u : 2u); ++half) {
          const std::uint8_t* want = drawn.data() + (static_cast<std::size_t>(row) * kHiresWidth + column + half) * 4u;
          if (!std::equal(got, got + 4, want)) {
            note("timeline", row, x, got, want);
            gpuDiffered = true;
          }
        }
      }
    }
    if (gpuDiffered) ++gpuDiffers;
    if (!kept.has_value() || frameAt != machine->state().master) return;
    if (kept->width != t.width || kept->height != t.height || kept->field != t.field) {
      ++frameDiffers;
      if (first.empty()) first = "report " + std::to_string(reports) + ": the frame's shape differs";
      return;
    }
    bool frameDiffered = false;
    for (unsigned row = t.firstLine - 1u; row < t.height; ++row) {
      for (unsigned x = 0u; x < t.width; ++x) {
        const std::uint8_t* got = gpu.data() + (static_cast<std::size_t>(row) * t.width + x) * 4u;
        const std::uint8_t* want = kept->pixels.data() + (static_cast<std::size_t>(row) * t.width + x) * 4u;
        if (!std::equal(got, got + 4, want)) {
          note("frame", row, x, got, want);
          frameDiffered = true;
        }
      }
    }
    if (frameDiffered) ++frameDiffers;
  }
};

// A device opened without a window, once for the executable; a machine with no
// device skips every case.
class GpuCase : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    if (!SDL_Init(SDL_INIT_VIDEO)) GTEST_SKIP() << "SDL does not start here: " << SDL_GetError();
  }
  void SetUp() override {
    std::string error;
    if (!device_.open(error)) GTEST_SKIP() << "no GPU device here: " << error;
    device_.setPlacement(gpu::FieldPlacement::Field);
  }

  // The scene run for `frames` frames on a machine with the device, every report
  // checked; the checker is returned for what a case asks of the reports.
  Checker run(const Scene& scene, unsigned frames, bool keepTimelines = false) {
    Checker checker;
    checker.keepTimelines = keepTimelines;
    Snes machine(config(image(scene), &device_));
    checker.machine = &machine;
    checker.device = &device_;
    machine.setFrameObserver(&checker);
    machine.setPpuTimelineObserver(&checker);
    for (unsigned call = 0u; call < frames; ++call) machine.run(kFrame);
    machine.setFrameObserver(nullptr);
    machine.setPpuTimelineObserver(nullptr);
    checker.machine = nullptr;
    return checker;
  }

  ::testing::AssertionResult drawsAlike(const Checker& checker, unsigned atLeast) {
    if (checker.checked < atLeast) {
      return ::testing::AssertionFailure() << checker.checked << " frames checked of " << atLeast << " expected; "
                                           << checker.first;
    }
    if (checker.gpuDiffers != 0u || checker.frameDiffers != 0u) {
      return ::testing::AssertionFailure() << checker.gpuDiffers << " frames differ from the timeline's and "
                                           << checker.frameDiffers << " from the machine's; first: " << checker.first;
    }
    return ::testing::AssertionSuccess();
  }

  player::SdlGpuDevice device_;
};

TEST_F(GpuCase, TheGpuDrawsTheFrameTheTimelineDraws) {
  const Checker checker = run(baseScene(), 16u);
  EXPECT_TRUE(drawsAlike(checker, 15u));
}

// Mode 7: the field at $0000 through a rotating matrix with the center and the
// scroll set, then each variant: screen-over bits 7-6 at each value, EXTBG with
// pixels above 127, direct color, and the two flips.
Scene fieldScene(std::uint8_t m7sel, bool extbg, bool directColor) {
  Scene scene;
  scene.vram = {{0x0000u, fieldMemory()}};
  scene.registers = {{0x2105u, 0x07u},
                     {0x211Au, m7sel},
                     {0x211Bu, 0x40u}, {0x211Bu, 0x01u},   // A = 1.25
                     {0x211Cu, 0x30u}, {0x211Cu, 0x00u},   // B = 0.19
                     {0x211Du, 0xD0u}, {0x211Du, 0xFFu},   // C = -0.19
                     {0x211Eu, 0xC0u}, {0x211Eu, 0x00u},   // D = 0.75
                     {0x211Fu, 0x80u}, {0x211Fu, 0x00u},   // the center
                     {0x2120u, 0x70u}, {0x2120u, 0x00u},
                     {0x210Du, 0x20u}, {0x210Du, 0x00u},   // the scroll
                     {0x210Eu, 0x10u}, {0x210Eu, 0x00u},
                     {0x2133u, static_cast<std::uint8_t>(extbg ? 0x40u : 0x00u)},
                     {0x2130u, static_cast<std::uint8_t>(directColor ? 0x01u : 0x00u)},
                     {0x212Cu, static_cast<std::uint8_t>(extbg ? 0x13u : 0x11u)},
                     {0x2101u, 0x01u}};
  // The matrix turns a little every frame: B and C written from the counter.
  scene.inBlank = joined({storeCounter(0x211Cu), store(0x211Cu, 0x00u), storeCounter(0x211Du), store(0x211Du, 0xFFu)});
  return scene;
}

TEST_F(GpuCase, ModeSevenEachWay) {
  for (const unsigned over : {0x00u, 0x40u, 0x80u, 0xC0u}) {
    SCOPED_TRACE(::testing::Message() << "screen-over " << over);
    EXPECT_TRUE(drawsAlike(run(fieldScene(static_cast<std::uint8_t>(over), false, false), 4u), 3u));
  }
  EXPECT_TRUE(drawsAlike(run(fieldScene(0x80u, true, false), 4u), 3u)) << "EXTBG";
  EXPECT_TRUE(drawsAlike(run(fieldScene(0x00u, false, true), 4u), 3u)) << "direct color";
  EXPECT_TRUE(drawsAlike(run(fieldScene(0x01u, true, false), 4u), 3u)) << "flipped across";
  EXPECT_TRUE(drawsAlike(run(fieldScene(0x02u, false, false), 4u), 3u)) << "flipped down";
  EXPECT_TRUE(drawsAlike(run(fieldScene(0x03u, true, true), 4u), 3u)) << "both flips, EXTBG and direct color";
}

// BG3's tilemap as an offset table: row 0 the horizontal entries and row 1, eight
// lines below, the vertical ones, each background under its own bit; Mode 4 reads
// one entry whose bit 15 names the axis.
Bytes offsetTable(unsigned mode) {
  Bytes out(2048u, 0u);
  const auto put = [&out](unsigned row, unsigned column, unsigned entry) {
    const std::size_t at = (row * 32u + column) * 2u;
    out[at] = static_cast<std::uint8_t>(entry & 0xFFu);
    out[at + 1u] = static_cast<std::uint8_t>(entry >> 8);
  };
  for (unsigned column = 0u; column < 32u; ++column) {
    unsigned horizontal = 0u;
    unsigned vertical = 0u;
    if (column % 3u == 1u) horizontal |= 0x2000u | ((column * 8u + 16u) & 0x3F8u);
    if (column % 4u == 2u) horizontal |= 0x4000u | 0x40u;
    if (column % 5u == 3u) vertical |= 0x4000u | (column * 4u + 5u);
    if (column % 7u == 1u) vertical |= 0x2000u | 0x30u;
    if (mode == 4u) {
      put(0u, column, (column % 2u) != 0u ? (0x8000u | vertical) : horizontal);
    } else {
      put(0u, column, horizontal);
      put(1u, column, vertical);
    }
  }
  return out;
}

// A scene in one of the tilemap modes: every character depth in VRAM — 4bpp at
// word 0, the sprites' at $2000, 2bpp at $3000, 8bpp at $4000 — the four maps at
// $0800, $0C00, $1000 and $1400, each background's character base at its depth's
// set, every background the mode has on the main screen with the sprites, and the
// base scene's dynamics. In modes 2, 4 and 6 BG3's map is the offset table.
Scene modeScene(unsigned mode, bool bg3Lifted = false, bool bg2Large = false) {
  Scene scene = baseScene();
  scene.vram = {{0x0000u, characterSet(4u, 4u, 0u)}, {0x2000u, characterSet(4u, 4u, 1u)},
                {0x3000u, characterSet(2u, 4u, 2u)}, {0x4000u, characterSet(8u, 4u, 3u)},
                {0x0800u, tilemap(0u, 4u)},          {0x0C00u, tilemap(1u, 4u)},
                {0x1000u, (mode == 2u || mode == 4u || mode == 6u) ? offsetTable(mode) : tilemap(2u, 4u)},
                {0x1400u, tilemap(3u, 4u)}};
  static constexpr std::uint8_t kBg12[7] = {0x33u, 0x00u, 0x00u, 0x04u, 0x34u, 0x30u, 0x00u};
  static constexpr std::uint8_t kBg34[7] = {0x33u, 0x03u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u};
  static constexpr std::uint8_t kMain[7] = {0x1Fu, 0x17u, 0x13u, 0x13u, 0x13u, 0x13u, 0x11u};
  const auto bgmode = static_cast<std::uint8_t>(mode | (bg3Lifted ? 0x08u : 0u) | (bg2Large ? 0x20u : 0u));
  scene.registers = {{0x2105u, bgmode},      {0x2107u, 0x08u},        {0x2108u, 0x0Cu}, {0x2109u, 0x10u},
                     {0x210Au, 0x14u},       {0x210Bu, kBg12[mode]},  {0x210Cu, kBg34[mode]},
                     {0x2101u, 0x01u},       {0x212Cu, kMain[mode]},  {0x212Du, 0x00u},
                     {0x210Du, 0x05u},       {0x210Du, 0x00u},        {0x2111u, 0x18u}, {0x2111u, 0x00u},
                     {0x2112u, 0x03u},       {0x2112u, 0x00u}};
  return scene;
}

TEST_F(GpuCase, EveryTilemapModeDrawsAlike) {
  for (unsigned mode = 0u; mode <= 6u; ++mode) {
    SCOPED_TRACE(::testing::Message() << "mode " << mode);
    EXPECT_TRUE(drawsAlike(run(modeScene(mode), 4u), 3u));
  }
  EXPECT_TRUE(drawsAlike(run(modeScene(1u, true, true), 4u), 3u)) << "mode 1 with BG3 lifted and 16x16 BG2 tiles";
  for (const unsigned mode : {3u, 4u}) {
    SCOPED_TRACE(::testing::Message() << "mode " << mode << " under direct color");
    Scene scene = modeScene(mode);
    scene.registers.emplace_back(0x2130u, 0x01u);
    EXPECT_TRUE(drawsAlike(run(scene, 4u), 3u));
  }
}

TEST_F(GpuCase, TheOffsetTableMovesTheColumns) {
  for (const unsigned mode : {2u, 4u, 6u}) {
    SCOPED_TRACE(::testing::Message() << "mode " << mode);
    Scene scene = modeScene(mode);
    // BG3's own coarse scroll moves which table column a tile reads.
    scene.inBlank = joined({scene.inBlank, storeCounter(0x2111u), clear(0x2111u)});
    EXPECT_TRUE(drawsAlike(run(scene, 6u), 5u));
  }
}

TEST_F(GpuCase, MosaicInEveryShape) {
  {
    Scene scene = modeScene(1u);
    scene.registers.emplace_back(0x2106u, 0x51u);
    scene.inBlank.clear();  // the switches would write $2106 back
    EXPECT_TRUE(drawsAlike(run(scene, 4u), 3u)) << "Mode 1, BG1 under 6x6 blocks";
  }
  {
    Scene scene = modeScene(5u);
    scene.registers.emplace_back(0x2106u, 0x53u);
    scene.inBlank.clear();
    EXPECT_TRUE(drawsAlike(run(scene, 4u), 3u)) << "Mode 5, blocks counted in half-pixels";
  }
  {
    Scene scene = fieldScene(0x80u, true, false);
    scene.registers.emplace_back(0x2106u, 0x73u);
    EXPECT_TRUE(drawsAlike(run(scene, 4u), 3u)) << "Mode 7 under EXTBG, the second layer's own bits";
  }
}

// The base scene with BG1's mosaic size written by HDMA on every line, so a row of
// blocks takes the size the line it began on holds.
TEST_F(GpuCase, MosaicSizeChangedByHdmaHoldsTheRowsSize) {
  Scene scene = baseScene();
  std::vector<std::uint8_t> sizes;
  for (unsigned line = 0u; line < 224u; ++line) sizes.push_back(static_cast<std::uint8_t>(((line / 7u) % 16u) << 4 | 0x01u));
  scene.hdmaTable = byteTable(sizes);
  scene.hdmaPattern = 0x00u;
  scene.hdmaPort = 0x06u;
  scene.inBlank.clear();
  EXPECT_TRUE(drawsAlike(run(scene, 6u), 5u));
}

TEST_F(GpuCase, TheWindowsAndTheColorRegions) {
  // Both windows, each background inverting or not, the four logic operations,
  // the sprites masked, and $2130's regions at every value.
  for (const unsigned logic : {0xE4u, 0x0Eu, 0xB1u}) {
    for (const unsigned regions : {0x00u, 0x10u, 0x20u, 0x30u, 0x40u, 0x80u, 0xC0u, 0xF0u}) {
      SCOPED_TRACE(::testing::Message() << "logic " << logic << " regions " << regions);
      Scene scene = baseScene();
      scene.registers.insert(scene.registers.end(),
                             {{0x2126u, 40u}, {0x2127u, 120u}, {0x2128u, 90u}, {0x2129u, 200u},
                              {0x2123u, 0x3Bu}, {0x2124u, 0xA0u}, {0x2125u, 0xAAu},
                              {0x212Au, static_cast<std::uint8_t>(logic)}, {0x212Bu, 0x0Du}, {0x212Eu, 0x17u}, {0x212Fu, 0x00u},
                              {0x2130u, static_cast<std::uint8_t>(regions)}});
      EXPECT_TRUE(drawsAlike(run(scene, 4u), 3u));
    }
  }
  // A window edge written inside line 100 reaches the positions drawn after it.
  Scene scene = baseScene();
  scene.registers.insert(scene.registers.end(), {{0x2126u, 40u}, {0x2127u, 200u}, {0x2123u, 0x22u}, {0x212Eu, 0x03u}});
  scene.inBlank = joined({scene.inBlank, store(0x2126u, 40u)});
  scene.irqBody = store(0x2126u, 100u);
  scene.htime = 90u;
  scene.vtime = 100u;
  scene.irq = true;
  EXPECT_TRUE(drawsAlike(run(scene, 4u), 3u)) << "an edge moved inside a line";
}

TEST_F(GpuCase, ColorMathEachWay) {
  struct Math {
    std::uint8_t cgadsub;
    std::uint8_t cgwsel;
    std::uint8_t sub;
  };
  // Add and subtract, halved and not, with the fixed color; the sub screen's pixel;
  // the sub screen's backdrop, which is not halved; the clip region.
  for (const Math math : {Math{0x3Fu, 0x00u, 0x00u}, Math{0xBFu, 0x00u, 0x00u}, Math{0x7Fu, 0x00u, 0x00u},
                          Math{0xFFu, 0x00u, 0x00u}, Math{0x7Fu, 0x02u, 0x02u}, Math{0xFFu, 0x02u, 0x02u},
                          Math{0x7Fu, 0x02u, 0x00u}, Math{0x7Fu, 0xC2u, 0x02u}, Math{0x3Fu, 0x80u, 0x02u}}) {
    SCOPED_TRACE(::testing::Message() << "cgadsub " << unsigned{math.cgadsub} << " cgwsel " << unsigned{math.cgwsel}
                                      << " ts " << unsigned{math.sub});
    Scene scene = baseScene();
    scene.inBlank = joined({{0xA5u, kFrameCounter, 0x29u, 0x01u, 0xF0u, 0x04u},  // the mosaic switch alone
                            {0xA9u, 0x32u, 0x80u, 0x02u}, {0xA9u, 0x00u}, {0x8Du, 0x06u, 0x21u}});
    scene.registers.insert(scene.registers.end(), {{0x2132u, 0x2Fu}, {0x2132u, 0x50u}, {0x2132u, 0x9Au},
                                                   {0x2131u, math.cgadsub}, {0x2130u, math.cgwsel}, {0x212Du, math.sub},
                                                   {0x2126u, 60u}, {0x2127u, 180u}, {0x2125u, 0x20u}});
    EXPECT_TRUE(drawsAlike(run(scene, 4u), 3u));
  }
}

// The base scene's handler switches color math by the frame; these variants keep
// their own math on throughout, since the left half of a half-pixel position is
// drawn under the decision of the main pixel to its left, which the math makes.
TEST_F(GpuCase, TheHalfPixelLine) {
  {
    // Mode 5, math halved with the sub screen's pixel, clipped outside the color
    // window.
    Scene scene = modeScene(5u);
    scene.registers.insert(scene.registers.end(), {{0x2131u, 0x7Fu}, {0x2130u, 0x42u}, {0x212Du, 0x02u},
                                                   {0x2126u, 30u}, {0x2127u, 150u}, {0x2125u, 0x20u}});
    scene.inBlank.clear();
    EXPECT_TRUE(drawsAlike(run(scene, 4u), 3u)) << "Mode 5 under math";
  }
  {
    Scene scene = baseScene();
    scene.registers.insert(scene.registers.end(), {{0x2133u, 0x08u}, {0x2131u, 0xBFu}, {0x2130u, 0x02u}, {0x212Du, 0x02u}});
    scene.inBlank = store(0x2133u, 0x08u);
    EXPECT_TRUE(drawsAlike(run(scene, 4u), 3u)) << "Mode 1 under $2133 bit 3";
  }
  {
    // The line enters half-pixels by a write inside it.
    Scene scene = baseScene();
    scene.registers.insert(scene.registers.end(), {{0x2131u, 0x3Fu}, {0x2130u, 0x02u}, {0x212Du, 0x02u}});
    scene.inBlank = clear(0x2133u);
    scene.irqBody = store(0x2133u, 0x08u);
    scene.htime = 90u;
    scene.vtime = 100u;
    scene.irq = true;
    const Checker checker = run(scene, 4u);
    EXPECT_TRUE(drawsAlike(checker, 3u));
    EXPECT_EQ(device_.lastFrame().width, 512u);
  }
}

TEST_F(GpuCase, TheInterlacedAndTallerPicture) {
  {
    Scene scene = modeScene(5u);
    scene.registers.emplace_back(0x2133u, 0x01u);
    EXPECT_TRUE(drawsAlike(run(scene, 6u), 5u)) << "Mode 5 in half-lines, both parities";
  }
  {
    Scene scene = baseScene();
    scene.registers.insert(scene.registers.end(), {{0x2133u, 0x02u}, {0x2101u, 0xC1u}});
    EXPECT_TRUE(drawsAlike(run(scene, 6u), 5u)) << "sprites at half height, 16x32 among them";
  }
  {
    Scene scene = baseScene();
    scene.registers.emplace_back(0x2133u, 0x04u);
    const Checker checker = run(scene, 4u);
    EXPECT_TRUE(drawsAlike(checker, 3u)) << "the taller picture";
    EXPECT_EQ(device_.lastFrame().height, 239u);
  }
  // The row mapping against the player's weaver, over four fields in each
  // placement: a field placed by rowMapFor into a persisting picture is the
  // picture the weaver composes.
  for (const auto& [placement, mode] : {std::pair{gpu::FieldPlacement::Weave, player::Interlace::Weave},
                                       std::pair{gpu::FieldPlacement::Bob, player::Interlace::Bob},
                                       std::pair{gpu::FieldPlacement::Field, player::Interlace::Off}}) {
    player::FieldWeaver weaver(mode);
    constexpr unsigned kWidth = 8u;
    constexpr unsigned kHeight = 3u;
    Bytes placed(kWidth * kHeight * 2u * 4u, 0u);
    for (unsigned field = 0u; field < 4u; ++field) {
      Bytes pixels(kWidth * kHeight * 4u);
      for (std::size_t i = 0u; i < pixels.size(); ++i) pixels[i] = static_cast<std::uint8_t>(field * 50u + i);
      const player::Shown shown = weaver.present(pixels.data(), kWidth, kHeight, static_cast<std::uint8_t>(field), true);
      const gpu::RowMap rows = gpu::rowMapFor(placement, true, static_cast<std::uint8_t>(field));
      for (unsigned line = 0u; line < kHeight; ++line) {
        for (unsigned copy = 0u; copy < rows.copies; ++copy) {
          const unsigned row = line * rows.step + rows.base + copy;
          std::copy(pixels.begin() + static_cast<std::ptrdiff_t>(line * kWidth * 4u),
                    pixels.begin() + static_cast<std::ptrdiff_t>((line + 1u) * kWidth * 4u),
                    placed.begin() + static_cast<std::ptrdiff_t>(row * kWidth * 4u));
        }
      }
      ASSERT_EQ(shown.height, gpu::rowsUnder(rows, kHeight));
      EXPECT_TRUE(std::equal(shown.pixels, shown.pixels + static_cast<std::size_t>(shown.width) * shown.height * 4u,
                             placed.begin()))
          << "placement " << static_cast<int>(placement) << " field " << field;
    }
  }
}

TEST_F(GpuCase, AnEventInsideTheLineReachesTheDotsAfterIt) {
  {
    // The long store to $2100: the bus byte at the write cycle's last dot, then
    // the value.
    Scene scene = baseScene();
    scene.inPicture = {0xA9u, 0x0Fu, 0x8Fu, 0x00u, 0x21u, 0x8Fu};
    EXPECT_TRUE(drawsAlike(run(scene, 6u), 5u)) << "the early read of $2100";
  }
  {
    // A palette word written under forced blank inside line 100.
    Scene scene = baseScene();
    scene.irqBody = joined({store(0x2100u, 0x80u), store(0x2121u, 50u), storeCounter(0x2122u), clear(0x2122u),
                            store(0x2100u, 0x0Fu)});
    scene.htime = 90u;
    scene.vtime = 100u;
    scene.irq = true;
    EXPECT_TRUE(drawsAlike(run(scene, 6u), 5u)) << "a palette word inside the picture";
  }
  {
    // $212C written inside line 100 and back in vertical blank.
    Scene scene = baseScene();
    scene.inBlank = joined({scene.inBlank, store(0x212Cu, 0x13u)});
    scene.irqBody = store(0x212Cu, 0x11u);
    scene.htime = 90u;
    scene.vtime = 100u;
    scene.irq = true;
    EXPECT_TRUE(drawsAlike(run(scene, 6u), 5u)) << "the main screen's layers inside a line";
  }
  {
    // The character base written between a column's entry read and its character
    // read: the interrupt's dot is moved until the event lands there.
    bool landed = false;
    for (std::uint16_t htime = 60u; htime < 76u && !landed; ++htime) {
      Scene scene = baseScene();
      scene.vram[2] = Block{0x0800u, solidMap(1u)};  // BG1 shows its solid character in every column
      scene.inBlank = joined({scene.inBlank, clear(0x210Bu)});
      scene.irqBody = store(0x210Bu, 0x04u);
      scene.htime = htime;
      scene.vtime = 100u;
      scene.irq = true;
      const Checker checker = run(scene, 6u, true);
      ASSERT_TRUE(drawsAlike(checker, 5u)) << "htime " << htime;
      for (const PpuTimeline::Event& event : checker.timelines.back().events) {
        if (event.field() == PpuTimeline::Field::Bg12nba && event.line() == 100u) {
          const unsigned remainder = (event.dot() - kEntryReadDot) % 8u;
          landed = remainder >= 1u && remainder <= 4u;
        }
      }
    }
    EXPECT_TRUE(landed) << "no interrupt dot placed the write between the two reads";
  }
}

// Sixty-four bytes a transfer writes into VRAM, at bank 0 $B000.
Bytes vramSource() {
  Bytes out;
  for (unsigned at = 0u; at < 64u; ++at) out.push_back(static_cast<std::uint8_t>(at * 37u + 5u));
  return out;
}

TEST_F(GpuCase, AVramWriteInsideTheFrameIsReadAfterItsPosition) {
  {
    // Forced blank on line 100 and 64 bytes into the solid character.
    Scene scene = baseScene();
    scene.data = vramSource();
    scene.irqBody = joined({store(0x2100u, 0x80u), store(0x2116u, 0x10u), clear(0x2117u),
                            dma(0x01u, 0x18u, 0x00B000u, 64u), store(0x2100u, 0x0Fu)});
    scene.htime = 60u;
    scene.vtime = 100u;
    scene.irq = true;
    const Checker checker = run(scene, 6u, true);
    EXPECT_TRUE(drawsAlike(checker, 5u));
    EXPECT_EQ(checker.timelines.back().vramWrites.size(), 64u);
  }
  {
    // A sprite character's row written in line 130's horizontal blank, before line
    // 131's Time; and the same bytes written again, which stores what is there.
    Scene scene = baseScene();
    scene.inBlank = joined({scene.inBlank, store(0x2116u, 0x10u), store(0x2117u, 0x20u)});
    scene.irqBody = joined({store(0x2100u, 0x80u), storeCounter(0x2118u), store(0x2119u, 0x55u),
                            store(0x2116u, 0x10u), store(0x2117u, 0x20u), storeCounter(0x2118u), store(0x2119u, 0x55u),
                            store(0x2100u, 0x0Fu)});
    scene.htime = 243u;
    scene.vtime = 130u;
    scene.irq = true;
    const Checker checker = run(scene, 6u, true);
    EXPECT_TRUE(drawsAlike(checker, 5u));
    EXPECT_EQ(checker.timelines.back().vramWrites.size(), 4u);
  }
  {
    // Forced blank early on line 172 and two bytes into the row of the sprites' fourth
    // character that sprite 7 shows on that line, the screen lit again well before the
    // sprite at the right edge: the line's sprites, gathered as the line began, are
    // drawn from the row as it stood then; the lines after it take the new bytes.
    Scene scene = baseScene();
    scene.data = vramSource();
    scene.irqBody = joined({store(0x2100u, 0x80u), store(0x2116u, 0x31u), store(0x2117u, 0x20u),
                            dma(0x01u, 0x18u, 0x00B000u, 2u), store(0x2100u, 0x0Fu)});
    scene.htime = 8u;
    scene.vtime = 172u;
    scene.irq = true;
    const Checker checker = run(scene, 6u, true);
    EXPECT_TRUE(drawsAlike(checker, 5u));
    EXPECT_EQ(checker.timelines.back().vramWrites.size(), 2u);
  }
}

TEST_F(GpuCase, TheSpriteWalkAndItsBudgets) {
  {
    // Thirty-six sprites on one line: Range keeps thirty-two.
    std::vector<SpriteRecord> sprites;
    for (unsigned i = 0u; i < 36u; ++i) {
      sprites.push_back(SpriteRecord{.x = static_cast<int>(i * 6u), .y = 50u, .tile = static_cast<std::uint8_t>(1u + i % 3u),
                                     .attributes = static_cast<std::uint8_t>(((i & 7u) << 1) | ((i & 3u) << 4)), .large = false});
    }
    Scene scene = baseScene();
    scene.oam = oamTable(sprites);
    EXPECT_TRUE(drawsAlike(run(scene, 4u), 3u)) << "thirty-six on a line";
  }
  {
    // Forty tiles on one line: five sprites sixty-four wide, thirty-four tiles loaded.
    std::vector<SpriteRecord> sprites;
    for (unsigned i = 0u; i < 5u; ++i) {
      sprites.push_back(SpriteRecord{.x = static_cast<int>(i * 48u), .y = 40u, .tile = static_cast<std::uint8_t>(i),
                                     .attributes = static_cast<std::uint8_t>((i << 1) | 0x30u), .large = true});
    }
    Scene scene = baseScene();
    scene.oam = oamTable(sprites);
    scene.registers.emplace_back(0x2101u, 0x41u);
    EXPECT_TRUE(drawsAlike(run(scene, 4u), 3u)) << "forty tiles on a line";
  }
  {
    // $2103 bit 7 moves the sprite the walk begins at.
    Scene scene = baseScene();
    scene.registers.insert(scene.registers.end(), {{0x2102u, 14u}, {0x2103u, 0x80u}});
    EXPECT_TRUE(drawsAlike(run(scene, 4u), 3u)) << "the first sprite moved";
  }
  {
    // Both flips on a 32x32 sprite, each of the eight size pairs, a sprite at
    // X = -256 and one hanging off the left edge, and two palettes overlapping.
    for (unsigned pair = 0u; pair < 8u; ++pair) {
      SCOPED_TRACE(::testing::Message() << "size pair " << pair);
      const std::vector<SpriteRecord> sprites{
          SpriteRecord{.x = 30, .y = 40u, .tile = 3u, .attributes = 0xC4u, .large = true},
          SpriteRecord{.x = 100, .y = 60u, .tile = 2u, .attributes = 0x12u, .large = false},
          SpriteRecord{.x = -256, .y = 60u, .tile = 1u, .attributes = 0x22u, .large = true},
          SpriteRecord{.x = -5, .y = 90u, .tile = 3u, .attributes = 0x36u, .large = true},
          SpriteRecord{.x = 60, .y = 120u, .tile = 3u, .attributes = 0x04u, .large = true},
          SpriteRecord{.x = 64, .y = 124u, .tile = 2u, .attributes = 0x3Au, .large = true},
          SpriteRecord{.x = 120, .y = 250u, .tile = 3u, .attributes = 0x08u, .large = true},  // hung above the picture
      };
      Scene scene = baseScene();
      scene.oam = oamTable(sprites);
      scene.registers.emplace_back(0x2101u, static_cast<std::uint8_t>((pair << 5) | 0x01u));
      EXPECT_TRUE(drawsAlike(run(scene, 4u), 3u));
    }
  }
}

TEST_F(GpuCase, ACarriedLineAndALineWithNone) {
  {
    // Forced blank as line 76 begins and the screen lit part-way along it: Time
    // gathered nothing, the line shows no sprite.
    Scene scene = baseScene();
    scene.irqBody = joined({{0xDAu}, store(0x2100u, 0x80u), wait(0x06u), store(0x2100u, 0x0Fu), {0xFAu}});
    scene.htime = 262u;
    scene.vtime = 75u;
    scene.irq = true;
    const Checker checker = run(scene, 6u, true);
    EXPECT_TRUE(drawsAlike(checker, 5u));
    bool none = false;
    for (const PpuTimeline::Line& line : checker.timelines.back().lines) {
      if (line.lineNumber() == 76u) none = line.spriteSource() == PpuTimeline::SpriteSource::None;
    }
    EXPECT_TRUE(none);
  }
  {
    // A state taken as line 155 begins — a line the front-most sprite stands on, in
    // front of both backgrounds — restored into a machine with the device: the line's
    // sprites are carried, and the frame is drawn from them.
    const Bytes rom = image(baseScene());
    Snes source(SnesConfig{.rom = rom});
    for (int call = 0; call < 5; ++call) source.run(kFrame);
    while (source.state().vpos != 155u) source.run(2u);
    ASSERT_LE(source.state().hpos, 8u);
    const SnesState state = source.state();
    Checker checker;
    checker.keepTimelines = true;
    Snes machine(config(rom, &device_));
    checker.machine = &machine;
    checker.device = &device_;
    machine.setFrameObserver(&checker);
    machine.setPpuTimelineObserver(&checker);
    machine.run(kFrame);
    checker.reports = 0u;
    checker.checked = 0u;
    checker.gpuDiffers = 0u;
    checker.frameDiffers = 0u;
    checker.first.clear();
    checker.timelines.clear();
    ASSERT_TRUE(machine.restore(state));
    machine.run(kFrame * 3u / 4u);
    machine.setFrameObserver(nullptr);
    machine.setPpuTimelineObserver(nullptr);
    ASSERT_EQ(checker.timelines.size(), 1u);
    EXPECT_EQ(checker.timelines.front().firstLine, 155u);
    // The carried buffer holds a sprite: the line is drawn from it and not from Time.
    bool carriedSprite = false;
    for (unsigned x = 0u; x < 256u; ++x) carriedSprite = carriedSprite || checker.timelines.front().spriteBuffers[256u + x] != kNoSprite;
    EXPECT_TRUE(carriedSprite);
    EXPECT_EQ(checker.timelines.front().lines.front().spriteSource(), PpuTimeline::SpriteSource::Carried);
    EXPECT_TRUE(drawsAlike(checker, 1u));
  }
}

TEST_F(GpuCase, TheNarrowFrameIsDrawnAtItsWidth) {
  {
    const Checker checker = run(baseScene(), 4u);
    EXPECT_TRUE(drawsAlike(checker, 3u));
    EXPECT_EQ(device_.lastFrame().width, 256u);
    // The columns past the narrow frame's hold what the clear left: black.
    Bytes wide;
    ASSERT_TRUE(device_.readBack(512u, 224u, wide));
    for (unsigned row = 0u; row < 224u; ++row) {
      for (unsigned x = 256u; x < 512u; ++x) {
        const std::size_t at = (static_cast<std::size_t>(row) * 512u + x) * 4u;
        ASSERT_EQ(wide[at] | wide[at + 1u] | wide[at + 2u], 0u) << "row " << row << " column " << x;
      }
    }
  }
  {
    const Checker checker = run(modeScene(5u), 4u);
    EXPECT_TRUE(drawsAlike(checker, 3u));
    EXPECT_EQ(device_.lastFrame().width, 512u);
  }
}

TEST_F(GpuCase, ARunSettlesItsBuffers) {
  EXPECT_TRUE(drawsAlike(run(baseScene(), 8u), 7u));
  const unsigned after = device_.growths();
  EXPECT_GE(after, 1u);
  EXPECT_TRUE(drawsAlike(run(baseScene(), 8u), 7u));
  EXPECT_EQ(device_.growths(), after);
}

}  // namespace
}  // namespace snaggletooth
