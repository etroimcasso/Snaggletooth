// BRR sample decode and the sample directory read.
//
// Every expected value is hand-derived from fullsnes's "SNES APU DSP BRR
// Samples" section (the primary contract), cross-checked against Anomie's S-DSP
// doc where both speak. Line references below are to the staged fullsnes text.
// The synthetic BRR blocks are authored inline: byte 0 is the header (shift 7-4,
// filter 3-2, loop/end 1-0), bytes 1-8 each hold two signed nibbles.

#include <array>
#include <cstdint>

#include <gtest/gtest.h>

#include "snaggletooth/apu/dsp.h"

namespace {

using snaggletooth::BrrBlock;
using snaggletooth::BrrSource;
using snaggletooth::decodeBrrBlock;
using snaggletooth::readBrrSource;

// Decodes a 9-byte block with the given entering filter history (0/0 for a
// sample's first block).
BrrBlock decode(std::array<std::uint8_t, 9> block, int old = 0, int older = 0) {
  return decodeBrrBlock(block, static_cast<std::int16_t>(old),
                        static_cast<std::int16_t>(older));
}

// ── Shift and Filter 0 (direct) ─────────────────────────────────────────────

TEST(Brr, DecodesFilter0Directly) {
  // sample = (nibble SHL shift) SAR 1 (line 2718); Filter 0 passes it through
  // (line 2731). shift 12 scales a nibble by 2048. Nibbles: 7,1 then -8,-1.
  const BrrBlock b = decode({0xC0, 0x71, 0x8F, 0x71, 0x8F, 0x71, 0x8F, 0x71, 0x8F});
  EXPECT_EQ(b.samples[0], 14336);   // +7 * 2048
  EXPECT_EQ(b.samples[1], 2048);    // +1 * 2048
  EXPECT_EQ(b.samples[2], -16384);  // -8 * 2048
  EXPECT_EQ(b.samples[3], -2048);   // -1 * 2048
  EXPECT_FALSE(b.endBlock);
  EXPECT_FALSE(b.loopBlock);
}

TEST(Brr, Shift0StripsTheLowBit) {
  // shift 0 is "rather useless (since it strips the low bit)" (line 2719): the
  // scale is 1, so sample = nibble SAR 1.
  const BrrBlock b = decode({0x00, 0x71, 0x8F, 0, 0, 0, 0, 0, 0});
  EXPECT_EQ(b.samples[0], 3);   // +7 >> 1
  EXPECT_EQ(b.samples[1], 0);   // +1 >> 1
  EXPECT_EQ(b.samples[2], -4);  // -8 >> 1
  EXPECT_EQ(b.samples[3], -1);  // -1 >> 1 (arithmetic, toward -inf)
}

TEST(Brr, Shift13To15DecodeAsShift12OverNibbleSar3) {
  // "When shift=13..15, decoding works as if shift=12 and nibble=(nibble SAR 3)"
  // (line 2720): -8 SAR 3 = -1 -> -2048; +7 SAR 3 = 0 -> 0.
  const BrrBlock s13 = decode({0xD0, 0x8F, 0x71, 0, 0, 0, 0, 0, 0});
  EXPECT_EQ(s13.samples[0], -2048);  // -8 -> -1 -> -2048
  EXPECT_EQ(s13.samples[1], -2048);  // -1 -> -1 -> -2048
  EXPECT_EQ(s13.samples[2], 0);      // +7 -> 0
  EXPECT_EQ(s13.samples[3], 0);      // +1 -> 0

  const BrrBlock s15 = decode({0xF0, 0x8F, 0, 0, 0, 0, 0, 0, 0});
  EXPECT_EQ(s15.samples[0], -2048);  // shift 15 decodes identically
}

// ── The three IIR filters (exact integer forms) ─────────────────────────────

TEST(Brr, AppliesFilter1) {
  // Filter 1: new = sample + old + ((-old) SAR 4) (line 2732). Sample 0 seeds
  // old=14336; sample 1 (nibble 0) = 0 + 14336 + (-14336>>4) = 14336 - 896.
  const BrrBlock b = decode({0xC4, 0x70, 0, 0, 0, 0, 0, 0, 0});
  EXPECT_EQ(b.samples[0], 14336);
  EXPECT_EQ(b.samples[1], 13440);
}

TEST(Brr, AppliesFilter2) {
  // Filter 2: new = sample + old*2 + ((-old*3) SAR 5) - older + (older SAR 4)
  // (line 2733). shift 8 seeds old=896; sample 1 = 0 + 1792 + (-2688>>5) = 1708.
  const BrrBlock b = decode({0x88, 0x70, 0, 0, 0, 0, 0, 0, 0});
  EXPECT_EQ(b.samples[0], 896);
  EXPECT_EQ(b.samples[1], 1708);
}

TEST(Brr, AppliesFilter3) {
  // Filter 3: new = sample + old*2 + ((-old*13) SAR 6) - older + ((older*3) SAR 4)
  // (line 2734). shift 8 seeds old=896; sample 1 = 0 + 1792 + (-11648>>6) = 1610.
  const BrrBlock b = decode({0x8C, 0x70, 0, 0, 0, 0, 0, 0, 0});
  EXPECT_EQ(b.samples[0], 896);
  EXPECT_EQ(b.samples[1], 1610);
}

// ── Clamp-to-16-then-clip-to-15 glitches ────────────────────────────────────

TEST(Brr, ClampsThenClipsLargeNegativeToZero) {
  // "If new<-8000h then new=-8000h (but, clipped to ZERO below)" — the
  // dirt-effect (lines 2740). Filter 3 with old=older'=-16384 and nibble -8:
  // -16384 + -32768 + (212992>>6) = -45824 -> clamp -32768 -> clip 0.
  const BrrBlock b = decode({0xCC, 0x80, 0, 0, 0, 0, 0, 0, 0}, -16384, 0);
  EXPECT_EQ(b.samples[0], 0);
}

TEST(Brr, FoldsPositiveOverflowToNegative) {
  // "If new=(+4000h..+7FFFh) then new=(-4000h..-1)" — lost-sign (line 2741).
  // Filter 1 with old=14336 and nibble +7: 14336 + 14336 + (-14336>>4) = 27776
  // (0x6C80) -> clip ((v & 0x7FFF) ^ 0x4000) - 0x4000 = -4992.
  const BrrBlock b = decode({0xC4, 0x70, 0, 0, 0, 0, 0, 0, 0}, 14336, 0);
  EXPECT_EQ(b.samples[0], -4992);
}

// ── Filter history carries into the next block ──────────────────────────────

TEST(Brr, CarriesFilterHistoryToTheNextBlock) {
  // The 15-bit output is "re-used for the next 1-2 sample(s) as older=old,
  // old=new" (lines 2745-2746). A block of all +7 (Filter 0) leaves old=older
  // =14336; those are exactly its last two samples.
  const BrrBlock a = decode({0xC0, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77});
  EXPECT_EQ(a.last, a.samples[15]);
  EXPECT_EQ(a.prev, a.samples[14]);
  EXPECT_EQ(a.last, 14336);
  EXPECT_EQ(a.prev, 14336);

  // A following Filter 1 block of all-zero nibbles reads that history: sample 0
  // = 0 + 14336 + (-14336>>4) = 13440, where a fresh 0/0 history would give 0.
  const std::array<std::uint8_t, 9> next{0xC4, 0, 0, 0, 0, 0, 0, 0, 0};
  EXPECT_EQ(decode(next, a.last, a.prev).samples[0], 13440);
  EXPECT_EQ(decode(next, 0, 0).samples[0], 0);
}

// ── Loop/End flags ──────────────────────────────────────────────────────────

TEST(Brr, DecodesLoopAndEndFlags) {
  // Header bits 1-0 (lines 2709-2714): 0 Normal, 1 End+Mute, 2 Ignored,
  // 3 End+Loop. endBlock is bit 0 (codes 1 and 3), loopBlock is bit 1.
  auto flags = [](std::uint8_t code) {
    return decode({static_cast<std::uint8_t>(0xC0 | code), 0, 0, 0, 0, 0, 0, 0, 0});
  };
  EXPECT_FALSE(flags(0).endBlock);
  EXPECT_FALSE(flags(0).loopBlock);
  EXPECT_TRUE(flags(1).endBlock);   // End+Mute
  EXPECT_FALSE(flags(1).loopBlock);
  EXPECT_FALSE(flags(2).endBlock);  // Ignored: same as Normal
  EXPECT_TRUE(flags(2).loopBlock);
  EXPECT_TRUE(flags(3).endBlock);   // End+Loop
  EXPECT_TRUE(flags(3).loopBlock);
}

// ── Sample directory ────────────────────────────────────────────────────────

TEST(Brr, ReadsDirectoryEntry) {
  // The table is based at DIR*$100; entry SRCN is at +SRCN*4 as little-endian
  // start then loop addresses (lines 2682-2692).
  std::array<std::uint8_t, 65536> ram{};
  // DIR=$02 -> table at $0200; SRCN 1 -> entry at $0204.
  ram[0x0204] = 0x34;
  ram[0x0205] = 0x12;  // start = $1234
  ram[0x0206] = 0x78;
  ram[0x0207] = 0x56;  // loop  = $5678
  const BrrSource s = readBrrSource(ram, 0x02, 1);
  EXPECT_EQ(s.start, 0x1234);
  EXPECT_EQ(s.loop, 0x5678);
}

TEST(Brr, DirectoryAddressWrapsWithin64K) {
  // DIR=$FF, SRCN=$40 -> $FF00 + $100 = $10000, which wraps to $0000.
  std::array<std::uint8_t, 65536> ram{};
  ram[0x0000] = 0xEF;
  ram[0x0001] = 0xBE;  // start = $BEEF
  ram[0x0002] = 0x0D;
  ram[0x0003] = 0xF0;  // loop  = $F00D
  const BrrSource s = readBrrSource(ram, 0xFF, 0x40);
  EXPECT_EQ(s.start, 0xBEEF);
  EXPECT_EQ(s.loop, 0xF00D);
}

}  // namespace
