# Disassembly framework

`tools/disasm/disasm.h` is the part of a tracing disassembler that does not depend on
which chip the bytes are for. A disassembler for a chip is a *backend* over it: the
backend decodes one instruction at an address and says how execution leaves it; the
framework follows control flow from the entry points it is given, keeps track of
what is code and what is not, and renders the result as assemblable source.

Two properties shape everything below.

**It traces, it does not sweep.** A byte is disassembled as an instruction only when
execution can reach it. Bytes nothing reaches are emitted as data. A linear walk
that decodes forward from the first byte turns jump tables, packed data and text
into instructions that read like code and mean nothing.

**It carries a context beside every address.** Some chips decode the same bytes
differently depending on state that changes along the path reaching them — the
65816's operand widths depend on two flags set and cleared by earlier instructions.
A backend keeps that state in the context; the framework propagates it along each
path and, where two paths reach one address with different contexts, reports the
conflict rather than choosing.

---

## Contents

- [Surface](#surface)
- [Writing a backend](#writing-a-backend)
- [Tracing and rendering](#tracing-and-rendering)
- [Addresses](#addresses)
- [Context and conflicts](#context-and-conflicts)
- [Output](#output)
- [Warnings](#warnings)
- [Cartridges](#cartridges)
- [Status](#status)
- [See also](#see-also)

## Surface

Everything lives in `snaggletooth::disasm`.

| Symbol | Purpose |
|---|---|
| `Address` | A 24-bit address: the bank in bits 16-23, the offset within it below. |
| `Context` | The state a trace carries beside an address. One `std::uint32_t` whose meaning belongs to the backend; the framework compares it through the backend. |
| `Backend` | The interface a chip's disassembler implements — see [Writing a backend](#writing-a-backend). |
| `Request` | What to disassemble: the image, its base address, the entry points and the context each starts with, an optional prior image, and symbols. |
| `trace(backend, request)` | Follows control flow and returns a `Listing`. |
| `render(listing)` | Renders a `Listing` as source text. |
| `formatAddress(address, bits)` | An address as a listing prints it: `$XXXX` at 16 bits, `$BB:XXXX` at 24. |

`Instruction`, `Decoded`, `Line`, `Listing`, `Flow` and `CycleCost` are the value
types those pass around. An `Instruction` carries its `address`, `opcode`, `length`,
`flow`, `cycles`, raw `bytes`, rendered `text`, an optional `target`, an optional
`operandAddress`, and a `note`. `Flow` is how an instruction reaches the rest of the
program: `Continue` falls through, `Branch` falls through and may also reach its
target, `Jump` never falls through, `Call` reaches its target and comes back,
`Return` and `Halt` never fall through. A `CycleCost` is a `base` and a `taken`
count, and a `known` flag a backend clears where the cost depends on state the
trace does not have. A `Line` of code carries the `context` it was decoded under
and the `directives` the backend wants printed before it.

## Writing a backend

A backend derives from `Backend` and implements four members:

```cpp
class MyBackend final : public snaggletooth::disasm::Backend {
 public:
  std::string_view name() const override { return "MyChip"; }
  unsigned addressBits() const override { return 24; }

  std::optional<Decoded> decode(std::span<const std::uint8_t> image, Address base,
                                Address at, Context context) const override;

  std::string_view registerName(Address address) const override;
};
```

`decode` reads the instruction at `at` in an image whose first byte occupies `base`,
under `context`, and returns the `Instruction` together with the context execution
carries out of it. It returns nothing when `at` lies outside the image or the
instruction's operand bytes would run past its end — the framework reports that as a
warning and stops that path.

What a backend fills in decides what the framework can do:

- `length` and `flow` drive the trace. `target` is followed when it is set; a
  computed destination is left unset and the trace stops there.
- `next` in the returned `Decoded` is the context for the instruction after this one
  and for its target alike. A backend whose instructions always read the same way
  returns the context it was given.
- `operandAddress` is the memory address the operand names, when there is one. The
  framework passes it to `registerName` and puts the answer in `note`.
- `cycles` and `text` are printed as they are.
Five members have defaults. `unreadable(image, base, at, context)` is why the
bytes at `at` cannot be read under the context, or empty when they can; the
framework asks it before decoding, reports a reason and stops that path, and the
bytes stay data. The default never refuses. `following(at, length)` is the address
of the next instruction, and wraps within the bank, which is what every chip built
so far does. `describe(context)` is how a warning names a context; the default
prints its bits.
`conflicts(first, second)` says whether an address reached under both contexts
reads two ways; the default says so whenever they differ, and a backend that
carries state beyond what decides a reading answers from the bits that do.
`directives(before, now)` is the source lines that must precede an instruction
decoded under `now` when the instruction above left `before` — or when nothing is
above it, at the start of a region; the default is none, and a backend whose
source has to be told the state an instruction assembles under answers with its
directives.

## Tracing and rendering

```cpp
#include "disasm/disasm.h"
#include "spc700_disasm.h"

snaggletooth::disasm::Request request;
request.image = bytes;            // std::span<const std::uint8_t>
request.base = 0x0400;
request.entries = {0x0430};
request.priorImage = pristine;    // optional; same length as image

const auto listing = snaggletooth::disasm::trace(snaggletooth::disasm::spc700Backend(), request);
std::string text = snaggletooth::disasm::render(listing);
```

Tracing starts at every address in `entries`; with no entries it starts at `base`.
`entries[i]` starts with `entryContexts[i]` when one is given there and with
`request.context` otherwise, so a reset handler and an interrupt handler, which
begin in different modes, trace in one request. `symbols` are labels that take
precedence over the generated ones. `annotateRegisters` turns the register
annotation off.

## Addresses

`Address` is 24 bits everywhere. A backend for a chip with a 16-bit address space
reports `addressBits() == 16` and never sets a bank; the listing then prints `$XXXX`
and generates labels such as `loc_0405`. A 24-bit backend's listing prints `$BB:XXXX`
and generates `loc_C02DF1`.

An image is a flat run of bytes at `base`; an entry or a target outside it is
reported and not followed.

## Context and conflicts

The trace's work item is an address *and* a context. The same address under a
different context is a different item, which is how the framework can tell that two
paths read one location two ways. When that happens the first reading is kept and a
warning names the address and both contexts, as the backend describes them:

```
; warning: $00:98E1 is reached with e=0 m=8 x=8 and with e=0 m=16 x=8
```

Whether two contexts read the bytes two ways is the backend's call, through
`conflicts`: a backend may carry state in the context that a reading does not
depend on, and two arrivals differing only in that are not reported.

Reaching an address twice under the same context is a loop and is not reported.

A backend may also answer that it cannot read an address under the context it was
given — the 65816's immediate under a width nothing on the path settled. The path
stops there, the bytes stay data, and a warning says why:

```
; warning: $00:8001 cannot be read: LDA # under an accumulator width the trace does not know
```

## Output

```
        ORG $C0:8000

entry:
        CALL $C0:8005                   ; $C0:8000  03 05 80 C0  6
        NOP                             ; $C0:8004  00           2

sub_C08005:
        RET                             ; $C0:8005  05           6
```

Each line carries the mnemonic, then a comment holding the address, the raw bytes,
the cycle cost, and any annotation. Everything that is not an instruction or a
directive is a comment, which is what lets the listing assemble back to the bytes it
came from. The raw-bytes field is as wide as the longest instruction in the listing,
so the costs stay aligned. A cost the backend marked unknown prints as `?`.

A backend's directives print indented like instructions, after the label and
before the instruction they belong to:

```
entry_008000:
        A8
        X8
        SEI                             ; $00:8000  78           2
```

Labels are generated for every target the trace reaches — `loc_` for a branch or
jump destination, `sub_` for a call destination, `entry_` for an address you passed,
`entry` for the base when no entries are given.

## Warnings

`Listing::warnings` is non-empty when the trace found something it could not resolve
cleanly — an entry outside the image, an instruction whose operands run past the end,
an instruction the backend cannot read under the context that reached it, a target
landing inside an instruction already decoded, or an address reached under two
contexts that read it two ways. `render` prints them at the top as comments. They
mean the listing is incomplete or that a region is being read two ways, not that
the tool failed.

## Cartridges

`tools/rom/cartridge_entries.h`, in the same namespace, is where a trace of a whole
cartridge starts. A cartridge's header names the handlers the CPU jumps to on reset
and on every interrupt — the addresses execution reaches before the program has run
an instruction — and those are the entry points a trace begins from.

```cpp
#include "rom/cartridge_entries.h"
#include "snaggletooth/snes/cartridge.h"

const std::optional<snaggletooth::CartridgeHeader> header =
    snaggletooth::parseCartridgeHeader(image);
for (const snaggletooth::disasm::VectorEntry& entry : snaggletooth::disasm::vectorEntries(*header)) {
  entry.address;  // in bank $00
  entry.name;     // "reset", "nmi", "irq", "cop", "abort"; "_native" on the native set
}
```

`vectorEntries` returns the vectors that land in ROM under the header's map, in
vector-table order with reset first: the emulation-mode set, then the native set. A
vector pointing at RAM or a register is left out, since nothing there can be
traced; two vectors naming one handler give two entries under their own names.

`codeOwner(map, address)` says which disassembler owns the bytes at a bus address:
`CodeOwner::Cpu65816` for cartridge ROM, `CodeOwner::None` for RAM, registers, a
save window and open bus. The map, the header and the address translations behind
both live in [snes-cartridge.md](snes-cartridge.md).

## Status

Two backends exist: the SPC700, described in
[spc700-disassembler.md](spc700-disassembler.md), and the 65816, described in
[65816-disassembler.md](65816-disassembler.md). The cartridge side — the header,
the three maps, and the entry points — is built, and so is whole-cartridge
disassembly over it, which traces every bank from the vectors with control flow
carried across banks, hands the sound program the cartridge uploads to the SPC700
backend, and writes a source tree with a manifest: [snes-disassembler.md](snes-disassembler.md).
The assemblers that close the round trip are built over a framework of their own,
[assemblers.md](assemblers.md), each dialect from the same table as its backend.

## See also

- [spc700-disassembler.md](spc700-disassembler.md) — the SPC700 backend and its command line.
- [65816-disassembler.md](65816-disassembler.md) — the 65816 backend and its command line.
- [assembly-lexicon.md](assembly-lexicon.md) — the layer the two dialects share;
  [spc700-assembly.md](spc700-assembly.md) and [65816-assembly.md](65816-assembly.md)
  are the dialects the backends emit.
- [snes-cartridge.md](snes-cartridge.md) — the cartridge header and maps the entry points come from.
