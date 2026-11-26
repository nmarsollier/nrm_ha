"""Host tests — PR5/E09: exact RMT accounting, drain and abort.

The canonical position counters advance only by pulses whose RMT completion was
actually consumed — never by an accepted-but-unconfirmed batch, and never twice
for the same completion.  A graceful STOP drains the batches already committed,
reconciling each exactly once; a batch whose completion is already available must
be accounted even if the *other* axis exhausted the shared drain deadline (an
axis that finished must not lose its reconciled pulses because its sibling
timed out).

The RMT mock records every STEP pulse at ``rmt_transmit`` and, for a normal axis,
fires the TX-done callback immediately (so a completion is available right after
the transmit).  ``host_rmt_set_absent(axis)`` suppresses that callback, modelling
a committed transmission that never completes.  The expected displacement is
computed independently from the recorded pulse count times the canonical factor
(8 for a coarse slew), never from the production counters.
"""
import unittest

from harness import (Harness, STATUS_READY, STATUS_SLEWING, STATUS_ERROR)

COARSE_FACTOR = 8


class RmtDrainTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.h = Harness()

    def setUp(self):
        self.h.reset()

    # ── Test 4: STOP drains all committed batches on both axes ─────────────

    def test_stop_drains_all_committed_batches_on_both_axes(self):
        h = self.h
        self.assertEqual(h.lib.host_enqueue_slew(20.0, 20.0, 4.0), 1)
        h.wait_status(STATUS_SLEWING)
        h.wait_until(lambda: h.lib.host_step_count() > 0, 3.0, "first pulse")

        self.assertEqual(h.lib.host_mount_stop(), 1)
        h.wait_status(STATUS_READY)
        self.assertEqual(h.lib.host_position_uncertain(), 0)

        n_ra = len(h.ra_steps_recorded())
        n_dec = len(h.dec_steps_recorded())
        self.assertGreater(n_ra, 0)
        self.assertGreater(n_dec, 0)
        self.assertEqual(h.lib.host_ra_steps(), COARSE_FACTOR * n_ra,
                         "every committed RA pulse is reconciled exactly once")
        self.assertEqual(h.lib.host_dec_steps(), COARSE_FACTOR * n_dec,
                         "every committed DEC pulse is reconciled exactly once")

    # ── Test 5: RA exhausting the deadline must not lose DEC's completions ──

    def test_completed_dec_batches_are_not_lost_when_ra_drain_times_out(self):
        h = self.h
        # RA's completion never arrives (absent), DEC completes normally.
        h.lib.host_rmt_set_absent(0)
        self.assertEqual(h.lib.host_enqueue_slew(20.0, 20.0, 4.0), 1)
        h.wait_status(STATUS_SLEWING)
        h.wait_until(lambda: h.lib.host_step_count() > 0, 3.0, "first pulse")

        # The graceful drain times out on RA → abort, position uncertain.
        self.assertEqual(h.lib.host_mount_stop(), 0)
        self.assertEqual(h.lib.host_position_uncertain(), 1)

        # But DEC's completed batches are still available and must be reconciled,
        # not lost because RA exhausted the shared deadline.
        n_dec = len(h.dec_steps_recorded())
        self.assertGreater(n_dec, 0)
        self.assertEqual(h.lib.host_dec_steps(), COARSE_FACTOR * n_dec,
                         "DEC's reconciled pulses must survive RA's drain timeout")

    def test_completed_ra_batches_are_not_lost_when_dec_drain_times_out(self):
        h = self.h
        h.lib.host_rmt_set_absent(1)   # DEC never completes
        self.assertEqual(h.lib.host_enqueue_slew(20.0, 20.0, 4.0), 1)
        h.wait_status(STATUS_SLEWING)
        h.wait_until(lambda: h.lib.host_step_count() > 0, 3.0, "first pulse")

        self.assertEqual(h.lib.host_mount_stop(), 0)
        self.assertEqual(h.lib.host_position_uncertain(), 1)

        n_ra = len(h.ra_steps_recorded())
        self.assertGreater(n_ra, 0)
        self.assertEqual(h.lib.host_ra_steps(), COARSE_FACTOR * n_ra,
                         "RA's reconciled pulses must survive DEC's drain timeout")

    # ── Test 2: a rejected transmission must not change position ───────────

    def test_rejected_rmt_transmission_does_not_change_position(self):
        h = self.h
        # RA's rmt_transmit returns ESP_FAIL → the batch is rejected and the
        # motion aborts with a hardware fault, accounting nothing.
        h.lib.host_rmt_set_tx_error(0)
        self.assertEqual(h.lib.host_enqueue_slew(20.0, 20.0, 4.0), 1)

        h.wait_status(STATUS_ERROR)
        self.assertEqual(h.lib.host_ra_steps(), 0,
                         "a rejected RA transmission must not change RA position")
        self.assertEqual(h.lib.host_dec_steps(), 0,
                         "a rejected RA transmission must not change DEC position")
        self.assertEqual(h.lib.host_admission_closed(), 1)


if __name__ == "__main__":
    unittest.main()
