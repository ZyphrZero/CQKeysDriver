"""Exercise the real DLL's ABI and failure reporting without opening a device."""
import ctypes as c
import pathlib
import sys

path = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else (
    pathlib.Path(__file__).resolve().parents[1] / "cqkeysdll/x64/Debug/cqkeysdll.dll"
)
dll = c.CDLL(str(path.resolve()), use_last_error=True)
dll.GetProtocolVersion.restype = c.c_uint32
dll.GetDetailedErrorCode.restype = c.c_int
assert dll.GetProtocolVersion() == 2
assert dll.GetDriverStatus() == 0

for name in ("ConnectDevice", "DisconnectDevice", "ResetInput",
             "CheckDeviceStatus", "GetDriverStatus", "GetLastCheckTime"):
    getattr(dll, name)  # Verify exports without connecting/installing.

for name in ("KeyDown", "KeyUp", "MouseWheelUp", "MouseWheelDown"):
    function = getattr(dll, name)
    function.argtypes = [c.c_ushort]
    function.restype = c.c_int
    c.set_last_error(0)
    assert not function(65535)
    assert c.get_last_error() == 87, (name, c.get_last_error())
    assert dll.GetDetailedErrorCode() == 87

dll.MouseMoveABSOLUTE.argtypes = [c.c_int32, c.c_int32]
dll.MouseMoveABSOLUTE.restype = c.c_int
assert not dll.MouseMoveABSOLUTE(-1, 0)
assert c.get_last_error() == 87

# No device was opened. Every ordinary input call must report invalid-handle.
for name in ("MouseLeftButtonDown", "MouseLeftButtonUp",
             "MouseRightButtonDown", "MouseRightButtonUp",
             "MouseMiddleButtonDown", "MouseMiddleButtonUp",
             "MouseXButton1Down", "MouseXButton1Up",
             "MouseXButton2Down", "MouseXButton2Up"):
    function = getattr(dll, name)
    function.argtypes = []
    function.restype = c.c_int
    assert not function()
    assert c.get_last_error() == 6, (name, c.get_last_error())

dll.DisconnectDevice.argtypes = []
dll.DisconnectDevice.restype = c.c_int
assert dll.DisconnectDevice()
assert dll.GetDriverStatus() == 0
print("DLL ABI and failure-reporting tests passed; no device opened")
