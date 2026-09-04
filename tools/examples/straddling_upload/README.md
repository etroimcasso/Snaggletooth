# straddling_upload

The `uploading` cartridge with two edits that put SPC700 instructions across
the edges of what was uploaded: the program's `STOP` becomes `MOV A,#`, whose
operand is the table's first byte, so one instruction spans the two uploaded
blocks; and the table's second byte branches to its last, which is `MOV A,#`
again, whose operand lies past the end of the upload.

What it shows: an instruction that straddles two blocks, or runs past the last
uploaded byte, is kept whole in the sound program's file and the bytes it
overhangs are written as data — the tree still assembles to the image.

Read by `tests/rom/verify_test.cpp`.
