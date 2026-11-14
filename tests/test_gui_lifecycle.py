"""Exercise GUI lifecycle methods without creating windows or loading a driver."""
from pathlib import Path
import ctypes
import queue
import sys
import threading
from types import ModuleType
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "cqkeyspy_test"))
from input_workers import InputWorkers
with patch.dict(sys.modules, {name: ModuleType(name) for name in ("win32gui", "win32api", "win32con")}):
    import gui as gui_module
CQKeysGUI = gui_module.CQKeysGUI


class GuiLifecycleTests(unittest.TestCase):
    def setUp(self):
        self.gui = CQKeysGUI.__new__(CQKeysGUI)
        self.gui.root = Mock()
        self.gui._init_ui_events()
        self.gui._hook_generation = 0
        self.gui._hook_enabled = False
        self.gui._rapid_cancellation = self.gui._auto_move_cancellation = None
        self.gui.driver_mgr = Mock()
        self.gui.input_tester = Mock()
        self.gui.keyboard_hook = None
        self.gui.is_driver_loaded = True
        self.gui.is_closing = self.gui.is_unloading = False
        self.gui._remove_package_requested = False
        self.gui.remove_package_var = Mock()
        self.gui.remove_package_var.get.return_value = False
        self.gui.auto_move_running = self.gui.rapid_test_running = True
        self.gui.input_workers = InputWorkers()
        self.gui._stop_results = queue.Queue()
        self.gui._cleanup_results = queue.Queue()
        self.gui._input_release_error = None
        self.gui._management_cleanup_error = None
        for name in ("driver_status", "last_check", "start_hook_btn", "auto_move_status", "rapid_test_btn"):
            setattr(self.gui, name, Mock())
        self.gui._update_button_states = Mock()
        self.dialog = patch.object(gui_module.messagebox, "showerror")
        self.dialog.start()
        self.addCleanup(self.dialog.stop)
        self.warning = patch.object(gui_module.messagebox, "showwarning")
        self.warning.start()
        self.addCleanup(self.warning.stop)

    def test_unload_failure_preserves_manager_and_window_for_retry(self):
        manager = self.gui.driver_mgr
        rapid_token = self.gui._rapid_cancellation = Mock()
        move_token = self.gui._auto_move_cancellation = Mock()
        self.gui.is_closing = True
        self.gui._finish_unload(OSError("device busy"), close_after=True)
        self.assertIs(self.gui.driver_mgr, manager)
        self.assertIs(self.gui._rapid_cancellation, rapid_token)
        self.assertIs(self.gui._auto_move_cancellation, move_token)
        self.assertFalse(self.gui.is_closing)
        self.assertFalse(self.gui.is_driver_loaded)
        self.gui.root.destroy.assert_not_called()

    def test_close_destroys_window_only_after_success(self):
        self.gui._finish_unload(None, close_after=True)
        self.assertIsNone(self.gui.driver_mgr)
        self.gui.root.destroy.assert_called_once()

    def test_unload_resets_pending_tasks_and_discards_old_ui_callbacks(self):
        self.gui._rapid_test_pending = self.gui._auto_move_pending = True
        self.gui._rapid_cancellation = Mock()
        self.gui._auto_move_cancellation = Mock()
        stale_callback = Mock()
        self.gui._post_ui(stale_callback)
        self.gui._finish_unload(None, close_after=False)
        self.gui._drain_ui_events()
        self.assertFalse(self.gui._rapid_test_pending)
        self.assertFalse(self.gui._auto_move_pending)
        self.assertIsNone(self.gui._rapid_cancellation)
        self.assertIsNone(self.gui._auto_move_cancellation)
        stale_callback.assert_not_called()

    def test_driver_and_tester_share_the_same_task_owner(self):
        self.gui.driver_mgr = None
        self.gui.dll_path_var, self.gui.inf_path_var = Mock(), Mock()
        self.gui._check_driver_status = Mock()
        with patch.object(gui_module, "DriverManager") as manager:
            manager.return_value.initialize.return_value = True
            self.assertTrue(self.gui._load_driver())
        self.assertIs(self.gui.input_tester.tasks, self.gui.driver_mgr.tasks)
        manager.return_value._setup_paths.assert_called_once_with(
            dll_path=self.gui.dll_path_var.get(), inf_path=self.gui.inf_path_var.get())

    def test_package_deletion_requires_explicit_confirmation(self):
        self.gui.remove_package_var.get.return_value = True
        with patch.object(gui_module.messagebox, "askyesno", return_value=False) as confirm, \
                patch.object(self.gui, "_begin_unload") as unload:
            self.gui._unload_driver()
        confirm.assert_called_once()
        unload.assert_not_called()
        self.assertFalse(self.gui._remove_package_requested)

    def test_confirmed_package_deletion_reaches_background_cleanup(self):
        manager = self.gui.driver_mgr
        self.gui.remove_package_var.get.return_value = True
        with patch.object(gui_module.messagebox, "askyesno", return_value=True), \
                patch.object(gui_module.threading, "Thread") as thread:
            self.gui._unload_driver()
            thread.call_args.kwargs["target"]()
            self.gui._poll_stop(False)
            thread.call_args.kwargs["target"]()
        manager.cleanup.assert_called_once_with(remove_package=True)
        self.gui._poll_cleanup(False)
        self.assertFalse(self.gui._remove_package_requested)

    def test_missing_export_does_not_latch_deletion_or_start_unload(self):
        self.gui.remove_package_var.get.return_value = True
        self.gui.driver_mgr.require_package_removal.side_effect = RuntimeError("loaded old DLL")
        with patch.object(gui_module.messagebox, "askyesno", return_value=True), \
                patch.object(self.gui, "_begin_unload") as unload:
            self.gui._unload_driver()
        unload.assert_not_called()
        self.assertFalse(self.gui._remove_package_requested)
        self.gui.driver_mgr.stop_input.assert_not_called()
        self.gui.driver_mgr.cleanup.assert_not_called()
        gui_module.messagebox.showerror.assert_called_once()
        self.gui.remove_package_var.get.return_value = False
        with patch.object(self.gui, "_begin_unload") as unload:
            self.gui._unload_driver()
        unload.assert_called_once()
        self.assertFalse(self.gui._remove_package_requested)

    def test_close_does_not_turn_a_checked_option_into_deletion(self):
        self.gui.remove_package_var.get.return_value = True
        manager = self.gui.driver_mgr
        with patch.object(gui_module.threading, "Thread") as thread:
            self.gui._begin_unload(close_after=True)
            thread.call_args.kwargs["target"]()
            self.gui._poll_stop(True)
            thread.call_args.kwargs["target"]()
        manager.cleanup.assert_called_once_with(remove_package=False)

    def test_unload_cancels_input_before_waiting(self):
        with patch.object(gui_module.threading, "Thread") as thread:
            self.gui._begin_unload()
        thread.return_value.start.assert_called_once()
        self.assertTrue(self.gui.is_unloading)
        self.assertFalse(self.gui.auto_move_running)
        self.assertFalse(self.gui.rapid_test_running)
        self.gui.input_tester.stop.assert_called_once()
        self.gui.driver_mgr.cleanup.assert_not_called()
        self.gui.root.after.assert_called_once_with(25, self.gui._poll_stop, False)

    def test_unload_failure_keeps_old_hotkeys_disabled_without_losing_other_callbacks(self):
        self.gui.hotkey_var = Mock()
        self.gui.hotkey_var.get.return_value = "F8"
        with patch.object(gui_module, "KeyboardHook") as hook:
            self.gui._toggle_keyboard_hook()
        hotkey = hook.return_value.register_hotkey.call_args.args[1]
        self.gui._toggle_auto_move = Mock()
        terminal = Mock()
        hotkey()
        self.gui._post_ui(terminal)
        with patch.object(gui_module.threading, "Thread") as thread:
            thread.return_value.start.side_effect = RuntimeError("thread unavailable")
            self.gui._begin_unload()
        hotkey()
        self.gui._drain_ui_events()
        self.gui._toggle_auto_move.assert_not_called()
        terminal.assert_called_once()
        self.assertIs(self.gui.keyboard_hook, hook.return_value)
        self.assertFalse(self.gui._hook_enabled)

    def test_native_stop_starts_while_input_worker_is_still_running(self):
        worker_entered = threading.Event()
        stop_entered = threading.Event()
        release_worker = threading.Event()
        release_stop = threading.Event()
        self.addCleanup(release_worker.set)
        self.addCleanup(release_stop.set)
        manager = self.gui.driver_mgr
        main_thread = threading.get_ident()
        stop_threads = []

        def work():
            worker_entered.set()
            release_worker.wait(2)

        def stop_input():
            stop_threads.append(threading.get_ident())
            stop_entered.set()
            release_stop.wait(2)

        manager.stop_input.side_effect = stop_input
        worker = self.gui.input_workers.start(work)
        self.assertTrue(worker_entered.wait(1))
        try:
            self.gui._begin_unload(close_after=True)
            self.assertTrue(stop_entered.wait(1))
            self.assertNotEqual(stop_threads, [main_thread])
            self.assertTrue(worker.is_alive())
            manager.cleanup.assert_not_called()
            self.gui.root.destroy.assert_not_called()
            self.gui._poll_stop(True)
            self.assertTrue(self.gui.is_unloading)
            self.assertFalse(self.gui._load_driver())
            release_stop.set()
            result = self.gui._stop_results.get(timeout=1)
            self.assertIsNone(result)
            self.gui._stop_results.put(result)
            self.gui._poll_stop(True)
            manager.cleanup.assert_not_called()
            self.assertTrue(self.gui.is_unloading)
        finally:
            release_stop.set()
            release_worker.set()
            worker.join(1)

    def test_idle_worker_does_not_allow_cleanup_before_native_completion(self):
        with patch.object(gui_module.threading, "Thread"):
            self.gui._begin_unload()
        self.gui._poll_stop(False)
        self.assertTrue(self.gui.is_unloading)
        self.gui.driver_mgr.cleanup.assert_not_called()
        self.gui._stop_results.put(None)
        self.gui._wait_for_workers = Mock()
        self.gui._poll_stop(False)
        self.gui._wait_for_workers.assert_called_once()

    def test_native_stop_failure_preserves_objects_and_retry_stops_again(self):
        manager, tester = self.gui.driver_mgr, self.gui.input_tester
        manager.stop_input.side_effect = OSError("release failed")
        with patch.object(gui_module.threading, "Thread") as thread:
            self.gui._begin_unload(close_after=True)
            thread.call_args.kwargs["target"]()
        self.gui._poll_stop(True)
        self.assertIs(self.gui.driver_mgr, manager)
        self.assertIs(self.gui.input_tester, tester)
        self.assertFalse(self.gui.is_unloading)
        self.assertFalse(self.gui.is_closing)
        manager.cleanup.assert_not_called()
        self.gui.root.destroy.assert_not_called()
        manager.stop_input.side_effect = None
        with patch.object(gui_module.threading, "Thread") as thread:
            self.gui._begin_unload()
            thread.call_args.kwargs["target"]()
        self.gui._wait_for_workers = Mock()
        self.gui._poll_stop(False)
        self.assertEqual(manager.stop_input.call_count, 2)
        self.gui._wait_for_workers.assert_called_once()

    def test_cleanup_runs_only_after_stop_and_workers_complete(self):
        manager = self.gui.driver_mgr
        self.gui.keyboard_hook = Mock()
        hook = self.gui.keyboard_hook
        operations = Mock()
        operations.attach_mock(manager.stop_input, "stop_input")
        operations.attach_mock(hook.stop, "stop_hook")
        operations.attach_mock(manager.cleanup, "cleanup")
        with patch.object(gui_module.threading, "Thread") as thread:
            self.gui._begin_unload(close_after=True)
            thread.call_args.kwargs["target"]()
            manager.cleanup.assert_not_called()
            self.gui._poll_stop(True)
            thread.call_args.kwargs["target"]()
        self.assertEqual([entry[0] for entry in operations.mock_calls],
                         ["stop_input", "stop_hook", "cleanup"])
        self.gui.root.destroy.assert_not_called()
        self.gui._poll_cleanup(True)
        self.gui.root.destroy.assert_called_once()

    def test_disconnect_allows_cleanup_without_claiming_input_release(self):
        for code in (1167, 433):
            with self.subTest(code=code):
                manager = Mock()
                self.gui.driver_mgr = manager
                self.gui.input_tester = Mock()
                release_error = ctypes.WinError(code)
                manager.stop_input.side_effect = release_error
                with patch.object(gui_module.threading, "Thread") as thread:
                    self.gui._begin_unload(close_after=True)
                    thread.call_args.kwargs["target"]()
                    self.gui._poll_stop(True)
                    thread.call_args.kwargs["target"]()
                manager.cleanup.assert_called_once()
                self.gui._poll_cleanup(True)
                self.assertIsNone(self.gui.driver_mgr)
                self.assertIs(self.gui._input_release_error, release_error)
                warning = gui_module.messagebox.showwarning.call_args.args[1]
                self.assertIn("输入释放未确认", warning)
                self.assertIn("PnP 管理清理已完成", warning)
                self.assertIn(str(code), warning)
                self.gui.root.destroy.assert_called_once()
                self.gui.root.destroy.reset_mock()

    def test_disconnect_and_cleanup_errors_survive_retry(self):
        manager = self.gui.driver_mgr
        release_error = ctypes.WinError(1167)
        cleanup_error = ctypes.WinError(5)
        manager.stop_input.side_effect = release_error
        manager.cleanup.side_effect = cleanup_error
        with patch.object(gui_module.threading, "Thread") as thread:
            self.gui._begin_unload(close_after=True)
            thread.call_args.kwargs["target"]()
            self.gui._poll_stop(True)
            thread.call_args.kwargs["target"]()
        self.gui._poll_cleanup(True)
        self.assertIs(self.gui.driver_mgr, manager)
        self.assertIs(self.gui._input_release_error, release_error)
        self.assertIs(self.gui._management_cleanup_error, cleanup_error)
        message = gui_module.messagebox.showerror.call_args.args[1]
        self.assertIn(str(release_error), message)
        self.assertIn(str(cleanup_error), message)
        self.gui.root.destroy.assert_not_called()
        manager.cleanup.side_effect = None
        with patch.object(gui_module.threading, "Thread") as thread:
            self.gui._begin_unload(close_after=True)
            thread.call_args.kwargs["target"]()
            self.gui._poll_stop(True)
            thread.call_args.kwargs["target"]()
        self.gui._poll_cleanup(True)
        self.assertEqual(manager.cleanup.call_count, 2)
        self.assertIsNone(self.gui.driver_mgr)
        self.assertIs(self.gui._input_release_error, release_error)
        self.assertIsNone(self.gui._management_cleanup_error)
        self.gui.root.destroy.assert_called_once()

    def test_disconnect_still_waits_for_input_workers(self):
        self.gui.input_workers = Mock()
        self.gui.input_workers.is_idle.return_value = False
        self.gui._stop_results.put(ctypes.WinError(1167))
        self.gui._poll_stop(True)
        self.gui.driver_mgr.cleanup.assert_not_called()
        self.gui.root.after.assert_called_once()
        self.gui.root.destroy.assert_not_called()

    def test_other_stop_errors_never_bypass_the_barrier(self):
        for code in (5, 6, 21, 29, 995, 1460):
            with self.subTest(code=code):
                self.gui._stop_results.put(ctypes.WinError(code))
                self.gui._poll_stop(True)
                self.gui.driver_mgr.cleanup.assert_not_called()
                self.gui.root.destroy.assert_not_called()

    def test_cleanup_failure_after_stop_retains_manager_for_retry(self):
        manager = self.gui.driver_mgr
        manager.cleanup.side_effect = OSError("cleanup failed")
        with patch.object(gui_module.threading, "Thread") as thread:
            self.gui._begin_unload(close_after=True)
            thread.call_args.kwargs["target"]()
            self.gui._poll_stop(True)
            thread.call_args.kwargs["target"]()
        self.gui._poll_cleanup(True)
        self.assertIs(self.gui.driver_mgr, manager)
        self.assertIsNotNone(self.gui.input_tester)
        self.assertFalse(self.gui.is_unloading)
        self.gui.root.destroy.assert_not_called()

    def test_pending_stop_cannot_be_started_twice(self):
        with patch.object(gui_module.threading, "Thread") as thread:
            self.gui._begin_unload()
            self.gui._begin_unload()
        thread.return_value.start.assert_called_once()

    def test_stop_thread_start_failure_is_retryable(self):
        manager = self.gui.driver_mgr
        with patch.object(gui_module.threading, "Thread") as thread:
            thread.return_value.start.side_effect = RuntimeError("thread unavailable")
            self.gui._begin_unload(close_after=True)
        self.assertIs(self.gui.driver_mgr, manager)
        self.assertFalse(self.gui.is_unloading)
        self.assertFalse(self.gui.is_closing)
        manager.cleanup.assert_not_called()
        self.gui.root.destroy.assert_not_called()

    def test_cleanup_thread_start_failure_is_retryable(self):
        manager = self.gui.driver_mgr
        with patch.object(gui_module.threading, "Thread") as thread:
            self.gui._begin_unload(close_after=True)
            thread.call_args.kwargs["target"]()
            thread.return_value.start.side_effect = RuntimeError("thread unavailable")
            self.gui._poll_stop(True)
        self.assertIs(self.gui.driver_mgr, manager)
        self.assertFalse(self.gui.is_unloading)
        self.assertFalse(self.gui.is_closing)
        manager.stop_input.assert_called_once()
        manager.cleanup.assert_not_called()
        self.gui.root.destroy.assert_not_called()

    def test_hook_failure_does_not_skip_stop_or_destroy_retryable_objects(self):
        manager = self.gui.driver_mgr
        self.gui.keyboard_hook = Mock()
        hook = self.gui.keyboard_hook
        hook.stop.side_effect = TimeoutError("hook still running")
        with patch.object(gui_module.threading, "Thread") as thread:
            self.gui._begin_unload(close_after=True)
            thread.call_args.kwargs["target"]()
            self.gui._poll_stop(True)
            thread.call_args.kwargs["target"]()
        self.gui._poll_cleanup(True)
        manager.stop_input.assert_called_once()
        manager.cleanup.assert_not_called()
        self.assertIs(self.gui.keyboard_hook, hook)
        self.assertIs(self.gui.driver_mgr, manager)
        self.gui.root.destroy.assert_not_called()

    def test_close_without_manager_waits_for_cleanup_notification(self):
        self.gui.driver_mgr = None
        self.gui.input_tester = None
        with patch.object(gui_module.threading, "Thread") as thread:
            self.gui._begin_unload(close_after=True)
            thread.call_args.kwargs["target"]()
            self.gui._poll_stop(True)
            thread.call_args.kwargs["target"]()
        self.gui.root.destroy.assert_not_called()
        self.gui._poll_cleanup(True)
        self.gui.root.destroy.assert_called_once()

    def test_live_worker_prevents_cleanup_and_reload(self):
        self.gui.input_workers = Mock()
        self.gui.input_workers.is_idle.return_value = False
        self.gui._wait_for_workers(float("inf"), False)
        self.gui.driver_mgr.cleanup.assert_not_called()
        self.gui.root.after.assert_called_once()
        self.assertFalse(self.gui._load_driver())

    def test_timeout_is_not_a_successful_unload(self):
        self.gui.input_workers = Mock()
        self.gui.input_workers.is_idle.return_value = False
        self.gui._wait_for_workers(0, False)
        self.gui.driver_mgr.cleanup.assert_not_called()
        self.gui.root.destroy.assert_not_called()
        self.assertIsNotNone(self.gui.driver_mgr)

    def test_no_worker_can_start_during_unload(self):
        self.gui.is_unloading = True
        target = Mock()
        self.assertIsNone(self.gui._start_worker(target))
        target.assert_not_called()


if __name__ == "__main__":
    unittest.main()
