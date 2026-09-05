# Example cartridges

Sixteen cartridges, each built by hand to do one thing, each in its own
directory with the source that builds it and a README saying what it does and
who reads it. They are the cartridges the tests run the tools on, and the
cartridges every page's example output comes from — a page shows what a tool
prints for a cartridge that is ours to publish, and anyone can produce the same
output by writing the cartridge to disk and running the tool on it.

```
snes_examples <directory>
```

writes every one under the directory as `<name>.smc` and lists them. From there
the toolkit reads them like any cartridge:

```
snes_disasm examples/mixed.smc -o mixed --no-run --no-sound
snes_verify mixed examples/mixed.smc
snes_lift mixed examples/mixed.smc
snes_differential mixed examples/mixed.smc -o mixed/differential --seconds 0.1
```

| Directory | The cartridge |
|---|---|
| [`three_bank/`](three_bank/README.md) | Three banks calling and jumping across each other, and a jump through a table |
| [`uploading/`](uploading/README.md) | Reset speaks the audio upload protocol and sends a sound program |
| [`cop_vector/`](cop_vector/README.md) | The COP and native BRK vectors named beside reset |
| [`straddling_upload/`](straddling_upload/README.md) | The uploading cartridge with two instructions across the upload's edges |
| [`dispatching/`](dispatching/README.md) | Every indirect jump and call form, each to a target nothing else names |
| [`stalled_jump/`](stalled_jump/README.md) | A transfer started on the instruction before an indirect jump |
| [`unreadable_pointer/`](unreadable_pointer/README.md) | An indirect jump whose pointer lies in a register |
| [`button_dispatch/`](button_dispatch/README.md) | Two indirect jumps taken only when a button is down, read both ways |
| [`mixed/`](mixed/README.md) | A spread of the effect layer's constructs, then three vertical-blank interrupts |
| [`waiting/`](waiting/README.md) | A wait for the vertical-blank interrupt, twice |
| [`transfer/`](transfer/README.md) | A transfer into work RAM through the data port, run while the CPU is held |
| [`ram_code/`](ram_code/README.md) | A routine copied into work RAM and called there |
| [`hdma/`](hdma/README.md) | One HDMA write per frame while the program idles |
| [`irq/`](irq/README.md) | A timer request held under the interrupt-disable flag, a wait it ends, then taken |
| [`frame_press/`](frame_press/README.md) | A Start press that counts only on the one frame the tenth interrupt reads |
| [`proving/`](proving/README.md) | Every register proved the way real code proves it, and three jump tables, two of them bounded |

Each directory's header builds its image as a function in
`snaggletooth::examples`; `common.h` holds what they share — a LoROM image with
a valid header, `put` to place bytes in it, and the vector layout the replay
cartridges use — and `example_cartridges.h` includes them all and lists them for
`snes_examples`. A test includes a cartridge's header and calls its function; a
page names the cartridge and shows the tool's output on it. The code in each
image is commented instruction by instruction with its address, so the source
reads like a listing.

## See also

- [`../README.md`](../README.md) — the tools and the libraries behind them.
- [`../../docs/README.md`](../../docs/README.md) — the pages, each with the
  cartridge its examples come from.
