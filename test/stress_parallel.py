#!/usr/bin/env python3
"""Stress test: reproduce N.I.N.A. (Alpaca) + web (REST) working in parallel.

Polls the exact Alpaca properties N.I.N.A. reads (same order, same frequency),
drives MoveAxis for manual control, and polls /api/status every 1 s — all
concurrently — while measuring the real client-side response time of every call.

Any call slower than SLOW_MS is printed immediately, so a stall is visible the
moment it happens.

Usage:
    python3 test/stress_parallel.py [host]
"""
import sys
import threading
import time
import urllib.request

HOST = sys.argv[1] if len(sys.argv) > 1 else "192.168.7.1"
ALPACA = f"http://{HOST}:11111/api/v1/telescope/0"
STATUS = f"http://{HOST}/api/status"

SLOW_MS = 200.0        # flag calls slower than this
WEB_PERIOD_S = 1.0     # /api/status every 1 s
ALPACA_PERIOD_S = 2.5  # N.I.N.A. property burst every ~2.5 s (from the log)
MANUAL_PERIOD_S = 2.5  # manual-control preamble + MoveAxis every ~2.5 s

# N.I.N.A.'s periodic status burst (the properties it re-reads, in order).
STATUS_BURST = [
    "connected", "atpark", "athome", "tracking", "altitude", "azimuth",
    "declination", "rightascension", "siderealtime", "sideofpier", "slewing",
    "cansetguiderates", "guideraterightascension", "guideratedeclination",
    "canpulseguide", "ispulseguiding", "utcdate",
]

# Manual-control preamble N.I.N.A. sends before each MoveAxis.
MANUAL_BURST = ["canslew", "atpark", "canmoveaxis?Axis=0", "axisrates?Axis=0"]

stop = threading.Event()
lock = threading.Lock()
stats = {"status": [], "alpaca": [], "manual": []}


def timed(url, method="GET", form=None):
    body = None
    if form is not None:
        body = "&".join(f"{k}={v}" for k, v in form.items()).encode()
    req = urllib.request.Request(url, data=body, method=method)
    if form is not None:
        req.add_header("Content-Type", "application/x-www-form-urlencoded")
    t0 = time.monotonic()
    try:
        with urllib.request.urlopen(req, timeout=5) as r:
            r.read()
        return (time.monotonic() - t0) * 1000.0, None
    except Exception as exc:
        return None, type(exc).__name__


def record(key, label, ms, err):
    if err is not None:
        print(f"[{label}] ERROR {err}")
        return
    with lock:
        stats[key].append(ms)
    if ms > SLOW_MS:
        print(f"[{label}] SLOW {ms:7.1f} ms")


def web_poller():
    while not stop.is_set():
        ms, err = timed(STATUS)
        record("status", "web ", ms, err)
        time.sleep(WEB_PERIOD_S)


def alpaca_poller():
    while not stop.is_set():
        for prop in STATUS_BURST:
            ms, err = timed(f"{ALPACA}/{prop}")
            record("alpaca", "nina", ms, err)
        time.sleep(ALPACA_PERIOD_S)


def manual_driver():
    rate = 6.0
    while not stop.is_set():
        for prop in MANUAL_BURST:
            ms, err = timed(f"{ALPACA}/{prop}")
            record("manual", "man ", ms, err)
        ms, err = timed(f"{ALPACA}/moveaxis", "PUT",
                        {"Axis": "0", "Rate": str(rate),
                         "ClientID": "1", "ClientTransactionID": "0"})
        record("manual", "man ", ms, err)
        rate = -rate
        time.sleep(MANUAL_PERIOD_S)


def summarize(name, key):
    xs = stats[key]
    if not xs:
        print(f"{name}: no data")
        return
    slow = [x for x in xs if x > SLOW_MS]
    print(f"{name}: n={len(xs)}  min={min(xs):.1f}  avg={sum(xs)/len(xs):.1f}  "
          f"max={max(xs):.1f} ms  slow(>{SLOW_MS:.0f}ms)={len(slow)}")


def main():
    print(f"web status every {WEB_PERIOD_S}s | N.I.N.A. burst every "
          f"{ALPACA_PERIOD_S}s | manual MoveAxis every {MANUAL_PERIOD_S}s")
    print(f"flagging > {SLOW_MS:.0f} ms.  Ctrl-C to stop.\n")

    threads = [
        threading.Thread(target=web_poller, daemon=True),
        threading.Thread(target=alpaca_poller, daemon=True),
        threading.Thread(target=manual_driver, daemon=True),
    ]
    for t in threads:
        t.start()

    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()
        timed(f"{ALPACA}/moveaxis", "PUT",
              {"Axis": "0", "Rate": "0.0", "ClientID": "1",
               "ClientTransactionID": "0"})
        time.sleep(0.5)
        print()
        summarize("web status ", "status")
        summarize("nina burst ", "alpaca")
        summarize("manual     ", "manual")


if __name__ == "__main__":
    main()
