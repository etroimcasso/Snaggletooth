// cpu65816_asm — assembles 65816 source into a flat image.
//
//   cpu65816_asm <source.asm> -o <out.bin> [--base ADDR] [--size N] [--fill BYTE]
//
// The source is the dialect in docs/65816-assembly.md over the common layer in
// docs/assembly-lexicon.md — what cpu65816_disasm emits. The output runs from
// the first byte the source emitted to the last, or is the window --base and
// --size ask for, with gaps holding --fill ($00 by default). Addresses are 24
// bits wide, so a bank's worth of source at $01:8000 writes an image whose first
// byte is that address.

#include "assembler/asm_main.h"
#include "cpu65816_asm.h"

int main(int argc, char** argv) {
  snaggletooth::assembler::Cpu65816Dialect dialect;
  return snaggletooth::assembler::assemblerMain(argc, argv, dialect);
}
