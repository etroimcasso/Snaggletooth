#pragma once

// The word layout of a PpuTimeline line's registers, shared by the machine that
// records a timeline and by PpuTimeline::draw, which reads one back: where each
// Field stands in words 0 to 15, its value in a PpuState, and the Fields a write
// to one of the chip's registers changes.

#include <array>
#include <cstddef>
#include <cstdint>

#include "snaggletooth/snes/ppu.h"
#include "snaggletooth/snes/ppu_timeline.h"

namespace snaggletooth::timeline {

using Field = PpuTimeline::Field;

// Where a register's value stands in a line's words: the word, the bit it begins
// at, and its width.
struct Place {
  unsigned word;
  unsigned shift;
  unsigned bits;
};

inline constexpr std::size_t kRegisterFields = static_cast<std::size_t>(Field::PaletteWord);

inline constexpr std::array<Place, kRegisterFields> kPlaces{{
    {0u, 0u, 8u},   {0u, 8u, 8u},   {0u, 16u, 8u},  {0u, 24u, 8u},    // Inidisp Bgmode Mosaic Setini
    {1u, 0u, 8u},   {1u, 8u, 8u},   {1u, 16u, 8u},  {1u, 24u, 8u},    // Bg1sc Bg2sc Bg3sc Bg4sc
    {2u, 0u, 8u},   {2u, 8u, 8u},   {2u, 16u, 8u},  {2u, 24u, 8u},    // Bg12nba Bg34nba M7sel Cgwsel
    {3u, 0u, 16u},  {3u, 16u, 16u}, {4u, 0u, 16u},  {4u, 16u, 16u},   // Bg1hofs Bg1vofs Bg2hofs Bg2vofs
    {5u, 0u, 16u},  {5u, 16u, 16u}, {6u, 0u, 16u},  {6u, 16u, 16u},   // Bg3hofs Bg3vofs Bg4hofs Bg4vofs
    {7u, 0u, 16u},  {7u, 16u, 16u}, {8u, 0u, 16u},  {8u, 16u, 16u},   // M7a M7b M7c M7d
    {9u, 0u, 16u},  {9u, 16u, 16u}, {10u, 0u, 16u}, {10u, 16u, 16u},  // M7x M7y M7hofs M7vofs
    {11u, 0u, 8u},  {11u, 8u, 8u},  {11u, 16u, 8u}, {11u, 24u, 8u},   // W12sel W34sel Wobjsel Wbglog
    {12u, 0u, 8u},  {12u, 8u, 8u},  {12u, 16u, 8u}, {12u, 24u, 8u},   // Wobjlog Wh0 Wh1 Wh2
    {13u, 0u, 8u},  {13u, 8u, 8u},  {13u, 16u, 8u}, {13u, 24u, 8u},   // Wh3 Tm Ts Tmw
    {14u, 0u, 8u},  {14u, 8u, 8u},  {14u, 16u, 8u}, {14u, 24u, 8u},   // Tsw Cgadsub FixedRed FixedGreen
    {15u, 0u, 8u},                                                    // FixedBlue
}};

// Word 15's two facts after the fixed blue: the mosaic row's size and the line it
// began on, which change only as a line begins and so are no Field.
inline constexpr unsigned kMosaicSizeShift = 8u;
inline constexpr unsigned kMosaicLineShift = 16u;

// A register's value as the state holds it.
[[nodiscard]] inline std::uint32_t valueOf(const PpuState& s, Field field) noexcept {
  switch (field) {
    case Field::Inidisp: return s.inidisp;
    case Field::Bgmode: return s.bgmode;
    case Field::Mosaic: return s.mosaic;
    case Field::Setini: return s.setini;
    case Field::Bg1sc: return s.bg1sc;
    case Field::Bg2sc: return s.bg2sc;
    case Field::Bg3sc: return s.bg3sc;
    case Field::Bg4sc: return s.bg4sc;
    case Field::Bg12nba: return s.bg12nba;
    case Field::Bg34nba: return s.bg34nba;
    case Field::M7sel: return s.m7sel;
    case Field::Cgwsel: return s.cgwsel;
    case Field::Bg1hofs: return s.bg1hofs;
    case Field::Bg1vofs: return s.bg1vofs;
    case Field::Bg2hofs: return s.bg2hofs;
    case Field::Bg2vofs: return s.bg2vofs;
    case Field::Bg3hofs: return s.bg3hofs;
    case Field::Bg3vofs: return s.bg3vofs;
    case Field::Bg4hofs: return s.bg4hofs;
    case Field::Bg4vofs: return s.bg4vofs;
    case Field::M7a: return s.m7a;
    case Field::M7b: return s.m7b;
    case Field::M7c: return s.m7c;
    case Field::M7d: return s.m7d;
    case Field::M7x: return s.m7x;
    case Field::M7y: return s.m7y;
    case Field::M7hofs: return s.m7hofs;
    case Field::M7vofs: return s.m7vofs;
    case Field::W12sel: return s.w12sel;
    case Field::W34sel: return s.w34sel;
    case Field::Wobjsel: return s.wobjsel;
    case Field::Wbglog: return s.wbglog;
    case Field::Wobjlog: return s.wobjlog;
    case Field::Wh0: return s.wh0;
    case Field::Wh1: return s.wh1;
    case Field::Wh2: return s.wh2;
    case Field::Wh3: return s.wh3;
    case Field::Tm: return s.tm;
    case Field::Ts: return s.ts;
    case Field::Tmw: return s.tmw;
    case Field::Tsw: return s.tsw;
    case Field::Cgadsub: return s.cgadsub;
    case Field::FixedRed: return s.fixedRed;
    case Field::FixedGreen: return s.fixedGreen;
    case Field::FixedBlue: return s.fixedBlue;
    case Field::PaletteWord: break;
  }
  return 0u;
}

// The state's register set to a value, which is a byte register's low eight bits
// or a scroll or Mode 7 word whole.
inline void setValue(PpuState& s, Field field, std::uint32_t value) noexcept {
  const auto byte = static_cast<std::uint8_t>(value);
  const auto word = static_cast<std::uint16_t>(value);
  switch (field) {
    case Field::Inidisp: s.inidisp = byte; break;
    case Field::Bgmode: s.bgmode = byte; break;
    case Field::Mosaic: s.mosaic = byte; break;
    case Field::Setini: s.setini = byte; break;
    case Field::Bg1sc: s.bg1sc = byte; break;
    case Field::Bg2sc: s.bg2sc = byte; break;
    case Field::Bg3sc: s.bg3sc = byte; break;
    case Field::Bg4sc: s.bg4sc = byte; break;
    case Field::Bg12nba: s.bg12nba = byte; break;
    case Field::Bg34nba: s.bg34nba = byte; break;
    case Field::M7sel: s.m7sel = byte; break;
    case Field::Cgwsel: s.cgwsel = byte; break;
    case Field::Bg1hofs: s.bg1hofs = word; break;
    case Field::Bg1vofs: s.bg1vofs = word; break;
    case Field::Bg2hofs: s.bg2hofs = word; break;
    case Field::Bg2vofs: s.bg2vofs = word; break;
    case Field::Bg3hofs: s.bg3hofs = word; break;
    case Field::Bg3vofs: s.bg3vofs = word; break;
    case Field::Bg4hofs: s.bg4hofs = word; break;
    case Field::Bg4vofs: s.bg4vofs = word; break;
    case Field::M7a: s.m7a = word; break;
    case Field::M7b: s.m7b = word; break;
    case Field::M7c: s.m7c = word; break;
    case Field::M7d: s.m7d = word; break;
    case Field::M7x: s.m7x = word; break;
    case Field::M7y: s.m7y = word; break;
    case Field::M7hofs: s.m7hofs = word; break;
    case Field::M7vofs: s.m7vofs = word; break;
    case Field::W12sel: s.w12sel = byte; break;
    case Field::W34sel: s.w34sel = byte; break;
    case Field::Wobjsel: s.wobjsel = byte; break;
    case Field::Wbglog: s.wbglog = byte; break;
    case Field::Wobjlog: s.wobjlog = byte; break;
    case Field::Wh0: s.wh0 = byte; break;
    case Field::Wh1: s.wh1 = byte; break;
    case Field::Wh2: s.wh2 = byte; break;
    case Field::Wh3: s.wh3 = byte; break;
    case Field::Tm: s.tm = byte; break;
    case Field::Ts: s.ts = byte; break;
    case Field::Tmw: s.tmw = byte; break;
    case Field::Tsw: s.tsw = byte; break;
    case Field::Cgadsub: s.cgadsub = byte; break;
    case Field::FixedRed: s.fixedRed = byte; break;
    case Field::FixedGreen: s.fixedGreen = byte; break;
    case Field::FixedBlue: s.fixedBlue = byte; break;
    case Field::PaletteWord: break;
  }
}

// Words 0 to 15 of a line from the state, and the state's registers from them.
inline void packRegisters(const PpuState& s, std::array<std::uint32_t, 32>& words) noexcept {
  for (unsigned word = 0u; word < 16u; ++word) words[word] = 0u;
  for (std::size_t index = 0u; index < kRegisterFields; ++index) {
    const Place& place = kPlaces[index];
    words[place.word] |= valueOf(s, static_cast<Field>(index)) << place.shift;
  }
  words[15] |= static_cast<std::uint32_t>(s.mosaicBlockSize) << kMosaicSizeShift;
  words[15] |= static_cast<std::uint32_t>(s.mosaicBlockLine) << kMosaicLineShift;
}

inline void unpackRegisters(const std::array<std::uint32_t, 32>& words, PpuState& s) noexcept {
  for (std::size_t index = 0u; index < kRegisterFields; ++index) {
    const Place& place = kPlaces[index];
    const std::uint32_t mask = place.bits == 32u ? ~0u : (1u << place.bits) - 1u;
    setValue(s, static_cast<Field>(index), (words[place.word] >> place.shift) & mask);
  }
  s.mosaicBlockSize = static_cast<std::uint8_t>((words[15] >> kMosaicSizeShift) & 0xFFu);
  s.mosaicBlockLine = static_cast<std::uint16_t>(words[15] >> kMosaicLineShift);
}

// The Fields a write to the register at `offset`, $2100-$213F, changes, at most
// three, and how many: none for a register the picture's dots do not read, two for
// $210D and $210E, one for each fixed-color channel a write of `value` to $2132
// names. $2122 is the palette word, which the recorder handles apart.
struct Changed {
  std::array<Field, 3> fields{};
  unsigned count = 0;
};

[[nodiscard]] inline Changed fieldsOf(std::uint16_t offset, std::uint8_t value) noexcept {
  Changed changed;
  const auto one = [&](Field field) { changed.fields[changed.count++] = field; };
  switch (offset) {
    case 0x2100u: one(Field::Inidisp); break;
    case 0x2105u: one(Field::Bgmode); break;
    case 0x2106u: one(Field::Mosaic); break;
    case 0x2107u: one(Field::Bg1sc); break;
    case 0x2108u: one(Field::Bg2sc); break;
    case 0x2109u: one(Field::Bg3sc); break;
    case 0x210Au: one(Field::Bg4sc); break;
    case 0x210Bu: one(Field::Bg12nba); break;
    case 0x210Cu: one(Field::Bg34nba); break;
    case 0x210Du: one(Field::Bg1hofs); one(Field::M7hofs); break;
    case 0x210Eu: one(Field::Bg1vofs); one(Field::M7vofs); break;
    case 0x210Fu: one(Field::Bg2hofs); break;
    case 0x2110u: one(Field::Bg2vofs); break;
    case 0x2111u: one(Field::Bg3hofs); break;
    case 0x2112u: one(Field::Bg3vofs); break;
    case 0x2113u: one(Field::Bg4hofs); break;
    case 0x2114u: one(Field::Bg4vofs); break;
    case 0x211Au: one(Field::M7sel); break;
    case 0x211Bu: one(Field::M7a); break;
    case 0x211Cu: one(Field::M7b); break;
    case 0x211Du: one(Field::M7c); break;
    case 0x211Eu: one(Field::M7d); break;
    case 0x211Fu: one(Field::M7x); break;
    case 0x2120u: one(Field::M7y); break;
    case 0x2123u: one(Field::W12sel); break;
    case 0x2124u: one(Field::W34sel); break;
    case 0x2125u: one(Field::Wobjsel); break;
    case 0x2126u: one(Field::Wh0); break;
    case 0x2127u: one(Field::Wh1); break;
    case 0x2128u: one(Field::Wh2); break;
    case 0x2129u: one(Field::Wh3); break;
    case 0x212Au: one(Field::Wbglog); break;
    case 0x212Bu: one(Field::Wobjlog); break;
    case 0x212Cu: one(Field::Tm); break;
    case 0x212Du: one(Field::Ts); break;
    case 0x212Eu: one(Field::Tmw); break;
    case 0x212Fu: one(Field::Tsw); break;
    case 0x2130u: one(Field::Cgwsel); break;
    case 0x2131u: one(Field::Cgadsub); break;
    case 0x2132u:
      if ((value & 0x20u) != 0u) one(Field::FixedRed);
      if ((value & 0x40u) != 0u) one(Field::FixedGreen);
      if ((value & 0x80u) != 0u) one(Field::FixedBlue);
      break;
    case 0x2133u: one(Field::Setini); break;
    default: break;
  }
  return changed;
}

// Whether setting a Field makes what the chip works out from the registers stale,
// as a write to its register does: the descriptions of the four backgrounds from
// $2105-$210C and $2133, and the windows from $2123-$212B.
[[nodiscard]] constexpr bool dropsDescriptions(Field field) noexcept {
  return field == Field::Bgmode || field == Field::Mosaic || field == Field::Bg1sc ||
         field == Field::Bg2sc || field == Field::Bg3sc || field == Field::Bg4sc ||
         field == Field::Bg12nba || field == Field::Bg34nba || field == Field::Setini;
}
[[nodiscard]] constexpr bool dropsWindows(Field field) noexcept {
  return field == Field::W12sel || field == Field::W34sel || field == Field::Wobjsel ||
         field == Field::Wh0 || field == Field::Wh1 || field == Field::Wh2 || field == Field::Wh3 ||
         field == Field::Wbglog || field == Field::Wobjlog;
}

// The line and dot packed into an event's or a VRAM write's `at`.
[[nodiscard]] constexpr std::uint32_t at(std::uint32_t line, std::uint32_t dot) noexcept {
  return line | (dot << 16);
}

}  // namespace snaggletooth::timeline
