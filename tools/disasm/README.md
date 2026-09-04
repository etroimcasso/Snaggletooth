# The disassembly framework

`disasm.h` is the part of a tracing disassembler that does not depend on which chip
the bytes are for. A chip's disassembler is a backend over it: the backend decodes
one instruction at an address and says how execution leaves it; the framework
follows control flow from the entry points it is given, keeps track of what is code
and what is not, and renders the result as assemblable source.

Two properties shape it. **It traces, it does not sweep** — a byte is an instruction
only when execution can reach it, and everything else is emitted as data. **It
carries a context beside every address** — a backend keeps whatever state decides
how bytes read in the context, the framework propagates it along each path, and
where two paths reach one address under contexts that read the bytes two ways it
reports the conflict rather than choosing.

## Contents

- [Surface](#surface)
- [Using it](#using-it)
- [See also](#see-also)

## Surface

Everything lives in `snaggletooth::disasm`.

| Symbol | Purpose |
|---|---|
| `Address` | A 24-bit address: the bank in bits 16-23, the offset below. |
| `Context` | The state a trace carries beside an address. One `std::uint32_t` whose meaning belongs to the backend. |
| `Backend` | The interface a chip's disassembler implements. |
| `Request` | What to disassemble: the image, its base, the entry points and the context each starts with, an optional prior image, and symbols. |
| `trace(backend, request)` | Follows control flow and returns a `Listing`. |
| `render(listing)` | Renders a `Listing` as source text, a target that carries a label written as the label. |
| `formatAddress(address, bits)` | `$XXXX` at 16 bits, `$BB:XXXX` at 24. |

## Using it

```cpp
#include "disasm/disasm.h"
#include "cpu65816_disasm.h"

snaggletooth::disasm::Request request;
request.image = bytes;       // std::span<const std::uint8_t>
request.base = 0x008000;
request.entries = {0x008000};
request.context = snaggletooth::disasm::contextOf(snaggletooth::disasm::Cpu65816Mode::reset());

const auto listing = snaggletooth::disasm::trace(snaggletooth::disasm::cpu65816Backend(), request);
std::string text = snaggletooth::disasm::render(listing);
```

The library target is `snaggletooth_disasm`; `tools/` is on its public include path.
It has no command line of its own — each backend's tool is where the framework is
driven from.

## See also

- [docs/disassembly-framework.md](../../docs/disassembly-framework.md) — the full
  page: writing a backend, the context and conflict rules, the listing format, the
  warnings, and the cartridge entry points.
- [`../spc700/`](../spc700/README.md) and [`../cpu65816/`](../cpu65816/README.md) —
  the two backends built over it.
