// closed_loop_probe.cpp — diagnostic tool, NOT part of the judged demo path.
//
// Companion to co_correction_probe.cpp and docs/COORDINATED_OMISSION_WRITEUP.md.
// bot.cpp is open-loop by construction (§2/§3 of the write-up): it never lets
// request generation depend on prior responses. This tool is the deliberate
// opposite — a textbook CLOSED-LOOP generator, exactly the thing Gil Tene's
// "How NOT to Measure Latency" warns against: send one order, block until its
// ack arrives, only then send the next. One request in flight at a time.
//
// Point of the comparison: against the IDENTICAL injected fault (null_responder
// --stall-mode --stall-every 20000 --stall-ms 5), a closed-loop generator
// only ever has ONE request in flight when the stall hits, so only ONE sample
// out of the whole run captures each 5ms stall — versus ~50 for the open-loop
// generator at 100us cadence. Over a fixed duration, a closed loop with a
// ~microsecond round trip also completes vastly more requests than a
// 100us-paced open loop, which dilutes the stall-affected fraction even
// further. The result: closed-loop's own raw percentiles can stay completely
// blind to a real, repeatedly-injected fault, even out to p99.99 — which is
// the entire reason Tene's talk exists. This tool measures that directly
// instead of asserting it.
//
// Usage:
//   ./null_responder --stall-mode --stall-every 20000 --stall-ms 5 --port 9502
//   ./closed_loop_probe --duration-sec 60 --port 9502

#include <arpa/inet.h>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <hdr/hdr_histogram.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

#include "../../contracts/interface_contract_v1.h"
#include "tsc_util.h"

int main(int argc, char* argv[]) {
    uint64_t duration_sec = 60;
    uint16_t port = 9000;
    std::string ip = "127.0.0.1";

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--duration-sec" && i + 1 < argc) duration_sec = std::stoull(argv[++i]);
        else if (a == "--port" && i + 1 < argc) port = static_cast<uint16_t>(std::stoi(argv[++i]));
        else if (a == "--ip" && i + 1 < argc) ip = argv[++i];
    }

    hft::calibrate_tsc();

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { fprintf(stderr, "socket() failed\n"); return 1; }
    int opt = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);
    if (connect(sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "connect() failed: %s\n", strerror(errno));
        return 1;
    }
    // Deliberately BLOCKING socket — this generator waits for the ack before
    // it does anything else. That wait is the entire point.

    struct hdr_histogram* hist = nullptr;
    hdr_init(1, 10'000'000'000LL, 3, &hist);

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

    uint8_t rx_hdr_buf[sizeof(FrameHeader)];
    uint8_t rx_payload_buf[sizeof(OrderAck)];

    const uint64_t start = hft::rdtscp_ns();
    const uint64_t end_time = start + duration_sec * 1'000'000'000ULL;
    uint64_t seq = 0;
    uint64_t total_round_trips = 0;

    while (hft::rdtscp_ns() < end_time) {
        ++seq;
        tx_order->seq = seq;

        uint64_t send_time = hft::rdtscp_ns();
        tx_order->timestamp_ns = send_time;

        ssize_t sent = send(sock, tx_buffer, sizeof(tx_buffer), 0);
        if (sent != static_cast<ssize_t>(sizeof(tx_buffer))) {
            fprintf(stderr, "send() failed/partial: %s\n", strerror(errno));
            break;
        }

        // Blocking, exact-size reads: this IS the closed loop — nothing else
        // happens on this connection until the full ack is in hand.
        ssize_t hr = recv(sock, rx_hdr_buf, sizeof(rx_hdr_buf), MSG_WAITALL);
        if (hr != static_cast<ssize_t>(sizeof(rx_hdr_buf))) {
            fprintf(stderr, "header recv failed/closed: %s\n", strerror(errno));
            break;
        }
        auto* hdr = reinterpret_cast<FrameHeader*>(rx_hdr_buf);
        ssize_t pr = recv(sock, rx_payload_buf, hdr->msg_len, MSG_WAITALL);
        uint64_t ack_time = hft::rdtscp_ns();
        if (pr != static_cast<ssize_t>(hdr->msg_len)) {
            fprintf(stderr, "payload recv failed/closed: %s\n", strerror(errno));
            break;
        }

        if (hdr->msg_type == MSG_ORDERACK) {
            int64_t latency = static_cast<int64_t>(ack_time - send_time);
            if (latency > 0) hdr_record_value(hist, latency);
            ++total_round_trips;
        }
    }

    close(sock);

    fprintf(stderr, "[closed_loop_probe] round_trips=%llu elapsed_s=%llu\n",
            (unsigned long long)total_round_trips,
            (unsigned long long)((hft::rdtscp_ns() - start) / 1'000'000'000ULL));

    printf("\n=================================================================\n");
    printf(" CLOSED_LOOP_PROBE — one request in flight at a time (Tene's textbook case)\n");
    printf(" Same null_responder, same --stall-mode --stall-every 20000 --stall-ms 5\n");
    printf("=================================================================\n");
    printf("total round trips: %llu\n", (unsigned long long)total_round_trips);
    double pcts[] = {50, 90, 99, 99.9, 99.99, 99.999};
    for (double p : pcts) {
        printf("p%-10.3f %15lld ns\n", p, (long long)hdr_value_at_percentile(hist, p));
    }
    printf("max        %15lld ns\n", (long long)hdr_max(hist));
    return 0;
}
