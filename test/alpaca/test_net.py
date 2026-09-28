"""NET — Network, concurrent clients and recovery.

Real network/link loss cases require a bench fixture and are left out. REST/Alpaca
interoperability is automatable and covered here.
"""
import json
import urllib.request

import pytest

from conftest import wait_not_slewing


# ── network ───
def test_control_rest_observe_alpaca(profile, client):
    """Control via one API and observe via the other sees the same state."""
    client.put_ok("utcdate", form={"UTCDate": "2026-09-28T02:00:00"})
    client.put_ok("slewtocoordinatesasync",
                  form={"RightAscension": "6.0", "Declination": "-60.0"})

    # observe the state via REST (supervision) while Alpaca commands
    def rest_status():
        with urllib.request.urlopen(profile["rest_base"] + "/api/status", timeout=5) as r:
            return json.load(r)

    s = rest_status()
    assert s["status"] in ("slewing", "ready")

    wait_not_slewing(client)
    assert client.get_value("slewing") is False


# ── network ───
def test_slow_client_does_not_block_service(profile, client):
    """A slow client / incomplete request does not block the service.

    A prefix of a request is sent over a raw socket and cut; the service must
    then keep responding on a new connection.
    """
    import socket

    host = profile["alpaca_base"].split("//")[1].split(":")[0]
    # open a connection and send only a prefix, keeping the request incomplete
    s = socket.create_connection((host, 11111), timeout=5)
    s.sendall(b"PUT /api/v1/telescope/0/slewto")  # incompleta, queda abierta

    # the service stays healthy for a new client while the stalled one lingers
    assert client.get_value("description") is not None
    assert client.get_value("slewing") is False

    s.close()  # cleanup

