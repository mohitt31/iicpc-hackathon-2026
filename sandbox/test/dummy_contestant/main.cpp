// dummy_contestant main.cpp — A trivial valid contestant submission
// for testing the sandbox pipeline. Implements a no-op matching engine
// that listens on TCP port 9000 and ACKs everything.
//
// Requirements per doc §7:
//   - Must support --version flag
//   - Must be statically linkable
//   - Must not use banned syscalls

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <netinet/tcp.h>

// Inline the contract structs (contestant wouldn't have our header)
#pragma pack(push, 1)
struct FrameHeader {
    uint8_t  msg_type;
    uint8_t  _pad;
    uint16_t msg_len;
};

struct NewOrder {
    uint64_t seq;
    uint64_t timestamp_ns;
    uint32_t symbol_id;
    uint8_t  order_type;
    uint8_t  side;
    uint8_t  _pad[2];
    int64_t  price;
    uint64_t quantity;
};

struct OrderAck {
    uint64_t seq;
    uint64_t timestamp_ns;
    uint32_t symbol_id;
    uint8_t  _pad[4];
    uint64_t order_seq;
};
#pragma pack(pop)

#define CONTESTANT_VERSION "dummy-contestant-tcp-v1.0.0"

int main(int argc, char* argv[]) {
    // §7 stage 3: must expose /version
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--version") {
            printf("%s\n", CONTESTANT_VERSION);
            return 0;
        }
    }

    fprintf(stderr, "[dummy] Starting dummy TCP contestant engine on port 9000\n");

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return 1;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(9000);

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        perror("bind");
        return 1;
    }

    if (listen(server_fd, 1) < 0) {
        perror("listen");
        return 1;
    }

    while (true) {
        int client = accept(server_fd, nullptr, nullptr);
        if (client < 0) continue;
        
        fprintf(stderr, "[dummy] Client connected\n");
        setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

        FrameHeader hdr;
        uint64_t engine_seq = 0;
        
        // Simple synchronous read loop
        while (recv(client, &hdr, sizeof(hdr), MSG_WAITALL) == sizeof(hdr)) {
            uint8_t payload[64];
            if (hdr.msg_len > sizeof(payload)) break;
            if (recv(client, payload, hdr.msg_len, MSG_WAITALL) != hdr.msg_len) break;

            if (hdr.msg_type == 1) {  // MSG_NEWORDER
                NewOrder o;
                memcpy(&o, payload, sizeof(o));

                OrderAck ack{};
                ack.seq = ++engine_seq;
                ack.timestamp_ns = engine_seq;
                ack.symbol_id = o.symbol_id;
                ack.order_seq = o.seq;

                FrameHeader out_hdr{};
                out_hdr.msg_type = 3;  // MSG_ORDERACK
                out_hdr.msg_len = sizeof(OrderAck);

                send(client, &out_hdr, sizeof(out_hdr), MSG_NOSIGNAL);
                send(client, &ack, sizeof(ack), MSG_NOSIGNAL);
            }
        }
        close(client);
    }

    return 0;
}
