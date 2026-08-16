#include "snes_ipl_stub.h"

#include "snaggletooth/apu/apu.h"

namespace snaggletooth {
namespace {

// The stub, hand-assembled from the SPC700 opcode set. It answers the documented
// upload handshake: the main CPU polls output ports 0 and 1 for the ready bytes
// $AA and $BB, then drives input ports 0-3 to set a destination, stream bytes, and
// finally start the loaded program; the stub acknowledges every step by echoing
// input port 0 back to output port 0.
//
// The first byte is $CD, the opcode of `MOV X,#imm`, which is also the first byte
// of the console's boot ROM. A loaded driver that jumps back to $FFC0 to receive
// more code checks for that byte to confirm the stub is present, so the stub is
// re-runnable from the top: entering it clears the input ports (through the CONTROL
// register, the way the console does) so a second upload starts as cleanly as the
// first, whatever the ports held.
//
// It keeps three bytes of the direct page:
//
//   $00/$01  the destination pointer (low, high)
//   $03      the last input-port-0 value it acted on
//
// The running byte index lives in the Y register. Ports $F4-$F7 on the audio CPU
// are input ports 0-3 when read and output ports 0-3 when written, so a read sees
// what the main CPU sent and a write is what the main CPU reads back.
//
//   $FFC0  CD EF      MOV X,#$EF      ; the $CD signature; X is unused
//   $FFC2  20         CLRP            ; the direct page is $00xx, so $F4-$F7 are the ports
//   $FFC3  8F B0 F1   MOV $F1,#$B0    ; clear the input ports through CONTROL
//   $FFC6  8D 00      MOV Y,#$00      ; the byte index starts at zero
//   $FFC8  CB 03      MOV $03,Y       ; and the last-acted value at zero
//   $FFCA  8F AA F4   MOV $F4,#$AA    ; ready: post $AA to output port 0
//   $FFCD  8F BB F5   MOV $F5,#$BB    ; ready: post $BB to output port 1
//   $FFD0  E4 F4      MOV A,$F4       ; poll: read input port 0
//   $FFD2  64 03      CMP A,$03       ; still the value already handled?
//   $FFD4  F0 FA      BEQ $FFD0       ; yes -> keep polling
//   $FFD6  C4 03      MOV $03,A       ; record this port-0 value
//   $FFD8  F8 F5      MOV X,$F5       ; read input port 1 (byte or flag) before it can change
//   $FFDA  C4 F4      MOV $F4,A       ; echo port 0 as the acknowledgement
//   $FFDC  7E 03      CMP Y,$03       ; index equals port 0?
//   $FFDE  D0 0A      BNE $FFEA       ; no -> handle it as a block command
//   $FFE0  7D         MOV A,X         ; data: A = the data byte from port 1
//   $FFE1  D7 00      MOV [$00]+Y,A   ;       store at destination + index
//   $FFE3  FC         INC Y           ;       advance the index
//   $FFE4  D0 EA      BNE $FFD0       ;       still within the page -> next byte
//   $FFE6  AB 01      INC $01         ;       index wrapped: carry the destination high byte
//   $FFE8  2F E6      BRA $FFD0       ;       next byte
//   $FFEA  E4 F6      MOV A,$F6       ; command: destination low from port 2
//   $FFEC  C4 00      MOV $00,A
//   $FFEE  E4 F7      MOV A,$F7       ;          destination high from port 3
//   $FFF0  C4 01      MOV $01,A
//   $FFF2  7D         MOV A,X         ;          port 1: zero starts the program, nonzero sets an address
//   $FFF3  D0 03      BNE $FFF8       ;          nonzero -> begin a transfer
//   $FFF5  1F 00 00   JMP [!$0000+X]  ; run: jump through the destination pointer (X is zero here)
//   $FFF8  8D 00      MOV Y,#$00      ; transfer: reset the byte index
//   $FFFA  2F D4      BRA $FFD0       ;           and receive the block
//   $FFFC..$FFFD      (unused, zero)
//   $FFFE  C0 FF      reset vector -> $FFC0
constexpr std::array<std::uint8_t, kIplStubSize> kImage = {
    0xCD, 0xEF,              // MOV X,#$EF     (the $CD signature)
    0x20,                    // CLRP
    0x8F, 0xB0, 0xF1,        // MOV $F1,#$B0   (clear the input ports)
    0x8D, 0x00,              // MOV Y,#$00
    0xCB, 0x03,              // MOV $03,Y
    0x8F, 0xAA, 0xF4,        // MOV $F4,#$AA   (ready)
    0x8F, 0xBB, 0xF5,        // MOV $F5,#$BB
    0xE4, 0xF4,              // MOV A,$F4      (poll)
    0x64, 0x03,              // CMP A,$03
    0xF0, 0xFA,              // BEQ $FFD0
    0xC4, 0x03,              // MOV $03,A
    0xF8, 0xF5,              // MOV X,$F5
    0xC4, 0xF4,              // MOV $F4,A      (echo)
    0x7E, 0x03,              // CMP Y,$03
    0xD0, 0x0A,              // BNE $FFEA
    0x7D,                    // MOV A,X        (data)
    0xD7, 0x00,              // MOV [$00]+Y,A
    0xFC,                    // INC Y
    0xD0, 0xEA,              // BNE $FFD0
    0xAB, 0x01,              // INC $01
    0x2F, 0xE6,              // BRA $FFD0
    0xE4, 0xF6,              // MOV A,$F6      (command)
    0xC4, 0x00,              // MOV $00,A
    0xE4, 0xF7,              // MOV A,$F7
    0xC4, 0x01,              // MOV $01,A
    0x7D,                    // MOV A,X
    0xD0, 0x03,              // BNE $FFF8
    0x1F, 0x00, 0x00,        // JMP [!$0000+X] (run)
    0x8D, 0x00,              // MOV Y,#$00     (transfer)
    0x2F, 0xD4,              // BRA $FFD0
    0x00, 0x00,              // unused
    0xC0, 0xFF,              // reset vector -> $FFC0
};

}  // namespace

const std::array<std::uint8_t, kIplStubSize>& iplStubImage() noexcept { return kImage; }

void seedIplStub(ApuState& apu) noexcept {
  const std::array<std::uint8_t, kIplStubSize>& image = kImage;
  for (std::size_t i = 0; i < kIplStubSize; ++i) {
    apu.ram[kIplStubBase + i] = image[i];
  }
  apu.inputPorts = {};   // the stub posts its own ready bytes; start the ports clear
  apu.outputPorts = {};
  apu.cpu.pc = kIplStubBase;
  apu.cpu.sp = 0xEFu;                 // the post-boot stack pointer ($01EF)
  apu.cpu.psw = 0u;                   // P clear, so a $F4-$F7 direct access reaches the ports at $00F4
  apu.cpu.run = RunState::Running;
}

}  // namespace snaggletooth
