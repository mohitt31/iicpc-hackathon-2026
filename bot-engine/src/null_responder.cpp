// null_responder.cpp — Test responder for the bot.
// Accepts multiple concurrent TCP connections (one per bot thread).
// Echoes OrderAck for every NewOrder received.
//
// Stall modes:
//   --stall-mode               enable stalls
//   --stall-every N            inject stall every Nth order (default 5000)
//   --stall-ms M               stall duration in milliseconds (default 5)

#include <iostream>
#include <cstdint>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cerrno>
#include <thread>
#include <chrono>
#include <string>
#include <atomic>
#include <vector>

#include "contracts/interface_contract_v1.h"
#include "tsc_util.h"

// Shared state across connection-handler threads
struct ResponderConfig {
    bool     stall_mode  = false;
    uint64_t stall_every = 5000;
    uint64_t stall_ms    = 5;
};

// Each client connection is handled in its own thread.
// monotonic_seq is per-connection — bots only see their own ack stream.
static void handle_client(int client_socket, uint32_t client_id,
                          const ResponderConfig& cfg) {
    std::cout << "[Responder] Client " << client_id << " connected.\n";

    int opt = 1;
    setsockopt(client_socket, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
#ifdef SO_QUICKACK
    setsockopt(client_socket, SOL_SOCKET, SO_QUICKACK, &opt, sizeof(opt));
#endif
#ifdef SO_BUSY_POLL
    int busy_poll_us = 50;
    setsockopt(client_socket, SOL_SOCKET, SO_BUSY_POLL, &busy_poll_us, sizeof(busy_poll_us));
#endif

    uint64_t monotonic_seq = 0;
    uint64_t orders_seen   = 0;

    constexpr size_t MAX_PAYLOAD = 64; // largest msg payload (Fill=56B)
   alignas(64) uint8_t rx_hdr_buf[sizeof(FrameHeader)];
    alignas(64) uint8_t rx_buffer[sizeof(FrameHeader) + MAX_PAYLOAD];
    alignas(64) uint8_t tx_buffer[sizeof(FrameHeader) + sizeof(OrderAck)];

    auto* tx_hdr     = reinterpret_cast<FrameHeader*>(tx_buffer);
    tx_hdr->msg_type = MSG_ORDERACK;
    tx_hdr->_pad     = 0;
    tx_hdr->msg_len  = sizeof(OrderAck);
    auto* tx_ack     = reinterpret_cast<OrderAck*>(tx_buffer + sizeof(FrameHeader));

    while (true) {
        // Two-step framing: read header, then exactly msg_len bytes (handles any type)
        ssize_t hr = recv(client_socket, rx_hdr_buf, sizeof(FrameHeader), MSG_WAITALL);
        if (__builtin_expect(hr != static_cast<ssize_t>(sizeof(FrameHeader)), 0)) {
            // 0 = clean close, <0 = error, 0<hr<4 = peer died mid-header.
            // Anything but a complete header means this stream is done.
            if (hr == 0) std::cout << "[Responder] Client " << client_id
                << " disconnected. Served " << orders_seen << " orders.\n";
            break;
        }
        const FrameHeader* rx_hdr_peek = reinterpret_cast<const FrameHeader*>(rx_hdr_buf);
        if (rx_hdr_peek->msg_len > MAX_PAYLOAD) break; // safety: unknown oversized message
        std::memcpy(rx_buffer, rx_hdr_buf, sizeof(FrameHeader));
        ssize_t bytes_read = recv(client_socket, rx_buffer + sizeof(FrameHeader),
                                  rx_hdr_peek->msg_len, MSG_WAITALL);
        if (__builtin_expect(bytes_read < 0 || (bytes_read == 0 && rx_hdr_peek->msg_len > 0), 0)) {
            break;
        }

        // recv with MSG_WAITALL returns:
        //   == EXPECTED_BYTES: success
        //   == 0: peer closed cleanly
        //   < 0: error (EINTR, ECONNRESET, etc)
        //   0 < n < EXPECTED_BYTES: peer closed mid-frame (partial)
        // We treat anything other than full-frame as disconnect.
        // (error handling already done in two-step recv above)

        const auto* rx_hdr = reinterpret_cast<const FrameHeader*>(rx_buffer);

        // null_responder is intentionally minimal: it only acks NewOrder.
        // Receiving any other message type (CancelOrder, etc) means the
        // contract changed or the bot sent something unexpected — we
        // silently skip rather than reinterpret garbage as NewOrder.
        if (__builtin_expect(rx_hdr->msg_type != MSG_NEWORDER, 0)) {
            // Drop the frame and continue (we already read full bytes)
            continue;
        }

        if (__builtin_expect(rx_hdr->msg_type == MSG_NEWORDER, 1)) {
            auto* rx_order = reinterpret_cast<const NewOrder*>(
                rx_buffer + sizeof(FrameHeader));
            orders_seen++;

            // Deterministic stall — every Nth order, sleep M ms
            if (__builtin_expect(cfg.stall_mode, 0)) {
                if (__builtin_expect(orders_seen % cfg.stall_every == 0, 0)) {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(cfg.stall_ms));
                }
            }

            tx_ack->seq          = ++monotonic_seq;
            tx_ack->timestamp_ns = hft::rdtscp_ns();
            tx_ack->symbol_id    = rx_order->symbol_id;
            tx_ack->order_seq    = rx_order->seq;

            // Blocking send on a 36-byte ack either completes in full or
            // fails — a short/failed send means the peer is gone.
            ssize_t sw = send(client_socket, tx_buffer, sizeof(tx_buffer),
                              MSG_NOSIGNAL);
            if (__builtin_expect(sw != static_cast<ssize_t>(sizeof(tx_buffer)), 0))
                break;

#ifdef SO_QUICKACK
            setsockopt(client_socket, SOL_SOCKET, SO_QUICKACK, &opt, sizeof(opt));
#endif
        }
    }

    close(client_socket);
}

int main(int argc, char* argv[]) {
    ResponderConfig cfg;
    uint16_t port = 9000;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (a == "--stall-mode")                   cfg.stall_mode  = true;
        else if (a == "--stall-every" && i + 1 < argc)  cfg.stall_every = std::stoull(argv[++i]);
        else if (a == "--stall-ms"    && i + 1 < argc)  cfg.stall_ms    = std::stoull(argv[++i]);
        else if (a == "--port"        && i + 1 < argc)  port            = static_cast<uint16_t>(std::stoi(argv[++i]));
    }

    hft::calibrate_tsc();
    std::cout << "[Responder] TSC calibrated.\n";
    if (cfg.stall_mode) {
        std::cout << "[Responder] DETERMINISTIC STALL MODE: every "
                  << cfg.stall_every << " orders, stall "
                  << cfg.stall_ms << " ms.\n";
    }

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { std::cerr << "socket() failed\n"; return 1; }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in address{};
    address.sin_family      = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port        = htons(port);

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        std::cerr << "bind() failed\n"; return 1;
    }
    // Backlog 32 — handles up to 32 simultaneously pending connections
    if (listen(server_fd, 32) < 0) {
        std::cerr << "listen() failed\n"; return 1;
    }

    std::cout << "[Responder] Listening on port " << port
              << ", accepting multiple connections...\n";

    std::vector<std::thread> client_threads;
    std::atomic<uint32_t> next_client_id{0};

    // Accept loop — spawn a handler thread per connection
    while (true) {
        int client_socket = accept(server_fd, nullptr, nullptr);
        if (client_socket < 0) {
            if (errno == EINTR) continue;
            std::cerr << "accept() failed: " << strerror(errno) << "\n";
            break;
        }
        uint32_t id = next_client_id.fetch_add(1);
        client_threads.emplace_back(handle_client, client_socket, id, std::cref(cfg));
        client_threads.back().detach(); // fire-and-forget; OS cleans up on exit
    }

    close(server_fd);
    return 0;
}
