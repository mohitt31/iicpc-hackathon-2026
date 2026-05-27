// bot.cpp — Open-loop multi-threaded latency benchmarking client
// Demonstrates Coordinated Omission detection and correction at scale.
//
// Architecture:
//   - Main thread (cold): args, thread launch, snapshot lifecycle, results
//   - N worker threads (hot, CPU-pinned): independent send/recv loops
//   - Per-bot snapshot writer (cold side): every 1s, dumps HDR percentiles
//     to a CSV file for time-series analysis and live leaderboard feed
//
// HFT-grade primitives:
//   - _mm_pause() inside busy-spin catch-up loop (CPU pipeline-friendly)
//   - SO_BUSY_POLL on Linux (kernel polls NIC for sub-IRQ latency)
//   - alignas(hardware_destructive_interference_size) on hot buffers
//   - Per-thread state, zero shared writes on hot path
//
// Snapshot lifecycle (deadlock-free):
//   main spawns snapshot threads -> main spawns workers -> workers run ->
//   workers join -> main sets stop_flag -> snapshot threads exit -> main joins
//   snapshot threads. This ordering is critical — see the comment at the
//   bottom of main().

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
#include <sys/stat.h>
#include <string>
#include <stdexcept>
#include <vector>
#include <chrono>
#include <thread>
#include <pthread.h>
#include <sched.h>
#include <fstream>
#include <atomic>

#include "contracts/interface_contract_v1.h"
#include "tsc_util.h"
#include <hdr/hdr_histogram.h>

// ── _mm_pause() — CPU pipeline hint inside busy-spin loops ───────────────
// On x86 this emits the PAUSE instruction (~30 cycle hint). It tells the
// CPU "I'm spinning; deprioritize this so other SMT siblings can use the
// pipeline, and don't penalize me with a memory-order-violation flush when
// the spin breaks." On non-x86 (e.g. macOS arm64) we fall through cleanly.
#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#define HFT_PAUSE() _mm_pause()
#elif defined(__aarch64__)
// ARM equivalent: 'yield' instruction
#define HFT_PAUSE() asm volatile("yield" ::: "memory")
#else
#define HFT_PAUSE() do {} while (0)
#endif

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
    std::string snapshot_dir;   // empty = snapshots disabled
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

static bool pin_to_core(int core_id) {
    if (core_id < 0) return false;
#if defined(__linux__)
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    return rc == 0;
#else
    (void)core_id;
    return false;
#endif
}

// ══════════════════════════════════════════════════════════════════════════
// SNAPSHOT WRITER — runs in cold thread per bot, owned by MAIN (not worker).
// Every 1 second, samples the hot thread's HDR histograms and appends a
// CSV row with p50/p90/p99/p99.9/p99.99/max + throughput counters.
//
// Lifecycle (deadlock-free):
//   main creates snapshot threads BEFORE workers.
//   workers run, write to histograms, finish, get joined by main.
//   main then sets stop_flag.
//   snapshot threads observe stop_flag, write their final row, exit.
//   main joins snapshot threads.
//
// IMPORTANT: hdr_value_at_percentile() does NOT modify the histogram.
// HDR's count writes are 64-bit aligned; torn reads bounded to one sample,
// statistically negligible at 1Hz snapshot rate.
// ══════════════════════════════════════════════════════════════════════════
static void snapshot_writer(uint32_t thread_id,
                            std::string snapshot_dir,
                            BotResult* result,
                            const std::atomic<bool>* stop_flag) {
    if (snapshot_dir.empty()) return;

    std::string path = snapshot_dir + "/bot_" + std::to_string(thread_id) + ".csv";
    std::ofstream csv(path);
    if (!csv) {
        std::cerr << "[Bot " << thread_id << "] snapshot: cannot open "
                  << path << "\n";
        return;
    }

    csv << "elapsed_sec,sent,acked,"
        << "naive_p50,naive_p90,naive_p99,naive_p99_9,naive_p99_99,naive_max,"
        << "co_p50,co_p90,co_p99,co_p99_9,co_p99_99,co_max\n";
    csv.flush();

    auto t_start = std::chrono::steady_clock::now();

    auto write_row = [&](int64_t elapsed) {
        struct hdr_histogram* nh = result->naive_hist;
        struct hdr_histogram* ch = result->co_hist;
        if (!nh || !ch) return;

        csv << elapsed << ","
            << result->total_sent << ","
            << result->total_acked << ","
            << hdr_value_at_percentile(nh, 50.0)    << ","
            << hdr_value_at_percentile(nh, 90.0)    << ","
            << hdr_value_at_percentile(nh, 99.0)    << ","
            << hdr_value_at_percentile(nh, 99.9)    << ","
            << hdr_value_at_percentile(nh, 99.99)   << ","
            << hdr_max(nh)                           << ","
            << hdr_value_at_percentile(ch, 50.0)    << ","
            << hdr_value_at_percentile(ch, 90.0)    << ","
            << hdr_value_at_percentile(ch, 99.0)    << ","
            << hdr_value_at_percentile(ch, 99.9)    << ","
            << hdr_value_at_percentile(ch, 99.99)   << ","
            << hdr_max(ch) << "\n";
        csv.flush();
    };

    while (!stop_flag->load(std::memory_order_acquire)) {
        // Sleep in small slices so stop_flag is checked frequently —
        // worker finish → stop flag flip → exit within ~100ms.
        for (int i = 0; i < 10; i++) {
            if (stop_flag->load(std::memory_order_acquire)) goto final_write;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - t_start).count();
        write_row(elapsed);
    }

final_write:
    // Write one final snapshot capturing end-of-run state
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - t_start).count();
    write_row(elapsed);

    csv.close();
}

// ══════════════════════════════════════════════════════════════════════════
// WORKER THREAD — one per bot. CPU-pinned, independent send/recv loop.
// Does NOT own its snapshot thread — that's main's job (see top comment).
// ══════════════════════════════════════════════════════════════════════════
static void bot_worker(BotConfig config, BotResult* result) {
    if (config.cpu_core >= 0) {
        if (pin_to_core(config.cpu_core)) {
            std::cout << "[Bot " << config.thread_id << "] Pinned to core "
                      << config.cpu_core << "\n";
        } else {
            std::cerr << "[Bot " << config.thread_id
                      << "] WARNING: could not pin to core "
                      << config.cpu_core << "\n";
        }
    }

    hdr_init(1, 30000000000LL, 3, &result->naive_hist);
    hdr_init(1, 30000000000LL, 3, &result->co_hist);

    OrderPool pool;
    uint64_t rng_state = 42 + config.thread_id;

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

    // ── SO_BUSY_POLL — kernel polls NIC ring instead of waiting for IRQ ──
    // On Linux only. Asks the kernel to spin briefly checking the NIC RX
    // queue when the socket has no data ready, bypassing interrupt-driven
    // wakeup latency (5-20μs IRQ handling overhead). Documented by Linux
    // network stack as the right primitive for microsecond-grade workloads
    // that already busy-spin in user space.
    //
    // Value is the poll budget in microseconds — 50μs is a typical HFT
    // setting: long enough to capture most ack arrivals, short enough that
    // a stall doesn't pin the kernel indefinitely. Requires CAP_NET_ADMIN
    // or sysctl net.core.busy_poll set system-wide; if blocked, setsockopt
    // returns EPERM and we just continue without it (graceful fallback).
#ifdef __linux__
#ifdef SO_BUSY_POLL
    int busy_poll_us = 50;
    if (setsockopt(sock, SOL_SOCKET, SO_BUSY_POLL,
                   &busy_poll_us, sizeof(busy_poll_us)) == 0) {
        // Successfully enabled — log once per bot for visibility
        if (config.thread_id == 0)
            std::cout << "[Bot 0] SO_BUSY_POLL enabled (" << busy_poll_us << "us)\n";
    }
    // EPERM is expected without CAP_NET_ADMIN; we proceed regardless
#endif
#endif

    set_non_blocking(sock);

    constexpr size_t PENDING_SLOTS = 1 << 20;
    constexpr size_t PENDING_MASK  = PENDING_SLOTS - 1;
    struct PendingSlot {
        uint64_t  intended_ts;
        uint64_t  seq;
        NewOrder* pool_ptr;
    };
    std::vector<PendingSlot> pending(PENDING_SLOTS, {0, 0, nullptr});

    constexpr size_t TX_FRAME_SIZE = sizeof(FrameHeader) + sizeof(NewOrder);
    alignas(hardware_destructive_interference_size) uint8_t tx_buffer[TX_FRAME_SIZE];
    auto* tx_hdr     = reinterpret_cast<FrameHeader*>(tx_buffer);
    tx_hdr->msg_type = MSG_NEWORDER;
    tx_hdr->_pad     = 0;
    tx_hdr->msg_len  = sizeof(NewOrder);
    auto* tx_order   = reinterpret_cast<NewOrder*>(tx_buffer + sizeof(FrameHeader));

    constexpr size_t RX_BUFFER_SIZE = 65536;
    alignas(hardware_destructive_interference_size) uint8_t rx_buffer[RX_BUFFER_SIZE];
    size_t residual_bytes = 0;

    // Counters live in result struct so snapshot thread can read them
    uint64_t& total_sent          = result->total_sent;
    uint64_t& total_acked         = result->total_acked;
    uint64_t& total_fills         = result->total_fills;
    uint64_t& total_rejects       = result->total_rejects;
    uint64_t  send_failures       = 0;
    uint64_t  partial_send_aborts = 0;
    uint64_t  pending_collisions  = 0;
    uint8_t   next_side           = (config.thread_id % 2 == 0) ? SIDE_BUY : SIDE_SELL;

    const uint64_t start_time = hft::rdtscp_ns();
    uint64_t next_send_time   = start_time;
    const uint64_t end_time   = start_time + config.duration_ns;

    while (true) {
        uint64_t now = hft::rdtscp_ns();
        if (__builtin_expect(now >= end_time, 0)) break;

        // ── SEND PATH (catch-up loop) ─────────────────────────────────────
        // The CO structural fix: fire all "overdue" intervals in a burst,
        // each tagged with its INTENDED time, never wall-clock-of-actual-send.
        while (__builtin_expect(now >= next_send_time, 0)) {
            // CPU pause hint at the top of the spin — lets the pipeline
            // breathe; on AMD/Intel SMT it gives the sibling thread fair
            // share when we're catching up. Free on the hot path; the cost
            // of the pause is recovered many times over by not flushing the
            // pipeline when the catch-up condition becomes false.
            HFT_PAUSE();

            NewOrder* slot = pool.acquire();
            if (__builtin_expect(slot == nullptr, 0)) {
                next_send_time += config.interval_ns;
                continue;
            }

            uint64_t this_seq = total_sent + 1;

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

        // ── RECEIVE PATH ──────────────────────────────────────────────────
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
    // ── Drain remaining acks (500ms grace) ────────────────────────────────
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

    result->send_failures       = send_failures;
    result->partial_send_aborts = partial_send_aborts;
    result->pending_collisions  = pending_collisions;
    result->pool_exhausted      = pool.exhausted_count;

    close(sock);

    // NOTE: snapshot thread is NOT owned by the worker. Main thread
    // manages its lifecycle, preventing the previous deadlock where
    // worker waited on snapshot, snapshot waited on stop_flag, and
    // main waited on worker.
}

// ══════════════════════════════════════════════════════════════════════════
// MAIN
// ══════════════════════════════════════════════════════════════════════════
int main(int argc, char* argv[]) {
    uint32_t num_bots        = 4;
    int      start_core      = 1;
    uint64_t interval_us     = 100;
    uint64_t duration_sec    = 10;
    std::string ip_addr      = "127.0.0.1";
    uint16_t port            = 9000;
    std::string snapshot_dir = "";

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if      (arg == "--bots"         && i + 1 < argc) num_bots     = std::stoul(argv[++i]);
        else if (arg == "--start-core"   && i + 1 < argc) start_core   = std::stoi(argv[++i]);
        else if (arg == "--interval-us"  && i + 1 < argc) interval_us  = std::stoull(argv[++i]);
        else if (arg == "--duration-sec" && i + 1 < argc) duration_sec = std::stoull(argv[++i]);
        else if (arg == "--ip"           && i + 1 < argc) ip_addr      = argv[++i];
        else if (arg == "--port"         && i + 1 < argc) port         = static_cast<uint16_t>(std::stoi(argv[++i]));
        else if (arg == "--no-pin")                       start_core   = -1;
        else if (arg == "--snapshot-dir" && i + 1 < argc) snapshot_dir = argv[++i];
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

    if (!snapshot_dir.empty()) {
        mkdir(snapshot_dir.c_str(), 0755);
        std::cout << "[Main] Snapshot dir: " << snapshot_dir
                  << " (writing bot_<id>.csv every 1s)\n";
    }

    std::vector<BotConfig> configs(num_bots);
    std::vector<BotResult> results(num_bots);

    // ── Lifecycle ordering (the deadlock fix) ────────────────────────────
    // 1. Initialize histograms BEFORE snapshot threads can read them.
    //    We do this lazily in the worker — but snapshot polls until
    //    histogram pointers are non-null, so safe.
    // 2. Start snapshot threads first (main owns them).
    // 3. Start worker threads.
    // 4. Join workers (they finish).
    // 5. Set stop_flag — snapshots see it, write final row, exit.
    // 6. Join snapshot threads.

    std::atomic<bool> snapshot_stop{false};
    std::vector<std::thread> snap_threads;
    std::vector<std::thread> work_threads;
    snap_threads.reserve(num_bots);
    work_threads.reserve(num_bots);

    for (uint32_t i = 0; i < num_bots; i++) {
        configs[i] = BotConfig{
            .thread_id    = i,
            .cpu_core     = (start_core >= 0) ? (start_core + static_cast<int>(i)) : -1,
            .ip_addr      = ip_addr,
            .port         = port,
            .interval_ns  = interval_ns,
            .duration_ns  = duration_ns,
            .snapshot_dir = snapshot_dir,
        };

        // Snapshot thread first (cold) — it polls until histograms init
        if (!snapshot_dir.empty()) {
            snap_threads.emplace_back(snapshot_writer,
                                      i, snapshot_dir,
                                      &results[i], &snapshot_stop);
        }
    }

    // Then workers (hot)
    for (uint32_t i = 0; i < num_bots; i++) {
        work_threads.emplace_back(bot_worker, configs[i], &results[i]);
    }

    // Wait for all workers to finish
    for (auto& t : work_threads) t.join();

    // Signal snapshot threads to write final row and exit
    snapshot_stop.store(true, std::memory_order_release);

    // Wait for snapshot threads to finish their final write
    for (auto& t : snap_threads) {
        if (t.joinable()) t.join();
    }

    // ── Merge per-thread histograms into unified aggregate ────────────────
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

    if (!snapshot_dir.empty()) {
        std::cout << "\n[Main] Per-second snapshots written to "
                  << snapshot_dir << "/bot_*.csv\n";
    }

    for (uint32_t i = 0; i < num_bots; i++) {
        if (results[i].naive_hist) hdr_close(results[i].naive_hist);
        if (results[i].co_hist)    hdr_close(results[i].co_hist);
    }
    hdr_close(unified_naive);
    hdr_close(unified_co);

    return 0;
}
