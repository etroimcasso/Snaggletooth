#pragma once

// The SNES machine — the 5A22 (its 65816 core) wired to the console's memory map,
// the work RAM, the PPU's register file, and the APU across the communication ports.
//
// The machine owns the CPU, the 128 KB of work RAM, the PPU's state, and the audio
// machine. Its bus maps a 24-bit address the way the console does: work RAM in
// banks $7E-$7F and mirrored into the low pages of every system bank, the
// cartridge ROM and its save RAM in whichever windows the cartridge's map gives
// them, the PPU at $2100-$213F, the APU communication ports at $2140-$2143, and
// the work-RAM data port at $2180-$2183. A read of an unmapped address returns
// the last value the data bus carried.
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
//
// A host that wants to see the bus rather than the state sets an observer: it is
// told every access the machine makes — the CPU's with the kind the core drove,
// the transfer engines' bytes, the work-RAM port's own reads and writes — and
// every internal CPU cycle, in the order they happen.

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "snaggletooth/apu/apu.h"
#include "snaggletooth/cpu/cpu65816.h"
#include "snaggletooth/snes/cartridge.h"
#include "snaggletooth/snes/ppu.h"
#include "snaggletooth/snes/video_frame.h"

namespace snaggletooth {

// The console clock rate. It sets the master clock, and so the ratio the APU is
// paced against; the memory-region speeds, counted in master cycles, are the same
// in both.
enum class Region : std::uint8_t { Ntsc, Pal };

// What a console of this region runs at: its master clock as the exact ratio it is
// — the 60 Hz console's is 236,250,000 / 11 Hz, which is not a whole number of
// cycles a second — and the master cycles one of its frames takes.
//
// The frame is the one a picture that is not interlaced draws: 262 lines of 1364
// master cycles less the four that one line of every second frame drops, and 312
// of the same. An interlaced picture carries a line more on one field of every
// pair, so a run that interlaces is not held to this.
struct ConsoleClock {
  std::uint64_t masterCyclesPerFrame = 0;
  std::uint64_t hertzNumerator = 0;
  std::uint64_t hertzDenominator = 1;
};

[[nodiscard]] constexpr ConsoleClock consoleClock(Region region) noexcept {
  return region == Region::Pal ? ConsoleClock{.masterCyclesPerFrame = 425568u,
                                              .hertzNumerator = 21281370u,
                                              .hertzDenominator = 1u}
                               : ConsoleClock{.masterCyclesPerFrame = 357366u,
                                              .hertzNumerator = 236250000u,
                                              .hertzDenominator = 11u};
}

// When each frame of a run is owed, in nanoseconds from the run's start — the
// console's own rate, for a host holding a run to it.
//
// It does not read a clock and does not wait: a host that wants to show a run at
// the speed the console ran it asks when the next frame is due and waits however
// it waits, which keeps the sleeping where the host's own loop is. A host with a
// clock of its own — one stepping the machine a tick at a time — ignores this and
// spends cycle budgets instead.
//
// The frame count and the interval are multiplied apart rather than together. The
// whole product passes 64 bits inside a couple of minutes, and a deadline that has
// wrapped is behind the clock, so a run stops waiting between frames and sprints
// until the wrap climbs back past it. Split into whole nanoseconds and the
// fraction left over, every deadline is exact out past 150 billion frames, which
// is eighty years at the console's rate.
class FrameDeadline {
 public:
  explicit constexpr FrameDeadline(Region region) noexcept
      : divisor_(consoleClock(region).hertzNumerator),
        whole_(nanosNumerator(region) / consoleClock(region).hertzNumerator),
        remainder_(nanosNumerator(region) % consoleClock(region).hertzNumerator) {}

  // The nanosecond the Nth frame of the run is owed at.
  [[nodiscard]] constexpr std::uint64_t owedAt(std::uint64_t frames) const noexcept {
    return frames * whole_ + frames * remainder_ / divisor_;
  }

  // One frame's interval, in whole nanoseconds and the fraction left over it.
  [[nodiscard]] constexpr std::uint64_t wholeNanos() const noexcept { return whole_; }

 private:
  static constexpr std::uint64_t nanosNumerator(Region region) noexcept {
    const ConsoleClock clock = consoleClock(region);
    return 1000000000ull * clock.masterCyclesPerFrame * clock.hertzDenominator;
  }

  std::uint64_t divisor_;
  std::uint64_t whole_;
  std::uint64_t remainder_;
};

// Both regions' first frame and one past the point a single product passes 64
// bits, worked out by hand, so a rearrangement that loses the fraction or wraps
// again does not compile.
static_assert(FrameDeadline(Region::Ntsc).owedAt(1u) == 16639263ull);
static_assert(FrameDeadline(Region::Ntsc).owedAt(4693u) == 78088063568ull);
static_assert(FrameDeadline(Region::Pal).owedAt(1u) == 19997208ull);
static_assert(FrameDeadline(Region::Pal).owedAt(4693u) == 93846901021ull);

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

  [[nodiscard]] bool operator==(const DmaChannel&) const noexcept = default;
};

// A standard controller's twelve buttons, in the order the pad shifts them out —
// which is the order they sit in the word the auto-read registers hold, and the
// order a recorded run names them in. The set is the console's, so it is fixed:
// a controller with other buttons is a different controller, not a longer list.
enum class Button : std::uint8_t {
  B, Y, Select, Start, Up, Down, Left, Right, A, X, L, R,
};
inline constexpr std::size_t kButtonCount = 12;

// Every button, in that order, for a caller walking the set.
[[nodiscard]] std::span<const Button> buttons() noexcept;

// What a button is called — "b", "select", "l" — and the button a name stands
// for, or nothing for a word that names none. Names are read in any case. This is
// the one place the twelve are spelled: a tool that reads a button out of a file
// and a host that prints one both ask here, so no two of them can disagree.
[[nodiscard]] std::string_view buttonName(Button button) noexcept;
[[nodiscard]] std::optional<Button> buttonFromName(std::string_view name) noexcept;

// A standard controller's twelve buttons as a value: true is pressed. A pad is
// presented to the machine with Snes::setJoypad and read by the program through
// the auto-read registers or the serial ports; the machine samples it when it
// latches, so a value set at any point in a frame is what that frame's read sees.
//
// A pad with nothing pressed is not an empty port: Snes::setJoypad takes an
// optional, and nothing at all is a socket with no controller in it, which a
// program can tell apart from this. See the port comment on setJoypad.
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

  // One button by name, read and written, so a caller working from the Button set
  // does not repeat the twelve fields to reach them.
  [[nodiscard]] bool holds(Button button) const noexcept;
  void hold(Button button, bool pressed) noexcept;

  // The sixteen bits the pad shifts out, in the layout the auto-read registers
  // hold them: bit 15 is B, the first bit on the wire, down to bit 4 for R; bits
  // 3-0 are the standard pad's identity code, all zero. The high byte is what
  // $4219 reads, the low byte $4218.
  [[nodiscard]] std::uint16_t bits() const noexcept;

  [[nodiscard]] bool operator==(const Joypad&) const noexcept = default;
};

// The two controller ports on the console's front.
enum class JoypadPort : std::uint8_t { One, Two };

// Who made a bus access. The CPU narrates every cycle of its own; the two
// transfer engines make their accesses while the CPU is held off the bus; the
// work-RAM port reaches work RAM on its own behalf when a program or a transfer
// moves a byte through $2180.
enum class AccessSource : std::uint8_t { Cpu, Dma, Hdma, WramPort };

// One access as the machine made it: the 24-bit address, the byte that crossed
// the bus, which way, what the cycle was for, and who made it. A transfer
// engine's accesses are plain data reads and writes; the CPU's carry the kind
// the core drove. An engine's access also says which of the eight channels it
// served, and the HDMA engine's whether it was reading its own table. A write
// to a video data port says where the port put the byte, which the address
// and the value alone cannot tell.
struct BusAccess {
  std::uint32_t address = 0;
  std::uint8_t value = 0;
  bool write = false;
  CycleKind kind = CycleKind::DataRead;
  AccessSource source = AccessSource::Cpu;
  std::uint8_t channel = 0;  // the channel an engine's access served, 0-7; 0 for the CPU's and the port's
  bool table = false;        // the HDMA engine reading its table — a line count, a direct table's inline value, an indirect entry's pointer — rather than a byte an indirect entry points at, or any write
  // A `table` read that lies past the table's end. An indirect channel whose count
  // comes up $00 still reads the one or two bytes after it, where an entry's pointer
  // would be; they belong to whatever follows the table, and the read says so here.
  bool pastTableEnd = false;
  // Where a write to a video data port landed: the VRAM word address for a
  // write to $2118 or $2119, after any address translation; the palette word
  // for a write to $2122, on both halves; the OAM byte for a write to $2104,
  // on both halves, within the 544 bytes. Absent on every other access.
  std::optional<std::uint16_t> landed;
};

// What a machine tells about every access it makes, and every CPU cycle that
// drives an address without one. A host derives the machine's whole observable
// behaviour from it: which addresses were read and written in what order, and
// how many cycles the CPU spent between two instruction boundaries — every CPU
// access plus every internal cycle. A halted cycle drives nothing and is not
// reported, and neither are a transfer engine's overhead cycles; the transfer's
// bytes are.
class BusObserver {
 public:
  virtual ~BusObserver() = default;

  // An access that crossed the bus, after its value is settled: a read carries
  // what the bus answered, a write what the source drove.
  virtual void access(const BusAccess& access) = 0;

  // A CPU cycle that drove `address` without a valid access. `kind` is set when
  // the pins say what the cycle was for — a read-modify-write's modify cycle —
  // and absent for a plain internal cycle.
  virtual void internal(std::uint32_t address, std::optional<CycleKind> kind) = 0;
};

// What a machine tells about the cartridge's battery-backed save window: that a
// frame changed it, and what it holds now. The machine says WHEN and hands over
// the bytes; where a save is kept, what it is called and whether it is written at
// all are the host's, and the machine never learns any of it.
class SaveObserver {
 public:
  virtual ~SaveObserver() = default;

  // The save window changed during the frame just finished, and `save` is the
  // whole of it as it now stands — the size the cartridge declares, not the bytes
  // that moved. Told once per frame however many stores landed in it, and not at
  // all in a frame where none did. The span is the machine's own storage and is
  // valid for the call.
  virtual void changed(std::span<const std::uint8_t> save) = 0;
};

// A host told, before a watched access takes effect, that the program is
// reading or writing a place it armed (Snes::watchAccess), and answering what
// happens (AccessAnswer, `apu/apu.h`). A place is a byte, not a bus address:
// the low 8 KB of work RAM answers at $7E:0000-$1FFF and at $0000-$1FFF of every
// system bank, every register answers at its offset in all 128 system banks,
// and the cartridge's image and save repeat across the banks the map gives
// them — arming any one of a byte's addresses arms the byte, and the watcher is
// told the 24-bit `address` the access drove, whichever alias it was.
//
// `source` is which part of the machine made the access; `kind` is what the
// cycle was for, as the CPU core drives it (`cpu/cpu65816.h`), so an opcode
// fetch and a data read of one address are told apart, and the read half of a
// read-modify-write from a load; an engine's and the port's accesses are plain
// data reads and writes. `cycle` is which cycle of the instruction the access
// is, counted as the chip spends them: 0 is the opcode fetch, every cycle counts
// whether or not it reaches the bus, a 16-bit load's low byte is at one cycle
// and its high byte at the next, a 16-bit push writes its high byte first, a
// native 16-bit read-modify-write reads low then high and writes back high then
// low, and an emulation-mode read-modify-write writes its address twice — the
// byte it read (RmwModifyWrite), then the new one (RmwWrite). For a transfer
// engine, `cycle` is the byte's position in the channel's transfer pattern,
// the read and the write of one byte carrying the same one; a table read
// carries 0 for a line count or a pointer's low byte and 1 for its high byte.
// The work-RAM port's own access carries the cycle of the access to $2180 that
// drove it. Width is not told — the chip does not know it at the bus — so a
// host answering one byte of a two-byte access is answering that byte alone.
//
// A register with a read side effect has already had it when its read is told:
// a read of $4210 has cleared the NMI flag, a read of $2140-$2143 has taken the
// port's byte. The answer changes only what the program receives; a veto
// cannot undo the effect, and nothing can, because a read cannot be prevented.
//
// A mechanism beside BusObserver, which still reports every settled access
// after the fact and cannot answer. Set by pointer like the observers: the
// host's object outlives every step it is set for, and it is not part of the
// state, so a snapshot does not carry it and restore() leaves it in place.
class AccessWatcher {
 public:
  virtual ~AccessWatcher() = default;
  // The program is about to read `address`, where the machine would answer
  // `value`.
  virtual AccessAnswer read(std::uint32_t address, std::uint8_t value, AccessSource source,
                            CycleKind kind, std::uint8_t cycle) = 0;
  // The program is about to write `value` to `address`.
  virtual AccessAnswer write(std::uint32_t address, std::uint8_t value, AccessSource source,
                             CycleKind kind, std::uint8_t cycle) = 0;
};

// What stands at a watched address while it is armed (Snes::watchInstruction):
// nothing, so the instruction there runs once the host has been told; or a
// return, so the routine there never runs — the fetch that begins the
// instruction answers the return's opcode in place of the byte the cartridge
// holds, and the one instruction that runs is the return.
enum class Standin : std::uint8_t {
  None,  // the instruction runs after the host is told
  Near,  // a return within the bank (RTS), for a routine a JSR entered
  Long,  // a return across banks (RTL), for one a JSL entered
};

// A host told, before the instruction at a watched address runs, that the CPU
// has reached it. Told once per instruction the chip begins — a block move
// begins each byte it moves as an instruction of its own — at an instruction
// boundary the CPU takes as a boundary: not while a transfer engine holds the
// bus, not while the core is halted, and not at a boundary a hardware interrupt
// takes, where the interrupted instruction is told when the handler returns to
// it. A watched place is a byte, as an access watch's is (physical): arming a
// routine in the low 8 KB of work RAM through bank $7E hears it entered from a
// system bank, and `address` is the 24-bit address the fetch drives.
//
// The machine's clock has not moved for this call: the host runs on its own
// time, and the cycles the instruction — or the return standing in for it —
// spends are the same as with no host at all. cpuState() is live inside the
// call, and a register file written with setCpuState() inside it is what the
// instruction runs under. The watcher is the host's object, set by pointer and
// not part of the state, so a snapshot does not carry it and restore() leaves
// it in place.
class InstructionWatcher {
 public:
  virtual ~InstructionWatcher() = default;
  // The instruction at `address` is about to run.
  virtual void reached(std::uint32_t address) = 0;
};

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
  // It is the map of the board the machine builds (`cartridgeBoard`).
  std::optional<CartridgeMap> map = std::nullopt;

  // The save RAM to give the machine, in bytes. Left absent, it is taken from the
  // cartridge header. Set it to zero to run a cartridge without its save. It
  // sizes the save and the board both: with zero, the board reads as one with
  // no save, so a LoROM window reads the image and a HiROM window open bus.
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
  // snapshot resumes on the exact dot. The frame-parity bit toggles as each frame's
  // first line begins and chooses the frame's one irregular scanline.
  std::uint16_t hpos = 0;   // master cycles into the current scanline (0..lineLength-1)
  std::uint16_t vpos = 0;   // the current scanline (0..261 NTSC, 0..311 PAL, one further under interlace)
  // The frame parity, which STAT78 bit 7 answers: 0 names the first frame of an
  // interlaced pair and 1 the second. It toggles at H = 1 of line 0, and the console
  // comes out of reset at H = 0, V = 0 with the flag clear — so the first frame it
  // runs is the pair's second, four cycles after the machine starts.
  std::uint8_t field = 0;

  // Vertical blank is a latched fact, not a comparison: the machine decides at the
  // start of each line from kVblankStartLine on, and once it has begun it holds to
  // the frame's end no matter what SETINI says afterwards. The line it began on
  // dates the two events that follow — the NMI flag and the sprite table's reload.
  bool inVblank = false;
  std::uint16_t vblankBeginLine = 0;

  // The length of the line before this one, which dates the H/V timer's trigger
  // point when HTIME is zero: that point is 1374 master cycles after the previous
  // line began. A normal line at power-on, as the line before line 0 is.
  std::uint16_t previousLineMaster = 1364;

  // The memory refresh. The CPU is held off the bus for forty master cycles once a
  // line, at a point that walks an eight-cycle grid. Both fields are state: a
  // snapshot taken inside a pause restores into the rest of it.
  std::uint64_t refreshAt = 538;  // the master cycle the next pause begins after
  std::uint8_t refreshLeft = 0;   // master cycles left in the pause in progress (0 = none)

  // ---- the interrupt registers ----------------------------------------------
  std::uint8_t nmitimen = 0;    // $4200: bit7 NMI enable, bits5-4 H/V IRQ mode, bit0 auto-joypad enable
  bool vblankNmi = false;       // $4210 bit7: set at the start of vblank, cleared on read and at vblank's end
  bool timeup = false;          // $4211 bit7: set when the H/V counter reaches its timer, cleared on read or write
  std::uint16_t htime = 0x01FF; // $4207/$4208: the H-count IRQ position, in dots (0..339)
  std::uint16_t vtime = 0x01FF; // $4209/$420A: the V-count IRQ position, in lines (0..261/311)

  // ---- the multiply/divide unit ---------------------------------------------
  // A write to the multiplier or the divisor starts the unit on the operands as
  // they stand at that write; the result is ready a fixed number of cycles later.
  // Until then the result registers hold their previous contents (the intermediate
  // is not documented, so it is not invented) — except that starting a multiply
  // immediately loads the quotient register with the multiplier, a documented quirk
  // of the shared unit. An operand register written while the unit runs is the next
  // job's, not this one's: the running job keeps the pair it started with.
  std::uint8_t wrmpya = 0xFF;   // $4202: the multiplicand
  std::uint8_t wrmpyb = 0xFF;   // $4203: the multiplier (its write starts a multiply)
  std::uint16_t wrdiv = 0xFFFF; // $4204/$4205: the dividend
  std::uint8_t wrdivb = 0xFF;   // $4206: the divisor (its write starts a divide)
  std::uint16_t rddiv = 0;      // $4214/$4215: the quotient
  std::uint16_t rdmpy = 0;      // $4216/$4217: the product, or the division remainder
  std::uint8_t mathClocks = 0;  // CPU cycles left before the result lands (0 = idle)
  MathOp mathOp = MathOp::None; // which result the pending job will commit
  std::uint16_t mathLeft = 0;   // the running job's multiplicand or dividend, as it stood at the start
  std::uint8_t mathRight = 0;   // the running job's multiplier or divisor, the same

  // ---- the controller ports -------------------------------------------------
  // The pads presented to the two ports (none by default, which reads as no
  // controller: every bit zero), the strobe line the program drives through
  // $4016, and each port's shift register — the sixteen bits latched at the
  // strobe's fall and how many of them have been clocked out. The auto-read uses
  // the same strobe and clock lines, so its sixteen clocks leave a port's register
  // at its padding until the program strobes again. When enabled, the auto-read
  // begins on the first line of vertical blank at a point on a 256-cycle grid
  // carried from one read to the next, strobes the pads, and clocks one bit out
  // of every port at a time into $4218-$421F, each register shifting as its bit
  // arrives; the busy flag holds from the start until the sixteenth bit lands,
  // 4224 master cycles later.
  std::array<std::optional<Joypad>, 2> pads{};  // what is plugged into each port
  bool joyStrobe = false;               // $4016 bit 0 as last written: the latch line held high
  std::array<std::uint16_t, 2> joyLatch{};  // per port: the bits latched at the strobe's fall
  std::array<std::uint8_t, 2> joyClocks{};  // per port: bits clocked out since the latch (16 = at the padding)
  std::uint64_t autoJoyStart = 0;       // the master cycle the latest auto-read began (0 = none yet); the next begins on its 256-cycle grid
  std::uint8_t autoJoyClocked = 0;      // bits the auto-read has clocked into the registers so far (16 = the read is done)
  std::array<std::uint8_t, 8> joy{};    // $4218-$421F: the four 16-bit pad reads, shifting as the auto-read clocks them
  // The programmable I/O port ($4201 written, $4213 read back). Its top bit is the
  // PPU's counter-latch line: while high the software latch works, and its fall
  // latches the counters itself. Every line reads back as written, since nothing
  // on the console drives one.
  std::uint8_t wrio = 0xFF;
  // The counter latch owed by a fall of the I/O port's top bit: the master cycle
  // the beam passes for it, one dot past where a $2137 read of the write's cycle
  // would land. The latch is captured there rather than at the write, so this
  // holds the debt between the two; zero when none is owed.
  std::uint64_t counterLatchAt = 0;

  // ---- the PPU ------------------------------------------------------------
  // The picture processor's whole state — its register file, the latches, and the
  // three video memories — as one value (`ppu.h`). Held here and nowhere else, so
  // a snapshot carries it and restore() puts it back.
  PpuState ppu{};

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

  std::uint8_t hdmaActive = 0;   // channels that have taken part in HDMA this frame and whose tables are still running; a line delivers on the channels this and hdmaen both name
  std::uint8_t hdmaEnded = 0;    // channels whose tables terminated this frame, which $420C cannot restart before the next one
  std::uint8_t hdmaDoWrite = 0;  // per channel: whether this scanline delivers a value (rather than waiting)
  bool hdmaInited = false;       // the start-of-frame initialisation has run this frame
  bool hdmaLineFired = false;    // this scanline's delivery has been triggered
  bool hdmaRunPending = false;   // an HDMA event is due on the next machine cycle
  bool hdmaIniting = false;      // that pending event is the start-of-frame init (rather than a delivery)

  [[nodiscard]] bool operator==(const SnesState&) const noexcept = default;
};

class Snes {
 public:
  // Builds the machine from a cartridge and seeds the power-on state: work RAM
  // cleared, the APU in its post-boot ready state, and the CPU in emulation mode
  // with its program counter at the cartridge's reset vector.
  explicit Snes(SnesConfig config);

  // A machine is moved, never copied or assigned: the audio machine inside it
  // runs in the machine's own state, and a move carries the machine after its
  // state to the new place. A copy would run two audio machines over one state.
  // The move names every member in `snes.cpp`; a member added to the machine
  // joins that list.
  Snes(const Snes&) = delete;
  Snes& operator=(const Snes&) = delete;
  Snes(Snes&& moved) noexcept;
  Snes& operator=(Snes&&) = delete;

  // The whole machine as a value. state() is coherent at any cycle the machine has
  // stopped on, mid-instruction included, and answers the machine's own state
  // without copying it; restore() replaces the mutable machine and resumes exactly
  // there, keeping the cartridge and clock rate in place.
  [[nodiscard]] const SnesState& state() const noexcept { return state_; }
  // Takes the state by const reference rather than by value: a SnesState is a quarter
  // of a megabyte, and a by-value parameter would copy it onto the caller's stack.
  void restore(const SnesState& state);

  // The console's reset line, pulled and let go: what the button on the console
  // does. The machine starts again where construction starts it in time — the CPU
  // about to fetch the first opcode at the cartridge's reset vector, the beam at
  // H = 0, V = 0 with the frame parity clear, the master counter at zero — and
  // keeps everything a reset does not initialise.
  //
  // The CPU takes the registers `afterReset` gives (`cpu/cpu65816.h`). $4200,
  // $420B, $420C and $420D go to $00 and $4201 to $FF; the NMI and IRQ flags, the
  // joypad strobe, $4218-$421F and the work-RAM port's address clear. $4202-$420A,
  // the arithmetic unit's results and every $43xx register keep what they held,
  // and so do work RAM, the save, and the pads in the ports. The PPU is forced
  // blank at the brightness it had; its other registers and its three memories
  // stand. The audio machine goes through Apu::reset(), and runs its boot program
  // again when the machine was built to run one. A transfer, an arithmetic job or
  // an auto-read in progress is abandoned, and the picture the beam was part-way
  // down is never delivered.
  //
  // The sequence's seven cycles are not spent and its five reads are not made,
  // as construction does not make them: both leave the machine at the instant the
  // sequence ends. Call it between step() and run() calls, never from inside an
  // observer's call; a budget run() was still owed is dropped with the counter.
  // The observers stay set.
  void reset();

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

  // The video memory a host reads to see what the program put there. The PPU's
  // ports fill it the way the console does (`ppu.h`), and these faces hand the
  // bytes back. VRAM is 64 KB (32K words), CGRAM 512 bytes (256 palette words), OAM
  // 544 bytes (128 four-byte sprite entries and 32 bytes of their high bits).
  [[nodiscard]] std::span<const std::uint8_t> vram() const noexcept { return state_.ppu.vram; }
  [[nodiscard]] std::span<const std::uint8_t> cgram() const noexcept { return state_.ppu.cgram; }
  [[nodiscard]] std::span<const std::uint8_t> oam() const noexcept { return state_.ppu.oam; }

  // The observer told every access the machine makes and every internal CPU
  // cycle, or none, which is how the machine starts. It is the host's object and
  // outlives every step it is set for; it is not part of the state, so a snapshot
  // does not carry it and restore() leaves it in place. With none set an access
  // costs one check.
  void setObserver(BusObserver* observer) noexcept { observer_ = observer; }
  [[nodiscard]] BusObserver* observer() const noexcept { return observer_; }

  // The observer told every frame the PPU finishes (FrameObserver,
  // `video_frame.h`), or none, which is how the machine starts. The machine
  // draws each visible dot only while one is set, so a machine nobody is watching
  // draws nothing; its state, and what a program can observe, are the same either
  // way.
  // Like the bus observer it is the host's object and not part of the state: a
  // snapshot does not carry it and restore() leaves it in place.
  void setFrameObserver(FrameObserver* observer) noexcept;
  [[nodiscard]] FrameObserver* frameObserver() const noexcept { return frameObserver_; }

  // The observer told that a frame changed the cartridge's save window
  // (SaveObserver, above), or none, which is how the machine starts. A host
  // persists a save from it; the machine keeps no file and knows no path. On the
  // same terms as the other observers: the host's object, not part of the state,
  // so a snapshot does not carry it and restore() leaves it in place — though a
  // restore replaces the window wholesale and is therefore reported, the caller
  // having changed it as surely as a store would.
  void setSaveObserver(SaveObserver* observer) noexcept { saveObserver_ = observer; }
  [[nodiscard]] SaveObserver* saveObserver() const noexcept { return saveObserver_; }

  // The audio machine's observer (ApuObserver, `apu/apu.h`), told every access
  // the sound CPU makes and every instruction boundary it crosses, under the
  // same terms as the bus observer: the host's object, not part of the state,
  // none by default. The audio machine runs inside the CPU's cycles, so its
  // report arrives from within step() and run().
  void setApuObserver(ApuObserver* observer) noexcept { apu_.setObserver(observer); }
  [[nodiscard]] ApuObserver* apuObserver() const noexcept { return apu_.observer(); }

  // What a fetch by the sound CPU at `address` returns, without making one:
  // Apu::peek on the live audio machine.
  [[nodiscard]] std::uint8_t peekApu(std::uint16_t address) const noexcept {
    return apu_.peek(address);
  }

  // A host reaching into the machine's memory by 24-bit bus address, without
  // spending a cycle and without a register's side effect. peek answers the
  // byte the address holds — work RAM, the cartridge's ROM, or its save — and
  // std::nullopt for anything this face does not reach as memory: a register,
  // or an address the cartridge leaves open. poke writes that byte and returns
  // whether it landed; a poke to ROM changes the machine's own copy of the
  // image, never a file, and no snapshot carries it. addressable answers, for
  // `bytes` bytes from `address`, whether every one is memory this face reaches;
  // it is peek's own answer, so no two callers are told different things about
  // one address, and a zero-length span is addressable.
  [[nodiscard]] std::optional<std::uint8_t> peek(std::uint32_t address) const noexcept;
  bool poke(std::uint32_t address, std::uint8_t value) noexcept;
  [[nodiscard]] bool addressable(std::uint32_t address, std::size_t bytes) const noexcept;

  // The four memories the bus cannot name, each written the way the chip reads
  // it: no port address steps, no latch moves, no increment happens. VRAM is
  // 64 KB, CGRAM 512 bytes, OAM 544 bytes, and an address past a memory's end
  // is ignored. writeApuRam writes the audio machine's RAM (Apu::writeRam). The
  // picture path reads these memories at every dot, so a write shows at the next
  // one; none of the four is a register write, so none drives a side effect.
  void writeVram(std::uint16_t address, std::uint8_t value) noexcept;
  void writeCgram(std::uint16_t address, std::uint8_t value) noexcept;
  void writeOam(std::uint16_t address, std::uint8_t value) noexcept;
  void writeApuRam(std::uint16_t address, std::uint8_t value) noexcept;

  // The CPU's register file, read and written whole on a stopped machine.
  // cpuState() answers the registers as they stand; setCpuState() reloads the
  // live core from `state`, the way restore() does, so the written set is live
  // for the next cycle. Instruction progress is part of the value: a machine
  // written mid-instruction resumes exactly where the value says.
  [[nodiscard]] const Cpu65816State& cpuState() const noexcept { return state_.cpu; }
  void setCpuState(const Cpu65816State& state);

  // The 32 kHz stereo frames produced since the last drain, into the caller's
  // own storage, returning how many were written. Frames past the end of `into`
  // stay queued for the next drain, and nothing is allocated — a host producing
  // sound on a callback that must not allocate drains here. takeFrames() with no
  // buffer is the drain that allocates a fresh vector.
  [[nodiscard]] std::size_t takeFrames(std::span<StereoFrame> into) noexcept;

  // The watcher (AccessWatcher, above) told every armed access, or none, which
  // is how the machine starts. With none set, or nothing armed, an access pays
  // one test.
  void setAccessWatcher(AccessWatcher* watcher) noexcept { accessWatcher_ = watcher; }
  [[nodiscard]] AccessWatcher* accessWatcher() const noexcept { return accessWatcher_; }

  // Arms or disarms a watch on `bytes` bytes from `address`, for reads, writes,
  // or both. A watch is on the byte an address reaches (physical, below), so
  // arming one of a byte's addresses arms every access to it through any of
  // them, and disarming through another alias disarms it. Arming a place
  // already armed does nothing and disarming one not armed does nothing; the two
  // directions are independent, so a host that wants writes does not make the
  // machine pay for reads. A machine that has never armed a place holds no
  // table at all, and disarming the last place frees it again. A watch sees
  // every access to the place — the CPU's, either transfer engine's, and the
  // work-RAM port's — and tells the watcher which made it.
  void watchAccess(std::uint32_t address, std::size_t bytes, bool onRead, bool onWrite);
  void unwatchAccess(std::uint32_t address, std::size_t bytes, bool onRead, bool onWrite);

  // The byte a bus address reaches, as the machine reads it: which memory, and
  // where in it. Two addresses that answer the same byte classify the same —
  // the low 8 KB of work RAM at $7E:0000-$1FFF and at $0000-$1FFF of every
  // system bank, a register at its offset in every system bank, the save
  // reduced to its size across its window, the image at the offset the map and
  // its mirroring reach — so a watch armed through one alias fires through
  // every other. An address that reaches no byte, open bus, is a place of its
  // own keyed by the address itself. WorkRam's index is the byte's offset in
  // the 128 KB; Register's the 16-bit offset; SaveRam's the offset into the
  // save; CartridgeRom's the offset into the image; OpenBus's the address.
  // Pure: no cycle is spent and no register is touched.
  enum class Space : std::uint8_t { WorkRam, Register, SaveRam, CartridgeRom, OpenBus };
  struct Physical {
    Space space;
    std::uint32_t index;
    [[nodiscard]] bool operator==(const Physical&) const noexcept = default;
  };
  [[nodiscard]] Physical physical(std::uint32_t address) const noexcept;

  // The watcher (InstructionWatcher, above) told each armed instruction the CPU
  // reaches, or none, which is how the machine starts. With none set, or nothing
  // armed, a cycle pays one test.
  void setInstructionWatcher(InstructionWatcher* watcher) noexcept {
    instructionWatcher_ = watcher;
  }
  [[nodiscard]] InstructionWatcher* instructionWatcher() const noexcept {
    return instructionWatcher_;
  }

  // Arms a watch on the instruction at `address`, with `standin` standing there
  // while it is armed: None tells the watcher and lets the instruction run; Near
  // or Long tells the watcher and answers the fetch with a return (Standin,
  // above), so the routine's body never runs and the caller resumes as it would
  // after the routine returned, the return spending exactly what a real one
  // spends. A watch is on the byte the address reaches (physical), so the
  // routine is heard entered through any alias. Arming an armed place replaces
  // what stands there; disarming a place not armed does nothing. A stand-in
  // stands whether or not a watcher is set. The cartridge is never written: peek
  // and a data read of the byte answer the cartridge, and disarming has nothing
  // to put back. A machine that has never armed an instruction holds no table
  // at all, and disarming the last one frees it again.
  void watchInstruction(std::uint32_t address, Standin standin = Standin::Near);
  void unwatchInstruction(std::uint32_t address);

  // Runs the routine at `entry` and returns when it returns: true when it did,
  // false when the guard tripped or the call was refused. The routine is the
  // machine running — its cycles are real and priced by region, the beam moves,
  // the audio machine is paced, a transfer the routine arms runs and a hardware
  // interrupt due is taken — and they are spent from the budget the host runs,
  // so state().master moves and the next run() runs that much less.
  //
  // callInContext runs the routine in the guest's own context: the registers and
  // the stack pointer as they stand, the landing pushed where the guest's own
  // call would push it, and the whole register file put back afterwards, so the
  // interrupted program carries on unaware. What the routine changed in memory
  // stands. A host that wants the routine's registers reads them live inside
  // an instruction watch on the routine's return (Standin::None), before the
  // file goes back.
  //
  // callOnStack runs it in a frame of the host's own: the register file is
  // whatever the host wrote with setCpuState, the stack pointer starts at
  // `stackTop`, and nothing is put back — cpuState() afterwards is the file the
  // routine left, or where it was abandoned, for the host to read and, if it
  // wants the guest's file back, to restore itself.
  //
  // `returns` names the return the routine ends with: Near pushes two bytes for
  // a routine an RTS leaves, Long three for one an RTL leaves. The landing is
  // the program counter as it stands, in the entry's bank for Near and the
  // current bank for Long; nothing there is executed. The call ends at the first
  // instruction boundary where the stack pointer is back at its value before the
  // push and the program counter is at the landing — both, so a routine that
  // branches through the landing without returning does not end it, and an
  // interrupt taken inside the routine returns into it, not out of it. The
  // landing's bytes are pushed through poke, spending no cycle.
  //
  // `guard` bounds a routine that never returns, in instructions the routine may
  // run; an idle cycle of a halted core counts as one, so a wait for an
  // interrupt that never comes trips it too. On overrun the routine is
  // abandoned at its boundary and the call returns false. Zero runs nothing.
  //
  // The call is refused, returning false with nothing done, when `returns` is
  // Standin::None, when the machine is not between instructions (inside an
  // access watcher's call, or after a run() that stopped mid-instruction),
  // when the entry's own bank does not map it (addressable(entry, 1)), or
  // when the stack the landing would land on is not memory this face reaches.
  // A call made from inside a watcher's call, at any depth, is the same call.
  bool callInContext(std::uint32_t entry, Standin returns, std::size_t guard);
  bool callOnStack(std::uint32_t entry, std::uint16_t stackTop, Standin returns,
                   std::size_t guard);

 private:
  // The mapped bus the CPU runs over. Each access records its region's master cost
  // on the machine and routes to work RAM, the cartridge, or a register; an
  // internal cycle drives an address without an access and costs the fast rate.
  // Every call is reported to the observer as the CPU's, with the kind the core
  // drove.
  struct Bus {
    Snes& m;
    std::uint8_t read(std::uint32_t address, CycleKind kind) {
      const std::uint8_t value = m.busRead(address, kind);
      m.observe(address, value, false, kind, AccessSource::Cpu);
      return value;
    }
    void write(std::uint32_t address, std::uint8_t value, CycleKind kind) {
      m.busWrite(address, value, kind);
      m.observe(address, value, true, kind, AccessSource::Cpu);
    }
    void internal(std::uint32_t address) {
      m.busInternal();
      if (m.observer_ != nullptr) m.observer_->internal(address, std::nullopt);
    }
    void internal(std::uint32_t address, CycleKind kind) {
      m.busInternal();
      if (m.observer_ != nullptr) m.observer_->internal(address, kind);
    }
  };

  // Reports one access to the observer, when one is set. `channel` and `table`
  // are an engine's to say; the CPU's accesses leave them at their defaults.
  // Where a video data port put the byte is what the port recorded as the
  // access routed through it, taken here so the next access starts clear.
  void observe(std::uint32_t address, std::uint8_t value, bool write, CycleKind kind,
               AccessSource source, std::uint8_t channel = 0, bool table = false,
               bool pastTableEnd = false) {
    const std::optional<std::uint16_t> landed = portLanding_;
    portLanding_.reset();
    if (observer_ == nullptr) return;
    BusAccess access;
    access.address = address;
    access.value = value;
    access.write = write;
    access.kind = kind;
    access.source = source;
    access.channel = channel;
    access.table = table;
    access.pastTableEnd = pastTableEnd;
    access.landed = landed;
    observer_->access(access);
  }

  // A transfer engine's two sides of one byte, reported as its accesses: the
  // read on one bus and the write on the other, in that order, each naming the
  // channel it served. A read of an HDMA table says so with `table`, and one past
  // the table's end with `pastTableEnd` as well. `cycle` is what the access
  // watch is told: the byte's position in the channel's transfer pattern, or for
  // a table read 0 for a count or a pointer's low byte and 1 for its high byte.
  std::uint8_t engineRead(std::uint32_t address, bool aBus, AccessSource source,
                          std::uint8_t channel, std::uint8_t cycle, bool table = false,
                          bool pastTableEnd = false);
  void engineWrite(std::uint32_t address, std::uint8_t value, bool aBus, AccessSource source,
                   std::uint8_t channel, std::uint8_t cycle);
  // One byte of a transfer between A-bus address `aAddr` and B-bus address `bAddr`,
  // into the A bus when `toA`; `unit` is the byte's position in the transfer
  // pattern, which both of its accesses carry. Work RAM named on both sides — a
  // work-RAM address on the A bus and $2180-$2183 on the B bus — is not copied:
  // the port's side is open bus, its address does not step, and the port reports
  // no access of its own.
  void engineByte(std::uint32_t aAddr, std::uint32_t bAddr, bool toA, AccessSource source,
                  std::uint8_t channel, std::uint8_t unit, bool table);

  // One master-cycle group: the CPU makes its single access (which prices the
  // cycle), the master counter advances by that cost, and the APU is paced forward
  // by the master cycles it now owes.
  void machineCycle();

  // The master cycles the refresh holds the CPU off the bus, spent a fast cycle at a
  // time, and what every cycle ends with: the timer's crossing settled, the interrupt
  // lines driven, the master counter advanced and the audio machine paced.
  void refreshCycle();
  void closeCycle();

  // Advances the machine's own events by `cost` master cycles: the beam and every
  // event its line carries, the H/V timer's trigger points, the arithmetic unit, and
  // the auto-joypad window. It runs before the cycle's memory access resolves, so a
  // register read sees the event it shares the cycle with. Called exactly once per
  // cycle.
  void tickVideo(std::uint32_t cost);

  // Resolves the picture's pixels for the visible dots the master-cycle span
  // (`from`, `to`] of a line beginning at `lineStart` passed, each from the
  // registers and memories as they stand at its own dot. With no frame observer
  // set it draws nothing and decides, at each of those dots, only what the chip
  // carries to the next position.
  void drawSpan(std::uint64_t lineStart, std::uint64_t from, std::uint64_t to);
  // Turns the frame in progress 512 wide at its first position drawn in
  // half-pixels, which is position `x` of picture row `line`: every pixel drawn
  // before it is doubled into both halves of its position.
  void widenFrame(std::size_t line, std::uint16_t x) noexcept;
  // A write to INIDISP outside vertical blank: the PPU reads the new value one dot
  // early, off the data bus before the CPU has driven it, so the last dot the
  // write's own cycle covered is drawn under the byte the bus held before the
  // write — the whole byte, forced blank and brightness both. Redraws that one dot
  // under `busBefore` after the cycle has drawn it under the old register, then the
  // written value stands for every dot after. A no-op for any other write.
  void redrawInidispEarly(std::uint32_t address, std::uint8_t busBefore);

  // Walks the PPU's Range pass as far as master cycle `to` of a line beginning at
  // `lineStart` reaches — two dots a sprite from the picture's first — finding the
  // sprites the next line crosses. How far the pass has already walked is the
  // chip's own, so the span needs only its end. Unlike the picture, this runs
  // whether or not anyone is watching: a program can read what the pass found
  // through $213E.
  void rangeSpan(std::uint64_t lineStart, std::uint64_t to) noexcept;

  // Hands the finished picture to the frame observer: the rows the frame's own
  // vertical blank left below it, the frame's parity, and the raster the dots
  // wrote. Called as the beam reaches the next frame's first line.
  void deliverFrame();
  // Hands the save window to the observer. Called from between cycles rather than
  // from the line that finished the frame, because that line cannot throw and what
  // a host does with a save can.
  void deliverSave();

  // The events inside one line, for the master-cycle span (`from`, `to`] of a line
  // that began at `lineStart`: the frame's parity, vertical blank's NMI flag and the
  // sprite table's reload, the H/V timer's points, and the refresh.
  void crossLine(std::uint64_t lineStart, std::uint64_t from, std::uint64_t to);

  // Whether the crossing this cycle's tick noted is the one the mode now selects, and
  // the raising of the timer flag when it is, which is part of closing a cycle. A read
  // of $4211 asks the first on its own, to learn that the flag rises in its cycle.
  [[nodiscard]] bool timerCrossed() const noexcept;
  void settleTimer() noexcept;

  // The master-cycle length of the current scanline and the number of lines in the
  // current frame. One line a frame is four cycles short or four long, and an
  // interlaced frame of even parity carries one line more.
  [[nodiscard]] std::uint16_t lineLength() const noexcept;
  [[nodiscard]] std::uint16_t frameLines() const noexcept;

  // The beam's dot, by a map whose dots 323 and 327 are six master cycles wide, and
  // whether the horizontal-blank flag stands. Both are what the PPU is told.
  [[nodiscard]] std::uint16_t hdot() const noexcept;
  // The dot for an arbitrary master offset into the current line, by that same map —
  // hdot() is this at the beam's own position. It reads the current line's length,
  // so it answers for the line the caller is on.
  [[nodiscard]] std::uint16_t hdotAt(std::uint16_t hpos) const noexcept;
  [[nodiscard]] bool inHblank() const noexcept;

  // The master cycle the refresh pauses on for a line beginning at `lineStart`: the
  // point on the previous pause's eight-cycle grid nearest the middle of that line.
  [[nodiscard]] std::uint64_t nextRefresh(std::uint64_t lineStart) const noexcept;

  // Commits the arithmetic result when its cycle countdown expires.
  void commitMath() noexcept;

  // Reloads the live cores from state_ after a construct or restore: the CPU from
  // its register set, the audio machine from the state it runs in.
  void load();
  // Copies the live CPU's register set into state_ before a public return. The
  // audio machine's state is state_.apu itself, so nothing else is copied.
  void sync();

  std::uint8_t busRead(std::uint32_t address, CycleKind kind);
  void busWrite(std::uint32_t address, std::uint8_t value, CycleKind kind);
  // The CPU's read on a machine with something armed: the bus, the return
  // standing in for a watched instruction over the fetch that begins it, and
  // the access watch. Kept out of busRead so the access a machine with nothing
  // armed makes stays one test and the bus.
  std::uint8_t readWithHost(std::uint32_t address, CycleKind kind, std::uint8_t cycle);
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
  // The access watch is applied here, told `source`, `kind` and `cycle`, so the
  // one site covers the CPU's and both engines' accesses (the work-RAM port's
  // own access is watched at readWramPort/writeWramPort, which reach work RAM
  // without passing here, and carry the driving access's cycle there).
  std::uint8_t routeRead(std::uint32_t address, AccessSource source, CycleKind kind,
                         std::uint8_t cycle);
  void routeWrite(std::uint32_t address, std::uint8_t value, AccessSource source, CycleKind kind,
                  std::uint8_t cycle);
  // The mapped bus itself, without the watch: which byte an address answers and
  // where a write lands, read through the page table (pages_, below).
  // routeRead/routeWrite wrap these with the watch; `cycle` passes through to
  // the work-RAM port for its own access's report.
  std::uint8_t routeReadRaw(std::uint32_t address, std::uint8_t cycle);
  void routeWriteRaw(std::uint32_t address, std::uint8_t value, std::uint8_t cycle);

  // What each 8 KB page of the bus reaches. The bus is 2048 pages, eight to a
  // bank, and every window it dispatches on is whole pages — work RAM's mirror
  // in a system bank's first page, the register pages, each map's save window,
  // the cartridge above $8000 and whole cartridge banks — so one entry per page
  // says which memory the page's bytes are in and where its first byte lands,
  // and an access finds its byte with one load of the entry and an add. The
  // table is built once from the cartridge functions (`cartridge.h`) at
  // construction, from the map, the image's size and the board, and is fixed
  // with them; the router, peek and poke, the classification and the watch all
  // read it, so one map answers every path. What can change afterwards is not
  // in it: the save's size, which a restore replaces, is read at each access,
  // and the image's bytes, which poke writes in place.
  enum class PageKind : std::uint8_t {
    WorkRam,     // base is the offset into the 128 KB
    System,      // a system bank's $2000-$5FFF: the register windows at their offsets, open bus between them
    SaveWindow,  // base is the offset into the save before it is reduced to the save's size; with no save the page is its fallback
    Rom,         // base is the image offset of the page's first byte, and the page is linear from it
    RomSlow,     // the image, when its size is not a multiple of 8 KB: a chip smaller than a page can repeat inside one, so the byte is found through romOffset at each access
    RomEmpty,    // the image, when there is none: reads zero and classifies to offset 0
    OpenBus,     // nothing decodes the page: a read answers the data bus's last byte, a write changes nothing
  };
  struct Page {
    PageKind kind;
    PageKind fallback;           // a save window on a cartridge with no save: what the page is then (Rom, RomSlow, RomEmpty or OpenBus)
    std::uint32_t base;          // where the page's first byte lands in its space, as kind says
    std::uint32_t fallbackBase;  // the fallback's base, for Rom
  };
  static constexpr std::size_t kPageBytes = 8192u;
  static constexpr std::size_t kPages = 2048u;
  // Fills pages_ from the cartridge functions, one page at a time: what the
  // board answers for the page's first address — its region, its save offset,
  // its image offset — and, for a save window, what the board answers there
  // with no save.
  void buildPages();
  // The image page beginning at a ROM address on `board`: linear when the image
  // is a multiple of 8 KB, found through romOffset at each access when it is
  // not, and empty when there is no image.
  [[nodiscard]] Page imagePage(const CartridgeBoard& board, std::uint32_t address) const noexcept;

  // The page an address reads once the save has answered: the page itself, or,
  // in a save window on a cartridge with no save, what the board reads there.
  // Under LoROM that is what the bank's upper half reads, so the address
  // carried is the upper half's; under HiROM and ExHiROM it is open bus, and
  // the address is not asked for.
  struct Resolved {
    PageKind kind;
    std::uint32_t base;
    std::uint32_t address;
  };
  [[nodiscard]] Resolved resolve(std::uint32_t address) const noexcept {
    const Page& page = pages_[(address >> 13) & (kPages - 1u)];
    if (page.kind != PageKind::SaveWindow || !state_.sram.empty()) {
      return Resolved{page.kind, page.base, address};
    }
    return Resolved{page.fallback, page.fallbackBase, address | 0x8000u};
  }

  // Applies the access watch to one access, when the watcher is set and the
  // byte the address reaches is armed. watchRead answers the byte to deliver;
  // watchWrite answers the byte to store, or nothing when a write is vetoed.
  // With nothing armed each is a single test of the table pointer; with
  // something armed, one load of the page's pointer in the armed set, null
  // unless a byte the page reaches is armed, then the bit at the address's
  // offset in the run; a page that points at kSlowRun is classified instead.
  [[nodiscard]] std::uint8_t watchRead(std::uint32_t address, std::uint8_t value,
                                       AccessSource source, CycleKind kind, std::uint8_t cycle);
  [[nodiscard]] std::optional<std::uint8_t> watchWrite(std::uint32_t address, std::uint8_t value,
                                                       AccessSource source, CycleKind kind,
                                                       std::uint8_t cycle);
  // physical(), answered only when the byte's space is in `spaces` (one bit per
  // Space) and nothing otherwise. With every bit set it is physical().
  [[nodiscard]] std::optional<Physical> classify(std::uint32_t address,
                                                 std::uint8_t spaces) const noexcept;
  // The armed table's slot for the chunk holding the byte `place`, or nothing
  // for a byte past its space's size — which a save enlarged by a restore after
  // the first arm can reach.
  [[nodiscard]] std::optional<std::size_t> armedSlot(Physical place) const noexcept;
  // Sets or clears the armed bit for the byte `place`, in either direction,
  // allocating its chunk on its first arm and freeing it when its last bit
  // clears.
  void setArmed(Physical place, bool onRead, bool onWrite, bool arm);
  // The layout both armed tables share: each Space's first slot and how many
  // slots it has, one per 64 K-byte chunk of the space, laid out space after
  // space — work RAM's two, the register file's one, the save's as it is sized
  // at this moment, the image's one per 64 KB, and one per bank of open bus.
  // Answers the total.
  [[nodiscard]] std::uint32_t chunkLayout(std::array<std::uint32_t, 5>& base,
                                          std::array<std::uint32_t, 5>& chunks) const noexcept;

  // The instruction watch at a boundary the CPU takes as one: running, between
  // instructions, no hardware request due. When the byte the program counter
  // reaches is armed, the watcher is told with the register file live, and what
  // stands there once the call returns is queued for the fetch that follows.
  // Called once per CPU cycle while an instruction is armed.
  void tellInstruction();
  // What stands at the byte `address` reaches: 0 for a byte not armed, else
  // 1 + the Standin.
  [[nodiscard]] std::uint8_t standinAt(std::uint32_t address) const noexcept;

  // The call both forms share, from `file` — the register file the routine
  // starts under, at a boundary, its stack pointer the stack to push on: the
  // landing pushed, the core pointed at `entry`, and the machine run to the
  // boundary where the routine has returned or the guard has tripped. Answers
  // whether it returned; false with nothing done when a push would not land.
  // The core is left where the loop ended.
  bool runCall(std::uint32_t entry, Cpu65816State file, Standin returns, std::size_t guard);

  // The general-purpose DMA engine ($420B): trigger, and one machine cycle of a
  // running transfer (an overhead cycle or a single byte, priced at eight master
  // cycles). The engine holds the bus between the CPU's instructions.
  void triggerDma(std::uint8_t channels);
  void dmaCycle();
  // The A-bus side of a DMA byte: a read of a memory-mapped region returns open
  // bus and a write to one is inert, the way the console forbids DMA there. A
  // reachable address routes through routeRead/routeWrite, carrying `source`
  // and `cycle`.
  std::uint8_t dmaReadA(std::uint32_t address, AccessSource source, std::uint8_t cycle);
  void dmaWriteA(std::uint32_t address, std::uint8_t value, AccessSource source,
                 std::uint8_t cycle);
  [[nodiscard]] static bool aBusExcluded(std::uint32_t address) noexcept;
  // Whether an A-bus address is work RAM: banks $7E-$7F, or the first $2000 of a
  // system bank.
  [[nodiscard]] static bool aBusIsWorkRam(std::uint32_t address) noexcept;
  // The resume-rounding pad added to the first CPU cycle after a transfer, so the
  // machine resumes on a whole CPU-clock boundary since the pause.
  [[nodiscard]] std::uint32_t resumePad(std::uint32_t cpuCycle) const noexcept;

  // The HDMA engine: one whole event — the start-of-frame initialisation, or a
  // single visible scanline's delivery for every active channel — and the $420C
  // write, which takes a channel out of the picture's remaining lines or brings one
  // into them.
  void hdmaCycle();
  void enableHdma(std::uint8_t channels);
  // The two reads that load a table entry into a channel: the line count, and an
  // indirect entry's pointer — both of its bytes, or with `highByteOnly` a single byte
  // into the high half over a low half of $00. `pastTableEnd` is what the pointer's
  // reads report: set when the count before them ended the table.
  void hdmaLoadCount(std::uint8_t index);
  void hdmaLoadPointer(std::uint8_t index, bool highByteOnly, bool pastTableEnd);
  // Ends the running DMA's current channel when an HDMA event involves it: `channels`
  // is the event's set. The channel's $420B bit clears and its count stands.
  void endDmaOnChannels(std::uint8_t channels) noexcept;

  // The DMA channel registers ($4300-$437F): the eight channels' sixteen-byte
  // register files, read and written by their documented layout.
  std::uint8_t readDmaReg(std::uint16_t offset);
  void writeDmaReg(std::uint16_t offset, std::uint8_t value);

  // The master-cycle cost of reaching `address`, by the documented region map. The
  // second waitstate region ($80-$BF:$8000-$FFFF and $C0-$FF) follows MEMSEL.
  [[nodiscard]] std::uint32_t accessCost(std::uint32_t address) const noexcept;

  // The work-RAM data port: $2180 reads or writes work RAM at the port address and
  // steps it; $2181-$2183 set the address and read back as open bus. `cycle` is
  // the driving access's, which the port's own access is watched with.
  std::uint8_t readWramPort(std::uint16_t offset, std::uint8_t cycle);
  void writeWramPort(std::uint16_t offset, std::uint8_t value, std::uint8_t cycle);

  // The PPU's input pins as they stand: where the beam is, the frame parity, the
  // two blank signals as $4212 reports them, the clock rate, and the level of the
  // counter-latch line. Built for every access to $2100-$213F; the PPU itself is
  // Ppu (`ppu.h`) over `state_.ppu`, and a write to a data port answers where the
  // port put the byte, recorded in `portLanding_` for the access's report.
  [[nodiscard]] PpuInputs ppuInputs() const noexcept;

  // The CPU-side registers ($4200-$421F): interrupt enables and flags, the H/V timer
  // settings, the multiply/divide unit, the I/O port, and the auto-joypad read.
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

  // Takes the beam to the line beginning at `lineStart`, wrapping the frame, and runs
  // the events that line's start carries: the frame's own — the overflow flags and
  // vertical blank's end — or the decision whether vertical blank begins here, with
  // the HDMA channels that follow from it.
  void advanceLine(std::uint64_t lineStart) noexcept;
  // The auto-read: where on vertical blank's first line, beginning at `lineStart`,
  // the read begins; whether it is busy; and the bits it has clocked into
  // $4218-$421F by master cycle `now`, one shift of every port's register per bit.
  [[nodiscard]] std::uint64_t autoJoypadStart(std::uint64_t lineStart) const noexcept;
  [[nodiscard]] bool autoJoypadBusy() const noexcept;
  void clockAutoJoypad(std::uint64_t now) noexcept;

  // Records `value` as the data bus's last byte and returns it, so an unmapped read
  // that follows sees it.
  std::uint8_t latch(std::uint8_t value) noexcept {
    state_.mdr = value;
    return value;
  }

  // The word at $00FFFC, where the CPU starts, read from the cartridge as the
  // bus reads it.
  [[nodiscard]] std::uint16_t resetVector() const noexcept;

  // The CPU's power-on state: emulation mode, the interrupt disable set, and the
  // program counter at the cartridge's reset vector.
  [[nodiscard]] Cpu65816State powerOnCpu() const;

  Cpu65816 cpu_;                     // the live CPU while the machine runs
  SnesState state_;                  // the machine's state: work RAM, registers, counters and the audio machine's whole state are authoritative here
  Apu apu_;                          // the live audio machine, paced by the interleave, running in state_.apu (declared after it: the storage exists before the machine built over it)
  std::vector<std::uint8_t> rom_;    // the cartridge image, fixed for the machine's life
  Region region_ = Region::Ntsc;     // the clock rate, fixed for the machine's life
  CartridgeBoard board_;             // the board the image is on: its map, its chip and its save, fixed with it
  std::array<Page, kPages> pages_{}; // what each page of the bus reaches, built from the two above and fixed with them
  bool bootsAudio_ = false;          // the audio CPU runs a boot image when it starts, fixed with them
  std::uint32_t apuNum_ = 5632u;     // the APU-to-master cycle ratio for this region (numerator)
  std::uint32_t apuDen_ = 118125u;   // and its denominator
  std::uint32_t lastCost_ = 6;       // the master cost of the cycle in progress
  bool videoAdvanced_ = false;       // whether this cycle's access already ticked the machine's events
  // The H/V timer's points the cycle in progress crossed, which the mode as the cycle
  // ends decides between. They belong to the cycle, not to the machine, so a snapshot
  // does not carry them.
  bool timerHPoint_ = false;         // the H point, on whatever line
  bool timerHPointOnVLine_ = false;  // the H point, on the line VTIME names
  bool timerZeroOnVLine_ = false;    // the H = 0 point, on that line
  BusObserver* observer_ = nullptr;  // told every access and internal cycle; none by default
  std::optional<std::uint16_t> portLanding_;  // where the access in progress landed through a video data port, until it is reported
  FrameObserver* frameObserver_ = nullptr;  // told every finished frame; none by default
  // The picture in progress, four bytes a pixel, as wide as a line drawn in
  // half-pixels and as tall as the taller picture — enough for any frame. It is
  // output rather than state, so it lives on the machine and not in its state
  // value, and it exists only while someone is watching. A frame's rows are 256
  // pixels until its first position drawn in half-pixels and 512 from then on; the
  // frame's own width and height say how much of the buffer it is.
  std::vector<std::uint8_t> raster_;
  // What the picture path works out from the chip's registers and reads at every
  // dot. Like the picture it is worked out from the state rather than part of it,
  // so it lives on the machine and not in its state value; a restore drops all of
  // it, and the chip drops whatever a write makes stale.
  Ppu::Derived derived_;
  bool frameFinished_ = false;  // the beam reached a new frame's first line this cycle
  bool frameWide_ = false;      // the frame in progress is 512 wide
  std::uint16_t framePictureLines_ = 0;  // the lines the finished picture holds
  std::uint16_t frameWidth_ = 0;         // and how wide it is
  std::uint8_t frameField_ = 0;          // the parity that picture ran under
  SaveObserver* saveObserver_ = nullptr;  // told a frame changed the save window; none by default
  // Whether anything has stored into the save window since it was last reported.
  // One flag rather than a comparison: every store into the window goes through
  // one site, so the machine already knows. It belongs to the report and not to
  // the console, so a snapshot does not carry it.
  bool saveChanged_ = false;
  bool saveFinished_ = false;  // a frame that changed the window ended this cycle
  // The bytes a host has armed for a watch, one bit per byte per direction,
  // keyed by the byte an address reaches (physical) and held behind one owning
  // pointer null until the first arm — which is also the access path's "is
  // anything armed" test. Each direction is one slot per 64 K-byte chunk of each
  // space, laid out space after space: work RAM's two, the register file's one,
  // the save's, the image's one per 64 KB, and one per bank of open bus. A
  // chunk's 8 KB of bits is allocated only when a byte in it is armed and freed
  // when its last one is disarmed, so an unused machine carries one null pointer
  // and allocates nothing, and arming a byte allocates one chunk however many
  // addresses reach it.
  //
  // An access finds its bit through the page it drives: the set holds, per page
  // of the bus and per direction, a pointer to the 1 KB of bits for the 8 KB run
  // of the space that page reaches, null when no byte of that run is armed, so
  // every alias of a run shares one pointer and an access to a page holding
  // nothing armed pays one load and one test. A page whose bytes the page
  // table does not reach linearly — an image found through romOffset, a save
  // wrapped or reduced unevenly inside the page, a system page with an open-bus
  // byte armed between its registers — points at kSlowRun instead, and each
  // access there is classified and looked up in its chunk. The pointers are
  // rebuilt from the chunks after every arm, every disarm and every restore.
  struct ArmedChunk {
    std::array<std::uint8_t, 8192> bits{};  // one bit per byte of the chunk
    std::array<std::uint16_t, 8> runs{};    // bits set in each 8 KB run of the chunk
    std::uint32_t armed = 0;                 // bits set here, to free the chunk at zero
    // Whether the byte at `index` within its space is armed here.
    [[nodiscard]] bool has(std::uint32_t index) const noexcept {
      return (bits[(index & 0xFFFFu) >> 3] & (1u << (index & 7u))) != 0u;
    }
  };
  struct AccessArmedSet {
    std::array<std::uint32_t, 5> base{};   // each Space's first slot
    std::array<std::uint32_t, 5> chunks{}; // and how many slots it has
    std::vector<std::unique_ptr<ArmedChunk>> read;
    std::vector<std::unique_ptr<ArmedChunk>> write;
    std::array<std::uint32_t, 5> perSpace{};  // set bits in each space, both directions
    std::uint8_t spaces = 0;                  // one bit per Space with anything armed
    std::size_t armed = 0;  // set (byte, direction) bits, to free the set at zero
    std::array<const std::uint8_t*, kPages> pageRead{};   // per page: the run's bits, kSlowRun, or null
    std::array<const std::uint8_t*, kPages> pageWrite{};
  };
  static constexpr std::uint8_t kSlowRun = 0u;  // the page pointer that says: classify this access
  AccessWatcher* accessWatcher_ = nullptr;  // told every armed access; none by default
  std::unique_ptr<AccessArmedSet> armed_;   // the armed table; null until the first arm
  // The bytes a host has armed for an instruction watch and what stands at each,
  // two bits per byte — 0 not armed, else 1 + the Standin — keyed by the byte an
  // address reaches (physical) in the same chunk layout as the access table,
  // and held behind one owning pointer null until the first arm, which is also
  // the cycle's "is anything armed" test. A chunk's 16 KB is allocated only when
  // a byte in it is armed and freed when its last one is disarmed. An
  // instruction finds its code as an access finds its bit: through a per-page
  // pointer to the 2 KB of codes for the run the page reaches, null when none
  // of the run is armed and kSlowRun where the page needs classifying. The set
  // also carries the return queued for the fetch that begins the instruction
  // the watcher was last told about: the opcode to answer and the address it is
  // for, cleared by the next opcode fetch whichever address that fetches.
  struct StandinChunk {
    std::array<std::uint8_t, 16384> codes{};  // two bits per byte of the chunk
    std::array<std::uint16_t, 8> runs{};      // bytes armed in each 8 KB run of the chunk
    std::uint32_t armed = 0;                   // bytes armed here, to free the chunk at zero
    // The code for the byte at `index` within its space.
    [[nodiscard]] std::uint8_t code(std::uint32_t index) const noexcept {
      return static_cast<std::uint8_t>((codes[(index & 0xFFFFu) >> 2] >> ((index & 3u) * 2u)) & 3u);
    }
    void set(std::uint32_t index, std::uint8_t code) noexcept {
      const std::uint32_t at = (index & 0xFFFFu) >> 2;
      const unsigned shift = (index & 3u) * 2u;
      codes[at] = static_cast<std::uint8_t>((codes[at] & ~(3u << shift)) | (code << shift));
    }
  };
  struct StandinSet {
    std::array<std::uint32_t, 5> base{};   // each Space's first slot
    std::array<std::uint32_t, 5> chunks{}; // and how many slots it has
    std::vector<std::unique_ptr<StandinChunk>> slots;
    std::array<std::uint32_t, 5> perSpace{};  // bytes armed in each space
    std::uint8_t spaces = 0;                  // one bit per Space with anything armed
    std::size_t armed = 0;                    // bytes armed, to free the set at zero
    std::uint8_t pendingOpcode = 0;           // the return the next opcode fetch answers, or 0
    std::uint32_t pendingAddress = 0;         // the address that fetch must drive
    std::array<const std::uint8_t*, kPages> page{};  // per page: the run's codes, kSlowRun, or null
  };
  InstructionWatcher* instructionWatcher_ = nullptr;  // told each armed instruction reached; none by default
  std::unique_ptr<StandinSet> standins_;              // the armed instructions; null until the first arm

  // What a page reaches, for the watch: one 8 KB run of one space — the space
  // and the index of the run's first byte — no byte at all, or bytes the page
  // does not reach linearly, which the classification settles per access. A
  // save window is read as the save now stands, so a restore can change the
  // answer. For Slow, `run.space` names the space whose armed bytes matter.
  struct PageRun {
    Space space;
    std::uint32_t start;
  };
  enum class PageReach : std::uint8_t { None, Run, Slow };
  [[nodiscard]] PageReach pageRun(std::size_t page, PageRun& run) const noexcept;
  // Rebuilds every per-page pointer of the armed set and the stand-in set from
  // their chunks and the page table.
  void refreshWatchPages();
  // Whether the byte `address` reaches is armed in `table`, by classification:
  // the lookup for a page that points at kSlowRun.
  [[nodiscard]] bool armedThrough(std::uint32_t address,
                                  const std::vector<std::unique_ptr<ArmedChunk>>& table) const noexcept;
};

}  // namespace snaggletooth
