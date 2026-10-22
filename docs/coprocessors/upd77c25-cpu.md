# The µPD77C25 core

The NEC µPD77C25 is the signal processor inside the DSP-1, DSP-2, DSP-3 and DSP-4 cartridges: a
16-bit fixed-point chip with its own instruction ROM, data ROM and data RAM, a hardware multiplier,
and a two-byte port the SNES reaches it through. This is its instruction-set core. It runs a program
it is handed, one instruction a chip cycle, and answers the host's reads and writes of its two ports.

The core is the chip alone. It has no clock of its own and no place in the SNES machine: a host
steps it and drives its ports. The DSP-1's own program is not part of the core; a program is a value
the host builds and hands over.

The contract is fullsnes, "SNES Cart DSP-n/ST010/ST011 - NEC uPD77C25", the only staged source for
the chip's instruction set. Where fullsnes leaves a behavior open or states it without a second
source, the core takes one reading, says so in a comment at the place it is decided, and the reading
is listed under [Gotchas](#gotchas) and in
[dsp-1-behavior.md §What is not settled yet](dsp-1-behavior.md#what-is-not-settled-yet).

## Contents

- [The program](#the-program)
- [State](#state)
- [Running instructions](#running-instructions)
- [The three forms](#the-three-forms)
- [The ALU word](#the-alu-word)
  - [The operations and their flags](#the-operations-and-their-flags)
  - [The operand](#the-operand)
  - [The sources and the destinations](#the-sources-and-the-destinations)
  - [The pointers](#the-pointers)
  - [The order inside one word](#the-order-inside-one-word)
- [The multiplier](#the-multiplier)
- [The load word](#the-load-word)
- [The jump word](#the-jump-word)
  - [The conditions](#the-conditions)
  - [Calls and returns](#calls-and-returns)
- [Reset](#reset)
- [The host's ports](#the-hosts-ports)
- [What a SNES board leaves unconnected](#what-a-snes-board-leaves-unconnected)
- [Gotchas](#gotchas)
- [Where to look](#where-to-look)

## The program

A program is two read-only memories: 2048 instruction words and 1024 data words.

```cpp
struct Upd77c25Program {
  std::array<std::uint32_t, 2048> code{};  // 24-bit instruction words, in the low bits
  std::array<std::uint16_t, 1024> data{};  // the data ROM
};
```

An instruction word is 24 bits held in the low bits of 32; the core ignores the top byte. The core
keeps a pointer to the program and never copies it, so the program must outlive the core. The
constructor that would take a temporary is deleted.

## State

The whole chip is one value struct. Snapshot it by copying, restore it by assignment:

```cpp
struct Upd77c25State {
  std::uint32_t product = 0;             // K·L·2, as last computed
  std::uint16_t pc = 0;                  // 11 bits
  std::uint16_t rp = 0;                  // 10 bits: the data ROM pointer
  std::array<std::uint16_t, 4> stack{};  // return addresses
  std::uint16_t k = 0;                   // the multiplier's factors
  std::uint16_t l = 0;
  std::uint16_t accA = 0;
  std::uint16_t accB = 0;
  std::uint16_t tr = 0;                  // the two temporary registers
  std::uint16_t trb = 0;
  std::uint16_t sr = 0;                  // the status register
  std::uint16_t dr = 0;                  // the data register the host reads and writes
  std::array<std::uint16_t, 256> ram{};  // the data RAM
  std::uint8_t dp = 0;                   // the data RAM pointer
  std::uint8_t sp = 0;                   // 2 bits: the entry the next call writes
  std::uint8_t flagA = 0;                // AccA's six flags
  std::uint8_t flagB = 0;                // AccB's six flags
};
```

The default is all zero, the state a power-on starts from. Every member is a fixed-width integer,
and the struct has no padding.

Each accumulator's flags are one byte. Named masks on the struct read them:

| Mask | Bit | Flag |
|---|---|---|
| `kFlagS1` | `0x20` | the sign of the last overflow |
| `kFlagS0` | `0x10` | the result's bit 15 |
| `kFlagC` | `0x08` | carry, or borrow on a subtraction |
| `kFlagZ` | `0x04` | the result is zero |
| `kFlagOv1` | `0x02` | an odd number of overflows since the flag was last cleared |
| `kFlagOv0` | `0x01` | the last operation overflowed |

The status register's bits have masks of their own: `kSrRqm` (bit 15), `kSrUsf1` and `kSrUsf0`
(14, 13), `kSrDrs` (12), `kSrDma` (11), `kSrDrc` (10), `kSrSoc` (9), `kSrSic` (8), `kSrEi` (7),
`kSrP1` and `kSrP0` (1, 0).

## Running instructions

Construct the core over a program and an optional starting state, and step it:

```cpp
#include "snaggletooth/upd77c25/upd77c25.h"

snaggletooth::Upd77c25Program program;
program.code[0] = 0xC00000u | (0x1234u << 6) | 0x1u;  // LD #1234h, A

snaggletooth::Upd77c25 chip(program);
chip.stepCycle();
// chip.state().accA == 0x1234
// chip.state().pc   == 1
```

The surface:

```cpp
class Upd77c25 {
 public:
  explicit Upd77c25(const Upd77c25Program& program, Upd77c25State state = {}) noexcept;

  const Upd77c25State& state() const noexcept;
  void restore(const Upd77c25State& state) noexcept;

  void reset() noexcept;      // the RST line
  void stepCycle() noexcept;  // one chip cycle, one whole instruction

  std::uint8_t hostRead(bool status) noexcept;  // the host's read; status selects SR over DR
  void hostWrite(std::uint8_t value) noexcept;  // the host's write to DR
  bool requestingHost() const noexcept;         // RQM
};
```

Every instruction takes one chip cycle, so `stepCycle()` executes a whole word: it fetches the word
at `pc`, advances `pc` (wrapping in 11 bits) and executes it. No instruction is ever part-way through
between two calls, so a snapshot taken between any two calls restores to an exact point. The core
holds no wall clock; how many cycles to run against the host's own clock is the host's to decide.

## The three forms

The top two bits of a word name its form:

| Bits 23–22 | Form |
|---|---|
| `00`, `01` | an ALU word; bit 22 is its return bit |
| `10` | a jump |
| `11` | a load |

```
   23 22 21 20 19 18 17 16 15 14 13 12 11 10  9  8  7  6  5  4  3  2  1  0
  | 0|RT|  P  |    ALU    | A| DPL |    DPH    |RP|    SRC    |    DST    |  ALU
  | 1| 0|           BRCH           |              NA              |  -  |  jump
  | 1| 1|                    ID                     |  -  |    DST    |  load
```

The header declares an enum for each field's values, so a word can be built by name: `Upd77c25AluOp`,
`Upd77c25Operand` (P), `Upd77c25Source`, `Upd77c25Destination` and `Upd77c25Branch`.

## The ALU word

An ALU word does five things at once: one operation on an accumulator, one move from a source to a
destination, an adjustment of each pointer, and an optional return from a call. The move's value is
also the internal bus, which the operation can take as its operand.

### The operations and their flags

The A bit picks the accumulator: AccA and FlagA when clear, AccB and FlagB when set. The operation
reads and writes that accumulator and that flag byte alone.

| Code | Operation | Result | S1 | S0 | C | Z | OV1 | OV0 |
|---|---|---|---|---|---|---|---|---|
| `0` | `Nop` | unchanged | – | – | – | – | – | – |
| `1` | `Or` | Acc OR P | sign | sign | 0 | zero | 0 | 0 |
| `2` | `And` | Acc AND P | sign | sign | 0 | zero | 0 | 0 |
| `3` | `Xor` | Acc XOR P | sign | sign | 0 | zero | 0 | 0 |
| `4` | `Sub` | Acc − P | ovf | sign | borrow | zero | ovf | overflow |
| `5` | `Add` | Acc + P | ovf | sign | carry | zero | ovf | overflow |
| `6` | `Sbb` | Acc − P − other C | ovf | sign | borrow | zero | ovf | overflow |
| `7` | `Adc` | Acc + P + other C | ovf | sign | carry | zero | ovf | overflow |
| `8` | `Dec` | Acc − 1 | ovf | sign | borrow | zero | ovf | overflow |
| `9` | `Inc` | Acc + 1 | ovf | sign | carry | zero | ovf | overflow |
| `A` | `Not` | Acc XOR FFFFh | sign | sign | 0 | zero | 0 | 0 |
| `B` | `Sar1` | Acc ÷ 2, the sign kept | sign | sign | bit 0 out | zero | 0 | 0 |
| `C` | `Rcl1` | Acc × 2 + other C | sign | sign | bit 15 out | zero | 0 | 0 |
| `D` | `Sll2` | Acc × 4 + 3 | sign | sign | 0 | zero | 0 | 0 |
| `E` | `Sll4` | Acc × 16 + 15 | sign | sign | 0 | zero | 0 | 0 |
| `F` | `Xchg` | the two bytes swapped | sign | sign | 0 | zero | 0 | 0 |

"Other C" is the carry of the accumulator the A bit does not name: `Adc`, `Sbb` and `Rcl1` on AccB
take FlagA's C. `Dec` and `Inc` ignore the operand.

"ovf" marks the overflow flags. OV0 is set when the signed result leaves −8000h..7FFFh. When it is
set, S1 takes the result's sign and OV1 flips; when it is clear, S1 and OV1 keep their values. A
positive overflow wraps to a negative result, so S1 is 1 after it, and the saturation value the
`Sgn` source reads, `8000h − S1`, is 7FFFh.

```cpp
// ADD A, TR — 7FFFh + 1 overflows upwards
state.accA = 0x7FFF;
state.tr = 0x0001;
// after: accA 8000h, flagA S1 | S0 | OV1 | OV0
```

### The operand

The P field picks what the operation takes besides the accumulator:

| P | `Upd77c25Operand` | Operand |
|---|---|---|
| 0 | `Ram` | RAM[DP] |
| 1 | `Bus` | the value the source put on the internal bus |
| 2 | `ProductHigh` | K·L·2's high word |
| 3 | `ProductLow` | K·L·2's low word |

### The sources and the destinations

The SRC field names what the word moves; the DST field names where it lands.

| Code | Source | Destination |
|---|---|---|
| `0` | TRB | none |
| `1` | AccA | AccA |
| `2` | AccB | AccB |
| `3` | TR | TR |
| `4` | DP | DP (its low 8 bits) |
| `5` | RP | RP (its low 10 bits) |
| `6` | RO: the data ROM word at RP | DR; sets RQM |
| `7` | SGN: 8000h less AccA's S1 | SR (see below) |
| `8` | DR; sets RQM | SOL: the serial output |
| `9` | DRNF: DR, RQM left alone | SOM: the serial output |
| `A` | SR | K |
| `B` | SIM: the serial input | KLR: K takes the value, L the data ROM word at RP |
| `C` | SIL: the serial input | KLM: L takes the value, K takes RAM[DP OR 40h] |
| `D` | K | L |
| `E` | L | TRB |
| `F` | MEM: RAM[DP] | MEM: RAM[DP] |

A write to SR reaches every bit but RQM (15) and DRS (12), which are the chip's own, and bits 6 to 2,
which read zero.

A program's own access to DR, as the DR source or the DR destination, sets RQM and starts the host's
next 16-bit access at the low byte. DRNF reads DR without either.

fullsnes prohibits three combinations in an ALU word: KLR or KLM as the destination with K or L as
the source, a source and destination naming the same register, and the RAM operand with the MEM
destination. The core runs them by the order below; no program written for the chip relies on what
they do.

### The pointers

| Field | Values |
|---|---|
| DPL (bits 14–13) | 0 leaves DP; 1 adds 1 to DP's low nibble; 2 subtracts 1 from it; 3 clears it. The nibble wraps inside itself: 3Fh plus one is 30h. |
| DPH (bits 12–9) | XORs DP's high nibble with the field's value |
| RP (bit 8) | 1 subtracts 1 from RP, wrapping in 10 bits |

```cpp
// DP 5Ah, DPH 3: the high nibble 5 XOR 3 is 6
// after: dp == 0x6A
```

### The order inside one word

fullsnes gives an ALU word's fields and nothing on the order they act in. The core acts in this
order:

1. The source is read, with its side effect, onto the internal bus.
2. The operand is formed: RAM[DP], the bus, or the product as it stands.
3. The operation runs on the accumulator the A bit names and writes it and its flags.
4. The bus is written to the destination. A destination naming the accumulator the operation wrote
   holds the moved value.
5. DP and RP are adjusted. A destination of DP or RP is adjusted after it is written; RAM[DP] and
   the data ROM word at RP are read at the pointers as they stood.
6. With the RT bit set, the word returns.
7. With K or L written, the product is recomputed.

A source naming the accumulator the operation writes reads its value from before the operation.

## The multiplier

The product is K and L as signed 16-bit numbers, multiplied and doubled, kept 32 bits wide in
`product`. It is recomputed after a word that wrote K or L, so the word that writes a factor reads
the product from before it, and the next word reads the new one.

```cpp
// LD #FFFFh, K   — K is -1
// LD #0003h, L   — L is 3: the product is -6, FFFFFFFAh
// OR A, P=ProductHigh  — AccA FFFFh
```

−8000h × −8000h × 2 is 2³¹, one past the largest signed 32-bit value; the core keeps the
arithmetic's wrap, 80000000h.

## The load word

A load word writes its 16-bit immediate to its destination, with the destination's side effects:
the DR destination sets RQM, KLR and KLM fill the second factor, and a written factor recomputes the
product. A load adjusts no pointer and never returns.

## The jump word

A jump word's BRCH field names the jump and NA, bits 12 to 2, the 11-bit address it goes to. A jump
whose condition holds sets `pc` to NA; one whose condition fails leaves `pc` on the word after it.

### The conditions

| BRCH | Jumps when |
|---|---|
| `100h` `Jmp` | always |
| `140h` `Call` | always, first writing the address after it to the stack |
| `080h` `Jnca` · `082h` `Jca` | AccA's C is 0 · 1 |
| `084h` `Jncb` · `086h` `Jcb` | AccB's C is 0 · 1 |
| `088h` `Jnza` · `08Ah` `Jza` | AccA's Z is 0 · 1 |
| `08Ch` `Jnzb` · `08Eh` `Jzb` | AccB's Z is 0 · 1 |
| `090h` `Jnova0` · `092h` `Jova0` | AccA's OV0 is 0 · 1 |
| `094h` `Jnovb0` · `096h` `Jovb0` | AccB's OV0 is 0 · 1 |
| `098h` `Jnova1` · `09Ah` `Jova1` | AccA's OV1 is 0 · 1 |
| `09Ch` `Jnovb1` · `09Eh` `Jovb1` | AccB's OV1 is 0 · 1 |
| `0A0h` `Jnsa0` · `0A2h` `Jsa0` | AccA's S0 is 0 · 1 |
| `0A4h` `Jnsb0` · `0A6h` `Jsb0` | AccB's S0 is 0 · 1 |
| `0A8h` `Jnsa1` · `0AAh` `Jsa1` | AccA's S1 is 0 · 1 |
| `0ACh` `Jnsb1` · `0AEh` `Jsb1` | AccB's S1 is 0 · 1 |
| `0B0h` `Jdpl0` · `0B1h` `Jdpln0` | DP's low nibble is 0 · is not 0 |
| `0B2h` `Jdplf` · `0B3h` `Jdplnf` | DP's low nibble is Fh · is not Fh |
| `0B4h` `Jnsiak` · `0B6h` `Jsiak` | the serial input's acknowledge is 0 (always) · 1 (never) |
| `0B8h` `Jnsoak` · `0BAh` `Jsoak` | the serial output's acknowledge is 0 (always) · 1 (never) |
| `0BCh` `Jnrqm` · `0BEh` `Jrqm` | RQM is 0 · 1 |

Any other BRCH value is a jump that is never taken. That covers the three only the ST010 and ST011
define, `000h`, `101h` and `141h`.

A program waits on the host with a jump to itself on RQM:

```
0:  LD  #00AAh, DR     ; RQM set
1:  JRQM 1             ; wait until the host has read DR
2:  ...
```

### Calls and returns

`Call` writes the address of the word after it to `stack[sp]` and steps `sp`; a word with the RT bit
set steps `sp` back and takes `pc` from that entry. `sp` is two bits and wraps, so the stack holds
four return addresses and a fifth call writes over the first.

## Reset

`reset()` is the RST line: `pc` 0, FlagA and FlagB 0, SR 0 (DR 16 bits wide, RQM and DRS clear), RP
3FFh. Every other register, the stack pointer among them, and the data RAM keep their values.

## The host's ports

A SNES cartridge reaches the chip through its A0 line: one address range reads and writes DR, the
other reads SR. `hostRead(true)` is the status read: SR's high byte, RQM in bit 7 and DRS in bit 4,
and it changes nothing. A cartridge waits for RQM with `BPL` on that byte.

`hostRead(false)` and `hostWrite()` move DR a byte at a time:

- **With SR's DRC bit set, DR is 8 bits wide.** A read takes DR's low byte, a write replaces it and
  keeps the high byte, and either one completes the access.
- **With DRC clear, DR is 16 bits wide**, moved low byte first. The first access sets DRS; the second
  moves the high byte, clears DRS and completes the access.

A completed access clears RQM. The access moves the same way whether or not RQM is set.

```cpp
// The program ran LD #BEEFh, DR
chip.hostRead(true);   // 80h: RQM
chip.hostRead(false);  // EFh
chip.hostRead(true);   // 90h: RQM and DRS
chip.hostRead(false);  // BEh, and RQM clears
```

A write with A0 set reaches nothing on the chip, so the core has no function for it.

## What a SNES board leaves unconnected

The serial port, the interrupt input, the DMA request line and the P0/P1 outputs are wired to
nothing on any SNES board:

- SIM and SIL read zero; SOL and SOM take a value that goes nowhere.
- The serial acknowledges stay 0, so `Jnsiak` and `Jnsoak` always jump and `Jsiak` and `Jsoak` never
  do.
- No interrupt arrives, so the core never enters address 100h on its own, and SR's EI bit changes
  nothing.
- SR's DMA bit and P0/P1 bits hold what the program writes and drive nothing.

## Gotchas

- **The program outlives the core.** The core keeps a pointer, and a temporary program does not
  compile.
- **`Sgn` reads AccA's S1 alone.** fullsnes gives it as `8000h − SA1`; the A bit does not change it.
- **The logical operations set S1.** fullsnes gives S1 as officially undefined after `Or`, `And`,
  `Xor`, `Not`, the shifts and `Xchg`, and as the result's sign in practice. The core takes the
  sign.
- **The word that writes K or L reads the old product.** The product is recomputed after the word.
- **DPL wraps inside the nibble.** 3Fh plus one is 30h, not 40h.
- **A destination of DP or RP is adjusted after it is written.** A move to DP with DPINC leaves the
  moved value plus one in the low nibble.
- **Readings fullsnes alone carries.** These are the core's, from one source, and unverified on the
  chip: the order inside an ALU word; the product's update point and its value at −8000h × −8000h ×
  2; the stack's wrap past four; a BRCH value fullsnes gives for no µPD77C25 jump; S1 after a
  logical operation; bits 6 to 2 of SR reading zero; a program's DR access restarting the host's
  fragments; an 8-bit host write keeping DR's high byte; and a host access moving DR with RQM
  clear. [dsp-1-behavior.md §What is not settled yet](dsp-1-behavior.md#what-is-not-settled-yet)
  carries each.

## Where to look

- `include/snaggletooth/upd77c25/upd77c25.h` — the program, the state, the field enums and the
  `Upd77c25` class.
- `src/upd77c25.cpp` — the decode, the ALU, the sources and destinations, the jumps, the stack, the
  multiplier and the host's ports.
- `tests/upd77c25/core_test.cpp` — every form, operation, source, destination, condition and port
  rule on hand-encoded words.
- [dsp-1-behavior.md](dsp-1-behavior.md) — the DSP-1's own behavior and what is not settled.
