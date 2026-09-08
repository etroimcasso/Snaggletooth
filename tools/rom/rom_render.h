#pragma once

// The bank files, rendered from the program file and the manifest.
//
// This is the back end of the cartridge toolkit. Its input is what the
// disassembler left on disk — `program.snagir`, the whole program in the
// intermediate representation, and `project.manifest`, the facts the run and
// the analysis found — and its output is one 65816 source file per region. It
// never holds the image, the listing the trace produced or the program the
// disassembler lifted: `readRenderInput` builds everything it needs from the two
// files, and `renderRegion` writes a file from that alone. The library links the
// representation, the 65816 backend and the cartridge map, and nothing that can
// trace, run or lift, so a bank file can only have come from the file.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cpu65816/cpu65816_disasm.h"
#include "disasm/disasm.h"
#include "ir/ir.h"
#include "rom/rom_observe.h"
#include "snaggletooth/snes/cartridge.h"

namespace snaggletooth::disasm {

// An inclusive address range.
struct Range {
  Address first = 0;
  Address last = 0;
};

// The lines of `listing` that fall inside `keep`, in order. A data run is cut to
// the parts inside. An instruction is kept when the whole of it is inside one
// range, and a label only when its instruction is, so the cut listing defines
// every label it holds; an instruction that runs past a range's edge is not an
// instruction of the cut listing, and the bytes of it that are inside are kept
// as data, so no byte inside a range goes unwritten.
[[nodiscard]] Listing keepRanges(const Listing& listing, const std::vector<Range>& keep);

// One region of source as the renderer sees it: its file, its range within one
// bank, and its listing — the code lines and the data runs in address order,
// the labels and the warnings.
struct RenderRegion {
  std::string file;
  Address first = 0;
  Address last = 0;  // inclusive
  Listing listing;
};

// A hardware register an instruction reaches, as the manifest's `access` line
// says it: the site, the register's address and its name from the register
// table.
struct RenderAccess {
  Address site = 0;
  Address registerAddress = 0;
  std::string_view name;
};

// The direct registers a run saw at a site, as the `seen` line says.
struct RenderSeen {
  Address address = 0;
  std::vector<std::uint16_t> d;
};

// A routine as the `routine` line says it: where it begins, its label, how many
// lines and bytes it holds, the routines it calls, and its role.
struct RenderRoutine {
  Address address = 0;
  std::string label;
  std::size_t lines = 0;
  std::size_t bytes = 0;
  std::vector<Address> calls;
  std::vector<RegisterClass> reaches;
  std::vector<RegisterClass> through;
};

// A lifted file as the `asset` line says it, with the register its bytes went
// to, which the `moved` and `dma` lines say.
struct RenderAsset {
  std::string file;
  std::vector<RegisterClass> classes;
  MovedKind kind = MovedKind::Dma;
  Address registerAddress = 0;
  Address first = 0;
  std::size_t bytes = 0;
};

// A block of the sound program the image holds, as the `block` line says it.
struct RenderBlock {
  std::size_t romOffset = 0;
  std::size_t bytes = 0;
};

struct RenderSound {
  std::string file;
  std::vector<RenderBlock> blocks;
};

// Everything a bank file is rendered from besides the program: the image's map
// and size, every region, and the facts.
struct RenderInput {
  CartridgeMap map = CartridgeMap::LoRom;
  std::size_t imageBytes = 0;
  std::vector<RenderRegion> regions;
  std::vector<RenderAccess> accesses;  // in site order
  std::vector<RenderSeen> seen;        // in address order
  std::vector<RenderRoutine> routines;
  std::vector<RenderAsset> assets;
  std::optional<RenderSound> sound;
};

// The region's listing with the bytes its file does not write itself left out:
// the sound program's blocks, which are the sound file's, and the lifted files'
// bytes, which are included where they were.
[[nodiscard]] Listing regionLines(const RenderRegion& region, const RenderInput& input);

// A region's source file. The instructions are written from their nodes in
// `program` — the first node at each code line's address — through
// `ir/ir_render.h`, in pieces with an `ORG` where a piece starts, a comment
// where a sound-program block's bytes are left out, and an `INCBIN` where a
// lifted file's bytes were; the data runs and the labels are the listing's. The
// file opens with an `EQU` line for every hardware register its absolute
// operands address and every label another file defines that it refers to, an
// absolute operand that addresses a register is written as the register's name,
// a direct-page operand that every path proves lands on a register carries the
// register's name in its comment — or, where the paths prove nothing and the run
// saw one direct register there, the register that value lands it on, marked
// `(run)` — a target with a label anywhere in the tree is written as the label,
// and each routine that begins in the file carries a comment with its role, what
// it calls and what calls it.
[[nodiscard]] std::string renderRegion(const RenderRegion& region, const RenderInput& input,
                                       const ir::Program& program);

// The renderer's input read from a tree on disk: `program.snagir` for the
// program, the regions, the labels and the data runs, and `project.manifest`
// for the map, the image size, the facts, the lifted files and the sound
// program's blocks. Nothing, with `error` naming the file and the line, when
// either file is missing or does not read.
[[nodiscard]] std::optional<RenderInput> readRenderInput(const std::filesystem::path& directory,
                                                         ir::Program& program, std::string& error);

// Renders every region's file from the tree's program file and manifest and
// writes it under `directory`. False, with `error` set, when the input does not
// read or a file cannot be written. `rendered` counts the files written.
bool renderTree(const std::filesystem::path& directory, std::size_t& rendered, std::string& error);

}  // namespace snaggletooth::disasm
