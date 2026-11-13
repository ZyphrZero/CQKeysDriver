# -*- coding: utf-8 -*-

import os
import logging
from ctypes import CDLL, c_int, c_ulonglong, c_char_p
from threading import RLock
import json
import sys
import ctypes


class DriverManager:
    def __init__(self):
        self.driver = None
        self._lock = RLock()
        self._setup_logging()
        
        # 在初始化时就检查权限
        if not self.check_and_elevate_privileges():
            raise PermissionError("需要管理员权限")
            
    def _setup_paths(self, dll_path=None, sys_path=None):
        """设置驱动路径
        
        Args:
            dll_path (str, optional): DLL文件的完整路径
            sys_path (str, optional): SYS文件的完整路径
        """
        # 尝试从配置文件加载路径
        config_file = "driver_config.json"
        if os.path.exists(config_file):
            try:
                with open(config_file, "r") as f:
                    config = json.load(f)
                    dll_path = dll_path or config.get("dll_path")
                    sys_path = sys_path or config.get("sys_path")
                    logging.info("从配置文件加载驱动路径")
            except Exception as e:
                logging.warning(f"加载配置文件失败: {str(e)}")
        
        # 如果没有提供路径且配置文件不存在或加载失败，则抛出异常
        if not dll_path or not sys_path:
            raise ValueError("未配置驱动路径，请在配置文件中设置或手动指定路径")
        
        self.dll_path = dll_path
        self.sys_path = sys_path
        
        # 验证路径是否存在
        if not os.path.exists(self.dll_path):
            raise FileNotFoundError(f"找不到DLL文件: {self.dll_path}")
            
        if not os.path.exists(self.sys_path):
            raise FileNotFoundError(f"找不到SYS文件: {self.sys_path}")
    
    def _setup_logging(self):
        """设置日志"""
        logging.basicConfig(
            level=logging.INFO,
            format='%(asctime)s - %(levelname)s - %(message)s'
        )
    
    def check_and_elevate_privileges(self):
        """检查并提升权限
        
        Returns:
            bool: 如果已经有管理员权限返回True，需要提权返回False
        """
        try:
            if not self.is_admin():
                logging.warning("需要管理员权限才能加载驱动")
                return False
            return True
        except Exception as e:
            logging.error(f"权限检查失败: {str(e)}")
            return False
    
    def is_admin(self):
        """检查是否具有管理员权限"""
        try:
            return ctypes.windll.shell32.IsUserAnAdmin()
        except:
            return False
            
    def run_as_admin(self):
        """请求管理员权限"""
        if not self.is_admin():
            logging.info("正在请求管理员权限...")
            script = os.path.abspath(sys.argv[0])
            params = ' '.join(sys.argv[1:])
            try:
                ret = ctypes.windll.shell32.ShellExecuteW(
                    None, 
                    "runas",
                    sys.executable,
                    f'"{script}" {params}',
                    None,
                    1
                )
                if ret > 32:
                    return True
                else:
                    logging.error(f"权限提升失败,错误码: {ret}")
                    return False
            except Exception as e:
                logging.error(f"权限提升异常: {str(e)}")
                return False
        return True

    def initialize(self):
        """加载并连接驱动；失败时保留可重试的管理对象，不删除已有服务。"""
        with self._lock:
            if self.driver is not None:
                raise RuntimeError("请先停止或卸载当前驱动会话")
            try:
                if not self.is_admin():
                    raise PermissionError("请以管理员身份运行程序")
                native = CDLL(self.dll_path, use_last_error=True)
                native.GetLifecycleVersion.argtypes = []
                native.GetLifecycleVersion.restype = ctypes.c_uint32
                if native.GetLifecycleVersion() != 1:
                    raise RuntimeError("DLL 生命周期接口版本不匹配，请使用本分支构建的 DLL")
                for name in ("LoadNTDriver", "StopNTDriver", "UnloadNTDriver"):
                    function = getattr(native, name)
                    function.argtypes = [c_char_p, c_char_p] if name == "LoadNTDriver" else [c_char_p]
                    function.restype = c_int
                for name in ("SetHandle", "StopInputProcessing"):
                    function = getattr(native, name)
                    function.argtypes = []
                    function.restype = c_int
                native.GetDriverStatus.restype = c_int
                native.GetLastCheckTime.restype = c_ulonglong
                for name in ("KeyDown", "KeyUp", "MouseWheelUp", "MouseWheelDown"):
                    function = getattr(native, name)
                    function.argtypes = [ctypes.c_ushort]
                    function.restype = None
                for name in ("MouseMoveRELATIVE", "MouseMoveABSOLUTE"):
                    function = getattr(native, name)
                    function.argtypes = [ctypes.c_long, ctypes.c_long]
                    function.restype = None
                for button in ("LeftButton", "RightButton", "MiddleButton", "XButton1", "XButton2"):
                    for action in ("Down", "Up"):
                        function = getattr(native, "Mouse" + button + action)
                        function.argtypes = []
                        function.restype = None
                native.CheckDeviceStatus.argtypes = []
                native.CheckDeviceStatus.restype = None
                self.driver = native
                if not self.driver.LoadNTDriver(b'cqkeys', self.sys_path.encode('mbcs')):
                    error = ctypes.get_last_error()
                    if error == 1073:  # ERROR_SERVICE_EXISTS: another binary owns this name.
                        self.driver = None
                    raise ctypes.WinError(error)
                if not self.driver.SetHandle():
                    raise ctypes.WinError(ctypes.get_last_error())
                logging.info("驱动初始化完成")
                return True
            except Exception:
                # DLL 负责清理连接失败的句柄；不能在这里卸载另一个客户端的服务。
                logging.exception("驱动初始化失败；可使用卸载按钮重试清理")
                return False

    def stop_input(self):
        """阻止新输入并释放本 DLL 会话记录的按键；失败必须交给调用方。"""
        with self._lock:
            if self.driver is not None and not self.driver.StopInputProcessing():
                raise ctypes.WinError(ctypes.get_last_error())

    def cleanup(self, uninstall=True):
        """停止驱动；uninstall=False 保留服务注册以便再次加载。"""
        with self._lock:
            if self.driver is None:
                return
            operation = self.driver.UnloadNTDriver if uninstall else self.driver.StopNTDriver
            if not operation(b'cqkeys'):
                raise ctypes.WinError(ctypes.get_last_error())
            self.driver = None
            logging.info("驱动已卸载" if uninstall else "驱动已停止，服务注册已保留")

    def get_driver(self):
        """获取驱动实例"""
        return self.driver 