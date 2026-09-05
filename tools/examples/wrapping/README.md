# wrapping

Two HiROM banks, in emulation mode — the one HiROM cartridge among the
examples. Reset sends thirty-two bytes from `$C1:FFF0` to VRAM and stops.

A transfer's address steps within its bank, so after `$C1:FFFF` the engine
reads `$C1:0000`: under HiROM that is the image too, the start of the same
bank. The run records one range of thirty-two bytes whose image offsets are two
runs of sixteen, and the asset pass lifts each run as a file of its own —
`vram/C1_FFF0.bin` and `vram/C1_0000.bin` — with `bank_C1.asm` including one at
its end and the other at its start.

What it shows: a range that wraps within its bank lifted as its pieces, under
the map where both pieces are the image; a HiROM tree with a lifted file at a
bank's first byte.

Read by `tests/rom/rom_disasm_test.cpp`.
