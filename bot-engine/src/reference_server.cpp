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
#include <atomic>
#include <mutex>
#include <poll.h>
#include <fcntl.h>

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
    if (listen(server_fd, 32) < 0) {
        std::cerr << "[RefServer] listen() failed\n";
        close(server_fd);
        return 1;
    }

    std::cout << "[RefServer] Listening on port " << port << "...\n";


    // Multi-client accept loop — one thread per connection (H3 fix)
    std::vector<std::thread> client_threads;
    std::atomic<uint32_t> next_client_id{0};
    // The journal ofstreams are shared by every client thread; ofstream
    // is not thread-safe and unsynchronized writes interleave frames,
    // corrupting the journal for the byte-exact diff. One mutex, taken
    // only on the journal write (cold path), keeps frames atomic.
    std::mutex journal_mtx;
    std::cout << "[RefServer] Accepting connections on port " << port << "...\n";

    int flags = fcntl(server_fd, F_GETFL, 0);
    fcntl(server_fd, F_SETFL, flags | O_NONBLOCK);

    while (g_running) {
        struct pollfd pfd{};
        pfd.fd = server_fd;
        pfd.events = POLLIN;
        int pret = poll(&pfd, 1, 100);
        if (pret <= 0) {
            if (pret < 0 && errno != EINTR) break;
            continue;
        }

        while (g_running) {
            int client_socket = accept(server_fd, nullptr, nullptr);
            if (client_socket < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) break;
                if (!g_running) break;
                std::cerr << "[RefServer] accept() failed: " << strerror(errno) << "\n";
                goto accept_fatal;
            }
            int cflags = fcntl(client_socket, F_GETFL, 0);
            fcntl(client_socket, F_SETFL, cflags & ~O_NONBLOCK);
            uint32_t cid = next_client_id.fetch_add(1);
            std::cout << "[RefServer] Client " << cid << " connected.\n";
            client_threads.emplace_back([client_socket, cid, &journal, &input_journal,
                                         &journal_mtx]() {
            int opt2 = 1;
            setsockopt(client_socket, IPPROTO_TCP, TCP_NODELAY, &opt2, sizeof(opt2));
#ifdef SO_QUICKACK
            setsockopt(client_socket, SOL_SOCKET, SO_QUICKACK, &opt2, sizeof(opt2));
#endif
            OrderBook book;
            ServerStats stats;
            constexpr size_t RX_BUF_SIZE = 65536;
            alignas(64) uint8_t rx_buffer[RX_BUF_SIZE];
            size_t residual_len = 0;
            std::ostringstream response_buf;

            while (true) {
                ssize_t bytes_read = recv(client_socket, rx_buffer + residual_len,
                                         RX_BUF_SIZE - residual_len, 0);
                if (bytes_read <= 0) {
                    if (bytes_read == 0) std::cout << "[RefServer] Client " << cid << " disconnected.\n";
                    else std::cerr << "[RefServer] Client " << cid << " recv error: " << strerror(errno) << ".\n";
                    break;
                }
                stats.bytes_rx += static_cast<uint64_t>(bytes_read);
                size_t available = residual_len + static_cast<size_t>(bytes_read);
                size_t offset = 0;
                while (offset + sizeof(FrameHeader) <= available) {
                    const auto* hdr = reinterpret_cast<const FrameHeader*>(rx_buffer + offset);
                    size_t frame_size = sizeof(FrameHeader) + hdr->msg_len;
                    if (offset + frame_size > available) break;
                    response_buf.str(std::string()); response_buf.clear();
                    switch (hdr->msg_type) {
                        case MSG_NEWORDER: {
                            if (hdr->msg_len < sizeof(NewOrder)) break;
                            NewOrder o; std::memcpy(&o, rx_buffer + offset + sizeof(FrameHeader), sizeof(o));
                            {
                                std::lock_guard<std::mutex> lk(journal_mtx);
                                input_journal.write(reinterpret_cast<const char*>(rx_buffer + offset), frame_size);
                            }
                            book.on_new_order(o, response_buf); stats.new_orders++; break;
                        }
                        case MSG_CANCEL: {
                            if (hdr->msg_len < sizeof(CancelOrder)) break;
                            CancelOrder c; std::memcpy(&c, rx_buffer + offset + sizeof(FrameHeader), sizeof(c));
                            {
                                std::lock_guard<std::mutex> lk(journal_mtx);
                                input_journal.write(reinterpret_cast<const char*>(rx_buffer + offset), frame_size);
                            }
                            book.on_cancel(c, response_buf); stats.cancels++; break;
                        }
                        default: stats.unknown_msgs++; break;
                    }
                    const std::string& resp = response_buf.str();
                    if (!resp.empty()) {
                        const auto* rd = reinterpret_cast<const uint8_t*>(resp.data());
                        count_responses(rd, resp.size(), stats);
                        send_all(client_socket, rd, resp.size());
                        stats.bytes_tx += resp.size();
                        {
                            std::lock_guard<std::mutex> lk(journal_mtx);
                            journal.write(resp.data(), static_cast<std::streamsize>(resp.size()));
                        }
                    }
                    offset += frame_size;
                }
                residual_len = available - offset;
                if (residual_len > 0 && offset > 0)
                    std::memmove(rx_buffer, rx_buffer + offset, residual_len);
            }
            close(client_socket);
            std::cout << "[RefServer] Client " << cid << " stats: orders=" << stats.new_orders
                      << " acks=" << stats.acks_sent << " fills=" << stats.fills_sent << "\n";
        });
        client_threads.back().detach();
        }
    }
accept_fatal:

    journal.flush(); input_journal.flush();
    for (auto& t : client_threads) if (t.joinable()) t.join();
    close(server_fd);
    return 0;
}