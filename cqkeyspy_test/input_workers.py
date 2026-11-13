"""Tracked, cooperatively cancellable input workers; independent of Tk."""
import threading


class WorkCancelled(Exception):
    pass


class InputWorkers:
    def __init__(self):
        self._cancelled = threading.Event()
        self._lock = threading.Lock()
        self._threads = set()

    def start(self, target, args=()):
        def run():
            try:
                self.check_cancelled()
                target(*args)
            except WorkCancelled:
                pass
            finally:
                with self._lock:
                    self._threads.discard(threading.current_thread())

        with self._lock:
            self.check_cancelled()
            thread = threading.Thread(target=run, daemon=True)
            self._threads.add(thread)
            try:
                thread.start()
            except Exception:
                self._threads.remove(thread)
                raise
            return thread

    def cancel(self):
        self._cancelled.set()

    def check_cancelled(self):
        if self._cancelled.is_set():
            raise WorkCancelled("输入会话已停止")

    def wait(self, seconds):
        if self._cancelled.wait(max(0, seconds)):
            raise WorkCancelled("输入会话已停止")

    def is_idle(self):
        with self._lock:
            return not self._threads

    def reset(self):
        with self._lock:
            if self._threads:
                raise RuntimeError("旧输入线程尚未退出，不能开始新会话")
            self._cancelled.clear()
