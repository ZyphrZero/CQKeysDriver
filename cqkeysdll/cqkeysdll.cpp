#include "pch.h"
#include "cqkeys.h"
#include "pnp_device.h"
#include "../shared/cqkeys_protocol.h"
#include <mutex>
#include <atomic>
#include <cstring>
#include <condition_variable>
#include <chrono>
#include <vector>
#include <new>

namespace {
    std::mutex g_lifecycleMutex;
    std::mutex g_deviceMutex;
    std::mutex g_inputMutex;
    std::mutex g_controlMutex;
    std::mutex g_taskMutex;
    std::condition_variable g_taskCondition;
    HANDLE g_device = INVALID_HANDLE_VALUE;
    std::atomic<bool> g_acceptingInput{false};
    std::atomic<uint32_t> g_stopRequests{0};
    std::atomic<bool> g_taskActive{false};
    std::atomic<bool> g_taskCancelled{false};
    bool g_ownsSession = false;
    bool g_cleanupRequired = false;
    DWORD g_retiredCleanupError = ERROR_SUCCESS;
    CqKeyboardReport g_keyboard{CQKEYS_KEYBOARD_ID};
    uint8_t g_buttons = 0;
    uint16_t g_consumer = 0;
    uint64_t g_epoch = 0;
    uint64_t g_session = 0;
    uint64_t g_sequence = 0;
    bool g_beginAttempted = false;
    OVERLAPPED* g_pendingReport = nullptr;
    bool g_pendingTaskReport = false;
    HANDLE g_taskThread = nullptr;
    std::vector<CqInputAction> g_actions;
    CqInputTaskResult g_taskResult{};
    uint64_t g_nextTaskId = 0;
    uint32_t g_repeats = 0;
    uint32_t g_repeatDelay = 0;
    thread_local bool g_taskCleanup = false;
    thread_local CqInputResult g_lastInput{sizeof(CqInputResult), CQ_INPUT_RESULT_ABI};
    std::atomic<DEVICE_STATUS> g_status{DEVICE_STATUS_UNKNOWN};
    std::atomic<DWORD> g_error{ERROR_SUCCESS};
    thread_local CqDriverStartResult g_lastDriverStart{sizeof(CqDriverStartResult), CQ_DRIVER_START_RESULT_ABI};
    std::atomic<ULONGLONG> g_lastCheck{0};

    struct StopRequest {
        StopRequest() {
            std::lock_guard<std::mutex> state(g_deviceMutex);
            ++g_stopRequests;
            g_acceptingInput = false;
        }
        ~StopRequest() { --g_stopRequests; }
    };

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

    DWORD IoControl(HANDLE device, DWORD code, void* input, DWORD inputSize,
                    void* output, DWORD outputSize, DWORD& returned, bool report = false, bool* issued = nullptr) {
        if (issued) *issued = false;
        OVERLAPPED operation{};
        operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!operation.hEvent) return GetLastError();
        DWORD error = ERROR_SUCCESS;
        BOOL completed = FALSE;
        if (report) {
            std::lock_guard<std::mutex> state(g_deviceMutex);
            if (!g_acceptingInput) error = ERROR_NOT_READY;
            else if (g_taskActive && g_taskCancelled && !g_taskCleanup) error = ERROR_OPERATION_ABORTED;
            else {
                g_pendingReport = &operation;
                g_pendingTaskReport = g_taskActive && !g_taskCleanup;
            }
        }
        if (!error) {
            if (issued) *issued = true;
            completed = DeviceIoControl(device, code, input, inputSize, output, outputSize,
                &returned, &operation);
            if (!completed) error = GetLastError();
        }
        if (!completed && error == ERROR_IO_PENDING) {
            if (report && (!g_acceptingInput || (g_taskActive && g_taskCancelled && !g_taskCleanup)))
                CancelIoEx(device, &operation);
            if (GetOverlappedResult(device, &operation, &returned, TRUE)) error = ERROR_SUCCESS;
            else error = GetLastError();
        }
        if (report) {
            std::lock_guard<std::mutex> state(g_deviceMutex);
            if (g_pendingReport == &operation) g_pendingReport = nullptr;
        }
        if (!CloseHandle(operation.hEvent) && error == ERROR_SUCCESS) error = GetLastError();
        return error;
    }

    DWORD ReadState(CqDeviceState& snapshot) {
        HANDLE device;
        {
            std::lock_guard<std::mutex> state(g_deviceMutex);
            device = g_device;
        }
        if (device == INVALID_HANDLE_VALUE) return ERROR_INVALID_HANDLE;
        auto request = CqHeader(sizeof(CqRequestHeader));
        CqDeviceState response{};
        DWORD returned = 0;
        DWORD error = IoControl(device, IOCTL_CQKEYS_QUERY_STATE, &request, sizeof(request),
            &response, sizeof(response), returned);
        if (error) return error;
        if (returned != sizeof(response) || !CqValidHeader(response.header, sizeof(response)) ||
            !response.header.deviceEpoch || response.phase > CQKEYS_SESSION_CLOSED ||
            response.ready > 1 || (response.pendingCleanup & ~CQKEYS_CLEANUP_ALL) ||
            response.reserved || response.reservedState ||
            response.lastSubmittedSequence > response.lastAcceptedSequence) return ERROR_INVALID_DATA;
        snapshot = response;
        return ERROR_SUCCESS;
    }

    DWORD CancelReport(bool taskOnly = false) {
        std::lock_guard<std::mutex> state(g_deviceMutex);
        if (!g_pendingReport || (taskOnly && !g_pendingTaskReport)) return ERROR_SUCCESS;
        if (CancelIoEx(g_device, g_pendingReport)) return ERROR_SUCCESS;
        DWORD error = GetLastError();
        return error == ERROR_NOT_FOUND ? ERROR_SUCCESS : error;
    }

    bool CleanupComplete(const CqDeviceState& state) {
        return state.phase == CQKEYS_SESSION_STOPPED && !state.pendingCleanup && state.cleanupStatus >= 0;
    }

    DWORD StopSession(CqDeviceState* snapshot = nullptr) {
        std::lock_guard<std::mutex> control(g_controlMutex);
        g_acceptingInput = false;
        g_status = DEVICE_STATUS_UNKNOWN;
        HANDLE device;
        uint64_t epoch, session;
        {
            std::lock_guard<std::mutex> state(g_deviceMutex);
            device = g_device;
            epoch = g_epoch;
            session = g_session;
            if (!g_beginAttempted) return ERROR_SUCCESS;
        }
        if (device == INVALID_HANDLE_VALUE) return ERROR_SUCCESS;
        DWORD cancelError = CancelReport();
        CqDeviceState state{};
        DWORD queryError = ReadState(state);
        if (!queryError) {
            if (snapshot) *snapshot = state;
            if (CleanupComplete(state) && (!session || state.header.sessionId == session)) return cancelError;
            if (!session) {
                session = state.header.sessionId;
            }
        }
        if (!epoch) return queryError ? queryError : ERROR_INVALID_STATE;
        auto request = CqHeader(sizeof(CqRequestHeader), epoch, session);
        CqDeviceState response{};
        DWORD returned = 0;
        DWORD error = IoControl(device, IOCTL_CQKEYS_STOP_SESSION, &request, sizeof(request),
            &response, sizeof(response), returned);
        bool stopped = (response.phase == CQKEYS_SESSION_STOPPED || response.phase == CQKEYS_SESSION_INVALIDATED ||
            response.phase == CQKEYS_SESSION_CLOSED) && !response.pendingCleanup && response.cleanupStatus >= 0;
        if (!error && (returned != sizeof(response) || !CqValidHeader(response.header, sizeof(response)) ||
            !response.header.deviceEpoch || response.header.sessionId != session ||
            !stopped)) error = ERROR_INVALID_DATA;
        if (error) {
            if (snapshot) ReadState(*snapshot);
            return error;
        }
        if (snapshot) *snapshot = response;
        return cancelError;
    }

    void SignalTaskCancellation() {
        std::lock_guard<std::mutex> task(g_taskMutex);
        g_taskCancelled = true;
        g_taskCondition.notify_all();
    }

    DWORD JoinTask(bool release) {
        if (g_taskThread) {
            if (WaitForSingleObject(g_taskThread, INFINITE) != WAIT_OBJECT_0) return GetLastError();
            if (!CloseHandle(g_taskThread)) return GetLastError();
            g_taskThread = nullptr;
        }
        if (release) {
            std::lock_guard<std::mutex> task(g_taskMutex);
            g_taskResult = {};
            std::vector<CqInputAction>().swap(g_actions);
            g_taskCondition.notify_all();
        }
        return ERROR_SUCCESS;
    }

    DWORD Quiesce() {
        if (g_device != INVALID_HANDLE_VALUE || g_ownsSession || g_taskThread) g_cleanupRequired = true;
        g_acceptingInput = false;
        SignalTaskCancellation();
        DWORD error = StopSession();
        if (!error) error = g_retiredCleanupError;
        {
            std::lock_guard<std::mutex> input(g_inputMutex);
            if (!error) ClearInputState();
        }
        DWORD joinError = JoinTask(false);
        return error ? error : joinError;
    }

    DWORD RetireDisconnectedDevice(DWORD error) {
        if ((error != ERROR_DEVICE_NOT_CONNECTED && error != ERROR_NO_SUCH_DEVICE) ||
            g_taskThread || g_taskActive) return error;
        std::lock_guard<std::mutex> state(g_deviceMutex);
        if (g_pendingReport) return error;
        if (g_device != INVALID_HANDLE_VALUE) {
            if (!CloseHandle(g_device)) return GetLastError();
            g_device = INVALID_HANDLE_VALUE;
        }
        g_epoch = g_session = g_sequence = 0;
        g_beginAttempted = false;
        g_retiredCleanupError = error;
        return error;
    }

    BOOL CloseDevice() {
        DWORD error = Quiesce();
        if (error) return Fail(RetireDisconnectedDevice(error));
        std::lock_guard<std::mutex> state(g_deviceMutex);
        if (g_device != INVALID_HANDLE_VALUE) {
            if (!CloseHandle(g_device)) return Fail(GetLastError());
            g_device = INVALID_HANDLE_VALUE;
        }
        g_epoch = g_session = g_sequence = 0;
        g_beginAttempted = false;
        return Success();
    }

    BOOL QueryStatus() {
        FILETIME time;
        GetSystemTimeAsFileTime(&time);
        ULARGE_INTEGER ticks;
        ticks.LowPart = time.dwLowDateTime;
        ticks.HighPart = time.dwHighDateTime;
        g_lastCheck = (ticks.QuadPart - 116444736000000000ull) / 10000;
        g_status = DEVICE_STATUS_ERROR;
        CqDeviceState snapshot{};
        DWORD error = ReadState(snapshot);
        if (error) { g_acceptingInput = false; return Fail(error); }
        std::lock_guard<std::mutex> state(g_deviceMutex);
        if (snapshot.header.deviceEpoch != g_epoch || snapshot.header.sessionId != g_session ||
            snapshot.phase == CQKEYS_SESSION_INVALIDATED) {
            g_acceptingInput = false;
            return Fail(ERROR_DEVICE_NOT_CONNECTED);
        }
        if (!snapshot.ready || snapshot.phase != CQKEYS_SESSION_ACTIVE || !g_acceptingInput) {
            g_acceptingInput = false;
            return Fail(ERROR_NOT_READY);
        }
        g_status = DEVICE_STATUS_READY;
        return Success();
    }

    template<typename Report> BOOL SendReport(const Report& report) {
        CqReportRequest request{};
        HANDLE device;
        {
            std::lock_guard<std::mutex> state(g_deviceMutex);
            if (!g_acceptingInput || g_device == INVALID_HANDLE_VALUE) return Fail(ERROR_NOT_READY);
            if (g_sequence == UINT64_MAX) { g_acceptingInput = false; return Fail(ERROR_ARITHMETIC_OVERFLOW); }
            request.header = CqHeader(sizeof(request), g_epoch, g_session);
            request.sequence = ++g_sequence;
            device = g_device;
        }
        request.reportLength = sizeof(report);
        memcpy(request.report, &report, sizeof(report));
        if (!CqValidReport(request)) return Fail(ERROR_INVALID_PARAMETER);
        DWORD returned = 0;
        CqSubmitResult response{};
        bool issued = false;
        DWORD error = IoControl(device, IOCTL_CQKEYS_SUBMIT_REPORT, &request, sizeof(request),
            &response, sizeof(response), returned, true, &issued);
        if (!error && (returned != sizeof(response) || !CqValidHeader(response.header, sizeof(response)) ||
            response.header.deviceEpoch != request.header.deviceEpoch ||
            response.header.sessionId != request.header.sessionId || response.sequence != request.sequence))
            error = ERROR_INVALID_DATA;
        if (error) {
            CqDeviceState snapshot{};
            DWORD queryError = ReadState(snapshot);
            bool cancelledTask = false;
            {
                std::lock_guard<std::mutex> state(g_deviceMutex);
                cancelledTask = error == ERROR_OPERATION_ABORTED && g_taskActive && g_taskCancelled &&
                    !queryError && snapshot.phase == CQKEYS_SESSION_ACTIVE &&
                    snapshot.header.deviceEpoch == g_epoch && snapshot.header.sessionId == g_session;
            }
            if (!cancelledTask) { g_acceptingInput = false; g_status = DEVICE_STATUS_ERROR; }
            if (issued) g_lastInput.flags |= CQ_INPUT_RESULT_UNCERTAIN;
            return Fail(error);
        }
        ++g_lastInput.confirmedReports;
        g_lastInput.lastSequence = request.sequence;
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
        CqMouseReport report{CQKEYS_MOUSE_ID};
        report.buttons = down ? g_buttons | mask : g_buttons & static_cast<uint8_t>(~mask);
        if (SendReport(report)) g_buttons = report.buttons;
    }

    void MouseWheel(USHORT delta, bool down) {
        if (delta % WHEEL_DELTA != 0 || delta / WHEEL_DELTA > 127) { Fail(ERROR_INVALID_PARAMETER); return; }
        CqMouseReport report{CQKEYS_MOUSE_ID, g_buttons};
        report.wheel = static_cast<int8_t>((down ? -1 : 1) * (delta / WHEEL_DELTA));
        SendReport(report);
    }

    void MoveRelative(LONG deltaX, LONG deltaY) {
        do {
            auto stepX = static_cast<int16_t>((std::max)(-32767L, (std::min)(32767L, deltaX)));
            auto stepY = static_cast<int16_t>((std::max)(-32767L, (std::min)(32767L, deltaY)));
            CqMouseReport report{CQKEYS_MOUSE_ID, g_buttons, stepX, stepY, 0};
            if (!SendReport(report)) return;
            g_lastInput.confirmedX += stepX;
            g_lastInput.confirmedY += stepY;
            deltaX -= stepX;
            deltaY -= stepY;
        } while (deltaX != 0 || deltaY != 0);
    }

    void MoveAbsolute(LONG screenX, LONG screenY) {
        LONG width = GetSystemMetrics(SM_CXSCREEN), height = GetSystemMetrics(SM_CYSCREEN);
        if (width <= 1 || height <= 1 || screenX < 0 || screenY < 0 || screenX >= width || screenY >= height) {
            Fail(ERROR_INVALID_PARAMETER);
            return;
        }
        CqAbsoluteReport report{CQKEYS_ABSOLUTE_ID,
            static_cast<uint16_t>(static_cast<int64_t>(screenX) * 32767 / (width - 1)),
            static_cast<uint16_t>(static_cast<int64_t>(screenY) * 32767 / (height - 1))};
        if (SendReport(report)) { g_lastInput.confirmedX = screenX; g_lastInput.confirmedY = screenY; }
    }

    template<typename Function> void LegacyInput(Function function) {
        g_lastInput = {sizeof(CqInputResult), CQ_INPUT_RESULT_ABI};
        if (g_taskActive) { g_lastInput.error = ERROR_BUSY; Fail(ERROR_BUSY); return; }
        std::lock_guard<std::mutex> input(g_inputMutex);
        if (g_taskActive) { g_lastInput.error = ERROR_BUSY; Fail(ERROR_BUSY); return; }
        if (!g_acceptingInput) { g_lastInput.error = ERROR_NOT_READY; Fail(ERROR_NOT_READY); return; }
        function();
        g_lastInput.error = GetLastError();
        if (g_lastInput.error && g_lastInput.confirmedReports) g_lastInput.flags |= CQ_INPUT_RESULT_PARTIAL;
        if (g_lastInput.error == ERROR_OPERATION_ABORTED) g_lastInput.flags |= CQ_INPUT_RESULT_CANCELLED;
    }

    bool HasHeldInput() {
        return g_keyboard.modifiers || g_buttons || g_consumer ||
            std::any_of(std::begin(g_keyboard.keys), std::end(g_keyboard.keys), [](uint8_t key) { return key != 0; });
    }

    bool ValidAction(const CqInputAction& action) {
        if (action.delayMs > CQ_INPUT_TASK_MAX_DELAY_MS) return false;
        switch (action.type) {
        case CQ_INPUT_KEY_DOWN: case CQ_INPUT_KEY_UP:
            return action.value2 == 0 && action.value1 > 0 && action.value1 <= 0xffff &&
                (KeyboardUsage(static_cast<USHORT>(action.value1)) || ConsumerUsage(static_cast<USHORT>(action.value1)));
        case CQ_INPUT_CONSUMER_DOWN: case CQ_INPUT_CONSUMER_UP:
            return action.value2 == 0 && action.value1 > 0 && action.value1 <= 0x3ff;
        case CQ_INPUT_MOVE_RELATIVE: return true;
        case CQ_INPUT_MOVE_ABSOLUTE: return action.value1 >= 0 && action.value2 >= 0;
        case CQ_INPUT_BUTTONS: return action.value2 == 0 && action.value1 >= 0 && action.value1 <= 31;
        case CQ_INPUT_WHEEL: return action.value2 == 0 && action.value1 >= -127 && action.value1 <= 127;
        case CQ_INPUT_WAIT: return action.value1 == 0 && action.value2 == 0;
        default: return false;
        }
    }

    void ExecuteAction(const CqInputAction& action) {
        switch (action.type) {
        case CQ_INPUT_KEY_DOWN: case CQ_INPUT_KEY_UP:
            KeyboardInput(static_cast<USHORT>(action.value1), action.type == CQ_INPUT_KEY_DOWN); break;
        case CQ_INPUT_CONSUMER_DOWN: case CQ_INPUT_CONSUMER_UP:
            ConsumerInput(static_cast<USHORT>(action.value1), action.type == CQ_INPUT_CONSUMER_DOWN); break;
        case CQ_INPUT_MOVE_RELATIVE: MoveRelative(action.value1, action.value2); break;
        case CQ_INPUT_MOVE_ABSOLUTE: MoveAbsolute(action.value1, action.value2); break;
        case CQ_INPUT_BUTTONS: {
            CqMouseReport report{CQKEYS_MOUSE_ID, static_cast<uint8_t>(action.value1)};
            if (SendReport(report)) g_buttons = report.buttons;
            break;
        }
        case CQ_INPUT_WHEEL: {
            CqMouseReport report{CQKEYS_MOUSE_ID, g_buttons, 0, 0, static_cast<int8_t>(action.value1)};
            SendReport(report);
            break;
        }
        case CQ_INPUT_WAIT: Success(); break;
        }
    }

    bool TaskDelay(uint32_t delay) {
        std::unique_lock<std::mutex> task(g_taskMutex);
        return !g_taskCondition.wait_for(task, std::chrono::milliseconds(delay), [] { return g_taskCancelled.load(); });
    }

    DWORD ReleaseTaskInput() {
        g_taskCleanup = true;
        CqKeyboardReport keyboard{CQKEYS_KEYBOARD_ID};
        CqMouseReport mouse{CQKEYS_MOUSE_ID};
        CqConsumerReport consumer{CQKEYS_CONSUMER_ID};
        DWORD error = ERROR_SUCCESS;
        if (!SendReport(keyboard) || !SendReport(mouse) || !SendReport(consumer)) error = GetLastError();
        if (!error) ClearInputState();
        g_taskCleanup = false;
        return error;
    }

    DWORD WINAPI TaskWorker(void*) {
        DWORD error = ERROR_SUCCESS;
        uint32_t resultFlags = 0;
        uint64_t confirmedReports = 0;
        for (uint32_t repeat = 0; repeat < g_repeats && !error && !g_taskCancelled; ++repeat) {
            for (const auto& action : g_actions) {
                {
                    std::lock_guard<std::mutex> input(g_inputMutex);
                    if (g_taskCancelled) break;
                    if (!g_acceptingInput) error = ERROR_NOT_READY;
                    else {
                        g_lastInput = {sizeof(CqInputResult), CQ_INPUT_RESULT_ABI};
                        ExecuteAction(action);
                        error = GetLastError();
                        confirmedReports += g_lastInput.confirmedReports;
                        resultFlags |= g_lastInput.flags;
                    }
                }
                if (error) break;
                {
                    std::lock_guard<std::mutex> task(g_taskMutex);
                    ++g_taskResult.completedActions;
                }
                if (!TaskDelay(action.delayMs)) break;
            }
            if (error || g_taskCancelled) break;
            {
                std::lock_guard<std::mutex> task(g_taskMutex);
                ++g_taskResult.completedRepeats;
            }
            if (repeat + 1 < g_repeats && !TaskDelay(g_repeatDelay)) break;
        }
        bool taskCancellation = g_taskCancelled && (error == ERROR_SUCCESS || error == ERROR_OPERATION_ABORTED);
        DWORD cleanupError = ERROR_SUCCESS;
        CqDeviceState state{};
        {
            std::lock_guard<std::mutex> input(g_inputMutex);
            if ((!error || taskCancellation) && g_acceptingInput) {
                g_lastInput = {sizeof(CqInputResult), CQ_INPUT_RESULT_ABI};
                cleanupError = ReleaseTaskInput();
                resultFlags |= g_lastInput.flags;
            }
        }
        if ((error && !taskCancellation) || cleanupError || !g_acceptingInput) {
            DWORD stopError = StopSession(&state);
            if (!cleanupError) cleanupError = stopError;
            if (!stopError) {
                std::lock_guard<std::mutex> input(g_inputMutex);
                ClearInputState();
            }
        }
        DWORD queryError = ReadState(state);
        if (!queryError && g_acceptingInput) {
            std::lock_guard<std::mutex> device(g_deviceMutex);
            if (state.header.deviceEpoch != g_epoch || state.header.sessionId != g_session ||
                state.phase != CQKEYS_SESSION_ACTIVE || state.pendingCleanup || state.cleanupStatus < 0)
                queryError = ERROR_INVALID_DATA;
        }
        if (queryError) {
            resultFlags |= CQ_INPUT_RESULT_UNCERTAIN;
            if (!error || taskCancellation) error = queryError;
            DWORD stopError = StopSession(&state);
            if (!cleanupError) cleanupError = stopError;
        }
        if (queryError || (error && !taskCancellation) || cleanupError) { g_acceptingInput = false; g_status = DEVICE_STATUS_ERROR; }
        {
            std::lock_guard<std::mutex> task(g_taskMutex);
            bool cancelled = g_taskCancelled;
            g_taskResult.state = (cleanupError || (error && (!cancelled ||
                (error != ERROR_OPERATION_ABORTED && error != ERROR_NOT_READY)))) ? CQ_INPUT_TASK_FAILED :
                (cancelled ? CQ_INPUT_TASK_CANCELLED : CQ_INPUT_TASK_COMPLETED);
            g_taskResult.error = error ? error : (cleanupError ? cleanupError : (cancelled ? ERROR_CANCELLED : ERROR_SUCCESS));
            g_taskResult.cleanupError = cleanupError;
            g_taskResult.lastAcceptedSequence = state.lastAcceptedSequence;
            g_taskResult.lastSubmittedSequence = state.lastSubmittedSequence;
            g_taskResult.deviceEpoch = state.header.deviceEpoch;
            g_taskResult.sessionId = state.header.sessionId;
            g_taskResult.sessionPhase = state.phase;
            g_taskResult.pendingCleanup = state.pendingCleanup;
            g_taskResult.inputStatus = state.inputStatus;
            g_taskResult.cleanupStatus = state.cleanupStatus;
            if (cancelled) resultFlags |= CQ_INPUT_RESULT_CANCELLED;
            if ((error || cleanupError || cancelled) && confirmedReports) resultFlags |= CQ_INPUT_RESULT_PARTIAL;
            g_taskResult.resultFlags = resultFlags;
            g_taskActive = false;
        }
        g_taskCondition.notify_all();
        return 0;
    }

    BOOL StopDriver(const char* name, bool remove) {
        if (!ValidateName(name)) return FALSE;
        StopRequest stopping;
        std::lock_guard<std::mutex> lifecycle(g_lifecycleMutex);
        if (!g_ownsSession) return Success();
        bool retired = g_device == INVALID_HANDLE_VALUE && g_retiredCleanupError != ERROR_SUCCESS;
        if (!CloseDevice() && (!retired || GetLastError() != g_retiredCleanupError)) return FALSE;
        DWORD error = JoinTask(true);
        if (error) return Fail(error);
        if (!CqChangeDevice(remove)) return Fail(GetLastError());
        if (!CqReleaseSession()) return Fail(GetLastError());
        g_ownsSession = false;
        g_cleanupRequired = false;
        g_retiredCleanupError = ERROR_SUCCESS;
        return Success();
    }
}

DWORD __cdecl GetLifecycleVersion() { return CQKEYS_LIFECYCLE_ABI; }

BOOL __cdecl QueryDriverInfo(CqDeviceInfo* info) {
    if (!info) return Fail(ERROR_INVALID_PARAMETER);
    std::lock_guard<std::mutex> lifecycle(g_lifecycleMutex);
    if (g_device == INVALID_HANDLE_VALUE) return Fail(ERROR_INVALID_HANDLE);
    auto request = CqHeader(sizeof(CqRequestHeader));
    CqDeviceInfo response{};
    DWORD returned = 0;
    DWORD error = IoControl(g_device, IOCTL_CQKEYS_QUERY_INFO, &request, sizeof(request),
        &response, sizeof(response), returned);
    if (!error && (returned != sizeof(response) || !CqValidHeader(response.header, sizeof(response)) ||
        !response.header.deviceEpoch || response.header.sessionId || response.reserved)) error = ERROR_INVALID_DATA;
    if (error) return Fail(error);
    *info = response;
    return Success();
}

BOOL __cdecl QueryInputState(CqDeviceState* state) {
    if (!state) return Fail(ERROR_INVALID_PARAMETER);
    std::lock_guard<std::mutex> lifecycle(g_lifecycleMutex);
    CqDeviceState snapshot{};
    DWORD error = ReadState(snapshot);
    if (error) return Fail(error);
    std::lock_guard<std::mutex> device(g_deviceMutex);
    *state = snapshot;
    if (state->header.deviceEpoch != g_epoch || state->header.sessionId != g_session ||
        state->phase != CQKEYS_SESSION_ACTIVE || !state->ready) g_acceptingInput = false;
    return Success();
}

BOOL __cdecl GetLastInputResult(CqInputResult* result) {
    if (!result || result->size != sizeof(*result) || result->version != CQ_INPUT_RESULT_ABI)
        return Fail(ERROR_INVALID_PARAMETER);
    *result = g_lastInput;
    return Success();
}

BOOL __cdecl LoadNTDriver(const char* name, const char* path) {
    std::lock_guard<std::mutex> lifecycle(g_lifecycleMutex);
    g_lastDriverStart = {sizeof(CqDriverStartResult), CQ_DRIVER_START_RESULT_ABI};
    auto fail = [](DWORD error) {
        g_lastDriverStart.error = error;
        return Fail(error);
    };
    if (!ValidateName(name)) return fail(GetLastError());
    if (g_device != INVALID_HANDLE_VALUE || g_cleanupRequired) return fail(ERROR_BUSY);
    if (!CqAcquireSession()) return fail(GetLastError());
    g_ownsSession = true;
    if (!CqInstallDevice(path, &g_lastDriverStart)) return fail(GetLastError());
    return Success();
}
BOOL __cdecl GetLastDriverStartResult(CqDriverStartResult* result) {
    if (!result || result->size != sizeof(*result) || result->version != CQ_DRIVER_START_RESULT_ABI)
        return Fail(ERROR_INVALID_PARAMETER);
    *result = g_lastDriverStart;
    return TRUE;
}
BOOL __cdecl StopNTDriver(const char* name) { return StopDriver(name, false); }
BOOL __cdecl UnloadNTDriver(const char* name) { return StopDriver(name, true); }
BOOL __cdecl SetHandle() {
    std::lock_guard<std::mutex> lifecycle(g_lifecycleMutex);
    if (g_cleanupRequired || g_stopRequests) return Fail(ERROR_BUSY);
    if (g_device != INVALID_HANDLE_VALUE) return g_acceptingInput ? Success() : Fail(ERROR_BUSY);
    if (!CqAcquireSession()) return Fail(GetLastError());
    g_ownsSession = true;
    HANDLE device = CqOpenInterface();
    if (device == INVALID_HANDLE_VALUE) return Fail(GetLastError());
    {
        std::lock_guard<std::mutex> state(g_deviceMutex);
        g_device = device;
    }
    auto request = CqHeader(sizeof(CqRequestHeader));
    CqDeviceInfo info{};
    DWORD returned = 0;
    DWORD error = IoControl(device, IOCTL_CQKEYS_QUERY_INFO, &request, sizeof(request), &info, sizeof(info), returned);
    if (!error && (returned != sizeof(info) || !CqValidHeader(info.header, sizeof(info)) ||
        !info.header.deviceEpoch || info.header.sessionId || info.reserved ||
        info.maxReportLength != CQKEYS_MAX_REPORT_LENGTH ||
        (info.capabilities & CQKEYS_CAPABILITIES) != CQKEYS_CAPABILITIES)) error = ERROR_REVISION_MISMATCH;
    if (!error && info.ready != 1) error = ERROR_NOT_READY;
    if (error) {
        if (!CloseHandle(device)) return Fail(GetLastError());
        std::lock_guard<std::mutex> state(g_deviceMutex);
        g_device = INVALID_HANDLE_VALUE;
        return Fail(error);
    }
    request = CqHeader(sizeof(request), info.header.deviceEpoch);
    CqRequestHeader response{};
    {
        std::lock_guard<std::mutex> state(g_deviceMutex);
        g_epoch = info.header.deviceEpoch;
        g_session = 0;
        g_beginAttempted = true;
    }
    error = IoControl(device, IOCTL_CQKEYS_BEGIN_SESSION, &request, sizeof(request), &response, sizeof(response), returned);
    if (!error && (returned != sizeof(response) || !CqValidHeader(response, sizeof(response)) ||
        response.deviceEpoch != info.header.deviceEpoch || !response.sessionId)) error = ERROR_INVALID_DATA;
    if (error) return Fail(error);
    {
        std::lock_guard<std::mutex> state(g_deviceMutex);
        g_session = response.sessionId;
        g_sequence = 0;
    }
    std::lock_guard<std::mutex> input(g_inputMutex);
    ClearInputState();
    {
        std::lock_guard<std::mutex> state(g_deviceMutex);
        if (g_stopRequests) return Fail(ERROR_NOT_READY);
        g_acceptingInput = true;
    }
    return QueryStatus();
}
BOOL __cdecl StopInputProcessing() {
    StopRequest stopping;
    std::lock_guard<std::mutex> lifecycle(g_lifecycleMutex);
    DWORD error = Quiesce();
    return error ? Fail(RetireDisconnectedDevice(error)) : Success();
}
void __cdecl CheckDeviceStatus() { std::lock_guard<std::mutex> lock(g_lifecycleMutex); QueryStatus(); }
DEVICE_STATUS __cdecl GetDriverStatus() { return g_status.load(); }
ULONGLONG __cdecl GetLastCheckTime() { return g_lastCheck.load(); }
HANDLE __cdecl GetDriverHandle() { std::lock_guard<std::mutex> lock(g_deviceMutex); return g_device; }
int __cdecl GetDetailedErrorCode() { return static_cast<int>(g_error.load()); }
void __cdecl KeyDown(USHORT key) { LegacyInput([=] { KeyboardInput(key, true); }); }
void __cdecl KeyUp(USHORT key) { LegacyInput([=] { KeyboardInput(key, false); }); }
void __cdecl ConsumerKeyDown(USHORT usage) { LegacyInput([=] { ConsumerInput(usage, true); }); }
void __cdecl ConsumerKeyUp(USHORT usage) { LegacyInput([=] { ConsumerInput(usage, false); }); }
void __cdecl MouseLeftButtonDown() { LegacyInput([] { MouseButton(1, true); }); }
void __cdecl MouseLeftButtonUp() { LegacyInput([] { MouseButton(1, false); }); }
void __cdecl MouseRightButtonDown() { LegacyInput([] { MouseButton(2, true); }); }
void __cdecl MouseRightButtonUp() { LegacyInput([] { MouseButton(2, false); }); }
void __cdecl MouseMiddleButtonDown() { LegacyInput([] { MouseButton(4, true); }); }
void __cdecl MouseMiddleButtonUp() { LegacyInput([] { MouseButton(4, false); }); }
void __cdecl MouseXButton1Down() { LegacyInput([] { MouseButton(8, true); }); }
void __cdecl MouseXButton1Up() { LegacyInput([] { MouseButton(8, false); }); }
void __cdecl MouseXButton2Down() { LegacyInput([] { MouseButton(16, true); }); }
void __cdecl MouseXButton2Up() { LegacyInput([] { MouseButton(16, false); }); }
void __cdecl MouseWheelUp(USHORT delta) { LegacyInput([=] { MouseWheel(delta, false); }); }
void __cdecl MouseWheelDown(USHORT delta) { LegacyInput([=] { MouseWheel(delta, true); }); }

void __cdecl MouseMoveRELATIVE(LONG deltaX, LONG deltaY) {
    LegacyInput([=] { MoveRelative(deltaX, deltaY); });
}

void __cdecl MouseMoveABSOLUTE(LONG screenX, LONG screenY) {
    LegacyInput([=] { MoveAbsolute(screenX, screenY); });
}

BOOL __cdecl SubmitInputTask(const CqInputAction* actions, uint32_t count, uint32_t repeats,
                            uint32_t repeatDelayMs, uint64_t* outId) {
    if (!outId) return Fail(ERROR_INVALID_PARAMETER);
    *outId = 0;
    if (!actions || !count || count > CQ_INPUT_TASK_MAX_ACTIONS || !repeats ||
        repeats > CQ_INPUT_TASK_MAX_REPEATS || repeatDelayMs > CQ_INPUT_TASK_MAX_DELAY_MS)
        return Fail(ERROR_INVALID_PARAMETER);
    std::lock_guard<std::mutex> lifecycle(g_lifecycleMutex);
    std::unique_lock<std::mutex> input(g_inputMutex, std::try_to_lock);
    if (!input.owns_lock()) return Fail(ERROR_BUSY);
    std::lock_guard<std::mutex> task(g_taskMutex);
    if (g_taskResult.taskId || HasHeldInput()) return Fail(ERROR_BUSY);
    if (!g_acceptingInput) return Fail(ERROR_NOT_READY);
    if (g_nextTaskId == UINT64_MAX) return Fail(ERROR_ARITHMETIC_OVERFLOW);
    for (uint32_t index = 0; index < count; ++index)
        if (!ValidAction(actions[index])) return Fail(ERROR_INVALID_PARAMETER);
    try { g_actions.assign(actions, actions + count); }
    catch (const std::bad_alloc&) { return Fail(ERROR_NOT_ENOUGH_MEMORY); }
    g_taskResult = {};
    g_taskResult.size = sizeof(g_taskResult);
    g_taskResult.version = CQ_INPUT_TASK_ABI;
    g_taskResult.state = CQ_INPUT_TASK_RUNNING;
    g_taskResult.taskId = ++g_nextTaskId;
    g_repeats = repeats;
    g_repeatDelay = repeatDelayMs;
    g_taskCancelled = false;
    g_taskActive = true;
    g_taskThread = CreateThread(nullptr, 0, TaskWorker, nullptr, 0, nullptr);
    if (!g_taskThread) {
        DWORD error = GetLastError();
        g_taskActive = false;
        g_taskResult = {};
        std::vector<CqInputAction>().swap(g_actions);
        return Fail(error);
    }
    *outId = g_taskResult.taskId;
    return Success();
}

BOOL __cdecl CancelInputTask(uint64_t taskId) {
    std::lock_guard<std::mutex> lifecycle(g_lifecycleMutex);
    {
        std::lock_guard<std::mutex> task(g_taskMutex);
        if (!taskId || g_taskResult.taskId != taskId) return Fail(ERROR_NOT_FOUND);
        if (g_taskResult.state != CQ_INPUT_TASK_RUNNING) return Success();
    }
    SignalTaskCancellation();
    DWORD error = CancelReport(true);
    return error ? Fail(error) : Success();
}

BOOL __cdecl WaitInputTask(uint64_t taskId, uint32_t timeoutMs) {
    std::unique_lock<std::mutex> task(g_taskMutex);
    if (!taskId || g_taskResult.taskId != taskId) return Fail(ERROR_NOT_FOUND);
    auto done = [=] { return g_taskResult.taskId != taskId || g_taskResult.state != CQ_INPUT_TASK_RUNNING; };
    if (timeoutMs == INFINITE) g_taskCondition.wait(task, done);
    else if (!g_taskCondition.wait_for(task, std::chrono::milliseconds(timeoutMs), done)) return Fail(WAIT_TIMEOUT);
    if (g_taskResult.taskId != taskId) return Fail(ERROR_NOT_FOUND);
    return Success();
}

BOOL __cdecl QueryInputTask(uint64_t taskId, CqInputTaskResult* result) {
    if (!result || result->size != sizeof(*result) || result->version != CQ_INPUT_TASK_ABI)
        return Fail(ERROR_INVALID_PARAMETER);
    std::lock_guard<std::mutex> task(g_taskMutex);
    if (!taskId || g_taskResult.taskId != taskId) return Fail(ERROR_NOT_FOUND);
    *result = g_taskResult;
    return Success();
}

BOOL __cdecl ReleaseInputTask(uint64_t taskId) {
    std::lock_guard<std::mutex> lifecycle(g_lifecycleMutex);
    {
        std::lock_guard<std::mutex> task(g_taskMutex);
        if (!taskId || g_taskResult.taskId != taskId) return Fail(ERROR_NOT_FOUND);
        if (g_taskResult.state == CQ_INPUT_TASK_RUNNING) return Fail(ERROR_BUSY);
    }
    DWORD error = JoinTask(true);
    return error ? Fail(error) : Success();
}

BOOL __cdecl CleanupInputRuntime() {
    StopRequest stopping;
    std::lock_guard<std::mutex> lifecycle(g_lifecycleMutex);
    if (!CloseDevice()) return FALSE;
    DWORD error = JoinTask(true);
    if (error) return Fail(error);
    if (g_ownsSession) {
        if (!CqReleaseSession()) return Fail(GetLastError());
        g_ownsSession = false;
    }
    g_cleanupRequired = false;
    return Success();
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }
