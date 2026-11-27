"""CAPABILITIES — device identity and capabilities for the gateway translation.

This is the ONE test that knows the specific mount it is validating: it checks
every parameter the firmware declares against the independent expected values in
profiles.py (``expected_profile``).  If the firmware reports a wrong capability
or parameter this test fails — which is why it is never subject to
capability-driven deselection, and why a wrongly-declared capability cannot
silently hide a regression by excluding other tests.
"""


def _assert_close(got, want, label, tol=1e-3):
    assert abs(got - want) < tol, f"{label} {got} != expected {want}"


def _assert_dict_close(got, want, label):
    for k, w in want.items():
        assert abs(got[k] - w) < 1e-3, f"{label}.{k} {got[k]} != expected {w}"


def test_capabilities_identity(expected_profile, caps):
    """CAPABILITIES reports the profile's identity and protocol version."""
    assert caps["proto"] == 1
    assert caps["device_type"] == "mount"
    assert caps["name"] == expected_profile["name"]
    assert caps["device_id"] == expected_profile["device_id"]
    assert caps["axes"] == ["ra", "dec"]
    assert isinstance(caps["boot_id"], int) and caps["boot_id"] != 0
    assert isinstance(caps["firmware_version"], str) and caps["firmware_version"]


def test_capabilities_geometry(expected_profile, caps):
    """CAPABILITIES reports the mount geometry."""
    assert caps["geometry"] == expected_profile["geometry"]


def test_capabilities_mechanical_scale(expected_profile, caps):
    """CAPABILITIES reports the mechanical scale the step↔degree math needs."""
    _assert_close(caps["gear_ratio"], expected_profile["gear_ratio"], "gear_ratio")
    _assert_close(caps["full_step_deg"], expected_profile["full_step_deg"], "full_step_deg")
    assert caps["canonical_microsteps"] == expected_profile["canonical_microsteps"]
    assert caps["slew_microsteps"] == expected_profile["slew_microsteps"]


def test_capabilities_slew_speeds(expected_profile, caps):
    """CAPABILITIES reports the four slew speed profiles in deg/s."""
    speeds = caps["slew_speeds_dps"]
    assert len(speeds) == 4
    for got, want in zip(speeds, expected_profile["slew_speeds_dps"]):
        assert abs(got - want) < 1e-3, f"slew speed {got} != expected {want}"


def test_capabilities_move_axis_speeds(expected_profile, caps):
    """CAPABILITIES reports the ten MoveAxis speed profiles in deg/s."""
    speeds = caps["move_axis_speeds_dps"]
    assert len(speeds) == 10
    for got, want in zip(speeds, expected_profile["move_axis_speeds_dps"]):
        assert abs(got - want) < 1e-3, f"move-axis speed {got} != expected {want}"


def test_capabilities_slew_ramp(expected_profile, caps):
    """CAPABILITIES reports the distance-based ramp profile."""
    _assert_dict_close(caps["slew"], expected_profile["slew"], "slew")


def test_capabilities_limits(expected_profile, caps):
    """CAPABILITIES reports the mechanical and default operational limits."""
    _assert_dict_close(caps["mechanical_limits_deg"],
                       expected_profile["mechanical_limits_deg"], "mechanical_limits_deg")
    _assert_dict_close(caps["default_limits_deg"],
                       expected_profile["default_limits_deg"], "default_limits_deg")


def test_capabilities_tracking_modes(caps):
    """CAPABILITIES reports the supported tracking modes."""
    assert caps["tracking_modes"] == ["none", "sidereal", "lunar", "solar"]


def test_capabilities_flags(expected_profile, caps):
    """CAPABILITIES reports the capabilities the gateway app must advertise."""
    c = caps["caps"]
    assert c["guide"] is True
    assert c["home"] is True
    assert c["park"] is True
    assert c["unpark"] is True
    assert c["move_axis"] is True
    assert c["slew"] is True
    assert c["tracking"] is True
    # Sync is deferred in this iteration.
    assert c["sync"] is False
    # German equatorial mounts flip; fork mounts do not.
    assert c["meridian_flip"] is expected_profile["meridian_flip"]


def test_capabilities_boot_id_stable(proto, caps):
    """boot_id is stable across requests within a boot."""
    assert proto.capabilities()["boot_id"] == caps["boot_id"]
