# uploading

One bank whose reset code speaks the audio upload protocol: it waits for the
audio unit's ready bytes on ports 0 and 1, sends a 24-byte SPC700 program to
`$0200` one acknowledged byte at a time, sends a 20-byte table to `$0218` the
same way from another place in the image, starts the program, and jumps past
both tables to stop. The program sits in the image at `$80A0` and the table at
`$8100`, so the two uploads land end to end in audio memory but come from two
places.

What it shows: the sound program captured from the machine rather than read
out of the upload loop, each uploaded block matched back to the image bytes it
came from, the bank file leaving those ranges to the sound program's file, and
a bank file whose last piece — after the ranges the sound program takes — holds
a line its first piece names.

Read by `tests/rom/rom_disasm_test.cpp` and `tests/rom/verify_test.cpp`; the
`straddling_upload` cartridge is this one with two edits.
