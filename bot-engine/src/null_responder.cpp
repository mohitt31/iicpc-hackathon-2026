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
#include <random>

#include "contracts/interface_contract_v1.h"
#include "tsc_util.h"

int main(int argc, char* argv[]) {
    bool stall_mode = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--stall-mode") {
            stall_mode = true;
        }
    }

    hft::calibrate_tsc();
    std::cout << "[Responder] TSC calibrated.\n";
    if (stall_mode) {
        std::cout << "[Responder] STALL MODE ENABLED: Simulating 5ms GC pauses occasionally.\n";
    }

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        std::cerr << "socket() failed\n";
        return 1;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(9000);

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        std::cerr << "bind() failed\n";
        return 1;
    }

    if (listen(server_fd, 1) < 0) {
        std::cerr << "listen() failed\n";
        return 1;
    }

    std::cout << "[Responder] Listening on port 9000...\n";
    
    int client_socket = accept(server_fd, nullptr, nullptr);
    if (client_socket < 0) {
        std::cerr << "accept() failed\n";
        return 1;
    }
    
    std::cout << "[Responder] Client connected. Entering strict busy-poll loop.\n";

    setsockopt(client_socket, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
#ifdef SO_QUICKACK
    setsockopt(client_socket, SOL_SOCKET, SO_QUICKACK, &opt, sizeof(opt));
#endif

    uint64_t monotonic_seq = 0;
    
    constexpr size_t EXPECTED_BYTES = sizeof(FrameHeader) + sizeof(NewOrder);
    alignas(64) uint8_t rx_buffer[EXPECTED_BYTES];
    
    alignas(64) uint8_t tx_buffer[sizeof(FrameHeader) + sizeof(OrderAck)];
    auto* tx_hdr = reinterpret_cast<FrameHeader*>(tx_buffer);
    tx_hdr->msg_type = MSG_ORDERACK;
    tx_hdr->msg_len = sizeof(OrderAck);
    auto* tx_ack = reinterpret_cast<OrderAck*>(tx_buffer + sizeof(FrameHeader));

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(1, 10000); // Trigger stall probability

    while (true) {
        ssize_t bytes_read = recv(client_socket, rx_buffer, EXPECTED_BYTES, MSG_WAITALL);
        
        if (__builtin_expect(bytes_read <= 0, 0)) { // [[unlikely]]
            std::cout << "[Responder] Client disconnected or error.\n";
            break;
        }

        auto* rx_hdr = reinterpret_cast<const FrameHeader*>(rx_buffer);
        if (__builtin_expect(rx_hdr->msg_type == MSG_NEWORDER, 1)) { // [[likely]]
            auto* rx_order = reinterpret_cast<const NewOrder*>(rx_buffer + sizeof(FrameHeader));
            
            // STALL SIMULATION
            if (__builtin_expect(stall_mode, 0)) { // [[unlikely]]
                if (__builtin_expect(dis(gen) == 1, 0)) { // 1 in 10000 chance to stall
                    // Simulate a severe matching engine GC pause / lock contention
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
            }

            tx_ack->seq = ++monotonic_seq;
            tx_ack->timestamp_ns = hft::rdtscp_ns();
            tx_ack->symbol_id = rx_order->symbol_id;
            tx_ack->order_seq = rx_order->seq;
            
            send(client_socket, tx_buffer, sizeof(tx_buffer), 0);
            
#ifdef SO_QUICKACK
            setsockopt(client_socket, SOL_SOCKET, SO_QUICKACK, &opt, sizeof(opt));
#endif
        }
    }

    close(client_socket);
    close(server_fd);
    return 0;
}
