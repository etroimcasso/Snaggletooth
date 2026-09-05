# cop_vector

One bank whose header names the emulation COP vector and the native BRK vector
beside reset. Reset takes each once — `COP` in emulation mode, then `BRK` in
native mode — and stops; each handler is a single `RTI`.

What it shows: `cop` and `brk` are mnemonics, so their handlers cannot carry
the vector's name as a label the way `reset` and `nmi` do — the tree names them
`cop_handler` and `brk_handler`, and the file assembles. Run on the machine, a
software interrupt reaches the vector the header names and its `RTI` returns
after the signature byte: both are flow the instructions name, and the run
records no landing for either.

Read by `tests/rom/verify_test.cpp`, `tests/rom/rom_disasm_test.cpp` and
`tests/rom/observe_test.cpp`.
