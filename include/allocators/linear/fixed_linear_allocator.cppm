module;

#include <cstddef>
#include <cstdint>
export module darkallocators:fixed_linear_allocator;

import :utility;

namespace darkallocators {
export class FixedLinearAllocator {
public:
  explicit FixedLinearAllocator(void *ptr, std::size_t size) noexcept
      : linear_ptr_(reinterpret_cast<std::uintptr_t>(ptr)), linear_size_(size) {
  }

  FixedLinearAllocator(const FixedLinearAllocator &) = delete;
  FixedLinearAllocator &operator=(const FixedLinearAllocator &) = delete;

  FixedLinearAllocator(FixedLinearAllocator &&) = delete;
  FixedLinearAllocator &operator=(FixedLinearAllocator &&) = delete;

  [[nodiscard]] void *allocate(std::size_t size, std::size_t align) noexcept {
    if (size == 0) [[unlikely]] {
      return nullptr;
    }
    const std::uintptr_t cur_ptr = linear_ptr_ + linear_current_;
    const std::uintptr_t aligned_ptr = utility::align_up(cur_ptr, align);
    const std::size_t padding = aligned_ptr - cur_ptr;

    const std::size_t remaining_bytes = linear_size_ - linear_current_;

    if (padding > remaining_bytes || size > (remaining_bytes - padding)) {
      return nullptr;
    }

    linear_current_ += padding + size;
    return reinterpret_cast<void *>(aligned_ptr);
  }

  void deallocate(void *) noexcept {}

  void reset() noexcept { linear_current_ = 0; }

private:
  std::uintptr_t linear_ptr_;
  std::size_t linear_current_{0};
  std::size_t linear_size_;
};

} // namespace darkallocators