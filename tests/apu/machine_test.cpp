// The APU machine: the $F0-$FF register overlay, DSPADDR/DSPDATA, the TEST
// register, the seeded boot state, whole-machine snapshot/restore, and the
// observer that reports what the sound CPU did.
//
// Every assertion is derived from the SNESdev SPC700 register documentation (the
// reverse-derived contract). The overlay is the SPC700's view, so the tests
// drive the real interpreter through it: a short program is loaded into RAM and
// stepped, and the resulting machine state is inspected. The observer's cases
// hold its report to the accesses the core makes and the boundaries the machine
// crosses, and hold the machine to running exactly the same with none set.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <vector>

#include <gtest/gtest.h>

#include "snaggletooth/apu/apu.h"

namespace {

using snaggletooth::Apu;
using snaggletooth::ApuObserver;
using snaggletooth::ApuState;
using snaggletooth::kIplWindowBytes;
using snaggletooth::RunState;
using snaggletooth::Spc700State;
using snaggletooth::StereoFrame;

// Loads `code` at $0200, points the CPU there, and runs `instructions` of it.
Apu run(std::initializer_list<std::uint8_t> code, int instructions) {
  Apu apu;
  std::uint16_t addr = 0x0200;
  for (std::uint8_t byte : code) apu.writeRam(addr++, byte);
  apu.setPc(0x0200);
  for (int i = 0; i < instructions; ++i) apu.step();
  return apu;
}

// ── Overlay laws ────────────────────────────────────────────────────────────

TEST(ApuOverlay, WriteLandsInRegisterAndUnderlyingRam) {
  // A write to $F0-$FF affects both the register and the RAM byte beneath it.
  Apu apu = run({0x8F, 0x55, 0xF2}, 1);  // MOV $F2,#$55  (DSPADDR)
  EXPECT_EQ(apu.state().dspAddr, 0x55);
  EXPECT_EQ(apu.readRam(0x00F2), 0x55);
}

TEST(ApuOverlay, ReadReturnsRegisterValueNotUnderlyingRam) {
  // Reads of $F0-$FF read the register, never the RAM behind it.
  Apu apu;
  apu.writeRam(0x0200, 0x8F); apu.writeRam(0x0201, 0x33); apu.writeRam(0x0202, 0xF2);  // MOV $F2,#$33
  apu.writeRam(0x0203, 0xE4); apu.writeRam(0x0204, 0xF2);  // MOV A,$F2
  apu.setPc(0x0200);
  apu.step();                    // DSPADDR := $33, RAM[$F2] := $33
  apu.writeRam(0x00F2, 0x99);    // make the underlying RAM disagree
  apu.step();                    // MOV A,$F2 reads the register
  EXPECT_EQ(apu.state().cpu.a, 0x33);    // the register value
  EXPECT_EQ(apu.readRam(0x00F2), 0x99);  // the read left RAM alone
}

TEST(ApuOverlay, WriteOnlyRegisterReadsBackZero) {
  // Write-only registers always read back 0 even after a write is stored.
  Apu apu = run({0x8F, 0x77, 0xFA,   // MOV $FA,#$77  (T0 target, write-only)
                 0xE4, 0xFA}, 2);    // MOV A,$FA
  EXPECT_EQ(apu.state().cpu.a, 0x00);
  EXPECT_EQ(apu.state().timers[0].target, 0x77);
  EXPECT_EQ(apu.readRam(0x00FA), 0x77);
}

TEST(ApuOverlay, AuxPortsReadBackWritesAndIgnoreTheRamBeneath) {
  // $F8/$F9 read back what was written — port bytes of their own, not a view of
  // the RAM beneath. An S-DSP echo-buffer write reaches only the RAM, so it
  // never changes what the CPU reads back (anomie-spc700 $00f8/$00f9: "not
  // altered by S-DSP echo buffer writes"; spc_dsp6 `Misc/$F0-$FF are not ram`
  // pins it by sweeping an echo write burst across the ports).
  Apu apu;
  apu.writeRam(0x0200, 0x8F); apu.writeRam(0x0201, 0x9A); apu.writeRam(0x0202, 0xF8);  // MOV $F8,#$9A
  apu.writeRam(0x0203, 0xE4); apu.writeRam(0x0204, 0xF8);  // MOV A,$F8
  apu.setPc(0x0200);
  apu.step();                    // port := $9A, RAM[$F8] := $9A
  EXPECT_EQ(apu.readRam(0x00F8), 0x9A);  // a CPU port write mirrors into the RAM beneath
  apu.writeRam(0x00F8, 0xFE);    // what an echo-buffer write does: the RAM alone
  apu.step();                    // MOV A,$F8 reads the port
  EXPECT_EQ(apu.state().cpu.a, 0x9A);    // the port byte, not the echo's
  EXPECT_EQ(apu.readRam(0x00F8), 0xFE);  // the RAM beneath keeps the echo byte
}

// ── TEST register ($F0) ─────────────────────────────────────────────────────

TEST(ApuTest, StoresWhenPFlagClear) {
  // A write with P clear updates TEST. Reached by absolute addressing so the
  // direct-page flag plays no part in selecting the register.
  Apu apu = run({0xE8, 0x1F,          // MOV A,#$1F
                 0xC5, 0xF0, 0x00}, 2);  // MOV !$00F0,A
  EXPECT_EQ(apu.state().test, 0x1F);
}

TEST(ApuTest, WriteHasNoEffectWhenPFlagSet) {
  // Writing TEST has no effect while the P flag is set; it keeps its power-on $0A.
  Apu apu = run({0xE8, 0x1F,          // MOV A,#$1F
                 0x40,                // SETP
                 0xC5, 0xF0, 0x00}, 3);  // MOV !$00F0,A
  EXPECT_EQ(apu.state().test, 0x0A);
}

// ── DSPADDR / DSPDATA ($F2 / $F3) ───────────────────────────────────────────

TEST(ApuDsp, AddrReadsBackLatchedValue) {
  Apu apu = run({0x8F, 0x3C, 0xF2,   // MOV $F2,#$3C
                 0xE4, 0xF2}, 2);    // MOV A,$F2
  EXPECT_EQ(apu.state().cpu.a, 0x3C);
}

TEST(ApuDsp, WriteBelowLimitStoresAndReadsBack) {
  Apu apu = run({0x8F, 0x10, 0xF2,   // select DSP register $10
                 0x8F, 0xAB, 0xF3,   // write it
                 0xE4, 0xF3}, 3);    // read it back
  EXPECT_EQ(apu.state().cpu.a, 0xAB);
  EXPECT_EQ(apu.state().dsp[0x10], 0xAB);
}

TEST(ApuDsp, WriteBeyondLimitIsIgnored) {
  // Writes with the DSP address above $7F are ignored.
  Apu apu = run({0x8F, 0xFF, 0xF2,   // address $FF (above $7F)
                 0x8F, 0xCD, 0xF3,   // ignored
                 0xE4, 0xF3}, 3);    // read masks to $7F, still empty
  EXPECT_EQ(apu.state().dsp[0x7F], 0x00);
  EXPECT_EQ(apu.state().cpu.a, 0x00);
}

TEST(ApuDsp, KonWriteSetsThePendingKeyOn) {
  // KON takes effect on the write: the written value becomes the internal KON
  // the next poll acts on, while the register itself keeps the value for
  // read-back (Anomie 720-727, fullsnes 3141-3150).
  Apu apu = run({0x8F, 0x4C, 0xF2,   // select KON
                 0x8F, 0x01, 0xF3}, 2);
  EXPECT_EQ(apu.state().dsp[0x4C], 0x01);
  EXPECT_EQ(apu.state().dsp.internalKon, 0x01);
}

TEST(ApuDsp, ASecondKonWriteReplacesThePendingKeyOn) {
  // Anomie's worked example (706-708): writing KON=1 then KON=2 in close
  // succession usually keys on voice 2 only, so the second write replaces the
  // pending value rather than accumulating into it.
  Apu apu = run({0x8F, 0x4C, 0xF2,   // select KON
                 0x8F, 0x01, 0xF3,   // KON := 1
                 0x8F, 0x02, 0xF3}, 3);
  EXPECT_EQ(apu.state().dsp.internalKon, 0x02);
}

TEST(ApuDsp, ReadMasksAddressWith7F) {
  // Reads mask the DSP address with $7F, so $85 reads slot $05.
  Apu apu = run({0x8F, 0x05, 0xF2,   // select $05
                 0x8F, 0x22, 0xF3,   // dsp[$05] := $22
                 0x8F, 0x85, 0xF2,   // select $85 (bit 7 set)
                 0xE4, 0xF3}, 4);    // read slot $85 & $7F = $05
  EXPECT_EQ(apu.state().cpu.a, 0x22);
}

// ── Timer registers stored (the counters go live in the next unit) ──────────

TEST(ApuTimerRegisters, EachTargetStoresIndependently) {
  Apu apu = run({0x8F, 0x11, 0xFA,   // T0 target
                 0x8F, 0x22, 0xFB,   // T1 target
                 0x8F, 0x33, 0xFC}, 3);  // T2 target
  EXPECT_EQ(apu.state().timers[0].target, 0x11);
  EXPECT_EQ(apu.state().timers[1].target, 0x22);
  EXPECT_EQ(apu.state().timers[2].target, 0x33);
}

// ── Boot state ──────────────────────────────────────────────────────────────

TEST(ApuBoot, PowerOnStateMatchesSeededValues) {
  Apu apu;
  const ApuState& s = apu.state();
  EXPECT_EQ(s.cpu.sp, 0xEF);
  EXPECT_EQ(s.cpu.pc, 0x0000);
  EXPECT_EQ(s.cpu.a, 0x00);
  EXPECT_EQ(s.cpu.x, 0x00);
  EXPECT_EQ(s.cpu.y, 0x00);
  EXPECT_EQ(s.cpu.psw, 0x00);
  EXPECT_EQ(s.cpu.run, RunState::Running);
  EXPECT_EQ(s.test, 0x0A);
  EXPECT_EQ(s.control, 0xB0);
  EXPECT_EQ(s.outputPorts[0], 0xAA);
  EXPECT_EQ(s.outputPorts[1], 0xBB);
  EXPECT_EQ(s.outputPorts[2], 0x00);
  EXPECT_EQ(s.outputPorts[3], 0x00);
  EXPECT_EQ(s.inputPorts[0], 0x00);
  EXPECT_EQ(s.dspAddr, 0x00);
  EXPECT_EQ(s.divider, 0x0000);
  for (int i = 0; i < 3; ++i) {
    EXPECT_EQ(s.timers[i].target, 0x00);
    EXPECT_EQ(s.timers[i].stage2, 0x00);
    EXPECT_EQ(s.timers[i].stage3, 0x0F);  // TnOUT reads $F on power on
  }
  bool ramZero = true;
  for (std::size_t i = 0; i < s.ram.size(); ++i)
    if (s.ram[i] != 0) { ramZero = false; break; }
  EXPECT_TRUE(ramZero);
  // The DSP register file powers on zeroed except FLG ($6C), whose documented
  // reset value is $E0 (soft reset / mute / echo-write-off / noise stopped).
  bool dspSeeded = true;
  for (std::size_t i = 0; i < 128; ++i) {
    const std::uint8_t expected = (i == 0x6C) ? 0xE0 : 0x00;
    if (s.dsp[i] != expected) { dspSeeded = false; break; }
  }
  EXPECT_TRUE(dspSeeded);
  EXPECT_EQ(s.dsp[0x6C], 0xE0);
}

// ── Snapshot / restore and construction ─────────────────────────────────────

TEST(ApuSnapshot, RestoreRoundTripsWholeMachine) {
  Apu apu = run({0x8F, 0x11, 0xF2,   // DSPADDR := $11
                 0x8F, 0x22, 0xF8,   // scratch $F8 := $22
                 0xE8, 0x33}, 3);    // A := $33
  apu.writePort(2, 0x44);            // host writes input port 2
  const ApuState snap = apu.state();

  Apu fresh;
  fresh.restore(snap);
  const ApuState& r = fresh.state();
  EXPECT_EQ(r.dspAddr, 0x11);
  EXPECT_EQ(r.ram[0x00F8], 0x22);
  EXPECT_EQ(r.cpu.a, 0x33);
  EXPECT_EQ(r.inputPorts[2], 0x44);
  EXPECT_EQ(r.cpu.pc, snap.cpu.pc);
}

TEST(ApuSnapshot, RestoredMachineResumesExecution) {
  // A snapshot taken mid-program restores the PC, so a restored machine runs the
  // next instruction identically.
  Apu original = run({0xE8, 0x40,        // MOV A,#$40  (runs)
                      0x8F, 0x77, 0xF8}, 1);  // MOV $F8,#$77 (pending at the snapshot)
  const ApuState snap = original.state();

  Apu a; a.restore(snap);
  Apu b; b.restore(snap);
  a.step();
  b.step();
  EXPECT_EQ(a.state().cpu.pc, b.state().cpu.pc);
  EXPECT_EQ(a.readRam(0x00F8), 0x77);  // the pending instruction ran on both
  EXPECT_EQ(b.readRam(0x00F8), 0x77);
}

TEST(ApuSnapshot, ConstructFromStateSeedsTheCpu) {
  // Constructing from a state makes its CPU the live one, not just a snapshot slot.
  ApuState s{};
  s.cpu.pc = 0x0200;
  s.ram[0x0200] = 0xE8; s.ram[0x0201] = 0x5C;  // MOV A,#$5C
  Apu apu(s);
  EXPECT_EQ(apu.state().cpu.pc, 0x0200);
  apu.step();
  EXPECT_EQ(apu.state().cpu.a, 0x5C);
}

// ── Host access and stepping ────────────────────────────────────────────────

TEST(ApuHost, SetPcRedirectsExecution) {
  Apu apu;
  apu.writeRam(0x0300, 0xE8); apu.writeRam(0x0301, 0x7E);  // MOV A,#$7E
  apu.setPc(0x0300);
  apu.step();
  EXPECT_EQ(apu.state().cpu.a, 0x7E);
  EXPECT_EQ(apu.state().cpu.pc, 0x0302);
}

TEST(ApuHost, StepReturnsInstructionCycleCount) {
  Apu apu;
  apu.writeRam(0x0200, 0x00);  // NOP
  apu.setPc(0x0200);
  EXPECT_EQ(apu.step(), 2u);
}

TEST(ApuHost, LoadRamCopiesABlock) {
  Apu apu;
  const std::uint8_t code[] = {0xE8, 0x12, 0x00};
  apu.loadRam(0x0400, code);
  EXPECT_EQ(apu.readRam(0x0400), 0xE8);
  EXPECT_EQ(apu.readRam(0x0401), 0x12);
  EXPECT_EQ(apu.readRam(0x0402), 0x00);
}

// ── The observer ────────────────────────────────────────────────────────────

// Everything the machine reports, in order: an access with its address, value
// and direction, or a boundary with the state on either side and the cycles.
struct Report {
  bool boundary = false;
  std::uint16_t address = 0;
  std::uint8_t value = 0;
  bool write = false;
  Spc700State before{};
  Spc700State after{};
  std::uint32_t cycles = 0;
};

struct Recorder final : ApuObserver {
  std::vector<Report> reports;
  void access(std::uint16_t address, std::uint8_t value, bool write) override {
    Report r;
    r.address = address;
    r.value = value;
    r.write = write;
    reports.push_back(r);
  }
  void instruction(const Spc700State& before, const Spc700State& after,
                   std::uint32_t cycles) override {
    Report r;
    r.boundary = true;
    r.before = before;
    r.after = after;
    r.cycles = cycles;
    reports.push_back(r);
  }
  [[nodiscard]] std::vector<Report> accesses() const {
    std::vector<Report> out;
    for (const Report& r : reports) {
      if (!r.boundary) out.push_back(r);
    }
    return out;
  }
  [[nodiscard]] std::vector<Report> boundaries() const {
    std::vector<Report> out;
    for (const Report& r : reports) {
      if (r.boundary) out.push_back(r);
    }
    return out;
  }
};

// A machine with `code` at $0200 and the CPU pointed there, nothing run yet.
Apu loaded(std::initializer_list<std::uint8_t> code) {
  Apu apu;
  std::uint16_t addr = 0x0200;
  for (std::uint8_t byte : code) apu.writeRam(addr++, byte);
  apu.setPc(0x0200);
  return apu;
}

TEST(ApuObserver, NoneIsSetAtPowerOn) {
  Apu apu;
  EXPECT_EQ(apu.observer(), nullptr);
}

TEST(ApuObserver, EveryAccessIsReportedAsItSettles) {
  // MOV $F2,#$55 fetches its three bytes, reads its destination — the DSPADDR
  // register, still $00 — and writes it; MOV A,$F2 then reads the register,
  // whose settled value is what the report carries, not the RAM beneath.
  Apu apu = loaded({0x8F, 0x55, 0xF2,   // MOV $F2,#$55
                    0xE4, 0xF2});       // MOV A,$F2
  Recorder rec;
  apu.setObserver(&rec);
  apu.step();
  const std::vector<Report> first = rec.accesses();
  ASSERT_GE(first.size(), 4u);
  EXPECT_EQ(first[0].address, 0x0200u);
  EXPECT_EQ(first[0].value, 0x8Fu);
  EXPECT_FALSE(first[0].write);
  EXPECT_EQ(first[1].address, 0x0201u);
  EXPECT_EQ(first[1].value, 0x55u);
  EXPECT_EQ(first[2].address, 0x0202u);
  EXPECT_EQ(first[2].value, 0xF2u);
  const Report& store = first.back();
  EXPECT_EQ(store.address, 0x00F2u);
  EXPECT_EQ(store.value, 0x55u);
  EXPECT_TRUE(store.write);
  const std::size_t readsOfRegister = static_cast<std::size_t>(
      std::count_if(first.begin(), first.end(),
                    [](const Report& r) { return r.address == 0x00F2u && !r.write; }));
  EXPECT_EQ(readsOfRegister, 1u) << "the read a store makes of its destination before writing";

  rec.reports.clear();
  apu.writeRam(0x00F2, 0x99);  // the RAM beneath disagrees with the register
  apu.step();
  const std::vector<Report> second = rec.accesses();
  ASSERT_EQ(second.size(), 3u);
  EXPECT_EQ(second[0].address, 0x0203u);
  EXPECT_EQ(second[0].value, 0xE4u);
  EXPECT_EQ(second[1].address, 0x0204u);
  EXPECT_EQ(second[1].value, 0xF2u);
  EXPECT_EQ(second[2].address, 0x00F2u);
  EXPECT_EQ(second[2].value, 0x55u) << "the register's value, as the CPU read it";
  EXPECT_FALSE(second[2].write);
}

TEST(ApuObserver, EveryBoundaryIsReportedWithTheStateOnEitherSideAndTheCycles) {
  Apu apu = loaded({0x8F, 0x55, 0xF2,   // MOV $F2,#$55
                    0xE4, 0xF2});       // MOV A,$F2
  Recorder rec;
  apu.setObserver(&rec);
  const std::uint32_t first = apu.step();
  const std::uint32_t second = apu.step();
  const std::vector<Report> boundaries = rec.boundaries();
  ASSERT_EQ(boundaries.size(), 2u);
  EXPECT_EQ(boundaries[0].before.pc, 0x0200u);
  EXPECT_EQ(boundaries[0].after.pc, 0x0203u);
  EXPECT_EQ(boundaries[0].cycles, first);
  EXPECT_EQ(boundaries[1].before.pc, 0x0203u);
  EXPECT_EQ(boundaries[1].after.pc, 0x0205u);
  EXPECT_EQ(boundaries[1].after.a, 0x55u);
  EXPECT_EQ(boundaries[1].cycles, second);
  // The accesses of an instruction come before its boundary.
  EXPECT_FALSE(rec.reports.front().boundary);
  EXPECT_TRUE(rec.reports.back().boundary);
}

TEST(ApuObserver, AnInstructionSplitAcrossRunsIsOneBoundaryWithItsWholeCost) {
  Apu apu = loaded({0x8F, 0x55, 0xF2});  // MOV $F2,#$55: five cycles
  Recorder rec;
  apu.setObserver(&rec);
  apu.run(2);
  EXPECT_TRUE(rec.boundaries().empty()) << "mid-instruction is no boundary";
  apu.run(3);
  const std::vector<Report> boundaries = rec.boundaries();
  ASSERT_EQ(boundaries.size(), 1u);
  EXPECT_EQ(boundaries[0].before.pc, 0x0200u);
  EXPECT_EQ(boundaries[0].after.pc, 0x0203u);
  EXPECT_EQ(boundaries[0].cycles, 5u);
}

TEST(ApuObserver, AHaltedCoreReportsIdleBoundariesWithNoAccessBetween) {
  Apu apu = loaded({0xFF});  // STOP
  Recorder rec;
  apu.setObserver(&rec);
  apu.step();
  ASSERT_EQ(rec.boundaries().size(), 1u);
  EXPECT_EQ(rec.boundaries()[0].after.run, RunState::Stopped);
  rec.reports.clear();
  EXPECT_EQ(apu.step(), 2u);
  ASSERT_EQ(rec.reports.size(), 2u);
  for (const Report& r : rec.reports) {
    EXPECT_TRUE(r.boundary);
    EXPECT_EQ(r.cycles, 1u);
    EXPECT_EQ(r.before.run, RunState::Stopped);
    EXPECT_EQ(r.after.run, RunState::Stopped);
    EXPECT_EQ(r.before.pc, r.after.pc);
  }
}

TEST(ApuObserver, RunsIdenticallyWithAndWithoutOne) {
  // Timers enabled, the amplifier unmuted, a voice keyed on, then a loop: the
  // clocked events and the output are the same with an observer reporting
  // every cycle and with none.
  const std::initializer_list<std::uint8_t> program = {
      0x8F, 0x07, 0xF1,   // MOV $F1,#$07   timers on
      0x8F, 0x10, 0xFA,   // MOV $FA,#$10   T0 target
      0x8F, 0x6C, 0xF2,   // MOV $F2,#$6C   FLG
      0x8F, 0x00, 0xF3,   // MOV $F3,#$00   unmute
      0x8F, 0x4C, 0xF2,   // MOV $F2,#$4C   KON
      0x8F, 0x01, 0xF3,   // MOV $F3,#$01   voice 0
      0xE4, 0xFD,         // MOV A,$FD      T0OUT
      0x2F, 0xFC,         // BRA back to the read
  };
  Apu watched = loaded(program);
  Apu alone = loaded(program);
  Recorder rec;
  watched.setObserver(&rec);
  watched.run(20'000);
  alone.run(20'000);
  EXPECT_FALSE(rec.reports.empty());
  const ApuState& a = watched.state();
  const ApuState& b = alone.state();
  EXPECT_EQ(a.cpu.pc, b.cpu.pc);
  EXPECT_EQ(a.cpu.a, b.cpu.a);
  EXPECT_EQ(a.cpu.psw, b.cpu.psw);
  EXPECT_EQ(a.cpu.tcu, b.cpu.tcu);
  EXPECT_EQ(a.divider, b.divider);
  EXPECT_EQ(a.control, b.control);
  EXPECT_EQ(a.dspAddr, b.dspAddr);
  for (std::size_t i = 0; i < 3; ++i) {
    EXPECT_EQ(a.timers[i].stage2, b.timers[i].stage2);
    EXPECT_EQ(a.timers[i].stage3, b.timers[i].stage3);
  }
  EXPECT_TRUE(std::equal(a.ram.begin(), a.ram.end(), b.ram.begin()));
  for (std::size_t i = 0; i < 128; ++i) EXPECT_EQ(a.dsp[i], b.dsp[i]) << i;
  const std::vector<StereoFrame> framesA = watched.takeFrames();
  const std::vector<StereoFrame> framesB = alone.takeFrames();
  EXPECT_EQ(framesA.size(), 20'000u / 32u);
  EXPECT_EQ(framesA, framesB);
}

TEST(ApuObserver, PeekAnswersWhatAFetchWouldReturnAndChangesNothing) {
  Apu apu = loaded({0x8F, 0x30, 0xF1});  // MOV $F1,#$30: CONTROL bit 7 off
  EXPECT_EQ(apu.peek(0x0200), 0x8Fu);
  std::array<std::uint8_t, kIplWindowBytes> image{};
  for (std::size_t i = 0; i < image.size(); ++i) image[i] = static_cast<std::uint8_t>(0xC0u + i);
  apu.mapIplRom(image);
  EXPECT_EQ(apu.state().control & 0x80u, 0x80u) << "power-on CONTROL maps the window";
  EXPECT_EQ(apu.peek(0xFFC0), 0xC0u) << "the image, as a fetch would read it";
  EXPECT_EQ(apu.peek(0xFFFF), 0xFFu);
  EXPECT_EQ(apu.readRam(0xFFC0), 0x00u) << "the RAM beneath, as the host reads it";
  apu.step();  // the bit clears
  EXPECT_EQ(apu.peek(0xFFC0), 0x00u) << "the RAM beneath, once the window is unmapped";
  // The overlay is answered from the RAM beneath: a program is not fetched
  // from the registers, and a peek there moves no register — T0OUT keeps its
  // power-on $F where a CPU read would have cleared it.
  apu.writePort(0, 0x77);
  EXPECT_EQ(apu.peek(0x00F4), 0x00u);
  EXPECT_EQ(apu.state().inputPorts[0], 0x77u);
  EXPECT_EQ(apu.peek(0x00FD), 0x00u);
  EXPECT_EQ(apu.state().timers[0].stage3, 0x0Fu);
}

TEST(ApuObserver, IsNotPartOfTheStateSoRestoreLeavesItInPlace) {
  Apu apu = loaded({0xE8, 0x40,         // MOV A,#$40
                    0xE8, 0x41});       // MOV A,#$41
  Recorder rec;
  apu.setObserver(&rec);
  apu.step();
  const ApuState snap = apu.state();
  apu.step();
  apu.restore(snap);
  EXPECT_EQ(apu.observer(), &rec);
  rec.reports.clear();
  apu.step();
  ASSERT_EQ(rec.boundaries().size(), 1u);
  EXPECT_EQ(rec.boundaries()[0].before.pc, snap.cpu.pc) << "the restored state is the before";
  EXPECT_EQ(rec.boundaries()[0].cycles, 2u);
}

TEST(ApuObserver, ClearingItStopsTheReport) {
  Apu apu = loaded({0xE8, 0x40, 0xE8, 0x41});
  Recorder rec;
  apu.setObserver(&rec);
  apu.step();
  apu.setObserver(nullptr);
  const std::size_t reported = rec.reports.size();
  apu.step();
  EXPECT_EQ(rec.reports.size(), reported);
  EXPECT_EQ(apu.observer(), nullptr);
}

}  // namespace
