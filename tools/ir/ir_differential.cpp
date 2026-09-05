#include "ir/ir_differential.h"

#include "ir/ir_text.h"
#include "snaggletooth/snes/cartridge.h"

#include <algorithm>
#include <initializer_list>
#include <set>
#include <string_view>
#include <utility>

namespace snaggletooth::ir {
namespace {

// The address the tree places the instruction the machine holds at `address`:
// the one home of the image bytes it reads, so a bank that mirrors the image
// finds the node placed in the bank the image is written for. An address
// outside the image — code in work RAM — is its own.
Address placed(CartridgeMap map, std::size_t imageBytes, Address address) {
  const std::optional<std::size_t> offset = romOffset(map, address, imageBytes);
  if (!offset) return address;
  const std::optional<std::uint32_t> home = romAddress(map, *offset);
  return home ? *home : address;
}

// The constructs the report counts, every one seeded at zero.
constexpr std::string_view kConstructs[] = {
    "accumulator 8 bits", "accumulator 16 bits", "index 8 bits", "index 16 bits",
    "REP or SEP", "REP or SEP under emulation",
    "PLP", "RTI", "XCE", "a node with a live-flag width",
    "CMP, CPX or CPY", "BIT immediate", "BIT from memory", "TSB or TRB", "INC or DEC", "XBA",
    "PLB or PLD",
    "ADC or SBC in decimal mode",
    "a direct-page form with a low byte in D", "a direct-page form under emulation with D page-aligned",
    "a 6502 pointer form under emulation with D zero", "a long pointer form", "a stack-relative form",
    "an indexed absolute form", "an indexed read crossing a page under an 8-bit index",
    "an indexed absolute form reaching into the next bank",
    "JMP (abs)", "JML [abs]", "JMP (abs,X)", "JSR (abs,X)",
    "a long form", "long,X",
    "BRL or PER", "JML, JSL or RTL",
    "a pinned push or pull under emulation", "a push or pull stepping out of page one under emulation",
    "TCS or TXS under emulation",
    "JSR or JSL", "RTS or RTL", "PEA, PEI or PER", "PEA followed by RTS",
    "BRK or COP", "NMI", "IRQ", "WAI", "a wait released", "STP",
    "a read-modify-write under emulation", "a 16-bit read-modify-write",
    "a block move byte", "a block move re-entered",
    "a store to a hardware register", "a load from a hardware register",
    "a direct-page cycle charged", "a page-crossing cycle charged",
    "a taken branch", "a taken branch crossing a page under emulation",
};

bool isDirectForm(Addressing a) {
  switch (a) {
    case Addressing::Direct: case Addressing::DirectX: case Addressing::DirectY:
    case Addressing::DirectIndirect: case Addressing::DirectIndirectX:
    case Addressing::DirectIndirectY: case Addressing::DirectIndirectLong:
    case Addressing::DirectIndirectLongY:
      return true;
    default:
      return false;
  }
}

bool isHardwareRegister(std::uint32_t address) {
  const std::uint32_t bank = (address >> 16) & 0xFFu;
  const std::uint32_t offset = address & 0xFFFFu;
  const bool systemBank = bank <= 0x3Fu || (bank >= 0x80u && bank <= 0xBFu);
  if (!systemBank) return false;
  return (offset >= 0x2100u && offset <= 0x21FFu) || offset == 0x4016u || offset == 0x4017u ||
         (offset >= 0x4200u && offset <= 0x43FFu);
}

bool oneOf(std::string_view mnemonic, std::initializer_list<std::string_view> names) {
  for (const std::string_view n : names) {
    if (mnemonic == n) return true;
  }
  return false;
}

// Records what a node exercised, from the registers before it, the node, the
// step's accesses and the outcome.
void cover(DifferentialReport& report, const Node& node, const Registers& before,
           const Registers& after, const std::vector<BusAccess>& data, std::uint32_t cycles,
           std::string_view lastMnemonic) {
  auto hit = [&](std::string_view name) { ++report.constructs[std::string(name)]; };
  const Instruction& i = node.instruction;
  const std::string_view m = i.mnemonic;
  const Addressing a = i.addressing;

  hit(before.accumulator8() ? "accumulator 8 bits" : "accumulator 16 bits");
  hit(before.index8() ? "index 8 bits" : "index 16 bits");
  if (oneOf(m, {"REP", "SEP"})) hit(before.e ? "REP or SEP under emulation" : "REP or SEP");
  if (m == "PLP") hit("PLP");
  if (m == "RTI") hit("RTI");
  if (m == "XCE") hit("XCE");
  if (!node.mode.accumulatorKnown || !node.mode.indexKnown) hit("a node with a live-flag width");
  if (oneOf(m, {"CMP", "CPX", "CPY"})) hit("CMP, CPX or CPY");
  if (m == "BIT") hit(a == Addressing::ImmediateM ? "BIT immediate" : "BIT from memory");
  if (oneOf(m, {"TSB", "TRB"})) hit("TSB or TRB");
  if (oneOf(m, {"INC", "DEC"})) hit("INC or DEC");
  if (m == "XBA") hit("XBA");
  if (oneOf(m, {"PLB", "PLD"})) hit("PLB or PLD");
  if (oneOf(m, {"ADC", "SBC"}) && (before.p & 0x08u) != 0) hit("ADC or SBC in decimal mode");

  const bool direct = isDirectForm(a) || m == "PEI";
  if (direct && (before.d & 0xFFu) != 0) hit("a direct-page form with a low byte in D");
  if (direct && before.e && (before.d & 0xFFu) == 0) {
    hit("a direct-page form under emulation with D page-aligned");
  }
  const bool pointer6502 = a == Addressing::DirectIndirect || a == Addressing::DirectIndirectX ||
                           a == Addressing::DirectIndirectY;
  if (pointer6502 && before.e && before.d == 0 && m != "PEI") {
    hit("a 6502 pointer form under emulation with D zero");
  }
  if (a == Addressing::DirectIndirectLong || a == Addressing::DirectIndirectLongY) {
    hit("a long pointer form");
  }
  if (a == Addressing::StackRelative || a == Addressing::StackRelativeY) hit("a stack-relative form");
  if (a == Addressing::AbsoluteX || a == Addressing::AbsoluteY) {
    hit("an indexed absolute form");
    const std::uint32_t index = a == Addressing::AbsoluteX ? before.x : before.y;
    if ((i.operand & 0xFFFFu) + index > 0xFFFFu) hit("an indexed absolute form reaching into the next bank");
  }
  if (a == Addressing::AbsoluteIndirect) hit("JMP (abs)");
  if (a == Addressing::AbsoluteIndirectLong) hit("JML [abs]");
  if (a == Addressing::AbsoluteIndexedIndirect) hit(m == "JSR" ? "JSR (abs,X)" : "JMP (abs,X)");
  if (a == Addressing::AbsoluteLong) hit("a long form");
  if (a == Addressing::AbsoluteLongX) hit("long,X");
  if (oneOf(m, {"BRL", "PER"})) hit("BRL or PER");
  if (oneOf(m, {"JML", "JSL", "RTL"})) hit("JML, JSL or RTL");
  if (before.e) {
    if (oneOf(m, {"PHA", "PHP", "PLA", "PLP", "JSR", "RTS", "RTI", "BRK", "COP"})) {
      hit("a pinned push or pull under emulation");
    }
    if (oneOf(m, {"PHX", "PHY", "PHB", "PHK", "PHD", "PEA", "PEI", "PER", "PLX", "PLY", "PLB",
                  "PLD", "RTL", "JSL"})) {
      hit("a push or pull stepping out of page one under emulation");
    }
    if (oneOf(m, {"TCS", "TXS"})) hit("TCS or TXS under emulation");
  }
  if (oneOf(m, {"JSR", "JSL"})) hit("JSR or JSL");
  if (oneOf(m, {"RTS", "RTL"})) hit("RTS or RTL");
  if (oneOf(m, {"PEA", "PEI", "PER"})) hit("PEA, PEI or PER");
  if (m == "RTS" && lastMnemonic == "PEA") hit("PEA followed by RTS");
  if (oneOf(m, {"BRK", "COP"})) hit("BRK or COP");
  if (m == "WAI") hit("WAI");
  if (m == "STP") hit("STP");

  const bool rmw = oneOf(m, {"INC", "DEC", "ASL", "LSR", "ROL", "ROR", "TSB", "TRB"}) &&
                   a != Addressing::Accumulator && a != Addressing::Implied;
  if (rmw && before.e) hit("a read-modify-write under emulation");
  if (rmw && !before.accumulator8()) hit("a 16-bit read-modify-write");
  if (a == Addressing::BlockMove) {
    hit("a block move byte");
    if (after.pc == before.pc && after.pbr == before.pbr) hit("a block move re-entered");
  }
  for (const BusAccess& access : data) {
    if (!isHardwareRegister(access.address)) continue;
    hit(access.write ? "a store to a hardware register" : "a load from a hardware register");
  }

  // The increments, from what the node cost beyond its base.
  const std::uint32_t base = node.cost.base[costIndex(before.accumulator8(), before.index8())];
  const std::uint32_t extra = cycles > base ? cycles - base : 0u;
  std::uint32_t accounted = 0;
  if (direct && (before.d & 0xFFu) != 0) {
    hit("a direct-page cycle charged");
    ++accounted;
  }
  if (i.flow == Flow::Branch && i.target && after.pc == (*i.target & 0xFFFFu) &&
      after.pc != ((i.address + i.length) & 0xFFFFu)) {
    hit("a taken branch");
    ++accounted;
    const Address next = (i.address & 0xFF0000u) | ((i.address + i.length) & 0xFFFFu);
    if (before.e && ((next ^ *i.target) & 0xFF00u) != 0) {
      hit("a taken branch crossing a page under emulation");
      ++accounted;
    }
  }
  const bool indexedRead = a == Addressing::AbsoluteX || a == Addressing::AbsoluteY ||
                           a == Addressing::DirectIndirectY;
  if (indexedRead && before.index8() && extra > accounted) {
    hit("a page-crossing cycle charged");
  }
}

}  // namespace

DifferentialReport differential(const Program& program, const Replay& replay) {
  DifferentialReport report;
  for (const std::string_view name : kConstructs) report.constructs[std::string(name)] = 0;

  Snes machine{SnesConfig{.rom = replay.rom}};
  StepObserver observer;
  machine.setObserver(&observer);
  Interpreter interpreter;
  interpreter.registers = registersOf(machine.state().cpu);
  const CartridgeMap map = detectCartridgeMap(replay.rom);
  const std::size_t imageBytes = replay.rom.size();

  // The recorded run is presented as the cartridge disassembler presents it: the
  // pads for a frame as the frame begins, a frame beginning when the beam wraps to
  // line 0, the first at power-on.
  std::uint32_t frame = 0;
  auto presentPads = [&]() {
    machine.setJoypad(JoypadPort::One, replay.input.padAt(JoypadPort::One, frame));
    machine.setJoypad(JoypadPort::Two, replay.input.padAt(JoypadPort::Two, frame));
  };
  presentPads();

  std::set<Address> unlifted;
  std::string lastMnemonic;
  std::uint64_t step = 0;
  std::uint64_t spent = 0;
  while (spent < replay.masterCycles && !report.stopped &&
         report.divergences.size() < replay.divergenceLimit) {
    const Cpu65816State before = machine.state().cpu;
    const std::uint16_t lineBefore = machine.state().vpos;
    observer.clear();
    spent += machine.step();
    if (machine.state().vpos < lineBefore) {
      ++frame;
      presentPads();
    }
    const Cpu65816State& after = machine.state().cpu;

    if (before.run != CpuRunState::Running) {
      // A halted cycle: nothing ran. If a line ended the wait, the interpreter's
      // wait ends with it.
      ++report.haltedCycles;
      if (after.run == CpuRunState::Running) {
        interpreter.release();
        ++report.releases;
        ++report.constructs["a wait released"];
      }
      if (after.run == CpuRunState::Stopped) report.stopped = true;
      continue;
    }
    if (!observer.cpuRan) {
      // A transfer engine held the bus for the whole step; the instruction is
      // still ahead of the CPU and is seen on the step that runs it.
      ++report.heldSteps;
      continue;
    }

    // What the machine took: a hardware request due at the boundary, or the
    // instruction at the program counter. The site is reported, and the node
    // looked up, at the address the tree places the instruction.
    const bool nmi = before.nmiPending;
    const bool irq = !nmi && before.irqLine && (before.p & kCpuFlagI) == 0;
    const Registers registersBefore = interpreter.registers;
    Divergence prototype;
    prototype.instruction = step;
    prototype.site =
        placed(map, imageBytes, (static_cast<Address>(before.pbr) << 16) | before.pc);
    std::uint32_t cycles = 0;
    const Node* node = nullptr;

    if (nmi || irq) {
      prototype.name = nmi ? "NMI" : "IRQ";
      cycles = checkInterrupt(interpreter, nmi ? program.nmi : program.irq, observer, after,
                              prototype, report.divergences);
      ++report.interrupts;
      ++report.constructs[nmi ? "NMI" : "IRQ"];
    } else {
      const Registers& r = interpreter.registers;
      node = program.find(placed(map, imageBytes, (static_cast<Address>(r.pbr) << 16) | r.pc), r.e,
                          r.accumulator8(), r.index8());
      if (node == nullptr) {
        ++report.unlifted;
        unlifted.insert(prototype.site);
        interpreter.registers = registersOf(after);
        ++step;
        continue;
      }
      cycles = checkNode(interpreter, *node, observer, after, prototype, report.divergences);
      ++report.instructions;
      ++report.forms[std::string(node->instruction.mnemonic) + " " +
                     std::string(addressingName(node->instruction.addressing)) + " " +
                     modeName(node->mode)];
    }
    report.cpuCycles += observer.cpuCycles;

    if (node != nullptr) {
      cover(report, *node, registersBefore, registersOf(after), observer.data, cycles, lastMnemonic);
      lastMnemonic = std::string(node->instruction.mnemonic);
    }
    if (after.run == CpuRunState::Stopped) report.stopped = true;
    ++step;
  }

  machine.setObserver(nullptr);
  report.masterCycles = spent;
  report.unliftedSites.assign(unlifted.begin(), unlifted.end());
  return report;
}

}  // namespace snaggletooth::ir
