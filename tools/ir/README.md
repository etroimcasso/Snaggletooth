# The intermediate representation

`ir.h` is a 65816 program as its meaning: one node per instruction, carrying what
source says about the instruction and the typed effects the chip performs for it,
with no bytes in it. `cpu65816_lift.h` builds a program from a listing the 65816
disassembler traced — the one place in the toolkit where bytes become meaning —
and `ir_interpret.h` runs a program's effects and nothing else.

## Surface

Everything lives in `snaggletooth::ir`.

| Symbol | Purpose |
|---|---|
| `Node` | One instruction at one address under one mode: the instruction layer, the effect layer, the measured cost. |
| `Program` | The nodes in address order with the two hardware interrupt sequences; `find` answers the node for the live flags. |
| `lift65816(listing, image, base)` | A whole 65816 listing as a program. |
| `liftInstruction(instruction, mode)` | One decoded instruction as a node. |
| `Interpreter` | Runs a node or an interrupt sequence over a `Bus` the host implements, and returns the cycles. |

## Using it

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
measured cycle tables. The interpreter's own translation unit includes neither.

## See also

- [docs/ir.md](../../docs/ir.md) — the full page: the two layers, the
  vocabulary, every rule the effects follow, the cost, and how the lift is held to
  the core.
- [`../cpu65816/`](../cpu65816/README.md) — the disassembler whose listing the
  lift reads.
- [`../disasm/`](../disasm/README.md) — the framework that shapes the listing.
