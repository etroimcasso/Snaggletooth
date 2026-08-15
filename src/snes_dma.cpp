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

std::uint8_t Snes::dmaReadA(std::uint32_t address) {
  if (aBusExcluded(address)) return state_.mdr;  // an excluded region reads back as open bus
  return routeRead(address);
}

void Snes::dmaWriteA(std::uint32_t address, std::uint8_t value) {
  state_.mdr = value;                 // the write drives the data bus either way
  if (aBusExcluded(address)) return;  // but lands nowhere in an excluded region
  routeWrite(address, value);
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
  const int channel = std::countr_zero(state_.mdmaen);
  DmaChannel& ch = state_.dma[static_cast<std::size_t>(channel)];

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
  if ((ch.dmap & 0x80u) == 0u) {
    routeWrite(bAddr, dmaReadA(aAddr));  // A -> B
  } else {
    dmaWriteA(aAddr, routeRead(bAddr));  // B -> A
  }

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

void Snes::hdmaLoadEntry(DmaChannel& channel, bool indirect) {
  // Read the next line-count byte from the table, advancing the table pointer. A
  // $00 terminates the channel — the caller deactivates it. An indirect entry then
  // carries a two-byte pointer, loaded into the channel's indirect address.
  channel.nltr = dmaReadA((static_cast<std::uint32_t>(channel.a1b) << 16) | channel.a2a);
  channel.a2a = static_cast<std::uint16_t>(channel.a2a + 1u);
  if (channel.nltr == 0u) return;
  if (indirect) {
    const std::uint32_t bank = static_cast<std::uint32_t>(channel.a1b) << 16;
    const std::uint8_t lo = dmaReadA(bank | channel.a2a);
    const std::uint8_t hi = dmaReadA(
        bank | static_cast<std::uint16_t>(channel.a2a + 1u));
    channel.das = static_cast<std::uint16_t>(lo | (hi << 8));
    channel.a2a = static_cast<std::uint16_t>(channel.a2a + 2u);
  }
}

void Snes::hdmaCycle() {
  std::uint32_t cost = kHdmaOverhead;

  if (state_.hdmaIniting) {
    // Start of frame: point each enabled channel's table cursor at its table start
    // and load its first entry. A channel whose first byte is $00 never activates.
    state_.hdmaActive = 0u;
    state_.hdmaDoWrite = 0u;
    for (int c = 0; c < 8; ++c) {
      if (((state_.hdmaen >> c) & 1u) == 0u) continue;
      DmaChannel& ch = state_.dma[static_cast<std::size_t>(c)];
      const bool indirect = (ch.dmap & 0x40u) != 0u;
      cost += indirect ? kHdmaIndirectInit : kHdmaChannel;
      ch.a2a = ch.a1t;
      hdmaLoadEntry(ch, indirect);
      if (ch.nltr != 0u) {
        state_.hdmaActive |= static_cast<std::uint8_t>(1u << c);
        state_.hdmaDoWrite |= static_cast<std::uint8_t>(1u << c);
      }
    }
    lastCost_ = cost;
    tickVideo(cost);
    videoAdvanced_ = true;
    return;
  }

  // A visible scanline's delivery, for every channel still active this frame.
  for (int c = 0; c < 8; ++c) {
    if (((state_.hdmaActive >> c) & 1u) == 0u) continue;
    DmaChannel& ch = state_.dma[static_cast<std::size_t>(c)];
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
        if ((ch.dmap & 0x80u) == 0u) {
          routeWrite(bAddr, dmaReadA(aAddr));  // A -> B, HDMA's usual direction
        } else {
          dmaWriteA(aAddr, routeRead(bAddr));  // B -> A
        }
        cost += kDmaByte;
      }
    }

    // Count the line down; a repeat entry writes every line, a plain one only on
    // its first. When the counter empties, load the next entry.
    const std::uint8_t entry = ch.nltr;
    const std::uint8_t count = static_cast<std::uint8_t>((entry & 0x7Fu) - 1u);
    ch.nltr = static_cast<std::uint8_t>((entry & 0x80u) | (count & 0x7Fu));
    const std::uint8_t bit = static_cast<std::uint8_t>(1u << c);
    if ((entry & 0x80u) != 0u) {
      state_.hdmaDoWrite |= bit;
    } else {
      state_.hdmaDoWrite = static_cast<std::uint8_t>(state_.hdmaDoWrite & ~bit);
    }
    if ((ch.nltr & 0x7Fu) == 0u) {
      if (indirect) cost += kHdmaIndirectLoad;
      hdmaLoadEntry(ch, indirect);
      if (ch.nltr == 0u) {  // a terminator ends the channel for the frame
        state_.hdmaActive = static_cast<std::uint8_t>(state_.hdmaActive & ~bit);
        state_.hdmaDoWrite = static_cast<std::uint8_t>(state_.hdmaDoWrite & ~bit);
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
