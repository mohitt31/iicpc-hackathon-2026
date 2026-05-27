// bot.cpp — Open-loop latency benchmarking client
// Demonstrates Coordinated Omission detection and correction.
//
// Key invariants:
//   1. OPEN-LOOP: bot fires on wall-clock schedule regardless of responses
//   2. CO-CORRECTED: latency measured against INTENDED send time, not actual
//   3. ZERO-MALLOC on hot path: memory pool pre-allocated at startup
//   4. DETERMINISTIC ORDER GENERATION: xorshift64 with fixed seed

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
#include <atomic>

#include "contracts/interface_contract_v1.h"
#include "tsc_util.h"
#include <hdr/hdr_histogram.h>

#ifdef __cpp_lib_hardware_interference_size
using std::hardware_destructive_interference_size;
#else
constexpr std::size_t hardware_destructive_interference_size = 64;
#endif

// ── xorshift64 RNG ────────────────────────────────────────────────────────
// Why xorshift64 and not std::mt19937?
// std::mt19937 has ~2KB state and 624-element init table — too slow for
// a hot loop that generates one order per 100μs. xorshift64 is 3 XOR-shift
// ops, 1 uint64_t of state, sub-nanosecond per call.
static inline uint64_t xorshift64(uint64_t& state) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}

// ── Memory Pool ───────────────────────────────────────────────────────────
// Pre-allocates N NewOrder slots at startup. acquire()/release() are O(1)
// and zero-malloc — a free-list index stack. All slots are 64-byte aligned
// to avoid false sharing when multiple bots share a pool.
//
// Why a pool instead of a static tx_buffer?
// Single-bot: the pool is equivalent. Multi-bot (Day 7): each bot thread
// acquires its own slot, fills it, sends it, and releases on ack. No locks
// needed if each thread has its own pool instance.
struct OrderPool {
    static constexpr size_t POOL_SIZE = 1024; // 1024 * 40B = 40KB — fits in L1

    // Each slot is 64-byte aligned to avoid cache-line sharing across bots
    alignas(64) NewOrder slots[POOL_SIZE];

    // Free-list: stack of available slot indices.
    // Using a simple array-backed stack — O(1) push/pop, zero allocation.
    uint16_t free_stack[POOL_SIZE];
    uint16_t free_top = 0;          // points to next available entry
    uint64_t exhausted_count = 0;   // how many times we had no free slots

    OrderPool() {
        // Initialize free-list with all slots available
        for (uint16_t i = 0; i < POOL_SIZE; i++) {
            free_stack[i] = i;
        }
        free_top = POOL_SIZE;
    }

    // acquire() — get a free slot. Returns nullptr if pool exhausted.
    // Caller fills the slot and sends it. Must call release() when done.
    inline NewOrder* acquire() {
        if (__builtin_expect(free_top == 0, 0)) {
            exhausted_count++;
            return nullptr; // pool exhausted — caller must handle
        }
        return &slots[free_stack[--free_top]];
    }

    // release() — return slot back to pool after ack/reject received.
    // No bounds check — caller must pass a pointer from this pool.
    inline void release(NewOrder* ptr) {
        uint16_t idx = static_cast<uint16_t>(ptr - slots);
        free_stack[free_top++] = idx;
    }
};

static void set_non_blocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) throw std::runtime_error("fcntl F_GETFL failed");
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1)
        throw std::runtime_error("fcntl F_SETFL failed");
}

int main(int argc, char* argv[]) {
    uint64_t interval_us  = 100;
    uint64_t duration_sec = 10;
    std::string ip_addr   = "127.0.0.1";

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--interval-us" && i + 1 < argc)
            interval_us = std::stoull(argv[++i]);
        else if (arg == "--duration-sec" && i + 1 < argc)
            duration_sec = std::stoull(argv[++i]);
        else if (arg == "--ip" && i + 1 < argc)
            ip_addr = argv[++i];
    }

    const uint64_t interval_ns       = interval_us * 1000;
    const uint64_t total_duration_ns = duration_sec * 1000000000ULL;

    hft::calibrate_tsc();
    std::cout << "[Bot] TSC calibrated.\n";
    std::cout << "[Bot] Interval: " << interval_us << " us, Duration: "
              << duration_sec << " s\n";

    // ── Memory Pool (startup allocation, zero-malloc after this) ─────────
    OrderPool pool;
    std::cout << "[Bot] Order pool initialized: " << OrderPool::POOL_SIZE
              << " slots, " << (OrderPool::POOL_SIZE * sizeof(NewOrder))
              << " bytes\n";

    // ── xorshift64 RNG — deterministic, seed=42 ──────────────────────────
    // Why seed=42? Reproducibility — same seed = same order sequence every
    // run, so latency differences between runs are from the system, not RNG.
    uint64_t rng_state = 42;

    // ── HDR histograms ────────────────────────────────────────────────────
    struct hdr_histogram* naive_hist = nullptr;
    struct hdr_histogram* co_hist    = nullptr;
    hdr_init(1, 30000000000LL, 3, &naive_hist);
    hdr_init(1, 30000000000LL, 3, &co_hist);

    // ── Socket setup ──────────────────────────────────────────────────────
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { std::cerr << "socket() failed\n"; return 1; }

    sockaddr_in serv_addr{};
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port   = htons(9000);
    inet_pton(AF_INET, ip_addr.c_str(), &serv_addr.sin_addr);

    std::cout << "[Bot] Connecting to " << ip_addr << ":9000...\n";
    if (connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        std::cerr << "connect() failed. Make sure engine is running.\n";
        return 1;
    }

    int opt = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    // Shrink SO_SNDBUF so kernel can't absorb thousands of orders during
    // a stall — makes backpressure visible, which is required for CO proof.
    int sndbuf_size = 4096;
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sndbuf_size, sizeof(sndbuf_size));

    set_non_blocking(sock);

    std::cout << "[Bot] Connected. Starting open-loop tight-poll.\n";
    std::cout << "[Bot] SO_SNDBUF pinned to " << sndbuf_size << " bytes.\n";

    // ── Pending map: seq → {intended_ts, pool_slot_ptr} ──────────────────
    // Power-of-2 size for fast modulo via bitmask.
    constexpr size_t PENDING_SLOTS = 1 << 20;
    constexpr size_t PENDING_MASK  = PENDING_SLOTS - 1;
    struct PendingSlot {
        uint64_t  intended_ts;
        uint64_t  seq;
        NewOrder* pool_ptr;    // non-null → release back to pool on ack
    };
    std::vector<PendingSlot> pending(PENDING_SLOTS, {0, 0, nullptr});

    // ── TX wire buffer (FrameHeader + NewOrder, pre-populated header) ─────
    constexpr size_t TX_FRAME_SIZE = sizeof(FrameHeader) + sizeof(NewOrder);
    alignas(hardware_destructive_interference_size) uint8_t tx_buffer[TX_FRAME_SIZE];
    auto* tx_hdr      = reinterpret_cast<FrameHeader*>(tx_buffer);
    tx_hdr->msg_type  = MSG_NEWORDER;
    tx_hdr->_pad      = 0;
    tx_hdr->msg_len   = sizeof(NewOrder);
    // NewOrder pointer inside the wire buffer — filled per-send from pool slot
    auto* tx_order    = reinterpret_cast<NewOrder*>(tx_buffer + sizeof(FrameHeader));

    // ── RX buffer with residual handling ──────────────────────────────────
    constexpr size_t RX_BUFFER_SIZE = 65536;
    alignas(hardware_destructive_interference_size) uint8_t rx_buffer[RX_BUFFER_SIZE];
    size_t residual_bytes = 0;

    // ── Counters ──────────────────────────────────────────────────────────
    uint64_t total_sent           = 0;
    uint64_t total_acked          = 0;
    uint64_t total_fills          = 0;
    uint64_t total_rejects        = 0;
    uint64_t send_failures        = 0;
    uint64_t partial_send_aborts  = 0;
    uint64_t pending_collisions   = 0;
    // side alternates BUY/SELL deterministically (no RNG needed)
    uint8_t  next_side            = SIDE_BUY;

    const uint64_t start_time   = hft::rdtscp_ns();
    uint64_t next_send_time     = start_time;
    uint64_t last_report_time   = start_time;
    const uint64_t end_time     = start_time + total_duration_ns;
    uint64_t sent_since_report  = 0;

    // ── MAIN LOOP ─────────────────────────────────────────────────────────
    while (true) {
        uint64_t now = hft::rdtscp_ns();
        if (__builtin_expect(now >= end_time, 0)) break;

        // ── Periodic report (every 1s) ────────────────────────────────────
        if (__builtin_expect(now - last_report_time >= 1000000000ULL, 0)) {
            std::cout << "[Report] Sent: " << total_sent
                      << " | Acked: " << total_acked
                      << " | Fills: " << total_fills
                      << " | Rejects: " << total_rejects
                      << " | Rate: " << sent_since_report << " msgs/sec"
                      << " | SendFail: " << send_failures << "\n";
            sent_since_report = 0;
            last_report_time += 1000000000ULL;
        }

        // ── SEND PATH (catch-up loop) ─────────────────────────────────────
        // Structural CO fix: fire all "overdue" slots in a burst, each
        // tagged with its INTENDED time (not wall-clock time of actual send).
        while (__builtin_expect(now >= next_send_time, 0)) {
            // Acquire pool slot — zero malloc
            NewOrder* slot = pool.acquire();
            if (__builtin_expect(slot == nullptr, 0)) {
                // Pool exhausted — too many in-flight orders.
                // Skip this interval slot, advance schedule.
                // This is preferable to a malloc: if pool is full we're
                // already overloaded — adding more would make it worse.
                next_send_time += interval_ns;
                continue;
            }

            uint64_t this_seq = total_sent + 1;

            // ── Realistic order generation (xorshift64, seed=42) ──────────
            // Why varied orders? Real exchange testing requires diverse
            // price/qty/side to trigger actual matching, not just acks.

            // price: uniform in [99, 101] ticks — narrow spread around par
            int64_t price = static_cast<int64_t>(99 + (xorshift64(rng_state) % 3));

            // qty: uniform in [1, 5]
            uint64_t qty = 1 + (xorshift64(rng_state) % 5);

            // order_type: LIMIT 95%, MARKET 5%
            // Why 95/5? Matches real market microstructure — most orders
            // are limit orders; market orders consume liquidity aggressively.
            bool is_market = (xorshift64(rng_state) % 20 == 0); // 1/20 = 5%

            // side: strictly alternating BUY/SELL — ensures balanced book
            // and avoids one-sided liquidity starvation in the reference engine
            uint8_t side = next_side;
            next_side = (next_side == SIDE_BUY) ? SIDE_SELL : SIDE_BUY;

            // Fill the pool slot
            slot->seq          = this_seq;
            slot->timestamp_ns = next_send_time; // INTENDED time, not actual
            slot->symbol_id    = 1;
            slot->order_type   = is_market ? ORDER_MARKET : ORDER_LIMIT;
            slot->side         = side;
            slot->price        = is_market ? 0 : price; // MARKET must be 0
            slot->quantity     = qty;

            // Copy pool slot into wire buffer (28 bytes of variable fields)
            // FrameHeader already pre-populated (msg_type=1, msg_len=40)
            std::memcpy(tx_order, slot, sizeof(NewOrder));

            ssize_t sent = send(sock, tx_buffer, TX_FRAME_SIZE, MSG_DONTWAIT);

            if (__builtin_expect(sent < 0, 0)) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    send_failures++;
                    pool.release(slot); // return slot, order never sent
                    break;
                }
                pool.release(slot);
                break;
            }

            // Partial send — wire now corrupt, abort run
            if (__builtin_expect(static_cast<size_t>(sent) != TX_FRAME_SIZE, 0)) {
                std::cerr << "[Bot] FATAL: partial send (" << sent
                          << "/" << TX_FRAME_SIZE << " bytes). Aborting.\n";
                partial_send_aborts++;
                pool.release(slot);
                goto end_run;
            }

            // Commit: store slot pointer in pending map for release on ack
            size_t map_slot = this_seq & PENDING_MASK;
            if (__builtin_expect(pending[map_slot].seq != 0, 0)) {
                uint64_t age = this_seq - pending[map_slot].seq;
                if (age <= PENDING_SLOTS / 2) pending_collisions++;
                // Release old slot if still occupied (shouldn't happen normally)
                if (pending[map_slot].pool_ptr)
                    pool.release(pending[map_slot].pool_ptr);
            }
            pending[map_slot] = {next_send_time, this_seq, slot};
            total_sent = this_seq;

            sent_since_report++;
            next_send_time += interval_ns;
        }

        // ── RECEIVE PATH (with residual buffer) ───────────────────────────
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
                            hdr_record_value(naive_hist, latency);
                            hdr_record_corrected_value(co_hist, latency, interval_ns);
                        }
                        // Release pool slot — order lifecycle complete
                        if (pslot.pool_ptr) pool.release(pslot.pool_ptr);
                        pslot = {0, 0, nullptr};
                    }
                    total_acked++;

                } else if (rx_hdr->msg_type == MSG_FILL) {
                    auto* rx_fill = reinterpret_cast<Fill*>(
                        rx_buffer + offset + sizeof(FrameHeader));
                    total_fills++;
                    // Full fill: remove from pending
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
                    // Reject: always remove from pending
                    size_t ps = rx_rej->order_seq & PENDING_MASK;
                    PendingSlot& pslot = pending[ps];
                    if (pslot.seq == rx_rej->order_seq) {
                        if (pslot.pool_ptr) pool.release(pslot.pool_ptr);
                        pslot = {0, 0, nullptr};
                    }
                }

                offset += frame_size;
            }

            // Preserve trailing partial bytes for next recv
            residual_bytes = total_bytes - offset;
            if (residual_bytes > 0 && offset > 0)
                std::memmove(rx_buffer, rx_buffer + offset, residual_bytes);
        }
    }

end_run:
    // ── Final drain: 500ms grace for in-flight acks ───────────────────────
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
                                hdr_record_value(naive_hist, latency);
                                hdr_record_corrected_value(co_hist, latency, interval_ns);
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

    // ── Results ───────────────────────────────────────────────────────────
    std::cout << "\n[Bot] Run complete."
              << " Total Sent: " << total_sent
              << " | Total Acked: " << total_acked
              << " | Total Fills: " << total_fills
              << " | Total Rejects: " << total_rejects
              << " | EAGAIN: " << send_failures
              << " | PartialAborts: " << partial_send_aborts
              << " | PendingCollisions: " << pending_collisions
              << " | PoolExhausted: " << pool.exhausted_count << "\n\n";

    std::cout << "=================================================================\n";
    std::cout << "        ROUND-TRIP LATENCY — Naive vs CO-Corrected (ns)          \n";
    std::cout << "=================================================================\n";
    std::cout << std::left  << std::setw(12) << "Percentile"
              << std::right << std::setw(18) << "Naive"
              << std::setw(18) << "CO-Corrected"
              << std::setw(12) << "Ratio" << "\n";
    std::cout << "-----------------------------------------------------------------\n";

    double percentiles[] = {50.0, 90.0, 99.0, 99.9, 99.99, 100.0};
    const char* names[]  = {"p50", "p90", "p99", "p99.9", "p99.99", "Max"};

    for (int i = 0; i < 6; i++) {
        int64_t n_val = hdr_value_at_percentile(naive_hist, percentiles[i]);
        int64_t c_val = hdr_value_at_percentile(co_hist,    percentiles[i]);
        double ratio  = (n_val > 0)
            ? static_cast<double>(c_val) / static_cast<double>(n_val) : 0.0;
        std::cout << std::left  << std::setw(12) << names[i]
                  << std::right << std::setw(18) << n_val
                  << std::setw(18) << c_val
                  << std::setw(11) << std::fixed << std::setprecision(1)
                  << ratio << "x\n";
    }
    std::cout << "=================================================================\n";
    std::cout << "\nNaive sample count:  " << naive_hist->total_count << "\n";
    std::cout << "CO-corrected count:  " << co_hist->total_count
              << "  (backfilled "
              << (co_hist->total_count - naive_hist->total_count)
              << " phantom samples)\n";

    hdr_close(naive_hist);
    hdr_close(co_hist);
    close(sock);
    return 0;
}
