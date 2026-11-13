#pragma once
#ifdef _KERNEL_MODE
using uint8_t = UCHAR;
using int8_t = CHAR;
using uint16_t = USHORT;
using int16_t = SHORT;
using uint32_t = ULONG;
using int32_t = LONG;
using uint64_t = ULONGLONG;
#else
#include <stdint.h>
#endif

#include <stddef.h>
#include <string.h>

#define CQKEYS_LIFECYCLE_ABI 2u
#define CQKEYS_PROTOCOL_MAJOR 3u
#define CQKEYS_PROTOCOL_MINOR 0u
#define CQKEYS_DRIVER_MAJOR 3u
#define CQKEYS_DRIVER_MINOR 0u
#define CQKEYS_DRIVER_PATCH 0u
#define CQKEYS_DRIVER_BUILD 1001u
#define CQKEYS_HARDWARE_ID L"Root\\cqkeys_vhf"
#define CQKEYS_INSTANCE_ID L"ROOT\\CQKEYS_VHF\\0000"
#define CQKEYS_DEVICE_TYPE 0x8000u
#define IOCTL_CQKEYS_QUERY_INFO CTL_CODE(CQKEYS_DEVICE_TYPE, 0x820, METHOD_BUFFERED, FILE_READ_ACCESS)
#define IOCTL_CQKEYS_BEGIN_SESSION CTL_CODE(CQKEYS_DEVICE_TYPE, 0x821, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define IOCTL_CQKEYS_SUBMIT_REPORT CTL_CODE(CQKEYS_DEVICE_TYPE, 0x822, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define IOCTL_CQKEYS_STOP_SESSION CTL_CODE(CQKEYS_DEVICE_TYPE, 0x823, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define IOCTL_CQKEYS_QUERY_STATE CTL_CODE(CQKEYS_DEVICE_TYPE, 0x824, METHOD_BUFFERED, FILE_READ_ACCESS)

static const GUID GUID_DEVINTERFACE_CQKEYS =
    {0x749cbb36, 0x7e54, 0x4774, {0xb4, 0x08, 0xb6, 0x57, 0xe8, 0x63, 0x93, 0x31}};

enum : uint8_t {
    CQKEYS_KEYBOARD_ID = 1,
    CQKEYS_MOUSE_ID = 2,
    CQKEYS_ABSOLUTE_ID = 3,
    CQKEYS_CONSUMER_ID = 4
};

#pragma pack(push, 1)
struct CqKeyboardReport {
    uint8_t id;
    uint8_t modifiers;
    uint8_t reserved;
    uint8_t keys[6];
};

struct CqMouseReport {
    uint8_t id;
    uint8_t buttons;
    int16_t x;
    int16_t y;
    int8_t wheel;
};

struct CqAbsoluteReport {
    uint8_t id;
    uint16_t x;
    uint16_t y;
};

struct CqConsumerReport {
    uint8_t id;
    uint16_t usage;
};
#pragma pack(pop)

enum : uint32_t {
    CQKEYS_MAX_REPORT_LENGTH = sizeof(CqKeyboardReport),
    CQKEYS_CAP_KEYBOARD = 1, CQKEYS_CAP_MOUSE = 2, CQKEYS_CAP_ABSOLUTE = 4,
    CQKEYS_CAP_CONSUMER = 8, CQKEYS_CAP_STOP_BARRIER = 16,
    CQKEYS_CAPABILITIES = 31,
    CQKEYS_CLEANUP_KEYBOARD = 1, CQKEYS_CLEANUP_MOUSE = 2, CQKEYS_CLEANUP_CONSUMER = 4,
    CQKEYS_CLEANUP_ALL = 7,
    CQKEYS_SESSION_CONNECTED = 0, CQKEYS_SESSION_STARTING = 1, CQKEYS_SESSION_ACTIVE = 2,
    CQKEYS_SESSION_STOPPING = 3, CQKEYS_SESSION_STOPPED = 4, CQKEYS_SESSION_CLEANUP_FAILED = 5,
    CQKEYS_SESSION_INVALIDATED = 6, CQKEYS_SESSION_CLOSED = 7,
    CQKEYS_STOP_EXPLICIT = 1, CQKEYS_STOP_FILE = 2, CQKEYS_STOP_POWER = 3,
    CQKEYS_STOP_REMOVAL = 4, CQKEYS_STOP_INPUT_ERROR = 5
};

#pragma pack(push, 8)
struct CqRequestHeader {
    uint32_t size;
    uint16_t major;
    uint16_t minor;
    uint64_t deviceEpoch;
    uint64_t sessionId;
};

struct CqReportRequest {
    CqRequestHeader header;
    uint64_t sequence;
    uint32_t reportLength;
    uint32_t reserved;
    uint8_t report[16];
};

struct CqSubmitResult {
    CqRequestHeader header;
    uint64_t sequence;
};

struct CqDeviceInfo {
    CqRequestHeader header;
    uint32_t driverMajor;
    uint32_t driverMinor;
    uint32_t driverPatch;
    uint32_t driverBuild;
    uint32_t capabilities;
    uint32_t maxReportLength;
    uint32_t ready;
    uint32_t reserved;
};

struct CqDeviceState {
    CqRequestHeader header;
    uint32_t phase;
    uint32_t ready;
    uint32_t pendingCleanup;
    uint32_t stopReason;
    uint64_t lastAcceptedSequence;
    uint64_t lastSubmittedSequence;
    int32_t inputStatus;
    int32_t cleanupStatus;
    int32_t firstCleanupStatus;
    uint32_t reserved;
    CqKeyboardReport keyboard;
    uint8_t buttons;
    uint16_t consumer;
    uint32_t reservedState;
};
#pragma pack(pop)

inline CqRequestHeader CqHeader(uint32_t size, uint64_t epoch = 0, uint64_t session = 0) {
    return {size, CQKEYS_PROTOCOL_MAJOR, CQKEYS_PROTOCOL_MINOR, epoch, session};
}

inline bool CqValidHeader(const CqRequestHeader& header, uint32_t size) {
    return header.size == size && header.major == CQKEYS_PROTOCOL_MAJOR &&
        header.minor == CQKEYS_PROTOCOL_MINOR;
}

static_assert(sizeof(CqKeyboardReport) == 9, "Keyboard wire layout");
static_assert(sizeof(CqMouseReport) == 7, "Mouse wire layout");
static_assert(sizeof(CqAbsoluteReport) == 5, "Absolute wire layout");
static_assert(sizeof(CqConsumerReport) == 3, "Consumer wire layout");
static_assert(sizeof(CqRequestHeader) == 24 && offsetof(CqRequestHeader, deviceEpoch) == 8, "Header wire layout");
static_assert(sizeof(CqReportRequest) == 56 && offsetof(CqReportRequest, report) == 40, "Report wire layout");
static_assert(sizeof(CqSubmitResult) == 32, "Receipt wire layout");
static_assert(sizeof(CqDeviceInfo) == 56, "Info wire layout");
static_assert(sizeof(CqDeviceState) == 88 && offsetof(CqDeviceState, keyboard) == 72, "State wire layout");

inline bool CqValidReport(const CqReportRequest& request) {
    if (!CqValidHeader(request.header, sizeof(request)) || request.sequence == 0 || request.reserved != 0 ||
        request.reportLength == 0 || request.reportLength > sizeof(request.report)) return false;
    for (uint32_t index = request.reportLength; index < sizeof(request.report); ++index)
        if (request.report[index] != 0) return false;
    switch (request.report[0]) {
    case CQKEYS_KEYBOARD_ID: {
        if (request.reportLength != sizeof(CqKeyboardReport)) return false;
        CqKeyboardReport keyboard;
        memcpy(&keyboard, request.report, sizeof(keyboard));
        if (keyboard.reserved != 0) return false;
        for (unsigned index = 0; index < 6; ++index) {
            const auto key = keyboard.keys[index];
            if (key == 0) continue;
            if (key < 4 || key > 0x73) return false;
            for (unsigned previous = 0; previous < index; ++previous)
                if (keyboard.keys[previous] == key) return false;
        }
        return true;
    }
    case CQKEYS_MOUSE_ID: {
        if (request.reportLength != sizeof(CqMouseReport)) return false;
        CqMouseReport mouse;
        memcpy(&mouse, request.report, sizeof(mouse));
        return mouse.buttons <= 31 && mouse.x != -32768 && mouse.y != -32768 && mouse.wheel != -128;
    }
    case CQKEYS_ABSOLUTE_ID: {
        if (request.reportLength != sizeof(CqAbsoluteReport)) return false;
        CqAbsoluteReport absolute;
        memcpy(&absolute, request.report, sizeof(absolute));
        return absolute.x <= 32767 && absolute.y <= 32767;
    }
    case CQKEYS_CONSUMER_ID: {
        if (request.reportLength != sizeof(CqConsumerReport)) return false;
        CqConsumerReport consumer;
        memcpy(&consumer, request.report, sizeof(consumer));
        return consumer.usage <= 0x03ff;
    }
    default:
        return false;
    }
}
