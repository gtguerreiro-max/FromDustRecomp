// FromDust shim for xenia/apu/audio_system.h.
#ifndef XENIA_APU_AUDIO_SYSTEM_H_
#define XENIA_APU_AUDIO_SYSTEM_H_

#include <string>

namespace xe {
namespace apu {

class AudioSystem {
 public:
  std::string name() const { return "FromDust"; }
};

}  // namespace apu
}  // namespace xe

#endif  // XENIA_APU_AUDIO_SYSTEM_H_
