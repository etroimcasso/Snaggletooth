// PpuTimeline: a line's register words read back, the frame drawn from the
// timeline alone through the chip's own dot code, and the frame packed into one
// buffer for a shader.

#include "snaggletooth/snes/ppu_timeline.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>

#include "ppu_timeline_words.h"

namespace snaggletooth {
namespace {

// A span's length in whole words, the last word padded.
[[nodiscard]] constexpr std::size_t wordsOf(std::size_t bytes) noexcept { return (bytes + 3u) / 4u; }

// A 32-bit word stored little-endian at a word index of the packed buffer.
void putWord(std::span<std::uint8_t> out, std::size_t word, std::uint32_t value) noexcept {
  const std::size_t at = word * 4u;
  out[at] = static_cast<std::uint8_t>(value & 0xFFu);
  out[at + 1u] = static_cast<std::uint8_t>((value >> 8) & 0xFFu);
  out[at + 2u] = static_cast<std::uint8_t>((value >> 16) & 0xFFu);
  out[at + 3u] = static_cast<std::uint8_t>(value >> 24);
}

[[nodiscard]] std::uint32_t getWord(std::span<const std::uint8_t> in, std::size_t word) noexcept {
  const std::size_t at = word * 4u;
  return static_cast<std::uint32_t>(in[at]) | (static_cast<std::uint32_t>(in[at + 1u]) << 8) |
         (static_cast<std::uint32_t>(in[at + 2u]) << 16) | (static_cast<std::uint32_t>(in[at + 3u]) << 24);
}

// A byte span copied to a word index, the padding of its last word zero.
void putBytes(std::span<std::uint8_t> out, std::size_t word, std::span<const std::uint8_t> bytes) noexcept {
  const std::size_t at = word * 4u;
  if (!bytes.empty()) std::memcpy(out.data() + at, bytes.data(), bytes.size());
  const std::size_t padded = wordsOf(bytes.size()) * 4u;
  if (padded > bytes.size()) std::memset(out.data() + at + bytes.size(), 0, padded - bytes.size());
}

}  // namespace

std::uint32_t PpuTimeline::Line::value(Field which) const noexcept {
  const auto index = static_cast<std::size_t>(which);
  if (index >= timeline::kRegisterFields) return 0u;
  const timeline::Place& place = timeline::kPlaces[index];
  const std::uint32_t mask = (1u << place.bits) - 1u;
  return (words[place.word] >> place.shift) & mask;
}

void PpuTimeline::draw(std::span<std::uint8_t> pixels) const {
  if (pixels.size() < kDrawBytes || lines.empty()) return;
  // One chip state for the whole frame: VRAM runs on through the frame's writes,
  // and each line takes its registers, its palette, its sprite table and its
  // sprites from its own records.
  const auto state = std::make_unique<PpuState>();
  PpuState& s = *state;
  Ppu::Derived derived{};
  Ppu ppu{s, derived};
  std::copy(vram.begin(), vram.begin() + static_cast<std::ptrdiff_t>(std::min(vram.size(), kVramBytes)),
            s.vram.begin());
  // A change applied to the state as the write that made it applied it, with what
  // the chip works out from the registers dropped where that write drops it.
  const auto apply = [&](const Event& event) {
    const Field changed = event.field();
    if (changed == Field::PaletteWord) {
      const std::size_t at = static_cast<std::size_t>(event.paletteIndex()) * 2u;
      s.cgram[at] = static_cast<std::uint8_t>(event.value() & 0xFFu);
      s.cgram[at + 1u] = static_cast<std::uint8_t>(event.value() >> 8);
      return;
    }
    timeline::setValue(s, changed, event.value());
    if (timeline::dropsDescriptions(changed)) derived.dropDescriptions();
    if (timeline::dropsWindows(changed)) derived.dropWindows();
  };
  std::size_t written = 0u;
  // The VRAM writes placed at or before the dot `dot` of `line`, applied in order.
  const auto writeVram = [&](std::uint32_t line, std::uint32_t dot) {
    while (written < vramWrites.size()) {
      const VramWrite& write = vramWrites[written];
      if (write.line() > line || (write.line() == line && write.dot() > dot)) break;
      s.vram[write.address()] = static_cast<std::uint8_t>(write.value());
      ++written;
    }
  };

  for (const Line& line : lines) {
    const std::uint32_t number = line.lineNumber();
    const PpuInputs in{.hdot = 0u,
                       .vpos = static_cast<std::uint16_t>(number),
                       .field = static_cast<std::uint8_t>(line.field()),
                       .vblank = false,
                       .pal = false,
                       .extLatch = true,
                       .lateHalf = false};

    // The sprites, as the line began: every VRAM write before the line's start
    // landed, then Time run over the line's own inputs, or the buffer it was left.
    writeVram(number - 1u, 0xFFFFu);
    switch (line.spriteSource()) {
      case SpriteSource::Gathered: {
        const std::size_t oamAt = static_cast<std::size_t>(line.oamVersion()) * kOamBytes;
        if (oamAt + kOamBytes <= oams.size()) {
          std::copy(oams.begin() + static_cast<std::ptrdiff_t>(oamAt),
                    oams.begin() + static_cast<std::ptrdiff_t>(oamAt + kOamBytes), s.oam.begin());
        }
        s.inidisp = 0u;  // Time gathered, so the screen was on as the line began
        s.objsel = static_cast<std::uint8_t>(line.spriteObjsel());
        s.setini = static_cast<std::uint8_t>(line.spriteSetini());
        s.sprites.found = static_cast<std::uint8_t>(line.spriteFound());
        s.sprites.first = static_cast<std::uint8_t>(line.spriteFirst());
        for (unsigned nth = 0u; nth < kSpritesPerLine; ++nth) {
          s.sprites.inRange[nth] = static_cast<std::uint8_t>(line.inRange(nth));
        }
        ppu.timeSprites(static_cast<std::uint16_t>(number), in.field);
        break;
      }
      case SpriteSource::None:
        s.sprites.line = 0u;  // no picture line's
        break;
      case SpriteSource::Carried: {
        const std::size_t bufferAt = static_cast<std::size_t>(line.spriteBuffer()) * kSpriteBufferBytes;
        if (bufferAt + kSpriteBufferBytes <= spriteBuffers.size()) {
          const auto from = spriteBuffers.begin() + static_cast<std::ptrdiff_t>(bufferAt);
          std::copy(from, from + kPictureWidth, s.sprites.word.begin());
          std::copy(from + kPictureWidth, from + 2 * kPictureWidth, s.sprites.priority.begin());
        }
        s.sprites.line = static_cast<std::uint16_t>(number);
        break;
      }
    }

    // The registers and the palette the line's first read dot found.
    timeline::unpackRegisters(line.words, s);
    const std::size_t paletteAt = static_cast<std::size_t>(line.paletteVersion()) * kPaletteBytes;
    if (paletteAt + kPaletteBytes <= palettes.size()) {
      std::copy(palettes.begin() + static_cast<std::ptrdiff_t>(paletteAt),
                palettes.begin() + static_cast<std::ptrdiff_t>(paletteAt + kPaletteBytes),
                s.cgram.begin());
    }
    s.lastMain = PpuState::MainDecision{};
    derived.dropDescriptions();
    derived.dropWindows();

    // The line's dots, each under the events that reach it and the VRAM written up
    // to it: the tile reads that fall on the dot, then from kFirstPictureDot on the
    // position's pixel.
    std::size_t next = line.firstEvent();
    const std::size_t end = std::min<std::size_t>(next + line.eventCount(), events.size());
    std::uint8_t* const row = pixels.data() + static_cast<std::size_t>(number - 1u) * kHiresWidth * 4u;
    for (std::uint32_t dot = kFirstTileReadDot; dot <= kLastPictureDot; ++dot) {
      while (next < end && events[next].dot() <= dot) apply(events[next++]);
      writeVram(number, dot);
      ppu.tileReads(static_cast<std::uint16_t>(dot), static_cast<std::uint16_t>(dot), in);
      if (dot < kFirstPictureDot) continue;
      const auto x = static_cast<std::uint16_t>(dot - kFirstPictureDot);
      const Ppu::Dot out = ppu.dot(x, in);
      std::uint8_t* const pixel = row + static_cast<std::size_t>(x) * 2u * 4u;
      std::copy(out.left.begin(), out.left.end(), pixel);
      std::copy(out.right.begin(), out.right.end(), pixel + 4);
    }
  }
}

std::size_t PpuTimeline::packedSize() const noexcept {
  const std::size_t words = kHeaderWords + lines.size() * 32u + events.size() * 2u + kVramBytes / 4u +
                            wordsOf(palettes.size()) + wordsOf(oams.size()) + wordsOf(spriteBuffers.size()) +
                            vramWrites.size() * 2u * 2u + kPageTableWords + kLineTableWords +
                            2u * kEventLineTableWords;
  return words * 4u;
}

std::size_t PpuTimeline::pack(std::span<std::uint8_t> out) const noexcept {
  const std::size_t size = packedSize();
  if (out.size() < size) return 0u;
  out = out.first(size);

  // The events' line tables: entry L + 1 counts a kind's events on line L, then each
  // entry becomes the count on the lines below it, entry 240 the kind's whole count.
  std::array<std::uint32_t, kEventLineTableWords> registerBelow{};
  std::array<std::uint32_t, kEventLineTableWords> paletteBelow{};
  const auto tableLine = [](const Event& event) {
    return std::min<std::size_t>(event.line(), kEventLineTableWords - 2u);
  };
  for (const Event& event : events) {
    std::array<std::uint32_t, kEventLineTableWords>& below =
        event.field() == Field::PaletteWord ? paletteBelow : registerBelow;
    ++below[tableLine(event) + 1u];
  }
  for (std::size_t line = 1u; line < kEventLineTableWords; ++line) {
    registerBelow[line] += registerBelow[line - 1u];
    paletteBelow[line] += paletteBelow[line - 1u];
  }
  const std::size_t registerCount = registerBelow[kEventLineTableWords - 1u];
  const std::size_t paletteCount = paletteBelow[kEventLineTableWords - 1u];

  // The sections, one after another behind the header.
  std::size_t next = kHeaderWords;
  const auto section = [&next](std::size_t words) {
    const std::size_t at = next;
    next += words;
    return at;
  };
  const std::size_t linesAt = section(lines.size() * 32u);
  const std::size_t registersAt = section(registerCount * 2u);
  const std::size_t paletteEventsAt = section(paletteCount * 2u);
  const std::size_t vramAt = section(kVramBytes / 4u);
  const std::size_t palettesAt = section(wordsOf(palettes.size()));
  const std::size_t oamsAt = section(wordsOf(oams.size()));
  const std::size_t buffersAt = section(wordsOf(spriteBuffers.size()));
  const std::size_t writesAt = section(vramWrites.size() * 2u);
  const std::size_t sortedAt = section(vramWrites.size() * 2u);
  const std::size_t pagesAt = section(kPageTableWords);
  const std::size_t lineTableAt = section(kLineTableWords);
  const std::size_t registerTableAt = section(kEventLineTableWords);
  const std::size_t paletteTableAt = section(kEventLineTableWords);

  std::memset(out.data(), 0, kHeaderWords * 4u);
  putWord(out, 0u, height);
  putWord(out, 1u, width);
  putWord(out, 2u, firstLine);
  putWord(out, 3u, field);
  putWord(out, 4u, static_cast<std::uint32_t>(linesAt));
  putWord(out, 5u, static_cast<std::uint32_t>(lines.size()));
  putWord(out, 6u, static_cast<std::uint32_t>(registersAt));
  putWord(out, 7u, static_cast<std::uint32_t>(registerCount));
  putWord(out, 8u, static_cast<std::uint32_t>(vramAt));
  putWord(out, 9u, static_cast<std::uint32_t>(palettesAt));
  putWord(out, 10u, static_cast<std::uint32_t>(palettes.size() / kPaletteBytes));
  putWord(out, 11u, static_cast<std::uint32_t>(oamsAt));
  putWord(out, 12u, static_cast<std::uint32_t>(oams.size() / kOamBytes));
  putWord(out, 13u, static_cast<std::uint32_t>(buffersAt));
  putWord(out, 14u, static_cast<std::uint32_t>(spriteBuffers.size() / kSpriteBufferBytes));
  putWord(out, 15u, static_cast<std::uint32_t>(writesAt));
  putWord(out, 16u, static_cast<std::uint32_t>(vramWrites.size()));
  putWord(out, 17u, static_cast<std::uint32_t>(sortedAt));
  putWord(out, 18u, static_cast<std::uint32_t>(pagesAt));
  putWord(out, 19u, static_cast<std::uint32_t>(lineTableAt));
  putWord(out, 20u, static_cast<std::uint32_t>(registerTableAt));
  putWord(out, 21u, static_cast<std::uint32_t>(paletteEventsAt));
  putWord(out, 22u, static_cast<std::uint32_t>(paletteCount));
  putWord(out, 23u, static_cast<std::uint32_t>(paletteTableAt));

  for (std::size_t i = 0u; i < lines.size(); ++i) {
    for (std::size_t word = 0u; word < 32u; ++word) putWord(out, linesAt + i * 32u + word, lines[i].words[word]);
  }

  // The register events in the recorded order, which is by line and then by dot.
  std::array<std::uint32_t, kEventLineTableWords> placed = registerBelow;
  for (const Event& event : events) {
    if (event.field() == Field::PaletteWord) continue;
    const std::size_t to = placed[tableLine(event)]++;
    putWord(out, registersAt + to * 2u, event.at);
    putWord(out, registersAt + to * 2u + 1u, event.change);
  }
  // The palette writes of each line, whose events stand together, by palette index
  // and then by dot: a stable counting sort on the index over the line's recorded
  // order.
  std::array<std::uint32_t, 257> starts{};
  for (std::size_t first = 0u; first < events.size();) {
    const std::uint32_t line = events[first].line();
    std::size_t end = first;
    while (end < events.size() && events[end].line() == line) ++end;
    starts.fill(0u);
    for (std::size_t i = first; i < end; ++i) {
      if (events[i].field() == Field::PaletteWord) ++starts[events[i].paletteIndex() + 1u];
    }
    for (std::size_t index = 0u; index < 256u; ++index) starts[index + 1u] += starts[index];
    const std::size_t base = paletteBelow[tableLine(events[first])];
    for (std::size_t i = first; i < end; ++i) {
      if (events[i].field() != Field::PaletteWord) continue;
      const std::size_t to = base + starts[events[i].paletteIndex()]++;
      putWord(out, paletteEventsAt + to * 2u, events[i].at);
      putWord(out, paletteEventsAt + to * 2u + 1u, events[i].change);
    }
    first = end;
  }
  putBytes(out, vramAt, vram.first(std::min(vram.size(), kVramBytes)));
  if (vram.size() < kVramBytes) std::memset(out.data() + vramAt * 4u + vram.size(), 0, kVramBytes - vram.size());
  putBytes(out, palettesAt, palettes);
  putBytes(out, oamsAt, oams);
  putBytes(out, buffersAt, spriteBuffers);

  // The writes sorted by address and then by ordinal: a stable counting sort on the
  // address's low byte, into the position section as scratch, then one on its high
  // byte into the sorted section. The position section then takes the originals.
  const std::size_t count = vramWrites.size();
  const auto entryOf = [this](std::size_t ordinal) {
    const VramWrite& write = vramWrites[ordinal];
    return std::pair<std::uint32_t, std::uint32_t>{write.address() | (static_cast<std::uint32_t>(ordinal & 0xFFFFu) << 16),
                                                    write.value()};
  };
  starts.fill(0u);
  for (std::size_t i = 0u; i < count; ++i) ++starts[(vramWrites[i].address() & 0xFFu) + 1u];
  for (std::size_t bucket = 0u; bucket < 256u; ++bucket) starts[bucket + 1u] += starts[bucket];
  for (std::size_t i = 0u; i < count; ++i) {
    const std::size_t to = starts[vramWrites[i].address() & 0xFFu]++;
    const auto [first, second] = entryOf(i);
    putWord(out, writesAt + to * 2u, first);
    putWord(out, writesAt + to * 2u + 1u, second);
  }
  starts.fill(0u);
  for (std::size_t i = 0u; i < count; ++i) ++starts[(vramWrites[i].address() >> 8) + 1u];
  for (std::size_t bucket = 0u; bucket < 256u; ++bucket) starts[bucket + 1u] += starts[bucket];
  for (std::size_t page = 0u; page < kPageTableWords; ++page) putWord(out, pagesAt + page, starts[page]);
  for (std::size_t i = 0u; i < count; ++i) {
    const std::uint32_t first = getWord(out, writesAt + i * 2u);
    const std::uint32_t second = getWord(out, writesAt + i * 2u + 1u);
    const std::size_t to = starts[(first & 0xFFFFu) >> 8]++;
    putWord(out, sortedAt + to * 2u, first);
    putWord(out, sortedAt + to * 2u + 1u, second);
  }
  for (std::size_t i = 0u; i < count; ++i) {
    putWord(out, writesAt + i * 2u, vramWrites[i].at);
    putWord(out, writesAt + i * 2u + 1u, vramWrites[i].change);
  }

  // The line table: how many writes stand on lines below each line.
  std::array<std::uint32_t, kLineTableWords> below{};
  below.fill(0u);
  for (std::size_t i = 0u; i < count; ++i) {
    const std::size_t line = std::min<std::size_t>(vramWrites[i].line(), kLineTableWords - 1u);
    for (std::size_t after = line + 1u; after < kLineTableWords; ++after) ++below[after];
  }
  for (std::size_t line = 0u; line < kLineTableWords; ++line) putWord(out, lineTableAt + line, below[line]);
  for (std::size_t line = 0u; line < kEventLineTableWords; ++line) {
    putWord(out, registerTableAt + line, registerBelow[line]);
    putWord(out, paletteTableAt + line, paletteBelow[line]);
  }
  return size;
}

}  // namespace snaggletooth
