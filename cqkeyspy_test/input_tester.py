# -*- coding: utf-8 -*-

import ctypes
import logging
import math
import threading
import time

from input_tasks import (InputTasks, KEY_DOWN, KEY_UP, BUTTONS, WAIT,
                         MAX_ACTIONS, MAX_REPEATS, action, milliseconds)
from input_workers import WorkCancelled
from virtuakeys_mapping import VirtualKeys


class InputTester:
    """Legacy primitives and finite native tasks; no Python input timing."""

    def __init__(self, driver, wait=None, tasks=None):
        self.driver = driver
        self.tasks = tasks if tasks is not None else InputTasks(driver)
        self._stopped = threading.Event()
        self.last_check_time = 0
        self.check_interval = 1.0

    def stop(self):
        self._stopped.set()

    def _call(self, name, *args):
        if self._stopped.is_set():
            raise WorkCancelled("输入会话已停止")
        ctypes.set_last_error(0)
        getattr(self.driver, name)(*args)
        error = ctypes.get_last_error()
        if error:
            raise ctypes.WinError(error)
        return True

    def _check_device_status(self):
        if self._stopped.is_set():
            return False
        current_time = time.monotonic()
        if current_time - self.last_check_time >= self.check_interval:
            self.check_device_status()
            self.last_check_time = current_time
        status = self.get_driver_status()
        if status != 1:
            logging.warning("驱动状态异常: %s", status)
        return status == 1

    def check_device_status(self):
        if self.driver:
            self._call("CheckDeviceStatus")

    def get_driver_status(self):
        return self.driver.GetDriverStatus() if self.driver else 0

    def get_last_check_time(self):
        return self.driver.GetLastCheckTime() if self.driver else 0

    @staticmethod
    def _vk(key):
        value = VirtualKeys.get_vk_code(key)
        if value is None:
            raise ValueError(f"无效的按键: {key}")
        return value

    @classmethod
    def key_actions(cls, key, duration=0.1, interval=0):
        value = cls._vk(key)
        hold_ms, interval_ms = milliseconds(duration), milliseconds(interval)
        shifted = VirtualKeys.needs_shift(key) or (len(key) == 1 and key.isupper())
        steps = [action(KEY_DOWN, 0x10)] if shifted else []
        steps.extend([action(KEY_DOWN, value, delay_ms=hold_ms), action(KEY_UP, value)])
        if shifted:
            steps.append(action(KEY_UP, 0x10))
        steps[-1].delayMs = interval_ms
        return steps

    @staticmethod
    def click_actions(button="left", duration=0.1, interval=0):
        buttons = {"left": 1, "right": 2, "middle": 4, "x1": 8, "x2": 16}
        if button not in buttons:
            raise ValueError(f"无效的鼠标按键: {button}")
        return [action(BUTTONS, buttons[button], delay_ms=milliseconds(duration)),
                action(BUTTONS, 0, delay_ms=milliseconds(interval))]

    def run_actions(self, actions, repeats=1, repeat_delay_ms=0, cancelled=None, progress=None):
        return self.tasks.run(
            actions, repeats, repeat_delay_ms,
            cancelled=lambda: self._stopped.is_set() or (cancelled is not None and cancelled()),
            progress=progress)

    def press_key(self, key, duration=0.1):
        self.run_actions(self.key_actions(key, duration))
        return True

    def type_text(self, text, interval=0.05, duration=0.1, initial_delay=0):
        steps = [action(WAIT, delay_ms=milliseconds(initial_delay))] if initial_delay else []
        for character in text:
            steps.extend(self.key_actions(character, duration, interval))
            if len(steps) > MAX_ACTIONS:
                raise ValueError(f"文本动作超过 {MAX_ACTIONS} 个；请缩短文本")
        if steps:
            return self.run_actions(steps)
        return None

    @staticmethod
    def repeat_count(duration, hold, interval):
        period_ms = milliseconds(hold) + milliseconds(interval)
        if not math.isfinite(duration) or duration <= 0 or not period_ms:
            raise ValueError("重复任务需要正的有限运行时长和非零周期")
        if duration > MAX_REPEATS * period_ms / 1000:
            raise ValueError(f"重复次数最多为 {MAX_REPEATS}；请缩短运行时长")
        repeats = math.ceil(duration * 1000 / period_ms)
        if not 1 <= repeats <= MAX_REPEATS:
            raise ValueError(f"重复次数必须为 1..{MAX_REPEATS}；请缩短运行时长")
        return repeats

    def repeat_key(self, key, hold, interval, duration, cancelled=None, progress=None):
        repeats = self.repeat_count(duration, hold, interval)
        return self.run_actions(self.key_actions(key, hold, interval), repeats,
                                cancelled=cancelled, progress=progress)

    def key_down(self, key):
        return self._call("KeyDown", self._vk(key))

    def key_up(self, key):
        return self._call("KeyUp", self._vk(key))

    def mouse_move_rel(self, dx, dy):
        return self._call("MouseMoveRELATIVE", dx, dy)

    def mouse_move_abs(self, x, y):
        return self._call("MouseMoveABSOLUTE", x, y)

    def mouse_left_down(self):
        return self._call("MouseLeftButtonDown")

    def mouse_left_up(self):
        return self._call("MouseLeftButtonUp")

    def mouse_right_down(self):
        return self._call("MouseRightButtonDown")

    def mouse_right_up(self):
        return self._call("MouseRightButtonUp")

    def mouse_middle_down(self):
        return self._call("MouseMiddleButtonDown")

    def mouse_middle_up(self):
        return self._call("MouseMiddleButtonUp")

    def mouse_x1_down(self):
        return self._call("MouseXButton1Down")

    def mouse_x1_up(self):
        return self._call("MouseXButton1Up")

    def mouse_x2_down(self):
        return self._call("MouseXButton2Down")

    def mouse_x2_up(self):
        return self._call("MouseXButton2Up")

    def _click(self, button, duration):
        self.run_actions(self.click_actions(button, duration))
        return True

    def mouse_click(self, duration=0.1):
        return self._click("left", duration)

    def mouse_right_click(self, duration=0.1):
        return self._click("right", duration)

    def mouse_middle_click(self, duration=0.1):
        return self._click("middle", duration)

    def mouse_x1_click(self, duration=0.1):
        return self._click("x1", duration)

    def mouse_x2_click(self, duration=0.1):
        return self._click("x2", duration)

    def mouse_wheel_up(self, wheel_delta=120):
        return self._call("MouseWheelUp", wheel_delta)

    def mouse_wheel_down(self, wheel_delta=120):
        return self._call("MouseWheelDown", wheel_delta)

    def mouse_wheel(self, delta):
        if delta > 0:
            return self.mouse_wheel_up(delta)
        if delta < 0:
            return self.mouse_wheel_down(-delta)
        return True
