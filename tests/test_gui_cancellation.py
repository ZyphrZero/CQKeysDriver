"""Verify GUI-owned cancellation against simulated native task boundaries."""

import threading
import unittest
from unittest.mock import Mock, patch

from test_gui_lifecycle import CQKeysGUI, gui_module
from test_input_tasks import native_tasks, fail
from input_tester import InputTester
from input_tasks import CANCELLED, COMPLETED, RUNNING
from input_workers import InputWorkers


class GuiCancellationTests(unittest.TestCase):
    def setUp(self):
        self.native = native_tasks()
        self.gui = CQKeysGUI.__new__(CQKeysGUI)
        self.gui.root = Mock()
        self.gui._init_ui_events()
        self.gui._hook_generation = 0
        self.gui._hook_enabled = False
        self.gui.keyboard_hook = None
        self.gui._rapid_cancellation = self.gui._auto_move_cancellation = None
        self.gui.rapid_test_running = self.gui.auto_move_running = False
        self.gui._rapid_test_pending = self.gui._auto_move_pending = False
        self.gui.is_closing = self.gui.is_unloading = False
        self.gui.is_driver_loaded = True
        self.gui.input_tester = InputTester(self.native)
        self.gui.input_workers = InputWorkers()
        for name in ("rapid_test_status", "rapid_test_btn", "auto_move_status", "start_hook_btn",
                     "press_count_label", "run_time_label", "click_rate_label"):
            setattr(self.gui, name, Mock())
        for name, value in (("test_key_var", "a"), ("press_time_var", "1"),
                            ("interval_time_var", "1"), ("duration_var", "1"),
                            ("speed_var", "10"), ("range_var", "100"), ("hotkey_var", "F8")):
            variable = Mock()
            variable.get.return_value = value
            setattr(self.gui, name, variable)
        errors = patch.object(gui_module.messagebox, "showerror")
        self.errors = errors.start()
        self.addCleanup(errors.stop)
        self.workers = []
        self.gui._start_worker = self.start_worker

    def start_worker(self, target, args=()):
        worker = self.gui.input_workers.start(target, args)
        self.workers.append(worker)
        return worker

    def finish(self):
        for worker in self.workers:
            worker.join(2)
            self.assertFalse(worker.is_alive(), "GUI task did not finish")
        self.workers.clear()
        self.gui._drain_ui_events()
        self.gui.root.report_callback_exception.assert_not_called()

    def select_task(self, kind):
        if kind == "rapid":
            return self.gui._toggle_rapid_test, "_rapid_cancellation", "_rapid_test_pending"
        return self.gui._toggle_auto_move, "_auto_move_cancellation", "_auto_move_pending"

    def block_wait(self):
        entered, resume = threading.Event(), threading.Event()

        def wait(*args):
            entered.set()
            if not resume.wait(2):
                raise AssertionError("test did not release native wait")
            return 1

        self.native.WaitInputTask.side_effect = wait
        self.addCleanup(resume.set)
        return entered, resume

    def cancel_native(self, task_id):
        self.assertEqual(task_id, self.native.task_result.taskId)
        self.native.task_result.state = CANCELLED
        self.native.task_result.error = 1223
        return 1

    def test_stop_before_worker_submission_cancels_owned_token(self):
        for kind in ("rapid", "auto_move"):
            with self.subTest(kind=kind):
                toggle, token_name, pending_name = self.select_task(kind)
                self.gui._start_worker = Mock(return_value=Mock())
                toggle()
                token = getattr(self.gui, token_name)
                self.assertIsNotNone(token)
                target, arguments = self.gui._start_worker.call_args.args
                toggle()
                self.assertTrue(token.is_set())
                self.assertTrue(getattr(self.gui, pending_name))
                target(*arguments)
                self.finish()
                self.assertFalse(getattr(self.gui, pending_name))
                self.assertIsNone(getattr(self.gui, token_name))
        self.native.SubmitInputTask.assert_not_called()
        self.native.CancelInputTask.assert_not_called()

    def test_stop_notifies_owned_native_task_without_waiting_for_poll(self):
        for kind in ("rapid", "auto_move"):
            with self.subTest(kind=kind):
                self.native.task_result.state = RUNNING
                self.native.task_result.error = 0
                toggle, token_name, pending_name = self.select_task(kind)
                entered, resume = self.block_wait()
                self.native.CancelInputTask.reset_mock()
                self.native.CancelInputTask.side_effect = self.cancel_native
                try:
                    toggle()
                    self.assertTrue(entered.wait(1))
                    token = getattr(self.gui, token_name)
                    operation_count = len(self.native.mock_calls)
                    toggle()
                    self.native.CancelInputTask.assert_called_once_with(17)
                    self.assertEqual([entry[0] for entry in self.native.mock_calls[operation_count:]],
                                     ["CancelInputTask"])
                    self.assertTrue(self.workers[-1].is_alive())
                    self.assertFalse(resume.is_set())
                    self.assertTrue(token.is_set())
                    self.assertTrue(getattr(self.gui, pending_name))
                finally:
                    resume.set()
                    self.finish()
                self.assertIsNone(getattr(self.gui, token_name))
        self.native.StopInputProcessing.assert_not_called()

    def test_cancel_failure_retains_token_and_stop_button_can_retry(self):
        for kind in ("rapid", "auto_move"):
            with self.subTest(kind=kind):
                self.native.task_result.state = RUNNING
                self.native.task_result.error = 0
                toggle, token_name, pending_name = self.select_task(kind)
                entered, resume = self.block_wait()
                self.native.CancelInputTask.reset_mock()
                self.native.CancelInputTask.side_effect = fail(5)
                try:
                    toggle()
                    self.assertTrue(entered.wait(1))
                    token = getattr(self.gui, token_name)
                    toggle()
                    self.assertIs(getattr(self.gui, token_name), token)
                    self.assertTrue(getattr(self.gui, pending_name))
                    self.assertIn("Win32 5", self.errors.call_args.args[1])
                    if kind == "rapid":
                        self.gui.rapid_test_btn.config.assert_called_with(state=gui_module.tk.NORMAL)
                    self.native.CancelInputTask.side_effect = self.cancel_native
                    toggle()
                    self.assertEqual(self.native.CancelInputTask.call_count, 2)
                finally:
                    resume.set()
                    self.finish()
                self.assertIsNone(getattr(self.gui, token_name))

    def test_failed_worker_keeps_token_for_cancel_retry_and_requires_cleanup(self):
        for kind in ("rapid", "auto_move"):
            with self.subTest(kind=kind):
                toggle, token_name, pending_name = self.select_task(kind)
                self.gui._start_worker = Mock(return_value=Mock())
                self.native.task_result.state = RUNNING
                self.native.task_result.error = 0
                original_submit = self.native.SubmitInputTask.side_effect

                def submit(*args):
                    getattr(self.gui, token_name).cancel()
                    return original_submit(*args)

                self.native.SubmitInputTask.side_effect = submit
                self.native.CancelInputTask.side_effect = fail(5)
                toggle()
                token = getattr(self.gui, token_name)
                self.assertIsNotNone(token)
                target, arguments = self.gui._start_worker.call_args.args
                target(*arguments)
                self.finish()
                self.assertIs(getattr(self.gui, token_name), token)
                self.assertFalse(getattr(self.gui, pending_name))
                self.assertIsNotNone(self.gui.input_tester.tasks.active)
                self.native.CancelInputTask.side_effect = self.cancel_native
                toggle()
                self.assertIs(getattr(self.gui, token_name), token)
                self.assertEqual(self.gui._start_worker.call_count, 1)
                status = self.gui.rapid_test_status if kind == "rapid" else self.gui.auto_move_status
                self.assertIn("卸载", status.config.call_args.kwargs["text"])
                self.gui.input_tester.tasks.collect()
                setattr(self.gui, token_name, None)
                self.native.SubmitInputTask.side_effect = original_submit

    def test_failed_submission_stop_does_not_cancel_foreign_task(self):
        foreign = self.gui.input_tester.tasks.submit(InputTester.key_actions("b"))
        for kind in ("rapid", "auto_move"):
            with self.subTest(kind=kind):
                toggle, token_name, _ = self.select_task(kind)
                toggle()
                self.finish()
                self.assertIsNotNone(getattr(self.gui, token_name))
                toggle()
                self.native.CancelInputTask.assert_not_called()
                self.assertIs(self.gui.input_tester.tasks.active, foreign)
        self.native.StopInputProcessing.assert_not_called()

    def test_finished_token_cannot_cancel_new_run_with_reused_task_id(self):
        for kind in ("rapid", "auto_move"):
            with self.subTest(kind=kind):
                self.native.task_result.state = COMPLETED
                self.native.task_result.error = 0
                self.native.task_result.completedActions = self.native.task_result.completedRepeats = 0
                self.native.WaitInputTask.side_effect = None
                toggle, token_name, _ = self.select_task(kind)
                toggle()
                token = getattr(self.gui, token_name)
                self.assertIsNotNone(token)
                self.finish()
                entered, resume = self.block_wait()
                try:
                    toggle()
                    self.assertTrue(entered.wait(1))
                    self.assertIsNot(getattr(self.gui, token_name), token)
                    token.cancel()
                    self.native.CancelInputTask.assert_not_called()
                finally:
                    resume.set()
                    self.finish()

    def test_stop_hook_retries_failed_motion_cancel_even_after_hook_is_gone(self):
        self.native.task_result.state = RUNNING
        entered, resume = self.block_wait()
        hook = self.gui.keyboard_hook = Mock()
        self.gui._hook_enabled = True
        self.native.CancelInputTask.side_effect = fail(5)
        try:
            self.gui._toggle_auto_move()
            self.assertTrue(entered.wait(1))
            token = self.gui._auto_move_cancellation
            self.gui._toggle_keyboard_hook()
            self.native.CancelInputTask.assert_called_once_with(17)
            hook.stop.assert_called_once()
            self.assertFalse(self.gui._hook_enabled)
            self.assertIs(self.gui._auto_move_cancellation, token)
            self.native.CancelInputTask.side_effect = self.cancel_native
            with patch.object(gui_module, "KeyboardHook") as constructor:
                self.gui._toggle_keyboard_hook()
            constructor.assert_not_called()
            self.assertEqual(self.native.CancelInputTask.call_count, 2)
        finally:
            resume.set()
            self.finish()

    def test_hook_stop_failure_still_notifies_owned_motion_task(self):
        self.native.task_result.state = RUNNING
        entered, resume = self.block_wait()
        hook = self.gui.keyboard_hook = Mock()
        hook.stop.side_effect = TimeoutError("hook did not exit")
        self.gui._hook_enabled = True
        self.native.CancelInputTask.side_effect = self.cancel_native
        try:
            self.gui._toggle_auto_move()
            self.assertTrue(entered.wait(1))
            self.gui._toggle_keyboard_hook()
            self.native.CancelInputTask.assert_called_once_with(17)
            self.assertIs(self.gui.keyboard_hook, hook)
            self.assertFalse(self.gui._hook_enabled)
            self.assertIn("hook did not exit", self.errors.call_args.args[1])
        finally:
            resume.set()
            self.finish()

    def test_cancel_during_submit_binds_to_the_same_gui_run(self):
        for kind in ("rapid", "auto_move"):
            with self.subTest(kind=kind):
                self.native.task_result.state = RUNNING
                self.native.task_result.error = 0
                toggle, token_name, _ = self.select_task(kind)
                entered, resume = threading.Event(), threading.Event()
                original_submit = self.native.SubmitInputTask.side_effect

                def submit(*arguments):
                    entered.set()
                    if not resume.wait(2):
                        raise AssertionError("test did not release native submit")
                    return original_submit(*arguments)

                self.native.SubmitInputTask.side_effect = submit
                self.native.CancelInputTask.reset_mock()
                self.native.CancelInputTask.side_effect = self.cancel_native
                try:
                    toggle()
                    self.assertTrue(entered.wait(1))
                    toggle()
                    self.assertTrue(getattr(self.gui, token_name).is_set())
                    self.native.CancelInputTask.assert_not_called()
                finally:
                    resume.set()
                    self.finish()
                self.native.CancelInputTask.assert_called_with(17)
                self.assertIsNone(getattr(self.gui, token_name))
                self.native.SubmitInputTask.side_effect = original_submit


if __name__ == "__main__":
    unittest.main()
