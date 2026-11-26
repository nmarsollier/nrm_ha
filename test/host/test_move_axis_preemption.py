"""Host tests — PR5/E09.2: MoveAxis setpoint preemption must not lose position.

A newer MoveAxis setpoint within the same manual session preempts the running
motion by draining its committed batches (reconciling every pulse exactly once)
and then re-reading the setpoint, WITHOUT finalising the session, WITHOUT
advancing the STOP generation, and WITHOUT flagging position_uncertain when the
drain succeeds.

These tests use the controlled RMT mode, which separates a batch's acceptance,
its simulated emission, and its completion, so a transmission can be held pending
while the preemption drains it.  The expected displacement is computed
independently from the recorded STEP pulses and their direction (fine resolution:
factor 1), never from the production counters.
"""
import unittest

from harness import Harness, STATUS_READY, STATUS_SLEWING, STATUS_TRACKING, \
    TRACKING_SIDEREAL, TRACKING_NONE


class MoveAxisPreemptionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.h = Harness()

    def setUp(self):
        self.h.reset()

    def _signed_sum(self):
        """Independent signed sum of the recorded RA pulses (fine, factor 1)."""
        h = self.h
        pos = h.lib.host_dir_positive_level(0)
        total = 0
        for s in h.ra_steps_recorded():
            total += 1 if s.dir_gpio == pos else -1
        return total

    def _dec_signed_sum(self):
        """Independent signed sum of the recorded DEC pulses (fine, factor 1)."""
        h = self.h
        pos = h.lib.host_dir_positive_level(1)
        total = 0
        for s in h.dec_steps_recorded():
            total += 1 if s.dir_gpio == pos else -1
        return total

    # ── Test 1 (RED): a rate preemption preserves the emitted pulses ───────

    def test_move_axis_rate_preemption_preserves_emitted_pulses(self):
        h = self.h
        self.assertEqual(h.lib.host_mount_tracking(TRACKING_SIDEREAL), 1)
        h.wait_status(STATUS_TRACKING)
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.5, 0.0), 1)
        h.wait_status(STATUS_SLEWING)

        session = h.lib.host_manual_session()
        gen_before = h.lib.host_stop_generation()
        saved = h.lib.host_saved_tracking()

        # Controlled mode: commit v1's first batch and hold it pending.
        h.lib.host_rmt_set_controlled(0, 1)
        h.wait_until(lambda: h.lib.host_rmt_pending_pulses(0) > 0, 3.0,
                     "v1 batch committed")
        steps_before_emit = h.lib.host_ra_steps()
        h.lib.host_rmt_emit(0)
        v1_period = h.ra_steps_recorded()[-1].dur0 + h.ra_steps_recorded()[-1].dur1
        n_before_v2 = h.lib.host_step_count()
        self.assertEqual(h.lib.host_ra_steps(), steps_before_emit,
                         "the pending batch must not be accounted before its completion")

        # A newer setpoint within the same session: the preemption drains.
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.8, 0.0), 1)
        self.assertEqual(h.lib.host_saved_tracking(), saved,
                         "a setpoint change must not touch saved tracking")

        # Complete the pending batch so the preemption drain can reconcile it,
        # then release controlled mode for the rest of the session.
        h.lib.host_rmt_complete(0)
        self.assertEqual(h.lib.host_rmt_set_controlled(0, 0), 0,
                         "the mode switch must succeed once the pending work is complete")

        # Wait until the preemption has fully resolved: either the new velocity
        # starts emitting (a shorter period) or, on the buggy path, the drain
        # fails and uncertainty is flagged.
        h.wait_until(
            lambda: h.lib.host_position_uncertain() == 1
                    or h.lib.host_step_count() > n_before_v2,
            5.0, "preemption resolved")

        # Concrete assertions — with the old abort-on-preemption these fail with
        # concrete values (uncertain=1, manual_move=0) rather than a timeout.
        self.assertEqual(h.lib.host_position_uncertain(), 0,
                         "a reconciled preemption must not flag uncertainty")
        self.assertEqual(h.lib.host_manual_move(), 1,
                         "the manual session must stay active across a setpoint change")
        self.assertEqual(h.lib.host_manual_session(), session,
                         "the manual session identity must be preserved")
        self.assertEqual(h.lib.host_stop_generation(), gen_before,
                         "a setpoint change must not advance the STOP generation")
        self.assertEqual(h.lib.host_saved_tracking(), saved,
                         "a setpoint change must not touch saved tracking")

        # The new velocity (0.8) really was applied: a pulse emitted AFTER the
        # preemption carries a shorter step period than the v1 batch.
        new_steps = h.ra_steps_recorded()[n_before_v2:]
        self.assertTrue(len(new_steps) > 0, "new pulses must be emitted")
        self.assertTrue(any(s.dur0 + s.dur1 < v1_period for s in new_steps),
                        "the new velocity must actually be applied")

        # Finalize the session: drains v2 and restores the saved tracking.
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.0, 0.0), 1)
        h.wait_status(STATUS_TRACKING)
        self.assertEqual(h.lib.host_tracking(), saved,
                         "tracking is restored only after the session is finalised")

        # Stop tracking so the counters fully settle.
        self.assertEqual(h.lib.host_mount_stop(), 1)
        h.wait_status(STATUS_READY)

        self.assertEqual(h.lib.host_ra_steps(), len(h.ra_steps_recorded()),
                         "every emitted pulse is preserved across a preemption")

    # ── Test 2: direction reversal preserves the previous batch sign ───────

    def test_direction_reversal_preserves_previous_batch_sign(self):
        h = self.h
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.5, 0.0), 1)  # +RA
        h.wait_status(STATUS_SLEWING)

        h.lib.host_rmt_set_controlled(0, 1)
        h.wait_until(lambda: h.lib.host_rmt_pending_pulses(0) > 0, 3.0,
                     "positive batch committed")
        pos_dir = h.lib.host_dir_level_read(0)

        # Reverse direction while the positive batch is still pending.
        self.assertEqual(h.lib.host_mount_move_axis_speed(-0.5, 0.0), 1)
        self.assertEqual(h.lib.host_dir_level_read(0), pos_dir,
                         "DIR must not change while a pending incompatible transmission exists")

        h.lib.host_rmt_emit(0)          # record the positive pulses
        h.lib.host_rmt_complete(0)      # reconcile the positive batch
        self.assertEqual(h.lib.host_rmt_set_controlled(0, 0), 0,
                         "the mode switch must succeed once the pending work is complete")

        # Wait for negative pulses (direction differs from the positive batch).
        h.wait_until(lambda: any(s.dir_gpio != pos_dir for s in h.ra_steps_recorded()),
                     5.0, "negative pulses emitted")

        self.assertEqual(h.lib.host_mount_move_axis_speed(0.0, 0.0), 1)
        h.wait_status(STATUS_READY)

        steps = h.ra_steps_recorded()
        self.assertTrue(any(s.dir_gpio == pos_dir for s in steps),
                        "positive pulses must exist")
        self.assertTrue(any(s.dir_gpio != pos_dir for s in steps),
                        "negative pulses must exist")
        self.assertEqual(h.lib.host_ra_steps(), self._signed_sum(),
                         "the counter equals the independent signed pulse sum")
        self.assertEqual(h.lib.host_position_uncertain(), 0)

    # ── Test 3: repeated setpoint updates do not lose position ─────────────

    def test_repeated_manual_setpoint_updates_do_not_lose_position(self):
        h = self.h
        gen_before = h.lib.host_stop_generation()

        self.assertEqual(h.lib.host_mount_move_axis_speed(0.3, 0.0), 1)
        h.wait_status(STATUS_SLEWING)
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.6, 0.0), 1)
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.2, 0.3), 1)   # add DEC
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.0, -0.4), 1)  # stop RA, DEC reversed
        h.wait_until(lambda: h.lib.host_step_count() > 0, 3.0, "pulses")

        self.assertEqual(h.lib.host_mount_move_axis_speed(0.0, 0.0), 1)
        h.wait_status(STATUS_READY)

        self.assertEqual(h.lib.host_ra_steps(), self._signed_sum(),
                         "RA counter equals the independent signed sum")
        self.assertEqual(h.lib.host_dec_steps(), self._dec_signed_sum(),
                         "DEC counter equals the independent signed sum")
        self.assertEqual(h.lib.host_position_uncertain(), 0)
        self.assertEqual(h.lib.host_stop_generation(), gen_before,
                         "setpoint updates must not advance the STOP generation")

    # ── Test 4: a failed preemption drain blocks unsafe motion ─────────────

    def test_failed_setpoint_preemption_drain_blocks_unsafe_motion(self):
        h = self.h
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.5, 0.0), 1)
        h.wait_status(STATUS_SLEWING)

        # Commit a batch and hold it pending; its completion will never arrive.
        h.lib.host_rmt_set_controlled(0, 1)
        h.wait_until(lambda: h.lib.host_rmt_pending_pulses(0) > 0, 3.0,
                     "batch committed")
        self.assertGreater(h.lib.host_rmt_pending_pulses(0), 0,
                           "a transmission must be pending before the preemption")
        h.lib.host_rmt_emit(0)
        emitted = h.lib.host_step_count()

        # A setpoint change: the preemption drain waits for the completion that
        # never arrives, times out, and aborts.
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.8, 0.0), 1)

        h.wait_until(lambda: h.lib.host_position_uncertain() == 1, 5.0,
                     "the failed preemption drain flags uncertainty")
        self.assertEqual(h.lib.host_admission_closed(), 1,
                         "a failed preemption drain must close admission")
        self.assertEqual(h.lib.host_tracking(), TRACKING_NONE,
                         "no tracking must be restored after a failed preemption")
        # The new velocity must not start physically after the failure, and the
        # un-reconciled batch must not be accounted as if it completed.
        self.assertEqual(h.lib.host_step_count(), emitted,
                         "the new velocity must not emit pulses after a failed drain")


if __name__ == "__main__":
    unittest.main()
