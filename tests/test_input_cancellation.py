"""Event-controlled task binding tests; only the native boundary is simulated."""

import ctypes
from pathlib import Path
import sys
import threading
import unittest
from unittest.mock import Mock, call

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'cqkeyspy_test'))
from input_tasks import (InputTasks, InputTaskCancelled, InputTaskError, NativeCallError,
                         CqInputTaskResult, RUNNING, COMPLETED, CANCELLED, FAILED,
                         KEY_DOWN, KEY_UP, action)
from input_tester import InputTester
from input_workers import WorkCancelled
from test_input_tasks import native_tasks, fail


class InputCancellationTests(unittest.TestCase):
    def setUp(self):
        from input_tasks import InputCancellation
        self.token = InputCancellation()
        self.native = native_tasks()
        self.tasks = InputTasks(self.native)
        self.actions = [action(KEY_DOWN, 65), action(KEY_UP, 65)]
        self.outcomes = []

    def start(self, operation):
        def run():
            try:
                self.outcomes.append(operation())
            except BaseException as error:
                self.outcomes.append(error)

        worker = threading.Thread(target=run, daemon=True)
        worker.start()
        return worker

    def join(self, worker):
        worker.join(2)
        self.assertFalse(worker.is_alive(), 'task/cancel synchronization deadlocked')

    def mark_cancelled(self, task_id):
        self.assertEqual(task_id, self.native.task_result.taskId)
        self.native.task_result.state = CANCELLED
        self.native.task_result.error = 1223
        return 1

    def block_wait(self):
        entered, resume = threading.Event(), threading.Event()

        def wait(*arguments):
            entered.set()
            if not resume.wait(2):
                raise AssertionError('native wait synchronization timed out')
            return 1

        self.native.WaitInputTask.side_effect = wait
        self.addCleanup(resume.set)
        return entered, resume

    def test_cancel_before_submit_never_sends_input(self):
        self.assertFalse(self.token.is_set())
        self.token.cancel()
        self.assertTrue(self.token.is_set())
        with self.assertRaises(WorkCancelled):
            self.tasks.run(self.actions, cancellation=self.token)
        self.native.SubmitInputTask.assert_not_called()
        self.native.CancelInputTask.assert_not_called()

    def test_cancel_while_submission_lock_is_busy_prevents_input(self):
        entered = threading.Event()

        def run():
            entered.set()
            return self.tasks.run(self.actions, cancellation=self.token)

        with self.tasks._lock:
            worker = self.start(run)
            self.assertTrue(entered.wait(1))
            self.token.cancel()
        self.join(worker)
        self.assertIsInstance(self.outcomes[0], WorkCancelled)
        self.native.SubmitInputTask.assert_not_called()

    def test_cancel_during_submit_is_sent_as_soon_as_task_id_exists(self):
        entered, resume = threading.Event(), threading.Event()
        original_submit = self.native.SubmitInputTask.side_effect

        def submit(*arguments):
            entered.set()
            if not resume.wait(2):
                raise AssertionError('submission synchronization timed out')
            return original_submit(*arguments)

        self.native.SubmitInputTask.side_effect = submit
        self.native.CancelInputTask.side_effect = self.mark_cancelled
        worker = self.start(lambda: self.tasks.run(self.actions, cancellation=self.token))
        try:
            self.assertTrue(entered.wait(1))
            self.token.cancel()
            self.native.CancelInputTask.assert_not_called()
        finally:
            resume.set()
            self.join(worker)
        self.assertIsInstance(self.outcomes[0], InputTaskCancelled)
        self.assertEqual([entry[0] for entry in self.native.mock_calls],
                         ['SubmitInputTask', 'CancelInputTask', 'WaitInputTask',
                          'QueryInputTask', 'ReleaseInputTask'])

    def test_bound_cancel_reaches_native_without_wait_poll_or_owner_lock(self):
        entered, resume = self.block_wait()
        self.native.CancelInputTask.side_effect = self.mark_cancelled
        tester = InputTester(self.native, tasks=self.tasks)
        worker = self.start(lambda: tester.repeat_key('a', .001, .001, 1,
                                                      cancellation=self.token))
        try:
            self.assertTrue(entered.wait(1))
            with self.tasks._lock:
                cancelling = self.start(self.token.cancel)
                self.join(cancelling)
                self.native.CancelInputTask.assert_called_once_with(17)
                self.native.QueryInputTask.assert_not_called()
            self.token.cancel()
            self.native.CancelInputTask.assert_called_once_with(17)
        finally:
            resume.set()
            self.join(worker)
        self.assertIsInstance(self.outcomes[-1], InputTaskCancelled)
        self.assertIsNone(self.tasks.active)

    def test_one_token_cannot_bind_to_two_tasks(self):
        entered, resume = self.block_wait()
        worker = self.start(lambda: self.tasks.run(self.actions, cancellation=self.token))
        other_native = native_tasks()
        try:
            self.assertTrue(entered.wait(1))
            with self.assertRaises(RuntimeError):
                InputTasks(other_native).run(self.actions, cancellation=self.token)
            other_native.SubmitInputTask.assert_not_called()
        finally:
            resume.set()
            self.join(worker)
        self.assertIsInstance(self.outcomes[0], CqInputTaskResult)
        with self.assertRaises(RuntimeError):
            self.tasks.run(self.actions, cancellation=self.token)
        self.assertEqual(self.native.SubmitInputTask.call_count, 1)

    def test_cancel_failure_propagates_and_bound_task_can_retry(self):
        self.native.task_result.state = RUNNING
        entered, resume = self.block_wait()
        self.native.CancelInputTask.side_effect = fail(5)
        worker = self.start(lambda: self.tasks.run(self.actions, cancellation=self.token))
        try:
            self.assertTrue(entered.wait(1))
            task = self.tasks.active
            with self.assertRaises(NativeCallError) as caught:
                self.token.cancel()
            self.assertEqual(caught.exception.code, 5)
            self.assertTrue(self.token.is_set())
            self.assertIs(self.tasks.active, task)
            self.native.ReleaseInputTask.assert_not_called()
            self.native.CancelInputTask.side_effect = self.mark_cancelled
            self.token.cancel()
        finally:
            resume.set()
            self.join(worker)
        self.assertEqual(self.native.CancelInputTask.call_count, 2)
        self.assertIsInstance(self.outcomes[0], InputTaskCancelled)

    def test_pending_cancel_failure_retains_task_for_retry_after_run_exits(self):
        original_submit = self.native.SubmitInputTask.side_effect

        def submit(*arguments):
            self.token.cancel()
            return original_submit(*arguments)

        self.native.SubmitInputTask.side_effect = submit
        self.native.CancelInputTask.side_effect = fail(5)
        with self.assertRaises(NativeCallError) as caught:
            self.tasks.run(self.actions, cancellation=self.token)
        self.assertEqual(caught.exception.operation, 'CancelInputTask')
        task = self.tasks.active
        self.assertIsNotNone(task)
        self.assertIsNone(self.tasks._waiter)
        self.native.ReleaseInputTask.assert_not_called()
        self.native.CancelInputTask.side_effect = self.mark_cancelled
        self.token.cancel()
        with self.assertRaises(InputTaskCancelled):
            self.tasks.wait_result(task)
        self.assertIsNone(self.tasks.active)

    def test_wait_failure_keeps_binding_until_explicit_collection(self):
        self.native.task_result.state = RUNNING
        self.native.WaitInputTask.side_effect = fail(5)
        with self.assertRaises(NativeCallError):
            self.tasks.run(self.actions, cancellation=self.token)
        self.native.CancelInputTask.side_effect = self.mark_cancelled
        self.token.cancel()
        self.native.WaitInputTask.side_effect = None
        self.tasks.collect()
        self.token.cancel()
        self.native.CancelInputTask.assert_called_once_with(17)
        self.assertIsNone(self.tasks.active)

    def test_terminal_result_unbinds_before_progress_even_when_failed(self):
        for state in (COMPLETED, FAILED):
            with self.subTest(state=state):
                from input_tasks import InputCancellation
                token = InputCancellation()
                native = native_tasks()
                native.task_result.state = state
                tasks = InputTasks(native)
                progress = Mock(side_effect=lambda result: token.cancel())
                if state == FAILED:
                    with self.assertRaises(InputTaskError):
                        tasks.run(self.actions, progress=progress, cancellation=token)
                    self.assertIsNotNone(tasks.active)
                else:
                    tasks.run(self.actions, progress=progress, cancellation=token)
                progress.assert_called_once()
                native.CancelInputTask.assert_not_called()

    def test_cancel_during_release_and_after_id_reuse_never_targets_new_task(self):
        def release(task_id):
            cancelling = self.start(self.token.cancel)
            self.join(cancelling)
            self.native.CancelInputTask.assert_not_called()
            return 1

        self.native.ReleaseInputTask.side_effect = release
        self.tasks.run(self.actions, cancellation=self.token)
        following = self.tasks.submit(self.actions)
        self.token.cancel()
        self.native.CancelInputTask.assert_not_called()
        self.assertIs(self.tasks.active, following)
        self.tasks.collect()

    def test_running_release_failure_retains_binding_for_cancel_and_retry(self):
        self.native.task_result.state = RUNNING
        task = self.tasks.submit(self.actions, cancellation=self.token)
        self.native.ReleaseInputTask.side_effect = fail(170)
        with self.assertRaises(NativeCallError) as caught:
            self.tasks.release(task)
        self.assertEqual(caught.exception.operation, 'ReleaseInputTask')
        self.assertEqual(caught.exception.code, 170)
        self.assertIs(self.tasks.active, task)
        self.assertFalse(task.released)
        self.native.CancelInputTask.side_effect = self.mark_cancelled
        self.token.cancel()
        self.native.CancelInputTask.assert_called_once_with(task.id)
        self.native.ReleaseInputTask.side_effect = None
        self.tasks.collect()
        self.assertTrue(task.released)
        self.assertIsNone(self.tasks.active)
        self.token.cancel()
        self.native.CancelInputTask.assert_called_once_with(task.id)

    def race_release_with_cancel(self, release_error):
        self.native.task_result.state = RUNNING if release_error else COMPLETED
        task = self.tasks.submit(self.actions, cancellation=self.token)
        entered, resume = threading.Event(), threading.Event()

        def release(task_id):
            self.assertEqual(task_id, task.id)
            entered.set()
            if not resume.wait(2):
                raise AssertionError('release synchronization timed out')
            return fail(release_error)(task_id) if release_error else 1

        self.native.ReleaseInputTask.side_effect = release
        self.native.CancelInputTask.side_effect = self.mark_cancelled
        releasing = self.start(lambda: self.tasks.release(task))
        cancelling = None
        try:
            self.assertTrue(entered.wait(1))
            cancelling = self.start(self.token.cancel)
            self.assertTrue(self.token._cancelled.wait(1))
            available = self.token._lock.acquire(blocking=False)
            if available:
                self.token._lock.release()
            self.assertFalse(available, 'native release must hold the token lock')
            self.native.CancelInputTask.assert_not_called()
        finally:
            resume.set()
            self.join(releasing)
            if cancelling is not None:
                self.join(cancelling)
        self.native.ReleaseInputTask.assert_called_once_with(task.id)
        if release_error:
            failures = [outcome for outcome in self.outcomes if outcome is not None]
            self.assertEqual(len(failures), 1)
            self.assertIsInstance(failures[0], NativeCallError)
            self.assertEqual(failures[0].code, release_error)
            self.assertIs(self.tasks.active, task)
            self.assertFalse(task.released)
            self.native.CancelInputTask.assert_called_once_with(task.id)
            self.native.ReleaseInputTask.side_effect = None
            self.tasks.collect()
        else:
            self.assertEqual(self.outcomes, [None, None])
            self.assertTrue(task.released)
            self.assertIsNone(self.tasks.active)
            self.native.CancelInputTask.assert_not_called()
            following = self.tasks.submit(self.actions)
            self.token.cancel()
            self.native.CancelInputTask.assert_not_called()
            self.assertIs(self.tasks.active, following)
            self.native.ReleaseInputTask.side_effect = None
            self.tasks.collect()

    def test_cancel_waiting_on_successful_release_never_uses_released_id(self):
        self.race_release_with_cancel(0)

    def test_cancel_waiting_on_failed_release_still_reaches_owned_task(self):
        self.race_release_with_cancel(170)

    def test_direct_release_waits_until_inflight_cancel_finishes(self):
        self.native.task_result.state = RUNNING
        task = self.tasks.submit(self.actions, cancellation=self.token)
        entered, resume = threading.Event(), threading.Event()
        release_started, cancel_finished = threading.Event(), threading.Event()

        def cancel(task_id):
            self.mark_cancelled(task_id)
            entered.set()
            if not resume.wait(2):
                raise AssertionError('cancel synchronization timed out')
            cancel_finished.set()
            return 1

        def release(task_id):
            self.assertEqual(task_id, task.id)
            self.assertTrue(cancel_finished.is_set())
            return 1

        def release_task():
            release_started.set()
            self.tasks.release(task)

        self.native.CancelInputTask.side_effect = cancel
        self.native.ReleaseInputTask.side_effect = release
        cancelling = self.start(self.token.cancel)
        releasing = None
        try:
            self.assertTrue(entered.wait(1))
            releasing = self.start(release_task)
            self.assertTrue(release_started.wait(1))
            self.native.ReleaseInputTask.assert_not_called()
        finally:
            resume.set()
            self.join(cancelling)
            if releasing is not None:
                self.join(releasing)
        self.assertEqual(self.outcomes, [None, None])
        self.assertTrue(task.released)
        self.assertIsNone(self.tasks.active)
        self.native.ReleaseInputTask.assert_called_once_with(task.id)
        self.token.cancel()
        self.native.CancelInputTask.assert_called_once_with(task.id)

    def test_collect_and_cleanup_failures_before_terminal_keep_token_bound(self):
        for operation in ('collect', 'cleanup'):
            for boundary in ('QueryInputTask', 'CancelInputTask', 'WaitInputTask'):
                with self.subTest(operation=operation, boundary=boundary):
                    native = native_tasks()
                    native.task_result.state = RUNNING
                    tasks, token = InputTasks(native), type(self.token)()
                    task = tasks.submit(self.actions, cancellation=token)
                    original = getattr(native, boundary).side_effect
                    getattr(native, boundary).side_effect = fail(5)
                    with self.assertRaises(NativeCallError):
                        getattr(tasks, operation)()
                    self.assertIs(tasks.active, task)
                    self.assertFalse(task.released)
                    native.ReleaseInputTask.assert_not_called()
                    native.CleanupInputRuntime.assert_not_called()
                    getattr(native, boundary).side_effect = original
                    native.CancelInputTask.reset_mock()
                    token.cancel()
                    native.CancelInputTask.assert_called_once_with(task.id)
                    native.task_result.state = CANCELLED
                    native.task_result.error = 1223
                    getattr(tasks, operation)()
                    self.assertIsNone(tasks.active)
                    token.cancel()
                    native.CancelInputTask.assert_called_once_with(task.id)

    def test_collect_and_cleanup_terminal_release_failure_keeps_task_not_token(self):
        for operation in ('collect', 'cleanup'):
            with self.subTest(operation=operation):
                native = native_tasks()
                tasks, token = InputTasks(native), type(self.token)()
                task = tasks.submit(self.actions, cancellation=token)
                native.ReleaseInputTask.side_effect = fail(5)
                with self.assertRaises(NativeCallError):
                    getattr(tasks, operation)()
                self.assertIs(tasks.active, task)
                self.assertFalse(task.released)
                self.assertEqual(tasks.last_result.state, COMPLETED)
                native.CleanupInputRuntime.assert_not_called()
                token.cancel()
                native.CancelInputTask.assert_not_called()
                native.ReleaseInputTask.side_effect = None
                getattr(tasks, operation)()
                self.assertTrue(task.released)
                self.assertIsNone(tasks.active)
                native.ReleaseInputTask.assert_has_calls([call(task.id)] * 2)
                if operation == 'cleanup':
                    native.CleanupInputRuntime.assert_called_once()

    def test_runtime_cleanup_failure_does_not_rebind_released_task(self):
        task = self.tasks.submit(self.actions, cancellation=self.token)
        self.native.CleanupInputRuntime.side_effect = fail(5)
        with self.assertRaises(NativeCallError) as caught:
            self.tasks.cleanup()
        self.assertEqual(caught.exception.operation, 'CleanupInputRuntime')
        self.assertTrue(task.released)
        self.assertIsNone(self.tasks.active)
        self.assertIs(self.tasks.last_result, task.result)
        self.token.cancel()
        self.native.CancelInputTask.assert_not_called()
        self.native.CleanupInputRuntime.side_effect = None
        self.tasks.cleanup()
        self.native.ReleaseInputTask.assert_called_once_with(task.id)
        self.assertEqual(self.native.CleanupInputRuntime.call_count, 2)

    def test_release_waits_for_inflight_cancel_without_lock_inversion(self):
        waiting, finish = self.block_wait()
        cancelling, finish_cancel = threading.Event(), threading.Event()
        queried = threading.Event()
        original_query = self.native.QueryInputTask.side_effect

        def cancel(task_id):
            self.mark_cancelled(task_id)
            cancelling.set()
            if not finish_cancel.wait(2):
                raise AssertionError('cancel synchronization timed out')
            return 1

        def query(*arguments):
            result = original_query(*arguments)
            queried.set()
            return result

        self.native.CancelInputTask.side_effect = cancel
        self.native.QueryInputTask.side_effect = query
        worker = self.start(lambda: self.tasks.run(self.actions, cancellation=self.token))
        cancel_worker = None
        try:
            self.assertTrue(waiting.wait(1))
            cancel_worker = self.start(self.token.cancel)
            self.assertTrue(cancelling.wait(1))
            finish.set()
            self.assertTrue(queried.wait(1))
            self.native.ReleaseInputTask.assert_not_called()
        finally:
            finish_cancel.set()
            finish.set()
            if cancel_worker is not None:
                self.join(cancel_worker)
            self.join(worker)
        self.assertEqual(sum(isinstance(result, InputTaskCancelled)
                             for result in self.outcomes), 1)
        self.native.ReleaseInputTask.assert_called_once_with(17)

    def test_global_stop_and_legacy_callable_still_prevent_token_submission(self):
        tester = InputTester(self.native, tasks=self.tasks)
        with self.assertRaises(WorkCancelled):
            tester.run_actions(self.actions, cancelled=lambda: True, cancellation=self.token)
        tester.stop()
        with self.assertRaises(WorkCancelled):
            tester.run_actions(self.actions, cancellation=self.token)
        self.native.SubmitInputTask.assert_not_called()


if __name__ == '__main__':
    unittest.main()
