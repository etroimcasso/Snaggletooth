# DSP-1 behavior and provenance

A reference for the DSP-1: what its commands do, how long each takes, and what settles each claim.
The DSP-1 is a NEC µPD77C25 running Nintendo's program; the chip's instruction set is described in
[upd77c25-cpu.md](upd77c25-cpu.md), and this page is the evidence for the behavior a cartridge sees
through the chip's two ports.

No command's behavior is pinned, so the page holds its standard, its sources, and the list of what
is open.

## Contents

- [The evidentiary standard](#the-evidentiary-standard)
- [Sources](#sources)
- [What is not settled yet](#what-is-not-settled-yet)
  - [The chip's clock](#the-chips-clock)
  - [The commands](#the-commands)
  - [The core's single-source readings](#the-cores-single-source-readings)

## The evidentiary standard

Four kinds of claim appear here, and they are not equally strong:

- **Documented and corroborated.** Two independent published sources agree. Taken as given.
- **Documented but contested.** The published sources disagree, or one is self-contradictory.
  Decided by a cartridge written for the question, and the decision is recorded with the
  observation that forced it.
- **Undocumented.** No published source states it. Derived from observed behavior, so the
  reconstruction behind the claim is described, not just asserted.
- **Measured on the console.** [The project's SNES](#sources) ran a cartridge written for the
  question. On this page that is the strongest claim available, with one limit stated in the
  sources table: the console reaches a DSP-1 through a flash cartridge, and the flash cartridge
  answers with its own implementation of the chip, not with DSP-1 silicon. A measurement through it
  is the flash cartridge's answer, recorded as such.

**No implementation's source is read, and no DSP-1 program image is read, run or compared against.**
Snaggletooth is a clean-room implementation: the contract is public hardware documentation plus
observable behavior.

## Sources

| Source | Role |
|---|---|
| **The project's SNES**: an SNS-001 of the US launch revision, 5A22 version 1, 5C77 (PPU1) version 1, 5C78 (PPU2) version 1 | **The arbiter above every other**, and the only instrument that runs a question on hardware. It runs the cartridges written here from a flash cartridge, which answers a DSP-1 board's ports with its own implementation of the chip. Its answers are that implementation's, at that standing, for every command's result and its timing, until DSP-1 silicon is under test. |
| Martin Korth's fullsnes, "SNES Cart DSP-n/ST010/ST011" | Primary, and the only staged source for the µPD77C25's instruction set. The chip's registers, the status register, the handshake with the host, the three instruction forms, the ALU and its flags, the sources and destinations, the jumps, reset, the multiplier, the boards' maps, and the DSP-1's commands by number. |
| SNESdev Wiki, "DSP-1" | Secondary. The two port addresses by board, the data types, and eight commands with equations and cycle counts. Its derivation is not stated, so it corroborates and never decides. |

## What is not settled yet

### The chip's clock

No staged source gives the DSP-1's clock on any board. fullsnes gives 8.192 MHz as the µPD77C25's
maximum and the board's oscillator as unknown. The rate at which the machine steps the chip against
the main CPU is open until it is measured.

### The commands

No command's equations or cycle counts are pinned. fullsnes lists the DSP-1's commands by number;
the SNESdev page gives equations and cycle counts for eight of them, which corroborate and do not
decide. Each command joins this page as a cartridge written for it settles its results and its
timing.

### The core's single-source readings

fullsnes is the only source for each of these, and the core takes the reading named. None is
verified on the chip.

- **The order inside one ALU word.** fullsnes gives the fields and nothing on the order they act
  in. The core reads the source, forms the operand, runs the operation, writes the destination,
  adjusts the pointers, returns, and recomputes the product, in that order
  ([upd77c25-cpu.md §The order inside one word](upd77c25-cpu.md#the-order-inside-one-word)).
- **The product's update point.** fullsnes says the product is computed after each instruction
  that changed K or L. The core reads that as: the word that writes K or L reads the product from
  before it.
- **The product of −8000h × −8000h × 2.** fullsnes gives it as unknown. The core keeps the 32-bit
  wrap, 80000000h.
- **The stack past four entries.** fullsnes gives four levels and nothing past them. The core's
  stack pointer wraps, so a fifth call writes over the first entry.
- **A jump value fullsnes gives for no µPD77C25 instruction**, the three only the ST010 and ST011
  define among them. The core never takes it.
- **S1 after a logical operation.** fullsnes gives S1 as officially undefined after `Or`, `And`,
  `Xor`, `Not`, the shifts and `Xchg`, and as the result's sign in practice. The core takes the
  sign.
- **SR's bits 6 to 2.** fullsnes gives them as reading zero, with a question mark. The core reads
  them as zero whatever a program writes.
- **A program's DR access and the host's fragments.** fullsnes gives DRS toggling after each byte
  the host moves in 16-bit mode. The core also starts the host's next access at the low byte when
  the program reads or writes DR.
- **An 8-bit host write.** fullsnes does not say what becomes of DR's high byte. The core keeps it.
- **A host access with RQM clear.** fullsnes gives the host's access as the completion of one the
  program asked for. The core moves DR the same way whether or not RQM is set.
