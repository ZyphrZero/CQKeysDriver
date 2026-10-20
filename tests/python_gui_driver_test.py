"""Hidden Tk tests. Installer outcomes are simulated; no driver is installed."""
import json
from pathlib import Path
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "example/python"))

import gui
from driver_manager import DEFAULT_DLL_PATH
from driver_setup import InstallError, InstallResult, query_device_status


@unittest.skipUnless(DEFAULT_DLL_PATH.is_file(), "Build the x64 DLL before running GUI tests")
class GuiDriverTests(unittest.TestCase):
    def setUp(self):
        self.dialogs = []
        self.callback_errors = []
        for name in ("showerror", "showinfo", "showwarning"):
            dialog = patch.object(
                gui.messagebox, name,
                side_effect=lambda title, message: self.dialogs.append((title, message)),
            )
            dialog.start()
            self.addCleanup(dialog.stop)
        with patch.object(gui.CQKeysGUI, "check_privileges", return_value=True):
            self.app = gui.CQKeysGUI()
        self.app.root.withdraw()
        self.app.root.report_callback_exception = (
            lambda *details: self.callback_errors.append(details)
        )
        self.temp = tempfile.TemporaryDirectory(prefix="cqkeys gui ")
        self.addCleanup(self.temp.cleanup)
        self.app.config_file = str(Path(self.temp.name) / "driver_config.json")
        self.app.dll_path_var.set(str(DEFAULT_DLL_PATH))
        self.app._suggest_driver_files()
        self.wait_idle()

    def tearDown(self):
        if self.app._driver_busy:
            self.wait_idle()
        self.app._on_closing()

    def wait_idle(self):
        started = time.monotonic()
        timed_out = []

        def check():
            elapsed = time.monotonic() - started
            if elapsed > 5:
                timed_out.append(True)
                self.app.root.quit()
            elif self.callback_errors or (elapsed > 0.2 and not self.app._driver_busy):
                self.app.root.quit()
            else:
                self.app.root.after(20, check)

        self.app.root.after(20, check)
        self.app.root.mainloop()
        self.assertFalse(timed_out, "GUI operation did not complete")
        self.assertFalse(self.callback_errors, self.callback_errors)

    def test_refresh_works_without_a_connection(self):
        self.assertFalse(self.app.is_connected)
        self.assertIn(query_device_status().text, self.app.device_status.cget("text"))
        self.assertEqual(str(self.app.check_status_btn.cget("state")), "normal")
        self.assertEqual(str(self.app.submit_btn.cget("state")), "disabled")

    def test_real_missing_device_connection_keeps_error_details(self):
        if query_device_status().present:
            self.skipTest("This test requires the CQKeys device to be absent")
        self.app._connect_device()
        self.wait_idle()
        self.assertFalse(self.app.is_connected)
        self.assertIsNone(self.app.driver_mgr)
        self.assertTrue(any("2" in message and "安装并连接" in message
                            for _, message in self.dialogs))

    def test_restart_result_never_attempts_connection(self):
        simulated = InstallResult(True, "SIMULATED installer exit 3010")
        with patch.object(gui, "install_driver", return_value=simulated):
            with patch.object(gui.DriverManager, "connect") as connect:
                with patch.object(gui, "query_device_status") as query:
                    self.app._install_and_connect()
                    self.wait_idle()
                    connect.assert_not_called()
                    query.assert_not_called()
        self.assertFalse(self.app.is_connected)
        self.assertIn("等待重启", self.app.driver_status.cget("text"))
        self.assertTrue(any("3010" in message for _, message in self.dialogs))

    def test_signature_failure_never_attempts_connection(self):
        simulated = InstallError(0x800B0109, "SIMULATED untrusted-root failure")
        with patch.object(gui, "install_driver", side_effect=simulated):
            with patch.object(gui.DriverManager, "connect") as connect:
                self.app._install_and_connect()
                self.wait_idle()
                connect.assert_not_called()
        self.assertFalse(self.app.is_connected)
        self.assertTrue(any("800B0109" in message and "SIMULATED" in message
                            for _, message in self.dialogs))

    def test_installer_zero_is_not_connection_success(self):
        if query_device_status().present:
            self.skipTest("This test requires the CQKeys device to be absent")
        simulated = InstallResult(False, "SIMULATED installer exit 0")
        with patch.object(gui, "install_driver", return_value=simulated):
            self.app._install_and_connect()
            self.wait_idle()
        self.assertFalse(self.app.is_connected)
        self.assertTrue(any("安装工具已完成，但设备连接失败" in message
                            for _, message in self.dialogs))

    def test_invalid_package_does_not_execute_native_installer(self):
        self.app.inf_path_var.set(str(Path(self.temp.name) / "cqkeys.inf"))
        with patch("driver_setup.subprocess.run") as run:
            self.app._install_and_connect()
            self.wait_idle()
            run.assert_not_called()
        self.assertFalse(self.app.is_connected)
        self.assertTrue(any("驱动包不完整" in message for _, message in self.dialogs))

    def test_background_work_is_serialized_and_blocks_closing(self):
        release = threading.Event()
        ran_second = []
        responsive = []
        try:
            self.app._run_driver_task(
                "测试后台任务", lambda: release.wait(3), lambda result, error: None,
            )
            self.app._run_driver_task(
                "重复任务", lambda: ran_second.append(True), lambda result, error: None,
            )
            self.assertEqual(str(self.app.install_btn.cget("state")), "disabled")
            self.app._on_closing()
            self.assertFalse(self.app.is_closing)
            self.app.root.after(30, lambda: responsive.append(True))
            self.app.root.after(100, release.set)
            self.wait_idle()
        finally:
            release.set()
        self.assertTrue(responsive)
        self.assertFalse(ran_second)
        self.assertEqual(str(self.app.install_btn.cget("state")), "normal")

    def test_configuration_preserves_paths_and_other_settings(self):
        original = {"custom": "keep", "auto_move": {"speed": 7}}
        Path(self.app.config_file).write_text(json.dumps(original))
        self.app.inf_path_var.set(r"C:\CQ Keys\cqkeys.inf")
        self.app.setup_path_var.set(r"C:\CQ Keys\device_setup.exe")
        self.app.auto_move_config["speed"] = 7
        self.app.save_path_config()
        saved = json.loads(Path(self.app.config_file).read_text())
        self.assertEqual(saved["custom"], "keep")
        self.assertEqual(saved["auto_move"]["speed"], 7)
        self.assertEqual(saved["inf_path"], r"C:\CQ Keys\cqkeys.inf")
        self.app.save_auto_move_config()
        saved = json.loads(Path(self.app.config_file).read_text())
        self.assertEqual(saved["setup_path"], r"C:\CQ Keys\device_setup.exe")
        self.assertEqual(saved["custom"], "keep")


if __name__ == "__main__":
    unittest.main()
