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

// 反调试保护宏
#define ANTI_DEBUG_CHECK() \
	if (IsDebuggerPresentEx()) { \
		SetLastError(ERROR_ACCESS_DENIED); \
		return FALSE; \
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
	DEVICE_STATUS_UNKNOWN = 0,
	DEVICE_STATUS_READY = 1,
	DEVICE_STATUS_ERROR = 2
} DEVICE_STATUS;

// 设备状态结构体
typedef struct _DEVICE_STATE_INFO {
	std::atomic<DEVICE_STATUS> status;
	std::atomic<ULONGLONG> lastCheckTime;
} DEVICE_STATE_INFO;

// 全局状态变量
namespace {
	DEVICE_STATE_INFO g_DriverState = { DEVICE_STATUS_UNKNOWN, 0 };
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
		return DEVICE_STATUS_ERROR;
	}

	// 尝试与驱动通信
	DWORD bytesReturned = 0;
	DWORD lastError = 0;
	
	if (!DeviceIoControl(deviceHandle, 
		IOCTL_KEYBOARD,  // 任意一个IOCTL都可以用来测试驱动状态
		NULL, 0,
		NULL, 0,
		&bytesReturned,
		NULL)) 
	{
		lastError = GetLastError();
		printf("Error: DeviceIoControl failed, error code: %d\n", lastError);
		return DEVICE_STATUS_ERROR;
	}

	return DEVICE_STATUS_READY;
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

// SetHandle函数
BOOL SetHandle()
{
	ANTI_DEBUG_CHECK();

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
		status == DEVICE_STATUS_ERROR ? "Error" : "Unknown");
	
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

// 鼠标相对移动
void __cdecl MouseMoveRELATIVE(LONG dx, LONG dy)
{
	if (!IsMouseReady()) {
		return;
	}

	MOUSE_INPUT_DATA mid;
	DWORD dwOutput;
	memset(&mid, 0, sizeof(MOUSE_INPUT_DATA));
	
	// 设置相对移动标志
	mid.Flags = MOUSE_MOVE_RELATIVE;
	
	// 1. 微小移动精确处理
	const float PRECISION_THRESHOLD = 10.0f;  // 精确移动阈值（像素）
	const int MAX_STEPS = 5;                  // 最大分步数
	const float MIN_STEP_TIME = 0.5f;         // 最小步进时间（毫秒）
	
	// 计算移动距离
	float distance = std::sqrt(static_cast<float>(dx * dx + dy * dy));
	
	if (distance < PRECISION_THRESHOLD) {
		// 对于微小移动，直接发送以保持精确性
		mid.LastX = dx;
		mid.LastY = dy;
		DeviceIoControl(drvhandle, IOCTL_MOUSE, &mid, sizeof(MOUSE_INPUT_DATA), NULL, 0, &dwOutput, NULL);
	} else {
		// 对于较大移动，进行分步处理以保持平滑性
		int steps = (std::min)((int)(distance / PRECISION_THRESHOLD), MAX_STEPS);
		float stepX = (float)dx / steps;
		float stepY = (float)dy / steps;
		
		// 计算每步的剩余量
		float remainderX = dx - (stepX * steps);
		float remainderY = dy - (stepY * steps);
		
		// 分步发送移动指令
		for (int i = 0; i < steps; i++) {
			mid.LastX = (LONG)stepX;
			mid.LastY = (LONG)stepY;
			DeviceIoControl(drvhandle, IOCTL_MOUSE, &mid, sizeof(MOUSE_INPUT_DATA), NULL, 0, &dwOutput, NULL);
			
			// 添加极短暂的延迟以确保平滑性，但不影响响应速度
			Sleep(0);  // 让出时间片但不实际等待
		}
		
		// 处理剩余的小数部分
		if (remainderX != 0 || remainderY != 0) {
			mid.LastX = (LONG)remainderX;
			mid.LastY = (LONG)remainderY;
			DeviceIoControl(drvhandle, IOCTL_MOUSE, &mid, sizeof(MOUSE_INPUT_DATA), NULL, 0, &dwOutput, NULL);
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

	// 获取主显示器信息
	const LONG screenWidth = GetSystemMetrics(SM_CXSCREEN);
	const LONG screenHeight = GetSystemMetrics(SM_CYSCREEN);

	// 边界检查和调整
	LONG adjustedX = (std::max)(0L, (std::min)(dx, screenWidth - 1));
	LONG adjustedY = (std::max)(0L, (std::min)(dy, screenHeight - 1));

	// 1. 设置绝对移动标志
	mid.Flags = MOUSE_MOVE_ABSOLUTE;

	// 2. 非线性插值优化
	// 在屏幕中心区域提供更高的精度，边缘区域提供更快的转向
	const float CENTER_ZONE = 0.3f;  // 中心区域范围（屏幕宽度的比例）
	const float CENTER_WEIGHT = 0.7f; // 中心区域权重

	// 计算到屏幕中心的距离
	float centerX = screenWidth / 2.0f;
	float centerY = screenHeight / 2.0f;
	float distanceX = std::abs(adjustedX - centerX) / centerX;
	float distanceY = std::abs(adjustedY - centerY) / centerY;

	// 应用非线性插值
	float scaleX = (distanceX < CENTER_ZONE) ? 
		(CENTER_WEIGHT + (1.0f - CENTER_WEIGHT) * (distanceX / CENTER_ZONE)) : 1.0f;
	float scaleY = (distanceY < CENTER_ZONE) ? 
		(CENTER_WEIGHT + (1.0f - CENTER_WEIGHT) * (distanceY / CENTER_ZONE)) : 1.0f;

	// 3. 坐标转换（使用64位整数避免溢出）
	// 将屏幕坐标映射到0-65535范围，考虑中心区域权重
	mid.LastX = (LONG)(((INT64)adjustedX * 65535 * scaleX) / screenWidth);
	mid.LastY = (LONG)(((INT64)adjustedY * 65535 * scaleY) / screenHeight);

	// 4. 确保最终坐标在有效范围内
	mid.LastX = (std::max)(0L, (std::min)(mid.LastX, 65535L));
	mid.LastY = (std::max)(0L, (std::min)(mid.LastY, 65535L));

	// 5. 发送移动命令
	DeviceIoControl(drvhandle, IOCTL_MOUSE, &mid, sizeof(MOUSE_INPUT_DATA), NULL, 0, &dwOutput, NULL);
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
	ANTI_DEBUG_CHECK();

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
	ANTI_DEBUG_CHECK();

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
