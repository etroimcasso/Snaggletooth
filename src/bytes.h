#pragma once

// The helpers every state's byte form is written and read with.
//
// A value goes down as its fixed little-endian width, a byte at a time, and comes
// back by shifting its bytes together, so the bytes a state makes are the same on
// every host: nothing is copied out of an object's memory, and no host's padding,
// byte order, `bool` or enum representation reaches them. A `bool` is one byte,
// 0 or 1; an enum is one byte, its underlying value; a signed value is its
// two's-complement form at its width.
//
// Each part of the machine has a putPart that writes its fields at a cursor and a
// readPart that reads them through a Reader, in the order its layout gives. A
// Reader walks the bytes in one of two passes: the first checks that every byte
// is one putPart could have written and writes nothing; the second fills an
// object. A part's fromBytes runs the first over the whole buffer and the second
// only when the first accepted it, so a refused buffer leaves the caller's object
// as it was.

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>
#include <vector>

namespace snaggletooth {

struct Cpu65816State;
struct Spc700State;
struct DspState;
struct ApuState;
struct PpuState;
struct DmaChannel;
struct Joypad;

}  // namespace snaggletooth

namespace snaggletooth::bytes {

// ---- writing --------------------------------------------------------------
// Each writes its value at `at` and moves `at` past it.

constexpr void put8(std::uint8_t*& at, std::uint8_t value) noexcept {
  *at++ = value;
}

constexpr void put16(std::uint8_t*& at, std::uint16_t value) noexcept {
  at[0] = static_cast<std::uint8_t>(value);
  at[1] = static_cast<std::uint8_t>(value >> 8);
  at += 2;
}

constexpr void put32(std::uint8_t*& at, std::uint32_t value) noexcept {
  for (unsigned shift = 0u; shift < 32u; shift += 8u) {
    *at++ = static_cast<std::uint8_t>(value >> shift);
  }
}

constexpr void put64(std::uint8_t*& at, std::uint64_t value) noexcept {
  for (unsigned shift = 0u; shift < 64u; shift += 8u) {
    *at++ = static_cast<std::uint8_t>(value >> shift);
  }
}

constexpr void putI8(std::uint8_t*& at, std::int8_t value) noexcept {
  put8(at, static_cast<std::uint8_t>(value));
}

constexpr void putI16(std::uint8_t*& at, std::int16_t value) noexcept {
  put16(at, static_cast<std::uint16_t>(value));
}

// An `int` field is written through this as four bytes.
constexpr void putI32(std::uint8_t*& at, std::int32_t value) noexcept {
  put32(at, static_cast<std::uint32_t>(value));
}

constexpr void putBool(std::uint8_t*& at, bool value) noexcept {
  put8(at, value ? 1u : 0u);
}

template <typename E>
  requires std::is_enum_v<E>
constexpr void putEnum(std::uint8_t*& at, E value) noexcept {
  put8(at, static_cast<std::uint8_t>(value));
}

template <std::size_t N>
void putBytes(std::uint8_t*& at, const std::array<std::uint8_t, N>& values) noexcept {
  at = std::copy(values.begin(), values.end(), at);
}

// ---- reading --------------------------------------------------------------
// Each reads its value at `at` and moves `at` past it.

constexpr std::uint8_t get8(const std::uint8_t*& at) noexcept {
  return *at++;
}

constexpr std::uint16_t get16(const std::uint8_t*& at) noexcept {
  const auto value = static_cast<std::uint16_t>(at[0] | (at[1] << 8));
  at += 2;
  return value;
}

constexpr std::uint32_t get32(const std::uint8_t*& at) noexcept {
  std::uint32_t value = 0u;
  for (unsigned shift = 0u; shift < 32u; shift += 8u) {
    value |= static_cast<std::uint32_t>(*at++) << shift;
  }
  return value;
}

constexpr std::uint64_t get64(const std::uint8_t*& at) noexcept {
  std::uint64_t value = 0u;
  for (unsigned shift = 0u; shift < 64u; shift += 8u) {
    value |= static_cast<std::uint64_t>(*at++) << shift;
  }
  return value;
}

constexpr std::int8_t getI8(const std::uint8_t*& at) noexcept {
  return static_cast<std::int8_t>(get8(at));
}

constexpr std::int16_t getI16(const std::uint8_t*& at) noexcept {
  return static_cast<std::int16_t>(get16(at));
}

constexpr std::int32_t getI32(const std::uint8_t*& at) noexcept {
  return static_cast<std::int32_t>(get32(at));
}

// Whether a byte is one putBool writes, and whether one is an enum value no
// greater than the type's last.
[[nodiscard]] constexpr bool checkBool(std::uint8_t byte) noexcept {
  return byte <= 1u;
}

[[nodiscard]] constexpr bool checkEnum(std::uint8_t byte, std::uint8_t last) noexcept {
  return byte <= last;
}

// Reads a part's bytes into its fields, or only checks them. Built to check, it
// writes nothing to the fields it is handed and answers through ok() whether
// every `bool` byte was 0 or 1, every enum byte in its range, and every byte
// that must be zero zero. Built to fill, it writes each field from its bytes;
// it is run only over bytes a checking pass has accepted.
class Reader {
 public:
  enum class Pass : std::uint8_t { Check, Fill };

  Reader(const std::uint8_t* at, Pass pass) noexcept : at_(at), fill_(pass == Pass::Fill) {}

  void u8(std::uint8_t& field) noexcept { set(field, get8(at_)); }
  void u16(std::uint16_t& field) noexcept { set(field, get16(at_)); }
  void u32(std::uint32_t& field) noexcept { set(field, get32(at_)); }
  void u64(std::uint64_t& field) noexcept { set(field, get64(at_)); }
  void i8(std::int8_t& field) noexcept { set(field, getI8(at_)); }
  void i16(std::int16_t& field) noexcept { set(field, getI16(at_)); }

  // Four bytes into an `int` field or a std::int32_t one.
  template <std::signed_integral T>
  void i32(T& field) noexcept {
    set(field, static_cast<T>(getI32(at_)));
  }

  void boolean(bool& field) noexcept {
    const std::uint8_t byte = get8(at_);
    if (!checkBool(byte)) {
      ok_ = false;
      return;
    }
    set(field, byte != 0u);
  }

  template <typename E>
    requires std::is_enum_v<E>
  void enumeration(E& field, E last) noexcept {
    const std::uint8_t byte = get8(at_);
    if (!checkEnum(byte, static_cast<std::uint8_t>(last))) {
      ok_ = false;
      return;
    }
    set(field, static_cast<E>(byte));
  }

  template <std::size_t N>
  void bytes(std::array<std::uint8_t, N>& field) noexcept {
    if (fill_) std::copy(at_, at_ + N, field.begin());
    at_ += N;
  }

  // The next byte, read and passed over, for a field whose shape depends on it.
  [[nodiscard]] std::uint8_t take() noexcept { return get8(at_); }

  // `count` bytes that must each be zero.
  void zeros(std::size_t count) noexcept {
    for (std::size_t i = 0u; i < count; ++i) {
      if (get8(at_) != 0u) ok_ = false;
    }
  }

  // Marks the bytes refused, for a check made outside the field readers.
  void refuse() noexcept { ok_ = false; }

  [[nodiscard]] bool fills() const noexcept { return fill_; }
  [[nodiscard]] bool ok() const noexcept { return ok_; }
  [[nodiscard]] const std::uint8_t* at() const noexcept { return at_; }

 private:
  template <typename T>
  void set(T& field, T value) noexcept {
    if (fill_) field = value;
  }

  const std::uint8_t* at_;
  bool fill_;
  bool ok_ = true;
};

// ---- the parts ------------------------------------------------------------
// Each part's two functions, defined beside the part's own code.

void putPart(std::uint8_t*& at, const Cpu65816State& state) noexcept;
void readPart(Reader& in, Cpu65816State& state) noexcept;
void putPart(std::uint8_t*& at, const Spc700State& state) noexcept;
void readPart(Reader& in, Spc700State& state) noexcept;
void putPart(std::uint8_t*& at, const DspState& state) noexcept;
void readPart(Reader& in, DspState& state) noexcept;
void putPart(std::uint8_t*& at, const ApuState& state) noexcept;
void readPart(Reader& in, ApuState& state) noexcept;
void putPart(std::uint8_t*& at, const PpuState& state) noexcept;
void readPart(Reader& in, PpuState& state) noexcept;
void putPart(std::uint8_t*& at, const DmaChannel& state) noexcept;
void readPart(Reader& in, DmaChannel& state) noexcept;
void putPart(std::uint8_t*& at, const Joypad& state) noexcept;
void readPart(Reader& in, Joypad& state) noexcept;

// A part's public functions, which every part of a fixed size shares. The span
// form writes byteSize() bytes when `out` holds them and answers how many it
// wrote, and writes nothing and answers 0 when it is shorter.
template <typename T>
[[nodiscard]] std::size_t encodeInto(const T& state, std::span<std::uint8_t> out) noexcept {
  if (out.size() < T::byteSize()) return 0u;
  std::uint8_t* at = out.data();
  putPart(at, state);
  return static_cast<std::size_t>(at - out.data());
}

template <typename T>
[[nodiscard]] std::vector<std::uint8_t> encode(const T& state) {
  std::vector<std::uint8_t> out(T::byteSize());
  const std::size_t written = encodeInto(state, out);
  out.resize(written);
  return out;
}

template <typename T>
[[nodiscard]] bool decodeInto(std::span<const std::uint8_t> bytes, T& into) noexcept {
  if (bytes.size() != T::byteSize()) return false;
  Reader check(bytes.data(), Reader::Pass::Check);
  readPart(check, into);
  if (!check.ok()) return false;
  Reader fill(bytes.data(), Reader::Pass::Fill);
  readPart(fill, into);
  return true;
}

template <typename T>
[[nodiscard]] std::optional<T> decode(std::span<const std::uint8_t> bytes) {
  std::optional<T> value{std::in_place};
  if (!decodeInto(bytes, *value)) value.reset();
  return value;
}

}  // namespace snaggletooth::bytes
