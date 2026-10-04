module;

#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
export module darkallocators:utility;

export namespace darkallocators::utility {
[[nodiscard]] constexpr bool is_power_of_two(std::size_t val) noexcept {
  return val != 0 && (val & (val - 1)) == 0;
}

template <std::integral T>
[[nodiscard]] constexpr T align_up(T val, std::size_t align) noexcept {
  assert(is_power_of_two(align));
  return (val + align - 1) & ~(align - 1);
}

template <typename T>
[[nodiscard]] constexpr T *align_up_ptr(const T *ptr,
                                        std::size_t align) noexcept {
  assert(is_power_of_two(align));
  return reinterpret_cast<T *>(
      align_up(reinterpret_cast<std::uintptr_t>(ptr), align));
}

[[nodiscard]] constexpr std::size_t align_down(std::size_t val,
                                               std::size_t align) noexcept {
  assert(is_power_of_two(align));
  return val & ~(align - 1);
}
template <typename T>
[[nodiscard]] constexpr bool is_aligned_ptr(const T *ptr,
                                            std::size_t align) noexcept {
  assert(is_power_of_two(align));
  return (reinterpret_cast<std::uintptr_t>(ptr) & (align - 1)) == 0;
}

[[nodiscard]] constexpr std::size_t
next_power_of_two(std::size_t val) noexcept {
  if (val == 0) {
    return 1;
  }
  --val;
  val |= val >> 1;
  val |= val >> 2;
  val |= val >> 4;
  val |= val >> 8;
  val |= val >> 16;
  val |= val >> 32;
  return val + 1;
}

} // namespace darkallocators::utility