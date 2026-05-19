#include <iostream>
#include <iomanip>
#include <cstdint>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <string>
#include <stdexcept>
#include <vector>

#include "contracts/interface_contract_v1.h"
#include "tsc_util.h"
#include <hdr/hdr_histogram.h>

#ifdef __cpp_lib_hardware_interference_size
using std::hardware_destructive_interference_size;
#else
constexpr std::size_t hardware_destructive_interference_size = 64;
#endif

void set_non_blocking(int fd) {
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
    
    uint64_t interval_ns = interval_us * 1000;
    uint64_t total_duration_ns = duration_sec * 1000000000ULL;

    hft::calibrate_tsc();
    std::cout << "[Bot] TSC calibrated.\n";
    std::cout << "[Bot] Interval: " << interval_us << " us, Duration: " << duration_sec << " s\n";

    // Initialize HdrHistograms
    struct hdr_histogram* naive_hist = nullptr;
    struct hdr_histogram* co_hist = nullptr;
    // Track 1 ns up to 10 seconds with 3 significant figures
    hdr_init(1, 10000000000ULL, 3, &naive_hist);
    hdr_init(1, 10000000000ULL, 3, &co_hist);

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        std::cerr << "socket() failed\n";
        return 1;
    }

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
    set_non_blocking(sock);

    std::cout << "[Bot] Connected. Starting STRICT OPEN-LOOP busy-spin.\n";

    alignas(hardware_destructive_interference_size) uint64_t total_sent = 0;
    alignas(hardware_destructive_interference_size) uint64_t total_acked = 0;
    
    // Zero-allocation pending map: seq & 1048575 gives index
    constexpr size_t PENDING_MASK = 1048575;
    std::vector<uint64_t> pending_send_ts(PENDING_MASK + 1, 0);

    alignas(hardware_destructive_interference_size) uint8_t tx_buffer[sizeof(FrameHeader) + sizeof(NewOrder)];
    auto* tx_hdr = reinterpret_cast<FrameHeader*>(tx_buffer);
    tx_hdr->msg_type = MSG_NEWORDER;
    tx_hdr->msg_len = sizeof(NewOrder);
    auto* tx_order = reinterpret_cast<NewOrder*>(tx_buffer + sizeof(FrameHeader));
    tx_order->symbol_id = 1;
    tx_order->order_type = ORDER_LIMIT;
    tx_order->side = SIDE_BUY;
    tx_order->price = 15050;
    tx_order->quantity = 100;

    alignas(hardware_destructive_interference_size) uint8_t rx_buffer[8192];

    uint64_t start_time = hft::rdtscp_ns();
    uint64_t next_send_time = start_time;
    uint64_t last_report_time = start_time;
    uint64_t end_time = start_time + total_duration_ns;
    
    uint64_t sent_since_last_report = 0;

    while (true) {
        uint64_t now = hft::rdtscp_ns();
        
        if (__builtin_expect(now >= end_time, 0)) { // [[unlikely]]
            break;
        }

        if (__builtin_expect(now - last_report_time >= 1000000000ULL, 0)) { // [[unlikely]]
            std::cout << "[Report] Sent: " << total_sent 
                      << " | Acked: " << total_acked 
                      << " | Rate: " << sent_since_last_report << " msgs/sec\n";
            sent_since_last_report = 0;
            last_report_time += 1000000000ULL;
        }

        // Send Path
        if (__builtin_expect(now >= next_send_time, 0)) { // [[unlikely]]
            tx_order->seq = ++total_sent;
            tx_order->timestamp_ns = now;
            
            pending_send_ts[tx_order->seq & PENDING_MASK] = now;
            
            send(sock, tx_buffer, sizeof(tx_buffer), MSG_DONTWAIT);
            
            sent_since_last_report++;
            next_send_time += interval_ns;
        }

        // Receive Path
        ssize_t bytes_read = recv(sock, rx_buffer, sizeof(rx_buffer), MSG_DONTWAIT);
        if (__builtin_expect(bytes_read > 0, 0)) { // [[unlikely]]
            size_t offset = 0;
            while (offset + sizeof(FrameHeader) <= static_cast<size_t>(bytes_read)) {
                auto* rx_hdr = reinterpret_cast<FrameHeader*>(rx_buffer + offset);
                if (rx_hdr->msg_type == MSG_ORDERACK) {
                    auto* rx_ack = reinterpret_cast<OrderAck*>(rx_buffer + offset + sizeof(FrameHeader));
                    uint64_t ack_now = hft::rdtscp_ns();
                    
                    uint64_t original_ts = pending_send_ts[rx_ack->order_seq & PENDING_MASK];
                    if (original_ts > 0) {
                        uint64_t latency = ack_now - original_ts;
                        
                        // Record latencies
                        hdr_record_value(naive_hist, latency);
                        hdr_record_corrected_value(co_hist, latency, interval_ns);
                    }
                    total_acked++;
                }
                offset += sizeof(FrameHeader) + rx_hdr->msg_len;
            }
        }
    }

    std::cout << "\n[Bot] Run complete. Total Sent: " << total_sent << " | Total Acked: " << total_acked << "\n\n";

    std::cout << "=========================================================\n";
    std::cout << "             ROUND-TRIP LATENCY (Nanoseconds)            \n";
    std::cout << "=========================================================\n";
    std::cout << std::left << std::setw(15) << "Percentile" 
              << std::setw(20) << "Naive Measurement" 
              << std::setw(20) << "CO-Corrected" << "\n";
    std::cout << "---------------------------------------------------------\n";
    
    double percentiles[] = {50.0, 90.0, 99.0, 99.9, 99.99, 100.0};
    const char* names[] = {"p50", "p90", "p99", "p99.9", "p99.99", "Max"};

    for (int i = 0; i < 6; i++) {
        uint64_t n_val = hdr_value_at_percentile(naive_hist, percentiles[i]);
        uint64_t c_val = hdr_value_at_percentile(co_hist, percentiles[i]);
        std::cout << std::left << std::setw(15) << names[i]
                  << std::setw(20) << n_val 
                  << std::setw(20) << c_val << "\n";
    }
    std::cout << "=========================================================\n";

    hdr_close(naive_hist);
    hdr_close(co_hist);
    close(sock);
    return 0;
}
