#include "pch.h"
#include "kmclass.h"
#include <windows.h>
#include <winsvc.h>  
#include <conio.h>  
#include <stdio.h>
#include <winternl.h>
#include <atomic>
#include <algorithm>
#include <sysinfoapi.h>
#include <time.h>

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
extern "C" __declspec(dllexport) void __cdecl SetHandle();
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

// 检查驱动状态
DEVICE_STATUS CheckDriverStatus(HANDLE deviceHandle) 
{
	if (deviceHandle == INVALID_HANDLE_VALUE || deviceHandle == NULL) {
		return DEVICE_STATUS_ERROR;
	}

	// 尝试与驱动通信
	DWORD bytesReturned = 0;
	if (!DeviceIoControl(deviceHandle, 
		IOCTL_KEYBOARD,  // 任意一个IOCTL都可以用来测试驱动状态
		NULL, 0,
		NULL, 0,
		&bytesReturned,
		NULL)) 
	{
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

// 修改SetHandle函数
void SetHandle()
{
	drvhandle = CreateFileA(
		"\\\\.\\kmclass",
		GENERIC_WRITE | GENERIC_READ,
		0,
		NULL,
		OPEN_EXISTING,
		0,
		0);

	// 初始化驱动状态
	CheckDeviceStatus();
	
	// 打印初始状态
	printf("Driver Status: %d\n", (int)g_DriverState.status.load());
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
	MOUSE_INPUT_DATA  mid;
	DWORD dwOutput;
	memset(&mid, 0, sizeof(MOUSE_INPUT_DATA));
	mid.Flags = MOUSE_MOVE_RELATIVE;
	mid.LastX = dx;
	mid.LastY = dy;
	DeviceIoControl(drvhandle, IOCTL_MOUSE, &mid, sizeof(MOUSE_INPUT_DATA), NULL, 0, &dwOutput, NULL);
}

// 鼠标绝对移动
void __cdecl MouseMoveABSOLUTE(LONG dx, LONG dy)
{
	MOUSE_INPUT_DATA  mid;
	DWORD dwOutput;
	memset(&mid, 0, sizeof(MOUSE_INPUT_DATA));
	mid.Flags = MOUSE_MOVE_ABSOLUTE;
	mid.LastX = dx * 0xffff / GetSystemMetrics(SM_CXSCREEN);
	mid.LastY = dy * 0xffff / GetSystemMetrics(SM_CYSCREEN);
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

// 装载驱动程序
BOOL __cdecl LoadNTDriver(char* lpszDriverName, char* lpszDriverPath)
{
	BOOL bRet = FALSE;
	SC_HANDLE hServiceMgr = NULL;
	SC_HANDLE hServiceDDK = NULL;
	const DWORD START_TIMEOUT = 3000; // 启动超时时间(毫秒)
	
	// 1. 快速检查系统兼容性
	if (!CheckSystemCompatibility()) {
		printf("System compatibility check failed\n");
		return FALSE;
	}
	
	// 2. 打开服务管理器
	hServiceMgr = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
	if (!hServiceMgr) {
		printf("OpenSCManager() Failed: %d\n", GetLastError());
		return FALSE;
	}
	
	// 3. 尝试打开已存在的服务
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
	
	// 4. 创建新服务
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
	
	// 5. 启动服务
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
	
	// 6. 等待服务启动完成
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
  { LoadNTDriver("kmclass","kmclass.sys");
	drvhandle= CreateFileA("\\\\.\\kmclass",
		GENERIC_WRITE | GENERIC_READ,
		0,
		NULL,
		OPEN_EXISTING,
		0,
		NULL);
   }
else if (ul_reason_for_call==DLL_PROCESS_DETACH)
   {CloseHandle(drvhandle);
	  UnloadNTDriver("kmclass");
	}*/
	return TRUE;
}

#ifdef _MANAGED
#pragma managed(pop)
#endif

