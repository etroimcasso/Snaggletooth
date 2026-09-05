# Tools

The command-line tools that ship with Snaggletooth, and the libraries behind them.
Every tool here is a thin `main` over a library target; the library is the product,
and a front end that wants the same capability links the library rather than
shelling out to the tool.

| Directory | Library | Command line | Page |
|---|---|---|---|
| [`disasm/`](disasm/README.md) | `snaggletooth_disasm` | — | [disassembly-framework.md](../docs/disassembly-framework.md) |
| [`assembler/`](assembler/README.md) | `snaggletooth_assembler` | — | [assemblers.md](../docs/assemblers.md) |
| [`spc700/`](spc700/README.md) | `snaggletooth_spc700` | `spc700_disasm`, `spc700_asm` | [spc700-disassembler.md](../docs/spc700-disassembler.md), [assemblers.md](../docs/assemblers.md) |
| [`cpu65816/`](cpu65816/README.md) | `snaggletooth_cpu65816` | `cpu65816_disasm`, `cpu65816_asm` | [65816-disassembler.md](../docs/65816-disassembler.md), [assemblers.md](../docs/assemblers.md) |
| [`rom/`](rom/README.md) | `snaggletooth_rom` | `snes_disasm`, `snes_verify`, `rom_render` | [snes-disassembler.md](../docs/snes-disassembler.md), [project-manifest.md](../docs/project-manifest.md), [input-script.md](../docs/input-script.md), [spc-rendering.md](../docs/spc-rendering.md) |
| [`ir/`](ir/README.md) | `snaggletooth_ir`, `snaggletooth_ir_differential` | `snes_lift`, `snes_differential` | [ir.md](../docs/ir.md) |
| [`examples/`](examples/README.md) | — (header-only) | `snes_examples` | one README per cartridge |
| [`spc/`](spc/README.md) | `snaggletooth_spc` | `spc_render` | [spc-rendering.md](../docs/spc-rendering.md) |

Two scripts sit beside the libraries. `corpus.py` runs a directory of
cartridges through the commands — the tree written, its program lifted to a
`.snagir` beside it, the tree assembled back into a rebuilt image with the
original's extension and compared with it, and the recorded run replayed beside
the interpreter — one line per image and a verdict, with the corpus-wide
aggregates the manifests carry on request:

```
tools/corpus.py <images> <output> --build build [--no-run] [--seconds N] [--input-dir <scripts>] [--no-lift] [--no-differential] [--facts] [--routines]
```

It exits 0 only when every image is OK, and writes nothing outside `<output>`.
`parse_dsp_tables.py` stands apart from the toolkit: it transcribes the S-DSP's
documented constant tables — the Gaussian interpolation table, and the envelope
and noise rate tables — from two public references, cross-checking them entry by
entry, into the generated includes the library compiles. It runs once when a table
changes, never at build time; its docstring carries the usage.

## Contents

- [Building](#building)
- [How the pieces fit](#how-the-pieces-fit)
- [See also](#see-also)

## Building

The tools build whenever Snaggletooth is the top-level project or the tests are on
(`SNAGGLETOOTH_BUILD_TESTS`). A parent build that vendors the library gets none of
them. Each is its own target:

```
cmake --build build --target snes_disasm snes_verify snes_lift snes_differential snes_examples cpu65816_disasm spc700_disasm cpu65816_asm spc700_asm rom_render spc_render
```

The binaries land in the build directory's root.

## How the pieces fit

The two disassemblers are backends over one framework, and the two assemblers
are dialects over another. `disasm/` is the part of a disassembler that does not
depend on the chip — it follows control flow from entry points, carries a context
beside every address, and renders assemblable source — and `assembler/` is the
part of an assembler that does not: the language's common layer, the two passes,
the diagnostics. `spc700/` and `cpu65816/` each hold one chip's backend and its
dialect, built from one instruction table, so what the disassembler writes the
assembler reads back to the same bytes. `rom/` reads a cartridge through the
library's own header functions, names the entry points a trace of the whole
cartridge starts from, and runs that trace: every bank through the 65816 backend
with control flow carried across banks, the uploaded sound program through the
SPC700 backend, and the result written as a source tree with a manifest — and
it runs the reverse, assembling every file of a tree through the two dialects
and comparing the result with the image. It runs the cartridge on the machine
to find the destinations of the jumps the bytes do not name, and traces from
them, and to record every range of bytes the transfer engines moved — where
from, where to, how many, and from which instruction. It also reports what the
traced code reaches: the hardware register behind every instruction that
touches one, the DMA transfers those add up to, and the routines the
instructions belong to, each with the routines it calls and the hardware it
drives itself and through them.
`ir/` lifts the 65816 listings a cartridge disassembly produces into a form
with no bytes in it — one node per instruction, carrying what source says and
the typed effects the chip performs — runs that form through an interpreter
that never sees the bytes, writes SNES assembly back from it, and replays a
cartridge's recorded run on the machine with the interpreter beside it, so the
representation is held to the core rather than taken on trust. The bank files
`rom/` writes come through that renderer, with the registers, labels and
routines the manifest carries attached as names. `examples/` holds the cartridges built by hand that the
tests run every tool on and every page's example output comes from, one
directory each. `spc/` reads and writes the two file formats the renderers
use, an SPC dump in and a WAV out.

The include paths follow the targets. `tools/` is on the public include path of
`snaggletooth_disasm`, `snaggletooth_assembler`, `snaggletooth_rom`,
`snaggletooth_ir`, `snaggletooth_ir_differential` and `snaggletooth_spc`, so
those headers are included by directory: `disasm/disasm.h`,
`assembler/assembler.h`, `rom/rom_disasm.h`, `rom/rom_verify.h`,
`rom/cartridge_entries.h`, `ir/ir.h`, `ir/cpu65816_lift.h`, `ir/ir_interpret.h`,
`ir/ir_render.h`, `ir/ir_text.h`, `ir/ir_dataflow.h`, `ir/ir_differential.h`,
`examples/example_cartridges.h`, `spc/spc_loader.h`. The two chip libraries put their own directory on the path,
so their headers are included bare: `spc700_disasm.h`, `spc700_asm.h`,
`cpu65816_disasm.h`, `cpu65816_asm.h`; `snaggletooth_rom` links both, so a
program that links it reaches them too.

## See also

- [docs/README.md](../docs/README.md) — the documentation index, with a page per tool.
- [The project README](../README.md) — what is built and what is not.
