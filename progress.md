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

## Next up

Wire `tryParseCommand` into the epoll server: give each client fd a
persistent `string` buffer (state that survives across multiple `read()`
calls, not just the local `char buf[1024]` used for echoing), append
incoming bytes to it, loop calling `tryParseCommand` and dispatching
`PING`/`SET`/`GET`/`DEL` against a hash map whenever it returns
`PARSE_COMPLETE`, then erase `bytesConsumed` bytes and check again in case
another full command is already queued up behind it.

Milestone: `redis-cli -p 6380 set k v` and `redis-cli -p 6380 get k` work
against the server.
