#include "allocations.hpp"

#ifdef __APPLE__
#include <malloc/malloc.h>
#else
#include <malloc.h>
#endif

#include <cstdlib>
#include <new>

// NOLINTBEGIN(*-avoid-non-const-global-variables)
std::atomic<bool> gCounting = false;
std::atomic<std::size_t> gAllocated = 0;
std::atomic<std::size_t> gLive = 0;
std::atomic<std::size_t> gPeak = 0;
std::atomic<long> gFailIn = 0;
std::atomic<std::thread::id> gMainThread;
// NOLINTEND(*-avoid-non-const-global-variables)

namespace {

// How big the block really is, so delete can count it off again.
// Not const void*: glibc's malloc_usable_size takes a plain void*.
std::size_t blockSize(void* memory) {  // NOLINT(misc-const-correctness)
#ifdef __APPLE__
    return malloc_size(memory);
#else
    return malloc_usable_size(memory);
#endif
}
}  // namespace

// The parameters are named as in libc++'s own declarations, because
// clang-tidy wants the names to match and can't be told otherwise there.
// Names starting with __ are reserved for the library, hence the NOLINT.
// NOLINTNEXTLINE(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp)
void* operator new(std::size_t __sz) {
    if (gFailIn > 0 && std::this_thread::get_id() != gMainThread.load()) {
        // fetch_sub returns the value from before it subtracted.
        if (gFailIn.fetch_sub(1) == 1) {
            throw std::bad_alloc();
        }
    }
    void* memory = std::malloc(__sz == 0 ? 1 : __sz);  // NOLINT(*-no-malloc,*-owning-memory)
    if (memory == nullptr) {
        throw std::bad_alloc();
    }
    const std::size_t live = gLive += blockSize(memory);
    if (gCounting) {
        gAllocated += __sz;
        // Raise gPeak to `live` unless another thread already raised it
        // higher. compare_exchange_weak updates `peak` when it fails.
        std::size_t peak = gPeak;
        while (live > peak && !gPeak.compare_exchange_weak(peak, live)) {
        }
    }
    return memory;
}

// NOLINTNEXTLINE(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp)
void operator delete(void* __p) noexcept {
    if (__p != nullptr) {
        gLive -= blockSize(__p);
    }
    std::free(__p);  // NOLINT(*-no-malloc,*-owning-memory)
}

// NOLINTNEXTLINE(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp)
void operator delete(void* __p, std::size_t /*__sz*/) noexcept {
    operator delete(__p);
}
