// FromDust shim for xenia/kernel/user_module.h.
#ifndef XENIA_KERNEL_USER_MODULE_H_
#define XENIA_KERNEL_USER_MODULE_H_

#include <cstdint>

namespace xe {
namespace kernel {

class UserModule {
 public:
  uint32_t title_id() const { return 0x58410A91; } // From Dust
};

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_USER_MODULE_H_
