#include "rom/rom_render.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

#include "cpu65816/cpu65816_asm.h"
#include "ir/ir_render.h"
#include "ir/ir_text.h"
#include "rom/rom_disasm.h"
#include "rom/rom_text.h"

namespace snaggletooth::disasm {

using text::address24;
using text::hex;

Listing keepRanges(const Listing& listing, const std::vector<Range>& keep) {
  Listing kept;
  kept.warnings = listing.warnings;
  kept.addressBits = listing.addressBits;
  auto keepData = [&](Address address, const std::vector<std::uint8_t>& data) {
    const Address end = address + static_cast<Address>(data.size()) - 1u;
    for (const Range& range : keep) {
      const Address from = std::max(address, range.first);
      const Address to = std::min(end, range.last);
      if (from > to) continue;
      Line piece;
      piece.isCode = false;
      piece.address = from;
      piece.data.assign(data.begin() + static_cast<std::ptrdiff_t>(from - address),
                        data.begin() + static_cast<std::ptrdiff_t>(to - address) + 1);
      kept.lines.push_back(std::move(piece));
    }
  };
  for (const Line& line : listing.lines) {
    if (line.isCode) {
      const Address end = line.address + line.instruction.length - 1u;
      bool whole = false;
      for (const Range& range : keep) {
        if (line.address >= range.first && end <= range.last) {
          kept.lines.push_back(line);
          if (const auto label = listing.labels.find(line.address); label != listing.labels.end()) {
            kept.labels.insert(*label);
          }
          whole = true;
          break;
        }
      }
      if (!whole) keepData(line.address, line.instruction.bytes);
      continue;
    }
    if (!line.data.empty()) keepData(line.address, line.data);
  }
  return kept;
}

namespace {

// ---- the cuts: what a region's file leaves to other files ------------------------

// The sound program's blocks the image holds, as address ranges within a
// region: the bytes the region's file leaves to the sound program.
std::vector<Range> placedBlockRanges(const RenderInput& input, const RenderRegion& region) {
  std::vector<Range> ranges;
  if (!input.sound) return ranges;
  const std::optional<std::size_t> start = romOffset(input.map, region.first, input.imageBytes);
  if (!start) return ranges;
  const std::size_t length = static_cast<std::size_t>(region.last - region.first) + 1u;
  for (const RenderBlock& block : input.sound->blocks) {
    if (!block.romOffset) continue;  // a block the image does not hold stays in its bank
    const std::size_t from = std::max(*block.romOffset, *start);
    const std::size_t to = std::min(*block.romOffset + block.bytes, *start + length);
    if (from >= to) continue;
    ranges.push_back(Range{.first = region.first + static_cast<Address>(from - *start),
                           .last = region.first + static_cast<Address>(to - *start) - 1u});
  }
  std::sort(ranges.begin(), ranges.end(),
            [](const Range& a, const Range& b) { return a.first < b.first; });
  return ranges;
}

// The region's range with `cut` taken out.
std::vector<Range> without(const RenderRegion& region, const std::vector<Range>& cut) {
  std::vector<Range> keep;
  Address next = region.first;
  for (const Range& range : cut) {
    if (range.first > next) keep.push_back(Range{.first = next, .last = range.first - 1u});
    next = std::max(next, range.last + 1u);
  }
  if (next <= region.last) keep.push_back(Range{.first = next, .last = region.last});
  return keep;
}

// A range of a region's bytes that its file does not write itself: a
// sound-program block's, written in the sound file, or a lifted file's, written
// there and included here. `fileOffset` and `length` are the part of the lifted
// file the region holds — the whole of it unless a file split cuts across it.
struct Cut {
  Range range;
  const RenderAsset* asset = nullptr;  // null for a sound-program block
  std::size_t fileOffset = 0;
  std::size_t length = 0;
};

// Every cut of a region, in address order.
std::vector<Cut> cutsOf(const RenderInput& input, const RenderRegion& region) {
  std::vector<Cut> cuts;
  for (const Range& range : placedBlockRanges(input, region)) {
    cuts.push_back(Cut{.range = range, .asset = nullptr, .fileOffset = 0, .length = 0});
  }
  for (const RenderAsset& asset : input.assets) {
    const Address last = asset.first + static_cast<Address>(asset.bytes) - 1u;
    const Address from = std::max(asset.first, region.first);
    const Address to = std::min(last, region.last);
    if (from > to) continue;
    cuts.push_back(Cut{.range = Range{.first = from, .last = to},
                       .asset = &asset,
                       .fileOffset = from - asset.first,
                       .length = static_cast<std::size_t>(to - from) + 1u});
  }
  std::sort(cuts.begin(), cuts.end(),
            [](const Cut& a, const Cut& b) { return a.range.first < b.range.first; });
  return cuts;
}

std::vector<Range> rangesOf(const std::vector<Cut>& cuts) {
  std::vector<Range> ranges;
  for (const Cut& cut : cuts) ranges.push_back(cut.range);
  return ranges;
}

}  // namespace

Listing regionLines(const RenderRegion& region, const RenderInput& input) {
  return keepRanges(region.listing, without(region, rangesOf(cutsOf(input, region))));
}

namespace {

// ---- a bank file, from the representation -------------------------------------

// Whether `name` can be a symbol of the 65816 dialect: the lexicon's name form,
// and not a mnemonic, a register or a directive.
bool symbolName(std::string_view name) {
  static const assembler::Cpu65816Dialect dialect;
  if (name.empty()) return false;
  auto letter = [](char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == '.';
  };
  if (!letter(name.front())) return false;
  for (const char c : name) {
    if (!letter(c) && !(c >= '0' && c <= '9')) return false;
  }
  const std::string upper = assembler::upper(name);
  return !dialect.reserved(upper) && !assembler::coreDirective(upper);
}

// The register the absolute operand at `site` addresses, from the access facts:
// the fact at the site whose register is the operand's own address. Empty where
// no fact names one.
std::string_view operandRegister(const RenderInput& input, Address site, std::uint32_t operand) {
  const std::vector<RenderAccess>& accesses = input.accesses;
  auto it = std::lower_bound(accesses.begin(), accesses.end(), site,
                             [](const RenderAccess& a, Address wanted) { return a.site < wanted; });
  for (; it != accesses.end() && it->site == site; ++it) {
    if (it->registerAddress == operand) return it->name;
  }
  return {};
}

// The label an address carries in any of the tree's listings, or nothing.
std::optional<std::string> labelAnywhere(const RenderInput& input, Address address) {
  for (const RenderRegion& region : input.regions) {
    const auto found = region.listing.labels.find(address);
    if (found != region.listing.labels.end()) return found->second;
  }
  return std::nullopt;
}

bool absoluteData(const ir::Instruction& instruction) {
  const bool absolute = instruction.addressing == ir::Addressing::Absolute ||
                        instruction.addressing == ir::Addressing::AbsoluteX ||
                        instruction.addressing == ir::Addressing::AbsoluteY;
  return absolute && !instruction.target;
}

bool directData(const ir::Instruction& instruction) {
  return instruction.addressing == ir::Addressing::Direct ||
         instruction.addressing == ir::Addressing::DirectX ||
         instruction.addressing == ir::Addressing::DirectY;
}

// The register a direct-page operand at `site` lands on under the direct
// register every path proves, from the access facts: the first fact at the
// site. Empty where none names one.
std::string_view directRegister(const RenderInput& input, Address site) {
  const std::vector<RenderAccess>& accesses = input.accesses;
  auto it = std::lower_bound(accesses.begin(), accesses.end(), site,
                             [](const RenderAccess& a, Address wanted) { return a.site < wanted; });
  if (it != accesses.end() && it->site == site) return it->name;
  return {};
}

// The register a plain direct-page operand at `site` lands on under the one
// direct register the run saw there, where the paths proved none: the
// register's name with `(run)` after it. Empty where the run saw no value or
// more than one, or the address is no register. The sum wraps as the chip's
// does, within bank zero.
std::string runDirectRegister(const RenderInput& input, Address site, std::uint32_t operand) {
  const std::vector<RenderSeen>& seen = input.seen;
  auto it = std::lower_bound(seen.begin(), seen.end(), site,
                             [](const RenderSeen& s, Address wanted) { return s.address < wanted; });
  if (it == seen.end() || it->address != site || it->d.size() != 1) return {};
  const std::string_view name = cpu65816RegisterName((it->d.front() + operand) & 0xFFFFu);
  if (name.empty()) return {};
  return std::string(name) + " (run)";
}

// A run of bytes execution never reached, as `DB` rows of eight with the bytes
// as text beside them — the framework's own form, so a source file and a
// listing read alike. The address prints at `addressBits`: 24 in a bank file,
// 16 in the sound program's.
std::string renderDataRun(Address address, const std::vector<std::uint8_t>& data, unsigned addressBits) {
  constexpr std::size_t kCommentColumn = 40;
  constexpr std::size_t kPerRow = 8;
  std::string out = "\n; ---- " + std::to_string(data.size()) + " bytes execution did not reach\n";
  for (std::size_t i = 0; i < data.size(); i += kPerRow) {
    const std::size_t end = std::min(i + kPerRow, data.size());
    std::string row = "        DB ";
    std::string ascii;
    for (std::size_t j = i; j < end; ++j) {
      if (j != i) row += ",";
      row += "$" + hex(data[j], 2);
      const std::uint8_t byte = data[j];
      ascii += (byte >= 0x20 && byte < 0x7F) ? static_cast<char>(byte) : '.';
    }
    if (row.size() < kCommentColumn) {
      row.append(kCommentColumn - row.size(), ' ');
    } else {
      row += "  ";
    }
    row += "; " + formatAddress(address + static_cast<Address>(i), addressBits) + "  |" + ascii + "|";
    out += row + "\n";
  }
  return out;
}

std::string classList(const std::vector<RegisterClass>& classes) {
  if (classes.empty()) return "none";
  std::string t;
  for (const RegisterClass cls : classes) {
    if (!t.empty()) t += ", ";
    t += std::string(cpu65816RegisterClassName(cls));
  }
  return t;
}

std::string nameList(const std::vector<std::string>& names) {
  if (names.empty()) return "none";
  std::string t;
  for (const std::string& name : names) {
    if (!t.empty()) t += ", ";
    t += name;
  }
  return t;
}

std::string counted(std::size_t count, const char* noun) {
  return std::to_string(count) + " " + noun + (count == 1 ? "" : "s");
}

}  // namespace

std::string renderRegion(const RenderRegion& region, const RenderInput& input,
                         const ir::Program& program) {
  const Listing lines = regionLines(region, input);
  const std::string soundFile = input.sound ? input.sound->file : std::string();

  // A code line's instruction as the program holds it: the first node at the
  // line's address, which is the listing's reading where two paths read it two
  // ways.
  auto nodeAt = [&](Address address) -> const ir::Node& {
    const auto found = std::lower_bound(
        program.nodes.begin(), program.nodes.end(), address,
        [](const ir::Node& node, Address wanted) { return node.instruction.address < wanted; });
    if (found == program.nodes.end() || found->instruction.address != address) {
      throw std::logic_error("the program has no node for the instruction at " + address24(address));
    }
    return *found;
  };

  // What the file names: the labels it defines, the registers its absolute
  // operands address, and the labels other files define that it refers to. A
  // register is named only where its name can be a symbol and the file defines
  // no label of that name.
  std::set<std::string> defined;
  for (const auto& [address, label] : lines.labels) defined.insert(label);
  std::map<std::uint32_t, std::string_view> registers;
  std::map<Address, std::string> foreign;
  for (const Line& line : lines.lines) {
    if (!line.isCode) continue;
    const ir::Instruction& instruction = nodeAt(line.address).instruction;
    // A target is looked up as the bytes name it: a jump through a mirror bank
    // finds no label there, and keeps its address, since a symbol would carry
    // the bank the bytes are placed in rather than the one they name.
    if (instruction.target) {
      if (lines.labels.find(*instruction.target) != lines.labels.end()) continue;
      const std::optional<std::string> label = labelAnywhere(input, *instruction.target);
      if (label && defined.find(*label) == defined.end()) foreign[*instruction.target] = *label;
      continue;
    }
    if (!absoluteData(instruction)) continue;
    const std::string_view name = operandRegister(input, line.address, instruction.operand);
    if (!name.empty() && symbolName(name) && defined.find(std::string(name)) == defined.end()) {
      registers[instruction.operand] = name;
    }
  }

  // The routines that begin in this file, and who calls each.
  std::map<Address, const RenderRoutine*> routines;
  std::map<Address, std::string> routineLabels;
  std::map<Address, std::vector<std::string>> callers;
  for (const RenderRoutine& routine : input.routines) routineLabels[routine.address] = routine.label;
  for (const RenderRoutine& routine : input.routines) {
    if (lines.labels.find(routine.address) != lines.labels.end()) {
      routines[routine.address] = &routine;
    }
    for (const Address callee : routine.calls) callers[callee].push_back(routine.label);
  }

  std::string out;
  for (const std::string& warning : lines.warnings) out += "; warning: " + warning + "\n";
  if (!lines.warnings.empty()) out += "\n";

  if (!registers.empty() || !foreign.empty()) {
    std::size_t width = 0;
    for (const auto& [address, name] : registers) width = std::max(width, name.size());
    for (const auto& [address, name] : foreign) width = std::max(width, name.size());
    auto equ = [&](std::string_view name, const std::string& value) {
      std::string row(name);
      row.append(width + 2 - name.size(), ' ');
      out += row + "EQU " + value + "\n";
    };
    out += "; The hardware registers this file names, and the labels other files\n"
           "; define that it refers to.\n";
    for (const auto& [address, name] : registers) equ(name, "$" + hex(address, 4));
    for (const auto& [address, name] : foreign) equ(name, "$" + hex(address, 6));
    out += "\n";
  }

  // The raw-bytes field is as wide as the longest instruction in the file, and
  // never narrower than three bytes, so the cycle costs stay aligned.
  std::size_t longest = 3;
  for (const Line& line : lines.lines) {
    if (line.isCode) longest = std::max<std::size_t>(longest, line.instruction.length);
  }
  const std::size_t bytesWidth = longest * 3;

  // One piece per run of consecutive lines, each under its own `ORG`; a gap is
  // a cut — the sound program's bytes, which are its file's, or a lifted file's,
  // included where they were. Every piece is a region to an assembler, and so
  // is whatever follows a run of data or an `INCBIN`.
  const std::vector<Cut> cuts = cutsOf(input, region);
  std::size_t nextCut = 0;
  bool open = false;
  ir::SourceMode mode;
  auto writeCutsBefore = [&](Address until) {
    while (nextCut < cuts.size() && cuts[nextCut].range.first < until) {
      const Cut& cut = cuts[nextCut++];
      const std::string span = address24(cut.range.first) + "-" + address24(cut.range.last);
      if (cut.asset == nullptr) {
        out += "\n; ---- " + span + ": the sound program, see " + soundFile + "\n";
        open = false;
        continue;
      }
      if (!open) {
        out += "        ORG " + address24(cut.range.first) + "\n";
        open = true;
      }
      const RenderAsset& asset = *cut.asset;
      const std::string_view name = cpu65816RegisterName(asset.registerAddress);
      const std::string to = name.empty() ? address24(asset.registerAddress) : std::string(name);
      // A file the shadow named is described by the class it went to, which
      // its line keeps; the register is the run's and is not.
      const std::string cls = text::classesText(asset.classes, " and ");
      std::string what;
      switch (asset.kind) {
        case MovedKind::Dma: what = counted(asset.bytes, "byte") + " a transfer carried to " + to; break;
        case MovedKind::Table: what = "an HDMA table walked to " + to; break;
        case MovedKind::Indirect: what = "a block an HDMA entry pointed at, sent to " + to; break;
        case MovedKind::Stream: what = counted(asset.bytes, "byte") + " a routine carried " + cls + " data from"; break;
        case MovedKind::Staged: what = counted(asset.bytes, "byte") + " a routine built " + cls + " data from"; break;
        case MovedKind::Proven: what = counted(asset.bytes, "byte") + " a transfer the code sets up to carry to " + to; break;
      }
      // The path as the lexicon reads it: relative to this file, which for a
      // file at the tree's root is the manifest's own path.
      const std::string included =
          std::filesystem::path(asset.file)
              .lexically_relative(std::filesystem::path(region.file).parent_path())
              .generic_string();
      out += "\n; ---- " + span + ": " + what + ", in " + asset.file + "\n";
      out += "        INCBIN \"" + included + "\"";
      if (cut.length != asset.bytes) {
        out += ", " + std::to_string(cut.fileOffset) + ", " + std::to_string(cut.length);
      }
      out += "\n";
      mode.reset();
    }
  };
  for (const Line& line : lines.lines) {
    writeCutsBefore(line.address);
    if (!open) {
      out += "        ORG " + address24(line.address) + "\n";
      open = true;
      mode.reset();
    }

    if (!line.isCode) {
      if (!line.data.empty()) out += renderDataRun(line.address, line.data, 24);
      mode.reset();
      continue;
    }

    const ir::Node& node = nodeAt(line.address);
    // A routine's header sits directly above its label: its size, its role, and
    // the call graph either way.
    bool headed = false;
    if (const auto routine = routines.find(line.address); routine != routines.end()) {
      const RenderRoutine& r = *routine->second;
      std::vector<std::string> calls;
      for (const Address callee : r.calls) calls.push_back(routineLabels.at(callee));
      const auto called = callers.find(r.address);
      out += "\n; routine " + r.label + ": " + counted(r.lines, "line") + ", " +
             counted(r.bytes, "byte") + "\n";
      out += ";   reaches " + classList(r.reaches) + "; through " + classList(r.through) + "\n";
      out += ";   calls " + nameList(calls) + "; called by " +
             nameList(called == callers.end() ? std::vector<std::string>{} : called->second) +
             "\n";
      headed = true;
    }
    if (const auto label = lines.labels.find(line.address); label != lines.labels.end()) {
      out += (headed ? "" : "\n") + label->second + ":\n";
    }
    for (const std::string& directive : mode.directives(node)) {
      out += "        " + directive + "\n";
    }

    ir::SourceNames names;
    std::string runNote;  // the annotation's text where it is built here rather than borrowed
    const ir::Instruction& instruction = node.instruction;
    if (instruction.target) {
      if (const auto own = lines.labels.find(*instruction.target); own != lines.labels.end()) {
        names.target = own->second;
      } else if (const auto other = foreign.find(*instruction.target); other != foreign.end()) {
        names.target = other->second;
      }
    } else if (absoluteData(instruction)) {
      const std::string_view name = operandRegister(input, line.address, instruction.operand);
      if (registers.find(instruction.operand) != registers.end()) {
        names.operand = name;
      } else {
        names.annotation = name;
      }
    } else if (directData(instruction)) {
      // A direct-page operand stays the offset it is; the register it lands on
      // under the proven direct register goes in the comment — or, where the
      // paths prove nothing and the run saw one direct register at a plain
      // direct-page form, the register that value lands it on, marked as the
      // run's.
      names.annotation = directRegister(input, line.address);
      if (names.annotation.empty() && instruction.addressing == ir::Addressing::Direct) {
        runNote = runDirectRegister(input, line.address, instruction.operand);
        names.annotation = runNote;
      }
    }
    out += ir::renderLine(node, names, bytesWidth);
  }
  writeCutsBefore(region.last + 1u);
  return out;
}

std::string renderSoundFile(const RenderInput& input, const ir::Program& program) {
  if (!input.sound) throw std::logic_error("no sound program to render");
  const RenderSound& sound = *input.sound;
  std::string out = "; The sound program the cartridge uploads at boot, traced from " +
                    formatAddress(sound.entry, 16) + ".\n";
  for (const RenderBlock& block : sound.blocks) {
    out += "; " + formatAddress(block.apuAddress, 16) + ": " + std::to_string(block.bytes) + " bytes";
    out += block.romOffset ? ", read from image offset $" + hex(static_cast<std::uint32_t>(*block.romOffset), 6)
                           : std::string(", not read from the image as they are");
    out += "\n";
  }
  out += "\n";

  // A target is written as its label wherever the file defines one.
  std::map<Address, std::string> labels;
  for (const ir::ProgramRegion& region : sound.regions) {
    for (const ir::ProgramLabel& label : region.labels) labels[label.address] = label.name;
  }

  std::optional<Address> previousEnd;
  for (const ir::ProgramRegion& region : sound.regions) {
    // The region's nodes: the first at each address.
    std::vector<const ir::Node*> nodes;
    auto it = std::lower_bound(program.spc700.begin(), program.spc700.end(), region.first,
                               [](const ir::Node& node, Address wanted) { return node.instruction.address < wanted; });
    for (; it != program.spc700.end() && it->instruction.address <= region.last; ++it) {
      if (!nodes.empty() && nodes.back()->instruction.address == it->instruction.address) continue;
      nodes.push_back(&*it);
    }

    // A gap between two regions is what the upload never wrote.
    if (previousEnd && *previousEnd != region.first) {
      out += "\n; ---- " + formatAddress(*previousEnd, 16) + "-" + formatAddress(region.first - 1u, 16) +
             ": not uploaded\n";
    }
    for (const std::string& warning : region.warnings) out += "; warning: " + warning + "\n";
    if (!region.warnings.empty()) out += "\n";
    out += "        ORG " + formatAddress(region.first, 16) + "\n";

    // The raw-bytes field is as wide as the longest instruction in the region,
    // and never narrower than three bytes.
    std::size_t longest = 3;
    for (const ir::Node* node : nodes) longest = std::max<std::size_t>(longest, node->instruction.length);
    const std::size_t bytesWidth = longest * 3;

    // The nodes and the data runs merged by address.
    std::size_t n = 0;
    std::size_t d = 0;
    while (n < nodes.size() || d < region.data.size()) {
      const Address na = n < nodes.size() ? nodes[n]->instruction.address : 0xFFFFFFFFu;
      const Address da = d < region.data.size() ? region.data[d].address : 0xFFFFFFFFu;
      if (da <= na) {
        const ir::DataRun& run = region.data[d++];
        if (!run.bytes.empty()) out += renderDataRun(run.address, run.bytes, 16);
        continue;
      }
      const ir::Node& node = *nodes[n++];
      if (const auto label = labels.find(node.instruction.address); label != labels.end()) {
        out += "\n" + label->second + ":\n";
      }
      std::string_view targetLabel;
      if (node.instruction.target) {
        if (const auto found = labels.find(*node.instruction.target); found != labels.end()) {
          targetLabel = found->second;
        }
      }
      out += ir::renderSpc700Line(node, targetLabel, bytesWidth);
    }
    previousEnd = region.last + 1u;
  }
  return out;
}

namespace {

std::optional<std::string> readText(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// The first node at each address of the program within `first`..`last`, as the
// code lines of a listing, merged with the data runs in address order. A code
// line's bytes are the node's own encoding, which the renderer's cuts need when
// an instruction runs past a range's edge.
Listing listingOf(const ir::Program& program, const ir::ProgramRegion& region) {
  Listing listing;
  listing.addressBits = 24;
  listing.warnings = region.warnings;
  for (const ir::ProgramLabel& label : region.labels) listing.labels[label.address] = label.name;
  std::vector<Line> code;
  std::optional<Address> previous;
  for (const ir::Node& node : program.nodes) {
    const Address at = node.instruction.address;
    if (at < region.first || at > region.last) continue;
    if (previous == at) continue;  // the second reading of an address is not a line
    previous = at;
    Line line;
    line.isCode = true;
    line.address = at;
    line.instruction.address = at;
    line.instruction.length = node.instruction.length;
    line.instruction.bytes = ir::encode(node.instruction);
    code.push_back(std::move(line));
  }
  std::size_t c = 0;
  std::size_t d = 0;
  while (c < code.size() || d < region.data.size()) {
    const Address ca = c < code.size() ? code[c].address : 0xFFFFFFFFu;
    const Address da = d < region.data.size() ? region.data[d].address : 0xFFFFFFFFu;
    if (da <= ca) {
      Line line;
      line.isCode = false;
      line.address = da;
      line.data = region.data[d++].bytes;
      listing.lines.push_back(std::move(line));
    } else {
      listing.lines.push_back(std::move(code[c++]));
    }
  }
  return listing;
}

// The register the `access` line names, at its address in bank zero, from the
// register table: the table is walked once and the name looked up in it.
Address registerAddressOf(std::string_view name) {
  static const std::map<std::string_view, Address> byName = [] {
    std::map<std::string_view, Address> out;
    for (Address offset = 0x2100u; offset <= 0x21FFu; ++offset) {
      if (const std::optional<Cpu65816Register> reg = cpu65816Register(offset)) out.emplace(reg->name, offset);
    }
    for (Address offset = 0x4000u; offset <= 0x43FFu; ++offset) {
      if (const std::optional<Cpu65816Register> reg = cpu65816Register(offset)) out.emplace(reg->name, offset);
    }
    return out;
  }();
  const auto found = byName.find(name);
  return found == byName.end() ? 0u : found->second;
}

using Span = std::pair<std::size_t, std::size_t>;  // image offsets, inclusive

// Where a lifted file's bytes lie in the image, or nothing for an address
// outside it.
std::optional<Span> imageSpan(CartridgeMap map, std::size_t imageBytes, Address first, std::size_t bytes) {
  const std::optional<std::size_t> start = romOffset(map, first, imageBytes);
  if (!start || bytes == 0) return std::nullopt;
  return Span{*start, *start + bytes - 1u};
}

// The image offsets a transfer's bytes were read from. The engine steps the
// address within its bank — up, down, or not at all — and wraps at the bank's
// edge, so a range is at most two runs; a run whose start is not in the image
// is not in it.
std::vector<Span> transferSpans(CartridgeMap map, std::size_t imageBytes, Address memory, std::size_t bytes,
                                MovedStep step) {
  std::vector<Span> spans;
  if (bytes == 0) return spans;
  if (step == MovedStep::Fixed) bytes = 1;
  const Address bank = memory & 0xFF0000u;
  const std::uint32_t offset = memory & 0xFFFFu;
  auto add = [&](Address first, std::size_t length) {
    const std::optional<std::size_t> start = romOffset(map, first, imageBytes);
    if (start && length != 0) spans.push_back(Span{*start, *start + length - 1u});
  };
  if (step == MovedStep::Decrement) {
    const std::size_t first = std::min<std::size_t>(bytes, offset + 1u);
    add(bank | (offset - static_cast<std::uint32_t>(first) + 1u), first);
    if (bytes > first) add(bank | (0x10000u - static_cast<std::uint32_t>(bytes - first)), bytes - first);
  } else {
    const std::size_t first = std::min<std::size_t>(bytes, 0x10000u - offset);
    add(bank | offset, first);
    if (bytes > first) add(bank, bytes - first);
  }
  return spans;
}

bool overlaps(const Span& a, const Span& b) { return a.first <= b.second && b.first <= a.second; }

bool anyOverlap(const Span& span, const std::vector<Span>& spans) {
  return std::any_of(spans.begin(), spans.end(), [&](const Span& s) { return overlaps(span, s); });
}

// The register a lifted file's bytes went to: for a transfer, a table or an
// indirect block, the register of the `moved` range that lifted it; for a
// transfer the code proves, the destination of the `dma` line that set it up.
// A stream's and a staged file's comment names the class alone, so theirs is
// not looked for.
Address assetRegister(const ManifestInput& manifest, CartridgeMap map, std::size_t imageBytes,
                      const ManifestAsset& asset) {
  const std::optional<Span> span = imageSpan(map, imageBytes, asset.first, asset.bytes);
  if (!span) return 0;
  if (asset.kind == MovedKind::Proven) {
    for (const ManifestDma& dma : manifest.dmas) {
      if (!dma.destination || !dma.source || !dma.bytes) continue;
      if (anyOverlap(*span, transferSpans(map, imageBytes, *dma.source, *dma.bytes, dma.step))) {
        return *dma.destination;
      }
    }
    return 0;
  }
  for (const MovedRange& moved : manifest.moved) {
    if (moved.kind != asset.kind || !moved.toRegister) continue;
    if (anyOverlap(*span, transferSpans(map, imageBytes, moved.memory, moved.bytes, moved.step))) {
      return moved.registerAddress;
    }
  }
  return 0;
}

}  // namespace

std::optional<RenderInput> readRenderInput(const std::filesystem::path& directory, ir::Program& program,
                                           std::string& error) {
  const std::optional<std::string> programText = readText(directory / "program.snagir");
  if (!programText) {
    error = "cannot open " + (directory / "program.snagir").string();
    return std::nullopt;
  }
  std::string why;
  const std::optional<ir::Parsed> parsed = ir::parseProgram(*programText, why);
  if (!parsed) {
    error = "program.snagir: " + why;
    return std::nullopt;
  }
  const std::optional<std::string> manifestText = readText(directory / "project.snagifest");
  if (!manifestText) {
    error = "cannot open " + (directory / "project.snagifest").string();
    return std::nullopt;
  }
  const std::optional<ManifestInput> manifest = parseManifest(*manifestText, why);
  if (!manifest) {
    error = "project.snagifest: " + why;
    return std::nullopt;
  }
  if (!manifest->map || !manifest->imageBytes) {
    error = "project.snagifest: the map and the image size are needed to render";
    return std::nullopt;
  }

  RenderInput input;
  input.map = *manifest->map;
  input.imageBytes = *manifest->imageBytes;
  for (const ir::ProgramRegion& region : parsed->file.regions) {
    input.regions.push_back(RenderRegion{.file = region.file,
                                         .first = region.first,
                                         .last = region.last,
                                         .listing = listingOf(parsed->program, region)});
  }
  input.accesses = manifest->accesses;
  for (RenderAccess& access : input.accesses) access.registerAddress = registerAddressOf(access.name);
  input.seen = manifest->seen;
  input.routines = manifest->routines;
  for (const ManifestAsset& asset : manifest->assets) {
    input.assets.push_back(RenderAsset{.file = asset.file,
                                       .classes = asset.classes,
                                       .kind = asset.kind,
                                       .registerAddress = assetRegister(*manifest, input.map, input.imageBytes, asset),
                                       .first = asset.first,
                                       .bytes = asset.bytes});
  }
  program = parsed->program;
  if (manifest->sound) {
    RenderSound sound;
    sound.file = manifest->sound->file;
    sound.entry = manifest->sound->entry;
    for (const ManifestBlock& block : manifest->sound->blocks) {
      sound.blocks.push_back(
          RenderBlock{.apuAddress = block.apuAddress, .bytes = block.length, .romOffset = block.romOffset});
    }
    // The sound program's own file: its nodes and the regions written to the
    // file the manifest names.
    const std::optional<std::string> soundText = readText(directory / "apu.snagir");
    if (!soundText) {
      error = "cannot open " + (directory / "apu.snagir").string();
      return std::nullopt;
    }
    const std::optional<ir::Parsed> soundParsed = ir::parseProgram(*soundText, why);
    if (!soundParsed) {
      error = "apu.snagir: " + why;
      return std::nullopt;
    }
    if (soundParsed->file.processor != ir::Processor::Spc700) {
      error = "apu.snagir: not the sound program's file";
      return std::nullopt;
    }
    for (const ir::ProgramRegion& region : soundParsed->file.regions) {
      if (region.file == sound.file) sound.regions.push_back(region);
    }
    if (sound.regions.empty()) {
      error = "apu.snagir: no region is written to " + sound.file + ", which the manifest names";
      return std::nullopt;
    }
    std::sort(sound.regions.begin(), sound.regions.end(),
              [](const ir::ProgramRegion& a, const ir::ProgramRegion& b) { return a.first < b.first; });
    program.spc700 = soundParsed->program.spc700;
    input.sound = std::move(sound);
  }
  return input;
}

bool renderTree(const std::filesystem::path& directory, std::size_t& rendered, std::string& error) {
  rendered = 0;
  ir::Program program;
  const std::optional<RenderInput> input = readRenderInput(directory, program, error);
  if (!input) return false;
  auto write = [&](const std::string& file, const std::string& text) {
    const std::filesystem::path path = directory / file;
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary);
    if (out) out << text;
    if (!out) {
      error = "cannot write " + path.string();
      return false;
    }
    ++rendered;
    return true;
  };
  for (const RenderRegion& region : input->regions) {
    if (!write(region.file, renderRegion(region, *input, program))) return false;
  }
  if (input->sound && !write(input->sound->file, renderSoundFile(*input, program))) return false;
  return true;
}

}  // namespace snaggletooth::disasm
