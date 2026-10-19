// PpuTimeline: a line's register words read back, and the frame drawn from the
// timeline alone through the chip's own dot code.

#include "snaggletooth/snes/ppu_timeline.h"

#include <algorithm>
#include <memory>

#include "ppu_timeline_words.h"

namespace snaggletooth {

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

}  // namespace snaggletooth
