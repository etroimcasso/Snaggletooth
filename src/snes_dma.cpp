#include <array>
#include <bit>
#include <cstdint>

#include "snaggletooth/snes/snes.h"

// The DMA and HDMA engines. A general-purpose DMA (started by $420B) halts the CPU
// and copies bytes between the A bus (memory) and the B bus ($2100-$21FF) one at a
// time; it holds the machine's bus between the CPU's instructions, so its progress
// lives in the machine state and a snapshot resumes on the exact byte. HDMA (armed
// by $420C) runs in the background, delivering a table's values to hardware
// registers once per visible scanline; the CPU is halted for each event, so an
// event runs whole and only the per-frame table state is carried between lines.
// Every byte either engine moves is reported to the machine's observer as two
// accesses in the engine's name, the read then the write; an engine's overhead
// cycles touch no address and are not.

namespace snaggletooth {
namespace {

// A transfer pattern chooses which B-bus registers a unit touches, as offsets from
// BBAD, and so how many bytes the unit is. Patterns 5-7 reuse the shapes of 2-4;
// 6 and 7 repeat 2 and 3. This is the console's documented table.
struct Pattern {
  std::uint8_t length;
  std::array<std::uint8_t, 4> offset;
};
constexpr std::array<Pattern, 8> kPatterns{{
    {1, {0, 0, 0, 0}},  // 0: one register
    {2, {0, 1, 0, 0}},  // 1: two registers
    {2, {0, 0, 0, 0}},  // 2: one register, written twice
    {4, {0, 0, 1, 1}},  // 3: two registers, each written twice
    {4, {0, 1, 2, 3}},  // 4: four registers
    {4, {0, 1, 0, 1}},  // 5: two registers, alternating, twice
    {2, {0, 0, 0, 0}},  // 6: as pattern 2
    {4, {0, 0, 1, 1}},  // 7: as pattern 3
}};

constexpr std::uint32_t kDmaByte = 8u;            // every DMA-engine cycle is eight master cycles
constexpr std::uint32_t kHdmaOverhead = 18u;      // the shared per-run HDMA overhead
constexpr std::uint32_t kHdmaChannel = 8u;        // per active channel, per scanline
constexpr std::uint32_t kHdmaIndirectInit = 24u;  // an indirect channel's start-of-frame overhead
constexpr std::uint32_t kHdmaIndirectLoad = 16u;  // loading a fresh indirect pointer mid-frame

}  // namespace

// ---- the A bus under DMA --------------------------------------------------

bool Snes::aBusExcluded(std::uint32_t address) noexcept {
  const std::uint8_t bank = static_cast<std::uint8_t>((address >> 16) & 0xFFu);
  const std::uint16_t offset = static_cast<std::uint16_t>(address & 0xFFFFu);
  const bool systemBank = bank <= 0x3Fu || (bank >= 0x80u && bank <= 0xBFu);
  if (!systemBank) return false;
  // DMA cannot reach the memory-mapped registers on the A bus: the PPU and APU
  // range, the manual joypad ports, the CPU registers, and the DMA registers.
  return (offset >= 0x2100u && offset <= 0x21FFu) ||
         (offset >= 0x4000u && offset <= 0x41FFu) ||
         (offset >= 0x4200u && offset <= 0x421Fu) ||
         (offset >= 0x4300u && offset <= 0x437Fu);
}

bool Snes::aBusIsWorkRam(std::uint32_t address) noexcept {
  const std::uint8_t bank = static_cast<std::uint8_t>((address >> 16) & 0xFFu);
  const std::uint16_t offset = static_cast<std::uint16_t>(address & 0xFFFFu);
  if (bank == 0x7Eu || bank == 0x7Fu) return true;
  const bool systemBank = bank <= 0x3Fu || (bank >= 0x80u && bank <= 0xBFu);
  return systemBank && offset <= 0x1FFFu;
}

std::uint8_t Snes::dmaReadA(std::uint32_t address, AccessSource source) {
  if (aBusExcluded(address)) return state_.mdr;  // an excluded region reads back as open bus
  return routeRead(address, source);
}

void Snes::dmaWriteA(std::uint32_t address, std::uint8_t value, AccessSource source) {
  state_.mdr = value;                 // the write drives the data bus either way
  if (aBusExcluded(address)) return;  // but lands nowhere in an excluded region
  routeWrite(address, value, source);
}

std::uint8_t Snes::engineRead(std::uint32_t address, bool aBus, AccessSource source,
                              std::uint8_t channel, bool table, bool pastTableEnd) {
  const std::uint8_t value = aBus ? dmaReadA(address, source) : routeRead(address, source);
  observe(address, value, false, CycleKind::DataRead, source, channel, table, pastTableEnd);
  return value;
}

void Snes::engineWrite(std::uint32_t address, std::uint8_t value, bool aBus,
                       AccessSource source, std::uint8_t channel) {
  if (aBus) {
    dmaWriteA(address, value, source);
  } else {
    routeWrite(address, value, source);
  }
  observe(address, value, true, CycleKind::DataWrite, source, channel);
}

void Snes::engineByte(std::uint32_t aAddr, std::uint32_t bAddr, bool toA, AccessSource source,
                      std::uint8_t channel, bool table) {
  // Work RAM is one chip on both buses, and a byte that names it on both — a work-RAM
  // address on the A bus and $2180-$2183 on the B bus — is not copied. The port's side
  // is open bus: a write through it lands nowhere, a read through it returns the byte
  // the data bus already holds, and the port's address does not step either way. The
  // engine still drives both accesses, so both are reported; the port, which moved
  // nothing, reports none of its own.
  const std::uint16_t bOffset = static_cast<std::uint16_t>(bAddr & 0xFFFFu);
  const bool portRefuses = aBusIsWorkRam(aAddr) && bOffset >= 0x2180u && bOffset <= 0x2183u;
  if (!toA) {
    const std::uint8_t byte = engineRead(aAddr, /*aBus=*/true, source, channel, table);
    if (portRefuses) {
      observe(bAddr, byte, true, CycleKind::DataWrite, source, channel);
    } else {
      engineWrite(bAddr, byte, /*aBus=*/false, source, channel);
    }
    return;
  }
  std::uint8_t byte = state_.mdr;
  if (portRefuses) {
    observe(bAddr, byte, false, CycleKind::DataRead, source, channel);
  } else {
    byte = engineRead(bAddr, /*aBus=*/false, source, channel);
  }
  engineWrite(aAddr, byte, /*aBus=*/true, source, channel);
}

std::uint32_t Snes::resumePad(std::uint32_t cpuCycle) const noexcept {
  // Wait to a whole number of CPU-clock cycles since the transfer paused; zero is
  // not an option, so an already-whole boundary waits a full CPU cycle.
  const std::uint64_t elapsed = state_.master - state_.dmaPauseMaster;
  return cpuCycle - static_cast<std::uint32_t>(elapsed % cpuCycle);
}

// ---- the general-purpose DMA engine ---------------------------------------

void Snes::triggerDma(std::uint8_t channels) {
  state_.mdmaen = channels;
  // A write of zero selects nothing and starts nothing. Otherwise the transfer
  // engages after one more CPU cycle — the following instruction's opcode fetch.
  if (channels != 0u && !state_.dmaRunning) state_.dmaArm = 1u;
}

void Snes::dmaCycle() {
  if (!state_.dmaOpened) {
    // The pause aligns to a whole multiple of eight master cycles since reset, then
    // the transfer pays one shared overhead. Both fold into this first cycle: the
    // beam and the APU advance by the total, which is all a caller can observe.
    const std::uint32_t align =
        8u - static_cast<std::uint32_t>(state_.dmaPauseMaster & 7u);
    lastCost_ = align + kDmaByte;
    tickVideo(lastCost_);
    videoAdvanced_ = true;
    state_.dmaOpened = true;
    return;
  }

  // The active channel is the lowest one still selected; mdmaen is nonzero here,
  // because the engine stops the moment it empties.
  const std::uint8_t channel = static_cast<std::uint8_t>(std::countr_zero(state_.mdmaen));
  DmaChannel& ch = state_.dma[channel];

  lastCost_ = kDmaByte;
  tickVideo(lastCost_);  // tick-first, the way a CPU access ticks before it resolves
  videoAdvanced_ = true;

  if (!state_.dmaChannelOpened) {  // the channel's own overhead cycle, before its bytes
    state_.dmaChannelOpened = true;
    state_.dmaUnit = 0u;
    return;
  }

  // One byte: a read on one bus is a write on the other. The A-bus address is the
  // channel's source; the B-bus address is $2100 plus BBAD plus the pattern offset.
  const Pattern& pattern = kPatterns[ch.dmap & 7u];
  const std::uint32_t bAddr =
      0x2100u | ((ch.bbad + pattern.offset[state_.dmaUnit]) & 0xFFu);
  const std::uint32_t aAddr = (static_cast<std::uint32_t>(ch.a1b) << 16) | ch.a1t;
  engineByte(aAddr, bAddr, /*toA=*/(ch.dmap & 0x80u) != 0u, AccessSource::Dma, channel,
             /*table=*/false);

  // Step the A-bus address by the adjust mode: increment, decrement, or fixed.
  const std::uint8_t adjust = (ch.dmap >> 3) & 3u;
  if (adjust == 0u) {
    ch.a1t = static_cast<std::uint16_t>(ch.a1t + 1u);
  } else if (adjust == 2u) {
    ch.a1t = static_cast<std::uint16_t>(ch.a1t - 1u);
  }
  state_.dmaUnit = static_cast<std::uint8_t>((state_.dmaUnit + 1u) % pattern.length);

  // Count the byte down; a count of zero meant the whole 65536, so the channel is
  // done only when the decrement reaches zero.
  ch.das = static_cast<std::uint16_t>(ch.das - 1u);
  if (ch.das == 0u) {
    state_.mdmaen =
        static_cast<std::uint8_t>(state_.mdmaen & ~(1u << channel));
    state_.dmaChannelOpened = false;
    if (state_.mdmaen == 0u) {  // the last channel finished; the transfer is over
      state_.dmaRunning = false;
      state_.dmaResumePad = true;  // the first CPU cycle back owes the resume pad
    }
  }
}

// ---- the HDMA engine ------------------------------------------------------

void Snes::hdmaLoadCount(std::uint8_t index) {
  // The next line-count byte, read from the table as the cursor steps past it. A $00
  // ends the channel for the frame, which is the caller's to act on. Every byte the
  // engine reads from a table is reported as the table's.
  DmaChannel& channel = state_.dma[index];
  channel.nltr = engineRead((static_cast<std::uint32_t>(channel.a1b) << 16) | channel.a2a,
                            /*aBus=*/true, AccessSource::Hdma, index, /*table=*/true);
  channel.a2a = static_cast<std::uint16_t>(channel.a2a + 1u);
}

void Snes::hdmaLoadPointer(std::uint8_t index, bool highByteOnly, bool pastTableEnd) {
  // An indirect entry's pointer, read from the table into the channel's indirect
  // address. The whole load is two bytes, low then high. The short one reads a single
  // byte into the high half and leaves the low half $00, the cursor stepping one.
  DmaChannel& channel = state_.dma[index];
  const std::uint32_t bank = static_cast<std::uint32_t>(channel.a1b) << 16;
  std::uint8_t lo = 0x00u;
  if (!highByteOnly) {
    lo = engineRead(bank | channel.a2a, /*aBus=*/true, AccessSource::Hdma, index, /*table=*/true,
                    pastTableEnd);
    channel.a2a = static_cast<std::uint16_t>(channel.a2a + 1u);
  }
  const std::uint8_t hi = engineRead(bank | channel.a2a, /*aBus=*/true, AccessSource::Hdma, index,
                                     /*table=*/true, pastTableEnd);
  channel.a2a = static_cast<std::uint16_t>(channel.a2a + 1u);
  channel.das = static_cast<std::uint16_t>(lo | (hi << 8));
}

void Snes::endDmaOnChannels(std::uint8_t channels) noexcept {
  // HDMA outranks a general-purpose DMA. An event on other channels only holds the
  // transfer for as long as it takes; one that involves the channel the transfer is on
  // ends that channel's transfer where it stands — its $420B bit clears and its count
  // keeps what was left — and the next selected channel, if there is one, runs from
  // its own overhead cycle.
  if (!state_.dmaRunning) return;
  const std::uint8_t current =
      static_cast<std::uint8_t>(1u << std::countr_zero(state_.mdmaen));
  if ((channels & current) == 0u) return;
  state_.mdmaen = static_cast<std::uint8_t>(state_.mdmaen & ~current);
  state_.dmaChannelOpened = false;
  if (state_.mdmaen == 0u) {
    state_.dmaRunning = false;
    state_.dmaResumePad = true;
  }
}

void Snes::enableHdma(std::uint8_t channels) {
  // $420C. A channel the write takes away stops delivering from the next line: the
  // bit gates a line's work, and the channel's place in its table is kept, so putting
  // it back resumes where it stood. A channel the write brings in for the first time
  // this frame joins the lines that are left with its registers exactly as the program
  // set them — nothing is reloaded, because the frame's one reload has passed — and it
  // delivers on its first line only if HDMA was already running, which is the
  // difference the console shows between starting the frame's first channel midway and
  // adding another to channels already going. A channel whose table has ended is done
  // until the next frame and this cannot bring it back.
  const std::uint8_t joining =
      static_cast<std::uint8_t>(channels & ~state_.hdmaen & ~state_.hdmaActive &
                                ~state_.hdmaEnded);
  const bool running = state_.hdmaen != 0u;
  state_.hdmaen = channels;
  // The vertical blank has no lines left to join: every channel is already out for the
  // frame, and the frame beginning after it hands each one its table afresh.
  if (joining == 0u || state_.inVblank) return;
  state_.hdmaActive = static_cast<std::uint8_t>(state_.hdmaActive | joining);
  if (running) {
    state_.hdmaDoWrite = static_cast<std::uint8_t>(state_.hdmaDoWrite | joining);
  } else {
    state_.hdmaDoWrite = static_cast<std::uint8_t>(state_.hdmaDoWrite & ~joining);
  }
}

void Snes::hdmaCycle() {
  std::uint32_t cost = kHdmaOverhead;

  if (state_.hdmaIniting) {
    // Start of frame: point each enabled channel's table cursor at its table start
    // and load its first entry. A channel whose first byte is $00 is done for the
    // frame before it delivers anything, and its pointer is not read.
    endDmaOnChannels(state_.hdmaen);
    state_.hdmaActive = 0u;
    state_.hdmaEnded = 0u;
    state_.hdmaDoWrite = 0u;
    for (std::uint8_t c = 0; c < 8; ++c) {
      if (((state_.hdmaen >> c) & 1u) == 0u) continue;
      DmaChannel& ch = state_.dma[c];
      const bool indirect = (ch.dmap & 0x40u) != 0u;
      cost += indirect ? kHdmaIndirectInit : kHdmaChannel;
      ch.a2a = ch.a1t;
      hdmaLoadCount(c);
      if (indirect && ch.nltr != 0u) {
        hdmaLoadPointer(c, /*highByteOnly=*/false, /*pastTableEnd=*/false);
      }
      if (ch.nltr != 0u) {
        state_.hdmaActive |= static_cast<std::uint8_t>(1u << c);
        state_.hdmaDoWrite |= static_cast<std::uint8_t>(1u << c);
      } else {
        state_.hdmaEnded |= static_cast<std::uint8_t>(1u << c);
      }
    }
    lastCost_ = cost;
    tickVideo(cost);
    videoAdvanced_ = true;
    return;
  }

  // A visible scanline's delivery, for every channel whose table is still running and
  // whose bit $420C still holds.
  const std::uint8_t delivering = static_cast<std::uint8_t>(state_.hdmaActive & state_.hdmaen);
  endDmaOnChannels(delivering);
  // The highest channel delivering is the line's last, which matters to one load below.
  // With none delivering the count is eight and this names no channel.
  const std::uint8_t lastChannel = static_cast<std::uint8_t>(7 - std::countl_zero(delivering));
  for (std::uint8_t c = 0; c < 8; ++c) {
    if (((delivering >> c) & 1u) == 0u) continue;
    DmaChannel& ch = state_.dma[c];
    const bool indirect = (ch.dmap & 0x40u) != 0u;
    cost += kHdmaChannel;

    if (((state_.hdmaDoWrite >> c) & 1u) != 0u) {
      const Pattern& pattern = kPatterns[ch.dmap & 7u];
      for (std::uint8_t i = 0; i < pattern.length; ++i) {
        const std::uint32_t bAddr = 0x2100u | ((ch.bbad + pattern.offset[i]) & 0xFFu);
        std::uint32_t aAddr;
        if (indirect) {  // the source is the running indirect address
          aAddr = (static_cast<std::uint32_t>(ch.dasb) << 16) | ch.das;
          ch.das = static_cast<std::uint16_t>(ch.das + 1u);
        } else {         // the source is the table itself
          aAddr = (static_cast<std::uint32_t>(ch.a1b) << 16) | ch.a2a;
          ch.a2a = static_cast<std::uint16_t>(ch.a2a + 1u);
        }
        // A direct table's value is read from the table itself; an indirect
        // entry's is read from where its pointer says. A -> B is HDMA's usual
        // direction, and the other is honoured.
        engineByte(aAddr, bAddr, /*toA=*/(ch.dmap & 0x80u) != 0u, AccessSource::Hdma, c,
                   /*table=*/!indirect);
        cost += kDmaByte;
      }
    }

    // The whole byte counts down, and the table's two ranges fall out of that one
    // rule. $01-$80 transfer on their first line and stand quiet for the rest of
    // theirs; $81-$FF transfer on every line of theirs. The count reaching zero
    // loads the next entry. $80 is the line the two ranges meet on — one transfer,
    // then 127 quiet lines — and it comes out right only because the borrow reaches
    // the top bit and clears it. Masking that bit out of the subtraction makes $80
    // repeat 127 times instead, which is a channel that never stops.
    ch.nltr = static_cast<std::uint8_t>(ch.nltr - 1u);
    const std::uint8_t bit = static_cast<std::uint8_t>(1u << c);
    if ((ch.nltr & 0x80u) != 0u) {
      state_.hdmaDoWrite |= bit;
    } else {
      state_.hdmaDoWrite = static_cast<std::uint8_t>(state_.hdmaDoWrite & ~bit);
    }
    if ((ch.nltr & 0x7Fu) == 0u) {
      hdmaLoadCount(c);
      if (indirect) {
        // The pointer is read whatever the count was, so a table's end still loads the
        // two bytes that follow its $00. The line's last channel is the exception: on a
        // $00 it reads one byte, and the load takes one eight-cycle read fewer.
        const bool ended = ch.nltr == 0u;
        const bool shortLoad = ended && c == lastChannel;
        hdmaLoadPointer(c, shortLoad, /*pastTableEnd=*/ended);
        cost += shortLoad ? kHdmaIndirectLoad - kDmaByte : kHdmaIndirectLoad;
      }
      if (ch.nltr == 0u) {  // a terminator ends the channel for the frame
        state_.hdmaActive = static_cast<std::uint8_t>(state_.hdmaActive & ~bit);
        state_.hdmaDoWrite = static_cast<std::uint8_t>(state_.hdmaDoWrite & ~bit);
        state_.hdmaEnded = static_cast<std::uint8_t>(state_.hdmaEnded | bit);
      } else {
        state_.hdmaDoWrite |= bit;  // the new entry writes on its first line
      }
    }
  }

  lastCost_ = cost;
  tickVideo(cost);
  videoAdvanced_ = true;
}

// ---- the channel registers ($4300-$437F) ----------------------------------

std::uint8_t Snes::readDmaReg(std::uint16_t offset) {
  DmaChannel& ch = state_.dma[(offset >> 4) & 7u];
  switch (offset & 0xFu) {
    case 0x0: return latch(ch.dmap);
    case 0x1: return latch(ch.bbad);
    case 0x2: return latch(static_cast<std::uint8_t>(ch.a1t & 0xFFu));
    case 0x3: return latch(static_cast<std::uint8_t>(ch.a1t >> 8));
    case 0x4: return latch(ch.a1b);
    case 0x5: return latch(static_cast<std::uint8_t>(ch.das & 0xFFu));
    case 0x6: return latch(static_cast<std::uint8_t>(ch.das >> 8));
    case 0x7: return latch(ch.dasb);
    case 0x8: return latch(static_cast<std::uint8_t>(ch.a2a & 0xFFu));
    case 0x9: return latch(static_cast<std::uint8_t>(ch.a2a >> 8));
    case 0xA: return latch(ch.nltr);
    case 0xB:
    case 0xF: return latch(ch.unused);
    default: return state_.mdr;  // $43xC-$43xE are open bus
  }
}

void Snes::writeDmaReg(std::uint16_t offset, std::uint8_t value) {
  DmaChannel& ch = state_.dma[(offset >> 4) & 7u];
  switch (offset & 0xFu) {
    case 0x0: ch.dmap = value; return;
    case 0x1: ch.bbad = value; return;
    case 0x2: ch.a1t = static_cast<std::uint16_t>((ch.a1t & 0xFF00u) | value); return;
    case 0x3: ch.a1t = static_cast<std::uint16_t>((ch.a1t & 0x00FFu) | (value << 8)); return;
    case 0x4: ch.a1b = value; return;
    case 0x5: ch.das = static_cast<std::uint16_t>((ch.das & 0xFF00u) | value); return;
    case 0x6: ch.das = static_cast<std::uint16_t>((ch.das & 0x00FFu) | (value << 8)); return;
    case 0x7: ch.dasb = value; return;
    case 0x8: ch.a2a = static_cast<std::uint16_t>((ch.a2a & 0xFF00u) | value); return;
    case 0x9: ch.a2a = static_cast<std::uint16_t>((ch.a2a & 0x00FFu) | (value << 8)); return;
    case 0xA: ch.nltr = value; return;
    case 0xB:
    case 0xF: ch.unused = value; return;
    default: return;  // $43xC-$43xE ignore writes
  }
}

}  // namespace snaggletooth
