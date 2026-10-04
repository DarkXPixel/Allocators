module;

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
export module darkallocators:OS;

import :utility;

export namespace darkallocators::OS {
class OSAllocator {
public:
  static std::size_t get_page_size() noexcept {
    static std::size_t page_size =
        static_cast<std::size_t>(::sysconf(_SC_PAGE_SIZE));
    return page_size;
  }

  [[nodiscard]] static void *allocate(std::size_t size,
                                      std::size_t alignment) noexcept {
    assert(utility::is_power_of_two(alignment));
    const std::size_t page_size = get_page_size();

    const std::size_t real_alignment = std::max(alignment, page_size);
    const std::size_t alloc_size = utility::align_up(size, page_size);

    if (real_alignment > page_size) {
      const std::size_t extra = alloc_size + real_alignment - page_size;
      void *raw = ::mmap(nullptr, extra, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
      if (raw == MAP_FAILED) [[unlikely]] {
        return nullptr;
      }

      auto raw_addr = reinterpret_cast<std::uintptr_t>(raw);
      auto aligned_addr = utility::align_up(raw_addr, real_alignment);

      const std::size_t prefix = aligned_addr - raw_addr;
      if (prefix > 0) {
        ::munmap(raw, prefix);
      }

      const std::size_t tail_size = extra - prefix - alloc_size;
      if (tail_size > 0) {
        void *tail = reinterpret_cast<void *>(aligned_addr + alloc_size);
        ::munmap(tail, tail_size);
      }

      return reinterpret_cast<void *>(aligned_addr);
    }

    void *ptr = ::mmap(nullptr, alloc_size, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return (ptr == MAP_FAILED) ? nullptr : ptr;
  }

  static void deallocate(void *ptr, std::size_t size) noexcept {
    if (!ptr)
      return;
    const std::size_t page_size = get_page_size();

    const std::size_t alloc_size = utility::align_up(size, page_size);
    ::munmap(ptr, alloc_size);
  }
};
} // namespace darkallocators::OS