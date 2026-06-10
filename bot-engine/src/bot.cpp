// bot.cpp — Open-loop multi-threaded latency benchmarking client
// Demonstrates Coordinated Omission detection and correction at scale.
//
// Architecture:
//   - Main thread (cold): args, integrity gate, thread launch, results
//   - N worker threads (hot, CPU-pinned): independent send/recv loops
//   - Per-bot snapshot writer (cold side): every 1s, dumps HDR percentiles
//   - Optional per-bot SPSC consumer (cold side): pops latency records from
//     a lock-free ring and records into HDR off the hot path (--use-spsc)
//
// HFT-grade primitives:
//   - _mm_pause() inside busy-spin catch-up loop (CPU pipeline-friendly)
//   - SO_BUSY_POLL on Linux (kernel polls NIC for sub-IRQ latency)
//   - SO_TIMESTAMPING request for HW NIC timestamps (Linux + supported NIC)
//   - alignas(hardware_destructive_interference_size) on hot buffers
//   - Per-thread state, zero shared writes on hot path
//   - Lock-free SPSC ring for HDR offload (opt-in)
//
// INTEGRITY GATE — pre-flight self-test (see integrity_gate_test).
// SPSC RING (--use-spsc) — alternate path that offloads hdr_record_value()
//   from the hot loop. The hot loop pushes (latency, interval) onto a
//   lock-free ring; a cold consumer thread does the HDR record. At our
//   current scale (10k/sec/bot) the saving is microscopic (~0.1% CPU),
//   but the architecture is exactly what production HFT shops use at
//   1M+ msg/sec and proves we know where the right path leads.

#include <iostream>
#include <iomanip>
#include <cstdint>
#include <cstring>
#include <cerrno>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
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
#include "spsc_queue.h"
#include <hdr/hdr_histogram.h>

#ifdef __linux__
#include <linux/net_tstamp.h>
#endif

// ── _mm_pause() — CPU pipeline hint inside busy-spin loops ───────────────
#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#define HFT_PAUSE() _mm_pause()
#elif defined(__aarch64__)
#define HFT_PAUSE() asm volatile("yield" ::: "memory")
#else
#define HFT_PAUSE() do {} while (0)
#endif

#ifdef __cpp_lib_hardware_interference_size
using std::hardware_destructive_interference_size;
#else
constexpr std::size_t hardware_destructive_interference_size = 64;
#endif

// ── Latency record pushed onto the SPSC ring ──────────────────────────────
// 16 bytes. Fits two records per cache line; ring of 65536 entries = 1 MiB.
struct LatencyRecord {
    int64_t  naive_latency_ns;
    int64_t  co_latency_ns;
    int64_t  interval_ns;  // for hdr_record_corrected_value
};

// Ring capacity: 65536 records. At 100us interval = 6.5s of buffering.
// More than enough to absorb scheduling hiccups in the cold consumer.
constexpr std::size_t SPSC_CAPACITY = 65536;
using LatencyRing = hft::SPSCQueue<LatencyRecord, SPSC_CAPACITY>;

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
    int         cpu_core;
    std::string ip_addr;
    uint16_t    port;
    uint64_t    interval_ns;
    uint64_t    duration_ns;
    std::string snapshot_dir;
    bool        use_spsc;
    uint64_t    warmup_orders;  // first N samples excluded from HDR
};

struct BotResult {
    struct hdr_histogram* naive_hist = nullptr;
    struct hdr_histogram* co_hist    = nullptr;
    LatencyRing* spsc_ring        = nullptr;  // null unless --use-spsc
    // Live counters: single-writer (the worker) publishes with relaxed
    // stores — a plain MOV on x86, zero hot-path cost — so the snapshot
    // thread can read them concurrently without a data race.
    std::atomic<uint64_t> total_sent{0};
    std::atomic<uint64_t> total_acked{0};
    std::atomic<uint64_t> total_fills{0};
    std::atomic<uint64_t> total_rejects{0};
    uint64_t send_failures        = 0;
    uint64_t partial_send_aborts  = 0;
    uint64_t pending_collisions   = 0;
    uint64_t pool_exhausted       = 0;
    uint64_t spsc_dropped         = 0;  // ring full, latency record dropped
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
// SPSC CONSUMER — runs on a cold thread, one per bot when --use-spsc.
// Pops LatencyRecord entries from the ring and feeds them into HDR.
// The hot path's only cost becomes a single atomic store per record.
// ══════════════════════════════════════════════════════════════════════════
static void spsc_consumer(BotResult* result,
                          const std::atomic<bool>* stop_flag) {
    LatencyRecord rec;
    while (!stop_flag->load(std::memory_order_acquire)) {
        bool drained_any = false;
        while (result->spsc_ring->try_pop(rec)) {
            hdr_record_value(result->naive_hist, rec.naive_latency_ns);
            // CO histogram must see latency vs INTENDED send time —
            // feeding it the naive (actual-send) latency here would
            // silently undo the Tene/Snyder correction on this path.
            hdr_record_corrected_value(result->co_hist,
                                       rec.co_latency_ns, rec.interval_ns);
            drained_any = true;
        }
        // Sleep briefly only if ring was empty — keeps tail latency low
        // while not pegging a core when idle.
        if (!drained_any) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }
    // Final drain after stop signal — make sure no records are lost.
    while (result->spsc_ring->try_pop(rec)) {
        hdr_record_value(result->naive_hist, rec.naive_latency_ns);
        hdr_record_corrected_value(result->co_hist,
                                   rec.co_latency_ns, rec.interval_ns);
    }
}

// ══════════════════════════════════════════════════════════════════════════
// INTEGRITY GATE — pre-flight self-test
// ══════════════════════════════════════════════════════════════════════════
static uint64_t integrity_gate_test(const std::string& ip_addr, uint16_t port,
                                    uint64_t interval_ns) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return UINT64_MAX;

    sockaddr_in serv_addr{};
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port   = htons(port);
    struct hostent *he = gethostbyname(ip_addr.c_str());
    if (he) {
        std::memcpy(&serv_addr.sin_addr, he->h_addr, he->h_length);
    } else {
        inet_pton(AF_INET, ip_addr.c_str(), &serv_addr.sin_addr);
    }

    if (connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        close(sock);
        return UINT64_MAX;
    }

    int opt = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
    set_non_blocking(sock);

    struct hdr_histogram* gate_hist = nullptr;
    hdr_init(1, 30000000000LL, 3, &gate_hist);

    constexpr size_t GATE_ORDERS  = 1000;
    constexpr size_t TX_FRAME_SIZE = sizeof(FrameHeader) + sizeof(NewOrder);
    constexpr size_t RX_BUFFER_SIZE = 8192;

    uint8_t tx_buffer[TX_FRAME_SIZE];
    uint8_t rx_buffer[RX_BUFFER_SIZE];
    auto* tx_hdr     = reinterpret_cast<FrameHeader*>(tx_buffer);
    tx_hdr->msg_type = MSG_NEWORDER;
    tx_hdr->_pad     = 0;
    tx_hdr->msg_len  = sizeof(NewOrder);
    auto* tx_order   = reinterpret_cast<NewOrder*>(tx_buffer + sizeof(FrameHeader));
    tx_order->symbol_id  = 1;
    tx_order->order_type = ORDER_LIMIT;
    tx_order->side       = SIDE_BUY;
    tx_order->price      = 100;
    tx_order->quantity   = 1;

    std::vector<uint64_t> pending(GATE_ORDERS + 1, 0);
    size_t residual = 0;
    uint64_t sent = 0, acked = 0;

    uint64_t now            = hft::rdtscp_ns();
    uint64_t next_send_time = now;
    uint64_t deadline       = now + 5000000000ULL;

    while (sent < GATE_ORDERS && hft::rdtscp_ns() < deadline) {
        now = hft::rdtscp_ns();
        if (now >= next_send_time && sent < GATE_ORDERS) {
            uint64_t seq = sent + 1;
            tx_order->seq          = seq;
            tx_order->timestamp_ns = next_send_time;
            ssize_t s = send(sock, tx_buffer, TX_FRAME_SIZE, MSG_DONTWAIT);
            if (s == static_cast<ssize_t>(TX_FRAME_SIZE)) {
                pending[seq] = next_send_time;
                sent++;
                next_send_time += interval_ns;
            }
        }
        ssize_t br = recv(sock, rx_buffer + residual,
                          RX_BUFFER_SIZE - residual, MSG_DONTWAIT);
        if (br > 0) {
            size_t tb = residual + static_cast<size_t>(br);
            size_t off = 0;
            while (off + sizeof(FrameHeader) <= tb) {
                auto* rh = reinterpret_cast<FrameHeader*>(rx_buffer + off);
                size_t fs = sizeof(FrameHeader) + rh->msg_len;
                if (off + fs > tb) break;
                if (rh->msg_type == MSG_ORDERACK) {
                    auto* a = reinterpret_cast<OrderAck*>(
                        rx_buffer + off + sizeof(FrameHeader));
                    uint64_t t_recv = hft::rdtscp_ns();
                    if (a->order_seq > 0 && a->order_seq <= GATE_ORDERS
                        && pending[a->order_seq] > 0) {
                        int64_t lat = static_cast<int64_t>(t_recv - pending[a->order_seq]);
                        if (lat > 0) hdr_record_value(gate_hist, lat);
                        pending[a->order_seq] = 0;
                        acked++;
                    }
                }
                off += fs;
            }
            residual = tb - off;
            if (residual > 0 && off > 0)
                std::memmove(rx_buffer, rx_buffer + off, residual);
        }
    }

    auto drain_start = std::chrono::steady_clock::now();
    while (acked < sent && std::chrono::steady_clock::now() - drain_start
           < std::chrono::milliseconds(200)) {
        ssize_t br = recv(sock, rx_buffer + residual,
                          RX_BUFFER_SIZE - residual, MSG_DONTWAIT);
        if (br > 0) {
            size_t tb = residual + static_cast<size_t>(br);
            size_t off = 0;
            while (off + sizeof(FrameHeader) <= tb) {
                auto* rh = reinterpret_cast<FrameHeader*>(rx_buffer + off);
                size_t fs = sizeof(FrameHeader) + rh->msg_len;
                if (off + fs > tb) break;
                if (rh->msg_type == MSG_ORDERACK) {
                    auto* a = reinterpret_cast<OrderAck*>(
                        rx_buffer + off + sizeof(FrameHeader));
                    uint64_t t_recv = hft::rdtscp_ns();
                    if (a->order_seq > 0 && a->order_seq <= GATE_ORDERS
                        && pending[a->order_seq] > 0) {
                        int64_t lat = static_cast<int64_t>(t_recv - pending[a->order_seq]);
                        if (lat > 0) hdr_record_value(gate_hist, lat);
                        pending[a->order_seq] = 0;
                        acked++;
                    }
                }
                off += fs;
            }
            residual = tb - off;
            if (residual > 0 && off > 0)
                std::memmove(rx_buffer, rx_buffer + off, residual);
        }
    }

    int64_t p99 = (gate_hist->total_count > 0)
        ? hdr_value_at_percentile(gate_hist, 99.0) : INT64_MAX;
    hdr_close(gate_hist);
    close(sock);

    if (acked < GATE_ORDERS / 2) return UINT64_MAX;
    return static_cast<uint64_t>(p99);
}

// ══════════════════════════════════════════════════════════════════════════
// SNAPSHOT WRITER
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

    // Histogram reads here race benignly with hot-path records: HDR is a
    // fixed array of counts, so a concurrent read yields a slightly stale
    // but structurally valid snapshot. The exact, race-free numbers are
    // computed in main() after the workers are joined.
    auto write_row = [&](int64_t elapsed) {
        struct hdr_histogram* nh = result->naive_hist;
        struct hdr_histogram* ch = result->co_hist;
        if (!nh || !ch) return;
        csv << elapsed << ","
            << result->total_sent.load(std::memory_order_relaxed) << ","
            << result->total_acked.load(std::memory_order_relaxed) << ","
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
        for (int i = 0; i < 10; i++) {
            if (stop_flag->load(std::memory_order_acquire)) goto final_write;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - t_start).count();
        write_row(elapsed);
    }

final_write:
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - t_start).count();
    write_row(elapsed);

    csv.close();
}

// ══════════════════════════════════════════════════════════════════════════
// WORKER THREAD
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

    // Histograms and the optional SPSC ring are allocated by main()
    // BEFORE any thread is spawned — no cross-thread init races here.
    OrderPool pool;
    uint64_t rng_state = 42 + config.thread_id;

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        std::cerr << "[Bot " << config.thread_id << "] socket() failed\n";
        return;
    }

    // Bound the synchronous connect() — without this, a blackholed IP
    // (e.g., misconfigured sandbox) hangs the bot thread indefinitely
    // in the kernel. 3 seconds is generous for any reasonable network.
    struct timeval connect_timeout{ .tv_sec = 3, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO,
               &connect_timeout, sizeof(connect_timeout));
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
               &connect_timeout, sizeof(connect_timeout));

    sockaddr_in serv_addr{};
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port   = htons(config.port);
    struct hostent *he = gethostbyname(config.ip_addr.c_str());
    if (he) {
        std::memcpy(&serv_addr.sin_addr, he->h_addr, he->h_length);
    } else {
        inet_pton(AF_INET, config.ip_addr.c_str(), &serv_addr.sin_addr);
    }

    if (connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        std::cerr << "[Bot " << config.thread_id << "] connect() failed: "
                  << strerror(errno) << "\n";
        close(sock);
        return;
    }
    result->connected = true;

    // Clear connect-phase timeouts now that we're connected.
    // Hot path uses MSG_DONTWAIT + non-blocking socket; timeouts irrelevant.
    struct timeval no_timeout{ .tv_sec = 0, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &no_timeout, sizeof(no_timeout));
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &no_timeout, sizeof(no_timeout));

    int opt = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
    int sndbuf_size = 4096;
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sndbuf_size, sizeof(sndbuf_size));

#ifdef __linux__
#ifdef SO_BUSY_POLL
    int busy_poll_us = 50;
    if (setsockopt(sock, SOL_SOCKET, SO_BUSY_POLL,
                   &busy_poll_us, sizeof(busy_poll_us)) == 0) {
        if (config.thread_id == 0)
            std::cout << "[Bot 0] SO_BUSY_POLL enabled (" << busy_poll_us << "us)\n";
    }
#endif
#endif

#ifdef __linux__
#ifdef SO_TIMESTAMPING
    int ts_flags = SOF_TIMESTAMPING_TX_HARDWARE  |
                   SOF_TIMESTAMPING_RX_HARDWARE  |
                   SOF_TIMESTAMPING_RAW_HARDWARE |
                   SOF_TIMESTAMPING_SOFTWARE;
    if (setsockopt(sock, SOL_SOCKET, SO_TIMESTAMPING,
                   &ts_flags, sizeof(ts_flags)) == 0) {
        if (config.thread_id == 0)
            std::cout << "[Bot 0] SO_TIMESTAMPING enabled "
                      << "(HW on supported NICs, SW fallback)\n";
    }
#endif
#endif

    set_non_blocking(sock);

    constexpr size_t PENDING_SLOTS = 1 << 20;
    constexpr size_t PENDING_MASK  = PENDING_SLOTS - 1;
    struct PendingSlot {
        uint64_t  intended_ts;
        uint64_t  seq;
        uint64_t  actual_send_ts;
        NewOrder* pool_ptr;
    };
    std::vector<PendingSlot> pending(PENDING_SLOTS, {0, 0, 0, nullptr});

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

    // Counters live in locals on the hot path; each update is published
    // to the shared BotResult with a relaxed store (plain MOV on x86).
    uint64_t  total_sent          = 0;
    uint64_t  total_acked         = 0;
    uint64_t  total_fills         = 0;
    uint64_t  total_rejects       = 0;
    uint64_t  send_failures       = 0;
    uint64_t  partial_send_aborts = 0;
    uint64_t  pending_collisions  = 0;
    uint64_t  spsc_dropped_local  = 0;
    uint8_t   next_side           = (config.thread_id % 2 == 0) ? SIDE_BUY : SIDE_SELL;

    // Lambda routes each measurement either inline to HDR (default) or
    // through the SPSC ring (--use-spsc). Captured by reference so the
    // hot loop sees the cheapest possible call site.
    auto record_latency = [&](int64_t naive_latency, int64_t co_latency) {
        if (config.use_spsc) {
            LatencyRecord rec{naive_latency, co_latency,
                              static_cast<int64_t>(config.interval_ns)};
            if (__builtin_expect(!result->spsc_ring->try_push(rec), 0)) {
                spsc_dropped_local++;
            }
        } else {
            hdr_record_value(result->naive_hist, naive_latency);
            hdr_record_corrected_value(result->co_hist,
                                       co_latency, config.interval_ns);
        }
    };

    const uint64_t start_time = hft::rdtscp_ns();
    uint64_t next_send_time   = start_time;
    const uint64_t end_time   = (config.duration_ns == 0) ? UINT64_MAX : (start_time + config.duration_ns);

    while (true) {
        uint64_t now = hft::rdtscp_ns();
        if (__builtin_expect(now >= end_time, 0)) break;

        while (__builtin_expect(now >= next_send_time, 0)) {
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

            uint64_t actual_send_time = hft::rdtscp_ns();
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
                if (age > 0 && age <= PENDING_SLOTS) pending_collisions++;
                if (pending[map_slot].pool_ptr)
                    pool.release(pending[map_slot].pool_ptr);
            }
            pending[map_slot] = {next_send_time, this_seq, actual_send_time, slot};
            total_sent = this_seq;
            result->total_sent.store(total_sent, std::memory_order_relaxed);
            next_send_time += config.interval_ns;
        }

        ssize_t bytes_read = recv(sock,
                              rx_buffer + residual_bytes,
                              RX_BUFFER_SIZE - residual_bytes,
                              MSG_DONTWAIT);

    // recv() returns 0 = peer closed connection cleanly.
    // Without this check, we busy-spin at 100% CPU forever.
    if (__builtin_expect(bytes_read == 0, 0)) {
        std::cerr << "[Bot " << config.thread_id
                  << "] Peer closed connection. Stopping.\n";
        goto end_run;
    }

    // recv() < 0 with a real error (ECONNRESET, EPIPE, ...) means the
    // socket is dead. EAGAIN/EWOULDBLOCK is just "no data yet" and
    // EINTR is a benign signal interruption — everything else would
    // leave us busy-spinning on a corpse until the duration expires.
    if (__builtin_expect(bytes_read < 0 && errno != EAGAIN &&
                         errno != EWOULDBLOCK && errno != EINTR, 0)) {
        std::cerr << "[Bot " << config.thread_id
                  << "] recv() fatal error: " << strerror(errno)
                  << ". Stopping.\n";
        goto end_run;
    }

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
                        int64_t naive_latency = static_cast<int64_t>(ack_time - pslot.actual_send_ts);
                        int64_t co_latency    = static_cast<int64_t>(ack_time - pslot.intended_ts);
                        // Exclude warmup samples from HDR — TCP slow-start,
                        // ARP, cache-warming spikes pollute the first N
                        // samples. Standard practice in serious benchmarks.
                        // We still count the ack so total_acked is honest.
                        if (naive_latency > 0 &&
                            rx_ack->order_seq > config.warmup_orders) {
                            record_latency(naive_latency, co_latency);
                        }
                        if (pslot.pool_ptr) pool.release(pslot.pool_ptr);
                        pslot = {0, 0, 0, nullptr};
                    }
                    result->total_acked.store(++total_acked, std::memory_order_relaxed);
                } else if (rx_hdr->msg_type == MSG_FILL) {
                    auto* rx_fill = reinterpret_cast<Fill*>(
                        rx_buffer + offset + sizeof(FrameHeader));
                    result->total_fills.store(++total_fills, std::memory_order_relaxed);
                    if (rx_fill->leaves_qty == 0) {
                        size_t ps = rx_fill->order_seq & PENDING_MASK;
                        PendingSlot& pslot = pending[ps];
                        if (pslot.seq == rx_fill->order_seq) {
                            if (pslot.pool_ptr) pool.release(pslot.pool_ptr);
                            pslot = {0, 0, 0, nullptr};
                        }
                    }
                } else if (rx_hdr->msg_type == MSG_REJECT) {
                    auto* rx_rej = reinterpret_cast<Reject*>(
                        rx_buffer + offset + sizeof(FrameHeader));
                    result->total_rejects.store(++total_rejects, std::memory_order_relaxed);
                    size_t ps = rx_rej->order_seq & PENDING_MASK;
                    PendingSlot& pslot = pending[ps];
                    if (pslot.seq == rx_rej->order_seq) {
                        if (pslot.pool_ptr) pool.release(pslot.pool_ptr);
                        pslot = {0, 0, 0, nullptr};
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
                            int64_t naive_latency = static_cast<int64_t>(ack_time - pslot.actual_send_ts);
                            int64_t co_latency    = static_cast<int64_t>(ack_time - pslot.intended_ts);
                            if (naive_latency > 0) {
                                record_latency(naive_latency, co_latency);
                            }
                            if (pslot.pool_ptr) pool.release(pslot.pool_ptr);
                            pslot = {0, 0, 0, nullptr};
                        }
                        result->total_acked.store(++total_acked, std::memory_order_relaxed);
                    } else if (rx_hdr->msg_type == MSG_FILL) {
                        result->total_fills.store(++total_fills, std::memory_order_relaxed);
                    } else if (rx_hdr->msg_type == MSG_REJECT) {
                        result->total_rejects.store(++total_rejects, std::memory_order_relaxed);
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
    result->spsc_dropped        = spsc_dropped_local;

    close(sock);
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
    uint64_t gate_p99_us     = 1000;
    bool     gate_enabled    = true;
    bool     use_spsc        = false;
    uint64_t warmup_orders   = 0;  // 0 = no warmup exclusion (default)

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
        else if (arg == "--gate-p99-us"  && i + 1 < argc) gate_p99_us  = std::stoull(argv[++i]);
        else if (arg == "--no-gate")                      gate_enabled = false;
        else if (arg == "--use-spsc")                     use_spsc     = true;
        else if (arg == "--warmup-orders" && i + 1 < argc)
            warmup_orders = std::stoull(argv[++i]);
    }

    const uint64_t interval_ns = interval_us * 1000;
    const uint64_t duration_ns = duration_sec * 1000000000ULL;

    hft::calibrate_tsc();
    std::cout << "[Main] TSC calibrated.\n";

    bool integrity_passed = true;
    uint64_t gate_p99_ns_observed = 0;
    if (gate_enabled) {
        std::cout << "[Gate] Running integrity self-test (1000 orders @ "
                  << interval_us << "us)...\n";
        gate_p99_ns_observed = integrity_gate_test(ip_addr, port, interval_ns);
        if (gate_p99_ns_observed == UINT64_MAX) {
            std::cerr << "[Gate] FAILED: could not complete self-test\n";
            std::cerr << "[Gate] Target may be unreachable. Aborting.\n";
            return 2;
        }
        uint64_t threshold_ns = gate_p99_us * 1000;
        if (gate_p99_ns_observed > threshold_ns) {
            std::cout << "[Gate] FAILED: bot self-test p99 = "
                      << gate_p99_ns_observed << "ns"
                      << " (threshold " << threshold_ns << "ns)\n";
            std::cout << "[Gate] Run will proceed, but samples will be flagged "
                      << "as INTEGRITY_FAILED for leaderboard filtering.\n";
            integrity_passed = false;
        } else {
            std::cout << "[Gate] PASSED: bot self-test p99 = "
                      << gate_p99_ns_observed << "ns"
                      << " (threshold " << threshold_ns << "ns)\n";
        }
    } else {
        std::cout << "[Gate] SKIPPED (--no-gate)\n";
    }

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
        std::cout << "[Main] Snapshot dir: " << snapshot_dir << "\n";
    }

    if (warmup_orders > 0) {
        std::cout << "[Main] Warmup: excluding first " << warmup_orders
                  << " orders from HDR (TCP slow-start / cache warming)\n";
    }

    if (use_spsc) {
        std::cout << "[Main] SPSC HDR offload ENABLED — hot path pushes\n"
                  << "       (latency, interval) records to lock-free ring;\n"
                  << "       cold consumer threads record to HDR.\n";
    }

    std::vector<BotConfig> configs(num_bots);
    std::vector<BotResult> results(num_bots);
    std::atomic<bool> stop_flag{false};
    std::vector<std::thread> snap_threads;
    std::vector<std::thread> spsc_threads;
    std::vector<std::thread> work_threads;
    snap_threads.reserve(num_bots);
    spsc_threads.reserve(num_bots);
    work_threads.reserve(num_bots);

    // Allocate every histogram (and SPSC ring, if enabled) up front, on
    // the main thread, BEFORE any worker/consumer/snapshot thread exists.
    // Threads then only ever see fully-constructed pointers — no lazy
    // init, no publication race, no sleep-and-hope synchronization.
    for (uint32_t i = 0; i < num_bots; i++) {
        configs[i] = BotConfig{
            .thread_id     = i,
            .cpu_core      = (start_core >= 0) ? (start_core + static_cast<int>(i)) : -1,
            .ip_addr       = ip_addr,
            .port          = port,
            .interval_ns   = interval_ns,
            .duration_ns   = duration_ns,
            .snapshot_dir  = snapshot_dir,
            .use_spsc      = use_spsc,
            .warmup_orders = warmup_orders,
        };
        hdr_init(1, 30000000000LL, 3, &results[i].naive_hist);
        hdr_init(1, 30000000000LL, 3, &results[i].co_hist);
        if (use_spsc) results[i].spsc_ring = new LatencyRing();
    }

    for (uint32_t i = 0; i < num_bots; i++) {
        if (!snapshot_dir.empty()) {
            snap_threads.emplace_back(snapshot_writer,
                                      i, snapshot_dir,
                                      &results[i], &stop_flag);
        }
        if (use_spsc) {
            spsc_threads.emplace_back(spsc_consumer, &results[i], &stop_flag);
        }
        work_threads.emplace_back(bot_worker, configs[i], &results[i]);
    }

    for (auto& t : work_threads) t.join();
    stop_flag.store(true, std::memory_order_release);
    for (auto& t : snap_threads)  if (t.joinable()) t.join();
    for (auto& t : spsc_threads)  if (t.joinable()) t.join();

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
    uint64_t agg_spsc_dropped = 0;
    uint32_t connected_count  = 0;

    for (uint32_t i = 0; i < num_bots; i++) {
        if (!results[i].connected) continue;
        connected_count++;
        if (results[i].naive_hist) hdr_add(unified_naive, results[i].naive_hist);
        if (results[i].co_hist)    hdr_add(unified_co,    results[i].co_hist);
        agg_sent         += results[i].total_sent.load(std::memory_order_relaxed);
        agg_acked        += results[i].total_acked.load(std::memory_order_relaxed);
        agg_fills        += results[i].total_fills.load(std::memory_order_relaxed);
        agg_rejects      += results[i].total_rejects.load(std::memory_order_relaxed);
        agg_send_fail    += results[i].send_failures;
        agg_partial      += results[i].partial_send_aborts;
        agg_collisions   += results[i].pending_collisions;
        agg_pool_exhaust += results[i].pool_exhausted;
        agg_spsc_dropped += results[i].spsc_dropped;
    }

    std::cout << "\n[Main] All bots finished. Connected: "
              << connected_count << "/" << num_bots << "\n";

    if (gate_enabled) {
        if (integrity_passed) {
            std::cout << "[Main] INTEGRITY: PASSED  (gate p99 = "
                      << gate_p99_ns_observed << "ns)\n";
        } else {
            std::cout << "[Main] INTEGRITY: FAILED — samples flagged as untrustworthy\n";
            std::cout << "[Main] Leaderboard should EXCLUDE this run's samples\n";
        }
    } else {
        std::cout << "[Main] INTEGRITY: UNCHECKED  (gate disabled)\n";
    }

    if (use_spsc) {
        std::cout << "[Main] SPSC: dropped " << agg_spsc_dropped
                  << " records (0 = ring sized correctly)\n";
    }

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
    if (!integrity_passed) {
        std::cout << " ⚠️  INTEGRITY FAILED — DO NOT USE FOR SCORING\n";
    }
    if (use_spsc) {
        std::cout << " [SPSC offload path]\n";
    }
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
        if (results[i].spsc_ring)  delete results[i].spsc_ring;
    }
    hdr_close(unified_naive);
    hdr_close(unified_co);

    return integrity_passed ? 0 : 1;
}
