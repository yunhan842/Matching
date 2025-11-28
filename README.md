# C++ Limit Order Matching Engine

A small, high-performance C++ matching engine that maintains a limit order book, matches orders, and exposes:

- A simple text protocol (L/M/C/R commands)
- Interactive “exchange shell” mode
- Logging + replay of events
- Sync and async matching engines
- Optional per-user position tracking and basic risk checks

This is meant as an educational / toy exchange core, not production infrastructure.

---

## Features

**Core matching**

- Multiple symbols (one `OrderBook` per symbol)
- Limit and market orders
- Price–time priority
- Partial fills & multiple-match sweeps
- Top-of-book queries (best bid/ask, sizes, mid)

**Order semantics**

- `GFD` (Good For Day) – resting orders
- `IOC` (Immediate-Or-Cancel) – fill what you can, drop the rest
- `FOK` (Fill-Or-Kill) – only execute if full quantity is available, otherwise nothing
- Cancel by order ID
- Simple replace (cancel + new)

**Engine layers**

- `OrderBook` – matching logic for a single symbol
- `MatchingEngine` – manages multiple books, routes events, tracks stats
- `AsyncMatchingEngine` – single-producer / single-consumer wrapper using a lock-free SPSC queue

**I/O & tooling**

- Text protocol for driving the engine from stdin
- Interactive shell mode (you type orders, see the book update)
- Event logging to `events.log`
- Trade logging to `trades.log`
- Replay mode: feed a past session back into the engine via `--replay events.log`

**Optional per-user tracking & risk**

- Compile-time switch to enable/disable user tracking:
  ```cpp
  // in matching_engine.hpp
  #define MATCHING_ENABLE_USER_TRACKING 1
