#pragma once

// Where every value came from — the origin that travels beside a value through
// the interpreter and rests in work RAM.
//
// A cartridge builds much of what it sends to the hardware in work RAM: it
// decompresses tiles into a buffer and transfers the buffer, it assembles a
// sprite table every frame, it fills a palette from a lookup. Every byte it
// writes there was computed from bytes it read, and every image byte it reads
// has an offset. The origin of a value is the set of image offsets it was
// computed from — data dependence only: a load takes the origin of the bytes at
// the address, never of the address; an operation's result takes the union of
// its operands' origins; a constant and a flag have none. A value the image does
// not hold — a joypad reading, the multiplier's result, a byte of the save —
// carries a mark saying so instead, so "computed" never hides a source.
//
// Origins are interned: a value carries one index, a set is stored once, and a
// union of two indices is remembered. Work RAM is shadowed byte for byte with
// the origin of what it holds and the site that last wrote it — the instruction,
// as the tree places it, or the trigger of the engine that moved it — so a range
// an engine later carries out of work RAM names the image bytes it was built
// from and the routine that built it, with no reading of the code at all.
//
// An origin is exact and is not the whole story: a decoder's output takes its
// values from the literal bytes of a compressed stream, and the counts and
// lengths between them decide only how many. So the shadow also follows every
// call and return and keeps, for each invocation, the image bytes it read —
// its helpers' reads joining it when they return. The source of a staged byte
// is then the run of image bytes its writer's invocation read that holds the
// byte's origin: the compressed stream whole, counts included, and not a table
// the decoder consulted whose values never reached the output.
//
// The shadow is beside the interpreter, never in it: nothing here reads a
// value, and a run with a shadow is checked against the machine exactly as a
// run without one.

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

#include "ir/ir_interpret.h"
#include "snaggletooth/snes/cartridge.h"

namespace snaggletooth::ir {

// One run of image offsets, inclusive at both ends.
struct OriginInterval {
  std::size_t first = 0;
  std::size_t last = 0;
  friend auto operator<=>(const OriginInterval&, const OriginInterval&) = default;
};

// Where a value came from: the image offsets its value was computed from, as
// intervals in ascending order with no two touching; the hardware registers
// whose value entered it, as `$00:XXXX` addresses in ascending order; whether a
// byte of the save entered it. `approximate` marks a set widened to its hull
// because it had more intervals than the cap — a hull still contains every
// source, so a lift from it contains the asset. A set with nothing in it is the
// origin of a value built from constants alone.
struct OriginSet {
  std::vector<OriginInterval> image;
  std::vector<std::uint32_t> registers;
  bool save = false;
  bool approximate = false;

  [[nodiscard]] bool empty() const noexcept { return image.empty() && registers.empty() && !save; }
  // The image bytes the intervals cover.
  [[nodiscard]] std::size_t imageBytes() const noexcept;
  friend auto operator<=>(const OriginSet&, const OriginSet&) = default;
};

// An origin as a value carries it: an index into the table, zero for nothing.
using Origin = std::uint32_t;
constexpr Origin kNoOrigin = 0;

// The table every origin is interned in. A set is stored once and named by its
// index; the union of two indices is computed once and remembered. Above `cap`
// intervals a union is widened to its hull and marked approximate.
class Origins {
 public:
  explicit Origins(std::size_t cap);

  // The origin of one image byte.
  [[nodiscard]] Origin image(std::size_t offset);
  // The origin of a value read from a hardware register, named by its address.
  [[nodiscard]] Origin hardwareRegister(std::uint32_t address);
  // The origin of a byte of the save.
  [[nodiscard]] Origin save();
  // The union of two origins.
  [[nodiscard]] Origin unite(Origin a, Origin b);
  // Adds an origin into a set that is being accumulated outside the table — a
  // range's bytes folded one by one — with no cap and no interning.
  void accumulate(OriginSet& into, Origin origin) const;
  // The same for two sets held outside the table.
  static void merge(OriginSet& into, const OriginSet& from);

  [[nodiscard]] const OriginSet& of(Origin origin) const noexcept { return sets_[origin]; }
  [[nodiscard]] std::size_t interned() const noexcept { return sets_.size(); }
  [[nodiscard]] std::size_t cap() const noexcept { return cap_; }

 private:
  Origin intern(OriginSet set);

  std::size_t cap_;
  std::vector<OriginSet> sets_;
  std::map<OriginSet, Origin> index_;
  std::unordered_map<std::size_t, Origin> images_;
  std::unordered_map<std::uint32_t, Origin> registers_;
  Origin save_ = kNoOrigin;
  std::unordered_map<std::uint64_t, Origin> unions_;
};

// Who last wrote a byte of work RAM: an instruction, as the tree places it, or
// the trigger of the engine that moved the byte there. A byte nothing wrote
// since power-on has no writer, and `writerOf` says so.
struct Writer {
  Address site = 0;
  bool engine = false;
  friend auto operator<=>(const Writer&, const Writer&) = default;
};

// One store the CPU made to a data register from a value with one image byte
// as its origin, and the stores after it that continued the sequence: the site
// of the first, the register — the first of a pair, for `VMDATAL`/`VMDATAH`
// and the audio ports — the image offset the first byte came from and how many
// consecutive offsets followed it. A store continues the sequence when its
// value's origin is the next image offset and it was made at the same site as
// the last store or on one straight run from it — no jump, branch, call or
// return between the two.
struct Stream {
  Address site = 0;
  std::uint32_t registerAddress = 0;  // as `$00:XXXX`
  std::size_t first = 0;
  std::size_t bytes = 0;
  std::uint32_t times = 1;
};

// The shadow of a run: every place's origin byte by byte, work RAM's origin and
// last writer byte by byte, and the streams the CPU carried a byte at a time.
// The interpreter drives it through `Shadow`; the host that runs the machine
// beside the interpreter tells it what the engines and the port did, since
// those bytes never pass through the interpreter.
//
// Before every instruction the host sets `site` — the instruction, as the tree
// places it — and says with `flowBroke` when the CPU did not arrive there by
// falling through; every byte the instruction writes to work RAM is written by
// `site`.
class Provenance final : public Shadow {
 public:
  Provenance(CartridgeMap map, std::size_t imageBytes, std::size_t cap);

  Address site = 0;
  void flowBroke() noexcept { broken_ = true; }

  // The origin of what the bus holds at `address`: the image byte's, through
  // any mirror; the shadow's, for work RAM through any mirror; a register mark
  // for the hardware registers; the save mark for the save window; nothing
  // for open bus. `$2180` is the port, and what it holds is what the port
  // reaches — the host says where through `portRead`.
  [[nodiscard]] Origin at(Address address);

  // A byte landed in work RAM at `address` carrying `origin`, written by
  // `writer`. Anywhere but work RAM, nothing changes.
  void written(Address address, Origin origin, Writer writer);

  // The shadow of a work-RAM byte, through any mirror: nothing for an address
  // outside work RAM, and no writer for a byte nothing has written.
  [[nodiscard]] std::optional<Origin> originOf(Address address) const noexcept;
  [[nodiscard]] std::optional<Writer> writerOf(Address address) const noexcept;

  // The source of a work-RAM byte: for each interval of its origin, the run of
  // image bytes the invocation that wrote it read that holds the interval, or
  // the interval itself when no read holds it — the byte was staged through
  // work RAM, or an engine wrote it. An approximate origin takes every run the
  // hull overlaps. Ascending, no two touching; empty when the byte has no
  // image origin.
  [[nodiscard]] std::vector<OriginInterval> sourcesOf(Address address) const;

  // A call the CPU made, and a return: an invocation begins, and ends with its
  // reads joining its caller's. A hardware interrupt taken is a call.
  void called();
  void returned();

  // The work-RAM addresses the port reached, in order, for the accesses the
  // CPU is about to make to `$2180` in the instruction being run: the machine
  // reports the port's own access before the CPU's, and the interpreter runs
  // after both. The host queues them before the instruction and clears them
  // after.
  std::vector<Address> portReads;
  std::vector<Address> portWrites;

  // A step the interpreter did not run — its bytes did not decode, or the
  // machine realigned it — leaves the places' origins unknown: nothing, and
  // said so.
  void forgetPlaces() noexcept;

  // The streams closed so far, each distinct one once with its count, and
  // whatever is open at the end of the run.
  [[nodiscard]] const std::vector<Stream>& streams() const noexcept { return streams_; }
  void finish();

  [[nodiscard]] Origins& origins() noexcept { return origins_; }
  [[nodiscard]] const Origins& origins() const noexcept { return origins_; }

  // The place-level moves the interpreter reports.
  void copy(Place dst, unsigned bits, Place a) override;
  void combine(Place dst, unsigned bits, Place a, Place b, Place c) override;
  void load(Place dst, unsigned bits, unsigned byte, Address address) override;
  void store(Address address, Place source, unsigned byte) override;
  void exchange() override;

 private:
  static constexpr std::size_t kPlaces = 24;
  static constexpr std::size_t kBytes = 4;

  [[nodiscard]] static bool carries(Place place) noexcept;
  [[nodiscard]] Origin& slot(Place place, unsigned byte) noexcept;
  [[nodiscard]] Origin unionOf(Place place);
  void wrote(Place place, unsigned bits) noexcept;
  [[nodiscard]] static std::optional<std::size_t> workRamIndex(Address address) noexcept;
  [[nodiscard]] static bool inSystemBank(Address address) noexcept;
  [[nodiscard]] static std::optional<std::uint32_t> dataRegister(Address address) noexcept;
  void streamed(std::uint32_t registerAddress, Origin origin);
  void closeStream(std::uint32_t registerAddress);

  // What the bus holds at an address, as `at` says, recorded as a read of the
  // running invocation when it is the image.
  [[nodiscard]] Origin read(Address address);
  void noteRead(std::size_t offset);
  void release(std::uint32_t invocation);

  CartridgeMap map_;
  std::size_t imageBytes_;
  Origins origins_;
  Origin places_[kPlaces][kBytes]{};
  std::vector<Origin> workRam_;
  std::vector<Writer> writers_;
  std::vector<bool> written_;

  // The invocations: the call stack, the root first, each with the image bytes
  // it has read as intervals; the ones that returned having written a byte of
  // work RAM still tagged with them; and the tag of every work-RAM byte, zero
  // for an engine's write. `refs_` counts the bytes each invocation still
  // owns, so a returned one is dropped when its last byte is overwritten.
  struct Invocation {
    std::uint32_t id = 0;
    std::map<std::size_t, std::size_t> reads;  // first -> last
    bool wrote = false;
  };
  std::vector<Invocation> frames_;
  std::uint32_t nextInvocation_ = 1;
  std::unordered_map<std::uint32_t, std::vector<OriginInterval>> kept_;
  std::unordered_map<std::uint32_t, std::uint32_t> refs_;
  std::vector<std::uint32_t> invocation_;

  struct OpenStream {
    Stream stream;
    Address lastSite = 0;  // the site of the last store that continued it
  };
  std::map<std::uint32_t, OpenStream> open_;
  std::vector<Stream> streams_;
  bool broken_ = false;  // the flow left a straight run since the last store to a data register
};

}  // namespace snaggletooth::ir
