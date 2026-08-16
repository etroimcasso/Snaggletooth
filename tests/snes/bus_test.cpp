// The SNES bus: the LoROM memory map, work RAM and its data port, the APU
// communication ports, open bus, and the region-by-region master-cycle cost.
// Programs run from the cartridge and their effects are read back from the machine
// state; instruction costs are read from step()'s return.

#include <cstdint>
#include <initializer_list>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "snaggletooth/snes/snes.h"

namespace snaggletooth {
namespace {

// Builds a machine from a raw LoROM image, pointing the reset vector at $8000
// unless the image already sets it. iplStub defaults on; a test that reads the APU
// output ports on the first instruction turns it off, so the ports carry the seeded
// ready bytes rather than waiting for the stub to post them.
Snes machineFromRom(std::vector<std::uint8_t> rom, bool iplStub = true) {
  if (rom.size() < 0x8000u) rom.resize(0x8000u, 0x00u);
  if (rom[0x7FFCu] == 0 && rom[0x7FFDu] == 0) {
    rom[0x7FFCu] = 0x00u;  // reset vector -> $8000
    rom[0x7FFDu] = 0x80u;
  }
  return Snes(SnesConfig{.rom = rom, .iplStub = iplStub});
}

// Builds a machine whose cartridge runs `program` from $8000.
Snes machineWith(std::initializer_list<std::uint8_t> program, bool iplStub = true) {
  std::vector<std::uint8_t> rom(program.begin(), program.end());
  return machineFromRom(std::move(rom), iplStub);
}

// Steps until the CPU halts, so a program ending in STP settles.
void runToStop(Snes& m, int cap = 200) {
  for (int i = 0; i < cap && m.state().cpu.run == CpuRunState::Running; ++i) m.step();
}

std::uint8_t loByte(std::uint16_t v) { return static_cast<std::uint8_t>(v & 0xFFu); }

// ---- work RAM -------------------------------------------------------------

TEST(SnesBus, DirectPageStoreReachesWorkRam) {
  Snes m = machineWith({0xA9, 0x42, 0x85, 0x10, 0xDB});  // LDA #$42; STA $10; STP
  runToStop(m);
  EXPECT_EQ(m.state().wram[0x10], 0x42u);
}

TEST(SnesBus, LongStoreReachesWorkRamBank7E) {
  Snes m = machineWith({0xA9, 0x99, 0x8F, 0x34, 0x12, 0x7E, 0xDB});  // LDA #$99; STA $7E1234; STP
  runToStop(m);
  EXPECT_EQ(m.state().wram[0x1234], 0x99u);
}

TEST(SnesBus, LongStoreReachesWorkRamBank7F) {
  Snes m = machineWith({0xA9, 0x77, 0x8F, 0x01, 0x00, 0x7F, 0xDB});  // LDA #$77; STA $7F0001; STP
  runToStop(m);
  EXPECT_EQ(m.state().wram[0x10001], 0x77u);
}

TEST(SnesBus, SystemBankLowPageMirrorsBank7E) {
  // A store through the $0000-$1FFF mirror and a long read of $7E:xxxx name the
  // same work-RAM byte.
  Snes m = machineWith({0xA9, 0x5A, 0x85, 0x05,        // LDA #$5A; STA $05
                        0xAF, 0x05, 0x00, 0x7E,        // LDA $7E0005
                        0xDB});                        // STP
  runToStop(m);
  EXPECT_EQ(m.state().wram[0x05], 0x5Au);
  EXPECT_EQ(loByte(m.state().cpu.a), 0x5Au);
}

// ---- cartridge ------------------------------------------------------------

TEST(SnesBus, AbsoluteReadReachesCartridge) {
  std::vector<std::uint8_t> rom(0x8000u, 0x00u);
  rom[0] = 0xADu; rom[1] = 0x10u; rom[2] = 0x80u;  // LDA $8010
  rom[3] = 0xDBu;                                   // STP
  rom[0x10u] = 0xC3u;                               // the byte at $00:$8010
  Snes m = machineFromRom(std::move(rom));
  runToStop(m);
  EXPECT_EQ(loByte(m.state().cpu.a), 0xC3u);
}

TEST(SnesBus, Bank80MirrorsBank00InLoRom) {
  std::vector<std::uint8_t> rom(0x8000u, 0x00u);
  rom[0] = 0xADu; rom[1] = 0x00u; rom[2] = 0x81u;  // LDA $8100
  rom[3] = 0x85u; rom[4] = 0x00u;                  // STA $00
  rom[5] = 0xAFu; rom[6] = 0x00u; rom[7] = 0x81u; rom[8] = 0x80u;  // LDA $808100
  rom[9] = 0xDBu;                                   // STP
  rom[0x100u] = 0x5Cu;                              // the byte both addresses reach
  Snes m = machineFromRom(std::move(rom));
  runToStop(m);
  EXPECT_EQ(m.state().wram[0x00], 0x5Cu);       // $00:$8100
  EXPECT_EQ(loByte(m.state().cpu.a), 0x5Cu);    // $80:$8100 mirrors it
}

TEST(SnesBus, WriteToCartridgeHasNoEffect) {
  std::vector<std::uint8_t> rom(0x8000u, 0x00u);
  rom[0] = 0xA9u; rom[1] = 0xFFu;                  // LDA #$FF
  rom[2] = 0x8Du; rom[3] = 0x20u; rom[4] = 0x80u;  // STA $8020
  rom[5] = 0xADu; rom[6] = 0x20u; rom[7] = 0x80u;  // LDA $8020
  rom[8] = 0xDBu;                                   // STP
  rom[0x20u] = 0x33u;                               // the cartridge byte the store targets, clear of the program
  Snes m = machineFromRom(std::move(rom));
  runToStop(m);
  EXPECT_EQ(loByte(m.state().cpu.a), 0x33u);  // the ROM byte, not the $FF the store tried to write
}

// ---- APU communication ports ---------------------------------------------

TEST(SnesBus, ReadingApuPortsReturnsTheReadyBytes) {
  // The ports are read on the first instruction, before an upload stub could post,
  // so the seeded ready state is the fixture under test.
  Snes m0 = machineWith({0xAD, 0x40, 0x21, 0xDB}, /*iplStub=*/false);  // LDA $2140; STP
  runToStop(m0);
  EXPECT_EQ(loByte(m0.state().cpu.a), 0xAAu);       // output port 0 posts $AA

  Snes m1 = machineWith({0xAD, 0x41, 0x21, 0xDB}, /*iplStub=*/false);  // LDA $2141; STP
  runToStop(m1);
  EXPECT_EQ(loByte(m1.state().cpu.a), 0xBBu);       // output port 1 posts $BB
}

TEST(SnesBus, WritingApuPortReachesTheInputLatch) {
  Snes m = machineWith({0xA9, 0x3C, 0x8D, 0x40, 0x21, 0xDB});  // LDA #$3C; STA $2140; STP
  runToStop(m);
  EXPECT_EQ(m.state().apu.inputPorts[0], 0x3Cu);
}

TEST(SnesBus, ApuPortsMirrorThrough217F) {
  // The read of the output-port mirror runs first, so the seeded ready state stands
  // in for a stub that has not yet posted.
  Snes m0 = machineWith({0xAD, 0x44, 0x21, 0xDB}, /*iplStub=*/false);  // LDA $2144 -> mirror of $2140
  runToStop(m0);
  EXPECT_EQ(loByte(m0.state().cpu.a), 0xAAu);

  Snes m1 = machineWith({0xA9, 0x5E, 0x8D, 0x7C, 0x21, 0xDB});  // STA $217C -> mirror of $2140
  runToStop(m1);
  EXPECT_EQ(m1.state().apu.inputPorts[0], 0x5Eu);
}

// ---- work-RAM data port ---------------------------------------------------

TEST(SnesBus, WorkRamPortWritesAndAutoIncrements) {
  Snes m = machineWith({0xA9, 0x00, 0x8D, 0x81, 0x21,   // LDA #$00; STA $2181 (addr low)
                        0xA9, 0x01, 0x8D, 0x82, 0x21,   // LDA #$01; STA $2182 (addr mid)
                        0xA9, 0x00, 0x8D, 0x83, 0x21,   // LDA #$00; STA $2183 (addr high) -> $00100
                        0xA9, 0xAB, 0x8D, 0x80, 0x21,   // LDA #$AB; STA $2180
                        0xA9, 0xCD, 0x8D, 0x80, 0x21,   // LDA #$CD; STA $2180
                        0xDB});
  runToStop(m);
  EXPECT_EQ(m.state().wram[0x100], 0xABu);
  EXPECT_EQ(m.state().wram[0x101], 0xCDu);
  EXPECT_EQ(m.state().wmadd, 0x102u);
}

TEST(SnesBus, WorkRamPortReadsAndAutoIncrements) {
  Snes m = machineWith({0xA9, 0x00, 0x8D, 0x81, 0x21,   // set port addr low  $00
                        0xA9, 0x02, 0x8D, 0x82, 0x21,   // set port addr mid  $02 -> $00200
                        0xA9, 0x12, 0x8D, 0x80, 0x21,   // STA $2180 -> wram[$200]=$12
                        0xA9, 0x34, 0x8D, 0x80, 0x21,   // STA $2180 -> wram[$201]=$34
                        0xA9, 0x00, 0x8D, 0x81, 0x21,   // reset port addr to $00200
                        0xA9, 0x02, 0x8D, 0x82, 0x21,
                        0xAD, 0x80, 0x21, 0x85, 0x10,   // LDA $2180; STA $10
                        0xAD, 0x80, 0x21, 0x85, 0x11,   // LDA $2180; STA $11
                        0xDB});
  runToStop(m);
  EXPECT_EQ(m.state().wram[0x10], 0x12u);
  EXPECT_EQ(m.state().wram[0x11], 0x34u);
  EXPECT_EQ(m.state().wmadd, 0x202u);
}

// ---- open bus -------------------------------------------------------------

TEST(SnesBus, UnmappedReadReturnsTheDataBusLatch) {
  // $2100 is a PPU register the machine does not map yet; a read of it returns the
  // last value the data bus carried, which is the high byte of the address the
  // absolute read just fetched.
  Snes m = machineWith({0xAD, 0x00, 0x21, 0xDB});  // LDA $2100; STP
  runToStop(m);
  EXPECT_EQ(loByte(m.state().cpu.a), 0x21u);
}

// ---- region-by-region master-cycle cost -----------------------------------
// Every instruction below fetches from $8000 (the first LoROM region, eight
// master cycles per access); the varying term is the single data access, so
// step()'s total pins that region's cost.

TEST(SnesBus, WorkRamAccessCostsEightMasterCycles) {
  Snes m = machineWith({0xAD, 0x10, 0x00});  // LDA $0010 -> work-RAM mirror
  EXPECT_EQ(m.step(), 8u + 8u + 8u + 8u);     // fetch + two operands + data
}

TEST(SnesBus, RegisterAccessCostsSixMasterCycles) {
  Snes m = machineWith({0xAD, 0x40, 0x21});  // LDA $2140 -> APU port, fast region
  EXPECT_EQ(m.step(), 8u + 8u + 8u + 6u);
}

TEST(SnesBus, JoypadAccessCostsTwelveMasterCycles) {
  Snes m = machineWith({0xAD, 0x00, 0x40});  // LDA $4000 -> manual joypad region
  EXPECT_EQ(m.step(), 8u + 8u + 8u + 12u);
}

TEST(SnesBus, ExpansionAccessCostsEightMasterCycles) {
  Snes m = machineWith({0xAD, 0x00, 0x60});  // LDA $6000 -> expansion region
  EXPECT_EQ(m.step(), 8u + 8u + 8u + 8u);
}

TEST(SnesBus, InternalCycleCostsSixMasterCycles) {
  Snes m = machineWith({0xAA});               // TAX: an opcode fetch and one internal cycle
  EXPECT_EQ(m.step(), 8u + 6u);
}

TEST(SnesBus, SecondRegionFollowsMemsel) {
  // The default slow rate for $C0-$FF is eight master cycles per access.
  Snes slow = machineWith({0xAF, 0x00, 0x80, 0xC0});  // LDA $C08000
  EXPECT_EQ(slow.step(), 8u + 8u + 8u + 8u + 8u);

  // With MEMSEL set, the same region runs at six.
  Snes fast = machineWith({0xA9, 0x01, 0x8D, 0x0D, 0x42,  // LDA #$01; STA $420D (MEMSEL)
                           0xAF, 0x00, 0x80, 0xC0});       // LDA $C08000
  fast.step();  // LDA #$01
  fast.step();  // STA $420D
  EXPECT_EQ(fast.step(), 8u + 8u + 8u + 8u + 6u);
}

}  // namespace
}  // namespace snaggletooth
