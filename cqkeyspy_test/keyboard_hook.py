import ctypes
from ctypes import wintypes
import win32con
import win32api
import win32gui
import threading
import logging
import sys
import os

# 定义KBDLLHOOKSTRUCT结构体
class KBDLLHOOKSTRUCT(ctypes.Structure):
    _fields_ = [
        ("vkCode", wintypes.DWORD),
        ("scanCode", wintypes.DWORD),
        ("flags", wintypes.DWORD),
        ("time", wintypes.DWORD),
        ("dwExtraInfo", ctypes.POINTER(wintypes.ULONG))
    ]

class KeyboardHook:
    """全局键盘钩子类"""
    
    # Windows钩子类型
    WH_KEYBOARD_LL = 13
    
    # 回调函数类型
    HOOKPROC = ctypes.WINFUNCTYPE(
        wintypes.LPARAM,
        ctypes.c_int,
        wintypes.WPARAM,
        wintypes.LPARAM
    )
    
    def __init__(self):
        """初始化键盘钩子"""
        self.user32 = ctypes.WinDLL("user32", use_last_error=True)
        self.kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        
        # 定义CallNextHookEx的参数类型
        self.user32.CallNextHookEx.argtypes = [
            wintypes.HHOOK,
            ctypes.c_int,
            wintypes.WPARAM,
            wintypes.LPARAM
        ]
        self.user32.CallNextHookEx.restype = wintypes.LPARAM
        
        self.hook_id = None
        self.hook_thread = None
        self.is_running = False
        self._hook_callback_ptr = None  # 保持回调函数的引用
        self._ready = threading.Event()
        self._stop_requested = threading.Event()
        self._hook_error = None
        self.user32.SetWindowsHookExW.argtypes = [ctypes.c_int, self.HOOKPROC, wintypes.HINSTANCE, wintypes.DWORD]
        self.user32.SetWindowsHookExW.restype = wintypes.HHOOK
        self.user32.UnhookWindowsHookEx.argtypes = [wintypes.HHOOK]
        self.user32.UnhookWindowsHookEx.restype = wintypes.BOOL
        self.kernel32.GetModuleHandleW.restype = wintypes.HMODULE
        
        # 热键回调字典 {vk_code: callback_func}
        self.hotkey_callbacks = {}
        
        # 当前按下的按键集合
        self.pressed_keys = set()
        
    def _hook_callback(self, nCode, wParam, lParam):
        """钩子回调函数"""
        if nCode >= 0:
            try:
                kb_struct = ctypes.cast(lParam, ctypes.POINTER(KBDLLHOOKSTRUCT)).contents
                vk_code = kb_struct.vkCode
                
                # 按键按下
                if wParam == win32con.WM_KEYDOWN:
                    self.pressed_keys.add(vk_code)
                    # 检查是否触发了任何热键组合
                    for hotkey_combo, callback in self.hotkey_callbacks.items():
                        if isinstance(hotkey_combo, tuple):
                            # 组合键
                            if all(key in self.pressed_keys for key in hotkey_combo):
                                callback()
                        else:
                            # 单个键
                            if vk_code == hotkey_combo:
                                callback()
                
                # 按键释放
                elif wParam == win32con.WM_KEYUP:
                    if vk_code in self.pressed_keys:
                        self.pressed_keys.remove(vk_code)
                
            except Exception as e:
                logging.error(f"键盘钩子回调错误: {str(e)}")
        
        # 继续传递给其他钩子
        return self.user32.CallNextHookEx(self.hook_id, nCode, wParam, lParam)
    
    def register_hotkey(self, vk_code, callback):
        """注册热键回调
        
        Args:
            vk_code: 虚拟键码或键码元组(组合键)
            callback: 回调函数
        """
        self.hotkey_callbacks[vk_code] = callback
    
    def unregister_hotkey(self, vk_code):
        """注销热键回调"""
        if vk_code in self.hotkey_callbacks:
            del self.hotkey_callbacks[vk_code]
    
    def start(self):
        if self.hook_thread is not None:
            raise RuntimeError("请先停止上一个热键线程")
        self._stop_requested.clear()
        self._ready.clear()
        self._hook_error = None

        def run_hook():
            try:
                msg = wintypes.MSG()
                # Create the message queue before stop() can post WM_QUIT.
                self.user32.PeekMessageW(ctypes.byref(msg), None, 0, 0, 0)
                self._hook_callback_ptr = self.HOOKPROC(self._hook_callback)
                module_handle = self.kernel32.GetModuleHandleW(None) if hasattr(sys, 'frozen') else None
                self.hook_id = self.user32.SetWindowsHookExW(
                    self.WH_KEYBOARD_LL, self._hook_callback_ptr, module_handle, 0)
                if not self.hook_id:
                    raise ctypes.WinError(ctypes.get_last_error())
                self.is_running = True
                self._ready.set()
                while not self._stop_requested.is_set():
                    result = self.user32.GetMessageW(ctypes.byref(msg), None, 0, 0)
                    if result == -1:
                        raise ctypes.WinError(ctypes.get_last_error())
                    if result == 0:
                        break
                    self.user32.TranslateMessage(ctypes.byref(msg))
                    self.user32.DispatchMessageW(ctypes.byref(msg))
            except Exception as error:
                self._hook_error = error
            finally:
                if self.hook_id:
                    if self.user32.UnhookWindowsHookEx(self.hook_id):
                        self.hook_id = None
                    else:
                        self._hook_error = ctypes.WinError(ctypes.get_last_error())
                self.is_running = False
                self._ready.set()

        self.hook_thread = threading.Thread(target=run_hook, daemon=True)
        self.hook_thread.start()
        if not self._ready.wait(timeout=2):
            self._stop_requested.set()
            raise TimeoutError("热键线程启动超时，请停止后重试")
        if self._hook_error is not None:
            raise self._hook_error

    def stop(self):
        thread = self.hook_thread
        if thread is None:
            return
        self._stop_requested.set()
        if self._ready.is_set() and thread.is_alive():
            self.user32.PostThreadMessageW(thread.native_id, win32con.WM_QUIT, 0, 0)
        thread.join(timeout=2)
        if thread.is_alive():
            # Keep the callback alive while Windows might still call it.
            raise TimeoutError("热键线程尚未退出，请稍后重试")
        if self.hook_id is not None:
            raise self._hook_error or RuntimeError("热键钩子尚未移除")
        self.hook_thread = None
        self._hook_callback_ptr = None
        self.pressed_keys.clear()
        if self._hook_error is not None:
            error, self._hook_error = self._hook_error, None
            raise error
        logging.info("键盘钩子已停止")
