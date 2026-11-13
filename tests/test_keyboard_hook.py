from pathlib import Path
import ctypes
import sys
import threading
from types import ModuleType, SimpleNamespace
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "cqkeyspy_test"))
with patch.dict(sys.modules, {
    "win32con": SimpleNamespace(WM_QUIT=0x0012, WM_KEYDOWN=0x0100, WM_KEYUP=0x0101,
                                WM_SYSKEYDOWN=0x0104, WM_SYSKEYUP=0x0105),
    "win32api": ModuleType("win32api"),
    "win32gui": ModuleType("win32gui"),
}):
    import keyboard_hook as hook_module
    from keyboard_hook import KeyboardHook, KBDLLHOOKSTRUCT, win32con


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

    def test_thread_start_failure_does_not_leave_an_unjoinable_thread(self):
        hook = self.make_hook()
        hook.hook_thread = None
        thread = Mock()
        thread.ident = None
        thread.start.side_effect = RuntimeError("thread unavailable")
        with patch.object(hook_module.threading, "Thread", return_value=thread):
            with self.assertRaisesRegex(RuntimeError, "thread unavailable"):
                hook.start()
        self.assertIsNone(hook.hook_thread)
        hook.stop()

    def test_native_cleanup_error_is_not_hidden(self):
        hook = self.make_hook()
        hook.hook_thread.is_alive.return_value = False
        hook._hook_error = OSError("unhook failed")
        hook.hook_id = 123
        with self.assertRaises(OSError):
            hook.stop()
        self.assertIsNotNone(hook._hook_callback_ptr)

    def test_failed_unhook_can_be_retried_without_losing_first_error(self):
        hook = self.make_hook()
        hook.hook_thread.is_alive.return_value = False
        hook._hook_error = OSError("initial unhook failed")
        hook.hook_id = 123

        with self.assertRaisesRegex(OSError, "initial unhook failed"):
            hook.stop()
        self.assertIsNotNone(hook._hook_callback_ptr)
        hook.user32.UnhookWindowsHookEx.assert_not_called()

        def fail_unhook(handle):
            ctypes.set_last_error(5)
            return False

        hook.user32.UnhookWindowsHookEx.side_effect = fail_unhook
        with self.assertRaises(OSError) as retry_error:
            hook.stop()
        self.assertEqual(retry_error.exception.winerror, 5)
        self.assertIsNotNone(hook._hook_callback_ptr)
        self.assertEqual(hook.hook_id, 123)

        hook.user32.UnhookWindowsHookEx.side_effect = None
        hook.user32.UnhookWindowsHookEx.return_value = True
        hook.stop()
        self.assertEqual(hook.user32.UnhookWindowsHookEx.call_count, 2)
        self.assertIsNone(hook.hook_id)
        self.assertIsNone(hook.hook_thread)
        self.assertIsNone(hook._hook_callback_ptr)

    def test_system_key_messages_trigger_and_release_hotkey(self):
        hook = self.make_hook()
        hook.pressed_keys.clear()
        callback = Mock()
        hook.hotkey_callbacks = {0x77: callback}
        packet = KBDLLHOOKSTRUCT(vkCode=0x77)
        hook._hook_callback(0, win32con.WM_SYSKEYDOWN, ctypes.addressof(packet))
        callback.assert_called_once_with()
        self.assertIn(0x77, hook.pressed_keys)
        hook._hook_callback(0, win32con.WM_SYSKEYUP, ctypes.addressof(packet))
        self.assertNotIn(0x77, hook.pressed_keys)
        self.assertEqual(hook.user32.CallNextHookEx.call_count, 2)

    def test_key_repeat_and_unrelated_keys_do_not_retrigger_hotkey(self):
        hook = self.make_hook()
        hook.pressed_keys.clear()
        single_callback, combo_callback = Mock(), Mock()
        hook.hotkey_callbacks = {0x77: single_callback, (0xA2, 0x77): combo_callback}

        def send(message, key):
            packet = KBDLLHOOKSTRUCT(vkCode=key)
            hook._hook_callback(0, message, ctypes.addressof(packet))

        send(win32con.WM_KEYDOWN, 0xA2)
        send(win32con.WM_KEYDOWN, 0x77)
        send(win32con.WM_KEYDOWN, 0x77)
        send(win32con.WM_KEYDOWN, 0x41)
        single_callback.assert_called_once_with()
        combo_callback.assert_called_once_with()
        send(win32con.WM_KEYUP, 0x77)
        send(win32con.WM_KEYDOWN, 0x77)
        self.assertEqual(single_callback.call_count, 2)
        self.assertEqual(combo_callback.call_count, 2)


if __name__ == "__main__":
    unittest.main()
