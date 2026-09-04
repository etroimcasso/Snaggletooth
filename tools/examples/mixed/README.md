# mixed

One bank. Reset switches to native mode and runs a spread of the effect
layer's constructs — both register widths, a direct-page store, a sixteen-bit
read-modify-write, an indexed read that crosses into the next bank, a push and a pull, a call to a routine that
moves two bytes into work RAM with `MVN` and restores the data bank — then
enables the vertical-blank NMI and loops until the handler has counted three,
and stops. The handler, at the native vector, acknowledges the flag and counts;
the emulation vector points at a bare `RTI`, so each handler is traced under one
mode.

What it shows: a run beside the core with no divergence, the construct counts a
replay reports, and a node whose width the trace did not settle — the handler's
— selecting by the live flag. The tests break one effect at a time in its
lifted program to see each break named.

Read by `tests/ir/differential_test.cpp`; shown in
[docs/ir.md](../../../docs/ir.md).
