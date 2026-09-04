# The intermediate representation

`tools/ir/ir.h` is a 65816 program as its meaning: one node per instruction, each
carrying what source says about the instruction and what the chip does for it,
with no bytes anywhere. `tools/ir/cpu65816_lift.h` builds it from a listing the
[65816 disassembler](65816-disassembler.md) traced, `tools/ir/ir_interpret.h`
runs it, and `tools/ir/ir_differential.h` runs it beside the machine and reports
every place the two disagree. Two commands put those in a person's hands:
`snes_lift` writes a cartridge's program out for reading, and `snes_differential`
replays a cartridge's recorded run and reports.

Three properties shape everything below.

**It holds no bytes.** A node names its instruction the way source does — address,
mnemonic, addressing mode, operand value, the mode it reads under — and says what
the instruction does as a sequence of typed effects over the CPU's registers, a
few temporaries and a bus. The mnemonic and the addressing mode together name one
opcode, and the operand with its width names the operand bytes, so a renderer can
reproduce the bytes without holding them; an interpreter runs the effects and
nothing else. The lift is the one place bytes enter, and it is where they stop.

**It models the CPU and nothing else.** A memory access is a load or a store at an
address the effects before it computed, with every wrap the chip has written into
the address arithmetic. Whether that address is a hardware register is the memory
map's answer, and the memory map belongs to whatever runs the program. A register
name is attached to a node only where the instruction's own bytes name the bank.

**A width is a type where the trace settled it, and a selection by the live flag
where it did not.** After `PLP` or `RTI` in native mode the flags are whatever came
off the stack; the node says its loads and stores are as wide as the flag makes
them, and the interpreter reads the flag when it runs.

---

## Contents

- [The two layers](#the-two-layers)
- [Vocabulary](#vocabulary)
  - [Places and widths](#places-and-widths)
  - [Steps, accesses and conditions](#steps-accesses-and-conditions)
  - [Operations](#operations)
- [The rules](#the-rules)
- [Cost](#cost)
- [Lifting a listing](#lifting-a-listing)
- [Running a program](#running-a-program)
- [Reading a program](#reading-a-program)
- [Running beside the machine](#running-beside-the-machine)
  - [What is checked](#what-is-checked)
  - [What is an input](#what-is-an-input)
  - [The report](#the-report)
- [How it is checked](#how-it-is-checked)
- [Library](#library)
- [Stability](#stability)
- [Status](#status)
- [See also](#see-also)

## The two layers

A `Node` is one instruction at one address under one mode.

The **instruction layer** (`Node::instruction`) is what source says: the address,
the length, the mnemonic, the addressing mode, the operand's value, and the
constant successor of a branch, jump or call when the instruction names one. The
operand is the value as the dialect writes it — an immediate, a direct-page
offset, an absolute or long address, the target address of a relative form, the
value `PEA` pushes, the address `PER` names, or a block move's source bank with the
destination bank beside it. `Node::mode` is the mode the instruction reads under:
the emulation flag, and each register width with whether the trace knew it.

The **effect layer** (`Node::effects`) is what the chip does: effects in the order
the chip makes their accesses. Every node begins by stepping the program counter
past the instruction; what follows may move it again. A load or store names the
address it reaches through a temporary an earlier effect computed, so the address
arithmetic is on the page rather than inside an operation.

Beside the two layers a node carries its measured cost, the hardware register
its operand names when a long operand names one, and whether it was lifted from
bytes that differ from the image the code started as.

A `Program` is nodes in address order — an address two paths read two ways is
two nodes, the first reading first — with the two hardware interrupt sequences
attached, which are the chip's rather than the program's.

## Vocabulary

Everything lives in `snaggletooth::ir`.

### Places and widths

An effect names its operands as places. The registers are the CPU's own; the four
temporaries belong to the node and start at zero each time it runs; a flag is one
bit of the status register; `Imm` is a constant carried in the operand's value.

| Place | Meaning |
|---|---|
| `A` | The 16-bit accumulator, A below and B above |
| `X`, `Y` | The index registers |
| `S`, `D`, `PC` | The stack pointer, the direct register, the program counter |
| `PBR`, `DBR` | The program and data bank registers |
| `P`, `E` | The status register as a byte, and the emulation flag beside it |
| `T0`–`T3` | The node's temporaries |
| `FlagN` … `FlagC` | One bit of `P`: N, V, M, X, D, I, Z, C |
| `Imm` | A constant |

A width sizes the values an effect works on:

| Width | Bits |
|---|---|
| `Byte`, `Word`, `Long` | 8, 16, 24 |
| `ByM` | 8 under emulation or with the M flag set; 16 otherwise |
| `ByX` | 8 under emulation or with the X flag set; 16 otherwise |

The lift writes `Byte` or `Word` where the trace settled a width and `ByM` or
`ByX` where it did not, so a node lifted after `PLP` is right whichever way the
flag falls.

### Steps, accesses and conditions

A multi-byte access finds its second and third bytes by a `Step`:

| Step | The next byte's address |
|---|---|
| `Flat` | The next 24-bit address |
| `Bank0` | The next address within bank zero |
| `Bank` | The next address within the first byte's bank |
| `Direct` | Within bank zero — or within the page, under emulation with the direct register's low byte zero |
| `DirectPointer` | Within bank zero — or within the page, under emulation with the direct register zero |

An `Access` says what a bus access is for — `Data`, `Rmw` for a read-modify-write's
read and write-back, `RmwUnmodified` for the write of the byte just read that
emulation mode drives before the write-back, `Vector` for an interrupt vector
pull. A run compares addresses, values and order alike whatever the kind; the kind
says which pins the chip drives.

A `Cond` guards an effect, and `Always` is the default:

| `When` | The effect runs when |
|---|---|
| `Emulation`, `Native` | The emulation flag is set, or clear |
| `FlagSet`, `FlagClear` | The flag `place` names is set, or clear |
| `PlaceIs`, `PlaceIsNot` | The register `place` names holds `value`, or does not |
| `DirectLowByte` | The direct register's low byte is non-zero |
| `IndexCrossed` | The index registers are eight bits wide and the last bank-relative address's low-byte addition carried |

`andEmulation` adds "and the emulation flag is set" to any of them.

### Operations

Each effect is `dst ← f(a, b)` where it has a destination. A register written at
eight bits follows the register's own rule: the accumulator keeps its high byte,
an index register clears its own, and the rest take the value as it is.

| Op | Meaning |
|---|---|
| `Set` | `dst ← a` |
| `SetNZ` | `dst ← a`, with N and Z from the value at the width — a load or a transfer |
| `Add`, `Sub`, `And`, `Or`, `Xor`, `Shr` | `dst ← a op b`, masked to the width, no flags |
| `DirectAddress` | `dst ←` the bank-zero address `D + a + b`, page-wrapped under emulation with D's low byte zero |
| `BankAddress` | `dst ← DBR:a + b` as a 24-bit sum, recording whether the low-byte addition carried |
| `LongAddress` | `dst ← a + b` as a 24-bit sum |
| `ProgramAddress` | `dst ← PBR:a` |
| `StackAddress` | `dst ← S + a` in bank zero |
| `Load` | `dst ←` the value at address `a`, bytes stepping by `step`, low byte first |
| `Store` | The value `b` to address `a`, low byte first |
| `StoreRmw` | The same, high byte first — a read-modify-write's write-back |
| `Push` | The value `a` onto the stack, high byte first; `pinned` keeps S in page one under emulation |
| `Pull` | `dst ←` the value pulled, low byte first; `pinned` likewise |
| `SettleStack` | Under emulation, S back into page one |
| `Adc`, `Sbc` | `dst ← a ± b` with the carry at the width, decimal when D is set; N V Z C |
| `Cmp` | N Z C from `a - b`; nothing written |
| `Bit` | Z from `a & b`; N and V from `b`'s two top bits |
| `BitImm` | Z from `a & b` alone |
| `Asl`, `Lsr`, `Rol`, `Ror` | The shift or rotate of `a`; C from the bit shifted out; N Z |
| `Inc`, `Dec` | `dst ← a ± 1`; N Z |
| `Tsb`, `Trb` | `dst ← a \| b` or `a & ~b`; Z from `a & b` |
| `WriteP` | `P ← a`, with M and X forced set under emulation, the index high bytes cleared when X narrows, and S pinned under emulation |
| `Xba` | Exchange the accumulator's halves; N Z from the new low byte |
| `Xce` | Exchange C and E; entering emulation forces both widths, clears the index high bytes and pins S |
| `Halt` | Stop running: `a` is 0 for a wait an interrupt ends, 1 for a stop only a reset ends |
| `Cycles` | The instruction costs `a` more cycles |

## The rules

What the lift writes for each construct, and what the interpreter honours. Each is
the chip's own behaviour, and the [core](65816-cpu.md) over a flat bus is what the
lift is held to.

- **Register widths.** A load, store, transfer or arithmetic effect carries a
  width; an eight-bit accumulator effect leaves B alone, an eight-bit index load
  clears the index's high byte. `REP` and `SEP` are `And` or `Or` on a copy of P
  followed by `WriteP`, which is the one write that re-establishes the invariants;
  under emulation the mask's M and X bits are inert. `PLP` and `RTI` end in the
  same `WriteP`. `XCE` entering emulation forces both widths, pins S to page one and
  clears the index high bytes.
- **The flags.** N, Z, V, C, D and I are places, and each operation writes exactly
  the flags the chip does: `Cmp` moves N Z C and not V; `BitImm` moves Z alone
  while `Bit` takes N and V from the operand's top bits; `Tsb` and `Trb` move Z
  only; `Inc` and `Dec` move N Z only; `Xba` sets N Z from the new low byte; the
  `SetNZ` of `PLB` and `PLD` sets them on what was loaded.
- **Decimal mode.** `Adc` and `Sbc` read the D flag: a nibble-adjusted add or
  subtract at eight or sixteen bits, C from the adjusted result, V from the binary
  sum for a subtract and from the high-nibble sum before the final adjust for an
  add.
- **The direct page.** `DirectAddress` is `(D + dp + index) & $FFFF` in bank zero,
  except under emulation with D's low byte zero, where the addition stays within
  the page; a direct operand's second byte steps by `Direct`, which follows the
  same rule.
- **Direct-page pointers.** The pointer of `(dp)`, `(dp,X)` and `(dp),Y` steps by
  `DirectPointer`: within the page only under emulation with the whole direct
  register zero. The pointers of `[dp]`, `[dp],Y`, `(sr,S),Y` and `PEI` step by
  `Bank0`, out of the page whatever the mode.
- **Absolute and long addressing.** `BankAddress` adds an index into all 24 bits,
  so `DBR:$FFFF,X` reaches the next bank rather than wrapping; so does
  `LongAddress`. The pointer of `JMP (abs)` and `JML [abs]` is in bank zero and
  wraps within it; the pointer of `JMP (abs,X)` and `JSR (abs,X)` is in the program
  bank and wraps within that.
- **The program counter.** It wraps within the program bank on every step; a
  relative form's target is computed by the lift and carried as an address; only
  `JML`, `JSL`, `RTL` and the interrupt sequences move PBR.
- **The stack.** A push writes at S and steps it down, a word high byte first; a
  pull steps S up and reads. Under emulation a `pinned` push or pull keeps S in
  page one on every byte — the registers the 6502 pushed and pulled, `JSR`, `RTS`,
  `RTI`, `BRK`, `COP` and the hardware sequences, and `JSR (abs,X)` — while the
  rest step out and `SettleStack` puts S back afterwards. `TXS` and `TCS` write S
  and settle it, so under emulation only the low byte moves.
- **Calls and returns.** `JSR` and `JSR (abs,X)` push the address of the
  instruction's last byte and `RTS` adds one to what it pulls; `JSL` pushes PBR
  first; `RTL` pulls three bytes; `PEA`, `PEI` and `PER` push sixteen bits whatever
  M says, so `PEA target-1` followed by `RTS` dispatches with no special case.
- **`BRK` and `COP`.** PBR under native mode, then the address after the signature
  byte, then P, are pushed pinned; I is set and D cleared; the vector the emulation
  flag selects is read from bank zero; PBR becomes zero.
- **Hardware interrupts.** Not effects of the program: `Program::nmi` and
  `Program::irq` are sequences the interpreter runs between two nodes when its host
  asks. Each reads the interrupted instruction and throws it away, pushes PBR under
  native mode, the program counter as it stands, and P — with bit 4 cleared under
  emulation, where it is the break flag — then enters the handler as `BRK` does.
- **`WAI` and `STP`.** A `Halt` effect. A waiting interpreter is released by an
  interrupt sequence, or by `Interpreter::release` when a masked request wakes the
  chip without being taken; a stopped one only by a reset, which reaches it as fresh
  registers.
- **Read-modify-write.** `Load` under `Rmw`, then — under emulation, for a node
  narrower than a word — a `Store` of the unmodified byte under `RmwUnmodified`,
  then the operation, then `StoreRmw`, whose sixteen-bit write-back goes high byte
  first.
- **Block moves.** One byte per run of the node: DBR takes the destination bank,
  the byte moves from the source bank at X to the data bank at Y, both indexes step
  at their width, the whole sixteen-bit accumulator counts down, and the program
  counter goes back on the instruction unless the count ran past zero. An interrupt
  between bytes is only an interrupt.
- **Loads and stores against hardware.** Always a `Load` or `Store` at the computed
  address. `Node::registerName` is set only for a long operand, from the same
  table the disassembler names registers from; for every other form the runtime
  classifies the address.
- **Reads.** A read's value is the bus's to answer. The effects assert the address
  and the width, never the value.
- **Immediates under an unknown width.** No node: the trace stops there, and the
  bytes have no length to lift.

## Cost

`Node::cost.base` holds the instruction's measured cost under each setting of the
widths, indexed by `costIndex(accumulator8, index8)`, from the same tables the
disassembler prints. The interpreter takes the base for the live widths when the
node starts and adds every `Cycles` effect that fires. The lift writes those
effects for the increments the tables leave out, each guarded by the condition
that charges it:

| Increment | Guard | Where |
|---|---|---|
| A low byte in the direct register | `DirectLowByte` | Every direct-page form, `PEI` included |
| A page crossed under an eight-bit index | `IndexCrossed` | A read through `abs,X`, `abs,Y` or `(dp),Y` |
| A branch taken | The branch's own flag | The eight conditional branches |
| A taken branch crossing a page under emulation | The flag, `andEmulation` | The nine relative branches, where the lift finds the target's page differs from the address after the branch |

A hardware interrupt sequence carries its own `Cycles` effects: seven, and one
more under native mode.

## Lifting a listing

```cpp
#include "cpu65816_disasm.h"
#include "ir/cpu65816_lift.h"

using namespace snaggletooth;

disasm::Request request;
request.image = bytes;  // std::span<const std::uint8_t>
request.base = 0x008000;
request.context = disasm::contextOf(disasm::Cpu65816Mode::reset());
const disasm::Listing listing = disasm::trace(disasm::cpu65816Backend(), request);

const ir::Program program = ir::lift65816(listing, bytes, request.base);
for (const ir::Node& node : program.nodes) {
  node.instruction.mnemonic;  // "LDA"
  node.instruction.operand;   // $2100
  node.effects.size();
}
```

`lift65816` makes one node per code line, in address order. The trace reports an
address two paths read two ways as a warning; given the image the listing was
traced from, the lift decodes the second reading and places it after the first,
and `Program::find(address, emulation, accumulator8, index8)` answers the node for
the live flags. Without the image, an address reads one way only.

`liftInstruction(instruction, mode)` lifts one decoded instruction on its own —
what a harness does with an instruction it decoded itself.

## Running a program

The interpreter holds the registers and reads memory through a `Bus` its host
implements:

```cpp
#include "ir/ir_interpret.h"

struct FlatBus final : snaggletooth::ir::Bus {
  std::vector<std::uint8_t> memory = std::vector<std::uint8_t>(0x1000000);
  std::uint8_t read(snaggletooth::ir::Address address, snaggletooth::ir::Access) override {
    return memory[address & 0xFFFFFFu];
  }
  void write(snaggletooth::ir::Address address, std::uint8_t value,
             snaggletooth::ir::Access) override {
    memory[address & 0xFFFFFFu] = value;
  }
};

FlatBus bus;
snaggletooth::ir::Interpreter interpreter;
interpreter.registers.pc = 0x8000;
interpreter.registers.e = true;

const snaggletooth::ir::Registers& r = interpreter.registers;
const snaggletooth::ir::Node* node =
    program.find((r.pbr << 16) | r.pc, r.e, r.accumulator8(), r.index8());
const std::uint32_t cycles = interpreter.execute(*node, bus);
```

`execute` re-establishes the invariants the chip holds between instructions —
the index high bytes zero while the index registers are eight bits wide, the
stack in page one under emulation — runs every effect whose condition holds, in
order, and returns the cycles. A block move leaves the program counter on itself
while bytes remain; the host runs the node again for each. `interrupt(program.nmi,
bus)` runs a hardware sequence the same way and releases a wait first.

The interpreter's own sources include no decoder and no listing, and name no
byte: the node is all it gets.

## Reading a program

`tools/ir/ir_text.h` names every value of the vocabulary — `opName`, `placeName`,
`widthName`, `stepName`, `accessName`, `whenName`, `addressingName`, `modeName` —
and writes a node out with `renderNode`: a header line with the address, the
mnemonic and its addressing mode, the operand, the length, the mode and the
measured costs under each width setting, then each effect on a line of its own
from `renderEffect`. The text is for reading; nothing parses it back.

`snes_lift` does it for a whole cartridge tree:

```
snes_lift <directory> <image> [-o <file>] [--file <name>]
```

It reads the directory's `project.manifest`, traces the image as the manifest
directs, lifts every 65816 region, and writes the summary and then every node,
region by region, in address order. `--file` limits it to one region's file and
`-o` writes to a file. On the `mixed` cartridge from
[`tools/examples/`](../tools/examples/README.md), after `snes_disasm mixed.smc
-o mixed --no-run --no-sound`:

```
snes_lift mixed mixed.smc
regions 1
code lines 34
nodes 34
nodes selecting a width by the live flag 3
nodes naming a hardware register 0
nodes lifted from patched bytes 0
effects 125

== bank_00.asm

$00:8000  CLC   operand $0  length 1  e=1  base 2/2/2/2
    Set PC <- $8001  [16]
    Set C <- $0  [8]

$00:8001  XCE   operand $0  length 1  e=1  base 2/2/2/2
    Set PC <- $8002  [16]
    Xce  [8]

$00:8002  REP #byte  operand $30  length 2  e=0 m=8 x=8  base 3/3/3/3
    Set PC <- $8004  [16]
    Set T0 <- P  [8]
    And T0 <- T0, $CF  [8]
    WriteP T0  [8]

$00:8004  LDX #imm(X)  operand $2  length 3  e=0 m=16 x=16  base 2/3/2/3
    Set PC <- $8007  [16]
    SetNZ X <- $2  [16]

$00:8007  LDA #imm(M)  operand $1234  length 3  e=0 m=16 x=16  base 2/2/3/3
    Set PC <- $800A  [16]
    SetNZ A <- $1234  [16]

$00:800A  STA abs  operand $100  length 3  e=0 m=16 x=16  base 4/4/5/5
    Set PC <- $800D  [16]
    BankAddress T0 <- $100  [24]
    Store T0, A  [16 flat]
```

Each effect's bracket carries the width, then the step for a load or store and
the access kind where it is not plain data, or `pinned` / `unpinned` for a push
or pull; a condition follows the bracket. The four costs are the measured base
under each setting of the widths, in `costIndex` order: both eight, index
sixteen, accumulator sixteen, both sixteen.

## Running beside the machine

The vector suite proves a node at unit grain: one instruction from a given
state over a sparse memory. A cartridge running on the [machine](snes-machine.md)
is the other grain — every instruction the program takes, under the memory map,
the interrupts, the transfers and the timing the console has — and
`differential` in `tools/ir/ir_differential.h` runs the interpreter beside the
machine through a whole recorded run and reports every place the two disagree.

```cpp
#include "ir/ir_differential.h"

snaggletooth::ir::Replay replay;
replay.rom = image;                          // the cartridge
replay.masterCycles = 60u * 21'477'272u;     // sixty seconds of the master clock
replay.input = script;                       // the recorded run, or an empty InputScript
const snaggletooth::ir::DifferentialReport report =
    snaggletooth::ir::differential(program, replay);
report.divergences.empty();                  // the verdict
```

`program` is the cartridge's nodes in address order, every region's lifted
program joined and sorted, so `Program::find` answers the node at the live
program counter for the live flags at every step.

### What is checked

The machine is the oracle and the interpreter owns no memory. The machine's
[observer](snes-machine.md#the-bus-observer) reports every access the CPU makes
in a step; the interpreter then runs the node for that instruction through a bus
that answers each read with the value the machine read and checks its address,
and checks each write's address and value — in the order the machine made them,
one access to one. After the instruction the registers, the flags, the emulation
bit and the run state are checked, and the cycles the interpreter reports are
checked against the CPU cycles the observer counted. The access kind is checked
too: a read-modify-write's write-back must be what the chip drove as one.

The interpreter cannot copy a write through, because it never sees one; it has
to compute every value from the effects. A run with no divergence says the
effects carry the instruction's whole meaning, for every instruction the run
took.

### What is an input

Three things reach the interpreter from the machine rather than from a node. A
hardware interrupt the machine takes between two instructions — the observer
shows the sequence's accesses, and the interpreter runs `Program::nmi` or
`Program::irq` and is checked the same way. A wait that an interrupt line
releases — the interpreter's wait is released with it, and the halted cycles
while the CPU sat are counted, not checked. And a step the CPU spent held off
the bus while a transfer ran — nothing ran, so nothing is checked, and the step
is counted as held. A transfer's own accesses, and the work-RAM port's, are
never held against the interpreter: they are the engines', not the program's.

An instruction at an address the program has no node for — code the trace never
reached, code copied into RAM — is counted as unlifted, the address recorded,
and the interpreter realigned to the machine's registers, since there is nothing
to run. After a divergence the interpreter is realigned the same way, so the run
goes on from the machine's truth rather than compounding one disagreement into
every step after it; `Replay::divergenceLimit` ends the run once that many have
been recorded.

### The report

`DifferentialReport` carries what was checked — instructions, hardware
interrupts, CPU cycles, master cycles — what was skipped or counted — held
steps, halted cycles, released waits, unlifted instructions and their distinct
addresses — whether the CPU stopped, and the divergences. Each `Divergence`
names the step, the node's address and mnemonic (or `NMI` / `IRQ`), the mode,
the effect that made the access for an access divergence, what disagreed (`read
address`, `write value`, `register p`, `cycles`, `a read the machine did not
make`, `accesses the machine made that the node did not`, …), and the two
values — the machine's and the interpreter's.

Two histograms say what the run exercised: `forms`, how many times each
instruction form ran under each mode (`LDA abs,X e=0 m=8 x=16`), and
`constructs`, how many times each named construct was exercised — a
read-modify-write under emulation, a block move re-entered, a page-crossing
cycle charged, a wait released. Every construct is present, so one the run never
reached reads zero, and is known to rest on the vector proof alone.

`snes_differential` does all of it from a tree:

```
snes_differential <directory> <image> -o <report> [--seconds N] [--input <script>]
```

It traces and lifts the tree as `snes_lift` does, runs the machine for
`--seconds` of the master clock (sixty by default), replays `--input` into the
controller ports exactly as `snes_disasm --input` does — so the run checked is
the run that produced the tree — and writes the report under `-o`:
`summary.txt`, `divergences.txt`, `forms.txt`, `constructs.txt` and
`unlifted.txt`. One line on standard output sums it up, and the exit status is
0 only when the run diverged nowhere. On the `mixed` cartridge, which stops on
its own after three interrupts:

```
snes_differential mixed mixed.smc -o mixed/differential --seconds 0.1
OK : 35228 instructions, 3 interrupts, 132147 CPU cycles, 0 held, 0 unlifted at 0 addresses, 29 forms, 34 of 57 constructs unexercised, stopped, 0 divergences
```

```
cat mixed/differential/summary.txt
code lines 34
nodes 34
master cycles run 1021912
instructions checked 35228
hardware interrupts checked 3
CPU cycles checked 132147
steps held by a transfer 0
halted cycles 0
waits released 0
instructions with no node 0 at 0 addresses
stopped yes
divergences 0
```

`constructs.txt` lists every construct with its count; the ones this cartridge
reaches include:

```
3       NMI
3       RTI
1       STP
1       XCE
1       a 16-bit read-modify-write
2       a block move byte
1       a block move re-entered
3       a load from a hardware register
9       a node with a live-flag width
1       a store to a hardware register
8797    a taken branch
```

The `ram_code` cartridge calls a routine it copied into work RAM; its report
diverges nowhere and names the two addresses it could not check:

```
cat ram_code/differential/unlifted.txt
$7E2000
$7E2001
```

## How it is checked

Three suites hold the lift to the core, at three grains.

`tests/ir/lift_test.cpp` runs one scenario per rule above through the interpreter
and through the core over the same flat memory from the same state, and holds the
two equal on the registers, every data access in order with its address, value
and direction, the memory after, and the cycle count. Every opcode under every
mode lifts, and the interpreter's sources are read to confirm they reach no bytes.

`tests/ir/ir_vector_test.cpp` replays the SingleStepTests 65816 vectors — the
cases the [core](65816-cpu.md#testing-against-the-vectors) is proven by — through the
interpreter: each case's instruction is decoded from the bytes at its program
counter, lifted under the case's mode, and run from its registers over its sparse
memory, then held to the recorded final registers, writes, data-read addresses,
memory and cycle count. A native case whose instruction has no immediate runs a
second time lifted with both widths unknown, so the width selected by the live
flag is proven on the same cases as the typed one. The runner registers one case
per opcode per mode and skips visibly without the vectors, exactly as the core's
does; `SNAGGLETOOTH_REQUIRE_65816_VECTORS` turns the skip into a failure.

`tests/ir/differential_test.cpp` replays the example cartridges on the machine
through `differential` — a clean run with its counts, a wait released by an
interrupt, a transfer holding the CPU, an HDMA event holding it for a whole
step, a routine run from work RAM — and then breaks one effect at a time in a
lifted program to see each break named: a wrong stored value, a wrong read
address, a dropped flag write, a dropped cycle, a missing access, an extra one,
a wrong access kind, a break in the interrupt sequence. `tests/snes/observer_test.cpp`
holds the machine's observer, which the replay rests on, to what the core and
the engines drive.

The two proofs are independent by construction, and that is checked: a
deliberate wrong rule in one effect reddens the replay and leaves `snes_verify`
green, since the bytes are unchanged; a deliberate wrong character in rendered
source reddens `snes_verify` and leaves the replay green. A break that reddened
both, or neither, would mean the layers leak into each other.

## Library

| Symbol | Purpose |
|---|---|
| `Node`, `Instruction`, `Mode` | One instruction at one address under one mode: the instruction layer, the mode, the effects, the cost, the register name, the patched mark. |
| `Effect`, `Op`, `Operand`, `Place`, `Width`, `Step`, `Access`, `Cond`, `When` | The effect layer's vocabulary. |
| `Cost`, `costIndex(accumulator8, index8)` | The measured base per width setting, and its index. |
| `Program`, `Program::find(address, emulation, accumulator8, index8)` | The nodes in address order with the interrupt sequences, and the node for the live flags. |
| `lift65816(listing, image, base)` | A whole 65816 listing as a program. |
| `liftInstruction(instruction, mode, patched)` | One decoded instruction as a node. |
| `interruptSequence(Interrupt::Nmi)`, `Interrupt::Irq` | A hardware interrupt's effects. |
| `Bus` | What the interpreter reads and writes through. |
| `Registers`, `Run` | The CPU state the effects name, and whether it is running, waiting or stopped. |
| `Interpreter::execute(node, bus)`, `interrupt(sequence, bus)`, `release()` | Run a node, run a hardware sequence, release a wait. |
| `Interpreter::effectIndex` | The index of the effect whose accesses the bus is answering, so a bus can name where an access came from. |
| `differential(program, replay)` | Replay a run on the machine beside the interpreter, held to every access, register and cycle. |
| `Replay` | The cartridge, the master-cycle budget, the recorded run, and the divergence limit. |
| `DifferentialReport`, `Divergence` | What was checked, counted and skipped; each disagreement with its step, node, effect and the two values; the form and construct histograms. |
| `registersOf(state)` | A core state as the interpreter's registers. |
| `opName`, `placeName`, `widthName`, `stepName`, `accessName`, `whenName`, `addressingName`, `modeName` | Every value of the vocabulary as text. |
| `renderEffect(effect)`, `renderNode(node)` | An effect on one line; a node with its header and every effect. |

The library target is `snaggletooth_ir`; `tools/` is on its public include path,
so the headers are `ir/ir.h`, `ir/cpu65816_lift.h`, `ir/ir_interpret.h`,
`ir/ir_differential.h` and `ir/ir_text.h`. It links the 65816 disassembler for
the lift and the cartridge tools for the recorded run the differential replays.
The commands are `snes_lift` and `snes_differential`.

## Stability

The vocabulary is a published surface. An operation, place, width, step, access
or condition is never renamed or given a different meaning; a new one may be
added, and a node lifted before the addition still runs. The effect sequence the
lift writes for a construct may change where the core's behaviour is found to
differ from it, and such a change is reported with the case that found it.

## Status

The lift covers the 65816: every opcode under every mode. The interpreter runs a
node, a hardware interrupt sequence and a halt, and is held to the core at unit
grain by the vector suite and at cartridge grain by the replay beside the
machine, which checks every instruction a run takes. The audio CPU has no lift,
and the vocabulary is written so it can take one — named state per chip, a bus,
typed widths — without a change to what is here. Nothing renders from the
representation; the source tree the [cartridge disassembler](snes-disassembler.md)
writes comes from the listing. The replay reports an instruction at an address
the tree has no node for, but does not trace from it; those addresses are the
person's to answer with entries.

## See also

- [65816 disassembler](65816-disassembler.md) — the listing the lift reads, and the
  register widths carried along every path.
- [65816 CPU core](65816-cpu.md) — the chip the effects model, and the vector suite
  both are held to.
- [Disassembly framework](disassembly-framework.md) — the listing's shape, the
  context beside every address, and how a conflict is reported.
- [Cartridge disassembler](snes-disassembler.md) — a whole cartridge traced into
  the listings a program is lifted from.
- [The SNES machine](snes-machine.md#the-bus-observer) — the observer the replay
  reads the machine's accesses through.
- [The example cartridges](../tools/examples/README.md) — the cartridges this
  page's output comes from, and the ones the replay is tested on.
