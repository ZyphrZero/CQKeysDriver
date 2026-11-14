"""Lifecycle regressions. Native calls are fakes; no driver is installed."""
import ctypes
import json
import logging
from pathlib import Path
import sys
import tempfile
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
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.directory = Path(directory.name) / "package"
        self.directory.mkdir()
        for name in ("cqkeysdll.dll", "cqkeys.inf", "cqkeys.sys", "cqkeys.cat"):
            (self.directory / name).touch()
        self.manager = DriverManager()
        self.manager.dll_path = str(self.directory / "cqkeysdll.dll")
        self.manager.inf_path = str(self.directory / "cqkeys.inf")
        self.native = Mock()
        self.native.GetLifecycleVersion.return_value = 2
        self.native.LoadNTDriver.return_value = 1
        self.native.SetHandle.return_value = 1
        self.native.StopInputProcessing.return_value = 1
        self.native.UnloadNTDriver.return_value = 1
        self.native.StopNTDriver.return_value = 1
        self.loader = patch("driver_manager.CDLL", return_value=self.native)
        self.load_dll = self.loader.start()
        self.addCleanup(self.loader.stop)
        module_path = patch.object(DriverManager, "_get_loaded_dll_path", return_value=self.manager.dll_path)
        self.module_path = module_path.start()
        self.addCleanup(module_path.stop)

    def test_initialize_does_not_remove_an_existing_device(self):
        self.assertTrue(self.manager.initialize())
        self.native.UnloadNTDriver.assert_not_called()

    def test_path_config_uses_the_same_utf8_encoding_as_the_gui(self):
        package = self.directory / "中文"
        package.mkdir()
        for name in ("cqkeys.inf", "cqkeys.sys", "cqkeys.cat"):
            (package / name).touch()
        config = {"dll_path": self.manager.dll_path, "inf_path": str(package / "cqkeys.inf")}
        with patch("driver_manager.open", mock_open(read_data=json.dumps(config, ensure_ascii=False))) as source, \
                patch("driver_manager.os.path.exists", return_value=True):
            self.manager._setup_paths()
        source.assert_called_once_with("driver_config.json", "r", encoding="utf-8")
        self.assertEqual(self.manager.dll_path, config["dll_path"])
        self.assertEqual(self.manager.inf_path, config["inf_path"])
        self.native.LoadNTDriver.assert_not_called()

    def test_initialize_passes_inf_path_to_native_installation(self):
        self.assertTrue(self.manager.initialize())
        self.load_dll.assert_called_once_with(self.manager.dll_path, use_last_error=True)
        self.native.LoadNTDriver.assert_called_once_with(
            b"cqkeys", self.manager.inf_path.encode("mbcs"))
        self.native.SetHandle.assert_called_once()

    def test_initialize_logs_actual_module_path_separately_from_requested_path(self):
        actual = str(self.directory / "old" / "cqkeysdll.dll")
        self.module_path.return_value = actual
        with self.assertLogs(level="INFO") as messages:
            self.assertTrue(self.manager.initialize())
        self.assertEqual(self.manager.loaded_dll_path, actual)
        self.assertIn("请求加载 DLL: " + self.manager.dll_path, "\n".join(messages.output))
        self.assertIn("实际加载 DLL: " + actual, "\n".join(messages.output))
        self.module_path.assert_called_once_with(self.native)

    def test_module_path_query_failure_is_not_hidden_by_installation(self):
        self.module_path.side_effect = OSError("module path unavailable")
        with self.assertLogs(level="ERROR"):
            self.assertFalse(self.manager.initialize())
        self.native.LoadNTDriver.assert_not_called()
        self.assertIsNone(self.manager.driver)

    def test_missing_deployment_member_blocks_native_loading(self):
        for name in ("cqkeysdll.dll", "cqkeys.inf", "cqkeys.sys", "cqkeys.cat"):
            with self.subTest(name=name):
                member = self.directory / name
                member.unlink()
                try:
                    with self.assertLogs(level="ERROR") as messages:
                        self.assertFalse(self.manager.initialize())
                    self.assertIn(str(member), "\n".join(messages.output))
                    self.load_dll.assert_not_called()
                    self.native.LoadNTDriver.assert_not_called()
                    self.assertIsNone(self.manager.driver)
                    self.assertIsNone(self.manager.tasks)
                finally:
                    member.touch()

    def test_setup_reports_every_missing_package_member(self):
        missing = [self.directory / name for name in ("cqkeys.sys", "cqkeys.cat")]
        for member in missing:
            member.unlink()
        with self.assertRaises(FileNotFoundError) as error:
            self.manager._setup_paths(dll_path=self.manager.dll_path, inf_path=self.manager.inf_path)
        for member in missing:
            self.assertIn(str(member), str(error.exception))
        self.load_dll.assert_not_called()

    def test_deployment_directory_is_not_accepted_as_a_file(self):
        for name in ("cqkeysdll.dll", "cqkeys.inf", "cqkeys.sys", "cqkeys.cat"):
            with self.subTest(name=name):
                member = self.directory / name
                member.unlink()
                member.mkdir()
                try:
                    with self.assertRaises(FileNotFoundError):
                        self.manager._setup_paths(
                            dll_path=self.manager.dll_path, inf_path=self.manager.inf_path)
                finally:
                    member.rmdir()
                    member.touch()
        self.load_dll.assert_not_called()

    def test_setup_requires_the_cqkeys_inf_not_a_sys_or_unrelated_inf(self):
        for name in ("cqkeys.sys", "other.inf"):
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, "cqkeys.inf"):
                self.manager._setup_paths(dll_path=self.manager.dll_path,
                                          inf_path=str(self.directory / name))
        self.load_dll.assert_not_called()

    def test_explicit_empty_paths_do_not_fall_back_to_saved_config(self):
        with patch("driver_manager.open") as source:
            for dll_path, inf_path in (("", self.manager.inf_path), (self.manager.dll_path, "")):
                with self.subTest(dll_path=dll_path, inf_path=inf_path), self.assertRaises(ValueError):
                    self.manager._setup_paths(dll_path=dll_path, inf_path=inf_path)
            source.assert_not_called()
        self.load_dll.assert_not_called()

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
        self.native.UnloadNTDriverAndPackage.assert_not_called()

    def test_package_cleanup_failure_retains_request_and_owner_for_retry(self):
        self.manager.driver = self.native
        self.native.UnloadNTDriverAndPackage.return_value = 0
        ctypes.set_last_error(5)
        with self.assertRaises(OSError):
            self.manager.cleanup(remove_package=True)
        self.assertIs(self.manager.driver, self.native)
        self.native.UnloadNTDriver.assert_not_called()
        self.native.UnloadNTDriverAndPackage.return_value = 1
        self.manager.cleanup()
        self.assertEqual(self.native.UnloadNTDriverAndPackage.call_count, 2)
        self.assertIsNone(self.manager.driver)
        self.assertFalse(self.manager._remove_package)

    def test_old_dll_cannot_silently_fall_back_from_package_cleanup(self):
        self.manager.driver = self.native
        self.manager.loaded_dll_path = str(self.directory / "old" / "cqkeysdll.dll")
        del self.native.UnloadNTDriverAndPackage
        with self.assertRaisesRegex(RuntimeError, "更新 cqkeysdll.dll") as error:
            self.manager.cleanup(remove_package=True)
        self.assertIn(self.manager.loaded_dll_path, str(error.exception))
        self.native.UnloadNTDriver.assert_not_called()
        self.assertIs(self.manager.driver, self.native)
        self.assertFalse(self.manager._remove_package)
        self.manager.cleanup()
        self.native.UnloadNTDriver.assert_called_once()

    def test_disable_only_cannot_delete_package(self):
        self.manager.driver = self.native
        with self.assertRaises(ValueError):
            self.manager.cleanup(uninstall=False, remove_package=True)
        self.native.StopNTDriver.assert_not_called()
        self.native.UnloadNTDriverAndPackage.assert_not_called()

    def test_legacy_configuration_resolves_inf_and_still_checks_catalog(self):
        config = {"dll_path": self.manager.dll_path, "sys_path": str(self.directory / "cqkeys.sys")}
        (self.directory / "cqkeys.cat").unlink()
        with patch("driver_manager.open", mock_open(read_data=json.dumps(config))), \
                patch("driver_manager.os.path.exists", return_value=True), \
                self.assertLogs(level="WARNING") as messages, \
                self.assertRaisesRegex(FileNotFoundError, "cqkeys.cat"):
            self.manager._setup_paths()
        self.assertIn("sys_path", "\n".join(messages.output))
        self.assertEqual(self.manager.inf_path, str(self.directory / "cqkeys.inf"))
        self.load_dll.assert_not_called()

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
