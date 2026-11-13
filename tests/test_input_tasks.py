"""Native boundaries only are faked; no DLL, device, windows, or input."""

import ctypes
from pathlib import Path
import sys
import threading
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "cqkeyspy_test"))
from input_tasks import (InputTasks, CqInputAction, CqInputTaskResult, InputTaskError,
                         InputTaskCancelled, NativeCallError, COMPLETED, CANCELLED,
                         FAILED, RUNNING, KEY_DOWN, KEY_UP, WAIT, action, milliseconds)


def native_tasks():
    """Record ABI buffers and supply explicitly configured boundary results."""
    native = Mock()
    native.task_result = CqInputTaskResult(size=96, version=1, state=COMPLETED, taskId=17)
    native.submissions = []

    def submit(actions, count, repeats, delay, output):
        native.submissions.append(([(step.type, step.value1, step.value2, step.delayMs)
                                    for step in actions], repeats, delay))
        ctypes.cast(output, ctypes.POINTER(ctypes.c_uint64))[0] = native.task_result.taskId
        if native.task_result.state == COMPLETED:
            if not native.task_result.completedActions:
                native.task_result.completedActions = count * repeats
            if not native.task_result.completedRepeats:
                native.task_result.completedRepeats = repeats
        return 1

    def query(task_id, output):
        target = ctypes.cast(output, ctypes.POINTER(CqInputTaskResult)).contents
        if target.size != 96 or target.version != 1:
            raise AssertionError("missing result size/version")
        ctypes.memmove(output, ctypes.byref(native.task_result), 96)
        return 1

    native.SubmitInputTask.side_effect = submit
    native.QueryInputTask.side_effect = query
    native.WaitInputTask.return_value = 1
    native.CancelInputTask.return_value = 1
    native.ReleaseInputTask.return_value = 1
    native.CleanupInputRuntime.return_value = 1
    return native


def fail(code):
    def boundary(*arguments):
        ctypes.set_last_error(code)
        return 0
    return boundary


class InputTasksTests(unittest.TestCase):
    def setUp(self):
        self.native = native_tasks()
        self.tasks = InputTasks(self.native)
        self.actions = [action(KEY_DOWN, 0x41, delay_ms=50), action(KEY_UP, 0x41)]

    def test_layout_matches_public_header(self):
        self.assertEqual(ctypes.sizeof(CqInputAction), 16)
        self.assertEqual(ctypes.sizeof(CqInputTaskResult), 96)
        for field, offset in (("taskId", 16), ("completedActions", 24), ("cleanupError", 36),
                              ("lastAcceptedSequence", 40), ("deviceEpoch", 56),
                              ("sessionPhase", 72), ("inputStatus", 80),
                              ("resultFlags", 88), ("reserved", 92)):
            self.assertEqual(getattr(CqInputTaskResult, field).offset, offset)

    def test_bindings_match_cdecl_public_signatures(self):
        self.assertEqual(self.native.SubmitInputTask.argtypes,
                         [ctypes.POINTER(CqInputAction), ctypes.c_uint32, ctypes.c_uint32,
                          ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint64)])
        self.assertEqual(self.native.QueryInputTask.argtypes,
                         [ctypes.c_uint64, ctypes.POINTER(CqInputTaskResult)])
        self.assertEqual(self.native.WaitInputTask.argtypes, [ctypes.c_uint64, ctypes.c_uint32])
        for name in ("SubmitInputTask", "QueryInputTask", "WaitInputTask", "CancelInputTask",
                     "ReleaseInputTask", "CleanupInputRuntime"):
            self.assertIs(getattr(self.native, name).restype, ctypes.c_int)

    def test_success_waits_queries_then_releases(self):
        self.native.task_result.completedActions = 6
        result = self.tasks.run(self.actions, repeats=3, repeat_delay_ms=20)
        self.assertEqual(result.completedActions, 6)
        self.assertEqual(self.native.submissions, [([(1, 65, 0, 50), (2, 65, 0, 0)], 3, 20)])
        self.assertEqual([entry[0] for entry in self.native.mock_calls],
                         ["SubmitInputTask", "WaitInputTask", "QueryInputTask", "ReleaseInputTask"])
        self.assertIsNone(self.tasks.active)

    def test_timeout_is_not_execution_failure_or_cancellation(self):
        responses = iter((False, True))

        def wait(*arguments):
            finished = next(responses)
            self.native.task_result.state = COMPLETED if finished else RUNNING
            ctypes.set_last_error(0 if finished else 258)
            return finished

        self.native.WaitInputTask.side_effect = wait
        progress = Mock()
        self.tasks.run(self.actions, progress=progress)
        self.assertEqual(self.native.WaitInputTask.call_count, 2)
        self.assertEqual(progress.call_count, 2)
        self.native.CancelInputTask.assert_not_called()

    def test_completed_with_error_is_not_success(self):
        self.native.task_result.error = 29
        with self.assertRaises(InputTaskError):
            self.tasks.run(self.actions)
        self.assertIsNotNone(self.tasks.active)
        self.native.ReleaseInputTask.assert_not_called()

    def test_completed_with_short_counts_or_failed_status_is_not_success(self):
        for field, value in (("completedActions", 1), ("completedRepeats", 0),
                             ("completedActions", 3), ("cleanupStatus", -1),
                             ("inputStatus", -1)):
            with self.subTest(field=field):
                native = native_tasks()
                tasks = InputTasks(native)
                task = tasks.submit(self.actions)
                setattr(native.task_result, field, value)
                with self.assertRaises(InputTaskError):
                    tasks.wait_result(task)
                self.assertIs(tasks.active, task)
                self.assertEqual(getattr(tasks.last_result, field), value)
                native.ReleaseInputTask.assert_not_called()

    def test_cancellation_racing_completed_task_reports_actual_completion(self):
        cancelled = iter((False, True))
        result = self.tasks.run(self.actions, cancelled=lambda: next(cancelled))
        self.assertEqual(result.state, COMPLETED)
        self.native.CancelInputTask.assert_called_once_with(17)
        self.native.ReleaseInputTask.assert_called_once_with(17)

    def test_completed_with_partial_or_uncertain_flags_is_not_full_success(self):
        for flags in (1, 2, 3, 4):
            with self.subTest(flags=flags):
                native = native_tasks()
                native.task_result.resultFlags = flags
                tasks = InputTasks(native)
                with self.assertRaises(InputTaskError) as caught:
                    tasks.run(self.actions)
                self.assertEqual(caught.exception.result.resultFlags, flags)
                self.assertIn(f"resultFlags={flags:#x}", str(caught.exception))
                self.assertIsNotNone(tasks.active)
                native.ReleaseInputTask.assert_not_called()

    def test_known_cancel_flags_are_diagnostics_not_cleanup_errors(self):
        self.native.task_result.state = CANCELLED
        self.native.task_result.error = 1223
        self.native.task_result.resultFlags = 5
        with self.assertRaises(InputTaskCancelled) as caught:
            self.tasks.run(self.actions)
        self.assertEqual(caught.exception.result.resultFlags, 5)
        self.assertIsNone(self.tasks.active)

    def test_unknown_flags_or_incompatible_result_fields_retain_task(self):
        for field, value in (("size", 92), ("version", 2), ("taskId", 18),
                             ("state", 5), ("resultFlags", 8), ("reserved", 1)):
            with self.subTest(field=field):
                native = native_tasks()
                tasks = InputTasks(native)
                task = tasks.submit(self.actions)
                setattr(native.task_result, field, value)
                with self.assertRaises(RuntimeError):
                    tasks.wait_result(task)
                self.assertIs(tasks.active, task)
                native.ReleaseInputTask.assert_not_called()

    def test_partial_failure_preserves_result_and_task_until_explicit_cleanup(self):
        self.native.task_result.state = FAILED
        self.native.task_result.error = 29
        self.native.task_result.completedActions = 1
        with self.assertRaises(InputTaskError) as caught:
            self.tasks.run(self.actions)
        self.assertEqual(caught.exception.result.completedActions, 1)
        self.assertIs(self.tasks.last_result, caught.exception.result)
        self.native.ReleaseInputTask.assert_not_called()
        with self.assertRaises(NativeCallError) as busy:
            self.tasks.submit(self.actions)
        self.assertEqual(busy.exception.code, 170)

    def test_cancellation_waits_for_terminal_result_and_allows_next_task(self):
        requests = iter((False, True))
        self.native.task_result.state = CANCELLED
        self.native.task_result.error = 1223
        with self.assertRaises(InputTaskCancelled):
            self.tasks.run(self.actions, cancelled=lambda: next(requests))
        self.assertEqual([entry[0] for entry in self.native.mock_calls],
                         ["SubmitInputTask", "CancelInputTask", "WaitInputTask",
                          "QueryInputTask", "ReleaseInputTask"])
        self.native.StopInputProcessing.assert_not_called()
        self.native.task_result.state = COMPLETED
        self.native.task_result.error = 0
        self.tasks.run(self.actions)
        self.assertEqual(self.native.SubmitInputTask.call_count, 2)

    def test_cancellation_cleanup_failure_is_error_not_silent_cancellation(self):
        self.native.task_result.state = CANCELLED
        self.native.task_result.error = 1223
        self.native.task_result.cleanupError = 5
        self.native.task_result.pendingCleanup = 1
        with self.assertRaises(InputTaskError):
            self.tasks.run(self.actions)
        self.assertIsNotNone(self.tasks.active)
        self.native.ReleaseInputTask.assert_not_called()

    def test_wait_query_cancel_and_release_failures_keep_task_for_retry(self):
        for name in ("WaitInputTask", "QueryInputTask", "CancelInputTask", "ReleaseInputTask"):
            with self.subTest(name=name):
                native = native_tasks()
                tasks = InputTasks(native)
                task = tasks.submit(self.actions)
                original = getattr(native, name).side_effect
                getattr(native, name).side_effect = fail(5)
                with self.assertRaises(NativeCallError):
                    tasks.wait_result(task, cancelled=lambda: name == "CancelInputTask")
                self.assertIs(tasks.active, task)
                getattr(native, name).side_effect = original
                tasks.wait_result(task)
                self.assertIsNone(tasks.active)

    def test_runtime_cleanup_failure_preserves_diagnostics_and_retry_succeeds(self):
        task = self.tasks.submit(self.actions)
        self.native.CleanupInputRuntime.side_effect = fail(5)
        with self.assertRaises(NativeCallError):
            self.tasks.cleanup()
        self.assertIsNone(self.tasks.active)
        self.assertIs(self.tasks.last_result, task.result)
        self.native.CleanupInputRuntime.side_effect = None
        self.tasks.cleanup()
        self.assertIsNone(self.tasks.active)
        self.native.ReleaseInputTask.assert_called_once()

    def test_interrupt_retains_primary_exception_and_cleanup_failure(self):
        interruption = KeyboardInterrupt("original interrupt")
        self.native.WaitInputTask.side_effect = interruption
        self.native.CancelInputTask.side_effect = fail(5)
        with self.assertRaises(KeyboardInterrupt) as caught:
            self.tasks.run(self.actions)
        self.assertIs(caught.exception, interruption)
        self.assertIsInstance(caught.exception.__cause__, NativeCallError)
        self.assertEqual(caught.exception.__cause__.code, 5)
        self.assertIsNotNone(self.tasks.active)

    def test_interrupt_keeps_terminal_input_error_as_cause(self):
        interruption = KeyboardInterrupt("original interrupt")
        self.native.WaitInputTask.side_effect = [interruption, 1]
        self.native.task_result.state = FAILED
        self.native.task_result.error = 29
        self.native.task_result.cleanupError = 5
        with self.assertRaises(KeyboardInterrupt) as caught:
            self.tasks.run(self.actions)
        self.assertIs(caught.exception, interruption)
        self.assertIsInstance(caught.exception.__cause__, InputTaskError)
        self.assertEqual(caught.exception.__cause__.result.error, 29)
        self.assertEqual(caught.exception.__cause__.result.cleanupError, 5)
        self.native.ReleaseInputTask.assert_not_called()

    def test_collect_does_not_release_a_task_owned_by_a_waiter(self):
        task = self.tasks.submit(self.actions)
        entered, resume = threading.Event(), threading.Event()
        errors = []

        def wait(*arguments):
            entered.set()
            if not resume.wait(2):
                raise AssertionError("waiter synchronization timed out")
            return 1

        def wait_result():
            try:
                self.tasks.wait_result(task)
            except Exception as error:
                errors.append(error)

        self.native.WaitInputTask.side_effect = wait
        worker = threading.Thread(target=wait_result)
        worker.start()
        try:
            self.assertTrue(entered.wait(1))
            with self.assertRaisesRegex(RuntimeError, "等待"):
                self.tasks.collect()
            self.native.ReleaseInputTask.assert_not_called()
        finally:
            resume.set()
            worker.join(2)
        self.assertFalse(worker.is_alive())
        self.assertEqual(errors, [])
        self.native.ReleaseInputTask.assert_called_once()

    def test_new_submission_waits_until_previous_waiter_unwinds(self):
        previous = self.tasks.submit(self.actions)
        released = threading.Event()
        resume_waiter = threading.Event()
        outcomes = []
        original_release = self.tasks.release

        def release_and_pause(task):
            original_release(task)
            released.set()
            if not resume_waiter.wait(2):
                raise AssertionError("waiter synchronization timed out")

        def wait_result():
            try:
                outcomes.append(self.tasks.wait_result(previous))
            except Exception as error:
                outcomes.append(error)

        with patch.object(self.tasks, "release", side_effect=release_and_pause):
            worker = threading.Thread(target=wait_result)
            worker.start()
            try:
                self.assertTrue(released.wait(1))
                self.assertIsNone(self.tasks.active)
                with self.assertRaises(NativeCallError) as busy:
                    self.tasks.submit(self.actions)
                self.assertEqual(busy.exception.code, 170)
                self.assertEqual(self.native.SubmitInputTask.call_count, 1)
            finally:
                resume_waiter.set()
                worker.join(2)
        self.assertFalse(worker.is_alive())
        self.assertEqual(len(outcomes), 1)
        self.assertIsInstance(outcomes[0], CqInputTaskResult)
        following = self.tasks.submit(self.actions)
        self.assertIs(self.tasks.active, following)

    def test_run_reserves_waiter_before_submission_unlocks(self):
        submitted = threading.Event()
        resume_submit = threading.Event()
        outcomes = []
        original_submit = self.tasks.submit

        def submit_and_pause(*arguments, **options):
            task = original_submit(*arguments, **options)
            submitted.set()
            if not resume_submit.wait(2):
                raise AssertionError("submission synchronization timed out")
            return task

        def run():
            try:
                outcomes.append(self.tasks.run(self.actions))
            except Exception as error:
                outcomes.append(error)

        with patch.object(self.tasks, "submit", side_effect=submit_and_pause):
            worker = threading.Thread(target=run)
            worker.start()
            try:
                self.assertTrue(submitted.wait(1))
                acquired = self.tasks._lock.acquire(blocking=False)
                if acquired:
                    self.tasks._lock.release()
                self.assertFalse(acquired, "task can be collected before its waiter is registered")
            finally:
                resume_submit.set()
                worker.join(2)
        self.assertFalse(worker.is_alive())
        self.assertEqual(len(outcomes), 1)
        self.assertIsInstance(outcomes[0], CqInputTaskResult)

    def test_collect_keeps_first_snapshot_if_cancel_fails(self):
        self.native.task_result.state = RUNNING
        self.native.task_result.completedActions = 1
        self.tasks.submit(self.actions)
        self.native.CancelInputTask.side_effect = fail(5)
        with self.assertRaises(NativeCallError):
            self.tasks.collect()
        self.assertIsNotNone(self.tasks.last_result)
        self.assertEqual(self.tasks.last_result.completedActions, 1)
        self.native.ReleaseInputTask.assert_not_called()

    def test_cleanup_holds_ownership_between_collect_and_runtime_cleanup(self):
        self.tasks.submit(self.actions)
        collect = self.tasks.collect
        acquired = []

        def probe_lock():
            available = self.tasks._lock.acquire(blocking=False)
            acquired.append(available)
            if available:
                self.tasks._lock.release()

        def collect_and_probe():
            result = collect()
            worker = threading.Thread(target=probe_lock)
            worker.start()
            worker.join(1)
            self.assertFalse(worker.is_alive())
            return result

        with patch.object(self.tasks, "collect", side_effect=collect_and_probe):
            self.tasks.cleanup()
        self.assertEqual(acquired, [False])
        self.native.CleanupInputRuntime.assert_called_once()

    def test_collect_releases_completed_task_and_allows_next_submit(self):
        self.native.task_result.completedActions = 2
        self.tasks.submit(self.actions)
        result = self.tasks.collect()
        self.assertIsNone(self.tasks.active)
        self.assertEqual(result.error, 0)
        self.assertEqual(result.completedActions, 2)
        task = self.tasks.submit(self.actions)
        self.assertIs(self.tasks.last_result, result)
        self.assertIs(self.tasks.active, task)

    def test_collect_release_failure_retains_task_and_diagnostics_for_retry(self):
        task = self.tasks.submit(self.actions)
        self.native.task_result.state = FAILED
        self.native.task_result.error = 29
        self.native.ReleaseInputTask.side_effect = fail(5)
        with self.assertRaises(NativeCallError):
            self.tasks.collect()
        self.assertIs(self.tasks.active, task)
        self.assertEqual(self.tasks.last_result.error, 29)
        self.native.ReleaseInputTask.side_effect = None
        result = self.tasks.collect()
        self.assertEqual(result.error, 29)
        self.assertIsNone(self.tasks.active)

    def test_collect_running_task_cancels_and_waits_before_release(self):
        self.native.task_result.state = RUNNING
        self.tasks.submit(self.actions)

        def cancel(task_id):
            self.native.task_result.state = CANCELLED
            self.native.task_result.error = 1223
            self.native.task_result.resultFlags = 4
            return 1

        self.native.CancelInputTask.side_effect = cancel
        result = self.tasks.collect()
        self.assertEqual(result.state, CANCELLED)
        self.assertEqual([entry[0] for entry in self.native.mock_calls],
                         ["SubmitInputTask", "QueryInputTask", "CancelInputTask",
                          "WaitInputTask", "QueryInputTask", "ReleaseInputTask"])
        self.assertIsNone(self.tasks.active)

    def test_partial_and_uncertain_failed_result_can_be_collected_for_diagnostics(self):
        self.native.task_result.state = FAILED
        self.native.task_result.error = 29
        self.native.task_result.resultFlags = 3
        with self.assertRaises(InputTaskError):
            self.tasks.run(self.actions)
        result = self.tasks.collect()
        self.assertEqual(result.resultFlags, 3)
        self.assertEqual(result.error, 29)
        self.assertIs(self.tasks.last_result, result)

    def test_terminal_wait_does_not_allow_running_result_to_be_released(self):
        self.native.task_result.state = RUNNING
        with self.assertRaises(InputTaskError):
            self.tasks.run(self.actions)
        self.native.ReleaseInputTask.assert_not_called()

    def test_submit_failure_does_not_create_successful_task(self):
        for code in (0, 170, 87):
            with self.subTest(code=code):
                self.native.SubmitInputTask.side_effect = fail(code)
                with self.assertRaises(NativeCallError) as caught:
                    self.tasks.run(self.actions)
                self.assertEqual(caught.exception.code, code)
                self.assertIsNone(self.tasks.active)

    def test_invalid_parameters_fail_before_submission(self):
        for steps, repeats, delay in (([], 1, 0), (self.actions * 2049, 1, 0),
                                      (self.actions, 0, 0), (self.actions, 65536, 0),
                                      (self.actions, 1, 60001), (self.actions, 1, -1)):
            with self.subTest(repeats=repeats, delay=delay, count=len(steps)):
                with self.assertRaises(ValueError):
                    self.tasks.submit(steps, repeats, delay)
        self.native.SubmitInputTask.assert_not_called()
        for seconds in (float("nan"), float("inf"), -1, 61):
            with self.assertRaises(ValueError):
                milliseconds(seconds)
        for fields in ((9, 1, 0, 0), (8, 128, 0, 0), (7, 32, 0, 0), (1, 65, 1, 0),
                       (5, 2147483648, 0, 0), (9, 0, 0, -1)):
            with self.assertRaises(ValueError):
                action(*fields)

    def test_stop_local_event_cancels_then_collects_native_completion(self):
        from input_tester import InputTester
        tester = InputTester(self.native, tasks=self.tasks)
        waiting, resume = threading.Event(), threading.Event()
        outcomes = []

        def wait(*arguments):
            waiting.set()
            if not resume.wait(2):
                raise AssertionError("test synchronization timed out")
            self.native.task_result.state = RUNNING
            ctypes.set_last_error(258)
            return 0

        def cancel(*arguments):
            self.native.WaitInputTask.side_effect = None
            self.native.task_result.state = CANCELLED
            self.native.task_result.error = 1223
            return 1

        def run():
            try:
                tester.press_key("A")
            except Exception as error:
                outcomes.append(error)

        self.native.WaitInputTask.side_effect = wait
        self.native.CancelInputTask.side_effect = cancel
        worker = threading.Thread(target=run)
        worker.start()
        try:
            self.assertTrue(waiting.wait(1))
            tester.stop()
        finally:
            resume.set()
            worker.join(2)
        self.assertFalse(worker.is_alive())
        self.assertIsInstance(outcomes[0], InputTaskCancelled)
        self.native.ReleaseInputTask.assert_called_once_with(17)


if __name__ == "__main__":
    unittest.main()
