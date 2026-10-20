"""Run only against an installed VHF build in a disposable Windows VM.

No nonzero key/mouse input is sent. Closing a test connection submits neutral
reports. Run once elevated, then as a standard user with --expect-denied.
"""
import argparse
import ctypes as c
from ctypes import wintypes as w

parser = argparse.ArgumentParser()
parser.add_argument("--expect-denied", action="store_true")
args = parser.parse_args()
api = c.WinDLL("kernel32", use_last_error=True)
api.CreateFileW.argtypes = [w.LPCWSTR, w.DWORD, w.DWORD, c.c_void_p,
                           w.DWORD, w.DWORD, w.HANDLE]
api.CreateFileW.restype = w.HANDLE
api.CloseHandle.argtypes = [w.HANDLE]
api.CloseHandle.restype = w.BOOL
api.DeviceIoControl.argtypes = [w.HANDLE, w.DWORD, c.c_void_p, w.DWORD,
                               c.c_void_p, w.DWORD, c.POINTER(w.DWORD), c.c_void_p]
api.DeviceIoControl.restype = w.BOOL
INVALID = c.c_void_p(-1).value
DEVICE = r"\\.\cqkeys"
KEYBOARD, MOUSE, STATUS = 0x8000A000, 0x8000A004, 0x80006008


def open_device(name=DEVICE, access=0xC0000000):
    return api.CreateFileW(name, access, 0, None, 3, 0, None)


def ioctl(handle, code, data=None, output_size=0):
    raw = c.create_string_buffer(data) if data is not None else None
    output = c.create_string_buffer(output_size) if output_size else None
    returned = w.DWORD()
    c.set_last_error(0)
    ok = api.DeviceIoControl(handle, code, raw, len(data) if data is not None else 0,
                             output, output_size, c.byref(returned), None)
    return bool(ok), c.get_last_error(), output, returned.value


if args.expect_denied:
    for name in (DEVICE, DEVICE + r"\child"):
        handle = open_device(name)
        error = c.get_last_error()
        if handle != INVALID:
            api.CloseHandle(handle)
            raise AssertionError(f"Unexpected access to {name}")
        assert error == 5, f"Expected access denied, got {error}; check installation/other clients"
    print("standard-user ACL checks passed")
else:
    handle = open_device()
    if handle == INVALID:
        raise c.WinError(c.get_last_error())
    try:
        ok, error, output, returned = ioctl(handle, STATUS, output_size=24)
        assert ok and returned == 24, f"VHF status query failed: {error}"
        fields = (c.c_uint32 * 6).from_buffer_copy(output.raw)
        assert fields[0] == 24 and fields[1] == 2 and fields[2] & 2, list(fields)
        for code, size in ((KEYBOARD, 12), (MOUSE, 24)):
            for short in range(size):
                ok, error, _, _ = ioctl(handle, code, bytes(short))
                assert not ok and error != 0, (hex(code), short, error)
            # An oversized output buffer must not make short input valid.
            ok, error, _, _ = ioctl(handle, code, b"\0", output_size=64)
            assert not ok and error != 0
        assert not ioctl(handle, STATUS, output_size=23)[0]
        assert not ioctl(handle, 0x80002000, bytes(12))[0]  # Unsupported IOCTL.
    finally:
        assert api.CloseHandle(handle)
    handle = open_device(DEVICE + r"\child")
    error = c.get_last_error()
    if handle != INVALID:
        api.CloseHandle(handle)
        raise AssertionError("Namespace open was accepted")
    assert error == 123, f"Expected invalid-name rejection, got {error}"
    handle = open_device(access=0)
    if handle == INVALID:
        raise c.WinError(c.get_last_error())
    try:
        ok, error, _, _ = ioctl(handle, KEYBOARD, bytes(12))
        assert not ok and error == 5, f"Write-access IOCTL check failed: {error}"
    finally:
        assert api.CloseHandle(handle)
    print("elevated device contract checks passed")
