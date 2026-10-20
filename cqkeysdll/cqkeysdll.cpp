#include "pch.h"
#include "cqkeys.h"
#include <atomic>

namespace {
SRWLOCK g_Lock = SRWLOCK_INIT;
HANDLE g_Device = INVALID_HANDLE_VALUE;
std::atomic<int> g_Status{0};
std::atomic<DWORD> g_Error{0};
std::atomic<ULONGLONG> g_LastCheck{0};

struct Guard {
    Guard() { AcquireSRWLockExclusive(&g_Lock); }
    ~Guard() { ReleaseSRWLockExclusive(&g_Lock); }
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
};

BOOL Result(BOOL success, DWORD error = ERROR_SUCCESS)
{
    g_Error = error;
    SetLastError(error);
    return success;
}

BOOL QueryLocked()
{
    FILETIME time;
    GetSystemTimeAsFileTime(&time);
    ULARGE_INTEGER value;
    value.LowPart = time.dwLowDateTime;
    value.HighPart = time.dwHighDateTime;
    g_LastCheck = (value.QuadPart - 116444736000000000ull) / 10000;
    CQ_DRIVER_STATUS status = {};
    DWORD returned = 0;
    if (!DeviceIoControl(g_Device, IOCTL_CQ_STATUS, nullptr, 0,
                         &status, sizeof(status), &returned, nullptr)) {
        const DWORD error = GetLastError();
        g_Status = 2;
        return Result(FALSE, error);
    }
    if (returned != sizeof(status) || status.Size != sizeof(status) ||
        status.Version != CQ_PROTOCOL_VERSION) {
        g_Status = 2;
        return Result(FALSE, ERROR_REVISION_MISMATCH);
    }
    if (!(status.Flags & CQ_STATUS_READY)) {
        g_Status = 2;
        return Result(FALSE, ERROR_NOT_READY);
    }
    g_Status = 1;
    return Result(TRUE);
}

BOOL Send(ULONG code, const void* input, DWORD length)
{
    Guard guard;
    DWORD returned = 0;
    const BOOL success = DeviceIoControl(g_Device, code, const_cast<void*>(input),
                                         length, nullptr, 0, &returned, nullptr);
    return Result(success, success ? ERROR_SUCCESS : GetLastError());
}

BOOL SendKey(USHORT key, bool released)
{
    CQ_KEYBOARD_INPUT input = {};
    if (key > 0xff) return Result(FALSE, ERROR_INVALID_PARAMETER);
    const UINT scan = MapVirtualKeyW(key, MAPVK_VK_TO_VSC_EX);
    input.MakeCode = static_cast<USHORT>(scan & 0xff);
    input.Flags = released ? KEY_BREAK : KEY_MAKE;
    if ((scan & 0xff00) == 0xe000) input.Flags |= KEY_E0;
    if ((scan & 0xff00) == 0xe100) input.Flags |= KEY_E1;
    if (key == VK_SNAPSHOT) { input.MakeCode = 0x37; input.Flags |= KEY_E0; }
    if (key == VK_PAUSE) { input.MakeCode = 0x45; input.Flags |= KEY_E1; }
    if (key == VK_NUMLOCK) input.Flags &= ~KEY_E0;
    if (!input.MakeCode) return Result(FALSE, ERROR_NOT_SUPPORTED);
    return Send(IOCTL_KEYBOARD, &input, sizeof(input));
}

BOOL SendButton(USHORT flags)
{
    CQ_MOUSE_INPUT input = {};
    input.ButtonFlags = flags;
    return Send(IOCTL_MOUSE, &input, sizeof(input));
}
}

BOOL __cdecl ConnectDevice()
{
    Guard guard;
    if (g_Device != INVALID_HANDLE_VALUE) return QueryLocked();
    g_Device = CreateFileW(KEYMOUSE_WIN32_DEVICE_NAME, GENERIC_READ | GENERIC_WRITE,
                           0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (g_Device == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        g_Status = 5;
        return Result(FALSE, error);
    }
    if (!QueryLocked()) {
        const DWORD error = g_Error.load();
        CloseHandle(g_Device);
        g_Device = INVALID_HANDLE_VALUE;
        return Result(FALSE, error);
    }
    return TRUE;
}

BOOL __cdecl DisconnectDevice()
{
    Guard guard;
    BOOL success = TRUE;
    if (g_Device != INVALID_HANDLE_VALUE) success = CloseHandle(g_Device);
    const DWORD error = success ? ERROR_SUCCESS : GetLastError();
    if (success) g_Device = INVALID_HANDLE_VALUE;
    g_Status = success ? 0 : 2;
    return Result(success, error);
}

BOOL __cdecl ResetInput() { return Send(IOCTL_CQ_RESET, nullptr, 0); }
ULONG __cdecl GetProtocolVersion() { return CQ_PROTOCOL_VERSION; }
BOOL __cdecl CheckDeviceStatus() { Guard guard; return QueryLocked(); }
int __cdecl GetDriverStatus() { return g_Status.load(); }
ULONGLONG __cdecl GetLastCheckTime() { return g_LastCheck.load(); }
int __cdecl GetDetailedErrorCode() { return static_cast<int>(g_Error.load()); }
BOOL __cdecl KeyDown(USHORT key) { return SendKey(key, false); }
BOOL __cdecl KeyUp(USHORT key) { return SendKey(key, true); }
BOOL __cdecl MouseLeftButtonDown() { return SendButton(MOUSE_LEFT_BUTTON_DOWN); }
BOOL __cdecl MouseLeftButtonUp() { return SendButton(MOUSE_LEFT_BUTTON_UP); }
BOOL __cdecl MouseRightButtonDown() { return SendButton(MOUSE_RIGHT_BUTTON_DOWN); }
BOOL __cdecl MouseRightButtonUp() { return SendButton(MOUSE_RIGHT_BUTTON_UP); }
BOOL __cdecl MouseMiddleButtonDown() { return SendButton(MOUSE_MIDDLE_BUTTON_DOWN); }
BOOL __cdecl MouseMiddleButtonUp() { return SendButton(MOUSE_MIDDLE_BUTTON_UP); }
BOOL __cdecl MouseXButton1Down() { return SendButton(MOUSE_BUTTON_4_DOWN); }
BOOL __cdecl MouseXButton1Up() { return SendButton(MOUSE_BUTTON_4_UP); }
BOOL __cdecl MouseXButton2Down() { return SendButton(MOUSE_BUTTON_5_DOWN); }
BOOL __cdecl MouseXButton2Up() { return SendButton(MOUSE_BUTTON_5_UP); }

BOOL __cdecl MouseMoveRELATIVE(LONG x, LONG y)
{
    CQ_MOUSE_INPUT input = {};
    input.LastX = x;
    input.LastY = y;
    return Send(IOCTL_MOUSE, &input, sizeof(input));
}

BOOL __cdecl MouseMoveABSOLUTE(LONG x, LONG y)
{
    const LONG width = GetSystemMetrics(SM_CXSCREEN);
    const LONG height = GetSystemMetrics(SM_CYSCREEN);
    if (width <= 1 || height <= 1 || x < 0 || x >= width || y < 0 || y >= height) {
        return Result(FALSE, ERROR_INVALID_PARAMETER);
    }
    CQ_MOUSE_INPUT input = {};
    input.Flags = MOUSE_MOVE_ABSOLUTE;
    input.LastX = static_cast<LONG>((static_cast<LONGLONG>(x) * 65535) / (width - 1));
    input.LastY = static_cast<LONG>((static_cast<LONGLONG>(y) * 65535) / (height - 1));
    return Send(IOCTL_MOUSE, &input, sizeof(input));
}

BOOL __cdecl MouseWheelUp(USHORT delta)
{
    if (delta > 32767) return Result(FALSE, ERROR_INVALID_PARAMETER);
    CQ_MOUSE_INPUT input = {};
    input.ButtonFlags = MOUSE_WHEEL;
    input.ButtonData = delta;
    return Send(IOCTL_MOUSE, &input, sizeof(input));
}

BOOL __cdecl MouseWheelDown(USHORT delta)
{
    if (delta > 32767) return Result(FALSE, ERROR_INVALID_PARAMETER);
    CQ_MOUSE_INPUT input = {};
    input.ButtonFlags = MOUSE_WHEEL;
    input.ButtonData = static_cast<USHORT>(-static_cast<SHORT>(delta));
    return Send(IOCTL_MOUSE, &input, sizeof(input));
}

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID reserved)
{
    // Do not wait on locks or perform installation/I/O under the loader lock.
    // Process termination closes handles; an explicit FreeLibrary closes ours.
    if (reason == DLL_PROCESS_DETACH && !reserved && g_Device != INVALID_HANDLE_VALUE)
        CloseHandle(g_Device);
    return TRUE;
}
