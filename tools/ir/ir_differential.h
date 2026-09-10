#pragma once

// The run beside the core — a recorded run replayed on the machine, one
// instruction at a time, with the interpreter running the same program from the
// same registers and held to everything the machine did.
//
// The machine is the oracle and the interpreter owns no memory. The machine's
// observer reports every access the CPU makes; the interpreter reads through a
// bus that answers each read with the value the machine read — an input, like an
// interrupt — and checks its address, and checks every write's address, value
// and order against the machine's. After each instruction the registers and
// flags are checked, and the cycle count the interpreter reports is checked
// against the CPU cycles the observer counted. The interpreter cannot copy a
// write through, because it never sees one; it has to compute every value from
// the effects, and a run with no divergence says the effects carry the
// instruction's whole meaning. The check itself is `ir/ir_lockstep.h`'s.
//
// Three things are inputs rather than nodes: a hardware interrupt the machine
// takes between two instructions, which the interpreter runs as the program's
// own sequence and is checked the same way; a wait an interrupt line releases;
// and a step the CPU spent held off the bus while a transfer ran, which is
// skipped. An instruction at an address the program has no node for is counted
// and the interpreter realigned to the machine, since there is nothing to run.
//
// A node is looked up, and a site reported, at the address the tree places the
// instruction: the one home of the image bytes the CPU fetched, so a program
// that runs through a mirror of its bank is checked against the nodes placed in
// the bank the image is written for. An address outside the image is its own.
//
// The sound program is replayed beside the same run. The audio machine reports
// every access the sound CPU makes and every instruction boundary it crosses;
// at each boundary the node at the program counter is looked up among the
// program's `spc700` nodes and held to what the machine did — the accesses
// with the instruction's own fetches set apart, the registers after, the
// cycles. An address with no node is counted, as on the main CPU; so is a node
// whose bytes are not the bytes the machine fetched there, which is a
// different program from the file's and is not checked against it.

#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>

#include "ir/ir.h"
#include "ir/ir_lockstep.h"
#include "rom/input_script.h"
#include "rom/progress.h"

namespace snaggletooth::ir {

// What to replay: the cartridge, how much of the master clock to run, and the
// recorded run to present at the controller ports frame by frame, exactly as
// the cartridge disassembler presents it. The run ends early when the main CPU
// has stopped — unless the program holds sound nodes and the sound CPU is
// still running, in which case it runs on until the sound CPU halts — or once
// `divergenceLimit` divergences have been recorded. `progress`,
// when set, is told `replaying the run` as it begins and every tenth of a
// second of the master clock after, with the cycles spent against
// `masterCycles`, and once more as it ends — short of the budget when the run
// ended early (`rom/progress.h`).
struct Replay {
  std::span<const std::uint8_t> rom;
  std::uint64_t masterCycles = 0;
  disasm::InputScript input;
  std::size_t divergenceLimit = 16;
  disasm::ProgressSink progress;
};

// What a replay found.
struct DifferentialReport {
  std::uint64_t instructions = 0;   // nodes run and checked
  std::uint64_t interrupts = 0;     // hardware sequences run and checked
  std::uint64_t heldSteps = 0;      // steps a transfer held the CPU off the bus for
  std::uint64_t haltedCycles = 0;   // cycles a waiting or stopped CPU sat through
  std::uint64_t releases = 0;       // waits an interrupt line ended
  std::uint64_t cpuCycles = 0;      // CPU cycles checked
  std::uint64_t masterCycles = 0;   // master cycles run
  std::uint64_t unlifted = 0;       // instructions at an address with no node
  std::vector<Address> unliftedSites;  // those addresses as the tree places them, each once, in address order
  bool stopped = false;             // the main CPU stopped; the run ended there, or once the sound program it was checking halted
  std::vector<Divergence> divergences;
  // How many times each instruction form ran, keyed by mnemonic, addressing
  // mode and the mode the node reads under — `LDA abs,X e=0 m=8 x=16`.
  std::map<std::string, std::uint64_t> forms;
  // How many times each named construct was exercised. Every name is present,
  // so a construct the run never reached reads zero.
  std::map<std::string, std::uint64_t> constructs;

  // The sound CPU's side: the nodes run and checked, the cycles checked, the
  // instructions at an audio address with no node and those addresses, the
  // instructions whose node's bytes are not what the machine fetched and
  // those addresses, and how many times each instruction form ran — `MOV
  // A,dp`, the mnemonic and the form word. The divergences are among
  // `divergences`, with the processor set.
  std::uint64_t spc700Instructions = 0;
  std::uint64_t spc700Cycles = 0;
  std::uint64_t spc700Unlifted = 0;
  std::vector<Address> spc700UnliftedSites;  // each once, in address order
  std::uint64_t spc700Patched = 0;
  std::vector<Address> spc700PatchedSites;   // each once, in address order
  std::map<std::string, std::uint64_t> spc700Forms;
};

// Replays the run on the machine beside the interpreter. `program` is the
// cartridge's nodes in address order, `Program::find` answering the node for
// the live flags at every step, and its `spc700` nodes the sound program's,
// `Program::findSpc700` answering the node at every audio address.
[[nodiscard]] DifferentialReport differential(const Program& program, const Replay& replay);

}  // namespace snaggletooth::ir
