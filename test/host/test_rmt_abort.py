"""Host tests — PR5/E09.3: urgent abort preserves known completions.

An urgent abort (RMT tx failure, hardware fault, power loss) stops emission and
resets the RMT channels — which discards any pending TX-done completion.  A batch
that fully completed before the fault must keep its known canonical displacement;
only the unknown (partially-emitted) work should surface as position_uncertain.

``host_rmt_set_tx_error_after(axis, n)`` lets a test fail the (n+1)-th accept
deterministically, so a known completed batch is in flight when the abort lands.
"""
import threading
import time
import unittest

from harness import Harness, STATUS_ERROR, STATUS_SLEWING, STATUS_TRACKING, \
    TRACKING_NONE


class RmtAbortTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.h = Harness()

    def setUp(self):
        self.h.reset()

    # ── Test 1: a completed-but-unaccounted batch survives an abort ────────

    def test_urgent_abort_preserves_completed_unaccounted_batch(self):
        h = self.h
        # Batches A and B succeed (their completions are published); the 3rd RA
        # accept fails and aborts the motion.
        h.lib.host_rmt_set_tx_error_after(0, 2)
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.5, 0.0), 1)
        h.wait_status(STATUS_ERROR)
        h.lib.host_wait_idle()   # wait for the executor to finish reconciling

        emitted = len(h.ra_steps_recorded())
        self.assertGreater(emitted, 0)
        self.assertEqual(h.lib.host_ra_steps(), emitted,
                         "every fully-emitted pulse must be preserved across an abort")
        self.assertEqual(h.lib.host_position_uncertain(), 1,
                         "the unknown (failed) work must flag uncertainty")
        self.assertEqual(h.lib.host_admission_closed(), 1)

    # ── Test 3: each axis keeps its own known completions ───────────────────

    def test_abort_preserves_known_completions_independently_per_axis(self):
        h = self.h
        # RA fails on its 3rd accept; DEC keeps running until the global abort.
        h.lib.host_rmt_set_tx_error_after(0, 2)
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.5, 0.5), 1)
        h.wait_status(STATUS_ERROR)
        h.lib.host_wait_idle()

        ra_emitted = len(h.ra_steps_recorded())
        dec_emitted = len(h.dec_steps_recorded())
        # RA's fully-emitted pulses are preserved; DEC's known pulses are preserved.
        self.assertEqual(h.lib.host_ra_steps(), ra_emitted,
                         "RA must keep its known displacement across the abort")
        self.assertEqual(h.lib.host_dec_steps(), dec_emitted,
                         "DEC must keep its known displacement across the abort")
        self.assertEqual(h.lib.host_position_uncertain(), 1)

    # ── Test 2: a partially-emitted batch is not invented as complete ───────

    def test_partial_batch_abort_does_not_invent_position(self):
        h = self.h
        # Controlled mode: commit ping+pong via a real MoveAxis, then emit the
        # front batch partially and abort (hardware fault) before its completion.
        h.lib.host_rmt_set_controlled(0, 1)
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.5, 0.0), 1)
        h.wait_status(STATUS_SLEWING)
        h.wait_until(lambda: h.lib.host_rmt_pending_pulses(0) > 0, 3.0,
                     "batch committed")
        h.lib.host_rmt_emit_partial(0, 40)   # emit 40 of the front batch

        # A real hardware fault: the executor detects ERROR and aborts.
        self.assertEqual(h.lib.host_enter_hardware_fault(), 1)
        h.wait_status(STATUS_ERROR)
        h.lib.host_wait_idle()

        partial = len(h.ra_steps_recorded())
        self.assertEqual(partial, 40, "only the partial pulses were emitted")
        # Production must not invent the partial quantity: the batch never gained
        # a completion, so it is not accounted as complete.
        self.assertEqual(h.lib.host_ra_steps(), 0,
                         "a partial batch must not be accounted as complete")
        self.assertEqual(h.lib.host_position_uncertain(), 1)

    # ── Test 4: a hardware fault interrupts the graceful drain ─────────────

    def test_hardware_fault_interrupts_graceful_drain_safely(self):
        h = self.h
        h.lib.host_rmt_set_controlled(0, 1)
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.5, 0.0), 1)
        h.wait_status(STATUS_SLEWING)
        h.wait_until(lambda: h.lib.host_rmt_pending_pulses(0) > 0, 3.0,
                     "batch committed")

        # Hold the executor inside the drain wait, then STOP from another thread.
        h.lib.host_set_pause_drain_wait(1)
        h.lib.host_clear_drain_wait()
        results = []

        def stopper():
            results.append(h.lib.host_mount_stop())

        t = threading.Thread(target=stopper)
        t.start()
        h.wait_until(lambda: h.lib.host_reached_drain_wait() == 1, 3.0,
                     "executor entered the drain wait")

        # Fault while the drain is waiting for a completion that never arrives.
        self.assertEqual(h.lib.host_enter_hardware_fault(), 1)
        t0 = time.monotonic()
        h.lib.host_set_pause_drain_wait(0)   # release: the drain must exit on the fault
        t.join(timeout=5.0)
        elapsed = time.monotonic() - t0

        self.assertEqual(results, [0], "STOP must not report success on a fault")
        # The drain exits because of the fault, not because the 500 ms deadline
        # elapsed: releasing the executor returns STOP almost immediately.
        self.assertLess(elapsed, 0.2,
                        "the drain must exit on the fault, not wait for the deadline")
        self.assertEqual(h.lib.host_status(), STATUS_ERROR)
        self.assertEqual(h.lib.host_admission_closed(), 1)
        self.assertEqual(h.lib.host_tracking(), TRACKING_NONE)

    # ── Test 5: a hardware fault during preemption prevents the new setpoint ──

    def test_hardware_fault_during_preemption_prevents_new_setpoint(self):
        h = self.h
        h.lib.host_rmt_set_controlled(0, 1)
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.5, 0.0), 1)
        h.wait_status(STATUS_SLEWING)
        h.wait_until(lambda: h.lib.host_rmt_pending_pulses(0) > 0, 3.0,
                     "batch committed")

        h.lib.host_set_pause_drain_wait(1)
        h.lib.host_clear_drain_wait()

        # A setpoint change within the same session triggers the preemption drain.
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.8, 0.0), 1)
        h.wait_until(lambda: h.lib.host_reached_drain_wait() == 1, 3.0,
                     "executor entered the preemption drain")

        self.assertEqual(h.lib.host_enter_hardware_fault(), 1)
        h.lib.host_set_pause_drain_wait(0)
        h.wait_status(STATUS_ERROR)
        h.lib.host_wait_idle()

        self.assertEqual(h.lib.host_admission_closed(), 1)
        self.assertEqual(h.lib.host_tracking(), TRACKING_NONE)
        # The new setpoint must not have started physically: no further emission.
        self.assertEqual(h.lib.host_step_count(), 0,
                         "the new setpoint must not emit after a fault")

    # ── Test: a completed batch before a partial batch ─────────────────────

    def test_abort_preserves_completed_batch_before_partial_batch(self):
        h = self.h
        # Controlled mode: commit two real transmissions (ping A + pong B).
        h.lib.host_rmt_set_controlled(0, 1)
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.5, 0.0), 1)
        h.wait_status(STATUS_SLEWING)
        h.wait_until(lambda: h.lib.host_rmt_pending_pulses(0) >= 100, 3.0,
                     "two transmissions committed")

        # Park the executor at the top of the main loop, before it consumes any
        # completion, so A's completion stays pending when the fault lands.
        h.lib.host_set_pause_loop_top(1)
        h.lib.host_clear_loop_top()
        h.wait_until(lambda: h.lib.host_reached_loop_top() == 1, 3.0,
                     "executor parked at the loop top")

        # Emit A fully and publish its completion; emit B only partially.
        h.lib.host_rmt_emit_one(0)
        a_pulses = h.lib.host_step_count()
        self.assertGreater(a_pulses, 0, "A must emit a known number of pulses")
        h.lib.host_rmt_complete(0)          # publish A's completion (pending)
        self.assertEqual(h.lib.host_ra_steps(), 0,
                         "A must remain unaccounted while the executor is parked")

        h.lib.host_rmt_emit_partial(0, 40)   # emit only part of B
        b_partial = h.lib.host_step_count() - a_pulses
        self.assertGreater(b_partial, 0, "B must emit some partial pulses")
        self.assertLess(b_partial, 100, "B must not be fully emitted")

        # Hardware fault, then release the executor.
        self.assertEqual(h.lib.host_enter_hardware_fault(), 1)
        h.lib.host_set_pause_loop_top(0)
        h.wait_status(STATUS_ERROR)
        h.lib.host_wait_idle()

        # A's canonical displacement (factor 1, fine resolution) is preserved; the
        # partial B is not invented.  The recorded pulses = A + partial B.
        self.assertEqual(h.lib.host_ra_steps(), a_pulses,
                         "A's known displacement must be preserved exactly")
        self.assertEqual(len(h.ra_steps_recorded()), a_pulses + b_partial,
                         "the recorded pulses are A (full) + B (partial)")
        self.assertEqual(h.lib.host_position_uncertain(), 1)
        self.assertEqual(h.lib.host_status(), STATUS_ERROR)
        self.assertEqual(h.lib.host_admission_closed(), 1)


if __name__ == "__main__":
    unittest.main()
