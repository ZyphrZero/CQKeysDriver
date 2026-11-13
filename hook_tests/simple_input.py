# -*- coding: utf-8 -*-

import os
from ctypes import CDLL
import sys
import ctypes
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "cqkeyspy_test"))
from input_tester import InputTester
from input_tasks import InputTasks, InputTaskCancelled
from virtuakeys_mapping import VirtualKeys


class InputInitializationError(RuntimeError):
    def __init__(self, error, tester):
        self.tester = tester
        super().__init__(f"初始化失败: {error}；可通过异常的 tester.cleanup() 重试清理")

class SimpleInputTester:
    def __init__(self, dll_path=None, sys_path=None):
        # 检查管理员权限
        if not ctypes.windll.shell32.IsUserAnAdmin():
            print("请以管理员权限运行此脚本")
            sys.exit(1)

        # 设置默认路径
        base_dir = os.path.dirname(os.path.dirname(__file__))
        self.dll_path = dll_path or os.path.join(base_dir, 'cqkeysdll', 'x64', 'Debug', 'cqkeysdll.dll')
        self.sys_path = sys_path or os.path.join(base_dir, 'cqkeys', 'x64', 'Debug', 'cqkeys', 'cqkeys.inf')

        # 加载驱动
        self.driver = CDLL(self.dll_path, use_last_error=True)
        self.needs_cleanup = False
        self.ready = False
        self.driver.GetLifecycleVersion.argtypes = []
        self.driver.GetLifecycleVersion.restype = ctypes.c_uint32
        if self.driver.GetLifecycleVersion() != 2:
            raise RuntimeError("需要 KMDF/VHF ABI v2 DLL")
        self.driver.LoadNTDriver.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
        self.driver.LoadNTDriver.restype = ctypes.c_int
        self.driver.UnloadNTDriver.argtypes = [ctypes.c_char_p]
        self.driver.UnloadNTDriver.restype = ctypes.c_int
        self.driver.SetHandle.argtypes = []
        self.driver.SetHandle.restype = ctypes.c_int
        for name in ("KeyDown", "KeyUp"):
            function = getattr(self.driver, name)
            function.argtypes = [ctypes.c_ushort]
            function.restype = None
            function.errcheck = self._check_input_error
        
        self.tasks = InputTasks(self.driver)
        self.input_tester = InputTester(self.driver, tasks=self.tasks)
        self.needs_cleanup = True
        try:
            if not self.driver.LoadNTDriver(b'cqkeys', self.sys_path.encode('mbcs')):
                raise ctypes.WinError(ctypes.get_last_error())
            if not self.driver.SetHandle():
                raise ctypes.WinError(ctypes.get_last_error())
            self.ready = True
        except (Exception, KeyboardInterrupt) as error:
            raise InputInitializationError(error, self) from error

    @staticmethod
    def _check_input_error(result, function, arguments):
        error = ctypes.get_last_error()
        if error:
            raise ctypes.WinError(error)
        return result

    def cleanup(self):
        """清理驱动"""
        if self.driver and self.needs_cleanup:
            self.ready = False
            self.input_tester.stop()
            self.tasks.collect()
            if not self.driver.UnloadNTDriver(b'cqkeys'):
                raise ctypes.WinError(ctypes.get_last_error())
            self.tasks.cleanup()
            self.driver = None
            self.needs_cleanup = False

    def press_key(self, key):
        """按下并释放按键"""
        if not self.ready:
            raise RuntimeError("输入会话未就绪")
        return self.input_tester.press_key(key, duration=0.05)

    def type_string(self, text, interval=0.05, initial_delay=0):
        """输入字符串"""
        if not self.ready:
            raise RuntimeError("输入会话未就绪")
        return self.input_tester.type_text(text, interval=interval, duration=0.05,
                                           initial_delay=initial_delay)

    def _get_virtual_key_code(self, key):
        """获取虚拟键码"""
        return VirtualKeys.get_vk_code(key)

    def _needs_shift(self, char):
        """判断字符是否需要按住Shift"""
        return (len(char) == 1 and char.isupper()) or VirtualKeys.needs_shift(char)

def main():
    tester = None
    exit_code = 0
    try:
        tester = SimpleInputTester()
        print("驱动已加载，3秒后开始输入...")
        
        # 测试字符串
        test_string = "Hello, World!"
        print(f"测试字符串输入: {test_string}")
        tester.type_string(test_string, initial_delay=3)
        
    except InputInitializationError as error:
        tester = error.tester
        print(str(error))
        exit_code = 130 if isinstance(error.__cause__, KeyboardInterrupt) else 1
    except (KeyboardInterrupt, InputTaskCancelled) as error:
        print("测试已取消。")
        exit_code = 130
        if error.__cause__ is not None:
            print(f"中断后任务清理失败: {error.__cause__}")
            exit_code = 1
    except Exception as e:
        print(f"测试出错: {str(e)}")
        exit_code = 1
    finally:
        if tester is not None:
            try:
                tester.cleanup()
            except Exception as error:
                print(f"清理失败: {error}；不能视为已完成清理。")
                exit_code = 1
    return exit_code

if __name__ == '__main__':
    raise SystemExit(main())
