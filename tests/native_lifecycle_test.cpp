#include "../cqkeysdll/pch.h"
#include "../cqkeysdll/cqkeys.h"
#include "../cqkeysdll/pnp_device.h"
#include "../shared/cqkeys_protocol.h"
#include "../shared/cqkeys_reports.h"
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <chrono>
#include <vector>
#include <iostream>
#include <cstring>

namespace fake {
    HANDLE device = reinterpret_cast<HANDLE>(3);
    DWORD installError = 0, changeError = 0, openError = 0, resetError = 0;
    uint32_t version = CQKEYS_PROTOCOL_VERSION, generation = 1;
    DWORD statusBytes = sizeof(CqDeviceStatus);
    int resets = 0, changes = 0, installs = 0, reports = 0, failReportAt = 0;
    std::atomic<int> closes{0};
    bool failReset = false, failClose = false, leaseBusy = false, lease = false, removed = false;
    std::vector<CqReportRequest> submitted;
    std::mutex ioMutex;
    std::condition_variable ioCondition;
    bool blockIo = false, enteredIo = false, releaseIo = false;

    BOOL error(DWORD code) { SetLastError(code); return FALSE; }
    void reset() {
        installError = changeError = openError = resetError = 0;
        version = CQKEYS_PROTOCOL_VERSION;
        generation = 1;
        statusBytes = sizeof(CqDeviceStatus);
        resets = changes = installs = reports = failReportAt = 0;
        closes = 0;
        failReset = failClose = leaseBusy = lease = removed = false;
        submitted.clear();
        blockIo = enteredIo = releaseIo = false;
    }
    BOOL WINAPI closeDevice(HANDLE handle) {
        if (handle != device) return error(ERROR_INVALID_HANDLE);
        if (failClose) return error(ERROR_INVALID_HANDLE);
        ++closes;
        return TRUE;
    }
    int WINAPI metrics(int index) { return index == SM_CXSCREEN ? 1920 : 1080; }
    BOOL WINAPI io(HANDLE handle, DWORD code, LPVOID input, DWORD size, LPVOID output,
                  DWORD outputSize, LPDWORD returned, LPOVERLAPPED) {
        *returned = 0;
        if (handle != device) return error(ERROR_INVALID_HANDLE);
        if (code == IOCTL_CQKEYS_STATUS) {
            if (input || size || outputSize != sizeof(CqDeviceStatus)) return error(ERROR_INVALID_PARAMETER);
            *static_cast<CqDeviceStatus*>(output) = {sizeof(CqDeviceStatus), version, generation, 1, 0};
            *returned = statusBytes;
            return TRUE;
        }
        if (code == IOCTL_CQKEYS_RESET) {
            ++resets;
            if (resetError) return error(resetError);
            return failReset ? error(ERROR_WRITE_FAULT) : TRUE;
        }
        if (code != IOCTL_CQKEYS_REPORT || size != sizeof(CqReportRequest)) return error(ERROR_INVALID_PARAMETER);
        const auto& request = *static_cast<CqReportRequest*>(input);
        if (!CqValidReport(request)) return error(ERROR_INVALID_PARAMETER);
        if (request.generation != generation) return error(ERROR_DEVICE_NOT_CONNECTED);
        if (++reports == failReportAt) return error(ERROR_WRITE_FAULT);
        {
            std::unique_lock<std::mutex> lock(ioMutex);
            if (blockIo) {
                enteredIo = true;
                ioCondition.notify_all();
                ioCondition.wait(lock, [] { return releaseIo; });
            }
        }
        submitted.push_back(request);
        return TRUE;
    }
}

HANDLE CqOpenInterface() {
    if (fake::openError) { SetLastError(fake::openError); return INVALID_HANDLE_VALUE; }
    return fake::device;
}
BOOL CqInstallDevice(const char*) {
    ++fake::installs;
    return fake::installError ? fake::error(fake::installError) : TRUE;
}
BOOL CqChangeDevice(bool remove) {
    ++fake::changes;
    if (fake::changeError) return fake::error(fake::changeError);
    fake::removed = remove;
    return TRUE;
}
BOOL CqAcquireSession() {
    if (fake::leaseBusy) return fake::error(ERROR_BUSY);
    fake::lease = true;
    return TRUE;
}
void CqReleaseSession() { fake::lease = false; }

#define CloseHandle fake::closeDevice
#define DeviceIoControl fake::io
#define GetSystemMetrics fake::metrics
#include "../cqkeysdll/cqkeysdll.cpp"
#undef CloseHandle
#undef DeviceIoControl
#undef GetSystemMetrics

int failures = 0;
void check(bool condition, const char* message) {
    if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}

template<typename Report> Report lastReport() {
    Report report{};
    if (!fake::submitted.empty()) memcpy(&report, fake::submitted.back().report, sizeof(report));
    return report;
}

void testDescriptor() {
    unsigned reportBits[5]{};
    unsigned reportId = 0, reportSize = 0, reportCount = 0, depth = 0, collections = 0;
    for (size_t offset = 0; offset < sizeof(CqReportDescriptor);) {
        unsigned prefix = CqReportDescriptor[offset++];
        unsigned size = prefix & 3;
        if (size == 3) size = 4;
        check(offset + size <= sizeof(CqReportDescriptor), "descriptor item is bounded");
        unsigned value = 0;
        for (unsigned byte = 0; byte < size; ++byte) value |= CqReportDescriptor[offset++] << (8 * byte);
        switch (prefix & 0xfc) {
        case 0x84: reportId = value; break;
        case 0x74: reportSize = value; break;
        case 0x94: reportCount = value; break;
        case 0x80:
            check(reportId > 0 && reportId < 5, "descriptor report id is valid");
            if (reportId < 5) reportBits[reportId] += reportSize * reportCount;
            break;
        case 0xa0: if (depth++ == 0) ++collections; break;
        case 0xc0: check(depth > 0, "collections are balanced"); --depth; break;
        }
    }
    check(depth == 0 && collections == 4, "four balanced HID top-level collections");
    check(reportBits[1] == (sizeof(CqKeyboardReport) - 1) * 8, "keyboard descriptor matches wire layout");
    check(reportBits[2] == (sizeof(CqMouseReport) - 1) * 8, "mouse descriptor matches wire layout");
    check(reportBits[3] == (sizeof(CqAbsoluteReport) - 1) * 8, "absolute descriptor matches wire layout");
    check(reportBits[4] == (sizeof(CqConsumerReport) - 1) * 8, "consumer descriptor matches wire layout");
}

int main() {
    char name[] = "cqkeys", path[] = "C:\\test\\cqkeys.inf", wrongName[] = "other";
    testDescriptor();
    fake::reset();
    check(GetLifecycleVersion() == 2, "lifecycle ABI version is 2");
    check(!LoadNTDriver(wrongName, path) && fake::installs == 0, "validate device name before installation");
    fake::leaseBusy = true;
    check(!LoadNTDriver(name, path) && GetLastError() == ERROR_BUSY, "another process owns the session");
    check(UnloadNTDriver(name) && fake::changes == 0, "rejected client cannot remove another session");
    fake::leaseBusy = false;
    fake::installError = ERROR_SUCCESS_REBOOT_REQUIRED;
    check(!LoadNTDriver(name, path) && GetLastError() == ERROR_SUCCESS_REBOOT_REQUIRED, "restart required is not ready");
    check(UnloadNTDriver(name), "partial installation can be cleaned up");

    fake::reset();
    fake::version = 1;
    check(!SetHandle() && GetLastError() == ERROR_REVISION_MISMATCH, "driver protocol mismatch is rejected");
    check(fake::closes == 1, "failed handshake closes handle");
    check(UnloadNTDriver(name), "cleanup mismatched session");
    fake::reset();
    fake::statusBytes = 1;
    check(!SetHandle() && GetLastError() == ERROR_REVISION_MISMATCH, "truncated status is rejected");
    check(UnloadNTDriver(name), "cleanup truncated handshake");

    fake::reset();
    check(LoadNTDriver(name, path) && SetHandle(), "load and open session");
    check(!LoadNTDriver(name, path) && GetLastError() == ERROR_BUSY, "do not reinstall an active session");
    KeyDown('A');
    check(lastReport<CqKeyboardReport>().keys[0] == 4, "A maps to HID usage 4");
    KeyDown(VK_LCONTROL); KeyDown(VK_RCONTROL); KeyUp(VK_LCONTROL);
    check(lastReport<CqKeyboardReport>().modifiers == 0x10, "left/right modifiers remain distinct");
    KeyDown(VK_LEFT); KeyDown(VK_NUMPAD4);
    auto keyboard = lastReport<CqKeyboardReport>();
    check(keyboard.keys[1] == 0x50 && keyboard.keys[2] == 0x5c, "navigation and numpad usages remain distinct");
    KeyDown('B'); KeyDown('C'); KeyDown('D');
    int before = fake::reports;
    KeyDown('E');
    check(GetLastError() == ERROR_TOO_MANY_CMDS && fake::reports == before, "reject seventh key without corrupting held state");
    KeyUp('A'); KeyDown('E');
    check(lastReport<CqKeyboardReport>().keys[0] == 8, "released slot can be reused");
    KeyDown(0xffff);
    check(GetLastError() == ERROR_NOT_SUPPORTED, "unsupported virtual key is explicit");

    MouseLeftButtonDown(); MouseXButton2Down(); MouseMoveRELATIVE(70000, -70000);
    auto mouse = lastReport<CqMouseReport>();
    check(mouse.buttons == 17 && mouse.x == 4466 && mouse.y == -4466, "split large motion preserving held buttons");
    MouseWheelDown(240);
    check(lastReport<CqMouseReport>().wheel == -2, "Windows wheel delta converts to HID detents");
    MouseWheelUp(1);
    check(GetLastError() == ERROR_INVALID_PARAMETER, "sub-detent input is not silently rounded");
    MouseMoveABSOLUTE(1919, 1079);
    auto absolute = lastReport<CqAbsoluteReport>();
    check(absolute.x == 32767 && absolute.y == 32767, "absolute range maps full primary display");
    MouseMoveABSOLUTE(-1, 10);
    check(GetLastError() == ERROR_INVALID_PARAMETER, "out-of-range absolute coordinates are rejected");

    KeyDown(VK_MEDIA_PLAY_PAUSE);
    check(lastReport<CqConsumerReport>().usage == 0xcd, "media virtual key uses consumer collection");
    ConsumerKeyDown(0xe9);
    check(GetLastError() == ERROR_BUSY, "single consumer slot does not overwrite held key");
    KeyUp(VK_MEDIA_PLAY_PAUSE);
    check(lastReport<CqConsumerReport>().usage == 0, "consumer key release submits neutral report");

    auto request = fake::submitted.front();
    request.version = 1;
    check(!CqValidReport(request), "incompatible protocol version is rejected");
    request = fake::submitted.front(); request.reportLength = 8;
    check(!CqValidReport(request), "incorrect report length is rejected");
    request = fake::submitted.front(); request.reserved[0] = 1;
    check(!CqValidReport(request), "nonzero reserved bytes are rejected");
    request = fake::submitted.front(); request.report[4] = 4;
    check(!CqValidReport(request), "duplicate keyboard usages are rejected");
    request = fake::submitted.back(); request.report[8] = 1;
    check(!CqValidReport(request), "nonzero trailing report data is rejected");
    request = {};
    request.size = sizeof(request); request.version = CQKEYS_PROTOCOL_VERSION;
    request.reportLength = sizeof(CqMouseReport); request.report[0] = CQKEYS_MOUSE_ID;
    check(CqValidReport(request), "neutral mouse report is valid");
    request.report[1] = 32;
    check(!CqValidReport(request), "mouse button padding is rejected");
    request.report[1] = 0; request.report[3] = 0x80;
    check(!CqValidReport(request), "relative axis below descriptor minimum is rejected");
    request.report[3] = 0; request.report[6] = 0x80;
    check(!CqValidReport(request), "wheel below descriptor minimum is rejected");
    request.report[6] = 0; request.reportLength = sizeof(CqAbsoluteReport);
    request.report[0] = CQKEYS_ABSOLUTE_ID; request.report[2] = 0x80;
    check(!CqValidReport(request), "absolute coordinate exceeds descriptor maximum");
    request.report[2] = 0; request.reportLength = sizeof(CqConsumerReport);
    request.report[0] = CQKEYS_CONSUMER_ID; request.report[2] = 4;
    check(!CqValidReport(request), "consumer usage exceeds descriptor maximum");
    request.report[2] = 0; request.report[0] = 0xff;
    check(!CqValidReport(request), "unknown report id is rejected");

    fake::failReset = true;
    check(!UnloadNTDriver(name) && GetLastError() == ERROR_WRITE_FAULT, "release failure prevents false unload success");
    check(fake::changes == 0 && fake::closes == 0 && fake::lease, "retain handle and ownership for retry");
    check(!SetHandle() && GetLastError() == ERROR_BUSY, "cannot bypass failed cleanup");
    fake::failReset = false;
    fake::changeError = ERROR_ACCESS_DENIED;
    check(!UnloadNTDriver(name) && GetLastError() == ERROR_ACCESS_DENIED, "PnP removal failure propagates");
    check(fake::closes == 1 && fake::lease, "retain session ownership after PnP failure");
    fake::changeError = 0;
    check(UnloadNTDriver(name) && !fake::lease, "removal failure is retryable");
    check(UnloadNTDriver(name) && fake::changes == 2, "completed unload is idempotent");

    fake::reset(); check(SetHandle(), "open failure propagation session");
    fake::failReportAt = 1;
    MouseMoveRELATIVE(70000, 20);
    check(GetLastError() == ERROR_WRITE_FAULT && fake::reports == 1, "split motion stops at first failed report");
    check(UnloadNTDriver(name), "close failed input session");

    fake::reset(); check(SetHandle(), "open close failure session");
    fake::failClose = true;
    check(!StopNTDriver(name) && fake::changes == 0 && fake::lease, "failed handle close retains cleanup ownership");
    fake::failClose = false;
    check(StopNTDriver(name), "handle close failure is retryable");

    fake::reset(); check(SetHandle(), "open quiesce session");
    KeyDown('A');
    check(StopInputProcessing() && StopInputProcessing(), "input quiesce is idempotent");
    before = fake::reports; KeyDown('B');
    check(GetLastError() == ERROR_NOT_READY && fake::reports == before && fake::closes == 0,
        "quiesce blocks input while retaining device handle");
    check(UnloadNTDriver(name), "unload quiesced session");

    fake::reset(); check(SetHandle(), "open resume session");
    KeyDown('A'); ++fake::generation; KeyDown('B');
    check(GetLastError() == ERROR_DEVICE_NOT_CONNECTED && fake::submitted.size() == 1, "resume cannot replay pre-suspend key state");
    check(!SetHandle(), "stale generation requires lifecycle restart");
    check(UnloadNTDriver(name), "close stale generation");

    fake::reset(); check(SetHandle(), "open surprise removal session");
    fake::resetError = ERROR_DEVICE_NOT_CONNECTED;
    check(!UnloadNTDriver(name) && GetLastError() == ERROR_DEVICE_NOT_CONNECTED,
        "surprise removal does not pretend a successful key release");
    check(fake::closes == 1 && fake::lease && fake::changes == 0, "retire disconnected handle but retain cleanup ownership");
    check(UnloadNTDriver(name) && !fake::lease, "retry can finish after surprise removal instead of retaining stale handle");

    fake::reset(); check(SetHandle(), "open concurrent I/O session");
    fake::blockIo = true;
    std::thread input([] { KeyDown('A'); });
    {
        std::unique_lock<std::mutex> lock(fake::ioMutex);
        check(fake::ioCondition.wait_for(lock, std::chrono::seconds(2), [] { return fake::enteredIo; }), "input entered I/O boundary");
    }
    std::atomic<bool> stopping{false}, stopped{false};
    std::thread stop([&] { stopping = true; check(UnloadNTDriver(name), "concurrent unload succeeds"); stopped = true; });
    while (!stopping) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    check(fake::closes == 0 && !stopped, "handle remains valid while input is outstanding");
    {
        std::lock_guard<std::mutex> lock(fake::ioMutex); fake::releaseIo = true;
    }
    fake::ioCondition.notify_all(); input.join(); stop.join();
    check(fake::closes == 1 && fake::resets == 1, "drain then reset then close");

    for (int cycle = 0; cycle < 20; ++cycle) {
        fake::reset();
        check(LoadNTDriver(name, path) && SetHandle(), "repeat install/open");
        KeyDown('A');
        check(StopNTDriver(name) && !fake::removed, "stop disables without removing");
        check(LoadNTDriver(name, path) && SetHandle(), "repeat re-enable/open");
        check(UnloadNTDriver(name) && fake::removed && !fake::lease, "repeat removal releases ownership");
    }
    if (failures) return 1;
    std::cout << "Native VHF protocol and DLL lifecycle tests passed (no kernel device installed)\n";
    return 0;
}
