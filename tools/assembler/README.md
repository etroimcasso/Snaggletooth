# The assembler

`assembler.h` is the part of an assembler that does not depend on which chip the
code is for: the common layer of the assembly language in
[docs/assembly-lexicon.md](../../docs/assembly-lexicon.md) — lines, comments,
numbers, character and string literals, labels, `EQU`, expressions, `ORG` and
the data directives, the second pass that resolves forward references, and the
diagnostics. A chip's assembler is a dialect over it, beside that chip's
disassembler, built from the same instruction table.

Assembly is absolute: every byte's address is known while it is assembled, and
the result is a set of address ranges with bytes.

## Surface

Everything lives in `snaggletooth::assembler`.

| Symbol | Purpose |
|---|---|
| `Assembly` | The result: `ranges` in address order, `symbols`, `errors`, and `ok()`. |
| `Range` | A run of bytes and the address of its first. |
| `Diagnostic` | A file, a line counted from 1, and what was expected. |
| `Dialect` | The interface a chip's assembler implements. |
| `Evaluator` | What a dialect evaluates the expressions in its operands with. |
| `assemble(dialect, source, file)` | Assembles a file under a dialect. |
| `image(assembly, base, size, fill)` | The ranges laid into one buffer. |
| `compact`, `expressionEnd`, `fits`, `hex`, `upper` | What a dialect builds its syntax and diagnostics on. |

`asm_main.h` is what the two command lines share: the arguments, the file in,
the diagnostics out, the image written. Each tool's `main` is one call to
`assemblerMain` with its dialect.

## Using it

```cpp
#include "assembler/assembler.h"
#include "cpu65816_asm.h"

snaggletooth::assembler::Cpu65816Dialect dialect;
const snaggletooth::assembler::Assembly assembly =
    snaggletooth::assembler::assemble(dialect, sourceText, "bank_00.asm");
```

The library target is `snaggletooth_assembler`; `tools/` is on its public
include path. It has no command line of its own — each dialect's tool is where it
is driven from.

## See also

- [docs/assemblers.md](../../docs/assemblers.md) — the full page: the command
  lines, what is written, the diagnostics, the 65816's widths, writing a dialect.
- [`../spc700/`](../spc700/README.md) and [`../cpu65816/`](../cpu65816/README.md) —
  the two dialects built over it.
