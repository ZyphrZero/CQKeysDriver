import ctypes
import contextlib
import io
import unittest
from unittest.mock import Mock, patch

from hook_tests.simple_input import SimpleInputTester, InputInitializationError, main
from test_input_tasks import native_tasks, fail
from input_tasks import FAILED, InputTaskError


class SimpleInputTests(unittest.TestCase):
    def setUp(self):
        self.native = native_tasks()
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
        for version in (1, 3):
            with self.subTest(version=version):
                self.native.GetLifecycleVersion.return_value = version
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

    def test_initialization_failure_retains_cleanup_owner(self):
        self.native.SetHandle.side_effect = fail(5)
        with self.assertRaises(InputInitializationError) as caught:
            SimpleInputTester("cqkeysdll.dll", "cqkeys.inf")
        tester = caught.exception.tester
        self.assertTrue(tester.needs_cleanup)
        tester.cleanup()
        self.assertIsNone(tester.driver)

    def test_main_cleans_up_failed_initialization(self):
        self.native.SetHandle.side_effect = fail(5)
        with patch("builtins.print"):
            self.assertEqual(main(), 1)
        self.native.UnloadNTDriver.assert_called_once()

    def test_main_reports_input_and_cleanup_failures_without_masking(self):
        self.native.task_result.state = FAILED
        self.native.task_result.error = 29
        self.native.UnloadNTDriver.side_effect = fail(5)
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            self.assertEqual(main(), 1)
        self.assertIn("error=29", output.getvalue())
        self.assertIn("清理失败", output.getvalue())
        self.assertIn("5", output.getvalue())

    def test_interrupted_initialization_keeps_owner_for_cleanup(self):
        interruption = KeyboardInterrupt("initialization interrupted")
        self.native.SetHandle.side_effect = interruption
        with self.assertRaises(BaseException) as caught:
            SimpleInputTester("cqkeysdll.dll", "cqkeys.inf")
        self.assertIsInstance(caught.exception, InputInitializationError)
        self.assertIs(caught.exception.__cause__, interruption)
        self.assertTrue(caught.exception.tester.needs_cleanup)
        caught.exception.tester.cleanup()
        self.native.UnloadNTDriver.assert_called_once()

    def test_main_reports_interrupt_and_cleanup_failure(self):
        self.native.WaitInputTask.side_effect = [KeyboardInterrupt("user cancelled"), 1, 1]
        self.native.UnloadNTDriver.side_effect = fail(5)
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            try:
                self.assertEqual(main(), 1)
            except KeyboardInterrupt:
                self.fail("main must report cancellation and cleanup separately")
        self.assertIn("取消", output.getvalue())
        self.assertIn("清理失败", output.getvalue())

    def test_text_uses_shared_task_binding_and_propagates_failure(self):
        tester = SimpleInputTester("cqkeysdll.dll", "cqkeys.inf")
        self.native.task_result.state = FAILED
        self.native.task_result.error = 29
        with self.assertRaises(InputTaskError):
            tester.type_string("A!", interval=.01)
        self.assertEqual(len(self.native.submissions), 1)
        self.native.KeyDown.assert_not_called()
        self.native.ReleaseInputTask.assert_not_called()
