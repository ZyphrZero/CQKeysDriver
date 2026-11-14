# -*- coding: utf-8 -*-

import os
import logging
from ctypes import CDLL, c_int, c_ulonglong, c_char_p
from threading import RLock
import json
import sys
import ctypes
from input_tasks import InputTasks


def configured_inf_path(config):
    if "inf_path" in config:
        return config["inf_path"] or ""
    legacy_path = config.get("sys_path", "").strip()
    if not legacy_path:
        return ""
    stem, extension = os.path.splitext(legacy_path)
    if extension.lower() not in (".sys", ".inf"):
        raise ValueError("旧 sys_path 配置必须指向 SYS 或 INF 文件，请重新选择 cqkeys.inf")
    inf_path = stem + ".inf"
    logging.warning("读取旧 sys_path 配置，改用 INF 路径: %s；请保存新配置", inf_path)
    return inf_path


class DriverManager:
    def __init__(self):
        self.driver = None
        self.tasks = None
        self.loaded_dll_path = None
        self._remove_package = False
        self._lock = RLock()
        self._setup_logging()
        
        # 在初始化时就检查权限
        if not self.check_and_elevate_privileges():
            raise PermissionError("需要管理员权限")
            
    def _setup_paths(self, dll_path=None, inf_path=None):
        """配置 DLL 和 INF 路径，并检查完整部署文件。"""
        config_file = "driver_config.json"
        if (dll_path is None or inf_path is None) and os.path.exists(config_file):
            with open(config_file, "r", encoding="utf-8") as source:
                config = json.load(source)
            if not isinstance(config, dict):
                raise ValueError("配置文件必须包含 JSON 对象")
            if dll_path is None:
                dll_path = config.get("dll_path")
            if inf_path is None:
                inf_path = configured_inf_path(config)
            logging.info("从配置文件加载驱动路径")
        if not dll_path or not dll_path.strip() or not inf_path or not inf_path.strip():
            raise ValueError("DLL 路径和 INF 路径不能为空，请选择完整驱动包中的 cqkeys.inf")
        self.dll_path = os.path.abspath(os.path.expanduser(dll_path.strip()))
        self.inf_path = os.path.abspath(os.path.expanduser(inf_path.strip()))
        self._validate_deployment_files()

    def _validate_deployment_files(self):
        if os.path.basename(self.inf_path).lower() != "cqkeys.inf":
            raise ValueError("INF 路径必须指向 cqkeys.inf，不能仅提供 SYS 文件")
        directory = os.path.dirname(self.inf_path)
        required = [self.inf_path, os.path.join(directory, "cqkeys.sys"),
                    os.path.join(directory, "cqkeys.cat"), self.dll_path]
        missing = [path for path in required if not os.path.isfile(path)]
        if missing:
            for path in missing:
                logging.error("部署文件不存在或不是普通文件: %s", path)
            raise FileNotFoundError("驱动部署文件不完整，请准备 DLL 和匹配的 INF/SYS/CAT：\n" + "\n".join(missing))
    
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
        """安装并连接 VHF 设备；失败时保留对象供 PnP 清理重试。"""
        with self._lock:
            if self.driver is not None:
                raise RuntimeError("请先停止或卸载当前驱动会话")
            try:
                if not self.is_admin():
                    raise PermissionError("请以管理员身份运行程序")
                self._validate_deployment_files()
                logging.info("请求加载 DLL: %s", self.dll_path)
                native = CDLL(self.dll_path, use_last_error=True)
                self.loaded_dll_path = self._get_loaded_dll_path(native)
                logging.info("实际加载 DLL: %s", self.loaded_dll_path)
                logging.info("Driver Store 删除接口: %s", "可用" if hasattr(native, "UnloadNTDriverAndPackage") else "不可用")
                native.GetLifecycleVersion.argtypes = []
                native.GetLifecycleVersion.restype = ctypes.c_uint32
                if native.GetLifecycleVersion() != 2:
                    raise RuntimeError("DLL 生命周期接口版本不匹配，需要 ABI v2 DLL")
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
                for name in ("KeyDown", "KeyUp", "ConsumerKeyDown", "ConsumerKeyUp", "MouseWheelUp", "MouseWheelDown"):
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
                self.tasks = InputTasks(native)
                self.driver = native
                if not self.driver.LoadNTDriver(b'cqkeys', self.inf_path.encode('mbcs')):
                    error = ctypes.get_last_error()
                    raise ctypes.WinError(error)
                if not self.driver.SetHandle():
                    raise ctypes.WinError(ctypes.get_last_error())
                logging.info("驱动初始化完成")
                return True
            except Exception:
                # DLL 会话租约决定是否拥有 PnP 清理权，未取得租约的 cleanup 不操作设备。
                logging.exception("驱动初始化失败；可使用卸载按钮重试清理")
                return False

    @staticmethod
    def _get_loaded_dll_path(native):
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        get_path = kernel.GetModuleFileNameW
        get_path.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_wchar), ctypes.c_uint32]
        get_path.restype = ctypes.c_uint32
        buffer = ctypes.create_unicode_buffer(32768)
        length = get_path(native._handle, buffer, len(buffer))
        if not length:
            raise ctypes.WinError(ctypes.get_last_error())
        if length >= len(buffer):
            raise ctypes.WinError(122)
        return buffer.value

    def require_package_removal(self):
        if self.driver is None:
            raise RuntimeError("当前没有 DLL 管理会话")
        try:
            operation = self.driver.UnloadNTDriverAndPackage
        except AttributeError as error:
            raise RuntimeError(
                "当前会话的 DLL 缺少 Driver Store 删除接口，请更新 cqkeysdll.dll；未退回普通卸载。\n"
                f"实际加载 DLL: {self.loaded_dll_path or '未记录'}\n"
                f"会话配置 DLL: {self.dll_path}\n"
                "本次未开始删除。可取消勾选后普通卸载（保留驱动包），再退出并用新版 DLL 重启；"
                "修改路径框不会替换当前已加载的 DLL。") from error
        operation.argtypes = [c_char_p]
        operation.restype = c_int
        return operation

    def stop_input(self):
        """建立内核停止屏障并释放会话输入；失败必须交给调用方。"""
        with self._lock:
            if self.driver is not None and not self.driver.StopInputProcessing():
                raise ctypes.WinError(ctypes.get_last_error())

    def cleanup(self, uninstall=True, remove_package=False):
        """移除或禁用节点；显式选择时删除本次会话相关的 Driver Store 包。"""
        with self._lock:
            if not uninstall and (remove_package or self._remove_package):
                raise ValueError("删除 Driver Store 包必须同时卸载设备")
            if self.driver is None:
                return
            operation = self.driver.UnloadNTDriver if uninstall else self.driver.StopNTDriver
            if remove_package or self._remove_package:
                operation = self.require_package_removal()
                self._remove_package = True
            if self.tasks is not None:
                self.tasks.collect()
            if not operation(b'cqkeys'):
                raise ctypes.WinError(ctypes.get_last_error())
            if self.tasks is not None:
                self.tasks.cleanup()
            self.driver = None
            self.tasks = None
            if self._remove_package:
                logging.info("驱动管理会话和本次相关 Driver Store 包已清理；未扫描历史包")
            else:
                logging.info("驱动管理会话已清理，驱动包保留" if uninstall else "驱动管理会话已停止")
            self._remove_package = False

    def get_driver(self):
        """获取驱动实例"""
        return self.driver
