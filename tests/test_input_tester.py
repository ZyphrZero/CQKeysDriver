import ctypes
from pathlib import Path
import sys
import unittest
from unittest.mock import Mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "cqkeyspy_test"))
from input_tester import InputTester
from input_workers import WorkCancelled


class InputTesterTests(unittest.TestCase):
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
