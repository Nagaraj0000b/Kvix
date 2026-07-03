# CppCache — Progress Log

Running log of what was built, what broke, and why. Written as the project
happens so the reasoning survives — this is the raw material for the
README's "Design decisions" section and for interview answers later.

---

## Milestone 1 — single-threaded blocking TCP echo server

**Built:** `socket()` → `bind()` → `listen()` → `accept()` → `read()`/`write()` loop,
one client at a time, all blocking calls. No epoll, no protocol, no classes —
just proving the raw socket plumbing works.

**Bug hit:** first version compiled and ran, but `nc localhost 6380` exited
instantly instead of connecting. The server itself looked "stuck" (blocked at
`accept()`, printed nothing), which made it look alive — but nothing was
actually listening on port 6380.

**Root cause:** `bind()` was missing entirely. On Linux, `listen()` on an
unbound socket doesn't fail — the kernel silently auto-binds it to a random
ephemeral port. So the server really was listening, just not on the port
`nc` was trying to reach. Fixed by adding the `bind(server_fd, ...)` call
between building the `sockaddr_in` and calling `listen()`.

**Verified:** `nc localhost 6380`, typed `hi`, got `hi` echoed back.

**Limitation found (the reason for Milestone 2):** with one client connected,
a *second* `nc localhost 6380` in another terminal just hangs — no
"client connected" printed, no response to anything typed. Reasoning: the
server has one thread and one blocking loop. While that thread is parked
inside the inner `read()`/`write()` loop serving client A, it can't go back
to `accept()` to pick up client B. One blocking call at a time = one client
at a time, no matter how many are waiting.

---

## Milestone 2 — epoll-based non-blocking event loop

**Why:** the fix isn't "add a thread per client" (that's the naive answer —
lock contention, context-switch overhead, doesn't scale past a few thousand
connections). Real Redis stays single-threaded and uses `epoll` instead:
one thread asks the kernel "which of all my fds are ready right now?" and
only touches the ones that are, instead of blocking on one fd and going deaf
to everything else.

**Built:**
- `server_fd` and every accepted `client_fd` set to non-blocking
  (`fcntl` + `O_NONBLOCK`).
- One `epoll` instance (`epoll_create1`) watching every fd currently open.
- `server_fd` registered with `EPOLLIN` — ready means "a connection is
  waiting in the accept queue," not "data to read."
- Main loop replaced: instead of `accept()` then an inner blocking
  read/write loop for that one client, it's now a single `epoll_wait()`
  loop. Each wakeup returns a list of *ready* fds; each one is handled
  briefly, then control returns to `epoll_wait()` — no fd can hog the
  thread anymore.

**Why drain all pending work per wakeup, instead of handling one item and
moving on:**
- On the listening socket: `epoll_wait` reports "the listener is ready,"
  not "exactly one connection is waiting." There can be several queued.
  Calling `accept()` exactly once per notification would leave the rest
  sitting in the queue until some *unrelated* event happened to wake the
  loop again — connections would stall for no reason.
- On a client fd: same logic for `read()`. Level-triggered epoll will
  re-notify if data is left unread, but it's cheaper and simpler to just
  loop `read()` until it returns `EAGAIN`/`EWOULDBLOCK` (kernel's way of
  saying "nothing more right now — not an error") than to rely on repeated
  wakeups for data that was already available.
- Net effect: each `epoll_wait()` wakeup does *all* the work it was told
  about, then goes back to waiting — no busy-looping, no starving other fds.

**Also handled:** removing a client from `epoll` (`EPOLL_CTL_DEL`) *before*
`close()`-ing its fd — once closed, that fd number can be reused instantly
by a brand-new connection, so deleting after close (or after reuse) risks
operating on the wrong fd's registration.

**Verified:** two `nc localhost 6380` sessions open at once, both echo
independently — neither blocks the other.

---

## Milestone 3 — RESP parser (standalone, no networking yet)

**Why build this in isolation first:** the parser is the piece that has to
handle a client sending half a command in one packet — interview question
#4 in the blueprint. Testing it against hardcoded strings, before it's
wired into sockets, makes that failure mode reproducible on demand instead
of something that only shows up under flaky network timing.

**Built (`test.cpp`):** `tryParseCommand(buf)` — takes the raw bytes
received so far and tries to pull one complete RESP command (an array of
bulk strings, e.g. `*3\r\n$3\r\nSET\r\n$4\r\nname\r\n$7\r\nnagaraj\r\n`) out
of the front of it. Returns one of three outcomes via a plain `int` status
code (`PARSE_INCOMPLETE` / `PARSE_COMPLETE` / `PARSE_PROTOCOL_ERROR`) —
deliberately plain `int` + struct instead of `enum class`/`std::optional`,
since the point of this project is being able to defend every construct,
not to reach for the most modern idiom available.

**The three-way result is the actual design decision here:**
- `PARSE_INCOMPLETE` — not enough bytes have arrived yet; caller should
  wait for more data from `read()` and try again later. Consumes 0 bytes.
- `PARSE_COMPLETE` — a full command was found; returns the parsed args
  *and* `bytesConsumed`, so the caller knows exactly how much to erase
  from the front of its buffer (there could be a second command already
  sitting right behind the first one).
- `PARSE_PROTOCOL_ERROR` — the bytes don't look like valid RESP at all.

**Bugs hit in the first attempt, and why they mattered:**
- `buf[i] != '\r\n'` — comparing a single `char` against a 2-character
  string literal doesn't do what it looks like; delimiter search needs
  `buf.find("\r\n", pos)`.
- `int len = buf[++i];` — this reads one raw character's ASCII value
  (e.g. `'3'` → 51), not the parsed number 3. Needed `stoi()` on the
  substring between the `$` and the `\r\n`.
- No loop over the array's elements — a command like `SET name nagaraj`
  has 3 elements; the first attempt only ever handled one pass, so it
  couldn't parse more than a single bulk string.
- A single running cursor (`pos`) is threaded through the whole function,
  advanced past each piece as it's consumed, and reused directly as
  `bytesConsumed` at the end — this is what the future epoll integration
  will use to know how much of the per-connection buffer to erase.

**Verified**, with hardcoded strings (no sockets involved):
| case | expected | got |
|---|---|---|
| full `SET name nagaraj` | `COMPLETE`, args=`["SET","name","nagaraj"]` | matched |
| value cut short mid-bulk-string | `INCOMPLETE`, 0 bytes consumed | matched |
| cut off before the array header line even finishes | `INCOMPLETE`, 0 bytes consumed | matched |
| full `PING` (no args) | `COMPLETE`, args=`["PING"]` | matched |

---

## Milestone 4 — RESP parser wired into the epoll server (redis-cli works)

**Built (`main.cpp`):** `tryParseCommand` from Milestone 3 moved into the
server and connected to a real hash map (`unordered_map<string, string>
store`) plus a dispatcher (`handleCommand`) for `PING`/`SET`/`GET`/`DEL`.

Two pieces of new state needed for this to work correctly:
- `unordered_map<int, string> clientBuffers` — one persistent buffer per
  client fd, since a command can arrive split across more than one
  `read()`. This is what `tryParseCommand`'s `PARSE_INCOMPLETE` result was
  built for back in Milestone 3.
- In the client-read branch of the epoll loop: incoming bytes are now
  appended to `clientBuffers[fd]` instead of being echoed straight back.
  Then a loop repeatedly calls `tryParseCommand` on that buffer — handling
  `PARSE_COMPLETE` by dispatching + replying + erasing the consumed bytes,
  and looping again in case a second full command is already queued up
  behind the first. `PARSE_INCOMPLETE` breaks out to wait for the next
  `read()`; `PARSE_PROTOCOL_ERROR` replies `-ERR protocol error\r\n` and
  closes the connection.

**Command replies (RESP-correct):**
- `PING` → `+PONG\r\n`
- `SET key value` → stores it, `+OK\r\n`
- `GET key` → `$<len>\r\n<value>\r\n`, or RESP nil `$-1\r\n` if missing
  (deliberately *not* an empty bulk string `$0\r\n\r\n` — those mean
  different things to a real RESP client)
- `DEL key` → `:1\r\n` if it existed, `:0\r\n` if it didn't
- unknown command → `-ERR unknown command '<name>'\r\n`

**Also fixed:** `clientBuffers.erase(fd)` added to the disconnect cleanup
path, alongside the existing `epoll_ctl(EPOLL_CTL_DEL)` + `close(fd)` —
otherwise every disconnected client leaks a stale buffer entry forever.

**Gotcha hit while testing:** a stale server process from earlier
echo-server testing was still bound to port 6380 in the background. The
first test run showed raw bytes echoed straight back unprocessed — looked
like the new dispatch code wasn't working, but it was actually just an old
binary still listening. Lesson: check `ss -ltnp | grep 6380` (or
`pkill -f ./cppcache`) before trusting a test result after rebuilding.

**Verified** with real `redis-cli`:
```
redis-cli -p 6380 ping        -> PONG
redis-cli -p 6380 set k v     -> OK
redis-cli -p 6380 get k       -> "v"
redis-cli -p 6380 del k       -> (integer) 1
redis-cli -p 6380 get k       -> (nil)
```
Also confirmed two `redis-cli` clients can run concurrently without
blocking each other — the epoll milestone still holds under the new logic.

---

## How to build and run

```
g++ -std=c++17 -Wall main.cpp -o cppcache
./cppcache
```
Leave that running in one terminal. In a second terminal:
```
redis-cli -p 6380 ping
redis-cli -p 6380 set k v
redis-cli -p 6380 get k
redis-cli -p 6380 del k
redis-cli -p 6380 get k
```
(install redis-cli first if needed: `sudo apt-get install -y redis-tools`)

Or, without installing anything, raw RESP bytes over `nc`:
```
printf '*1\r\n$4\r\nPING\r\n*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n*2\r\n$3\r\nGET\r\n$1\r\nk\r\n' | nc localhost 6380
```

> NOTE: as of Milestone 9 the code moved from a single `main.cpp` to the
> `src/` folder tree. Build with the multi-file command below (or CMake):
> ```
> g++ -std=c++17 -Wall -I src $(find src -name '*.cpp') -o cppcache
> # or:  cmake -B build && cmake --build build
> ```

To run the standalone RESP parser tests (no server involved):
```
g++ -std=c++17 -Wall test.cpp -o test_resp
./test_resp
```

If a server run seems stuck on stale behavior after a rebuild, check
nothing old is still bound to the port:
```
ss -ltnp | grep 6380
pkill -f ./cppcache
```

---

## Milestone 5 — TTL / expiry (lazy + active)

**Built (`main.cpp`):** keys can now carry an expiry. `store`'s value type
changed from plain `string` to:
```cpp
struct Entry {
    string value;
    long long expireAt;  // 0 = never expires; otherwise a unix timestamp (seconds)
};
```
plus one shared check used everywhere: `isExpired(entry)` →
`expireAt != 0 && time(nullptr) >= expireAt`.

**New commands:**
- `EXPIRE key seconds` → sets `expireAt = time(nullptr) + seconds` on an
  existing key, `:1\r\n`; `:0\r\n` if the key doesn't exist.
- `TTL key` → `:-2\r\n` if the key doesn't exist, `:-1\r\n` if it exists
  but has no TTL set, otherwise the remaining seconds as `:N\r\n`.
- `SET` now always resets `expireAt` back to `0`, so a stale TTL from a
  previous `SET` on the same key can't silently linger.

**Lazy expiration:** a key isn't removed the instant its TTL hits zero —
it just sits there until something reads it. `GET` checks `isExpired()`
before returning a value; if true, it erases the key right there and
replies nil instead. The delete only happens at the moment of access.

**Active expiration:** a periodic sweep (`sweepExpiredKeys()`) removes
expired keys even if nobody reads them again — otherwise dead keys with
no future access would sit in memory forever. This rides on the existing
epoll loop instead of a separate thread/timer: `epoll_wait`'s timeout
changed from `-1` (block forever) to `1000` ms, and whenever it returns
`0` (timed out, nothing was ready), that's the cue to sweep. Real Redis
samples random keys instead of a full scan, to stay cheap at millions of
keys — a full scan here is a documented simplification, fine at this
project's scale (interview Q10: "how is this different from real Redis").

**Why both, not just one:** lazy alone leaks memory for keys nobody reads
again; active alone (if it were the only mechanism) means scanning
constantly even when nothing expired. Together: reads stay cheap (lazy
catches the common case for free on access), and the periodic sweep mops
up the leftover "cold" keys nobody asked about.

**Non-bug hit while testing:** manually typing `set k v` / `expire k 2` /
`get k` as separate `redis-cli` invocations, `get k` sometimes came back
`(nil)` immediately, looking like a bug. It wasn't — confirmed by firing
the same sequence back-to-back with no typing delay (`SET`→`EXPIRE k
2`→`GET k` returned `v`, `TTL k` returned `2` correctly). A 2-second TTL
is shorter than the real time it takes to read output, retype, and let
`redis-cli` pay its per-invocation process-startup + TCP-connect cost —
the key really had expired for real by the time the next command landed.
Lesson: use a longer TTL (10-15s) for comfortable manual testing.

**Verified:**
```
set k v            -> OK
expire k 2          -> (integer) 1
ttl k               -> (integer) 2  (immediately after, no delay)
get k               -> "v"          (immediately after, no delay)
... wait past the TTL ...
get k               -> (nil)
ttl k               -> (integer) -2
set k2 v2 (no expire) ; ttl k2 -> (integer) -1
```

---

## Milestone 6 — LRU eviction (O(1) access + evict)

**Built (`main.cpp`):** a fixed-size cap on the store; inserting a new key
once full evicts the least-recently-used key first.

**Data structure:**
```cpp
struct Entry {
    string value;
    long long expireAt;
    list<string>::iterator lruIt;  // this key's node inside lruList
};
unordered_map<string, Entry> store;
list<string> lruList;              // front = most recently used, back = least
const size_t MAX_KEYS = 5;         // small on purpose - makes eviction observable manually
```

**Why `list<string>::iterator` gives O(1) instead of O(n):** `std::list` is
a doubly linked list, and an iterator into it is a direct handle to one
specific node (not a searchable position like a `vector` index). Because
each `Entry` caches its own node's iterator, moving a key to the front or
removing it (`lruList.erase(it)`) never has to scan the list looking for a
match — it already knows exactly where that key lives. This only works
because `std::list` guarantees other iterators stay valid when you
insert/erase elsewhere in the list (unlike `vector`, where that would
shift things and invalidate iterators/indices past the change).

**The two helpers:**
- `touch(key)` — marks a key as most-recently-used: erase its current
  node from `lruList`, push it to the front, update its cached iterator.
  Called on every `SET` of an existing key and every successful `GET`
  (reads count as "used" too, not just writes).
- `evictIfFull()` — called only when `SET` is about to insert a *brand
  new* key: if `store.size() >= MAX_KEYS`, take `lruList.back()` (the
  actual least-recently-used key), erase it from both `store` and
  `lruList`, making room before inserting the new one.

**Bug caught before it shipped:** `sweepExpiredKeys()` (the active TTL
sweep from Milestone 5) was erasing expired keys from `store` but not from
`lruList` — that would have left `lruList` holding stale keys no longer in
`store`, and `evictIfFull()` could then try to evict a key that was
already gone. Fixed by erasing from `lruList` (via the entry's `lruIt`)
alongside the `store` erase in the sweep, matching what `GET`'s lazy-expiry
path and `DEL` already do.

**Simplification, documented on purpose:** the cap counts *number of
keys*, not actual memory/byte-size like real Redis's `maxmemory`. Tracking
true byte usage would mean summing key+value lengths on every mutation;
counting keys is a fair, defensible simplification for a project at this
stage (interview Q10 territory again).

**Verified**, with `MAX_KEYS = 5`:
```
set a 1 / b 2 / c 3 / d 4 / e 5   -> all OK, store now at cap (5 keys)
get a                              -> "1"   (touches 'a' - now most recently used)
set f 6                            -> OK    (triggers eviction of the actual LRU key)
get a                              -> "1"   (survived - was touched right before eviction)
get b                              -> (nil) (evicted - was LRU after 'a' got touched)
```

---

## Milestone 7 — INCR / EXISTS / KEYS / FLUSHALL

**Built (`main.cpp`):** the remaining Week 2 commands, added to
`handleCommand` after `TTL`.

- **`EXISTS key`** — `:1` if the key exists and isn't expired, else `:0`.
  Made consistent with the other lazy-expiry paths: an expired key it
  finds gets actually erased from both `store` and `lruList`, not just
  reported as missing (same pattern `GET`/`DEL`/the sweep already follow).

- **`INCR key`** — first place user-supplied (untrusted) text gets parsed
  as a number. `stoll()` throws (`std::invalid_argument` /
  `std::out_of_range`) on non-numeric input, so the parse is wrapped in
  `try { ... } catch (...) { return "-ERR value is not an integer or out
  of range\r\n"; }`. This is different from the RESP parser's `stoll`
  calls, which only ever ran on digit-length fields already validated by
  construction. Missing key is treated as `"0"` before incrementing.
  Deliberately does **not** reset `expireAt` the way `SET` does - `INCR`
  is an in-place mutation of an existing value, not a fresh write, so an
  existing TTL survives it (matches real Redis behavior).

- **`KEYS pattern`** — simplified to only supporting `KEYS *` (list every
  live key); real glob matching (`user:*`) is out of scope for this
  project. This is the **first RESP array reply** written instead of a
  single-line one: `*N\r\n` followed by each key as a bulk string, the
  same shape the RESP *parser* already reads on the request side, just
  built in the write direction.

- **`FLUSHALL`** — `store.clear()` + `lruList.clear()`. Clearing both
  matters for the same reason as the TTL-sweep bug in Milestone 6: only
  clearing `store` would leave `lruList` full of dangling keys.

**Verified:**
```
set n 10 ; incr n              -> (integer) 11
exists n / exists ghost        -> (integer) 1 / (integer) 0
keys *                         -> n
set notanumber abc ; incr notanumber -> ERR value is not an integer or out of range
flushall ; keys *              -> empty (*0\r\n at the byte level)
set n 5 ; expire n 100 ; incr n ; ttl n -> 100 (TTL preserved across INCR)
```

---

## Milestone 8 — persistence: AOF + snapshot, crash recovery verified

**The problem this solves:** everything built through Milestone 7 lives
only in RAM. A crash or `kill -9` loses it all instantly. This is the same
problem a database's write-ahead log + checkpointing solves — this
milestone builds the same idea at a much smaller scale (blueprint §7 calls
this out directly: "WAL / checkpointing under a different name").

**Built (`main.cpp`):**
- `encodeCommand(args)` — mirrors `tryParseCommand`: encodes a command's
  args back into the same RESP array-of-bulk-strings shape. One
  length-prefixed format reused in both directions - reading commands off
  the wire, and now writing them to disk.
- `logWrite(args)` — appends the RESP-encoded command to `cppcache.aof`,
  then `fflush` + `fsync`s it to physical disk **before** the client gets
  its reply. This is "always fsync" durability: if the client saw `+OK`,
  that write is guaranteed to survive a crash. Deliberately the slowest of
  Redis's three `appendfsync` modes (`always`/`everysec`/`no`) but the
  simplest to reason about and verify - chosen on purpose for this stage,
  documented as the durability-vs-performance tradeoff it is.
- `isWriteCommand(cmd)` — only `SET`/`DEL`/`EXPIRE`/`INCR`/`FLUSHALL`
  actually mutate state, so only those get logged; reads never do.
- New `SAVE` command → `saveSnapshot()`: dumps every live (non-expired)
  key as a `[key, value, expireAt]` RESP record to `cppcache.snapshot`,
  then truncates the AOF - the snapshot now covers everything up to that
  point, so old AOF entries would just be redundant replay on top of it.
- Startup recovery, in order: `loadSnapshot()` (fast bulk restore of the
  last checkpoint) then `replayAof()` (replays whatever writes happened
  *after* that snapshot, through the normal `handleCommand` path, catching
  state up to the moment the process died).

**Subtlety that would have been a silent bug:** `loadSnapshot()` builds
`Entry` structs directly and inserts into `store`/`lruList` itself,
instead of routing restored keys through `handleCommand`'s `SET` path -
because plain `SET` always resets `expireAt` to `0`. Using it to restore
snapshot data would have silently wiped every TTL on every restart.

**Documented simplification:** both `loadSnapshot()` and `replayAof()`
call `.substr()` on the remaining buffer each loop iteration, making
replay O(n²) in file size overall. Fine for a startup-only replay at this
project's scale; would need a real cursor-based version (no copying) to
scale to a large AOF.

**Verified, in sequence, using real `kill -9` (not a clean shutdown):**
```
set a 1 ; set b 2 ; expire b 500
kill -9 <pid>                        -> process actually dead, port free
(restart)
get a -> "1"   get b -> "2"   ttl b -> 499   (AOF replay rebuilt everything)

save                                  -> OK
wc -c cppcache.aof                    -> 0 (truncated)
xxd cppcache.snapshot                 -> both keys present, b's expireAt timestamp intact

set c 3                               (written AFTER the snapshot)
kill -9 <pid>
(restart)
keys *  -> a, b, c                    (a/b from snapshot, c from AOF replay on top)
```
This satisfies the blueprint's Week 3 definition-of-done milestone: kill
the server mid-run, restart, data survives.

---

## Milestone 9 — SOLID refactor into the src/ folder structure

**Why:** everything worked as one 600-line `main.cpp`, but the whole point
of the project is being able to defend the *design*, not just the behavior.
This splits the monolith into the layered structure from blueprint §6, with
the **command pattern** as the SOLID centerpiece. No behavior changed - this
is purely structure. (Blueprint §12 explicitly lists "refactoring toward
SOLID once logic works" as a delegate-to-Claude task, unlike the core logic
which had to be written by hand first - which it was, across Milestones 1-8.)

**New layout (each folder = one responsibility - the SRP story):**
```
src/
├── main.cpp                    # 5 lines: build a Server, run it
├── net/Server.{h,cpp}          # sockets + epoll loop + per-client buffers
├── protocol/
│   ├── RespParser.{h,cpp}      # bytes <-> command (tryParseCommand + encodeCommand)
│   └── RespReply.h             # helpers to build reply bytes (integer(), bulkString(), ...)
├── command/
│   ├── Command.h               # abstract base class + CommandContext
│   ├── CommandRegistry.{h,cpp} # name -> command object; the ONLY dispatcher
│   └── <One>Command.h          # one class per command (Ping/Set/Get/Del/...11 total)
├── store/DataStore.{h,cpp}     # the hash map + TTL + LRU, all behind methods
└── persistence/
    ├── Aof.{h,cpp}             # append-only log: append/readAllCommands/reset
    └── Snapshot.{h,cpp}        # full dump save/load
```

**The command pattern = the Open/Closed Principle in action.** Every command
is a small class inheriting `Command` with two methods: `run(context, args)`
returns the reply bytes, `changesData()` says whether it should be logged to
the AOF. Adding a command = write one new header + add one `registerCommand()`
line in `CommandRegistry`'s constructor. You never touch the dispatcher or any
existing command. That's "open for extension, closed for modification" made
literal - the single strongest interview talking point in the codebase.

**Other SOLID/encapsulation wins from the split:**
- The `DataStore` class hides the map + `list` + TTL behind named methods
  (`setValue`, `getTimeToLive`, `incrementValue`, ...). The network and
  command layers can't touch the internals - they ask, they don't reach in.
- `RespReply` helpers turned unreadable byte-mashing like
  `"$" + to_string(v.size()) + "\r\n" + v + "\r\n"` into `RespReply::bulkString(v)`.
- `CommandContext` bundles `{store, snapshot, aof}` into one thing passed to
  every command - most only use `store`; `SAVE` uses all three.
- The old startup-replay / AOF-logging tangle is now clean orchestration in
  `Server`: `recoverFromDisk()` does snapshot.load -> aof.readAllCommands ->
  replay (without re-logging) -> aof.openForAppend. Commands never know they're
  being logged; `processCommand` decides that via `changesData()`.

**Manual memory note (deliberate, interview-relevant):** `CommandRegistry`
owns each command object - `new`ed in the constructor, `delete`d in the
destructor. Chose explicit new/delete over `unique_ptr` to keep the ownership
visible and the syntax beginner-plain, matching the rest of the codebase's
style. The commands are stateless singletons that live for the whole run.

**Build changed** (many files now): either
`g++ -std=c++17 -I src $(find src -name '*.cpp') -o cppcache`, or the new
`CMakeLists.txt` (`cmake -B build && cmake --build build`).

**Verified behavior-identical to the monolith**, all against the refactored
binary:
- every command: PING/SET/GET/EXISTS/DEL/INCR/EXPIRE/TTL/KEYS/FLUSHALL +
  unknown-command error
- INCR preserves an existing TTL (counter=43 with ttl still 100)
- LRU: filled to cap, touched `a`, inserted `f` -> `a` survived, `b` evicted
- crash recovery: snapshot + post-snapshot AOF write, `kill -9`, restart ->
  all keys back, TTL preserved and correctly counted down

*(`test.cpp` in the repo root still has its own standalone copy of the parser
and still compiles/runs on its own - left as-is for now; a future cleanup
could point it at `src/protocol/RespParser` and grow into real unit tests.)*

---

## Next up

Week 4 harden & prepare (per blueprint): benchmark with `redis-benchmark`
(record ops/sec), write the README (§10: features, architecture diagram,
design decisions, benchmarks, "what I'd do next"), and pre-write answers to
all 12 of §11's interview questions. GoogleTest for the parser/store/LRU is
still deferred and now easier to add, since each layer is its own unit.
