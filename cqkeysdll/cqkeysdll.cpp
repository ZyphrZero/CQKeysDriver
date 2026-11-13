#include "pch.h"
#include "cqkeys.h"
#include <windows.h>
#include <winsvc.h>  
#include <conio.h>  
#include <stdio.h>
#include <winternl.h>
#include <atomic>
#include <algorithm>
#include <sysinfoapi.h>
#include <time.h>
#include <cmath>
#include <mutex>
#include <array>
#include <vector>
#include <string>

// 设备状态枚举扩展
typedef enum _DEVICE_STATUS {
	DEVICE_STATUS_UNKNOWN = 0,	// 未知状态
	DEVICE_STATUS_READY = 1,	// 准备就绪
	DEVICE_STATUS_ERROR = 2,	// 错误状态
	DEVICE_STATUS_NO_KEYBOARD = 3,  // 无法找到键盘设备
	DEVICE_STATUS_NO_MOUSE = 4,     // 无法找到鼠标设备
	DEVICE_STATUS_INIT_FAILED = 5   // 初始化失败
} DEVICE_STATUS;

// 设备状态结构体
typedef struct _DEVICE_STATE_INFO {
	std::atomic<DEVICE_STATUS> status;
	std::atomic<ULONGLONG> lastCheckTime;
	std::atomic<int> detailedErrorCode;   // 详细错误代码
} DEVICE_STATE_INFO;

// 全局状态变量
namespace {
	DEVICE_STATE_INFO g_DriverState = { DEVICE_STATUS_UNKNOWN, 0, 0 };
}

// 获取系统时间的辅助函数
__forceinline ULONGLONG GetSystemTimeMs() {
	FILETIME ft;
	ULARGE_INTEGER uli;
	
	// 直接获取系统时间，精度更高
	GetSystemTimeAsFileTime(&ft);
	
	// 转换为64位整数以便计算
	uli.LowPart = ft.dwLowDateTime;
	uli.HighPart = ft.dwHighDateTime;
	
	// 从1601年到1970年的100纳秒数
	// Windows文件时间从1601年开始
	// Unix时间戳从1970年开始
	// 需要减去这段差值
	const ULONGLONG EPOCH_DIFFERENCE = 116444736000000000ull;
	
	// 转换为Unix时间戳（毫秒）
	// 1. 减去时间差
	// 2. 除以10000转换为毫秒（因为FILETIME是100纳秒为单位）
	return (uli.QuadPart - EPOCH_DIFFERENCE) / 10000;
}

#ifdef _MANAGED
#pragma managed(push, off)
#endif


// 装载NT驱动程序
HANDLE drvhandle = INVALID_HANDLE_VALUE;
namespace {
    // Lock order: lifecycle -> device. Every exported input call holds device
    // until all of its synchronous IOCTLs have returned.
    std::mutex g_lifecycleMutex;
    std::mutex g_deviceMutex;
    std::atomic<bool> g_acceptingInput{false};
    std::array<KEYBOARD_INPUT_DATA, 256> g_pressedKeys{};
    std::array<bool, 256> g_keyDown{};
    USHORT g_pressedMouse = 0;

    BOOL Fail(DWORD error) {
        g_DriverState.detailedErrorCode = error;
        SetLastError(error);
        return FALSE;
    }

    BOOL SendInputRequest(DWORD code, LPVOID input, DWORD size, LPVOID output,
                          DWORD outputSize, LPDWORD returned, LPOVERLAPPED overlapped) {
        if (!g_acceptingInput.load()) return Fail(ERROR_OPERATION_ABORTED);
        if (!DeviceIoControl(drvhandle, code, input, size, output, outputSize, returned, overlapped))
            return Fail(GetLastError());
        if (code == IOCTL_KEYBOARD) {
            const auto& key = *static_cast<KEYBOARD_INPUT_DATA*>(input);
            if (key.MakeCode < g_keyDown.size()) {
                g_keyDown[key.MakeCode] = !(key.Flags & KEY_BREAK);
                g_pressedKeys[key.MakeCode] = key;
            }
        } else if (code == IOCTL_MOUSE) {
            USHORT flags = static_cast<MOUSE_INPUT_DATA*>(input)->ButtonFlags;
            for (USHORT down = 1; down <= MOUSE_BUTTON_5_DOWN; down <<= 2) {
                if (flags & down) g_pressedMouse |= down;
                if (flags & (down << 1)) g_pressedMouse &= ~down;
            }
        }
        SetLastError(ERROR_SUCCESS);
        return TRUE;
    }

    // Called with device locked and input disabled. Retain failed releases so
    // the caller can retry; never close the handle and report a false success.
    BOOL ReleasePressedInput() {
        DWORD firstError = ERROR_SUCCESS, returned = 0;
        for (size_t i = 0; i < g_keyDown.size(); ++i) {
            if (!g_keyDown[i]) continue;
            auto key = g_pressedKeys[i];
            key.Flags |= KEY_BREAK;
            if (DeviceIoControl(drvhandle, IOCTL_KEYBOARD, &key, sizeof(key), nullptr, 0, &returned, nullptr))
                g_keyDown[i] = false;
            else if (!firstError) firstError = GetLastError();
        }
        for (USHORT down = 1; down <= MOUSE_BUTTON_5_DOWN; down <<= 2) {
            if (!(g_pressedMouse & down)) continue;
            MOUSE_INPUT_DATA mouse{};
            mouse.ButtonFlags = down << 1;
            if (DeviceIoControl(drvhandle, IOCTL_MOUSE, &mouse, sizeof(mouse), nullptr, 0, &returned, nullptr))
                g_pressedMouse &= ~down;
            else if (!firstError) firstError = GetLastError();
        }
        if (firstError) return Fail(firstError);
        SetLastError(ERROR_SUCCESS);
        return TRUE;
    }

    BOOL CloseDevice() {
        g_acceptingInput = false;
        std::lock_guard<std::mutex> lock(g_deviceMutex);
        g_DriverState.status = DEVICE_STATUS_UNKNOWN;
        if (!ReleasePressedInput()) return FALSE;
        if (drvhandle != INVALID_HANDLE_VALUE && drvhandle != nullptr) {
            if (!CloseHandle(drvhandle)) return Fail(GetLastError());
            drvhandle = INVALID_HANDLE_VALUE;
        }
        return TRUE;
    }
}

extern "C" __declspec(dllexport) DWORD __cdecl GetLifecycleVersion() { return 1; }
extern "C" __declspec(dllexport) BOOL __cdecl StopNTDriver(char* name);
extern "C" __declspec(dllexport) BOOL __cdecl StopInputProcessing() {
    std::lock_guard<std::mutex> lifecycle(g_lifecycleMutex);
    g_acceptingInput = false;
    std::lock_guard<std::mutex> device(g_deviceMutex);
    g_DriverState.status = DEVICE_STATUS_UNKNOWN;
    return ReleasePressedInput();
}
extern "C" __declspec(dllexport) BOOL __cdecl SetHandle();
// extern "C" __declspec(dllexport) void __cdecl SetHandle(HANDLE handle);
extern "C" __declspec(dllexport) void __cdecl KeyDown(USHORT VirtualKey);
extern "C" __declspec(dllexport) void __cdecl KeyUp(USHORT VirtualKey);
extern "C" __declspec(dllexport) void __cdecl MouseLeftButtonDown();
extern "C" __declspec(dllexport) void __cdecl MouseLeftButtonUp();
extern "C" __declspec(dllexport) void __cdecl MouseRightButtonDown();
extern "C" __declspec(dllexport) void __cdecl MouseRightButtonUp();
extern "C" __declspec(dllexport) void __cdecl MouseMiddleButtonDown();
extern "C" __declspec(dllexport) void __cdecl MouseMiddleButtonUp();
extern "C" __declspec(dllexport) void __cdecl MouseMoveRELATIVE(LONG dx, LONG dy);
extern "C" __declspec(dllexport) void __cdecl MouseMoveABSOLUTE(LONG dx, LONG dy);
extern "C" __declspec(dllexport) BOOL __cdecl LoadNTDriver(char* lpszDriverName, char* lpszDriverPath);
extern "C" __declspec(dllexport) BOOL __cdecl UnloadNTDriver(char* szSvrName);
extern "C" __declspec(dllexport) void __cdecl MouseXButton1Down();
extern "C" __declspec(dllexport) void __cdecl MouseXButton1Up();
extern "C" __declspec(dllexport) void __cdecl MouseXButton2Down();
extern "C" __declspec(dllexport) void __cdecl MouseXButton2Up();
extern "C" __declspec(dllexport) void __cdecl MouseWheelUp(USHORT wheelDelta);
extern "C" __declspec(dllexport) void __cdecl MouseWheelDown(USHORT wheelDelta);

// 检查驱动状态
DEVICE_STATUS CheckDriverStatus(HANDLE deviceHandle) 
{
	if (deviceHandle == INVALID_HANDLE_VALUE || deviceHandle == NULL) {
		printf("Error: Invalid device handle\n");
		g_DriverState.detailedErrorCode = 1001; // 句柄无效错误
        SetLastError(ERROR_INVALID_HANDLE);
		return DEVICE_STATUS_ERROR;
	}

	// 尝试与驱动通信
	DWORD bytesReturned = 0;
	DWORD lastError = 0;
	
	// 先尝试检查键盘设备
	bool keyboardAvailable = DeviceIoControl(deviceHandle, 
		IOCTL_KEYBOARD,
		NULL, 0,
		NULL, 0,
		&bytesReturned,
		NULL);
	
	if (!keyboardAvailable) {
		lastError = GetLastError();
		g_DriverState.detailedErrorCode = 2001; // 键盘设备不可用
		printf("Error: Keyboard device not available, error code: %d\n", lastError);
	}
	
	// 再检查鼠标设备
	bool mouseAvailable = DeviceIoControl(deviceHandle, 
		IOCTL_MOUSE,
		NULL, 0,
		NULL, 0,
		&bytesReturned,
		NULL);
	
	if (!mouseAvailable) {
		lastError = GetLastError();
		g_DriverState.detailedErrorCode = 3001; // 鼠标设备不可用
		printf("Error: Mouse device not available, error code: %d\n", lastError);
	}
	
    SetLastError(lastError);
	// 根据检查结果返回不同状态
	if (!keyboardAvailable && !mouseAvailable) {
		return DEVICE_STATUS_NO_KEYBOARD; // 键盘和鼠标设备都不可用，报告键盘不可用
	} else if (!keyboardAvailable) {
		return DEVICE_STATUS_NO_KEYBOARD; // 只有键盘设备不可用
	} else if (!mouseAvailable) {
		return DEVICE_STATUS_NO_MOUSE; // 只有鼠标设备不可用
	}

	g_DriverState.detailedErrorCode = 0; // 无错误
	return DEVICE_STATUS_READY; // 键盘和鼠标设备都可用
}

// 导出函数：检查驱动状态
extern "C" __declspec(dllexport) void __cdecl CheckDeviceStatus()
{
    std::lock_guard<std::mutex> lock(g_deviceMutex);
    g_DriverState.status = g_acceptingInput ? CheckDriverStatus(drvhandle) : DEVICE_STATUS_UNKNOWN;
    g_DriverState.lastCheckTime = GetSystemTimeMs();
}

// 导出函数：获取驱动状态
extern "C" __declspec(dllexport) DEVICE_STATUS __cdecl GetDriverStatus()
{
	return g_DriverState.status;
}

// 导出函数：获取上次检查时间
extern "C" __declspec(dllexport) ULONGLONG __cdecl GetLastCheckTime()
{
	return g_DriverState.lastCheckTime;
}

// 导出函数：获取驱动句柄
extern "C" __declspec(dllexport) HANDLE __cdecl GetDriverHandle()
{
	std::lock_guard<std::mutex> lock(g_deviceMutex);
	return drvhandle; // Borrowed handle: callers must not close it or use it for concurrent I/O.
}

// 添加导出函数：获取详细错误代码
extern "C" __declspec(dllexport) int __cdecl GetDetailedErrorCode()
{
	return g_DriverState.detailedErrorCode;
}

// Connect only after the service is running. Reopening a paused session must
// not discard its outstanding key releases; finish Stop/Unload first.
BOOL SetHandle()
{
    std::lock_guard<std::mutex> lifecycle(g_lifecycleMutex);
    std::lock_guard<std::mutex> device(g_deviceMutex);
    if (drvhandle != INVALID_HANDLE_VALUE && drvhandle != nullptr) {
        if (!g_acceptingInput) return Fail(ERROR_BUSY);
        SetLastError(ERROR_SUCCESS);
        return TRUE;
    }
    drvhandle = CreateFileA("\\\\.\\cqkeys", GENERIC_READ | GENERIC_WRITE, 0,
                            nullptr, OPEN_EXISTING, 0, nullptr);
    if (drvhandle == INVALID_HANDLE_VALUE) return Fail(GetLastError());
    g_DriverState.status = CheckDriverStatus(drvhandle);
    g_DriverState.lastCheckTime = GetSystemTimeMs();
    if (g_DriverState.status != DEVICE_STATUS_READY) {
        DWORD error = GetLastError();
        CloseHandle(drvhandle);
        drvhandle = INVALID_HANDLE_VALUE;
        return Fail(error ? error : ERROR_NOT_READY);
    }
    g_acceptingInput = true;
    SetLastError(ERROR_SUCCESS);
    return TRUE;
}

__forceinline bool IsKeyboardReady() {
    if (!g_acceptingInput || g_DriverState.status != DEVICE_STATUS_READY) {
        Fail(ERROR_NOT_READY);
        return false;
    }
    return true;
}

__forceinline bool IsMouseReady() { return IsKeyboardReady(); }

// 键盘按下
void __cdecl KeyDown(USHORT VirtualKey)
{
    std::lock_guard<std::mutex> lock(g_deviceMutex);
    SetLastError(ERROR_SUCCESS);
	if (!IsKeyboardReady()) {
		return;
	}

	KEYBOARD_INPUT_DATA kid;
	DWORD dwOutput;
	memset(&kid, 0, sizeof(KEYBOARD_INPUT_DATA));
	kid.Flags = KEY_DOWN;
	kid.MakeCode = (USHORT)MapVirtualKey(VirtualKey, 0);

	SendInputRequest(IOCTL_KEYBOARD, &kid, sizeof(KEYBOARD_INPUT_DATA), NULL, 0, &dwOutput, NULL);
}

// 键盘抬起
void __cdecl KeyUp(USHORT VirtualKey)
{
    std::lock_guard<std::mutex> lock(g_deviceMutex);
    SetLastError(ERROR_SUCCESS);
	KEYBOARD_INPUT_DATA  kid;
	DWORD dwOutput;
	memset(&kid, 0, sizeof(KEYBOARD_INPUT_DATA));
	kid.Flags = KEY_UP;
	kid.MakeCode = (USHORT)MapVirtualKey(VirtualKey, 0);

	SendInputRequest(IOCTL_KEYBOARD, &kid, sizeof(KEYBOARD_INPUT_DATA), NULL, 0, &dwOutput, NULL);

}

// 鼠标左键按下
void __cdecl MouseLeftButtonDown()
{
    std::lock_guard<std::mutex> lock(g_deviceMutex);
    SetLastError(ERROR_SUCCESS);
	if (!IsMouseReady()) {
		return;
	}

	MOUSE_INPUT_DATA mid;
	DWORD dwOutput;
	memset(&mid, 0, sizeof(MOUSE_INPUT_DATA));
	mid.ButtonFlags = MOUSE_LEFT_BUTTON_DOWN;
	SendInputRequest(IOCTL_MOUSE, &mid, sizeof(MOUSE_INPUT_DATA), NULL, 0, &dwOutput, NULL);
}


// 鼠标左键抬起
void __cdecl MouseLeftButtonUp()
{
    std::lock_guard<std::mutex> lock(g_deviceMutex);
    SetLastError(ERROR_SUCCESS);
	MOUSE_INPUT_DATA  mid;
	DWORD dwOutput;
	memset(&mid, 0, sizeof(MOUSE_INPUT_DATA));
	mid.ButtonFlags = MOUSE_LEFT_BUTTON_UP;
	SendInputRequest(IOCTL_MOUSE, &mid, sizeof(MOUSE_INPUT_DATA), NULL, 0, &dwOutput, NULL);
}

// 鼠标右键按下
void __cdecl MouseRightButtonDown()
{
    std::lock_guard<std::mutex> lock(g_deviceMutex);
    SetLastError(ERROR_SUCCESS);
	MOUSE_INPUT_DATA  mid;
	DWORD dwOutput;
	memset(&mid, 0, sizeof(MOUSE_INPUT_DATA));
	mid.ButtonFlags = MOUSE_RIGHT_BUTTON_DOWN;
	SendInputRequest(IOCTL_MOUSE, &mid, sizeof(MOUSE_INPUT_DATA), NULL, 0, &dwOutput, NULL);
}

// 鼠标右键抬起
void __cdecl MouseRightButtonUp()
{
    std::lock_guard<std::mutex> lock(g_deviceMutex);
    SetLastError(ERROR_SUCCESS);
	MOUSE_INPUT_DATA  mid;
	DWORD dwOutput;
	memset(&mid, 0, sizeof(MOUSE_INPUT_DATA));
	mid.ButtonFlags = MOUSE_RIGHT_BUTTON_UP;
	SendInputRequest(IOCTL_MOUSE, &mid, sizeof(MOUSE_INPUT_DATA), NULL, 0, &dwOutput, NULL);
}

// 鼠标中键按下
void __cdecl MouseMiddleButtonDown()
{
    std::lock_guard<std::mutex> lock(g_deviceMutex);
    SetLastError(ERROR_SUCCESS);
	MOUSE_INPUT_DATA  mid;
	DWORD dwOutput;
	memset(&mid, 0, sizeof(MOUSE_INPUT_DATA));
	mid.ButtonFlags = MOUSE_MIDDLE_BUTTON_DOWN;
	SendInputRequest(IOCTL_MOUSE, &mid, sizeof(MOUSE_INPUT_DATA), NULL, 0, &dwOutput, NULL);
}

// 鼠标中键抬起
void __cdecl MouseMiddleButtonUp()
{
    std::lock_guard<std::mutex> lock(g_deviceMutex);
    SetLastError(ERROR_SUCCESS);
	MOUSE_INPUT_DATA  mid;
	DWORD dwOutput;
	memset(&mid, 0, sizeof(MOUSE_INPUT_DATA));
	mid.ButtonFlags = MOUSE_MIDDLE_BUTTON_UP;
	SendInputRequest(IOCTL_MOUSE, &mid, sizeof(MOUSE_INPUT_DATA), NULL, 0, &dwOutput, NULL);
}

// 鼠标相对移动优化版
void __cdecl MouseMoveRELATIVE(LONG dx, LONG dy)
{
    std::lock_guard<std::mutex> lock(g_deviceMutex);
    SetLastError(ERROR_SUCCESS);
    if (!IsMouseReady()) {
        return;
    }

    // 空移动检查
    if (dx == 0 && dy == 0) {
        return;
    }

    MOUSE_INPUT_DATA mid;
    DWORD dwOutput;
    
    // 1. 获取系统DPI和显示器信息
    const LONG screenWidth = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const LONG screenHeight = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    
    // 获取系统DPI设置（如果支持）
    int dpiX = 96, dpiY = 96; // 默认DPI值
    HDC hdc = GetDC(NULL);
    if (hdc) {
        dpiX = GetDeviceCaps(hdc, LOGPIXELSX);
        dpiY = GetDeviceCaps(hdc, LOGPIXELSY);
        ReleaseDC(NULL, hdc);
    }
    
    // 2. 定义移动参数和阈值
    const float BASE_DPI = 96.0f;                          // 基准DPI值
    const float DPI_SCALE_X = dpiX / BASE_DPI;             // X方向DPI缩放因子
    const float DPI_SCALE_Y = dpiY / BASE_DPI;             // Y方向DPI缩放因子
    
    const float PRECISION_THRESHOLD = 15.0f;               // 精确移动阈值（像素）
    const float MICRO_MOVE_THRESHOLD = 3.0f;               // 微小移动阈值
    const int MAX_STEPS = 10;                              // 最大分步数（提高上限）
    const int FAST_MOVE_MULTIPLIER = 2;                    // 快速移动倍率
    
    // 3. 调整移动距离（考虑DPI缩放）
    float scaledDx = dx / DPI_SCALE_X;
    float scaledDy = dy / DPI_SCALE_Y;
    
    // 计算实际移动距离
    float distance = sqrtf(scaledDx * scaledDx + scaledDy * scaledDy);
    
    // 4. 根据距离选择移动策略
    if (distance <= MICRO_MOVE_THRESHOLD) {
        // 4.1 微小移动 - 直接精确发送
        memset(&mid, 0, sizeof(MOUSE_INPUT_DATA));
        mid.Flags = MOUSE_MOVE_RELATIVE;
        mid.LastX = dx;
        mid.LastY = dy;
        
        if (!SendInputRequest(IOCTL_MOUSE, &mid, sizeof(MOUSE_INPUT_DATA),
                            NULL, 0, &dwOutput, NULL)) {
            DWORD error = GetLastError();
        }
    }
    else if (distance <= PRECISION_THRESHOLD) {
        // 4.2 小距离移动 - 单步精确移动
        memset(&mid, 0, sizeof(MOUSE_INPUT_DATA));
        mid.Flags = MOUSE_MOVE_RELATIVE;
        mid.LastX = dx;
        mid.LastY = dy;
        
        if (!SendInputRequest(IOCTL_MOUSE, &mid, sizeof(MOUSE_INPUT_DATA),
                            NULL, 0, &dwOutput, NULL)) {
            DWORD error = GetLastError();
        }
    }
    else {
        // 4.3 大距离移动 - 使用自适应步进实现平滑移动
        // 计算步数 - 距离越大步数越多，但有上限
        int steps = (int)std::min<float>(MAX_STEPS, distance / PRECISION_THRESHOLD + 0.5f);
        steps = std::max<int>(2, steps); // 至少分2步
        
        // 计算每步移动量
        float stepX = scaledDx / steps;
        float stepY = scaledDy / steps;
        
        // 快速移动通道 - 对大幅度移动优化
        if (distance > PRECISION_THRESHOLD * FAST_MOVE_MULTIPLIER) {
            steps = steps / 2 + 1; // 减少步数，加快移动速度
            stepX = scaledDx / steps;
            stepY = scaledDy / steps;
        }
        
        // 分步发送移动指令
        float accumulatedErrorX = 0.0f, accumulatedErrorY = 0.0f;
        
        for (int i = 0; i < steps; i++) {
            // 累积误差处理：保留小数部分，避免精度损失
            accumulatedErrorX += stepX - (int)stepX;
            accumulatedErrorY += stepY - (int)stepY;
            
            // 当累积误差超过1像素时，加入下一步的移动
            int moveX = (int)stepX;
            int moveY = (int)stepY;
            
            if (accumulatedErrorX >= 1.0f) {
                moveX += 1;
                accumulatedErrorX -= 1.0f;
            }
            else if (accumulatedErrorX <= -1.0f) {
                moveX -= 1;
                accumulatedErrorX += 1.0f;
            }
            
            if (accumulatedErrorY >= 1.0f) {
                moveY += 1;
                accumulatedErrorY -= 1.0f;
            }
            else if (accumulatedErrorY <= -1.0f) {
                moveY -= 1;
                accumulatedErrorY += 1.0f;
            }
            
            // 发送单步移动
            memset(&mid, 0, sizeof(MOUSE_INPUT_DATA));
            mid.Flags = MOUSE_MOVE_RELATIVE;
            mid.LastX = moveX;
            mid.LastY = moveY;
            
            if (!SendInputRequest(IOCTL_MOUSE, &mid, sizeof(MOUSE_INPUT_DATA),
                                NULL, 0, &dwOutput, NULL)) {
                return; // Preserve the first failure; do not send a final correction.
            }
            
            // 动态延迟：距离越大，延迟越短，但确保有最小延迟
            if (i < steps - 1) { // 最后一步不延迟
                if (distance > 100) {
                    Sleep(0); // 大距离移动只让出时间片
                }
                else {
                    Sleep(1); // 小距离移动添加极小延迟，提高平滑度
                }
            }
        }
        
        // 处理余下的误差（确保总移动量精确）
        int finalMoveX = dx - (int)(stepX * steps);
        int finalMoveY = dy - (int)(stepY * steps);
        
        if (finalMoveX != 0 || finalMoveY != 0) {
            memset(&mid, 0, sizeof(MOUSE_INPUT_DATA));
            mid.Flags = MOUSE_MOVE_RELATIVE;
            mid.LastX = finalMoveX;
            mid.LastY = finalMoveY;
            
            if (!SendInputRequest(IOCTL_MOUSE, &mid, sizeof(MOUSE_INPUT_DATA),
                                NULL, 0, &dwOutput, NULL)) {
                DWORD error = GetLastError();
            }
        }
    }
}

// 鼠标绝对移动
void __cdecl MouseMoveABSOLUTE(LONG dx, LONG dy)
{
    std::lock_guard<std::mutex> lock(g_deviceMutex);
    SetLastError(ERROR_SUCCESS);
    if (!IsMouseReady()) {
        return;
    }

    MOUSE_INPUT_DATA mid;
    DWORD dwOutput;
    memset(&mid, 0, sizeof(MOUSE_INPUT_DATA));

    // 1. 获取虚拟屏幕信息（支持多显示器）
    const LONG virtualScreenWidth = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const LONG virtualScreenHeight = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    const LONG virtualScreenLeft = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const LONG virtualScreenTop = GetSystemMetrics(SM_YVIRTUALSCREEN);
    
    // 计算相对于虚拟屏幕的坐标
    LONG adjustedX = dx;
    LONG adjustedY = dy;

    // 2. 相对与虚拟屏幕的边界检查
    // 允许更大范围内的坐标，适应多显示器
    if (adjustedX < virtualScreenLeft || adjustedX >= virtualScreenLeft + virtualScreenWidth ||
        adjustedY < virtualScreenTop || adjustedY >= virtualScreenTop + virtualScreenHeight) {
        
        // 如果坐标在虚拟屏幕外，进行边界调整
        adjustedX = (std::max)(virtualScreenLeft, (std::min)(dx, virtualScreenLeft + virtualScreenWidth - 1));
        adjustedY = (std::max)(virtualScreenTop, (std::min)(dy, virtualScreenTop + virtualScreenHeight - 1));
    }
    
    // 3. 转换为相对于虚拟屏幕左上角的坐标
    LONG relativeX = adjustedX - virtualScreenLeft;
    LONG relativeY = adjustedY - virtualScreenTop;
    
    // 4. 设置绝对移动标志
    mid.Flags = MOUSE_MOVE_ABSOLUTE;
    
    // 5. 优化的坐标转换（去除复杂的非线性插值，提高精确度）
    // 转换为0-65535范围的绝对坐标
    LONG normalizedX = (LONG)(((INT64)relativeX * 65535) / (virtualScreenWidth - 1));
    LONG normalizedY = (LONG)(((INT64)relativeY * 65535) / (virtualScreenHeight - 1));
    
    // 确保在有效范围内
    mid.LastX = (std::max)(0L, (std::min)(normalizedX, 65535L));
    mid.LastY = (std::max)(0L, (std::min)(normalizedY, 65535L));

    // 6. 发送移动命令
    if (!SendInputRequest(IOCTL_MOUSE, &mid, sizeof(MOUSE_INPUT_DATA), NULL, 0, &dwOutput, NULL)) {
        DWORD error = GetLastError();
        printf("鼠标移动失败，错误码: %ld\n", error);
        Fail(error);
    }
}

// 鼠标XButton1按下
void __cdecl MouseXButton1Down()
{
    std::lock_guard<std::mutex> lock(g_deviceMutex);
    SetLastError(ERROR_SUCCESS);
	MOUSE_INPUT_DATA  mid;
	DWORD dwOutput;
	memset(&mid, 0, sizeof(MOUSE_INPUT_DATA));
	mid.ButtonFlags = MOUSE_BUTTON_4_DOWN;
	SendInputRequest(IOCTL_MOUSE, &mid, sizeof(MOUSE_INPUT_DATA), NULL, 0, &dwOutput, NULL);
}

// 鼠标XButton1抬起
void __cdecl MouseXButton1Up()
{
    std::lock_guard<std::mutex> lock(g_deviceMutex);
    SetLastError(ERROR_SUCCESS);
	MOUSE_INPUT_DATA  mid;
	DWORD dwOutput;
	memset(&mid, 0, sizeof(MOUSE_INPUT_DATA));
	mid.ButtonFlags = MOUSE_BUTTON_4_UP;
	SendInputRequest(IOCTL_MOUSE, &mid, sizeof(MOUSE_INPUT_DATA), NULL, 0, &dwOutput, NULL);
}

// 鼠标XButton2按下
void __cdecl MouseXButton2Down()
{
    std::lock_guard<std::mutex> lock(g_deviceMutex);
    SetLastError(ERROR_SUCCESS);
	MOUSE_INPUT_DATA  mid;
	DWORD dwOutput;
	memset(&mid, 0, sizeof(MOUSE_INPUT_DATA));
	mid.ButtonFlags = MOUSE_BUTTON_5_DOWN;
	SendInputRequest(IOCTL_MOUSE, &mid, sizeof(MOUSE_INPUT_DATA), NULL, 0, &dwOutput, NULL);
}

// 鼠标XButton2抬起
void __cdecl MouseXButton2Up()
{
    std::lock_guard<std::mutex> lock(g_deviceMutex);
    SetLastError(ERROR_SUCCESS);
	MOUSE_INPUT_DATA  mid;
	DWORD dwOutput;
	memset(&mid, 0, sizeof(MOUSE_INPUT_DATA));
	mid.ButtonFlags = MOUSE_BUTTON_5_UP;
	SendInputRequest(IOCTL_MOUSE, &mid, sizeof(MOUSE_INPUT_DATA), NULL, 0, &dwOutput, NULL);
}

// 鼠标滚轮向上
void __cdecl MouseWheelUp(USHORT wheelDelta)
{
    std::lock_guard<std::mutex> lock(g_deviceMutex);
    SetLastError(ERROR_SUCCESS);
	MOUSE_INPUT_DATA  mid;
	DWORD dwOutput;
	memset(&mid, 0, sizeof(MOUSE_INPUT_DATA));
	mid.ButtonFlags = MOUSE_WHEEL;
	mid.ButtonData = wheelDelta;
	SendInputRequest(IOCTL_MOUSE, &mid, sizeof(MOUSE_INPUT_DATA), NULL, 0, &dwOutput, NULL);
}

// 鼠标滚轮向下
void __cdecl MouseWheelDown(USHORT wheelDelta)
{
    std::lock_guard<std::mutex> lock(g_deviceMutex);
    SetLastError(ERROR_SUCCESS);
	MOUSE_INPUT_DATA  mid;
	DWORD dwOutput;
	memset(&mid, 0, sizeof(MOUSE_INPUT_DATA));
	mid.ButtonFlags = MOUSE_WHEEL;
	mid.ButtonData = (USHORT)(-wheelDelta);  // 负值表示向下滚动
	SendInputRequest(IOCTL_MOUSE, &mid, sizeof(MOUSE_INPUT_DATA), NULL, 0, &dwOutput, NULL);
}

namespace {
    constexpr DWORD kServiceTimeoutMs = 5000;
    class ServiceHandle {
    public:
        explicit ServiceHandle(SC_HANDLE handle) : value(handle) {}
        ~ServiceHandle() { Close(); }
        ServiceHandle(const ServiceHandle&) = delete;
        ServiceHandle& operator=(const ServiceHandle&) = delete;
        void Close() {
            DWORD error = GetLastError();
            if (value) { CloseServiceHandle(value); value = nullptr; }
            SetLastError(error);
        }
        SC_HANDLE value;
    };

    BOOL Success() {
        g_DriverState.detailedErrorCode = ERROR_SUCCESS;
        SetLastError(ERROR_SUCCESS);
        return TRUE;
    }

    BOOL ValidateName(const char* name) {
        // All input functions address the fixed cqkeys device. Never stop an
        // unrelated service supplied by mistake through the legacy ABI.
        if (!name || _stricmp(name, "cqkeys") != 0) return Fail(ERROR_INVALID_NAME);
        return TRUE;
    }

    BOOL WaitForState(SC_HANDLE service, DWORD wanted) {
        DWORD start = GetTickCount();
        for (;;) {
            SERVICE_STATUS status{};
            if (!QueryServiceStatus(service, &status)) return Fail(GetLastError());
            if (status.dwCurrentState == wanted) return TRUE;
            if (wanted == SERVICE_RUNNING && status.dwCurrentState == SERVICE_STOPPED)
                return Fail(status.dwWin32ExitCode ? status.dwWin32ExitCode : ERROR_SERVICE_NOT_ACTIVE);
            if (GetTickCount() - start >= kServiceTimeoutMs) return Fail(ERROR_TIMEOUT);
            Sleep(50);
        }
    }

    BOOL StopServiceAndWait(SC_HANDLE service) {
        SERVICE_STATUS status{};
        DWORD start = GetTickCount();
        do {
            if (!QueryServiceStatus(service, &status)) return Fail(GetLastError());
            if (status.dwServiceType != SERVICE_KERNEL_DRIVER) return Fail(ERROR_INVALID_SERVICE_CONTROL);
            if (status.dwCurrentState != SERVICE_START_PENDING) break;
            if (GetTickCount() - start >= kServiceTimeoutMs) return Fail(ERROR_TIMEOUT);
            Sleep(50);
        } while (true);
        if (status.dwCurrentState == SERVICE_STOPPED) return TRUE;
        if (status.dwCurrentState != SERVICE_STOP_PENDING &&
            !ControlService(service, SERVICE_CONTROL_STOP, &status)) {
            DWORD error = GetLastError();
            if (error != ERROR_SERVICE_NOT_ACTIVE) return Fail(error);
        }
        // Query failures and timeouts must never fall through to DeleteService.
        return WaitForState(service, SERVICE_STOPPED);
    }

    BOOL WaitForDeletion(SC_HANDLE scm, const char* name) {
        DWORD start = GetTickCount();
        for (;;) {
            ServiceHandle probe(OpenServiceA(scm, name, SERVICE_QUERY_STATUS));
            if (!probe.value) {
                DWORD error = GetLastError();
                if (error == ERROR_SERVICE_DOES_NOT_EXIST) return TRUE;
                if (error != ERROR_SERVICE_MARKED_FOR_DELETE) return Fail(error);
            }
            probe.Close();
            if (GetTickCount() - start >= kServiceTimeoutMs) return Fail(ERROR_TIMEOUT);
            Sleep(50);
        }
    }

    BOOL StopDriver(const char* name, bool uninstall) {
        std::lock_guard<std::mutex> lifecycle(g_lifecycleMutex);
        if (!ValidateName(name)) return FALSE;
        if (!CloseDevice()) return FALSE;
        ServiceHandle scm(OpenSCManagerA(nullptr, nullptr, SC_MANAGER_CONNECT));
        if (!scm.value) return Fail(GetLastError());
        ServiceHandle service(OpenServiceA(scm.value, name,
            SERVICE_STOP | SERVICE_QUERY_STATUS | (uninstall ? DELETE : 0)));
        if (!service.value) {
            DWORD error = GetLastError();
            return error == ERROR_SERVICE_DOES_NOT_EXIST ? Success() : Fail(error);
        }
        if (!StopServiceAndWait(service.value)) return FALSE;
        if (uninstall) {
            if (!DeleteService(service.value) && GetLastError() != ERROR_SERVICE_MARKED_FOR_DELETE)
                return Fail(GetLastError());
            service.Close();
            if (!WaitForDeletion(scm.value, name)) return FALSE;
        }
        return Success();
    }
}

BOOL __cdecl LoadNTDriver(char* name, char* path)
{
    std::lock_guard<std::mutex> lifecycle(g_lifecycleMutex);
    if (!ValidateName(name)) return FALSE;
    if (!path || !*path) return Fail(ERROR_INVALID_PARAMETER);
    char fullPath[MAX_PATH];
    DWORD length = GetFullPathNameA(path, MAX_PATH, fullPath, nullptr);
    if (!length) return Fail(GetLastError());
    if (length >= MAX_PATH) return Fail(ERROR_FILENAME_EXCED_RANGE);
    DWORD attributes = GetFileAttributesA(fullPath);
    if (attributes == INVALID_FILE_ATTRIBUTES) return Fail(GetLastError());
    if (attributes & FILE_ATTRIBUTE_DIRECTORY) return Fail(ERROR_INVALID_PARAMETER);

    ServiceHandle scm(OpenSCManagerA(nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE));
    if (!scm.value) return Fail(GetLastError());
    ServiceHandle service(OpenServiceA(scm.value, name, SERVICE_START | SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG));
    if (!service.value) {
        DWORD error = GetLastError();
        if (error != ERROR_SERVICE_DOES_NOT_EXIST) return Fail(error);
        service.value = CreateServiceA(scm.value, name, name,
            SERVICE_START | SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG,
            SERVICE_KERNEL_DRIVER, SERVICE_DEMAND_START, SERVICE_ERROR_NORMAL,
            fullPath, nullptr, nullptr, nullptr, nullptr, nullptr);
        if (!service.value) return Fail(GetLastError());
    }
    // Reuse an existing registration only when it actually names this binary.
    // Never delete/recreate a running service as part of initialization.
    DWORD bytes = 0;
    QueryServiceConfigA(service.value, nullptr, 0, &bytes);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) return Fail(GetLastError());
    std::vector<BYTE> buffer(bytes);
    auto config = reinterpret_cast<QUERY_SERVICE_CONFIGA*>(buffer.data());
    if (!QueryServiceConfigA(service.value, config, bytes, &bytes)) return Fail(GetLastError());
    std::string registered = config->lpBinaryPathName;
    if (registered.size() >= 2 && registered.front() == '"' && registered.back() == '"')
        registered = registered.substr(1, registered.size() - 2);
    if (registered.compare(0, 4, "\\??\\") == 0) registered.erase(0, 4);
    if (config->dwServiceType != SERVICE_KERNEL_DRIVER || _stricmp(registered.c_str(), fullPath) != 0)
        return Fail(ERROR_SERVICE_EXISTS);

    SERVICE_STATUS status{};
    if (!QueryServiceStatus(service.value, &status)) return Fail(GetLastError());
    if (status.dwCurrentState == SERVICE_STOP_PENDING) {
        if (!WaitForState(service.value, SERVICE_STOPPED)) return FALSE;
        status.dwCurrentState = SERVICE_STOPPED;
    }
    if (status.dwCurrentState == SERVICE_RUNNING) return Success();
    if (status.dwCurrentState != SERVICE_START_PENDING && !StartServiceA(service.value, 0, nullptr)) {
        DWORD error = GetLastError();
        if (error != ERROR_SERVICE_ALREADY_RUNNING) return Fail(error);
    }
    if (!WaitForState(service.value, SERVICE_RUNNING)) return FALSE;
    return Success();
}

BOOL __cdecl StopNTDriver(char* name) { return StopDriver(name, false); }
BOOL __cdecl UnloadNTDriver(char* name) { return StopDriver(name, true); }

// Never perform SCM work or wait for threads under the loader lock.
// Call StopNTDriver/UnloadNTDriver explicitly before FreeLibrary.
BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }

#ifdef _MANAGED
#pragma managed(pop)
#endif
