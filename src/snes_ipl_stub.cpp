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
// The echo is the last thing each step does, because it is what releases the main
// CPU: a game treats the acknowledgement as permission to set up whatever comes
// next, and it may rewrite any input port immediately. So a step reads everything
// it needs from the ports first and echoes afterwards, and a command reads the
// destination as one 16-bit word rather than two bytes, leaving no instant at
// which half of a new address can be mixed with half of the old one.
//
//   $FFC0  CD EF      MOV X,#$EF      ; the $CD signature; X is set again below
//   $FFC2  20         CLRP            ; the direct page is $00xx, so $F4-$F7 are the ports
//   $FFC3  8F B0 F1   MOV $F1,#$B0    ; clear the input ports through CONTROL
//   $FFC6  8F AA F4   MOV $F4,#$AA    ; ready: post $AA to output port 0
//   $FFC9  8F BB F5   MOV $F5,#$BB    ; ready: post $BB to output port 1
//   $FFCC  E4 F4      MOV A,$F4       ; ready wait: read input port 0
//   $FFCE  68 CC      CMP A,#$CC      ;             only $CC begins an upload
//   $FFD0  D0 FA      BNE $FFCC       ;             anything else is not addressed here
//   $FFD2  C4 03      MOV $03,A       ; record it, so the echo can send it back
//   $FFD4  F8 F5      MOV X,$F5       ; read input port 1 before the echo frees the host
//   $FFD6  BA F6      MOVW YA,$F6     ; command: the destination, ports 2 and 3 as one word
//   $FFD8  DA 00      MOVW $00,YA     ;          keep it
//   $FFDA  FA 03 F4   MOV $F4,$03     ;          echo, the destination now safely held
//   $FFDD  7D         MOV A,X         ;          port 1: zero runs the program, nonzero an address
//   $FFDE  D0 03      BNE $FFE3       ;          nonzero -> begin a transfer
//   $FFE0  1F 00 00   JMP [!$0000+X]  ; run: jump through the destination pointer (X is zero here)
//   $FFE3  8D 00      MOV Y,#$00      ; transfer: the byte index restarts
//   $FFE5  E4 F4      MOV A,$F4       ; poll: read input port 0
//   $FFE7  64 03      CMP A,$03       ;       still the value already handled?
//   $FFE9  F0 FA      BEQ $FFE5       ;       yes -> keep polling
//   $FFEB  C4 03      MOV $03,A       ; record this port-0 value
//   $FFED  F8 F5      MOV X,$F5       ; read input port 1 (byte or flag) before it can change
//   $FFEF  7E 03      CMP Y,$03       ; index equals port 0?
//   $FFF1  D0 E3      BNE $FFD6       ; no -> handle it as a block command
//   $FFF3  C4 F4      MOV $F4,A       ; data: echo, the byte already held in X
//   $FFF5  7D         MOV A,X         ;       A = the data byte from port 1
//   $FFF6  D7 00      MOV [$00]+Y,A   ;       store at destination + index
//   $FFF8  FC         INC Y           ;       advance the index
//   $FFF9  D0 EA      BNE $FFE5       ;       still within the page -> next byte
//   $FFFB  AB 01      INC $01         ;       index wrapped: carry the destination high byte
//   $FFFD  2F E6      BRA $FFE5       ;       next byte
//   $FFFF  00                         ; (spare; the machine seeds the program counter directly)
constexpr std::array<std::uint8_t, kIplStubSize> kImage = {
    0xCD, 0xEF,              // MOV X,#$EF     (the $CD signature)
    0x20,                    // CLRP
    0x8F, 0xB0, 0xF1,        // MOV $F1,#$B0   (clear the input ports)
    0x8F, 0xAA, 0xF4,        // MOV $F4,#$AA   (ready)
    0x8F, 0xBB, 0xF5,        // MOV $F5,#$BB
    0xE4, 0xF4,              // MOV A,$F4      (ready wait: only $CC begins)
    0x68, 0xCC,              // CMP A,#$CC
    0xD0, 0xFA,              // BNE $FFCC
    0xC4, 0x03,              // MOV $03,A
    0xF8, 0xF5,              // MOV X,$F5
    0xBA, 0xF6,              // MOVW YA,$F6    (command: the destination as one word)
    0xDA, 0x00,              // MOVW $00,YA
    0xFA, 0x03, 0xF4,        // MOV $F4,$03    (echo)
    0x7D,                    // MOV A,X
    0xD0, 0x03,              // BNE $FFE3
    0x1F, 0x00, 0x00,        // JMP [!$0000+X] (run)
    0x8D, 0x00,              // MOV Y,#$00     (transfer)
    0xE4, 0xF4,              // MOV A,$F4      (poll)
    0x64, 0x03,              // CMP A,$03
    0xF0, 0xFA,              // BEQ $FFE5
    0xC4, 0x03,              // MOV $03,A
    0xF8, 0xF5,              // MOV X,$F5
    0x7E, 0x03,              // CMP Y,$03
    0xD0, 0xE3,              // BNE $FFD6
    0xC4, 0xF4,              // MOV $F4,A      (data: echo)
    0x7D,                    // MOV A,X
    0xD7, 0x00,              // MOV [$00]+Y,A
    0xFC,                    // INC Y
    0xD0, 0xEA,              // BNE $FFE5
    0xAB, 0x01,              // INC $01
    0x2F, 0xE6,              // BRA $FFE5
    0x00,                    // (spare)
};

}  // namespace

const std::array<std::uint8_t, kIplStubSize>& iplStubImage() noexcept { return kImage; }

void seedIplStub(ApuState& apu) noexcept { seedIplStub(apu, kImage); }

void seedIplStub(ApuState& apu, std::span<const std::uint8_t, kIplStubSize> image) noexcept {
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
