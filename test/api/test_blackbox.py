#!/usr/bin/env python3
"""REST validation for the NRM-HA mount.

This file covers the **REST entry point** (port 80). The Alpaca entry point is
covered in `test/alpaca/`. No overlap: only the REST endpoints that Alpaca does
not expose are exercised here.

No dependencies (stdlib only). No movement: only input rejection and absence
of partial effects.
"""
import json
import os
import sys
import urllib.error
import urllib.request

HOST = sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith("-") else os.environ.get("NRM_HOST", "192.168.7.1")
REST = f"http://{HOST}"

_failures = 0
_checks = 0


def record(caso, ok, detail=""):
    global _checks, _failures
    _checks += 1
    if ok:
        print(f"  ok    {caso}" + (f"  [{detail}]" if detail else ""))
    else:
        _failures += 1
        print(f"  FAIL  {caso} — {detail}")


def http(method, url, body=None, timeout=6):
    data = None if body is None else body.encode()
    req = urllib.request.Request(url, data=data, method=method,
                                 headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, r.read().decode()
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode()
    except Exception as e:
        return None, f"<{type(e).__name__}: {e}>"


def rest_get(path):
    return http("GET", REST + path)


def rest_post(path, obj):
    return http("POST", REST + path, json.dumps(obj))


def status():
    _, body = rest_get("/api/status")
    return json.loads(body)


def ra_steps():
    return status()["debug"]["ra_steps"]


def dec_steps():
    return status()["debug"]["dec_steps"]


def test_validation():
    """Invalid REST requests are rejected without partial effects."""
    print("REST input validation")
    ra0, dec0 = ra_steps(), dec_steps()

    def no_effect():
        return ra_steps() == ra0 and dec_steps() == dec0

    code, body = rest_post("/api/tracking", {"tracking": "bogus"})
    record("invalid tracking → 400", code == 400, f"HTTP {code}")

    code, body = rest_post("/api/move-axis", {"axis": "ra", "degrees": "10basura", "speed": 2})
    record("move-axis '10basura' → 400", code == 400, f"HTTP {code}")

    code, body = rest_post("/api/settings", {"lat": float('nan'), "lon": -68.8, "elevation": 750})
    record("settings lat NaN → 400", code == 400, f"HTTP {code}")

    code, body = rest_post("/api/settings", {"lat": 95, "lon": -68.8, "elevation": 750})
    record("settings lat 95 → 409", code == 409, f"HTTP {code}")

    code, body = rest_post("/api/settings", {"lat": -32.9, "lon": "basura", "elevation": 750})
    record("settings invalid lon → 400", code == 400, f"HTTP {code}")

    code, body = rest_post("/api/limits", {"ra_min": 50, "ra_max": -50})
    record("limits min>max → rechazo", code in (400, 409), f"HTTP {code}")

    record("position unchanged after rejections", no_effect(),
           f"ra_steps {ra0}->{ra_steps()} dec {dec0}->{dec_steps()}")


def main():
    print(f"NRM-HA REST validation  host={HOST}\n")
    test_validation()
    print(f"\nRESULT: {_checks} checks, {_failures} FAIL")
    return 1 if _failures else 0


if __name__ == "__main__":
    sys.exit(main())
