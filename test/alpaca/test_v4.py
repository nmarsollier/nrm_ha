"""V4 — DeviceState ("read all") and Connecting.

Telescope Interface V4 exposes the aggregated DeviceState endpoint and the
Connecting property.  These cases verify the new endpoints without depending
on the individual property endpoints (which older clients still use).
"""


# The 10 operational properties the ASCOM Telescope V4 DeviceState exposes.
OPERATIONAL_NAMES = {
    "Altitude", "AtHome", "AtPark", "Azimuth", "Declination",
    "IsPulseGuiding", "RightAscension", "SideOfPier", "Slewing", "Tracking",
}


def _by_name(client):
    r = client.get("devicestate")
    assert r.ok, f"devicestate err={r.error_number}: {r.error_message}"
    values = r.value
    assert isinstance(values, list) and values, "devicestate Value vacío"
    return {v["Name"]: v["Value"] for v in values}


def test_devicestate_operational_names(client):
    by_name = _by_name(client)
    missing = OPERATIONAL_NAMES - set(by_name)
    assert not missing, f"faltan propiedades operacionales: {missing}"


def test_devicestate_value_types(client):
    by_name = _by_name(client)
    for name in ("AtHome", "AtPark", "IsPulseGuiding", "Slewing", "Tracking"):
        assert isinstance(by_name[name], bool), f"{name} no es bool: {by_name[name]!r}"
    for name in ("Altitude", "Azimuth", "Declination", "RightAscension"):
        assert isinstance(by_name[name], (int, float)), f"{name} no es numérico: {by_name[name]!r}"
    assert isinstance(by_name["SideOfPier"], int), f"SideOfPier no es int: {by_name['SideOfPier']!r}"


def test_devicestate_matches_individual(client):
    """A coherent snapshot must agree with the individual endpoints."""
    by_name = _by_name(client)
    assert by_name["Slewing"] == client.get_value("slewing")
    assert by_name["Tracking"] == client.get_value("tracking")
    assert by_name["AtPark"] == client.get_value("atpark")
    assert by_name["AtHome"] == client.get_value("athome")
    assert by_name["SideOfPier"] == client.get_value("sideofpier")


def test_connecting_is_false(client):
    assert client.get_value("connecting") is False
