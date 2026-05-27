// buggy_engine.cpp — DEMO: Reference engine with price-time priority bug.
//
// THE BUG (intentional, planted for demo):
//   In match_aggressive(), this engine walks the price-level list from
//   the BACK instead of the FRONT. This means the NEWEST resting order
//   at a given price gets matched first, violating FIFO (price-TIME
//   priority).
//
// WHY THIS BUG MATTERS:
//   Price-time priority is a foundational exchange rule. Real exchanges
//   (NYSE Pillar, NASDAQ ITCH) have had production bugs of exactly this
//   shape — sometimes costing millions before detection. A correctness
//   validator that can't catch this is useless.
//
// THE DEMO:
//   Run the same input journal through both refengine (reference) and
//   buggy_engine. Then diff the outputs. The diff tool will pinpoint
//   the exact Fill message where the engines diverge, showing the
//   WRONG order_seq matched first — instantly visible bug.
//
// Build (from bot-engine/):
//   g++ -std=c++20 -O2 -Wall -Wextra -I.. demo/buggy_engine.cpp -o build/buggy_engine

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

// ── Identical types to reference_engine ───────────────────────────────────
struct RestingOrder {
    uint64_t order_seq;
    uint64_t leaves_qty;
    uint8_t  side;
};

struct PriceLevel {
    std::list<RestingOrder> orders;
    uint64_t total_qty = 0;
};

// BuggyOrderBook — one instance per submission per symbol.
// Identical to reference OrderBook EXCEPT for match_aggressive() iteration.
class BuggyOrderBook {
public:
    static constexpr int64_t MIN_PRICE  = 1;
    static constexpr int64_t MAX_PRICE  = 1'000'000;
    static constexpr int64_t TICK_SIZE  = 1;
    static constexpr size_t  NUM_LEVELS =
        static_cast<size_t>((MAX_PRICE - MIN_PRICE) / TICK_SIZE + 1);

    BuggyOrderBook() : levels_(NUM_LEVELS) {}

    void on_new_order(const NewOrder& o, std::ostream& out);
    void on_cancel  (const CancelOrder& c, std::ostream& out);

private:
    std::vector<PriceLevel> levels_;
    std::unordered_map<
        uint64_t,
        std::pair<int64_t, std::list<RestingOrder>::iterator>
    > by_id_;

    int64_t best_bid_ = -1;
    int64_t best_ask_ = -1;
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

void BuggyOrderBook::emit_ack(uint64_t order_seq, uint32_t sym, std::ostream& out) {
    OrderAck a{};
    a.seq          = next_engine_counter_;
    a.timestamp_ns = next_engine_counter_;
    next_engine_counter_++;
    a.symbol_id    = sym;
    a.order_seq    = order_seq;
    write_framed(out, MSG_ORDERACK, a);
}

void BuggyOrderBook::emit_fill(uint64_t order_seq, uint32_t sym, uint8_t side,
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

void BuggyOrderBook::emit_reject(uint64_t order_seq, uint32_t sym, uint8_t reason,
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

void BuggyOrderBook::refresh_best_bid_from(int64_t hint_idx) {
    if (hint_idx < 0) hint_idx = static_cast<int64_t>(NUM_LEVELS) - 1;
    for (int64_t i = hint_idx; i >= 0; --i) {
        if (!levels_[i].orders.empty()) { best_bid_ = i; return; }
    }
    best_bid_ = -1;
}

void BuggyOrderBook::refresh_best_ask_from(int64_t hint_idx) {
    if (hint_idx < 0) hint_idx = 0;
    for (int64_t i = hint_idx; i < static_cast<int64_t>(NUM_LEVELS); ++i) {
        if (!levels_[i].orders.empty()) { best_ask_ = i; return; }
    }
    best_ask_ = -1;
}

// ═══════════════════════════════════════════════════════════════════════════
// THE BUG IS HERE — match_aggressive walks the price level from BACK,
// matching newest order first instead of oldest. Violates price-TIME
// priority. Compare to reference_engine.cpp which uses lvl.orders.begin().
// ═══════════════════════════════════════════════════════════════════════════
void BuggyOrderBook::match_aggressive(uint8_t taker_side, int64_t limit_price_ticks,
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

        // ▼▼▼ THE BUG ▼▼▼
        // Correct (reference) implementation walks from FRONT:
        //   auto it = lvl.orders.begin();
        //   while (it != lvl.orders.end() && ...) ++it;
        //
        // This buggy version walks from BACK — LIFO instead of FIFO.
        // The newest resting order at this price level matches first,
        // violating price-time priority.
        while (!lvl.orders.empty() && leaves_qty > 0) {
            auto it = std::prev(lvl.orders.end());  // ← BUG: back instead of begin

            uint64_t trade_qty = (it->leaves_qty < leaves_qty)
                ? it->leaves_qty : leaves_qty;

            it->leaves_qty -= trade_qty;
            lvl.total_qty  -= trade_qty;
            leaves_qty     -= trade_qty;

            // Maker Fill first (resting order), then taker Fill — same as reference
            emit_fill(it->order_seq, symbol_id, it->side,
                      best_price, trade_qty, it->leaves_qty, out);
            emit_fill(taker_order_seq, symbol_id, taker_side,
                      best_price, trade_qty, leaves_qty, out);

            if (it->leaves_qty == 0) {
                by_id_.erase(it->order_seq);
                lvl.orders.erase(it);
            }
        }
        // ▲▲▲ END BUG ▲▲▲

        if (lvl.orders.empty()) {
            if (taker_is_buy) refresh_best_ask_from(best + 1);
            else              refresh_best_bid_from(best - 1);
        }
    }
}

void BuggyOrderBook::rest_order(const NewOrder& o, uint64_t leaves_qty,
                                 std::ostream& out) {
    if (leaves_qty == 0) return;

    if (o.order_type == ORDER_MARKET) {
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
    lvl.total_qty += leaves_qty;
    auto it = std::prev(lvl.orders.end());
    by_id_[o.seq] = {idx, it};

    if (o.side == SIDE_BUY) {
        if (best_bid_ < 0 || idx > best_bid_) best_bid_ = idx;
    } else {
        if (best_ask_ < 0 || idx < best_ask_) best_ask_ = idx;
    }
}

void BuggyOrderBook::on_new_order(const NewOrder& o, std::ostream& out) {
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

void BuggyOrderBook::on_cancel(const CancelOrder& c, std::ostream& out) {
    auto it = by_id_.find(c.order_seq);
    if (it == by_id_.end()) {
        emit_reject(c.seq, c.symbol_id, REJECT_ORDER_NOT_FOUND, out);
        return;
    }

    int64_t price_idx = it->second.first;
    auto    list_it   = it->second.second;
    PriceLevel& lvl   = levels_[price_idx];

    lvl.total_qty -= list_it->leaves_qty;
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

    BuggyOrderBook book;
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

int main(int argc, char** argv) {
    if (argc != 4 || std::string(argv[1]) != "replay") {
        std::cerr << "usage: buggy_engine replay <input.jrn> <output.jrn>\n";
        return 2;
    }
    replay_journal(argv[2], argv[3]);
    return 0;
}
