// FromDust shim for xenia/emulator.h.
#ifndef XENIA_EMULATOR_H_
#define XENIA_EMULATOR_H_

#include "xenia/apu/audio_system.h"

namespace xe {

class Emulator {
 public:
  apu::AudioSystem* audio_system() const { return nullptr; }
};

}  // namespace xe

#endif  // XENIA_EMULATOR_H_
