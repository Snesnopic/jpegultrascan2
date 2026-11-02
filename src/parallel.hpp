#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <thread>
#include <vector>

namespace jpegultrascan::detail {

inline unsigned thread_count(unsigned requested) {
    if (requested) return requested;
    const unsigned hw = std::thread::hardware_concurrency();
    return hw ? hw : 1;
}

// calls f(i) for i in [0, n) on up to `threads` threads; f must not throw. which thread
// runs which index varies, so f may only write to slots of its own index.
template <class F>
void parallel_for(std::size_t n, unsigned threads, F&& f) {
    threads = static_cast<unsigned>(std::min<std::size_t>(threads ? threads : 1, n));
    if (threads <= 1) {
        for (std::size_t i = 0; i < n; ++i) f(i);
        return;
    }
    std::atomic<std::size_t> next{0};
    const auto work = [&] {
        for (std::size_t i; (i = next.fetch_add(1, std::memory_order_relaxed)) < n;) f(i);
    };
    std::vector<std::thread> pool;
    pool.reserve(threads - 1);
    for (unsigned t = 1; t < threads; ++t) pool.emplace_back(work);
    work();
    for (std::thread& t : pool) t.join();
}

}  // namespace jpegultrascan::detail
