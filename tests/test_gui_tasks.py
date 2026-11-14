"""Run existing GUI handlers with Tk/Win32/native boundaries replaced, never a GUI."""

import queue
import ctypes
import logging
import threading
import unittest
from unittest.mock import Mock, patch

from test_gui_lifecycle import CQKeysGUI, gui_module
from test_input_tasks import native_tasks
from input_tester import InputTester
from input_tasks import InputTasks, InputCancellation, FAILED, CANCELLED, RUNNING, COMPLETED
from input_workers import InputWorkers, WorkCancelled
from driver_manager import DriverManager


class GuiTaskTests(unittest.TestCase):
    def setUp(self):
        self.native = native_tasks()
        self.gui = CQKeysGUI.__new__(CQKeysGUI)
        self.gui.root = Mock()
        self.gui._init_ui_events()
        self.gui._hook_generation = 0
        self.gui._hook_enabled = False
        self.gui.keyboard_hook = None
        self.gui._rapid_cancellation = self.gui._auto_move_cancellation = None
        self.gui._input_release_error = None
        self.gui._management_cleanup_error = None
        self.gui.input_tester = InputTester(self.native)
        self.gui.is_driver_loaded = True
        self.gui.is_unloading = self.gui.is_closing = False
        self.gui._remove_package_requested = False
        self.gui.rapid_test_running = self.gui.auto_move_running = True
        self.gui._rapid_test_pending = self.gui._auto_move_pending = False
        self.gui.input_workers = Mock()
        self.gui._start_worker = lambda target, args=(): target(*args)
        self.gui._update_button_states = Mock()
        for name in ("input_entry", "rapid_test_btn", "rapid_test_status", "auto_move_status",
                     "driver_status", "last_check", "start_hook_btn", "run_time_label",
                     "click_rate_label", "press_count_label"):
            setattr(self.gui, name, Mock())
        self.dialog = patch.object(gui_module.messagebox, "showerror")
        self.dialog.start()
        self.addCleanup(self.dialog.stop)

    def flush_ui(self):
        self.gui._drain_ui_events()

    def run_background(self, target, args=()):
        errors = []

        def run():
            try:
                target(*args)
            except Exception as error:
                errors.append(error)

        worker = threading.Thread(target=run)
        worker.start()
        worker.join(2)
        self.assertFalse(worker.is_alive())
        self.assertEqual(errors, [])
        return worker

    def test_smooth_move_reads_tk_values_before_starting_worker(self):
        main_thread = threading.get_ident()

        def read_distance():
            self.assertEqual(threading.get_ident(), main_thread)
            return "7"

        self.gui.rel_distance_var = Mock()
        self.gui.rel_distance_var.get.side_effect = read_distance
        self.gui._start_worker = self.run_background
        self.gui._test_smooth_move_rel("left")
        self.assertEqual(len(self.native.submissions), 1)
        self.gui.root.after.assert_not_called()

    def test_absolute_move_and_wheel_snapshot_tk_values_on_main_thread(self):
        main_thread = threading.get_ident()

        def read_value():
            self.assertEqual(threading.get_ident(), main_thread)
            return "100"

        for name in ("abs_x_var", "abs_y_var", "wheel_delta_var"):
            variable = Mock()
            variable.get.side_effect = read_value
            setattr(self.gui, name, variable)
        self.gui._start_worker = self.run_background
        self.native.GetDriverStatus.return_value = 1
        with patch.object(gui_module.win32gui, "GetCursorPos", create=True, return_value=(0, 0)):
            self.gui._test_smooth_move_abs()
        self.gui._test_mouse_wheel("up")
        self.native.MouseWheelUp.assert_called_once_with(100)
        self.assertEqual(len(self.native.submissions), 1)
        gui_module.messagebox.showerror.assert_not_called()
        self.gui.root.after.assert_not_called()

    def test_wheel_overflow_is_rejected_before_starting_worker(self):
        self.gui.wheel_delta_var = Mock()
        self.gui.wheel_delta_var.get.return_value = "65536"
        self.gui._start_worker = Mock()
        self.gui._test_mouse_wheel("up")
        self.gui._start_worker.assert_not_called()
        self.native.MouseWheelUp.assert_not_called()
        self.assertIn("65535", gui_module.messagebox.showerror.call_args.args[1])

    def test_ui_callback_error_is_reported_and_does_not_stop_queue(self):
        error = ValueError("bad UI callback")
        successful_callback = Mock()
        self.gui._post_ui(Mock(side_effect=error))
        self.gui._post_ui(successful_callback)
        self.flush_ui()
        self.assertIs(self.gui.root.report_callback_exception.call_args.args[1], error)
        successful_callback.assert_called_once()
        self.gui.root.after.assert_called_once_with(25, self.gui._drain_ui_events)

    def test_worker_logging_is_queued_and_handler_removed_before_destroy(self):
        self.gui.right_panel = Mock()
        self.gui.default_font = ("font", 10)
        logger = logging.getLogger()
        level = logger.level
        with patch.object(gui_module, "ttk"), patch.object(gui_module.tk, "Text"):
            self.gui._setup_logging()
        self.gui.log_text.index.return_value = "1.0"
        self.gui.log_text.count.return_value = (0,)
        handler = self.gui._log_handler
        self.addCleanup(logger.removeHandler, handler)
        self.addCleanup(logger.setLevel, level)
        self.run_background(lambda: logging.info("background log"))
        self.gui.root.after.assert_not_called()
        self.gui.log_text.insert.assert_not_called()
        self.flush_ui()
        self.assertIn("background log", self.gui.log_text.insert.call_args.args[1])
        self.gui._finish_unload(None, close_after=True)
        self.assertNotIn(handler, logger.handlers)
        self.assertTrue(self.gui._ui_closed)
        callback = Mock()
        self.gui._post_ui(callback)
        self.gui.root.after.reset_mock()
        self.flush_ui()
        callback.assert_not_called()
        self.gui.root.after.assert_not_called()

    def test_rapid_tab_does_not_replace_loaded_configuration_variables(self):
        variables = {}
        for name in ("test_key_var", "press_time_var", "interval_time_var", "duration_var"):
            variables[name] = Mock()
            setattr(self.gui, name, variables[name])
        self.gui.notebook = Mock()
        self.gui.default_font = ("font", 10)
        with patch.object(gui_module, "ttk"), patch.object(gui_module.tk, "StringVar") as variable:
            self.gui.create_rapid_test_tab()
        variable.assert_not_called()
        for name, original in variables.items():
            self.assertIs(getattr(self.gui, name), original)

    def test_worker_errors_are_queued_and_survive_except_scope(self):
        self.native.task_result.state = FAILED
        self.native.task_result.error = 29
        with patch.object(gui_module.ctypes.windll.user32, "GetSystemMetrics", return_value=1000):
            self.gui._start_worker = self.run_background
            self.gui._test_mouse_click("left")
        gui_module.messagebox.showerror.assert_not_called()
        self.gui.root.after.assert_not_called()
        self.flush_ui()
        self.assertIn("error=29", gui_module.messagebox.showerror.call_args.args[1])
        self.gui.root.report_callback_exception.assert_not_called()

    def test_corner_failure_before_coordinate_assignment_retains_original_error(self):
        self.gui._start_worker = self.run_background
        with patch.object(gui_module.ctypes.windll.user32, "GetSystemMetrics",
                          side_effect=OSError("metrics unavailable")):
            self.gui._move_to_corner("top_left")
        self.flush_ui()
        self.assertIn("metrics unavailable", gui_module.messagebox.showerror.call_args.args[1])

    def test_rapid_worker_never_calls_tk_even_to_schedule(self):
        self.run_background(self.gui._run_rapid_test, ("a", .001, .001, 1, InputCancellation()))
        self.gui.root.after.assert_not_called()
        self.gui.rapid_test_status.config.assert_not_called()
        self.flush_ui()
        self.gui.rapid_test_status.config.assert_called_with(text="状态: 任务完成")

    def test_invalid_hotkey_does_not_create_an_owned_hook(self):
        self.gui.keyboard_hook = None
        self.gui.hotkey_var = Mock()
        self.gui.hotkey_var.get.return_value = "invalid"
        with patch.object(gui_module, "KeyboardHook") as hook:
            self.gui._toggle_keyboard_hook()
        hook.assert_not_called()
        self.assertIsNone(self.gui.keyboard_hook)

    def test_hook_construction_failure_keeps_start_button_available(self):
        self.gui.keyboard_hook = None
        self.gui.hotkey_var = Mock()
        self.gui.hotkey_var.get.return_value = "F8"
        with patch.object(gui_module, "KeyboardHook", side_effect=OSError("hook unavailable")):
            self.gui._toggle_keyboard_hook()
        self.assertIsNone(self.gui.keyboard_hook)
        self.gui.start_hook_btn.config.assert_called_with(text="启动热键监听")
        gui_module.messagebox.showerror.assert_called_once()

    def start_fake_hook(self, hook=None):
        self.gui.keyboard_hook = None
        self.gui.hotkey_var = Mock()
        self.gui.hotkey_var.get.return_value = "F8"
        hook = hook if hook is not None else Mock()
        with patch.object(gui_module, "KeyboardHook", return_value=hook):
            self.gui._toggle_keyboard_hook()
        return hook, hook.register_hotkey.call_args.args[1]

    def test_stopping_hook_invalidates_queued_and_late_hotkeys_only(self):
        hook, hotkey = self.start_fake_hook()
        self.gui._toggle_auto_move = Mock()
        unrelated = Mock()
        self.gui._post_ui(unrelated)
        hotkey()
        self.gui._toggle_keyboard_hook()
        hotkey()
        self.flush_ui()
        self.gui._toggle_auto_move.assert_not_called()
        unrelated.assert_called_once()
        hook.stop.assert_called_once()
        self.assertIsNone(self.gui.keyboard_hook)

    def test_restarting_hook_does_not_reactivate_previous_callback(self):
        _, old_hotkey = self.start_fake_hook()
        old_hotkey()
        self.gui._toggle_keyboard_hook()
        _, new_hotkey = self.start_fake_hook()
        self.gui._toggle_auto_move = Mock()
        old_hotkey()
        new_hotkey()
        self.flush_ui()
        self.gui._toggle_auto_move.assert_called_once()

    def test_failed_hook_stop_invalidates_callbacks_and_cancels_motion_before_stop(self):
        hook, hotkey = self.start_fake_hook()
        self.gui._toggle_auto_move = Mock()

        def fail_stop():
            self.assertFalse(self.gui.auto_move_running)
            hotkey()
            raise TimeoutError("hook still alive")

        hook.stop.side_effect = fail_stop
        hotkey()
        self.gui._toggle_keyboard_hook()
        hotkey()
        self.flush_ui()
        self.gui._toggle_auto_move.assert_not_called()
        self.assertIs(self.gui.keyboard_hook, hook)
        self.assertIn("hook still alive", gui_module.messagebox.showerror.call_args.args[1])
        hook.stop.side_effect = None
        self.gui._toggle_keyboard_hook()
        self.assertIsNone(self.gui.keyboard_hook)

    def test_failed_hook_start_invalidates_callbacks_but_retains_cleanup_owner(self):
        hook = Mock()

        def fail_start():
            hook.register_hotkey.call_args.args[1]()
            raise TimeoutError("start timed out")

        hook.start.side_effect = fail_start
        _, hotkey = self.start_fake_hook(hook)
        self.gui._toggle_auto_move = Mock()
        hotkey()
        self.flush_ui()
        self.gui._toggle_auto_move.assert_not_called()
        self.assertIs(self.gui.keyboard_hook, hook)
        self.gui._toggle_keyboard_hook()
        hook.stop.assert_called_once()

    def test_rapid_labels_explain_submission_counts_and_theoretical_duration(self):
        self.gui.notebook = Mock()
        self.gui.default_font = ("font", 10)
        for name in ("test_key_var", "press_time_var", "interval_time_var", "duration_var"):
            setattr(self.gui, name, Mock())
        with patch.object(gui_module, "ttk") as widgets:
            self.gui.create_rapid_test_tab()
        labels = "\n".join(call.kwargs.get("text", "") for call in widgets.Label.call_args_list)
        for text in ("理论周期", "折算", "非硬截止", "任务完成重复数", "提交侧", "接收未验证"):
            self.assertIn(text, labels)
        self.gui.press_count_label = Mock()
        self.gui.click_rate_label = Mock()
        self.gui._update_press_count(123)
        self.gui._update_status(2, 61.5)
        self.assertIn("任务完成重复数: 123", self.gui.press_count_label.config.call_args.kwargs["text"])
        self.assertIn("提交侧", self.gui.click_rate_label.config.call_args.kwargs["text"])

    def test_gui_integer_milliseconds_reach_native_task_unchanged(self):
        self.gui.rapid_test_running = False
        for name, value in (("test_key_var", "a"), ("press_time_var", "2007"),
                            ("interval_time_var", "2015"), ("duration_var", "8")):
            variable = Mock()
            variable.get.return_value = value
            setattr(self.gui, name, variable)
        self.gui._toggle_rapid_test()
        self.assertEqual(self.native.submissions[0][:2],
                         ([(1, 65, 0, 2007), (2, 65, 0, 2015)], 2))

    def test_fixed_text_has_shift_inside_task_and_only_preparation_wait(self):
        self.gui._test_input()
        self.assertEqual(len(self.native.submissions), 1)
        self.assertEqual(self.native.submissions[0][0][:4],
                         [(1, 16, 0, 0), (1, 72, 0, 100), (2, 72, 0, 0), (2, 16, 0, 100)])
        self.native.KeyDown.assert_not_called()
        self.native.KeyUp.assert_not_called()
        self.gui.input_workers.wait.assert_called_once_with(3)

    def test_text_failure_never_updates_success_output_or_clears_input(self):
        self.gui.input_entry.get.return_value = "A!"
        self.native.task_result.state = FAILED
        self.native.task_result.error = 29
        self.native.task_result.completedActions = 2
        self.gui._send_input_text()
        self.gui.root.after.assert_not_called()
        self.assertIsNotNone(self.gui.input_tester.tasks.active)
        self.native.KeyDown.assert_not_called()

    def test_uncertain_result_never_displays_completed(self):
        self.native.task_result.resultFlags = 2
        self.gui._run_rapid_test("a", .001, .001, 1, InputCancellation())
        self.flush_ui()
        self.gui.rapid_test_status.config.assert_called_with(text="状态: 出错，请卸载重试清理")
        self.native.ReleaseInputTask.assert_not_called()
        self.assertIsNotNone(self.gui.input_tester.tasks.active)

    def test_smooth_relative_path_retains_exact_distance_and_native_delays(self):
        self.gui.rel_distance_var = Mock()
        self.gui.rel_distance_var.get.return_value = "7"
        self.gui._test_smooth_move_rel("left")
        steps = self.native.submissions[0][0]
        self.assertEqual(len(steps), 10)
        self.assertEqual(sum(step[1] for step in steps), -7)
        self.assertTrue(all(step[0] == 5 and step[2] == 0 and step[3] == 20 for step in steps))
        self.gui.input_workers.wait.assert_called_once_with(3)
        self.native.MouseMoveRELATIVE.assert_not_called()

    def test_smooth_absolute_path_is_one_timed_task(self):
        self.gui.abs_x_var, self.gui.abs_y_var = Mock(), Mock()
        self.gui.abs_x_var.get.return_value = "100"
        self.gui.abs_y_var.get.return_value = "200"
        with patch.object(gui_module.win32gui, "GetCursorPos", create=True, return_value=(10, 20)):
            self.gui._test_smooth_move_abs()
        steps = self.native.submissions[0][0]
        self.assertEqual(steps[0], (6, 10, 20, 10))
        self.assertEqual(steps[-1], (6, 100, 200, 10))
        self.assertEqual(len(steps), 21)
        self.gui.input_workers.wait.assert_not_called()
        self.native.MouseMoveABSOLUTE.assert_not_called()

    def test_double_click_moves_and_clicks_in_one_native_task(self):
        with patch.object(gui_module.ctypes.windll.user32, "GetSystemMetrics", return_value=1000):
            self.gui._test_mouse_click("left")
        self.assertEqual(self.native.submissions[0][0],
                         [(6, 750, 500, 1000), (7, 1, 0, 100), (7, 0, 0, 100),
                          (7, 1, 0, 100), (7, 0, 0, 0)])
        self.gui.input_workers.wait.assert_not_called()

    def test_mouse_advanced_test_is_one_task_not_per_event_waits(self):
        def metrics(index):
            return {0: 1000, 1: 800, 78: 1000, 79: 800, 76: 0, 77: 0, 80: 1}[index]

        with patch.object(gui_module.ctypes.windll.user32, "GetSystemMetrics", side_effect=metrics):
            self.gui._test_mouse()
        self.assertEqual(len(self.native.submissions), 1)
        steps = self.native.submissions[0][0]
        self.assertEqual(steps[0], (5, 100, 0, 500))
        self.assertEqual(steps[-1], (7, 0, 0, 500))
        self.gui.input_workers.wait.assert_called_once_with(3)

    def test_mouse_absolute_points_stay_on_primary_monitor_with_secondary_display(self):
        def metrics(index):
            return {0: 1000, 1: 800, 78: 2920, 79: 1080, 76: -1920, 77: -280, 80: 2}[index]

        with patch.object(gui_module.ctypes.windll.user32, "GetSystemMetrics", side_effect=metrics) as query, \
                patch.object(gui_module.ctypes.windll.user32, "EnumDisplayMonitors") as enumerate_monitors, \
                patch.object(gui_module.ctypes.windll.user32, "GetMonitorInfoW") as monitor_info:
            self.gui._test_mouse()
        self.assertEqual(len(self.native.submissions), 1)
        points = [(step[1], step[2]) for step in self.native.submissions[0][0] if step[0] == 6]
        self.assertEqual(points, [(0, 0), (999, 0), (0, 799), (999, 799), (500, 400)])
        self.assertEqual([entry.args[0] for entry in query.call_args_list], [0, 1])
        enumerate_monitors.assert_not_called()
        monitor_info.assert_not_called()

    def test_invalid_primary_screen_dimensions_do_not_submit_partial_mouse_test(self):
        for width, height in ((0, 800), (1000, 0), (1, 800), (1000, 1)):
            with self.subTest(width=width, height=height), \
                    patch.object(gui_module.ctypes.windll.user32, "GetSystemMetrics",
                                 side_effect=lambda index: {0: width, 1: height}[index]), \
                    self.assertLogs(level="ERROR") as logs:
                self.gui._test_mouse()
            self.assertIn("主显示器尺寸无效", logs.output[0])
        self.native.SubmitInputTask.assert_not_called()

    def test_repeated_task_uses_native_counts_and_no_python_input_loop(self):
        def wait(*arguments):
            if self.native.WaitInputTask.call_count == 1:
                self.native.task_result.state = RUNNING
                self.native.task_result.completedRepeats = 123
                self.native.task_result.completedActions = 492
                ctypes.set_last_error(258)
                return 0
            self.native.task_result.state = COMPLETED
            self.native.task_result.completedRepeats = 500
            self.native.task_result.completedActions = 2000
            return 1

        self.native.WaitInputTask.side_effect = wait
        self.gui._run_rapid_test("A", .001, .001, 1, InputCancellation())
        self.assertEqual(self.native.submissions[0][1], 500)
        self.gui.input_workers.wait.assert_not_called()
        self.native.KeyDown.assert_not_called()
        self.flush_ui()
        self.assertEqual(self.gui.press_count, 500)
        self.assertEqual([entry.kwargs["text"] for entry in self.gui.press_count_label.config.call_args_list],
                         ["任务完成重复数: 500"])
        self.gui.rapid_test_status.config.assert_called_with(text="状态: 任务完成")

    def test_ui_stop_stays_pending_until_native_result(self):
        self.gui._rapid_test_pending = True
        self.gui._toggle_rapid_test()
        self.assertFalse(self.gui.rapid_test_running)
        self.assertTrue(self.gui._rapid_test_pending)
        self.gui.rapid_test_status.config.assert_called_with(text="状态: 正在取消，等待原生结果")
        self.gui._stop_test("状态: 已取消")
        self.assertFalse(self.gui._rapid_test_pending)

    def test_repeat_limit_is_a_visible_validation_error(self):
        self.gui.rapid_test_running = False
        for name, value in (("test_key_var", "a"), ("press_time_var", "1"),
                            ("interval_time_var", "1"), ("duration_var", "1000")):
            variable = Mock()
            variable.get.return_value = value
            setattr(self.gui, name, variable)
        self.gui._toggle_rapid_test()
        self.assertIn("65535", gui_module.messagebox.showerror.call_args.args[1])
        self.native.SubmitInputTask.assert_not_called()

    def test_worker_start_failure_does_not_leave_rapid_test_pending(self):
        self.gui.rapid_test_running = False
        self.gui._start_worker = Mock(side_effect=RuntimeError("thread unavailable"))
        for name, value in (("test_key_var", "a"), ("press_time_var", "1"),
                            ("interval_time_var", "1"), ("duration_var", "1")):
            variable = Mock()
            variable.get.return_value = value
            setattr(self.gui, name, variable)
        self.gui._toggle_rapid_test()
        self.assertFalse(self.gui.rapid_test_running)
        self.assertFalse(self.gui._rapid_test_pending)
        self.assertIsNone(self.gui._rapid_cancellation)
        self.gui.rapid_test_status.config.assert_called_with(text="状态: 未启动")
        self.native.SubmitInputTask.assert_not_called()

    def test_worker_start_failure_does_not_leave_auto_move_pending(self):
        self.gui.auto_move_running = False
        self.gui._start_worker = Mock(side_effect=RuntimeError("thread unavailable"))
        self.gui.speed_var, self.gui.range_var = Mock(), Mock()
        self.gui.speed_var.get.return_value = "10"
        self.gui.range_var.get.return_value = "100"
        self.gui._toggle_auto_move()
        self.assertFalse(self.gui.auto_move_running)
        self.assertFalse(self.gui._auto_move_pending)
        self.assertIsNone(self.gui._auto_move_cancellation)
        self.gui.auto_move_status.config.assert_called_with(text="状态: 未启动")
        self.native.SubmitInputTask.assert_not_called()

    def test_auto_move_is_closed_finite_native_trajectory(self):
        self.gui._run_auto_move(10, 100, InputCancellation())
        steps, repeats, _ = self.native.submissions[0]
        self.assertEqual(repeats, 65535)
        self.assertEqual(sum(step[1] for step in steps), 0)
        self.assertEqual(sum(step[2] for step in steps), 0)
        self.assertTrue(all(step[3] == 16 for step in steps))
        self.gui.input_workers.wait.assert_not_called()

    def test_auto_move_failure_does_not_retry_or_requeue_input(self):
        self.native.task_result.state = FAILED
        self.native.task_result.error = 29
        self.gui._run_auto_move(10, 100, InputCancellation())
        self.flush_ui()
        self.assertEqual(len(self.native.submissions), 1)
        self.gui.auto_move_status.config.assert_called_with(text="状态: 出错，请卸载重试清理")
        self.gui.input_workers.wait.assert_not_called()

    def test_auto_move_limits_reject_invalid_or_unbounded_generation(self):
        for speed, radius in ((float("nan"), 1), (0, 1), (1e-300, 100), (10, 1e308)):
            with self.subTest(speed=speed, radius=radius):
                with self.assertRaises(ValueError):
                    CQKeysGUI._auto_move_actions(speed, radius)

    def test_auto_move_rejects_trajectories_without_effective_movement(self):
        for speed, move_range in ((1, 1), (1000, 1.2)):
            with self.subTest(speed=speed, move_range=move_range):
                with self.assertRaisesRegex(ValueError, "有效位移"):
                    CQKeysGUI._auto_move_actions(speed, move_range)

    def test_global_stop_collects_task_before_pnp_cleanup(self):
        entered, stopped = threading.Event(), threading.Event()
        self.gui.input_workers = InputWorkers()
        self.gui._stop_results, self.gui._cleanup_results = queue.Queue(), queue.Queue()
        self.gui.keyboard_hook = None
        manager = DriverManager.__new__(DriverManager)
        manager.driver, manager.tasks = self.native, self.gui.input_tester.tasks
        manager._lock = threading.RLock()
        manager._remove_package = False
        self.gui.driver_mgr = manager
        self.native.UnloadNTDriver.return_value = 1

        def wait(*arguments):
            entered.set()
            if not stopped.wait(2):
                raise AssertionError("native stop was not requested")
            return 1

        def stop():
            self.native.task_result.state = CANCELLED
            self.native.task_result.error = 1223
            stopped.set()
            return 1

        self.native.WaitInputTask.side_effect = wait
        self.native.StopInputProcessing.side_effect = stop
        worker = self.gui.input_workers.start(lambda: self.gui.input_tester.press_key("A"))
        try:
            self.assertTrue(entered.wait(1))
            self.gui._begin_unload(close_after=True)
            notification = self.gui._stop_results.get(timeout=1)
            self.gui._stop_results.put(notification)
            worker.join(1)
            self.assertFalse(worker.is_alive())
            self.gui._poll_stop(True)
            result = self.gui._cleanup_results.get(timeout=1)
            self.assertIsNone(result)
            self.gui._cleanup_results.put(result)
            self.gui._poll_cleanup(True)
        finally:
            stopped.set()
            worker.join(1)
        operations = [entry[0] for entry in self.native.mock_calls]
        self.assertLess(operations.index("StopInputProcessing"), operations.index("QueryInputTask"))
        self.assertLess(operations.index("ReleaseInputTask"), operations.index("UnloadNTDriver"))
        self.gui.root.destroy.assert_called_once()


if __name__ == "__main__":
    unittest.main()
