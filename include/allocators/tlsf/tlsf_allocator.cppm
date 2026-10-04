module;

#include <algorithm>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
export module darkallocators:tlsf;

import :utility;
import :allocator_concept;

namespace darkallocators {

inline constexpr std::size_t FL_INDEX_MAX = 64;
inline constexpr std::size_t SL_INDEX_SHIFT = 5;
inline constexpr std::size_t SL_INDEX_COUNT = 1U << SL_INDEX_SHIFT; // 32
inline constexpr std::size_t MIN_BLOCK_SIZE = sizeof(void *) * 4;
static constexpr std::size_t ALIGNMENT = alignof(std::max_align_t);

struct alignas(ALIGNMENT) BlockHeader {
  std::size_t size_and_flags;

  BlockHeader *prev_physical;
  BlockHeader *next_free;
  BlockHeader *prev_free;

  static constexpr std::size_t FLAG_FREE = 0b1;
  static constexpr std::size_t FLAG_PREV_FREE = 0b10;
  static constexpr std::size_t FLAGS_MASK = 0b11;
  static constexpr std::size_t SIZE_MASK = ~FLAGS_MASK;

  [[nodiscard]] std::size_t size() const noexcept {
    return size_and_flags & SIZE_MASK;
  }

  [[nodiscard]] bool is_free() const noexcept {
    return (size_and_flags & FLAG_FREE) != 0;
  }

  [[nodiscard]] bool is_prev_free() const noexcept {
    return (size_and_flags & FLAG_PREV_FREE) != 0;
  }

  void set_size(std::size_t size) noexcept {
    size_and_flags = (size_and_flags & FLAGS_MASK) | (size & SIZE_MASK);
  }

  void set_free(bool free) noexcept {
    size_and_flags =
        free ? (size_and_flags | FLAG_FREE) : (size_and_flags & ~FLAG_FREE);
  }

  void set_prev_free(bool free) noexcept {
    size_and_flags = free ? (size_and_flags | FLAG_PREV_FREE)
                          : (size_and_flags & ~FLAG_PREV_FREE);
  }

  [[nodiscard]] BlockHeader *next_physical() const noexcept {
    return reinterpret_cast<BlockHeader *>(
        reinterpret_cast<std::uintptr_t>(this) + sizeof(BlockHeader) + size());
  }

  [[nodiscard]] void *user_ptr() const noexcept {
    return reinterpret_cast<void *>(reinterpret_cast<std::uintptr_t>(this) +
                                    sizeof(BlockHeader));
  }

  [[nodiscard]] static BlockHeader *from_user_ptr(void *ptr) noexcept {
    return reinterpret_cast<BlockHeader *>(
        reinterpret_cast<std::uintptr_t>(ptr) - sizeof(BlockHeader));
  }
};

static constexpr std::size_t HEADER_SIZE = sizeof(BlockHeader);

struct TLSFPool {
  void *memory{nullptr};
  std::size_t size{0};
  TLSFPool *next{nullptr};
};

struct TLSFControl {
  std::uint64_t fl_bitmap{0};
  std::uint64_t sl_bitmap[FL_INDEX_MAX] = {};
  BlockHeader *free_lists[FL_INDEX_MAX][SL_INDEX_COUNT];
  BlockHeader null_block{};

  TLSFControl() noexcept {
    for (auto &fl : free_lists) {
      for (auto &sl : fl) {
        sl = &null_block;
      }
    }

    null_block.next_free = &null_block;
    null_block.prev_free = &null_block;
  }
};

static_assert(std::has_single_bit(ALIGNMENT));
static_assert(std::has_single_bit(MIN_BLOCK_SIZE));
static_assert(MIN_BLOCK_SIZE >= sizeof(void *) * 2);
static_assert(HEADER_SIZE % ALIGNMENT == 0);

export template <AllocatorConcept allocator> class TLSFAllocator {
public:
  explicit TLSFAllocator(allocator &alloc) noexcept : alloc_(alloc) {}
  ~TLSFAllocator() { free_all_pool(); }

  bool grow(std::size_t min_size = 4096) {
    const std::size_t pool_size = std::max(min_size, 4096UZ);
    return add_pool(pool_size);
  }

  [[nodiscard]] void *allocate(std::size_t size, std::size_t align) noexcept {
    if (size == 0) [[unlikely]] {
      return nullptr;
    }
    const std::size_t req_align = std::max(align, ALIGNMENT);

    const std::size_t adjusted_size = adjust_required_size(size, req_align);

    const std::size_t search_size =
        (req_align > ALIGNMENT)
            ? (adjusted_size + req_align + sizeof(BlockHeader) + MIN_BLOCK_SIZE)
            : adjusted_size;

    BlockHeader *block = find_free_block(search_size);
    if (block == nullptr) {
      if (!grow(search_size)) [[unlikely]] {
        return nullptr;
      }
      block = find_free_block(search_size);
      if (block == nullptr) [[unlikely]] {
        return nullptr;
      }
    }

    if (req_align > ALIGNMENT) {
      block = align_block(block, adjusted_size, req_align);
    }

    split_block(block, adjusted_size);
    block->set_free(false);
    block->next_physical()->set_prev_free(false);
    void *ptr = block->user_ptr();
    return ptr;
  }

  void deallocate(void *ptr) noexcept {
    if (ptr == nullptr) [[unlikely]] {
      return;
    }

    BlockHeader *block = BlockHeader::from_user_ptr(ptr);
    const std::size_t size = block->size();
    block->set_free(true);
    block = merge_prev(block);
    block = merge_next(block);
    block->next_physical()->set_prev_free(true);
    insert_free_block(block);
  }

  [[nodiscard]] std::size_t available() const noexcept {
    std::size_t total = 0;

    for (std::size_t fl = 0; fl < FL_INDEX_MAX; ++fl) {
      for (std::size_t sl = 0; sl < SL_INDEX_COUNT; ++sl) {
        BlockHeader *block = control_.free_lists[fl][sl];

        while (block != &control_.null_block) {
          total += block->size();
          block = block->next_free;
        }
      }
    }
    return total;
  }

  [[nodiscard]]
  std::size_t debug_available() const noexcept {
    std::size_t total = 0;

    for (std::size_t fl = 0; fl < FL_INDEX_MAX; ++fl) {
      for (std::size_t sl = 0; sl < SL_INDEX_COUNT; ++sl) {
        BlockHeader *block = control_.free_lists[fl][sl];

        while (block != &control_.null_block) {
          total += block->size();
          block = block->next_free;
        }
      }
    }

    return total;
  }

private:
  struct Mapping {
    std::uint64_t fl;
    std::uint64_t sl;
  };

  static std::size_t adjust_required_size(std::size_t size,
                                          std::size_t align) noexcept {
    std::size_t adjusted = std::max(size, MIN_BLOCK_SIZE);
    return align > alignof(std::max_align_t)
               ? utility::align_up(adjusted, align)
               : utility::align_up(adjusted, alignof(std::max_align_t));
  }

  static Mapping mapping_search(std::size_t size) noexcept {
    if (size < MIN_BLOCK_SIZE) {
      size = MIN_BLOCK_SIZE;
    }

    const std::size_t fl_approx = 63U - std::countl_zero(size);
    if (fl_approx >= SL_INDEX_SHIFT) {
      const std::size_t round_mask = (1ULL << (fl_approx - SL_INDEX_SHIFT)) - 1;
      if (size <= std::numeric_limits<std::size_t>::max() - round_mask) {
        size += round_mask;
      }
    }

    const std::size_t fl = 63U - std::countl_zero(size);
    const std::size_t shift =
        (fl >= SL_INDEX_SHIFT) ? (fl - SL_INDEX_SHIFT) : 0;
    const std::size_t sl = (size >> shift) & (SL_INDEX_COUNT - 1);

    return {fl, sl};
  }

  static Mapping mapping_insert(std::size_t size) noexcept {
    if (size < MIN_BLOCK_SIZE) {
      return {0, 0};
    }
    const std::size_t fl = 63U - std::countl_zero(size);
    const std::size_t sl =
        (size >> (fl - SL_INDEX_SHIFT)) & (SL_INDEX_COUNT - 1);
    return {fl, sl};
  }

  void insert_free_block(BlockHeader *block) noexcept {
    const auto [fl, sl] = mapping_insert(block->size());

    BlockHeader *header = control_.free_lists[fl][sl];
    block->next_free = header;
    block->prev_free = &control_.null_block;
    header->prev_free = block;

    control_.free_lists[fl][sl] = block;
    control_.fl_bitmap |= (1ULL << fl);
    control_.sl_bitmap[fl] |= (1ULL << sl);
  }

  void remove_free_block(BlockHeader *block, std::uint64_t fl,
                         std::uint64_t sl) noexcept {
    BlockHeader *prev = block->prev_free;
    BlockHeader *next = block->next_free;

    prev->next_free = next;
    next->prev_free = prev;

    if (control_.free_lists[fl][sl] == block) {
      control_.free_lists[fl][sl] = next;
      if (next == &control_.null_block) {
        control_.sl_bitmap[fl] &= ~(1ULL << sl);

        if (control_.sl_bitmap[fl] == 0) {
          control_.fl_bitmap &= ~(1ULL << fl);
        }
      }
    }
  }

  [[nodiscard]] BlockHeader *align_block(BlockHeader *block, std::size_t size,
                                         std::size_t align) noexcept {
    const std::uintptr_t user_addr =
        reinterpret_cast<std::uintptr_t>(block->user_ptr());
    const std::uintptr_t aligned_user_addr =
        utility::align_up(user_addr, align);
    if (user_addr == aligned_user_addr) {
      return block;
    }

    std::size_t offset = aligned_user_addr - user_addr;
    if (offset < sizeof(BlockHeader) + MIN_BLOCK_SIZE) {
      const std::size_t needed =
          (sizeof(BlockHeader) + MIN_BLOCK_SIZE) - offset;
      const std::size_t align_steps = utility::align_up(needed, align);
      offset += align_steps;
    }

    BlockHeader *aligned_block = reinterpret_cast<BlockHeader *>(
        reinterpret_cast<std::uintptr_t>(block) + offset);
    const std::size_t prev_size = offset - sizeof(BlockHeader);
    const std::size_t remaining_size = block->size() - offset;

    block->set_size(prev_size);
    block->set_free(true);

    aligned_block->size_and_flags = 0;
    aligned_block->set_size(remaining_size);
    aligned_block->set_free(true);
    aligned_block->set_prev_free(true);
    aligned_block->prev_physical = block;

    link_next(aligned_block);

    insert_free_block(block);
    return aligned_block;
  }

  [[nodiscard]] BlockHeader *find_free_block(std::size_t size) noexcept {
    auto [fl, sl] = mapping_search(size);
    std::uint64_t sl_map = control_.sl_bitmap[fl] & (~0ULL << sl);

    if (sl_map == 0) {
      if (fl >= 63) {
        return nullptr;
      }
      const std::uint64_t fl_map = control_.fl_bitmap & (~0ULL << (fl + 1));
      if (fl_map == 0) {
        return nullptr;
      }
      fl = std::countr_zero(fl_map);
      sl_map = control_.sl_bitmap[fl];
    }

    sl = std::countr_zero(sl_map);
    BlockHeader *block = control_.free_lists[fl][sl];
    remove_free_block(block, fl, sl);
    return block;
  }

  [[nodiscard]] BlockHeader *merge_prev(BlockHeader *block) noexcept {
    if (!block->is_prev_free()) {
      return block;
    }

    BlockHeader *prev = block->prev_physical;

    const auto [fl, sl] = mapping_insert(prev->size());
    remove_free_block(prev, fl, sl);

    prev->set_size(prev->size() + sizeof(BlockHeader) + block->size());
    link_next(prev);
    return prev;
  }

  [[nodiscard]] BlockHeader *merge_next(BlockHeader *block) noexcept {
    BlockHeader *next = block->next_physical();
    if (next->size() == 0 || !next->is_free()) {
      return block;
    }

    const auto [fl, sl] = mapping_insert(next->size());
    remove_free_block(next, fl, sl);
    block->set_size(block->size() + sizeof(BlockHeader) + next->size());
    link_next(block);
    return block;
  }
  void link_next(BlockHeader *block) noexcept {
    BlockHeader *next = block->next_physical();
    next->prev_physical = block;
  }

  void split_block(BlockHeader *block, std::size_t size) noexcept {
    const std::size_t required_size =
        size + sizeof(BlockHeader) + MIN_BLOCK_SIZE;
    if (block->size() < required_size) {
      return;
    }
    const std::size_t remain_size = block->size() - size - sizeof(BlockHeader);

    BlockHeader *remain = reinterpret_cast<BlockHeader *>(
        reinterpret_cast<std::uintptr_t>(block) + sizeof(BlockHeader) + size);
    remain->set_size(remain_size);
    remain->set_free(true);
    remain->prev_physical = block;
    block->set_size(size);
    link_next(remain);

    remain->next_physical()->set_prev_free(true);
    insert_free_block(remain);
  }

  bool add_pool(std::size_t pool_size) noexcept {
    constexpr std::size_t pool_struct_size =
        utility::align_up(sizeof(TLSFPool), alignof(std::max_align_t));
    static_assert(
        sizeof(BlockHeader) ==
            utility::align_up(sizeof(BlockHeader), alignof(std::align_val_t)),
        "BlockHeader must be aligned to target alignment");

    const std::size_t total = pool_struct_size + sizeof(BlockHeader) +
                              pool_size + sizeof(BlockHeader);
    void *memory = alloc_.allocate(total, alignof(std::max_align_t));
    if (memory == nullptr) [[unlikely]] {
      return false;
    }

    TLSFPool *pool = static_cast<TLSFPool *>(memory);
    pool->memory = memory;
    pool->size = total;
    pool->next = pool_list_;
    pool_list_ = pool;

    BlockHeader *block = reinterpret_cast<BlockHeader *>(
        reinterpret_cast<std::uintptr_t>(memory) + pool_struct_size);

    const std::size_t usable =
        total - pool_struct_size - sizeof(BlockHeader) - sizeof(BlockHeader);

    block->set_size(usable);
    block->set_free(true);
    block->set_prev_free(false);
    block->prev_physical = nullptr;
    block->next_free = &control_.null_block;

    BlockHeader *sentiel = block->next_physical();
    sentiel->size_and_flags = 0;
    sentiel->prev_physical = block;
    sentiel->set_prev_free(true);

    insert_free_block(block);
    return true;
  }

  void free_all_pool() noexcept {
    TLSFPool *pool = pool_list_;
    while (pool != nullptr) {
      TLSFPool *next = pool->next;
      alloc_.deallocate(pool->memory, pool->size);
      pool = next;
    }
    pool_list_ = nullptr;
  }

  TLSFControl control_;
  TLSFPool *pool_list_{nullptr};

  allocator &alloc_;
};
} // namespace darkallocators