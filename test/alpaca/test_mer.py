"""MER — Pier side and meridian."""
import pytest

from conftest import wait_not_slewing, wait_tracking


@pytest.fixture(autouse=True)
def _quiet(client):
    yield
    client.put_ok("abortslew")
    client.put_ok("tracking", form={"Tracking": "false"})


# ── pier side ───
def test_side_of_pier_report(client):
    """SideOfPier returns a valid value (0=East, 1=West)."""
    sop = client.get_value("sideofpier")
    assert sop in (0, 1), f"invalid SideOfPier: {sop}"


# ── pier side ───
def test_destination_side_of_pier(client):
    """DestinationSideOfPier predicts the side for a given destination."""
    client.put_ok("utcdate", form={"UTCDate": "2026-09-28T02:00:00"})
    r = client.get("destinationsideofpier",
                   RightAscension="6.0", Declination="-60.0")
    assert r.error_number == 0, f"DestinationSideOfPier err={r.error_number}"
    assert r.value in (0, 1), f"invalid DestinationSideOfPier: {r.value}"


# ── pier side — meridian flip ───
def test_meridian_flip_same_object(profile, client):
    """GOTO to the same object after crossing the meridian flips the pier."""
    client.put_ok("utcdate", form={"UTCDate": "2026-09-28T02:00:00"})
    client.put_ok("sitelatitude", form={"SiteLatitude": "-32.89"})
    client.put_ok("sitelongitude", form={"SiteLongitude": "-68.83"})

    lst = client.get_value("siderealtime")
    ra = (lst + 2.0) % 24.0  # HA = -2h (east) → pierWest

    client.put_ok("slewtocoordinatesasync",
                  form={"RightAscension": str(ra), "Declination": "-60.0"})
    wait_not_slewing(client)
    side1 = client.get_value("sideofpier")
    assert side1 == 1, f"HA=-2h should be pierWest, got {side1}"

    # advance the clock 4h so this RA crosses the meridian (HA → +2h)
    client.put_ok("utcdate", form={"UTCDate": "2026-09-28T06:00:00"})

    client.put_ok("slewtocoordinatesasync",
                  form={"RightAscension": str(ra), "Declination": "-60.0"})
    wait_not_slewing(client)
    side2 = client.get_value("sideofpier")
    assert side2 == 0, f"HA=+2h should be pierEast (flip), got {side2}"


def test_meridian_flip_preserves_tracking(profile, client):
    """A flip GOTO with tracking active resumes tracking afterwards."""
    client.put_ok("utcdate", form={"UTCDate": "2026-09-28T02:00:00"})
    client.put_ok("sitelatitude", form={"SiteLatitude": "-32.89"})
    client.put_ok("sitelongitude", form={"SiteLongitude": "-68.83"})

    lst = client.get_value("siderealtime")
    ra = (lst + 2.0) % 24.0  # HA = -2h
    client.put_ok("slewtocoordinatesasync",
                  form={"RightAscension": str(ra), "Declination": "-60.0"})
    wait_not_slewing(client)

    client.put_ok("tracking", form={"Tracking": "true"})
    wait_tracking(client, True)

    client.put_ok("utcdate", form={"UTCDate": "2026-09-28T06:00:00"})
    client.put_ok("slewtocoordinatesasync",
                  form={"RightAscension": str(ra), "Declination": "-60.0"})
    wait_not_slewing(client)

    assert client.get_value("tracking") is True, "flip GOTO did not resume tracking"
