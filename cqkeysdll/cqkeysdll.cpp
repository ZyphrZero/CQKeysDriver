#include "pch.h"
#include "cqkeys.h"
#include "pnp_device.h"
#include "../shared/cqkeys_protocol.h"
#include <mutex>
#include <atomic>
#include <cstring>

namespace {
    std::mutex g_lifecycleMutex;
    std::mutex g_deviceMutex;
    HANDLE g_device = INVALID_HANDLE_VALUE;
    bool g_acceptingInput = false;
    bool g_ownsSession = false;
    CqKeyboardReport g_keyboard{CQKEYS_KEYBOARD_ID};
    uint8_t g_buttons = 0;
    uint16_t g_consumer = 0;
    uint32_t g_generation = 0;
    std::atomic<DEVICE_STATUS> g_status{DEVICE_STATUS_UNKNOWN};
    std::atomic<DWORD> g_error{ERROR_SUCCESS};
    std::atomic<ULONGLONG> g_lastCheck{0};

    BOOL Fail(DWORD error) {
        g_error = error;
        SetLastError(error);
        return FALSE;
    }

    BOOL Success() { g_error = ERROR_SUCCESS; SetLastError(ERROR_SUCCESS); return TRUE; }

    bool ValidateName(const char* name) {
        if (!name || _stricmp(name, "cqkeys") != 0) { Fail(ERROR_INVALID_NAME); return false; }
        return true;
    }

    void ClearInputState() {
        g_keyboard = {CQKEYS_KEYBOARD_ID};
        g_buttons = 0;
        g_consumer = 0;
    }

    BOOL ResetInput() {
        if (g_device == INVALID_HANDLE_VALUE) return Success();
        DWORD returned = 0;
        if (!DeviceIoControl(g_device, IOCTL_CQKEYS_RESET, nullptr, 0, nullptr, 0, &returned, nullptr))
            return Fail(GetLastError());
        ClearInputState();
        return Success();
    }

    BOOL CloseDevice() {
        g_acceptingInput = false;
        g_status = DEVICE_STATUS_UNKNOWN;
        if (!ResetInput()) {
            DWORD error = GetLastError();
            if (error == ERROR_DEVICE_NOT_CONNECTED || error == ERROR_NO_SUCH_DEVICE) {
                if (!CloseHandle(g_device)) return Fail(GetLastError());
                g_device = INVALID_HANDLE_VALUE;
                ClearInputState();
            }
            return Fail(error);
        }
        if (g_device != INVALID_HANDLE_VALUE) {
            if (!CloseHandle(g_device)) return Fail(GetLastError());
            g_device = INVALID_HANDLE_VALUE;
        }
        return Success();
    }

    BOOL QueryStatus() {
        FILETIME time;
        GetSystemTimeAsFileTime(&time);
        ULARGE_INTEGER ticks;
        ticks.LowPart = time.dwLowDateTime;
        ticks.HighPart = time.dwHighDateTime;
        g_lastCheck = (ticks.QuadPart - 116444736000000000ull) / 10000;
        CqDeviceStatus status{};
        DWORD returned = 0;
        g_status = DEVICE_STATUS_ERROR;
        if (!DeviceIoControl(g_device, IOCTL_CQKEYS_STATUS, nullptr, 0, &status, sizeof(status), &returned, nullptr))
            return Fail(GetLastError());
        if (returned != sizeof(status) || status.size != sizeof(status) || status.version != CQKEYS_PROTOCOL_VERSION)
            return Fail(ERROR_REVISION_MISMATCH);
        if (status.ready != 1) return Fail(ERROR_NOT_READY);
        if (g_acceptingInput && status.generation != g_generation) {
            g_acceptingInput = false;
            return Fail(ERROR_DEVICE_NOT_CONNECTED);
        }
        g_generation = status.generation;
        g_status = DEVICE_STATUS_READY;
        return Success();
    }

    template<typename Report> BOOL SendReport(const Report& report) {
        if (!g_acceptingInput || g_device == INVALID_HANDLE_VALUE) return Fail(ERROR_NOT_READY);
        CqReportRequest request{};
        request.size = sizeof(request);
        request.version = CQKEYS_PROTOCOL_VERSION;
        request.generation = g_generation;
        request.reportLength = sizeof(report);
        memcpy(request.report, &report, sizeof(report));
        if (!CqValidReport(request)) return Fail(ERROR_INVALID_PARAMETER);
        DWORD returned = 0;
        if (!DeviceIoControl(g_device, IOCTL_CQKEYS_REPORT, &request, sizeof(request), nullptr, 0, &returned, nullptr)) {
            DWORD error = GetLastError();
            if (error == ERROR_DEVICE_NOT_CONNECTED || error == ERROR_NO_SUCH_DEVICE) {
                g_acceptingInput = false;
                g_status = DEVICE_STATUS_ERROR;
            }
            return Fail(error);
        }
        return Success();
    }

    uint8_t KeyboardUsage(USHORT key) {
        if (key >= 'A' && key <= 'Z') return static_cast<uint8_t>(4 + key - 'A');
        if (key >= '1' && key <= '9') return static_cast<uint8_t>(0x1e + key - '1');
        if (key >= VK_F1 && key <= VK_F12) return static_cast<uint8_t>(0x3a + key - VK_F1);
        if (key >= VK_F13 && key <= VK_F24) return static_cast<uint8_t>(0x68 + key - VK_F13);
        if (key >= VK_NUMPAD1 && key <= VK_NUMPAD9) return static_cast<uint8_t>(0x59 + key - VK_NUMPAD1);
        switch (key) {
        case '0': return 0x27;
        case VK_RETURN: return 0x28;
        case VK_ESCAPE: return 0x29;
        case VK_BACK: return 0x2a;
        case VK_TAB: return 0x2b;
        case VK_SPACE: return 0x2c;
        case VK_OEM_MINUS: return 0x2d;
        case VK_OEM_PLUS: return 0x2e;
        case VK_OEM_4: return 0x2f;
        case VK_OEM_6: return 0x30;
        case VK_OEM_5: return 0x31;
        case VK_OEM_1: return 0x33;
        case VK_OEM_7: return 0x34;
        case VK_OEM_3: return 0x35;
        case VK_OEM_COMMA: return 0x36;
        case VK_OEM_PERIOD: return 0x37;
        case VK_OEM_2: return 0x38;
        case VK_CAPITAL: return 0x39;
        case VK_SNAPSHOT: return 0x46;
        case VK_SCROLL: return 0x47;
        case VK_PAUSE: return 0x48;
        case VK_INSERT: return 0x49;
        case VK_HOME: return 0x4a;
        case VK_PRIOR: return 0x4b;
        case VK_DELETE: return 0x4c;
        case VK_END: return 0x4d;
        case VK_NEXT: return 0x4e;
        case VK_RIGHT: return 0x4f;
        case VK_LEFT: return 0x50;
        case VK_DOWN: return 0x51;
        case VK_UP: return 0x52;
        case VK_NUMLOCK: return 0x53;
        case VK_DIVIDE: return 0x54;
        case VK_MULTIPLY: return 0x55;
        case VK_SUBTRACT: return 0x56;
        case VK_ADD: return 0x57;
        case VK_NUMPAD0: return 0x62;
        case VK_DECIMAL: return 0x63;
        case VK_OEM_102: return 0x64;
        case VK_APPS: return 0x65;
        case VK_CONTROL: case VK_LCONTROL: return 0xe0;
        case VK_SHIFT: case VK_LSHIFT: return 0xe1;
        case VK_MENU: case VK_LMENU: return 0xe2;
        case VK_LWIN: return 0xe3;
        case VK_RCONTROL: return 0xe4;
        case VK_RSHIFT: return 0xe5;
        case VK_RMENU: return 0xe6;
        case VK_RWIN: return 0xe7;
        default: return 0;
        }
    }

    uint16_t ConsumerUsage(USHORT key) {
        switch (key) {
        case VK_VOLUME_MUTE: return 0xe2;
        case VK_VOLUME_DOWN: return 0xea;
        case VK_VOLUME_UP: return 0xe9;
        case VK_MEDIA_NEXT_TRACK: return 0xb5;
        case VK_MEDIA_PREV_TRACK: return 0xb6;
        case VK_MEDIA_STOP: return 0xb7;
        case VK_MEDIA_PLAY_PAUSE: return 0xcd;
        default: return 0;
        }
    }

    void ConsumerInput(USHORT usage, bool down) {
        if (usage == 0 || usage > 0x3ff) { Fail(ERROR_INVALID_PARAMETER); return; }
        if (down && g_consumer && g_consumer != usage) { Fail(ERROR_BUSY); return; }
        if (!down && g_consumer != usage && g_consumer != 0) { Fail(ERROR_INVALID_PARAMETER); return; }
        CqConsumerReport report{CQKEYS_CONSUMER_ID, down ? usage : static_cast<uint16_t>(0)};
        if (SendReport(report)) g_consumer = report.usage;
    }

    void KeyboardInput(USHORT virtualKey, bool down) {
        std::lock_guard<std::mutex> lock(g_deviceMutex);
        if (auto consumer = ConsumerUsage(virtualKey)) { ConsumerInput(consumer, down); return; }
        auto usage = KeyboardUsage(virtualKey);
        if (!usage) { Fail(ERROR_NOT_SUPPORTED); return; }
        auto report = g_keyboard;
        if (usage >= 0xe0) {
            auto mask = static_cast<uint8_t>(1u << (usage - 0xe0));
            if (down) report.modifiers |= mask;
            else report.modifiers &= static_cast<uint8_t>(~mask);
        } else {
            auto found = std::find(std::begin(report.keys), std::end(report.keys), usage);
            if (down && found == std::end(report.keys)) {
                auto empty = std::find(std::begin(report.keys), std::end(report.keys), 0);
                if (empty == std::end(report.keys)) { Fail(ERROR_TOO_MANY_CMDS); return; }
                *empty = usage;
            } else if (!down && found != std::end(report.keys)) *found = 0;
        }
        if (SendReport(report)) g_keyboard = report;
    }

    void MouseButton(uint8_t mask, bool down) {
        std::lock_guard<std::mutex> lock(g_deviceMutex);
        CqMouseReport report{CQKEYS_MOUSE_ID};
        report.buttons = down ? g_buttons | mask : g_buttons & static_cast<uint8_t>(~mask);
        if (SendReport(report)) g_buttons = report.buttons;
    }

    void MouseWheel(USHORT delta, bool down) {
        std::lock_guard<std::mutex> lock(g_deviceMutex);
        if (delta % WHEEL_DELTA != 0 || delta / WHEEL_DELTA > 127) { Fail(ERROR_INVALID_PARAMETER); return; }
        CqMouseReport report{CQKEYS_MOUSE_ID, g_buttons};
        report.wheel = static_cast<int8_t>((down ? -1 : 1) * (delta / WHEEL_DELTA));
        SendReport(report);
    }

    BOOL StopDriver(const char* name, bool remove) {
        std::lock_guard<std::mutex> lifecycle(g_lifecycleMutex);
        if (!ValidateName(name)) return FALSE;
        std::lock_guard<std::mutex> device(g_deviceMutex);
        if (!g_ownsSession) return Success();
        if (!CloseDevice()) return FALSE;
        if (!CqChangeDevice(remove)) return Fail(GetLastError());
        CqReleaseSession();
        g_ownsSession = false;
        return Success();
    }
}

DWORD __cdecl GetLifecycleVersion() { return CQKEYS_PROTOCOL_VERSION; }
BOOL __cdecl LoadNTDriver(const char* name, const char* path) {
    std::lock_guard<std::mutex> lifecycle(g_lifecycleMutex);
    if (!ValidateName(name)) return FALSE;
    std::lock_guard<std::mutex> device(g_deviceMutex);
    if (g_device != INVALID_HANDLE_VALUE) return Fail(ERROR_BUSY);
    if (!CqAcquireSession()) return Fail(GetLastError());
    g_ownsSession = true;
    if (!CqInstallDevice(path)) return Fail(GetLastError());
    return Success();
}
BOOL __cdecl StopNTDriver(const char* name) { return StopDriver(name, false); }
BOOL __cdecl UnloadNTDriver(const char* name) { return StopDriver(name, true); }
BOOL __cdecl SetHandle() {
    std::lock_guard<std::mutex> lifecycle(g_lifecycleMutex);
    std::lock_guard<std::mutex> device(g_deviceMutex);
    if (g_device != INVALID_HANDLE_VALUE) return g_acceptingInput ? Success() : Fail(ERROR_BUSY);
    if (!CqAcquireSession()) return Fail(GetLastError());
    g_ownsSession = true;
    g_device = CqOpenInterface();
    if (g_device == INVALID_HANDLE_VALUE) return Fail(GetLastError());
    if (!QueryStatus()) {
        DWORD error = GetLastError();
        if (!CloseHandle(g_device)) return Fail(GetLastError());
        g_device = INVALID_HANDLE_VALUE;
        return Fail(error);
    }
    ClearInputState();
    g_acceptingInput = true;
    return Success();
}
BOOL __cdecl StopInputProcessing() {
    std::lock_guard<std::mutex> lifecycle(g_lifecycleMutex);
    std::lock_guard<std::mutex> device(g_deviceMutex);
    g_acceptingInput = false;
    g_status = DEVICE_STATUS_UNKNOWN;
    return ResetInput();
}
void __cdecl CheckDeviceStatus() { std::lock_guard<std::mutex> lock(g_deviceMutex); QueryStatus(); }
DEVICE_STATUS __cdecl GetDriverStatus() { return g_status.load(); }
ULONGLONG __cdecl GetLastCheckTime() { return g_lastCheck.load(); }
HANDLE __cdecl GetDriverHandle() { std::lock_guard<std::mutex> lock(g_deviceMutex); return g_device; }
int __cdecl GetDetailedErrorCode() { return static_cast<int>(g_error.load()); }
void __cdecl KeyDown(USHORT key) { KeyboardInput(key, true); }
void __cdecl KeyUp(USHORT key) { KeyboardInput(key, false); }
void __cdecl ConsumerKeyDown(USHORT usage) { std::lock_guard<std::mutex> lock(g_deviceMutex); ConsumerInput(usage, true); }
void __cdecl ConsumerKeyUp(USHORT usage) { std::lock_guard<std::mutex> lock(g_deviceMutex); ConsumerInput(usage, false); }
void __cdecl MouseLeftButtonDown() { MouseButton(1, true); }
void __cdecl MouseLeftButtonUp() { MouseButton(1, false); }
void __cdecl MouseRightButtonDown() { MouseButton(2, true); }
void __cdecl MouseRightButtonUp() { MouseButton(2, false); }
void __cdecl MouseMiddleButtonDown() { MouseButton(4, true); }
void __cdecl MouseMiddleButtonUp() { MouseButton(4, false); }
void __cdecl MouseXButton1Down() { MouseButton(8, true); }
void __cdecl MouseXButton1Up() { MouseButton(8, false); }
void __cdecl MouseXButton2Down() { MouseButton(16, true); }
void __cdecl MouseXButton2Up() { MouseButton(16, false); }
void __cdecl MouseWheelUp(USHORT delta) { MouseWheel(delta, false); }
void __cdecl MouseWheelDown(USHORT delta) { MouseWheel(delta, true); }

void __cdecl MouseMoveRELATIVE(LONG deltaX, LONG deltaY) {
    std::lock_guard<std::mutex> lock(g_deviceMutex);
    do {
        auto stepX = static_cast<int16_t>((std::max)(-32767L, (std::min)(32767L, deltaX)));
        auto stepY = static_cast<int16_t>((std::max)(-32767L, (std::min)(32767L, deltaY)));
        CqMouseReport report{CQKEYS_MOUSE_ID, g_buttons, stepX, stepY, 0};
        if (!SendReport(report)) return;
        deltaX -= stepX;
        deltaY -= stepY;
    } while (deltaX != 0 || deltaY != 0);
}

void __cdecl MouseMoveABSOLUTE(LONG screenX, LONG screenY) {
    std::lock_guard<std::mutex> lock(g_deviceMutex);
    LONG width = GetSystemMetrics(SM_CXSCREEN), height = GetSystemMetrics(SM_CYSCREEN);
    if (width <= 1 || height <= 1 || screenX < 0 || screenY < 0 || screenX >= width || screenY >= height) {
        Fail(ERROR_INVALID_PARAMETER);
        return;
    }
    CqAbsoluteReport report{CQKEYS_ABSOLUTE_ID,
        static_cast<uint16_t>(static_cast<int64_t>(screenX) * 32767 / (width - 1)),
        static_cast<uint16_t>(static_cast<int64_t>(screenY) * 32767 / (height - 1))};
    SendReport(report);
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }
