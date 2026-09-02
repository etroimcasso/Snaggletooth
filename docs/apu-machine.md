# The APU machine

The APU machine is the SPC700 given real memory: the CPU core wrapped in its 64KB of RAM, the
hardware registers that overlay the top of zero page, the three timers, the communication ports the
main system talks to it through, and a store for DSP register writes. It is the whole audio
processor as a value — everything a running sound driver can see or change lives in one struct you
can copy, run, and restore.

Where the [SPC700 core](spc700-cpu.md) reads and writes an address space you supply, the machine
*is* that address space. It routes the CPU's reads and writes: `$00F0`–`$00FF` reach the register
overlay; `$FFC0`–`$FFFF` return a mapped boot-ROM image when one is set and CONTROL bit 7 is on (see
*The boot-ROM window* below); everything else is plain RAM. The same interpreter the vectors pin runs
unchanged over it.

There is no console boot ROM. A real SNES runs Sony's 64-byte IPL program to bring the audio chip up;
the machine skips that and constructs the state the IPL leaves behind, so no copyrighted bytes are
ever needed. You load a driver image straight into RAM and point the CPU at it — or, to run the
console's upload handshake, map an original 64-byte boot program over the `$FFC0` window (see *The
boot-ROM window*).

## Constructing and stepping

`Apu()` builds the seeded power-on machine. Load a program into RAM, point the CPU at it, and step:

```cpp
#include "snaggletooth/apu/apu.h"

snaggletooth::Apu apu;

const std::uint8_t program[] = {0xE8, 0x2A};  // MOV A,#$2A
apu.loadRam(0x0200, program);
apu.setPc(0x0200);

std::uint32_t cycles = apu.step();  // runs one instruction, cycle by cycle

// apu.state().cpu.a == 0x2A
// cycles            == 2
```

`step()` runs to the end of one instruction and returns how many cycles it took. To run against a
cycle budget, `run()` runs exactly that many cycles:

```cpp
std::uint64_t ran = apu.run(1024);  // exactly 1024 cycles; ran == 1024
```

`run()` spends its budget to the cycle. If the budget runs out partway through an instruction the
machine stops there, which is a legal resting place — the instruction's progress is part of the state
value, so a snapshot carries it and `restore()` resumes on the next cycle. Splitting a run changes
nothing: `run(a)` then `run(b)` leaves the machine exactly where one `run(a + b)` leaves it.
`run(0)` runs nothing.

Calling `step()` after a run stopped mid-instruction finishes the instruction in progress rather than
starting a new one, and returns only the cycles it still owed.

### What a cycle is

A machine cycle is one pass of everything the chip does at once, in a fixed order:

1. The master counter advances.
2. The timer stage-1 ticks that land on the new count are taken.
3. The DSP takes its sample if the new count is a frame boundary.
4. The CPU makes its one bus access — a read, a write, or nothing on an internal or halted cycle.

The CPU going last is the multiplexed bus's own order, and it decides every race between the CPU and
the rest of the chip on a shared cycle. A read of a timer output on the cycle that timer ticks returns
the incremented count and clears it. A write that enables or disables a timer on a tick cycle applies
*after* that tick, so the tick lands under the old enable state. A write to a DSP register on a frame
boundary misses that frame and reaches the next one.

## The register overlay

The sixteen bytes `$F0`–`$FF` are hardware registers laid over RAM. From the CPU's side:

- A write to a register address updates the register **and** the RAM byte beneath it.
- A read of a register address returns the register, never the RAM behind it.
- Write-only registers (TEST, CONTROL, the timer targets) read back `0`.
- `$F8` and `$F9` are the auxiliary ports (the S-SMP's unconnected P4/P5 pins): storage of their
  own that reads back what the CPU wrote. The RAM beneath them takes the same write, but a DSP echo
  buffer sweeping the region writes only that RAM — the port bytes the CPU reads never change.

The registers:

| Address | Register | Behavior |
|---|---|---|
| `$F0` | TEST | Stored; power-on `$0A`. A write is ignored while the CPU's `P` flag is set. |
| `$F1` | CONTROL | Write-only. Timer enables, the input-port clears, and the boot-ROM window enable (bit 7 — see *The boot-ROM window*). |
| `$F2` | DSPADDR | The DSP register address latch. |
| `$F3` | DSPDATA | Reads/writes the selected DSP register. |
| `$F4`–`$F7` | Ports 0–3 | The communication latches (see below). |
| `$F8`, `$F9` | AUXIO | Port bytes of their own; read back the last CPU write, untouched by echo writes to the RAM beneath. Power-on `$FF` (the unconnected pins). |
| `$FA`–`$FC` | T0–T2 TARGET | Write-only timer targets. |
| `$FD`–`$FF` | T0–T2 OUT | Read-only 4-bit timer outputs; a read clears the value. |

### DSP register file

DSPADDR (`$F2`) selects one of 128 DSP registers; DSPDATA (`$F3`) reads or writes the selected one.
A write with the address above `$7F` is ignored; a read masks the address with `$7F`. The [DSP](dsp.md)
reads this file as it runs, so a write here changes what the next sample does — and the cycle a write
lands on decides which sample that is. Snapshots carry the file.

```cpp
const std::uint8_t select_and_write[] = {
    0x8F, 0x10, 0xF2,  // MOV DSPADDR,#$10   select DSP register $10
    0x8F, 0x7F, 0xF3,  // MOV DSPDATA,#$7F   write it
};
apu.loadRam(0x0200, select_and_write);
apu.setPc(0x0200);
apu.run(10);
// apu.state().dsp[0x10] == 0x7F
```

## The boot-ROM window

`$FFC0`–`$FFFF` can serve a 64-byte boot-ROM image to the SPC700 while CONTROL bit 7 is set.
`mapIplRom` sets the image:

```cpp
std::array<std::uint8_t, 64> boot = /* an original upload program */;
apu.mapIplRom(boot);
```

With an image mapped and CONTROL bit 7 set, a CPU read in `$FFC0`–`$FFFF` returns the image. Writes
always reach the RAM beneath the window, so a driver can scratch those addresses and still read the
image back unchanged — the way the console's boot ROM reads back after the upload handshake writes
over the RAM under it. Clearing bit 7, or mapping no image (the default), exposes that RAM: the window
reads as plain memory.

The image is configuration, not machine state. It is fixed for the machine's life the way a cartridge
is: `restore()` and `reset()` keep it, and it is not part of the `ApuState` a snapshot carries. Host
RAM access (`readRam`/`writeRam`) ignores the window and reaches the RAM beneath.

No console boot ROM is ever needed. The image a host maps is an original program written to the
documented upload protocol, never Sony's bytes.

## The communication ports

The main system and the SPC700 pass bytes through eight latches behind four addresses. Each port has
two independent halves: an **input** the host writes and the CPU reads, and an **output** the CPU
writes and the host reads. Writing one half never disturbs the other. From the host side:

```cpp
apu.writePort(0, 0xCC);              // host -> SPC700 (the CPU reads this at $F4)
std::uint8_t reply = apu.readPort(0);  // SPC700 -> host (what the CPU wrote to $F4)
```

`writePort`/`readPort` are the *other side of the same bus* the CPU reaches at `$F4`–`$F7`. Today the
host drives them directly; when a main-CPU component lands it drives the same four registers, so the
port surface is shaped for a bus, not for a host convenience.

Two CONTROL bits let the SPC700 clear its input ports: writing CONTROL with bit 4 set zeroes input
ports 0 and 1, bit 5 zeroes ports 2 and 3. The clear happens on every write with the bit set (it is
a level, not an edge), and it never touches the output latches.

At power-on the machine posts the ready bytes a booting system polls for: output port 0 holds `$AA`
and port 1 holds `$BB`.

## The timers

Three timers divide the SPC700's clock down to periodic ticks a driver polls. Timers 0 and 1 have a
base period of 128 cycles (~8 kHz); timer 2 runs at 16 cycles (~64 kHz). Each has three stages:

1. **Stage 1** — the machine's master counter drives all three. It runs constantly and can never be
   stopped or reset; a timer's stage-1 tick is a slot of that counter (see *The tick slots* below).
2. **Stage 2** — while the timer is enabled, an 8-bit counter increments on each stage-1 tick. When
   it reaches the timer's TARGET it advances stage 3 and resets to zero. A target of `$00` means 256
   (it wraps all the way around), and TARGET 1 means every stage-1 tick passes through.
3. **Stage 3** — a 4-bit counter of the ticks stage 2 has passed on. Reading the timer's OUT register
   returns it and clears it to zero.

Enable a timer through CONTROL (bits 0–2 enable timers 0–2). A disabled timer's stage 2 does not
count; the master counter keeps running regardless. Enabling a timer — a 0→1 transition on its
CONTROL bit — resets its stage 2 and stage 3 to zero, but never the counter.

```cpp
snaggletooth::ApuState s{};
s.cpu.sp = 0xEF;
s.cpu.pc = 0x0200;         // away from page 0, so the CPU never fetches through the overlay
s.control = 0xB4;          // keep the power-on bits, enable timer 2 (bit 2)
s.timers[2].target = 32;   // a stage-3 tick every 32 stage-1 ticks
snaggletooth::Apu timed(std::move(s));

timed.run(32 * 16);        // 32 timer-2 stage-1 ticks
// timed.state().timers[2].stage3 == 1
```

Point the CPU somewhere before running it, as above. Left at `$0000` over cleared RAM it executes
NOPs upward into `$00F0`–`$00FF`, where its own instruction fetches are register reads — and a fetch
at `$00FF` reads T2OUT, which clears the count this example is watching.

### The tick slots

The SPC700 and the DSP share a reset line and a clock, so the timers do not run on a phase of their
own — their stage-1 ticks land on fixed slots of the DSP's 32-cycle sample frame. Timer 2 ticks on
the first slot of each half-frame and timers 0 and 1 on the first slot of every fourth frame, which
puts them on master-counter values one past a multiple of 16 and of 128:

| Event | Master counter |
|---|---|
| Timer 2 stage-1 tick | ≡ 1 (mod 16) |
| Timers 0 and 1 stage-1 tick | ≡ 1 (mod 128) |
| DSP sample frame | ≡ 0 (mod 32) |

The counter wraps at 65536 — a multiple of all three periods — so every slot survives the wrap.

A halted core (after SLEEP or STOP) makes no bus access, but its cycles still pass: `step()` prices
two of them and `run()` keeps spending the budget, so the timers and the DSP keep going. The clock
belongs to the machine, not to the CPU.

## Boot and reset

`Apu()` constructs the state the boot ROM leaves behind: RAM cleared to zero, the stack pointer at
`$EF` (the stack lives in page `$01`), TEST `$0A`, CONTROL `$B0`, the `$AA`/`$BB` ready bytes posted,
timer targets `$00`, and timer outputs `$F`. The CPU's PC is `$0000` — there is no IPL to point it
at, so a host sets the entry with `setPc` after loading an image.

`reset()` re-seeds that state with the documented reset differences: the timer outputs go to `0`
instead of `$F`, the timer targets are retained (not cleared), the master counter is retained (it
cannot be reset, so the tick slots and sample boundaries keep their phase), zero page is cleared but
the rest of RAM keeps its contents, and CONTROL, TEST, the ports, and the CPU return to their reset
values.

```cpp
apu.reset();  // the machine returns to a known state without losing loaded sample data above page 0
```

## Snapshot and restore

The whole machine is one value. `state()` returns it; copy it to snapshot, and `restore()` assigns it
back. The snapshot carries everything: the CPU, all 64KB of RAM, the overlay registers, both sides of
every port, the DSP file, and the timer stages. Two machines restored from the same snapshot run
identically.

```cpp
const snaggletooth::ApuState snapshot = apu.state();  // capture
apu.run(50'000);                                       // run on
apu.restore(snapshot);                                 // rewind to the capture
```

This is the whole-state-as-a-value model: `Apu` holds no hidden state, so a snapshot plus a record of
host port writes replays a session exactly.

## Host RAM access

For loading images and inspecting memory, the machine exposes RAM directly:

```cpp
const std::uint8_t image[] = {0xE8, 0x00};  // MOV A,#$00

apu.writeRam(0x0400, 0x12);                 // write one byte
std::uint8_t b = apu.readRam(0x0400);       // read one back
apu.loadRam(0x0200, image);                 // copy a span in
apu.setPc(0x0200);                          // point the CPU at a loaded image
```

These bypass the overlay — RAM is RAM from the host side, so `readRam($00F2)` returns the byte beneath
DSPADDR, not the register the CPU would see there.

## Gotchas

- **The CPU's access is the last thing in its cycle.** Everything the machine clocks — the counter,
  the timer ticks, the DSP's sample — has already happened when the access lands. So a read sees the
  tick that shares its cycle, and a write does not reach the sample that shares its own.
- **A budget can stop mid-instruction.** `run()` is exact, so `state().cpu.tcu` may be non-zero when
  it returns. That is a complete state, not a torn one: snapshot it, restore it, and `step()` or
  `run()` picks up on the next cycle.
- **Reading a timer output clears it.** `$FD`–`$FF` return the 4-bit stage-3 count and zero it. A
  dummy read counts: an instruction that writes a timer-output address reads it first (most write
  opcodes read their destination), and that read clears the output.
- **Write-only registers read back 0.** TEST, CONTROL, and the timer targets return 0 when read; the
  write still lands in the RAM byte beneath them.
- **The boot-ROM window is opt-in.** With no image mapped — the default — `$FFC0`–`$FFFF` is plain
  RAM and CONTROL bit 7 maps nothing; load a driver into RAM and set the entry with `setPc`. Map an
  image with `mapIplRom` and CONTROL bit 7 turns the window into that image for CPU reads, while
  writes still fall through to the RAM beneath. No console boot ROM is needed either way.
- **TEST is stored, not fully modeled.** The machine stores TEST and honors the documented
  "ignored while `P` is set" rule, but the register's behavioral bits (clock scaling, the timer gate)
  are not modeled — no sound driver touches them, and the power-on `$0A` satisfies the state the
  timers document.

## Where to look

- `include/snaggletooth/apu/apu.h` — the `ApuState`/`TimerState` value structs and the `Apu` class.
- `src/apu.cpp` — the machine cycle, the overlay routing, the timers, `step()`/`run()`, and `reset()`.
- `tests/apu/` — the overlay, port, timer and cycle-timing suites, each derived from the register
  and low-level-timing documentation.
- [docs/spc700-cpu.md](spc700-cpu.md) — the CPU core the machine wraps.
