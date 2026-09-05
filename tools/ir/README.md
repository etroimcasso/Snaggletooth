# The intermediate representation

`ir.h` is a 65816 program as its meaning: one node per instruction, carrying what
source says about the instruction and the typed effects the chip performs for it,
with no bytes in it. `cpu65816_lift.h` builds a program from a listing the 65816
disassembler traced — the one place in the toolkit where bytes become meaning —
`ir_interpret.h` runs a program's effects and nothing else, `ir_render.h` writes
SNES assembly from its instruction layer, `ir_lockstep.h` holds the interpreter
to one step of the machine, `ir_differential.h` runs a program beside the
machine through a recorded run and reports every disagreement, `ir_dataflow.h`
runs the effects over every path at once and says what each instruction can
rely on — the direct register, the data bank, the stack pointer, a stored
value, the slots of a jump table — and `ir_text.h` writes it out for reading. Two commands sit over the library, and the cartridge
disassembler in [`../rom/`](../rom/README.md) writes its bank files through the
renderer.

## Contents

- [Surface](#surface)
- [`snes_lift`](#snes_lift)
- [`snes_differential`](#snes_differential)
- [Using the library](#using-the-library)
- [See also](#see-also)

## Surface

Everything lives in `snaggletooth::ir`.

| Symbol | Purpose |
|---|---|
| `Node` | One instruction at one address under one mode: the instruction layer, the effect layer, the measured cost. |
| `Program` | The nodes in address order with the two hardware interrupt sequences; `find` answers the node for the live flags. |
| `lift65816(listing, image, base)` | A whole 65816 listing as a program. |
| `liftInstruction(instruction, mode)` | One decoded instruction as a node. |
| `Interpreter` | Runs a node or an interrupt sequence over a `Bus` the host implements, and returns the cycles. |
| `StepObserver`, `checkNode(…)`, `checkInterrupt(…)`, `registersOf(state)` | One step of the machine collected — the fetches, the data accesses, the cycles — and the interpreter run over it and checked; every disagreement is a `Divergence`. |
| `differential(program, replay)` | Replays a run on the machine beside the interpreter; a `DifferentialReport` of what was checked and every `Divergence`. |
| `Dataflow(program, entries, sightings, image, canonical)` | Runs the effects over every path from the entries; `before(address)` is what is proven there, `derived()` every table slot a bounded index selects. |
| `evaluate(node, before, image)` | One node over a `State`: the state after and every access it can make. |
| `renderNode(node)`, `renderEffect(effect)` | A node, an effect, as text. |
| `renderInstruction(instruction, names)`, `renderLine(node, names, bytesWidth)` | An instruction as source, with a label, a register name and an annotation in place of addresses where given; a line with its comment. |
| `encode(instruction)`, `renderCost(node)` | The bytes an instruction assembles to; the cost as a listing prints it. |
| `SourceMode` | The mode a region of source carries in file order, and the directives each instruction needs before it. |

## `snes_lift`

```
snes_lift <directory> <image> [-o <file.snagir>] [--file <name>]
```

Lifts a cartridge tree — the directory's `project.manifest` and the image it
was written for — and writes the summary, then every node with its effects,
region by region in address order. `--file` limits it to one region's file;
`-o` writes to a file, which carries the `.snagir` extension.

```
snes_lift mixed mixed.smc
regions 1
code lines 34
nodes 34
…
$00:800A  STA abs  operand $100  length 3  e=0 m=16 x=16  base 4/4/5/5
    Set PC <- $800D  [16]
    BankAddress T0 <- $100  [24]
    Store T0, A  [16 flat]
```

## `snes_differential`

```
snes_differential <directory> <image> -o <report> [--seconds N] [--input <script>]
```

Lifts the tree the same way, runs the machine for `--seconds` of the master
clock (sixty by default) with the recorded run `--input` replayed into the
controller ports, and holds the interpreter to every access, every register and
every cycle the machine made. The report — `summary.txt`, `divergences.txt`,
`forms.txt`, `constructs.txt`, `unlifted.txt` — lands under `-o`; one line sums
it up, and the exit status is 0 only when the run diverged nowhere.

```
snes_differential mixed mixed.smc -o mixed/differential --seconds 0.1
OK : 35228 instructions, 3 interrupts, 132147 CPU cycles, 0 held, 0 unlifted at 0 addresses, 29 forms, 34 of 57 constructs unexercised, stopped, 0 divergences
```

## Using the library

```cpp
#include "ir/cpu65816_lift.h"
#include "ir/ir_interpret.h"

const snaggletooth::ir::Program program =
    snaggletooth::ir::lift65816(listing, bytes, base);

snaggletooth::ir::Interpreter interpreter;
const auto& r = interpreter.registers;
const snaggletooth::ir::Node* node =
    program.find((r.pbr << 16) | r.pc, r.e, r.accumulator8(), r.index8());
interpreter.execute(*node, bus);
```

The library target is `snaggletooth_ir`; `tools/` is on its public include path.
It links `snaggletooth_cpu65816` for the lift, which reads a listing and the
measured cycle tables, and for the renderer, which reads the opcode table and
follows the widths through the same function the assembler does. The
interpreter's own translation unit includes neither. The lockstep is its own
target, `snaggletooth_ir_lockstep`, which links the representation and the
machine, and two things link it: the differential, its own target
`snaggletooth_ir_differential`, which links `snaggletooth_rom` as well for the
recorded run it replays; and `snaggletooth_rom` itself, whose run on the
machine lifts every executed instruction from its fetches and holds it to the
same check — and which links the representation for the bank files it renders.

## See also

- [docs/ir.md](../../docs/ir.md) — the full page: the two layers, the
  vocabulary, every rule the effects follow, the cost, reading a program,
  rendering source, running beside the machine, and how the lift is held to
  the core.
- [`../rom/`](../rom/README.md) — the cartridge disassembler, whose bank files
  the renderer writes and whose run on the machine drives the lockstep.
- [`../examples/`](../examples/README.md) — the cartridges the examples above
  are run on.
- [`../cpu65816/`](../cpu65816/README.md) — the disassembler whose listing the
  lift reads.
- [`../disasm/`](../disasm/README.md) — the framework that shapes the listing.
