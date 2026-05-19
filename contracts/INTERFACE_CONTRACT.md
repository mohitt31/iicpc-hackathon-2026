# INTERFACE CONTRACT v1 — FROZEN
**HFT Benchmarking Platform — Wire Protocol**

| | |
|---|---|
| Version | 1 |
| Status | **FROZEN** |
| Endianness | Little-endian, x86-64 only |
| Header file | `contracts/interface_contract_v1.h` |

> DO NOT modify this document or the header after freeze.  
> All three developers build against this. One field change = three codebases broken.

---

## Wire Format

Every message on TCP/UDP is:

```
[ FrameHeader 4B ][ Payload N bytes ]
```

Receiver algorithm:
1. Read exactly **4 bytes** → parse FrameHeader
2. Read `msg_type` → know which struct to use
3. Read `msg_len` bytes → overlay correct struct
4. Process

---

## FrameHeader — 4 bytes (every message)

```c
struct FrameHeader {
    uint8_t  msg_type;   // offset 0
    uint8_t  _pad;       // offset 1 — always 0
    uint16_t msg_len;    // offset 2 — payload bytes, excludes this 4B header
};
```

| msg_type value | Message |
|---|---|
| 1 | NewOrder |
| 2 | CancelOrder |
| 3 | OrderAck |
| 4 | Fill |
| 5 | Reject |

---

## Message 1 — NewOrder (Bot → Engine)
**msg_type=1 · payload=40B · total on wire=44B**

```c
struct NewOrder {
    uint64_t seq;          // offset  0
    uint64_t timestamp_ns; // offset  8
    uint32_t symbol_id;    // offset 16
    uint8_t  order_type;   // offset 20 — LIMIT=0, MARKET=1
    uint8_t  side;         // offset 21 — BUY=0, SELL=1
    uint8_t  _pad[2];      // offset 22
    int64_t  price;        // offset 24 — ticks; set 0 for MARKET
    uint64_t quantity;     // offset 32
};
```

| Field | Type | Size | Offset | Purpose |
|---|---|---|---|---|
| seq | uint64_t | 8B | 0 | Bot's unique ID; echoed in all responses |
| timestamp_ns | uint64_t | 8B | 8 | rdtscp() send time; latency baseline |
| symbol_id | uint32_t | 4B | 16 | Instrument identifier |
| order_type | uint8_t | 1B | 20 | LIMIT=0, MARKET=1 |
| side | uint8_t | 1B | 21 | BUY=0, SELL=1 |
| _pad[2] | uint8_t[2] | 2B | 22 | Alignment padding |
| price | int64_t | 8B | 24 | Fixed-point ticks. **MUST be 0 for MARKET** |
| quantity | uint64_t | 8B | 32 | Shares / contracts |

---

## Message 2 — CancelOrder (Bot → Engine)
**msg_type=2 · payload=32B · total on wire=36B**

```c
struct CancelOrder {
    uint64_t seq;          // offset  0
    uint64_t timestamp_ns; // offset  8
    uint32_t symbol_id;    // offset 16
    uint8_t  _pad[4];      // offset 20
    uint64_t order_seq;    // offset 24 — seq of the NewOrder to cancel
};
```

**Cancel Response Path:**
- Success → `OrderAck` with `order_seq = this CancelOrder.seq`
- Failure → `Reject` with `order_seq = this CancelOrder.seq`, `reason = ORDER_NOT_FOUND`
- Bot maps internally: `cancel_seq → original_order_seq`

| Field | Type | Size | Offset | Purpose |
|---|---|---|---|---|
| seq | uint64_t | 8B | 0 | This cancel message's unique ID |
| timestamp_ns | uint64_t | 8B | 8 | Send timestamp |
| symbol_id | uint32_t | 4B | 16 | Book routing key |
| _pad[4] | uint8_t[4] | 4B | 20 | Alignment padding |
| order_seq | uint64_t | 8B | 24 | Seq of the NewOrder being cancelled |

---

## Message 3 — OrderAck (Engine → Bot)
**msg_type=3 · payload=32B · total on wire=36B**

```c
struct OrderAck {
    uint64_t seq;          // offset  0 — engine's own counter
    uint64_t timestamp_ns; // offset  8 — moment order committed to book
    uint32_t symbol_id;    // offset 16
    uint8_t  _pad[4];      // offset 20
    uint64_t order_seq;    // offset 24
};
```

**Dual use:**
- NewOrder ack → `order_seq = NewOrder.seq`
- CancelOrder ack → `order_seq = CancelOrder.seq` ← NOT original order seq

| Field | Type | Size | Offset | Purpose |
|---|---|---|---|---|
| seq | uint64_t | 8B | 0 | Engine monotonic counter; gaps = engine drops |
| timestamp_ns | uint64_t | 8B | 8 | Latency = this − NewOrder.timestamp_ns |
| symbol_id | uint32_t | 4B | 16 | Telemetry routing |
| _pad[4] | uint8_t[4] | 4B | 20 | Alignment padding |
| order_seq | uint64_t | 8B | 24 | Correlation key — see dual use above |

---

## Message 4 — Fill (Engine → Bot)
**msg_type=4 · payload=56B · total on wire=60B**

```c
struct Fill {
    uint64_t seq;          // offset  0
    uint64_t timestamp_ns; // offset  8
    uint32_t symbol_id;    // offset 16
    uint8_t  side;         // offset 20 — BUY=0, SELL=1
    uint8_t  _pad[3];      // offset 21
    uint64_t order_seq;    // offset 24
    int64_t  fill_price;   // offset 32
    uint64_t fill_qty;     // offset 40
    uint64_t leaves_qty;   // offset 48
};
```

- `leaves_qty > 0` → order still live in book
- `leaves_qty == 0` → order fully done; **remove from pending map**
- One Fill per execution event. Never batched.

| Field | Type | Size | Offset | Purpose |
|---|---|---|---|---|
| seq | uint64_t | 8B | 0 | Engine sequence |
| timestamp_ns | uint64_t | 8B | 8 | Execution time; reference engine replays against this |
| symbol_id | uint32_t | 4B | 16 | Telemetry routing |
| side | uint8_t | 1B | 20 | Inline P&L update without order lookup |
| _pad[3] | uint8_t[3] | 3B | 21 | Alignment padding |
| order_seq | uint64_t | 8B | 24 | Correlation to original NewOrder |
| fill_price | int64_t | 8B | 32 | Actual exec price in ticks |
| fill_qty | uint64_t | 8B | 40 | Qty filled in THIS event |
| leaves_qty | uint64_t | 8B | 48 | Remaining open qty. **0 = fully done** |

---

## Message 5 — Reject (Engine → Bot)
**msg_type=5 · payload=32B · total on wire=36B**

```c
struct Reject {
    uint64_t seq;          // offset  0
    uint64_t timestamp_ns; // offset  8
    uint32_t symbol_id;    // offset 16
    uint8_t  reason;       // offset 20
    uint8_t  _pad[3];      // offset 21
    uint64_t order_seq;    // offset 24
};
```

| reason value | Meaning |
|---|---|
| 0 | SELF_CROSS |
| 1 | INVALID_PRICE |
| 2 | INVALID_QTY |
| 3 | UNKNOWN_SYMBOL |
| 4 | DUPLICATE_SEQ |
| 5 | ORDER_NOT_FOUND (cancel target gone) |

| Field | Type | Size | Offset | Purpose |
|---|---|---|---|---|
| seq | uint64_t | 8B | 0 | Engine sequence |
| timestamp_ns | uint64_t | 8B | 8 | Reject timestamp |
| symbol_id | uint32_t | 4B | 16 | Telemetry routing |
| reason | uint8_t | 1B | 20 | Failure reason code |
| _pad[3] | uint8_t[3] | 3B | 21 | Alignment padding |
| order_seq | uint64_t | 8B | 24 | Echoes rejected NewOrder.seq or CancelOrder.seq |

---

## Byte Budget

```
Message        Payload   + Header   = Wire Total
NewOrder       40B         4B         44B
CancelOrder    32B         4B         36B
OrderAck       32B         4B         36B
Fill           56B         4B         60B   ← fits single 64B cache line
Reject         32B         4B         36B
```

---

## Common Header — Same Offset in ALL 5 Types

```
offset  0  →  seq           (uint64_t)
offset  8  →  timestamp_ns  (uint64_t)
offset 16  →  symbol_id     (uint32_t)
```

Telemetry extracts these 3 fields from any message without knowing msg_type.

---

## Journal Format

Reference engine and correctness validator use the same format as wire:

```
[FrameHeader 4B][Payload N bytes] ... sequentially written
```

mmap-readable. No extra wrapping. Validator mmaps journal and replays message by message.

---

## Developer Checklist

**Bot developer**
- `seq` is global atomic counter, not per-symbol
- MARKET orders: `price` field MUST be exactly 0
- Remove from pending map when `Fill.leaves_qty == 0`
- Remove from pending map on ANY Reject
- Track `cancel_seq → original_order_seq` internally for cancel ack correlation
- On cancel ack (OrderAck where order_seq ∈ cancel_map): remove `pending_orders[cancel_map[order_seq]]`, then erase `cancel_map` entry

**Engine developer**
- `OrderAck.order_seq` = received `NewOrder.seq` OR `CancelOrder.seq` — never transform
- `Fill.leaves_qty` = `original_qty − cumulative_filled` — must be exact
- `Fill.side` = copy from original `NewOrder.side`
- Engine `seq` counter: monotonic, global, never reset
- Cancel fail reason = `ORDER_NOT_FOUND = 5`

**Telemetry developer**
- `seq` @ 0, `timestamp_ns` @ 8, `symbol_id` @ 16 — identical in all 5 types
- Latency = `response.timestamp_ns − NewOrder.timestamp_ns`
- Correctness check: compare `fill_price + fill_qty + leaves_qty` against reference engine fills
