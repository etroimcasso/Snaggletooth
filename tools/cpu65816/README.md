# The 65816 disassembler and assembler

`cpu65816_disasm` turns a block of 65816 code into assembly source, and
`cpu65816_asm` turns that source back into bytes. The disassembler reads a raw
image with a 24-bit load address, so it serves one bank of a cartridge, a block
copied into work RAM, or a whole mapped image alike. The source it emits, and the
assembler reads, is the dialect in [65816-assembly.md](../../docs/65816-assembly.md).

It is the 65816 backend over the [disassembly framework](../disasm/README.md), and
it carries the one thing no other chip in the machine has: an instruction's length
depends on state. The accumulator and the index registers are each 8 or 16 bits
wide under two flags, and an immediate operand is as wide as the register it loads.
The backend carries those flags in the trace context, moves them through `REP`,
`SEP` and `XCE`, and where a path reaches an immediate under a width it does not
know, says so and stops rather than guess.

## Contents

- [Command line](#command-line)
- [Output](#output)
- [Library](#library)
- [See also](#see-also)

## Command line

```
cpu65816_disasm <image> [--base ADDR] [--entry ADDR]... [--offset N] [--length N]
                        [--native [--a16] [--x16] | --native --widths-unknown]
                        [--prior <image> [--prior-offset N]] [-o <out>]
```

The first bank of a LoROM cartridge, traced from its reset vector:

```
cpu65816_disasm game.sfc --length 0x8000 --base 0x008000 --entry 0x008000
```

Every entry starts in emulation mode with both widths eight — the reset vector's
mode — unless `--native` says otherwise. An interrupt handler starts in native mode
with whatever widths the interrupted code had, which is nothing the image can say:
`--native --widths-unknown` starts it that way and the trace stops at the first
operand whose width it would have to guess.

```
cpu65816_asm <source.asm> -o <out.bin> [--base ADDR] [--size N] [--fill BYTE]
```

Assembling a bank file back into the 32 KB it came from:

```
cpu65816_asm bank_00.asm -o bank_00.bin
```

## Output

```
entry_008000:
        EMULATION
        SEI                             ; $00:8000  78           2
        STZ !$4200                      ; $00:8001  9C 00 42     4  NMITIMEN
        LDA #$80                        ; $00:8016  A9 80        2
        STA !$2100                      ; $00:8018  8D 00 21     4  INIDISP
        CLC                             ; $00:801B  18           2
        XCE                             ; $00:801C  FB           2
        REP #$38                        ; $00:801D  C2 38        3
        LDA #$0000                      ; $00:801F  A9 00 00     3
```

`EMULATION`, `A8` and `X8` are the directives an assembler needs; they appear at
the start of every region and wherever the trace reads an instruction under a mode
the instruction above did not leave. The cost column is measured under the flags at
that address, and prints `?` where a width is unknown and the cost depends on it.

## Library

Both tools are thin wrappers over headers in target `snaggletooth_cpu65816`, whose
directory is on its own include path. The disassembler is `cpu65816_disasm.h`:

```cpp
#include "cpu65816_disasm.h"

using namespace snaggletooth::disasm;
Request request;
request.image = bytes;
request.base = 0x008000;
request.entries = {0x008000, 0x00816A};
request.entryContexts = {contextOf(Cpu65816Mode::reset()),
                         contextOf(Cpu65816Mode::nativeUnknown())};

const Listing listing = trace(cpu65816Backend(), request);
std::string text = render(listing);
```

`Cpu65816Mode` is the state the context carries, with `contextOf` and `modeOf` to
pack and unpack it and `cpu65816ModeAfter` to move it through an instruction;
`decodeAt` decodes a single instruction under a mode; `cpu65816CycleTable` is the
measured cost of all 256 opcodes under one setting of the flags;
`cpu65816RegisterName` names the registers in the banks that show them;
`cpu65816Opcodes()` is the instruction table itself, which the assembler is built
from.

The assembler is `cpu65816_asm.h`, the 65816 dialect over the
[assembler library](../assembler/README.md):

```cpp
#include "cpu65816_asm.h"

const snaggletooth::assembler::Assembly assembly =
    snaggletooth::assembler::assembleCpu65816(sourceText, "bank_00.asm");
```

`Cpu65816Dialect` is the dialect, carrying the mode from line to line through the
same `cpu65816ModeAfter` the disassembler uses; `assembleCpu65816` assembles a file
under it.

## See also

- [docs/65816-disassembler.md](../../docs/65816-disassembler.md) — the full page:
  the modes, the conflict and unknown-width reports, the costs and what they assume,
  and the register naming.
- [docs/assemblers.md](../../docs/assemblers.md) — the assembler's page: the
  command line, what is written, the diagnostics, the widths, the library.
- [docs/65816-assembly.md](../../docs/65816-assembly.md) — the dialect they share.
- [docs/65816-cpu.md](../../docs/65816-cpu.md) — the core the costs are measured
  from.
