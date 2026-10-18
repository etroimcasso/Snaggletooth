// The picture thread: a machine built with SnesConfig::ppuThread on draws the
// picture on a thread of its own, and one built with it off, the default, draws it
// inside the console's cycles. The cases hold the two equal — the whole state, every
// audio frame and every picture frame — at the return of every call, over a
// cartridge that keeps the chip busy: two backgrounds and eight sprites in Mode 1,
// a palette entry rewritten by HDMA on every line, a scroll register written in the
// picture every frame, and color math switched every few frames. Variants of it make
// the write to $2100 the chip reads a dot early, turn a frame wide, read the VRAM
// port back before writing it, and flood the thread with more accesses than its ring
// holds. They pin the faces that meet the thread: a snapshot and a restore, the move,
// the reset line, a report that throws and what a report reads, a machine nobody
// watches, a guest call and a memory write between calls, both threads on at once,
// the thread's park inside a report and its wake, the console sleeping while it
// waits, and the thread's end with the machine.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <sys/resource.h>
#endif

#include "gtest/gtest.h"
#include "snaggletooth/snes/snes.h"

namespace snaggletooth {
namespace {

// One NTSC frame in master cycles, and a quarter of one.
constexpr std::uint64_t kFrame = 357366u;
constexpr std::uint64_t kQuarterFrame = kFrame / 4u;

// Where the scene cartridge keeps its pieces in bank $00, and the work-RAM byte its
// NMI handler counts frames in.
constexpr std::uint16_t kBgTiles = 0x8400u;      // the program at $8000 runs to here; four 4bpp tiles, 128 bytes
constexpr std::uint16_t kSpriteTiles = 0x8480u;  // four more, for the sprites
constexpr std::uint16_t kBg1Map = 0x8800u;       // one 32x32 screen, 2048 bytes
constexpr std::uint16_t kBg2Map = 0x9000u;
constexpr std::uint16_t kPalette = 0x9800u;      // 256 words
constexpr std::uint16_t kOamTable = 0x9A00u;     // 544 bytes
constexpr std::uint16_t kHdmaTable = 0x9D00u;    // 901 bytes
constexpr std::uint16_t kNmiHandler = 0xA200u;
constexpr std::uint16_t kRoutine = 0xA300u;      // the handler runs to here
constexpr std::uint8_t kFrameCounter = 0x10u;

// The video memory layout the scene builds: the backgrounds' characters at byte 0,
// the sprites' at byte $4000 (name base 1), BG1's map at byte $1000 (base 2) and
// BG2's at byte $1800 (base 3).

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

// The channels' registers for the burst: four channels of 65536 bytes each from bank
// $00 to BG1's horizontal scroll register, then all four started with one write.
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

// Waits for vertical blank to begin, or to end: LDA $4212 ; BPL / BMI back.
Bytes awaitVblank() { return {0xADu, 0x12u, 0x42u, 0x10u, 0xFBu}; }
Bytes awaitPicture() { return {0xADu, 0x12u, 0x42u, 0x30u, 0xFBu}; }

// A 4bpp character from eight rows of eight color indices: planes 0 and 1 in the
// low and high bytes of eight words, planes 2 and 3 the same sixteen bytes on, the
// leftmost pixel in bit 7.
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

// 256 palette words, none of them zero but the backdrop's, which the HDMA rewrites.
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

// The HDMA table: for each of 224 lines, the palette address 0 twice and the line's
// backdrop color, under transfer pattern 3 ($2121, $2121, $2122, $2122).
Bytes hdmaTable() {
  Bytes out;
  const auto lines = [&out](unsigned from, unsigned count) {
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

// What the scene's handler does every frame beyond the count: the parts a variant
// adds in vertical blank, after the color math switch, and in the picture, after
// the scroll write.
struct Variant {
  Bytes inBlank;
  Bytes inPicture;
};

// The scene's NMI handler: the flag read, the frame counted, HDMA kept on, color
// math on every layer with halving for 32 frames in 64, BG2 under 4x4 mosaic
// blocks on every other frame, the variant's blank-time part, a wait for the
// picture to begin and a line more, BG1's scroll written from the counter in the
// picture, the variant's picture-time part.
Bytes handler(const Variant& variant) {
  return joined({{0x48u, 0xDAu},                                 // PHA ; PHX
                 {0xADu, 0x10u, 0x42u},                          // LDA $4210
                 {0xE6u, kFrameCounter},                         // INC $10
                 store(0x420Cu, 0x02u),                          // HDMAEN = channel 1
                 {0xA5u, kFrameCounter, 0x29u, 0x20u, 0xF0u, 0x04u},  // LDA $10 ; AND #$20 ; BEQ +4
                 {0xA9u, 0x3Fu, 0x80u, 0x02u},                   // LDA #$3F ; BRA +2
                 {0xA9u, 0x00u},                                 // LDA #$00
                 {0x8Du, 0x31u, 0x21u},                          // STA $2131
                 {0xA5u, kFrameCounter, 0x29u, 0x01u, 0xF0u, 0x04u},  // LDA $10 ; AND #$01 ; BEQ +4
                 {0xA9u, 0x32u, 0x80u, 0x02u},                   // LDA #$32 ; BRA +2
                 {0xA9u, 0x00u},                                 // LDA #$00
                 {0x8Du, 0x06u, 0x21u},                          // STA $2106
                 variant.inBlank,
                 awaitPicture(),
                 {0xA2u, 0x30u, 0xCAu, 0xD0u, 0xFDu},            // LDX #$30 ; DEX ; BNE
                 {0xA5u, kFrameCounter, 0x8Du, 0x0Du, 0x21u},    // LDA $10 ; STA $210D
                 clear(0x210Du),                                 // STZ $210D
                 variant.inPicture,
                 {0xFAu, 0x68u, 0x40u}});                        // PLX ; PLA ; RTI
}

// The scene's setup under forced blank: the characters, the two maps, the palette
// and the sprite table loaded by DMA, Mode 1 with both backgrounds and the sprites
// on the main screen, the HDMA channel pointed at its table.
Bytes setup() {
  return joined({store(0x2100u, 0x80u),
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
                 store(0x4312u, static_cast<std::uint8_t>(kHdmaTable & 0xFFu)),
                 store(0x4313u, static_cast<std::uint8_t>(kHdmaTable >> 8)), clear(0x4314u),
                 store(0x420Cu, 0x02u)});
}

// The scene lit and running: forced blank released at the next vertical blank, NMI
// on, and the main loop a branch to itself.
Bytes go() {
  return joined({awaitVblank(), store(0x2100u, 0x0Fu), store(0x4200u, 0x80u), {0x80u, 0xFEu}});
}

// A one-bank LoROM image: `program` at $8000, the data at its places, the handler at
// $A200, the routine at $A300, and the two vectors.
Bytes image(const Bytes& program, const Variant& variant) {
  Bytes rom(0x8000u, 0x00u);
  const auto place = [&rom](std::uint16_t address, const Bytes& bytes) {
    std::copy(bytes.begin(), bytes.end(), rom.begin() + (address - 0x8000u));
  };
  const Bytes nmi = handler(variant);
  if (program.size() > kBgTiles - 0x8000u) throw std::length_error("the program runs into the data");
  if (nmi.size() > kRoutine - kNmiHandler) throw std::length_error("the handler runs into the routine");
  place(0x8000u, program);
  place(kNmiHandler, nmi);
  // LDA #$1F ; STA $2122 ; LDA #$7C ; STA $2122 ; RTS
  place(kRoutine, joined({store(0x2122u, 0x1Fu), store(0x2122u, 0x7Cu), {0x60u}}));
  place(kBgTiles, characterSet(0u));
  place(kSpriteTiles, characterSet(1u));
  place(kBg1Map, tilemap(0u));
  place(kBg2Map, tilemap(1u));
  place(kPalette, palette());
  place(kOamTable, oamTable());
  place(kHdmaTable, hdmaTable());
  rom[0x7FFAu] = static_cast<std::uint8_t>(kNmiHandler & 0xFFu);  // emulation-mode NMI
  rom[0x7FFBu] = static_cast<std::uint8_t>(kNmiHandler >> 8);
  rom[0x7FFCu] = 0x00u;  // reset -> $8000
  rom[0x7FFDu] = 0x80u;
  return rom;
}

// The scene: SEI, the setup, and the run.
Bytes scene(const Variant& variant = {}) { return image(joined({{0x78u}, setup(), go()}), variant); }

// The scene with a write to $2100 in the picture every frame, made through a long
// store whose bank byte $8F is on the bus ahead of the value, so the chip's early
// read of the register draws one dot forced black.
Bytes earlyRedraw() {
  return scene(Variant{.inBlank = {}, .inPicture = {0xA9u, 0x0Fu, 0x8Fu, 0x00u, 0x21u, 0x8Fu}});
}

// The scene in mode 5 for one frame in every sixteen.
Bytes widening() {
  return scene(Variant{.inBlank = {0xA5u, kFrameCounter, 0x29u, 0x0Fu, 0xD0u, 0x04u,  // LDA $10 ; AND #$0F ; BNE +4
                                   0xA9u, 0x05u, 0x80u, 0x02u,                          // LDA #$05 ; BRA +2
                                   0xA9u, 0x01u,                                        // LDA #$01
                                   0x8Du, 0x05u, 0x21u},                                // STA $2105
                       .inPicture = {}});
}

// The scene with, in every vertical blank, the VRAM address set to the solid
// character's first word, four reads of the port's two bytes, which step it, and
// sixteen writes of the frame counter through the port, which land where the reads
// left the address.
Bytes readBack() {
  Bytes blank = joined({store(0x2115u, 0x80u), store(0x2116u, 0x10u), clear(0x2117u)});
  for (unsigned pair = 0u; pair < 4u; ++pair) blank = joined({blank, {0xADu, 0x39u, 0x21u, 0xADu, 0x3Au, 0x21u}});
  blank = joined({blank, {0xA5u, kFrameCounter}});
  for (unsigned word = 0u; word < 8u; ++word) blank = joined({blank, {0x8Du, 0x18u, 0x21u, 0x8Du, 0x19u, 0x21u}});
  return scene(Variant{.inBlank = blank, .inPicture = {}});
}

// The scene, lit, then a burst: four channels write 65536 bytes each to BG1's
// horizontal scroll register while the picture is drawn, every one an access the
// thread is handed and a decision it makes between two of them — then forced blank
// and the scene loaded again.
Bytes burst() {
  return image(joined({{0x78u}, setup(), awaitVblank(), store(0x2100u, 0x0Fu), awaitPicture(),
                       burstDma(),
                       setup(), go()}),
               {});
}

// The scene in mode 5 with the CPU halted once the screen is on: the console's
// cycles cost it almost nothing while every line the thread draws is in
// half-pixels, so the thread falls behind the beam by more lines than the line ring
// holds.
Bytes haltedHires() {
  return image(joined({{0x78u}, setup(), store(0x2105u, 0x05u), awaitVblank(), store(0x2100u, 0x0Fu),
                       {0xDBu}}),
               {});
}

SnesConfig config(const Bytes& rom, bool pictureThread, bool audioThread = false) {
  return SnesConfig{
      .rom = rom,
      .region = Region::Ntsc,
      .iplStub = true,
      .map = std::nullopt,
      .saveRamBytes = std::nullopt,
      .bootRom = std::nullopt,
      .apuThread = audioThread,
      .ppuThread = pictureThread,
  };
}

// A finished frame, kept whole.
struct Frame {
  unsigned width = 0;
  unsigned height = 0;
  std::uint8_t field = 0;
  Bytes pixels;
  [[nodiscard]] bool operator==(const Frame&) const = default;
};

// Every frame handed over since the last take.
struct Pictures final : FrameObserver {
  std::vector<Frame> frames;
  void frame(const VideoFrame& picture) override {
    frames.push_back(Frame{.width = picture.width,
                           .height = picture.height,
                           .field = picture.field,
                           .pixels = Bytes(picture.pixels.begin(), picture.pixels.end())});
  }
  [[nodiscard]] std::vector<Frame> take() {
    std::vector<Frame> taken = std::move(frames);
    frames.clear();
    return taken;
  }
};

// The whole state, every audio frame and every picture frame since the last look,
// equal.
::testing::AssertionResult equal(Snes& lockstep, Snes& threaded, Pictures& lockstepFrames,
                                 Pictures& threadedFrames) {
  if (!(lockstep.state() == threaded.state())) {
    return ::testing::AssertionFailure()
           << "the states differ: master " << lockstep.state().master << " and "
           << threaded.state().master;
  }
  if (lockstep.takeFrames() != threaded.takeFrames()) {
    return ::testing::AssertionFailure() << "the audio frames differ";
  }
  const std::vector<Frame> a = lockstepFrames.take();
  const std::vector<Frame> b = threadedFrames.take();
  if (a.size() != b.size()) {
    return ::testing::AssertionFailure() << "the lockstep machine handed over " << a.size()
                                         << " frames and the threaded one " << b.size();
  }
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (!(a[i] == b[i])) {
      return ::testing::AssertionFailure()
             << "picture frame " << i << " of " << a.size() << " differs: " << a[i].width << "x"
             << a[i].height << " against " << b[i].width << "x" << b[i].height;
    }
  }
  return ::testing::AssertionSuccess();
}

// The same cartridge on a machine with the thread off and one with it on, each
// handing its frames to a recorder of its own.
struct Pair {
  explicit Pair(const Bytes& rom, bool watch = true, bool audioThread = false)
      : lockstep(config(rom, false, audioThread)), threaded(config(rom, true, audioThread)) {
    if (watch) {
      lockstep.setFrameObserver(&lockstepFrames);
      threaded.setFrameObserver(&threadedFrames);
    }
  }
  Pair(const Pair&) = delete;
  Pair& operator=(const Pair&) = delete;
  Snes lockstep;
  Snes threaded;
  Pictures lockstepFrames;
  Pictures threadedFrames;
  [[nodiscard]] ::testing::AssertionResult same() {
    return equal(lockstep, threaded, lockstepFrames, threadedFrames);
  }
  void run(std::uint64_t budget) {
    lockstep.run(budget);
    threaded.run(budget);
  }
};

// Whether a frame holds a pixel that is not black.
bool lit(const Frame& frame) {
  for (std::size_t at = 0; at + 3u < frame.pixels.size(); at += 4u) {
    if (frame.pixels[at] != 0u || frame.pixels[at + 1u] != 0u || frame.pixels[at + 2u] != 0u) return true;
  }
  return false;
}

TEST(SnesPpuThread, AThreadedMachineAndALockstepMachineAreEqualAtEveryRunsReturn) {
  Pair t(scene());
  for (int call = 0; call < 120; ++call) {
    t.run(kQuarterFrame);
    ASSERT_TRUE(t.same()) << "after run " << call;
  }
  EXPECT_GE(t.threaded.state().wram[kFrameCounter], 25u) << "the handler counted the frames";
}

TEST(SnesPpuThread, AThreadedMachineAndALockstepMachineAreEqualAtEveryStepsReturn) {
  Pair t(scene());
  for (int call = 0; call < 20000; ++call) {
    t.lockstep.step();
    t.threaded.step();
    ASSERT_TRUE(t.same()) << "after step " << call;
  }
  t.run(kQuarterFrame);
  ASSERT_TRUE(t.same()) << "after the run";
  for (int call = 0; call < 2000; ++call) {
    t.lockstep.step();
    t.threaded.step();
    ASSERT_TRUE(t.same()) << "after step " << call << " past the run";
  }
}

TEST(SnesPpuThread, EveryFrameIsThePixelsTheLockstepMachineDrew) {
  Pair t(scene());
  unsigned frames = 0;
  unsigned litFrames = 0;
  for (int call = 0; call < 60; ++call) {
    t.lockstep.run(kFrame);
    t.threaded.run(kFrame);
    const std::vector<Frame> a = t.lockstepFrames.take();
    const std::vector<Frame> b = t.threadedFrames.take();
    ASSERT_EQ(a, b) << "after frame run " << call;
    ASSERT_TRUE(t.lockstep.state() == t.threaded.state()) << "after frame run " << call;
    for (const Frame& frame : a) {
      ++frames;
      if (lit(frame)) ++litFrames;
    }
  }
  EXPECT_GE(frames, 59u);
  EXPECT_GE(litFrames, 50u) << "the scene is drawn";
}

TEST(SnesPpuThread, AWriteToInidispMidLineRedrawsTheDotOnBothMachines) {
  Pair t(earlyRedraw());
  for (int call = 0; call < 20; ++call) {
    t.run(kFrame);
    ASSERT_TRUE(t.same()) << "after frame " << call;
  }
}

TEST(SnesPpuThread, AFrameDrawnInHalfPixelsIsWidenedOnBothMachines) {
  Pair t(widening());
  bool wide = false;
  bool narrow = false;
  for (int call = 0; call < 40; ++call) {
    t.lockstep.run(kFrame);
    t.threaded.run(kFrame);
    const std::vector<Frame> a = t.lockstepFrames.take();
    const std::vector<Frame> b = t.threadedFrames.take();
    ASSERT_EQ(a, b) << "after frame " << call;
    ASSERT_TRUE(t.lockstep.state() == t.threaded.state()) << "after frame " << call;
    for (const Frame& frame : b) {
      if (frame.width == 512u) wide = true;
      if (frame.width == 256u) narrow = true;
    }
  }
  EXPECT_TRUE(wide);
  EXPECT_TRUE(narrow);
}

TEST(SnesPpuThread, AVramReadBackStepsTheAddressOnBothMachines) {
  Pair t(readBack());
  for (int call = 0; call < 20; ++call) {
    t.run(kFrame);
    ASSERT_TRUE(t.same()) << "after frame " << call;
  }
  // The writes landed past the reads: the solid character's later rows hold the
  // counter, its first rows the character as loaded.
  const SnesState& s = t.threaded.state();
  EXPECT_EQ(s.ppu.vram[0x20], 0xFFu) << "row 0 of the solid character, plane 0, as loaded";
  EXPECT_EQ(s.ppu.vram[0x28], s.wram[kFrameCounter]) << "row 4, written through the port";
}

TEST(SnesPpuThread, AMachineNobodyWatchesKeepsTheTileReadsAndTheDecision) {
  Pair t(scene(), /*watch=*/false);
  for (int call = 0; call < 120; ++call) {
    t.run(kQuarterFrame);
    ASSERT_TRUE(t.same()) << "after run " << call;
  }
  EXPECT_TRUE(t.threaded.state().ppu.lastMain.present);
}

TEST(SnesPpuThread, ASnapshotOfAThreadedMachineRestoresIntoALockstepOneAndBack) {
  Pair t(scene());
  for (int call = 0; call < 10; ++call) t.run(kQuarterFrame);
  t.run(1001u);  // mid-instruction, mid-line
  ASSERT_TRUE(t.same());
  const SnesState fromThreaded = t.threaded.state();
  const SnesState fromLockstep = t.lockstep.state();
  for (int call = 0; call < 7; ++call) t.run(kQuarterFrame);
  ASSERT_TRUE(t.same());
  t.lockstep.restore(fromThreaded);
  t.threaded.restore(fromLockstep);
  ASSERT_TRUE(t.same()) << "after the swap";
  for (int call = 0; call < 20; ++call) {
    t.run(kQuarterFrame);
    ASSERT_TRUE(t.same()) << "after run " << call << " past the restore";
  }
}

TEST(SnesPpuThread, ARestoreAndAMemoryWriteBeforeTheFirstCallDoNotWaitForAThreadNotYetStarted) {
  // A host that restores a save into a machine before it runs, as the player does,
  // and one that writes a memory first: neither call has a thread to wait for, and
  // the machine runs on equal to the lockstep one.
  Pair t(scene());
  SnesState state = t.lockstep.state();
  state.wram[0x20] = 0x5Au;
  t.lockstep.restore(state);
  t.threaded.restore(state);
  t.lockstep.writeVram(0x0020u, 0x3Cu);
  t.threaded.writeVram(0x0020u, 0x3Cu);
  t.lockstep.reset();
  t.threaded.reset();
  ASSERT_TRUE(t.same()) << "before the first call";
  for (int call = 0; call < 8; ++call) {
    t.run(kFrame);
    ASSERT_TRUE(t.same()) << "after frame " << call;
  }
  Snes moved(std::move(t.threaded));
  moved.restore(state);
  t.lockstep.restore(state);
  ASSERT_TRUE(equal(t.lockstep, moved, t.lockstepFrames, t.threadedFrames)) << "after the move's restore";
  t.lockstep.run(kFrame);
  moved.run(kFrame);
  EXPECT_TRUE(equal(t.lockstep, moved, t.lockstepFrames, t.threadedFrames));
}

TEST(SnesPpuThread, AMovedThreadedMachineRunsOn) {
  Pair t(scene());
  for (int call = 0; call < 10; ++call) t.run(kQuarterFrame);
  t.run(777u);
  Snes moved(std::move(t.threaded));
  ASSERT_TRUE(equal(t.lockstep, moved, t.lockstepFrames, t.threadedFrames)) << "after the move";
  for (int call = 0; call < 20; ++call) {
    t.lockstep.run(kQuarterFrame);
    moved.run(kQuarterFrame);
    ASSERT_TRUE(equal(t.lockstep, moved, t.lockstepFrames, t.threadedFrames))
        << "after run " << call << " past the move";
  }
}

TEST(SnesPpuThread, AResetMidRunLeavesBothMachinesEqual) {
  Pair t(scene());
  for (int call = 0; call < 10; ++call) t.run(kQuarterFrame);
  t.run(555u);
  t.lockstep.reset();
  t.threaded.reset();
  ASSERT_TRUE(t.same()) << "after the reset";
  for (int call = 0; call < 20; ++call) {
    t.run(kQuarterFrame);
    ASSERT_TRUE(t.same()) << "after run " << call << " past the reset";
  }
}

// What a frame report reads of the picture's two fields of the state, frame by frame,
// folded into one value.
struct TilesAtReport final : FrameObserver {
  Snes* machine = nullptr;
  std::vector<std::uint64_t> digests;
  void frame(const VideoFrame&) override {
    std::uint64_t value = 1469598103934665603ull;
    const auto fold = [&value](std::uint64_t word) { value = (value ^ word) * 1099511628211ull; };
    for (const auto& layer : machine->state().ppu.tiles) {
      for (const TileRead& read : layer) {
        fold(read.entry);
        fold((std::uint64_t{read.bgX} << 16) | read.bgY);
        fold((std::uint64_t{read.planes} << 8) | read.fine);
        for (const std::uint8_t pixel : read.pixel) fold(pixel);
      }
    }
    const PpuState::MainDecision& main = machine->state().ppu.lastMain;
    fold((std::uint64_t{main.colour} << 8) | (main.present ? 1u : 0u) | (main.clipped ? 2u : 0u) |
         (main.addend << 2) | (main.subtract ? 16u : 0u) | (main.halved ? 32u : 0u));
    digests.push_back(value);
  }
};

TEST(SnesPpuThread, AFrameReportReadsTheWholeStateAtTheReportsCycle) {
  Pair t(scene(), /*watch=*/false);
  TilesAtReport onLockstep;
  TilesAtReport onThreaded;
  onLockstep.machine = &t.lockstep;
  onThreaded.machine = &t.threaded;
  t.lockstep.setFrameObserver(&onLockstep);
  t.threaded.setFrameObserver(&onThreaded);
  t.run(12u * kFrame);
  EXPECT_TRUE(t.same());
  ASSERT_GE(onLockstep.digests.size(), 11u);
  EXPECT_EQ(onThreaded.digests, onLockstep.digests);
}

struct Thrown {};

// A frame observer that throws on the second frame it is told.
struct ThrowsOnSecond final : FrameObserver {
  int frames = 0;
  void frame(const VideoFrame&) override {
    if (++frames == 2) throw Thrown{};
  }
};

TEST(SnesPpuThread, AReportThatThrowsLeavesBothMachinesAtOneTime) {
  Pair t(scene(), /*watch=*/false);
  ThrowsOnSecond onLockstep;
  ThrowsOnSecond onThreaded;
  t.lockstep.setFrameObserver(&onLockstep);
  t.threaded.setFrameObserver(&onThreaded);
  EXPECT_THROW(t.lockstep.run(3u * kFrame), Thrown);
  EXPECT_THROW(t.threaded.run(3u * kFrame), Thrown);
  ASSERT_TRUE(t.same()) << "where the throw left them";
  t.run(kFrame);
  EXPECT_TRUE(t.same()) << "after the run that finished the budget";
}

TEST(SnesPpuThread, TheThreadIsOffByDefault) {
  const Bytes rom = scene();
  SnesConfig defaults{};
  EXPECT_FALSE(defaults.ppuThread);
  defaults.rom = rom;
  Snes machine(defaults);
  Pictures pictures;
  machine.setFrameObserver(&pictures);
  machine.run(4u * kFrame);
  const std::vector<Frame> frames = pictures.take();
  ASSERT_GE(frames.size(), 3u);
  EXPECT_TRUE(lit(frames.back())) << "the default machine draws the scene";
}

TEST(SnesPpuThread, TheMachineStopsItsThreadWhenItGoes) {
  const Bytes rom = scene();
  {
    Snes never(config(rom, true));  // no call: no thread to stop
  }
  {
    Snes spinning(config(rom, true));
    spinning.run(kFrame);  // destroyed while its thread is awake after the call
  }
  {
    Snes parked(config(rom, true));
    parked.run(kFrame);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));  // past the awake window
    parked.run(kQuarterFrame);  // a call wakes it
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }  // destroyed while its thread is parked
  SUCCEED() << "every machine's thread ended with it";
}

TEST(SnesPpuThread, TheEventRingNeverLosesAnEntryUnderBackpressure) {
  // Four channels of 65536 accesses each in one call on a machine nobody watches,
  // so no frame report waits for the thread inside the call: every access is a
  // decision the thread makes, the thread falls behind by more entries than the
  // ring holds, and the console waits on it. A scroll write lost would leave the
  // view's scroll latch, and so the tile reads copied back, differing.
  Pair t(burst(), /*watch=*/false);
  t.run(16u * kFrame);
  ASSERT_TRUE(t.same()) << "after the burst";
  for (int call = 0; call < 4; ++call) {
    t.run(kFrame);
    ASSERT_TRUE(t.same()) << "after frame " << call << " past the burst";
  }
}

TEST(SnesPpuThread, TheLineRingNeverLosesALineUnderBackpressure) {
  // Ten frames of lines in one call, more than the line ring holds, the console's
  // cycles nearly free and every line drawn in half-pixels. A watched machine waits
  // for the thread at every frame report, so its lag never reaches the ring's size;
  // the case holds the ring over more lines than it has slots.
  Pair t(haltedHires());
  t.run(10u * kFrame);
  const std::vector<Frame> a = t.lockstepFrames.take();
  const std::vector<Frame> b = t.threadedFrames.take();
  ASSERT_EQ(a, b);
  ASSERT_GE(b.size(), 9u);
  EXPECT_EQ(b.back().width, 512u);
  ASSERT_TRUE(t.same());
  t.run(kFrame);
  EXPECT_TRUE(t.same());
}

TEST(SnesPpuThread, BothThreadsOnAreEqualToTheLockstepMachine) {
  Pair t(scene(), /*watch=*/true, /*audioThread=*/true);
  for (int call = 0; call < 60; ++call) {
    t.run(kFrame);
    ASSERT_TRUE(t.same()) << "after frame " << call;
  }
}

// A frame observer that sleeps inside each of the first eight reports it is told,
// as a host holding a run to the console's rate does.
struct SleepsInReport final : FrameObserver {
  int frames = 0;
  std::chrono::milliseconds sleep{20};
  void frame(const VideoFrame&) override {
    if (++frames <= 8) std::this_thread::sleep_for(sleep);
  }
};

TEST(SnesPpuThread, AThreadParkedInsideAReportIsWokenWhenTheConsoleRunsOn) {
  // Each sleep is 200 times the thread's awake window, so the thread parks inside
  // the report with the call still in progress; the console's next entries must
  // wake it.
  Pair t(scene(), /*watch=*/false);
  SleepsInReport onLockstep;
  SleepsInReport onThreaded;
  t.lockstep.setFrameObserver(&onLockstep);
  t.threaded.setFrameObserver(&onThreaded);
  for (int call = 0; call < 12; ++call) {
    t.run(kFrame);
    ASSERT_TRUE(t.same()) << "after run " << call;
  }
  EXPECT_GE(onThreaded.frames, 11);
}

TEST(SnesPpuThread, AGuestCallBetweenCallsJoinsThePictureThread) {
  Pair t(scene());
  t.run(4u * kFrame);
  ASSERT_TRUE(t.same());
  std::this_thread::sleep_for(std::chrono::milliseconds(50));  // past the awake window
  EXPECT_TRUE(t.lockstep.callInContext(kRoutine, Standin::Near, 100));
  EXPECT_TRUE(t.threaded.callInContext(kRoutine, Standin::Near, 100));
  ASSERT_TRUE(t.same()) << "after the call";
  t.run(2u * kFrame);
  EXPECT_TRUE(t.same()) << "after the frames that follow";
}

TEST(SnesPpuThread, AHostWriteToVramBetweenCallsShowsOnBothMachines) {
  Pair t(scene());
  t.run(4u * kFrame);
  ASSERT_TRUE(t.same());
  for (Snes* machine : {&t.lockstep, &t.threaded}) {
    for (std::uint16_t byte = 0x20u; byte < 0x40u; ++byte) machine->writeVram(byte, 0xA5u);
    machine->writeCgram(2u, 0xFFu);
    machine->writeCgram(3u, 0x03u);
    machine->writeOam(0u, 0x40u);
  }
  ASSERT_TRUE(t.same()) << "after the writes";
  t.run(2u * kFrame);
  EXPECT_TRUE(t.same()) << "after the frames that follow";
}

// Every thread of the process, user plus system, in seconds.
double processCpuSeconds() {
#if defined(_WIN32)
  FILETIME creation{};
  FILETIME exit{};
  FILETIME kernel{};
  FILETIME user{};
  if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) return 0.0;
  const auto ticks = [](const FILETIME& time) {
    return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
  };
  return static_cast<double>(ticks(kernel) + ticks(user)) / 1e7;
#else
  rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage) != 0) return 0.0;
  return static_cast<double>(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) +
         static_cast<double>(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1e6;
#endif
}

// The process's CPU and the wall time over `frames` whole-frame runs.
struct Stretch {
  double cpu = 0.0;
  double wall = 0.0;
};
Stretch stretch(Snes& machine, int frames) {
  const double cpuBefore = processCpuSeconds();
  const auto started = std::chrono::steady_clock::now();
  for (int call = 0; call < frames; ++call) machine.run(kFrame);
  return Stretch{.cpu = processCpuSeconds() - cpuBefore,
                 .wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count()};
}

TEST(SnesPpuThread, TheConsoleSleepsWhileItWaitsForThePicture) {
  // Eight frames run flat out, then eight with the report sleeping 100 ms in each:
  // the thread parks through the sleep and the console sleeps for the thread where
  // it waits, so the sleeping stretch adds wall time and next to no CPU. A thread
  // spinning through the sleeps would add a core's worth of CPU for the whole of it.
  // The flat-out stretch is the control, which holds whatever the host or a
  // sanitizer makes the emulation itself cost.
  const Bytes rom = scene();
  Snes machine(config(rom, true));
  SleepsInReport sleeps;
  sleeps.sleep = std::chrono::milliseconds(0);
  machine.setFrameObserver(&sleeps);
  machine.run(kFrame);  // the first frame, before either stretch
  const Stretch flatOut = stretch(machine, 8);
  sleeps.frames = 0;
  sleeps.sleep = std::chrono::milliseconds(100);
  const Stretch sleeping = stretch(machine, 8);
  EXPECT_GE(sleeps.frames, 8);
  EXPECT_GT(sleeping.wall - flatOut.wall, 0.7);
  EXPECT_LT(sleeping.cpu - flatOut.cpu, (sleeping.wall - flatOut.wall) / 2.0)
      << "flat out " << flatOut.cpu << " s cpu over " << flatOut.wall << " s; sleeping " << sleeping.cpu
      << " s cpu over " << sleeping.wall << " s";
}

// Where the machine stands at each frame report, the count the handler had reached,
// and the frame's shape.
struct ReportPoint final : FrameObserver {
  struct At {
    std::uint16_t vpos = 0;
    std::uint16_t hpos = 0;
    std::uint16_t vblankBeginLine = 0;
    bool inVblank = false;
    std::uint8_t counter = 0;
    unsigned width = 0;
    unsigned height = 0;
    [[nodiscard]] bool operator==(const At&) const = default;
  };
  const Snes* machine = nullptr;
  std::vector<At> reports;
  void frame(const VideoFrame& picture) override {
    const SnesState& s = machine->state();
    reports.push_back(At{.vpos = s.vpos,
                         .hpos = s.hpos,
                         .vblankBeginLine = s.vblankBeginLine,
                         .inVblank = s.inVblank,
                         .counter = s.wram[kFrameCounter],
                         .width = picture.width,
                         .height = picture.height});
  }
};

TEST(SnesPpuThread, BothMachinesReportAtTheBlanksCycle) {
  // Each frame is reported at the end of the cycle its vertical blank begins, on the
  // threaded machine as on the lockstep one: the same line, the same position on it.
  Pair t(scene(), /*watch=*/false);
  ReportPoint lockstep;
  ReportPoint threaded;
  lockstep.machine = &t.lockstep;
  threaded.machine = &t.threaded;
  t.lockstep.setFrameObserver(&lockstep);
  t.threaded.setFrameObserver(&threaded);
  for (int call = 0; call < 12; ++call) {
    t.run(kFrame);
    ASSERT_TRUE(t.lockstep.state() == t.threaded.state()) << "after frame " << call;
  }
  EXPECT_GE(lockstep.reports.size(), 11u);
  EXPECT_EQ(lockstep.reports, threaded.reports);
  for (const ReportPoint::At& at : threaded.reports) {
    EXPECT_TRUE(at.inVblank);
    EXPECT_EQ(at.vpos, at.vblankBeginLine);
    EXPECT_EQ(at.vpos, 225u);
    EXPECT_LE(at.hpos, 12u);
  }
}

TEST(SnesPpuThread, TheWidthIsSettledAtTheBlankOnEitherThread) {
  // The scene in mode 5 for one frame in sixteen and, from the blank that lights
  // it, under SETINI's taller picture: each machine settles a frame's width as its
  // blank begins, at line 240, so the report after the handler chose mode 5 at a
  // count of 16 is 512 wide and every other lit report 256, 239 lines tall.
  const Bytes tallWidening = scene(
      Variant{.inBlank = joined({{0xA5u, kFrameCounter, 0x29u, 0x0Fu, 0xD0u, 0x04u,  // LDA $10 ; AND #$0F ; BNE +4
                                  0xA9u, 0x05u, 0x80u, 0x02u,                          // LDA #$05 ; BRA +2
                                  0xA9u, 0x01u,                                        // LDA #$01
                                  0x8Du, 0x05u, 0x21u},                                // STA $2105
                                 store(0x2133u, 0x04u)}),
              .inPicture = {}});
  Pair t(tallWidening, /*watch=*/false);
  ReportPoint lockstep;
  ReportPoint threaded;
  lockstep.machine = &t.lockstep;
  threaded.machine = &t.threaded;
  t.lockstep.setFrameObserver(&lockstep);
  t.threaded.setFrameObserver(&threaded);
  for (int call = 0; call < 40; ++call) {
    t.run(kFrame);
    ASSERT_TRUE(t.lockstep.state() == t.threaded.state()) << "after frame " << call;
  }
  EXPECT_EQ(lockstep.reports, threaded.reports);
  unsigned wide = 0;
  unsigned lit = 0;
  for (const ReportPoint::At& at : threaded.reports) {
    if (at.counter == 0u) continue;  // before the blank that lights the scene
    ++lit;
    SCOPED_TRACE(::testing::Message() << "count " << unsigned{at.counter});
    EXPECT_EQ(at.vpos, 240u);
    EXPECT_EQ(at.height, 239u);
    EXPECT_EQ(at.width, at.counter % 16u == 0u ? 512u : 256u);
    if (at.width == 512u) ++wide;
  }
  EXPECT_GE(lit, 30u);
  EXPECT_EQ(wide, 2u) << "counts 16 and 32";
}

}  // namespace
}  // namespace snaggletooth
