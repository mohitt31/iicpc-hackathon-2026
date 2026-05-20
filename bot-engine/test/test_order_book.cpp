// test_order_book.cpp — Unit tests for OrderBook matching engine
//
// Tests all contract-specified behaviors from interface_contract_v1.h:
//   - LIMIT order ack + resting
//   - LIMIT order matching (price-time priority)
//   - MARKET order IOC semantics (partial fill + reject for residual)
//   - Cancel success / ORDER_NOT_FOUND
//   - Reject: INVALID_PRICE, INVALID_QTY, DUPLICATE_SEQ
//   - Maker-before-taker fill ordering (invariant #5)
//   - Determinism: same input → byte-identical output
//
// Build:
//   g++ -std=c++20 -O2 -Wall -Wextra -I../../ -I../src test_order_book.cpp -o test_order_book
// Run:
//   ./test_order_book

#include "order_book.h"

#include <sstream>
#include <cstring>
#include <iostream>
#include <vector>
#include <cassert>
#include <cstdlib>

// ── Helpers ────────────────────────────────────────────────────

struct ParsedMessage {
    FrameHeader header;
    std::vector<uint8_t> payload;
};

static std::vector<ParsedMessage> parse_output(const std::string& data) {
    std::vector<ParsedMessage> msgs;
    size_t offset = 0;
    while (offset + sizeof(FrameHeader) <= data.size()) {
        ParsedMessage m;
        std::memcpy(&m.header, data.data() + offset, sizeof(FrameHeader));
        if (offset + sizeof(FrameHeader) + m.header.msg_len > data.size()) break;
        m.payload.resize(m.header.msg_len);
        std::memcpy(m.payload.data(), data.data() + offset + sizeof(FrameHeader), m.header.msg_len);
        msgs.push_back(std::move(m));
        offset += sizeof(FrameHeader) + m.header.msg_len;
    }
    return msgs;
}

template<typename T>
static T extract(const ParsedMessage& m) {
    T result{};
    assert(m.payload.size() >= sizeof(T));
    std::memcpy(&result, m.payload.data(), sizeof(T));
    return result;
}

static NewOrder make_limit(uint64_t seq, uint32_t sym, uint8_t side,
                           int64_t price, uint64_t qty) {
    NewOrder o{};
    o.seq = seq;
    o.timestamp_ns = seq;  // deterministic
    o.symbol_id = sym;
    o.order_type = ORDER_LIMIT;
    o.side = side;
    o.price = price;
    o.quantity = qty;
    return o;
}

static NewOrder make_market(uint64_t seq, uint32_t sym, uint8_t side, uint64_t qty) {
    NewOrder o{};
    o.seq = seq;
    o.timestamp_ns = seq;
    o.symbol_id = sym;
    o.order_type = ORDER_MARKET;
    o.side = side;
    o.price = 0;
    o.quantity = qty;
    return o;
}

static CancelOrder make_cancel(uint64_t seq, uint32_t sym, uint64_t target_seq) {
    CancelOrder c{};
    c.seq = seq;
    c.timestamp_ns = seq;
    c.symbol_id = sym;
    c.order_seq = target_seq;
    return c;
}

// ── Test counters ──────────────────────────────────────────────
static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) \
    do { \
        tests_run++; \
        std::cout << "  TEST: " << #name << " ... "; \
        try { name(); tests_passed++; std::cout << "PASS\n"; } \
        catch (const std::exception& e) { std::cout << "FAIL: " << e.what() << "\n"; } \
        catch (...) { std::cout << "FAIL: unknown exception\n"; } \
    } while(0)

#define ASSERT_EQ(a, b) \
    do { if ((a) != (b)) { \
        std::ostringstream _ss; \
        _ss << "ASSERT_EQ failed: " << #a << " = " << (a) << " != " << #b << " = " << (b) \
            << " at " << __FILE__ << ":" << __LINE__; \
        throw std::runtime_error(_ss.str()); \
    }} while(0)

#define ASSERT_TRUE(x) \
    do { if (!(x)) { \
        std::ostringstream _ss; \
        _ss << "ASSERT_TRUE failed: " << #x << " at " << __FILE__ << ":" << __LINE__; \
        throw std::runtime_error(_ss.str()); \
    }} while(0)

// ═══════════════════════════════════════════════════════════════
// TESTS
// ═══════════════════════════════════════════════════════════════

void test_limit_order_ack() {
    // A single LIMIT order should produce exactly one OrderAck.
    OrderBook book;
    std::ostringstream out;
    auto o = make_limit(1, 1, SIDE_BUY, 100, 10);
    book.on_new_order(o, out);

    auto msgs = parse_output(out.str());
    ASSERT_EQ(msgs.size(), 1u);
    ASSERT_EQ(msgs[0].header.msg_type, MSG_ORDERACK);

    auto ack = extract<OrderAck>(msgs[0]);
    ASSERT_EQ(ack.order_seq, 1u);
    ASSERT_EQ(ack.symbol_id, 1u);
}

void test_limit_order_match_buy_sell() {
    // BUY@100 rests, then SELL@100 should match → 2 Fills (maker, taker)
    OrderBook book;
    std::ostringstream out;

    auto buy = make_limit(1, 1, SIDE_BUY, 100, 5);
    book.on_new_order(buy, out);
    out.str(""); out.clear();

    auto sell = make_limit(2, 1, SIDE_SELL, 100, 5);
    book.on_new_order(sell, out);

    auto msgs = parse_output(out.str());
    // Expected: OrderAck(sell) + Fill(maker=buy) + Fill(taker=sell)
    ASSERT_EQ(msgs.size(), 3u);
    ASSERT_EQ(msgs[0].header.msg_type, MSG_ORDERACK);
    ASSERT_EQ(msgs[1].header.msg_type, MSG_FILL);
    ASSERT_EQ(msgs[2].header.msg_type, MSG_FILL);

    // Invariant #5: maker fill BEFORE taker fill
    auto maker_fill = extract<Fill>(msgs[1]);
    auto taker_fill = extract<Fill>(msgs[2]);
    ASSERT_EQ(maker_fill.order_seq, 1u);  // resting buy
    ASSERT_EQ(taker_fill.order_seq, 2u);  // incoming sell
    ASSERT_EQ(maker_fill.fill_qty, 5u);
    ASSERT_EQ(taker_fill.fill_qty, 5u);
    ASSERT_EQ(maker_fill.leaves_qty, 0u);  // fully filled
    ASSERT_EQ(taker_fill.leaves_qty, 0u);
    ASSERT_EQ(maker_fill.fill_price, 100);
    ASSERT_EQ(taker_fill.fill_price, 100);
    ASSERT_EQ(maker_fill.side, SIDE_BUY);
    ASSERT_EQ(taker_fill.side, SIDE_SELL);
}

void test_partial_fill() {
    // BUY@100 qty=10, then SELL@100 qty=3 → partial fill
    OrderBook book;
    std::ostringstream out;

    book.on_new_order(make_limit(1, 1, SIDE_BUY, 100, 10), out);
    out.str(""); out.clear();

    book.on_new_order(make_limit(2, 1, SIDE_SELL, 100, 3), out);
    auto msgs = parse_output(out.str());

    ASSERT_EQ(msgs.size(), 3u);  // Ack + 2 fills
    auto maker_fill = extract<Fill>(msgs[1]);
    auto taker_fill = extract<Fill>(msgs[2]);
    ASSERT_EQ(maker_fill.fill_qty, 3u);
    ASSERT_EQ(maker_fill.leaves_qty, 7u);  // 10 - 3
    ASSERT_EQ(taker_fill.fill_qty, 3u);
    ASSERT_EQ(taker_fill.leaves_qty, 0u);  // fully filled
}

void test_price_time_priority() {
    // Two buys at same price; first one should fill first (time priority)
    OrderBook book;
    std::ostringstream out;

    book.on_new_order(make_limit(1, 1, SIDE_BUY, 100, 5), out);
    book.on_new_order(make_limit(2, 1, SIDE_BUY, 100, 5), out);
    out.str(""); out.clear();

    book.on_new_order(make_limit(3, 1, SIDE_SELL, 100, 5), out);
    auto msgs = parse_output(out.str());

    ASSERT_EQ(msgs.size(), 3u);
    auto maker_fill = extract<Fill>(msgs[1]);
    ASSERT_EQ(maker_fill.order_seq, 1u);  // first resting order fills first
    ASSERT_EQ(maker_fill.leaves_qty, 0u);
}

void test_price_priority() {
    // BUY@101 and BUY@100; sell should match BUY@101 first (better price)
    OrderBook book;
    std::ostringstream out;

    book.on_new_order(make_limit(1, 1, SIDE_BUY, 100, 5), out);
    book.on_new_order(make_limit(2, 1, SIDE_BUY, 101, 5), out);
    out.str(""); out.clear();

    book.on_new_order(make_limit(3, 1, SIDE_SELL, 100, 5), out);
    auto msgs = parse_output(out.str());

    ASSERT_EQ(msgs.size(), 3u);
    auto maker_fill = extract<Fill>(msgs[1]);
    ASSERT_EQ(maker_fill.order_seq, 2u);  // higher bid fills first
    ASSERT_EQ(maker_fill.fill_price, 101);
}

void test_no_cross_limit() {
    // BUY@99 and SELL@101 should NOT match
    OrderBook book;
    std::ostringstream out;

    book.on_new_order(make_limit(1, 1, SIDE_BUY, 99, 5), out);
    out.str(""); out.clear();

    book.on_new_order(make_limit(2, 1, SIDE_SELL, 101, 5), out);
    auto msgs = parse_output(out.str());

    ASSERT_EQ(msgs.size(), 1u);  // only ack, no fills
    ASSERT_EQ(msgs[0].header.msg_type, MSG_ORDERACK);
}

void test_market_order_full_fill() {
    // Resting SELL@100, MARKET BUY qty=5 fills fully
    OrderBook book;
    std::ostringstream out;

    book.on_new_order(make_limit(1, 1, SIDE_SELL, 100, 5), out);
    out.str(""); out.clear();

    book.on_new_order(make_market(2, 1, SIDE_BUY, 5), out);
    auto msgs = parse_output(out.str());

    // Ack + maker fill + taker fill
    ASSERT_EQ(msgs.size(), 3u);
    auto taker_fill = extract<Fill>(msgs[2]);
    ASSERT_EQ(taker_fill.leaves_qty, 0u);
}

void test_market_order_ioc_reject() {
    // MARKET BUY with no liquidity → Ack + Reject (MARKET_INSUFFICIENT_LIQUIDITY)
    OrderBook book;
    std::ostringstream out;

    book.on_new_order(make_market(1, 1, SIDE_BUY, 5), out);
    auto msgs = parse_output(out.str());

    ASSERT_EQ(msgs.size(), 2u);
    ASSERT_EQ(msgs[0].header.msg_type, MSG_ORDERACK);
    ASSERT_EQ(msgs[1].header.msg_type, MSG_REJECT);

    auto rej = extract<Reject>(msgs[1]);
    ASSERT_EQ(rej.reason, REJECT_MARKET_INSUFFICIENT_LIQUIDITY);
    ASSERT_EQ(rej.order_seq, 1u);
}

void test_market_order_partial_fill_then_reject() {
    // Resting SELL@100 qty=3, MARKET BUY qty=5 → fills 3, rejects 2
    OrderBook book;
    std::ostringstream out;

    book.on_new_order(make_limit(1, 1, SIDE_SELL, 100, 3), out);
    out.str(""); out.clear();

    book.on_new_order(make_market(2, 1, SIDE_BUY, 5), out);
    auto msgs = parse_output(out.str());

    // Ack + maker fill + taker fill + reject for residual
    ASSERT_EQ(msgs.size(), 4u);
    ASSERT_EQ(msgs[0].header.msg_type, MSG_ORDERACK);
    ASSERT_EQ(msgs[1].header.msg_type, MSG_FILL);
    ASSERT_EQ(msgs[2].header.msg_type, MSG_FILL);
    ASSERT_EQ(msgs[3].header.msg_type, MSG_REJECT);

    auto taker_fill = extract<Fill>(msgs[2]);
    ASSERT_EQ(taker_fill.fill_qty, 3u);
    ASSERT_EQ(taker_fill.leaves_qty, 2u);  // 5 - 3 remaining

    auto rej = extract<Reject>(msgs[3]);
    ASSERT_EQ(rej.reason, REJECT_MARKET_INSUFFICIENT_LIQUIDITY);
}

void test_market_order_price_must_be_zero() {
    // MARKET with non-zero price → REJECT_INVALID_PRICE
    OrderBook book;
    std::ostringstream out;

    NewOrder o{};
    o.seq = 1;
    o.timestamp_ns = 1;
    o.symbol_id = 1;
    o.order_type = ORDER_MARKET;
    o.side = SIDE_BUY;
    o.price = 100;  // violation
    o.quantity = 5;

    book.on_new_order(o, out);
    auto msgs = parse_output(out.str());

    ASSERT_EQ(msgs.size(), 1u);
    ASSERT_EQ(msgs[0].header.msg_type, MSG_REJECT);
    auto rej = extract<Reject>(msgs[0]);
    ASSERT_EQ(rej.reason, REJECT_INVALID_PRICE);
}

void test_cancel_success() {
    // Place LIMIT order, then cancel it
    OrderBook book;
    std::ostringstream out;

    book.on_new_order(make_limit(1, 1, SIDE_BUY, 100, 5), out);
    out.str(""); out.clear();

    auto c = make_cancel(2, 1, 1);
    book.on_cancel(c, out);
    auto msgs = parse_output(out.str());

    ASSERT_EQ(msgs.size(), 1u);
    ASSERT_EQ(msgs[0].header.msg_type, MSG_ORDERACK);
    auto ack = extract<OrderAck>(msgs[0]);
    ASSERT_EQ(ack.order_seq, 2u);  // cancel's seq, NOT original order seq
}

void test_cancel_not_found() {
    // Cancel a non-existent order
    OrderBook book;
    std::ostringstream out;

    auto c = make_cancel(1, 1, 999);
    book.on_cancel(c, out);
    auto msgs = parse_output(out.str());

    ASSERT_EQ(msgs.size(), 1u);
    ASSERT_EQ(msgs[0].header.msg_type, MSG_REJECT);
    auto rej = extract<Reject>(msgs[0]);
    ASSERT_EQ(rej.reason, REJECT_ORDER_NOT_FOUND);
    ASSERT_EQ(rej.order_seq, 1u);
}

void test_cancel_after_full_fill() {
    // Place order, fill it completely, then try to cancel → ORDER_NOT_FOUND
    OrderBook book;
    std::ostringstream out;

    book.on_new_order(make_limit(1, 1, SIDE_BUY, 100, 5), out);
    book.on_new_order(make_limit(2, 1, SIDE_SELL, 100, 5), out);
    out.str(""); out.clear();

    book.on_cancel(make_cancel(3, 1, 1), out);  // order 1 is gone
    auto msgs = parse_output(out.str());

    ASSERT_EQ(msgs.size(), 1u);
    ASSERT_EQ(msgs[0].header.msg_type, MSG_REJECT);
    auto rej = extract<Reject>(msgs[0]);
    ASSERT_EQ(rej.reason, REJECT_ORDER_NOT_FOUND);
}

void test_reject_invalid_qty() {
    // quantity == 0 → reject
    OrderBook book;
    std::ostringstream out;

    auto o = make_limit(1, 1, SIDE_BUY, 100, 0);
    book.on_new_order(o, out);
    auto msgs = parse_output(out.str());

    ASSERT_EQ(msgs.size(), 1u);
    ASSERT_EQ(msgs[0].header.msg_type, MSG_REJECT);
    auto rej = extract<Reject>(msgs[0]);
    ASSERT_EQ(rej.reason, REJECT_INVALID_QTY);
}

void test_reject_invalid_price_limit() {
    // LIMIT with price=0 → reject
    OrderBook book;
    std::ostringstream out;

    auto o = make_limit(1, 1, SIDE_BUY, 0, 5);
    book.on_new_order(o, out);
    auto msgs = parse_output(out.str());

    ASSERT_EQ(msgs.size(), 1u);
    ASSERT_EQ(msgs[0].header.msg_type, MSG_REJECT);
    auto rej = extract<Reject>(msgs[0]);
    ASSERT_EQ(rej.reason, REJECT_INVALID_PRICE);
}

void test_reject_duplicate_seq() {
    // Same seq twice → second one rejected
    OrderBook book;
    std::ostringstream out;

    book.on_new_order(make_limit(1, 1, SIDE_BUY, 100, 5), out);
    out.str(""); out.clear();

    book.on_new_order(make_limit(1, 1, SIDE_BUY, 101, 3), out);
    auto msgs = parse_output(out.str());

    ASSERT_EQ(msgs.size(), 1u);
    ASSERT_EQ(msgs[0].header.msg_type, MSG_REJECT);
    auto rej = extract<Reject>(msgs[0]);
    ASSERT_EQ(rej.reason, REJECT_DUPLICATE_SEQ);
}

void test_engine_seq_monotonic() {
    // Engine seq should be monotonically increasing across all emitted messages
    OrderBook book;
    std::ostringstream out;

    book.on_new_order(make_limit(1, 1, SIDE_BUY, 100, 5), out);
    book.on_new_order(make_limit(2, 1, SIDE_SELL, 100, 5), out);

    auto msgs = parse_output(out.str());
    uint64_t prev_seq = 0;
    for (const auto& m : msgs) {
        uint64_t seq = 0;
        std::memcpy(&seq, m.payload.data(), sizeof(seq));
        ASSERT_TRUE(seq > prev_seq);
        prev_seq = seq;
    }
}

void test_determinism() {
    // Two runs with identical input must produce byte-identical output.
    auto run_once = []() -> std::string {
        OrderBook book;
        std::ostringstream out;

        book.on_new_order(make_limit(1, 1, SIDE_BUY, 100, 10), out);
        book.on_new_order(make_limit(2, 1, SIDE_BUY, 99, 5), out);
        book.on_new_order(make_limit(3, 1, SIDE_SELL, 100, 7), out);
        book.on_new_order(make_market(4, 1, SIDE_SELL, 20), out);
        book.on_cancel(make_cancel(5, 1, 2), out);
        book.on_new_order(make_limit(6, 1, SIDE_SELL, 98, 3), out);

        return out.str();
    };

    std::string a = run_once();
    std::string b = run_once();
    ASSERT_EQ(a.size(), b.size());
    ASSERT_TRUE(a == b);
}

void test_maker_before_taker_ordering() {
    // Contract invariant #5: maker fill emitted before taker fill.
    // Verify fill ordering across multiple price levels.
    OrderBook book;
    std::ostringstream out;

    book.on_new_order(make_limit(1, 1, SIDE_SELL, 100, 3), out);
    book.on_new_order(make_limit(2, 1, SIDE_SELL, 101, 3), out);
    out.str(""); out.clear();

    // Aggressive BUY sweeps both levels
    book.on_new_order(make_limit(3, 1, SIDE_BUY, 101, 6), out);
    auto msgs = parse_output(out.str());

    // Ack + (maker fill @100, taker fill @100) + (maker fill @101, taker fill @101)
    ASSERT_EQ(msgs.size(), 5u);
    // First pair: @100
    ASSERT_EQ(msgs[1].header.msg_type, MSG_FILL);
    ASSERT_EQ(msgs[2].header.msg_type, MSG_FILL);
    auto m1 = extract<Fill>(msgs[1]);
    auto t1 = extract<Fill>(msgs[2]);
    ASSERT_EQ(m1.order_seq, 1u);  // maker
    ASSERT_EQ(t1.order_seq, 3u);  // taker
    // Second pair: @101
    auto m2 = extract<Fill>(msgs[3]);
    auto t2 = extract<Fill>(msgs[4]);
    ASSERT_EQ(m2.order_seq, 2u);  // maker
    ASSERT_EQ(t2.order_seq, 3u);  // taker
}

// ═══════════════════════════════════════════════════════════════
// MAIN
// ═══════════════════════════════════════════════════════════════

int main() {
    std::cout << "\n=== OrderBook Unit Tests ===\n\n";

    TEST(test_limit_order_ack);
    TEST(test_limit_order_match_buy_sell);
    TEST(test_partial_fill);
    TEST(test_price_time_priority);
    TEST(test_price_priority);
    TEST(test_no_cross_limit);
    TEST(test_market_order_full_fill);
    TEST(test_market_order_ioc_reject);
    TEST(test_market_order_partial_fill_then_reject);
    TEST(test_market_order_price_must_be_zero);
    TEST(test_cancel_success);
    TEST(test_cancel_not_found);
    TEST(test_cancel_after_full_fill);
    TEST(test_reject_invalid_qty);
    TEST(test_reject_invalid_price_limit);
    TEST(test_reject_duplicate_seq);
    TEST(test_engine_seq_monotonic);
    TEST(test_determinism);
    TEST(test_maker_before_taker_ordering);

    std::cout << "\n=== Results: " << tests_passed << "/" << tests_run << " passed ===\n\n";

    return (tests_passed == tests_run) ? 0 : 1;
}
