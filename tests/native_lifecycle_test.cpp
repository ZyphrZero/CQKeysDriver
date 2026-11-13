// Exercises the real DLL implementation against a deterministic Win32 test adapter.
// These tests never open SCM or a real device. They are not kernel integration tests.
#include "../cqkeysdll/pch.h"
#include "../cqkeysdll/cqkeys.h"
#include <winsvc.h>
#include <winternl.h>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <chrono>
#include <vector>
#include <string>
#include <iostream>
#include <cstring>

namespace fake {
    const auto scm = reinterpret_cast<SC_HANDLE>(1);
    const auto service = reinterpret_cast<SC_HANDLE>(2);
    const auto device = reinterpret_cast<HANDLE>(3);
    DWORD state = SERVICE_RUNNING;
    DWORD clock = 0;
    int queries = 0, failQuery = 0, controls = 0, deletes = 0, starts = 0;
    std::atomic<int> closes{0};
    bool exists = true, deleted = false, failRelease = false, holdPending = false;
    bool failDelete = false, holdDeletion = false;
    char registeredPath[MAX_PATH] = "C:\\test\\cqkeys.sys";
    DWORD openError = 0;
    int inputCalls = 0, failInputAt = 0;
    std::vector<USHORT> keyFlags, mouseFlags;
    std::mutex ioMutex;
    std::condition_variable ioCondition;
    bool blockIo = false, enteredIo = false, releaseIo = false;

    void reset() {
        state = SERVICE_RUNNING; clock = 0; queries = failQuery = controls = deletes = closes = 0;
        exists = true; deleted = failRelease = holdPending = false; openError = 0;
        failDelete = holdDeletion = false; starts = 0;
        inputCalls = failInputAt = 0;
        keyFlags.clear(); mouseFlags.clear();
        blockIo = enteredIo = releaseIo = false;
    }
    SC_HANDLE WINAPI openManager(LPCSTR, LPCSTR, DWORD) { return scm; }
    SC_HANDLE WINAPI openService(SC_HANDLE, LPCSTR, DWORD) {
        if (openError || !exists) { SetLastError(openError ? openError : ERROR_SERVICE_DOES_NOT_EXIST); return nullptr; }
        return service;
    }
    BOOL WINAPI query(SC_HANDLE, LPSERVICE_STATUS result) {
        if (++queries == failQuery) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
        if (state == SERVICE_STOP_PENDING && !holdPending && queries > 1) state = SERVICE_STOPPED;
        *result = {}; result->dwServiceType = SERVICE_KERNEL_DRIVER; result->dwCurrentState = state;
        return TRUE;
    }
    BOOL WINAPI control(SC_HANDLE, DWORD, LPSERVICE_STATUS result) {
        ++controls; state = SERVICE_STOP_PENDING; result->dwCurrentState = state; return TRUE;
    }
    BOOL WINAPI remove(SC_HANDLE) {
        ++deletes;
        if (failDelete) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
        deleted = true; return TRUE;
    }
    BOOL WINAPI closeService(SC_HANDLE handle) {
        if (handle == service && deleted && !holdDeletion) exists = false;
        SetLastError(ERROR_INVALID_HANDLE); // Cleanup must not overwrite the real error.
        return TRUE;
    }
    int WINAPI dpi(HDC, int) { return 144; }
    DWORD WINAPI attributes(LPCSTR) { return FILE_ATTRIBUTE_NORMAL; }
    BOOL WINAPI config(SC_HANDLE, LPQUERY_SERVICE_CONFIGA result, DWORD size, LPDWORD needed) {
        *needed = sizeof(QUERY_SERVICE_CONFIGA);
        if (!size) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
        *result = {}; result->dwServiceType = SERVICE_KERNEL_DRIVER;
        result->lpBinaryPathName = registeredPath; return TRUE;
    }
    SC_HANDLE WINAPI create(SC_HANDLE, LPCSTR, LPCSTR, DWORD, DWORD, DWORD, DWORD,
                            LPCSTR, LPCSTR, LPDWORD, LPCSTR, LPCSTR, LPCSTR) {
        exists = true; state = SERVICE_STOPPED; return service;
    }
    BOOL WINAPI start(SC_HANDLE, DWORD, LPCSTR*) { ++starts; state = SERVICE_RUNNING; return TRUE; }
    void WINAPI sleep(DWORD milliseconds) { clock += milliseconds; }
    DWORD WINAPI ticks() { return clock; }
    BOOL WINAPI closeDevice(HANDLE) { ++closes; return TRUE; }
    HANDLE WINAPI openDevice(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE) { return device; }
    BOOL WINAPI io(HANDLE, DWORD code, LPVOID data, DWORD size, LPVOID, DWORD, LPDWORD returned, LPOVERLAPPED) {
        if (returned) *returned = 0;
        if (!data || !size) return TRUE;
        if (++inputCalls == failInputAt) { SetLastError(ERROR_WRITE_FAULT); return FALSE; }
        {
            std::unique_lock<std::mutex> lock(ioMutex);
            if (blockIo) { enteredIo = true; ioCondition.notify_all(); ioCondition.wait(lock, [] { return releaseIo; }); }
        }
        if (code == IOCTL_KEYBOARD) {
            auto input = static_cast<KEYBOARD_INPUT_DATA*>(data);
            if (failRelease && (input->Flags & KEY_BREAK)) { SetLastError(ERROR_WRITE_FAULT); return FALSE; }
            keyFlags.push_back(input->Flags);
        } else if (code == IOCTL_MOUSE) {
            mouseFlags.push_back(static_cast<MOUSE_INPUT_DATA*>(data)->ButtonFlags);
        }
        return TRUE;
    }
}

#undef OpenSCManager
#undef CreateFile
#define OpenSCManager fake::openManager
#define OpenSCManagerA fake::openManager
#define OpenServiceA fake::openService
#define QueryServiceStatus fake::query
#define QueryServiceConfigA fake::config
#define CreateServiceA fake::create
#define StartServiceA fake::start
#define GetFileAttributesA fake::attributes
#define ControlService fake::control
#define DeleteService fake::remove
#define CloseServiceHandle fake::closeService
#define CloseHandle fake::closeDevice
#define CreateFile fake::openDevice
#define CreateFileA fake::openDevice
#define DeviceIoControl fake::io
#define GetDeviceCaps fake::dpi
#define GetTickCount fake::ticks
#define Sleep fake::sleep
#include "../cqkeysdll/cqkeysdll.cpp"

int failures = 0;
void check(bool condition, const char* message) {
    if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
int main() {
    char name[] = "cqkeys";
    fake::reset(); fake::failQuery = 2;
    check(!UnloadNTDriver(name), "query failure must fail unload");
    check(fake::deletes == 0, "query failure must not delete the service");
    check(GetLastError() == ERROR_ACCESS_DENIED, "cleanup must preserve query error");

    fake::reset(); fake::state = SERVICE_STOP_PENDING;
    check(UnloadNTDriver(name), "already pending stop can finish");
    check(fake::controls == 0, "do not resend STOP to a STOP_PENDING service");

    fake::reset(); fake::openError = ERROR_ACCESS_DENIED;
    check(!UnloadNTDriver(name), "access denied is not service absent");
    check(GetLastError() == ERROR_ACCESS_DENIED, "preserve open-service error");
    check(fake::deletes == 0, "access denied must not delete");

    fake::reset(); fake::holdPending = true;
    check(!UnloadNTDriver(name), "stop timeout must fail");
    check(GetLastError() == ERROR_TIMEOUT, "stop timeout has an explicit error");
    check(fake::deletes == 0, "timed-out service is not deleted");

    fake::reset(); fake::failDelete = true;
    check(!UnloadNTDriver(name), "delete failure must fail");
    check(GetLastError() == ERROR_ACCESS_DENIED, "preserve deletion error");
    fake::failDelete = false;
    check(UnloadNTDriver(name), "deletion failure is retryable");
    check(UnloadNTDriver(name), "unload is idempotent when service is absent");

    fake::reset(); fake::holdDeletion = true;
    check(!UnloadNTDriver(name), "marked for deletion is not deleted yet");
    check(GetLastError() == ERROR_TIMEOUT, "held service handle causes deletion timeout");

    fake::reset();
    char path[] = "C:\\test\\cqkeys.sys";
    check(LoadNTDriver(name, path), "reuse running service");
    check(fake::starts == 0 && fake::deletes == 0, "load does not restart or delete running service");
    char otherPath[] = "C:\\test\\other.sys";
    check(!LoadNTDriver(name, otherPath), "refuse existing service pointing at another binary");
    check(GetLastError() == ERROR_SERVICE_EXISTS, "report service path conflict");

    check(SetHandle(), "open input session");
    KeyDown('A'); MouseLeftButtonDown(); MouseXButton2Down();
    check(StopNTDriver(name), "stop without deleting registration");
    check(fake::deletes == 0 && fake::exists, "stop retains registration");
    check(fake::closes == 1, "close device once");
    check(fake::keyFlags.size() == 2 && fake::keyFlags.back() == KEY_BREAK, "release only held keys before stop");
    check(fake::mouseFlags.size() == 4 && fake::mouseFlags.back() == MOUSE_BUTTON_5_UP, "release held mouse buttons");
    auto sent = fake::keyFlags.size();
    KeyDown('B');
    check(fake::keyFlags.size() == sent, "reject input after stop");
    check(LoadNTDriver(name, path) && SetHandle(), "stopped service can be loaded again");
    check(UnloadNTDriver(name), "reloaded session can be uninstalled");

    fake::reset(); check(SetHandle(), "open for release failure test");
    KeyDown('A'); fake::failRelease = true;
    check(!UnloadNTDriver(name), "failed key release prevents false unload success");
    check(GetLastError() == ERROR_WRITE_FAULT, "preserve release error");
    check(fake::closes == 0 && fake::controls == 0, "retain device for retry before stopping SCM");
    check(!SetHandle(), "cannot reopen a partially drained session");
    fake::failRelease = false;
    check(UnloadNTDriver(name), "retry releases key and unloads");

    fake::reset(); check(SetHandle(), "open for stop-input test");
    KeyDown('A'); check(StopInputProcessing(), "quiesce input independently");
    check(StopInputProcessing(), "quiesce input is idempotent");
    check(fake::keyFlags.size() == 2 && fake::closes == 0, "stop input releases keys but retains handle");
    check(UnloadNTDriver(name), "unload quiesced input");

    fake::reset(); check(SetHandle(), "open for multi-step input failure test");
    fake::failInputAt = 1;
    MouseMoveRELATIVE(100, 20);
    check(GetLastError() == ERROR_WRITE_FAULT, "multi-step input preserves its first I/O error");
    check(fake::inputCalls == 1, "failed move does not send a successful final correction");
    fake::failInputAt = 0;
    check(UnloadNTDriver(name), "unload after failed input");

    fake::reset(); check(SetHandle(), "open for concurrent I/O test");
    fake::blockIo = true;
    std::thread input([] { KeyDown('A'); });
    {
        std::unique_lock<std::mutex> lock(fake::ioMutex);
        check(fake::ioCondition.wait_for(lock, std::chrono::seconds(2), [] { return fake::enteredIo; }), "input entered adapter");
    }
    std::atomic<bool> stopping{false}, stopped{false};
    std::thread stop([&] { stopping = true; check(UnloadNTDriver(name), "concurrent unload succeeds"); stopped = true; });
    while (!stopping) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    check(fake::closes == 0 && !stopped, "handle remains open while input I/O is outstanding");
    {
        std::lock_guard<std::mutex> lock(fake::ioMutex); fake::releaseIo = true;
    }
    fake::ioCondition.notify_all(); input.join(); stop.join();
    check(fake::closes == 1 && fake::keyFlags.back() == KEY_BREAK, "drain input then release key and close");

    for (int cycle = 0; cycle < 20; ++cycle) {
        fake::reset(); fake::state = SERVICE_STOPPED;
        check(LoadNTDriver(name, path) && SetHandle(), "repeated load opens session");
        KeyDown('A');
        check(StopNTDriver(name), "repeated stop retains registration");
        check(LoadNTDriver(name, path) && SetHandle(), "repeated restart opens session");
        check(UnloadNTDriver(name), "repeated unload deletes registration");
        check(fake::closes == 2 && !fake::exists, "repeated cycle leaves no test device or service");
    }

    if (failures) return 1;
    std::cout << "Native lifecycle tests passed\n";
    return 0;
}
