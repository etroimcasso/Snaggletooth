#include "ir/ir_text.h"

#include <cstdio>

namespace snaggletooth::ir {
namespace {

constexpr std::string_view kOps[] = {
    "Set", "SetNZ", "Add", "Sub", "And", "Or", "Xor", "Shr",
    "DirectAddress", "BankAddress", "LongAddress", "ProgramAddress", "StackAddress",
    "Load", "Store", "StoreRmw", "Push", "Pull", "SettleStack",
    "Adc", "Sbc", "Cmp", "Bit", "BitImm", "Asl", "Lsr", "Rol", "Ror", "Inc", "Dec",
    "Tsb", "Trb", "WriteP", "Xba", "Xce", "Halt", "Cycles",
};

constexpr std::string_view kPlaces[] = {
    "",   "imm", "A", "X",  "Y",  "S",  "D",  "PC", "PBR", "DBR", "P", "E",
    "T0", "T1",  "T2", "T3", "N", "V", "M", "X", "D", "I", "Z", "C",
};

constexpr std::string_view kWidths[] = {"8", "16", "24", "byM", "byX"};
constexpr std::string_view kSteps[] = {"flat", "bank0", "bank", "direct", "pointer"};
constexpr std::string_view kAccesses[] = {"data", "rmw", "rmw-unmodified", "vector"};
constexpr std::string_view kWhens[] = {
    "", "if e", "if !e", "if set", "if clear", "if is", "if is not", "if D.lo", "if crossed",
};

constexpr std::string_view kAddressings[] = {
    "",       "A",      "#imm(M)", "#imm(X)", "#byte",  "dp",      "dp,X",     "dp,Y",
    "(dp)",   "(dp,X)", "(dp),Y",  "[dp]",    "[dp],Y", "sr,S",    "(sr,S),Y", "abs",
    "abs,X",  "abs,Y",  "long",    "long,X",  "(abs)",  "[abs]",   "(abs,X)",  "rel",
    "rel16",  "src,dst", "#abs",   "rel16",
};

std::string hex(std::uint32_t value) {
  char b[16];
  std::snprintf(b, sizeof b, "$%X", value);
  return b;
}

std::string operandText(const Operand& o) {
  if (o.place == Place::Imm) return hex(o.value);
  return std::string(placeName(o.place));
}

}  // namespace

std::string_view opName(Op op) noexcept { return kOps[static_cast<std::size_t>(op)]; }
std::string_view placeName(Place place) noexcept { return kPlaces[static_cast<std::size_t>(place)]; }
std::string_view widthName(Width width) noexcept { return kWidths[static_cast<std::size_t>(width)]; }
std::string_view stepName(Step step) noexcept { return kSteps[static_cast<std::size_t>(step)]; }
std::string_view accessName(Access access) noexcept {
  return kAccesses[static_cast<std::size_t>(access)];
}
std::string_view whenName(When when) noexcept { return kWhens[static_cast<std::size_t>(when)]; }
std::string_view addressingName(Addressing addressing) noexcept {
  return kAddressings[static_cast<std::size_t>(addressing)];
}

std::string modeName(const Mode& mode) {
  if (mode.emulation) return "e=1";
  std::string out = "e=0 m=";
  out += mode.accumulatorKnown ? (mode.accumulator8 ? "8" : "16") : "?";
  out += " x=";
  out += mode.indexKnown ? (mode.index8 ? "8" : "16") : "?";
  return out;
}

std::string renderEffect(const Effect& e) {
  std::string line(opName(e.op));
  if (e.dst.place != Place::None) line += " " + operandText(e.dst) + " <-";
  if (e.a.place != Place::None) line += " " + operandText(e.a);
  if (e.b.place != Place::None) line += ", " + operandText(e.b);
  line += "  [";
  line += widthName(e.width);
  if (e.op == Op::Load || e.op == Op::Store || e.op == Op::StoreRmw) {
    line += " ";
    line += stepName(e.step);
    if (e.access != Access::Data) {
      line += " ";
      line += accessName(e.access);
    }
  }
  if (e.op == Op::Push || e.op == Op::Pull) line += e.pinned ? " pinned" : " unpinned";
  line += "]";
  if (e.when.when != When::Always) {
    line += "  ";
    line += whenName(e.when.when);
    if (e.when.place != Place::None) {
      line += " ";
      line += placeName(e.when.place);
    }
    if (e.when.when == When::PlaceIs || e.when.when == When::PlaceIsNot) {
      line += " " + hex(e.when.value);
    }
    if (e.when.andEmulation) line += " and e";
  }
  return line;
}

std::string renderNode(const Node& node) {
  const Instruction& i = node.instruction;
  char header[160];
  std::snprintf(header, sizeof header, "$%02X:%04X  %s %s  operand $%X  length %d  %s  base %d/%d/%d/%d",
                (i.address >> 16) & 0xFFu, i.address & 0xFFFFu, std::string(i.mnemonic).c_str(),
                std::string(addressingName(i.addressing)).c_str(), i.operand, int{i.length},
                modeName(node.mode).c_str(), int{node.cost.base[0]}, int{node.cost.base[1]},
                int{node.cost.base[2]}, int{node.cost.base[3]});
  std::string out = header;
  if (!node.registerName.empty()) {
    out += "  ";
    out += node.registerName;
  }
  if (node.patched) out += "  patched";
  out += "\n";
  for (const Effect& e : node.effects) out += "    " + renderEffect(e) + "\n";
  return out;
}

}  // namespace snaggletooth::ir
