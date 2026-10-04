#include <algorithm>
#include <cassert>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <iostream>
#include <memory>
#include <print>
#include <random>
#include <vector>

import darkallocators;

namespace TLSFTest {

template <typename Allocator> void run_correctness_test(Allocator &allocator) {
  std::cout << "[+] [Correctness] Running TLSF...\n";

  constexpr size_t NUM_ALLOCS = 10'000;
  constexpr size_t MAX_SIZE = 8 * 1024; // 8 KB
  constexpr size_t ALIGNMENTS[] = {4, 8, 16, 32, 64};

  std::mt19937_64 rng(1337);
  std::uniform_int_distribution<size_t> dist_size(1, MAX_SIZE);
  std::uniform_int_distribution<size_t> dist_align_idx(
      0, std::size(ALIGNMENTS) - 1);

  struct Allocation {
    uint8_t *ptr;
    size_t size;
    size_t alignment;
    uint8_t pattern;
  };

  std::vector<Allocation> allocs;
  allocs.reserve(NUM_ALLOCS);

  for (size_t i = 0; i < NUM_ALLOCS; ++i) {
    size_t sz = dist_size(rng);
    size_t align = ALIGNMENTS[dist_align_idx(rng)];

    auto *p = static_cast<uint8_t *>(allocator.allocate(sz, align));
    if (!p) {
      std::cerr << "[-] Error: Allocator not allocate mem " << sz
                << " bytes!\n";
      std::exit(EXIT_FAILURE);
    }

    assert(reinterpret_cast<uintptr_t>(p) % align == 0 &&
           "Error: Error align!");

    uint8_t pattern = static_cast<uint8_t>((i ^ sz) & 0xFF);
    std::fill_n(p, sz, pattern);

    allocs.push_back({p, sz, align, pattern});
  }

  for (const auto &alloc : allocs) {
    for (size_t j = 0; j < alloc.size; ++j) {
      if (alloc.ptr[j] != alloc.pattern) {
        std::cerr << "[-] Error:  corruption memory"
                  << static_cast<void *>(alloc.ptr + j) << "\n";
        std::exit(EXIT_FAILURE);
      }
    }
  }

  std::ranges::shuffle(allocs, rng);

  const size_t half = allocs.size() / 2;
  for (size_t i = 0; i < half; ++i) {
    allocator.deallocate(allocs[i].ptr);
  }

  for (size_t i = 0; i < half; ++i) {
    size_t sz = dist_size(rng);
    size_t align = ALIGNMENTS[dist_align_idx(rng)];
    void *p = allocator.allocate(sz, align);

    assert(p != nullptr && "Fragmented memory");
    allocator.deallocate(p);
  }

  for (size_t i = half; i < allocs.size(); ++i) {
    allocator.deallocate(allocs[i].ptr);
  }

  std::cout << "[✓] [Correctness] Its oK!\n\n";
}

struct BenchResult {
  std::chrono::nanoseconds total_time;
  size_t operations;

  [[nodiscard]] double ns_per_op() const {
    return static_cast<double>(total_time.count()) /
           static_cast<double>(operations);
  }
};

template <typename AllocFunc, typename FreeFunc>
BenchResult run_benchmark(size_t iterations, AllocFunc &&alloc_fn,
                          FreeFunc &&free_fn) {
  constexpr size_t POOL_SLOTS = 16'384;
  std::vector<void *> ptrs(POOL_SLOTS, nullptr);

  std::mt19937_64 rng(42);
  std::uniform_int_distribution<size_t> dist_size(16, 1024);
  std::uniform_int_distribution<size_t> dist_slot(0, POOL_SLOTS - 1);

  auto start = std::chrono::high_resolution_clock::now();

  size_t ops = 0;
  for (size_t i = 0; i < iterations; ++i) {
    size_t slot = dist_slot(rng);

    if (ptrs[slot] != nullptr) {
      free_fn(ptrs[slot]);
      ptrs[slot] = nullptr;
    } else {
      size_t sz = dist_size(rng);
      ptrs[slot] = alloc_fn(sz);
      assert(ptrs[slot] != nullptr);
    }
    ++ops;
  }

  for (void *p : ptrs) {
    if (p)
      free_fn(p);
  }

  auto end = std::chrono::high_resolution_clock::now();
  return {std::chrono::duration_cast<std::chrono::nanoseconds>(end - start),
          ops};
}
} // namespace TLSFTest

int main() {
  std::println("Hello, World!");

  darkallocators::TLSFAllocator<darkallocators::OS::OSAllocator> allocator;
  allocator.grow(128 * 1024 * 1024);

  std::println("Avaliable: {}", allocator.available());

  TLSFTest::run_correctness_test(allocator);

  constexpr size_t BENCH_ITERATIONS = 5'000'000;
  std::cout << "[+] [Benchmark] Run benchmark(" << BENCH_ITERATIONS
            << " ops)...\n";

  auto std_res = TLSFTest::run_benchmark(
      BENCH_ITERATIONS, [](size_t sz) { return std::malloc(sz); },
      [](void *p) { std::free(p); });

  auto tlsf_res = TLSFTest::run_benchmark(
      BENCH_ITERATIONS,
      [&allocator](size_t sz) {
        return allocator.allocate(sz, alignof(std::max_align_t));
      },
      [&allocator](void *p) { allocator.deallocate(p); });

  std::cout << "--------------------------------------------------\n";
  std::cout << "std::malloc / std::free : " << std_res.ns_per_op()
            << " ns/op\n";
  std::cout << "TLSF allocate / deallocate: " << tlsf_res.ns_per_op()
            << " ns/op\n";
  std::cout << "--------------------------------------------------\n";

  if (tlsf_res.ns_per_op() < std_res.ns_per_op()) {
    std::cout << "[End] TLSF faster malloc "
              << std_res.ns_per_op() / tlsf_res.ns_per_op() << "раз(а).\n ";
  } else {
    std::cout << "[End] Malloc faster.\n";
  }

  std::println("Avaliable: {}", allocator.available());

  return 0;
}