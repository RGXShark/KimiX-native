#pragma once

#include <atomic>
#include <core/intrin.h>

namespace kimix {

// ---------------------------------------------------------------------------
// spin_mutex — simple spinlock using std::atomic_flag
// ---------------------------------------------------------------------------

class spin_mutex {
public:
  spin_mutex() noexcept { _flag.clear(); }

  spin_mutex(const spin_mutex &) = delete;
  spin_mutex &operator=(const spin_mutex &) = delete;
  spin_mutex(spin_mutex &&) = delete;
  spin_mutex &operator=(spin_mutex &&) = delete;

  void lock() noexcept {
    while (_flag.test_and_set(std::memory_order_acquire)) {
      // spin until lock is acquired
      while (_flag.test(std::memory_order_relaxed)) {
        KIMIX_INTRIN_PAUSE();
      }
    }
  }

  bool try_lock() noexcept {
    return !_flag.test_and_set(std::memory_order_acquire);
  }

  void unlock() noexcept { _flag.clear(std::memory_order_release); }

private:
  std::atomic_flag _flag;
};

} // namespace kimix
