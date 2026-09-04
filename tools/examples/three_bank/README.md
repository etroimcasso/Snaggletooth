# three_bank

Three LoROM banks. Reset in bank `$00` switches to native mode with both widths
sixteen, calls a routine in bank `$01` with `JSL`, and jumps long into bank `$02`
through its mirror at `$82`. The bank-`$01` routine calls back into bank `$00`,
at `$00:8100`, an address only it reaches. The bank-`$02` code narrows the
accumulator, calls into work RAM at `$7E:2000`, and ends in `JMP (!$8100,X)`, a
jump through a table whose one target is `$02:8100`, where a `BRK` continues at
the vector's handler.

What it shows: a trace carried across banks under the mode each call and jump
was made in, a mirror bank resolved to the bytes it repeats, a call into memory
the image does not hold, and a jump the bytes cannot follow — the trace's `stop`,
which the run answers.

Read by `tests/rom/rom_disasm_test.cpp`, `tests/rom/verify_test.cpp`,
`tests/rom/facts_test.cpp` and `tests/rom/routines_test.cpp`; shown in
[docs/snes-disassembler.md](../../../docs/snes-disassembler.md).
