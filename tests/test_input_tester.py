import ctypes
from pathlib import Path
import sys
import unittest
from unittest.mock import Mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "cqkeyspy_test"))
from input_tester import InputTester
from input_workers import WorkCancelled
from input_tasks import InputTaskError, FAILED
from virtuakeys_mapping import VirtualKeys
from test_input_tasks import native_tasks


class InputTesterTests(unittest.TestCase):
    def test_key_validation_matches_resolved_function_keys_and_aliases(self):
        for key in ("F1", "f24", "SPACE", "return", "DELETE", "A", "+", "invalid", "F25", ""):
            with self.subTest(key=key):
                self.assertEqual(VirtualKeys.is_valid_key(key), VirtualKeys.get_vk_code(key) is not None)

    def test_uppercase_and_symbols_are_one_native_task_without_python_waits(self):
        native = native_tasks()
        wait = Mock(side_effect=AssertionError("Python must not time input"))
        tester = InputTester(native, wait=wait)
        tester.type_text("A!=", interval=0.02, duration=0.05)
        steps = native.submissions[0][0]
        self.assertEqual(steps, [(1, 16, 0, 0), (1, 65, 0, 50), (2, 65, 0, 0),
                                 (2, 16, 0, 20), (1, 16, 0, 0), (1, 49, 0, 50),
                                 (2, 49, 0, 0), (2, 16, 0, 20),
                                 (1, 187, 0, 50), (2, 187, 0, 20)])
        native.KeyDown.assert_not_called()
        native.KeyUp.assert_not_called()
        wait.assert_not_called()

    def test_invalid_text_rejects_whole_task_before_any_input(self):
        native = native_tasks()
        with self.assertRaises(ValueError):
            InputTester(native).type_text("ok中文")
        native.SubmitInputTask.assert_not_called()

    def test_repeat_timing_and_count_go_to_dll(self):
        native = native_tasks()
        InputTester(native).repeat_key("a", 0.001, 0.002, 3)
        self.assertEqual(native.submissions, [([(1, 65, 0, 1), (2, 65, 0, 2)], 1000, 0)])

    def test_invalid_repeat_duration_and_overflow_are_rejected(self):
        for duration, hold, interval in ((0, .001, .001), (float("nan"), .001, .001),
                                         (1, 0, 0), (1000, .001, 0), (1, -1, .1)):
            with self.subTest(duration=duration, hold=hold, interval=interval):
                with self.assertRaises(ValueError):
                    InputTester.repeat_count(duration, hold, interval)

    def test_all_mouse_clicks_are_native_button_snapshot_tasks(self):
        native = native_tasks()
        tester = InputTester(native)
        for method, mask in ((tester.mouse_click, 1), (tester.mouse_right_click, 2),
                             (tester.mouse_middle_click, 4), (tester.mouse_x1_click, 8),
                             (tester.mouse_x2_click, 16)):
            self.assertTrue(method(.025))
            self.assertEqual(native.submissions[-1][0], [(7, mask, 0, 25), (7, 0, 0, 0)])

    def test_press_failure_is_propagated_not_false_success(self):
        native = native_tasks()
        native.task_result.state = FAILED
        native.task_result.error = 29
        tester = InputTester(native)
        with self.assertRaises(InputTaskError):
            tester.press_key("!")
        self.assertIsNotNone(tester.tasks.active)

    def test_old_tester_never_sends_after_stop(self):
        native = Mock()
        tester = InputTester(native)
        tester.stop()
        with self.assertRaises(WorkCancelled):
            tester.mouse_move_rel(1, 2)
        native.MouseMoveRELATIVE.assert_not_called()

    def test_native_failure_is_not_reported_as_success(self):
        native = Mock()
        native.MouseMoveRELATIVE.side_effect = lambda *args: ctypes.set_last_error(29)
        tester = InputTester(native)
        with self.assertRaises(OSError):
            tester.mouse_move_rel(1, 2)

    def test_cached_status_does_not_turn_failure_into_success(self):
        native = Mock()
        native.GetDriverStatus.return_value = 2
        tester = InputTester(native)
        self.assertFalse(tester._check_device_status())
        self.assertFalse(tester._check_device_status())


if __name__ == "__main__":
    unittest.main()
