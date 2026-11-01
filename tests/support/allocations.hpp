#pragma once

#include <atomic>
#include <cstddef>
#include <thread>

// The test program replaces the global operator new (allocations.cpp), so
// tests can see how much memory code uses and make an allocation fail.
// Replacing it is the standard way to do that; it applies to the whole
// test program.
//
// - Live bytes are counted all the time. While gCounting is on, gAllocated
//   adds up what's allocated and gPeak keeps the most live at once.
// - If gFailIn is set, the gFailIn-th allocation on a thread other than
//   gMainThread throws std::bad_alloc, as when memory runs out.
//
// Globals, because operator new has nowhere else to keep them.
// NOLINTBEGIN(*-avoid-non-const-global-variables)
extern std::atomic<bool> gCounting;
extern std::atomic<std::size_t> gAllocated;
extern std::atomic<std::size_t> gLive;
extern std::atomic<std::size_t> gPeak;
extern std::atomic<long> gFailIn;
extern std::atomic<std::thread::id> gMainThread;
// NOLINTEND(*-avoid-non-const-global-variables)
