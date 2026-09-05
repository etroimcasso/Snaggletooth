#include "ir/ir_provenance.h"

#include <algorithm>
#include <iterator>
#include <utility>

namespace snaggletooth::ir {
namespace {

std::size_t intervalFirst(const OriginInterval& i) { return i.first; }
std::size_t intervalLast(const OriginInterval& i) { return i.last; }
std::size_t intervalFirst(const std::pair<const std::size_t, std::size_t>& i) { return i.first; }
std::size_t intervalLast(const std::pair<const std::size_t, std::size_t>& i) { return i.second; }

// Merges `from`'s intervals into `into`'s, keeping them ascending and joining
// any two that touch.
void mergeIntervals(std::vector<OriginInterval>& into, const std::vector<OriginInterval>& from) {
  if (from.empty()) return;
  std::vector<OriginInterval> merged;
  merged.reserve(into.size() + from.size());
  std::merge(into.begin(), into.end(), from.begin(), from.end(), std::back_inserter(merged));
  std::vector<OriginInterval> out;
  out.reserve(merged.size());
  for (const OriginInterval& interval : merged) {
    if (!out.empty() && interval.first <= out.back().last + 1u) {
      out.back().last = std::max(out.back().last, interval.last);
    } else {
      out.push_back(interval);
    }
  }
  into = std::move(out);
}

void mergeRegisters(std::vector<std::uint32_t>& into, const std::vector<std::uint32_t>& from) {
  if (from.empty()) return;
  std::vector<std::uint32_t> merged;
  merged.reserve(into.size() + from.size());
  std::set_union(into.begin(), into.end(), from.begin(), from.end(), std::back_inserter(merged));
  into = std::move(merged);
}

void mergeSets(OriginSet& into, const OriginSet& from) {
  mergeIntervals(into.image, from.image);
  mergeRegisters(into.registers, from.registers);
  into.save = into.save || from.save;
  into.approximate = into.approximate || from.approximate;
}

// Adds [first, last] to a map of intervals, joining it with any it touches.
void insertInterval(std::map<std::size_t, std::size_t>& into, std::size_t first, std::size_t last) {
  auto it = into.upper_bound(first);
  if (it != into.begin()) {
    auto before = std::prev(it);
    if (before->second + 1u >= first) {
      // Touches the one before: extend it.
      first = before->first;
      last = std::max(last, before->second);
      into.erase(before);
    }
  }
  it = into.lower_bound(first);
  while (it != into.end() && it->first <= last + 1u) {
    last = std::max(last, it->second);
    it = into.erase(it);
  }
  into.emplace(first, last);
}

// The interval in a sorted, disjoint list that holds `offset`, if one does.
template <typename Range>
std::optional<OriginInterval> holding(const Range& intervals, std::size_t offset) {
  auto it = std::upper_bound(intervals.begin(), intervals.end(), offset,
                             [](std::size_t o, const auto& interval) { return o < intervalFirst(interval); });
  if (it == intervals.begin()) return std::nullopt;
  --it;
  if (intervalLast(*it) < offset) return std::nullopt;
  return OriginInterval{.first = intervalFirst(*it), .last = intervalLast(*it)};
}

}  // namespace

std::size_t OriginSet::imageBytes() const noexcept {
  std::size_t total = 0;
  for (const OriginInterval& interval : image) total += interval.last - interval.first + 1u;
  return total;
}

Origins::Origins(std::size_t cap) : cap_(cap) {
  sets_.push_back(OriginSet{});  // index zero: nothing
  index_.emplace(OriginSet{}, kNoOrigin);
}

Origin Origins::intern(OriginSet set) {
  const auto found = index_.find(set);
  if (found != index_.end()) return found->second;
  const Origin origin = static_cast<Origin>(sets_.size());
  sets_.push_back(set);
  index_.emplace(std::move(set), origin);
  return origin;
}

Origin Origins::image(std::size_t offset) {
  const auto found = images_.find(offset);
  if (found != images_.end()) return found->second;
  OriginSet set;
  set.image.push_back(OriginInterval{.first = offset, .last = offset});
  const Origin origin = intern(std::move(set));
  images_.emplace(offset, origin);
  return origin;
}

Origin Origins::hardwareRegister(std::uint32_t address) {
  const auto found = registers_.find(address);
  if (found != registers_.end()) return found->second;
  OriginSet set;
  set.registers.push_back(address);
  const Origin origin = intern(std::move(set));
  registers_.emplace(address, origin);
  return origin;
}

Origin Origins::save() {
  if (save_ == kNoOrigin) {
    OriginSet set;
    set.save = true;
    save_ = intern(std::move(set));
  }
  return save_;
}

Origin Origins::unite(Origin a, Origin b) {
  if (a == b || b == kNoOrigin) return a;
  if (a == kNoOrigin) return b;
  const std::uint64_t key = (static_cast<std::uint64_t>(std::min(a, b)) << 32) | std::max(a, b);
  const auto found = unions_.find(key);
  if (found != unions_.end()) return found->second;
  OriginSet set = sets_[a];
  mergeSets(set, sets_[b]);
  if (set.image.size() > cap_) {
    // Wider than the cap: the hull, and said to be.
    set.image = {OriginInterval{.first = set.image.front().first, .last = set.image.back().last}};
    set.approximate = true;
  }
  const Origin origin = intern(std::move(set));
  unions_.emplace(key, origin);
  return origin;
}

void Origins::accumulate(OriginSet& into, Origin origin) const {
  if (origin == kNoOrigin) return;
  mergeSets(into, sets_[origin]);
}

void Origins::merge(OriginSet& into, const OriginSet& from) { mergeSets(into, from); }

// ---- the shadow of a run --------------------------------------------------------

Provenance::Provenance(CartridgeMap map, std::size_t imageBytes, std::size_t cap)
    : map_(map), imageBytes_(imageBytes), origins_(cap), workRam_(0x20000u, kNoOrigin),
      writers_(0x20000u), written_(0x20000u, false), invocation_(0x20000u, 0u) {
  frames_.push_back(Invocation{.id = nextInvocation_++, .reads = {}, .wrote = false});
}

bool Provenance::carries(Place place) noexcept {
  switch (place) {
    case Place::A:
    case Place::X:
    case Place::Y:
    case Place::S:
    case Place::D:
    case Place::PC:
    case Place::PBR:
    case Place::DBR:
    case Place::T0:
    case Place::T1:
    case Place::T2:
    case Place::T3:
      return true;
    default:
      return false;  // a constant, a flag, the status register, the emulation flag
  }
}

Origin& Provenance::slot(Place place, unsigned byte) noexcept {
  return places_[static_cast<std::size_t>(place)][byte];
}

Origin Provenance::unionOf(Place place) {
  if (!carries(place)) return kNoOrigin;
  Origin out = kNoOrigin;
  for (unsigned byte = 0; byte < kBytes; ++byte) out = origins_.unite(out, slot(place, byte));
  return out;
}

// After a write of `bits` to `place`: the register's own rule for the bytes
// above the write — the accumulator keeps its high byte, the rest clear theirs.
void Provenance::wrote(Place place, unsigned bits) noexcept {
  if (!carries(place)) {
    for (unsigned byte = 0; byte < kBytes; ++byte) slot(place, byte) = kNoOrigin;
    return;
  }
  const bool keepsHigh = place == Place::A && bits == 8;
  for (unsigned byte = bits / 8; byte < kBytes; ++byte) {
    if (keepsHigh && byte == 1) continue;
    slot(place, byte) = kNoOrigin;
  }
}

std::optional<std::size_t> Provenance::workRamIndex(Address address) noexcept {
  const std::uint32_t bank = (address >> 16) & 0xFFu;
  const std::uint32_t offset = address & 0xFFFFu;
  if (bank == 0x7Eu || bank == 0x7Fu) return ((bank - 0x7Eu) << 16) | offset;
  if (inSystemBank(address) && offset < 0x2000u) return offset;
  return std::nullopt;
}

bool Provenance::inSystemBank(Address address) noexcept {
  const std::uint32_t bank = (address >> 16) & 0xFFu;
  return bank <= 0x3Fu || (bank >= 0x80u && bank <= 0xBFu);
}

// The data registers a stream is carried into, each pair named by its first.
std::optional<std::uint32_t> Provenance::dataRegister(Address address) noexcept {
  if (!inSystemBank(address)) return std::nullopt;
  switch (address & 0xFFFFu) {
    case 0x2118u:
    case 0x2119u:
      return 0x2118u;  // VMDATAL, VMDATAH
    case 0x2122u:
      return 0x2122u;  // CGDATA
    case 0x2104u:
      return 0x2104u;  // OAMDATA
    case 0x2140u:
    case 0x2141u:
      return 0x2140u;  // APUIO0, APUIO1
    case 0x2142u:
    case 0x2143u:
      return 0x2142u;  // APUIO2, APUIO3
    default:
      return std::nullopt;
  }
}

Origin Provenance::at(Address address) {
  address &= 0xFFFFFFu;
  if (const std::optional<std::size_t> index = workRamIndex(address)) return workRam_[*index];
  if (inSystemBank(address)) {
    const std::uint32_t offset = address & 0xFFFFu;
    if (offset == 0x2180u) {
      // The port: what it holds is the work-RAM byte it reached.
      if (portReads.empty()) return origins_.hardwareRegister(offset);
      const Address reached = portReads.front();
      portReads.erase(portReads.begin());
      const std::optional<std::size_t> index = workRamIndex(reached);
      return index ? workRam_[*index] : kNoOrigin;
    }
    if ((offset >= 0x2100u && offset <= 0x21FFu) || (offset >= 0x4000u && offset <= 0x43FFu)) {
      return origins_.hardwareRegister(offset);
    }
  }
  if (const std::optional<std::size_t> offset = romOffset(map_, address, imageBytes_)) {
    return origins_.image(*offset);
  }
  if (saveRamOffset(map_, address)) return origins_.save();
  return kNoOrigin;
}

void Provenance::written(Address address, Origin origin, Writer writer) {
  const std::optional<std::size_t> index = workRamIndex(address & 0xFFFFFFu);
  if (!index) return;
  workRam_[*index] = origin;
  writers_[*index] = writer;
  written_[*index] = true;
  // The byte changes hands: the invocation that owned it lets go, and the
  // running one takes it — or nobody does, for an engine's write.
  if (const std::uint32_t old = invocation_[*index]; old != 0) release(old);
  if (writer.engine) {
    invocation_[*index] = 0;
    return;
  }
  Invocation& running = frames_.back();
  running.wrote = true;
  invocation_[*index] = running.id;
  ++refs_[running.id];
}

void Provenance::release(std::uint32_t invocation) {
  const auto found = refs_.find(invocation);
  if (found == refs_.end()) return;
  if (--found->second != 0) return;
  refs_.erase(found);
  kept_.erase(invocation);
}

void Provenance::noteRead(std::size_t offset) { insertInterval(frames_.back().reads, offset, offset); }

Origin Provenance::read(Address address) {
  address &= 0xFFFFFFu;
  if (const std::optional<std::size_t> offset = romOffset(map_, address, imageBytes_)) noteRead(*offset);
  return at(address);
}

void Provenance::called() {
  frames_.push_back(Invocation{.id = nextInvocation_++, .reads = {}, .wrote = false});
}

void Provenance::returned() {
  if (frames_.size() < 2) return;  // the root never returns; a return with nothing to pop is the code's own stack play
  Invocation ended = std::move(frames_.back());
  frames_.pop_back();
  Invocation& caller = frames_.back();
  for (const auto& [first, last] : ended.reads) insertInterval(caller.reads, first, last);
  if (!ended.wrote || refs_.find(ended.id) == refs_.end()) return;
  std::vector<OriginInterval> reads;
  reads.reserve(ended.reads.size());
  for (const auto& [first, last] : ended.reads) reads.push_back(OriginInterval{.first = first, .last = last});
  kept_.emplace(ended.id, std::move(reads));
}

std::vector<OriginInterval> Provenance::sourcesOf(Address address) const {
  std::vector<OriginInterval> out;
  const std::optional<std::size_t> index = workRamIndex(address & 0xFFFFFFu);
  if (!index) return out;
  const OriginSet& origin = origins_.of(workRam_[*index]);
  if (origin.image.empty()) return out;
  // The reads of the invocation that wrote the byte: on the stack still, or kept.
  const std::uint32_t id = invocation_[*index];
  const std::map<std::size_t, std::size_t>* live = nullptr;
  const std::vector<OriginInterval>* kept = nullptr;
  if (id != 0) {
    for (const Invocation& frame : frames_) {
      if (frame.id == id) live = &frame.reads;
    }
    if (live == nullptr) {
      const auto found = kept_.find(id);
      if (found != kept_.end()) kept = &found->second;
    }
  }
  auto holder = [&](std::size_t offset) -> std::optional<OriginInterval> {
    if (live != nullptr) return holding(*live, offset);
    if (kept != nullptr) return holding(*kept, offset);
    return std::nullopt;
  };
  std::vector<OriginInterval> found;
  for (const OriginInterval& interval : origin.image) {
    if (!origin.approximate) {
      if (const std::optional<OriginInterval> run = holder(interval.first)) {
        found.push_back(*run);
      } else {
        found.push_back(interval);
      }
      continue;
    }
    // A hull: every run it overlaps, and the hull itself where none does.
    bool any = false;
    for (std::size_t offset = interval.first; offset <= interval.last;) {
      const std::optional<OriginInterval> run = holder(offset);
      if (run) {
        found.push_back(*run);
        any = true;
        offset = run->last + 1u;
      } else {
        ++offset;
      }
    }
    if (!any) found.push_back(interval);
  }
  std::sort(found.begin(), found.end());
  mergeIntervals(out, found);
  return out;
}

std::optional<Origin> Provenance::originOf(Address address) const noexcept {
  const std::optional<std::size_t> index = workRamIndex(address & 0xFFFFFFu);
  if (!index) return std::nullopt;
  return workRam_[*index];
}

std::optional<Writer> Provenance::writerOf(Address address) const noexcept {
  const std::optional<std::size_t> index = workRamIndex(address & 0xFFFFFFu);
  if (!index || !written_[*index]) return std::nullopt;
  return writers_[*index];
}

void Provenance::forgetPlaces() noexcept {
  for (auto& place : places_) {
    for (Origin& byte : place) byte = kNoOrigin;
  }
}

void Provenance::copy(Place dst, unsigned bits, Place a) {
  for (unsigned byte = 0; byte < bits / 8; ++byte) {
    slot(dst, byte) = carries(a) ? slot(a, byte) : kNoOrigin;
  }
  wrote(dst, bits);
}

void Provenance::combine(Place dst, unsigned bits, Place a, Place b, Place c) {
  const Origin all = origins_.unite(unionOf(a), origins_.unite(unionOf(b), unionOf(c)));
  for (unsigned byte = 0; byte < bits / 8; ++byte) slot(dst, byte) = all;
  wrote(dst, bits);
}

void Provenance::load(Place dst, unsigned bits, unsigned byte, Address address) {
  slot(dst, byte) = read(address);
  if (byte + 1u == bits / 8) wrote(dst, bits);
}

void Provenance::store(Address address, Place source, unsigned byte) {
  address &= 0xFFFFFFu;
  const Origin origin = carries(source) ? slot(source, byte) : kNoOrigin;
  if (inSystemBank(address) && (address & 0xFFFFu) == 0x2180u) {
    // The port: the byte landed where the port reached.
    if (portWrites.empty()) return;
    const Address reached = portWrites.front();
    portWrites.erase(portWrites.begin());
    written(reached, origin, Writer{.site = site, .engine = false});
    return;
  }
  if (const std::optional<std::uint32_t> reg = dataRegister(address)) {
    streamed(*reg, origin);
    return;
  }
  written(address, origin, Writer{.site = site, .engine = false});
}

void Provenance::exchange() { std::swap(slot(Place::A, 0), slot(Place::A, 1)); }

// A store to a data register: the sequence its register has open continues
// when the value came from exactly the next image byte and the store was made
// at the same site as the last or on one straight run from it; otherwise the
// sequence closes, and a value from exactly one image byte opens a new one.
void Provenance::streamed(std::uint32_t registerAddress, Origin origin) {
  const OriginSet& set = origins_.of(origin);
  const bool single = set.image.size() == 1 && set.image.front().first == set.image.front().last &&
                      set.registers.empty() && !set.save && !set.approximate;
  const auto found = open_.find(registerAddress);
  const bool straight = !broken_;
  broken_ = false;
  if (found != open_.end()) {
    OpenStream& open = found->second;
    const bool next = single && set.image.front().first == open.stream.first + open.stream.bytes;
    if (next && (site == open.lastSite || straight)) {
      ++open.stream.bytes;
      open.lastSite = site;
      return;
    }
    closeStream(registerAddress);
  }
  if (!single) return;
  open_.emplace(registerAddress, OpenStream{.stream = Stream{.site = site,
                                                            .registerAddress = registerAddress,
                                                            .first = set.image.front().first,
                                                            .bytes = 1,
                                                            .times = 1},
                                           .lastSite = site});
}

void Provenance::closeStream(std::uint32_t registerAddress) {
  const auto found = open_.find(registerAddress);
  if (found == open_.end()) return;
  const Stream& stream = found->second.stream;
  if (stream.bytes >= 2) {
    const auto same = std::find_if(streams_.begin(), streams_.end(), [&](const Stream& s) {
      return s.site == stream.site && s.registerAddress == stream.registerAddress &&
             s.first == stream.first && s.bytes == stream.bytes;
    });
    if (same == streams_.end()) {
      streams_.push_back(stream);
    } else {
      ++same->times;
    }
  }
  open_.erase(found);
}

void Provenance::finish() {
  while (!open_.empty()) closeStream(open_.begin()->first);
}

}  // namespace snaggletooth::ir
