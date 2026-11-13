import sys
from pathlib import Path
import threading
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "cqkeyspy_test"))
from input_workers import InputWorkers, WorkCancelled


class InputWorkersTests(unittest.TestCase):
    def test_cancel_interrupts_wait_and_rejects_new_work(self):
        workers = InputWorkers()
        entered = threading.Event()
        completed = []

        def work():
            entered.set()
            workers.wait(30)
            completed.append(True)

        thread = workers.start(work)
        self.assertTrue(entered.wait(1))
        workers.cancel()
        thread.join(1)
        self.assertFalse(thread.is_alive())
        self.assertEqual(completed, [])
        self.assertTrue(workers.is_idle())
        with self.assertRaises(WorkCancelled):
            workers.start(lambda: None)
        workers.reset()
        workers.start(lambda: completed.append(True)).join(1)
        self.assertEqual(completed, [True])

    def test_reset_cannot_reuse_a_live_session(self):
        workers = InputWorkers()
        release = threading.Event()
        thread = workers.start(lambda: release.wait(2))
        try:
            workers.cancel()
            with self.assertRaises(RuntimeError):
                workers.reset()
        finally:
            release.set()
            thread.join(1)
        workers.reset()


if __name__ == "__main__":
    unittest.main()
