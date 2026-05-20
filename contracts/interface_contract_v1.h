/**
 * interface_contract_v1.h
 * =============================================================
 * HFT Benchmarking Platform — Wire Protocol Schema
 * VERSION : 1.1 (1.0 + REJECT_MARKET_INSUFFICIENT_LIQUIDITY=6, additive only)
 * STATUS  : FROZEN
 * =============================================================
 *
 * ENDIANNESS : Little-endian only. x86-64 assumed.
 *              Do NOT use on ARM or big-endian targets.
 *
 * WIRE FORMAT: Every message on TCP/UDP is:
 *              [FrameHeader 4B][Payload N bytes]
 *              Read 4 bytes first → get msg_type + msg_len
 *              Read msg_len bytes → overlay with payload struct
 *
 * ALIGNMENT  : All structs use explicit _pad fields.
 *              __attribute__((packed)) prevents any compiler padding.
 *              uint64_t fields always land on 8-byte boundaries.
 *
 * PRICES     : Fixed-point int64_t ticks. NO floats. NO doubles.
 *              Example: 150.50 = 15050 ticks (multiply by 100)
 *
 * MARKET ORDERS: IOC (immediate-or-cancel) semantics.
 *                Engine emits Fills for whatever filled. If any qty
 *                remains unfillable, engine emits a single Reject with
 *                reason = REJECT_MARKET_INSUFFICIENT_LIQUIDITY (6),
 *                order_seq = the original NewOrder.seq.
 *
 * CANCEL ACK : Engine reuses OrderAck to confirm cancel.
 *              OrderAck.order_seq = CancelOrder.seq (NOT original order seq)
 *              Bot tracks internally: cancel_seq → original_order_seq.
 *              On cancel ack (OrderAck where order_seq ∈ cancel_map):
 *                remove pending_orders[cancel_map[order_seq]], then erase cancel_map entry.
 *              Cancel fail → Reject with reason = ORDER_NOT_FOUND
 *
 * DO NOT MODIFY THIS FILE AFTER FREEZE.
 * =============================================================
 */

#pragma once
#include <stdint.h>

#define SCHEMA_VERSION 1

/* ─────────────────────────────────────────────
   MSG TYPE CODES  (used in FrameHeader.msg_type)
   ───────────────────────────────────────────── */
#define MSG_NEWORDER   1
#define MSG_CANCEL     2
#define MSG_ORDERACK   3
#define MSG_FILL       4
#define MSG_REJECT     5

/* ─────────────────────────────────────────────
   SIDE
   ───────────────────────────────────────────── */
#define SIDE_BUY   0
#define SIDE_SELL  1

/* ─────────────────────────────────────────────
   ORDER TYPE
   ───────────────────────────────────────────── */
#define ORDER_LIMIT   0
#define ORDER_MARKET  1

/* ─────────────────────────────────────────────
   REJECT REASON CODES
   ───────────────────────────────────────────── */
#define REJECT_SELF_CROSS      0
#define REJECT_INVALID_PRICE   1  /* price <= 0 for LIMIT, or non-zero for MARKET */
#define REJECT_INVALID_QTY     2  /* quantity == 0 */
#define REJECT_UNKNOWN_SYMBOL  3
#define REJECT_DUPLICATE_SEQ   4  /* seq already seen — replay protection */
#define REJECT_ORDER_NOT_FOUND 5  /* cancel target doesn't exist or already fully filled */
#define REJECT_MARKET_INSUFFICIENT_LIQUIDITY 6  /* MARKET order has residual qty after matching loop;
                                                   IOC semantics — partial fills already emitted, this
                                                   reject covers only the unfillable remainder. */


/* ═════════════════════════════════════════════
   FRAME HEADER  —  4 bytes  —  EVERY message
   ═════════════════════════════════════════════
   Wire: [FrameHeader][Payload]
   Receiver: read 4B → check msg_type → read msg_len bytes → overlay struct
*/
struct __attribute__((packed)) FrameHeader {
    uint8_t  msg_type;   /* offset 0 — MSG_* constants above              */
    uint8_t  _pad;       /* offset 1 — reserved, always 0                 */
    uint16_t msg_len;    /* offset 2 — payload bytes EXCLUDING this header */
};                       /* TOTAL: 4 bytes                                 */


/* ═════════════════════════════════════════════
   1. NewOrder  —  40 bytes payload
      Direction: Bot → Engine
      FrameHeader.msg_type = MSG_NEWORDER (1)
      FrameHeader.msg_len  = 40
   ═════════════════════════════════════════════
   LIMIT  order: order_type=0, price=<ticks>, quantity=<shares>
   MARKET order: order_type=1, price=0 (engine ignores), quantity=<shares>
*/
struct __attribute__((packed)) NewOrder {
    uint64_t seq;          /* offset  0 — bot's unique message ID; echoed in all responses */
    uint64_t timestamp_ns; /* offset  8 — rdtscp() at wire send; latency baseline          */
    uint32_t symbol_id;    /* offset 16 — instrument identifier                            */
    uint8_t  order_type;   /* offset 20 — ORDER_LIMIT=0, ORDER_MARKET=1                   */
    uint8_t  side;         /* offset 21 — SIDE_BUY=0, SIDE_SELL=1                         */
    uint8_t  _pad[2];      /* offset 22 — explicit pad; aligns price to 8B boundary @ 24  */
    int64_t  price;        /* offset 24 — fixed-point ticks; MUST be 0 for MARKET orders  */
    uint64_t quantity;     /* offset 32 — shares / contracts                               */
};                         /* TOTAL: 40 bytes                                              */


/* ═════════════════════════════════════════════
   2. CancelOrder  —  32 bytes payload
      Direction: Bot → Engine
      FrameHeader.msg_type = MSG_CANCEL (2)
      FrameHeader.msg_len  = 32
   ═════════════════════════════════════════════
   Engine replies with:
     Success → OrderAck  { order_seq = THIS CancelOrder.seq }
     Failure → Reject    { order_seq = THIS CancelOrder.seq,
                           reason    = REJECT_ORDER_NOT_FOUND }
   Bot maps internally: cancel_seq → original_order_seq.
   On cancel ack (OrderAck where order_seq ∈ cancel_map):
     remove pending_orders[cancel_map[order_seq]], then erase cancel_map entry.
*/
struct __attribute__((packed)) CancelOrder {
    uint64_t seq;          /* offset  0 — unique ID for this cancel request              */
    uint64_t timestamp_ns; /* offset  8 — rdtscp() at send                               */
    uint32_t symbol_id;    /* offset 16 — engine uses for order book routing             */
    uint8_t  _pad[4];      /* offset 20 — explicit pad; aligns order_seq to 8B @ 24     */
    uint64_t order_seq;    /* offset 24 — seq of the NewOrder to cancel                  */
};                         /* TOTAL: 32 bytes                                             */


/* ═════════════════════════════════════════════
   3. OrderAck  —  32 bytes payload
      Direction: Engine → Bot
      FrameHeader.msg_type = MSG_ORDERACK (3)
      FrameHeader.msg_len  = 32
   ═════════════════════════════════════════════
   DUAL USE:
     NewOrder ack  → order_seq = NewOrder.seq
     CancelOrder ack → order_seq = CancelOrder.seq  ← NOT original order seq
   Bot must distinguish using its own cancel_seq tracking map.
*/
struct __attribute__((packed)) OrderAck {
    uint64_t seq;          /* offset  0 — engine's own monotonic counter                  */
    uint64_t timestamp_ns; /* offset  8 — moment order/cancel committed; used for latency */
    uint32_t symbol_id;    /* offset 16 — instrument                                      */
    uint8_t  _pad[4];      /* offset 20 — explicit pad                                    */
    uint64_t order_seq;    /* offset 24 — echoes NewOrder.seq OR CancelOrder.seq          */
};                         /* TOTAL: 32 bytes                                              */


/* ═════════════════════════════════════════════
   4. Fill  —  56 bytes payload
      Direction: Engine → Bot
      FrameHeader.msg_type = MSG_FILL (4)
      FrameHeader.msg_len  = 56
   ═════════════════════════════════════════════
   ONE Fill per execution event. Never batched.
   Partial fill : fill_qty < original_qty, leaves_qty > 0  → order still LIVE
   Full fill    : leaves_qty == 0  → remove from pending map immediately
*/
struct __attribute__((packed)) Fill {
    uint64_t seq;          /* offset  0 — engine sequence                                 */
    uint64_t timestamp_ns; /* offset  8 — execution timestamp; reference engine replays   */
    uint32_t symbol_id;    /* offset 16 — instrument                                      */
    uint8_t  side;         /* offset 20 — SIDE_BUY=0, SIDE_SELL=1; inline P&L, no lookup */
    uint8_t  _pad[3];      /* offset 21 — explicit pad; aligns order_seq to 8B @ 24      */
    uint64_t order_seq;    /* offset 24 — correlation to original NewOrder.seq            */
    int64_t  fill_price;   /* offset 32 — actual exec price in ticks (may differ limit)   */
    uint64_t fill_qty;     /* offset 40 — shares filled in THIS event only                */
    uint64_t leaves_qty;   /* offset 48 — remaining open qty; 0 = fully done              */
};                         /* TOTAL: 56 bytes                                              */


/* ═════════════════════════════════════════════
   5. Reject  —  32 bytes payload
      Direction: Engine → Bot
      FrameHeader.msg_type = MSG_REJECT (5)
      FrameHeader.msg_len  = 32
   ═════════════════════════════════════════════
   Covers: NewOrder reject AND CancelOrder reject (ORDER_NOT_FOUND)
   Bot removes order_seq from pending map on any Reject.
*/
struct __attribute__((packed)) Reject {
    uint64_t seq;          /* offset  0 — engine sequence                                 */
    uint64_t timestamp_ns; /* offset  8 — reject timestamp                               */
    uint32_t symbol_id;    /* offset 16 — instrument                                      */
    uint8_t  reason;       /* offset 20 — REJECT_* constants above                       */
    uint8_t  _pad[3];      /* offset 21 — explicit pad; aligns order_seq to 8B @ 24      */
    uint64_t order_seq;    /* offset 24 — echoes rejected NewOrder.seq or CancelOrder.seq */
};                         /* TOTAL: 32 bytes                                             */


/* ─────────────────────────────────────────────
   JOURNAL FORMAT  (reference engine + diff tool)
   ───────────────────────────────────────────────
   Sequentially written to file, mmap-readable.
   Each entry: [FrameHeader 4B][Payload N bytes]
   Identical to wire format — no extra wrapping.
   Reference engine writes; correctness validator mmaps and replays.
   ───────────────────────────────────────────── */


/* ─────────────────────────────────────────────
   BYTE BUDGET SUMMARY
   ───────────────────────────────────────────────
   Message       Payload   +Header   Total
   NewOrder      40B       +4B       44B
   CancelOrder   32B       +4B       36B
   OrderAck      32B       +4B       36B
   Fill          56B       +4B       60B  ← fits single 64B cache line
   Reject        32B       +4B       36B
   ───────────────────────────────────────────── */


/* ─────────────────────────────────────────────
   STATIC SIZE ASSERTIONS  —  compile-time check
   ───────────────────────────────────────────── */
#ifdef __cplusplus
  static_assert(sizeof(FrameHeader)  ==  4, "FrameHeader must be 4B");
  static_assert(sizeof(NewOrder)     == 40, "NewOrder must be 40B");
  static_assert(sizeof(CancelOrder)  == 32, "CancelOrder must be 32B");
  static_assert(sizeof(OrderAck)     == 32, "OrderAck must be 32B");
  static_assert(sizeof(Fill)         == 56, "Fill must be 56B");
  static_assert(sizeof(Reject)       == 32, "Reject must be 32B");
#else
  _Static_assert(sizeof(struct FrameHeader)  ==  4, "FrameHeader must be 4B");
  _Static_assert(sizeof(struct NewOrder)     == 40, "NewOrder must be 40B");
  _Static_assert(sizeof(struct CancelOrder)  == 32, "CancelOrder must be 32B");
  _Static_assert(sizeof(struct OrderAck)     == 32, "OrderAck must be 32B");
  _Static_assert(sizeof(struct Fill)         == 56, "Fill must be 56B");
  _Static_assert(sizeof(struct Reject)       == 32, "Reject must be 32B");
#endif
