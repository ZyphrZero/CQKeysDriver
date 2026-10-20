"""ctypes bindings for the CQKeys DLL."""
import ctypes
import logging
from pathlib import Path

_ROOT = Path(__file__).resolve().parents[2]
_DLL_CANDIDATES = (
    Path(__file__).resolve().parent / "cqkeysdll.dll",
    _ROOT / "cqkeysdll/x64/Release/cqkeysdll.dll",
    _ROOT / "cqkeysdll/x64/Debug/cqkeysdll.dll",
)
DEFAULT_DLL_PATH = next((path for path in _DLL_CANDIDATES if path.is_file()), _DLL_CANDIDATES[1])


def _check_input_result(result, function, arguments):
    if not result:
        error = ctypes.get_last_error()
        raise OSError(error, f"{function.__name__} 失败：{ctypes.FormatError(error).strip()}")
    return result


def load_library(path):
    driver = ctypes.CDLL(str(Path(path).resolve()), use_last_error=True)
    getters = {
        "GetProtocolVersion": ctypes.c_uint32,
        "GetDriverStatus": ctypes.c_int,
        "GetDetailedErrorCode": ctypes.c_int,
        "GetLastCheckTime": ctypes.c_ulonglong,
    }
    for name, result_type in getters.items():
        function = getattr(driver, name)
        function.argtypes = []
        function.restype = result_type
    if driver.GetProtocolVersion() != 2:
        raise RuntimeError("DLL 协议版本与示例不匹配")

    operations = {
        "ConnectDevice": [], "DisconnectDevice": [], "ResetInput": [],
        "CheckDeviceStatus": [],
        "KeyDown": [ctypes.c_ushort], "KeyUp": [ctypes.c_ushort],
        "MouseMoveRELATIVE": [ctypes.c_long, ctypes.c_long],
        "MouseMoveABSOLUTE": [ctypes.c_long, ctypes.c_long],
        "MouseWheelUp": [ctypes.c_ushort], "MouseWheelDown": [ctypes.c_ushort],
    }
    for stem in ("MouseLeftButton", "MouseRightButton", "MouseMiddleButton",
                 "MouseXButton1", "MouseXButton2"):
        operations[stem + "Down"] = []
        operations[stem + "Up"] = []
    for name, arguments in operations.items():
        function = getattr(driver, name)
        function.argtypes = arguments
        function.restype = ctypes.c_int  # Win32 BOOL
        function.errcheck = _check_input_result
    return driver


class DriverManager:
    def __init__(self, dll_path=None):
        if not ctypes.windll.shell32.IsUserAnAdmin():
            raise PermissionError("请以管理员身份运行示例")
        self.dll_path = Path(dll_path) if dll_path else DEFAULT_DLL_PATH
        if not self.dll_path.is_file():
            raise FileNotFoundError(f"找不到 DLL: {self.dll_path}")
        self.driver = None
        self.last_error = None

    def connect(self):
        self.last_error = None
        try:
            if self.driver is None:
                candidate = load_library(self.dll_path)
                candidate.ConnectDevice()
                self.driver = candidate
            else:
                self.driver.CheckDeviceStatus()
            logging.info("设备已连接")
            return True
        except (OSError, AttributeError, RuntimeError) as error:
            self.last_error = error
            logging.error("连接失败: %s", error)
            return False

    def disconnect(self):
        if self.driver is not None:
            self.driver.DisconnectDevice()
            self.driver = None
            logging.info("设备已断开")

    def get_driver(self):
        return self.driver
