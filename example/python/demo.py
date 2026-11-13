"""CQKeys ABI v2 演示：连接、查询状态、可选输入测试和 PnP 会话清理。"""

import argparse
import ctypes
import math
import os
from pathlib import Path
import sys
import time


DEFAULT_DLL = Path(__file__).resolve().parents[2] / "cqkeysdll/x64/Release/cqkeysdll.dll"


class DriverCallError(RuntimeError):
    def __init__(self, operation, code):
        self.code = code & 0xffffffff
        if self.code == 0xE000020B:
            description = (
                "ERROR_NO_SUCH_DEVINST：找不到指定设备实例。"
                "当前版本需要 ROOT\\CQKEYS_VHF\\0000；"
                "请使用 --inf 指定完整且签名有效的驱动包安装并启用设备。"
            )
        else:
            description = ctypes.FormatError(ctypes.c_int32(self.code).value).strip() if self.code else "函数返回失败，但未设置错误码"
        super().__init__(f"{operation} 失败：Win32 {self.code} (0x{self.code:08X})，{description}")


def validate_package(path):
    inf = Path(path).expanduser().resolve(strict=True)
    if inf.name.lower() != "cqkeys.inf":
        raise ValueError("--inf 必须指向 cqkeys.inf，不能仅提供 SYS 文件")
    for suffix in (".inf", ".sys", ".cat"):
        member = inf.with_suffix(suffix)
        if not member.is_file():
            raise FileNotFoundError(f"驱动包缺少文件：{member}")
    return inf


def require_windows_admin():
    if os.name != "nt":
        raise RuntimeError("此演示仅支持 Windows")
    if ctypes.sizeof(ctypes.c_void_p) != 8:
        raise RuntimeError("请使用 64 位 Python 和 x64 cqkeysdll.dll")
    shell = ctypes.WinDLL("shell32", use_last_error=True)
    shell.IsUserAnAdmin.argtypes = []
    shell.IsUserAnAdmin.restype = ctypes.c_int
    if not shell.IsUserAnAdmin():
        raise PermissionError("请以管理员身份打开终端后运行")


class CqKeys:
    def __init__(self, dll_path):
        path = Path(dll_path).expanduser().resolve(strict=True)
        if not path.is_file():
            raise ValueError(f"DLL 路径不是文件：{path}")
        self.native = ctypes.CDLL(str(path), use_last_error=True)
        self.needs_cleanup = False
        signatures = {
            "GetLifecycleVersion": ([], ctypes.c_uint32),
            "LoadNTDriver": ([ctypes.c_char_p, ctypes.c_char_p], ctypes.c_int),
            "SetHandle": ([], ctypes.c_int),
            "CheckDeviceStatus": ([], None),
            "GetDriverStatus": ([], ctypes.c_int),
            "StopNTDriver": ([ctypes.c_char_p], ctypes.c_int),
            "UnloadNTDriver": ([ctypes.c_char_p], ctypes.c_int),
            "KeyDown": ([ctypes.c_uint16], None),
            "KeyUp": ([ctypes.c_uint16], None),
            "ConsumerKeyDown": ([ctypes.c_uint16], None),
            "ConsumerKeyUp": ([ctypes.c_uint16], None),
            "MouseMoveRELATIVE": ([ctypes.c_int32, ctypes.c_int32], None),
        }
        for name, (arguments, result) in signatures.items():
            function = getattr(self.native, name)
            function.argtypes = arguments
            function.restype = result
        version = self.native.GetLifecycleVersion()
        if version != 2:
            raise RuntimeError(f"需要 ABI v2 DLL，实际版本为 {version}")

    def call(self, name, *arguments, boolean=False):
        ctypes.set_last_error(0)
        result = getattr(self.native, name)(*arguments)
        error = ctypes.get_last_error()
        if error or (boolean and not result):
            raise DriverCallError(name, error)
        return result

    def connect(self, inf_path=None):
        if self.needs_cleanup:
            raise RuntimeError("请先清理当前会话")
        encoded_path = None
        if inf_path is not None:
            encoded_path = str(validate_package(inf_path)).encode("mbcs", errors="strict")
        self.needs_cleanup = True
        if encoded_path is not None:
            self.call("LoadNTDriver", b"cqkeys", encoded_path, boolean=True)
        self.call("SetHandle", boolean=True)
        self.call("CheckDeviceStatus")
        status = self.native.GetDriverStatus()
        if status != 1:
            raise RuntimeError(f"设备未就绪，状态码：{status}")
        return status

    def tap(self, usage, consumer=False):
        if not 1 <= usage <= (0x3ff if consumer else 0xff):
            raise ValueError("按键代码超出范围")
        prefix = "ConsumerKey" if consumer else "Key"
        self.call(prefix + "Down", usage)
        try:
            time.sleep(0.05)
        finally:
            self.call(prefix + "Up", usage)

    def move(self, delta_x, delta_y):
        if not all(-32767 <= delta <= 32767 for delta in (delta_x, delta_y)):
            raise ValueError("演示的单次相对位移范围为 -32767 到 32767")
        self.call("MouseMoveRELATIVE", delta_x, delta_y)

    def close(self, uninstall=False):
        if self.needs_cleanup:
            operation = "UnloadNTDriver" if uninstall else "StopNTDriver"
            self.call(operation, b"cqkeys", boolean=True)
            self.needs_cleanup = False


def integer(value):
    try:
        return int(value, 0)
    except ValueError as error:
        raise argparse.ArgumentTypeError("请输入十进制或 0x 开头的十六进制整数") from error


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dll", type=Path, default=DEFAULT_DLL, help="x64 ABI v2 DLL 路径")
    parser.add_argument("--inf", type=Path, help="安装并启用该完整驱动包；省略时连接已安装且已启用的设备")
    action = parser.add_mutually_exclusive_group()
    action.add_argument("--tap-key", type=integer, help="按下并释放一个 Windows VK，例如 0x41 表示 A")
    action.add_argument("--move", type=integer, nargs=2, metavar=("X", "Y"), help="发送一次相对鼠标移动")
    action.add_argument("--consumer", type=integer, help="按下并释放媒体 usage，例如 0xCD 表示播放/暂停")
    parser.add_argument("--delay", type=float, default=3.0, help="发送输入前的等待秒数，默认 3 秒")
    parser.add_argument("--uninstall", action="store_true", help="退出时移除 PnP 节点；默认禁用节点，均保留驱动包")
    args = parser.parse_args(argv)
    if not math.isfinite(args.delay) or not 0 <= args.delay <= 60:
        parser.error("--delay 必须在 0 到 60 秒之间")
    if args.tap_key is not None and not 1 <= args.tap_key <= 0xff:
        parser.error("--tap-key 必须在 1 到 0xFF 之间")
    if args.consumer is not None and not 1 <= args.consumer <= 0x3ff:
        parser.error("--consumer 必须在 1 到 0x3FF 之间")
    if args.move is not None and not all(-32767 <= delta <= 32767 for delta in args.move):
        parser.error("--move 每轴必须在 -32767 到 32767 之间")
    return args


def main(argv=None):
    args = parse_args(argv)
    client = None
    exit_code = 0
    try:
        require_windows_admin()
        client = CqKeys(args.dll)
        status = client.connect(args.inf)
        print(f"设备已连接，ABI=2，状态={status}（READY）")
        if args.tap_key is not None or args.move is not None or args.consumer is not None:
            print(f"将在 {args.delay:g} 秒后发送输入，请将焦点放到测试窗口。")
            time.sleep(args.delay)
            if args.tap_key is not None:
                client.tap(args.tap_key)
            elif args.move is not None:
                client.move(*args.move)
            else:
                client.tap(args.consumer, consumer=True)
            print("输入请求已成功提交；是否被目标程序接收需现场确认。")
        else:
            print("状态检查完成，未发送测试按键、鼠标移动或媒体键。")
    except KeyboardInterrupt:
        print("测试已取消。", file=sys.stderr)
        exit_code = 130
    except (OSError, RuntimeError, ValueError, AttributeError) as error:
        print(f"测试失败：{error}", file=sys.stderr)
        if isinstance(error, DriverCallError) and error.code == 3010:
            print("系统要求重启，请保存工作后手动处理；脚本不会自动重启。", file=sys.stderr)
        exit_code = 1
    finally:
        if client is not None and client.needs_cleanup:
            try:
                client.close(uninstall=args.uninstall)
                if exit_code == 0:
                    print("管理会话已清理，Driver Store 中的驱动包保留。")
                else:
                    print("已清理本次管理会话；驱动测试仍然失败。")
            except (OSError, RuntimeError) as error:
                print(f"清理失败：{error}；请检查 PnP 设备状态，不能视为已完成清理。", file=sys.stderr)
                exit_code = 1
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
