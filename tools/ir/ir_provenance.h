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
// call and return and keeps, for each invocation, the stretches of the image
// it read as runs: a run is a maximal stretch of image bytes the invocation
// read — a read inside a run changes nothing, any other read starts a run of
// the one byte, and runs that come to touch are one run — and a helper's runs
// join its caller's, when it returns, as if the caller had read them. So two
// files read a chunk at a time turn and turn about stay two runs, a decoder's
// stream stays one however many tables it consults and however often it
// re-reads a byte, and a copy that walks its source from the end reads one
// run. The source of a staged byte is the run holding its origin among the
// writer's invocation's runs, followed outward: a caller's run that begins or
// ends where it does replaces it — that caller read on from it, so a decoder
// called once per chunk has the whole file for its source — and the search
// ends at a caller whose run holds it strictly inside, or holds none: the
// compressed stream whole, counts included, and not a table the decoder
// consulted whose values never reached the output.
//
// A byte the CPU loads from work RAM keeps, beside its origin, the address it
// was loaded from until something computes with it, so a store of it to a data
// register is the buffer being carried out, and the host is told byte by byte
// — the buffer is an extent the run carried exactly as an engine would have.
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

// One store the CPU made to a data register, and the stores after it that
// continued the sequence: the site of the first, the register — the first of a
// pair, for `VMDATAL`/`VMDATAH` and the audio ports — where the first byte came
// from and how many consecutive ones followed it. A store continues the
// sequence when its value is the next byte and it was made at the same site as
// the last store or on one straight run from it — no jump, branch, call or
// return between the two. A sequence is one of two kinds. A value loaded from
// work RAM and stored as it was is the buffer at `memory` being carried out,
// and the next byte is the next address; the host is told each byte through
// `CarrySink`, and `first` and `source` mean nothing. Otherwise a value with
// exactly one image byte as its origin is the image at `first` being carried,
// the next byte the next offset, and `source` is the run holding `first`
// among those the invocation that carried it read, followed out through its
// callers exactly as a staged byte's source is — the file the stream is
// lifted as, which may be wider than the bytes carried. Where the bytes went
// on the other side of the port — the lowest and the highest VRAM word,
// palette word or OAM byte the port put one at — is what the host reported
// for each store through `CarrySink::landedAt`; a stream to the audio ports
// has none.
struct Stream {
  Address site = 0;
  std::uint32_t registerAddress = 0;  // as `$00:XXXX`
  std::size_t first = 0;
  std::size_t bytes = 0;
  std::uint32_t times = 1;
  OriginInterval source;
  std::optional<Address> memory;
  bool landed = false;
  std::uint16_t lowest = 0;
  std::uint16_t highest = 0;
};

// What a host does with the bytes the CPU carries out of work RAM to a data
// register, told as they go so it can read the shadow under each byte at that
// moment — the buffer may be rebuilt before the sequence ends; and what it
// knows about the port the bytes went through, which the shadow does not.
class CarrySink {
 public:
  virtual ~CarrySink() = default;
  // The CPU stored the byte it had loaded from work RAM at `memory` to the
  // register; `continues` says it was the next byte of the sequence open for
  // the register rather than the first of a new one.
  virtual void carriedByte(std::uint32_t registerAddress, Address memory, bool continues) = 0;
  // The sequence open for the register ended; `recorded` says it was two
  // bytes or more and is among `streams()`.
  virtual void carryEnded(std::uint32_t registerAddress, bool recorded) = 0;
  // Where the port put the byte the instruction being run stored to the
  // register — the pair's first, for `VMDATAL`/`VMDATAH` and the audio ports
  // — as the machine reported it for that store; nothing for a register whose
  // bytes land in no memory, or for a host that does not know.
  virtual std::optional<std::uint16_t> landedAt(std::uint32_t registerAddress) {
    (void)registerAddress;
    return std::nullopt;
  }
  // A stream of two bytes or more closed, as this closing saw it: its count
  // is one, and its landing is this closing's own.
  virtual void streamClosed(const Stream& stream) { (void)stream; }
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

  // Told about every byte the CPU carries out of work RAM, or nobody.
  CarrySink* carries = nullptr;

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

  // The source of a work-RAM byte: for each interval of its origin, the run
  // holding the interval among those the invocation that wrote it read,
  // followed out through the callers whose run grew from it, or the interval
  // itself when no run holds it — the byte was staged through work RAM, or an
  // engine wrote it. An
  // approximate origin takes every run the hull overlaps. Ascending, no two
  // touching; empty when the byte has no image origin.
  [[nodiscard]] std::vector<OriginInterval> sourcesOf(Address address) const;

  // A call the CPU made, and a return: an invocation begins, and ends with its
  // runs joining its caller's. A hardware interrupt taken is a call.
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

  static constexpr std::uint32_t kNotLoaded = 0xFFFFFFFFu;

  [[nodiscard]] static bool carriesOrigin(Place place) noexcept;
  [[nodiscard]] Origin& slot(Place place, unsigned byte) noexcept;
  [[nodiscard]] std::uint32_t& loaded(Place place, unsigned byte) noexcept;
  [[nodiscard]] Origin unionOf(Place place);
  void wrote(Place place, unsigned bits) noexcept;
  [[nodiscard]] static std::optional<std::size_t> workRamIndex(Address address) noexcept;
  [[nodiscard]] static Address workRamAddress(std::size_t index) noexcept;
  [[nodiscard]] static bool inSystemBank(Address address) noexcept;
  [[nodiscard]] static std::optional<std::uint32_t> dataRegister(Address address) noexcept;
  void streamed(std::uint32_t registerAddress, Origin origin, std::uint32_t loadedFrom);
  void closeStream(std::uint32_t registerAddress);

  // What the bus holds at an address, as `at` says, recorded as a read of the
  // running invocation when it is the image; `reached_` is the work-RAM byte
  // it was, when it was one.
  [[nodiscard]] Origin read(Address address);
  void noteRead(std::size_t offset);
  void release(std::uint32_t invocation);
  void hold(std::uint32_t invocation);

  // One run of image bytes an invocation read, inclusive at both ends.
  struct Run {
    std::size_t first = 0;
    std::size_t last = 0;
  };
  // The runs of one invocation, by first byte, no two overlapping or
  // touching.
  struct Runs {
    std::map<std::size_t, Run> byFirst;
    void read(std::size_t offset);
    void join(const Run& run);
    [[nodiscard]] std::optional<OriginInterval> holding(std::size_t offset) const;
    void place(Run run);  // enters `run`, taking into it every run it touches or overlaps
  };

  // The invocations: the call stack, the root first, each with its runs; the
  // ones that returned and are still held — by a byte of work RAM tagged with
  // them, by a returned invocation whose runs joined theirs, or by an open
  // stream they carried — with the invocation their runs joined; and the tag
  // of every work-RAM byte, zero for an engine's write. `refs_` counts what
  // holds each invocation, so a returned one is dropped when nothing does.
  struct Invocation {
    std::uint32_t id = 0;
    Runs runs;
  };
  struct Kept {
    Runs runs;
    std::uint32_t parent = 0;
  };
  std::vector<Invocation> frames_;
  std::unordered_map<std::uint32_t, std::size_t> frameIndex_;  // a live invocation's place on the stack
  std::uint32_t nextInvocation_ = 1;
  std::unordered_map<std::uint32_t, Kept> kept_;
  std::unordered_map<std::uint32_t, std::uint32_t> refs_;
  std::vector<std::uint32_t> invocation_;

  // The run holding `offset` among the invocation's own, followed out through
  // the callers whose run begins or ends where it does.
  [[nodiscard]] std::optional<OriginInterval> sourceRun(std::uint32_t invocation, std::size_t offset) const;

  CartridgeMap map_;
  std::size_t imageBytes_;
  Origins origins_;
  Origin places_[kPlaces][kBytes]{};
  std::uint32_t loaded_[kPlaces][kBytes]{};  // the work-RAM index each byte was loaded from, or `kNotLoaded`
  std::optional<std::size_t> reached_;
  std::vector<Origin> workRam_;
  std::vector<Writer> writers_;
  std::vector<bool> written_;

  struct OpenStream {
    Stream stream;
    Address lastSite = 0;      // the site of the last store that continued it
    std::uint32_t carrier = 0;  // the invocation running at the last store, held while open
  };
  std::map<std::uint32_t, OpenStream> open_;
  std::vector<Stream> streams_;
  bool broken_ = false;  // the flow left a straight run since the last store to a data register
};

}  // namespace snaggletooth::ir
