"""Package discovery, PnP inspection and the explicit driver installation action."""
import ctypes
from ctypes import wintypes
from dataclasses import dataclass
from pathlib import Path
import re
import subprocess


DEVICE_INSTANCE = r"ROOT\CQKEYSVHF\0000"


@dataclass(frozen=True)
class DeviceStatus:
    present: bool
    started: bool = False
    problem: int = 0

    @property
    def text(self):
        if not self.present:
            return "设备未安装：请选择配套驱动包，点击“安装并连接”"
        if self.problem:
            reasons = {
                10: "设备无法启动，请检查驱动及系统事件日志",
                22: "设备已禁用，请在设备管理器中启用",
                28: "未安装兼容的驱动",
                31: "Windows 无法加载所需驱动",
                39: "驱动缺失或无法加载",
                48: "驱动被系统阻止",
                52: "Windows 无法验证驱动签名，请检查证书和签名环境",
            }
            reason = reasons.get(self.problem, "请查看设备管理器中的详细信息")
            return f"设备异常（代码 {self.problem}）：{reason}"
        if self.started:
            return "设备已安装并已启动"
        return "设备已安装，但尚未启动；请检查设备管理器或重启要求"


def query_device_status():
    """Read only the project's fixed device instance; preserve unexpected API failures."""
    cfg = ctypes.WinDLL("cfgmgr32")
    locate = cfg.CM_Locate_DevNodeW
    locate.argtypes = [ctypes.POINTER(wintypes.DWORD), wintypes.LPWSTR, wintypes.ULONG]
    locate.restype = wintypes.ULONG
    get_status = cfg.CM_Get_DevNode_Status
    get_status.argtypes = [
        ctypes.POINTER(wintypes.ULONG), ctypes.POINTER(wintypes.ULONG),
        wintypes.DWORD, wintypes.ULONG,
    ]
    get_status.restype = wintypes.ULONG
    node = wintypes.DWORD()
    result = locate(ctypes.byref(node), ctypes.create_unicode_buffer(DEVICE_INSTANCE), 0)
    if result == 0x0D:  # CR_NO_SUCH_DEVNODE
        return DeviceStatus(False)
    if result:
        raise RuntimeError(f"查询设备失败：CM_Locate_DevNodeW 返回 0x{result:08X}")
    status, problem = wintypes.ULONG(), wintypes.ULONG()
    result = get_status(ctypes.byref(status), ctypes.byref(problem), node, 0)
    if result == 0x0D:  # The device may have been removed between the two queries.
        return DeviceStatus(False)
    if result:
        raise RuntimeError(f"查询设备状态失败：CM_Get_DevNode_Status 返回 0x{result:08X}")
    return DeviceStatus(True, bool(status.value & 0x08), problem.value)  # DN_STARTED


def find_driver_files(dll_path):
    """Prefer adjacent release files, otherwise use the selected DLL's build configuration."""
    dll = Path(dll_path).expanduser().resolve()
    inf = dll.parent / "cqkeys.inf"
    setup = dll.parent / "device_setup.exe"
    if (dll.parent.name in ("Release", "Debug") and
            dll.parent.parent.name == "x64" and
            dll.parent.parent.parent.name == "cqkeysdll"):
        root = dll.parents[3]
        configuration = dll.parent.name
        if not inf.is_file():
            inf = root / "cqkeys" / "x64" / configuration / "cqkeys" / "cqkeys.inf"
        if not setup.is_file():
            setup = root / "tools" / "x64" / configuration / "device_setup.exe"
    return inf if inf.is_file() else None, setup if setup.is_file() else None


def validate_package(inf_path, setup_path):
    if not str(inf_path).strip() or not str(setup_path).strip():
        raise ValueError("请选择 cqkeys.inf 和 device_setup.exe")
    inf = Path(inf_path).expanduser().resolve()
    setup = Path(setup_path).expanduser().resolve()
    if inf.name.lower() != "cqkeys.inf":
        raise ValueError("请选择新版 CQKeys 驱动包的 cqkeys.inf，不能直接安装 SYS 或旧版 lykeys.inf")
    if setup.name.lower() != "device_setup.exe":
        raise ValueError("请选择本项目构建的 device_setup.exe")
    for path in (inf, inf.parent / "cqkeys.sys", inf.parent / "cqkeys.cat", setup):
        if not path.is_file():
            raise FileNotFoundError(f"驱动包不完整，找不到：{path}")
    raw = inf.read_bytes()
    text = raw.decode("utf-16" if raw.startswith((b"\xff\xfe", b"\xfe\xff")) else "utf-8-sig")
    if not re.search(r"(?im)^[^;\r\n]*,\s*ROOT\\CQKEYSVHF\s*(?:;[^\r\n]*)?$", text):
        raise ValueError(r"INF 不包含新版设备 ROOT\CQKEYSVHF，请选择配套驱动包")
    return inf, setup


@dataclass(frozen=True)
class InstallResult:
    reboot_required: bool
    output: str


class InstallError(RuntimeError):
    def __init__(self, code, output):
        self.code = code & 0xFFFFFFFF
        self.output = output
        if self.code == 5:
            hint = "请以管理员身份启动 GUI。"
        elif self.code in (577, 1275, 0x800B0100, 0x800B0109, 0x80096010,
                           0xE000022F, 0xE0000247, 0xE000024B):
            hint = ("请检查完整驱动包的签名与证书信任；自签名驱动还需要测试签名环境。"
                    "GUI 不会自动修改 Secure Boot、测试签名或证书存储。")
        else:
            hint = r"请查看下方安装输出及 C:\Windows\INF\setupapi.dev.log。"
        super().__init__(
            f"驱动安装失败，错误码 {self.code}（0x{self.code:08X}）。\n{hint}\n\n{output.strip()}"
        )


def install_driver(inf_path, setup_path):
    inf, setup = validate_package(inf_path, setup_path)
    if not ctypes.windll.shell32.IsUserAnAdmin():
        raise PermissionError("安装驱动需要管理员权限，请以管理员身份重新启动 GUI")
    try:
        completed = subprocess.run(
            [str(setup), "install", str(inf)], cwd=str(inf.parent),
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
            creationflags=subprocess.CREATE_NO_WINDOW, timeout=120,
        )
    except subprocess.TimeoutExpired as error:
        raise RuntimeError(
            r"安装工具超过 120 秒未结束，安装结果未知。请刷新设备状态并查看 "
            r"C:\Windows\INF\setupapi.dev.log 后再操作。"
        ) from error
    if completed.returncode not in (0, 3010):
        raise InstallError(completed.returncode, completed.stdout)
    return InstallResult(completed.returncode == 3010, completed.stdout)


def describe_connection_error(error):
    code = getattr(error, "winerror", None) or getattr(error, "errno", None)
    hints = {
        2: r"找不到设备 \\.\cqkeys。请检查设备状态；尚未安装时点击“安装并连接”。DLL 已找到并不表示驱动已安装。",
        3: r"设备路径不可用，请检查 \\.\cqkeys 对应的驱动是否已安装并启动。",
        5: "请以管理员身份运行 GUI，并检查设备访问权限。",
        21: "驱动尚未就绪，请检查设备状态；如安装要求重启，请先重启系统。",
        32: "设备采用独占连接，请先断开其他 GUI、Python 脚本或 C# 客户端。",
        170: "设备正忙，请先关闭其他客户端。",
        126: "DLL 或其依赖文件缺失，请选择完整发布包中的 DLL。",
        193: "DLL 与 Python 的位数不匹配，请使用 64 位 Python 和 x64 DLL。",
        577: "Windows 无法验证驱动签名，请检查驱动包签名和目标系统的签名环境。",
        1306: "DLL 与已安装驱动的协议不匹配，请使用同一次构建的 DLL 和驱动包。",
    }
    if isinstance(error, AttributeError):
        hint = "DLL 缺少新版接口，请选择当前构建的 cqkeysdll.dll，不能混用旧版 DLL。"
    else:
        hint = hints.get(code, "")
    return f"{error}\n{hint}".strip()
