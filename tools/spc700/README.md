# The SPC700 disassembler and assembler

`spc700_disasm` turns a block of SPC700 memory into assembly source, and
`spc700_asm` turns that source back into bytes. The disassembler reads a raw
image with a load address, so it serves a RAM dump, a driver blob carved out of a
ROM, and the RAM half of an `.spc` equally. The source it emits, and the
assembler reads, is the dialect in
[spc700-assembly.md](../../docs/spc700-assembly.md).

It is the SPC700 backend over the [disassembly framework](../disasm/README.md): it
follows control flow from the entry points you give, so bytes execution cannot
reach are printed as data, and it measures each opcode's cycle cost by running the
core over a synthetic bus, so a listing and the emulator cannot disagree about what
an instruction costs.

## Command line

```
spc700_disasm <image> [--base ADDR] [--entry ADDR]... [--offset N] [--length N]
                      [--prior <image> [--prior-offset N]] [-o <out>]
```

Carving a 4 KB driver out of a ROM and disassembling it at the address it runs at:

```
spc700_disasm game.sfc --offset 0x8000 --length 0x1000 --base 0x0400 --entry 0x0430
```

Numbers are decimal, or hexadecimal behind `0x` or `$`. `--prior` names the same
region before the code ran, and every byte that differs is called out on its line.

```
spc700_asm <source.asm> -o <out.bin> [--base ADDR] [--size N] [--fill BYTE]
```

Assembling the listing back into the 64 KB it came from, and proving it the same:

```
spc700_asm driver.asm -o ram-rebuilt.bin --base 0 --size 65536
cmp ram.bin ram-rebuilt.bin
```

## Library

Both tools are thin wrappers over headers in target `snaggletooth_spc700`, whose
directory is on its own include path. The disassembler is `spc700_disasm.h`:

```cpp
#include "spc700_disasm.h"

snaggletooth::disasm::DisasmRequest request;
request.image = bytes;
request.base = 0x0400;
request.entries = {0x0430};

const auto listing = snaggletooth::disasm::trace(request);
std::string text = snaggletooth::disasm::render(listing);
```

`Spc700Backend` and `spc700Backend()` are the backend and its one instance;
`decodeAt` decodes a single instruction; `cycleTable()` is the measured cost of all
256 opcodes; `registerName` names the hardware registers at `$00F0`–`$00FF`;
`spc700Opcodes()` is the instruction table itself, which the assembler is built
from.

The assembler is `spc700_asm.h`, the SPC700 dialect over the
[assembler library](../assembler/README.md):

```cpp
#include "spc700_asm.h"

const snaggletooth::assembler::Assembly assembly =
    snaggletooth::assembler::assembleSpc700(sourceText, "driver.asm");
```

`Spc700Dialect` is the dialect; `assembleSpc700` assembles a file under it.

## See also

- [docs/spc700-disassembler.md](../../docs/spc700-disassembler.md) — the full page:
  every option, the output format, the register and patched-byte annotations, and
  the warnings.
- [docs/assemblers.md](../../docs/assemblers.md) — the assembler's page: the
  command line, what is written, the diagnostics, the library.
- [docs/spc700-assembly.md](../../docs/spc700-assembly.md) — the dialect they share.
- [docs/spc700-cpu.md](../../docs/spc700-cpu.md) — the core the costs are measured
  from.
