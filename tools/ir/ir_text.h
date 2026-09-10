#pragma once

// The intermediate representation as text: the program file, `.snagir`.
//
// A program file is one chip's whole program as text that stands alone — the
// image it is a program of, each region of source with its labels and the runs
// of bytes execution never reached, every node with its effects, and, for the
// main CPU, the two hardware interrupt sequences — in the grammar
// `docs/snagir.md` gives record by record: braces open a region, a node's
// effects and each sequence, a semicolon ends every record and every effect,
// `//` opens a comment, and whitespace separates words and means nothing else.
// A tree carries two: `program.snagir`, the main CPU's, and `apu.snagir`, the
// sound program's, whose version record says `apu` and whose addresses are the
// audio unit's sixteen bits. `renderProgram` writes one from a program and what
// the program does not carry; `parseProgram` reads one back into the same
// program, and reading what was written gives the program back equal on every
// field. The names of the vocabulary are written by `opName` and the rest, and
// a flag is qualified by the register it is a bit of — `P.C` — so that the
// register `X` and the flag `X` are two words on the page.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ir/ir.h"

namespace snaggletooth::ir {

[[nodiscard]] std::string_view opName(Op op) noexcept;
[[nodiscard]] std::string_view placeName(Place place) noexcept;
[[nodiscard]] std::string_view widthName(Width width) noexcept;
[[nodiscard]] std::string_view stepName(Step step) noexcept;
[[nodiscard]] std::string_view accessName(Access access) noexcept;
[[nodiscard]] std::string_view whenName(When when) noexcept;
[[nodiscard]] std::string_view flowName(Flow flow) noexcept;

// An addressing mode as source spells its operand — `abs,X`, `(dp),Y`, `#imm(M)`.
[[nodiscard]] std::string_view addressingName(Addressing addressing) noexcept;

// A mode as the text names it: `e=1`, or `e=0 m=8 x=?` with `?` for a width the
// trace did not know.
[[nodiscard]] std::string modeName(const Mode& mode);

// One effect as the file writes it, semicolon included: the operation, its
// destination and operands, the width, the step or the pin where the operation
// has one, and the condition.
[[nodiscard]] std::string renderEffect(const Effect& effect);

// One node as the file writes it inside a region: its address, mnemonic,
// operand, length, flow, target, mode, measured costs and register name, then
// its effects between braces. A sound-CPU node is written with its form word in
// place of an addressing mode, a 16-bit address, no mode and one cost; a
// mnemonic and form that name no opcode throw `std::invalid_argument`.
[[nodiscard]] std::string renderNode(const Node& node, Processor processor = Processor::Cpu65816);

// A label the trace gave an address: the name a branch, jump or call to it is
// written with.
struct ProgramLabel {
  Address address = 0;
  std::string name;
  friend bool operator==(const ProgramLabel&, const ProgramLabel&) = default;
};

// A run of bytes execution never reached, as the bytes are.
struct DataRun {
  Address address = 0;
  std::vector<std::uint8_t> bytes;
  friend bool operator==(const DataRun&, const DataRun&) = default;
};

// A region of source: the file it is written to, the address range it covers
// within one bank — or, for the sound program, within the audio unit's space —
// what the trace could not settle there, and its labels and data runs, each in
// address order. The region's nodes are the program's whose address lies in
// the range.
struct ProgramRegion {
  std::string file;
  Address first = 0;
  Address last = 0;  // inclusive
  std::vector<std::string> warnings;
  std::vector<ProgramLabel> labels;
  std::vector<DataRun> data;
  friend bool operator==(const ProgramRegion&, const ProgramRegion&) = default;
};

// What the file carries that the program does not: the chip the file is
// written for, the image the program is a program of, by its size and its map
// as the manifest names it, and the regions in the order they are written.
struct ProgramFile {
  Processor processor = Processor::Cpu65816;
  std::size_t imageBytes = 0;
  std::string map;
  std::vector<ProgramRegion> regions;
  friend bool operator==(const ProgramFile&, const ProgramFile&) = default;
};

// The program file as text: the main CPU's `nodes` with the interrupt
// sequences, or, for a file whose processor is the sound CPU, the `spc700`
// nodes and no sequence. Every node is written under the region whose range
// holds its address; a node no region holds throws `std::invalid_argument`.
[[nodiscard]] std::string renderProgram(const Program& program, const ProgramFile& file);

// What a program file holds: the main CPU's file fills `nodes`, `nmi` and
// `irq`; the sound program's fills `spc700` and nothing else.
struct Parsed {
  Program program;  // the nodes in address order, whatever order the regions came in
  ProgramFile file;
};

// A program file read back. The mnemonic and the register name of every node
// are the instruction table's and the register table's own storage, so a parsed
// node compares equal to a lifted one. Nothing, with `error` naming the line,
// for a record the grammar does not have, a field it does not have or lacks, a
// name that is not one, a version or a processor this reader does not know, or
// a record out of the order the grammar gives.
[[nodiscard]] std::optional<Parsed> parseProgram(std::string_view text, std::string& error);

// A parsed file cut to the regions written to one source file: those regions
// with their warnings, labels and data runs, the nodes whose addresses they
// hold, and the image line and the interrupt sequences as they were — what
// `snes_lift --file` prints. Nothing when no region is written to `file`.
[[nodiscard]] std::optional<Parsed> selectFile(const Parsed& parsed, std::string_view file);

// What a reader counts over a parsed file, as `snes_lift` and
// `snes_differential` report it: the regions; the code lines, one per address
// a node stands at; the nodes, two where an address is read two ways; the nodes
// whose width is a selection by the live flag; the nodes naming a hardware
// register; the nodes lifted from patched bytes; and the effects. The counts
// are over the file's own chip's nodes; a sound program selects no width.
struct ProgramCounts {
  std::size_t regions = 0;
  std::size_t codeLines = 0;
  std::size_t nodes = 0;
  std::size_t liveWidth = 0;
  std::size_t named = 0;
  std::size_t patched = 0;
  std::size_t effects = 0;
  friend bool operator==(const ProgramCounts&, const ProgramCounts&) = default;
};

[[nodiscard]] ProgramCounts countProgram(const Parsed& parsed);

// Whether two programs are the same program as the files carry one: every
// field equal, the sound program's nodes included, except a width the mode
// does not know — the file writes `?` for it, the node selects by the live
// flag, and no reader of the mode looks at the bit — so a program read back is
// equivalent to the one written even where that bit differs. `==` on the types
// compares the bit as well.
[[nodiscard]] bool equivalent(const Mode& a, const Mode& b) noexcept;
[[nodiscard]] bool equivalent(const Node& a, const Node& b);
[[nodiscard]] bool equivalent(const Program& a, const Program& b);

}  // namespace snaggletooth::ir
