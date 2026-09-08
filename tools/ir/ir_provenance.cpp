#include "ir/ir_provenance.h"

#include <algorithm>
#include <iterator>
#include <utility>

namespace snaggletooth::ir {
namespace {

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

// ---- the runs an invocation reads ----------------------------------------------

// Enters a run, taking into it the run it begins inside or just past, and
// every run that begins within it or just past its last — so no two runs
// ever overlap or touch.
void Provenance::Runs::place(Run run) {
  if (auto before = byFirst.upper_bound(run.first); before != byFirst.begin()) {
    --before;
    if (before->second.last + 1u >= run.first) {
      run.first = before->first;
      run.last = std::max(run.last, before->second.last);
      byFirst.erase(before);
    }
  }
  for (auto it = byFirst.lower_bound(run.first); it != byFirst.end() && it->first <= run.last + 1u;) {
    run.last = std::max(run.last, it->second.last);
    it = byFirst.erase(it);
  }
  byFirst[run.first] = run;
}

// A read inside a run changes nothing; any other read starts a run of the
// one byte, which `place` joins to whatever it touches.
void Provenance::Runs::read(std::size_t offset) {
  if (holding(offset)) return;
  place(Run{.first = offset, .last = offset});
}

// A helper's run joining its caller's: exactly as if the caller had read
// its bytes.
void Provenance::Runs::join(const Run& run) { place(run); }

// The run holding `offset`: at most one, since no two overlap.
std::optional<OriginInterval> Provenance::Runs::holding(std::size_t offset) const {
  auto it = byFirst.upper_bound(offset);
  if (it == byFirst.begin()) return std::nullopt;
  --it;
  if (it->second.last < offset) return std::nullopt;
  return OriginInterval{.first = it->second.first, .last = it->second.last};
}

// ---- the shadow of a run --------------------------------------------------------

Provenance::Provenance(CartridgeMap map, std::size_t imageBytes, std::size_t cap)
    : invocation_(0x20000u, 0u), map_(map), imageBytes_(imageBytes), origins_(cap),
      workRam_(0x20000u, kNoOrigin), writers_(0x20000u), written_(0x20000u, false) {
  frames_.push_back(Invocation{.id = nextInvocation_++, .runs = {}});
  frameIndex_.emplace(frames_.back().id, 0u);
  for (auto& place : loaded_) {
    for (std::uint32_t& byte : place) byte = kNotLoaded;
  }
}

bool Provenance::carriesOrigin(Place place) noexcept {
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

std::uint32_t& Provenance::loaded(Place place, unsigned byte) noexcept {
  return loaded_[static_cast<std::size_t>(place)][byte];
}

Origin Provenance::unionOf(Place place) {
  if (!carriesOrigin(place)) return kNoOrigin;
  Origin out = kNoOrigin;
  for (unsigned byte = 0; byte < kBytes; ++byte) out = origins_.unite(out, slot(place, byte));
  return out;
}

// After a write of `bits` to `place`: the register's own rule for the bytes
// above the write — the accumulator keeps its high byte, the rest clear theirs.
void Provenance::wrote(Place place, unsigned bits) noexcept {
  if (!carriesOrigin(place)) {
    for (unsigned byte = 0; byte < kBytes; ++byte) {
      slot(place, byte) = kNoOrigin;
      loaded(place, byte) = kNotLoaded;
    }
    return;
  }
  const bool keepsHigh = place == Place::A && bits == 8;
  for (unsigned byte = bits / 8; byte < kBytes; ++byte) {
    if (keepsHigh && byte == 1) continue;
    slot(place, byte) = kNoOrigin;
    loaded(place, byte) = kNotLoaded;
  }
}

std::optional<std::size_t> Provenance::workRamIndex(Address address) noexcept {
  const std::uint32_t bank = (address >> 16) & 0xFFu;
  const std::uint32_t offset = address & 0xFFFFu;
  if (bank == 0x7Eu || bank == 0x7Fu) return ((bank - 0x7Eu) << 16) | offset;
  if (inSystemBank(address) && offset < 0x2000u) return offset;
  return std::nullopt;
}

Address Provenance::workRamAddress(std::size_t index) noexcept {
  return 0x7E0000u + static_cast<Address>(index);
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
  reached_.reset();
  if (const std::optional<std::size_t> index = workRamIndex(address)) {
    reached_ = index;
    return workRam_[*index];
  }
  if (inSystemBank(address)) {
    const std::uint32_t offset = address & 0xFFFFu;
    if (offset == 0x2180u) {
      // The port: what it holds is the work-RAM byte it reached.
      if (portReads.empty()) return origins_.hardwareRegister(offset);
      const Address reached = portReads.front();
      portReads.erase(portReads.begin());
      const std::optional<std::size_t> index = workRamIndex(reached);
      if (!index) return kNoOrigin;
      reached_ = index;
      return workRam_[*index];
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
  const Invocation& running = frames_.back();
  invocation_[*index] = running.id;
  hold(running.id);
}

void Provenance::hold(std::uint32_t invocation) { ++refs_[invocation]; }

// One less thing holds the invocation; a returned one nothing holds is
// dropped, and lets go of the invocation its runs joined.
void Provenance::release(std::uint32_t invocation) {
  const auto found = refs_.find(invocation);
  if (found == refs_.end()) return;
  if (--found->second != 0) return;
  refs_.erase(found);
  const auto kept = kept_.find(invocation);
  if (kept == kept_.end()) return;
  const std::uint32_t parent = kept->second.parent;
  kept_.erase(kept);
  release(parent);
}

void Provenance::noteRead(std::size_t offset) { frames_.back().runs.read(offset); }

Origin Provenance::read(Address address) {
  address &= 0xFFFFFFu;
  if (const std::optional<std::size_t> offset = romOffset(map_, address, imageBytes_)) noteRead(*offset);
  return at(address);
}

void Provenance::called() {
  frames_.push_back(Invocation{.id = nextInvocation_++, .runs = {}});
  frameIndex_.emplace(frames_.back().id, frames_.size() - 1u);
}

void Provenance::returned() {
  if (frames_.size() < 2) return;  // the root never returns; a return with nothing to pop is the code's own stack play
  Invocation ended = std::move(frames_.back());
  frames_.pop_back();
  frameIndex_.erase(ended.id);
  Invocation& caller = frames_.back();
  for (const auto& [first, run] : ended.runs.byFirst) caller.runs.join(run);
  if (refs_.find(ended.id) == refs_.end()) return;
  // Something still holds it: its runs stay readable, and it holds the caller
  // its runs joined so the source can be followed there.
  hold(caller.id);
  kept_.emplace(ended.id, Kept{.runs = std::move(ended.runs), .parent = caller.id});
}

std::optional<OriginInterval> Provenance::sourceRun(std::uint32_t invocation, std::size_t offset) const {
  std::optional<OriginInterval> best;
  // A caller's run that begins or ends where the run found so far does grew
  // from it, and is the run to follow; one that holds it strictly inside had
  // the bytes already, and the search ends.
  const auto follow = [&](const Runs& runs) {
    const std::optional<OriginInterval> run = runs.holding(offset);
    if (!run) return best.has_value() ? false : true;
    if (!best) {
      best = run;
      return true;
    }
    if (run->first != best->first && run->last != best->last) return false;
    if (run->last - run->first > best->last - best->first) best = run;
    return true;
  };
  // The returned invocations first, each holding the one its runs joined,
  // until one that is still on the stack; then every frame below it.
  std::uint32_t id = invocation;
  while (id != 0) {
    const auto kept = kept_.find(id);
    if (kept == kept_.end()) break;
    if (!follow(kept->second.runs)) return best;
    id = kept->second.parent;
  }
  const auto live = frameIndex_.find(id);
  if (live == frameIndex_.end()) return best;
  for (std::size_t i = live->second + 1u; i-- > 0;) {
    if (!follow(frames_[i].runs)) return best;
  }
  return best;
}

std::vector<OriginInterval> Provenance::sourcesOf(Address address) const {
  std::vector<OriginInterval> out;
  const std::optional<std::size_t> index = workRamIndex(address & 0xFFFFFFu);
  if (!index) return out;
  const OriginSet& origin = origins_.of(workRam_[*index]);
  if (origin.image.empty()) return out;
  const std::uint32_t id = invocation_[*index];
  std::vector<OriginInterval> found;
  for (const OriginInterval& interval : origin.image) {
    if (!origin.approximate) {
      const std::optional<OriginInterval> run = id != 0 ? sourceRun(id, interval.first) : std::nullopt;
      found.push_back(run.value_or(interval));
      continue;
    }
    // A hull: every run it overlaps, and the hull itself where none does.
    bool any = false;
    for (std::size_t offset = interval.first; offset <= interval.last;) {
      const std::optional<OriginInterval> run = id != 0 ? sourceRun(id, offset) : std::nullopt;
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
  for (auto& place : loaded_) {
    for (std::uint32_t& byte : place) byte = kNotLoaded;
  }
}

void Provenance::copy(Place dst, unsigned bits, Place a) {
  for (unsigned byte = 0; byte < bits / 8; ++byte) {
    slot(dst, byte) = carriesOrigin(a) ? slot(a, byte) : kNoOrigin;
    loaded(dst, byte) = carriesOrigin(a) ? loaded(a, byte) : kNotLoaded;
  }
  wrote(dst, bits);
}

void Provenance::combine(Place dst, unsigned bits, Place a, Place b, Place c) {
  const Origin all = origins_.unite(unionOf(a), origins_.unite(unionOf(b), unionOf(c)));
  for (unsigned byte = 0; byte < bits / 8; ++byte) {
    slot(dst, byte) = all;
    loaded(dst, byte) = kNotLoaded;  // computed: not the byte at any address
  }
  wrote(dst, bits);
}

void Provenance::load(Place dst, unsigned bits, unsigned byte, Address address) {
  slot(dst, byte) = read(address);
  loaded(dst, byte) = reached_ ? static_cast<std::uint32_t>(*reached_) : kNotLoaded;
  if (byte + 1u == bits / 8) wrote(dst, bits);
}

void Provenance::store(Address address, Place source, unsigned byte) {
  address &= 0xFFFFFFu;
  const Origin origin = carriesOrigin(source) ? slot(source, byte) : kNoOrigin;
  const std::uint32_t loadedFrom = carriesOrigin(source) ? loaded(source, byte) : kNotLoaded;
  if (inSystemBank(address) && (address & 0xFFFFu) == 0x2180u) {
    // The port: the byte landed where the port reached.
    if (portWrites.empty()) return;
    const Address reached = portWrites.front();
    portWrites.erase(portWrites.begin());
    written(reached, origin, Writer{.site = site, .engine = false});
    return;
  }
  if (const std::optional<std::uint32_t> reg = dataRegister(address)) {
    streamed(*reg, origin, loadedFrom);
    return;
  }
  written(address, origin, Writer{.site = site, .engine = false});
}

void Provenance::exchange() {
  std::swap(slot(Place::A, 0), slot(Place::A, 1));
  std::swap(loaded(Place::A, 0), loaded(Place::A, 1));
}

namespace {

// Where the port put a stream's byte, folded into the stream's extent.
void landed(Stream& stream, std::optional<std::uint16_t> at) {
  if (!at) return;
  if (!stream.landed) {
    stream.landed = true;
    stream.lowest = *at;
    stream.highest = *at;
    return;
  }
  stream.lowest = std::min(stream.lowest, *at);
  stream.highest = std::max(stream.highest, *at);
}

}  // namespace

// A store to a data register: the sequence its register has open continues
// when the value is the next byte — of the buffer, for a byte loaded from work
// RAM; of the image, for a value with exactly one image byte as its origin —
// and the store was made at the same site as the last or on one straight run
// from it; otherwise the sequence closes, and a value of either kind opens a
// new one. Every byte of a sequence carries where the port put it, as the
// host reports it.
void Provenance::streamed(std::uint32_t registerAddress, Origin origin, std::uint32_t loadedFrom) {
  const bool fromBuffer = loadedFrom != kNotLoaded;
  const OriginSet& set = origins_.of(origin);
  const bool single = set.image.size() == 1 && set.image.front().first == set.image.front().last &&
                      set.registers.empty() && !set.save && !set.approximate;
  const auto found = open_.find(registerAddress);
  const bool straight = !broken_;
  broken_ = false;
  const std::uint32_t running = frames_.back().id;
  const std::optional<std::uint16_t> at =
      carries != nullptr ? carries->landedAt(registerAddress) : std::nullopt;
  if (found != open_.end()) {
    OpenStream& open = found->second;
    const bool sameKind = open.stream.memory.has_value() == fromBuffer;
    const std::size_t next = open.stream.memory ? *workRamIndex(*open.stream.memory) + open.stream.bytes
                                                : open.stream.first + open.stream.bytes;
    const bool nextByte = fromBuffer ? loadedFrom == next : single && set.image.front().first == next;
    if (sameKind && nextByte && (site == open.lastSite || straight)) {
      ++open.stream.bytes;
      open.lastSite = site;
      landed(open.stream, at);
      if (open.carrier != running) {
        release(open.carrier);
        open.carrier = running;
        hold(running);
      }
      if (fromBuffer && carries != nullptr) carries->carriedByte(registerAddress, workRamAddress(loadedFrom), true);
      return;
    }
    closeStream(registerAddress);
  }
  if (!fromBuffer && !single) return;
  Stream stream{.site = site,
                .registerAddress = registerAddress,
                .first = single ? set.image.front().first : 0,
                .bytes = 1,
                .times = 1,
                .source = {},
                .memory = fromBuffer ? std::optional<Address>{workRamAddress(loadedFrom)} : std::nullopt,
                .landed = false,
                .lowest = 0,
                .highest = 0};
  landed(stream, at);
  open_.emplace(registerAddress, OpenStream{.stream = stream, .lastSite = site, .carrier = running});
  hold(running);
  if (fromBuffer && carries != nullptr) carries->carriedByte(registerAddress, workRamAddress(loadedFrom), false);
}

void Provenance::closeStream(std::uint32_t registerAddress) {
  const auto found = open_.find(registerAddress);
  if (found == open_.end()) return;
  Stream stream = found->second.stream;
  const std::uint32_t carrier = found->second.carrier;
  const bool recorded = stream.bytes >= 2;
  if (recorded) {
    if (!stream.memory) {
      // The file the stream is lifted as: the run its carrier read that holds
      // the first byte, or the bytes themselves where no run does.
      stream.source = sourceRun(carrier, stream.first)
                          .value_or(OriginInterval{.first = stream.first, .last = stream.first + stream.bytes - 1u});
    }
    const auto same = std::find_if(streams_.begin(), streams_.end(), [&](const Stream& s) {
      return s.site == stream.site && s.registerAddress == stream.registerAddress && s.memory == stream.memory &&
             s.first == stream.first && s.bytes == stream.bytes;
    });
    if (same == streams_.end()) {
      streams_.push_back(stream);
    } else {
      ++same->times;
      // The run may have grown since: the wider source stands.
      if (stream.source.last - stream.source.first > same->source.last - same->source.first) {
        same->source = stream.source;
      }
    }
    if (carries != nullptr) carries->streamClosed(stream);
  }
  const bool fromBuffer = stream.memory.has_value();
  open_.erase(found);
  release(carrier);
  if (fromBuffer && carries != nullptr) carries->carryEnded(registerAddress, recorded);
}

void Provenance::finish() {
  while (!open_.empty()) closeStream(open_.begin()->first);
}

}  // namespace snaggletooth::ir
