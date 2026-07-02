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

## Next up

Week 2 continued (per blueprint): LRU eviction with a memory cap
(`unordered_map` + `std::list`, O(1) access/evict), then
`INCR`/`EXISTS`/`KEYS`/`FLUSHALL`, then starting GoogleTest for the
parser, store, and LRU.
