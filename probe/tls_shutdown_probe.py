"""Probe: does a TLS peer see a clean close_notify or a connection reset?

Background
----------
CPython's ``test_asyncio.test_sslproto`` has a test,
``test_shutdown_corrupted_ssl_sends_close_notify`` (gh-98078), whose server
half calls ``sock.unwrap()`` and asserts it does *not* fail.  On Windows CI
that ``unwrap()`` raises ``ConnectionResetError: [WinError 10054]`` instead.
Until cpython#155028 that error was silently discarded by the test harness,
so nobody noticed.

The open question is *why*, and it has two very different answers:

  (a) **Platform.**  The client closes its socket while unread data is still
      sitting in its receive buffer.  TCP requires an RST in that case.  The
      client's ``close_notify`` really was sent, but the RST reaches the
      server first and Windows discards already-received data when a reset
      arrives, whereas Linux delivers the buffered bytes and only reports the
      reset on the following read.  If this is the answer, asyncio is
      blameless and the *test* needs to accept the reset on Windows.

  (b) **asyncio.**  The ``close_notify`` is genuinely never put on the wire on
      Windows.  If this is the answer, there is a real bug in CPython and the
      test is correctly red.

This probe is designed to tell (a) and (b) apart, on real Windows, using
GitHub's free runners.  It deliberately does not import anything from
CPython's test suite: each scenario is self-contained, so it runs on any
stock CPython 3.11+ and measures the mechanism rather than the test harness.

Scenarios
---------
``clean``
    Positive control.  Ordinary TLS shutdown, nothing corrupted, nothing left
    unread.  Must come back clean on every platform.  If it does not, this
    probe is broken and no other result here means anything.

``unread``
    The mechanism from (a) in isolation, with **no TLS corruption and no
    asyncio**.  The client leaves data unread in its receive buffer, sends a
    proper ``close_notify``, then closes.  If Windows reports a reset here and
    Linux does not, (a) is established without asyncio ever being involved.

``corrupt_sync``
    The corrupted-record sequence from the real test, driven by a plain
    blocking socket client instead of asyncio.  Isolates the corruption from
    asyncio.

``corrupt_asyncio``
    A faithful replication of the real test's client half, using asyncio,
    ``pause_reading()`` and all.  This is the scenario that should reproduce
    the CI failure.

Server variants
---------------
``sslsock``
    Mirrors the real test: an ``ssl``-wrapped socket calling ``unwrap()``.
    Answers *does it fail, and with what*.

``membio``
    Drives the same TLS session through ``ssl.SSLObject`` over memory BIOs, so
    every byte that arrives is visible to us before OpenSSL sees it.  Answers
    *did the peer's close_notify actually reach us* — which is precisely the
    (a)-versus-(b) discriminator.  Under TLS 1.3 a close_notify alert is
    encrypted, so it cannot be identified by looking at the raw bytes; we let
    OpenSSL classify it and report what OpenSSL concluded alongside the raw
    byte count.

Output
------
One JSON object per run on stdout, plus a human summary on stderr.  Nothing
here is a pass/fail assertion: the probe reports what happened on each
platform and leaves the conclusion to the reader.
"""

from __future__ import annotations

import argparse
import asyncio
import collections
import errno
import json
import os
import platform
import select
import socket
import ssl
import struct
import sys
import threading
import time
import traceback

HERE = os.path.dirname(os.path.abspath(__file__))
CERT = os.path.join(HERE, "certdata", "ssl_cert.pem")
KEY = os.path.join(HERE, "certdata", "ssl_key.pem")

# Sent raw, outside the TLS record layer, to corrupt the stream.  Same string
# the CPython test uses, so the corruption is byte-identical.
CORRUPTION = b"please corrupt the SSL connection"

TIMEOUT = 10.0


def server_sslcontext() -> ssl.SSLContext:
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(CERT, KEY)
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    return ctx


def client_sslcontext() -> ssl.SSLContext:
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    return ctx


def describe_exception(exc: BaseException | None) -> dict | None:
    """Reduce an exception to the fields that actually discriminate."""
    if exc is None:
        return None
    info = {
        "type": type(exc).__name__,
        "module": type(exc).__module__,
        "str": str(exc),
        "is_ssl_error": isinstance(exc, ssl.SSLError),
        "is_oserror": isinstance(exc, OSError),
    }
    if isinstance(exc, OSError):
        info["errno"] = exc.errno
        info["errno_name"] = errno.errorcode.get(exc.errno) if exc.errno else None
        # On Windows the interesting number is winerror, not errno.
        info["winerror"] = getattr(exc, "winerror", None)
    if isinstance(exc, ssl.SSLError):
        info["ssl_reason"] = getattr(exc, "reason", None)
        info["ssl_library"] = getattr(exc, "library", None)
    return info


def recv_exactly(sock: socket.socket, n: int) -> bytes:
    """Read exactly n bytes, or raise if the peer goes away first."""
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise EOFError(f"peer closed after {len(buf)} of {n} bytes")
        buf += chunk
    return buf


# ---------------------------------------------------------------------------
# Server halves
# ---------------------------------------------------------------------------


class ServerResult:
    """What the server observed.  This is the measurement."""

    def __init__(self) -> None:
        self.unwrap_error: BaseException | None = None
        self.other_error: BaseException | None = None
        self.close_notify_seen: bool | None = None
        self.raw_bytes_after_handshake: int = 0
        self.recv_error: BaseException | None = None
        self.notes: list[str] = []

    def to_dict(self) -> dict:
        return {
            "unwrap_error": describe_exception(self.unwrap_error),
            "other_error": describe_exception(self.other_error),
            "recv_error": describe_exception(self.recv_error),
            "close_notify_seen": self.close_notify_seen,
            "raw_bytes_after_handshake": self.raw_bytes_after_handshake,
            "notes": self.notes,
        }


def server_sslsock(conn: socket.socket, scenario: str, result: ServerResult) -> None:
    """Server half that mirrors the real test: ssl-wrapped socket + unwrap()."""
    ctx = server_sslcontext()
    # dup() before wrapping, exactly as the CPython test does, so we keep a
    # raw handle we can use to inject plaintext into the TLS stream.
    raw = conn.dup()
    tls = None
    try:
        tls = ctx.wrap_socket(conn, server_side=True)
        tls.sendall(b"A\n")
        recv_exactly(tls, 1)

        if scenario in ("corrupt_sync", "corrupt_asyncio"):
            raw.send(CORRUPTION)
            result.notes.append("sent %d corruption bytes" % len(CORRUPTION))
        elif scenario == "unread":
            # No corruption: just give the client data it will never read, so
            # that its close() has to emit an RST.
            tls.sendall(b"X" * 4096)
            result.notes.append("sent 4096 bytes the client will not read")

        try:
            tls.unwrap()
            result.close_notify_seen = True
        except BaseException as exc:  # noqa: BLE001 - classification is the point
            result.unwrap_error = exc
            result.close_notify_seen = False
    except BaseException as exc:  # noqa: BLE001
        result.other_error = exc
    finally:
        try:
            raw.close()
        except OSError:
            pass
        try:
            if tls is not None:
                tls.close()
            else:
                conn.close()
        except OSError:
            pass


def server_membio(conn: socket.socket, scenario: str, result: ServerResult) -> None:
    """Server half over memory BIOs, so every arriving byte is visible.

    This is what distinguishes "the close_notify never arrived" from "the
    close_notify arrived and something else went wrong afterwards".
    """
    ctx = server_sslcontext()
    incoming = ssl.MemoryBIO()
    outgoing = ssl.MemoryBIO()
    obj = ctx.wrap_bio(incoming, outgoing, server_side=True)
    raw = conn.dup()

    def flush() -> None:
        data = outgoing.read()
        if data:
            conn.sendall(data)

    def pump(want_handshake: bool = False) -> None:
        """Feed one chunk from the socket into the incoming BIO."""
        chunk = conn.recv(65536)
        if not chunk:
            incoming.write_eof()
            return
        if not want_handshake:
            result.raw_bytes_after_handshake += len(chunk)
        incoming.write(chunk)

    try:
        # Handshake.
        while True:
            try:
                obj.do_handshake()
                break
            except ssl.SSLWantReadError:
                flush()
                pump(want_handshake=True)
        flush()

        obj.write(b"A\n")
        flush()

        # Read one application byte from the client.
        while True:
            try:
                obj.read(1)
                break
            except ssl.SSLWantReadError:
                pump()

        if scenario in ("corrupt_sync", "corrupt_asyncio"):
            raw.send(CORRUPTION)
            result.notes.append("sent %d corruption bytes" % len(CORRUPTION))
        elif scenario == "unread":
            obj.write(b"X" * 4096)
            flush()
            result.notes.append("sent 4096 bytes the client will not read")

        # Now attempt the shutdown and find out what actually reaches us.
        deadline = time.monotonic() + TIMEOUT
        while True:
            try:
                obj.unwrap()
                flush()
                result.close_notify_seen = True
                break
            except ssl.SSLWantReadError:
                flush()
                if time.monotonic() > deadline:
                    result.notes.append("timed out waiting for close_notify")
                    result.close_notify_seen = False
                    break
                try:
                    chunk = conn.recv(65536)
                except BaseException as exc:  # noqa: BLE001
                    # A reset here is the headline event: it tells us the
                    # transport died before OpenSSL could classify anything.
                    result.recv_error = exc
                    result.close_notify_seen = False
                    break
                if not chunk:
                    result.notes.append("clean EOF without close_notify")
                    incoming.write_eof()
                    result.close_notify_seen = False
                    break
                result.raw_bytes_after_handshake += len(chunk)
                incoming.write(chunk)
            except BaseException as exc:  # noqa: BLE001
                result.unwrap_error = exc
                result.close_notify_seen = False
                break
    except BaseException as exc:  # noqa: BLE001
        result.other_error = exc
    finally:
        try:
            raw.close()
        except OSError:
            pass
        try:
            conn.close()
        except OSError:
            pass


SERVERS = {"sslsock": server_sslsock, "membio": server_membio}


# ---------------------------------------------------------------------------
# Client halves
# ---------------------------------------------------------------------------


def connect_tls(addr) -> ssl.SSLSocket:
    """Connect and wrap.

    Note ``wrap_socket()`` *detaches* the socket it is given: the original
    object's fd becomes -1 and every later operation must go through the
    returned ``SSLSocket``.  Getting this wrong silently turns close() into a
    no-op, which is fatal for scenarios whose whole point is how the close
    happens.
    """
    ctx = client_sslcontext()
    sock = socket.create_connection(addr, timeout=TIMEOUT)
    # The asyncio test passes server_hostname='' to suppress SNI; wrap_socket()
    # rejects the empty string, so the socket-level equivalent is None.
    return ctx.wrap_socket(sock, server_hostname=None)


def client_clean(addr) -> dict:
    """Ordinary, well-behaved TLS shutdown.  Nothing left unread."""
    tls = connect_tls(addr)
    err = None
    try:
        assert tls.recv(2) == b"A\n"
        tls.sendall(b"B")
        tls.unwrap().close()
    except BaseException as exc:  # noqa: BLE001
        err = exc
        try:
            tls.close()
        except OSError:
            pass
    return {"client_error": describe_exception(err)}


def client_unread(addr) -> dict:
    """Send close_notify, then close with data still unread in the buffer.

    This is answer (a) stripped to its bones: no corruption, no asyncio.  The
    only thing under test is what the peer sees when a socket is closed while
    unread data is still queued, which TCP requires be signalled with an RST.
    """
    tls = connect_tls(addr)
    err = None
    unread_confirmed = False
    try:
        assert tls.recv(2) == b"A\n"
        tls.sendall(b"B")
        # Wait for the server's 4 KiB to actually arrive, so that there is
        # certainly unread data in the receive buffer at close() time.
        # Without this the scenario is a race and proves nothing.
        deadline = time.monotonic() + 5.0
        while time.monotonic() < deadline:
            readable, _, _ = select.select([tls], [], [], 0.05)
            if readable:
                unread_confirmed = True
                break
        try:
            # SSL_shutdown puts close_notify on the wire on its first call and
            # only then waits for the peer's reply.  We want the send but not
            # the wait, so give it a short deadline and move on.
            tls.settimeout(1.0)
            tls.unwrap()
        except (ssl.SSLError, OSError) as exc:
            err = exc
    except BaseException as exc:  # noqa: BLE001
        err = exc
    finally:
        # Close with data still unread -> the stack must send an RST.
        try:
            tls.close()
        except OSError:
            pass
    return {
        "client_error": describe_exception(err),
        # If this is False the scenario never established the condition it
        # exists to test, and its result must be discarded.
        "unread_data_confirmed": unread_confirmed,
    }


def client_rst(addr) -> dict:
    """Negative control: force a genuine RST and check the probe reports it.

    Every other scenario here can only tell us something if this probe is
    actually able to *see* a connection reset and classify it as one.  A run
    in which nothing ever comes back as ``server_connection_reset`` is
    indistinguishable from a probe whose detection is broken, so we
    manufacture one: SO_LINGER with a zero timeout makes close() emit an RST
    rather than a FIN, by definition.
    """
    tls = connect_tls(addr)
    err = None
    try:
        assert tls.recv(2) == b"A\n"
        tls.sendall(b"B")
        tls.setsockopt(
            socket.SOL_SOCKET,
            socket.SO_LINGER,
            struct.pack("ii", 1, 0),  # linger on, timeout 0 -> RST on close
        )
    except BaseException as exc:  # noqa: BLE001
        err = exc
    finally:
        try:
            tls.close()
        except OSError:
            pass
    return {"client_error": describe_exception(err)}


def client_corrupt_sync(addr) -> dict:
    """The corrupted-record sequence, driven by a plain blocking socket."""
    tls = connect_tls(addr)
    err = None
    unread_confirmed = False
    try:
        assert tls.recv(2) == b"A\n"
        tls.sendall(b"B")
        # Let the corrupted record land in the receive buffer, unread.
        deadline = time.monotonic() + 5.0
        while time.monotonic() < deadline:
            readable, _, _ = select.select([tls], [], [], 0.05)
            if readable:
                unread_confirmed = True
                break
        try:
            # OpenSSL sends close_notify first, then fails reading the peer's
            # reply off the corrupted stream.  The send is what matters.
            tls.settimeout(1.0)
            tls.unwrap()
        except (ssl.SSLError, OSError) as exc:
            err = exc
    except BaseException as exc:  # noqa: BLE001
        err = exc
    finally:
        try:
            tls.close()
        except OSError:
            pass
    return {
        "client_error": describe_exception(err),
        "unread_data_confirmed": unread_confirmed,
    }


def client_corrupt_asyncio(addr) -> dict:
    """Faithful replication of the CPython test's asyncio client half."""

    async def main() -> dict:
        ctx = client_sslcontext()
        err = None
        reader, writer = await asyncio.open_connection(
            *addr, ssl=ctx, server_hostname=""
        )
        try:
            # Drain post-handshake data (session tickets) so that only the
            # corrupted record can be buffered next.
            line = await reader.readline()
            assert line == b"A\n", line
            writer.transport.pause_reading()
            writer.write(b"B")
            await writer.drain()
            async with asyncio.timeout(TIMEOUT):
                while not writer.transport.get_read_buffer_size():
                    await asyncio.sleep(0)
            writer.close()
            try:
                await writer.wait_closed()
            except (ssl.SSLError, OSError) as exc:
                err = exc
        except BaseException as exc:  # noqa: BLE001
            err = exc
        return {"client_error": describe_exception(err)}

    return asyncio.run(main())


CLIENTS = {
    "clean": client_clean,
    "rst": client_rst,
    "unread": client_unread,
    "corrupt_sync": client_corrupt_sync,
    "corrupt_asyncio": client_corrupt_asyncio,
}

# Scenarios whose result is meaningless unless they come out the expected way:
# they exist to show the probe can see each outcome it is asked to distinguish.
CONTROLS = {"clean": "clean_close_notify", "rst": "server_connection_reset"}


# ---------------------------------------------------------------------------
# Driver
# ---------------------------------------------------------------------------


def run_once(scenario: str, server_variant: str) -> dict:
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", 0))
    listener.listen(1)
    addr = listener.getsockname()

    result = ServerResult()
    server_fn = SERVERS[server_variant]

    def serve() -> None:
        try:
            listener.settimeout(TIMEOUT)
            conn, _ = listener.accept()
            conn.settimeout(TIMEOUT)
            server_fn(conn, scenario, result)
        except BaseException as exc:  # noqa: BLE001
            result.other_error = exc
        finally:
            try:
                listener.close()
            except OSError:
                pass

    thread = threading.Thread(target=serve, daemon=True)
    thread.start()

    started = time.monotonic()
    try:
        client_info = CLIENTS[scenario](addr)
    except BaseException as exc:  # noqa: BLE001
        client_info = {
            "client_error": describe_exception(exc),
            "client_traceback": traceback.format_exc(),
        }
    thread.join(TIMEOUT)
    elapsed = time.monotonic() - started

    record = {
        "scenario": scenario,
        "server_variant": server_variant,
        "elapsed_s": round(elapsed, 3),
        "server_thread_alive": thread.is_alive(),
    }
    record.update(client_info)
    record["server"] = result.to_dict()
    record["outcome"] = classify(record)
    return record


def classify(record: dict) -> str:
    """Reduce a run to one of a small number of named outcomes."""
    srv = record["server"]
    for key in ("unwrap_error", "recv_error", "other_error"):
        info = srv.get(key)
        if not info:
            continue
        if info.get("winerror") == 10054 or info.get("errno_name") == "ECONNRESET":
            return "server_connection_reset"
        if info["is_ssl_error"]:
            return "server_ssl_error"
        return "server_other_error:" + info["type"]
    if srv.get("close_notify_seen"):
        return "clean_close_notify"
    return "no_close_notify_no_error"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--scenario",
        action="append",
        choices=sorted(CLIENTS),
        help="scenario to run (repeatable; default: all)",
    )
    parser.add_argument(
        "--server",
        action="append",
        choices=sorted(SERVERS),
        help="server variant (repeatable; default: all)",
    )
    parser.add_argument(
        "--iterations",
        type=int,
        default=20,
        help="repetitions per cell, to measure the rate rather than assume it",
    )
    parser.add_argument("--jsonl", help="write one JSON object per run to this file")
    args = parser.parse_args(argv)

    scenarios = args.scenario or list(CLIENTS)
    variants = args.server or sorted(SERVERS)

    env = {
        "platform": platform.platform(),
        "system": platform.system(),
        "python_version": sys.version,
        "openssl_version": ssl.OPENSSL_VERSION,
        "asyncio_loop": type(asyncio.new_event_loop()).__name__,
    }
    print(json.dumps({"kind": "environment", **env}), flush=True)

    handle = open(args.jsonl, "w", encoding="utf-8") if args.jsonl else None
    tally: dict[tuple[str, str, str], int] = collections.Counter()

    try:
        for variant in variants:
            for scenario in scenarios:
                for i in range(args.iterations):
                    record = run_once(scenario, variant)
                    record["iteration"] = i
                    record["kind"] = "run"
                    line = json.dumps(record)
                    print(line, flush=True)
                    if handle:
                        handle.write(line + "\n")
                        handle.flush()
                    tally[(variant, scenario, record["outcome"])] += 1
    finally:
        if handle:
            handle.close()

    print("\n=== summary ===", file=sys.stderr)
    print(f"{env['system']} / {env['openssl_version']}", file=sys.stderr)
    for (variant, scenario, outcome), count in sorted(tally.items()):
        print(
            f"  {variant:8s} {scenario:16s} {outcome:32s} {count}/{args.iterations}",
            file=sys.stderr,
        )

    summary = {"kind": "summary", "environment": env,
               "tally": [{"server_variant": v, "scenario": s, "outcome": o,
                          "count": c, "of": args.iterations}
                         for (v, s, o), c in sorted(tally.items())]}
    print(json.dumps(summary), flush=True)

    # The probe reports; it does not judge.  Always exit 0 unless a control
    # failed, which would mean the probe itself is untrustworthy: either it
    # cannot complete an ordinary shutdown, or it cannot recognise a reset
    # when one is manufactured for it.  Both make every other row noise.
    control_failures = {}
    for (variant, scenario, outcome), count in tally.items():
        expected = CONTROLS.get(scenario)
        if expected is not None and outcome != expected:
            control_failures[(variant, scenario, outcome)] = count
    if control_failures:
        print("CONTROL FAILED — no other result in this run can be trusted:",
              file=sys.stderr)
        for (variant, scenario, outcome), count in sorted(control_failures.items()):
            print(f"  {variant} {scenario}: expected {CONTROLS[scenario]}, "
                  f"got {outcome} in {count} runs", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
