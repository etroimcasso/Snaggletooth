// The 65816 state as bytes and back. The core itself lives whole in its header;
// this file holds only its state's byte form, whose layout is the fields of
// Cpu65816State in the order they are declared.

#include "snaggletooth/cpu/cpu65816.h"

#include "bytes.h"

namespace snaggletooth {

namespace bytes {

void putPart(std::uint8_t*& at, const Cpu65816State& state) noexcept {
  put16(at, state.pc);
  put16(at, state.s);
  put16(at, state.a);
  put16(at, state.x);
  put16(at, state.y);
  put16(at, state.d);
  put8(at, state.p);
  put8(at, state.dbr);
  put8(at, state.pbr);
  putBool(at, state.e);
  put8(at, state.ir);
  put8(at, state.tcu);
  put32(at, state.ea);
  put16(at, state.ptr);
  put16(at, state.tmp);
  putBool(at, state.pageCross);
  putBool(at, state.nmiPending);
  putBool(at, state.irqLine);
  putEnum(at, state.servicing);
  putEnum(at, state.run);
}

void readPart(Reader& in, Cpu65816State& state) noexcept {
  in.u16(state.pc);
  in.u16(state.s);
  in.u16(state.a);
  in.u16(state.x);
  in.u16(state.y);
  in.u16(state.d);
  in.u8(state.p);
  in.u8(state.dbr);
  in.u8(state.pbr);
  in.boolean(state.e);
  in.u8(state.ir);
  in.u8(state.tcu);
  in.u32(state.ea);
  in.u16(state.ptr);
  in.u16(state.tmp);
  in.boolean(state.pageCross);
  in.boolean(state.nmiPending);
  in.boolean(state.irqLine);
  in.enumeration(state.servicing, InterruptRequest::Nmi);
  in.enumeration(state.run, CpuRunState::Stopped);
}

}  // namespace bytes

std::vector<std::uint8_t> Cpu65816State::asBytes() const {
  return bytes::encode(*this);
}

std::size_t Cpu65816State::asBytes(std::span<std::uint8_t> out) const noexcept {
  return bytes::encodeInto(*this, out);
}

std::optional<Cpu65816State> Cpu65816State::fromBytes(std::span<const std::uint8_t> bytes) {
  return bytes::decode<Cpu65816State>(bytes);
}

bool Cpu65816State::fromBytes(std::span<const std::uint8_t> bytes,
                              Cpu65816State& into) noexcept {
  return bytes::decodeInto(bytes, into);
}

}  // namespace snaggletooth
