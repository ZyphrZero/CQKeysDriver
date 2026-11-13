"""Lifecycle regressions. Native calls are fakes; no driver is installed."""
import ctypes
import logging
from pathlib import Path
import sys
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "cqkeyspy_test"))
from driver_manager import DriverManager


class DriverManagerTests(unittest.TestCase):
    def setUp(self):
        self.admin = patch.object(DriverManager, "is_admin", return_value=True)
        self.admin.start()
        self.addCleanup(self.admin.stop)
        self.manager = DriverManager()
        self.manager.dll_path = "test.dll"
        self.manager.sys_path = "C:\\test\\cqkeys.sys"
        self.native = Mock()
        self.native.GetLifecycleVersion.return_value = 1
        self.native.LoadNTDriver.return_value = 1
        self.native.SetHandle.return_value = 1
        self.native.StopInputProcessing.return_value = 1
        self.native.UnloadNTDriver.return_value = 1
        self.native.StopNTDriver.return_value = 1
        self.loader = patch("driver_manager.CDLL", return_value=self.native)
        self.loader.start()
        self.addCleanup(self.loader.stop)

    def test_initialize_does_not_uninstall_an_existing_service(self):
        self.assertTrue(self.manager.initialize())
        self.native.UnloadNTDriver.assert_not_called()

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

    def test_old_dll_is_rejected_before_touching_the_service(self):
        self.native.GetLifecycleVersion.return_value = 0
        self.assertFalse(self.manager.initialize())
        self.native.LoadNTDriver.assert_not_called()
        self.assertIsNone(self.manager.driver)

    def test_stop_retains_registration(self):
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

    def test_path_conflict_does_not_allow_cleanup_to_uninstall_the_other_binary(self):
        self.native.LoadNTDriver.return_value = 0
        ctypes.set_last_error(1073)
        self.assertFalse(self.manager.initialize())
        self.manager.cleanup()
        self.native.UnloadNTDriver.assert_not_called()
        self.assertIsNone(self.manager.driver)

    def test_load_failure_does_not_delete_a_service_it_did_not_start(self):
        self.native.LoadNTDriver.return_value = 0
        ctypes.set_last_error(5)
        self.assertFalse(self.manager.initialize())
        self.native.UnloadNTDriver.assert_not_called()


if __name__ == "__main__":
    logging.disable(logging.CRITICAL)
    unittest.main()
