from pathlib import Path
import sys
import threading
import unittest
from unittest.mock import Mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "cqkeyspy_test"))
from keyboard_hook import KeyboardHook


class HookCleanupTests(unittest.TestCase):
    def make_hook(self):
        hook = KeyboardHook.__new__(KeyboardHook)
        hook.hook_thread = Mock(native_id=42, ident=999)
        hook._stop_requested = threading.Event()
        hook._ready = threading.Event()
        hook._ready.set()
        hook._hook_error = None
        hook._hook_callback_ptr = object()
        hook.hook_id = None
        hook.pressed_keys = {65}
        hook.user32 = Mock()
        return hook

    def test_timeout_preserves_thread_and_callback_for_retry(self):
        hook = self.make_hook()
        thread, callback = hook.hook_thread, hook._hook_callback_ptr
        thread.is_alive.return_value = True
        with self.assertRaises(TimeoutError):
            hook.stop()
        self.assertIs(hook.hook_thread, thread)
        self.assertIs(hook._hook_callback_ptr, callback)
        self.assertEqual(hook.user32.PostThreadMessageW.call_args.args[0], 42)
        thread.is_alive.return_value = False
        hook.stop()
        self.assertIsNone(hook.hook_thread)
        self.assertIsNone(hook._hook_callback_ptr)

    def test_startup_thread_is_not_ignored(self):
        hook = self.make_hook()
        hook._ready.clear()
        hook.hook_thread.is_alive.return_value = True
        with self.assertRaises(TimeoutError):
            hook.stop()
        self.assertTrue(hook._stop_requested.is_set())
        hook.hook_thread.join.assert_called_once_with(timeout=2)

    def test_native_cleanup_error_is_not_hidden(self):
        hook = self.make_hook()
        hook.hook_thread.is_alive.return_value = False
        hook._hook_error = OSError("unhook failed")
        hook.hook_id = 123
        with self.assertRaises(OSError):
            hook.stop()
        self.assertIsNotNone(hook._hook_callback_ptr)


if __name__ == "__main__":
    unittest.main()
