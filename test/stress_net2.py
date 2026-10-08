#!/usr/bin/env python3
"""Concurrency stress test: Alpaca (N.I.N.A.) + web (REST) blocking.

Two logical threads, each driving several concurrent connections (matching the
real clients — the web UI holds 2 REST sockets and N.I.N.A. holds 2 Alpaca
sockets, per the RUNTIME_LOOP "rest=2 alpaca=2" line):

  Thread "web"   -> N concurrent pollers of GET /api/status (1 Hz each).
  Thread "alpaca"-> N concurrent pollers replaying the N.I.N.A. request
                    sequence from the boot log (property burst + MoveAxis).
  Plus an optional loader of the ~82 KiB embedded page "/" (the large response
  that bursts the USB TX queue).

The single-TCP/IP-thread + single-httpd-task + TinyUSB-send architecture
serialises these; under load the USB TX queue used to overflow and drop frames,
forcing TCP retransmissions and ~1 s stalls.  This test measures the real
client-side latency of every call so the blocking is visible as >SLOW_MS.

Usage:
    python3 test/stress_net2.py [host] [--duration S] [--connections N]
                                [--no-motion] [--no-html] [--json out.json]
"""
import argparse
import json
import sys
import threading
import time
import http.client

HOST = "192.168.7.1"

# N.I.N.A.'s periodic property burst (order + set from the boot log).
STATUS_BURST = [
    "connected", "atpark", "athome", "tracking", "altitude", "azimuth",
    "declination", "rightascension", "siderealtime", "sideofpier", "slewing",
    "cansetguiderates", "guideraterightascension", "guideratedeclination",
    "canpulseguide", "ispulseguiding", "utcdate",
]
MANUAL_BURST = ["canslew", "atpark", "canmoveaxis?Axis=0", "axisrates?Axis=0"]

ALPACA = "http://{h}:11111/api/v1/telescope/0"
STATUS = "http://{h}/api/status"

WEB_PERIOD_S = 1.0
ALPACA_PERIOD_S = 1.2
HTML_PERIOD_S = 5.0
MOVE_RATE = 1.0     # deg/s — small so the mount oscillates within limits
SLOW_MS = 200.0

stop = threading.Event()
lock = threading.Lock()
samples = []


def record(label, path, lat):
    with lock:
        samples.append({"label": label, "path": path, "lat_ms": round(lat, 1)})
    if lat > SLOW_MS:
        print(f"[{label:5s}] SLOW {lat:7.1f} ms  {path}", flush=True)


def get(port, path, label):
    t0 = time.monotonic()
    try:
        c = http.client.HTTPConnection(HOST, port, timeout=6)
        c.request("GET", path)
        r = c.getresponse()
        r.read()
        c.close()
        record(label, path, (time.monotonic() - t0) * 1000.0)
    except Exception as exc:
        print(f"[{label:5s}] ERR {type(exc).__name__}  {path}", flush=True)


def status_worker():
    while not stop.is_set():
        get(80, "/api/status", "web")
        time.sleep(WEB_PERIOD_S)


def alpaca_worker(no_motion):
    while not stop.is_set():
        for p in STATUS_BURST + ([] if no_motion else MANUAL_BURST):
            get(11111, f"/api/v1/telescope/0/{p}", "nina")
        if not no_motion:
            t0 = time.monotonic()
            try:
                c = http.client.HTTPConnection(HOST, 11111, timeout=6)
                c.request("PUT", "/api/v1/telescope/0/moveaxis",
                          body="Axis=0&Rate=1.0&ClientID=1&ClientTransactionID=0",
                          headers={"Content-Type": "application/x-www-form-urlencoded"})
                r = c.getresponse()
                r.read()
                c.close()
                record("nina", "moveaxis", (time.monotonic() - t0) * 1000.0)
            except Exception as exc:
                print(f"[nina ] ERR {type(exc).__name__} moveaxis", flush=True)
        time.sleep(ALPACA_PERIOD_S)


def html_worker():
    while not stop.is_set():
        get(80, "/", "html")
        time.sleep(HTML_PERIOD_S)


def summarize(name, xs):
    if not xs:
        print(f"{name}: no data")
        return
    s = sorted(xs)
    n = len(s)
    def pct(p):
        return s[max(0, min(n - 1, int(round(p / 100.0 * (n - 1)))))]
    slow = sum(1 for x in s if x > SLOW_MS)
    print(f"{name}: n={n}  min={s[0]:.1f}  avg={sum(s)/n:.1f}  "
          f"p50={pct(50):.1f}  p95={pct(95):.1f}  p99={pct(99):.1f}  "
          f"max={s[-1]:.1f} ms  slow(>{SLOW_MS:.0f}ms)={slow}")


def main():
    global HOST
    ap = argparse.ArgumentParser()
    ap.add_argument("host", nargs="?", default=HOST)
    ap.add_argument("--duration", type=float, default=60.0)
    ap.add_argument("--connections", type=int, default=2)
    ap.add_argument("--no-motion", action="store_true")
    ap.add_argument("--no-html", action="store_true")
    ap.add_argument("--json", default=None)
    args = ap.parse_args()
    HOST = args.host

    print(f"host={HOST}  duration={args.duration}s  connections/thread="
          f"{args.connections}  motion={'off' if args.no_motion else 'on'}  "
          f"html={'off' if args.no_html else 'on'}")
    print(f"flag > {SLOW_MS:.0f} ms\n")

    threads = []
    for _ in range(args.connections):
        threads.append(threading.Thread(target=status_worker, daemon=True))
        threads.append(threading.Thread(target=alpaca_worker,
                                        args=(args.no_motion,), daemon=True))
    if not args.no_html:
        threads.append(threading.Thread(target=html_worker, daemon=True))

    for t in threads:
        t.start()
    try:
        time.sleep(args.duration)
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()
        time.sleep(0.3)
        print()

    with lock:
        web = [s["lat_ms"] for s in samples if s["label"] == "web"]
        nina = [s["lat_ms"] for s in samples if s["label"] == "nina"]
        html = [s["lat_ms"] for s in samples if s["label"] == "html"]
    summarize("web  ", web)
    summarize("nina ", nina)
    summarize("html ", html)

    if args.json:
        with lock:
            json.dump({"samples": samples}, open(args.json, "w"), indent=2)
        print(f"saved {len(samples)} samples -> {args.json}")


if __name__ == "__main__":
    main()
