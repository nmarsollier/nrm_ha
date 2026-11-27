"""Per-mount *expected* values, used ONLY by test_capabilities.py and identity
resolution.

These are the known-good values the firmware must declare via CAPABILITIES.  A
wrong value fails test_capabilities instead of silently shifting other tests.

Keyed by the protocol ``device_id`` (``nrm-ha``, ``bft``); ``build_profile`` maps
the build/CLI name (``nrm_ha``) to that device id in one place.  Every field the
firmware publishes through CAPABILITIES is listed and validated here; nothing
else in the suite reads these static values.
"""


NRM_HA = {
    "name": "NRM-HA",
    "device_id": "nrm-ha",
    "build_profile": "nrm_ha",
    "geometry": "german",
    "gear_ratio": 300.0,
    "full_step_deg": 1.8,
    "canonical_microsteps": 64,
    "slew_microsteps": 8,
    "slew_speeds_dps": [1.0, 3.0, 4.5, 6.0],
    "move_axis_speeds_dps": [0.005, 0.009, 0.016, 0.029, 0.053, 0.095,
                             0.171, 0.308, 0.555, 1.0],
    "slew": {
        "min_speed_dps": 0.8,
        "short_distance_deg": 2.0,
        "gentle_distance_deg": 8.0,
        "gentle_max_dps": 3.2,
        "accel_distance_deg": 5.0,
        "decel_distance_deg": 5.0,
    },
    "mechanical_limits_deg": {"ra_min": -180.0, "ra_max": 180.0,
                              "dec_min": -180.0, "dec_max": 180.0},
    "default_limits_deg": {"ra_min": -100.0, "ra_max": 100.0,
                           "dec_min": -150.0, "dec_max": 150.0},
    "meridian_flip": True,
}

BFT = {
    "name": "BFT",
    "device_id": "bft",
    "build_profile": "bft",
    "geometry": "fork",
    "gear_ratio": 500.0,
    "full_step_deg": 1.8,
    "canonical_microsteps": 64,
    "slew_microsteps": 8,
    # max_slew_dps 4 °/s → 4/6, 2, 3, 4 (the firmware prints 4/6 as "0.667").
    "slew_speeds_dps": [0.667, 2.0, 3.0, 4.0],
    "move_axis_speeds_dps": [0.005, 0.009, 0.016, 0.029, 0.053, 0.095,
                             0.171, 0.308, 0.555, 1.0],
    "slew": {
        "min_speed_dps": 0.8,
        "short_distance_deg": 2.0,
        "gentle_distance_deg": 8.0,
        "gentle_max_dps": 3.2,
        "accel_distance_deg": 10.0,
        "decel_distance_deg": 10.0,
    },
    "mechanical_limits_deg": {"ra_min": -100.0, "ra_max": 100.0,
                              "dec_min": -135.0, "dec_max": 50.0},
    "default_limits_deg": {"ra_min": -100.0, "ra_max": 100.0,
                           "dec_min": -135.0, "dec_max": 50.0},
    "meridian_flip": False,
}

PROFILES = {"nrm-ha": NRM_HA, "bft": BFT}
