#pragma once

// The dataflow — what every path proves about the registers at each instruction.
//
// A program's effect layer says what each instruction does to the CPU's named
// state. Run over the whole program at once, with every value a set of the
// values a register can hold rather than one of them, the same effects say what
// is true at an instruction however execution reached it: the direct register is
// `$4300` here because every path in loaded it so; the accumulator at this store
// is `$80` because the load twenty instructions and a call earlier was an
// immediate and nothing since has touched it; the index at this jump is one of
// eight values because a mask bounded it and a shift doubled it. A register is
// known at a site only when every path into the site proves it; where two paths
// prove different values the set holds both, and a reader of it sees the
// disagreement rather than a choice; where a path proves nothing the register is
// not known, and stays not known.
//
// Nothing here guesses. A value comes from an immediate, from a transfer of a
// register whose value is known, from a pull of a byte whose push is on the same
// straight path, or from a load whose address is known and lies in the image —
// which is how a jump through a table becomes a set of destinations when the
// index is bounded and the table is in the cartridge. Memory is otherwise
// unknown: a load from work RAM or a register is a value the runtime supplies,
// and the analysis says so by knowing nothing about it.
//
// Paths are the program's own: falling through, branching, jumping, calling and
// returning, and the destinations a run saw or an earlier analysis derived. A
// call carries the caller's state into the routine and brings back what every
// return of the routine proves — except a register the routine gives back as it
// took it, which comes back as the caller left it. Which registers those are is
// read off the routine itself, run once from a named entry: a register never
// written keeps its name, and so does one pushed and pulled around the routine's
// own work. A hardware interrupt is not a path: its handler begins at its vector
// with nothing proven, and what it does between two of the program's instructions
// is the handler's to restore.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include "ir/ir.h"

namespace snaggletooth::ir {

// The most values a set holds and is still worth knowing. A mask of nine bits
// bounds an index to this many.
inline constexpr std::size_t kMostValues = 512;

// A value that is not known but is named: what a register held when the routine
// being examined was entered, whole (`part` 2 for a sixteen-bit register, 0 for
// an eight-bit one) or one byte of it (0 the low, 1 the high), plus a count of
// bytes the stack pointer has moved since. The analysis follows a named value
// through copies, pushes and pulls and through nothing else, which is how a
// routine is seen to give a register back as it took it.
struct Symbol {
  Place place = Place::None;
  std::uint8_t part = 0;
  int offset = 0;
  friend bool operator==(const Symbol&, const Symbol&) = default;
};

// The values a place can hold as every path proves them: not known, or a set.
// One value is a fact; several are what different paths prove, or the values an
// idiom bounded an index to. `values` is sorted and distinct. A value that is not
// known may still be a `symbol`.
struct Values {
  bool known = false;
  std::vector<std::uint32_t> values;
  std::optional<Symbol> symbol;
  // Of a value that is not known, the bits that are known to be zero: a shift
  // left leaves the low bit clear, a shift right the high one, and a mask says
  // which bits it admits. Zero when the value is known.
  std::uint32_t zeroBits = 0;

  [[nodiscard]] static Values one(std::uint32_t value) { return Values{true, {value}, std::nullopt, 0}; }
  [[nodiscard]] static Values none() { return Values{}; }
  [[nodiscard]] static Values entry(Place place, std::uint8_t part) {
    return Values{false, {}, Symbol{place, part, 0}, 0};
  }

  // The one value, when there is exactly one.
  [[nodiscard]] std::optional<std::uint32_t> single() const {
    if (known && values.size() == 1) return values.front();
    return std::nullopt;
  }
  friend bool operator==(const Values&, const Values&) = default;
};

// The registers the analysis follows. The accumulator and the index registers
// are two bytes each, so an eight-bit load is known while the other byte is not;
// the direct register and the stack pointer are sixteen bits, the data bank
// eight. The program bank is the bank the instruction lies in and is not carried.
struct RegisterState {
  Values aLow, aHigh;
  Values xLow, xHigh;
  Values yLow, yHigh;
  Values d;
  Values s;
  Values dbr;

  // A register whole: the product of its two bytes, or not known when either is.
  [[nodiscard]] Values a() const;
  [[nodiscard]] Values x() const;
  [[nodiscard]] Values y() const;
  friend bool operator==(const RegisterState&, const RegisterState&) = default;
};

// The compare the instruction before made against an immediate: the register,
// the immediate, and the width. The branch after it reads the carry it set.
struct Compare {
  Place place = Place::None;
  std::uint32_t value = 0;
  unsigned bits = 8;
  friend bool operator==(const Compare&, const Compare&) = default;
};

// What is proven on a path at one point: the registers, the compare the last
// instruction made, and the bytes pushed on this path and not yet pulled — the
// top last. A pull past what the path pushed is a value the path does not know;
// a call, a store the stack could be under and two paths that pushed differently
// empty it.
struct State {
  RegisterState registers;
  std::optional<Compare> compare;
  std::vector<Values> pushed;
  friend bool operator==(const State&, const State&) = default;
};

// Where the reset vector begins: the direct register and the data bank both
// zero, which the chip clears on reset, and nothing else known.
[[nodiscard]] State resetState();

// Where an interrupt vector or an entry a person added begins: nothing known.
[[nodiscard]] State nothingProven();

// The image's byte at a bus address, or nothing where the address is not the
// image. A load answers from it and from nothing else.
using ImageReader = std::function<std::optional<std::uint8_t>(Address)>;

// The address the program places the bytes at a bus address at — the home of a
// mirror — or nothing where the address is not the image.
using Canonical = std::function<std::optional<Address>(Address)>;

// Whether the stack could lie at a bus address: the memory the stack pointer can
// point into, which the host's memory map says. A store to an address the stack
// cannot be at leaves what a path has pushed alone; one to an address it could be
// at, when the stack pointer is not known, is a store the pushed bytes could be
// under. Empty, the stack could be anywhere in bank zero.
using StackReach = std::function<bool(Address)>;

// An entry the analysis starts from, and what is proven there.
struct FlowEntry {
  Address address = 0;
  State state;
};

// A destination something else proved a jump or a call at `site` takes: a run's
// sighting, or an earlier analysis's derivation. The site's state flows to it.
struct Sighting {
  Address site = 0;
  Address target = 0;
};

// A destination the analysis proved a jump or a call through a pointer takes:
// the site, the address the pointer was read from, and where it leads. `call`
// says whether the site calls or jumps.
struct DerivedTarget {
  Address site = 0;
  Address pointer = 0;
  Address target = 0;
  bool call = false;
  friend bool operator==(const DerivedTarget&, const DerivedTarget&) = default;
};

// One access an instruction's effects make under what is proven before it: the
// effect's index, its operation and width, the addresses it can reach and the
// values it can move. A store's value is what it writes; a load's is what it
// read, known only where every address lies in the image.
struct ProvenAccess {
  std::size_t effect = 0;
  Op op = Op::Load;
  Width width = Width::Byte;
  Values address;
  Values value;
};

// One node evaluated over a state: the state after it, every access it makes,
// and the program counter and program bank its effects leave — which name the
// destinations of a jump or a call through a pointer when they are known.
struct Evaluation {
  State after;
  std::vector<ProvenAccess> accesses;
  Values pc;
  Values pbr;
};

// Runs a node's effects over `before`. The image answers loads whose every
// address lies in it; every other load is not known. `stack` says where a store
// could reach the bytes the path has pushed.
[[nodiscard]] Evaluation evaluate(const Node& node, const State& before, const ImageReader& image,
                                  const StackReach& stack = {});

// The analysis over a whole program.
class Dataflow {
 public:
  // Runs to a fixed point from `entries`, following the program's own flow and
  // `sightings`. `canonical` places every successor the way the program places
  // its nodes, so a jump through a mirror finds the node in the home bank.
  Dataflow(const Program& program, const std::vector<FlowEntry>& entries,
           const std::vector<Sighting>& sightings, ImageReader image, Canonical canonical,
           StackReach stack = {});

  // What is proven before the node, or nothing for a node no path reached.
  [[nodiscard]] const State* before(std::size_t node) const;

  // What is proven before the first node at an address, or nothing.
  [[nodiscard]] const State* before(Address address) const;

  // Every destination the analysis derived, in site order then target order.
  [[nodiscard]] const std::vector<DerivedTarget>& derived() const { return derived_; }

  // How many nodes a path reached.
  [[nodiscard]] std::size_t reachedNodes() const;

 private:
  const Program& program_;
  std::vector<std::optional<State>> before_;
  std::vector<DerivedTarget> derived_;
};

}  // namespace snaggletooth::ir
