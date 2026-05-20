/**
 * order_book.h — Reference matching engine (header-only)
 * =============================================================
 * Extracted from reference_engine.cpp so that both the journal-
 * replay CLI and the TCP reference server share the same logic.
 *
 * Invariants:
 *   1. SINGLE-WRITER PER SYMBOL: one thread mutates one book.
 *   2. DETERMINISTIC: same input -> byte-identical output every run.
 *      IMPORTANT: by_id_ (unordered_map) must NEVER be iterated. Hash
 *      iteration order is implementation-defined and would break
 *      determinism. Find/insert/erase are deterministic — those only.
 *   3. PRICE-TIME PRIORITY: orders at the same price match in arrival order.
 *   4. FIXED-POINT ONLY: int64_t ticks.
 *   5. MAKER FILL BEFORE TAKER FILL per match event — this is contract,
 *      and the byte-exact diff tool depends on it.
 * =============================================================
 */

#pragma once

#include "contracts/interface_contract_v1.h"

#include <cstdint>
#include <cstring>
#include <vector>
#include <list>
#include <unordered_map>
#include <ostream>

/* ─────────────────────────────────────────────
   Wire helper — write FrameHeader + Payload to an ostream
   ───────────────────────────────────────────── */
template <typename Payload>
static inline void write_framed(std::ostream& out, uint8_t msg_type, const Payload& p) {
    FrameHeader h{};
    h.msg_type = msg_type;
    h._pad     = 0;
    h.msg_len  = sizeof(Payload);
    out.write(reinterpret_cast<const char*>(&h), sizeof(h));
    out.write(reinterpret_cast<const char*>(&p), sizeof(p));
}

/* ─────────────────────────────────────────────
   Wire helper — write FrameHeader + Payload to a raw buffer
   Returns: number of bytes written (sizeof(FrameHeader) + sizeof(Payload))
   Caller must ensure buf has enough space.
   ───────────────────────────────────────────── */
template <typename Payload>
static inline size_t write_framed_buf(uint8_t* buf, uint8_t msg_type, const Payload& p) {
    FrameHeader h{};
    h.msg_type = msg_type;
    h._pad     = 0;
    h.msg_len  = sizeof(Payload);
    std::memcpy(buf, &h, sizeof(h));
    std::memcpy(buf + sizeof(h), &p, sizeof(p));
    return sizeof(h) + sizeof(p);
}

struct RestingOrder {
    uint64_t order_seq;
    uint64_t leaves_qty;
    uint8_t  side;
};

struct PriceLevel {
    std::list<RestingOrder> orders;
    uint64_t total_qty = 0;   // kept for future depth-snapshot debugging
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
    void on_cancel   (const CancelOrder& c, std::ostream& out);

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


/* ═════════════════════════════════════════════════════════════════
   INLINE IMPLEMENTATION — kept in header so both reference_engine
   and reference_server share identical logic without link-time
   duplication risk.
   ═════════════════════════════════════════════════════════════════ */

inline void OrderBook::emit_ack(uint64_t order_seq, uint32_t sym, std::ostream& out) {
    OrderAck a{};
    a.seq          = next_engine_counter_;
    a.timestamp_ns = next_engine_counter_;
    next_engine_counter_++;
    a.symbol_id    = sym;
    a.order_seq    = order_seq;
    write_framed(out, MSG_ORDERACK, a);
}

inline void OrderBook::emit_fill(uint64_t order_seq, uint32_t sym, uint8_t side,
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

inline void OrderBook::emit_reject(uint64_t order_seq, uint32_t sym, uint8_t reason,
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

inline void OrderBook::refresh_best_bid_from(int64_t hint_idx) {
    if (hint_idx < 0) hint_idx = static_cast<int64_t>(NUM_LEVELS) - 1;
    for (int64_t i = hint_idx; i >= 0; --i) {
        if (!levels_[i].orders.empty()) { best_bid_ = i; return; }
    }
    best_bid_ = -1;
}

inline void OrderBook::refresh_best_ask_from(int64_t hint_idx) {
    if (hint_idx < 0) hint_idx = 0;
    for (int64_t i = hint_idx; i < static_cast<int64_t>(NUM_LEVELS); ++i) {
        if (!levels_[i].orders.empty()) { best_ask_ = i; return; }
    }
    best_ask_ = -1;
}

inline void OrderBook::match_aggressive(uint8_t taker_side, int64_t limit_price_ticks,
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
            lvl.total_qty  -= trade_qty;
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

inline void OrderBook::rest_order(const NewOrder& o, uint64_t leaves_qty,
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
    lvl.total_qty += leaves_qty;
    auto it = std::prev(lvl.orders.end());
    by_id_[o.seq] = {idx, it};

    if (o.side == SIDE_BUY) {
        if (best_bid_ < 0 || idx > best_bid_) best_bid_ = idx;
    } else {
        if (best_ask_ < 0 || idx < best_ask_) best_ask_ = idx;
    }
}

inline void OrderBook::on_new_order(const NewOrder& o, std::ostream& out) {
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

inline void OrderBook::on_cancel(const CancelOrder& c, std::ostream& out) {
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

// end of order_book.h — include guard handled by #pragma once
