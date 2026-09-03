// spc700_asm — assembles SPC700 source into a flat image.
//
//   spc700_asm <source.asm> -o <out.bin> [--base ADDR] [--size N] [--fill BYTE]
//
// The source is the dialect in docs/spc700-assembly.md over the common layer in
// docs/assembly-lexicon.md — what spc700_disasm emits. The output runs from the
// first byte the source emitted to the last, or is the window --base and --size
// ask for, with gaps holding --fill ($00 by default).

#include "assembler/asm_main.h"
#include "spc700_asm.h"

int main(int argc, char** argv) {
  snaggletooth::assembler::Spc700Dialect dialect;
  return snaggletooth::assembler::assemblerMain(argc, argv, dialect);
}
