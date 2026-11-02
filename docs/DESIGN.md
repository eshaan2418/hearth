# hearth design

## Goals

- Serve static files over HTTP/1.1 at the throughput nginx reaches, from about 1,300 lines of C++20 (library
  and tools) with no dependencies.
- Be strict about the protocol: a request is either clearly valid or rejected, so the parser can't
  be used for request smuggling.
- Stay up against hostile clients. That covers slowloris, clients that stop reading, path
  traversal, symlinks pointing out of the root, and oversized headers.

Non-goals are TLS, HTTP/2, dynamic content, directory listings and configuration files.

## Threads and the event loop

```
            listen fd (non-blocking, shared)
       ┌──────────────┼──────────────┐
   worker 0        worker 1  ...  worker N-1       N = threads (default ncpu/2)
   kqueue/epoll    kqueue/epoll
   conns map       conns map
   file cache      file cache
```

Each worker owns one poller (kqueue on macOS, epoll on Linux), its connections and its file
cache. Workers share nothing mutable. The only cross-thread state is a set of relaxed atomic
counters for `stats()`, plus the stop flag and deadline.

Every worker registers the listening socket. When a connection arrives, the woken workers race
in `accept()`, and the losers get `EAGAIN` and go back to waiting. That is a mild thundering
herd: with 4 workers at most 3 wake for nothing. In exchange the load balances itself with no
acceptor thread and no hand-off queue. `SO_REUSEPORT` with one listener per worker would avoid
the herd, but macOS does not load-balance connections across reuse-port sockets the way Linux does, and
this keeps one code path for both.

Interest is level-triggered. Read interest is always on. Write interest is turned on only while
a connection has output stuck behind `EAGAIN`, and turned off once its queue drains. Readiness
for writing is therefore never polled for idle connections.

## Connection state machine

A `Conn` has:
- an input buffer;
- an output queue of `Chunk`s, where each chunk is either an owned byte string (headers, small
  bodies) or a range of an open file;
- timestamps for the last activity and for when the current request head started;
- flags for a partial head, write interest, and close-after-flush;
- a count of request-body bytes still to be discarded.

```
read() until EAGAIN ──► Process: parse complete heads one by one ──► Handle ──► append to out
                              │ incomplete: start header timer                     │
                              ▼                                                    ▼
                         wait for more                                  Flush: writev / sendfile
                                                                         │ EAGAIN → want write
                                                                         ▼
                                                       queue empty & close_after → close
```

**Pipelining.** `Process` loops over every complete request in the buffer and appends each
response to the queue in order. Responses can never be reordered, because a single queue is
flushed strictly front to back. The test sends 22 pipelined requests, including a 3 MB file in
the middle, and checks the order.

**Backpressure.** `OnReadable` stops reading once 64 KB of unparsed input is buffered. The
parser rejects heads larger than 16 KB with 431, so a client can never make a connection buffer
unbounded input. Output is bounded too. File bodies are queued as `(fd, offset, length)`, not
bytes, so a pipelined burst of 1 GB requests costs a few hundred bytes per response.

**Request bodies.** A static server ignores bodies, but it still has to skip them to find the
next request. Content-Length bytes are dropped as they arrive (`body_to_discard`), and parsing
stops until they have all been consumed. Bodies above `max_body_bytes` get 413 and the
connection closes.

## Output path

`Flush` loops over the front of the queue:
- **Headers followed by a file range**: one system call. On macOS, `sendfile` takes the header
  bytes as an `sf_hdtr` header vector. On Linux, the headers are sent with `MSG_MORE` so the
  kernel coalesces them with the following `sendfile`. Without this, every response was a tiny
  header packet followed by the body. That looked like the cause of a 100 KB gap against nginx
  in an early noisy run, but an interleaved ablation against the previous commit showed no measurable
  difference (README). The change is kept because it saves a system call, not for speed.
- **Memory chunks**: a run of up to 16 consecutive memory chunks is sent with one `writev`. This
  covers pipelined small responses and error bodies.
- **File chunks**: `sendfile` loops until the range is done or the call returns `EAGAIN`. Partial
  progress is recorded in the chunk's offset and length.

Response bytes are never copied into user space for files. The `shared_ptr<OpenFile>` in a queued
chunk keeps the descriptor open even if the cache evicts the entry or the file is replaced on
disk while a transfer is in flight.

## File cache

Each worker keeps an LRU of 4,096 open files keyed by sanitised relative path. An entry holds
the fd, size, mtime, a precomputed ETag (`"mtime-size"` in hex), the `Last-Modified` string and
the MIME type. A hit costs no system calls for one second. After that, the next hit runs one
`stat()` to revalidate, and if the size or mtime changed the entry is dropped and the file
reopened. Edits therefore show up within one second, which is the same model as nginx's
`open_file_cache_valid`.

## Path safety

There are two layers, and both are needed:

1. **`SanitizePath`, which is lexical.** It percent-decodes once and rejects NUL, backslash,
   invalid escapes and any `..` segment, including the encoded `%2e%2e`. It collapses `.` and
   empty segments. Rejection is 400, not "resolve and see": a `..` is never legitimate in a
   request for a static file.
2. **`realpath` plus a prefix check, which is physical.** After the lexical check, the full path
   is resolved and must equal the root or start with `root + "/"`. The separator check matters:
   with a root of `/srv/www`, the path `/srv/www-private` shares the prefix but is outside the
   root. This layer stops symlinks inside the root that point outside it, which a lexical check
   cannot see. The response is 404, so the server does not reveal that the target exists.

The root itself is resolved once at start-up, so a root reached through a symlink works.

## Timeouts

A sweep runs every 500 ms on each worker and closes:
- a connection whose request head has been incomplete for longer than `header_timeout_ms`
  (slowloris), however much it keeps trickling;
- an idle keep-alive connection with no activity for `idle_timeout_ms`;
- a connection with queued output that has made no write progress for `idle_timeout_ms`, meaning
  the client stopped reading.

The sweep is O(connections) every 500 ms. A timer wheel would make it O(expired); that is the
first thing to change for very large connection counts.

## Graceful stop

`Stop(grace_ms)` publishes a deadline, then sets `stopping`. Each worker then:
1. stops polling the listening socket;
2. closes idle connections at the next sweep;
3. keeps flushing connections that still have queued responses;
4. exits when none remain or the deadline passes.

`Stop` joins the workers and then closes the listening socket. A test starts a 3 MB transfer,
calls `Stop` in the middle of it, and checks the client receives every byte.

## Parser

`ParseRequest` works only on a complete head: it does nothing until `\r\n\r\n` is present, so it
never has to resume. Every rejection is a deliberate decision about request smuggling:

| input | response | why |
|---|---|---|
| `Transfer-Encoding` present | 501 (400 if Content-Length is also present) | chunked request bodies are out of scope; TE+CL is the classic desynchronisation |
| duplicate or comma-joined differing `Content-Length` | 400 | front and back ends could disagree on body length |
| non-digit or overflowing `Content-Length` | 400 | |
| bare LF, obs-fold, whitespace before `:` | 400 | lenient parsers split these differently |
| HTTP/1.1 without exactly one `Host` | 400 | RFC 9112 §3.2 |
| version other than 1.0/1.1 | 505 | |
| head > 16 KB, or too many header fields | 431 (414 if the request line alone exceeds 16 KB) | bounds memory per connection |

The parser and the path sanitiser have been fuzzed with libFuzzer (ASan + UBSan) for 25
million inputs. The target checks four invariants:
- nothing crashes;
- `head_bytes` never exceeds the input, and an accepted request always has a path;
- a sanitised path always starts with `/` and never contains `/../` or `//`;
- every rejection carries a 4xx or 5xx status.
