"""Exercise GUI lifecycle methods without creating windows or loading a driver."""
from pathlib import Path
import queue
import sys
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "cqkeyspy_test"))
from gui import CQKeysGUI
from input_workers import InputWorkers


class GuiLifecycleTests(unittest.TestCase):
    def setUp(self):
        self.gui = CQKeysGUI.__new__(CQKeysGUI)
        self.gui.root = Mock()
        self.gui.driver_mgr = Mock()
        self.gui.input_tester = Mock()
        self.gui.keyboard_hook = None
        self.gui.is_driver_loaded = True
        self.gui.is_closing = self.gui.is_unloading = False
        self.gui.auto_move_running = self.gui.rapid_test_running = True
        self.gui.input_workers = InputWorkers()
        self.gui._cleanup_results = queue.Queue()
        for name in ("driver_status", "last_check", "start_hook_btn", "auto_move_status", "rapid_test_btn"):
            setattr(self.gui, name, Mock())
        self.gui._update_button_states = Mock()
        self.dialog = patch("gui.messagebox.showerror")
        self.dialog.start()
        self.addCleanup(self.dialog.stop)

    def test_unload_failure_preserves_manager_and_window_for_retry(self):
        manager = self.gui.driver_mgr
        self.gui.is_closing = True
        self.gui._finish_unload(OSError("device busy"), close_after=True)
        self.assertIs(self.gui.driver_mgr, manager)
        self.assertFalse(self.gui.is_closing)
        self.assertFalse(self.gui.is_driver_loaded)
        self.gui.root.destroy.assert_not_called()

    def test_close_destroys_window_only_after_success(self):
        self.gui._finish_unload(None, close_after=True)
        self.assertIsNone(self.gui.driver_mgr)
        self.gui.root.destroy.assert_called_once()

    def test_unload_cancels_input_before_waiting(self):
        self.gui._wait_for_workers = Mock()
        self.gui._begin_unload()
        self.assertTrue(self.gui.is_unloading)
        self.assertFalse(self.gui.auto_move_running)
        self.assertFalse(self.gui.rapid_test_running)
        self.gui.input_tester.stop.assert_called_once()
        self.gui.driver_mgr.cleanup.assert_not_called()
        self.gui._wait_for_workers.assert_called_once()

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
