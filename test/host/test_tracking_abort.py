"""Host — PR5/E09.3: urgent abort during tracking.

Tracking emits one STEP pulse per transaction and accounts the canonical step
only once its completion is consumed (motors_rmt_wait_*).  A fault that lands
while the executor is blocked waiting for that completion must:

  * preserve the previously-accounted (known) steps,
  * NOT invent the indeterminate pulse as a confirmed step,
  * flag position_uncertain (a single indeterminate microstep is still an
    unknown physical displacement),
  * interrupt the wait promptly instead of blocking the full 2 s,
  * keep ERROR + closed admission, and not restore tracking.

The controlled RMT simulator separates accept / emit / complete, so a test can
emit the pulse physically while keeping its completion absent (the indeterminate
case) or publish the completion before the abort (the valid-completion case).
"""
import threading
import time
import unittest

from harness import (Harness, STATUS_TRACKING, STATUS_ERROR, STATUS_READY,
                     TRACKING_SIDEREAL, TRACKING_NONE)


class TrackingAbortTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.h = Harness()

    def setUp(self):
        self.h.reset()

    def _start_controlled_tracking(self):
        h = self.h
        h.lib.host_rmt_set_controlled(0, 1)
        self.assertEqual(h.lib.host_mount_tracking(TRACKING_SIDEREAL), 1)
        h.wait_status(STATUS_TRACKING)

    def _account_known_steps(self, count):
        """Emit + complete `count` tracking steps so they are accounted.  Returns
        the final known RA step count."""
        h = self.h
        for _ in range(count):
            h.wait_until(lambda: h.lib.host_rmt_pending_pulses(0) > 0, 3.0,
                         "tracking step committed")
            before = h.lib.host_ra_steps()
            h.lib.host_rmt_emit(0)
            h.lib.host_rmt_complete(0)
            h.wait_until(lambda: h.lib.host_ra_steps() == before + 1, 3.0,
                         "tracking step accounted")
        return h.lib.host_ra_steps()

    def test_tracking_abort_preserves_known_step_accounting(self):
        h = self.h
        self._start_controlled_tracking()

        # 1-2. Account a few known steps (emit + complete each).
        known_steps = self._account_known_steps(2)
        self.assertGreater(known_steps, 0)

        # 3-4. Commit the next transmission; the executor blocks waiting for it.
        h.wait_until(lambda: h.lib.host_rmt_pending_pulses(0) > 0, 3.0,
                     "next step committed")

        # 5. Emit the pulse physically, but do NOT publish its completion.
        h.lib.host_rmt_emit(0)
        steps_emitted = h.lib.host_step_count()

        # 6-7. Hardware fault via the real path; the completion stays absent.
        t0 = time.monotonic()
        self.assertEqual(h.lib.host_enter_hardware_fault(), 1)

        # 8. Wait for the executor's effective finalization (RMT reset + idle).
        h.wait_until(lambda: h.lib.host_rmt_del_channel_entered() == 1, 5.0,
                     "executor reset the RMT channel")
        latency = time.monotonic() - t0
        h.lib.host_wait_idle()

        # Known steps remain accounted; the indeterminate pulse is NOT invented.
        self.assertEqual(h.lib.host_ra_steps(), known_steps,
                         "known steps must remain; the indeterminate pulse must not be counted")
        # A single indeterminate tracking microstep still flags uncertainty.
        self.assertEqual(h.lib.host_position_uncertain(), 1,
                         "an indeterminate tracking pulse must flag position_uncertain")
        self.assertEqual(h.lib.host_status(), STATUS_ERROR, "ERROR must stay set")
        self.assertEqual(h.lib.host_admission_closed(), 1, "admission must stay closed")
        self.assertEqual(h.lib.host_tracking(), TRACKING_NONE,
                         "tracking must not be restored after the abort")
        # No further pulse was emitted after the fault.
        self.assertEqual(h.lib.host_step_count(), steps_emitted,
                         "no unsafe pulse may be emitted after the fault")
        # The wait was interrupted, not left to the 2 s worst case.
        self.assertLess(latency, 1.5,
                        f"fault-to-reset latency {latency*1e3:.0f} ms should be well below 2 s")

    def test_tracking_abort_with_valid_completion_preserves_step(self):
        h = self.h
        self._start_controlled_tracking()

        known_steps = self._account_known_steps(2)

        # Commit the next step and publish its completion BEFORE the abort, so
        # the executor consumes a valid completion and accounts it exactly once.
        h.wait_until(lambda: h.lib.host_rmt_pending_pulses(0) > 0, 3.0,
                     "next step committed")
        h.lib.host_rmt_emit(0)
        h.lib.host_rmt_complete(0)
        h.wait_until(lambda: h.lib.host_ra_steps() == known_steps + 1, 3.0,
                     "the confirmed step must be accounted")

        # Hardware fault AFTER the step was fully confirmed.
        self.assertEqual(h.lib.host_enter_hardware_fault(), 1)
        h.wait_status(STATUS_ERROR)
        h.wait_until(lambda: h.lib.host_rmt_del_channel_entered() == 1, 5.0,
                     "executor reset the RMT channel")
        h.lib.host_wait_idle()

        # The confirmed step is preserved exactly once (direction +, resolution 1)
        # across the reset; the reset must not discard known recoverable info.
        self.assertEqual(h.lib.host_ra_steps(), known_steps + 1,
                         "the confirmed step must be accounted exactly once")
        self.assertEqual(h.lib.host_status(), STATUS_ERROR)
        self.assertEqual(h.lib.host_has_hardware_fault(), 1)

    def test_tracking_stop_reconciles_committed_step_without_fault(self):
        h = self.h
        self._start_controlled_tracking()

        # Account a few known steps, then commit one more and emit it physically
        # (its completion is not yet published).
        known_steps = self._account_known_steps(2)
        h.wait_until(lambda: h.lib.host_rmt_pending_pulses(0) > 0, 3.0,
                     "next step committed")
        h.lib.host_rmt_emit(0)

        # A normal STOP lands while the executor waits for the completion.  Hold
        # the stop just after the motion is aborted (active=false) so the
        # ordering is deterministic: the wait's next slice has already observed
        # the stop, but the completion is not yet available.
        h.lib.host_set_pause_after_stop_begin(1)
        h.lib.host_clear_stop_begin_hook()
        stop_result = []

        def stopper():
            stop_result.append(h.lib.host_mount_stop())

        t = threading.Thread(target=stopper)
        t.start()
        h.wait_until(lambda: h.lib.host_reached_stop_begin_hook() == 1, 3.0,
                     "stop reached the post-abort hook")

        # Let the 50 ms wait slice expire, then publish the completion.  A stop
        # must NOT convert this into a hardware fault: the committed pulse's
        # completion reconciles and the stop completes gracefully.
        time.sleep(0.15)
        h.lib.host_rmt_complete(0)
        h.lib.host_set_pause_after_stop_begin(0)
        t.join(timeout=5.0)

        self.assertEqual(stop_result, [1],
                         "a normal stop must reconcile the committed step, not fault")
        self.assertEqual(h.lib.host_has_hardware_fault(), 0,
                         "a normal stop must not latch a hardware fault")
        self.assertEqual(h.lib.host_status(), STATUS_READY)
        self.assertEqual(h.lib.host_tracking(), TRACKING_NONE)
        self.assertEqual(h.lib.host_ra_steps(), known_steps + 1,
                         "the committed step must be reconciled exactly once")

    def test_tracking_stop_with_absent_completion_latches_fault_in_time(self):
        h = self.h
        self._start_controlled_tracking()

        # Commit a pulse, emit it, but never publish its completion.
        h.wait_until(lambda: h.lib.host_rmt_pending_pulses(0) > 0, 3.0,
                     "pulse committed")
        h.lib.host_rmt_emit(0)

        # A STOP with the completion genuinely absent: the tracking step wait
        # must fault within the rest-confirmation timeout, so the stop reports a
        # hardware error and the fault is already latched when it returns — not
        # "not ready" with the executor still waiting.
        self.assertEqual(h.lib.host_mount_stop(), 0,
                         "the absent completion must fail the stop")
        self.assertEqual(h.lib.host_has_hardware_fault(), 1,
                         "the fault must be latched before the stop returns")
        self.assertEqual(h.lib.host_status(), STATUS_ERROR)
        self.assertEqual(h.lib.host_admission_closed(), 1)


if __name__ == "__main__":
    unittest.main()
