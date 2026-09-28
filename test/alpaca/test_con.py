"""CON — Discovery, connection and availability.

Cases pass by querying Alpaca, without depending on /api/status.
"""
import pytest

from client import DeviceError
from conftest import discover, current_radec


def _host(profile):
    return profile["alpaca_base"].split("//")[1].split(":")[0]


# ── connection ───
def test_discover_from_new_client(profile, client):
    """Find the mount via UDP 32227 and management/configureddevices."""
    host = _host(profile)

    disc = discover(host)
    assert "AlpacaPort" in disc, f"discovery sin AlpacaPort: {disc}"
    assert disc["AlpacaPort"] == 11111, f"AlpacaPort inesperado: {disc['AlpacaPort']}"

    mgmt = client.management("configureddevices")
    assert mgmt.ok, f"configureddevices err={mgmt.error_number}"
    devs = mgmt.value
    assert len(devs) == 1, f"se esperaba 1 dispositivo, hay {len(devs)}"
    assert devs[0]["DeviceType"] == "Telescope"
    assert devs[0]["DeviceNumber"] == profile.get("device_number", 0)

    # stable identity between discovery and management
    assert disc.get("ServerName") == devs[0]["DeviceName"]


# ── connection ───
def test_identity_version_capabilities(profile, client):
    """Identity, version and capabilities consistent with a GEM."""
    assert client.get_value("interfaceversion") == profile["interface_version"]

    assert client.get_value("alignmentmode") == profile["alignment_mode"]
    assert client.get_value("equatorialsystem") == profile["equatorial_system"]

    # required capabilities: true (not skipped via CanX=false)
    req = profile["required_capabilities"]
    caps = {
        "slew": "canslew",
        "slew_async": "canslewasync",
        "tracking": "cansettracking",
        "pulse_guide": "canpulseguide",
        "park": "canpark",
        "unpark": "canunpark",
        "find_home": "canfindhome",
    }
    for cap_name, prop in caps.items():
        if req.get(cap_name):
            assert client.get_value(prop) is True, f"capacidad requerida {prop} false"

    # move axis requiere Axis (0/1)
    for axis in (0, 1):
        assert client.get_value("canmoveaxis", Axis=axis) is True, f"canmoveaxis(Axis={axis}) false"

    # absent optionals: not advertised as present
    opt = profile["optional_capabilities"]
    if opt.get("slew_altaz") is False:
        assert client.get_value("canslewaltaz") is False
    if opt.get("sync") is False:
        assert client.get_value("cansync") is False
    if opt.get("set_park") is False:
        assert client.get_value("cansetpark") is False
    if opt.get("set_side_of_pier") is False:
        assert client.get_value("cansetpierside") is False

    # non-empty identity
    assert client.get_value("name")
    assert client.get_value("description")
    assert client.get_value("driverinfo")
    assert client.get_value("driverversion")

    assert client.get_value("trackingrates") == profile["tracking_rates"]


def test_slewsettletime_honest(client):
    """SlewSettleTime is not supported: the PUT is rejected, not silently stored."""
    r = client.put("slewsettletime", form={"SlewSettleTime": "5"})
    assert r.error_number != 0, "SlewSettleTime PUT accepted though unsupported"


# ── connection ───
def test_connect_disconnect(client):
    """Connected reflects each transition; disconnecting does not move."""
    client.put_ok("connected", form={"Connected": "true"})
    assert client.get_value("connected") is True

    before = current_radec(client)
    client.put_ok("connected", form={"Connected": "false"})
    assert client.get_value("connected") is False

    after = current_radec(client)
    # connect/disconnect starts no motion (celestial RA/DEC drift with
    # sidereal time even when the mount is still).
    assert before["slewing"] is False and after["slewing"] is False
    assert before["tracking"] is False and after["tracking"] is False

    # reconectar
    client.put_ok("connected", form={"Connected": "true"})
    assert client.get_value("connected") is True


# ── connection ───
def test_connect_idempotent(profile, client):
    """Repeated connection accumulates no sessions/errors and does not move."""
    before = current_radec(client)
    for _ in range(5):
        client.put_ok("connected", form={"Connected": "true"})
        assert client.get_value("connected") is True
    for _ in range(3):
        client.put_ok("connected", form={"Connected": "false"})
        assert client.get_value("connected") is False
    client.put_ok("connected", form={"Connected": "true"})
    after = current_radec(client)
    assert after["slewing"] is False and after["tracking"] is False


# ── connection ───
def test_query_during_motion(client):
    """Querying state during motion neither interrupts nor fakes the end."""
    from conftest import setup_site_time, wait_slewing, wait_not_slewing
    setup_site_time(client)
    client.put_ok("slewtocoordinatesasync",
                  form={"RightAscension": "6.0", "Declination": "-60.0"})
    wait_slewing(client)
    # while slewing, queries respond and do not announce a false end
    for _ in range(5):
        assert client.get_value("slewing") is True
        assert client.get_value("rightascension") is not None
    client.put_ok("abortslew")
    wait_not_slewing(client)
    assert client.get_value("slewing") is False


# ── connection — (REST complementary) ───
def test_rest_status_parseable(profile, client):
    """/api/status returns a complete, parseable state (REST complementary)."""
    import json
    import urllib.request
    with urllib.request.urlopen(profile["rest_base"] + "/api/status", timeout=5) as r:
        s = json.load(r)
    assert s["status"] in ("ready", "slewing", "tracking", "parked", "error")
    assert isinstance(s["power"], bool)
    assert isinstance(s["debug"]["uptime_s"], int)
