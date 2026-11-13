"""Lifecycle regressions. Native calls are fakes; no driver is installed."""
import ctypes
import logging
from pathlib import Path
import sys
import unittest
from unittest.mock import Mock, mock_open, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "cqkeyspy_test"))
from driver_manager import DriverManager
from input_tasks import InputTasks, NativeCallError, action, KEY_DOWN
from test_input_tasks import native_tasks, fail


class DriverManagerTests(unittest.TestCase):
    def setUp(self):
        self.admin = patch.object(DriverManager, "is_admin", return_value=True)
        self.admin.start()
        self.addCleanup(self.admin.stop)
        self.manager = DriverManager()
        self.manager.dll_path = "test.dll"
        self.manager.sys_path = "C:\\test\\cqkeys.sys"
        self.native = Mock()
        self.native.GetLifecycleVersion.return_value = 2
        self.native.LoadNTDriver.return_value = 1
        self.native.SetHandle.return_value = 1
        self.native.StopInputProcessing.return_value = 1
        self.native.UnloadNTDriver.return_value = 1
        self.native.StopNTDriver.return_value = 1
        self.loader = patch("driver_manager.CDLL", return_value=self.native)
        self.loader.start()
        self.addCleanup(self.loader.stop)

    def test_initialize_does_not_remove_an_existing_device(self):
        self.assertTrue(self.manager.initialize())
        self.native.UnloadNTDriver.assert_not_called()

    def test_path_config_uses_the_same_utf8_encoding_as_the_gui(self):
        config = '{"dll_path": "中文/cqkeysdll.dll", "sys_path": "中文/cqkeys.sys"}'
        with patch("driver_manager.open", mock_open(read_data=config)) as source, \
                patch("driver_manager.os.path.exists", return_value=True):
            self.manager._setup_paths()
        source.assert_called_once_with("driver_config.json", "r", encoding="utf-8")
        self.assertEqual(self.manager.dll_path, "中文/cqkeysdll.dll")
        self.assertEqual(self.manager.sys_path, "中文/cqkeys.sys")
        self.native.LoadNTDriver.assert_not_called()

    def test_failed_device_open_is_not_reported_ready(self):
        self.native.SetHandle.return_value = 0
        ctypes.set_last_error(5)
        self.assertFalse(self.manager.initialize())

    def test_unload_failure_is_reported_and_can_be_retried(self):
        self.manager.driver = self.native
        self.native.UnloadNTDriver.return_value = 0
        ctypes.set_last_error(5)
        with self.assertRaises(OSError):
            self.manager.cleanup()
        self.assertIs(self.manager.driver, self.native)
        self.native.UnloadNTDriver.return_value = 1
        self.manager.cleanup()
        self.assertIsNone(self.manager.driver)

    def test_successful_cleanup_is_idempotent(self):
        self.manager.driver = self.native
        self.manager.cleanup()
        self.manager.cleanup()
        self.assertEqual(self.native.UnloadNTDriver.call_count, 1)

    def test_incompatible_dll_is_rejected_before_installation(self):
        for version in (1, 3):
            with self.subTest(version=version):
                self.native.GetLifecycleVersion.return_value = version
                self.assertFalse(self.manager.initialize())
                self.native.LoadNTDriver.assert_not_called()
                self.assertIsNone(self.manager.driver)

    def test_stop_disables_without_removing_the_device(self):
        self.manager.driver = self.native
        self.manager.cleanup(uninstall=False)
        self.native.StopNTDriver.assert_called_once_with(b'cqkeys')
        self.native.UnloadNTDriver.assert_not_called()
        self.assertIsNone(self.manager.driver)

    def test_failed_input_release_is_not_hidden(self):
        self.manager.driver = self.native
        self.native.StopInputProcessing.return_value = 0
        ctypes.set_last_error(29)
        with self.assertRaises(OSError):
            self.manager.stop_input()
        self.assertIs(self.manager.driver, self.native)

    def test_busy_session_retains_manager_for_native_ownership_aware_cleanup(self):
        self.native.LoadNTDriver.return_value = 0
        ctypes.set_last_error(170)
        self.assertFalse(self.manager.initialize())
        self.manager.cleanup()
        self.native.UnloadNTDriver.assert_called_once_with(b'cqkeys')
        self.assertIsNone(self.manager.driver)

    def test_install_failure_does_not_automatically_remove_the_device(self):
        self.native.LoadNTDriver.return_value = 0
        ctypes.set_last_error(5)
        self.assertFalse(self.manager.initialize())
        self.native.UnloadNTDriver.assert_not_called()

    def test_task_results_are_collected_before_pnp_and_cleanup_can_retry(self):
        native = native_tasks()
        self.manager.driver = native
        self.manager.tasks = InputTasks(native)
        task = self.manager.tasks.submit([action(KEY_DOWN, 65)])
        native.UnloadNTDriver.return_value = 1
        native.CleanupInputRuntime.side_effect = fail(5)
        with self.assertRaises(NativeCallError):
            self.manager.cleanup()
        self.assertIs(self.manager.driver, native)
        self.assertIsNone(self.manager.tasks.active)
        self.assertIs(self.manager.tasks.last_result, task.result)
        self.assertTrue(task.released)
        operations = [entry[0] for entry in native.mock_calls]
        self.assertLess(operations.index("QueryInputTask"), operations.index("UnloadNTDriver"))
        native.CleanupInputRuntime.side_effect = None
        self.manager.cleanup()
        self.assertIsNone(self.manager.driver)
        native.ReleaseInputTask.assert_called_once()

    def test_result_collection_failure_prevents_pnp_cleanup(self):
        native = native_tasks()
        self.manager.driver = native
        self.manager.tasks = InputTasks(native)
        task = self.manager.tasks.submit([action(KEY_DOWN, 65)])
        native.QueryInputTask.side_effect = fail(5)
        with self.assertRaises(NativeCallError):
            self.manager.cleanup()
        native.UnloadNTDriver.assert_not_called()
        self.assertIs(self.manager.tasks.active, task)


if __name__ == "__main__":
    logging.disable(logging.CRITICAL)
    unittest.main()
