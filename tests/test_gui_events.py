"""Exercise bounded GUI telemetry without Tk windows or native input."""

import logging
import threading
import unittest
from unittest.mock import Mock, patch

from test_gui_lifecycle import CQKeysGUI, gui_module
from input_tasks import InputCancellation


class LogText:
    def __init__(self):
        self.content = ""
        self.insert_threads = []
        self.configure = Mock()
        self.grid = Mock()
        self.yview = Mock()

    def insert(self, index, text):
        self.insert_threads.append(threading.get_ident())
        self.content += text

    def count(self, start, end, unit):
        return (len(self.content),)

    def index(self, index):
        return f"{self.content.count(chr(10)) + 1}.{len(self.content.rsplit(chr(10), 1)[-1])}"

    def delete(self, start, end):
        if end.endswith("c"):
            self.content = self.content[int(end.removeprefix("1.0+")[:-1]):]
        else:
            line_count = int(end.split(".")[0]) - 1
            self.content = "".join(self.content.splitlines(keepends=True)[line_count:])

    def yview_moveto(self, fraction):
        pass

class GuiEventTests(unittest.TestCase):
    def setUp(self):
        self.gui = CQKeysGUI.__new__(CQKeysGUI)
        self.gui.root = Mock()
        self.gui._init_ui_events()
        self.gui.is_closing = self.gui.is_unloading = False
        self.gui.is_driver_loaded = True
        self.gui.right_panel = Mock()
        self.gui.default_font = ("font", 10)
        self.text = LogText()
        logger = logging.getLogger()
        previous_level = logger.level
        with patch.object(gui_module, "ttk"), patch.object(gui_module.tk, "Text", return_value=self.text):
            self.gui._setup_logging()
        self.addCleanup(logger.removeHandler, self.gui._log_handler)
        self.addCleanup(logger.setLevel, previous_level)
        for name in ("press_count_label", "run_time_label", "click_rate_label",
                     "rapid_test_btn", "rapid_test_status"):
            setattr(self.gui, name, Mock())

    def emit(self, text, level=logging.INFO):
        self.gui._log_handler.emit(logging.LogRecord("test", level, __file__, 1, text, (), None))

    def drain(self, ticks=30):
        for _ in range(ticks):
            self.gui._drain_ui_events()

    def start_hook(self):
        self.gui.keyboard_hook = None
        self.gui._hook_generation = 0
        self.gui._hook_enabled = False
        self.gui._auto_move_cancellation = None
        self.gui._auto_move_pending = False
        self.gui.auto_move_running = False
        self.gui.start_hook_btn = Mock()
        self.gui.auto_move_status = Mock()
        self.gui.hotkey_var = Mock()
        self.gui.hotkey_var.get.return_value = "F8"
        with patch.object(gui_module, "KeyboardHook") as constructor:
            self.gui._toggle_keyboard_hook()
        self.gui._toggle_auto_move = Mock()
        return constructor.return_value.register_hotkey.call_args.args[1]

    def test_hotkey_flood_uses_one_slot_and_preserves_error_and_terminal_callbacks(self):
        hotkey = self.start_hook()
        errors = []

        def produce():
            try:
                for _ in range(2500):
                    hotkey()
            except Exception as error:
                errors.append(error)

        producers = [threading.Thread(target=produce, daemon=True) for _ in range(4)]
        for producer in producers:
            producer.start()
        for producer in producers:
            producer.join(2)
            self.assertFalse(producer.is_alive())
        self.assertEqual(errors, [])
        error_callback, terminal_callback = Mock(), Mock()
        self.gui._post_ui(error_callback, "native failure")
        self.gui._post_ui(terminal_callback)
        self.assertEqual(self.gui._ui_events.qsize(), 2)
        self.gui._toggle_auto_move.assert_not_called()
        self.gui.root.after.assert_not_called()
        self.gui._drain_ui_events()
        self.gui._toggle_auto_move.assert_called_once()
        error_callback.assert_called_once_with("native failure")
        terminal_callback.assert_called_once()
        self.assertIn("热键过载", self.text.content)
        self.assertIn("合并 9999", self.text.content)
        self.drain()
        self.gui._toggle_auto_move.assert_called_once()
        self.assertEqual(self.text.content.count("热键过载"), 1)
        self.gui.root.report_callback_exception.assert_not_called()

    def test_stopping_hook_discards_flooded_slot_and_late_callbacks(self):
        hotkey = self.start_hook()
        for _ in range(10000):
            hotkey()
        self.gui._toggle_keyboard_hook()
        for _ in range(10000):
            hotkey()
        self.assertTrue(self.gui._ui_events.empty())
        self.gui._drain_ui_events()
        self.gui._toggle_auto_move.assert_not_called()
        self.assertIn("合并 9999", self.text.content)

    def test_repeated_hook_generations_do_not_accumulate_pending_notifications(self):
        old_hotkey = self.start_hook()
        for _ in range(100):
            old_hotkey()
            self.gui._toggle_keyboard_hook()
            with patch.object(gui_module, "KeyboardHook") as constructor:
                self.gui._toggle_keyboard_hook()
            new_hotkey = constructor.return_value.register_hotkey.call_args.args[1]
            old_hotkey()
            new_hotkey()
            old_hotkey = new_hotkey
        self.assertTrue(self.gui._ui_events.empty())
        self.gui._drain_ui_events()
        self.gui._toggle_auto_move.assert_called_once()

    def test_failed_hook_stop_invalidates_flooded_slot(self):
        hotkey = self.start_hook()
        hook = self.gui.keyboard_hook
        hook.stop.side_effect = TimeoutError("hook still running")
        for _ in range(10000):
            hotkey()
        with patch.object(gui_module.messagebox, "showerror") as dialog:
            self.gui._toggle_keyboard_hook()
        dialog.assert_called_once()
        hotkey()
        self.assertTrue(self.gui._ui_events.empty())
        self.gui._drain_ui_events()
        self.gui._toggle_auto_move.assert_not_called()
        self.assertIs(self.gui.keyboard_hook, hook)
        self.assertIn("合并 9999", self.text.content)

    def test_closed_gui_never_stages_hotkeys(self):
        hotkey = self.start_hook()
        self.gui._ui_closed = True
        for _ in range(10000):
            hotkey()
        self.assertTrue(self.gui._ui_events.empty())
        self.assertIsNone(self.gui._pending_hotkey)

    def test_hotkeys_posted_during_dispatch_wait_for_next_tick(self):
        hotkey = self.start_hook()

        def dispatch():
            for _ in range(1000):
                hotkey()

        self.gui._toggle_auto_move.side_effect = dispatch
        hotkey()
        terminal = Mock()
        self.gui._post_ui(terminal)
        self.gui._drain_ui_events()
        self.gui._toggle_auto_move.assert_called_once()
        terminal.assert_called_once()
        self.assertTrue(self.gui._ui_events.empty())
        self.gui._toggle_auto_move.side_effect = None
        self.gui._drain_ui_events()
        self.assertEqual(self.gui._toggle_auto_move.call_count, 2)
        self.assertIn("合并 999", self.text.content)

    def test_hotkey_dispatch_failure_does_not_drop_control_callbacks(self):
        hotkey = self.start_hook()
        failure = RuntimeError("hotkey dispatch failed")
        self.gui._toggle_auto_move.side_effect = failure
        hotkey()
        terminal = Mock()
        self.gui._post_ui(terminal)
        self.gui._drain_ui_events()
        terminal.assert_called_once()
        self.assertIs(self.gui.root.report_callback_exception.call_args.args[1], failure)
        self.gui._toggle_auto_move.side_effect = None
        hotkey()
        self.gui._drain_ui_events()
        self.assertEqual(self.gui._toggle_auto_move.call_count, 2)

    def test_log_flood_never_fills_control_queue_or_delays_terminal_callbacks(self):
        terminal = Mock()
        errors = []

        def produce():
            try:
                for index in range(10000):
                    self.emit(f"line {index}")
                self.emit("important failure", logging.ERROR)
                self.gui._post_ui(terminal)
            except Exception as error:
                errors.append(error)

        worker = threading.Thread(target=produce, daemon=True)
        worker.start()
        worker.join(2)
        self.assertFalse(worker.is_alive(), "logging must not wait for the Tk drain")
        self.assertEqual(errors, [])
        self.assertEqual(self.gui._ui_events.qsize(), 1)
        handler = self.gui._log_handler
        self.assertEqual(len(handler._pending), handler.QUEUE_LIMIT)
        self.assertEqual(handler.dropped_logs, 10000 - handler.QUEUE_LIMIT)
        self.assertEqual(self.text.insert_threads, [])
        self.gui._drain_ui_events()
        terminal.assert_called_once()
        self.assertIn("important failure", self.text.content)
        self.assertIn("溢出", self.text.content)
        self.assertIn(str(handler.dropped_logs), self.text.content)
        self.assertEqual(set(self.text.insert_threads), {threading.get_ident()})

    def test_error_overflow_is_explicit_and_retains_recent_error_details(self):
        for index in range(10000):
            self.emit(f"failure {index}", logging.ERROR)
        handler = self.gui._log_handler
        self.assertEqual(len(handler._errors), handler.ERROR_LIMIT)
        self.assertEqual(handler.dropped_errors, 10000 - handler.ERROR_LIMIT)
        self.gui._drain_ui_events()
        self.assertIn("溢出", self.text.content)
        self.assertIn("错误", self.text.content)
        self.assertIn(str(handler.dropped_errors), self.text.content)
        self.drain()
        self.assertIn("failure 9999", self.text.content)
        self.gui.root.report_callback_exception.assert_not_called()

    def test_normal_flood_cannot_evict_error_details(self):
        self.emit("preserve this error", logging.ERROR)
        for index in range(10000):
            self.emit(f"normal {index}")
        self.gui._drain_ui_events()
        self.assertIn("preserve this error", self.text.content)
        self.assertEqual(self.gui._log_handler.dropped_errors, 0)

    def test_each_tick_has_a_bounded_log_batch_and_reschedules(self):
        for index in range(1000):
            self.emit(f"normal {index}")
        handler = self.gui._log_handler
        self.gui._drain_ui_events()
        self.assertEqual(len(handler._pending), handler.QUEUE_LIMIT - handler.DRAIN_LIMIT)
        self.gui.root.after.assert_called_once_with(25, self.gui._drain_ui_events)

    def test_long_error_keeps_head_tail_and_explicit_truncation_count(self):
        self.emit("error header " + "x" * 100000 + " error traceback tail", logging.ERROR)
        handler = self.gui._log_handler
        self.assertLessEqual(len(handler._errors[0]), handler.MESSAGE_CHARS)
        self.gui._drain_ui_events()
        for text in ("error header", "error traceback tail", "截断 1 条"):
            self.assertIn(text, self.text.content)

    def test_log_display_is_bounded_by_lines_and_characters(self):
        for _ in range(50):
            for index in range(40):
                self.emit(f"{index}\n")
            self.gui._drain_ui_events()
        self.assertLessEqual(self.text.content.count("\n"), 1000)
        for _ in range(50):
            self.emit("x" * 10000)
            self.gui._drain_ui_events()
        self.assertLessEqual(len(self.text.content), 64000)
        self.assertIn("截断", self.text.content)
        self.gui.root.report_callback_exception.assert_not_called()

    def test_logs_survive_ui_generation_change_and_closing_attempt(self):
        self.emit("failure before unload", logging.ERROR)
        self.gui._ui_generation += 1
        self.gui.is_closing = True
        self.emit("failure during unload", logging.ERROR)
        self.drain()
        self.assertIn("failure before unload", self.text.content)
        self.assertIn("failure during unload", self.text.content)

    def test_rapid_progress_keeps_only_latest_snapshot_and_finishes_after_it(self):
        self.gui.rapid_test_running = self.gui._rapid_test_pending = True
        self.gui.input_tester = Mock()

        def repeat_key(*args, progress, **kwargs):
            result = Mock()
            for count in range(10000):
                result.completedRepeats = count
                progress(result)
            result.completedRepeats = -1

        self.gui.input_tester.repeat_key.side_effect = repeat_key
        self.gui._run_rapid_test("a", .001, .001, 1, InputCancellation())
        self.assertLessEqual(self.gui._ui_events.qsize(), 1)
        self.gui._drain_ui_events()
        self.gui.press_count_label.config.assert_called_once_with(text="任务完成重复数: 9999")
        self.gui.rapid_test_status.config.assert_called_with(text="状态: 任务完成")
        self.assertFalse(self.gui._rapid_test_pending)
        self.gui.root.report_callback_exception.assert_not_called()

    def test_stale_progress_is_discarded_after_unload(self):
        self.gui._post_rapid_progress(10, 2)
        self.gui._ui_generation += 1
        self.gui._drain_ui_events()
        self.gui.press_count_label.config.assert_not_called()
        self.assertIsNone(self.gui._rapid_progress)

    def test_terminal_callback_flushes_progress_posted_during_current_tick(self):
        self.gui._post_ui(self.gui._post_rapid_progress, 20, 2)
        self.gui._post_ui(self.gui._stop_test, "状态: 已取消")
        self.gui._drain_ui_events()
        self.gui.press_count_label.config.assert_called_once_with(text="任务完成重复数: 20")
        self.gui.rapid_test_status.config.assert_called_with(text="状态: 已取消")
        self.assertIsNone(self.gui._rapid_progress)

    def test_closed_gui_rejects_logs_progress_and_callbacks(self):
        self.gui._ui_closed = True
        callback = Mock()
        self.emit("after close", logging.ERROR)
        self.gui._post_rapid_progress(20, 2)
        self.gui._post_ui(callback)
        self.gui._drain_ui_events()
        self.assertEqual(len(self.gui._log_handler._errors), 0)
        self.assertIsNone(self.gui._rapid_progress)
        self.assertTrue(self.gui._ui_events.empty())
        self.gui.root.after.assert_not_called()
        self.assertEqual(self.text.content, "")


if __name__ == "__main__":
    unittest.main()
