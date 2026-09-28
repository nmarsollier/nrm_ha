"""RESILIENCE — the mount survives a lost reply without duplicating work.

A client that sends a command and then loses the response must be able to
reconnect and read the true state; the firmware must not re-execute the
command on its own.
"""
import socket
import time

from conftest import setup_site_time, wait_not_slewing, wait_slewing
from client import AlpacaClient


def _raw_put(profile, path, form):
    """Send a raw PUT over a socket and drop the response (simulated loss)."""
    host = profile["alpaca_base"].split("//")[1].split(":")[0]
    body = "&".join(f"{k}={v}" for k, v in form.items())
    req = (f"PUT {path}?ClientID=1&ClientTransactionID=1 HTTP/1.1\r\n"
           f"Host: {host}:11111\r\n"
           "Content-Type: application/x-www-form-urlencoded\r\n"
           f"Content-Length: {len(body)}\r\n"
           "Connection: close\r\n\r\n" + body)
    s = socket.create_connection((host, 11111), timeout=5)
    s.sendall(req.encode())
    s.close()  # never read the response — the client "lost" it


def test_lost_response_goto_still_executes(profile, client):
    """A goto accepted through a lost reply still runs; reconnect shows it."""
    setup_site_time(client)
    _raw_put(profile, "/api/v1/telescope/0/slewtocoordinatesasync",
             {"RightAscension": "6.0", "Declination": "-60.0"})

    # the command was accepted server-side; a fresh client sees the real state
    time.sleep(0.5)
    assert client.get_value("slewing") is True, "goto not started despite lost reply"
    wait_not_slewing(client)
    assert client.get_value("slewing") is False


def test_moveaxis_survives_client_disconnect(profile, client):
    """MoveAxis keeps moving when the commanding client goes away.

    There is no renewal timer: the move is bounded only by the local limits.
    A fresh client sees the real state and can stop it; reconnecting does not
    restart a previous order.
    """
    mover = AlpacaClient(profile["alpaca_base"], device_number=0)
    mover.put_ok("moveaxis", form={"Axis": "0", "Rate": "1.0"})
    wait_slewing(mover)

    # a fresh client (as if the first one disconnected) sees it still moving
    fresh = AlpacaClient(profile["alpaca_base"], device_number=0)
    assert fresh.get_value("slewing") is True, "move stopped when the client left"

    fresh.put_ok("moveaxis", form={"Axis": "0", "Rate": "0.0"})
    wait_not_slewing(fresh)
    assert fresh.get_value("slewing") is False
