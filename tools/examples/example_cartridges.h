#pragma once

// Cartridge images built by hand: a LoROM header, a few banks of 65816 code that
// call and jump across them, one whose reset code speaks the audio upload
// protocol, ones that dispatch through pointers, read the controllers, run a
// transfer, wait for an interrupt, copy code into work RAM, prove their
// registers the way real code does and dispatch through bounded tables, send
// the image's bytes to the hardware every way the asset pass has a rule for, or
// run through a mirror of the bank the tree places them in. The
// tests of the cartridge tools read them, and `snes_examples` writes them to disk, so every
// example a page shows is the real output of a tool on a cartridge that is ours
// to publish.

#include <string_view>
#include <vector>

#include "examples/three_bank/three_bank.h"
#include "examples/uploading/uploading.h"
#include "examples/cop_vector/cop_vector.h"
#include "examples/straddling_upload/straddling_upload.h"
#include "examples/dispatching/dispatching.h"
#include "examples/stalled_jump/stalled_jump.h"
#include "examples/unreadable_pointer/unreadable_pointer.h"
#include "examples/button_dispatch/button_dispatch.h"
#include "examples/mixed/mixed.h"
#include "examples/waiting/waiting.h"
#include "examples/transfer/transfer.h"
#include "examples/ram_code/ram_code.h"
#include "examples/hdma/hdma.h"
#include "examples/irq/irq.h"
#include "examples/frame_press/frame_press.h"
#include "examples/proving/proving.h"
#include "examples/moving/moving.h"
#include "examples/lifting/lifting.h"
#include "examples/wrapping/wrapping.h"
#include "examples/mirrored/mirrored.h"

namespace snaggletooth::examples {

// Every example cartridge by name, with a line on what it does, for
// `snes_examples` and anyone else who wants them all.
struct Example {
  std::string_view name;
  std::string_view description;
  std::vector<std::uint8_t> (*build)();
};

inline const std::vector<Example>& examples() {
  static const std::vector<Example> all = {
      {"three_bank", "three banks calling and jumping across each other, and a jump through a table", &threeBankImage},
      {"uploading", "reset speaks the audio upload protocol and sends a sound program", &uploadingImage},
      {"cop_vector", "the COP and native BRK vectors named beside reset", &copVectorImage},
      {"straddling_upload", "the uploading cartridge with two instructions across the upload's edges", &straddlingUploadImage},
      {"dispatching", "every indirect jump and call form, each to a target nothing else names", &dispatchingImage},
      {"stalled_jump", "a transfer started on the instruction before an indirect jump", &stalledJumpImage},
      {"unreadable_pointer", "an indirect jump whose pointer lies in a register", &unreadablePointerImage},
      {"button_dispatch", "two indirect jumps taken only when a button is down, read both ways", &buttonDispatchImage},
      {"mixed", "a spread of the effect layer's constructs, then three vertical-blank interrupts", &mixedImage},
      {"waiting", "a wait for the vertical-blank interrupt, twice", &waitingImage},
      {"transfer", "a transfer into work RAM through the data port, run while the CPU is held", &transferImage},
      {"ram_code", "a routine copied into work RAM and called there", &ramCodeImage},
      {"hdma", "one HDMA write per frame while the program idles", &hdmaImage},
      {"irq", "a timer request held under the interrupt-disable flag, a wait it ends, then taken", &irqImage},
      {"frame_press", "a Start press that counts only on the one frame the tenth interrupt reads", &framePressImage},
      {"proving", "every register proved the way real code proves it, and three jump tables, two of them bounded", &provingImage},
      {"moving", "bytes moved every way the transfer engines can, and a sprite table sent every frame", &movingImage},
      {"lifting", "bytes sent from the image to the hardware every way the asset pass has a rule for", &liftingImage},
      {"wrapping", "a HiROM transfer that runs off its bank's end and on from the bank's start", &wrappingImage},
      {"mirrored", "a program that runs through the fast mirror of the bank the tree places it in", &mirroredImage},
  };
  return all;
}

}  // namespace snaggletooth::examples
