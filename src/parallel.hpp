#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <exception>
#include <functional>
#include <mutex>
#include <system_error>
#include <thread>
#include <vector>

#include "jpegultrascan/jpegultrascan.hpp"

namespace jpegultrascan::detail {

// thrown by a piece of work once stop_requested says so; find_best_script turns it into
// Status::Interrupted
struct Stopped : std::exception {
    const char* what() const noexcept override { return "interrupted"; }
};

// 0: half the hardware's threads, leaving the rest to everything else
inline unsigned thread_count(unsigned requested) {
    if (requested) return requested;
    return std::max(1U, std::thread::hardware_concurrency() / 2);
}

// the loop behind the engine when the host gives none, with the same contract: body(i) once
// for every i < n, then the first exception thrown, once every started piece is done
inline ParallelFor own_threads(unsigned threads) {
    return [threads](std::size_t n, const std::function<void(std::size_t)>& body) {
        const auto count = static_cast<unsigned>(std::min<std::size_t>(threads, n));
        if (count <= 1) {
            for (std::size_t i = 0; i < n; ++i) body(i);
            return;
        }
        std::atomic<std::size_t> next{0};
        std::mutex mutex;
        std::exception_ptr error;
        const auto work = [&] {
            for (std::size_t i = 0; (i = next.fetch_add(1, std::memory_order_relaxed)) < n;) {
                try {
                    body(i);
                } catch (...) {
                    // kept for the caller; the pieces nobody has started are dropped
                    const std::scoped_lock lock(mutex);
                    if (!error) error = std::current_exception();
                    next.store(n, std::memory_order_relaxed);
                }
            }
        };
        std::vector<std::thread> pool;
        pool.reserve(count - 1);
        for (unsigned t = 1; t < count; ++t) {
            try {
                pool.emplace_back(work);
            } catch (const std::system_error&) {
                // fewer threads, same work
                break;
            }
        }
        work();
        for (std::thread& t : pool) t.join();
        if (error) std::rethrow_exception(error);
    };
}

// how the engine runs independent pieces of work: the host's loop or threads of its own
struct Loop {
    ParallelFor run;
    unsigned threads = 1;  // behind run, 0 when the host didn't say
    std::function<bool()> stop;

    void check_stop() const {
        if (stop && stop()) throw Stopped();
    }
};

inline Loop own_loop(unsigned threads) {
    const unsigned count = thread_count(threads);
    return {own_threads(count), count, {}};
}

inline Loop loop_for(const Options& options) {
    if (options.parallel_for) return {options.parallel_for, options.threads, options.stop_requested};
    Loop loop = own_loop(options.threads);
    loop.stop = options.stop_requested;
    return loop;
}

// f may only write to slots of its own index: which thread runs which index, and in what
// order, varies
template <class F>
void parallel_for(const Loop& loop, std::size_t n, const F& f) {
    if (n == 0) return;
    const std::function<void(std::size_t)> body = [&f](std::size_t i) { f(i); };
    loop.run(n, body);
}

}  // namespace jpegultrascan::detail
