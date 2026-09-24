#pragma once

// The APU machine — the SPC700 wrapped in its 64KB RAM and the $F0-$FF register
// overlay.
//
// The machine owns the CPU, the RAM, and the hardware registers the CPU reaches
// through the top sixteen bytes of zero page. Its internal bus satisfies the
// ApuBus concept: reads and writes in $00F0-$00FF hit the register overlay; an
// SPC700 read in $FFC0-$FFFF returns the mapped boot-ROM image when one is set and
// CONTROL bit 7 is on (see mapIplRom); everything else is plain RAM. The host
// drives the comm ports from the other side and loads programs directly into RAM.
// The machine holds no console boot ROM — it comes up in the seeded post-IPL ready
// state, and any image a host maps over the window is an original program written
// to the documented upload protocol.
//
// The machine runs one cycle at a time. Within a cycle the clocked events go
// first — the master counter advances, the stage-1 timer ticks land, the DSP
// takes its slot — and the CPU's single bus access lands last. So a read of a
// timer output on the cycle that timer ticks returns the incremented count, and
// a DSP write lands on its true cycle relative to the 32-cycle sample boundary.
//
// One counter drives all of it: the SPC700 and the DSP share a clock, so the
// timer phase and the sample phase are the same free-running phase. T0 and T1
// tick every 128 cycles and T2 every 16, on the counter's documented slots; the
// DSP takes a sample every 32. Each timer's stage-2 counter increments on its
// stage-1 ticks while enabled and, on reaching its target, advances a 4-bit
// stage-3 counter that an overlay read of TnOUT returns and clears.
//
// step() runs one instruction; run() runs an exact number of cycles and may
// stop mid-instruction, which is a legal resting place — instruction progress
// is part of the state value.
//
// A host that wants to know what the sound CPU did, not only what the machine
// holds afterwards, sets an observer (ApuObserver): it is told every access the
// CPU makes as its value settles, and every instruction boundary the machine
// crosses with the state on either side and the cycles between.

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "snaggletooth/apu/dsp.h"
#include "snaggletooth/apu/spc700.h"

namespace snaggletooth {

// The audio unit's boot-ROM window: 64 bytes at $FFC0-$FFFF. When CONTROL bit 7 is
// set and an image is mapped (Apu::mapIplRom), an SPC700 read in this range returns
// the image byte; the RAM beneath stays writable and reappears when the bit clears.
inline constexpr std::uint16_t kIplWindowBase = 0xFFC0u;
inline constexpr std::size_t kIplWindowBytes = 64u;

// One timer's mutable stage counters. The enable bit lives in the CONTROL
// register (ApuState::control); the stage-1 ticks come off the machine's master
// counter, ApuState::divider.
struct TimerState {
  std::uint8_t stage2 = 0;  // 0-255 counter compared against the target
  std::uint8_t stage3 = 0;  // 4-bit output counter; an overlay read of TnOUT clears it
  std::uint8_t target = 0;  // TnTARGET; a target of 0 divides by 256

  [[nodiscard]] bool operator==(const TimerState&) const noexcept = default;
};

// The whole machine as a value: snapshot by copy, restore by assignment.
struct ApuState {
  Spc700State cpu{};
  std::array<std::uint8_t, 65536> ram{};
  std::uint8_t test = 0;
  std::uint8_t control = 0;
  std::uint8_t dspAddr = 0;
  DspState dsp{};
  std::array<std::uint8_t, 4> inputPorts{};   // host -> SPC700 (the CPU reads these at $F4-$F7)
  std::array<std::uint8_t, 4> outputPorts{};  // SPC700 -> host (the CPU writes these at $F4-$F7)
  // The auxiliary port bytes the CPU reads at $F8/$F9 (the S-SMP's unconnected
  // P4/P5 pins). Storage of their own, not a view of RAM: a CPU write lands here
  // as well as in the RAM beneath, a CPU read returns this value, and the S-DSP's
  // echo buffer writes — which reach only the RAM — never alter it. The
  // unconnected pins read back as written; power-on leaves them high ($FF).
  std::array<std::uint8_t, 2> auxPorts{0xFF, 0xFF};
  std::uint16_t divider = 0;                  // the master cycle counter: timer ticks and sample boundaries
  std::array<TimerState, 3> timers{};

  [[nodiscard]] bool operator==(const ApuState&) const noexcept = default;
};

// What the audio machine tells about the sound CPU: every access it makes and
// every instruction boundary it crosses. A host derives the CPU's whole
// observable behaviour from it — which addresses were read and written in what
// order with what values, and how many cycles each instruction took. The DSP's
// own reads of RAM, the sample and echo fetches, are not the CPU's and are not
// reported.
class ApuObserver {
 public:
  virtual ~ApuObserver() = default;

  // An access the CPU made through the machine's bus, after its value settled:
  // a read carries what the bus answered — a register's value for an address in
  // the overlay, the boot-ROM image for a read in the mapped window — and a
  // write what the CPU drove. The instruction's own fetches are reported like
  // any other read.
  virtual void access(std::uint16_t address, std::uint8_t value, bool write) = 0;

  // An instruction boundary the machine crossed: the CPU's state at the
  // boundary before, its state at this one, and the cycles between — the whole
  // cost of the instruction that ran, however many run() calls it spanned. A
  // halted core crosses a boundary every cycle, reported with no access between
  // and the same halted state on both sides.
  virtual void instruction(const Spc700State& before, const Spc700State& after,
                           std::uint32_t cycles) = 0;
};

// What a host answers a watched access with, on either machine: let it happen,
// prevent it, or put another byte in its place. A read cannot be prevented —
// nothing stops a CPU receiving a byte — so a vetoed read delivers the byte the
// access would have answered; only a substitute changes what a read delivers.
// A vetoed write stores nothing.
class AccessAnswer {
 public:
  // The access happens as it would: a read delivers its own byte, a write
  // stores the value the source drove.
  [[nodiscard]] static AccessAnswer proceed() noexcept { return {Kind::Proceed, 0}; }
  // A write stores nothing; a read still delivers its own byte.
  [[nodiscard]] static AccessAnswer veto() noexcept { return {Kind::Veto, 0}; }
  // `byte` takes the access's place: a read delivers it, a write stores it.
  [[nodiscard]] static AccessAnswer instead(std::uint8_t byte) noexcept {
    return {Kind::Instead, byte};
  }

  // The byte a read delivers, given the byte the access would have answered.
  [[nodiscard]] std::uint8_t applyToRead(std::uint8_t answered) const noexcept {
    return kind_ == Kind::Instead ? byte_ : answered;
  }
  // Whether a write stores at all — false only for a veto.
  [[nodiscard]] bool storesWrite() const noexcept { return kind_ != Kind::Veto; }
  // The byte a stored write lands, given the value the source drove.
  [[nodiscard]] std::uint8_t applyToWrite(std::uint8_t driven) const noexcept {
    return kind_ == Kind::Instead ? byte_ : driven;
  }

 private:
  enum class Kind : std::uint8_t { Proceed, Veto, Instead };
  constexpr AccessAnswer(Kind kind, std::uint8_t byte) noexcept : kind_(kind), byte_(byte) {}
  Kind kind_;
  std::uint8_t byte_;
};

// A host told, before a watched access takes effect, that the sound CPU is
// reading or writing a place it armed (Apu::watchAccess), and answering what
// happens. The sound CPU is the only thing on this bus, so the watcher is told
// a 16-bit address and no source; the DSP's own sample and echo fetches are not
// the CPU's and are not watched, the same reads ApuObserver leaves out. `cycle`
// is which cycle of the instruction the access is, counted as the chip spends
// them: 0 is the opcode fetch, an operand fetch follows at 1, and every cycle
// counts whether or not it reaches the bus, so a read of the destination that a
// write instruction makes before its write sits one cycle before it. The bus
// carries no kind, so an opcode fetch and a data read of one address are told
// apart by the cycle alone. A register read is told after the register's own
// effect — a timer output has already cleared when its read is told — and a
// veto cannot undo it.
//
// A mechanism beside ApuObserver, which still reports every settled access and
// cannot answer. Set by pointer: the host's object, not part of the state, so a
// snapshot does not carry it and restore() leaves it in place.
class ApuAccessWatcher {
 public:
  virtual ~ApuAccessWatcher() = default;
  // The CPU is about to read `address`, where the machine would answer `value`.
  virtual AccessAnswer read(std::uint16_t address, std::uint8_t value, std::uint8_t cycle) = 0;
  // The CPU is about to write `value` to `address`.
  virtual AccessAnswer write(std::uint16_t address, std::uint8_t value, std::uint8_t cycle) = 0;
};

// What stands at a watched address while it is armed (Apu::watchInstruction):
// nothing, so the instruction there runs once the host has been told; or a
// return (RET), so the routine there never runs — the fetch that begins the
// instruction answers the return's opcode in place of the byte RAM holds, and
// the one instruction that runs is the return.
enum class ApuStandin : std::uint8_t {
  None,    // the instruction runs after the host is told
  Return,  // a return (RET), for a routine a CALL entered
};

// A host told, before the instruction at a watched address runs, that the
// sound CPU has reached it. Told once per instruction, at an instruction
// boundary of a running core: a sleeping or stopped core sits on a boundary
// and is not told. This bus has no aliases, so a watched place is the address.
//
// The machine's clock has not moved for this call: the host runs on its own
// time, and the cycles the instruction — or the return standing in for it —
// spends are the same as with no host at all. cpuState() is live inside the
// call, and a register file written with setCpuState() inside it is what the
// instruction runs under. The watcher is the host's object, set by pointer and
// not part of the state, so a snapshot does not carry it and restore() leaves
// it in place.
class ApuInstructionWatcher {
 public:
  virtual ~ApuInstructionWatcher() = default;
  // The instruction at `address` is about to run.
  virtual void reached(std::uint16_t address) = 0;
};

class Apu {
 public:
  // The seeded post-IPL power-on machine: zeroed RAM, SP at $01EF, TEST $0A,
  // CONTROL $B0, the $AA/$BB ready bytes posted to output ports 0 and 1, and the
  // timers at their power-on values (TnTARGET $00, TnOUT $F). No image is mapped
  // over the $FFC0 window; a host loads a program into RAM and points the CPU at it
  // with setPc, or maps a boot-ROM image with mapIplRom to run the upload protocol.
  Apu();
  explicit Apu(ApuState state);

  // The seeded power-on machine over state that lives elsewhere: `storage` is the
  // machine's whole state for the machine's life, seeded here exactly as Apu()
  // seeds its own, then read and written in place by every cycle. state() answers
  // that object and restore() and reset() write into it. The caller keeps it alive
  // for as long as the machine runs. A caller that wants a different starting
  // state assigns it into the storage and calls reload().
  explicit Apu(ApuState* storage);

  // A machine is moved, never copied: a copy would either run two machines over
  // one storage or split an owning machine from its snapshot. A moved machine
  // keeps its storage — its own, or the caller's object it was built over.
  Apu(const Apu&) = delete;
  Apu& operator=(const Apu&) = delete;
  Apu(Apu&&) noexcept = default;
  Apu& operator=(Apu&&) noexcept = default;

  // The machine `moved` carrying on in `storage`, which already holds its state:
  // the owner of the storage the machine was built over has moved that object to
  // a new place and moves the machine after it. The live core, the pending output,
  // the mapped image, the observer and the boundary record all come across; the
  // state is read from `storage` from here on and `moved` is left with nothing.
  Apu(Apu&& moved, ApuState* storage) noexcept;

  // The whole machine as a value. state() is coherent at any cycle the machine
  // has stopped on, mid-instruction included; restore() replaces every field and
  // resumes exactly there.
  [[nodiscard]] const ApuState& state() const noexcept { return *state_; }
  void restore(ApuState state);

  // Resumes from the state as it now stands after the storage was written from
  // outside — the owner of the storage assigned a snapshot into it. The live core
  // is reloaded from it, the DSP's sample slot re-locked to the master counter, and
  // pending output discarded, as restore() does after its own assignment.
  void reload();

  // Re-seeds the post-IPL state with the documented reset differences: the timer
  // outputs clear to 0 (the power-on value is $F), but the targets and the master
  // counter are retained (a free-running counter cannot be reset), CONTROL and
  // TEST return to their reset values, the ready bytes are re-posted, zero page
  // is cleared, and the rest of RAM is left as it was.
  void reset();

  // The host face of the comm ports (index 0-3). writePort sets an input latch
  // the CPU reads; readPort returns an output latch the CPU wrote. Writing one
  // side never disturbs the other.
  void writePort(std::uint8_t index, std::uint8_t value);
  [[nodiscard]] std::uint8_t readPort(std::uint8_t index) const noexcept;

  // Host RAM access for loading images and inspecting memory. These bypass the
  // overlay — RAM is RAM from the host side, and a read of $00F0-$00FF returns
  // the underlying byte, not the register the CPU would see.
  [[nodiscard]] std::uint8_t readRam(std::uint16_t address) const noexcept {
    return state_->ram[address];
  }
  void writeRam(std::uint16_t address, std::uint8_t value) noexcept {
    state_->ram[address] = value;
  }
  void loadRam(std::uint16_t address, std::span<const std::uint8_t> bytes) noexcept;

  // What a fetch by the sound CPU at `address` returns, without making one: the
  // boot-ROM image while an image is mapped and CONTROL bit 7 is set, the RAM
  // byte otherwise. The sixteen register bytes are answered from the RAM
  // beneath them, as readRam answers them; no program is fetched from the
  // overlay. Nothing changes, so a host can decode the instruction the CPU is
  // about to run.
  [[nodiscard]] std::uint8_t peek(std::uint16_t address) const noexcept;

  // Points the CPU at a loaded image (the machine has no IPL ROM to set an entry
  // for). A convenience over restoring a whole state with a changed PC.
  void setPc(std::uint16_t pc);

  // The observer told every access the CPU makes and every instruction boundary
  // the machine crosses, or none, which is how the machine starts. It is the
  // host's object and outlives every cycle it is set for; it is not part of the
  // state, so a snapshot does not carry it and restore() leaves it in place.
  // With none set an access costs one check and a cycle one more. Set it at an
  // instruction boundary: the state the machine holds when it is set is the
  // `before` of the first boundary reported.
  void setObserver(ApuObserver* observer) noexcept {
    observer_ = observer;
    markBoundary();
  }
  [[nodiscard]] ApuObserver* observer() const noexcept { return observer_; }

  // Maps a 64-byte boot-ROM image over the $FFC0-$FFFF window. While CONTROL bit 7
  // is set, an SPC700 read in that range returns the image; every write still lands
  // in the RAM beneath, and clearing the bit exposes that RAM again. A machine with
  // no image mapped — the default — reads plain RAM there regardless of the bit.
  // The image is fixed configuration, not machine state: restore() and reset() keep
  // it, the way the console keeps its boot ROM. There is no console ROM here; the
  // image is an original program written to the documented upload protocol.
  void mapIplRom(std::span<const std::uint8_t, kIplWindowBytes> image);

  // Runs machine cycles to the end of one CPU instruction and returns how many it
  // took. Called after run() stopped mid-instruction, it finishes the instruction
  // in progress rather than starting one. A halted core runs 2 cycles and returns
  // 2, so the timers and the DSP keep going while the CPU sits idle.
  std::uint32_t step();

  // Runs exactly `budget` machine cycles and returns that count. The stop lands
  // wherever the budget falls, mid-instruction included — run(a) then run(b) is
  // bitwise run(a+b). run(0) runs nothing.
  std::uint64_t run(std::uint64_t budget);

  // Drains the 32 kHz stereo frames the DSP has produced since the last drain,
  // clearing the internal queue. One frame lands per DSP sample — every 32
  // machine cycles — so a caller drains periodically to bound the queue. Frames
  // are output, not machine state: they are not part of a snapshot, and
  // restore() and reset() discard any that are pending.
  [[nodiscard]] std::vector<StereoFrame> takeFrames();

  // The frames produced since the last drain, into the caller's own storage, and
  // how many were written. Frames past the end of `into` stay queued for the
  // next drain, and nothing is allocated — a host draining on a callback that
  // must not allocate uses this form. takeFrames() with no buffer is the drain
  // that allocates a fresh vector.
  [[nodiscard]] std::size_t takeFrames(std::span<StereoFrame> into) noexcept;

  // A host reaching into the machine's RAM by 16-bit address, beside readRam /
  // writeRam and in the console's vocabulary. peek answers the byte the sound
  // CPU would fetch (above); poke writes the RAM beneath the register overlay,
  // and always lands, because the whole 64 KB is RAM. addressable answers
  // whether `bytes` from `address` are RAM this face reaches — every address is,
  // so it answers whether the range fits without running past the end. The
  // sixteen registers at $00F0-$00FF are reached by name below, not here, and a
  // read of $00F0-$00FF through peek returns the RAM beneath, not the register.
  bool poke(std::uint16_t address, std::uint8_t value) noexcept {
    state_->ram[address] = value;
    return true;
  }
  [[nodiscard]] bool addressable(std::uint16_t address, std::size_t bytes) const noexcept {
    return static_cast<std::size_t>(address) + bytes <= 0x10000u;
  }

  // A DSP register, written as a write through DSPDATA writes it — the same
  // acknowledge for ENDX, the same arming for KON, the same cycle stamp a
  // register the chip wrote carries — so a host write and a program write are
  // one thing. An index above $7F is ignored, as DSPDATA ignores it. Writing one
  // spends no cycle and does not advance or re-lock the sample slot.
  // readDspRegister answers the register's stored byte, masking the index with
  // $7F as a DSPDATA read does, without a side effect.
  void writeDspRegister(std::uint8_t index, std::uint8_t value);
  [[nodiscard]] std::uint8_t readDspRegister(std::uint8_t index) const noexcept;

  // One of the sixteen registers the sound CPU reaches at $00F0-$00FF (index
  // 0-15): TEST, CONTROL, the DSP address and data, the four port latches, the
  // two AUXIO bytes and the three timers. Written and read as the sound CPU's
  // own access does, side effects and all — a write lands in the RAM beneath and
  // applies the register's effect, and reading a timer's output clears it, so
  // readOverlayRegister is not const.
  void writeOverlayRegister(std::uint8_t index, std::uint8_t value);
  [[nodiscard]] std::uint8_t readOverlayRegister(std::uint8_t index);

  // The sound CPU's register file, read and written whole on a stopped machine.
  // cpuState() answers the registers as they stand; setCpuState() reloads the
  // live core from `state` and re-locks the DSP's sample slot, so the written
  // set is live for the next cycle. Instruction progress is part of the value: a
  // machine written mid-instruction resumes exactly where the value says.
  [[nodiscard]] const Spc700State& cpuState() const noexcept { return state_->cpu; }
  void setCpuState(const Spc700State& state);

  // The watcher (ApuAccessWatcher, above) told every armed access, or none,
  // which is how the machine starts. With none set, or nothing armed, an access
  // pays one test.
  void setAccessWatcher(ApuAccessWatcher* watcher) noexcept { accessWatcher_ = watcher; }
  [[nodiscard]] ApuAccessWatcher* accessWatcher() const noexcept { return accessWatcher_; }

  // Arms or disarms a watch on `bytes` bytes from `address`, for reads, writes,
  // or both. Arming a place already armed does nothing and disarming one not
  // armed does nothing; the directions are independent. A machine that has never
  // armed a place holds no table, and disarming the last place frees it again.
  void watchAccess(std::uint16_t address, std::size_t bytes, bool onRead, bool onWrite);
  void unwatchAccess(std::uint16_t address, std::size_t bytes, bool onRead, bool onWrite);

  // The watcher (ApuInstructionWatcher, above) told each armed instruction the
  // CPU reaches, or none, which is how the machine starts. With none set, or
  // nothing armed, a cycle pays one test.
  void setInstructionWatcher(ApuInstructionWatcher* watcher) noexcept {
    instructionWatcher_ = watcher;
  }
  [[nodiscard]] ApuInstructionWatcher* instructionWatcher() const noexcept {
    return instructionWatcher_;
  }

  // Arms a watch on the instruction at `address`, with `standin` standing there
  // while it is armed: None tells the watcher and lets the instruction run;
  // Return tells the watcher and answers the fetch with RET (ApuStandin,
  // above), so the routine's body never runs and the caller resumes as it would
  // after the routine returned, the return spending exactly what a real one
  // spends. Arming an armed place replaces what stands there; disarming a place
  // not armed does nothing. A stand-in stands whether or not a watcher is set.
  // RAM is never written: peek and a data read of the byte answer RAM, and
  // disarming has nothing to put back. A machine that has never armed an
  // instruction holds no table at all, and disarming the last one frees it
  // again.
  void watchInstruction(std::uint16_t address, ApuStandin standin = ApuStandin::Return);
  void unwatchInstruction(std::uint16_t address);

  // Runs the routine at `entry` and returns when it returns: true when it did,
  // false when the guard tripped or the call was refused. The routine is the
  // machine running — its cycles are real, the timers tick and the DSP
  // produces its samples through them — and state().divider moves by them;
  // run() afterwards runs its whole budget on top.
  //
  // callInContext runs the routine in the sound program's own context: the
  // registers and the stack pointer as they stand, the landing pushed where a
  // CALL would push it, and the whole register file put back afterwards, so
  // the interrupted program carries on unaware. What the routine changed in
  // RAM and in the registers it wrote stands. A host that wants the routine's
  // registers reads them live inside an instruction watch on the routine's RET
  // (ApuStandin::None), before the file goes back.
  //
  // callOnStack runs it in a frame of the host's own: the register file is
  // whatever the host wrote with setCpuState, the stack pointer starts at
  // `stackTop`, and nothing is put back — cpuState() afterwards is the file the
  // routine left, or where it was abandoned, for the host to read and, if it
  // wants the program's file back, to restore itself.
  //
  // `returns` is Return, the one return the sound CPU has: the landing is the
  // program counter as it stands, pushed as two bytes in page one, and nothing
  // there is executed. The call ends at the first instruction boundary where
  // the stack pointer is back at its value before the push and the program
  // counter is at the landing — both, so a routine that branches through the
  // landing without returning does not end it. The landing's bytes are pushed
  // through poke, spending no cycle.
  //
  // `guard` bounds a routine that never returns, in instructions the routine may
  // run; an idle cycle of a sleeping or stopped core counts as one. On overrun
  // the routine is abandoned at its boundary and the call returns false. Zero
  // runs nothing.
  //
  // The call is refused, returning false with nothing done, when `returns` is
  // ApuStandin::None or when the machine is not between instructions (inside
  // an access watcher's call, or after a run() that stopped mid-instruction).
  // A call made from inside a watcher's call, at any depth, is the same call.
  bool callInContext(std::uint16_t entry, ApuStandin returns, std::size_t guard);
  bool callOnStack(std::uint16_t entry, std::uint8_t stackTop, ApuStandin returns,
                   std::size_t guard);

 private:
  // The internal bus: $00F0-$00FF route to the register overlay, everything else
  // is RAM. Both the CPU and its dummy reads pass through here, and every call
  // is reported to the observer once its value is settled.
  struct Bus {
    Apu& apu;
    std::uint8_t read(std::uint16_t address) {
      const std::uint8_t value = apu.busRead(address);
      if (apu.observer_) apu.observer_->access(address, value, false);
      return value;
    }
    void write(std::uint16_t address, std::uint8_t value) {
      apu.busWrite(address, value);
      if (apu.observer_) apu.observer_->access(address, value, true);
    }
  };

  // Reloads the live CPU from the state and re-locks the DSP sample slot to the
  // master counter after a construct, restore, reload, or reset.
  void syncCpuAndSlot();

  // Takes the live CPU's state as the `before` of the next boundary reported
  // and starts its cycle count over: after every reload of the CPU, and when
  // an observer is set.
  void markBoundary() noexcept {
    boundaryState_ = cpu_.state();
    sinceBoundary_ = 0;
  }

  std::uint8_t busRead(std::uint16_t address);
  void busWrite(std::uint16_t address, std::uint8_t value);
  // The CPU's read on a machine with something armed, given the byte the bus
  // answered: the return standing in for a watched instruction over the fetch
  // that begins it, and the access watch. Kept out of busRead so the access a
  // machine with nothing armed makes stays one test and the bus.
  std::uint8_t readWithHost(std::uint16_t address, std::uint8_t value);
  std::uint8_t readRegister(std::uint8_t reg);
  void writeRegister(std::uint8_t reg, std::uint8_t value);

  // Applies the access watch to one CPU access, when the watcher is set and the
  // place is armed; `cycle` is the CPU's cycle index at the access, which busRead
  // and busWrite take from the live core. watchRead answers the byte to deliver;
  // watchWrite answers the byte to store, or nothing when a write is vetoed.
  // With nothing armed each is a single test of the table pointer.
  [[nodiscard]] std::uint8_t watchRead(std::uint16_t address, std::uint8_t value,
                                       std::uint8_t cycle);
  [[nodiscard]] std::optional<std::uint8_t> watchWrite(std::uint16_t address, std::uint8_t value,
                                                       std::uint8_t cycle);
  // Sets or clears the armed bit for `address`, in either direction.
  void setArmed(std::uint16_t address, bool onRead, bool onWrite, bool arm);

  // The instruction watch at a boundary of a running core. When the program
  // counter's address is armed, the watcher is told with the register file
  // live, and what stands there once the call returns is queued for the fetch
  // that follows. Called once per cycle while an instruction is armed.
  void tellInstruction();

  // The call both forms share, from `file` — the register file the routine
  // starts under, at a boundary, its stack pointer the stack to push on: the
  // landing pushed, the core pointed at `entry`, and the machine run to the
  // boundary where the routine has returned or the guard has tripped. Answers
  // whether it returned. The core is left where the loop ended.
  bool runCall(std::uint16_t entry, Spc700State file, std::size_t guard);

  // One machine cycle. The master counter advances, the timer ticks and the DSP
  // sample boundary that land on the new count are taken, and then the CPU makes
  // its one bus access — the order the chips share their multiplexed bus in, and
  // the reason a read sees the tick that shares its cycle.
  void machineCycle();

  // One stage-1 tick for timer `index`: its stage-2 counter increments while the
  // timer is enabled and, on reaching the target, advances stage 3 and zeroes.
  void tickTimer(std::size_t index);

  // Runs one of the DSP sample's 32 slots this cycle and, on the wrap slot, queues
  // the stereo frame the sample finished. The RAM span is writable so the echo unit
  // can reach its delay line in APU RAM directly.
  void sampleFrame();

  Spc700 cpu_;      // the live CPU state while the machine runs
  // The machine's state: RAM, overlay, timers and the DSP are authoritative in it,
  // and cpu is written back before every return. An owning machine (Apu(),
  // Apu(ApuState)) keeps it in owned_; one built over storage (Apu(ApuState*))
  // leaves owned_ empty and state_ points at the caller's object.
  std::unique_ptr<ApuState> owned_;
  ApuState* state_ = nullptr;
  std::vector<StereoFrame> frames_;  // DSP output awaiting the host's drain; not part of the snapshot
  std::optional<std::array<std::uint8_t, kIplWindowBytes>> iplImage_;  // the $FFC0 window image; config, absent by default, kept across restore()/reset()
  ApuObserver* observer_ = nullptr;  // told every access and every boundary; none by default
  Spc700State boundaryState_{};      // the CPU at the last boundary reported, the `before` of the next
  std::uint32_t sinceBoundary_ = 0;  // cycles run since it
  // The addresses a host has armed for a watch, one bit per 16-bit address per
  // direction, behind one owning pointer held null until the first arm. The
  // sound CPU's space is 64 KB, so each direction is a single 8 KB bitmap; the
  // structure is freed and the pointer restored to null when the last place is
  // disarmed, so an unused machine carries one null pointer and allocates
  // nothing. A lookup is one null test and one bit test.
  struct AccessArmedSet {
    std::array<std::uint8_t, 8192> read{};   // one bit per 16-bit address
    std::array<std::uint8_t, 8192> write{};
    std::size_t armed = 0;  // set (address, direction) bits, to free the set at zero
  };
  ApuAccessWatcher* accessWatcher_ = nullptr;  // told every armed access; none by default
  std::unique_ptr<AccessArmedSet> armed_;      // the armed table; null until the first arm
  // The addresses a host has armed for an instruction watch and what stands at
  // each, two bits per 16-bit address — 0 not armed, else 1 + the ApuStandin —
  // behind one owning pointer held null until the first arm, which is also the
  // cycle's "is anything armed" test; the structure is freed and the pointer
  // restored to null when the last address is disarmed. It also carries the
  // return queued for the fetch that begins the instruction the watcher was
  // last told about: the opcode to answer and the address it is for, cleared by
  // the next opcode fetch whichever address that fetches.
  struct StandinSet {
    std::array<std::uint8_t, 16384> codes{};  // two bits per address
    std::size_t armed = 0;                     // addresses armed, to free the set at zero
    std::uint8_t pendingOpcode = 0;            // the return the next opcode fetch answers, or 0
    std::uint16_t pendingAddress = 0;          // the address that fetch must drive
    [[nodiscard]] std::uint8_t code(std::uint16_t address) const noexcept {
      return static_cast<std::uint8_t>((codes[address >> 2] >> ((address & 3u) * 2u)) & 3u);
    }
    void set(std::uint16_t address, std::uint8_t code) noexcept {
      const unsigned shift = (address & 3u) * 2u;
      codes[address >> 2] =
          static_cast<std::uint8_t>((codes[address >> 2] & ~(3u << shift)) | (code << shift));
    }
  };
  ApuInstructionWatcher* instructionWatcher_ = nullptr;  // told each armed instruction reached; none by default
  std::unique_ptr<StandinSet> standins_;                 // the armed instructions; null until the first arm
};

}  // namespace snaggletooth
