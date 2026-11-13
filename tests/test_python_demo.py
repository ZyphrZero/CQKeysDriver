"""Tests use simulated DLL calls and never install or send input to a driver."""

import contextlib
import ctypes
import io
from pathlib import Path
import tempfile
import unittest
from unittest.mock import Mock, patch

from example.python.demo import CqKeys, DriverCallError, main, parse_args, validate_package
from test_input_tasks import native_tasks, fail
from input_tasks import CANCELLED, NativeCallError


class PythonDemoTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.dll = self.root / "cqkeysdll.dll"
        self.dll.touch()
        self.native = native_tasks()
        self.native.GetLifecycleVersion.return_value = 2
        self.native.GetDriverStatus.return_value = 1
        self.native.LoadNTDriver.return_value = 1
        self.native.SetHandle.return_value = 1
        self.native.CheckDeviceStatus.return_value = None
        self.native.StopNTDriver.return_value = 1
        self.native.UnloadNTDriver.return_value = 1
        self.loader = patch("example.python.demo.ctypes.CDLL", return_value=self.native)
        self.load_dll = self.loader.start()
        self.addCleanup(self.loader.stop)

    def package(self):
        for extension in ("inf", "sys", "cat"):
            (self.root / f"cqkeys.{extension}").touch()
        return self.root / "cqkeys.inf"

    def test_missing_catalog_fails_before_installation(self):
        inf = self.root / "cqkeys.inf"
        inf.touch()
        (self.root / "cqkeys.sys").touch()
        client = CqKeys(self.dll)
        with self.assertRaises(FileNotFoundError):
            client.connect(inf)
        self.assertFalse(client.needs_cleanup)
        self.native.LoadNTDriver.assert_not_called()

    def test_bare_sys_is_not_an_installation_package(self):
        sys_file = self.package().with_suffix(".sys")
        with self.assertRaises(ValueError):
            validate_package(sys_file)

    def test_package_connect_and_explicit_removal(self):
        client = CqKeys(self.dll)
        inf = self.package()
        self.assertEqual(client.connect(inf), 1)
        self.native.LoadNTDriver.assert_called_once_with(b"cqkeys", str(inf.resolve()).encode("mbcs"))
        client.close(uninstall=True)
        self.native.UnloadNTDriver.assert_called_once_with(b"cqkeys")
        self.assertFalse(client.needs_cleanup)

    def test_incompatible_abi_is_rejected(self):
        for version in (1, 3):
            with self.subTest(version=version):
                self.native.GetLifecycleVersion.return_value = version
                with self.assertRaises(RuntimeError):
                    CqKeys(self.dll)
                self.native.SetHandle.assert_not_called()
                self.native.LoadNTDriver.assert_not_called()

    def test_bool_failure_preserves_win32_error(self):
        self.native.LoadNTDriver.side_effect = lambda *_: (ctypes.set_last_error(577), 0)[1]
        client = CqKeys(self.dll)
        with self.assertRaises(DriverCallError) as caught:
            client.connect(self.package())
        self.assertEqual(caught.exception.code, 577)
        self.assertTrue(client.needs_cleanup)
        self.native.SetHandle.assert_not_called()

    def test_void_failure_is_not_reported_successful(self):
        self.native.MouseMoveRELATIVE.side_effect = lambda *_: ctypes.set_last_error(29)
        client = CqKeys(self.dll)
        with self.assertRaises(DriverCallError) as caught:
            client.move(100, 0)
        self.assertEqual(caught.exception.code, 29)

    def test_interrupted_key_hold_cancels_and_releases_task(self):
        client = CqKeys(self.dll)
        self.native.WaitInputTask.side_effect = [KeyboardInterrupt, 1]
        self.native.task_result.state = CANCELLED
        self.native.task_result.error = 1223
        with self.assertRaises(KeyboardInterrupt):
            client.tap(0x41)
        self.native.CancelInputTask.assert_called_once_with(17)
        self.native.ReleaseInputTask.assert_called_once_with(17)
        self.native.KeyUp.assert_not_called()

    def test_tap_consumer_and_initial_delay_are_native_actions(self):
        client = CqKeys(self.dll)
        client.tap(0xcd, consumer=True, initial_delay=3)
        self.assertEqual(self.native.submissions[0][0],
                         [(9, 0, 0, 3000), (3, 0xcd, 0, 50), (4, 0xcd, 0, 0)])

    def test_runtime_cleanup_failure_preserves_client_for_retry(self):
        client = CqKeys(self.dll)
        client.connect()
        self.native.CleanupInputRuntime.side_effect = fail(5)
        with self.assertRaises(NativeCallError):
            client.close()
        self.assertTrue(client.needs_cleanup)
        self.native.CleanupInputRuntime.side_effect = None
        client.close()
        self.assertFalse(client.needs_cleanup)

    def test_cleanup_failure_can_be_retried(self):
        client = CqKeys(self.dll)
        client.connect()
        self.native.StopNTDriver.side_effect = lambda *_: (ctypes.set_last_error(5), 0)[1]
        with self.assertRaises(DriverCallError):
            client.close()
        self.assertTrue(client.needs_cleanup)
        self.native.StopNTDriver.side_effect = None
        client.close()
        self.assertFalse(client.needs_cleanup)

    def test_default_cli_only_checks_status_and_disables(self):
        with patch("example.python.demo.require_windows_admin"), contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(main(["--dll", str(self.dll)]), 0)
        self.native.KeyDown.assert_not_called()
        self.native.MouseMoveRELATIVE.assert_not_called()
        self.native.ConsumerKeyDown.assert_not_called()
        self.native.StopNTDriver.assert_called_once_with(b"cqkeys")

    def test_cli_reports_install_and_cleanup_failures(self):
        self.native.LoadNTDriver.side_effect = lambda *_: (ctypes.set_last_error(577), 0)[1]
        self.native.StopNTDriver.side_effect = lambda *_: (ctypes.set_last_error(5), 0)[1]
        errors = io.StringIO()
        with patch("example.python.demo.require_windows_admin"), contextlib.redirect_stderr(errors):
            result = main(["--dll", str(self.dll), "--inf", str(self.package())])
        self.assertEqual(result, 1)
        self.assertIn("LoadNTDriver", errors.getvalue())
        self.assertIn("StopNTDriver", errors.getvalue())

    def test_cli_reports_interrupt_cleanup_cause_as_failure(self):
        self.native.WaitInputTask.side_effect = [KeyboardInterrupt("user cancelled"), 1]
        self.native.CancelInputTask.side_effect = fail(5)
        errors = io.StringIO()
        with patch("example.python.demo.require_windows_admin"), contextlib.redirect_stderr(errors), \
                contextlib.redirect_stdout(io.StringIO()):
            result = main(["--dll", str(self.dll), "--tap-key", "0x41"])
        self.assertEqual(result, 1)
        self.assertIn("CancelInputTask", errors.getvalue())
        self.assertIn("5", errors.getvalue())

    def test_invalid_input_is_rejected_before_dll_loading(self):
        for arguments in (["--tap-key", "0x100"], ["--consumer", "0"], ["--move", "32768", "0"], ["--delay", "nan"]):
            with self.subTest(arguments=arguments), contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit):
                    parse_args(arguments)
        self.load_dll.assert_not_called()

    def test_device_instance_error_normalizes_signed_and_unsigned_codes(self):
        for code in (-536870389, 0xE000020B):
            with self.subTest(code=code):
                error = DriverCallError("SetHandle", code)
                self.assertEqual(error.code, 0xE000020B)
                self.assertIn("ERROR_NO_SUCH_DEVINST", str(error))
                self.assertIn("ROOT\\CQKEYS_VHF\\0000", str(error))
                self.assertIn("--inf", str(error))

    def test_other_setupapi_error_preserves_unsigned_code_without_overflow(self):
        error = DriverCallError("LoadNTDriver", 0xE000FFFF)
        self.assertEqual(error.code, 0xE000FFFF)
        self.assertIn("0xE000FFFF", str(error))

    def test_missing_device_remains_failure_after_cleanup(self):
        self.native.SetHandle.side_effect = lambda: (ctypes.set_last_error(-536870389), 0)[1]
        output = io.StringIO()
        errors = io.StringIO()
        with patch("example.python.demo.require_windows_admin"), contextlib.redirect_stdout(output), contextlib.redirect_stderr(errors):
            result = main(["--dll", str(self.dll)])
        self.assertEqual(result, 1)
        self.assertIn("ERROR_NO_SUCH_DEVINST", errors.getvalue())
        self.assertIn("驱动测试仍然失败", output.getvalue())
        self.native.LoadNTDriver.assert_not_called()
        self.native.StopNTDriver.assert_called_once_with(b"cqkeys")
