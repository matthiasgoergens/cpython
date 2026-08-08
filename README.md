# TLS shutdown probe

A throwaway experiment, kept on an orphan branch of a CPython fork so it can
use GitHub's free Windows runners. It shares no history with CPython and is
not proposed for upstream.

## The question

[python/cpython#155028](https://github.com/python/cpython/pull/155028) makes
`test_asyncio`'s socket harness able to fail a test from a worker thread.
Before it, an exception raised in the client or server thread was silently
discarded. Turning that on immediately exposed a real failure on Windows CI:

```
ERROR: test_shutdown_corrupted_ssl_sends_close_notify
  File "Lib\test\test_asyncio\test_sslproto.py", line 920, in server
    sock.unwrap()
  File "Lib\ssl.py", line 1402, in unwrap
    s = self._sslobj.shutdown()
ConnectionResetError: [WinError 10054] An existing connection was forcibly
closed by the remote host
```

The test (gh-98078) asserts that a peer shutting down a *corrupted* TLS
connection still sends `close_notify`, so that the other side sees a clean TLS
EOF rather than a reset. Its server half catches only `ssl.SSLError`, so a
`ConnectionResetError` escapes rather than being recorded.

There are two very different explanations and they call for opposite fixes:

**(a) Platform.** The client closes its socket while unread data is still
queued in its receive buffer. TCP requires an RST in that case. The
`close_notify` really was sent, but the RST arrives too, and Windows discards
already-received data when a reset comes in, whereas Linux delivers the
buffered bytes and only reports the reset on the next read. If this is the
answer, asyncio is blameless and the *test* needs to accept the reset on
Windows.

**(b) asyncio.** The `close_notify` genuinely never reaches the wire on
Windows. If this is the answer there is a real bug in CPython and the test is
correctly red.

Guessing wrong in direction (a) papers over a real bug; guessing wrong in
direction (b) turns a real signal green. Hence measuring instead.

## What the probe does

`probe/tls_shutdown_probe.py` is self-contained — it imports nothing from
CPython's test suite, so it runs against any interpreter and measures the
mechanism rather than the test harness.

Scenarios, each run many times so the result is a rate rather than an
anecdote:

| scenario | what it isolates |
|---|---|
| `clean` | **control.** Ordinary TLS shutdown. Must come back clean everywhere. |
| `rst` | **control.** `SO_LINGER` with a zero timeout, which forces an RST by definition. Must be reported as a reset everywhere. |
| `unread` | Answer (a) with no corruption and no asyncio: close with unread data queued. |
| `corrupt_sync` | The corrupted-record sequence driven by a plain blocking socket. |
| `corrupt_asyncio` | Faithful replication of the real test's asyncio client half. |

Two server halves run every scenario:

- `sslsock` mirrors the real test — an `ssl`-wrapped socket calling
  `unwrap()`. Answers *does it fail, and with what*.
- `membio` drives the same session through `ssl.SSLObject` over memory BIOs,
  so every arriving byte is visible before OpenSSL sees it. Answers *did the
  peer's `close_notify` actually reach us*, which is the (a)-versus-(b)
  discriminator. Under TLS 1.3 the alert is encrypted, so it cannot be
  identified from the raw bytes; OpenSSL classifies it and the probe reports
  that alongside the raw byte count.

The two controls exist because a run in which nothing is ever reported as a
reset is indistinguishable from a probe whose detection is broken. If either
control comes out wrong the probe exits non-zero and says every other row in
that run is untrustworthy.

## Why it builds CPython

The gh-98078 fix and the test that exercises it are newer than every released
Python — the test does not exist in 3.14.6. Running `corrupt_asyncio` against
a stock interpreter would measure the *absence* of the fix, not the platform.
So the `built` job clones CPython and builds it; the `platform` job uses stock
interpreters but only for the scenarios that do not depend on the fix.

It builds the head of #155028 rather than plain `main`, because on `main` the
harness swallows the failure and the stdlib test reports success either way.

Windows is built for `Win32` as well as `x64`, since the CI job that actually
goes red is the Win32 one.

## Local validation before it was pushed

On Linux, against a build of CPython `main` (`8ed1479619a`):

- both controls behave (`clean` → clean close_notify, `rst` → reset), 5/5;
- all three real scenarios come back clean, 5/5, which matches the real stdlib
  test passing on Linux;
- both server halves agree on every scenario.

Against system Python 3.14.6, which predates the fix, `corrupt_asyncio` fails
instead — confirming the probe is sensitive to the thing it is measuring
rather than reporting clean unconditionally.

## Reading the output

One JSON object per run on stdout, a summary table on stderr. The interesting
comparison is Windows versus Linux on `unread`: if Windows reports a reset
there and Linux does not, answer (a) is established without asyncio being
involved at all.
