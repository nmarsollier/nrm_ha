#!/usr/bin/env python3
"""Stress test: status polling (500 ms) vs Alpaca MoveAxis (seconds per direction).

Simulates the real scenario: the web UI polls /api/status every 500 ms while
N.I.N.A. drives MoveAxis, holding each direction for several seconds before
reversing.  Measures the status response time and prints:
  - every status request slower than SLOW_MS, immediately
  - a rolling min/avg/max summary every ~10 s
  - a final idle-vs-moving comparison

Usage:
    python3 test/stress_net.py [host]
"""
import sys
import threading
import time
import urllib.request

HOST = sys.argv[1] if len(sys.argv) > 1 else "192.168.7.1"
STATUS = f"http://{HOST}/api/status"
MOVE = f"http://{HOST}:11111/api/v1/telescope/0/moveaxis"

POLL_INTERVAL_S = 0.5   # status every 500 ms
HOLD_S = 5.0            # hold each MoveAxis direction for several seconds
RATE = 6.0              # deg/s
SLOW_MS = 200.0         # flag status latencies above this

stop = threading.Event()
latencies = []          # status latencies while moving (ms)
lock = threading.Lock()


def timed_status():
    t0 = time.monotonic()
    try:
        with urllib.request.urlopen(STATUS, timeout=5) as r:
            r.read()
    except Exception as exc:
        return None, type(exc).__name__
    return (time.monotonic() - t0) * 1000.0, None


def move(axis, rate):
    body = f"Axis={axis}&Rate={rate}&ClientID=1&ClientTransactionID=0".encode()
    req = urllib.request.Request(MOVE, data=body, method="PUT")
    req.add_header("Content-Type", "application/x-www-form-urlencoded")
    try:
        with urllib.request.urlopen(req, timeout=5) as r:
            r.read()
    except Exception:
        pass


def status_poller():
    while not stop.is_set():
        ms, err = timed_status()
        if err is not None:
            print(f"[status] ERROR {err}")
        else:
            with lock:
                latencies.append(ms)
            if ms > SLOW_MS:
                print(f"[status] SLOW {ms:7.1f} ms")
        time.sleep(POLL_INTERVAL_S)


def summarize(label, xs):
    if not xs:
        print(f"{label}: no data")
        return
    print(f"{label}: n={len(xs)}  min={min(xs):.1f}  avg={sum(xs)/len(xs):.1f}  max={max(xs):.1f} ms")


def main():
    # Baseline while idle (no motion).
    print("baseline (idle, no motion)...")
    base = []
    t0 = time.time()
    while time.time() - t0 < 3:
        ms, err = timed_status()
        if err is None:
            base.append(ms)
        time.sleep(POLL_INTERVAL_S)
    summarize("baseline", base)

    print(f"\nmoving axis 0, reversing every {HOLD_S}s, "
          f"polling status every {POLL_INTERVAL_S}s")
    print(f"flagging status > {SLOW_MS:.0f} ms.  Ctrl-C to stop.\n")

    poller = threading.Thread(target=status_poller, daemon=True)
    poller.start()

    rate = RATE
    start = time.time()
    last_summary = start
    try:
        while True:
            move(0, rate)
            time.sleep(HOLD_S)
            rate = -rate
            if time.time() - last_summary >= 10:
                with lock:
                    recent = list(latencies)
                summarize("moving (rolling)", recent)
                last_summary = time.time()
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()
        move(0, 0.0)          # release the axis
        poller.join(timeout=2)
        with lock:
            final = list(latencies)
        print()
        summarize("moving (final)", final)


if __name__ == "__main__":
    main()
