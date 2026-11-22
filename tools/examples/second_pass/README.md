# second_pass

Three LoROM banks. Reset in bank `$00` switches to native mode with the accumulator eight bits and the index registers sixteen, and jumps long into bank `$01`. There the code loads X with zero and spins on `BEQ` to itself, so a run never leaves it; past the branch, `JMP (!$8100,X)` reads the table at `$01:8100`, whose one entry leads to `$01:8200`, which narrows the index registers with `SEP #$10` and jumps long into bank `$02`. Bank `$02` reaches `$02:8008` two ways: `BRA` from `$02:8000`, where the trace arrives from bank `$01` with the index registers eight bits, and `BRA` from `$02:8004`, where `JMP (!$8100,X)` at `$02:8012` leads through the table at `$02:8100` with them sixteen. The bytes at `$02:8008` read `LDX #$EA12` under the one width and `LDX #$12` then `NOP` under the other, and both streams go on at `$02:800B`.

What it shows: the order the entries stand in decides the reading, not the order they are found in. The trace carries bank `$02` its entry a turn before the bytes derive `$02:8004`; the derived entry stands first all the same, so `$02:8008` reads `LDX #$EA12` in the first pass and in every pass over the tree after it, and the warning names the derived width first.

Read by `tests/rom/rom_disasm_test.cpp`; shown in [docs/snes-disassembler.md](../../../docs/snes-disassembler.md).
