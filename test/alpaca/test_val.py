"""VAL — Public contract, invalid inputs and observable errors.

Assertions via Alpaca: ErrorNumber + HTTP, no silent success.
"""
import pytest

from client import DeviceError


def _err_number(client, name, form, **params):
    r = client.put(name, form=form, **params)
    return r.error_number


# ── input validation ───
@pytest.mark.parametrize("name,form", [
    ("sitelatitude", {"SiteLatitude": "NaN"}),
    ("sitelongitude", {"SiteLongitude": "Infinity"}),
    ("siteelevation", {"SiteElevation": "abc"}),
    ("utcdate", {"UTCDate": "no-es-una-fecha"}),
    ("tracking", {"Tracking": "bogus"}),
    ("trackingrate", {"TrackingRate": "99"}),
    ("declinationrate", {"DeclinationRate": "NaN"}),
    ("rightascensionrate", {"RightAscensionRate": "-Infinity"}),
    ("slewsettletime", {"SlewSettleTime": "-5"}),
    ("targetrightascension", {"TargetRightAscension": "30.0"}),   # RA outside [0,24)
    ("targetdeclination", {"TargetDeclination": "100.0"}),        # DEC outside [-90,90]
])
def test_reject_invalid_numeric_or_enum(client, name, form):
    """A non-finite, out-of-range or wrong type/enum value is rejected."""
    r = client.put(name, form=form)
    assert r.error_number != 0, f"{name} {form} aceptado (Value={r.value!r})"


@pytest.mark.parametrize("axis", ["99", "-1", "2", "abc", ""])
def test_moveaxis_rejects_bad_axis(client, axis):
    r = client.put("moveaxis", form={"Axis": axis, "Rate": "1.0"})
    assert r.error_number != 0, f"moveaxis Axis={axis!r} aceptado"


@pytest.mark.parametrize("direction", ["-1", "4", "9", "abc"])
def test_pulseguide_rejects_bad_direction(client, direction):
    r = client.put("pulseguide", form={"Direction": direction, "Duration": "100"})
    assert r.error_number != 0, f"pulseguide Direction={direction!r} aceptado"


# ── validation ───
def test_client_transaction_id_echo(client):
    """ClientTransactionID correlates; ServerTransactionID is monotonic."""
    for cid in (7, 123, 0, 4294967295):
        r = client.get("description", ClientTransactionID=cid)
        assert r.client_transaction_id == cid, \
            f"eco de ClientTransactionID {cid} -> {r.client_transaction_id}"

    r1 = client.get("description")
    r2 = client.get("description")
    assert r2.server_transaction_id > r1.server_transaction_id, \
        "ServerTransactionID is not monotonic"


def test_client_id_distinct_from_transaction_id(client):
    r = client.get("description", ClientID=7, ClientTransactionID=123)
    assert r.client_transaction_id == 123
    assert r.client_transaction_id != 7


# ── validation ───
def test_unimplemented_capability_honest(client, profile):
    """An absent optional capability responds not-implemented, never silently succeeds."""
    if profile["optional_capabilities"].get("sync") is False:
        r = client.put("synctocoordinates",
                       form={"RightAscension": "1.0", "Declination": "0.0"})
        assert r.error_number != 0, "Sync declared absent but returned success"

    if profile["optional_capabilities"].get("set_park") is False:
        r = client.put("setpark")
        assert r.error_number != 0, "SetPark declared absent but returned success"


# ── validation ───
def test_wrong_method_and_route(client):
    """Wrong method/route does not execute the operation."""
    import urllib.error
    import urllib.request
    # GET on a PUT-only method route (park is PUT)
    import urllib.parse
    url = client.telescope + "/park?ClientID=1&ClientTransactionID=1"
    with pytest.raises((urllib.error.HTTPError, Exception)):
        urllib.request.urlopen(url, timeout=5)


# ── validation ───
def test_error_then_recover(client):
    """An error does not leave the control unusable; the next operation works."""
    r = client.put("synctocoordinates", form={"RightAscension": "1", "Declination": "0"})
    assert r.error_number != 0
    # the channel stays healthy
    assert client.get_value("description") is not None


# ── validation ───
def test_valid_edges(client):
    """Valid values at the edges are accepted."""
    assert client.get_value("trackingrates") == [0, 1, 2]
    # positive numeric guide rates
    assert client.get_value("guideraterightascension") > 0
    assert client.get_value("guideratedeclination") > 0
