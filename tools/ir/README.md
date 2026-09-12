# The intermediate representation

`ir.h` is a program as its meaning: one node per instruction, carrying what
source says about the instruction and the typed effects the chip performs for it,
with no bytes in it — the main CPU's program in the 65816's terms and the sound
program in the SPC700's, one vocabulary over each chip's own places.
`cpu65816_lift.h` builds the first from a listing the 65816 disassembler traced
and `spc700_lift.h` the second from the SPC700 disassembler's — the two places
in the toolkit where bytes become meaning —
`ir_interpret.h` runs a program's effects and nothing else, `ir_provenance.h`
is the shadow that follows where every value came from, `ir_render.h` writes
SNES assembly from its instruction layer, `ir_lockstep.h` holds the interpreter
to one step of the machine, `ir_differential.h` runs a program beside the
machine through a recorded run and reports every disagreement, `ir_dataflow.h`
runs the effects over every path at once and says what each instruction can
rely on — the direct register, the data bank, the stack pointer, a stored
value, the slots of a jump table — and `ir_text.h` writes a program file and
reads it back, the main CPU's `program.snagir` or the sound program's
`apu.snagir`. Two commands sit over the library, both readers of those files;
the cartridge disassembler in [`../rom/`](../rom/README.md) writes them, and
`snes_render` writes the bank files and the sound file from them through the
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
| `Program` | The main CPU's nodes in address order with the two hardware interrupt sequences, and the sound program's nodes in their own; `find` answers the node for the live flags, `findSpc700` the sound node at an audio address. |
| `lift65816(listing, image, base)` | A whole 65816 listing as a program. |
| `liftInstruction(instruction, mode)` | One decoded 65816 instruction as a node. |
| `liftSpc700(listing)`, `liftSpc700Instruction(instruction)` | A whole SPC700 listing as the sound program's nodes; one decoded SPC700 instruction as a node. |
| `Interpreter` | Runs a 65816 node or an interrupt sequence over a `Bus` the host implements, and returns the cycles; tells its `Shadow`, if one is set, every move a value makes. |
| `Spc700Interpreter` | Runs a sound-CPU node over the same `Bus` with the sound CPU's registers and rules, and returns the cycles. |
| `Provenance`, `Origins`, `OriginSet`, `CarrySink` | The shadow that carries, beside every value, the image bytes it was computed from — interned interval sets, a mark for a register or the save — and keeps work RAM's origin, last writer and each invocation's reads as runs, the maximal stretches it read; `originOf`, `writerOf`, `sourcesOf` read it back, `streams()` is every sequence of stores the CPU made to a data register — from the image, with the run its carrier read as the file, or from a buffer in work RAM, told to the `CarrySink` byte by byte — each with where the port put its bytes, which the sink answers store by store and is told of as the stream closes. |
| `StepObserver`, `checkNode(…)`, `checkInterrupt(…)`, `registersOf(state)` | One step of the main CPU collected — the fetches, the data accesses, the cycles — and the interpreter run over it and checked; every disagreement is a `Divergence`, which says which CPU it is on. |
| `Spc700Access`, `splitSpc700Step(step, pc, length)`, `checkSpc700Node(…)` | One sound-CPU access as the audio machine's observer reports it; a step's accesses with the instruction's own fetches set apart; the sound interpreter run over the data and checked against the registers after and the cycles. |
| `differential(program, replay)` | Replays a run on the machine beside both interpreters; a `DifferentialReport` of what was checked on each CPU and every `Divergence`. |
| `Dataflow(program, entries, sightings, image, canonical)` | Runs the effects over every path from the entries; `before(address)` is what is proven there, `derived()` every table slot a bounded index selects. |
| `evaluate(node, before, image)` | One node over a `State`: the state after and every access it can make. |
| `renderProgram(program, file)`, `parseProgram(text, error)`, `Processor` | One chip's program file (`docs/snagir.md`) written from a program and what it does not carry — the chip among them — and read back to both. |
| `renderNode(node, processor)`, `renderEffect(effect)`, `equivalent(a, b)` | A node and an effect as the file has them; two programs compared as the files carry them, both node lists. |
| `selectFile(parsed, file)`, `countProgram(parsed)` | A parsed file cut to one source file's regions and their nodes; what a parsed file carries, counted over its own chip's nodes as `snes_lift` prints it. |
| `renderInstruction(instruction, names)`, `renderLine(node, names, bytesWidth)` | An instruction as source, with a label, a register name and an annotation in place of addresses where given; a line with its comment. |
| `encode(instruction)`, `renderCost(node)` | The bytes an instruction assembles to; the cost as a listing prints it. |
| `renderSpc700Instruction(instruction, targetLabel)`, `renderSpc700Line(node, targetLabel, bytesWidth)` | A sound-CPU instruction as source, with a label in place of its target where given; one line of the sound program's source with its comment. |
| `encodeSpc700(instruction)`, `opcodeOfSpc700(instruction)`, `renderSpc700Cost(node)` | The bytes a sound-CPU instruction assembles to; the opcode its mnemonic and form name; its cost as the listing prints it. |
| `SourceMode` | The mode a region of source carries in file order, and the directives each instruction needs before it. |

## `snes_lift`

```
snes_lift <directory> [--apu] [-o <file.snagir>] [--file <name>]
```

Reads the directory's `program.snagir` — or, with `--apu`, its `apu.snagir`,
the sound program's ([docs/snagir.md](../../docs/snagir.md)) — and writes the
summary — the regions, the code lines, the nodes, and how many select a width
by the live flag, name a hardware register or were lifted from patched bytes,
and the effects — then the program file written again from what it read:
every region with its labels, its data runs and its nodes in address order,
and, for the main CPU's file, the interrupt sequences. `--file` limits both
to the regions written to one source file; `-o` writes the file there. It
reads no image and runs nothing.

```
snes_lift mixed
regions 1
code lines 34
nodes 34
…
  $00:800A STA abs operand $100 length 3 flow continue e=0 m=16 x=16 base 4/4/5/5 {
    Set PC <- $800D [16];
    BankAddress T0 <- $100 [24];
    Store T0, A [16 flat];
  }
```

## `snes_differential`

```
snes_differential <directory> <image> -o <report> [--seconds N] [--input <script> | --input-dir <directory>] [--quiet]
```

Reads the directory's `program.snagir` the same way, and its `apu.snagir`
where the tree has one, refuses an image of
another size than the file's own `image` line, runs the machine for
`--seconds` of the master clock (sixty by default) with the recorded run
`--input` replayed into the controller ports — or the run named for the image
under `--input-dir`, or that directory's `default.snaginput` — and holds each interpreter to every access, every
register and every cycle its CPU made. The report — `summary.txt`, `divergences.txt`,
`forms.txt`, `constructs.txt`, `unlifted.txt`, `patched.txt` — lands under `-o`, the sound CPU's
lines after the main CPU's and marked `apu`; one line sums
it up, and the exit status is 0 only when the run diverged nowhere. While it
replays, standard error says how far it has come, `replaying the run: 23.5 of
60.0 s`, refreshed in place on a terminal and one line per ten seconds in a
log; `--quiet` turns it off. The library reports through `Replay::progress`
(`rom/progress.h`) and prints nothing itself.

```
snes_differential mixed mixed.smc -o mixed/differential --seconds 0.1
OK : 35228 instructions, 3 interrupts, 132147 CPU cycles, 0 held, 0 unlifted at 0 addresses, 29 forms, 34 of 57 constructs unexercised, stopped, 0 divergences; sound: 0 instructions, 0 cycles, 16240 unlifted at 8 addresses, 0 with other bytes at 0 addresses, 0 forms, 0 divergences
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
It links `snaggletooth_cpu65816` for the 65816 lift, which reads a listing and the
measured cycle tables, and for the renderer, which reads the opcode table and
follows the widths through the same function the assembler does; and
`snaggletooth_spc700` for the SPC700 lift, which reads a listing, the measured
cycle table and the mnemonic and form of each opcode, and for the sound
program's renderer and file, which resolve a node's mnemonic and form against
the same table. The two interpreters' own translation units include none of
them. `snes_lift` links this
target alone, so it reads a program file and cannot trace a cartridge. The lockstep is its own
target, `snaggletooth_ir_lockstep`, which links the representation and the
machine for the two observers it reads, and two things link it: the differential, its own target
`snaggletooth_ir_differential`, which links `snaggletooth_rom` as well for the
recorded run it replays; and `snaggletooth_rom` itself, whose run on the
machine lifts every executed instruction, on either CPU, and holds it to the
same check — and which links the representation for the bank files it renders.
The shadow is its own target too, `snaggletooth_ir_provenance`, which links the
representation and the cartridge map and nothing else; `snaggletooth_rom` links
it for the run.

## See also

- [docs/ir.md](../../docs/ir.md) — the full page: the two layers, the
  vocabulary, every rule the effects follow, the cost, reading a program,
  rendering source, running beside the machine, and how the lift is held to
  the core.
- [`../rom/`](../rom/README.md) — the cartridge disassembler, whose bank files
  and sound file the renderer writes and whose run on the machine drives the
  lockstep.
- [`../examples/`](../examples/README.md) — the cartridges the examples above
  are run on.
- [`../cpu65816/`](../cpu65816/README.md) — the disassembler whose listing the
  65816 lift reads.
- [`../spc700/`](../spc700/README.md) — the disassembler whose listing the
  SPC700 lift reads, and whose table names each opcode by mnemonic and form.
- [`../disasm/`](../disasm/README.md) — the framework that shapes the listing.
