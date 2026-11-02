# hearth deep dive

### 1. Why one event loop per thread instead of one loop plus a worker pool?

A static file request involves almost no computation: parse the head, look up the file, queue
headers and a file range. The cost is system calls and cache misses. Handing a connection to a
pool thread means a queue, a wake-up and a cache migration on every request, and those cost
more than the request itself. With N independent loops, each connection lives on one core for
its whole life, its state stays in that core's cache, and the hot path takes no locks. This is
the structure nginx uses (worker processes) and so do most fast servers. The cost: a slow
operation in one loop stalls every connection on it. hearth's only potentially slow call is
`open`/`stat` on a cold file. The per-worker file cache keeps that off the hot path, but a cold
read from slow storage would still stall that loop (see question 9).

### 2. How can pipelined responses never come out of order?

Each connection has a single output queue. `Process` parses every complete request in the
input buffer, in order, and appends each response's chunks to the back of the queue. `Flush`
only ever writes from the front. No response is generated asynchronously, so a later request
can never finish first. The integration test sends 22 requests in one write: 20 small, a 3 MB
file, then one more small. It checks every body arrives in that order.

### 3. What is request smuggling, and which parser choices prevent it?

When a proxy and a server disagree on where one request ends, an attacker can hide a second
request inside the first one's body. The proxy forwards it as body; the server executes it as
a request. Every disagreement comes from leniency:
- two `Content-Length` headers;
- `Transfer-Encoding` together with `Content-Length`;
- a bare LF;
- a space before the colon (`Content-Length : 5`);
- obs-fold continuation lines.

hearth rejects all of them with 400 and closes the connection, rather than picking an
interpretation. The rejection is always correct: none of these appear in traffic from a
compliant client. `Transfer-Encoding` on its own gets 501, because a static server has no use
for chunked uploads. The tests include each vector, and the fuzzer checks that no input makes
the parser claim more bytes than it was given.

### 4. Why does the parser wait for the whole head instead of being incremental?

An incremental parser has to save and restore its position across reads. That is a classic
source of bugs at buffer boundaries. Waiting for `\r\n\r\n` makes the parser a pure function
from a byte string to a result, which also makes it easy to fuzz. The cost is re-scanning for
the terminator when a head arrives in pieces. Heads are capped at 16 KB and almost always
arrive in one segment, so the re-scan is negligible. It cannot be abused either: a client that
trickles a head byte by byte is closed by the header timeout.

### 5. Why reject `..` instead of resolving it?

Resolving `/a/../b` to `/b` is what a browser does to a URL. A static server that resolves dot
segments after percent-decoding can be tricked by `%2e%2e`. One that resolves before decoding
behaves differently from every proxy in front of it. No legitimate client sends `..` in a path:
browsers normalise before sending. Rejecting with 400 is safe and removes the whole class of
bugs. The lexical check is still not enough, because a symlink inside the root can point
anywhere. The second layer, `realpath` plus a check that the result is the root or under
`root + "/"`, catches that, and the test plants exactly such a symlink.

### 6. Why `sendfile`, and what does the header-vector change buy?

Without it, the kernel copies file pages into a user buffer and `write` copies them back into
socket buffers: two copies and a user-space round trip per chunk. `sendfile` moves pages from
the page cache to the socket inside the kernel. The first version sent the response headers with
`write` and then the body with `sendfile`. That costs an extra system call per response, and
with `TCP_NODELAY` the headers leave as their own small segment. The fix sends both in one
operation: `sf_hdtr` header vectors on macOS, and `MSG_MORE` on the header `send` on Linux.

I made that change because an early benchmark pass showed hearth clearly behind nginx on 100 KB
files, and the extra segment was the obvious suspect. That pass's raw output was overwritten, so
it is not cited here. Then I measured it properly: the old and new
builds, interleaved, 5 runs each. The medians were 84.7k against 82.2k req/s at 100 KB, and
126.3k against 128.6k at 1 KB, which is within run-to-run spread. The next full pass against
nginx showed the 100 KB gap gone (51.9k against 52.0k). The gap was background load, not the
syscall. Loopback TCP with `TCP_NODELAY` coalesces well enough that one extra small write per
response barely registers. The lesson is to ablate before believing a story about a number.

### 7. How do slowloris and slow-reader attacks fail?

Slowloris opens many connections and sends a byte of the request head every few seconds, so
each connection looks alive forever. hearth tracks when the current head started, not when the
last byte arrived, and closes the connection when the head is not complete within
`header_timeout_ms`, however steadily it trickles. The opposite attack requests a large file
and never reads, pinning the queued response and the open file. A connection with queued output
that has made no write progress for `idle_timeout_ms` is closed. Both cases are integration
tests with short timeouts, and the `timeouts` counter must rise.

### 8. What happens to a file that changes while it is being served?

The open descriptor is shared by the cache entry and every queued chunk through
`shared_ptr<OpenFile>`. If the file is replaced by rename, the usual way to deploy, in-flight
transfers keep reading the old inode and finish consistently. The cache notices the new file at
its next revalidation, within one second, and new requests get it. If the file is truncated in
place, `sendfile` returns fewer bytes than promised, and the client sees a short body. nginx
behaves the same way. Atomic rename is the supported deploy method.

### 9. What would you change to run this in production?

- **TLS.** It is required on the public internet. kTLS on Linux would keep `sendfile` zero-copy;
  otherwise every byte has to pass through user space for encryption.
- **Asynchronous `open`/`stat`** through a small thread pool or io_uring, so a cold file on slow
  storage cannot stall every connection on a loop.
- **A timer wheel** instead of the 500 ms O(n) sweep, once connection counts are very large.
- **`SO_REUSEPORT` listeners on Linux** to remove the accept thundering herd.
- **Access logs, metrics and precompressed `.gz`/`.br` variants**, which matter as much as raw
  speed for real sites.

### 10. How trustworthy are the benchmark numbers?

They are trustworthy as a comparison, not as absolute figures. The load generator, hearth and
nginx all ran on the same laptop, and other jobs kept the load average between about 10 and 30
throughout. Each run records its load. The defences are interleaving (hearth and nginx
alternate within every repetition, so both see the same background), medians of 5 repetitions,
and two independent passes. The passes differ noticeably, which is the honest measure of the
noise. The conclusion that holds in both is that hearth and nginx are within a few percent of
each other on 100 KB and 1 MB files. Neither is reliably ahead. Differences of 5–10% in a
single cell should not be read as real.
