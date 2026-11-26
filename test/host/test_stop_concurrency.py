"""Host tests — STOP drains in-flight RMT batches exactly and admission is
closed atomically around the stop window.

The host harness records every STEP pulse the motion task emits.  After a
graceful STOP the position counters must exactly equal the emitted pulses (each
emitted pulse accounted once, none lost or double-counted), and a clean drain
must not flag position uncertainty.  A concurrent STOP is also exercised to
prove the admission/stop window is linearised rather than raced.
"""
import threading
import time
import unittest

from harness import (Harness, STATUS_READY, STATUS_SLEWING, STATUS_TRACKING,
                     STATUS_PARKED, TRACKING_SIDEREAL)


class StopConcurrencyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.h = Harness()

    def setUp(self):
        self.h.reset()

    def test_graceful_stop_accounts_every_emitted_pulse(self):
        h = self.h
        # Continuous MoveAxis runs at fine resolution (one STEP = one canonical
        # microstep), keeping the motion task in its double-buffered RMT loop
        # with batches in flight when the stop lands.
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.5, 0.3), 1)
        h.wait_status(STATUS_SLEWING)
        time.sleep(0.2)   # let several batches be emitted and accounted

        self.assertEqual(h.lib.host_mount_stop(), 1)
        self.assertEqual(h.lib.host_status(), STATUS_READY)
        self.assertEqual(h.lib.host_position_uncertain(), 0,
                         "graceful drain must not flag uncertainty")

        # Each emitted STEP pulse advances the canonical counter by exactly one
        # fine microstep during MoveAxis, so the counter must equal the pulse
        # count — no lost pulse and no double-count.
        self.assertEqual(h.lib.host_ra_steps(), len(h.ra_steps_recorded()),
                         "RA counter != emitted RA pulses")
        self.assertEqual(h.lib.host_dec_steps(), len(h.dec_steps_recorded()),
                         "DEC counter != emitted DEC pulses")

    def test_negative_direction_accounts_sign_correctly(self):
        h = self.h
        # Positive RA and negative DEC: the counters must track the emitted
        # pulses with the correct sign (RA increases, DEC decreases).
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.5, -0.3), 1)
        h.wait_status(STATUS_SLEWING)
        time.sleep(0.2)

        self.assertEqual(h.lib.host_mount_stop(), 1)
        self.assertEqual(h.lib.host_status(), STATUS_READY)
        self.assertEqual(h.lib.host_position_uncertain(), 0)

        self.assertGreater(h.lib.host_ra_steps(), 0, "RA must advance positive")
        self.assertEqual(h.lib.host_ra_steps(), len(h.ra_steps_recorded()),
                         "RA counter != emitted RA pulses")
        self.assertLess(h.lib.host_dec_steps(), 0, "DEC must advance negative")
        self.assertEqual(-h.lib.host_dec_steps(), len(h.dec_steps_recorded()),
                         "DEC counter != -emitted DEC pulses")

    def test_admission_reopens_after_stop(self):
        h = self.h
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.5, 0.0), 1)
        h.wait_status(STATUS_SLEWING)
        self.assertEqual(h.lib.host_mount_stop(), 1)

        # Admission was reopened: a fresh command is accepted after the stop,
        # proving the stop window did not leave admission latched closed.
        self.assertEqual(h.lib.host_mount_tracking(TRACKING_SIDEREAL), 1)
        h.wait_status(STATUS_TRACKING)
        self.assertEqual(h.lib.host_mount_stop(), 1)
        h.wait_status(STATUS_READY)

    def test_concurrent_stops_are_linearised(self):
        h = self.h
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.5, 0.0), 1)
        h.wait_status(STATUS_SLEWING)

        results = []

        def stopper():
            results.append(h.lib.host_mount_stop())

        t1 = threading.Thread(target=stopper)
        t2 = threading.Thread(target=stopper)
        t1.start()
        t2.start()
        t1.join()
        t2.join()

        # Both STOPs serialise through motors_stop_begin()/wait-idle: each
        # confirms rest at its own generation and publishes READY.
        self.assertEqual(results, [1, 1],
                         "both concurrent STOPs must confirm a clean rest")
        self.assertEqual(h.lib.host_status(), STATUS_READY)
        self.assertEqual(h.lib.host_position_uncertain(), 0)

    def test_concurrent_stop_and_park_are_serialized(self):
        h = self.h
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.5, 0.0), 1)
        h.wait_status(STATUS_SLEWING)

        results = []

        def stopper():
            results.append(h.lib.host_mount_stop())

        def parker():
            results.append(h.lib.host_mount_park())

        t1 = threading.Thread(target=stopper)
        t2 = threading.Thread(target=parker)
        t1.start()
        t2.start()
        t1.join()
        t2.join()

        # The control mutex serialises the whole STOP and the whole PARK, so each
        # confirms rest, publishes its own final state and reopens admission
        # before the other proceeds — no deadlock, no interleaved reopen.
        self.assertEqual(results, [1, 1],
                         "both concurrent STOP and PARK must succeed")
        self.assertIn(h.lib.host_status(), (STATUS_READY, STATUS_PARKED))
        self.assertEqual(h.lib.host_position_uncertain(), 0)


if __name__ == "__main__":
    unittest.main()
