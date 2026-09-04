# cop_vector

One bank whose header names the emulation COP vector and the native BRK vector
beside reset. Reset stops at once; each handler is a single `RTI`.

What it shows: `cop` and `brk` are mnemonics, so their handlers cannot carry
the vector's name as a label the way `reset` and `nmi` do — the tree names them
`cop_handler` and `brk_handler`, and the file assembles.

Read by `tests/rom/verify_test.cpp` and `tests/rom/rom_disasm_test.cpp`.
