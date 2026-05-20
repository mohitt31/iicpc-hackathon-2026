// reference_server.cpp — TCP-serving reference matching engine
//
// Wraps OrderBook (from order_book.h) in a TCP server so the bot
// can send NewOrder / CancelOrder messages over the network and
// receive real OrderAck / Fill / Reject responses.
//
// This is the "real engine" that replaces null_responder for
// Day 4-6 integration. The matching logic is identical to
// refengine replay — same header, same code path.
//
// Differences from null_responder:
//   - Routes through the real OrderBook (price-time matching)
//   - Sends back OrderAck, Fill, AND Reject messages
//   - Journals all output to a file for offline diff verification
//
// Usage:
//   reference_server [--port 9000] [--journal output.jrn] [--symbol-id 1]
//
// Build (from bot-engine/):
//   g++ -std=c++20 -O2 -Wall -Wextra -I.. -Isrc src/reference_server.cpp -o build/refserver -lpthread

#include "order_book.h"
#include "tsc_util.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <cstring>
#include <string>
#include <vector>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cerrno>
#include <csignal>
#include <thread>
#include <chrono>

static volatile sig_atomic_t g_running = 1;

static void signal_handler(int) { g_running = 0; }

// ── Stats ──────────────────────────────────────────────────────
struct ServerStats {
    uint64_t new_orders   = 0;
    uint64_t cancels      = 0;
    uint64_t acks_sent    = 0;
    uint64_t fills_sent   = 0;
    uint64_t rejects_sent = 0;
    uint64_t bytes_rx     = 0;
    uint64_t bytes_tx     = 0;
    uint64_t unknown_msgs = 0;
};

// Count outbound messages in a response buffer by parsing the framed stream.
static void count_responses(const uint8_t* buf, size_t len, ServerStats& stats) {
    size_t offset = 0;
    while (offset + sizeof(FrameHeader) <= len) {
        const auto* h = reinterpret_cast<const FrameHeader*>(buf + offset);
        if (offset + sizeof(FrameHeader) + h->msg_len > len) break;
        switch (h->msg_type) {
            case MSG_ORDERACK: stats.acks_sent++;    break;
            case MSG_FILL:     stats.fills_sent++;   break;
            case MSG_REJECT:   stats.rejects_sent++; break;
            default: break;
        }
        offset += sizeof(FrameHeader) + h->msg_len;
    }
}

// ── Send all bytes, handling partial sends ─────────────────────
static bool send_all(int fd, const uint8_t* buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, buf + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                // Busy-wait briefly; this is a reference server, not prod.
                continue;
            }
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

int main(int argc, char* argv[]) {
    // ── Parse args ─────────────────────────────────────────────
    uint16_t    port       = 9000;
    std::string journal_path = "server_output.jrn";
    std::string input_journal_path = "server_input.jrn";
    uint32_t    symbol_id  = 1;  // unused currently, book handles any

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--port"      && i + 1 < argc) port       = static_cast<uint16_t>(std::stoi(argv[++i]));
        if (a == "--journal"   && i + 1 < argc) journal_path = argv[++i];
        if (a == "--input-journal" && i + 1 < argc) input_journal_path = argv[++i];
        if (a == "--symbol-id" && i + 1 < argc) symbol_id  = static_cast<uint32_t>(std::stoul(argv[++i]));
    }
    (void)symbol_id;

    // ── Signal handling ────────────────────────────────────────
    struct sigaction sa{};
    sa.sa_handler = signal_handler;
    sigaction(SIGINT,  &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    // ── TSC calibration ────────────────────────────────────────
    hft::calibrate_tsc();
    std::cout << "[RefServer] TSC calibrated.\n";
    std::cout << "[RefServer] Port: " << port
              << " | Output journal: " << journal_path
              << " | Input journal: " << input_journal_path << "\n";

    // ── Journal files ──────────────────────────────────────────
    std::ofstream journal(journal_path, std::ios::binary);
    if (!journal) {
        std::cerr << "[RefServer] ERROR: cannot open output journal: " << journal_path << "\n";
        return 1;
    }
    std::ofstream input_journal(input_journal_path, std::ios::binary);
    if (!input_journal) {
        std::cerr << "[RefServer] ERROR: cannot open input journal: " << input_journal_path << "\n";
        return 1;
    }

    // ── Socket setup ───────────────────────────────────────────
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { std::cerr << "[RefServer] socket() failed\n"; return 1; }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in address{};
    address.sin_family      = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port        = htons(port);

    if (bind(server_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        std::cerr << "[RefServer] bind() failed on port " << port << "\n";
        close(server_fd);
        return 1;
    }
    if (listen(server_fd, 1) < 0) {
        std::cerr << "[RefServer] listen() failed\n";
        close(server_fd);
        return 1;
    }

    std::cout << "[RefServer] Listening on port " << port << "...\n";

    int client_socket = accept(server_fd, nullptr, nullptr);
    if (client_socket < 0) {
        std::cerr << "[RefServer] accept() failed\n";
        close(server_fd);
        return 1;
    }
    std::cout << "[RefServer] Client connected.\n";

    // TCP tuning — same as null_responder
    setsockopt(client_socket, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
#ifdef SO_QUICKACK
    setsockopt(client_socket, SOL_SOCKET, SO_QUICKACK, &opt, sizeof(opt));
#endif

    // ── OrderBook + processing loop ────────────────────────────
    OrderBook book;
    ServerStats stats;

    // rx_buffer for incoming messages; large enough for batching
    constexpr size_t RX_BUF_SIZE = 65536;
    alignas(64) uint8_t rx_buffer[RX_BUF_SIZE];
    size_t residual_len = 0;

    // We use a stringstream to capture OrderBook output, then
    // send it over TCP and write to journal. This keeps the
    // OrderBook interface unchanged from the journal-replay path.
    std::ostringstream response_buf;

    uint64_t last_report_time = hft::rdtscp_ns();

    while (g_running) {
        // ── Receive ────────────────────────────────────────────
        ssize_t bytes_read = recv(client_socket,
                                  rx_buffer + residual_len,
                                  RX_BUF_SIZE - residual_len,
                                  0);  // blocking recv

        if (bytes_read <= 0) {
            if (bytes_read == 0) {
                std::cout << "[RefServer] Client disconnected.\n";
            } else if (errno == EINTR) {
                continue;
            } else {
                std::cerr << "[RefServer] recv() error: " << strerror(errno) << "\n";
            }
            break;
        }

        stats.bytes_rx += static_cast<uint64_t>(bytes_read);
        size_t available = residual_len + static_cast<size_t>(bytes_read);
        size_t offset = 0;

        // ── Process all complete messages ──────────────────────
        while (offset + sizeof(FrameHeader) <= available) {
            const auto* hdr = reinterpret_cast<const FrameHeader*>(rx_buffer + offset);

            // Check if full payload is available
            size_t frame_size = sizeof(FrameHeader) + hdr->msg_len;
            if (offset + frame_size > available) break;

            // Clear the response buffer for this message
            response_buf.str(std::string());
            response_buf.clear();

            switch (hdr->msg_type) {
                case MSG_NEWORDER: {
                    if (hdr->msg_len < sizeof(NewOrder)) break;
                    NewOrder o;
                    std::memcpy(&o, rx_buffer + offset + sizeof(FrameHeader), sizeof(o));

                    // Journal the inbound message
                    input_journal.write(reinterpret_cast<const char*>(rx_buffer + offset), frame_size);

                    book.on_new_order(o, response_buf);
                    stats.new_orders++;
                    break;
                }
                case MSG_CANCEL: {
                    if (hdr->msg_len < sizeof(CancelOrder)) break;
                    CancelOrder c;
                    std::memcpy(&c, rx_buffer + offset + sizeof(FrameHeader), sizeof(c));

                    // Journal the inbound message
                    input_journal.write(reinterpret_cast<const char*>(rx_buffer + offset), frame_size);

                    book.on_cancel(c, response_buf);
                    stats.cancels++;
                    break;
                }
                default: {
                    stats.unknown_msgs++;
                    break;
                }
            }

            // ── Send response over TCP + journal ───────────────
            const std::string& resp = response_buf.str();
            if (!resp.empty()) {
                const auto* resp_data = reinterpret_cast<const uint8_t*>(resp.data());
                size_t resp_len = resp.size();

                // Count outbound message types for stats
                count_responses(resp_data, resp_len, stats);

                // Send to client
                if (!send_all(client_socket, resp_data, resp_len)) {
                    std::cerr << "[RefServer] send() failed, client gone?\n";
                    g_running = 0;
                    break;
                }
                stats.bytes_tx += resp_len;

                // Write to journal (for offline diff)
                journal.write(resp.data(), static_cast<std::streamsize>(resp_len));
            }

#ifdef SO_QUICKACK
            setsockopt(client_socket, SOL_SOCKET, SO_QUICKACK, &opt, sizeof(opt));
#endif

            offset += frame_size;
        }

        // ── Preserve residual partial message ──────────────────
        residual_len = available - offset;
        if (residual_len > 0 && offset > 0) {
            std::memmove(rx_buffer, rx_buffer + offset, residual_len);
        }

        // ── Periodic report (every 2s) ─────────────────────────
        uint64_t now = hft::rdtscp_ns();
        if (now - last_report_time >= 2'000'000'000ULL) {
            std::cout << "[RefServer] Orders: " << stats.new_orders
                      << " | Cancels: " << stats.cancels
                      << " | Acks: " << stats.acks_sent
                      << " | Fills: " << stats.fills_sent
                      << " | Rejects: " << stats.rejects_sent
                      << " | Unknown: " << stats.unknown_msgs
                      << "\n";
            last_report_time = now;
        }
    }

    // ── Final report ───────────────────────────────────────────
    journal.flush();
    input_journal.flush();

    std::cout << "\n[RefServer] === FINAL STATS ===\n"
              << "  NewOrders received: " << stats.new_orders << "\n"
              << "  Cancels received:   " << stats.cancels << "\n"
              << "  OrderAcks sent:     " << stats.acks_sent << "\n"
              << "  Fills sent:         " << stats.fills_sent << "\n"
              << "  Rejects sent:       " << stats.rejects_sent << "\n"
              << "  Unknown msgs:       " << stats.unknown_msgs << "\n"
              << "  Bytes RX:           " << stats.bytes_rx << "\n"
              << "  Bytes TX:           " << stats.bytes_tx << "\n"
              << "  Output journal:     " << journal_path << "\n"
              << "  Input journal:      " << input_journal_path << "\n";

    close(client_socket);
    close(server_fd);
    return 0;
}
