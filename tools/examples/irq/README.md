# irq

One bank, in emulation mode, with the interrupt-disable flag set from power-on.
Reset arms the H/V-timer IRQ at line `$30` and spins for about two frames with
the flag still set — the timer's request latches and the line stays asserted,
but nothing is taken — then waits with `WAI`, which the asserted line ends
without a dispatch, then clears the flag with `CLI`, and the request is taken at
once. The handler acknowledges the timer and counts; after two the program
stops.

What it shows: a maskable request is taken only while the flag is clear, and a
replay that took it earlier would diverge; a wait ended by a masked request
resumes at the next instruction with no sequence run, which the interpreter
must mirror; and the `IRQ` construct, taken and checked.

Read by `tests/ir/differential_test.cpp`.
