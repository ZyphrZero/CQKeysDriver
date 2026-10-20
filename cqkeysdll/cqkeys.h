#pragma once
#include <windows.h>
#include "../shared/cqkeys_protocol.h"
#ifdef CQKEYSDLL_EXPORTS
#define CQKEYSDLL_API __declspec(dllexport)
#else
#define CQKEYSDLL_API __declspec(dllimport)
#endif
#define CQ_API extern "C" CQKEYSDLL_API
CQ_API BOOL __cdecl ConnectDevice();
CQ_API BOOL __cdecl DisconnectDevice();
CQ_API BOOL __cdecl ResetInput();
CQ_API ULONG __cdecl GetProtocolVersion();
CQ_API BOOL __cdecl CheckDeviceStatus();
CQ_API int __cdecl GetDriverStatus();
CQ_API ULONGLONG __cdecl GetLastCheckTime();
CQ_API int __cdecl GetDetailedErrorCode();
CQ_API BOOL __cdecl KeyDown(USHORT key);
CQ_API BOOL __cdecl KeyUp(USHORT key);
CQ_API BOOL __cdecl MouseMoveRELATIVE(LONG x, LONG y);
CQ_API BOOL __cdecl MouseMoveABSOLUTE(LONG x, LONG y);
CQ_API BOOL __cdecl MouseLeftButtonDown();
CQ_API BOOL __cdecl MouseLeftButtonUp();
CQ_API BOOL __cdecl MouseRightButtonDown();
CQ_API BOOL __cdecl MouseRightButtonUp();
CQ_API BOOL __cdecl MouseMiddleButtonDown();
CQ_API BOOL __cdecl MouseMiddleButtonUp();
CQ_API BOOL __cdecl MouseXButton1Down();
CQ_API BOOL __cdecl MouseXButton1Up();
CQ_API BOOL __cdecl MouseXButton2Down();
CQ_API BOOL __cdecl MouseXButton2Up();
CQ_API BOOL __cdecl MouseWheelUp(USHORT delta);
CQ_API BOOL __cdecl MouseWheelDown(USHORT delta);
