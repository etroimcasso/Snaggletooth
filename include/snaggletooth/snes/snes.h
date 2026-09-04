#pragma once

// The SNES machine — the 5A22 (its 65816 core) wired to the console's memory map,
// the work RAM, and the APU across the communication ports.
//
// The machine owns the CPU, the 128 KB of work RAM, and the audio machine. Its
// bus maps a 24-bit address the way the console does: work RAM in banks $7E-$7F
// and mirrored into the low pages of every system bank, the cartridge ROM and its
// save RAM in whichever windows the cartridge's map gives them, the APU
// communication ports at $2140-$2143, and the work-RAM data port at $2180-$2183.
// A read of an unmapped address returns the last value the data bus carried.
//
// Every access is priced by the region it reaches. The console runs three memory
// speeds — six, eight, or twelve master cycles per access — and the machine
// charges each cycle its region's cost as the CPU makes it. The APU keeps its own
// slower clock: the machine advances it by the exact rational share of the master
// cycles that have elapsed, computed in integer arithmetic so a run is
// reproducible to the byte.
//
// step() runs one CPU instruction and returns the master cycles it took. run()
// spends an exact master-cycle budget and may stop part-way through an
// instruction, which is a legal resting place — instruction progress is part of
// the state value, so run(a) followed by run(b) advances the machine exactly as
// run(a + b) would.
//
// A machine runs at one of the two console clock rates, chosen at build. The
// region-speed map is counted in master cycles either way, so it does not change;
// only the master clock does, and with it the exact share of master cycles the
// APU's own crystal is paced against.

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "snaggletooth/apu/apu.h"
#include "snaggletooth/cpu/cpu65816.h"
#include "snaggletooth/snes/cartridge.h"

namespace snaggletooth {

// The console clock rate. It sets the master clock, and so the ratio the APU is
// paced against; the memory-region speeds, counted in master cycles, are the same
// in both.
enum class Region : std::uint8_t { Ntsc, Pal };

// The arithmetic unit's current job, if any. A write to the multiplier or the
// divisor starts one; it finishes its documented number of cycles later, at which
// point the result registers take their value.
enum class MathOp : std::uint8_t { None, Multiply, Divide };

// One of the eight DMA/HDMA channels as a plain value: the sixteen bytes of its
// register file at $43n0-$43nF. Each channel serves both general-purpose DMA and
// HDMA, so several registers carry a second meaning under HDMA, noted per field.
// The power-on values are the console's ($FF everywhere).
struct DmaChannel {
  std::uint8_t dmap = 0xFF;    // $43n0: direction (bit7), indirect HDMA (bit6), address step (bits4-3), transfer pattern (bits2-0)
  std::uint8_t bbad = 0xFF;    // $43n1: the B-bus register, the low byte of a $21xx address
  std::uint16_t a1t = 0xFFFF;  // $43n2/$43n3: the DMA source address (HDMA: the table start), low 16 bits
  std::uint8_t a1b = 0xFF;     // $43n4: the bank of that address; fixed across a transfer, which cannot cross a bank
  std::uint16_t das = 0xFFFF;  // $43n5/$43n6: the DMA byte count (HDMA: the indirect address), low 16 bits
  std::uint8_t dasb = 0xFF;    // $43n7: the bank of the HDMA indirect address
  std::uint16_t a2a = 0xFFFF;  // $43n8/$43n9: the HDMA table's current address, low 16 bits
  std::uint8_t nltr = 0xFF;    // $43nA: the HDMA line counter (bits6-0) and the repeat flag (bit7)
  std::uint8_t unused = 0xFF;  // $43nB/$43nF: one unused byte, readable and writable through two addresses
};

// A standard controller's twelve buttons as a value: true is pressed. A pad is
// presented to the machine with Snes::setJoypad and read by the program through
// the auto-read registers or the serial ports; the machine samples it when it
// latches, so a value set at any point in a frame is what that frame's read sees.
struct Joypad {
  bool b = false;
  bool y = false;
  bool select = false;
  bool start = false;
  bool up = false;
  bool down = false;
  bool left = false;
  bool right = false;
  bool a = false;
  bool x = false;
  bool l = false;
  bool r = false;

  // The sixteen bits the pad shifts out, in the layout the auto-read registers
  // hold them: bit 15 is B, the first bit on the wire, down to bit 4 for R; bits
  // 3-0 are the standard pad's identity code, all zero. The high byte is what
  // $4219 reads, the low byte $4218.
  [[nodiscard]] std::uint16_t bits() const noexcept;

  [[nodiscard]] bool operator==(const Joypad&) const noexcept = default;
};

// The two controller ports on the console's front.
enum class JoypadPort : std::uint8_t { One, Two };

// How a machine is built: the cartridge image, the clock rate, and whether to
// seed the APU upload stub. The ROM is copied in, so the span need not outlive
// the call.
struct SnesConfig {
  std::span<const std::uint8_t> rom;   // a cartridge image under any map
  Region region = Region::Ntsc;        // the console clock rate
  bool iplStub = true;                 // seed the APU with its upload stub (arrives with the loader)

  // The cartridge's map. Left absent, it is read from the image's own header,
  // which is what lets any cartridge boot without the caller knowing its layout.
  // Set it to run an image whose header is wrong, absent, or not a header at all.
  std::optional<CartridgeMap> map = std::nullopt;

  // The save RAM to give the machine, in bytes. Left absent, it is taken from the
  // cartridge header. Set it to zero to run a cartridge without its save.
  std::optional<std::size_t> saveRamBytes = std::nullopt;

  // An audio boot ROM to run in place of the built-in stub. Absent by default, so
  // the machine boots on the stub. Supply a console's own 64-byte boot ROM and the
  // audio unit runs that code instead: it is seeded into RAM and mapped over the
  // $FFC0 window exactly as the stub is, so everything downstream is unchanged.
  // The image belongs to whoever supplies it and is never carried here. Ignored
  // when iplStub is off, which skips the boot sequence entirely.
  std::optional<std::array<std::uint8_t, kIplWindowBytes>> bootRom = std::nullopt;
};

// The whole machine as a value: snapshot by copy, restore by assignment. The ROM
// is fixed configuration rather than state and lives in the machine, not here, so
// restore() replaces the mutable machine and keeps the cartridge in place.
struct SnesState {
  Cpu65816State cpu{};
  std::array<std::uint8_t, 131072> wram{};  // 128 KB work RAM (banks $7E-$7F)

  // The cartridge's save RAM, as large as its header declares and empty when it
  // declares none. It is machine state rather than configuration: a snapshot
  // carries the save with it, and restore() puts it back. A game persists one by
  // reading it out.
  std::vector<std::uint8_t> sram{};

  ApuState apu{};
  std::uint32_t wmadd = 0;      // the work-RAM port address ($2181-$2183); $2180 auto-increments it
  std::uint8_t memsel = 0;      // $420D bit 0: the second waitstate region runs fast (1) or slow (0)
  std::uint8_t mdr = 0;         // the last value on the data bus, returned by a read of open bus
  std::uint64_t master = 0;     // the free-running master-cycle counter
  std::uint64_t consumed = 0;   // master cycles reported through run(); master - consumed is the budget carried between calls
  std::uint32_t apuPhase = 0;   // the APU-clock accumulator for the exact master-to-APU cycle ratio

  // ---- the video position ---------------------------------------------------
  // Where the beam is, tracked in master cycles within the scanline and in whole
  // scanlines down the frame. Both advance as the machine runs, so a mid-frame
  // snapshot resumes on the exact dot. The frame-parity bit alternates every frame
  // and selects the one short scanline NTSC uses to keep the colour signal in step.
  std::uint16_t hpos = 0;   // master cycles into the current scanline (0..lineLength-1)
  std::uint16_t vpos = 0;   // the current scanline (0..261 NTSC, 0..311 PAL)
  std::uint8_t field = 0;   // frame parity (0/1); NTSC line 240 is short on odd frames

  // ---- the interrupt registers ----------------------------------------------
  std::uint8_t nmitimen = 0;    // $4200: bit7 NMI enable, bits5-4 H/V IRQ mode, bit0 auto-joypad enable
  bool vblankNmi = false;       // $4210 bit7: set at the start of vblank, cleared on read and at vblank's end
  bool timeup = false;          // $4211 bit7: set when the H/V counter reaches its timer, cleared on read
  std::uint16_t htime = 0x01FF; // $4207/$4208: the H-count IRQ position, in dots (0..339)
  std::uint16_t vtime = 0x01FF; // $4209/$420A: the V-count IRQ position, in lines (0..261/311)

  // ---- the multiply/divide unit ---------------------------------------------
  // A write to the multiplier or the divisor loads the operands and starts the
  // unit; the result is ready a fixed number of cycles later. Until then the result
  // registers hold their previous contents (the intermediate is not documented, so
  // it is not invented) — except that starting a multiply immediately loads the
  // quotient register with the multiplier, a documented quirk of the shared unit.
  std::uint8_t wrmpya = 0xFF;   // $4202: the multiplicand
  std::uint8_t wrmpyb = 0xFF;   // $4203: the multiplier (its write starts a multiply)
  std::uint16_t wrdiv = 0xFFFF; // $4204/$4205: the dividend
  std::uint8_t wrdivb = 0xFF;   // $4206: the divisor (its write starts a divide)
  std::uint16_t rddiv = 0;      // $4214/$4215: the quotient
  std::uint16_t rdmpy = 0;      // $4216/$4217: the product, or the division remainder
  std::uint8_t mathClocks = 0;  // CPU cycles left before the result lands (0 = idle)
  MathOp mathOp = MathOp::None; // which result the pending job will commit

  // ---- the controller ports -------------------------------------------------
  // The pads presented to the two ports (none by default, which reads as no
  // controller: every bit zero), the strobe line the program drives through
  // $4016, and each port's shift register — the sixteen bits latched at the
  // strobe's fall and how many of them have been clocked out. The auto-read uses
  // the same strobe and clock lines, so its sixteen clocks leave a port's register
  // at its padding until the program strobes again. When enabled, the auto-read
  // spends a fixed window each frame; the busy flag is raised for that window and
  // the result registers take their new value as it ends.
  std::array<std::optional<Joypad>, 2> pads{};  // what is plugged into each port
  bool joyStrobe = false;               // $4016 bit 0 as last written: the latch line held high
  std::array<std::uint16_t, 2> joyLatch{};  // per port: the bits latched at the strobe's fall
  std::array<std::uint8_t, 2> joyClocks{};  // per port: bits clocked out since the latch (16 = at the padding)
  std::uint16_t autoJoyClocks = 0;      // master cycles left in the auto-read busy window (0 = idle)
  std::array<std::uint8_t, 8> joy{};    // $4218-$421F: the four 16-bit pad reads

  // ---- the PPU register-file stub -------------------------------------------
  // Video memory and the register fields that reach it. Nothing renders — the arrays
  // are exposed for a host to read, and the ports store into them the way the console
  // does, so a program that fills VRAM or the palette leaves the memory a real PPU
  // would have seen.
  std::array<std::uint8_t, 65536> vram{};  // 64 KB video RAM (32K words)
  std::array<std::uint8_t, 512> cgram{};   // 512 B palette RAM (256 words)
  std::uint8_t inidisp = 0x80;  // $2100: bit7 forced blank (set at power-on), bits3-0 brightness
  std::uint8_t vmain = 0;       // $2115: VRAM address increment mode and translation
  std::uint16_t vmadd = 0;      // $2116/$2117: the VRAM word address
  std::uint16_t vramLatch = 0;  // the 16-bit read-prefetch register behind $2139/$213A
  std::uint8_t cgadd = 0;       // $2121: the CGRAM word address
  bool cgLatchHigh = false;     // the $2122/$213B low/high access flip-flop (false = low byte next)
  std::uint8_t cgLatch = 0;     // the low byte held between the two halves of a CGRAM write
  std::uint8_t bg1sc = 0;       // $2107: BG1 screen base and size
  std::uint8_t bg2sc = 0;       // $2108: BG2 screen base and size
  std::uint8_t bg3sc = 0;       // $2109: BG3 screen base and size
  std::uint8_t bg4sc = 0;       // $210A: BG4 screen base and size
  std::uint8_t bg12nba = 0;     // $210B: BG1/BG2 character base
  std::uint8_t bg34nba = 0;     // $210C: BG3/BG4 character base
  std::uint8_t tm = 0;          // $212C: main-screen layer enables

  // ---- DMA and HDMA ---------------------------------------------------------
  // The eight channels and the two enable registers, plus the engines' progress.
  // A general-purpose DMA holds the bus one byte at a time, so a snapshot taken
  // mid-transfer resumes on the exact byte; HDMA runs a whole event at once (the
  // CPU is halted, so no program sees a partial one), and its per-frame state is
  // the only thing carried between scanlines.
  std::array<DmaChannel, 8> dma{};
  std::uint8_t mdmaen = 0;      // $420B: the channels a general-purpose DMA is running (a bit clears as its channel finishes)
  std::uint8_t hdmaen = 0;      // $420C: the channels HDMA is enabled on

  std::uint8_t dmaArm = 0;          // CPU cycles left before a triggered DMA engages (the "one more cycle"; 0 = none)
  bool dmaRunning = false;          // a general-purpose DMA holds the bus
  bool dmaOpened = false;           // the alignment pad and whole-transfer overhead have been paid
  bool dmaChannelOpened = false;    // the current channel's overhead has been paid
  std::uint8_t dmaUnit = 0;         // the byte position within the transfer pattern for the current channel
  std::uint64_t dmaPauseMaster = 0; // the master counter at the transfer's pause, for the resume rounding
  bool dmaResumePad = false;        // the first CPU cycle after the transfer owes the resume-rounding pad

  std::uint8_t hdmaActive = 0;   // channels still running HDMA this frame (a bit clears when a table terminates)
  std::uint8_t hdmaDoWrite = 0;  // per channel: whether this scanline delivers a value (rather than waiting)
  bool hdmaInited = false;       // the start-of-frame initialisation has run this frame
  bool hdmaLineFired = false;    // this scanline's delivery has been triggered
  bool hdmaRunPending = false;   // an HDMA event is due on the next machine cycle
  bool hdmaIniting = false;      // that pending event is the start-of-frame init (rather than a delivery)
};

class Snes {
 public:
  // Builds the machine from a cartridge and seeds the power-on state: work RAM
  // cleared, the APU in its post-boot ready state, and the CPU in emulation mode
  // with its program counter at the cartridge's reset vector.
  explicit Snes(SnesConfig config);

  // The whole machine as a value. state() is coherent at any cycle the machine has
  // stopped on, mid-instruction included; restore() replaces the mutable machine
  // and resumes exactly there, keeping the cartridge and clock rate in place.
  [[nodiscard]] const SnesState& state() const noexcept { return state_; }
  // Takes the state by const reference rather than by value: a SnesState is a quarter
  // of a megabyte, and a by-value parameter would copy it onto the caller's stack.
  void restore(const SnesState& state);

  // The clock rate the machine was built at. Fixed for its life, like the
  // cartridge.
  [[nodiscard]] Region region() const noexcept { return region_; }

  // Runs master cycles to the end of one CPU instruction and returns how many it
  // took. Called after run() stopped mid-instruction, it finishes the instruction
  // in progress. A halted core runs one idle cycle and returns its cost, so the
  // APU keeps going while the CPU sits.
  std::uint32_t step();

  // Runs exactly `budget` master cycles and returns that count. A cycle is priced
  // by its region, so the machine may pass the budget part-way through a cycle; the
  // overshoot is carried into the next call, making run(a) then run(b) advance the
  // machine exactly as run(a + b). run(0) runs nothing.
  std::uint64_t run(std::uint64_t budget);

  // Drains the 32 kHz stereo frames the APU has produced since the last drain. The
  // machine paces the APU, so frames accumulate as it runs; a caller drains
  // periodically to bound the queue. Frames are output, not state.
  [[nodiscard]] std::vector<StereoFrame> takeFrames();

  // The controller in a port: a pad, or nothing, which is how the machine starts.
  // The program sees it the way it sees a real one — through the auto-read at
  // $4218-$421F once a frame when $4200 bit 0 enables it, and through the serial
  // ports at $4016/$4017 whenever it strobes and clocks them — so a pad presented
  // before a frame's vertical blank is what that frame's read returns. The pad is
  // part of the machine's state: a snapshot carries it and restore() puts it back.
  void setJoypad(JoypadPort port, std::optional<Joypad> pad) noexcept;
  [[nodiscard]] const std::optional<Joypad>& joypad(JoypadPort port) const noexcept;

  // The video memory a host reads to see what the program drew. Nothing renders it —
  // the register ports store here the way the console does, and these faces hand the
  // bytes back. VRAM is 64 KB (32K words), CGRAM 512 bytes (256 palette words).
  [[nodiscard]] std::span<const std::uint8_t> vram() const noexcept { return state_.vram; }
  [[nodiscard]] std::span<const std::uint8_t> cgram() const noexcept { return state_.cgram; }

 private:
  // The mapped bus the CPU runs over. Each access records its region's master cost
  // on the machine and routes to work RAM, the cartridge, or a register; an
  // internal cycle drives an address without an access and costs the fast rate.
  struct Bus {
    Snes& m;
    std::uint8_t read(std::uint32_t address, CycleKind) { return m.busRead(address); }
    void write(std::uint32_t address, std::uint8_t value, CycleKind) {
      m.busWrite(address, value);
    }
    void internal(std::uint32_t) { m.busInternal(); }
    void internal(std::uint32_t, CycleKind) { m.busInternal(); }
  };

  // One master-cycle group: the CPU makes its single access (which prices the
  // cycle), the master counter advances by that cost, and the APU is paced forward
  // by the master cycles it now owes.
  void machineCycle();

  // Advances the machine's own events by `cost` master cycles: the H/V counters and
  // the vblank flag, the H/V-timer compare, the arithmetic unit, and the auto-joypad
  // window. It runs before the cycle's memory access resolves, so a register read
  // sees the event it shares the cycle with. Called exactly once per cycle.
  void tickVideo(std::uint32_t cost);

  // Recomputes the NMI and IRQ line levels from the flags and enables as they now
  // stand and drives them onto the core. It runs after the cycle's access, so a
  // write that enables an interrupt takes effect this cycle and is sampled next.
  void driveLines();

  // The master-cycle length of the current scanline: 1364, except NTSC's line 240 on
  // an odd frame, which is four cycles short to keep the colour signal in step.
  [[nodiscard]] std::uint16_t lineLength() const noexcept;

  // Commits the arithmetic result when its cycle countdown expires.
  void commitMath() noexcept;

  // Reloads the live CPU and APU from state_ after a construct or restore.
  void load();
  // Copies the live CPU and APU back into state_ before a public return.
  void sync();

  std::uint8_t busRead(std::uint32_t address);
  void busWrite(std::uint32_t address, std::uint8_t value);
  void busInternal() {
    lastCost_ = 6;
    if (state_.dmaResumePad) {  // an internal cycle can be the first one after a transfer
      lastCost_ += resumePad(6);
      state_.dmaResumePad = false;
    }
    tickVideo(lastCost_);  // an internal cycle drives an address but makes no access; it still passes time
    videoAdvanced_ = true;
  }

  // The mapped bus without the pricing: which byte an address reaches (with a
  // register's read or write side effect), and nothing about the cycle's cost or
  // its tick. busRead/busWrite price and tick a CPU access and then route through
  // these; the DMA engine routes two accesses through them under one priced cycle.
  std::uint8_t routeRead(std::uint32_t address);
  void routeWrite(std::uint32_t address, std::uint8_t value);

  // The general-purpose DMA engine ($420B): trigger, and one machine cycle of a
  // running transfer (an overhead cycle or a single byte, priced at eight master
  // cycles). The engine holds the bus between the CPU's instructions.
  void triggerDma(std::uint8_t channels);
  void dmaCycle();
  // The A-bus side of a DMA byte: a read of a memory-mapped region returns open
  // bus and a write to one is inert, the way the console forbids DMA there.
  std::uint8_t dmaReadA(std::uint32_t address);
  void dmaWriteA(std::uint32_t address, std::uint8_t value);
  [[nodiscard]] static bool aBusExcluded(std::uint32_t address) noexcept;
  // The resume-rounding pad added to the first CPU cycle after a transfer, so the
  // machine resumes on a whole CPU-clock boundary since the pause.
  [[nodiscard]] std::uint32_t resumePad(std::uint32_t cpuCycle) const noexcept;

  // The HDMA engine: one whole event — the start-of-frame initialisation, or a
  // single visible scanline's delivery for every active channel — and the helper
  // that loads the next table entry into a channel.
  void hdmaCycle();
  void hdmaLoadEntry(DmaChannel& channel, bool indirect);

  // The DMA channel registers ($4300-$437F): the eight channels' sixteen-byte
  // register files, read and written by their documented layout.
  std::uint8_t readDmaReg(std::uint16_t offset);
  void writeDmaReg(std::uint16_t offset, std::uint8_t value);

  // The master-cycle cost of reaching `address`, by the documented region map. The
  // second waitstate region ($80-$BF:$8000-$FFFF and $C0-$FF) follows MEMSEL.
  [[nodiscard]] std::uint32_t accessCost(std::uint32_t address) const noexcept;

  // The cartridge byte an address reaches under the machine's map, mirrored across
  // the image; zero for an address that reaches no cartridge. Pure — it neither
  // prices the cycle nor touches the data bus.
  [[nodiscard]] std::uint8_t romByte(std::uint8_t bank, std::uint16_t offset) const noexcept;

  // Whether an address lands on the cartridge under the machine's map, and whether
  // it lands on save RAM. Both answer through the cartridge functions, so the
  // machine reads an image exactly where the header says it is. saveRamIndex
  // answers the offset into the save, already reduced to its size, for an address
  // that reaches it — nothing when the cartridge has no save.
  [[nodiscard]] bool addressIsRom(std::uint8_t bank, std::uint16_t offset) const noexcept;
  [[nodiscard]] std::optional<std::size_t> saveRamIndex(std::uint8_t bank,
                                                        std::uint16_t offset) const noexcept;

  // The work-RAM data port: $2180 reads or writes work RAM at the port address and
  // steps it; $2181-$2183 set the address and read back as open bus.
  std::uint8_t readWramPort(std::uint16_t offset);
  void writeWramPort(std::uint16_t offset, std::uint8_t value);

  // The PPU register file ($2100-$213F): the forced-blank and background fields, the
  // VRAM and CGRAM ports with their address translation and prefetch. Nothing renders.
  std::uint8_t readPpuReg(std::uint16_t offset);
  void writePpuReg(std::uint16_t offset, std::uint8_t value);

  // The CPU-side registers ($4200-$421F): interrupt enables and flags, the H/V timer
  // settings, the multiply/divide unit, and the auto-joypad read.
  std::uint8_t readCpuReg(std::uint16_t offset);
  void writeCpuReg(std::uint16_t offset, std::uint8_t value);

  // The serial controller ports: a write to $4016 drives the strobe line, and a
  // read of $4016 or $4017 returns a port's next bit and clocks its register.
  std::uint8_t readJoypadPort(std::uint16_t offset);
  void writeJoypadStrobe(std::uint8_t value) noexcept;
  // Latches every port's sixteen bits — the strobe pulse a program or the
  // auto-read gives — and one bit clocked out of a port's register.
  void latchJoypads() noexcept;
  [[nodiscard]] std::uint8_t clockJoypad(std::size_t port) noexcept;

  // The VRAM word the address currently reaches, after any $2115 address translation.
  [[nodiscard]] std::uint16_t vramWordAddress() const noexcept;
  // The 16-bit word at that address, the value the read-prefetch register takes.
  [[nodiscard]] std::uint16_t readVramWord() const noexcept;
  // Advances the VRAM word address by the step $2115 selects, after a low- or
  // high-byte access as the increment mode directs.
  void stepVramAddress(bool highByte) noexcept;

  // Moves the beam to the next scanline, wrapping the frame and toggling its parity,
  // and setting or clearing the vblank flag and starting the auto-joypad read at the
  // boundaries the console does.
  void advanceLine() noexcept;
  // The auto-read's end: the sixteen bits it clocked out of each port land in
  // $4218-$421F.
  void finishAutoJoypadRead() noexcept;
  // Whether the H/V-timer condition currently holds, by the mode $4200 selects.
  [[nodiscard]] bool irqConditionMet() const noexcept;

  // Records `value` as the data bus's last byte and returns it, so an unmapped read
  // that follows sees it.
  std::uint8_t latch(std::uint8_t value) noexcept {
    state_.mdr = value;
    return value;
  }

  // The CPU's power-on state: emulation mode, the interrupt disable set, and the
  // program counter at the cartridge's reset vector.
  [[nodiscard]] Cpu65816State powerOnCpu() const;

  Cpu65816 cpu_;                     // the live CPU while the machine runs
  Apu apu_;                          // the live audio machine, paced by the interleave
  SnesState state_;                  // work RAM, registers and counters are authoritative here
  std::vector<std::uint8_t> rom_;    // the cartridge image, fixed for the machine's life
  Region region_ = Region::Ntsc;     // the clock rate, fixed for the machine's life
  CartridgeMap map_ = CartridgeMap::LoRom;  // how that image lays across the bus, fixed with it
  std::uint32_t apuNum_ = 5632u;     // the APU-to-master cycle ratio for this region (numerator)
  std::uint32_t apuDen_ = 118125u;   // and its denominator
  std::uint32_t lastCost_ = 6;       // the master cost of the cycle in progress
  bool videoAdvanced_ = false;       // whether this cycle's access already ticked the machine's events
};

}  // namespace snaggletooth
