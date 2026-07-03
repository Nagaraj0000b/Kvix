# Kvix

> A concurrent, in-memory key-value store written from scratch in C++17 — raw
> TCP sockets, an `epoll` event loop, the Redis wire protocol (RESP), TTL
> expiry, LRU eviction, and disk persistence with crash recovery.

Because it implements the real Redis protocol, you can talk to it with the
actual `redis-cli` and drive it with the real `redis-benchmark`.

---

## Features

- **Raw TCP networking** — `socket` / `bind` / `listen` / `accept`, no framework.
- **Single-threaded `epoll` event loop** — one thread serves many clients
  concurrently with non-blocking I/O (the same model real Redis uses).
- **RESP protocol** — the Redis wire format, so `redis-cli -p 6380 ...` just works.
- **Commands** — `PING`, `SET`, `GET`, `DEL`, `EXISTS`, `EXPIRE`, `TTL`, `KEYS`,
  `INCR`, `FLUSHALL`, `SAVE`.
- **TTL / expiry** — keys can auto-expire, cleaned up both lazily (on access)
  and actively (a periodic background sweep).
- **LRU eviction** — when the store is full, the least-recently-used key is
  evicted in O(1).
- **Persistence** — an append-only file (AOF) logs every write, plus full
  snapshots; on restart the data is rebuilt from disk. Survives `kill -9`.

---

## Architecture

Requests flow top-to-bottom through five layers, each in its own folder:

```
        TCP clients (redis-cli, redis-benchmark, netcat)
                          |
                          v
   ┌──────────────────────────────────────────────┐
   │  net/         Server                           │
   │  socket setup + epoll loop + non-blocking I/O  │
   │  one buffer per connection (handles partial    │
   │  reads: a command split across packets)        │
   └──────────────────────────────────────────────┘
                          |
                          v
   ┌──────────────────────────────────────────────┐
   │  protocol/    RespParser + RespReply           │
   │  bytes  -> command (vector<string>)            │
   │  reply  -> bytes                               │
   └──────────────────────────────────────────────┘
                          |
                          v
   ┌──────────────────────────────────────────────┐
   │  command/     Command + CommandRegistry        │
   │  one class per command; registry maps a name   │
   │  to its object. Add a command = add a file.    │
   └──────────────────────────────────────────────┘
                          |
                          v
   ┌──────────────────────────────────────────────┐
   │  store/       DataStore                        │
   │  hash map (unordered_map) + TTL expiry +       │
   │  LRU eviction (map + doubly linked list)       │
   └──────────────────────────────────────────────┘
                          |
                          v
   ┌──────────────────────────────────────────────┐
   │  persistence/ Aof + Snapshot                   │
   │  AOF: log every write, replay on startup       │
   │  Snapshot: full dump; recovery = load + replay │
   └──────────────────────────────────────────────┘
```

**Request flow, end to end (`SET name nagaraj`):** the bytes arrive on a
socket the epoll loop is watching → they're appended to that connection's
buffer → `RespParser` pulls out one complete command as
`["SET", "name", "nagaraj"]` → `CommandRegistry` looks up `SetCommand` and
runs it → `DataStore` stores the value → because `SET` is a write, the command
is appended to the AOF and `fsync`'d to disk → the `+OK\r\n` reply is written
back to the socket.

```
src/
├── main.cpp                    # entry point: build a Server, run it
├── net/          Server        # sockets + epoll loop + per-client buffers
├── protocol/     RespParser    # bytes <-> command
│                 RespReply      # helpers that build reply bytes
├── command/      Command        # abstract base class + CommandContext
│                 CommandRegistry # the one dispatcher: name -> command object
│                 *Command.h      # one small class per command (11 of them)
├── store/        DataStore      # hash map + TTL + LRU, all behind methods
└── persistence/  Aof            # append-only write log
                  Snapshot       # full state dump / load
```

---

## Design decisions

**Single-threaded `epoll` event loop (not a thread per client).**
One thread asks the kernel "which of my sockets are ready right now?" and only
touches those. No locks, no race conditions, and every command runs
atomically. Network I/O — not CPU — is the bottleneck for a cache, so a thread
per client would mostly add lock contention and context-switching overhead.
This is the same reason real Redis is single-threaded. The counterpoint (a
thread pool) would help only for CPU-heavy commands, which a key-value store
doesn't have.

**TTL expiry: lazy + active, no timer per key.**
A million keys would mean a million timers. Instead: *lazy* expiration checks
whether a key is expired whenever it's accessed (and deletes it then), and an
*active* sweep periodically removes expired keys nobody has touched. Lazy alone
would leak memory for keys never read again; active alone would scan
needlessly. Together they're cheap.

**LRU eviction in O(1).**
A hash map (`key -> item`) plus a doubly linked list of keys in recency order.
Each item stores an *iterator* to its own node in the list, so moving a key to
the front (on access) or removing the least-recently-used key (from the back)
is O(1) — no scanning. `std::list` guarantees iterators stay valid across
inserts/erases elsewhere, which is what makes caching the iterator safe.

**Persistence: AOF + snapshot (durability vs. performance).**
The AOF logs every write command and `fsync`s it to disk *before* replying —
so if the client saw `+OK`, the write survives a crash (this is Redis's
`appendfsync=always` mode: safest, slowest). Snapshots are full dumps that make
recovery fast; after a snapshot the AOF is truncated, since the snapshot now
covers everything. Recovery on startup = load the snapshot, then replay the
AOF entries that came after it. This is a write-ahead log + checkpointing, the
same idea a database uses.

**Honest simplifications** (each a deliberate, defensible choice):
- LRU caps by *number of keys*, not real memory bytes (Redis tracks
  `maxmemory`). The cap is small by default so eviction is easy to observe.
- `KEYS` supports only `*` (all keys), not glob patterns like `user:*`.
- The active-expiry sweep scans all keys; Redis samples random keys to stay
  cheap at millions of keys.
- AOF replay copies the remaining buffer each step (O(n²) in file size) — fine
  for a startup-only replay at this scale.

---

## Build & run

Needs Linux (`epoll` is Linux-only; WSL2 is fine).

```bash
# Option A: one g++ command
g++ -std=c++17 -O2 -I src $(find src -name '*.cpp') -o cppcache

# Option B: CMake
cmake -B build && cmake --build build   # binary at build/cppcache

./cppcache          # listens on port 6380
```

Talk to it with the real `redis-cli`:

```bash
sudo apt-get install -y redis-tools     # if you don't have it

redis-cli -p 6380 ping                  # PONG
redis-cli -p 6380 set name nagaraj      # OK
redis-cli -p 6380 get name              # "nagaraj"
redis-cli -p 6380 expire name 60        # (integer) 1
redis-cli -p 6380 ttl name              # (integer) 60
redis-cli -p 6380 incr counter          # (integer) 1
redis-cli -p 6380 keys "*"              # 1) "name"  2) "counter"
redis-cli -p 6380 save                  # OK  (snapshot to disk)
```

**Crash recovery:** write some keys, `kill -9` the server, restart it — the
data (and any TTLs) come back, rebuilt from `cppcache.snapshot` +
`cppcache.aof`.

---

## Benchmarks

Measured with the real `redis-benchmark`, 50 concurrent connections, on WSL2:

| Command | Throughput | p50 latency |
|---|---|---|
| `GET` (read)  | **~173,600 ops/sec** | 0.12 ms |
| `SET` (write) | ~201 ops/sec         | ~239 ms |

```bash
redis-benchmark -p 6380 -t get -n 100000 -c 50 -q
redis-benchmark -p 6380 -t set -n 2000   -c 50 -q
```

The ~860x gap is the durability tradeoff, measured: the only extra work a `SET`
does versus a `GET` is `fsync`-ing the AOF to physical disk, and one `fsync`
costs several milliseconds. `GET`'s 173k ops/sec is the raw throughput of the
single-threaded epoll loop across 50 concurrent clients.

*(Note: `redis-benchmark`'s default `PING_INLINE` test uses Redis's inline
protocol, which this server doesn't accept — it only speaks RESP arrays — so
benchmark with `-t get,set,incr` rather than `ping`.)*

---

## What I'd do next

- **Configurable AOF fsync policy** (`always` vs `everysec`): batching syncs on
  a 1-second timer would push writes into the tens of thousands/sec, at the
  cost of losing at most ~1s of data on a crash — the other half of the
  durability tradeoff.
- **Replication** (primary → replica): stream the write log to a follower for
  read scaling and failover.
- **More data types**: lists (`LPUSH`/`LRANGE`), hashes, sorted sets.
- **Pub/Sub** (`SUBSCRIBE`/`PUBLISH`).
- **Unit tests** (GoogleTest) for the parser, store, and LRU — each layer is
  already isolated enough to test on its own.
- **Real memory-based eviction** and glob-pattern `KEYS`.
