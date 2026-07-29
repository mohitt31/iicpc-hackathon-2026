// co_correction_probe.cpp — diagnostic tool, NOT part of the judged demo path.
//
// Question it answers: bot.cpp already computes an honest per-order latency
// (ack_time - intended_send_time, the open-loop CO fix). It then also feeds
// that value through HdrHistogram's hdr_record_corrected_value(), which
// backfills synthetic samples below any value larger than the expected
// interval. Is that backfill redundant, or does it change where the injected
// stall becomes visible in the percentile curve?
//
// This tool drives the exact same wire protocol against the same
// null_responder test double, but records every per-order latency into TWO
// histograms in parallel — one via hdr_record_value (no backfill) and one via
// hdr_record_corrected_value (with backfill) — so the two can be compared
// side by side from one real run, instead of a synthetic model.
//
// Usage matches bot.cpp's CO-proof invocation:
//   ./null_responder --stall-mode --stall-every 20000 --stall-ms 5 &
//   ./co_correction_probe --orders 600000 --interval-us 100 --port 9000

#include <arpa/inet.h>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <hdr/hdr_histogram.h>
#include <iostream>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

#include "../../contracts/interface_contract_v1.h"
#include "tsc_util.h"

int main(int argc, char* argv[]) {
    uint64_t n_orders    = 600000;
    uint64_t interval_us = 100;
    uint16_t port        = 9000;
    std::string ip       = "127.0.0.1";

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--orders" && i + 1 < argc) n_orders = std::stoull(argv[++i]);
        else if (a == "--interval-us" && i + 1 < argc) interval_us = std::stoull(argv[++i]);
        else if (a == "--port" && i + 1 < argc) port = static_cast<uint16_t>(std::stoi(argv[++i]));
        else if (a == "--ip" && i + 1 < argc) ip = argv[++i];
    }
    const uint64_t interval_ns = interval_us * 1000;

    hft::calibrate_tsc();

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { std::cerr << "socket() failed\n"; return 1; }
    int opt = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);
    if (connect(sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "connect() failed: " << strerror(errno) << "\n";
        return 1;
    }
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    struct hdr_histogram* plain_hist     = nullptr; // hdr_record_value only
    struct hdr_histogram* corrected_hist = nullptr; // hdr_record_corrected_value on top
    hdr_init(1, 10'000'000'000LL, 3, &plain_hist);
    hdr_init(1, 10'000'000'000LL, 3, &corrected_hist);

    // Same pending-slot design as bot.cpp: fixed grid, no send ever gated on ack.
    const size_t PENDING_SLOTS = 1 << 20;
    const size_t PENDING_MASK  = PENDING_SLOTS - 1;
    struct PendingSlot { uint64_t intended_ts; uint64_t seq; bool live; };
    std::vector<PendingSlot> pending(PENDING_SLOTS, {0, 0, false});

    uint8_t tx_buffer[sizeof(FrameHeader) + sizeof(NewOrder)];
    auto* tx_hdr = reinterpret_cast<FrameHeader*>(tx_buffer);
    tx_hdr->msg_type = MSG_NEWORDER;
    tx_hdr->_pad = 0;
    tx_hdr->msg_len = sizeof(NewOrder);
    auto* tx_order = reinterpret_cast<NewOrder*>(tx_buffer + sizeof(FrameHeader));
    tx_order->symbol_id  = 1;
    tx_order->order_type = ORDER_LIMIT;
    tx_order->side       = SIDE_BUY;
    tx_order->_pad[0] = tx_order->_pad[1] = 0;
    tx_order->price    = 100;
    tx_order->quantity = 1;

    uint8_t rx_buffer[65536];
    size_t residual = 0;

    uint64_t total_sent = 0, total_acked = 0;
    uint64_t next_send_time = hft::rdtscp_ns();
    const uint64_t start = next_send_time;

    while (total_acked < n_orders) {
        uint64_t now = hft::rdtscp_ns();

        while (total_sent < n_orders && now >= next_send_time) {
            uint64_t this_seq = total_sent + 1;
            tx_order->seq = this_seq;
            tx_order->timestamp_ns = next_send_time;

            ssize_t sent = send(sock, tx_buffer, sizeof(tx_buffer), MSG_DONTWAIT);
            if (sent < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break; // retry same slot next loop
                std::cerr << "send() fatal: " << strerror(errno) << "\n";
                return 1;
            }
            size_t ps = this_seq & PENDING_MASK;
            pending[ps] = {next_send_time, this_seq, true};
            total_sent = this_seq;
            next_send_time += interval_ns; // unconditional — this IS the open-loop fix
            now = hft::rdtscp_ns();
        }

        ssize_t n = recv(sock, rx_buffer + residual, sizeof(rx_buffer) - residual, MSG_DONTWAIT);
        if (n > 0) {
            size_t total = residual + static_cast<size_t>(n);
            size_t off = 0;
            while (off + sizeof(FrameHeader) <= total) {
                auto* hdr = reinterpret_cast<FrameHeader*>(rx_buffer + off);
                size_t frame = sizeof(FrameHeader) + hdr->msg_len;
                if (off + frame > total) break;
                if (hdr->msg_type == MSG_ORDERACK) {
                    uint64_t ack_time = hft::rdtscp_ns();
                    auto* ack = reinterpret_cast<OrderAck*>(rx_buffer + off + sizeof(FrameHeader));
                    size_t ps = ack->order_seq & PENDING_MASK;
                    if (pending[ps].live && pending[ps].seq == ack->order_seq) {
                        int64_t co_latency = static_cast<int64_t>(ack_time - pending[ps].intended_ts);
                        if (co_latency > 0) {
                            hdr_record_value(plain_hist, co_latency);
                            hdr_record_corrected_value(corrected_hist, co_latency, interval_ns);
                        }
                        pending[ps].live = false;
                        ++total_acked;
                    }
                }
                off += frame;
            }
            residual = total - off;
            if (residual > 0 && off > 0) std::memmove(rx_buffer, rx_buffer + off, residual);
        } else if (n == 0) {
            std::cerr << "peer closed\n";
            break;
        }
    }

    close(sock);

    uint64_t elapsed_s = (hft::rdtscp_ns() - start) / 1'000'000'000ULL;
    std::cerr << "[probe] sent=" << total_sent << " acked=" << total_acked
              << " elapsed_s=" << elapsed_s << "\n";

    printf("\n=================================================================\n");
    printf(" CO_CORRECTION_PROBE — plain hdr_record_value vs hdr_record_corrected_value\n");
    printf(" (both fed the SAME per-order co_latency = ack - intended_ts)\n");
    printf("=================================================================\n");
    printf("%-10s %18s %18s\n", "pct", "plain (no backfill)", "corrected (backfill)");
    double pcts[] = {50, 90, 99, 99.9, 99.99};
    for (double p : pcts) {
        printf("%-10.2f %18lld %18lld\n", p,
               (long long)hdr_value_at_percentile(plain_hist, p),
               (long long)hdr_value_at_percentile(corrected_hist, p));
    }
    printf("%-10s %18lld %18lld\n", "max",
           (long long)hdr_max(plain_hist), (long long)hdr_max(corrected_hist));
    printf("total_count %14lld %18lld\n",
           (long long)plain_hist->total_count, (long long)corrected_hist->total_count);
    printf("phantom_backfilled: %lld\n",
           (long long)(corrected_hist->total_count - plain_hist->total_count));
    return 0;
}
