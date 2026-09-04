// The bus observer: every access the machine makes and every internal CPU cycle,
// reported in order with its address, value, direction, kind and source. The
// cases hold the report to what the core and the engines actually drive — the
// CPU's kinds cycle by cycle, a transfer's bytes in the engine's name, the
// work-RAM port's own accesses — and hold the machine to running exactly the same
// with no observer set, which is how it starts.

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <vector>

#include "gtest/gtest.h"
#include "snaggletooth/snes/snes.h"

namespace snaggletooth {
namespace {

// Everything the machine reports, in order: an access, or an internal cycle
// with its address and optional kind.
struct Event {
  bool internal = false;
  BusAccess access;
  std::uint32_t address = 0;
  std::optional<CycleKind> kind;
};

struct Recorder final : BusObserver {
  std::vector<Event> events;
  void access(const BusAccess& access) override {
    Event e;
    e.access = access;
    events.push_back(e);
  }
  void internal(std::uint32_t address, std::optional<CycleKind> kind) override {
    Event e;
    e.internal = true;
    e.address = address;
    e.kind = kind;
    events.push_back(e);
  }

  // How many CPU cycles the report holds: every CPU access plus every internal cycle.
  [[nodiscard]] std::size_t cpuCycles() const {
    std::size_t n = 0;
    for (const Event& e : events) {
      if (e.internal || e.access.source == AccessSource::Cpu) ++n;
    }
    return n;
  }
  [[nodiscard]] std::vector<BusAccess> accessesFrom(AccessSource source) const {
    std::vector<BusAccess> out;
    for (const Event& e : events) {
      if (!e.internal && e.access.source == source) out.push_back(e.access);
    }
    return out;
  }
};

// A machine running `program` from $8000 in a one-bank LoROM image.
Snes programMachine(std::initializer_list<std::uint8_t> program) {
  std::vector<std::uint8_t> rom(program.begin(), program.end());
  rom.resize(0x8000u, 0x00u);
  rom[0x7FFCu] = 0x00u;  // reset -> $8000
  rom[0x7FFDu] = 0x80u;
  return Snes(SnesConfig{.rom = rom});
}

constexpr std::uint8_t kLdaImm = 0xA9u;
constexpr std::uint8_t kLdaAbs = 0xADu;
constexpr std::uint8_t kStaAbs = 0x8Du;
constexpr std::uint8_t kIncA = 0x1Au;
constexpr std::uint8_t kIncAbs = 0xEEu;
constexpr std::uint8_t kNop = 0xEAu;
constexpr std::uint8_t kStp = 0xDBu;
constexpr std::uint8_t kBrk = 0x00u;
constexpr std::uint8_t kWai = 0xCBu;

// ---- the CPU's own accesses -----------------------------------------------------

TEST(SnesObserver, NoneIsSetAtPowerOn) {
  Snes m = programMachine({kNop, kStp});
  EXPECT_EQ(m.observer(), nullptr);
}

TEST(SnesObserver, ACpuReadIsReportedWithItsAddressValueKindAndSource) {
  Snes m = programMachine({kLdaAbs, 0x10u, 0x00u, kStp});  // LDA !$0010
  SnesState s = m.state();
  s.wram[0x10] = 0x5Au;
  m.restore(s);
  Recorder r;
  m.setObserver(&r);
  m.step();
  // An opcode fetch, two operand fetches, then the data read: four CPU cycles.
  ASSERT_EQ(r.events.size(), 4u);
  EXPECT_EQ(r.events[0].access.kind, CycleKind::OpcodeFetch);
  EXPECT_EQ(r.events[0].access.address, 0x008000u);
  EXPECT_EQ(int{r.events[0].access.value}, int{kLdaAbs});
  EXPECT_EQ(r.events[1].access.kind, CycleKind::OperandFetch);
  EXPECT_EQ(r.events[2].access.kind, CycleKind::OperandFetch);
  const BusAccess& data = r.events[3].access;
  EXPECT_FALSE(r.events[3].internal);
  EXPECT_EQ(data.address, 0x000010u);
  EXPECT_EQ(int{data.value}, 0x5A);
  EXPECT_FALSE(data.write);
  EXPECT_EQ(data.kind, CycleKind::DataRead);
  EXPECT_EQ(data.source, AccessSource::Cpu);
}

TEST(SnesObserver, ACpuWriteIsReportedWithTheValueItDrove) {
  Snes m = programMachine({kLdaImm, 0x77u, kStaAbs, 0x20u, 0x00u, kStp});
  Recorder r;
  m.setObserver(&r);
  m.step();  // LDA #$77
  r.events.clear();
  m.step();  // STA !$0020
  ASSERT_EQ(r.events.size(), 4u);
  const BusAccess& w = r.events[3].access;
  EXPECT_TRUE(w.write);
  EXPECT_EQ(w.address, 0x000020u);
  EXPECT_EQ(int{w.value}, 0x77);
  EXPECT_EQ(w.kind, CycleKind::DataWrite);
  EXPECT_EQ(w.source, AccessSource::Cpu);
}

TEST(SnesObserver, AnInternalCycleIsReportedWithItsAddressAndNoKind) {
  Snes m = programMachine({kIncA, kStp});
  Recorder r;
  m.setObserver(&r);
  m.step();  // INC A: the fetch, then one internal cycle
  ASSERT_EQ(r.events.size(), 2u);
  EXPECT_TRUE(r.events[1].internal);
  EXPECT_EQ(r.events[1].address, 0x008001u) << "the address the next fetch would use";
  EXPECT_FALSE(r.events[1].kind.has_value());
}

TEST(SnesObserver, AReadModifyWriteUnderEmulationWritesItsAddressTwice) {
  Snes m = programMachine({kIncAbs, 0x30u, 0x00u, kStp});  // INC !$0030
  SnesState s = m.state();
  s.wram[0x30] = 0x0Fu;
  m.restore(s);
  Recorder r;
  m.setObserver(&r);
  m.step();
  const std::vector<BusAccess> cpu = r.accessesFrom(AccessSource::Cpu);
  ASSERT_EQ(cpu.size(), 6u);  // three fetches, the read, the unmodified write, the write-back
  EXPECT_EQ(cpu[3].kind, CycleKind::RmwRead);
  EXPECT_EQ(int{cpu[3].value}, 0x0F);
  EXPECT_EQ(cpu[4].kind, CycleKind::RmwModifyWrite);
  EXPECT_TRUE(cpu[4].write);
  EXPECT_EQ(int{cpu[4].value}, 0x0F) << "the unmodified byte first";
  EXPECT_EQ(cpu[5].kind, CycleKind::RmwWrite);
  EXPECT_EQ(int{cpu[5].value}, 0x10);
  EXPECT_EQ(cpu[5].address, 0x000030u);
  EXPECT_EQ(r.cpuCycles(), 6u);
}

TEST(SnesObserver, AReadModifyWriteInNativeModeReportsTheModifyCycleAsInternal) {
  Snes m = programMachine({0x18u, 0xFBu,             // CLC ; XCE -> native
                           kIncAbs, 0x30u, 0x00u,     // INC !$0030 (eight bits: M stays set)
                           kStp});
  Recorder r;
  m.setObserver(&r);
  m.step();
  m.step();
  r.events.clear();
  m.step();  // INC !$0030
  ASSERT_EQ(r.events.size(), 6u);
  EXPECT_EQ(r.events[3].access.kind, CycleKind::RmwRead);
  EXPECT_TRUE(r.events[4].internal);
  ASSERT_TRUE(r.events[4].kind.has_value());
  EXPECT_EQ(*r.events[4].kind, CycleKind::RmwModify);
  EXPECT_EQ(r.events[4].address, 0x000030u);
  EXPECT_EQ(r.events[5].access.kind, CycleKind::RmwWrite);
}

TEST(SnesObserver, AVectorPullIsReportedAsOne) {
  std::vector<std::uint8_t> rom(0x8000u, 0x00u);
  rom[0] = kBrk;   // BRK #$00 at $8000
  rom[0x7FFCu] = 0x00u;
  rom[0x7FFDu] = 0x80u;
  rom[0x7FFEu] = 0x00u;  // the emulation IRQ/BRK vector -> $8100
  rom[0x7FFFu] = 0x81u;
  rom[0x0100u] = kStp;
  Snes m(SnesConfig{.rom = rom});
  Recorder r;
  m.setObserver(&r);
  m.step();  // BRK: the sequence through $FFFE
  const std::vector<BusAccess> cpu = r.accessesFrom(AccessSource::Cpu);
  ASSERT_GE(cpu.size(), 2u);
  const BusAccess& low = cpu[cpu.size() - 2];
  const BusAccess& high = cpu.back();
  EXPECT_EQ(low.kind, CycleKind::VectorRead);
  EXPECT_EQ(low.address, 0x00FFFEu);
  EXPECT_EQ(high.kind, CycleKind::VectorRead);
  EXPECT_EQ(high.address, 0x00FFFFu);
  EXPECT_EQ(m.state().cpu.pc, 0x8100u);
}

TEST(SnesObserver, AHaltedCycleReportsNothing) {
  Snes m = programMachine({kStp});
  Recorder r;
  m.setObserver(&r);
  m.step();  // STP
  r.events.clear();
  m.step();  // one halted cycle
  m.step();
  EXPECT_TRUE(r.events.empty());
}

TEST(SnesObserver, TheCpuCycleCountFallsOutOfTheReport) {
  // Each instruction's CPU cycles — its accesses and internal cycles together —
  // are the datasheet's counts.
  Snes m = programMachine({
      kLdaImm, 0x01u,             // 2
      kStaAbs, 0x40u, 0x00u,      // 4
      kIncA,                      // 2
      kIncAbs, 0x40u, 0x00u,      // 6 (the emulation-mode double write)
      0x48u,                      // PHA: 3
      0x68u,                      // PLA: 4
      0x20u, 0x10u, 0x80u,        // JSR !$8010: 6
      kStp,
  });
  const std::uint8_t expected[] = {2, 4, 2, 6, 3, 4, 6};
  Recorder r;
  m.setObserver(&r);
  for (const std::uint8_t cycles : expected) {
    r.events.clear();
    m.step();
    EXPECT_EQ(r.cpuCycles(), std::size_t{cycles});
  }
}

// ---- the engines and the port -------------------------------------------------------

// Arms channel 0 for an A->B transfer of `bytes` from $7E:0010 to the WRAM data
// port, so the bytes land at the port address, and triggers it from CPU code.
Snes dmaMachine(std::uint16_t bytes, std::uint8_t pattern = 0x00u) {
  Snes m = programMachine({kLdaImm, 0x01u, kStaAbs, 0x0Bu, 0x42u, kNop, kStp});
  SnesState s = m.state();
  s.dma[0].dmap = pattern;  // A->B, increment
  s.dma[0].bbad = 0x80u;    // $2180
  s.dma[0].a1t = 0x0010u;
  s.dma[0].a1b = 0x7Eu;
  s.dma[0].das = bytes;
  s.wmadd = 0x001000u;
  for (std::uint16_t i = 0; i < bytes; ++i) s.wram[0x10u + i] = static_cast<std::uint8_t>(0xA0u + i);
  m.restore(s);
  return m;
}

TEST(SnesObserver, ATransfersBytesAreReportedInTheEnginesNameReadThenWrite) {
  Snes m = dmaMachine(3);
  Recorder r;
  m.setObserver(&r);
  m.run(20000u);
  const std::vector<BusAccess> dma = r.accessesFrom(AccessSource::Dma);
  ASSERT_EQ(dma.size(), 6u);
  for (std::size_t i = 0; i < 3; ++i) {
    const BusAccess& read = dma[2 * i];
    const BusAccess& write = dma[2 * i + 1];
    EXPECT_FALSE(read.write);
    EXPECT_EQ(read.address, 0x7E0010u + i);
    EXPECT_EQ(int{read.value}, 0xA0 + static_cast<int>(i));
    EXPECT_EQ(read.kind, CycleKind::DataRead);
    EXPECT_TRUE(write.write);
    EXPECT_EQ(write.address, 0x002180u);
    EXPECT_EQ(int{write.value}, 0xA0 + static_cast<int>(i));
    EXPECT_EQ(write.kind, CycleKind::DataWrite);
  }
  // None of it is the CPU's: the CPU's accesses are its own instructions' only.
  for (const BusAccess& a : r.accessesFrom(AccessSource::Cpu)) {
    EXPECT_NE(a.address, 0x7E0010u);
  }
}

TEST(SnesObserver, ThePortsOwnAccessIsReportedBeforeTheAccessThatMovedTheByte) {
  Snes m = dmaMachine(1);
  Recorder r;
  m.setObserver(&r);
  m.run(20000u);
  // The engine's write to $2180 lands a byte in work RAM at the port address:
  // the port's write is reported, then the engine's.
  std::size_t at = 0;
  for (; at < r.events.size(); ++at) {
    const Event& e = r.events[at];
    if (!e.internal && e.access.source == AccessSource::WramPort) break;
  }
  ASSERT_LT(at + 1, r.events.size());
  const BusAccess& port = r.events[at].access;
  const BusAccess& engine = r.events[at + 1].access;
  EXPECT_TRUE(port.write);
  EXPECT_EQ(port.address, 0x7E1000u);
  EXPECT_EQ(int{port.value}, 0xA0);
  EXPECT_EQ(port.kind, CycleKind::DataWrite);
  EXPECT_EQ(engine.source, AccessSource::Dma);
  EXPECT_EQ(engine.address, 0x002180u);
  EXPECT_EQ(m.state().wram[0x1000], 0xA0u);
}

TEST(SnesObserver, ACpuReadThroughThePortReportsThePortsRead) {
  Snes m = programMachine({kLdaAbs, 0x80u, 0x21u, kStp});  // LDA !$2180
  SnesState s = m.state();
  s.wmadd = 0x012345u;
  s.wram[0x12345u] = 0x3Cu;
  m.restore(s);
  Recorder r;
  m.setObserver(&r);
  m.step();
  ASSERT_EQ(r.events.size(), 5u);  // three fetches, the port's read, the CPU's read
  const BusAccess& port = r.events[3].access;
  const BusAccess& cpu = r.events[4].access;
  EXPECT_EQ(port.source, AccessSource::WramPort);
  EXPECT_FALSE(port.write);
  EXPECT_EQ(port.address, 0x7F2345u);
  EXPECT_EQ(int{port.value}, 0x3C);
  EXPECT_EQ(cpu.source, AccessSource::Cpu);
  EXPECT_EQ(cpu.address, 0x002180u);
  EXPECT_EQ(int{cpu.value}, 0x3C);
}

TEST(SnesObserver, AnHdmaEventIsReportedInTheEnginesName) {
  // Channel 0 delivers a direct table to $2100: one write, wait one line, stop.
  Snes m = programMachine({kNop, 0x80u, 0xFDu});  // NOP ; BRA back
  SnesState s = m.state();
  s.hdmaen = 0x01u;
  s.dma[0].dmap = 0x00u;
  s.dma[0].bbad = 0x00u;   // $2100
  s.dma[0].a1t = 0x0100u;  // the table at $7E:0100
  s.dma[0].a1b = 0x7Eu;
  s.wram[0x100] = 0x01u;   // one line
  s.wram[0x101] = 0x0Fu;   // the value
  s.wram[0x102] = 0x00u;   // stop
  m.restore(s);
  Recorder r;
  m.setObserver(&r);
  m.run(2u * 1364u);  // through line 0's initialisation and line 1's delivery
  const std::vector<BusAccess> hdma = r.accessesFrom(AccessSource::Hdma);
  ASSERT_GE(hdma.size(), 3u);
  EXPECT_EQ(hdma[0].address, 0x7E0100u) << "the line count, read at initialisation";
  EXPECT_FALSE(hdma[0].write);
  EXPECT_EQ(int{hdma[0].value}, 0x01);
  EXPECT_EQ(hdma[1].address, 0x7E0101u) << "the value, read at the delivery";
  EXPECT_EQ(hdma[2].address, 0x002100u);
  EXPECT_TRUE(hdma[2].write);
  EXPECT_EQ(int{hdma[2].value}, 0x0F);
  EXPECT_EQ(m.state().inidisp, 0x0Fu);
}

// ---- the machine with and without one --------------------------------------------

struct Counter final : BusObserver {
  std::size_t count = 0;
  void access(const BusAccess&) override { ++count; }
  void internal(std::uint32_t, std::optional<CycleKind>) override { ++count; }
};

TEST(SnesObserver, RunsIdenticallyWithAndWithoutOne) {
  Snes plain = dmaMachine(8, 0x01u);
  Snes watched = dmaMachine(8, 0x01u);
  Counter c;
  watched.setObserver(&c);
  plain.run(30000u);
  watched.run(30000u);
  EXPECT_GT(c.count, 0u);
  EXPECT_EQ(plain.state().master, watched.state().master);
  EXPECT_EQ(plain.state().cpu.pc, watched.state().cpu.pc);
  EXPECT_EQ(plain.state().cpu.a, watched.state().cpu.a);
  EXPECT_EQ(plain.state().wmadd, watched.state().wmadd);
  EXPECT_EQ(plain.state().wram, watched.state().wram);
  EXPECT_EQ(plain.state().mdr, watched.state().mdr);
}

TEST(SnesObserver, ClearingItStopsTheReport) {
  Snes m = programMachine({kNop, kNop, kNop, kStp});
  Counter c;
  m.setObserver(&c);
  m.step();
  const std::size_t seen = c.count;
  EXPECT_GT(seen, 0u);
  m.setObserver(nullptr);
  m.step();
  EXPECT_EQ(c.count, seen);
  EXPECT_EQ(m.observer(), nullptr);
}

TEST(SnesObserver, IsNotPartOfTheStateSoRestoreLeavesItInPlace) {
  Snes m = programMachine({kNop, kNop, kStp});
  Counter c;
  const SnesState saved = m.state();
  m.setObserver(&c);
  m.step();
  m.restore(saved);
  EXPECT_EQ(m.observer(), &c);
  const std::size_t before = c.count;
  m.step();
  EXPECT_GT(c.count, before);
}

TEST(SnesObserver, AWaitReleasedByAnInterruptReportsTheSequenceOnTheReleasingStep) {
  // WAI with interrupts disabled; an NMI releases it, and the sequence runs on
  // the step after the halted cycles.
  std::vector<std::uint8_t> rom(0x8000u, 0x00u);
  const std::uint8_t program[] = {kLdaImm, 0x80u, kStaAbs, 0x00u, 0x42u,  // NMI on
                                  kWai, kNop, kStp};
  for (std::size_t i = 0; i < sizeof program; ++i) rom[i] = program[i];
  rom[0x7FFCu] = 0x00u;
  rom[0x7FFDu] = 0x80u;
  rom[0x7FFAu] = 0x00u;  // the emulation NMI vector -> $8100
  rom[0x7FFBu] = 0x81u;
  rom[0x0100u] = 0x40u;  // RTI
  Snes m(SnesConfig{.rom = rom});
  Recorder r;
  m.setObserver(&r);
  m.step();
  m.step();
  m.step();  // WAI
  EXPECT_EQ(m.state().cpu.run, CpuRunState::Waiting);
  r.events.clear();
  std::size_t halted = 0;
  while (m.state().cpu.run == CpuRunState::Waiting) {
    m.step();
    ++halted;
  }
  EXPECT_GT(halted, 100u);
  EXPECT_TRUE(r.events.empty()) << "halted cycles report nothing";
  m.step();  // the sequence
  EXPECT_EQ(m.state().cpu.pc, 0x8100u);
  EXPECT_EQ(r.cpuCycles(), 7u);
}

}  // namespace
}  // namespace snaggletooth
