#pragma once

// Test-only support shared by the run's contract files: two observations held
// equal field for field, the one input script every example is run under, and
// the two cartridges the reset line is run through.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "examples/example_cartridges.h"
#include "gtest/gtest.h"
#include "rom/input_script.h"
#include "rom/rom_observe.h"
#include "rom/rom_observe_internal.h"
#include "snaggletooth/apu/apu.h"

namespace snaggletooth::disasm {

inline void expectSameLanding(const PortLanding& a, const PortLanding& b, const std::string& what) {
  EXPECT_EQ(a.memory, b.memory) << what;
  EXPECT_EQ(a.lowest, b.lowest) << what;
  EXPECT_EQ(a.highest, b.highest) << what;
  EXPECT_EQ(a.shown, b.shown) << what;
  EXPECT_EQ(a.areas, b.areas) << what;
  EXPECT_EQ(a.depths, b.depths) << what;
  EXPECT_EQ(a.palette, b.palette) << what;
}

// Every field of two observations, in the order `RunObservation` declares them.
inline void expectSameObservation(const RunObservation& a, const RunObservation& b, const std::string& what) {
  ASSERT_EQ(a.reached.size(), b.reached.size()) << what;
  for (std::size_t i = 0; i < a.reached.size(); ++i) {
    EXPECT_TRUE(sameSighting(a.reached[i], b.reached[i])) << what << " reached " << i;
    EXPECT_EQ(a.reached[i].call, b.reached[i].call) << what;
    EXPECT_EQ(a.reached[i].name, b.reached[i].name) << what;
  }
  ASSERT_EQ(a.moved.size(), b.moved.size()) << what;
  for (std::size_t i = 0; i < a.moved.size(); ++i) {
    EXPECT_EQ(detail::keyOf(a.moved[i]), detail::keyOf(b.moved[i])) << what << " moved " << i;
    EXPECT_EQ(a.moved[i].times, b.moved[i].times) << what << " moved " << i;
  }
  ASSERT_EQ(a.landed.size(), b.landed.size()) << what;
  for (std::size_t i = 0; i < a.landed.size(); ++i) {
    EXPECT_EQ(detail::keyOf(a.landed[i]), detail::keyOf(b.landed[i])) << what << " landed " << i;
    EXPECT_EQ(a.landed[i].landing.palette, b.landed[i].landing.palette) << what << " landed " << i;
    EXPECT_EQ(a.landed[i].times, b.landed[i].times) << what << " landed " << i;
  }
  ASSERT_EQ(a.walked.size(), b.walked.size()) << what;
  for (std::size_t i = 0; i < a.walked.size(); ++i) {
    EXPECT_EQ(detail::keyOf(a.walked[i]), detail::keyOf(b.walked[i])) << what << " walked " << i;
    EXPECT_EQ(a.walked[i].times, b.walked[i].times) << what << " walked " << i;
  }
  EXPECT_EQ(a.palettes, b.palettes) << what;
  ASSERT_EQ(a.ran.size(), b.ran.size()) << what;
  for (std::size_t i = 0; i < a.ran.size(); ++i) {
    EXPECT_TRUE(sameLanding(a.ran[i], b.ran[i])) << what << " ran " << i;
    EXPECT_EQ(a.ran[i].name, b.ran[i].name) << what;
  }
  ASSERT_EQ(a.seen.size(), b.seen.size()) << what;
  for (std::size_t i = 0; i < a.seen.size(); ++i) {
    EXPECT_EQ(a.seen[i].address, b.seen[i].address) << what;
    EXPECT_EQ(a.seen[i].d, b.seen[i].d) << what;
    EXPECT_EQ(a.seen[i].dbr, b.seen[i].dbr) << what;
  }
  ASSERT_EQ(a.staged.size(), b.staged.size()) << what;
  for (std::size_t i = 0; i < a.staged.size(); ++i) {
    const StagedRange& x = a.staged[i];
    const StagedRange& y = b.staged[i];
    EXPECT_EQ(x.memory, y.memory) << what << " staged " << i;
    EXPECT_EQ(x.bytes, y.bytes) << what << " staged " << i;
    EXPECT_EQ(x.origin, y.origin) << what << " staged " << i;
    ASSERT_EQ(x.writers.size(), y.writers.size()) << what << " staged " << i;
    for (std::size_t w = 0; w < x.writers.size(); ++w) {
      EXPECT_EQ(x.writers[w].writer, y.writers[w].writer) << what << " staged " << i;
      EXPECT_EQ(x.writers[w].unwritten, y.writers[w].unwritten) << what << " staged " << i;
      EXPECT_EQ(x.writers[w].bytes, y.writers[w].bytes) << what << " staged " << i;
      EXPECT_EQ(x.writers[w].origin, y.writers[w].origin) << what << " staged " << i;
      EXPECT_EQ(x.writers[w].sources, y.writers[w].sources) << what << " staged " << i;
    }
    ASSERT_EQ(x.contents.size(), y.contents.size()) << what << " staged " << i;
    for (std::size_t c = 0; c < x.contents.size(); ++c) {
      const CarriedContent& p = x.contents[c];
      const CarriedContent& q = y.contents[c];
      EXPECT_TRUE(sameContent(p, q)) << what << " staged " << i << " content " << c;
      EXPECT_EQ(p.origin, q.origin) << what << " staged " << i << " content " << c;
      ASSERT_EQ(p.landing.has_value(), q.landing.has_value()) << what;
      if (p.landing) expectSameLanding(*p.landing, *q.landing, what);
      ASSERT_EQ(p.byteOrigins.size(), q.byteOrigins.size()) << what;
      for (std::size_t k = 0; k < p.byteOrigins.size(); ++k) {
        EXPECT_EQ(p.byteOrigins[k].bytes, q.byteOrigins[k].bytes) << what;
        EXPECT_EQ(p.byteOrigins[k].origin, q.byteOrigins[k].origin) << what;
      }
      EXPECT_EQ(p.order, q.order) << what << " staged " << i << " content " << c;
    }
  }
  ASSERT_EQ(a.streamed.size(), b.streamed.size()) << what;
  for (std::size_t i = 0; i < a.streamed.size(); ++i) {
    EXPECT_EQ(detail::keyOf(a.streamed[i]), detail::keyOf(b.streamed[i])) << what << " streamed " << i;
    EXPECT_EQ(a.streamed[i].times, b.streamed[i].times) << what << " streamed " << i;
    EXPECT_EQ(a.streamed[i].source, b.streamed[i].source) << what << " streamed " << i;
  }
  ASSERT_EQ(a.samples.size(), b.samples.size()) << what;
  for (std::size_t i = 0; i < a.samples.size(); ++i) {
    EXPECT_EQ(a.samples[i].start, b.samples[i].start) << what;
    EXPECT_EQ(a.samples[i].loop, b.samples[i].loop) << what;
    EXPECT_EQ(a.samples[i].bytes, b.samples[i].bytes) << what;
    EXPECT_EQ(a.samples[i].times, b.samples[i].times) << what;
  }
  EXPECT_EQ(a.instructions, b.instructions) << what;
  EXPECT_EQ(a.interrupts, b.interrupts) << what;
  EXPECT_EQ(a.nodes, b.nodes) << what;
  EXPECT_EQ(a.divergences, b.divergences) << what;
  EXPECT_EQ(a.spc700Instructions, b.spc700Instructions) << what;
  EXPECT_EQ(a.spc700Nodes, b.spc700Nodes) << what;
  EXPECT_EQ(a.spc700Divergences, b.spc700Divergences) << what;
  EXPECT_EQ(a.originSets, b.originSets) << what;
  EXPECT_EQ(a.originCap, b.originCap) << what;
  // A host's run runs no routine again, so neither side holds an outcome to compare.
  EXPECT_EQ(a.enumerated.size(), b.enumerated.size()) << what;
  EXPECT_EQ(a.variantsPlanned, b.variantsPlanned) << what;
  EXPECT_EQ(a.variantsRun, b.variantsRun) << what;
  EXPECT_EQ(a.variantsReturned, b.variantsReturned) << what;
  EXPECT_EQ(a.variantsConfined, b.variantsConfined) << what;
  EXPECT_EQ(a.enumerationCycles, b.enumerationCycles) << what;
}

// One script for every example: buttons pressed and let go over the first
// frames, so a cartridge that reads the pads takes its paths. No example has a
// script of its own.
inline InputScript exampleScript() {
  std::string error;
  const std::optional<InputScript> parsed =
      parseInputScript("frame 2 1 b\nframe 4 1 none\nframe 6 1 start\nframe 8 1 start b\nframe 10 1 none\n", error);
  EXPECT_TRUE(parsed.has_value()) << error;
  return parsed.value_or(InputScript{});
}

// A cartridge for the reset line, in emulation mode throughout. Every pass
// starts a fill of sixty-four bytes from $9100 to VMDATAL on channel 0 and
// sets channel 1 up on a one-line HDMA table at $9200. A flag in work RAM,
// which a reset keeps, says which pass it is. The first enables the table from
// $8049, sets the flag and calls a helper that loads Y from $9007, reads
// $9000-$900F and spins; the pass after the reset enables the table from $8057,
// stores Y to VMDATAL, then $9008-$900A one at a time, a stream, and spins.
inline std::vector<std::uint8_t> resettingImage() {
  std::vector<std::uint8_t> rom = examples::loRomImage(1);
  examples::put(rom, 0x0000u, {
      0xA9u, 0x08u, 0x8Du, 0x00u, 0x43u,  // $8000 DMAP0 = $08: A->B, fixed source, pattern 0
      0xA9u, 0x18u, 0x8Du, 0x01u, 0x43u,  // $8005 BBAD0 = $18: VMDATAL
      0xA9u, 0x00u, 0x8Du, 0x02u, 0x43u,  // $800A A1T0 low
      0xA9u, 0x91u, 0x8Du, 0x03u, 0x43u,  // $800F A1T0 high: $9100
      0xA9u, 0x00u, 0x8Du, 0x04u, 0x43u,  // $8014 A1B0 = $00
      0xA9u, 0x40u, 0x8Du, 0x05u, 0x43u,  // $8019 DAS0 low: 64
      0xA9u, 0x00u, 0x8Du, 0x06u, 0x43u,  // $801E DAS0 high
      0xA9u, 0x01u, 0x8Du, 0x0Bu, 0x42u,  // $8023 MDMAEN = $01 (the write at $8025)
      0xA9u, 0x00u, 0x8Du, 0x10u, 0x43u,  // $8028 DMAP1 = $00: direct, pattern 0
      0xA9u, 0x32u, 0x8Du, 0x11u, 0x43u,  // $802D BBAD1 = $32: COLDATA
      0xA9u, 0x00u, 0x8Du, 0x12u, 0x43u,  // $8032 A1T1 low
      0xA9u, 0x92u, 0x8Du, 0x13u, 0x43u,  // $8037 A1T1 high: $9200
      0xA9u, 0x00u, 0x8Du, 0x14u, 0x43u,  // $803C A1B1 = $00
      0xAFu, 0x00u, 0x00u, 0x7Eu,         // $8041 LDA $7E:0000: the flag
      0xD0u, 0x0Eu,                       // $8045 BNE $8055
      0xA9u, 0x02u, 0x8Du, 0x0Cu, 0x42u,  // $8047 HDMAEN = $02 (the write at $8049)
      0xA9u, 0x01u,                       // $804C LDA #$01
      0x8Fu, 0x00u, 0x00u, 0x7Eu,         // $804E STA $7E:0000
      0x20u, 0x80u, 0x80u,                // $8052 JSR $8080
      0xA9u, 0x02u, 0x8Du, 0x0Cu, 0x42u,  // $8055 HDMAEN = $02 (the write at $8057)
      0x8Cu, 0x18u, 0x21u,                // $805A STY $2118: Y as the reset left it
      0xADu, 0x08u, 0x90u,                // $805D LDA $9008
      0x8Du, 0x18u, 0x21u,                // $8060 STA $2118
      0xADu, 0x09u, 0x90u,                // $8063 LDA $9009
      0x8Du, 0x18u, 0x21u,                // $8066 STA $2118
      0xADu, 0x0Au, 0x90u,                // $8069 LDA $900A
      0x8Du, 0x18u, 0x21u,                // $806C STA $2118
      0x80u, 0xFEu,                       // $806F BRA $806F
  });
  examples::put(rom, 0x0080u, {
      0xACu, 0x07u, 0x90u, // $8080 LDY $9007
      0xA2u, 0x00u,        // $8083 LDX #$00
      0xBDu, 0x00u, 0x90u, // $8085 LDA $9000,X
      0xE8u,               // $8088 INX
      0xE0u, 0x10u,        // $8089 CPX #$10
      0xD0u, 0xF8u,        // $808B BNE $8085
      0x80u, 0xFEu,        // $808D BRA $808D
  });
  for (std::size_t i = 0; i < 16; ++i) rom[0x1000u + i] = static_cast<std::uint8_t>(0x20u + i);
  rom[0x1100u] = 0x5Au;                   // the fill's byte
  examples::put(rom, 0x1200u, {0x01u, 0x1Fu, 0x00u});  // one line of $1F, then the table's end
  return rom;
}

// A sound boot program of the test's own, in place of the stub. It reads X
// before anything writes it. Before the reset, with the flag at $0300 clear,
// it writes a directory entry at $0400 naming a one-block sample at $0500,
// sets DIR to $04 and leaves the DSP's address at DIR, sets the flag, writes
// `BRA $FFC0` into the RAM under the window, sets X to $55, unmaps the window
// and spins there. After the reset, the window mapped again, it writes $04
// through $F3 to whatever register the address names, and keys voice 0 on
// without setting DIR.
inline std::array<std::uint8_t, kIplWindowBytes> resettingBootImage() {
  std::array<std::uint8_t, kIplWindowBytes> image{};
  const std::vector<std::uint8_t> code = {
      0x7Du,                    // $FFC0 MOV A,X
      0xE5u, 0x00u, 0x03u,      // $FFC1 MOV A,!$0300: the flag
      0xD0u, 0x28u,             // $FFC4 BNE $FFEE
      0xE8u, 0x05u,             // $FFC6 MOV A,#$05
      0xC5u, 0x01u, 0x04u,      // $FFC8 MOV !$0401,A: entry 0 starts at $0500
      0xC5u, 0x03u, 0x04u,      // $FFCB MOV !$0403,A: and loops there
      0xE8u, 0x01u,             // $FFCE MOV A,#$01
      0xC5u, 0x00u, 0x05u,      // $FFD0 MOV !$0500,A: a block with the end flag
      0xC5u, 0x00u, 0x03u,      // $FFD3 MOV !$0300,A: the flag
      0x8Fu, 0x5Du, 0xF2u,      // $FFD6 MOV $F2,#$5D: DIR
      0x8Fu, 0x04u, 0xF3u,      // $FFD9 MOV $F3,#$04: the directory at $0400
      0xE8u, 0x2Fu,             // $FFDC MOV A,#$2F
      0xC5u, 0xC0u, 0xFFu,      // $FFDE MOV !$FFC0,A
      0xE8u, 0xFEu,             // $FFE1 MOV A,#$FE
      0xC5u, 0xC1u, 0xFFu,      // $FFE3 MOV !$FFC1,A: BRA $FFC0 in the RAM beneath
      0xCDu, 0x55u,             // $FFE6 MOV X,#$55
      0x8Fu, 0x00u, 0xF1u,      // $FFE8 MOV $F1,#$00: the window unmapped
      0x5Fu, 0xC0u, 0xFFu,      // $FFEB JMP !$FFC0
      0x8Fu, 0x04u, 0xF3u,      // $FFEE MOV $F3,#$04
      0x8Fu, 0x4Cu, 0xF2u,      // $FFF1 MOV $F2,#$4C: KON
      0x8Fu, 0x01u, 0xF3u,      // $FFF4 MOV $F3,#$01: voice 0
      0x2Fu, 0xFEu,             // $FFF7 BRA $FFF7
  };
  std::copy(code.begin(), code.end(), image.begin());
  return image;
}

}  // namespace snaggletooth::disasm
