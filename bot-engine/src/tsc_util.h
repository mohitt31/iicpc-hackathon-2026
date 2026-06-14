#pragma once
// ============================================================================
// HFT TIMEKEEPING AND MULTI-SOCKET TSC DRIFT WARNING
// ============================================================================
// For the hackathon MVP, we use `rdtscp` to read the CPU's Invariant TSC.
// This is perfectly valid ONLY IF the Network RX, Matching Engine, and
// Network TX threads are strictly pinned to the exact same physical NUMA node.
// 
// THE FLAW: If Ingress is stamped on Socket 0, and Egress on Socket 1, the 
// math is fundamentally broken. Different physical sockets have independent
// oscillators. Due to boot-skew and thermal drift, Socket 0's TSC might 
// lead or lag Socket 1 by thousands of cycles. Subtracting them yields 
// negative or hallucinated latencies.
//
// THE TIER-1 PRODUCTION FIX (IEEE 1588 PTP):
// In production, we do not trust the CPU clock. We use Hardware NIC Timestamping.
// The Solarflare/Mellanox SmartNIC maintains a GPS-synced atomic clock (PTP).
// - Ingress: The NIC writes a 64-bit nanosecond timestamp into the descriptor
//   the exact instant the Start-of-Frame hits the MAC layer.
// - Egress: The NIC stamps the packet the exact instant it leaves the port.
//
// Latency = NIC_TX_Timestamp - NIC_RX_Timestamp. 
// This guarantees pure Wire-to-Wire latency measurement, mathematically 
// immune to CPU topology, PCIe delays, and OS thread scheduling.
// ============================================================================

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

inline uint64_t g_tsc_ns_num = 1;
inline uint64_t g_tsc_ns_den = 1;

#if defined(__SIZEOF_INT128__)
// 128-bit intermediate for tick→ns scaling: (tsc * num) overflows u64
// after ~minutes of uptime at GHz rates; widening makes it exact.
// __extension__ keeps -Wpedantic quiet about the GCC/Clang extension.
__extension__ typedef unsigned __int128 u128;
#endif

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
    
    g_tsc_ns_num = static_cast<uint64_t>(elapsed_ns);
    g_tsc_ns_den = elapsed_tsc;
}

static inline uint64_t rdtscp_ns() {
    uint32_t aux;
    uint64_t tsc = __builtin_ia32_rdtscp(&aux);
    return static_cast<uint64_t>( (static_cast<u128>(tsc) * g_tsc_ns_num) / g_tsc_ns_den );
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
    
    g_tsc_ns_num = static_cast<uint64_t>(elapsed_ns);
    g_tsc_ns_den = elapsed_tsc;
}

static inline uint64_t rdtscp_ns() {
    uint64_t tsc;
    asm volatile("mrs %0, cntvct_el0" : "=r" (tsc));
    return static_cast<uint64_t>( (static_cast<u128>(tsc) * g_tsc_ns_num) / g_tsc_ns_den );
}
#else
// Fallback for other architectures
inline void calibrate_tsc() {
    g_tsc_ns_num = 1;
    g_tsc_ns_den = 1;
}

static inline uint64_t rdtscp_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
}
#endif

} // namespace hft
