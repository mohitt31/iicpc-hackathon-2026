// reference_engine.cpp — gold-master matching engine + diff tool
//
// Invariants:
//   1. SINGLE-WRITER PER SYMBOL: one thread mutates one book.
//   2. DETERMINISTIC: same input -> byte-identical output every run.
//      IMPORTANT: by_id_ (unordered_map) must NEVER be iterated. Hash
//      iteration order is implementation-defined and would break
//      determinism. Find/insert/erase are deterministic — those only.
//   3. PRICE-TIME PRIORITY: orders at the same price match in arrival order.
//   4. FIXED-POINT ONLY: int64_t ticks.
//   5. MAKER FILL BEFORE TAKER FILL per match event — this is contract,
//      and the byte-exact diff tool depends on it.
//
// Build (from bot-engine/):
//   mkdir -p build
//   g++ -std=c++20 -O2 -Wall -Wextra -I.. src/reference_engine.cpp -o build/refengine

#include "order_book.h"

#include <fstream>
#include <iostream>
#include <string>
#include <stdexcept>
#include <iomanip>

static void replay_journal(const std::string& in_path,
                           const std::string& out_path) {
    std::ifstream in (in_path,  std::ios::binary);
    std::ofstream out(out_path, std::ios::binary);
    if (!in)  throw std::runtime_error("cannot open input journal");
    if (!out) throw std::runtime_error("cannot open output journal");

    OrderBook book;
    FrameHeader hdr;
    std::vector<uint8_t> payload;

    while (in.read(reinterpret_cast<char*>(&hdr), sizeof(hdr))) {
        payload.resize(hdr.msg_len);
        in.read(reinterpret_cast<char*>(payload.data()), hdr.msg_len);

        switch (hdr.msg_type) {
            case MSG_NEWORDER: {
                NewOrder o;
                std::memcpy(&o, payload.data(), sizeof(o));
                book.on_new_order(o, out);
                break;
            }
            case MSG_CANCEL: {
                CancelOrder c;
                std::memcpy(&c, payload.data(), sizeof(c));
                book.on_cancel(c, out);
                break;
            }
            default: break;
        }
    }
}

// Report which MESSAGE diverged, not just byte offset.
// On stage during the correctness encore, "diverged at message 47
// (Fill, order_seq=23, my fill_qty=5 vs theirs=4)" is far more compelling
// than "byte 1843 differs."
static void describe_message_at(const std::string& path, size_t target_offset,
                                const char* label) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return;
    FrameHeader hdr;
    size_t cursor = 0;
    size_t msg_num = 0;
    std::vector<uint8_t> payload;
    while (f.read(reinterpret_cast<char*>(&hdr), sizeof(hdr))) {
        msg_num++;
        size_t frame_size = sizeof(hdr) + hdr.msg_len;
        if (cursor + frame_size > target_offset) {
            payload.resize(hdr.msg_len);
            f.read(reinterpret_cast<char*>(payload.data()), hdr.msg_len);
            if (!f) return;  // truncated journal — don't print garbage
            std::cerr << "  " << label << ": message #" << msg_num
                      << " (msg_type=" << static_cast<int>(hdr.msg_type)
                      << ", msg_len=" << hdr.msg_len;
            // Common header: seq @ 0, timestamp @ 8, symbol_id @ 16
            if (hdr.msg_len >= 24) {
                uint64_t seq, ts;
                uint32_t sym;
                std::memcpy(&seq, payload.data() + 0,  8);
                std::memcpy(&ts,  payload.data() + 8,  8);
                std::memcpy(&sym, payload.data() + 16, 4);
                std::cerr << ", engine_seq=" << seq
                          << ", ts=" << ts
                          << ", sym=" << sym;
                if (hdr.msg_len >= 32) {
                    uint64_t order_seq;
                    std::memcpy(&order_seq, payload.data() + 24, 8);
                    std::cerr << ", order_seq=" << order_seq;
                }
            }
            std::cerr << ")\n";
            return;
        }
        f.seekg(hdr.msg_len, std::ios::cur);
        cursor += frame_size;
    }
}

static int diff_journals(const std::string& a_path,
                         const std::string& b_path) {
    std::ifstream a(a_path, std::ios::binary), b(b_path, std::ios::binary);
    if (!a || !b) { std::cerr << "diff: cannot open inputs\n"; return 2; }

    size_t offset = 0;
    char ca, cb;
    while (a.get(ca)) {
        if (!b.get(cb)) {
            std::cerr << "DIVERGE @ byte " << offset
                      << ": A has more bytes than B\n";
            describe_message_at(a_path, offset, "A");
            return 1;
        }
        if (ca != cb) {
            std::cerr << "DIVERGE @ byte " << offset
                      << ": A=0x" << std::hex << std::setw(2) << std::setfill('0')
                      << (int)(uint8_t)ca
                      << " B=0x" << std::setw(2) << (int)(uint8_t)cb
                      << std::dec << "\n";
            describe_message_at(a_path, offset, "A");
            describe_message_at(b_path, offset, "B");
            return 1;
        }
        ++offset;
    }
    if (b.get(cb)) {
        std::cerr << "DIVERGE @ byte " << offset
                  << ": B has more bytes than A\n";
        describe_message_at(b_path, offset, "B");
        return 1;
    }
    std::cout << "IDENTICAL: " << offset << " bytes match\n";
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr <<
          "usage:\n"
          "  refengine replay <input.jrn> <output.jrn>\n"
          "  refengine diff   <a.jrn> <b.jrn>\n";
        return 2;
    }
    std::string cmd = argv[1];
    if (cmd == "replay" && argc == 4) {
        replay_journal(argv[2], argv[3]);
        return 0;
    }
    if (cmd == "diff" && argc == 4) {
        return diff_journals(argv[2], argv[3]);
    }
    std::cerr << "bad args\n"; return 2;
}
