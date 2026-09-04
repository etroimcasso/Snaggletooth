# stalled_jump

One bank. Reset sets channel 0 up for a 4096-byte transfer from the image into
the work-RAM data port, starts it, and takes `JMP (!$8100)` on the very next
instruction.

What it shows: the transfer engages inside the instruction after the one that
started it and holds the CPU off the bus, so a step of the machine that meets
the jump may run no instruction at all before the jump does; the run records
the jump on the step that runs it.

Read by `tests/rom/observe_test.cpp`.
