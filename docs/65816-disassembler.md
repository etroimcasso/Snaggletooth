# 65816 disassembler

`cpu65816_disasm` turns a block of 65816 code into assembly source. It reads a raw
image with a 24-bit load address, so it serves one bank of a cartridge, a block
copied into work RAM, or a whole mapped image alike.

Three properties shape everything below.

**It traces, it does not sweep.** A byte is disassembled as an instruction only when
execution can reach it, following control flow from the entry points you give. Bytes
nothing reaches are emitted as data.

**It carries the register widths along every path.** The accumulator and the index
registers are each 8 or 16 bits wide under two flags, and an immediate operand is as
wide as the register it loads — so `LDA #` is two bytes or three depending on every
`REP` and `SEP` on the path that reached it. The disassembler moves the widths through
the instructions that change them, reports an address that two paths read two ways,
and where a path reaches an immediate under a width nothing settled, says so and
stops. It never guesses a width.

**Its cycle costs come from the interpreter.** Each opcode's cost is measured by
running the core over a synthetic bus under every setting of the mode flags, so a
listing and the emulator cannot disagree about what an instruction costs under the
flags at that address.

The output is assembly source, not a report about the bytes — see
[65816 assembly language](65816-assembly.md) for the dialect.

---

## Contents

- [Command line](#command-line)
- [Entry points and the mode they start in](#entry-points-and-the-mode-they-start-in)
- [Output](#output)
- [How the widths are followed](#how-the-widths-are-followed)
- [What is reported rather than guessed](#what-is-reported-rather-than-guessed)
- [Cycle costs](#cycle-costs)
- [Hardware registers](#hardware-registers)
- [Patched bytes](#patched-bytes)
- [Library](#library)
- [Warnings](#warnings)
- [Status](#status)
- [See also](#see-also)

## Command line

```
cpu65816_disasm <image> [--base ADDR] [--entry ADDR]... [--offset N] [--length N]
                        [--native [--a16] [--x16] | --native --widths-unknown]
                        [--prior <image> [--prior-offset N]] [-o <out>]
```

| Option | Meaning |
|---|---|
| `--base ADDR` | The 24-bit address `image[0]` occupies. Defaults to `$00:0000`. |
| `--entry ADDR` | An address to trace from. Repeatable. With none given, tracing starts at `--base`. |
| `--offset N` | Skip `N` bytes of the file before the image starts. |
| `--length N` | Use only `N` bytes. Zero, the default, means to the end of the file. |
| `--native` | Every entry starts in native mode rather than emulation mode. |
| `--a16`, `--x16` | With `--native`: the accumulator, or the index registers, start sixteen bits wide. |
| `--widths-unknown` | With `--native`: neither width is known at the entries. |
| `--prior FILE` | The same region before the code ran — see [Patched bytes](#patched-bytes). |
| `--prior-offset N` | Where that region starts inside `--prior`. |
| `-o FILE` | Write to a file instead of standard output. |

Numbers are decimal, or hexadecimal behind `0x` or `$`.

The first bank of a LoROM cartridge, traced from its reset vector at `$00:8000`:

```
cpu65816_disasm game.sfc --length 0x8000 --base 0x008000 --entry 0x008000
```

The same bank's vertical-blank handler, whose widths at entry the image cannot say:

```
cpu65816_disasm game.sfc --length 0x8000 --base 0x008000 --entry 0x00816A --native --widths-unknown
```

## Entry points and the mode they start in

An address passed to `--entry` is asserted to be an instruction, and the trace
believes it — so a wrong entry produces a confident disassembly of whatever is there.
When a listing opens with instructions that make no sense as a prologue, suspect the
entry before suspecting the disassembler.

Every entry also starts in a mode, and the mode decides how the bytes read. The reset
vector is entered in emulation mode with both widths eight, which is the default. An
interrupt handler is entered in native mode with whatever widths the interrupted code
had, which is nothing the image can say: `--native --widths-unknown` starts it that
way, and the trace reads every instruction whose length does not depend on a width,
follows the `REP` or `SEP` a handler nearly always opens with, and stops at the first
immediate it would otherwise have to guess.

## Output

```
entry_008000:
        A8
        X8
        SEI                             ; $00:8000  78           2
        STZ !$4200                      ; $00:8001  9C 00 42     4  NMITIMEN
        STZ !$420C                      ; $00:8004  9C 0C 42     4  HDMAEN
        LDA #$80                        ; $00:8016  A9 80        2
        STA !$2100                      ; $00:8018  8D 00 21     4  INIDISP
        CLC                             ; $00:801B  18           2
        XCE                             ; $00:801C  FB           2
        REP #$38                        ; $00:801D  C2 38        3
        LDA #$0000                      ; $00:801F  A9 00 00     3
        TCD                             ; $00:8022  5B           2
        LDX #$017D                      ; $00:802E  A2 7D 01     3

loc_008034:
        LDA #$008D                      ; $00:8034  A9 8D 00     3
        STA $7F:8002,X                  ; $00:8037  9F 02 80 7F  6
        DEX                             ; $00:8045  CA           2
        BPL $00:8034                    ; $00:8048  10 EA        2/3
```

Each line carries the mnemonic, then a comment holding the address, the raw bytes,
the cycle cost, and any annotation. Everything that is not an instruction or a
directive is a comment, which is what lets the listing assemble back to the bytes it
came from.

`A8` and `X8` are the width directives an assembler needs. They appear at the start
of every region and wherever the trace read an instruction under a width the
instruction above did not leave — nowhere else, because a `REP` or `SEP` already says
its own change. `2/3` on the branch is the conditional cost: two cycles when the
condition fails, three when it holds.

Labels are generated for every target the trace reaches — `loc_` for a branch or
jump destination, `sub_` for a call destination, `entry_` for an address you passed.
Supply your own through `Request::symbols` and they take precedence.

## How the widths are followed

The trace carries the emulation flag and the two widths beside every address, and
moves them the way the hardware does:

- `REP` clears the widths its mask names and `SEP` sets them. In emulation mode
  neither moves a width, since emulation holds both at eight.
- `XCE` exchanges the carry with the emulation flag. The carry is known when the
  instruction before is `CLC` or `SEC` — the idiom every reset handler uses — and
  then the mode after `XCE` is settled: entering emulation forces both widths to
  eight, leaving it keeps them where emulation held them. With any other instruction
  before it the carry is unknown, the mode is kept, and the line says so.
- `PLP` and `RTI` load the status byte from the stack, which the image cannot say
  anything about. In native mode both widths become unknown until a `REP` or `SEP`
  settles them; in emulation mode they stay forced.

## What is reported rather than guessed

Two things stop the disassembler, and both are written into the listing as warnings
rather than resolved by a choice it has no basis for.

**An immediate under an unknown width.** The bytes cannot be read, so the path stops
there and they stay data:

```
; warning: $00:8001 cannot be read: LDA # under an accumulator width the trace does not know

entry:
        PLP                             ; $00:8000  28        4

; ---- 3 bytes execution did not reach
        DB $A9,$12,$60                  ; $00:8001  |..`|
```

**An address two paths reach with different widths.** The first reading is kept and
the warning names both, so the reader can decide:

```
; warning: $00:98E1 is reached with e=0 m=8 x=8 and with e=0 m=16 x=8
```

The listing carries the reading it kept, and a width directive where that reading
differs from what the instruction above left — here a `REP #$20` reached by a branch
under an 8-bit accumulator and by fall-through under a 16-bit one, which reads the
same either way and settles the width itself on the next line:

```
        CLC                             ; $00:98E0  18           2

loc_0098E1:
        A8
        REP #$20                        ; $00:98E1  C2 20        3
        LDA #$0004                      ; $00:98E3  A9 04 00     3
```

`e`, `m` and `x` in a warning are the emulation flag and the two widths, `?` a width
the trace does not know.

## Cycle costs

Every cost is measured by running the core, under each of the five settings of the
flags that read bytes differently: native mode under each pair of widths, and
emulation mode. The listing prints the cost under the flags at that address, so
`LDA !$1234` costs 4 under an 8-bit accumulator and 5 under a 16-bit one, and the
same line of source costs what the hardware charges there.

The measurement assumes what the datasheet's own cycle table assumes: a direct
register whose low byte is zero, and no index addition carrying across a page. Where
the code runs with a direct register set otherwise, a direct-page instruction costs
one more than printed; where an index crosses a page on an 8-bit index read, that
instruction costs one more; and a taken branch that crosses a page in emulation mode
costs one more. None of those depend on the bytes alone, so none are printed.

Where a width is unknown and the cost depends on it, the listing prints `?` instead of
a number:

```
        PLP                             ; $00:8000  28        4
        LDA $12                         ; $00:8001  A5 12     ?
        RTS                             ; $00:8003  60        6
```

`cpu65816CycleTable(emulation, accumulator8, index8)` exposes each measured table.

## Hardware registers

The console's registers sit at `$2100`–`$21FF` and `$4000`–`$43FF` of banks
`$00`–`$3F` and `$80`–`$BF`; the same offsets in any other bank are memory. An
operand that lands on one is annotated with the register's name from the documented
register table — `INIDISP` through `STAT78` for the picture, `APUIO0`–`APUIO3`,
`WMDATA`–`WMADDH`, `NMITIMEN` through `JOY4H`, and `DMAP0`–`UNUSED7` for the eight
DMA channels:

```
        STA !$2100                      ; $00:8000  8D 00 21     4  INIDISP
        STA !$4300,X                    ; $00:8003  9D 00 43     5  DMAP0
        LDA $3F:4210                    ; $00:8006  AF 10 42 3F  5  RDNMI
        LDA $12                         ; $00:800A  A5 12        3
```

Which operands are annotated follows from what the image can know. A long operand
names its own bank and is annotated by it. An absolute operand names an offset in
the data bank, which the image cannot say; it is annotated as though in bank zero,
since the register-visible banks are where the registers are reached from. A
direct-page operand is never annotated, because it is an offset from the direct
register, which the image cannot say either. Immediates and branch targets are
values and code, never registers. `$4016` carries both of its names, `JOYSER0` read
and `JOYOUT` written.

`cpu65816RegisterName(address)` exposes the same mapping over a 24-bit address.

## Patched bytes

`--prior` names the same memory region before the code ran. Any byte that differs is
called out on the line carrying it, with the bytes it held before. Self-modifying
code is otherwise invisible: the image reads as though it had always held those
bytes, and the instruction that wrote them is somewhere else entirely.

## Library

The tool is a thin wrapper over `tools/cpu65816/cpu65816_disasm.h`, which is the
65816 backend over the [disassembly framework](disassembly-framework.md):

```cpp
#include "cpu65816_disasm.h"

using namespace snaggletooth::disasm;
Request request;
request.image = bytes;          // std::span<const std::uint8_t>
request.base = 0x008000;
request.entries = {0x008000, 0x00816A};
request.entryContexts = {contextOf(Cpu65816Mode::reset()),
                         contextOf(Cpu65816Mode::nativeUnknown())};

const Listing listing = trace(cpu65816Backend(), request);
std::string text = render(listing);
```

| Symbol | Purpose |
|---|---|
| `Cpu65816Mode` | The state the context carries: the emulation flag, the two widths and whether each is known, and one instruction's memory of the carry. `reset()`, `native(accumulator8, index8)` and `nativeUnknown()` build the three starting modes. |
| `contextOf(mode)`, `modeOf(context)` | A mode as the framework carries it, and back. |
| `Cpu65816Backend`, `cpu65816Backend()` | The backend, and the one instance of it; it holds no state. |
| `decodeAt(image, base, address, mode)` | Decodes one instruction under a mode. Empty when the address is outside the image, the operands run past its end, or the mode does not settle the operand's width. |
| `cpu65816CycleTable(emulation, accumulator8, index8)` | The measured cost of all 256 opcodes under one setting of the flags. |
| `cpu65816RegisterName(address)` | A hardware register's name at a 24-bit address, or empty. |

`Listing`, `Instruction`, `Flow` and `CycleCost` are the framework's types, described
on its page. A `Line` of code carries the context it was decoded under and the
width directives printed before it.

## Warnings

`Listing::warnings` is non-empty when the trace found something it could not resolve
cleanly — an entry outside the image, an instruction whose operands run past the end,
an immediate under a width it does not know, a target landing inside an instruction
already decoded, or an address reached under two widths. `render` prints them at the
top as comments. They mean the listing is incomplete or that a region is being read
two ways, not that the tool failed.

## Status

The disassembler reads one image at a time — a bank carved out of a cartridge, or a
region of RAM. Reading a whole cartridge, with every bank mapped through its header,
control flow carried across banks and the uploaded sound program handed to the SPC700
backend, is the [cartridge disassembler](snes-disassembler.md), built over this one.
The cartridge coprocessors and the assembler that closes the round trip are not
built yet.

Targets that are not constants — `JMP (!abs,X)` through a table, `JML [!abs]`
through a pointer, `JSR (!abs,X)` — are decoded and printed, but the trace cannot
follow them. Code reachable only that way needs an explicit `--entry`.

## See also

- [65816 assembly language](65816-assembly.md) — the dialect this emits, over the
  [common layer](assembly-lexicon.md).
- [65816 CPU core](65816-cpu.md) — the interpreter the cycle costs are measured from,
  and the widths and modes themselves.
- [Disassembly framework](disassembly-framework.md) — the tracer and renderer this
  backend runs on, and the cartridge entry points.
- [Cartridge disassembler](snes-disassembler.md) — a whole cartridge traced
  through this backend, bank by bank, into a source tree.
- [SPC700 disassembler](spc700-disassembler.md) — the audio CPU's disassembler over
  the same framework.
