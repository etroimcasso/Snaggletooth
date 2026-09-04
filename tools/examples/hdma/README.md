# hdma

One bank, in emulation mode. Reset arms HDMA on channel 0 with a direct table
at `$8100` — one write of `$0F` to `$2100` on the first visible line, then
stop — enables the vertical-blank NMI, and idles until the handler has counted
three frames, then stops.

What it shows: an HDMA event that fires while the CPU sits between two
instructions holds it for a whole step, which the replay skips as a step no
instruction ran in; the event's accesses are the engine's, in its name.

Read by `tests/ir/differential_test.cpp` and `tests/snes/observer_test.cpp`.
