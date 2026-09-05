# proving

One LoROM bank. Reset proves each register the dataflow follows the way real
code does: `LDX #$1FFF`/`TXS` for the stack pointer, `LDA #$4300`/`TCD` for the
direct register, `PEA $7E7E`/`PLB`/`PLB` and `PHK`/`PLB` for the data bank. With
the direct register on the DMA channels it writes channel 0 through the direct
page — `STA $00`, `STA $01` — calls a routine at `$00:8100` that saves and
restores the accumulator around a write of its own, and writes `STA $02` with
the value the call gave back. It then dispatches through three tables: at
`$00:8033` an index a mask bounds to eight values, at `$00:83A9` an index a
compare and a branch on the carry bound to three, and at `$00:83B1` an index
nothing bounds. Two of the first table's targets set the direct register to
`$0100` and `$0200` and meet at `$00:8380`; the other six set it to `$0000` and
five of them agree at `$00:8390`, while the last breaks to the software-interrupt
vector with `BRK` before it stores. Of the second table's targets, one calls a
routine at `$00:8120` that jumps through a pointer in work RAM, one calls the
saving routine again with `$80` in the accumulator and writes the screen with
what comes back, and one jumps straight to `$00:8500`, the label after the
unfollowable call. The NMI handler saves and restores the data bank and writes
the screen through the direct page.

What it shows: `state` lines proving the direct register, the data bank and the
stack pointer at labels, a disagreement reported as the values the paths carry,
a label after a call the analysis cannot follow with no line at all and none
after a `BRK`, a routine two callers enter with different accumulators giving
each its own back; `derived`
lines for the eleven destinations the two bounded tables name, the third table's
`stop` standing; `access` lines for direct-page operands under a proven direct
register, and a value proven across a call.

Read by `tests/ir/dataflow_test.cpp`; shown in
[docs/project-manifest.md](../../../docs/project-manifest.md) and
[docs/snes-disassembler.md](../../../docs/snes-disassembler.md).
