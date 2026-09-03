#include "disasm/disasm.h"

#include <algorithm>
#include <cstdio>
#include <deque>
#include <iterator>
#include <set>
#include <utility>

namespace snaggletooth::disasm {
namespace {

std::string hex8(std::uint8_t value) {
  char buffer[3];
  std::snprintf(buffer, sizeof buffer, "%02X", value);
  return buffer;
}

std::string hex16(std::uint16_t value) {
  char buffer[5];
  std::snprintf(buffer, sizeof buffer, "%04X", value);
  return buffer;
}

// An address as a label carries it: the same digits a listing prints, without
// the punctuation, so the label is an identifier.
std::string labelDigits(Address address, unsigned addressBits) {
  if (addressBits > 16) {
    return hex8(static_cast<std::uint8_t>(address >> 16)) +
           hex16(static_cast<std::uint16_t>(address & 0xFFFFu));
  }
  return hex16(static_cast<std::uint16_t>(address & 0xFFFFu));
}

}  // namespace

std::string formatAddress(Address address, unsigned addressBits) {
  if (addressBits > 16) {
    return "$" + hex8(static_cast<std::uint8_t>(address >> 16)) + ":" +
           hex16(static_cast<std::uint16_t>(address & 0xFFFFu));
  }
  return "$" + hex16(static_cast<std::uint16_t>(address & 0xFFFFu));
}

std::string Backend::describe(Context context) const {
  char buffer[11];
  std::snprintf(buffer, sizeof buffer, "%X", static_cast<unsigned>(context.bits));
  return std::string("context ") + buffer;
}

std::vector<std::string> Backend::directives(std::optional<Context>, Context) const {
  return {};
}

std::string Backend::unreadable(std::span<const std::uint8_t>, Address, Address, Context) const {
  return {};
}

Listing trace(const Backend& backend, const Request& request) {
  Listing listing;
  listing.addressBits = backend.addressBits();
  const std::span<const std::uint8_t> image = request.image;
  const Address base = request.base;
  if (image.empty()) return listing;

  const std::size_t size = image.size();
  const unsigned bits = listing.addressBits;
  std::vector<bool> isOpcode(size, false);
  std::vector<bool> covered(size, false);
  std::map<Address, Instruction> decoded;
  std::map<Address, Context> contextAt;
  std::map<Address, Context> contextAfter;

  auto inRange = [&](Address address) {
    return address >= base && static_cast<std::size_t>(address - base) < size;
  };

  // A work item is an address and the context execution reaches it with. The
  // same address under a different context is a different item, which is what
  // lets a conflict be seen rather than dropped.
  struct Item {
    Address address;
    Context context;
  };
  std::deque<Item> pending;
  std::set<std::pair<Address, std::uint32_t>> queued;
  auto visit = [&](Address address, Context context) {
    if (!inRange(address)) return;
    if (!queued.insert({address, context.bits}).second) return;
    pending.push_back({address, context});
  };

  if (request.entries.empty()) {
    visit(base, request.context);
    listing.labels[base] = "entry";
  }
  for (std::size_t i = 0; i < request.entries.size(); ++i) {
    const Address entry = request.entries[i];
    if (!inRange(entry)) {
      listing.warnings.push_back("entry " + formatAddress(entry, bits) + " is outside the image");
      continue;
    }
    visit(entry, i < request.entryContexts.size() ? request.entryContexts[i] : request.context);
    listing.labels[entry] = "entry_" + labelDigits(entry, bits);
  }

  while (!pending.empty()) {
    const Item item = pending.front();
    pending.pop_front();
    const Address address = item.address;
    const std::size_t offset = static_cast<std::size_t>(address - base);

    if (isOpcode[offset]) {
      // Decoded already, and under a different context — the queue admits each
      // address once per context, so a loop closing under the same context never
      // arrives here. The backend says whether the two contexts read the bytes two
      // ways; when they do it names both readings so the reader can decide, and
      // the first one stands.
      if (backend.conflicts(contextAt[address], item.context)) {
        listing.warnings.push_back(formatAddress(address, bits) + " is reached with " +
                                   backend.describe(contextAt[address]) + " and with " +
                                   backend.describe(item.context));
      }
      continue;
    }

    const std::string reason = backend.unreadable(image, base, address, item.context);
    if (!reason.empty()) {
      listing.warnings.push_back(formatAddress(address, bits) + " cannot be read: " + reason);
      continue;
    }
    std::optional<Decoded> result = backend.decode(image, base, address, item.context);
    if (!result) {
      listing.warnings.push_back("instruction at " + formatAddress(address, bits) +
                                 " runs past the end of the image");
      continue;
    }
    const Instruction& instruction = result->instruction;

    // A target that lands inside an instruction already decoded means the trace
    // read one of the two as code when it is not. Report it rather than silently
    // preferring either reading.
    bool overlaps = false;
    for (std::uint8_t i = 0; i < instruction.length; ++i) {
      if (covered[offset + i]) overlaps = true;
    }
    if (overlaps) {
      listing.warnings.push_back(formatAddress(address, bits) +
                                 " overlaps an instruction already decoded");
    }

    isOpcode[offset] = true;
    for (std::uint8_t i = 0; i < instruction.length; ++i) covered[offset + i] = true;
    contextAt[address] = item.context;

    const Address next = backend.following(address, instruction.length);
    const Context after = result->next;
    contextAfter[address] = after;

    switch (instruction.flow) {
      case Flow::Continue:
        visit(next, after);
        break;
      case Flow::Branch:
        visit(next, after);
        if (instruction.target) visit(*instruction.target, after);
        break;
      case Flow::Call:
        if (instruction.target) visit(*instruction.target, after);
        visit(next, after);
        break;
      case Flow::Jump:
        if (instruction.target) visit(*instruction.target, after);
        break;
      case Flow::Return:
      case Flow::Halt:
        break;
    }

    if (instruction.target && inRange(*instruction.target)) {
      const Address destination = *instruction.target;
      if (listing.labels.find(destination) == listing.labels.end()) {
        listing.labels[destination] = (instruction.flow == Flow::Call ? "sub_" : "loc_") +
                                      labelDigits(destination, bits);
      }
    }

    decoded.emplace(address, std::move(result->instruction));
  }

  // Supplied symbols win over generated labels.
  for (const auto& [address, name] : request.symbols) listing.labels[address] = name;

  // Annotate: a named hardware register the operand reaches, and any byte that
  // differs from the image the code started as.
  const bool havePrior = request.priorImage.size() == image.size();
  for (auto& [address, instruction] : decoded) {
    if (request.annotateRegisters && instruction.operandAddress) {
      const std::string_view named = backend.registerName(*instruction.operandAddress);
      if (!named.empty()) instruction.note = std::string(named);
    }
    if (havePrior) {
      const std::size_t at = static_cast<std::size_t>(address - base);
      std::string was;
      for (std::uint8_t i = 0; i < instruction.length; ++i) {
        if (image[at + i] != request.priorImage[at + i]) {
          was += (was.empty() ? "" : " ") + hex8(request.priorImage[at + i]);
        }
      }
      if (!was.empty()) {
        if (!instruction.note.empty()) instruction.note += "; ";
        instruction.note += "PATCHED at run time, was " + was;
      }
    }
  }

  // Emit lines in address order: decoded instructions, and runs of everything the
  // trace never reached. Each instruction carries the source lines the backend
  // wants before it, judged against the context the instruction above left
  // behind — or against nothing at the start of the listing and after a run of
  // data, where an assembler starts fresh.
  std::size_t offset = 0;
  std::optional<Context> left;
  while (offset < size) {
    const Address address = base + static_cast<Address>(offset);
    auto found = decoded.find(address);
    if (found != decoded.end() && isOpcode[offset]) {
      Line line;
      line.isCode = true;
      line.address = address;
      line.instruction = found->second;
      line.context = contextAt[address];
      line.directives = backend.directives(left, line.context);
      left = contextAfter[address];
      listing.lines.push_back(std::move(line));
      offset += found->second.length;
      continue;
    }
    const std::size_t start = offset;
    while (offset < size && !isOpcode[offset]) ++offset;
    Line line;
    line.isCode = false;
    line.address = base + static_cast<Address>(start);
    line.data.assign(image.begin() + static_cast<std::ptrdiff_t>(start),
                     image.begin() + static_cast<std::ptrdiff_t>(offset));
    listing.lines.push_back(std::move(line));
    left.reset();
  }

  // A label names a line the listing prints. A target that landed in data, or
  // inside an instruction already decoded, has no line and keeps no label, so an
  // operand is never written as a name nothing defines.
  std::set<Address> printed;
  for (const Line& line : listing.lines) {
    if (line.isCode) printed.insert(line.address);
  }
  for (auto it = listing.labels.begin(); it != listing.labels.end();) {
    it = printed.count(it->first) != 0 ? std::next(it) : listing.labels.erase(it);
  }

  return listing;
}

std::string render(const Listing& listing) {
  std::string out;
  const unsigned bits = listing.addressBits;

  for (const std::string& warning : listing.warnings) {
    out += "; warning: " + warning + "\n";
  }
  if (!listing.warnings.empty()) out += "\n";

  // A listing is source, not a report about one: it assembles back to the bytes it
  // came from, so a ROM can be disassembled, edited and rebuilt without the
  // original source ever existing. Everything that is not an instruction or a
  // directive — the address, the raw bytes, the cycle cost, an annotation — rides
  // in a trailing comment, which is what keeps that true.
  if (!listing.lines.empty()) {
    out += "        ORG " + formatAddress(listing.lines.front().address, bits) + "\n";
  }

  auto labelAt = [&](Address address) -> const std::string* {
    auto found = listing.labels.find(address);
    return found == listing.labels.end() ? nullptr : &found->second;
  };

  // Where the trailing comment starts. Wide enough for the longest mnemonic a
  // backend renders, so the comment column does not move down the listing.
  constexpr std::size_t kCommentColumn = 40;
  auto padTo = [](std::string& text, std::size_t column) {
    if (text.size() < column) {
      text.append(column - text.size(), ' ');
    } else {
      text += "  ";
    }
  };

  // The raw-bytes field is as wide as the longest instruction in the listing, and
  // never narrower than three bytes, so the cycle costs stay aligned whatever the
  // mix of lengths above and below.
  std::size_t longest = 3;
  for (const Line& line : listing.lines) {
    if (line.isCode) longest = std::max<std::size_t>(longest, line.instruction.length);
  }
  const std::size_t bytesWidth = longest * 3;

  for (const Line& line : listing.lines) {
    if (line.isCode) {
      if (const std::string* label = labelAt(line.address)) {
        out += "\n" + *label + ":\n";
      }
      for (const std::string& directive : line.directives) {
        out += "        " + directive + "\n";
      }
      // The target as the label it carries, where the dialect can write one;
      // the address otherwise. A label is only ever at a line of the listing, so
      // the name is one the same source defines.
      const Instruction& instruction = line.instruction;
      const std::string* targetLabel =
          instruction.target && instruction.symbolic ? labelAt(*instruction.target) : nullptr;
      std::string row = "        " + (targetLabel ? instruction.symbolic->before + *targetLabel +
                                                        instruction.symbolic->after
                                                  : instruction.text);
      padTo(row, kCommentColumn);

      std::string bytes;
      for (std::uint8_t byte : instruction.bytes) bytes += hex8(byte) + " ";
      bytes.append(bytesWidth - bytes.size(), ' ');

      row += "; " + formatAddress(instruction.address, bits) + "  " + bytes + " ";
      if (!instruction.cycles.known) {
        row += "?";
      } else {
        row += std::to_string(instruction.cycles.base);
        if (instruction.cycles.taken != 0) {
          row += "/" + std::to_string(instruction.cycles.taken);
        }
      }
      if (!instruction.note.empty()) row += "  " + instruction.note;
      out += row + "\n";
      continue;
    }

    if (line.data.empty()) continue;
    out += "\n; ---- " + std::to_string(line.data.size()) +
           " bytes execution did not reach\n";
    constexpr std::size_t kPerRow = 8;
    for (std::size_t i = 0; i < line.data.size(); i += kPerRow) {
      const std::size_t end = std::min(i + kPerRow, line.data.size());
      std::string row = "        DB ";
      std::string ascii;
      for (std::size_t j = i; j < end; ++j) {
        if (j != i) row += ",";
        row += "$" + hex8(line.data[j]);
        const std::uint8_t byte = line.data[j];
        ascii += (byte >= 0x20 && byte < 0x7F) ? static_cast<char>(byte) : '.';
      }
      padTo(row, kCommentColumn);
      row += "; " + formatAddress(line.address + static_cast<Address>(i), bits) + "  |" +
             ascii + "|";
      out += row + "\n";
    }
  }

  return out;
}

}  // namespace snaggletooth::disasm
