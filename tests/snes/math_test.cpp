// The SNES hardware multiply and divide unit: the multiplicand/multiplier and
// dividend/divisor ports, the fixed cycle latency before a result lands, the shared
// unit's documented quirks, and division by zero. Programs poke the ports the way a
// game does; the results are read back from the machine state.

#include <cstdint>
#include <vector>

#include "gtest/gtest.h"
#include "snaggletooth/snes/snes.h"

namespace snaggletooth {
namespace {

// Runs a cartridge that ends in STP, then lets the machine idle long enough for any
// in-flight multiply or divide to finish.
Snes settle(std::vector<std::uint8_t> rom) {
  rom.resize(0x8000u, 0x00u);
  rom[0x7FFCu] = 0x00u;  // reset -> $8000
  rom[0x7FFDu] = 0x80u;
  Snes m(SnesConfig{.rom = rom});
  while (m.state().cpu.run == CpuRunState::Running) m.step();
  m.run(200u);  // idle cycles carry the arithmetic countdown to completion
  return m;
}

// ---- multiply -------------------------------------------------------------

TEST(SnesMath, MultiplyProducesTheProduct) {
  // 7 * 9 = 63.
  Snes m = settle({
      0xA9, 0x07,        // LDA #7
      0x8D, 0x02, 0x42,  // STA $4202 (WRMPYA)
      0xA9, 0x09,        // LDA #9
      0x8D, 0x03, 0x42,  // STA $4203 (WRMPYB -> start multiply)
      0xDB,              // STP
  });
  EXPECT_EQ(m.state().rdmpy, 63u);
}

TEST(SnesMath, MultiplyUsesTheFullEightBitRange) {
  // 255 * 255 = 65025 = $FE01.
  Snes m = settle({
      0xA9, 0xFF, 0x8D, 0x02, 0x42,  // WRMPYA = $FF
      0xA9, 0xFF, 0x8D, 0x03, 0x42,  // WRMPYB = $FF
      0xDB,
  });
  EXPECT_EQ(m.state().rdmpy, 0xFE01u);
}

TEST(SnesMath, StartingAMultiplyLoadsTheQuotientRegisterWithTheMultiplier) {
  // The shared unit sets RDDIVL = WRMPYB (and RDDIVH = 0) the moment a multiply
  // starts — a documented quirk that survives the multiply.
  Snes m = settle({
      0xA9, 0x07, 0x8D, 0x02, 0x42,  // WRMPYA = 7
      0xA9, 0x2A, 0x8D, 0x03, 0x42,  // WRMPYB = $2A
      0xDB,
  });
  EXPECT_EQ(m.state().rddiv, 0x002Au);  // low = the multiplier, high = 0
  EXPECT_EQ(m.state().rdmpy, 7u * 0x2Au);
}

TEST(SnesMath, TheMultiplyResultIsNotReadyBeforeItsLatency) {
  // A read taken right after the start, before the eight-cycle latency, sees the old
  // product (zero at reset); a read after enough cycles sees the new one. If the
  // multiply were instantaneous the two reads would agree.
  std::vector<std::uint8_t> rom(0x8000u, 0x00u);
  rom[0x0000] = 0xA9; rom[0x0001] = 0xFF; rom[0x0002] = 0x8D; rom[0x0003] = 0x02; rom[0x0004] = 0x42;  // WRMPYA=$FF
  rom[0x0005] = 0xA9; rom[0x0006] = 0x10; rom[0x0007] = 0x8D; rom[0x0008] = 0x03; rom[0x0009] = 0x42;  // WRMPYB=$10 (start; $FF*$10=$0FF0)
  rom[0x000A] = 0xAD; rom[0x000B] = 0x16; rom[0x000C] = 0x42;  // LDA $4216 (early: ~4 cycles in, < 8)
  rom[0x000D] = 0x85; rom[0x000E] = 0x40;                      // STA $40
  rom[0x000F] = 0xEA; rom[0x0010] = 0xEA; rom[0x0011] = 0xEA;  // NOP NOP NOP
  rom[0x0012] = 0xEA; rom[0x0013] = 0xEA;                      // NOP NOP  (well past 8 cycles now)
  rom[0x0014] = 0xAD; rom[0x0015] = 0x16; rom[0x0016] = 0x42;  // LDA $4216 (late)
  rom[0x0017] = 0x85; rom[0x0018] = 0x41;                      // STA $41
  rom[0x0019] = 0xDB;                                          // STP
  rom[0x7FFC] = 0x00; rom[0x7FFD] = 0x80;
  Snes m(SnesConfig{.rom = rom});
  while (m.state().cpu.run == CpuRunState::Running) m.step();

  EXPECT_EQ(m.state().wram[0x40], 0x00u);  // early read: product not yet landed
  EXPECT_EQ(m.state().wram[0x41], 0xF0u);  // late read: low byte of $0FF0
}

// ---- divide ---------------------------------------------------------------

TEST(SnesMath, DivideProducesQuotientAndRemainder) {
  // 1000 / 7 = 142 remainder 6.  1000 = $03E8.
  Snes m = settle({
      0xA9, 0xE8, 0x8D, 0x04, 0x42,  // WRDIVL = $E8
      0xA9, 0x03, 0x8D, 0x05, 0x42,  // WRDIVH = $03
      0xA9, 0x07, 0x8D, 0x06, 0x42,  // WRDIVB = 7 (start divide)
      0xDB,
  });
  EXPECT_EQ(m.state().rddiv, 142u);  // quotient
  EXPECT_EQ(m.state().rdmpy, 6u);    // remainder
}

TEST(SnesMath, DivideByZeroReturnsAllOnesAndTheDividend) {
  Snes m = settle({
      0xA9, 0x34, 0x8D, 0x04, 0x42,  // WRDIVL = $34
      0xA9, 0x12, 0x8D, 0x05, 0x42,  // WRDIVH = $12  (dividend $1234)
      0xA9, 0x00, 0x8D, 0x06, 0x42,  // WRDIVB = 0 (start divide by zero)
      0xDB,
  });
  EXPECT_EQ(m.state().rddiv, 0xFFFFu);  // quotient is all ones
  EXPECT_EQ(m.state().rdmpy, 0x1234u);  // remainder is the dividend
}

TEST(SnesMath, TheDivideResultIsNotReadyBeforeItsLatency) {
  // Divide takes sixteen cycles; a read four cycles in still sees the prior quotient.
  std::vector<std::uint8_t> rom(0x8000u, 0x00u);
  rom[0x0000] = 0xA9; rom[0x0001] = 0xE8; rom[0x0002] = 0x8D; rom[0x0003] = 0x04; rom[0x0004] = 0x42;  // WRDIVL=$E8
  rom[0x0005] = 0xA9; rom[0x0006] = 0x03; rom[0x0007] = 0x8D; rom[0x0008] = 0x05; rom[0x0009] = 0x42;  // WRDIVH=$03
  rom[0x000A] = 0xA9; rom[0x000B] = 0x07; rom[0x000C] = 0x8D; rom[0x000D] = 0x06; rom[0x000E] = 0x42;  // WRDIVB=7 (start)
  rom[0x000F] = 0xAD; rom[0x0010] = 0x14; rom[0x0011] = 0x42;  // LDA $4214 (early)
  rom[0x0012] = 0x85; rom[0x0013] = 0x40;                      // STA $40
  rom[0x0014] = 0xDB;                                          // STP
  rom[0x7FFC] = 0x00; rom[0x7FFD] = 0x80;
  Snes m(SnesConfig{.rom = rom});
  // Step through the store of the early read (eight instructions: six of setup, the
  // read, the store). The read lands ~4 cycles after the divide starts, well inside
  // the sixteen-cycle latency.
  for (int i = 0; i < 8; ++i) m.step();
  EXPECT_EQ(m.state().wram[0x40], 0x00u);  // quotient not yet landed (reset value)

  m.run(200u);  // now let it finish
  EXPECT_EQ(m.state().rddiv, 142u);
}

}  // namespace
}  // namespace snaggletooth
