#pragma once
#include <windows.h>
#include <stdint.h>
#include <stddef.h>
#include <winioctl.h>
#include "../shared/cqkeys_protocol.h"

#ifdef CQKEYSDLL_EXPORTS
#define CQKEYSDLL_API __declspec(dllexport)
#else
#define CQKEYSDLL_API __declspec(dllimport)
#endif

enum DEVICE_STATUS {
    DEVICE_STATUS_UNKNOWN = 0,
    DEVICE_STATUS_READY = 1,
    DEVICE_STATUS_ERROR = 2
};

enum CQ_INPUT_ACTION_TYPE : uint32_t {
    CQ_INPUT_KEY_DOWN = 1,
    CQ_INPUT_KEY_UP = 2,
    CQ_INPUT_CONSUMER_DOWN = 3,
    CQ_INPUT_CONSUMER_UP = 4,
    CQ_INPUT_MOVE_RELATIVE = 5,
    CQ_INPUT_MOVE_ABSOLUTE = 6,
    CQ_INPUT_BUTTONS = 7,
    CQ_INPUT_WHEEL = 8,
    CQ_INPUT_WAIT = 9
};

enum CQ_INPUT_TASK_STATE : uint32_t {
    CQ_INPUT_TASK_RUNNING = 1,
    CQ_INPUT_TASK_COMPLETED = 2,
    CQ_INPUT_TASK_CANCELLED = 3,
    CQ_INPUT_TASK_FAILED = 4
};

enum : uint32_t {
    CQ_INPUT_TASK_ABI = 1,
    CQ_INPUT_TASK_MAX_ACTIONS = 4096,
    CQ_INPUT_TASK_MAX_REPEATS = 65535,
    CQ_INPUT_TASK_MAX_DELAY_MS = 60000,
    CQ_INPUT_MAX_MOVE_REPORTS = 256
};

enum : uint32_t {
    CQ_DRIVER_START_RESULT_ABI = 1
};

enum : uint32_t {
    CQ_INPUT_RESULT_ABI = 1,
    CQ_INPUT_RESULT_PARTIAL = 1,
    CQ_INPUT_RESULT_UNCERTAIN = 2,
    CQ_INPUT_RESULT_CANCELLED = 4
};

#pragma pack(push, 8)
struct CqDriverStartResult {
    uint32_t size;
    uint32_t version;
    uint32_t error;
    uint32_t problemCode;
    int32_t problemStatus;
    uint32_t diagnosticError;
};

struct CqInputAction {
    uint32_t type;
    int32_t value1;
    int32_t value2;
    uint32_t delayMs;
};

struct CqInputResult {
    uint32_t size;
    uint32_t version;
    uint32_t error;
    uint32_t flags;
    uint64_t confirmedReports;
    int64_t confirmedX;
    int64_t confirmedY;
    uint64_t lastSequence;
};

struct CqInputTaskResult {
    uint32_t size;
    uint32_t version;
    uint32_t state;
    uint32_t error;
    uint64_t taskId;
    uint64_t completedActions;
    uint32_t completedRepeats;
    uint32_t cleanupError;
    uint64_t lastAcceptedSequence;
    uint64_t lastSubmittedSequence;
    uint64_t deviceEpoch;
    uint64_t sessionId;
    uint32_t sessionPhase;
    uint32_t pendingCleanup;
    int32_t inputStatus;
    int32_t cleanupStatus;
    uint32_t resultFlags;
    uint32_t reserved;
};
#pragma pack(pop)

static_assert(sizeof(CqDriverStartResult) == 24 && offsetof(CqDriverStartResult, problemStatus) == 16,
    "Driver start result ABI");
static_assert(sizeof(CqInputAction) == 16, "Input action ABI");
static_assert(sizeof(CqInputResult) == 48, "Input result ABI");
static_assert(sizeof(CqInputTaskResult) == 96 && offsetof(CqInputTaskResult, taskId) == 16 &&
    offsetof(CqInputTaskResult, resultFlags) == 88,
    "Input task result ABI");

extern "C" {
CQKEYSDLL_API DWORD __cdecl GetLifecycleVersion();
CQKEYSDLL_API BOOL __cdecl QueryDriverInfo(CqDeviceInfo* info);
CQKEYSDLL_API BOOL __cdecl QueryInputState(CqDeviceState* state);
CQKEYSDLL_API BOOL __cdecl GetLastInputResult(CqInputResult* result);
CQKEYSDLL_API BOOL __cdecl SubmitInputTask(const CqInputAction* actions, uint32_t count,
    uint32_t repeats, uint32_t repeatDelayMs, uint64_t* outId);
CQKEYSDLL_API BOOL __cdecl CancelInputTask(uint64_t taskId);
CQKEYSDLL_API BOOL __cdecl WaitInputTask(uint64_t taskId, uint32_t timeoutMs);
CQKEYSDLL_API BOOL __cdecl QueryInputTask(uint64_t taskId, CqInputTaskResult* result);
CQKEYSDLL_API BOOL __cdecl ReleaseInputTask(uint64_t taskId);
CQKEYSDLL_API BOOL __cdecl CleanupInputRuntime();
CQKEYSDLL_API BOOL __cdecl LoadNTDriver(const char* name, const char* path);
CQKEYSDLL_API BOOL __cdecl GetLastDriverStartResult(CqDriverStartResult* result);
CQKEYSDLL_API BOOL __cdecl StopNTDriver(const char* name);
CQKEYSDLL_API BOOL __cdecl UnloadNTDriver(const char* name);
CQKEYSDLL_API BOOL __cdecl UnloadNTDriverAndPackage(const char* name);
CQKEYSDLL_API BOOL __cdecl SetHandle();
CQKEYSDLL_API BOOL __cdecl StopInputProcessing();
CQKEYSDLL_API void __cdecl CheckDeviceStatus();
CQKEYSDLL_API DEVICE_STATUS __cdecl GetDriverStatus();
CQKEYSDLL_API ULONGLONG __cdecl GetLastCheckTime();
CQKEYSDLL_API HANDLE __cdecl GetDriverHandle();
CQKEYSDLL_API int __cdecl GetDetailedErrorCode();
CQKEYSDLL_API void __cdecl KeyDown(USHORT virtualKey);
CQKEYSDLL_API void __cdecl KeyUp(USHORT virtualKey);
CQKEYSDLL_API void __cdecl ConsumerKeyDown(USHORT usage);
CQKEYSDLL_API void __cdecl ConsumerKeyUp(USHORT usage);
CQKEYSDLL_API void __cdecl MouseMoveRELATIVE(LONG deltaX, LONG deltaY);
CQKEYSDLL_API void __cdecl MouseMoveABSOLUTE(LONG screenX, LONG screenY);
CQKEYSDLL_API void __cdecl MouseLeftButtonDown();
CQKEYSDLL_API void __cdecl MouseLeftButtonUp();
CQKEYSDLL_API void __cdecl MouseRightButtonDown();
CQKEYSDLL_API void __cdecl MouseRightButtonUp();
CQKEYSDLL_API void __cdecl MouseMiddleButtonDown();
CQKEYSDLL_API void __cdecl MouseMiddleButtonUp();
CQKEYSDLL_API void __cdecl MouseXButton1Down();
CQKEYSDLL_API void __cdecl MouseXButton1Up();
CQKEYSDLL_API void __cdecl MouseXButton2Down();
CQKEYSDLL_API void __cdecl MouseXButton2Up();
CQKEYSDLL_API void __cdecl MouseWheelUp(USHORT wheelDelta);
CQKEYSDLL_API void __cdecl MouseWheelDown(USHORT wheelDelta);
}
