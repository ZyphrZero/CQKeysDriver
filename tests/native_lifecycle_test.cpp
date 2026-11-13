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
    DWORD installError = 0, changeError = 0, openError = 0, stopError = 0;
    uint32_t startProblemCode = 0, startDiagnosticError = 0;
    int32_t startProblemStatus = 0;
    uint16_t infoMajor = CQKEYS_PROTOCOL_MAJOR;
    DWORD infoBytes = sizeof(CqDeviceInfo);
    uint32_t infoMaxReportLength = CQKEYS_MAX_REPORT_LENGTH;
    uint64_t epoch = 10, openedEpoch = 0;
    int stops = 0, changes = 0, installs = 0, reports = 0, failReportAt = 0, queries = 0;
    std::atomic<int> closes{0}, cancels{0};
    bool failClose = false, leaseBusy = false, lease = false, removed = false;
    bool failLeaseRelease = false;
    bool failThreadClose = false;
    bool badReceipt = false, badBegin = false, invalidateOnBegin = false, failQuery = false;
    bool blockReport = false, deferCompletion = false, completionReady = false;
    bool holdIssue = false, issueEntered = false, releaseIssue = false, failBegin = false;
    bool blockOnce = false;
    bool asyncStop = false, releaseControl = false;
    bool holdBegin = false, beginEntered = false, releaseBegin = false;
    bool holdQuery = false, queryEntered = false, releaseQuery = false;
    int failQueryAfterReports = 0;
    OVERLAPPED* pendingControl = nullptr;
    CqDeviceState* controlOutput = nullptr;
    CqDeviceState state{};
    std::vector<CqReportRequest> submitted;
    std::mutex ioMutex;
    std::condition_variable ioCondition;
    OVERLAPPED* pending = nullptr;
    CqSubmitResult* pendingOutput = nullptr;
    CqReportRequest pendingRequest{};
    DWORD pendingError = 0;
    std::mutex handleMutex;
    std::vector<HANDLE> liveEvents, liveThreads;

    HANDLE WINAPI createEvent(LPSECURITY_ATTRIBUTES attributes, BOOL manualReset, BOOL initialState, LPCWSTR name) {
        HANDLE handle = ::CreateEventW(attributes, manualReset, initialState, name);
        if (handle) {
            std::lock_guard<std::mutex> lock(handleMutex);
            liveEvents.push_back(handle);
        }
        return handle;
    }
    HANDLE WINAPI createThread(LPSECURITY_ATTRIBUTES attributes, SIZE_T stackSize, LPTHREAD_START_ROUTINE entry,
                               LPVOID argument, DWORD flags, LPDWORD threadId) {
        HANDLE handle = ::CreateThread(attributes, stackSize, entry, argument, flags, threadId);
        if (handle) {
            std::lock_guard<std::mutex> lock(handleMutex);
            liveThreads.push_back(handle);
        }
        return handle;
    }

    BOOL error(DWORD code) { SetLastError(code); return FALSE; }
    void reset() {
        installError = changeError = openError = stopError = 0;
        startProblemCode = startDiagnosticError = 0; startProblemStatus = 0;
        infoMajor = CQKEYS_PROTOCOL_MAJOR;
        infoBytes = sizeof(CqDeviceInfo);
        infoMaxReportLength = CQKEYS_MAX_REPORT_LENGTH;
        epoch = 10; openedEpoch = 0;
        stops = changes = installs = reports = failReportAt = queries = 0;
        closes = cancels = 0;
        failClose = leaseBusy = lease = removed = false;
        failLeaseRelease = false;
        failThreadClose = false;
        badReceipt = badBegin = invalidateOnBegin = failQuery = false;
        blockReport = deferCompletion = completionReady = false;
        holdIssue = issueEntered = releaseIssue = failBegin = false;
        blockOnce = false;
        asyncStop = releaseControl = false;
        holdBegin = beginEntered = releaseBegin = false;
        holdQuery = queryEntered = releaseQuery = false;
        failQueryAfterReports = 0;
        pendingControl = nullptr; controlOutput = nullptr;
        state = {};
        state.header = CqHeader(sizeof(state), epoch);
        state.phase = CQKEYS_SESSION_CONNECTED;
        state.ready = 1;
        state.keyboard.id = CQKEYS_KEYBOARD_ID;
        submitted.clear(); pending = nullptr; pendingOutput = nullptr; pendingError = 0;
    }
    BOOL WINAPI closeDevice(HANDLE handle) {
        if (handle != device) {
            std::lock_guard<std::mutex> lock(handleMutex);
            auto thread = std::find(liveThreads.begin(), liveThreads.end(), handle);
            if (thread != liveThreads.end() && failThreadClose) return error(ERROR_INVALID_HANDLE);
            if (!::CloseHandle(handle)) return FALSE;
            if (thread != liveThreads.end()) liveThreads.erase(thread);
            auto event = std::find(liveEvents.begin(), liveEvents.end(), handle);
            if (event != liveEvents.end()) liveEvents.erase(event);
            return TRUE;
        }
        std::lock_guard<std::mutex> lock(ioMutex);
        if (failClose || pending || pendingControl) return error(ERROR_INVALID_HANDLE);
        ++closes;
        return TRUE;
    }
    int WINAPI metrics(int index) { return index == SM_CXSCREEN ? 1920 : 1080; }
    void apply(const CqReportRequest& request) {
        state.lastSubmittedSequence = request.sequence;
        submitted.push_back(request);
        ioCondition.notify_all();
        switch (request.report[0]) {
        case CQKEYS_KEYBOARD_ID: memcpy(&state.keyboard, request.report, sizeof(state.keyboard)); break;
        case CQKEYS_MOUSE_ID: state.buttons = request.report[1]; break;
        case CQKEYS_CONSUMER_ID: memcpy(&state.consumer, request.report + 1, sizeof(state.consumer)); break;
        }
        bool keyboardHeld = state.keyboard.modifiers ||
            std::any_of(std::begin(state.keyboard.keys), std::end(state.keyboard.keys), [](uint8_t key) { return key != 0; });
        state.pendingCleanup = (keyboardHeld ? CQKEYS_CLEANUP_KEYBOARD : 0u) |
            (state.buttons ? CQKEYS_CLEANUP_MOUSE : 0u) | (state.consumer ? CQKEYS_CLEANUP_CONSUMER : 0u);
    }
    void completePending(DWORD code) {
        if (!pending) return;
        pendingError = code;
        completionReady = true;
        ioCondition.notify_all();
    }
    BOOL WINAPI cancel(HANDLE handle, LPOVERLAPPED operation) {
        std::lock_guard<std::mutex> lock(ioMutex);
        if (handle != device || !pending || operation != pending) return error(ERROR_NOT_FOUND);
        ++cancels;
        completePending(ERROR_OPERATION_ABORTED);
        return TRUE;
    }
    BOOL WINAPI overlapped(HANDLE handle, LPOVERLAPPED operation, LPDWORD returned, BOOL wait) {
        std::unique_lock<std::mutex> lock(ioMutex);
        if (handle == device && operation == pendingControl && wait) {
            ioCondition.wait(lock, [] { return releaseControl; });
            *controlOutput = state;
            *returned = sizeof(state);
            pendingControl = nullptr;
            return TRUE;
        }
        if (handle != device || pending != operation || !wait) return error(ERROR_INVALID_PARAMETER);
        ioCondition.wait(lock, [] { return completionReady && !deferCompletion; });
        DWORD result = pendingError;
        if (!result) {
            apply(pendingRequest);
            *pendingOutput = {CqHeader(sizeof(CqSubmitResult), epoch, state.header.sessionId), pendingRequest.sequence};
            *returned = sizeof(CqSubmitResult);
        }
        pending = nullptr;
        ioCondition.notify_all();
        return result ? error(result) : TRUE;
    }
    BOOL WINAPI io(HANDLE handle, DWORD code, LPVOID input, DWORD size, LPVOID output,
                  DWORD outputSize, LPDWORD returned, LPOVERLAPPED operation) {
        std::unique_lock<std::mutex> lock(ioMutex);
        *returned = 0;
        if (handle != device || !operation || !operation->hEvent || !input) return error(ERROR_INVALID_PARAMETER);
        const auto& header = *static_cast<CqRequestHeader*>(input);
        if (!CqValidHeader(header, size)) return error(ERROR_REVISION_MISMATCH);
        if (code == IOCTL_CQKEYS_QUERY_INFO) {
            if (size != sizeof(CqRequestHeader) || header.deviceEpoch || header.sessionId ||
                outputSize != sizeof(CqDeviceInfo)) return error(ERROR_INVALID_PARAMETER);
            CqDeviceInfo info{CqHeader(sizeof(CqDeviceInfo), epoch), 3, 0, 0, 1001,
                CQKEYS_CAPABILITIES, infoMaxReportLength, 1, 0};
            info.header.major = infoMajor;
            *static_cast<CqDeviceInfo*>(output) = info;
            *returned = infoBytes;
            return TRUE;
        }
        if (code == IOCTL_CQKEYS_QUERY_STATE) {
            ++queries;
            if (failQuery || (failQueryAfterReports && reports >= failQueryAfterReports)) return error(ERROR_GEN_FAILURE);
            if (size != sizeof(CqRequestHeader) || header.deviceEpoch || header.sessionId ||
                outputSize != sizeof(state)) return error(ERROR_INVALID_PARAMETER);
            state.header.deviceEpoch = epoch;
            if (openedEpoch != epoch && state.phase != CQKEYS_SESSION_STOPPED) state.phase = CQKEYS_SESSION_INVALIDATED;
            *static_cast<CqDeviceState*>(output) = state;
            *returned = sizeof(state);
            if (holdQuery) {
                holdQuery = false;
                queryEntered = true;
                ioCondition.notify_all();
                ioCondition.wait(lock, [] { return releaseQuery; });
            }
            return TRUE;
        }
        if (code == IOCTL_CQKEYS_BEGIN_SESSION) {
            if (invalidateOnBegin) { ++epoch; state.phase = CQKEYS_SESSION_INVALIDATED; }
            if (header.deviceEpoch != openedEpoch || openedEpoch != epoch || header.sessionId)
                return error(ERROR_DEVICE_NOT_CONNECTED);
            if (size != sizeof(CqRequestHeader) || outputSize != sizeof(CqRequestHeader)) return error(ERROR_INVALID_PARAMETER);
            if (failBegin) {
                state.phase = CQKEYS_SESSION_CLEANUP_FAILED;
                state.pendingCleanup = CQKEYS_CLEANUP_ALL;
                return error(ERROR_WRITE_FAULT);
            }
            state.header = CqHeader(sizeof(state), epoch, 41);
            state.phase = CQKEYS_SESSION_ACTIVE;
            *static_cast<CqRequestHeader*>(output) = CqHeader(sizeof(CqRequestHeader), epoch, badBegin ? 0 : 41);
            *returned = sizeof(CqRequestHeader);
            if (holdBegin) {
                beginEntered = true;
                ioCondition.notify_all();
                ioCondition.wait(lock, [] { return releaseBegin; });
            }
            return TRUE;
        }
        if (code == IOCTL_CQKEYS_STOP_SESSION) {
            ++stops;
            if (size != sizeof(CqRequestHeader) || outputSize != sizeof(state)) return error(ERROR_INVALID_PARAMETER);
            completePending(ERROR_OPERATION_ABORTED);
            if (stopError) {
                state.phase = CQKEYS_SESSION_CLEANUP_FAILED;
                state.pendingCleanup = CQKEYS_CLEANUP_ALL;
                state.cleanupStatus = -1;
                state.firstCleanupStatus = -1;
                return error(stopError);
            }
            state.phase = openedEpoch == epoch ? CQKEYS_SESSION_STOPPED : CQKEYS_SESSION_INVALIDATED;
            state.header.deviceEpoch = epoch;
            state.pendingCleanup = 0;
            state.cleanupStatus = 0;
            state.keyboard = {CQKEYS_KEYBOARD_ID};
            state.buttons = 0; state.consumer = 0;
            if (asyncStop) {
                pendingControl = operation;
                controlOutput = static_cast<CqDeviceState*>(output);
                ioCondition.notify_all();
                return error(ERROR_IO_PENDING);
            }
            *static_cast<CqDeviceState*>(output) = state;
            *returned = sizeof(state);
            ioCondition.notify_all();
            return TRUE;
        }
        if (holdIssue) {
            issueEntered = true;
            ioCondition.notify_all();
            ioCondition.wait(lock, [] { return releaseIssue; });
        }
        if (header.deviceEpoch != openedEpoch || openedEpoch != epoch || header.sessionId != state.header.sessionId)
            return error(ERROR_DEVICE_NOT_CONNECTED);
        if (state.phase != CQKEYS_SESSION_ACTIVE) return error(ERROR_NOT_READY);
        if (code != IOCTL_CQKEYS_SUBMIT_REPORT || size != sizeof(CqReportRequest) ||
            outputSize != sizeof(CqSubmitResult) || state.phase != CQKEYS_SESSION_ACTIVE || pending)
            return error(ERROR_INVALID_PARAMETER);
        const auto& request = *static_cast<CqReportRequest*>(input);
        if (!CqValidReport(request)) return error(ERROR_INVALID_PARAMETER);
        ++reports;
        state.lastAcceptedSequence = request.sequence;
        if (reports == failReportAt) {
            state.inputStatus = -1;
            return error(ERROR_WRITE_FAULT);
        }
        if (blockReport) {
            if (blockOnce) blockReport = false;
            pending = operation;
            pendingOutput = static_cast<CqSubmitResult*>(output);
            pendingRequest = request;
            completionReady = false;
            ioCondition.notify_all();
            return error(ERROR_IO_PENDING);
        }
        apply(request);
        *static_cast<CqSubmitResult*>(output) = {CqHeader(sizeof(CqSubmitResult), epoch, state.header.sessionId),
            request.sequence + (badReceipt ? 1 : 0)};
        *returned = sizeof(CqSubmitResult);
        return TRUE;
    }
}

HANDLE CqOpenInterface() {
    if (fake::openError) { SetLastError(fake::openError); return INVALID_HANDLE_VALUE; }
    fake::openedEpoch = fake::epoch;
    return fake::device;
}
BOOL CqInstallDevice(const char*, CqDriverStartResult* diagnostics) {
    ++fake::installs;
    if (diagnostics) {
        diagnostics->problemCode = fake::startProblemCode;
        diagnostics->problemStatus = fake::startProblemStatus;
        diagnostics->diagnosticError = fake::startDiagnosticError;
    }
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
BOOL CqReleaseSession() {
    if (fake::failLeaseRelease) return fake::error(ERROR_INVALID_HANDLE);
    fake::lease = false;
    return TRUE;
}

#define CloseHandle fake::closeDevice
#define CreateEventW fake::createEvent
#define CreateThread fake::createThread
#define DeviceIoControl fake::io
#define GetOverlappedResult fake::overlapped
#define CancelIoEx fake::cancel
#define GetSystemMetrics fake::metrics
#include "../cqkeysdll/cqkeysdll.cpp"
#undef CloseHandle
#undef CreateEventW
#undef CreateThread
#undef DeviceIoControl
#undef GetOverlappedResult
#undef CancelIoEx
#undef GetSystemMetrics

std::atomic<int> failures{0};
void check(bool condition, const char* message) {
    if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}

template<typename Report> Report lastReport() {
    Report report{};
    if (!fake::submitted.empty()) memcpy(&report, fake::submitted.back().report, sizeof(report));
    return report;
}

bool waitPending() {
    std::unique_lock<std::mutex> lock(fake::ioMutex);
    return fake::ioCondition.wait_for(lock, std::chrono::seconds(2), [] { return fake::pending != nullptr; });
}

template<typename Stop> void testStopAdmission(Stop stop) {
    fake::reset(); check(SetHandle(), "admission session");
    std::unique_lock<std::mutex> lifecycle(g_lifecycleMutex);
    std::thread stopping([&] { check(stop(), "stop after lifecycle contention"); });
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (g_acceptingInput && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    check(!g_acceptingInput, "stop closes admission before waiting for lifecycle lock");
    KeyDown('A');
    check(GetLastError() == ERROR_NOT_READY && fake::reports == 0, "contended stop rejects new input");
    lifecycle.unlock();
    stopping.join();
    check(CleanupInputRuntime(), "admission test cleanup");
}

void testStopDuringBegin() {
    fake::reset(); fake::holdBegin = true;
    std::thread opening([] { check(!SetHandle() && GetLastError() == ERROR_NOT_READY, "pending stop prevents BEGIN from reopening admission"); });
    {
        std::unique_lock<std::mutex> lock(fake::ioMutex);
        check(fake::ioCondition.wait_for(lock, std::chrono::seconds(2), [] { return fake::beginEntered; }), "BEGIN entered");
    }
    std::thread stopping([] { check(StopInputProcessing(), "stop after BEGIN"); });
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!g_stopRequests && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    check(g_stopRequests != 0, "stop request registered before BEGIN completes");
    {
        std::lock_guard<std::mutex> lock(fake::ioMutex);
        fake::releaseBegin = true;
    }
    fake::ioCondition.notify_all();
    opening.join(); stopping.join();
    check(!g_acceptingInput && !fake::reports && CleanupInputRuntime(), "BEGIN race drains without input");
}

void testStateSnapshotOwnership() {
    fake::reset(); check(SetHandle(), "snapshot session");
    fake::holdQuery = true;
    CqDeviceState older{}, newer{};
    std::thread oldQuery([&] { check(ReadState(older) == ERROR_SUCCESS, "older query completes"); });
    {
        std::unique_lock<std::mutex> lock(fake::ioMutex);
        check(fake::ioCondition.wait_for(lock, std::chrono::seconds(2), [] { return fake::queryEntered; }), "older query snapshot captured");
    }
    KeyDown('A');
    check(ReadState(newer) == ERROR_SUCCESS, "newer query observes submitted input");
    {
        std::lock_guard<std::mutex> lock(fake::ioMutex);
        fake::releaseQuery = true;
    }
    fake::ioCondition.notify_all(); oldQuery.join();
    check(older.lastSubmittedSequence == 0 && newer.lastSubmittedSequence == 1 && newer.pendingCleanup == CQKEYS_CLEANUP_KEYBOARD,
        "older query cannot replace a newer caller's diagnosis");
    check(CleanupInputRuntime(), "snapshot cleanup");
}

template<typename Cleanup> void testLeaseReleaseFailure(Cleanup cleanup) {
    fake::reset(); check(SetHandle(), "lease close failure session");
    fake::failLeaseRelease = true;
    check(!cleanup() && GetLastError() == ERROR_INVALID_HANDLE, "lease close failure reaches caller");
    check(fake::lease && g_ownsSession && g_cleanupRequired, "failed lease release retains ownership and cleanup gate");
    check(!SetHandle() && GetLastError() == ERROR_BUSY, "failed lease release cannot be bypassed");
    fake::failLeaseRelease = false;
    check(cleanup() && !fake::lease && !g_ownsSession && !g_cleanupRequired, "lease release retry clears ownership only on success");
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

void testDriverStartDiagnostics() {
    fake::reset();
    fake::installError = static_cast<DWORD>(CERT_E_REVOKED);
    fake::startProblemCode = 39;
    fake::startProblemStatus = static_cast<int32_t>(0xC0000603);
    check(!LoadNTDriver("cqkeys", "C:\\test\\cqkeys.inf"), "simulated revoked driver fails to load");
    CqDriverStartResult result{sizeof(CqDriverStartResult), CQ_DRIVER_START_RESULT_ABI};
    check(GetLastDriverStartResult(&result) && result.error == fake::installError && result.problemCode == 39 &&
        result.problemStatus == fake::startProblemStatus && !result.diagnosticError,
        "startup snapshot retains original system error and PnP details");
    check(GetLastError() == fake::installError && static_cast<DWORD>(GetDetailedErrorCode()) == fake::installError,
        "reading startup diagnostics does not clear the original error");
    std::thread other([] {
        check(!LoadNTDriver("other", nullptr), "other thread records an independent startup failure");
        CqDriverStartResult snapshot{sizeof(CqDriverStartResult), CQ_DRIVER_START_RESULT_ABI};
        check(GetLastDriverStartResult(&snapshot) && snapshot.error == ERROR_INVALID_NAME && !snapshot.problemCode,
            "other thread does not inherit startup diagnostics");
    });
    other.join();
    check(GetLastDriverStartResult(&result) && result.problemCode == 39 && result.error == fake::installError,
        "other thread cannot overwrite startup diagnostics");
    check(!GetLastDriverStartResult(nullptr) && GetLastError() == ERROR_INVALID_PARAMETER, "null startup result rejected");
    result.size = 0;
    check(!GetLastDriverStartResult(&result) && GetLastError() == ERROR_INVALID_PARAMETER, "startup result size checked");
    result.size = sizeof(result); result.version = 2;
    check(!GetLastDriverStartResult(&result) && GetLastError() == ERROR_INVALID_PARAMETER, "startup result version checked");
    result.version = CQ_DRIVER_START_RESULT_ABI;
    check(UnloadNTDriver("cqkeys"), "failed startup retains a cleanup path");
    check(GetLastDriverStartResult(&result) && result.error == fake::installError,
        "cleanup does not erase the last startup result");
    check(!LoadNTDriver("other", nullptr) && GetLastDriverStartResult(&result) &&
        result.error == ERROR_INVALID_NAME && !result.problemCode && !result.problemStatus && !result.diagnosticError,
        "early validation failure resets stale startup details");
    fake::reset();
    check(LoadNTDriver("cqkeys", "C:\\test\\cqkeys.inf") && GetLastDriverStartResult(&result) &&
        !result.error && !result.problemCode && !result.problemStatus && !result.diagnosticError,
        "successful load resets the last startup failure");
    check(UnloadNTDriver("cqkeys"), "successful simulated startup cleanup");
}

#ifndef CQ_INPUT_TASK_TEST
int main() {
    const char* name = "cqkeys";
    const char* path = "C:\\test\\cqkeys.inf";
    testDescriptor();
    testStopAdmission([] { return StopInputProcessing(); });
    testStopAdmission([] { return StopNTDriver("cqkeys"); });
    testStopAdmission([] { return UnloadNTDriver("cqkeys"); });
    testStopAdmission([] { return CleanupInputRuntime(); });
    testStopDuringBegin();
    testStateSnapshotOwnership();
    testLeaseReleaseFailure([] { return CleanupInputRuntime(); });
    testLeaseReleaseFailure([] { return StopNTDriver("cqkeys"); });
    testLeaseReleaseFailure([] { return UnloadNTDriver("cqkeys"); });
    testDriverStartDiagnostics();
    fake::reset();
    check(GetLifecycleVersion() == 2, "lifecycle ABI is independent of wire 3.0");
    check(sizeof(CqDeviceInfo) == 56 && sizeof(CqDeviceState) == 88 && sizeof(CqSubmitResult) == 32, "wire sizes");
    check(!LoadNTDriver("other", path) && fake::installs == 0, "validate before installation boundary");
    fake::leaseBusy = true;
    check(!LoadNTDriver(name, path) && GetLastError() == ERROR_BUSY, "busy lease");
    check(UnloadNTDriver(name) && !fake::changes, "unowned session cannot remove device");
    fake::leaseBusy = false;
    fake::installError = ERROR_SUCCESS_REBOOT_REQUIRED;
    check(!LoadNTDriver(name, path) && GetLastError() == ERROR_SUCCESS_REBOOT_REQUIRED, "installation error preserved");
    check(UnloadNTDriver(name), "partial installation cleanup");
    fake::reset(); fake::infoMajor = 1;
    check(!SetHandle() && GetLastError() == ERROR_REVISION_MISMATCH && fake::closes == 1, "protocol mismatch");
    check(CleanupInputRuntime(), "mismatch releases lease explicitly");
    fake::reset(); fake::infoBytes = 1;
    check(!SetHandle() && GetLastError() == ERROR_REVISION_MISMATCH, "truncated info");
    check(CleanupInputRuntime(), "truncated info cleanup");
    fake::reset(); fake::infoMaxReportLength = 16;
    check(!SetHandle() && GetLastError() == ERROR_REVISION_MISMATCH,
        "report slot capacity is not a supported HID report length");
    check(CleanupInputRuntime(), "invalid report capability cleanup");
    fake::reset(); fake::infoMajor = 1; fake::failClose = true;
    check(!SetHandle() && GetLastError() == ERROR_INVALID_HANDLE && fake::lease, "failed handshake close retains resource");
    fake::failClose = false;
    check(CleanupInputRuntime() && fake::closes == 1 && !fake::lease, "pre-BEGIN close failure can retry without assuming a session");
    fake::reset(); fake::badBegin = true;
    check(!SetHandle() && GetLastError() == ERROR_INVALID_DATA && fake::closes == 0, "ambiguous begin retains resource");
    check(CleanupInputRuntime() && fake::stops == 1, "query discovers bound session for cleanup");
    fake::reset(); fake::failBegin = true;
    check(!SetHandle() && GetLastError() == ERROR_WRITE_FAULT, "begin failure has no guaranteed output");
    check(fake::queries == 0, "failed BEGIN does not issue an unused diagnostic query");
    check(CleanupInputRuntime() && fake::stops == 1, "zero-session STOP cleans failed begin");
    fake::reset(); fake::invalidateOnBegin = true;
    check(!SetHandle() && GetLastError() == ERROR_DEVICE_NOT_CONNECTED, "power change before BEGIN invalidates connection");
    check(!SetHandle() && GetLastError() == ERROR_BUSY, "stale connection cannot revive");
    check(CleanupInputRuntime() && fake::stops == 1, "invalidated unbound connection requires STOP");

    fake::reset(); check(SetHandle(), "open session");
    check(!StopNTDriver("other") && GetLastError() == ERROR_INVALID_NAME && g_acceptingInput,
        "invalid stop name does not close the active input gate");
    KeyDown('A'); check(lastReport<CqKeyboardReport>().keys[0] == 4, "A HID mapping");
    check(fake::state.pendingCleanup == CQKEYS_CLEANUP_KEYBOARD, "held keyboard sets cleanup obligation");
    KeyDown(VK_LCONTROL); KeyDown(VK_RCONTROL); KeyUp(VK_LCONTROL);
    check(lastReport<CqKeyboardReport>().modifiers == 0x10, "independent modifier releases");
    KeyDown(VK_LEFT); KeyDown(VK_NUMPAD4); KeyDown('B'); KeyDown('C'); KeyDown('D');
    int before = fake::reports; KeyDown('E');
    check(GetLastError() == ERROR_TOO_MANY_CMDS && fake::reports == before, "six-key capacity");
    KeyUp('A'); KeyDown('E'); check(lastReport<CqKeyboardReport>().keys[0] == 8, "released slot reused");
    KeyDown(0xffff); check(GetLastError() == ERROR_NOT_SUPPORTED, "unsupported key");
    MouseLeftButtonDown(); MouseXButton2Down(); MouseMoveRELATIVE(70000, -70000);
    auto mouse = lastReport<CqMouseReport>();
    check(mouse.buttons == 17 && mouse.x == 4466 && mouse.y == -4466, "split movement preserves buttons");
    MouseWheelDown(240); check(lastReport<CqMouseReport>().wheel == -2, "wheel conversion");
    MouseWheelUp(1); check(GetLastError() == ERROR_INVALID_PARAMETER, "fractional detent rejected");
    MouseMoveABSOLUTE(1919, 1079);
    auto absolute = lastReport<CqAbsoluteReport>();
    check(absolute.x == 32767 && absolute.y == 32767, "absolute endpoints");
    ConsumerKeyDown(0xe9); ConsumerKeyDown(0xea); check(GetLastError() == ERROR_BUSY, "consumer slot protected");
    ConsumerKeyUp(0xe9); check(lastReport<CqConsumerReport>().usage == 0, "consumer neutral");
    for (size_t index = 0; index < fake::submitted.size(); ++index)
        check(fake::submitted[index].sequence == index + 1, "monotonic sequence and matching receipt");
    fake::stopError = ERROR_WRITE_FAULT;
    int queriesBeforeStop = fake::queries;
    check(!UnloadNTDriver(name) && GetLastError() == ERROR_WRITE_FAULT, "stop failure preserved");
    check(fake::queries == queriesBeforeStop + 1, "failed STOP without a diagnostic consumer avoids an unused second query");
    check(fake::lease && fake::closes == 0 && fake::changes == 0, "retain failed cleanup resources");
    check(!SetHandle() && GetLastError() == ERROR_BUSY, "cannot bypass failed cleanup");
    fake::stopError = 0; fake::changeError = ERROR_ACCESS_DENIED;
    check(!UnloadNTDriver(name) && fake::closes == 1 && fake::lease, "PnP failure retains lease");
    check(!SetHandle() && GetLastError() == ERROR_BUSY, "cannot reconnect around failed PnP cleanup");
    check(!LoadNTDriver(name, path) && GetLastError() == ERROR_BUSY, "cannot reinstall around failed PnP cleanup");
    fake::changeError = 0; check(UnloadNTDriver(name) && !fake::lease, "retry PnP cleanup");
    fake::reset(); check(SetHandle(), "partial move session");
    fake::failReportAt = 2; MouseMoveRELATIVE(70000, 0);
    check(GetLastError() == ERROR_WRITE_FAULT && fake::reports == 2 && fake::submitted.size() == 1, "partial move never replayed");
    check(fake::state.pendingCleanup == 0, "pure relative motion does not create held-input cleanup obligation");
    CqDeviceInfo publicInfo{};
    CqDeviceState publicState{};
    CqInputResult inputResult{sizeof(CqInputResult), CQ_INPUT_RESULT_ABI};
    check(QueryDriverInfo(&publicInfo) && publicInfo.header.sessionId == 0 && publicInfo.capabilities == CQKEYS_CAPABILITIES &&
        publicInfo.maxReportLength == sizeof(CqKeyboardReport),
        "public info returns capabilities and unbound header");
    check(QueryInputState(&publicState) && publicState.lastAcceptedSequence == 2 && publicState.lastSubmittedSequence == 1,
        "public state distinguishes acceptance from submission");
    check(GetLastInputResult(&inputResult) && inputResult.error == ERROR_WRITE_FAULT && inputResult.confirmedReports == 1 &&
        inputResult.confirmedX == 32767 && inputResult.confirmedY == 0 && inputResult.lastSequence == 1 &&
        (inputResult.flags & CQ_INPUT_RESULT_PARTIAL) && (inputResult.flags & CQ_INPUT_RESULT_UNCERTAIN),
        "diagnostic queries preserve partial input result");
    std::thread independent([] { KeyDown('A'); }); independent.join();
    check(GetLastInputResult(&inputResult) && inputResult.error == ERROR_WRITE_FAULT && inputResult.confirmedReports == 1,
        "input result is thread local");
    before = fake::reports; KeyDown('A');
    check(GetLastError() == ERROR_NOT_READY && fake::reports == before, "input failure closes admission");
    check(CleanupInputRuntime(), "partial move cleanup");
    fake::reset(); check(SetHandle(), "receipt mismatch session"); fake::badReceipt = true;
    KeyDown('A'); check(GetLastError() == ERROR_INVALID_DATA && fake::queries >= 2, "receipt mismatch queries independent state");
    check(CleanupInputRuntime(), "receipt mismatch cleanup");
    fake::reset(); check(SetHandle(), "close retry session"); fake::failClose = true;
    check(!CleanupInputRuntime() && fake::lease, "close failure retains lease");
    fake::failClose = false; check(CleanupInputRuntime(), "retry close");
    fake::reset(); check(SetHandle(), "disconnect cleanup session");
    KeyDown('A'); fake::stopError = ERROR_DEVICE_NOT_CONNECTED;
    check(!StopInputProcessing() && GetLastError() == ERROR_DEVICE_NOT_CONNECTED,
        "disconnect does not masquerade as confirmed input release");
    check(fake::closes == 1 && fake::lease && fake::changes == 0,
        "retire disconnected handle after drain but retain cleanup lease");
    check(!StopInputProcessing() && GetLastError() == ERROR_DEVICE_NOT_CONNECTED,
        "retired handle does not turn repeated STOP into confirmed release");
    check(!CleanupInputRuntime() && GetLastError() == ERROR_DEVICE_NOT_CONNECTED && fake::lease && g_cleanupRequired,
        "runtime cleanup cannot erase unconfirmed disconnected cleanup");
    check(!SetHandle() && GetLastError() == ERROR_BUSY, "disconnected cleanup cannot be bypassed");
    fake::stopError = 0;
    fake::changeError = ERROR_ACCESS_DENIED;
    check(!UnloadNTDriver(name) && GetLastError() == ERROR_ACCESS_DENIED && fake::lease && g_cleanupRequired,
        "disconnected management failure retains ownership");
    check(!StopInputProcessing() && GetLastError() == ERROR_DEVICE_NOT_CONNECTED,
        "failed management does not erase the unconfirmed release error");
    fake::changeError = 0;
    check(UnloadNTDriver(name) && fake::changes == 2 && !fake::lease,
        "cleanup retry confirms device management without retaining dead handle forever");
    fake::reset(); check(SetHandle(), "epoch session"); ++fake::epoch;
    CheckDeviceStatus(); check(GetLastError() == ERROR_DEVICE_NOT_CONNECTED, "query does not rebind epoch");
    before = fake::reports; KeyDown('A'); check(fake::reports == before, "epoch invalidation closes gate");
    check(StopInputProcessing() && fake::stops == 1 && fake::state.phase == CQKEYS_SESSION_INVALIDATED,
        "successful STOP accepts invalidated response with current epoch");
    check(!SetHandle() && GetLastError() == ERROR_BUSY && g_epoch != fake::epoch,
        "old bound epoch stays unchanged and cannot revive after power cleanup");
    check(CleanupInputRuntime() && fake::stops == 2, "query invalidated state never skips explicit STOP");

    fake::reset(); check(SetHandle(), "synchronous issuance session"); fake::holdIssue = true;
    std::thread issuing([] { KeyDown('A'); });
    {
        std::unique_lock<std::mutex> lock(fake::ioMutex);
        check(fake::ioCondition.wait_for(lock, std::chrono::seconds(2), [] { return fake::issueEntered; }), "report entered API");
    }
    std::thread quiesce([] { check(StopInputProcessing(), "stop across synchronous API invocation"); });
    {
        std::unique_lock<std::mutex> lock(fake::ioMutex);
        check(fake::ioCondition.wait_for(lock, std::chrono::seconds(2), [] { return fake::stops > 0; }),
            "state lock does not cross DeviceIoControl and early cancel race is gated by STOP");
        fake::releaseIssue = true;
    }
    fake::ioCondition.notify_all(); issuing.join(); quiesce.join();
    check(fake::submitted.empty() && CleanupInputRuntime(), "late report cannot pass stopped gate");

    fake::reset(); check(SetHandle(), "concurrent session");
    fake::blockReport = fake::deferCompletion = true;
    std::thread input([] { KeyDown('A'); });
    check(waitPending(), "report pending at boundary");
    std::atomic<bool> stopped{false};
    std::thread stop([&] { check(CleanupInputRuntime(), "concurrent cleanup"); stopped = true; });
    {
        std::unique_lock<std::mutex> lock(fake::ioMutex);
        check(fake::ioCondition.wait_for(lock, std::chrono::seconds(2), [] { return fake::stops > 0; }),
            "STOP issued while REPORT completion is held");
        check(fake::cancels >= 1 && fake::closes == 0 && !stopped, "cancel is not completion and handle remains open");
    }
    check(GetDriverHandle() == fake::device, "short state lock available during wait");
    {
        std::lock_guard<std::mutex> lock(fake::ioMutex); fake::deferCompletion = false;
    }
    fake::ioCondition.notify_all(); input.join(); stop.join();
    check(fake::closes == 1 && !fake::lease && fake::submitted.empty(), "close only after actual completion");
    fake::reset(); check(SetHandle(), "overlapped STOP session"); fake::asyncStop = true;
    std::atomic<bool> controlDone{false};
    std::thread control([&] { check(StopInputProcessing(), "pending STOP completion"); controlDone = true; });
    {
        std::unique_lock<std::mutex> lock(fake::ioMutex);
        check(fake::ioCondition.wait_for(lock, std::chrono::seconds(2), [] { return fake::pendingControl != nullptr; }), "STOP pending");
        check(!controlDone && fake::closes == 0, "STOP request is not completion");
    }
    check(GetDriverHandle() == fake::device, "state access available while STOP is pending");
    KeyDown('A'); check(GetLastError() == ERROR_NOT_READY, "STOP closes gate before completion");
    {
        std::lock_guard<std::mutex> lock(fake::ioMutex); fake::releaseControl = true;
    }
    fake::ioCondition.notify_all(); control.join();
    check(CleanupInputRuntime(), "close after pending STOP completes");
    for (int cycle = 0; cycle < 20; ++cycle) {
        fake::reset(); check(SetHandle(), "repeated open"); KeyDown('A');
        check(StopInputProcessing() && StopInputProcessing(), "idempotent quiesce");
        check(CleanupInputRuntime() && fake::changes == 0, "explicit cleanup does not perform PnP");
    }
    if (failures) return 1;
    std::cout << "Native DLL lifecycle tests passed (fake Win32 device boundary; no kernel verification)\n";
    return 0;
}
#endif
