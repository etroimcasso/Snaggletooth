# The APU machine

The APU machine is the SPC700 given real memory: the CPU core wrapped in its 64KB of RAM, the
hardware registers that overlay the top of zero page, the three timers, the communication ports the
main system talks to it through, and a store for DSP register writes. It is the whole audio
processor as a value — everything a running sound driver can see or change lives in one struct you
can copy, run, and restore.

Where the [SPC700 core](spc700-cpu.md) reads and writes an address space you supply, the machine
*is* that address space. It routes the CPU's reads and writes: `$00F0`–`$00FF` reach the register
overlay, everything else is plain RAM. The same interpreter the vectors pin runs unchanged over it.

There is no boot ROM. A real SNES runs Sony's 64-byte IPL program to bring the audio chip up; the
machine skips that and constructs the state the IPL leaves behind, so no copyrighted bytes are ever
needed. You load a driver image straight into RAM and point the CPU at it.

## Constructing and stepping

`Apu()` builds the seeded power-on machine. Load a program into RAM, point the CPU at it, and step:

```cpp
#include "snaggletooth/apu/apu.h"

snaggletooth::Apu apu;

const std::uint8_t program[] = {0xE8, 0x2A};  // MOV A,#$2A
apu.loadRam(0x0200, program);
apu.setPc(0x0200);

std::uint32_t cycles = apu.step();  // runs one instruction, advances the timers

// apu.state().cpu.a == 0x2A
// cycles            == 2
```

`step()` runs exactly one instruction, then advances the timers by the cycles it took, and returns
that count. To run against a cycle budget, `run()` steps whole instructions until the budget is met:

```cpp
std::uint64_t ran = apu.run(1024);  // step until at least 1024 cycles have passed
```

The final instruction may carry `ran` past the budget — it runs whole instructions, never a partial
one — so the caller keeps the returned count and subtracts its budget to carry the remainder into the
next call. `run(0)` runs nothing and returns 0. Every instruction costs at least two cycles, so a
budget is always reached.

## The register overlay

The sixteen bytes `$F0`–`$FF` are hardware registers laid over RAM. From the CPU's side:

- A write to a register address updates the register **and** the RAM byte beneath it.
- A read of a register address returns the register, never the RAM behind it.
- Write-only registers (TEST, CONTROL, the timer targets) read back `0`.
- `$F8` and `$F9` are plain RAM with no register attached.

The registers:

| Address | Register | Behavior |
|---|---|---|
| `$F0` | TEST | Stored; power-on `$0A`. A write is ignored while the CPU's `P` flag is set. |
| `$F1` | CONTROL | Write-only. Timer enables, the input-port clears, the IPL-mapping bit. |
| `$F2` | DSPADDR | The DSP register address latch. |
| `$F3` | DSPDATA | Reads/writes the selected DSP register. |
| `$F4`–`$F7` | Ports 0–3 | The communication latches (see below). |
| `$F8`, `$F9` | — | Plain RAM. |
| `$FA`–`$FC` | T0–T2 TARGET | Write-only timer targets. |
| `$FD`–`$FF` | T0–T2 OUT | Read-only 4-bit timer outputs; a read clears the value. |

### DSP register file

DSPADDR (`$F2`) selects one of 128 DSP registers; DSPDATA (`$F3`) reads or writes the selected one.
A write with the address above `$7F` is ignored; a read masks the address with `$7F`. The machine
only stores these bytes — no DSP behavior attaches to them yet. Snapshots carry the file, so a later
DSP component and the `.spc` loader both find their register state here.

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

1. **Stage 1** — a single shared divider counts machine cycles for all three. It runs constantly and
   can never be stopped or reset; a timer's stage-1 tick is a crossing of its base period.
2. **Stage 2** — while the timer is enabled, an 8-bit counter increments on each stage-1 tick. When
   it reaches the timer's TARGET it advances stage 3 and resets to zero. A target of `$00` means 256
   (it wraps all the way around), and TARGET 1 means every stage-1 tick passes through.
3. **Stage 3** — a 4-bit counter of the ticks stage 2 has passed on. Reading the timer's OUT register
   returns it and clears it to zero.

Enable a timer through CONTROL (bits 0–2 enable timers 0–2). A disabled timer's stage 2 does not
count; the shared stage-1 divider keeps running regardless. Enabling a timer — a 0→1 transition on
its CONTROL bit — resets its stage 2 and stage 3 to zero, but never the shared divider.

```cpp
snaggletooth::ApuState s{};
s.cpu.sp = 0xEF;
s.control = 0xB4;          // keep the power-on bits, enable timer 2 (bit 2)
s.timers[2].target = 32;   // a stage-3 tick every 32 stage-1 ticks
snaggletooth::Apu timed(std::move(s));

timed.run(32 * 16);        // 32 timer-2 stage-1 ticks
// timed.state().timers[2].stage3 == 1
```

### Timing granularity

The timers advance once per instruction, by that instruction's whole cycle count — not cycle by
cycle. A read the CPU issues in the middle of an instruction sees the timer state from before the
instruction ran; the tick lands afterward. This follows the core's cycle bar: it commits to each
instruction's documented total and leaves the placement of accesses *within* an instruction
unspecified. A sound driver's output does not depend on that sub-instruction placement.

A halted core (after SLEEP or STOP) still delivers two cycles per step and still advances the timers,
because the clock belongs to the machine, not to the CPU. So `run()` keeps time passing over a halted
core.

## Boot and reset

`Apu()` constructs the state the boot ROM leaves behind: RAM cleared to zero, the stack pointer at
`$EF` (the stack lives in page `$01`), TEST `$0A`, CONTROL `$B0`, the `$AA`/`$BB` ready bytes posted,
timer targets `$00`, and timer outputs `$F`. The CPU's PC is `$0000` — there is no IPL to point it
at, so a host sets the entry with `setPc` after loading an image.

`reset()` re-seeds that state with the documented reset differences: the timer outputs go to `0`
instead of `$F`, the timer targets are retained (not cleared), the shared stage-1 divider is retained
(it cannot be reset), zero page is cleared but the rest of RAM keeps its contents, and CONTROL, TEST,
the ports, and the CPU return to their reset values.

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

- **Timers step per instruction, not per cycle.** The tick from an instruction lands after the whole
  instruction, so a mid-instruction read sees the pre-step timer state. This is by design, matching
  the core's cycle bar.
- **Reading a timer output clears it.** `$FD`–`$FF` return the 4-bit stage-3 count and zero it. A
  dummy read counts: an instruction that writes a timer-output address reads it first (most write
  opcodes read their destination), and that read clears the output.
- **Write-only registers read back 0.** TEST, CONTROL, and the timer targets return 0 when read; the
  write still lands in the RAM byte beneath them.
- **There is no IPL ROM.** `$FFC0`–`$FFFF` is plain RAM, not a boot ROM window; CONTROL's high bit is
  carried as state but maps nothing. Set the CPU entry yourself with `setPc`.
- **TEST is stored, not fully modeled.** The machine stores TEST and honors the documented
  "ignored while `P` is set" rule, but the register's behavioral bits (clock scaling, the timer gate)
  are not modeled — no sound driver touches them, and the power-on `$0A` satisfies the state the
  timers document.

## Where to look

- `include/snaggletooth/apu/apu.h` — the `ApuState`/`TimerState` value structs and the `Apu` class.
- `src/apu.cpp` — the overlay routing, the timers, `step()`/`run()`, and `reset()`.
- `tests/apu/` — the overlay, port, and timer suites, each derived from the register documentation.
- [docs/spc700-cpu.md](spc700-cpu.md) — the CPU core the machine wraps.
