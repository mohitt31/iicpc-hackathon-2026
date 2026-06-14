#ifndef EGRESS_BUFFER_HPP
#define EGRESS_BUFFER_HPP

#include <sys/socket.h>
#include <cstring>
#include <cstdint>
#include <stdexcept>

// 1400 bytes prevents IP fragmentation (standard Ethernet MTU is 1500)
constexpr size_t MAX_EGRESS_BUFFER = 1400; 

class EgressBuffer {
private:
    alignas(64) uint8_t buffer[MAX_EGRESS_BUFFER]; // Cache-line aligned
    size_t current_offset;
    int socket_fd;

public:
    explicit EgressBuffer(int fd) : current_offset(0), socket_fd(fd) {}

    // Force inline for zero function-call overhead
    template <typename T>
    __attribute__((always_inline)) inline void push(const T& message) {
        constexpr size_t msg_size = sizeof(T);
        
        // If the next message breaches MTU, flush immediately
        if (__builtin_expect(current_offset + msg_size > MAX_EGRESS_BUFFER, 0)) {
            flush();
        }

        // Compiler will optimize constant-size memcpy to raw MOV instructions
        std::memcpy(buffer + current_offset, &message, msg_size);
        current_offset += msg_size;
    }

    __attribute__((always_inline)) inline void flush() {
        if (__builtin_expect(current_offset == 0, 0)) return;

        // MSG_NOSIGNAL prevents the kernel from throwing SIGPIPE if the client disconnected
        ssize_t sent = send(socket_fd, buffer, current_offset, MSG_NOSIGNAL);
        
        if (__builtin_expect(sent < 0, 0)) {
            // Handle EAGAIN (socket buffer full) - DO NOT crash the engine
            // In a real hackathon Day-2, you trigger the session disconnect here
        }

        current_offset = 0; // Reset buffer
    }
};

#endif // EGRESS_BUFFER_HPP
