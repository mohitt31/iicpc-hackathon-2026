// bot.cpp — Open-loop latency benchmarking client
// Demonstrates Coordinated Omission detection and correction.
//
// The critical invariant: latency is measured against INTENDED send time,
// not actual send time. When the bot falls behind schedule (because TCP
// backpressure blocked send(), or for any other reason), the intended
// time keeps advancing at the configured interval. This means a stall
// that delays 50 orders by 5ms records 50 latencies of ~5ms, not 50
// latencies of ~15µs measured from the post-stall burst.
//
// This is the structural fix for Coordinated Omission.

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
#include <cassert>

#include "contracts/interface_contract_v1.h"
#include "tsc_util.h"
#include <hdr/hdr_histogram.h>

#ifdef __cpp_lib_hardware_interference_size
using std::hardware_destructive_interference_size;
#else
constexpr std::size_t hardware_destructive_interference_size = 64;
#endif

static void set_non_blocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) throw std::runtime_error("fcntl F_GETFL failed");
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        throw std::runtime_error("fcntl F_SETFL failed");
    }
}

int main(int argc, char* argv[]) {
    uint64_t interval_us = 100;
    uint64_t duration_sec = 10;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--interval-us" && i + 1 < argc) {
            interval_us = std::stoull(argv[++i]);
        } else if (arg == "--duration-sec" && i + 1 < argc) {
            duration_sec = std::stoull(argv[++i]);
        }
    }

    const uint64_t interval_ns = interval_us * 1000;
    const uint64_t total_duration_ns = duration_sec * 1000000000ULL;

    hft::calibrate_tsc();
    std::cout << "[Bot] TSC calibrated.\n";
    std::cout << "[Bot] Interval: " << interval_us << " us, Duration: " << duration_sec << " s\n";

    // ── HdrHistograms ──────────────────────────────────────────────
    // Track 1 ns up to 30 seconds with 3 significant figures.
    struct hdr_histogram* naive_hist = nullptr;
    struct hdr_histogram* co_hist = nullptr;
    hdr_init(1, 30000000000LL, 3, &naive_hist);
    hdr_init(1, 30000000000LL, 3, &co_hist);

    // ── Socket setup ───────────────────────────────────────────────
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { std::cerr << "socket() failed\n"; return 1; }

    sockaddr_in serv_addr{};
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(9000);
    inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr);

    if (connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        std::cerr << "connect() failed. Make sure null_responder is running.\n";
        return 1;
    }

    int opt = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    // FIX #3: Shrink SO_SNDBUF so the kernel can't silently absorb
    // thousands of orders during a responder stall. With a 4KB buffer,
    // backpressure becomes visible after ~90 orders (44B each), which
    // forces send() to block or return EAGAIN much sooner.
    int sndbuf_size = 4096;
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sndbuf_size, sizeof(sndbuf_size));

    set_non_blocking(sock);

    std::cout << "[Bot] Connected. Starting open-loop tight-poll.\n";
    std::cout << "[Bot] SO_SNDBUF pinned to " << sndbuf_size << " bytes.\n";

    // ── Pending map ────────────────────────────────────────────────
    // Stores the INTENDED send time for each seq number.
    // FIX #4: Power-of-2 size with collision detection.
    constexpr size_t PENDING_SLOTS = 1 << 20; // 1,048,576
    constexpr size_t PENDING_MASK  = PENDING_SLOTS - 1;
    // Each slot: [intended_send_ts, seq_that_wrote_it]
    struct PendingSlot {
        uint64_t intended_ts;
        uint64_t seq;
    };
    std::vector<PendingSlot> pending(PENDING_SLOTS, {0, 0});

    // ── TX/RX buffers (pre-populated, zero-alloc on hot path) ─────
    alignas(hardware_destructive_interference_size)
        uint8_t tx_buffer[sizeof(FrameHeader) + sizeof(NewOrder)];
    auto* tx_hdr   = reinterpret_cast<FrameHeader*>(tx_buffer);
    tx_hdr->msg_type = MSG_NEWORDER;
    tx_hdr->msg_len  = sizeof(NewOrder);
    auto* tx_order = reinterpret_cast<NewOrder*>(tx_buffer + sizeof(FrameHeader));
    tx_order->symbol_id  = 1;
    tx_order->order_type = ORDER_LIMIT;
    tx_order->side       = SIDE_BUY;
    tx_order->price      = 15050;
    tx_order->quantity   = 100;

    // rx_buffer holds fresh recv() bytes; residual_buf holds the trailing
    // partial message from the previous recv() that couldn't be parsed yet.
    alignas(hardware_destructive_interference_size) uint8_t rx_buffer[8192];
    // BUG 2 FIX: residual buffer — max one partial message can be leftover
    constexpr size_t MSG_MAX = sizeof(FrameHeader) + sizeof(OrderAck);
    uint8_t residual_buf[MSG_MAX];
    size_t  residual_len = 0;  // bytes currently sitting in residual_buf

    // ── Counters ──────────────────────────────────────────────────
    uint64_t total_sent = 0;
    uint64_t total_acked = 0;
    uint64_t send_failures = 0;

    const uint64_t start_time = hft::rdtscp_ns();
    uint64_t next_send_time   = start_time;
    uint64_t last_report_time = start_time;
    const uint64_t end_time   = start_time + total_duration_ns;

    uint64_t sent_since_last_report = 0;

    // ── Main loop ─────────────────────────────────────────────────
    while (true) {
        uint64_t now = hft::rdtscp_ns();

        if (__builtin_expect(now >= end_time, 0)) break;

        // ── Periodic report (every 1s) ────────────────────────────
        if (__builtin_expect(now - last_report_time >= 1000000000ULL, 0)) {
            std::cout << "[Report] Sent: " << total_sent
                      << " | Acked: " << total_acked
                      << " | Rate: " << sent_since_last_report << " msgs/sec"
                      << " | SendFail: " << send_failures << "\n";
            sent_since_last_report = 0;
            last_report_time += 1000000000ULL;
        }

        // ── Send path: CATCH-UP LOOP ──────────────────────────────
        // FIX #1 + #2: This is the structural CO fix.
        //
        // When the bot falls behind schedule (because send() returned
        // EAGAIN, or the loop was slow), it fires multiple orders
        // back-to-back to catch up to wall-clock. Each order records
        // its INTENDED send time (the slot it was supposed to fill),
        // NOT the actual wall-clock time of the send() call.
        //
        // This means: if the responder stalled 5ms and the bot is
        // now 50 intervals behind, we send 50 orders in a burst,
        // each tagged with the time they *should* have been sent.
        // When the acks come back, latency = ack_time - intended_time,
        // which correctly captures the full 5ms of delay.
        while (__builtin_expect(now >= next_send_time, 0)) {
            uint64_t this_seq = total_sent + 1;   // tentative, don't commit yet

            tx_order->seq = this_seq;
            tx_order->timestamp_ns = next_send_time; // intended, not actual

            ssize_t sent = send(sock, tx_buffer, sizeof(tx_buffer), MSG_DONTWAIT);
            // BUG 1 FIX: treat partial send (0 < sent < full size) same as EAGAIN.
            // TCP may write fewer bytes than requested when SNDBUF is small.
            // A partial write = garbled NewOrder on the wire = wrong ack or reject.
            // Don't commit; the wire never saw a complete message.
            if (__builtin_expect(sent != static_cast<ssize_t>(sizeof(tx_buffer)), 0)) {
                send_failures++;
                break;  // retry next outer iteration; schedule NOT advanced
            }

            // Full message reached the wire. Now commit everything.
            size_t slot = this_seq & PENDING_MASK;
            assert((pending[slot].seq == 0) ||
                   ((this_seq - pending[slot].seq) > (PENDING_SLOTS / 2)));
            pending[slot] = {next_send_time, this_seq};
            total_sent = this_seq;

            sent_since_last_report++;
            next_send_time += interval_ns;
        }

        // ── Receive path ──────────────────────────────────────────
        // BUG 2 FIX: prepend any leftover bytes from the previous recv()
        // so a message split across two recv() calls is reassembled.
        size_t recv_offset = residual_len;
        if (residual_len > 0) {
            std::memcpy(rx_buffer, residual_buf, residual_len);
        }
        ssize_t fresh = recv(sock, rx_buffer + recv_offset,
                             sizeof(rx_buffer) - recv_offset, MSG_DONTWAIT);
        if (__builtin_expect(fresh > 0, 1)) {
            size_t bytes_available = recv_offset + static_cast<size_t>(fresh);
            size_t offset = 0;
            while (offset + sizeof(FrameHeader) + sizeof(OrderAck) <= bytes_available) {
                auto* rx_hdr = reinterpret_cast<FrameHeader*>(rx_buffer + offset);

                if (__builtin_expect(rx_hdr->msg_type == MSG_ORDERACK, 1)) {
                    auto* rx_ack = reinterpret_cast<OrderAck*>(rx_buffer + offset + sizeof(FrameHeader));
                    uint64_t ack_time = hft::rdtscp_ns();

                    size_t ack_slot = rx_ack->order_seq & PENDING_MASK;
                    PendingSlot& ps = pending[ack_slot];

                    if (__builtin_expect(ps.seq == rx_ack->order_seq && ps.intended_ts > 0, 1)) {
                        int64_t latency = static_cast<int64_t>(ack_time - ps.intended_ts);
                        if (__builtin_expect(latency > 0, 1)) {
                            hdr_record_value(naive_hist, latency);
                            hdr_record_corrected_value(co_hist, latency, interval_ns);
                        }
                        ps = {0, 0};
                    }
                    total_acked++;
                }
                offset += sizeof(FrameHeader) + rx_hdr->msg_len;
            }

            // Save any trailing partial message for the next iteration.
            residual_len = bytes_available - offset;
            if (residual_len > 0) {
                std::memcpy(residual_buf, rx_buffer + offset, residual_len);
            }
        } else {
            // Nothing new from recv() — residual stays as-is for next iter.
            // (If fresh == 0 the connection closed; if < 0 it's EAGAIN.)
        }
    }

    // ── Final drain: give responder 500ms to flush remaining acks ──
    // Uses the same residual-buffer logic so split messages at drain
    // boundary are also handled correctly.
    {
        auto drain_start = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - drain_start < std::chrono::milliseconds(500)) {
            size_t d_offset = residual_len;
            if (residual_len > 0) std::memcpy(rx_buffer, residual_buf, residual_len);

            ssize_t fresh = recv(sock, rx_buffer + d_offset,
                                 sizeof(rx_buffer) - d_offset, MSG_DONTWAIT);
            if (fresh <= 0) continue;

            size_t avail = d_offset + static_cast<size_t>(fresh);
            size_t offset = 0;
            while (offset + sizeof(FrameHeader) + sizeof(OrderAck) <= avail) {
                auto* rx_hdr = reinterpret_cast<FrameHeader*>(rx_buffer + offset);
                if (rx_hdr->msg_type == MSG_ORDERACK) {
                    auto* rx_ack = reinterpret_cast<OrderAck*>(rx_buffer + offset + sizeof(FrameHeader));
                    uint64_t ack_time = hft::rdtscp_ns();
                    size_t ack_slot = rx_ack->order_seq & PENDING_MASK;
                    PendingSlot& ps = pending[ack_slot];
                    if (ps.seq == rx_ack->order_seq && ps.intended_ts > 0) {
                        int64_t latency = static_cast<int64_t>(ack_time - ps.intended_ts);
                        if (latency > 0) {
                            hdr_record_value(naive_hist, latency);
                            hdr_record_corrected_value(co_hist, latency, interval_ns);
                        }
                        ps = {0, 0};
                    }
                    total_acked++;
                }
                offset += sizeof(FrameHeader) + rx_hdr->msg_len;
            }
            residual_len = avail - offset;
            if (residual_len > 0) std::memcpy(residual_buf, rx_buffer + offset, residual_len);
        }
    }

    // ── Results ───────────────────────────────────────────────────
    std::cout << "\n[Bot] Run complete. Total Sent: " << total_sent
              << " | Total Acked: " << total_acked
              << " | Send Failures (EAGAIN): " << send_failures << "\n\n";

    std::cout << "=================================================================\n";
    std::cout << "        ROUND-TRIP LATENCY — Naive vs CO-Corrected (ns)          \n";
    std::cout << "=================================================================\n";
    std::cout << std::left << std::setw(12) << "Percentile"
              << std::right << std::setw(18) << "Naive"
              << std::setw(18) << "CO-Corrected"
              << std::setw(12) << "Ratio" << "\n";
    std::cout << "-----------------------------------------------------------------\n";

    double percentiles[] = {50.0, 90.0, 99.0, 99.9, 99.99, 100.0};
    const char* names[]  = {"p50", "p90", "p99", "p99.9", "p99.99", "Max"};

    for (int i = 0; i < 6; i++) {
        int64_t n_val = hdr_value_at_percentile(naive_hist, percentiles[i]);
        int64_t c_val = hdr_value_at_percentile(co_hist, percentiles[i]);
        double ratio = (n_val > 0) ? static_cast<double>(c_val) / static_cast<double>(n_val) : 0.0;
        std::cout << std::left  << std::setw(12) << names[i]
                  << std::right << std::setw(18) << n_val
                  << std::setw(18) << c_val
                  << std::setw(11) << std::fixed << std::setprecision(1) << ratio << "x\n";
    }
    std::cout << "=================================================================\n";
    std::cout << "\nNaive sample count:  " << naive_hist->total_count << "\n";
    std::cout << "CO-corrected count:  " << co_hist->total_count
              << "  (backfilled " << (co_hist->total_count - naive_hist->total_count) << " phantom samples)\n";

    hdr_close(naive_hist);
    hdr_close(co_hist);
    close(sock);
    return 0;
}
