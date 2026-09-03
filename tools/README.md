# Tools

The command-line tools that ship with Snaggletooth, and the libraries behind them.
Every tool here is a thin `main` over a library target; the library is the product,
and a front end that wants the same capability links the library rather than
shelling out to the tool.

| Directory | Library | Command line | Page |
|---|---|---|---|
| [`disasm/`](disasm/README.md) | `snaggletooth_disasm` | — | [disassembly-framework.md](../docs/disassembly-framework.md) |
| [`spc700/`](spc700/README.md) | `snaggletooth_spc700` | `spc700_disasm` | [spc700-disassembler.md](../docs/spc700-disassembler.md) |
| [`cpu65816/`](cpu65816/README.md) | `snaggletooth_cpu65816` | `cpu65816_disasm` | [65816-disassembler.md](../docs/65816-disassembler.md) |
| [`rom/`](rom/README.md) | `snaggletooth_rom` | `rom_render` | [spc-rendering.md](../docs/spc-rendering.md), [disassembly-framework.md §Cartridges](../docs/disassembly-framework.md#cartridges) |
| [`spc/`](spc/README.md) | `snaggletooth_spc` | `spc_render` | [spc-rendering.md](../docs/spc-rendering.md) |

`parse_dsp_tables.py` stands apart from the toolkit: it transcribes the S-DSP's
documented constant tables — the Gaussian interpolation table, and the envelope
and noise rate tables — from two public references, cross-checking them entry by
entry, into the generated includes the library compiles. It runs once when a table
changes, never at build time; its docstring carries the usage.

## Building

The tools build whenever Snaggletooth is the top-level project or the tests are on
(`SNAGGLETOOTH_BUILD_TESTS`). A parent build that vendors the library gets none of
them. Each is its own target:

```
cmake --build build --target cpu65816_disasm spc700_disasm rom_render spc_render
```

The binaries land in the build directory's root.

## How the pieces fit

The two disassemblers are backends over one framework. `disasm/` is the part that
does not depend on the chip — it follows control flow from entry points, carries a
context beside every address, and renders assemblable source — and `spc700/` and
`cpu65816/` each decode one instruction set for it. `rom/` reads a cartridge through
the library's own header functions and names the entry points a trace of the whole
cartridge starts from. `spc/` reads and writes the two file formats the renderers
use, an SPC dump in and a WAV out.

The include paths follow the targets. `tools/` is on the public include path of
`snaggletooth_disasm`, `snaggletooth_rom` and `snaggletooth_spc`, so those headers
are included by directory: `disasm/disasm.h`, `rom/cartridge_entries.h`,
`spc/spc_loader.h`. The two backends put their own directory on the path, so their
headers are included bare: `spc700_disasm.h`, `cpu65816_disasm.h`.

## See also

- [docs/README.md](../docs/README.md) — the documentation index, with a page per tool.
- [The project README](../README.md) — what is built and what is not.
