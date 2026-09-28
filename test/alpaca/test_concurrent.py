"""CONCURRENT — multiple real clients against the mount at once.

N.I.N.A. and a guiding client talk to the mount simultaneously.  Polling from
one client must not disturb the motion commanded by another, and a third must
be able to cancel it within the response budget.
"""
import time

from conftest import wait_not_slewing, wait_slewing, setup_site_time
from client import AlpacaClient


def _new(profile):
    return AlpacaClient(profile["alpaca_base"], device_number=0)


def test_polling_does_not_disturb_motion(profile, client):
    """A client polling during a goto neither cancels nor alters the motion."""
    setup_site_time(client)
    mover = _new(profile)
    canceller = _new(profile)

    mover.put_ok("slewtocoordinatesasync",
                 form={"RightAscension": "6.0", "Declination": "-60.0"})
    wait_slewing(mover)

    # poll continuously from a second client while the move runs
    saw_slewing = []
    for _ in range(6):
        saw_slewing.append(client.get_value("slewing"))
        time.sleep(0.15)

    assert any(saw_slewing), "polling client never observed the slew"
    assert mover.get_value("slewing") is True, "polling cancelled the motion"

    # a third client cancels it
    canceller.put_ok("abortslew")
    wait_not_slewing(mover)
    assert mover.get_value("slewing") is False


def test_transaction_ids_correlate(client):
    """Each request echoes its own ClientTransactionID (and does not cross)."""
    a = client.next_txn()
    b = client.next_txn()
    ra = client.get("interfaceversion", ClientID=1, ClientTransactionID=a)
    rb = client.get("interfaceversion", ClientID=1, ClientTransactionID=b)
    assert ra.client_transaction_id == a, \
        f"ClientTransactionID mismatch: sent {a}, got {ra.client_transaction_id}"
    assert rb.client_transaction_id == b, \
        f"ClientTransactionID mismatch: sent {b}, got {rb.client_transaction_id}"
    assert a != b
