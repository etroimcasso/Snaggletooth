// The SPC700 state as bytes and back. The core itself lives whole in its header;
// this file holds only its state's byte form, whose layout is the fields of
// Spc700State in the order they are declared.

#include "snaggletooth/apu/spc700.h"

#include "bytes.h"

namespace snaggletooth {

namespace bytes {

void putPart(std::uint8_t*& at, const Spc700State& state) noexcept {
  put16(at, state.pc);
  put8(at, state.a);
  put8(at, state.x);
  put8(at, state.y);
  put8(at, state.sp);
  put8(at, state.psw);
  putEnum(at, state.run);
  put8(at, state.ir);
  put8(at, state.tcu);
  put16(at, state.ea);
  put16(at, state.ptr);
  put16(at, state.tmp);
  putBool(at, state.taken);
}

void readPart(Reader& in, Spc700State& state) noexcept {
  in.u16(state.pc);
  in.u8(state.a);
  in.u8(state.x);
  in.u8(state.y);
  in.u8(state.sp);
  in.u8(state.psw);
  in.enumeration(state.run, RunState::Stopped);
  in.u8(state.ir);
  in.u8(state.tcu);
  in.u16(state.ea);
  in.u16(state.ptr);
  in.u16(state.tmp);
  in.boolean(state.taken);
}

}  // namespace bytes

std::vector<std::uint8_t> Spc700State::asBytes() const {
  return bytes::encode(*this);
}

std::size_t Spc700State::asBytes(std::span<std::uint8_t> out) const noexcept {
  return bytes::encodeInto(*this, out);
}

std::optional<Spc700State> Spc700State::fromBytes(std::span<const std::uint8_t> bytes) {
  return bytes::decode<Spc700State>(bytes);
}

bool Spc700State::fromBytes(std::span<const std::uint8_t> bytes, Spc700State& into) noexcept {
  return bytes::decodeInto(bytes, into);
}

}  // namespace snaggletooth
