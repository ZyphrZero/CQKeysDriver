"""Unit tests for package selection and installer result handling; never install a driver."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "example/python"))

from driver_setup import (
    DeviceStatus, InstallError, describe_connection_error, find_driver_files,
    install_driver, validate_package,
)


class DriverSetupTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="cqkeys package ")
        self.root = Path(self.temp.name).resolve()
        self.addCleanup(self.temp.cleanup)

    def file(self, relative, content=""):
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")
        return path

    def package(self):
        inf = self.file("cqkeys.inf", "[Models]\n%Device%=Install,ROOT\\CQKEYSVHF\n")
        self.file("cqkeys.sys")
        self.file("cqkeys.cat")
        setup = self.file("device_setup.exe")
        return inf, setup

    def test_flat_release_package(self):
        inf, setup = self.package()
        dll = self.file("cqkeysdll.dll")
        self.assertEqual(find_driver_files(dll), (inf, setup))

    def test_build_layout_uses_dll_configuration(self):
        dll = self.file("cqkeysdll/x64/Release/cqkeysdll.dll")
        inf = self.file("cqkeys/x64/Release/cqkeys/cqkeys.inf")
        setup = self.file("tools/x64/Release/device_setup.exe")
        self.file("cqkeys/x64/Debug/cqkeys/cqkeys.inf")
        self.assertEqual(find_driver_files(dll), (inf, setup))

    def test_custom_dll_does_not_select_unrelated_build(self):
        dll = self.file("custom/cqkeysdll.dll")
        self.file("cqkeys/x64/Release/cqkeys/cqkeys.inf")
        self.assertEqual(find_driver_files(dll), (None, None))

    def test_incomplete_package_never_runs_installer(self):
        inf, setup = self.package()
        (self.root / "cqkeys.cat").unlink()
        with patch("driver_setup.subprocess.run") as run:
            with self.assertRaisesRegex(FileNotFoundError, "cqkeys.cat"):
                install_driver(inf, setup)
            run.assert_not_called()

    def test_old_hardware_id_is_rejected(self):
        inf, setup = self.package()
        inf.write_text("[Models]\n%Device%=Install,ROOT\\lykeys\n", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "CQKEYSVHF"):
            validate_package(inf, setup)

    def test_restart_is_reported_without_claiming_connection(self):
        inf, setup = self.package()
        # Simulated native exit code exercises the wrapper, not Windows installation.
        completed = subprocess.CompletedProcess([], 3010, "Restart required: yes")
        with patch("driver_setup.ctypes.windll.shell32.IsUserAnAdmin", return_value=1):
            with patch("driver_setup.subprocess.run", return_value=completed) as run:
                result = install_driver(inf, setup)
        self.assertTrue(result.reboot_required)
        self.assertEqual(run.call_args.args[0], [str(setup), "install", str(inf)])
        self.assertFalse(run.call_args.kwargs.get("shell", False))

    def test_failure_retains_native_output_and_unsigned_error_code(self):
        inf, setup = self.package()
        completed = subprocess.CompletedProcess([], -536870325, "Staging INF failed")
        with patch("driver_setup.ctypes.windll.shell32.IsUserAnAdmin", return_value=1):
            with patch("driver_setup.subprocess.run", return_value=completed):
                with self.assertRaises(InstallError) as raised:
                    install_driver(inf, setup)
        self.assertEqual(raised.exception.code, 0xE000024B)
        self.assertIn("Staging INF failed", str(raised.exception))
        self.assertIn("签名", str(raised.exception))

    def test_permission_failure_never_runs_installer(self):
        inf, setup = self.package()
        with patch("driver_setup.ctypes.windll.shell32.IsUserAnAdmin", return_value=0):
            with patch("driver_setup.subprocess.run") as run:
                with self.assertRaisesRegex(PermissionError, "管理员"):
                    install_driver(inf, setup)
                run.assert_not_called()

    def test_timeout_reports_unknown_outcome_without_retrying(self):
        inf, setup = self.package()
        with patch("driver_setup.ctypes.windll.shell32.IsUserAnAdmin", return_value=1):
            with patch("driver_setup.subprocess.run",
                       side_effect=subprocess.TimeoutExpired("device_setup.exe", 120)) as run:
                with self.assertRaisesRegex(RuntimeError, "安装结果未知"):
                    install_driver(inf, setup)
                self.assertEqual(run.call_count, 1)

    def test_missing_device_error_explains_installation(self):
        error = OSError(2, "ConnectDevice failed")
        message = describe_connection_error(error)
        self.assertIn("安装", message)
        self.assertIn(r"\\.\cqkeys", message)
        self.assertIn("2", message)

    def test_access_denied_does_not_suggest_reinstalling(self):
        message = describe_connection_error(OSError(5, "access denied"))
        self.assertIn("管理员", message)
        self.assertNotIn("安装并连接", message)

    def test_device_problem_is_not_reported_as_ready(self):
        self.assertIn("未安装", DeviceStatus(False).text)
        self.assertIn("52", DeviceStatus(True, False, 52).text)
        self.assertIn("签名", DeviceStatus(True, False, 52).text)
        self.assertIn("已启动", DeviceStatus(True, True, 0).text)


if __name__ == "__main__":
    unittest.main()
