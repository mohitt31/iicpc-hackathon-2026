#ifndef SPSC_RING_HPP
#define SPSC_RING_HPP

#include <atomic>
#include <cstdint>
#include <new> // For std::hardware_destructive_interference_size

// If C++17 destructive interference size isn't available, default to 128 bytes 
// to defeat aggressive spatial hardware prefetchers on Intel architectures.
#ifdef __cpp_lib_hardware_interference_size
    constexpr size_t CACHE_LINE_SIZE = std::hardware_destructive_interference_size;
#else
    constexpr size_t CACHE_LINE_SIZE = 128; 
#endif

template <typename T, size_t Capacity>
class SPSCQueue {
private:
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of 2 for bitwise masking");
    static constexpr size_t Mask = Capacity - 1;

    // ---------------------------------------------------------
    // PRODUCER STATE
    // ---------------------------------------------------------
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> write_index{0};
    size_t cached_read_index{0}; // Thread-local cache of the consumer's position

    // ---------------------------------------------------------
    // CONSUMER STATE
    // ---------------------------------------------------------
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> read_index{0};
    size_t cached_write_index{0}; // Thread-local cache of the producer's position

    // ---------------------------------------------------------
    // PAYLOAD DATA
    // ---------------------------------------------------------
    alignas(CACHE_LINE_SIZE) T ring[Capacity];

public:
    SPSCQueue() = default;

    // Called ONLY by the Producer (Network Thread)
    bool push(const T& item) {
        const size_t current_write = write_index.load(std::memory_order_relaxed);
        size_t next_write = current_write + 1;

        // Check against the cached read index first (zero cache-line bouncing)
        if (next_write - cached_read_index > Capacity) {
            // Only pay the MESI cross-core penalty if we actually think we are full
            cached_read_index = read_index.load(std::memory_order_acquire);
            if (next_write - cached_read_index > Capacity) {
                return false; // Actually full, return EAGAIN to trigger TCP Window 0
            }
        }

        ring[current_write & Mask] = item;
        
        // Publish the write to the consumer
        write_index.store(next_write, std::memory_order_release);
        return true;
    }

    // Called ONLY by the Consumer (Matching Engine Thread)
    bool pop(T& out_item) {
        const size_t current_read = read_index.load(std::memory_order_relaxed);

        // Check against the cached write index first
        if (current_read == cached_write_index) {
            // Only fetch across the CPU ring bus if we think the queue is empty
            cached_write_index = write_index.load(std::memory_order_acquire);
            if (current_read == cached_write_index) {
                return false; // Queue empty
            }
        }

        out_item = ring[current_read & Mask];

        // Acknowledge the read to the producer
        read_index.store(current_read + 1, std::memory_order_release);
        return true;
    }
};

#endif // SPSC_RING_HPP
