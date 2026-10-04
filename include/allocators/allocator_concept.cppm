module;

#include <concepts>
#include <cstddef>
export module darkallocators:allocator_concept;

export namespace darkallocators {
template <typename T>
concept AllocatorConcept =
    requires(T allocator, void *ptr, std::size_t size, std::size_t align) {
      { allocator.allocate(size, align) } -> std::same_as<void *>;
      { allocator.deallocate(ptr, size) } -> std::same_as<void>;
    };
} // namespace darkallocators