#pragma once
#ifdef _KERNEL_MODE
#include <ntddk.h>
typedef UINT8 uint8_t;
typedef UINT16 uint16_t;
typedef UINT32 uint32_t;
typedef INT16 int16_t;
typedef INT32 int32_t;
#else
#include <stdint.h>
#endif

// Fixed-size wire protocol. Input requires write access; status requires read.
#define CQ_PROTOCOL_VERSION 2u
#define CQ_DEVICE_TYPE 0x8000u
#define CQ_IOCTL(function, access) ((CQ_DEVICE_TYPE << 16) | ((access) << 14) | ((function) << 2))
#define IOCTL_KEYBOARD CQ_IOCTL(0x800u, 2u)
#define IOCTL_MOUSE CQ_IOCTL(0x801u, 2u)
#define IOCTL_CQ_STATUS CQ_IOCTL(0x802u, 1u)
#define IOCTL_CQ_RESET CQ_IOCTL(0x803u, 2u)

#define KEYMOUSE_WIN32_DEVICE_NAME L"\\\\.\\cqkeys"
#define KEYMOUSE_DEVICE_NAME L"\\Device\\cqkeys"
#define KEYMOUSE_DOS_DEVICE_NAME L"\\DosDevices\\cqkeys"
#define KEYMOUSE_DRIVER_NAME L"cqkeys"

#define KEY_MAKE 0
#define KEY_BREAK 1
#define KEY_E0 2
#define KEY_E1 4
#define KEY_DOWN KEY_MAKE
#define KEY_UP KEY_BREAK
#define MOUSE_MOVE_RELATIVE 0
#define MOUSE_MOVE_ABSOLUTE 1
#ifndef MOUSE_VIRTUAL_DESKTOP
#define MOUSE_VIRTUAL_DESKTOP 2
#endif
#define MOUSE_LEFT_BUTTON_DOWN 0x0001
#define MOUSE_LEFT_BUTTON_UP 0x0002
#define MOUSE_RIGHT_BUTTON_DOWN 0x0004
#define MOUSE_RIGHT_BUTTON_UP 0x0008
#define MOUSE_MIDDLE_BUTTON_DOWN 0x0010
#define MOUSE_MIDDLE_BUTTON_UP 0x0020
#define MOUSE_BUTTON_4_DOWN 0x0040
#define MOUSE_BUTTON_4_UP 0x0080
#define MOUSE_BUTTON_5_DOWN 0x0100
#define MOUSE_BUTTON_5_UP 0x0200
#define MOUSE_WHEEL 0x0400
#define MOUSE_HWHEEL 0x0800

typedef struct CQ_KEYBOARD_INPUT {
    uint16_t UnitId;
    uint16_t MakeCode;
    uint16_t Flags;
    uint16_t Reserved;
    uint32_t ExtraInformation;
} CQ_KEYBOARD_INPUT;

typedef struct CQ_MOUSE_INPUT {
    uint16_t UnitId;
    uint16_t Flags;
    uint16_t ButtonFlags;
    uint16_t ButtonData;
    uint32_t RawButtons;
    int32_t LastX;
    int32_t LastY;
    uint32_t ExtraInformation;
} CQ_MOUSE_INPUT;

#define CQ_STATUS_ONLINE 1u
#define CQ_STATUS_READY 2u
#define CQ_REPORT_QUEUE_CAPACITY 64u

typedef struct CQ_DRIVER_STATUS {
    uint32_t Size;
    uint32_t Version;
    uint32_t Flags;
    uint32_t QueuedReports;
    uint32_t QueueCapacity;
    int32_t LastSubmitStatus;
} CQ_DRIVER_STATUS;

#ifdef __cplusplus
static_assert(sizeof(CQ_KEYBOARD_INPUT) == 12, "keyboard ABI");
static_assert(sizeof(CQ_MOUSE_INPUT) == 24, "mouse ABI");
static_assert(sizeof(CQ_DRIVER_STATUS) == 24, "status ABI");
#endif
