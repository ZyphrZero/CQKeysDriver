import ctypes
import unittest
from unittest.mock import Mock, patch

from hook_tests.simple_input import SimpleInputTester


class SimpleInputTests(unittest.TestCase):
    def setUp(self):
        self.native = Mock()
        self.native.GetLifecycleVersion.return_value = 2
        self.native.LoadNTDriver.return_value = 1
        self.native.SetHandle.return_value = 1
        self.native.UnloadNTDriver.return_value = 1
        self.admin = patch("hook_tests.simple_input.ctypes.windll.shell32.IsUserAnAdmin", return_value=1)
        self.loader = patch("hook_tests.simple_input.CDLL", return_value=self.native)
        self.admin.start()
        self.loader.start()
        self.addCleanup(self.admin.stop)
        self.addCleanup(self.loader.stop)

    def test_explicit_package_paths_and_no_preemptive_unload(self):
        tester = SimpleInputTester("cqkeysdll.dll", "C:\\test\\cqkeys.inf")
        self.native.LoadNTDriver.assert_called_once_with(b"cqkeys", b"C:\\test\\cqkeys.inf")
        self.native.UnloadNTDriver.assert_not_called()
        tester.cleanup()

    def test_incompatible_dll_is_rejected_before_install(self):
        self.native.GetLifecycleVersion.return_value = 1
        with self.assertRaises(RuntimeError):
            SimpleInputTester("incompatible.dll", "cqkeys.inf")
        self.native.LoadNTDriver.assert_not_called()
        self.native.UnloadNTDriver.assert_not_called()

    def test_void_input_errors_are_not_ignored(self):
        ctypes.set_last_error(29)
        with self.assertRaises(OSError):
            SimpleInputTester._check_input_error(None, None, ())

    def test_cleanup_failure_retains_retryable_driver(self):
        tester = SimpleInputTester("cqkeysdll.dll", "C:\\test\\cqkeys.inf")
        self.native.UnloadNTDriver.return_value = 0
        ctypes.set_last_error(5)
        with self.assertRaises(OSError):
            tester.cleanup()
        self.assertIs(tester.driver, self.native)
