#pragma once
#ifdef _KERNEL_MODE
using uint8_t = UCHAR;
using int8_t = CHAR;
using uint16_t = USHORT;
using int16_t = SHORT;
using uint32_t = ULONG;
using int32_t = LONG;
#else
#include <stdint.h>
#endif

#define CQKEYS_PROTOCOL_VERSION 2u
#define CQKEYS_HARDWARE_ID L"Root\\cqkeys_vhf"
#define CQKEYS_INSTANCE_ID L"ROOT\\CQKEYS_VHF\\0000"
#define CQKEYS_DEVICE_TYPE 0x8000u
#define IOCTL_CQKEYS_STATUS CTL_CODE(CQKEYS_DEVICE_TYPE, 0x810, METHOD_BUFFERED, FILE_READ_ACCESS)
#define IOCTL_CQKEYS_REPORT CTL_CODE(CQKEYS_DEVICE_TYPE, 0x811, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define IOCTL_CQKEYS_RESET CTL_CODE(CQKEYS_DEVICE_TYPE, 0x812, METHOD_BUFFERED, FILE_WRITE_ACCESS)

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

struct CqReportRequest {
    uint32_t size;
    uint32_t version;
    uint32_t generation;
    uint32_t reportLength;
    uint8_t report[sizeof(CqKeyboardReport)];
    uint8_t reserved[3];
};

struct CqDeviceStatus {
    uint32_t size;
    uint32_t version;
    uint32_t generation;
    uint32_t ready;
    int32_t lastStatus;
};

static_assert(sizeof(CqKeyboardReport) == 9, "Keyboard wire layout");
static_assert(sizeof(CqMouseReport) == 7, "Mouse wire layout");
static_assert(sizeof(CqAbsoluteReport) == 5, "Absolute wire layout");
static_assert(sizeof(CqConsumerReport) == 3, "Consumer wire layout");
static_assert(sizeof(CqReportRequest) == 28, "IOCTL wire layout");
static_assert(sizeof(CqDeviceStatus) == 20, "Status wire layout");

inline bool CqValidReport(const CqReportRequest& request) {
    if (request.size != sizeof(request) || request.version != CQKEYS_PROTOCOL_VERSION ||
        request.reportLength == 0 || request.reportLength > sizeof(request.report)) return false;
    for (auto value : request.reserved) if (value != 0) return false;
    for (uint32_t index = request.reportLength; index < sizeof(request.report); ++index)
        if (request.report[index] != 0) return false;
    switch (request.report[0]) {
    case CQKEYS_KEYBOARD_ID: {
        if (request.reportLength != sizeof(CqKeyboardReport)) return false;
        const auto& keyboard = *reinterpret_cast<const CqKeyboardReport*>(request.report);
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
        const auto& mouse = *reinterpret_cast<const CqMouseReport*>(request.report);
        return mouse.buttons <= 31 && mouse.x != -32768 && mouse.y != -32768 && mouse.wheel != -128;
    }
    case CQKEYS_ABSOLUTE_ID: {
        if (request.reportLength != sizeof(CqAbsoluteReport)) return false;
        const auto& absolute = *reinterpret_cast<const CqAbsoluteReport*>(request.report);
        return absolute.x <= 32767 && absolute.y <= 32767;
    }
    case CQKEYS_CONSUMER_ID:
        return request.reportLength == sizeof(CqConsumerReport) &&
            reinterpret_cast<const CqConsumerReport*>(request.report)->usage <= 0x03ff;
    default:
        return false;
    }
}
