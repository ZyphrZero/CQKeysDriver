"""cdecl task ABI v1; Python builds content, the DLL owns input timing."""

import ctypes
import math
import threading

from input_workers import WorkCancelled


KEY_DOWN, KEY_UP, CONSUMER_DOWN, CONSUMER_UP = 1, 2, 3, 4
MOVE_RELATIVE, MOVE_ABSOLUTE, BUTTONS, WHEEL, WAIT = 5, 6, 7, 8, 9
RUNNING, COMPLETED, CANCELLED, FAILED = 1, 2, 3, 4
PARTIAL, UNCERTAIN, CANCELLED_FLAG = 1, 2, 4
MAX_ACTIONS, MAX_REPEATS, MAX_DELAY_MS = 4096, 65535, 60000


class CqInputAction(ctypes.Structure):
    _pack_ = 8
    _fields_ = [("type", ctypes.c_uint32), ("value1", ctypes.c_int32),
                ("value2", ctypes.c_int32), ("delayMs", ctypes.c_uint32)]


class CqInputTaskResult(ctypes.Structure):
    _pack_ = 8
    _fields_ = [
        ("size", ctypes.c_uint32), ("version", ctypes.c_uint32),
        ("state", ctypes.c_uint32), ("error", ctypes.c_uint32),
        ("taskId", ctypes.c_uint64), ("completedActions", ctypes.c_uint64),
        ("completedRepeats", ctypes.c_uint32), ("cleanupError", ctypes.c_uint32),
        ("lastAcceptedSequence", ctypes.c_uint64), ("lastSubmittedSequence", ctypes.c_uint64),
        ("deviceEpoch", ctypes.c_uint64), ("sessionId", ctypes.c_uint64),
        ("sessionPhase", ctypes.c_uint32), ("pendingCleanup", ctypes.c_uint32),
        ("inputStatus", ctypes.c_int32), ("cleanupStatus", ctypes.c_int32),
        ("resultFlags", ctypes.c_uint32), ("reserved", ctypes.c_uint32),
    ]


class NativeCallError(RuntimeError):
    def __init__(self, operation, code):
        self.operation, self.code = operation, code & 0xffffffff
        super().__init__(f"{operation} 失败：Win32 {self.code}")


class InputTaskError(RuntimeError):
    def __init__(self, result):
        self.result = result
        super().__init__(
            f"任务 {result.taskId}: state={result.state}, error={result.error}, "
            f"completedActions={result.completedActions}, completedRepeats={result.completedRepeats}, "
            f"cleanupError={result.cleanupError}, "
            f"pendingCleanup={result.pendingCleanup}, inputStatus={result.inputStatus}, "
            f"cleanupStatus={result.cleanupStatus}, resultFlags={result.resultFlags:#x}")


class InputTaskCancelled(WorkCancelled):
    def __init__(self, result):
        self.result = result
        super().__init__(f"任务 {result.taskId} 已取消，完整动作数 {result.completedActions}，"
                         f"resultFlags={result.resultFlags:#x}")


def checked_int(value, lower, upper, name):
    if not isinstance(value, int) or isinstance(value, bool) or not lower <= value <= upper:
        raise ValueError(f"{name} 必须是 {lower}..{upper} 范围内的整数")
    return value


def milliseconds(seconds):
    if not math.isfinite(seconds) or not 0 <= seconds <= MAX_DELAY_MS / 1000:
        raise ValueError("动作等待时间必须为 0..60 秒的有限数值")
    return math.ceil(seconds * 1000)


def action(kind, value1=0, value2=0, delay_ms=0):
    checked_int(kind, KEY_DOWN, WAIT, "动作类型")
    checked_int(value1, -2147483648, 2147483647, "value1")
    checked_int(value2, -2147483648, 2147483647, "value2")
    checked_int(delay_ms, 0, MAX_DELAY_MS, "delayMs")
    if kind in (KEY_DOWN, KEY_UP):
        checked_int(value1, 1, 0xffff, "VK")
    elif kind in (CONSUMER_DOWN, CONSUMER_UP):
        checked_int(value1, 1, 0x3ff, "consumer usage")
    elif kind == BUTTONS:
        checked_int(value1, 0, 31, "buttons")
    elif kind == WHEEL:
        checked_int(value1, -127, 127, "wheel")
    elif kind == WAIT and value1 != 0:
        raise ValueError("WAIT 的值必须为零")
    if kind not in (MOVE_RELATIVE, MOVE_ABSOLUTE) and value2 != 0:
        raise ValueError("该动作的 value2 必须为零")
    return CqInputAction(kind, value1, value2, delay_ms)


def bind_tasks(native):
    signatures = {
        "SubmitInputTask": [ctypes.POINTER(CqInputAction), ctypes.c_uint32, ctypes.c_uint32,
                            ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint64)],
        "CancelInputTask": [ctypes.c_uint64],
        "WaitInputTask": [ctypes.c_uint64, ctypes.c_uint32],
        "QueryInputTask": [ctypes.c_uint64, ctypes.POINTER(CqInputTaskResult)],
        "ReleaseInputTask": [ctypes.c_uint64],
        "CleanupInputRuntime": [],
    }
    for name, arguments in signatures.items():
        function = getattr(native, name)
        function.argtypes, function.restype = arguments, ctypes.c_int


def call_bool(native, name, *arguments):
    ctypes.set_last_error(0)
    if not getattr(native, name)(*arguments):
        raise NativeCallError(name, ctypes.get_last_error())


class InputTask:
    def __init__(self, native, task_id, expected_actions=None, expected_repeats=None):
        self.native, self.id = native, task_id
        self.expected_actions = expected_actions
        self.expected_repeats = expected_repeats
        self.result = None
        self.released = False

    def cancel(self):
        call_bool(self.native, "CancelInputTask", self.id)

    def wait(self, timeout_ms=100):
        checked_int(timeout_ms, 0, 0xffffffff, "timeoutMs")
        try:
            call_bool(self.native, "WaitInputTask", self.id, timeout_ms)
        except NativeCallError as error:
            if error.code == 258:
                return False
            raise
        return True

    def query(self):
        result = CqInputTaskResult(size=ctypes.sizeof(CqInputTaskResult), version=1)
        call_bool(self.native, "QueryInputTask", self.id, ctypes.byref(result))
        if (result.size != ctypes.sizeof(result) or result.version != 1 or
                result.taskId != self.id or result.state not in (RUNNING, COMPLETED, CANCELLED, FAILED) or
                result.resultFlags & ~(PARTIAL | UNCERTAIN | CANCELLED_FLAG) or result.reserved):
            raise RuntimeError("DLL 返回了不兼容的任务结果")
        self.result = result
        return result


class InputTasks:
    def __init__(self, native):
        bind_tasks(native)
        self.native = native
        self.active = None
        self.last_result = None
        self._lock = threading.RLock()
        self._waiter = None

    def submit(self, actions, repeats=1, repeat_delay_ms=0):
        checked_int(repeats, 1, MAX_REPEATS, "repeats")
        checked_int(repeat_delay_ms, 0, MAX_DELAY_MS, "repeatDelayMs")
        checked_int(len(actions), 1, MAX_ACTIONS, "action count")
        validated = [action(step.type, step.value1, step.value2, step.delayMs) for step in actions]
        buffer = (CqInputAction * len(validated))(*validated)
        task_id = ctypes.c_uint64()
        with self._lock:
            if self.active is not None or self._waiter is not None:
                raise NativeCallError("SubmitInputTask", 170)
            call_bool(self.native, "SubmitInputTask", buffer, len(buffer), repeats,
                      repeat_delay_ms, ctypes.byref(task_id))
            if not task_id.value:
                raise RuntimeError("DLL 成功返回但没有任务 ID")
            self.active = InputTask(self.native, task_id.value, len(buffer) * repeats, repeats)
            return self.active

    def release(self, task):
        with self._lock:
            if self.active is not task:
                raise RuntimeError("任务不属于当前调用对象")
            if self._waiter not in (None, threading.get_ident()):
                raise RuntimeError("任务仍有等待线程，不能释放")
            call_bool(self.native, "ReleaseInputTask", task.id)
            task.released = True
            self.active = None

    def wait_result(self, task, cancelled=None, progress=None):
        with self._lock:
            if self.active is not task:
                raise RuntimeError("任务不属于当前调用对象")
            if self._waiter is not None:
                raise RuntimeError("任务已有等待线程")
            self._waiter = threading.get_ident()
        try:
            return self._wait_result(task, cancelled, progress)
        finally:
            with self._lock:
                self._waiter = None

    def _wait_result(self, task, cancelled, progress):
        cancel_sent = False
        while True:
            if cancelled is not None and cancelled() and not cancel_sent:
                task.cancel()
                cancel_sent = True
            finished = task.wait()
            result = task.query()
            self.last_result = result
            if progress is not None:
                progress(result)
            if finished:
                break
        if (result.state not in (COMPLETED, CANCELLED) or result.cleanupError or
                result.pendingCleanup or result.cleanupStatus < 0 or
                result.resultFlags & UNCERTAIN or
                (result.state == COMPLETED and (
                    result.error or result.resultFlags or result.inputStatus < 0 or
                    (task.expected_actions is not None and result.completedActions != task.expected_actions) or
                    (task.expected_repeats is not None and result.completedRepeats != task.expected_repeats))) or
                (result.state == CANCELLED and result.error not in (0, 995, 1223, 21))):
            raise InputTaskError(result)
        self.release(task)
        if result.state == CANCELLED:
            raise InputTaskCancelled(result)
        return result

    def run(self, actions, repeats=1, repeat_delay_ms=0, cancelled=None, progress=None):
        if cancelled is not None and cancelled():
            raise WorkCancelled("任务提交前已取消")
        with self._lock:
            task = self.submit(actions, repeats, repeat_delay_ms)
            self._waiter = threading.get_ident()
        try:
            return self._wait_result(task, cancelled, progress)
        except KeyboardInterrupt as interrupted:
            try:
                task.cancel()
                self._wait_result(task, None, None)
            except InputTaskCancelled:
                pass
            except Exception as cleanup_error:
                raise interrupted from cleanup_error
            raise
        finally:
            with self._lock:
                self._waiter = None

    def cleanup(self):
        """Call after callers have stopped using tasks; failures retain ownership."""
        with self._lock:
            self.collect()
            call_bool(self.native, "CleanupInputRuntime")
            self.active = None

    def collect(self):
        """Explicit cleanup retry: cancel, collect diagnostics, then release the task."""
        with self._lock:
            if self._waiter is not None:
                raise RuntimeError("任务仍有等待线程，请停止调用线程后重试清理")
            task = self.active
            if task is None:
                return self.last_result
            self.last_result = task.query()
            if self.last_result.state == RUNNING:
                task.cancel()
            while not task.wait():
                pass
            result = task.query()
            self.last_result = result
            if result.state == RUNNING:
                raise InputTaskError(result)
            call_bool(self.native, "ReleaseInputTask", task.id)
            task.released = True
            self.active = None
            return result
