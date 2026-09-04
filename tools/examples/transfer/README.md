# transfer

One bank, in emulation mode. Reset sets channel 0 up to move sixteen bytes from
the image at `$8100` into work RAM through the data port at `$7E:1000`, starts
the transfer, runs two `NOP`s — the engine engages inside the first — reads the
first byte back from work RAM, and stops.

What it shows: a transfer's bytes are the engine's accesses, not the CPU's, so
the replay leaves them out of what it holds the interpreter to; the CPU's own
cycles for the instruction the engine interrupted are still its own; and the
value the engine moved is what the CPU then reads.

Read by `tests/ir/differential_test.cpp`.
