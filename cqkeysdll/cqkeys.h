#pragma once
#include <windows.h>

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

extern "C" {
CQKEYSDLL_API DWORD __cdecl GetLifecycleVersion();
CQKEYSDLL_API BOOL __cdecl LoadNTDriver(const char* name, const char* path);
CQKEYSDLL_API BOOL __cdecl StopNTDriver(const char* name);
CQKEYSDLL_API BOOL __cdecl UnloadNTDriver(const char* name);
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
