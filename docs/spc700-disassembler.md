# SPC700 disassembler

`spc700_disasm` turns a block of SPC700 memory into assembly source. It reads a raw
image with a load address, so it serves a RAM dump, a driver blob carved out of a
ROM, and the RAM half of an `.spc` equally.

Two properties shape everything below.

**It traces, it does not sweep.** A byte is disassembled as an instruction only when
execution can reach it, following control flow from the entry points you give. Bytes
nothing reaches are emitted as data. A linear walk that decodes forward from the
first byte turns jump tables, packed data and text into instructions that read like
code and mean nothing.

**Its cycle costs come from the interpreter.** Each opcode's cost is measured by
running the core over a synthetic bus, so a listing and the emulator cannot disagree
about what an instruction costs. Instructions whose cost depends on a condition are
measured both ways and print as `base/taken`.

The output is assembly source, not a report about the bytes — see
[SPC700 assembly language](spc700-assembly.md) for the dialect.

---

## Command line

```
spc700_disasm <image> [--base ADDR] [--entry ADDR]... [--offset N] [--length N]
                      [--prior <image> [--prior-offset N]] [-o <out>]
```

| Option | Meaning |
|---|---|
| `--base ADDR` | The address `image[0]` occupies. Defaults to `$0000`. |
| `--entry ADDR` | An address to trace from. Repeatable. With none given, tracing starts at `--base`. |
| `--offset N` | Skip `N` bytes of the file before the image starts. |
| `--length N` | Use only `N` bytes. Zero, the default, means to the end of the file. |
| `--prior FILE` | The same region before the code ran — see [Patched bytes](#patched-bytes). |
| `--prior-offset N` | Where that region starts inside `--prior`. |
| `-o FILE` | Write to a file instead of standard output. |

Numbers are decimal, or hexadecimal behind `0x` or `$`.

Carving a 4 KB driver out of a ROM and disassembling it at the address it runs at:

```
spc700_disasm game.sfc --offset 0x8000 --length 0x1000 --base 0x0400 --entry 0x0430
```

## Entry points decide what is code

The single largest influence on output quality. An address passed to `--entry` is
asserted to be an instruction, and the trace believes it — so a wrong entry produces
a confident disassembly of whatever is there.

Pointing it at a string, for instance, yields valid-looking instructions:

```
        CMP A,!$206D                    ; $0402  65 6D 20  4
        TCALL 6                         ; $0405  61        8
```

Those bytes are the text `"m access"`. Nothing about the output says so.

When a listing opens with instructions that make no sense as a prologue, suspect the
entry before suspecting the disassembler. Reasonable first entries are a reset or
jump target, an address a caller reaches, or the start of a region known to be code.

## Output

```
        ORG $0400

entry_0430:
        CLRP                            ; $0430  20        2
        MOV X,#$FF                      ; $0431  CD FF     2  T2OUT
        MOV SP,X                        ; $0433  BD        2
        BEQ $0445                       ; $0434  F0 0F     2/4

; ---- 6 bytes execution did not reach
        DB $00,$11,$00,$6F,$0D,$00      ; $0436  |...o..|
```

Each line carries the mnemonic, then a comment holding the address, the raw bytes,
the cycle cost, and any annotation. Everything that is not an instruction or a
directive is a comment, which is what lets the listing assemble back to the bytes it
came from.

`2/4` on the branch is the conditional cost: two cycles when the condition fails,
four when it holds.

Labels are generated for every target the trace reaches — `loc_` for a branch or
jump destination, `sub_` for a call destination, `entry_` for an address you passed.
Supply your own through `DisasmRequest::symbols` and they take precedence.

## Hardware registers

An operand reaching `$00F0`–`$00FF` is annotated with the register's name:

```
        MOV X,$FD                       ; $0A42  F8 FD     3  T0OUT
        MOV $FA,#$03                    ; $0A1C  8F 03 FA  5
```

`registerName()` exposes the same mapping: `TEST`, `CONTROL`, `DSPADDR`, `DSPDATA`,
`CPUIO0`–`CPUIO3`, `AUXIO4`, `AUXIO5`, `T0TARGET`–`T2TARGET`, `T0OUT`–`T2OUT`.

## Patched bytes

`--prior` names the same memory region before the code ran. Any byte that differs is
called out on the line carrying it:

```
        CALL !$0A7D                     ; $0A35  3F 7D 0A  8  PATCHED at run time, was 35
        MOV Y,$FA+X                     ; $0A8A  FB FA     4  T0TARGET; PATCHED at run time, was 00 11
```

Self-modifying code is otherwise invisible: the image reads as though it had always
held those bytes, and the instruction that wrote them is somewhere else entirely.
Comparing a post-run RAM dump against the image that was uploaded turns every patched
slot into an annotation.

## Library

The tool is a thin wrapper over `tools/spc700/spc700_disasm.h`:

```cpp
#include "spc700_disasm.h"

snaggletooth::disasm::DisasmRequest request;
request.image = bytes;          // std::span<const std::uint8_t>
request.base = 0x0400;
request.entries = {0x0430};
request.priorImage = pristine;  // optional; same length as image

const auto listing = snaggletooth::disasm::trace(request);
std::string text = snaggletooth::disasm::render(listing);
```

| Symbol | Purpose |
|---|---|
| `decodeAt(image, base, address)` | Decodes one instruction. Empty when the address is outside the image or the operands run past its end. |
| `trace(request)` | Follows control flow and returns a `Listing`. |
| `render(listing)` | Renders a `Listing` as source text. |
| `cycleTable()` | The measured cost of all 256 opcodes. |
| `registerName(address)` | A hardware register's name, or empty for ordinary memory. |

`Listing` carries `lines` in address order, the `labels` the trace found, and
`warnings`. An `Instruction` carries its `address`, `opcode`, `length`, `flow`,
`cycles`, raw `bytes`, rendered `text`, an optional `target`, and a `note`.

`Flow` is how an instruction reaches the rest of the program: `Continue` falls
through, `Branch` falls through and may also reach its target, `Jump` never falls
through, `Call` reaches its target and comes back, `Return` and `Halt` never fall
through.

## Warnings

`Listing::warnings` is non-empty when the trace found something it could not resolve
cleanly — an entry outside the image, an instruction whose operands run past the end,
or a target landing inside an instruction already decoded. `render` prints them at
the top as comments. They mean the listing is incomplete or that a region is being
read two ways, not that the tool failed.

## Status

The disassembler covers the SPC700 only. The 65816 and the cartridge coprocessors,
whole-ROM disassembly with bank mapping, and the assembler that closes the round trip
are not built yet.

Targets that are not constants — `JMP [!abs+X]` through a table, a call whose
destination is computed — are decoded and printed, but the trace cannot follow them.
Code reachable only that way needs an explicit `--entry`.

## See also

- [SPC700 assembly language](spc700-assembly.md) — the dialect this emits.
- [SPC700 CPU core](spc700-cpu.md) — the interpreter the cycle costs are measured from.
