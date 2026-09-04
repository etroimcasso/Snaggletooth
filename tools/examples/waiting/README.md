# waiting

One bank, in emulation mode throughout. Reset enables the vertical-blank NMI
and waits for it with `WAI`; the handler counts; after two the program stops.

What it shows: a wait is a halt the interpreter mirrors, a hardware interrupt
is an input the replay supplies between two instructions, and the halted cycles
while the CPU sits are counted rather than checked.

Read by `tests/ir/differential_test.cpp`.
