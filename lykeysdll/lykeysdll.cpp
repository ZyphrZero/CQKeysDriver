#include "pch.h"
#include "lykeys.h"
#include <windows.h>
#include <winsvc.h>  
#include <conio.h>  
#include <stdio.h>
#include <winternl.h>
#include <atomic>
#include <algorithm>
#include <sysinfoapi.h>
#include <time.h>
#include <TlHelp32.h>
#include <cmath>

// 使用std命名空间中的min和max
using std::min;
using std::max;
using std::abs;

// 字符串加密宏和常量
#define XSTR_SEED (0x11223344 ^ 0xABCDEF12)  // 更复杂的种子
#define ENCRYPT_CHAR(ch, seed) ((ch) ^ ((seed) >> 8) ^ ((seed) & 0xFF))  // 增强的加密算法
#define DECRYPT_CHAR(ch, seed) ENCRYPT_CHAR(ch, seed)

// 字符串加密类
class XorString {
private:
	unsigned char* m_encryptedData;
	size_t m_length;
	DWORD m_seed;
	bool m_isDecrypted;

	void Encrypt() {
		DWORD rolling_seed = m_seed;
		for (size_t i = 0; i < m_length; i++) {
			m_encryptedData[i] = ENCRYPT_CHAR(m_encryptedData[i], rolling_seed);
			rolling_seed = _rotl(rolling_seed, 3) + m_encryptedData[i] + 0x37;
		}
		m_isDecrypted = false;
	}

public:
	XorString(const char* data, DWORD seed = XSTR_SEED) : m_seed(seed), m_isDecrypted(false) {
		m_length = strlen(data);
		m_encryptedData = new unsigned char[m_length + 1];
		memcpy(m_encryptedData, data, m_length);
		m_encryptedData[m_length] = '\0';
		Encrypt();
	}

	~XorString() {
		if (m_encryptedData) {
			if (m_isDecrypted) {
				SecureZeroMemory(m_encryptedData, m_length);
			}
			delete[] m_encryptedData;
		}
	}

	const char* Decrypt() {
		if (!m_isDecrypted) {
			DWORD rolling_seed = m_seed;
			for (size_t i = 0; i < m_length; i++) {
				unsigned char decrypted = DECRYPT_CHAR(m_encryptedData[i], rolling_seed);
				rolling_seed = _rotl(rolling_seed, 3) + m_encryptedData[i] + 0x37;
				m_encryptedData[i] = decrypted;
			}
			m_isDecrypted = true;
		}
		return (const char*)m_encryptedData;
	}

	void ReEncrypt() {
		if (m_isDecrypted) {
			Encrypt();
		}
	}
};

// 加密的关键字符串
namespace EncryptedStrings {
	static XorString g_DevicePath("\\\\.\\lykeys", XSTR_SEED ^ 0x12345678);
	static XorString g_DriverName("lykeys", XSTR_SEED ^ 0x87654321);
	static XorString g_NtdllName("ntdll.dll", XSTR_SEED ^ 0xABCDEF00);
	static XorString g_Kernel32Name("kernel32.dll", XSTR_SEED ^ 0x55AA55AA);
	static XorString g_NtQueryInfoProcess("NtQueryInformationProcess", XSTR_SEED ^ 0x11111111);
}

// 反调试检测函数
bool IsDebuggerPresentEx() {
	typedef NTSTATUS(NTAPI* pNtQueryInformationProcess)(
		IN HANDLE ProcessHandle,
		IN PROCESSINFOCLASS ProcessInformationClass,
		OUT PVOID ProcessInformation,
		IN ULONG ProcessInformationLength,
		OUT PULONG ReturnLength OPTIONAL
	);

	HMODULE hNtdll = GetModuleHandleA("ntdll.dll");
	if (!hNtdll) return false;

	pNtQueryInformationProcess NtQueryInformationProcess = (pNtQueryInformationProcess)
		GetProcAddress(hNtdll, "NtQueryInformationProcess");
	if (!NtQueryInformationProcess) return false;

	DWORD_PTR debugPort = 0;
	NTSTATUS status = NtQueryInformationProcess(
		GetCurrentProcess(),
		ProcessDebugPort,
		&debugPort,
		sizeof(debugPort),
		NULL
	);

	// 检查调试标志
	BOOL isDebuggerPresent = FALSE;
	CheckRemoteDebuggerPresent(GetCurrentProcess(), &isDebuggerPresent);
	
	// 检查进程名称
	HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (hSnapshot != INVALID_HANDLE_VALUE) {
		PROCESSENTRY32W pe32;
		pe32.dwSize = sizeof(pe32);
		if (Process32FirstW(hSnapshot, &pe32)) {
			do {
				if (wcsstr(pe32.szExeFile, L"ollydbg.exe") ||
					wcsstr(pe32.szExeFile, L"x64dbg.exe") ||
					wcsstr(pe32.szExeFile, L"windbg.exe")) {
					CloseHandle(hSnapshot);
					return true;
				}
			} while (Process32NextW(hSnapshot, &pe32));
		}
		CloseHandle(hSnapshot);
	}

	return (NT_SUCCESS(status) && debugPort) || isDebuggerPresent;
}

typedef LONG NTSTATUS;
typedef NTSTATUS (WINAPI *pfnRtlGetVersion)(PRTL_OSVERSIONINFOW);

// 系统版本信息结构
typedef struct _DLL_SYSTEM_INFO {
	DWORD MajorVersion; // 主版本号
	DWORD MinorVersion; // 次版本号
	DWORD BuildNumber; // 构建号
	BOOL Is64BitOS; // 用于区分系统是32位还是64位
	BOOL Is64BitProcess; // 用于区分进程是32位还是64位
} DLL_SYSTEM_INFO, *PDLL_SYSTEM_INFO;

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

// 获取系统信息
BOOL GetSystemInfo(PDLL_SYSTEM_INFO pSysInfo) 
{
	SYSTEM_INFO si;
	GetNativeSystemInfo(&si);
	
	OSVERSIONINFOEXW osvi = { sizeof(osvi) };
	HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
	if (!hNtdll) return FALSE;
	
	pfnRtlGetVersion RtlGetVersion = (pfnRtlGetVersion)GetProcAddress(hNtdll, "RtlGetVersion");
	if (!RtlGetVersion) return FALSE;
	
	RtlGetVersion((PRTL_OSVERSIONINFOW)&osvi);
	
	pSysInfo->MajorVersion = osvi.dwMajorVersion;
	pSysInfo->MinorVersion = osvi.dwMinorVersion;
	pSysInfo->BuildNumber = osvi.dwBuildNumber;
	
	// 检查是否64位操作系统
	typedef BOOL (WINAPI *LPFN_ISWOW64PROCESS) (HANDLE, PBOOL);
	LPFN_ISWOW64PROCESS fnIsWow64Process = (LPFN_ISWOW64PROCESS)
		GetProcAddress(GetModuleHandle(TEXT("kernel32")), "IsWow64Process");
	
	if (fnIsWow64Process) {
		BOOL bIsWow64 = FALSE;
		if (fnIsWow64Process(GetCurrentProcess(), &bIsWow64)) {
			pSysInfo->Is64BitOS = (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64) || bIsWow64;
		}
	}
	
#ifdef _WIN64
	pSysInfo->Is64BitProcess = TRUE;
#else
	pSysInfo->Is64BitProcess = FALSE;
#endif
	
	return TRUE;
}

// 检查系统兼容性
BOOL CheckSystemCompatibility() 
{
	DLL_SYSTEM_INFO sysInfo;
	if (!GetSystemInfo(&sysInfo)) {
		printf("Failed to get system information\n");
		return FALSE;
	}
	
	// 检查Windows版本兼容性
	if (sysInfo.MajorVersion < 6) {  // 低于Windows Vista
		printf("Unsupported Windows version: %d.%d\n", 
			sysInfo.MajorVersion, sysInfo.MinorVersion);
		return FALSE;
	}
	
	// Windows版本特定功能检查
	if (sysInfo.MajorVersion == 6) {  // Windows Vista/7/8/8.1
		printf("When running on Windows Vista/7/8/8.1, some advanced features may not be available\n");
	} else if (sysInfo.MajorVersion == 10) {  // Windows 10/11
		printf("Running on Windows 10/11, all features are available\n");
	}
	
	// 检查32/64位兼容性
#ifdef _WIN64
	if (!sysInfo.Is64BitOS) {
		printf("64-bit DLL cannot run on 32-bit system\n");
		return FALSE;
	}
#endif
	
	printf("System Version: Windows %d.%d.%d (%s)\n",
		sysInfo.MajorVersion,
		sysInfo.MinorVersion,
		sysInfo.BuildNumber,
		sysInfo.Is64BitOS ? "64-bit" : "32-bit");
		
	return TRUE;
}

#ifdef _MANAGED
#pragma managed(push, off)
#endif


// 装载NT驱动程序
HANDLE drvhandle;
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
	ULONGLONG currentTime = GetSystemTimeMs();
	
	// 检查驱动状态
	g_DriverState.status = CheckDriverStatus(drvhandle);
	g_DriverState.lastCheckTime = currentTime;
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
	return drvhandle;
}

// 添加导出函数：获取详细错误代码
extern "C" __declspec(dllexport) int __cdecl GetDetailedErrorCode()
{
	return g_DriverState.detailedErrorCode;
}

// SetHandle函数
BOOL SetHandle()
{
	// 检查是否已经有有效句柄
	if (drvhandle != NULL && drvhandle != INVALID_HANDLE_VALUE) {
		CloseHandle(drvhandle);
		drvhandle = NULL;
	}

	const char* devicePath = EncryptedStrings::g_DevicePath.Decrypt();
	drvhandle = CreateFileA(
		devicePath,
		GENERIC_WRITE | GENERIC_READ,
		0,
		NULL,
		OPEN_EXISTING,
		0,
		0);
	EncryptedStrings::g_DevicePath.ReEncrypt();

	if (drvhandle == INVALID_HANDLE_VALUE || drvhandle == NULL) {
		DWORD error = GetLastError();
		printf("Error: Cannot open device, error code: %d\n", error);
		return FALSE;
	}

	// 初始化驱动状态
	CheckDeviceStatus();
	
	// 打印初始状态
	DEVICE_STATUS status = g_DriverState.status.load();
	printf("Driver Status: %s\n", 
		status == DEVICE_STATUS_READY ? "Ready" :
		status == DEVICE_STATUS_ERROR ? "Error" :
		status == DEVICE_STATUS_NO_KEYBOARD ? "No Keyboard" :
		status == DEVICE_STATUS_NO_MOUSE ? "No Mouse" : "Unknown");
	
	return status == DEVICE_STATUS_READY;
}

// 内联辅助函数
__forceinline bool IsKeyboardReady() {
	return g_DriverState.status == DEVICE_STATUS_READY;
}

__forceinline bool IsMouseReady() {
	return g_DriverState.status == DEVICE_STATUS_READY;
}


// 键盘按下
void __cdecl KeyDown(USHORT VirtualKey)
{
	if (!IsKeyboardReady()) {
		return;
	}

	KEYBOARD_INPUT_DATA kid;
	DWORD dwOutput;
	memset(&kid, 0, sizeof(KEYBOARD_INPUT_DATA));
	kid.Flags = KEY_DOWN;
	kid.MakeCode = (USHORT)MapVirtualKey(VirtualKey, 0);

	DeviceIoControl(drvhandle, IOCTL_KEYBOARD, &kid, sizeof(KEYBOARD_INPUT_DATA), NULL, 0, &dwOutput, NULL);
}

// 键盘抬起
void __cdecl KeyUp(USHORT VirtualKey)
{
	KEYBOARD_INPUT_DATA  kid;
	DWORD dwOutput;
	memset(&kid, 0, sizeof(KEYBOARD_INPUT_DATA));
	kid.Flags = KEY_UP;
	kid.MakeCode = (USHORT)MapVirtualKey(VirtualKey, 0);

	DeviceIoControl(drvhandle, IOCTL_KEYBOARD, &kid, sizeof(KEYBOARD_INPUT_DATA), NULL, 0, &dwOutput, NULL);

}

// 鼠标左键按下
void __cdecl MouseLeftButtonDown()
{
	if (!IsMouseReady()) {
		return;
	}

	MOUSE_INPUT_DATA mid;
	DWORD dwOutput;
	memset(&mid, 0, sizeof(MOUSE_INPUT_DATA));
	mid.ButtonFlags = MOUSE_LEFT_BUTTON_DOWN;
	DeviceIoControl(drvhandle, IOCTL_MOUSE, &mid, sizeof(MOUSE_INPUT_DATA), NULL, 0, &dwOutput, NULL);
}


// 鼠标左键抬起
void __cdecl MouseLeftButtonUp()
{
	MOUSE_INPUT_DATA  mid;
	DWORD dwOutput;
	memset(&mid, 0, sizeof(MOUSE_INPUT_DATA));
	mid.ButtonFlags = MOUSE_LEFT_BUTTON_UP;
	DeviceIoControl(drvhandle, IOCTL_MOUSE, &mid, sizeof(MOUSE_INPUT_DATA), NULL, 0, &dwOutput, NULL);
}

// 鼠标右键按下
void __cdecl MouseRightButtonDown()
{
	MOUSE_INPUT_DATA  mid;
	DWORD dwOutput;
	memset(&mid, 0, sizeof(MOUSE_INPUT_DATA));
	mid.ButtonFlags = MOUSE_RIGHT_BUTTON_DOWN;
	DeviceIoControl(drvhandle, IOCTL_MOUSE, &mid, sizeof(MOUSE_INPUT_DATA), NULL, 0, &dwOutput, NULL);
}

// 鼠标右键抬起
void __cdecl MouseRightButtonUp()
{
	MOUSE_INPUT_DATA  mid;
	DWORD dwOutput;
	memset(&mid, 0, sizeof(MOUSE_INPUT_DATA));
	mid.ButtonFlags = MOUSE_RIGHT_BUTTON_UP;
	DeviceIoControl(drvhandle, IOCTL_MOUSE, &mid, sizeof(MOUSE_INPUT_DATA), NULL, 0, &dwOutput, NULL);
}

// 鼠标中键按下
void __cdecl MouseMiddleButtonDown()
{
	MOUSE_INPUT_DATA  mid;
	DWORD dwOutput;
	memset(&mid, 0, sizeof(MOUSE_INPUT_DATA));
	mid.ButtonFlags = MOUSE_MIDDLE_BUTTON_DOWN;
	DeviceIoControl(drvhandle, IOCTL_MOUSE, &mid, sizeof(MOUSE_INPUT_DATA), NULL, 0, &dwOutput, NULL);
}

// 鼠标中键抬起
void __cdecl MouseMiddleButtonUp()
{
	MOUSE_INPUT_DATA  mid;
	DWORD dwOutput;
	memset(&mid, 0, sizeof(MOUSE_INPUT_DATA));
	mid.ButtonFlags = MOUSE_MIDDLE_BUTTON_UP;
	DeviceIoControl(drvhandle, IOCTL_MOUSE, &mid, sizeof(MOUSE_INPUT_DATA), NULL, 0, &dwOutput, NULL);
}

// 鼠标相对移动优化版
void __cdecl MouseMoveRELATIVE(LONG dx, LONG dy)
{
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
        
        if (!DeviceIoControl(drvhandle, IOCTL_MOUSE, &mid, sizeof(MOUSE_INPUT_DATA), 
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
        
        if (!DeviceIoControl(drvhandle, IOCTL_MOUSE, &mid, sizeof(MOUSE_INPUT_DATA), 
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
            
            if (!DeviceIoControl(drvhandle, IOCTL_MOUSE, &mid, sizeof(MOUSE_INPUT_DATA), 
                                NULL, 0, &dwOutput, NULL)) {
                DWORD error = GetLastError();
                break; // 发生错误时终止剩余步骤
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
            
            if (!DeviceIoControl(drvhandle, IOCTL_MOUSE, &mid, sizeof(MOUSE_INPUT_DATA), 
                                NULL, 0, &dwOutput, NULL)) {
                DWORD error = GetLastError();
            }
        }
    }
}

// 鼠标绝对移动
void __cdecl MouseMoveABSOLUTE(LONG dx, LONG dy)
{
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
    if (!DeviceIoControl(drvhandle, IOCTL_MOUSE, &mid, sizeof(MOUSE_INPUT_DATA), NULL, 0, &dwOutput, NULL)) {
        DWORD error = GetLastError();
        printf("鼠标移动失败，错误码: %ld\n", error);
    }
}

// 鼠标XButton1按下
void __cdecl MouseXButton1Down()
{
	MOUSE_INPUT_DATA  mid;
	DWORD dwOutput;
	memset(&mid, 0, sizeof(MOUSE_INPUT_DATA));
	mid.ButtonFlags = MOUSE_BUTTON_4_DOWN;
	DeviceIoControl(drvhandle, IOCTL_MOUSE, &mid, sizeof(MOUSE_INPUT_DATA), NULL, 0, &dwOutput, NULL);
}

// 鼠标XButton1抬起
void __cdecl MouseXButton1Up()
{
	MOUSE_INPUT_DATA  mid;
	DWORD dwOutput;
	memset(&mid, 0, sizeof(MOUSE_INPUT_DATA));
	mid.ButtonFlags = MOUSE_BUTTON_4_UP;
	DeviceIoControl(drvhandle, IOCTL_MOUSE, &mid, sizeof(MOUSE_INPUT_DATA), NULL, 0, &dwOutput, NULL);
}

// 鼠标XButton2按下
void __cdecl MouseXButton2Down()
{
	MOUSE_INPUT_DATA  mid;
	DWORD dwOutput;
	memset(&mid, 0, sizeof(MOUSE_INPUT_DATA));
	mid.ButtonFlags = MOUSE_BUTTON_5_DOWN;
	DeviceIoControl(drvhandle, IOCTL_MOUSE, &mid, sizeof(MOUSE_INPUT_DATA), NULL, 0, &dwOutput, NULL);
}

// 鼠标XButton2抬起
void __cdecl MouseXButton2Up()
{
	MOUSE_INPUT_DATA  mid;
	DWORD dwOutput;
	memset(&mid, 0, sizeof(MOUSE_INPUT_DATA));
	mid.ButtonFlags = MOUSE_BUTTON_5_UP;
	DeviceIoControl(drvhandle, IOCTL_MOUSE, &mid, sizeof(MOUSE_INPUT_DATA), NULL, 0, &dwOutput, NULL);
}

// 鼠标滚轮向上
void __cdecl MouseWheelUp(USHORT wheelDelta)
{
	MOUSE_INPUT_DATA  mid;
	DWORD dwOutput;
	memset(&mid, 0, sizeof(MOUSE_INPUT_DATA));
	mid.ButtonFlags = MOUSE_WHEEL;
	mid.ButtonData = wheelDelta;
	DeviceIoControl(drvhandle, IOCTL_MOUSE, &mid, sizeof(MOUSE_INPUT_DATA), NULL, 0, &dwOutput, NULL);
}

// 鼠标滚轮向下
void __cdecl MouseWheelDown(USHORT wheelDelta)
{
	MOUSE_INPUT_DATA  mid;
	DWORD dwOutput;
	memset(&mid, 0, sizeof(MOUSE_INPUT_DATA));
	mid.ButtonFlags = MOUSE_WHEEL;
	mid.ButtonData = (USHORT)(-wheelDelta);  // 负值表示向下滚动
	DeviceIoControl(drvhandle, IOCTL_MOUSE, &mid, sizeof(MOUSE_INPUT_DATA), NULL, 0, &dwOutput, NULL);
}

// 装载驱动程序
BOOL __cdecl LoadNTDriver(char* lpszDriverName, char* lpszDriverPath)
{
	// 先检查驱动是否已经加载
	if (drvhandle != NULL && drvhandle != INVALID_HANDLE_VALUE) {
		printf("Driver already loaded!\n");
		return TRUE;
	}

	BOOL bRet = FALSE;
	SC_HANDLE hServiceMgr = NULL;
	SC_HANDLE hServiceDDK = NULL;
	const DWORD START_TIMEOUT = 3000; // 启动超时时间(毫秒)
	
	// 使用加密的DLL名称
	const char* ntdllName = EncryptedStrings::g_NtdllName.Decrypt();
	HMODULE hNtdll = GetModuleHandleA(ntdllName);
	EncryptedStrings::g_NtdllName.ReEncrypt();
	
	// 1. 添加权限检查
	HANDLE hToken;
	BOOL isElevated = FALSE;
	if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
		TOKEN_ELEVATION elevation;
		DWORD dwSize;
		if (GetTokenInformation(hToken, TokenElevation, &elevation, sizeof(elevation), &dwSize)) {
			isElevated = elevation.TokenIsElevated;
		}
		CloseHandle(hToken);
	}
	
	if (!isElevated) {
		SetLastError(ERROR_ACCESS_DENIED);
		printf("Elevated privileges required. Please run as administrator.\n");
		return FALSE;
	}
	
	// 2. 增强错误处理的OpenSCManager调用
	hServiceMgr = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
	if (!hServiceMgr) {
		DWORD error = GetLastError();
		char* errorMessage = NULL;
		FormatMessageA(
			FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM,
			NULL,
			error,
			0,
			(LPSTR)&errorMessage,
			0,
			NULL
		);
		printf("OpenSCManager failed with detailed error: %d - %s\n", error, errorMessage);
		LocalFree(errorMessage);
		return FALSE;
	}
	
	// 3. 快速检查系统兼容性
	if (!CheckSystemCompatibility()) {
		printf("System compatibility check failed\n");
		return FALSE;
	}
	
	// 4. 尝试打开已存在的服务
	hServiceDDK = OpenServiceA(hServiceMgr, lpszDriverName, SERVICE_ALL_ACCESS);
	if (hServiceDDK) {
		// 服务已存在，尝试直接启动
		SERVICE_STATUS status;
		QueryServiceStatus(hServiceDDK, &status);
		
		if (status.dwCurrentState == SERVICE_RUNNING) {
			printf("Service is already running\n");
			bRet = TRUE;
			goto cleanup;
		}
		
		// 停止服务以确保清洁状态
		ControlService(hServiceDDK, SERVICE_CONTROL_STOP, &status);
		Sleep(100); // 简短等待
		
		// 删除现有服务
		DeleteService(hServiceDDK);
		CloseServiceHandle(hServiceDDK);
		hServiceDDK = NULL;
	}
	
	// 5. 创建新服务
	hServiceDDK = CreateServiceA(
		hServiceMgr,
		lpszDriverName,        // 驱动程序的在注册表中的名字  
		lpszDriverName,        // 注册表驱动程序的 DisplayName 值  
		SERVICE_ALL_ACCESS,    // 加载驱动程序的访问权限  
		SERVICE_KERNEL_DRIVER, // 表示加载的服务是驱动程序  
		SERVICE_DEMAND_START,  // 注册表驱动程序的 Start 值  
		SERVICE_ERROR_IGNORE,  // 注册表驱动程序的 ErrorControl 值  
		lpszDriverPath,        // 注册表驱动程序的 ImagePath 值  
		NULL,                  // LoadOrderGroup
		NULL,                  // TagId
		NULL,                  // Dependencies
		NULL,                  // ServiceStartName
		NULL                   // Password
	);
	
	if (!hServiceDDK) {
		printf("Create service failed: %d\n", GetLastError());
		goto cleanup;
	}
	
	// 6. 启动服务
	if (!StartService(hServiceDDK, 0, NULL)) {
		DWORD error = GetLastError();
		if (error == ERROR_SERVICE_ALREADY_RUNNING) {
			printf("Service is already running\n");
			bRet = TRUE;
		} else {
			printf("Start service failed: %d\n", error);
		}
		goto cleanup;
	}
	
	// 7. 等待服务启动完成
	SERVICE_STATUS status;
	DWORD startTime = GetTickCount();
	while (TRUE) {
		if (!QueryServiceStatus(hServiceDDK, &status)) {
			break;
		}
		
		if (status.dwCurrentState == SERVICE_RUNNING) {
			printf("Service start success!\n");
			bRet = TRUE;
			break;
		}
		
		if (GetTickCount() - startTime > START_TIMEOUT) {
			printf("Service start timeout!\n");
			break;
		}
		
		Sleep(50); // 短暂等待
	}
	
cleanup:
	if (hServiceDDK) {
		CloseServiceHandle(hServiceDDK);
	}
	if (hServiceMgr) {
		CloseServiceHandle(hServiceMgr);
	}
	return bRet;
}

// 卸载驱动程序  
BOOL __cdecl UnloadNTDriver(char* szSvrName)
{
	BOOL bRet = FALSE;
	SC_HANDLE hServiceMgr = NULL;	// SCM管理器的句柄
	SC_HANDLE hServiceDDK = NULL;	// NT驱动程序的服务句柄
	SERVICE_STATUS status;		// 服务状态
	const DWORD STOP_TIMEOUT = 3000; // 停止超时时间(毫秒)
	
	// 1. 关闭设备句柄并重置状态
	if (drvhandle != INVALID_HANDLE_VALUE && drvhandle != NULL) {
		CloseHandle(drvhandle);
		drvhandle = INVALID_HANDLE_VALUE;
	}
	g_DriverState.status = DEVICE_STATUS_UNKNOWN;
	
	// 2. 打开服务管理器
	hServiceMgr = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
	if (!hServiceMgr) {
		printf("OpenSCManager() Failed: %d\n", GetLastError());
		return FALSE;
	}
	printf("OpenSCManager() ok ! \n");
	
	// 3. 打开服务
	hServiceDDK = OpenServiceA(hServiceMgr, szSvrName, SERVICE_ALL_ACCESS);
	if (!hServiceDDK) {
		DWORD error = GetLastError();
		if (error == ERROR_SERVICE_DOES_NOT_EXIST) {
			printf("Service does not exist, considering unload successful\n");
			bRet = TRUE;
		} else {
			printf("Open service failed: %d\n", error);
		}
		goto cleanup;
	}
	
	// 4. 获取服务状态
	if (!QueryServiceStatus(hServiceDDK, &status)) {
		printf("Get service status failed: %d\n", GetLastError());
		goto cleanup;
	}
	
	// 5. 如果服务正在运行，尝试停止
	if (status.dwCurrentState != SERVICE_STOPPED) {
		// 发送停止信号
		if (!ControlService(hServiceDDK, SERVICE_CONTROL_STOP, &status)) {
			DWORD error = GetLastError();
			if (error != ERROR_SERVICE_NOT_ACTIVE) {
				printf("Stop service failed: %d\n", error);
				goto cleanup;
			}
		}
		
		// 等待服务停止
		DWORD startTime = GetTickCount();
		while (status.dwCurrentState != SERVICE_STOPPED) {
			Sleep(50); // 短暂等待
			if (!QueryServiceStatus(hServiceDDK, &status)) {
				break;
			}
			
			if (GetTickCount() - startTime > STOP_TIMEOUT) {
				printf("Stop service timeout\n");
				goto cleanup;
			}
		}
	}
	
	// 6. 删除服务
	if (!DeleteService(hServiceDDK)) {
		DWORD error = GetLastError();
		if (error != ERROR_SERVICE_MARKED_FOR_DELETE) {
			printf("Delete service failed: %d\n", error);
			goto cleanup;
		}
	}
	
	printf("Service unload success!\n");
	bRet = TRUE;
	
cleanup:
	if (hServiceDDK) {
		CloseServiceHandle(hServiceDDK);
	}
	if (hServiceMgr) {
		CloseServiceHandle(hServiceMgr);
	}
	return bRet;
}

BOOL APIENTRY DllMain(HMODULE hModule,
	DWORD  ul_reason_for_call,
	LPVOID lpReserved
)
{/*
if (ul_reason_for_call==DLL_PROCESS_ATTACH)
  { LoadNTDriver("lykeys","lykeys.sys");
	drvhandle= CreateFileA("\\\\.\\lykeys",
		GENERIC_WRITE | GENERIC_READ,
		0,
		NULL,
		OPEN_EXISTING,
		0,
		NULL);
   }
else if (ul_reason_for_call==DLL_PROCESS_DETACH)
   {CloseHandle(drvhandle);
	  UnloadNTDriver("lykeys");
	}*/
	return TRUE;
}

#ifdef _MANAGED
#pragma managed(pop)
#endif
