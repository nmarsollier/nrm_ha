"""Host tests — PR5/E07: identity and stability of the global STOP confirmation.

The global stop generation (``motors_stop_generation``) gives every STOP/PARK/
limits request an identity, and the executor publishes a confirmation of rest —
an (idle generation, drain result) pair — under ``motors_state_lock``.
``motors_motion_wait_idle(generation, aborted)`` consumes that confirmation.

These tests pin the contract that:

  * a confirmation is not published before the executor is actually at rest;
  * a published drain result is stable: an idle re-poll never erases a failure
    into a false success, and an observation never destroys the result;
  * the drain result belongs to exactly one generation: a later request never
    inherits an earlier request's failure (or success), and a drain result is
    tagged with the generation at which the motion ended — even when a STOP
    times out and a later STOP advances the generation before the result is
    published (E07-F);
  * the power recovery's ``wait_idle(gen, NULL)`` is a pure observation and does
    not consume a pending result;
  * manual-session finalization stays independent of the global generation.

The observable pair is ``host_idle_aborted_generation()`` / ``host_idle_aborted()``:
the generation the last drain result belongs to, and that result.  A failed drain
is produced deterministically by ``host_rmt_set_absent(0)`` (the RA batch never
completes) plus ``wait_until(host_step_count() > 0)`` (the first batch is in
flight), so the graceful stop's drain times out and aborts — no sleep needed to
order the race.
"""
import threading
import time
import unittest

from harness import (Harness, STATUS_READY, STATUS_SLEWING, STATUS_TRACKING,
                     STATUS_ERROR, TRACKING_SIDEREAL, TRACKING_NONE)


class StopGenerationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.h = Harness()

    def setUp(self):
        self.h.reset()

    # ── Helpers ────────────────────────────────────────────────────────────

    def start_failed_drain_motion(self):
        """Start a MoveAxis whose RA completion is absent, so a graceful stop's
        drain deterministically times out and aborts.  Returns after the first
        batch is confirmed in flight."""
        h = self.h
        h.lib.host_rmt_set_absent(0)
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.5, 0.0), 1)
        h.wait_until(lambda: h.lib.host_step_count() > 0, 3.0,
                     "first RA batch in flight")

    # ── Test 1: no confirmation before real rest ───────────────────────────

    def test_stop_generation_is_not_acknowledged_before_executor_rest(self):
        h = self.h
        self.start_failed_drain_motion()

        # Hold the executor at the idle barrier, before it publishes the stop
        # confirmation, so the drain result is known but rest is not yet public.
        h.lib.host_set_pause_before_idle_publish(1)
        h.lib.host_clear_before_idle_publish()

        results = []

        def stopper():
            results.append(h.lib.host_mount_stop())

        t = threading.Thread(target=stopper)
        t.start()

        # The executor drained and reached the barrier but is held before
        # publishing rest: STOP must still be waiting.
        h.wait_until(lambda: h.lib.host_reached_before_idle_publish() == 1,
                     3.0, "executor reached the idle barrier")
        self.assertEqual(results, [], "STOP must not acknowledge before rest")
        self.assertEqual(h.lib.host_admission_closed(), 1,
                         "admission stays closed until rest is confirmed")

        # Release the executor; the failed drain is now published and STOP sees it.
        h.lib.host_set_pause_before_idle_publish(0)
        t.join(timeout=5.0)
        self.assertEqual(results, [0], "STOP must report the failed drain")
        self.assertEqual(h.lib.host_position_uncertain(), 1)

    # ── Test 2: observing a confirmation does not erase its result ─────────

    def test_stop_completion_result_is_stable_across_observations(self):
        h = self.h
        self.start_failed_drain_motion()
        self.assertEqual(h.lib.host_mount_stop(), 0)
        self.assertEqual(h.lib.host_position_uncertain(), 1)

        gen = h.lib.host_idle_aborted_generation()
        self.assertEqual(h.lib.host_idle_aborted(), 1,
                         "a failed drain must be published")
        self.assertEqual(gen, h.lib.host_stop_generation(),
                         "the drain result must be tagged with its generation")

        # The executor re-publishes the idle generation every ~100 ms.  A
        # re-poll must not erase the published failure into a false success.
        time.sleep(0.35)
        self.assertEqual(h.lib.host_idle_aborted(), 1,
                         "an idle re-poll must not erase a published failure")
        self.assertEqual(h.lib.host_idle_aborted_generation(), gen,
                         "the result must stay tagged to its own generation")

    # ── Test 3: power recovery does not consume a pending result ───────────

    def test_power_recovery_does_not_consume_pending_stop_result(self):
        h = self.h
        self.start_failed_drain_motion()
        self.assertEqual(h.lib.host_mount_stop(), 0)
        self.assertEqual(h.lib.host_idle_aborted(), 1)
        aborted_gen = h.lib.host_idle_aborted_generation()

        # Power loss (recoverable) then return: the recovery path calls
        # motors_motion_wait_idle(gen, NULL), a pure observation that must not
        # consume the drain result published by the failed stop above.
        h.lib.host_update_power(0)
        h.wait_status(STATUS_ERROR)
        h.lib.host_update_power(1)
        h.wait_status(STATUS_READY)

        self.assertEqual(h.lib.host_idle_aborted(), 1,
                         "power recovery must not consume the drain result")
        self.assertEqual(h.lib.host_idle_aborted_generation(), aborted_gen,
                         "the drain result stays tagged to its generation")
        # The failed drain's uncertainty is latched: recovery reopens admission
        # but does not clear the uncertain position.
        self.assertEqual(h.lib.host_position_uncertain(), 1)

    # ── Test 4: a concurrent STOP is serialized by the control mutex ───────

    def test_concurrent_stop_cannot_advance_generation_while_another_finalizes(self):
        h = self.h
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.5, 0.0), 1)
        h.wait_status(STATUS_SLEWING)

        # Hold STOP A right after it closed admission and advanced the generation,
        # still holding the control mutex, so a concurrent STOP B cannot advance
        # the generation while A is mid-stop.  (This is the *serialization*
        # guarantee; a generation replaced by a hardware fault while admission is
        # open is covered by E06's test_fault_admission.py — during a stop's wait
        # motors_admission_close() is idempotent and never re-advances it.)
        h.lib.host_set_pause_after_stop_begin(1)
        h.lib.host_clear_stop_begin_hook()

        results = []

        def stopper():
            results.append(h.lib.host_mount_stop())

        ta = threading.Thread(target=stopper)
        ta.start()
        h.wait_until(lambda: h.lib.host_reached_stop_begin_hook() == 1,
                     2.0, "STOP A reached the stop-begin hook")

        gen_a = h.lib.host_stop_generation()

        tb = threading.Thread(target=stopper)
        tb.start()
        # B blocks on the control mutex: it must not bump the generation while
        # A is still finalizing.
        time.sleep(0.1)
        self.assertEqual(h.lib.host_stop_generation(), gen_a,
                         "a concurrent STOP must not advance the generation "
                         "while another is finalizing")

        h.lib.host_set_pause_after_stop_begin(0)
        ta.join(timeout=5.0)
        tb.join(timeout=5.0)

        self.assertEqual(results, [1, 1],
                         "both serialized STOPs must confirm their own clean rest")
        self.assertEqual(h.lib.host_status(), STATUS_READY)

    # ── Test 5: a late completion does not contaminate the next stop ───────

    def test_late_stop_completion_does_not_contaminate_next_generation(self):
        h = self.h
        self.start_failed_drain_motion()

        # Hold the executor before it publishes the confirmation so STOP A's
        # bounded wait times out without ever consuming the result.
        h.lib.host_set_pause_before_idle_publish(1)
        h.lib.host_clear_before_idle_publish()

        results_a = []

        def stop_a():
            results_a.append(h.lib.host_mount_stop())

        ta = threading.Thread(target=stop_a)
        ta.start()
        h.wait_until(lambda: h.lib.host_reached_before_idle_publish() == 1,
                     3.0, "executor reached the idle barrier")

        # STOP A times out (the executor is held): it reports an error and leaves
        # admission closed, but the drain result is still unknown to it.
        ta.join(timeout=5.0)
        self.assertEqual(results_a, [0], "STOP A must time out without rest")
        self.assertEqual(h.lib.host_admission_closed(), 1)

        # Release the executor: it now publishes A's (late) failed-drain result,
        # tagged with A's generation.  Wait explicitly for that publication.
        h.lib.host_set_pause_before_idle_publish(0)
        h.wait_until(lambda: h.lib.host_idle_aborted() == 1, 3.0,
                     "executor published the late drain result")
        gen_a = h.lib.host_idle_aborted_generation()

        # STOP B (recovery): a fresh generation must not inherit A's failure.
        self.assertEqual(h.lib.host_mount_stop(), 1,
                         "a later STOP must confirm its own clean rest")
        # A's failed-drain record is NOT reassigned to B's generation: it stays
        # tagged to A.  B's clean rest advances the rest generation, but no motion
        # drained, so the last drain result is still A's.
        self.assertEqual(h.lib.host_idle_aborted_generation(), gen_a,
                         "B must not reassign A's result to its own generation")
        self.assertGreater(h.lib.host_idle_generation(), gen_a,
                           "B must confirm rest at a newer generation")
        # A's uncertainty is latched; B's clean confirmation does not clear it.
        self.assertEqual(h.lib.host_position_uncertain(), 1)

    # ── E07-F: a later STOP advances the generation before a late publish ──

    def test_new_stop_before_late_completion_publication_preserves_result_identity(self):
        h = self.h
        self.start_failed_drain_motion()

        # Hold the executor before it publishes A's failed-drain result.
        h.lib.host_set_pause_before_idle_publish(1)
        h.lib.host_clear_before_idle_publish()

        results_a = []

        def stop_a():
            results_a.append(h.lib.host_mount_stop())

        ta = threading.Thread(target=stop_a)
        ta.start()
        h.wait_until(lambda: h.lib.host_reached_before_idle_publish() == 1,
                     3.0, "executor reached the idle barrier")

        # STOP A times out (executor held) and releases the control mutex.
        ta.join(timeout=5.0)
        self.assertEqual(results_a, [0], "STOP A must time out without rest")
        gen_a = h.lib.host_stop_generation()

        # STOP B begins and advances the generation while the executor is still
        # held; the executor must NOT publish A's result until released.
        h.lib.host_set_pause_after_stop_begin(1)
        h.lib.host_clear_stop_begin_hook()
        results_b = []

        def stop_b():
            results_b.append(h.lib.host_mount_stop())

        tb = threading.Thread(target=stop_b)
        tb.start()
        h.wait_until(lambda: h.lib.host_reached_stop_begin_hook() == 1,
                     2.0, "STOP B reached the stop-begin hook")
        gen_b = h.lib.host_stop_generation()
        self.assertNotEqual(gen_b, gen_a, "STOP B must advance the generation")

        # Release the executor: A's drain result must be published tagged with
        # gen_a (the generation at which the motion ended), NOT gen_b.
        h.lib.host_set_pause_before_idle_publish(0)
        h.wait_until(lambda: h.lib.host_idle_aborted() == 1, 3.0,
                     "executor published the late drain result")
        self.assertEqual(h.lib.host_idle_aborted_generation(), gen_a,
                         "the late result must stay tagged to A's generation")

        # Release STOP B: it must confirm rest at its own generation and must not
        # inherit A's failed drain.
        h.lib.host_set_pause_after_stop_begin(0)
        tb.join(timeout=5.0)
        self.assertEqual(results_b, [1], "STOP B must confirm its own clean rest")
        self.assertEqual(h.lib.host_idle_generation(), gen_b,
                         "B confirms rest at its own generation")
        # A's uncertainty is latched; B's clean stop does not clear it.
        self.assertEqual(h.lib.host_position_uncertain(), 1)
        # B reopened admission safely (no ERROR latched).
        self.assertEqual(h.lib.host_admission_closed(), 0)

    def test_new_stop_before_late_successful_completion_preserves_identity(self):
        h = self.h
        # A healthy motion: the drain succeeds (no absent completion).
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.5, 0.0), 1)
        h.wait_status(STATUS_SLEWING)
        h.wait_until(lambda: h.lib.host_step_count() > 0, 3.0,
                     "first batch in flight")

        h.lib.host_set_pause_before_idle_publish(1)
        h.lib.host_clear_before_idle_publish()

        results_a = []

        def stop_a():
            results_a.append(h.lib.host_mount_stop())

        ta = threading.Thread(target=stop_a)
        ta.start()
        h.wait_until(lambda: h.lib.host_reached_before_idle_publish() == 1,
                     3.0, "executor reached the idle barrier")

        ta.join(timeout=5.0)
        self.assertEqual(results_a, [0], "STOP A must time out without rest")
        gen_a = h.lib.host_stop_generation()

        h.lib.host_set_pause_after_stop_begin(1)
        h.lib.host_clear_stop_begin_hook()
        results_b = []

        def stop_b():
            results_b.append(h.lib.host_mount_stop())

        tb = threading.Thread(target=stop_b)
        tb.start()
        h.wait_until(lambda: h.lib.host_reached_stop_begin_hook() == 1,
                     2.0, "STOP B reached the stop-begin hook")
        gen_b = h.lib.host_stop_generation()
        self.assertNotEqual(gen_b, gen_a, "STOP B must advance the generation")

        h.lib.host_set_pause_before_idle_publish(0)
        # The successful drain (aborted=false) is published, tagged to gen_a.
        h.wait_until(lambda: h.lib.host_idle_aborted_generation() == gen_a, 3.0,
                     "executor published the successful drain result")
        self.assertEqual(h.lib.host_idle_aborted(), 0,
                         "a successful drain must not be reported as aborted")
        self.assertEqual(h.lib.host_idle_aborted_generation(), gen_a,
                         "the result must stay tagged to A's generation")

        h.lib.host_set_pause_after_stop_begin(0)
        tb.join(timeout=5.0)
        self.assertEqual(results_b, [1], "STOP B must confirm its own clean rest")
        self.assertEqual(h.lib.host_position_uncertain(), 0,
                         "a successful drain must not flag uncertainty")

    # ── E07 (review): identity survives a preemption before the idle barrier ──

    def test_drain_identity_survives_preemption_before_idle_barrier(self):
        h = self.h
        self.start_failed_drain_motion()

        # Suspend the executor at the end of motion_loop — right after the drain
        # result is final and its identity (drain_generation) is captured, but
        # before the idle barrier publishes it.  This models the scheduler
        # preempting the executor in that exact window.
        h.lib.host_set_pause_after_motion_loop(1)
        h.lib.host_clear_after_motion_loop()

        results_a = []

        def stop_a():
            results_a.append(h.lib.host_mount_stop())

        ta = threading.Thread(target=stop_a)
        ta.start()
        h.wait_until(lambda: h.lib.host_reached_after_motion_loop() == 1,
                     3.0, "executor finished the drain and captured its identity")

        # STOP A times out and releases the control mutex.
        ta.join(timeout=5.0)
        self.assertEqual(results_a, [0], "STOP A must time out without rest")
        gen_a = h.lib.host_stop_generation()

        # STOP B advances the generation while the executor is still held.
        h.lib.host_set_pause_after_stop_begin(1)
        h.lib.host_clear_stop_begin_hook()
        results_b = []

        def stop_b():
            results_b.append(h.lib.host_mount_stop())

        tb = threading.Thread(target=stop_b)
        tb.start()
        h.wait_until(lambda: h.lib.host_reached_stop_begin_hook() == 1,
                     2.0, "STOP B reached the stop-begin hook")
        gen_b = h.lib.host_stop_generation()
        self.assertNotEqual(gen_b, gen_a, "STOP B must advance the generation")

        # Release the executor: it must publish A's result tagged with gen_a (the
        # identity captured at drain finalisation), NOT gen_b.
        h.lib.host_set_pause_after_motion_loop(0)
        h.wait_until(lambda: h.lib.host_idle_aborted() == 1, 3.0,
                     "executor published the drain result")
        self.assertEqual(h.lib.host_idle_aborted_generation(), gen_a,
                         "the result must stay tagged to the drain's own generation")

        h.lib.host_set_pause_after_stop_begin(0)
        tb.join(timeout=5.0)
        self.assertEqual(results_b, [1], "STOP B must confirm its own clean rest")
        self.assertEqual(h.lib.host_position_uncertain(), 1,
                         "A's uncertainty is latched; B does not clear it")
        self.assertEqual(h.lib.host_admission_closed(), 0,
                         "B reopens admission safely (no ERROR latched)")

    def test_drain_identity_survives_preemption_before_idle_barrier_successful(self):
        h = self.h
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.5, 0.0), 1)
        h.wait_status(STATUS_SLEWING)
        h.wait_until(lambda: h.lib.host_step_count() > 0, 3.0, "first batch in flight")

        h.lib.host_set_pause_after_motion_loop(1)
        h.lib.host_clear_after_motion_loop()

        results_a = []

        def stop_a():
            results_a.append(h.lib.host_mount_stop())

        ta = threading.Thread(target=stop_a)
        ta.start()
        h.wait_until(lambda: h.lib.host_reached_after_motion_loop() == 1,
                     3.0, "executor finished the drain and captured its identity")

        ta.join(timeout=5.0)
        self.assertEqual(results_a, [0], "STOP A must time out without rest")
        gen_a = h.lib.host_stop_generation()

        h.lib.host_set_pause_after_stop_begin(1)
        h.lib.host_clear_stop_begin_hook()
        results_b = []

        def stop_b():
            results_b.append(h.lib.host_mount_stop())

        tb = threading.Thread(target=stop_b)
        tb.start()
        h.wait_until(lambda: h.lib.host_reached_stop_begin_hook() == 1,
                     2.0, "STOP B reached the stop-begin hook")
        gen_b = h.lib.host_stop_generation()
        self.assertNotEqual(gen_b, gen_a, "STOP B must advance the generation")

        h.lib.host_set_pause_after_motion_loop(0)
        h.wait_until(lambda: h.lib.host_idle_aborted_generation() == gen_a, 3.0,
                     "executor published the successful drain result")
        self.assertEqual(h.lib.host_idle_aborted(), 0,
                         "a successful drain must not be reported as aborted")
        self.assertEqual(h.lib.host_idle_aborted_generation(), gen_a,
                         "the result must stay tagged to the drain's own generation")

        h.lib.host_set_pause_after_stop_begin(0)
        tb.join(timeout=5.0)
        self.assertEqual(results_b, [1], "STOP B must confirm its own clean rest")
        self.assertEqual(h.lib.host_position_uncertain(), 0,
                         "a successful drain must not flag uncertainty")

    # ── Test 6: idle polling does not change a published confirmation ──────

    def test_idle_polling_does_not_change_stop_completion(self):
        h = self.h
        self.start_failed_drain_motion()
        self.assertEqual(h.lib.host_mount_stop(), 0)

        gen = h.lib.host_idle_aborted_generation()
        idle_gen = h.lib.host_idle_generation()
        self.assertEqual(h.lib.host_idle_aborted(), 1)

        # Several idle re-polls (the executor wakes every ~100 ms with no new
        # command) must neither erase the result nor advance the identity to a
        # generation that was never requested.
        time.sleep(0.35)
        self.assertEqual(h.lib.host_idle_aborted(), 1,
                         "idle polling must not erase a published result")
        self.assertEqual(h.lib.host_idle_aborted_generation(), gen,
                         "idle polling must not reassign the result's generation")
        self.assertEqual(h.lib.host_idle_generation(), idle_gen,
                         "idle polling must not fabricate a new generation")

    # ── Test 7: a command validated before STOP cannot start afterwards ────

    def test_prevalidated_slew_cannot_start_after_stop_confirmation(self):
        h = self.h
        # Enqueue a SLEW directly (the mount layer's slew isn't in the host
        # build); the command is admitted and the executor dequeues + validates
        # it before we pause.
        h.lib.host_set_pause_after_process_command(1)
        h.lib.host_clear_process_command_hook()
        h.lib.host_set_pause_after_stop_begin(1)
        h.lib.host_clear_stop_begin_hook()

        self.assertEqual(h.lib.host_fill_queue_with_slew(1), 1)
        h.wait_until(lambda: h.lib.host_reached_process_command_hook() == 1,
                     2.0, "executor validated the SLEW command")

        stop_results = []

        def stopper():
            stop_results.append(h.lib.host_mount_stop())

        t = threading.Thread(target=stopper)
        t.start()
        h.wait_until(lambda: h.lib.host_reached_stop_begin_hook() == 1,
                     2.0, "STOP reached the stop-begin hook")

        # Release the executor (it must see active=false and never emit), then
        # release STOP.
        h.lib.host_set_pause_after_process_command(0)
        h.lib.host_set_pause_after_stop_begin(0)
        t.join(timeout=5.0)

        self.assertEqual(stop_results, [1], "STOP must succeed")
        h.wait_status(STATUS_READY)
        self.assertEqual(h.lib.host_step_count(), 0,
                         "a SLEW invalidated by STOP must not emit pulses")

    # ── Test 8: serialized control operations keep their own results ───────

    def test_serialized_control_operations_receive_their_own_completion(self):
        h = self.h
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.5, 0.0), 1)
        h.wait_status(STATUS_SLEWING)

        # Each operation's result is recorded against its own identity, so a
        # swapped drain result would be observable per-operation rather than as
        # three indistinguishable successes.
        out = {}

        def op(name, fn):
            out[name] = fn()

        t_stop1 = threading.Thread(target=op, args=("stop1", h.lib.host_mount_stop))
        t_park = threading.Thread(target=op, args=("park", h.lib.host_mount_park))
        t_stop2 = threading.Thread(target=op, args=("stop2", h.lib.host_mount_stop))
        t_stop1.start(); t_park.start(); t_stop2.start()
        t_stop1.join(timeout=5.0); t_park.join(timeout=5.0); t_stop2.join(timeout=5.0)
        self.assertFalse(t_stop1.is_alive(), "stop1 must not deadlock")
        self.assertFalse(t_park.is_alive(), "park must not deadlock")
        self.assertFalse(t_stop2.is_alive(), "stop2 must not deadlock")

        # Each serialized operation confirms rest cleanly and publishes its own
        # final state; none deadlocks and none inherits another's result.
        self.assertEqual(out, {"stop1": 1, "park": 1, "stop2": 1})
        self.assertIn(h.lib.host_status(), (STATUS_READY, STATUS_TRACKING, 3))

    # ── Test 9: origin/limits require a valid stop generation ──────────────

    @unittest.skip("origin/limits setters are not in the host build (NVS); "
                   "they call motors_stop_and_wait() which propagates the fixed "
                   "confirmation — see E07_advance.txt §NOT VERIFIED")
    def test_reference_operations_require_valid_stop_generation(self):
        pass

    # ── Test 10: a stop on an idle mount confirms its own generation ───────

    def test_stop_on_idle_mount_confirms_new_generation_without_stale_result(self):
        h = self.h
        h.wait_status(STATUS_READY)

        self.assertEqual(h.lib.host_mount_stop(), 1,
                         "a stop on an idle mount must succeed cleanly")
        self.assertEqual(h.lib.host_idle_generation(), h.lib.host_stop_generation(),
                         "the stop must confirm rest at its own generation")
        self.assertEqual(h.lib.host_idle_aborted(), 0,
                         "a stop on an idle mount must not report an abort")

        # A second stop with no intermediate motion confirms a fresh generation,
        # never reusing the previous stop's result.
        self.assertEqual(h.lib.host_mount_stop(), 1)
        self.assertEqual(h.lib.host_idle_generation(), h.lib.host_stop_generation(),
                         "each stop must confirm rest at its own generation")
        self.assertEqual(h.lib.host_idle_aborted(), 0)
        self.assertEqual(h.lib.host_step_count(), 0,
                         "no pulses must be emitted on an idle stop")

    # ── Test 11: manual completion does not acknowledge a global stop ──────

    def test_manual_completion_does_not_acknowledge_global_stop(self):
        h = self.h
        gen_before = h.lib.host_stop_generation()

        # A manual MoveAxis session starts and finishes normally (full zero).
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.5, 0.0), 1)
        h.wait_status(STATUS_SLEWING)
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.0, 0.0), 1)
        h.wait_status(STATUS_READY)

        # Manual finalization must not advance the global stop generation.
        self.assertEqual(h.lib.host_stop_generation(), gen_before,
                         "manual finalization must not advance the global generation")

        # A manual finalization must not be mistaken for a global drain failure.
        self.assertEqual(h.lib.host_idle_aborted(), 0,
                         "a clean manual finalization is not a failed global drain")


if __name__ == "__main__":
    unittest.main()
