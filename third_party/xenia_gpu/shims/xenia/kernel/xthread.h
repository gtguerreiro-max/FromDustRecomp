// FromDust shim for xenia/kernel/xthread.h: host threads used by the GPU
// (command processor worker, frame limiter). Each one gets a guest identity in
// the FromDust runtime so it can run guest interrupt callbacks.
#ifndef XENIA_KERNEL_XTHREAD_H_
#define XENIA_KERNEL_XTHREAD_H_

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "xenia/base/threading.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {

class KernelState;

// Minimal intrusive-free reference: the GPU owns these threads for its lifetime.
template <typename T>
class object_ref {
 public:
  object_ref() = default;
  explicit object_ref(T* value) : value_(value) {}
  T* get() const { return value_.get(); }
  T* operator->() const { return value_.get(); }
  explicit operator bool() const { return value_ != nullptr; }
  void reset() { value_.reset(); }

 private:
  std::shared_ptr<T> value_;
};

class XThread {
 public:
  virtual ~XThread() = default;
  xe::threading::Thread* thread() const { return thread_.get(); }
  void set_name(std::string name) {
    name_ = std::move(name);
    if (thread_) thread_->set_name(name_);
  }
  void set_can_debugger_suspend(bool) {}
  X_STATUS Wait(uint32_t, uint32_t, uint32_t, void*) {
    if (thread_) xe::threading::Wait(thread_.get(), false);
    return X_STATUS_SUCCESS;
  }
  static bool IsInThread(XThread* other);
  static bool IsInThread();

 protected:
  std::unique_ptr<xe::threading::Thread> thread_;
  std::string name_;
};

class XHostThread : public XThread {
 public:
  XHostThread(KernelState* kernel_state, uint32_t stack_size, uint32_t creation_flags,
              std::function<int()> host_fn, uint32_t guest_process = 0)
      : stack_size_(stack_size), host_fn_(std::move(host_fn)) {}

  X_STATUS Create();

 private:
  uint32_t stack_size_;
  std::function<int()> host_fn_;
};

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XTHREAD_H_
