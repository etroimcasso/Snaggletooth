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
- [Status](#status)
- [See also](#see-also)

## Surface

Everything lives in `snaggletooth::disasm`.

| Symbol | Purpose |
|---|---|
| `Address` | A 24-bit address: the bank in bits 16-23, the offset within it below. |
| `Context` | The state a trace carries beside an address. One `std::uint32_t` whose meaning belongs to the backend; the framework only compares it. |
| `Backend` | The interface a chip's disassembler implements — see [Writing a backend](#writing-a-backend). |
| `Request` | What to disassemble: the image, its base address, the entry points, an optional prior image, symbols, and the context every entry starts with. |
| `trace(backend, request)` | Follows control flow and returns a `Listing`. |
| `render(listing)` | Renders a `Listing` as source text. |
| `formatAddress(address, bits)` | An address as a listing prints it: `$XXXX` at 16 bits, `$BB:XXXX` at 24. |

`Instruction`, `Decoded`, `Line`, `Listing`, `Flow` and `CycleCost` are the value
types those pass around. An `Instruction` carries its `address`, `opcode`, `length`,
`flow`, `cycles`, raw `bytes`, rendered `text`, an optional `target`, an optional
`operandAddress`, and a `note`. `Flow` is how an instruction reaches the rest of the
program: `Continue` falls through, `Branch` falls through and may also reach its
target, `Jump` never falls through, `Call` reaches its target and comes back,
`Return` and `Halt` never fall through.

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

Two members have defaults. `following(at, length)` is the address of the next
instruction, and wraps within the bank, which is what every chip built so far does.
`describe(context)` is how a warning names a context; the default prints its bits.

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

Tracing starts at every address in `entries`, each with `request.context`; with no
entries it starts at `base`. `symbols` are labels that take precedence over the
generated ones. `annotateRegisters` turns the register annotation off.

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
warning names the address and both contexts:

```
; warning: $00:8008 is reached with context 1 and with context 0
```

Reaching an address twice under the same context is a loop and is not reported.

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
so the costs stay aligned.

Labels are generated for every target the trace reaches — `loc_` for a branch or
jump destination, `sub_` for a call destination, `entry_` for an address you passed,
`entry` for the base when no entries are given.

## Warnings

`Listing::warnings` is non-empty when the trace found something it could not resolve
cleanly — an entry outside the image, an instruction whose operands run past the end,
a target landing inside an instruction already decoded, or an address reached under
two contexts. `render` prints them at the top as comments. They mean the listing is
incomplete or that a region is being read two ways, not that the tool failed.

## Status

One backend exists: the SPC700, described in
[spc700-disassembler.md](spc700-disassembler.md). The 65816 backend, whole-ROM
disassembly with the cartridge map, and the assembler that closes the round trip are
not built yet.

## See also

- [spc700-disassembler.md](spc700-disassembler.md) — the SPC700 backend and its command line.
- [spc700-assembly.md](spc700-assembly.md) — the dialect that backend emits.
