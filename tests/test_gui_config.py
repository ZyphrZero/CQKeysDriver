"""Exercise configuration persistence with temporary files, never a Tk window."""

import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import Mock, patch

from test_gui_lifecycle import CQKeysGUI, gui_module


class GuiConfigTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.config_path = Path(directory.name) / "driver_config.json"
        self.gui = CQKeysGUI.__new__(CQKeysGUI)
        self.gui.config_file = str(self.config_path)
        self.gui.auto_move_config = {"hotkey": "F8", "speed": 10, "range": 100}
        for name, value in (("dll_path_var", "new.dll"), ("sys_path_var", "new.inf"),
                            ("test_key_var", "a"), ("press_time_var", "2"),
                            ("interval_time_var", "3"), ("duration_var", "4"),
                            ("hotkey_var", "F9"), ("speed_var", "20"), ("range_var", "200")):
            variable = Mock()
            variable.get.return_value = value
            setattr(self.gui, name, variable)
        errors = patch.object(gui_module.messagebox, "showerror")
        success = patch.object(gui_module.messagebox, "showinfo")
        self.errors = errors.start()
        self.success = success.start()
        self.addCleanup(errors.stop)
        self.addCleanup(success.stop)

    def save_config(self, config):
        self.config_path.write_text(json.dumps(config), encoding="utf-8")

    def read_config(self):
        return json.loads(self.config_path.read_text(encoding="utf-8"))

    def test_load_uses_utf8_for_non_ascii_paths_and_settings(self):
        saved = {
            "dll_path": "C:\\输入\\工具.dll",
            "sys_path": "C:\\输入\\设备.inf",
            "auto_move": {"hotkey": "自定义", "speed": 20, "range": 200},
        }
        self.config_path.write_text(json.dumps(saved, ensure_ascii=False), encoding="utf-8")
        with patch("builtins.open", wraps=open) as opened:
            self.gui.load_path_config()
            self.gui.load_auto_move_config()
        self.gui.dll_path_var.set.assert_called_once_with(saved["dll_path"])
        self.gui.sys_path_var.set.assert_called_once_with(saved["sys_path"])
        self.assertEqual(self.gui.auto_move_config["hotkey"], "自定义")
        self.assertEqual(opened.call_count, 2)
        for call in opened.call_args_list:
            self.assertEqual(call.kwargs.get("encoding"), "utf-8")

    def test_path_save_preserves_auto_move_and_unknown_settings(self):
        saved = {
            "dll_path": "old.dll", "sys_path": "old.inf",
            "auto_move": {"hotkey": "F7", "speed": 5, "range": 90, "custom": "keep"},
            "rapid_test": {"test_key": "b", "custom": {"keep": True}},
            "other_tool": {"value": [1, 2, 3]},
        }
        self.save_config(saved)
        self.gui.save_path_config()
        updated = self.read_config()
        self.assertEqual(updated["auto_move"], saved["auto_move"])
        self.assertEqual(updated["other_tool"], saved["other_tool"])
        self.assertEqual(updated["rapid_test"], {
            "test_key": "a", "press_time": "2", "interval_time": "3", "duration": "4",
            "custom": {"keep": True},
        })
        self.assertEqual((updated["dll_path"], updated["sys_path"]), ("new.dll", "new.inf"))
        self.errors.assert_not_called()

    def test_auto_move_save_only_updates_its_keys_from_current_file(self):
        saved = {
            "dll_path": "saved.dll", "sys_path": "saved.inf",
            "rapid_test": {"test_key": "z", "custom": "keep"},
            "auto_move": {"hotkey": "F7", "speed": 5, "range": 90, "custom": "keep"},
            "other_tool": {"value": True},
        }
        self.save_config(saved)
        self.gui.save_auto_move_config()
        expected = dict(saved)
        expected["auto_move"] = {"hotkey": "F9", "speed": 20.0, "range": 200.0, "custom": "keep"}
        self.assertEqual(self.read_config(), expected)
        self.assertEqual(self.gui.auto_move_config, expected["auto_move"])
        self.errors.assert_not_called()

    def test_invalid_json_or_wrong_root_is_not_overwritten(self):
        for method in (self.gui.save_path_config, self.gui.save_auto_move_config):
            for original in (b'{"auto_move":', b'', b'[]', b'null'):
                with self.subTest(method=method.__name__, original=original):
                    self.config_path.write_bytes(original)
                    cached = self.gui.auto_move_config.copy()
                    self.errors.reset_mock()
                    self.success.reset_mock()
                    method()
                    self.assertEqual(self.config_path.read_bytes(), original)
                    self.assertEqual(self.gui.auto_move_config, cached)
                    self.errors.assert_called_once()
                    self.success.assert_not_called()

    def test_invalid_target_section_is_not_overwritten(self):
        for method, section in ((self.gui.save_path_config, "rapid_test"),
                                (self.gui.save_auto_move_config, "auto_move")):
            with self.subTest(section=section):
                self.save_config({section: ["preserve"], "other_tool": True})
                original = self.config_path.read_bytes()
                self.errors.reset_mock()
                method()
                self.assertEqual(self.config_path.read_bytes(), original)
                self.errors.assert_called_once()

    def test_missing_file_can_be_created_by_either_save_action(self):
        for method, expected_key in ((self.gui.save_path_config, "rapid_test"),
                                     (self.gui.save_auto_move_config, "auto_move")):
            with self.subTest(method=method.__name__):
                if self.config_path.exists():
                    self.config_path.unlink()
                method()
                self.assertIn(expected_key, self.read_config())
        self.errors.assert_not_called()

    def test_failed_replace_preserves_file_and_in_memory_auto_move_settings(self):
        self.save_config({"auto_move": {"hotkey": "F7"}, "other_tool": True})
        original = self.config_path.read_bytes()
        cached = self.gui.auto_move_config.copy()
        with patch.object(gui_module.os, "replace", side_effect=PermissionError("file busy")):
            self.gui.save_auto_move_config()
        self.assertEqual(self.config_path.read_bytes(), original)
        self.assertEqual(self.gui.auto_move_config, cached)
        self.errors.assert_called_once()
        self.success.assert_not_called()
        self.assertEqual(list(self.config_path.parent.iterdir()), [self.config_path])

    def test_partial_write_failure_does_not_truncate_original_config(self):
        self.save_config({"auto_move": {"hotkey": "F7"}, "other_tool": True})
        original = self.config_path.read_bytes()

        def fail_write(config, target, **options):
            target.write('{"dll_path":')
            raise OSError("disk write failed")

        with patch.object(gui_module.json, "dump", side_effect=fail_write):
            self.gui.save_path_config()
        self.assertEqual(self.config_path.read_bytes(), original)
        self.assertIn("disk write failed", self.errors.call_args.args[1])
        self.assertEqual(list(self.config_path.parent.iterdir()), [self.config_path])


if __name__ == "__main__":
    unittest.main()
