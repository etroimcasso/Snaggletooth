# ram_code

One bank. Reset writes a two-instruction routine — `INC A` and `RTL` — into
work RAM at `$7E:2000`, calls it there with `JSL`, stores the result, and stops.

What it shows: the trace never reaches the routine, since it does not exist in
the image, so the lifted program has no node for `$7E:2000` or `$7E:2001`; a
replay counts each instruction run there as unlifted, realigns the interpreter
to the machine afterwards, and checks the store that follows.

Read by `tests/ir/differential_test.cpp`.
