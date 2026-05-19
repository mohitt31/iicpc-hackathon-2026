#pragma once
#include <cstdint>
#include <chrono>
#include <thread>
#include <bit>

// Wire protocol is little-endian only. Refuse to compile on big-endian.
static_assert(std::endian::native == std::endian::little,
    "This platform is big-endian. The wire protocol requires little-endian byte order.");

#if defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#endif

namespace hft {

static double g_tsc_to_ns_ratio = 1.0;

#if defined(__x86_64__) || defined(__i386__)
inline void calibrate_tsc() {
    uint32_t aux;
    __builtin_ia32_rdtscp(&aux);
    
    auto t0 = std::chrono::steady_clock::now();
    uint64_t tsc0 = __builtin_ia32_rdtscp(&aux);
    
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    uint64_t tsc1 = __builtin_ia32_rdtscp(&aux);
    auto t1 = std::chrono::steady_clock::now();
    
    auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    uint64_t elapsed_tsc = tsc1 - tsc0;
    
    g_tsc_to_ns_ratio = static_cast<double>(elapsed_ns) / static_cast<double>(elapsed_tsc);
}

static inline uint64_t rdtscp_ns() {
    uint32_t aux;
    uint64_t tsc = __builtin_ia32_rdtscp(&aux);
    return static_cast<uint64_t>(tsc * g_tsc_to_ns_ratio);
}
#elif defined(__aarch64__)
// ARM64 hardware counter
inline void calibrate_tsc() {
    uint64_t tsc0;
    asm volatile("mrs %0, cntvct_el0" : "=r" (tsc0));
    auto t0 = std::chrono::steady_clock::now();
    
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    uint64_t tsc1;
    asm volatile("mrs %0, cntvct_el0" : "=r" (tsc1));
    auto t1 = std::chrono::steady_clock::now();
    
    auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    uint64_t elapsed_tsc = tsc1 - tsc0;
    
    g_tsc_to_ns_ratio = static_cast<double>(elapsed_ns) / static_cast<double>(elapsed_tsc);
}

static inline uint64_t rdtscp_ns() {
    uint64_t tsc;
    asm volatile("mrs %0, cntvct_el0" : "=r" (tsc));
    return static_cast<uint64_t>(tsc * g_tsc_to_ns_ratio);
}
#else
// Fallback for other architectures
inline void calibrate_tsc() {
    g_tsc_to_ns_ratio = 1.0;
}

static inline uint64_t rdtscp_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
}
#endif

} // namespace hft
