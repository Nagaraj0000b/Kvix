# CppCache — In-Memory Key-Value Store (mini Redis)

**Goal:** one deep, defensible backend project that doubles as revision for OS, CN, DBMS, OOP/SOLID.
**Timeline:** ~3 weeks build + 1 week harden & prepare interview answers.
**Language:** C++17.

---

## 1. What it is (one-line pitch)

> A concurrent, in-memory key-value store written from scratch in C++ — raw TCP networking, an `epoll` event loop, a Redis-style wire protocol, TTL expiry, LRU eviction, and disk persistence with crash recovery.

That single sentence hits networking, concurrency, DBMS, and data structures. Memorize it.

---

## 2. Tech stack & tools

| Concern | Choice | Why |
|---|---|---|
| Language | C++17 | Your strength; manual memory = good interview material |
| Build | CMake | Industry standard; interviewers expect it |
| Networking | POSIX sockets + `epoll` (Linux) | The whole point — no framework hiding it |
| Concurrency | `epoll` event loop (primary) + optional thread pool | The core CN/OS story |
| Testing | GoogleTest (gtest) | Shows you test; great talking point |
| Benchmarking | `redis-benchmark` tool OR your own client | Proves it actually works under load |
| Version control | Git + GitHub, meaningful commits | Interviewers read your commit history |
| Client for testing | `redis-cli` (works if you follow RESP), `netcat`, `telnet` | Free manual testing |
| Docs | README + architecture diagram | Where most students lose the interview |

**Platform:** build on Linux (WSL2 if you're on Windows). `epoll` is Linux-only — that's fine and expected.

---

## 3. Architecture (the mental model)

```
        TCP clients (redis-cli, netcat, your bench tool)
                          |
                          v
   ┌──────────────────────────────────────────────┐
   │  NETWORK LAYER                                 │
   │  - Socket setup (socket/bind/listen)           │
   │  - epoll event loop (accept + read/write)      │
   │  - Non-blocking I/O, per-connection buffers    │
   └──────────────────────────────────────────────┘
                          |
                          v
   ┌──────────────────────────────────────────────┐
   │  PROTOCOL LAYER                                │
   │  - RESP parser (bytes -> Command object)       │
   │  - RESP serializer (Reply -> bytes)            │
   └──────────────────────────────────────────────┘
                          |
                          v
   ┌──────────────────────────────────────────────┐
   │  COMMAND LAYER  (Command pattern → SOLID)      │
   │  - Dispatch: GET/SET/DEL/EXPIRE/...            │
   │  - Each command = one class, one responsibility│
   └──────────────────────────────────────────────┘
                          |
                          v
   ┌──────────────────────────────────────────────┐
   │  STORAGE ENGINE                                │
   │  - Hash map (key -> value + metadata)          │
   │  - TTL / expiry (lazy + active)                │
   │  - LRU eviction (hashmap + doubly linked list) │
   └──────────────────────────────────────────────┘
                          |
                          v
   ┌──────────────────────────────────────────────┐
   │  PERSISTENCE LAYER                             │
   │  - Append-Only File (AOF): log every write     │
   │  - Snapshot: dump full state to disk           │
   │  - Recovery: rebuild store on startup          │
   └──────────────────────────────────────────────┘
```

Draw this by hand once. If you can redraw it on a whiteboard in the interview, you've already won.

---

## 4. Features

### Core (must build — this is your whole project)

1. **TCP server** — accept multiple clients concurrently.
2. **Commands:** `PING`, `SET`, `GET`, `DEL`, `EXISTS`, `EXPIRE`, `TTL`, `KEYS`, `INCR`, `FLUSHALL`.
3. **RESP protocol** — so `redis-cli` can talk to your server (huge credibility signal).
4. **TTL / expiry** — keys auto-die after a timeout (lazy + active expiration).
5. **LRU eviction** — when memory cap hit, evict least-recently-used key in O(1).
6. **Persistence** — AOF (log writes) + snapshot, and **recovery on restart**.
7. **Concurrency** — single-threaded `epoll` event loop (like real Redis).

### Stretch (only if ahead of schedule — pick 1, don't chase all)

- `EXPIRE`-based pub/sub OR a simple `SUBSCRIBE`/`PUBLISH`.
- More types: list (`LPUSH`/`RPUSH`/`LRANGE`) or sorted set (skiplist — advanced).
- A thread-pool variant to *compare* against the event loop (great for the "why single-threaded?" question).
- Simple master → replica replication (advanced, big talking point if done).

> Discipline rule: a finished 7-feature project beats a half-built 15-feature one. Stop adding features in Week 3.

---

## 5. The RESP protocol (what to actually parse)

RESP is how the client encodes a command. Example — `SET name nagaraj` arrives on the socket as:

```
*3\r\n$3\r\nSET\r\n$4\r\nname\r\n$7\r\nnagaraj\r\n
```

Decode:
- `*3` → array of 3 elements (the command + 2 args)
- `$3\r\nSET` → a bulk string of length 3 = "SET"
- `$4\r\nname` → length 4 = "name"
- `$7\r\nnagaraj` → length 7 = "nagaraj"

Reply types you send back:
- `+OK\r\n` — simple string
- `-ERR message\r\n` — error
- `:1000\r\n` — integer
- `$5\r\nhello\r\n` — bulk string
- `$-1\r\n` — nil (key not found)

Implementing RESP is the single highest-leverage move: it lets you test with the **real `redis-cli`**, and "I implemented the Redis wire protocol" sounds far stronger than "I made up a text format."

---

## 6. Module / file structure

```
cppcache/
├── CMakeLists.txt
├── README.md
├── docs/
│   └── architecture.png        # your hand-drawn diagram, photographed
├── src/
│   ├── main.cpp                # entry point, config, starts server
│   ├── net/
│   │   ├── Server.h / .cpp     # socket setup + epoll loop
│   │   └── Connection.h/.cpp   # per-client buffer + state
│   ├── protocol/
│   │   ├── RespParser.h/.cpp   # bytes -> Command
│   │   └── RespWriter.h/.cpp   # Reply -> bytes
│   ├── command/
│   │   ├── Command.h           # abstract base (SOLID: interface)
│   │   ├── CommandFactory.h/.cpp
│   │   ├── GetCommand.cpp
│   │   ├── SetCommand.cpp
│   │   └── ...                 # one file per command
│   ├── store/
│   │   ├── DataStore.h/.cpp    # the hash map + value metadata
│   │   ├── Expiry.h/.cpp       # TTL logic
│   │   └── LruCache.h/.cpp     # eviction (map + linked list)
│   └── persistence/
│       ├── Aof.h/.cpp          # append-only log
│       └── Snapshot.h/.cpp     # dump / load full state
└── tests/
    ├── test_resp.cpp
    ├── test_store.cpp
    └── test_lru.cpp
```

This structure *is* your SOLID story: each folder = one responsibility, commands are open for extension (add a file) but closed for modification (don't touch the dispatcher).

---

## 7. Component deep-dive (design decisions to know cold)

**Storage engine.** `std::unordered_map<std::string, Entry>` where `Entry { std::string value; optional<time_point> expiry; }`. O(1) average get/set. Know the answer to "what if a key is huge / map rehashes / hash collisions?"

**Expiry — lazy + active.**
- *Lazy:* on every `GET`, check if expired; if so delete and return nil.
- *Active:* a periodic sweep samples random keys and removes expired ones (Redis does exactly this — cite it).
- Interview Q: "why not a timer per key?" → millions of timers = wasteful; sampling is the real-world tradeoff.

**LRU eviction — O(1).** `unordered_map<key, list_iterator>` + `std::list<key>` (doubly linked list). On access, move node to front; on evict, pop from back. This is a classic interview problem *and* a real feature — double value.

**Concurrency — single-threaded event loop.** One thread, `epoll` watches all sockets, handle whatever is ready. No locks, no race conditions. **This is the most important design answer you'll give.** Real Redis is single-threaded for the same reason: avoids lock contention, keeps operations atomic, and network I/O — not CPU — is the bottleneck. Have the thread-pool tradeoff ready as the counterpoint.

**Persistence.**
- *AOF:* append each write command to a log file. On restart, replay the log → state rebuilt.
- *Snapshot:* periodically dump the whole map to disk (faster recovery, but you lose writes since last snapshot).
- The tradeoff (durability vs performance) is a direct DBMS-syllabus answer — this is WAL / checkpointing under a different name.

---

## 8. Week-by-week plan

**Week 1 — a server that talks.**
- Day 1–2: `socket → bind → listen → accept`, echo back whatever a client sends. Test with `netcat`.
- Day 3–4: convert to non-blocking + `epoll`; handle multiple clients in one loop.
- Day 5–7: RESP parser + `PING`, `SET`, `GET`, `DEL` backed by the hash map. **Milestone: `redis-cli -p <port> set k v` and `get k` work.**

**Week 2 — make it a real store.**
- TTL / `EXPIRE` / `TTL` (lazy + active expiry).
- LRU eviction with a memory cap.
- `INCR`, `EXISTS`, `KEYS`, `FLUSHALL`.
- Start GoogleTest: unit-test the parser, store, and LRU. **Milestone: keys expire and evict correctly under test.**

**Week 3 — durability & clean architecture.**
- AOF logging + replay on startup.
- Snapshot dump/load.
- Refactor into the SOLID command-pattern structure (if not already).
- **Milestone: kill the server mid-run, restart, data survives.**

**Week 4 — harden & prepare (do NOT skip).**
- Benchmark with `redis-benchmark` or your own client; record ops/sec.
- Write the README (see §10) + architecture diagram.
- Pre-write answers to every question in §11.
- Rehearse explaining the codebase out loud in 3 minutes.

---

## 9. Resources

**Networking (sockets, epoll):**
- *Beej's Guide to Network Programming* — the classic free socket tutorial. Search "Beej's Guide Network Programming."
- `man epoll`, `man socket` — read them; interviewers respect man-page fluency.

**Building a Redis clone (structure & ideas):**
- *Build Your Own Redis* (build-your-own.org) — a book that walks the exact architecture. Great scaffold; write the code yourself, don't copy.
- Redis official docs on RESP protocol and persistence (AOF/RDB) — search "Redis RESP protocol specification" and "Redis persistence."

**Systems fundamentals (ties to your syllabus):**
- *CS:APP (Computer Systems: A Programmer's Perspective)* — chapters on networking & concurrency. This overlaps directly with your OS/CN prep.
- MIT 6.S081 / any OS course notes for the event-loop vs thread model.

**C++ specifics:**
- `cppreference.com` for `unordered_map`, `std::list`, smart pointers, RAII.

> Read to understand the *concept*, then implement from a blank file. If you can only rebuild it by copying, you can't defend it.

---

## 10. README structure (recruiters & interviewers read this first)

```
# CppCache
One-line pitch.

## Features
Bulleted: RESP protocol, epoll event loop, TTL, LRU, AOF + snapshot persistence.

## Architecture
Embed the diagram. 3–4 sentences on request flow.

## Design decisions
- Why single-threaded event loop (with the tradeoff)
- Why AOF + snapshot (durability vs speed)
- LRU in O(1)

## Build & run
cmake commands + how to connect with redis-cli.

## Benchmarks
Your ops/sec number. Even a rough one signals rigor.

## What I'd do next
Replication, clustering, more types. Shows you know the limits.
```

The "Design decisions" and "What I'd do next" sections are what separate you from someone who followed a tutorial.

---

## 11. Interview questions — pre-write answers for ALL of these

1. Walk me through what happens from a client sending `SET k v` to the reply. (End-to-end flow — practice this most.)
2. Why single-threaded event loop instead of a thread per client? Tradeoffs?
3. How does `epoll` work, and why is it better than `select`/`poll`?
4. How do you handle a client that sends half a command in one packet? (Partial reads → per-connection buffer.)
5. How does TTL expiry work without a timer per key?
6. Explain your LRU — why is it O(1)?
7. AOF vs snapshot — durability vs performance. What do you lose on crash with each?
8. What breaks when you have 1 million keys / 100k clients? How would you scale? (Sharding, replication.)
9. Hardest bug you hit? (Have a **real** one — a partial-read or epoll edge-triggered bug is a perfect story.)
10. How is this different from real Redis? What did you simplify?
11. How did you test it? (Unit tests + redis-cli + benchmark.)
12. If you had two more weeks, what next? (Replication is the strong answer.)

---

## 12. How to use Claude Code on this (so it survives scrutiny)

**Write yourself (interviewers WILL probe these):**
- The `epoll` loop and socket setup
- RESP parser
- LRU eviction logic
- Expiry logic
- Persistence replay

**Delegate to Claude Code:**
- CMake setup and build errors
- GoogleTest boilerplate
- Explaining `epoll` flags / socket options when stuck
- Refactoring toward SOLID once logic works
- README polish and diagram description

Rule of thumb: if you can't explain a file line-by-line, you didn't build it — rewrite it yourself. The project's only value is that you can defend every part.

---

## 13. Definition of done

- [ ] `redis-cli` connects and runs your commands
- [ ] Keys expire (TTL) and evict (LRU) correctly, verified by tests
- [ ] Kill & restart → data recovered from disk
- [ ] Benchmark number recorded in README
- [ ] Architecture diagram drawn
- [ ] All §11 questions answered in your own words, out loud, once
