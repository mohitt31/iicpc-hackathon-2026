// bot.cpp — Open-loop multi-threaded latency benchmarking client
// Demonstrates Coordinated Omission detection and correction at scale.
//
// Architecture:
//   - Main thread (cold): args, thread launch, histogram merge, results
//   - N worker threads (hot, CPU-pinned): independent send/recv loops
//   - Zero shared state on hot path — each thread owns its socket, pool,
//     RNG, and histograms. Main merges histograms at the end.
//
// Why CPU pinning?
//   Without pinning, OS scheduler moves the bot across cores, causing
//   ~1ms preemption jitter that pollutes the histogram. With each thread
//   locked to one isolated core, the only stalls visible are real ones.

#include <iostream>
#include <iomanip>
#include <cstdint>
#include <cstring>
#include <cerrno>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <string>
#include <stdexcept>
#include <vector>
#include <chrono>
#include <thread>
#include <pthread.h>
#include <sched.h>

#include "contracts/interface_contract_v1.h"
#include "tsc_util.h"
#include <hdr/hdr_histogram.h>

#ifdef __cpp_lib_hardware_interference_size
using std::hardware_destructive_interference_size;
#else
constexpr std::size_t hardware_destructive_interference_size = 64;
#endif

// ── xorshift64 RNG ────────────────────────────────────────────────────────
static inline uint64_t xorshift64(uint64_t& state) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}

// ── Memory Pool (per-thread, zero-malloc on hot path) ────────────────────
struct OrderPool {
    static constexpr size_t POOL_SIZE = 1024;
    alignas(64) NewOrder slots[POOL_SIZE];
    uint16_t free_stack[POOL_SIZE];
    uint16_t free_top = 0;
    uint64_t exhausted_count = 0;

    OrderPool() {
        for (uint16_t i = 0; i < POOL_SIZE; i++) free_stack[i] = i;
        free_top = POOL_SIZE;
    }

    inline NewOrder* acquire() {
        if (__builtin_expect(free_top == 0, 0)) {
            exhausted_count++;
            return nullptr;
        }
        return &slots[free_stack[--free_top]];
    }

    inline void release(NewOrder* ptr) {
        uint16_t idx = static_cast<uint16_t>(ptr - slots);
        free_stack[free_top++] = idx;
    }
};

// ── Per-thread config and result structs ─────────────────────────────────
struct BotConfig {
    uint32_t    thread_id;
    int         cpu_core;       // -1 = no pinning
    std::string ip_addr;
    uint16_t    port;
    uint64_t    interval_ns;
    uint64_t    duration_ns;
};

struct BotResult {
    struct hdr_histogram* naive_hist = nullptr;
    struct hdr_histogram* co_hist    = nullptr;
    uint64_t total_sent           = 0;
    uint64_t total_acked          = 0;
    uint64_t total_fills          = 0;
    uint64_t total_rejects        = 0;
    uint64_t send_failures        = 0;
    uint64_t partial_send_aborts  = 0;
    uint64_t pending_collisions   = 0;
    uint64_t pool_exhausted       = 0;
    bool     connected            = false;
};

static void set_non_blocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) throw std::runtime_error("fcntl F_GETFL failed");
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1)
        throw std::runtime_error("fcntl F_SETFL failed");
}

// Pin current thread to specific CPU core.
// Called from inside the worker so the thread's TLS/stack pages get
// allocated on the target core's NUMA node.
static bool pin_to_core(int core_id) {
    if (core_id < 0) return false;
#ifdef __APPLE__
    // macOS does not support sched_setaffinity or pthread_setaffinity_np
    return false;
#else
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    return rc == 0;
#endif
}

// ══════════════════════════════════════════════════════════════════════════
// WORKER THREAD — one per bot. CPU-pinned, independent send/recv loop.
// ══════════════════════════════════════════════════════════════════════════
static void bot_worker(BotConfig config, BotResult* result) {
    // Pin to dedicated CPU core (skip core 0 — handles most IRQs)
    if (config.cpu_core >= 0) {
        if (pin_to_core(config.cpu_core)) {
            std::cout << "[Bot " << config.thread_id << "] Pinned to core "
                      << config.cpu_core << "\n";
        } else {
            std::cerr << "[Bot " << config.thread_id
                      << "] WARNING: could not pin to core "
                      << config.cpu_core << " (perhaps not supported on this OS)\n";
        }
    }

    // Per-thread HDR histograms
    hdr_init(1, 30000000000LL, 3, &result->naive_hist);
    hdr_init(1, 30000000000LL, 3, &result->co_hist);

    // Per-thread memory pool
    OrderPool pool;

    // Per-thread RNG (seed = 42 + thread_id → different stream per bot,
    // reproducible across runs for the same thread_id).
    uint64_t rng_state = 42 + config.thread_id;

    // Socket setup
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        std::cerr << "[Bot " << config.thread_id << "] socket() failed\n";
        return;
    }

    sockaddr_in serv_addr{};
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port   = htons(config.port);
    inet_pton(AF_INET, config.ip_addr.c_str(), &serv_addr.sin_addr);

    if (connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        std::cerr << "[Bot " << config.thread_id << "] connect() failed: "
                  << strerror(errno) << "\n";
        close(sock);
        return;
    }
    result->connected = true;

    int opt = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
    int sndbuf_size = 4096;
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sndbuf_size, sizeof(sndbuf_size));
    set_non_blocking(sock);

    // Pending map (per-thread, no locks)
    constexpr size_t PENDING_SLOTS = 1 << 20;
    constexpr size_t PENDING_MASK  = PENDING_SLOTS - 1;
    struct PendingSlot {
        uint64_t  intended_ts;
        uint64_t  seq;
        NewOrder* pool_ptr;
    };
    std::vector<PendingSlot> pending(PENDING_SLOTS, {0, 0, nullptr});

    // TX buffer
    constexpr size_t TX_FRAME_SIZE = sizeof(FrameHeader) + sizeof(NewOrder);
    alignas(hardware_destructive_interference_size) uint8_t tx_buffer[TX_FRAME_SIZE];
    auto* tx_hdr     = reinterpret_cast<FrameHeader*>(tx_buffer);
    tx_hdr->msg_type = MSG_NEWORDER;
    tx_hdr->_pad     = 0;
    tx_hdr->msg_len  = sizeof(NewOrder);
    auto* tx_order   = reinterpret_cast<NewOrder*>(tx_buffer + sizeof(FrameHeader));

    // RX buffer
    constexpr size_t RX_BUFFER_SIZE = 65536;
    alignas(hardware_destructive_interference_size) uint8_t rx_buffer[RX_BUFFER_SIZE];
    size_t residual_bytes = 0;

    // Local counters (no atomics, no shared state)
    uint64_t total_sent          = 0;
    uint64_t total_acked         = 0;
    uint64_t total_fills         = 0;
    uint64_t total_rejects       = 0;
    uint64_t send_failures       = 0;
    uint64_t partial_send_aborts = 0;
    uint64_t pending_collisions  = 0;
    uint8_t  next_side           = (config.thread_id % 2 == 0) ? SIDE_BUY : SIDE_SELL;

    const uint64_t start_time = hft::rdtscp_ns();
    uint64_t next_send_time   = start_time;
    const uint64_t end_time   = start_time + config.duration_ns;

    // MAIN LOOP
    while (true) {
        uint64_t now = hft::rdtscp_ns();
        if (__builtin_expect(now >= end_time, 0)) break;

        // SEND PATH — catch-up loop (CO structural fix)
        while (__builtin_expect(now >= next_send_time, 0)) {
            NewOrder* slot = pool.acquire();
            if (__builtin_expect(slot == nullptr, 0)) {
                next_send_time += config.interval_ns;
                continue;
            }

            uint64_t this_seq = total_sent + 1;

            // Realistic order generation
            int64_t  price     = static_cast<int64_t>(99 + (xorshift64(rng_state) % 3));
            uint64_t qty       = 1 + (xorshift64(rng_state) % 5);
            bool     is_market = (xorshift64(rng_state) % 20 == 0);
            uint8_t  side      = next_side;
            next_side = (next_side == SIDE_BUY) ? SIDE_SELL : SIDE_BUY;

            slot->seq          = this_seq;
            slot->timestamp_ns = next_send_time;
            slot->symbol_id    = 1;
            slot->order_type   = is_market ? ORDER_MARKET : ORDER_LIMIT;
            slot->side         = side;
            slot->price        = is_market ? 0 : price;
            slot->quantity     = qty;

            std::memcpy(tx_order, slot, sizeof(NewOrder));

            ssize_t sent = send(sock, tx_buffer, TX_FRAME_SIZE, MSG_DONTWAIT);

            if (__builtin_expect(sent < 0, 0)) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    send_failures++;
                    pool.release(slot);
                    break;
                }
                pool.release(slot);
                break;
            }

            if (__builtin_expect(static_cast<size_t>(sent) != TX_FRAME_SIZE, 0)) {
                std::cerr << "[Bot " << config.thread_id
                          << "] FATAL: partial send (" << sent
                          << "/" << TX_FRAME_SIZE << "). Aborting.\n";
                partial_send_aborts++;
                pool.release(slot);
                goto end_run;
            }

            size_t map_slot = this_seq & PENDING_MASK;
            if (__builtin_expect(pending[map_slot].seq != 0, 0)) {
                uint64_t age = this_seq - pending[map_slot].seq;
                if (age <= PENDING_SLOTS / 2) pending_collisions++;
                if (pending[map_slot].pool_ptr)
                    pool.release(pending[map_slot].pool_ptr);
            }
            pending[map_slot] = {next_send_time, this_seq, slot};
            total_sent = this_seq;
            next_send_time += config.interval_ns;
        }

        // RECEIVE PATH
        ssize_t bytes_read = recv(sock,
                                  rx_buffer + residual_bytes,
                                  RX_BUFFER_SIZE - residual_bytes,
                                  MSG_DONTWAIT);

        if (__builtin_expect(bytes_read > 0, 1)) {
            size_t total_bytes = residual_bytes + static_cast<size_t>(bytes_read);
            size_t offset = 0;

            while (offset + sizeof(FrameHeader) <= total_bytes) {
                auto* rx_hdr = reinterpret_cast<FrameHeader*>(rx_buffer + offset);
                size_t frame_size = sizeof(FrameHeader) + rx_hdr->msg_len;
                if (offset + frame_size > total_bytes) break;

                uint64_t ack_time = hft::rdtscp_ns();

                if (rx_hdr->msg_type == MSG_ORDERACK) {
                    auto* rx_ack = reinterpret_cast<OrderAck*>(
                        rx_buffer + offset + sizeof(FrameHeader));
                    size_t ps = rx_ack->order_seq & PENDING_MASK;
                    PendingSlot& pslot = pending[ps];
                    if (__builtin_expect(pslot.seq == rx_ack->order_seq &&
                                        pslot.intended_ts > 0, 1)) {
                        int64_t latency = static_cast<int64_t>(
                            ack_time - pslot.intended_ts);
                        if (latency > 0) {
                            hdr_record_value(result->naive_hist, latency);
                            hdr_record_corrected_value(result->co_hist,
                                                       latency, config.interval_ns);
                        }
                        if (pslot.pool_ptr) pool.release(pslot.pool_ptr);
                        pslot = {0, 0, nullptr};
                    }
                    total_acked++;
                } else if (rx_hdr->msg_type == MSG_FILL) {
                    auto* rx_fill = reinterpret_cast<Fill*>(
                        rx_buffer + offset + sizeof(FrameHeader));
                    total_fills++;
                    if (rx_fill->leaves_qty == 0) {
                        size_t ps = rx_fill->order_seq & PENDING_MASK;
                        PendingSlot& pslot = pending[ps];
                        if (pslot.seq == rx_fill->order_seq) {
                            if (pslot.pool_ptr) pool.release(pslot.pool_ptr);
                            pslot = {0, 0, nullptr};
                        }
                    }
                } else if (rx_hdr->msg_type == MSG_REJECT) {
                    auto* rx_rej = reinterpret_cast<Reject*>(
                        rx_buffer + offset + sizeof(FrameHeader));
                    total_rejects++;
                    size_t ps = rx_rej->order_seq & PENDING_MASK;
                    PendingSlot& pslot = pending[ps];
                    if (pslot.seq == rx_rej->order_seq) {
                        if (pslot.pool_ptr) pool.release(pslot.pool_ptr);
                        pslot = {0, 0, nullptr};
                    }
                }
                offset += frame_size;
            }

            residual_bytes = total_bytes - offset;
            if (residual_bytes > 0 && offset > 0)
                std::memmove(rx_buffer, rx_buffer + offset, residual_bytes);
        }
    }

end_run:
    // Drain remaining acks (500ms grace)
    {
        auto drain_start = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - drain_start <
               std::chrono::milliseconds(500)) {
            ssize_t bytes_read = recv(sock,
                                      rx_buffer + residual_bytes,
                                      RX_BUFFER_SIZE - residual_bytes,
                                      MSG_DONTWAIT);
            if (bytes_read > 0) {
                size_t total_bytes = residual_bytes + static_cast<size_t>(bytes_read);
                size_t offset = 0;
                while (offset + sizeof(FrameHeader) <= total_bytes) {
                    auto* rx_hdr = reinterpret_cast<FrameHeader*>(rx_buffer + offset);
                    size_t frame_size = sizeof(FrameHeader) + rx_hdr->msg_len;
                    if (offset + frame_size > total_bytes) break;
                    uint64_t ack_time = hft::rdtscp_ns();

                    if (rx_hdr->msg_type == MSG_ORDERACK) {
                        auto* rx_ack = reinterpret_cast<OrderAck*>(
                            rx_buffer + offset + sizeof(FrameHeader));
                        size_t ps = rx_ack->order_seq & PENDING_MASK;
                        PendingSlot& pslot = pending[ps];
                        if (pslot.seq == rx_ack->order_seq && pslot.intended_ts > 0) {
                            int64_t latency = static_cast<int64_t>(
                                ack_time - pslot.intended_ts);
                            if (latency > 0) {
                                hdr_record_value(result->naive_hist, latency);
                                hdr_record_corrected_value(result->co_hist,
                                                           latency, config.interval_ns);
                            }
                            if (pslot.pool_ptr) pool.release(pslot.pool_ptr);
                            pslot = {0, 0, nullptr};
                        }
                        total_acked++;
                    } else if (rx_hdr->msg_type == MSG_FILL) {
                        total_fills++;
                    } else if (rx_hdr->msg_type == MSG_REJECT) {
                        total_rejects++;
                    }
                    offset += frame_size;
                }
                residual_bytes = total_bytes - offset;
                if (residual_bytes > 0 && offset > 0)
                    std::memmove(rx_buffer, rx_buffer + offset, residual_bytes);
            }
        }
    }

    // Copy local counters back into shared result
    result->total_sent          = total_sent;
    result->total_acked         = total_acked;
    result->total_fills         = total_fills;
    result->total_rejects       = total_rejects;
    result->send_failures       = send_failures;
    result->partial_send_aborts = partial_send_aborts;
    result->pending_collisions  = pending_collisions;
    result->pool_exhausted      = pool.exhausted_count;

    close(sock);
}

// ══════════════════════════════════════════════════════════════════════════
// MAIN — parse args, spawn worker threads, merge histograms, print results
// ══════════════════════════════════════════════════════════════════════════
int main(int argc, char* argv[]) {
    uint32_t num_bots        = 4;
    int      start_core      = 1;       // skip core 0 (IRQ-heavy)
    uint64_t interval_us     = 100;
    uint64_t duration_sec    = 10;
    std::string ip_addr      = "127.0.0.1";
    uint16_t port            = 9000;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if      (arg == "--bots"         && i + 1 < argc) num_bots     = std::stoul(argv[++i]);
        else if (arg == "--start-core"   && i + 1 < argc) start_core   = std::stoi(argv[++i]);
        else if (arg == "--interval-us"  && i + 1 < argc) interval_us  = std::stoull(argv[++i]);
        else if (arg == "--duration-sec" && i + 1 < argc) duration_sec = std::stoull(argv[++i]);
        else if (arg == "--ip"           && i + 1 < argc) ip_addr      = argv[++i];
        else if (arg == "--port"         && i + 1 < argc) port         = static_cast<uint16_t>(std::stoi(argv[++i]));
        else if (arg == "--no-pin")                       start_core   = -1;
    }

    const uint64_t interval_ns = interval_us * 1000;
    const uint64_t duration_ns = duration_sec * 1000000000ULL;

    hft::calibrate_tsc();
    std::cout << "[Main] TSC calibrated.\n";
    std::cout << "[Main] Launching " << num_bots << " bots, "
              << "interval " << interval_us << "us, "
              << "duration " << duration_sec << "s, "
              << "target " << ip_addr << ":" << port << "\n";
    if (start_core >= 0)
        std::cout << "[Main] CPU pinning: cores " << start_core
                  << " through " << (start_core + num_bots - 1) << "\n";
    else
        std::cout << "[Main] CPU pinning DISABLED\n";

    // Spawn worker threads
    std::vector<BotConfig> configs(num_bots);
    std::vector<BotResult> results(num_bots);
    std::vector<std::thread> threads;
    threads.reserve(num_bots);

    for (uint32_t i = 0; i < num_bots; i++) {
        configs[i] = BotConfig{
            .thread_id   = i,
            .cpu_core    = (start_core >= 0) ? (start_core + static_cast<int>(i)) : -1,
            .ip_addr     = ip_addr,
            .port        = port,
            .interval_ns = interval_ns,
            .duration_ns = duration_ns,
        };
        threads.emplace_back(bot_worker, configs[i], &results[i]);
    }

    // Wait for all to finish
    for (auto& t : threads) t.join();

    // Merge per-thread histograms into unified aggregate
    struct hdr_histogram* unified_naive = nullptr;
    struct hdr_histogram* unified_co    = nullptr;
    hdr_init(1, 30000000000LL, 3, &unified_naive);
    hdr_init(1, 30000000000LL, 3, &unified_co);

    uint64_t agg_sent         = 0;
    uint64_t agg_acked        = 0;
    uint64_t agg_fills        = 0;
    uint64_t agg_rejects      = 0;
    uint64_t agg_send_fail    = 0;
    uint64_t agg_partial      = 0;
    uint64_t agg_collisions   = 0;
    uint64_t agg_pool_exhaust = 0;
    uint32_t connected_count  = 0;

    for (uint32_t i = 0; i < num_bots; i++) {
        if (!results[i].connected) continue;
        connected_count++;
        if (results[i].naive_hist) hdr_add(unified_naive, results[i].naive_hist);
        if (results[i].co_hist)    hdr_add(unified_co,    results[i].co_hist);
        agg_sent         += results[i].total_sent;
        agg_acked        += results[i].total_acked;
        agg_fills        += results[i].total_fills;
        agg_rejects      += results[i].total_rejects;
        agg_send_fail    += results[i].send_failures;
        agg_partial      += results[i].partial_send_aborts;
        agg_collisions   += results[i].pending_collisions;
        agg_pool_exhaust += results[i].pool_exhausted;
    }

    // Final report
    std::cout << "\n[Main] All bots finished. Connected: "
              << connected_count << "/" << num_bots << "\n";
    std::cout << "[Main] Aggregate: Sent=" << agg_sent
              << " Acked=" << agg_acked
              << " Fills=" << agg_fills
              << " Rejects=" << agg_rejects
              << " EAGAIN=" << agg_send_fail
              << " PartialAborts=" << agg_partial
              << " Collisions=" << agg_collisions
              << " PoolExhausted=" << agg_pool_exhaust << "\n\n";

    std::cout << "=================================================================\n";
    std::cout << " AGGREGATE ROUND-TRIP LATENCY — " << num_bots
              << " bots — Naive vs CO-Corrected (ns)\n";
    std::cout << "=================================================================\n";
    std::cout << std::left  << std::setw(12) << "Percentile"
              << std::right << std::setw(18) << "Naive"
              << std::setw(18) << "CO-Corrected"
              << std::setw(12) << "Ratio" << "\n";
    std::cout << "-----------------------------------------------------------------\n";

    double percentiles[] = {50.0, 90.0, 99.0, 99.9, 99.99, 100.0};
    const char* names[]  = {"p50", "p90", "p99", "p99.9", "p99.99", "Max"};

    for (int i = 0; i < 6; i++) {
        int64_t n_val = hdr_value_at_percentile(unified_naive, percentiles[i]);
        int64_t c_val = hdr_value_at_percentile(unified_co,    percentiles[i]);
        double ratio  = (n_val > 0)
            ? static_cast<double>(c_val) / static_cast<double>(n_val) : 0.0;
        std::cout << std::left  << std::setw(12) << names[i]
                  << std::right << std::setw(18) << n_val
                  << std::setw(18) << c_val
                  << std::setw(11) << std::fixed << std::setprecision(1)
                  << ratio << "x\n";
    }
    std::cout << "=================================================================\n";
    std::cout << "\nNaive sample count:  " << unified_naive->total_count << "\n";
    std::cout << "CO-corrected count:  " << unified_co->total_count
              << "  (backfilled "
              << (unified_co->total_count - unified_naive->total_count)
              << " phantom samples)\n";

    // Cleanup
    for (uint32_t i = 0; i < num_bots; i++) {
        if (results[i].naive_hist) hdr_close(results[i].naive_hist);
        if (results[i].co_hist)    hdr_close(results[i].co_hist);
    }
    hdr_close(unified_naive);
    hdr_close(unified_co);

    return 0;
}
