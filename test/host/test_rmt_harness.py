"""Host tests — PR5/E09.2: the controlled RMT simulator itself is a valid
measuring instrument.

These tests exercise the mock's accept/emit/complete separation directly (through
the production MoveAxis path plus the host_rmt_* controls, and the raw
host_rmt_transmit_raw for the queue-full/truncation paths production never
reaches) so the preemption tests cannot be fooled by a simulator that records
pulses at accept time, completes un-emitted batches, or hides a premature DIR
change.
"""
import threading
import unittest

from harness import Harness, STATUS_SLEWING, STATUS_ERROR, STATUS_READY


class RmtHarnessTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.h = Harness()

    def setUp(self):
        self.h.reset()

    def _commit_move_axis(self):
        h = self.h
        h.lib.host_rmt_set_controlled(0, 1)
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.5, 0.0), 1)
        h.wait_status(STATUS_SLEWING)
        h.wait_until(lambda: h.lib.host_rmt_pending_pulses(0) > 0, 3.0,
                     "batch committed")

    def test_accepted_transmission_does_not_emit_pulses(self):
        self._commit_move_axis()
        self.assertGreater(self.h.lib.host_rmt_pending_pulses(0), 0)
        self.assertEqual(self.h.lib.host_step_count(), 0,
                         "an accepted batch must not yet be recorded as emitted")

    def test_unemitted_transmission_cannot_complete_normally(self):
        self._commit_move_axis()
        pending_before = self.h.lib.host_rmt_pending_pulses(0)
        steps_before = self.h.lib.host_ra_steps()
        self.h.lib.host_rmt_complete(0)   # complete WITHOUT emit
        self.assertEqual(self.h.lib.host_ra_steps(), steps_before,
                         "an un-emitted batch must not fire a completion")
        self.assertEqual(self.h.lib.host_rmt_pending_pulses(0), pending_before,
                         "an un-emitted batch must remain pending after complete")

        # Now emit and complete it; the position reconciles exactly once.
        self.h.lib.host_rmt_emit(0)
        self.h.lib.host_rmt_complete(0)
        self.h.wait_until(
            lambda: self.h.lib.host_ra_steps() == steps_before + pending_before,
            3.0, "emitted pulses reconciled exactly once")

    def test_complete_generates_one_notification_per_emitted_batch(self):
        self._commit_move_axis()
        pending = self.h.lib.host_rmt_pending_pulses(0)
        self.h.lib.host_rmt_emit(0)
        self.assertEqual(self.h.lib.host_step_count(), pending,
                         "emit records exactly the committed pulses")
        self.h.lib.host_rmt_complete(0)
        self.h.wait_until(lambda: self.h.lib.host_ra_steps() == pending, 3.0,
                          "each emitted pulse reconciled exactly once")

    def test_full_queue_rejects_transmission(self):
        h = self.h
        h.lib.host_rmt_set_controlled(0, 1)
        for i in range(4):
            self.assertEqual(h.lib.host_rmt_transmit_raw(0, 100), 1,
                             f"batch {i} must be accepted")
        self.assertEqual(h.lib.host_rmt_transmit_raw(0, 100), 0,
                         "a full queue must reject the 5th transmission")
        self.assertEqual(h.lib.host_rmt_pending_pulses(0), 400,
                         "the 4 accepted batches remain intact")

    def test_complete_consumes_only_fifo_emitted_prefix(self):
        h = self.h
        h.lib.host_rmt_set_controlled(0, 1)
        self.assertEqual(h.lib.host_rmt_transmit_raw(0, 100), 1)  # A
        self.assertEqual(h.lib.host_rmt_transmit_raw(0, 100), 1)  # B
        h.lib.host_rmt_emit_one(0)    # emit only A (FIFO front)
        self.assertEqual(h.lib.host_step_count(), 100, "only the front batch emitted")
        h.lib.host_rmt_complete(0)    # complete A, preserve B (un-emitted)
        self.assertEqual(h.lib.host_rmt_pending_pulses(0), 100,
                         "the un-emitted B must remain pending")

    def test_mode_change_does_not_discard_pending_work(self):
        self._commit_move_axis()
        self.assertEqual(self.h.lib.host_rmt_set_controlled(0, 0), -1)
        self.assertGreater(self.h.lib.host_rmt_pending_pulses(0), 0,
                           "pending work must not be discarded by a mode switch")

    def test_direction_recorded_at_emission(self):
        h = self.h
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.5, 0.0), 1)  # +RA
        h.wait_status(STATUS_SLEWING)
        h.lib.host_rmt_set_controlled(0, 1)
        h.wait_until(lambda: h.lib.host_rmt_pending_pulses(0) > 0, 3.0,
                     "batch committed")
        pos = h.lib.host_dir_positive_level(0)
        h.lib.host_rmt_emit(0)
        steps = h.ra_steps_recorded()
        self.assertTrue(all(s.dir_gpio == pos for s in steps),
                        "the recorded direction must be the physical DIR at emission")

    def test_direction_change_between_accept_and_emit_records_emit_dir(self):
        h = self.h
        # Accept with DIR level 1 (positive for NRM), then change the physical
        # DIR level before emitting.  A simulator that froze DIR at accept would
        # record the old level; the correct one records the emit-time level.
        h.lib.host_dir_level_set(0, 1)
        h.lib.host_rmt_set_controlled(0, 1)
        self.assertEqual(h.lib.host_rmt_transmit_raw(0, 100), 1)
        self.assertEqual(h.lib.host_rmt_pending_dir(0), 1,
                         "accepted with DIR level 1")
        h.lib.host_dir_level_set(0, 0)
        h.lib.host_rmt_emit(0)
        steps = h.ra_steps_recorded()
        self.assertTrue(all(s.dir_gpio == 0 for s in steps),
                        "the recorded direction must be the physical DIR at emission")

    def test_controlled_rmt_partial_emission_has_no_normal_completion(self):
        h = self.h
        h.lib.host_rmt_set_controlled(0, 1)
        self.assertEqual(h.lib.host_rmt_transmit_raw(0, 100), 1)
        h.lib.host_rmt_emit_partial(0, 50)
        self.assertEqual(h.lib.host_step_count(), 50,
                         "partial emission records exactly the emitted pulses")
        # A partially-emitted batch has no completion: complete() must not fire.
        h.lib.host_rmt_complete(0)
        self.assertEqual(h.lib.host_ra_steps(), 0,
                         "a partial batch must not gain a normal completion")
        self.assertEqual(h.lib.host_rmt_pending_pulses(0), 50,
                         "the remaining pulses stay pending")

    def test_partial_emission_can_resume_without_duplicate_pulses(self):
        h = self.h
        h.lib.host_rmt_set_controlled(0, 1)
        self.assertEqual(h.lib.host_rmt_transmit_raw(0, 100), 1)
        h.lib.host_rmt_emit_partial(0, 40)
        self.assertEqual(h.lib.host_step_count(), 40)
        self.assertEqual(h.lib.host_rmt_pending_pulses(0), 60)
        h.lib.host_rmt_emit(0)   # resumes from the progress cursor
        self.assertEqual(h.lib.host_step_count(), 100,
                         "resuming emission must not duplicate already-emitted pulses")
        self.assertEqual(h.lib.host_rmt_pending_pulses(0), 0)
        h.lib.host_rmt_complete(0)

    def test_repeated_partial_emission_preserves_progress(self):
        h = self.h
        h.lib.host_rmt_set_controlled(0, 1)
        self.assertEqual(h.lib.host_rmt_transmit_raw(0, 100), 1)
        h.lib.host_rmt_emit_partial(0, 30)
        h.lib.host_rmt_emit_partial(0, 30)
        h.lib.host_rmt_emit_partial(0, 30)
        self.assertEqual(h.lib.host_step_count(), 90)
        self.assertEqual(h.lib.host_rmt_pending_pulses(0), 10)
        h.lib.host_rmt_emit(0)
        self.assertEqual(h.lib.host_step_count(), 100,
                         "repeated partial emission must not exceed the committed count")

    def test_partial_batch_cannot_complete_before_remaining_pulses(self):
        h = self.h
        h.lib.host_rmt_set_controlled(0, 1)
        self.assertEqual(h.lib.host_rmt_transmit_raw(0, 100), 1)
        h.lib.host_rmt_emit_partial(0, 40)
        h.lib.host_rmt_complete(0)   # partial → no completion
        self.assertEqual(h.lib.host_ra_steps(), 0,
                         "a partial batch must not gain a completion")
        self.assertEqual(h.lib.host_rmt_pending_pulses(0), 60,
                         "the remaining work must be preserved")

    def test_controlled_rmt_callback_lifetime_is_safe_across_reset(self):
        h = self.h
        # Controlled mode: commit a real transmission on the production channel
        # (whose on_done is the real on_ra_tx_done), and fully emit it, so
        # host_rmt_complete has a completion to publish.
        h.lib.host_rmt_set_controlled(0, 1)
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.5, 0.0), 1)
        h.wait_status(STATUS_SLEWING)
        h.wait_until(lambda: h.lib.host_rmt_pending_pulses(0) > 0, 3.0,
                     "batch committed")
        h.lib.host_rmt_emit(0)

        # Pause the publisher between preparing the completion (queue drained,
        # channel pinned) and firing on_done, so a concurrent channel reset can
        # land in exactly that window.
        h.lib.host_rmt_arm_complete_barrier(1)

        published = []

        def publisher():
            h.lib.host_rmt_complete(0)
            published.append(True)

        tp = threading.Thread(target=publisher)
        tp.start()
        h.wait_until(lambda: h.lib.host_rmt_reached_complete_barrier() == 1, 3.0,
                     "publisher prepared the completion")

        # A hardware fault makes the executor abort and reset the RMT channels
        # (rmt_tx_wait_all_done + rmt_del_channel), racing the pending on_done.
        self.assertEqual(h.lib.host_enter_hardware_fault(), 1)
        h.wait_status(STATUS_ERROR)
        h.wait_until(lambda: h.lib.host_rmt_del_channel_entered() == 1, 3.0,
                     "executor entered channel teardown")

        # Release the publisher: its on_done must run on a live channel (the
        # lifetime guard blocks the free until the publication drains), then the
        # reset frees it.  Both threads must terminate — no deadlock, no UAF.
        h.lib.host_rmt_release_complete_barrier()
        tp.join(timeout=5.0)

        self.assertFalse(tp.is_alive(), "publisher thread must terminate")
        self.assertTrue(published, "the pending completion must be published")
        self.assertEqual(h.lib.host_rmt_freed_during_publication(), 0,
                         "no channel may be freed while a callback is in flight")
        # The executor must finish its reset cleanly and reach idle.
        h.lib.host_wait_idle()
        self.assertEqual(h.lib.host_status(), STATUS_ERROR)
        h.lib.host_rmt_arm_complete_barrier(0)

    def test_destroyed_rmt_channel_cannot_reuse_pending_batches(self):
        h = self.h
        # 1. Commit real transmissions on the production channel (controlled mode).
        h.lib.host_rmt_set_controlled(0, 1)
        self.assertEqual(h.lib.host_mount_move_axis_speed(0.5, 0.0), 1)
        h.wait_status(STATUS_SLEWING)
        h.wait_until(lambda: h.lib.host_rmt_pending_pulses(0) > 0, 3.0,
                     "batch committed")

        # 2. Fully emit the front batch, leaving the later batch un-emitted.
        h.lib.host_rmt_emit_one(0)
        self.assertGreater(h.lib.host_rmt_pending_pulses(0), 0,
                           "a batch must remain un-emitted before the abort")
        steps_before_abort = h.lib.host_step_count()

        # 3. Abort via a real, recoverable path: power loss puts the executor
        #    into ERROR, which aborts the motion and resets the RMT channels.
        h.lib.host_update_power(0)
        h.wait_status(STATUS_ERROR)

        # 4. Recover power: it waits for the executor's RMT reset, re-inits the
        #    drivers and returns to READY.
        h.lib.host_update_power(1)
        h.wait_status(STATUS_READY)

        # 5. The destroyed channel's pending batches are discarded — they must
        #    not survive the reset to be re-associated with the recreated channel.
        self.assertEqual(h.lib.host_rmt_pending_pulses(0), 0,
                         "pending batches of the destroyed channel must be discarded")
        self.assertEqual(h.lib.host_position_uncertain(), 1,
                         "the abort must preserve position uncertainty")

        # 6-7. Commit a NEW transmission on the recreated channel (known count).
        self.assertEqual(h.lib.host_rmt_transmit_raw(0, 50), 1,
                         "a new transmission must be accepted")
        self.assertEqual(h.lib.host_rmt_pending_pulses(0), 50,
                         "the new queue must contain only the new transmission")

        # 8. Emitting the new queue emits only the new pulses — no discarded
        #    work reappears.
        h.lib.host_rmt_emit(0)
        self.assertEqual(h.lib.host_step_count(), steps_before_abort + 50,
                         "no discarded pulses may be emitted later")


if __name__ == "__main__":
    unittest.main()
