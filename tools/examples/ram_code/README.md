# ram_code

One bank, in emulation mode until its last few instructions. Reset writes a
two-instruction routine — `INC A` and `RTL` — into work RAM at `$7E:2000`,
calls it there with `JSL`, and stores the result. It then rewrites the
routine's first byte to `DEC A`, points the direct register at the DMA channels
with `PEA`/`PLD`, builds two return frames on the stack and enters the routine
with `RTL`; the routine's own `RTL` lands on a store at `$802C` that no
instruction names a path to. That store heads a loop run twice: each pass
stores through the direct page — `DMAP0` the first time, `DMAP1` the second —
moves the direct register on by `$10`, and lands past a `PEA`/`RTS` pair. After
the loop a direct-page store lands on `BBAD2` under the one direct register the
run saw there, an indexed store sits beside it, and reset goes native, builds a
frame whose status byte has both widths sixteen bits, and returns through `RTI`
into the stop.

What it shows: the trace never reaches the routine, since it does not exist in
the image, so the lifted program has no node for `$7E:2000` or `$7E:2001`; a
replay counts each instruction run there as unlifted, realigns the interpreter
to the machine afterwards, and checks the stores that follow. The run beside
the interpreter lifts the routine from the bytes the CPU fetched — two nodes at
`$7E:2000`, one per byte the program put there — records the `RTL` into work
RAM as a note, and records three landings the trace then starts from: the
routine's return to `$802C`, the `RTS` to `$803A` seen on both passes and
written once, and the `RTI` to `$804D` arriving with both widths sixteen bits
while the `RTI` itself ran with both eight. It records the direct registers
each site ran under — two at `$802C`, one at `$803F` — and names the register
the store at `$803F` lands on under that one value, while the store under two
values, the indexed store, and the store the paths already prove get no such
name.

Read by `tests/ir/differential_test.cpp`, `tests/rom/observe_test.cpp` and
`tests/rom/rom_disasm_test.cpp`.
