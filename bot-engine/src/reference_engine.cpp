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
//   Built via CMakeLists.txt (cmake .. && make -j refengine)

#include "contracts/interface_contract_v1.h"

#include <cstdint>
#include <cstring>
#include <vector>
#include <list>
#include <unordered_map>
#include <fstream>
#include <iostream>
#include <string>
#include <stdexcept>
#include <iomanip>

struct RestingOrder {
    uint64_t order_seq;
    uint64_t leaves_qty;
    uint8_t  side;
};

struct PriceLevel {
    std::list<RestingOrder> orders;
};

// OrderBook — one instance per contestant submission per symbol.
// next_engine_counter_ is per-instance and starts at 1. Reusing the
// same OrderBook across submissions would cause engine_seq collisions
// in the journal, breaking the diff tool's per-submission isolation.
// Track B's batching layer is responsible for instantiating a fresh
// OrderBook for each contestant run.
class OrderBook {
public:
    static constexpr int64_t MIN_PRICE  = 1;
    static constexpr int64_t MAX_PRICE  = 1'000'000;
    static constexpr int64_t TICK_SIZE  = 1;
    static constexpr size_t  NUM_LEVELS =
        static_cast<size_t>((MAX_PRICE - MIN_PRICE) / TICK_SIZE + 1);

    OrderBook() : levels_(NUM_LEVELS) {}

    void on_new_order(const NewOrder& o, std::ostream& out);
    void on_cancel  (const CancelOrder& c, std::ostream& out);

private:
    std::vector<PriceLevel> levels_;

    // O(1) lookup for cancel. NEVER iterated — see invariant #2.
    std::unordered_map<
        uint64_t,
        std::pair<int64_t, std::list<RestingOrder>::iterator>
    > by_id_;

    int64_t best_bid_ = -1;
    int64_t best_ask_ = -1;

    // Single counter for both engine seq and engine ts.
    // Each emit consumes one tick and advances both fields by the same
    // amount, so collapsing them is safe and the schema looks cleaner.
    uint64_t next_engine_counter_ = 1;

    void match_aggressive(uint8_t taker_side, int64_t limit_price_ticks,
                          uint64_t& leaves_qty, uint64_t taker_order_seq,
                          uint32_t symbol_id, std::ostream& out,
                          bool is_market);
    void rest_order(const NewOrder& o, uint64_t leaves_qty, std::ostream& out);

    void refresh_best_bid_from(int64_t hint_idx);
    void refresh_best_ask_from(int64_t hint_idx);

    void emit_ack   (uint64_t order_seq, uint32_t sym, std::ostream& out);
    void emit_fill  (uint64_t order_seq, uint32_t sym, uint8_t side,
                     int64_t px_ticks, uint64_t fill_qty, uint64_t leaves,
                     std::ostream& out);
    void emit_reject(uint64_t order_seq, uint32_t sym, uint8_t reason,
                     std::ostream& out);

    int64_t price_to_index(int64_t px) const { return (px - MIN_PRICE) / TICK_SIZE; }
    int64_t index_to_price(int64_t i)  const { return MIN_PRICE + i * TICK_SIZE;   }
};

template <typename Payload>
static void write_framed(std::ostream& out, uint8_t msg_type, const Payload& p) {
    FrameHeader h{};
    h.msg_type = msg_type;
    h._pad     = 0;
    h.msg_len  = sizeof(Payload);
    out.write(reinterpret_cast<const char*>(&h), sizeof(h));
    out.write(reinterpret_cast<const char*>(&p), sizeof(p));
}

void OrderBook::emit_ack(uint64_t order_seq, uint32_t sym, std::ostream& out) {
    OrderAck a{};
    a.seq          = next_engine_counter_;
    a.timestamp_ns = next_engine_counter_;
    next_engine_counter_++;
    a.symbol_id    = sym;
    a.order_seq    = order_seq;
    write_framed(out, MSG_ORDERACK, a);
}

void OrderBook::emit_fill(uint64_t order_seq, uint32_t sym, uint8_t side,
                          int64_t px, uint64_t qty, uint64_t leaves,
                          std::ostream& out) {
    Fill f{};
    f.seq          = next_engine_counter_;
    f.timestamp_ns = next_engine_counter_;
    next_engine_counter_++;
    f.symbol_id    = sym;
    f.side         = side;
    f.order_seq    = order_seq;
    f.fill_price   = px;
    f.fill_qty     = qty;
    f.leaves_qty   = leaves;
    write_framed(out, MSG_FILL, f);
}

void OrderBook::emit_reject(uint64_t order_seq, uint32_t sym, uint8_t reason,
                            std::ostream& out) {
    Reject r{};
    r.seq          = next_engine_counter_;
    r.timestamp_ns = next_engine_counter_;
    next_engine_counter_++;
    r.symbol_id    = sym;
    r.reason       = reason;
    r.order_seq    = order_seq;
    write_framed(out, MSG_REJECT, r);
}

void OrderBook::refresh_best_bid_from(int64_t hint_idx) {
    if (hint_idx < 0) hint_idx = static_cast<int64_t>(NUM_LEVELS) - 1;
    for (int64_t i = hint_idx; i >= 0; --i) {
        if (!levels_[i].orders.empty()) { best_bid_ = i; return; }
    }
    best_bid_ = -1;
}

void OrderBook::refresh_best_ask_from(int64_t hint_idx) {
    if (hint_idx < 0) hint_idx = 0;
    for (int64_t i = hint_idx; i < static_cast<int64_t>(NUM_LEVELS); ++i) {
        if (!levels_[i].orders.empty()) { best_ask_ = i; return; }
    }
    best_ask_ = -1;
}

void OrderBook::match_aggressive(uint8_t taker_side, int64_t limit_price_ticks,
                                 uint64_t& leaves_qty, uint64_t taker_order_seq,
                                 uint32_t symbol_id, std::ostream& out,
                                 bool is_market) {
    const bool taker_is_buy = (taker_side == SIDE_BUY);

    while (leaves_qty > 0) {
        int64_t best = taker_is_buy ? best_ask_ : best_bid_;
        if (best < 0) break;

        int64_t best_price = index_to_price(best);

        if (!is_market) {
            bool crosses = taker_is_buy
                ? (limit_price_ticks >= best_price)
                : (limit_price_ticks <= best_price);
            if (!crosses) break;
        }

        PriceLevel& lvl = levels_[best];
        auto it = lvl.orders.begin();
        while (it != lvl.orders.end() && leaves_qty > 0) {
            uint64_t trade_qty = (it->leaves_qty < leaves_qty)
                ? it->leaves_qty : leaves_qty;

            // Apply the trade
            it->leaves_qty -= trade_qty;
            leaves_qty     -= trade_qty;

            // Emit TWO Fills per execution event. Maker Fill first
            // (resting order's perspective), then taker Fill (incoming
            // aggressive order's perspective). This matches what real
            // exchanges do and what a real contestant matching engine
            // will produce — so the diff tool actually has matching
            // streams to compare. Order is contract (invariant #5).
            //
            // Maker side: it->side is the resting order's side.
            // Maker's leaves_qty after the trade is it->leaves_qty.
            emit_fill(it->order_seq, symbol_id, it->side,
                      best_price, trade_qty, it->leaves_qty, out);

            // Taker side: the incoming aggressive order's side.
            // Taker's leaves_qty after the trade is the outer
            // 'leaves_qty' variable (which we just decremented).
            emit_fill(taker_order_seq, symbol_id, taker_side,
                      best_price, trade_qty, leaves_qty, out);

            if (it->leaves_qty == 0) {
                by_id_.erase(it->order_seq);
                it = lvl.orders.erase(it);
            } else {
                ++it;
            }
        }

        if (lvl.orders.empty()) {
            if (taker_is_buy) refresh_best_ask_from(best + 1);
            else              refresh_best_bid_from(best - 1);
        }
    }
}

void OrderBook::rest_order(const NewOrder& o, uint64_t leaves_qty,
                           std::ostream& out) {
    if (leaves_qty == 0) return;

    if (o.order_type == ORDER_MARKET) {
        // MARKET = IOC semantics (per contract). Fills for what filled
        // have already been emitted by match_aggressive(). This emits
        // the single Reject covering the residual unfillable qty.
        emit_reject(o.seq, o.symbol_id, REJECT_MARKET_INSUFFICIENT_LIQUIDITY, out);
        return;
    }

    int64_t idx = price_to_index(o.price);
    if (idx < 0 || static_cast<size_t>(idx) >= NUM_LEVELS) {
        emit_reject(o.seq, o.symbol_id, REJECT_INVALID_PRICE, out);
        return;
    }

    PriceLevel& lvl = levels_[idx];
    lvl.orders.push_back(RestingOrder{o.seq, leaves_qty, o.side});
    auto it = std::prev(lvl.orders.end());
    by_id_[o.seq] = {idx, it};

    if (o.side == SIDE_BUY) {
        if (best_bid_ < 0 || idx > best_bid_) best_bid_ = idx;
    } else {
        if (best_ask_ < 0 || idx < best_ask_) best_ask_ = idx;
    }
}

void OrderBook::on_new_order(const NewOrder& o, std::ostream& out) {
    if (o.quantity == 0) {
        emit_reject(o.seq, o.symbol_id, REJECT_INVALID_QTY, out); return;
    }
    if (o.order_type == ORDER_LIMIT) {
        if (o.price <= 0 || o.price < MIN_PRICE || o.price > MAX_PRICE) {
            emit_reject(o.seq, o.symbol_id, REJECT_INVALID_PRICE, out); return;
        }
    } else if (o.order_type == ORDER_MARKET) {
        if (o.price != 0) {
            emit_reject(o.seq, o.symbol_id, REJECT_INVALID_PRICE, out); return;
        }
    } else {
        emit_reject(o.seq, o.symbol_id, REJECT_INVALID_PRICE, out); return;
    }
    if (by_id_.count(o.seq)) {
        emit_reject(o.seq, o.symbol_id, REJECT_DUPLICATE_SEQ, out); return;
    }

    emit_ack(o.seq, o.symbol_id, out);

    uint64_t leaves = o.quantity;
    match_aggressive(o.side, o.price, leaves, o.seq, o.symbol_id, out,
                     o.order_type == ORDER_MARKET);
    rest_order(o, leaves, out);
}

void OrderBook::on_cancel(const CancelOrder& c, std::ostream& out) {
    auto it = by_id_.find(c.order_seq);
    if (it == by_id_.end()) {
        emit_reject(c.seq, c.symbol_id, REJECT_ORDER_NOT_FOUND, out);
        return;
    }

    int64_t price_idx = it->second.first;
    auto    list_it   = it->second.second;
    PriceLevel& lvl   = levels_[price_idx];

    lvl.orders.erase(list_it);
    by_id_.erase(it);

    if (lvl.orders.empty()) {
        if (price_idx == best_bid_) refresh_best_bid_from(price_idx - 1);
        if (price_idx == best_ask_) refresh_best_ask_from(price_idx + 1);
    }

    emit_ack(c.seq, c.symbol_id, out);
}

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
