#include "pch.h"
#include "kmclass.h"
#include <windows.h>
#include <winsvc.h>  
#include <conio.h>  
#include <stdio.h>
#include <winternl.h>

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


//装载NT驱动程序
HANDLE drvhandle;
extern "C" __declspec(dllexport) void __cdecl SetHandle();
//extern "C" __declspec(dllexport) void __cdecl SetHandle(HANDLE handle);
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

// 设备状态枚举
typedef enum _DEVICE_STATUS {
	DEVICE_STATUS_UNKNOWN = 0,
	DEVICE_STATUS_READY = 1,
	DEVICE_STATUS_ERROR = 2,
	DEVICE_STATUS_DISCONNECTED = 3
} DEVICE_STATUS;

// 设备状态检测函数
DEVICE_STATUS CheckDeviceStatus(HANDLE deviceHandle) 
{
	if (deviceHandle == INVALID_HANDLE_VALUE || deviceHandle == NULL) {
		return DEVICE_STATUS_DISCONNECTED;
	}

	// 尝试与设备通信
	DWORD bytesReturned = 0;
	if (!DeviceIoControl(deviceHandle, 
		IOCTL_KEYBOARD,  // 使用一个无害的IOCTL来测试设备
		NULL, 0,
		NULL, 0,
		&bytesReturned,
		NULL)) 
	{
		DWORD error = GetLastError();
		if (error == ERROR_DEVICE_NOT_CONNECTED) {
			return DEVICE_STATUS_DISCONNECTED;
		}
		return DEVICE_STATUS_ERROR;
	}

	return DEVICE_STATUS_READY;
}

// 打印设备状态信息
void PrintDeviceStatus(DEVICE_STATUS status) 
{
	switch (status) {
		case DEVICE_STATUS_READY:
			printf("Device Status: Ready\n");
			break;
		case DEVICE_STATUS_ERROR:
			printf("Device Status: Error\n");
			break;
		case DEVICE_STATUS_DISCONNECTED:
			printf("Device Status: Disconnected\n");
			break;
		default:
			printf("Device Status: Unknown\n");
			break;
	}
}

// 修改SetHandle函数以包含状态检测
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

	// 检查设备状态
	DEVICE_STATUS status = CheckDeviceStatus(drvhandle);
	PrintDeviceStatus(status);
	
	if (status != DEVICE_STATUS_READY) {
		printf("Warning: Device not ready, operation may fail\n");
	}
}

//void SetHandle(HANDLE handle)
//{
//	drvhandle = handle;
//}


// 按键按下
void __cdecl KeyDown(USHORT VirtualKey)
{
	// 检查设备状态
	if (CheckDeviceStatus(drvhandle) != DEVICE_STATUS_READY) {
		printf("Error: Device not ready, cannot perform key operation\n");
		return;
	}

	KEYBOARD_INPUT_DATA  kid;
	DWORD dwOutput;
	memset(&kid, 0, sizeof(KEYBOARD_INPUT_DATA));
	kid.Flags = KEY_DOWN;
	kid.MakeCode = (USHORT)MapVirtualKey(VirtualKey, 0);

	DeviceIoControl(drvhandle, IOCTL_KEYBOARD, &kid, sizeof(KEYBOARD_INPUT_DATA), NULL, 0, &dwOutput, NULL);

}


void __cdecl KeyUp(USHORT VirtualKey)
{
	KEYBOARD_INPUT_DATA  kid;
	DWORD dwOutput;
	memset(&kid, 0, sizeof(KEYBOARD_INPUT_DATA));
	kid.Flags = KEY_UP;
	kid.MakeCode = (USHORT)MapVirtualKey(VirtualKey, 0);

	DeviceIoControl(drvhandle, IOCTL_KEYBOARD, &kid, sizeof(KEYBOARD_INPUT_DATA), NULL, 0, &dwOutput, NULL);

}


void __cdecl MouseLeftButtonDown()
{
	MOUSE_INPUT_DATA  mid;
	DWORD dwOutput;
	memset(&mid, 0, sizeof(MOUSE_INPUT_DATA));
	mid.ButtonFlags = MOUSE_LEFT_BUTTON_DOWN;
	DeviceIoControl(drvhandle, IOCTL_MOUSE, &mid, sizeof(MOUSE_INPUT_DATA), NULL, 0, &dwOutput, NULL);
}

void __cdecl MouseLeftButtonUp()
{
	MOUSE_INPUT_DATA  mid;
	DWORD dwOutput;
	memset(&mid, 0, sizeof(MOUSE_INPUT_DATA));
	mid.ButtonFlags = MOUSE_LEFT_BUTTON_UP;
	DeviceIoControl(drvhandle, IOCTL_MOUSE, &mid, sizeof(MOUSE_INPUT_DATA), NULL, 0, &dwOutput, NULL);
}
void __cdecl MouseRightButtonDown()
{
	MOUSE_INPUT_DATA  mid;
	DWORD dwOutput;
	memset(&mid, 0, sizeof(MOUSE_INPUT_DATA));
	mid.ButtonFlags = MOUSE_RIGHT_BUTTON_DOWN;
	DeviceIoControl(drvhandle, IOCTL_MOUSE, &mid, sizeof(MOUSE_INPUT_DATA), NULL, 0, &dwOutput, NULL);
}

void __cdecl MouseRightButtonUp()
{
	MOUSE_INPUT_DATA  mid;
	DWORD dwOutput;
	memset(&mid, 0, sizeof(MOUSE_INPUT_DATA));
	mid.ButtonFlags = MOUSE_RIGHT_BUTTON_UP;
	DeviceIoControl(drvhandle, IOCTL_MOUSE, &mid, sizeof(MOUSE_INPUT_DATA), NULL, 0, &dwOutput, NULL);
}

void __cdecl MouseMiddleButtonDown()
{
	MOUSE_INPUT_DATA  mid;
	DWORD dwOutput;
	memset(&mid, 0, sizeof(MOUSE_INPUT_DATA));
	mid.ButtonFlags = MOUSE_MIDDLE_BUTTON_DOWN;
	DeviceIoControl(drvhandle, IOCTL_MOUSE, &mid, sizeof(MOUSE_INPUT_DATA), NULL, 0, &dwOutput, NULL);
}

void __cdecl MouseMiddleButtonUp()
{
	MOUSE_INPUT_DATA  mid;
	DWORD dwOutput;
	memset(&mid, 0, sizeof(MOUSE_INPUT_DATA));
	mid.ButtonFlags = MOUSE_MIDDLE_BUTTON_UP;
	DeviceIoControl(drvhandle, IOCTL_MOUSE, &mid, sizeof(MOUSE_INPUT_DATA), NULL, 0, &dwOutput, NULL);
}

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

void __cdecl MouseXButton1Down()
{
	MOUSE_INPUT_DATA  mid;
	DWORD dwOutput;
	memset(&mid, 0, sizeof(MOUSE_INPUT_DATA));
	mid.ButtonFlags = MOUSE_BUTTON_4_DOWN;
	DeviceIoControl(drvhandle, IOCTL_MOUSE, &mid, sizeof(MOUSE_INPUT_DATA), NULL, 0, &dwOutput, NULL);
}

void __cdecl MouseXButton1Up()
{
	MOUSE_INPUT_DATA  mid;
	DWORD dwOutput;
	memset(&mid, 0, sizeof(MOUSE_INPUT_DATA));
	mid.ButtonFlags = MOUSE_BUTTON_4_UP;
	DeviceIoControl(drvhandle, IOCTL_MOUSE, &mid, sizeof(MOUSE_INPUT_DATA), NULL, 0, &dwOutput, NULL);
}

void __cdecl MouseXButton2Down()
{
	MOUSE_INPUT_DATA  mid;
	DWORD dwOutput;
	memset(&mid, 0, sizeof(MOUSE_INPUT_DATA));
	mid.ButtonFlags = MOUSE_BUTTON_5_DOWN;
	DeviceIoControl(drvhandle, IOCTL_MOUSE, &mid, sizeof(MOUSE_INPUT_DATA), NULL, 0, &dwOutput, NULL);
}

void __cdecl MouseXButton2Up()
{
	MOUSE_INPUT_DATA  mid;
	DWORD dwOutput;
	memset(&mid, 0, sizeof(MOUSE_INPUT_DATA));
	mid.ButtonFlags = MOUSE_BUTTON_5_UP;
	DeviceIoControl(drvhandle, IOCTL_MOUSE, &mid, sizeof(MOUSE_INPUT_DATA), NULL, 0, &dwOutput, NULL);
}

BOOL __cdecl LoadNTDriver(char* lpszDriverName, char* lpszDriverPath)
{
	if (!CheckSystemCompatibility()) {
		printf("System compatibility check failed\n");
		return FALSE;
	}
	
	//char szDriverImagePath[256];
	//得到完整的驱动路径
	//GetFullPathNameA(lpszDriverPath, 256, szDriverImagePath, NULL);

	BOOL bRet = FALSE;

	SC_HANDLE hServiceMgr = NULL;//SCM管理器的句柄
	SC_HANDLE hServiceDDK = NULL;//NT驱动程序的服务句柄

	//打开服务控制管理器
	hServiceMgr = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);

	if (hServiceMgr == NULL)
	{
		//OpenSCManager失败
		printf("OpenSCManager() Faild %d ! \n", GetLastError());
		bRet = FALSE;
		goto BeforeLeave;
	}
	else
	{
		////OpenSCManager成功
		printf("OpenSCManager() ok ! \n");
	}

	//创建驱动所对应的服务
	hServiceDDK = CreateServiceA(
		hServiceMgr,
		lpszDriverName,        //驱动程序的在注册表中的名字  
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

	DWORD dwRtn;
	//判断服务是否失败
	if (hServiceDDK == NULL)
	{
		dwRtn = GetLastError();
		if (dwRtn != ERROR_IO_PENDING && dwRtn != ERROR_SERVICE_EXISTS)
		{
			//由于其他原因创建服务失败
			printf("CrateService() Faild %d ! \n", dwRtn);
			bRet = FALSE;
			goto BeforeLeave;
		}
		else
		{
			//服务创建失败，是由于服务已经创立过
			printf("CrateService() Faild Service is ERROR_IO_PENDING or ERROR_SERVICE_EXISTS! \n");
		}

		// 驱动程序已经加载，只需要打开  
		hServiceDDK = OpenServiceA(hServiceMgr, lpszDriverName, SERVICE_ALL_ACCESS);
		if (hServiceDDK == NULL)
		{
			//如果打开服务也失败，则意味错误
			dwRtn = GetLastError();
			printf("OpenService() Faild %d ! \n", dwRtn);
			bRet = FALSE;
			goto BeforeLeave;
		}
		else
		{
			printf("OpenService() ok ! \n");
		}
	}
	else
	{
		printf("CrateService() ok ! \n");
	}

	//开启此项服务
	bRet = StartService(hServiceDDK, NULL, NULL);
	if (!bRet)
	{
		DWORD dwRtn = GetLastError();
		if (dwRtn != ERROR_IO_PENDING && dwRtn != ERROR_SERVICE_ALREADY_RUNNING)
		{
			printf("StartService() Faild %d ! \n", dwRtn);
			bRet = FALSE;
			goto BeforeLeave;
		}
		else
		{
			if (dwRtn == ERROR_IO_PENDING)
			{
				//设备被挂住
				printf("StartService() Faild ERROR_IO_PENDING ! \n");
				bRet = FALSE;
				goto BeforeLeave;
			}
			else
			{
				//服务已经开启
				printf("StartService() Faild ERROR_SERVICE_ALREADY_RUNNING ! \n");
				bRet = TRUE;
				goto BeforeLeave;
			}
		}
	}
	bRet = TRUE;
	//离开前关闭句柄
BeforeLeave:
	if (hServiceDDK)
	{
		CloseServiceHandle(hServiceDDK);
	}
	if (hServiceMgr)
	{
		CloseServiceHandle(hServiceMgr);
	}
	return bRet;
}

//卸载驱动程序  
BOOL __cdecl UnloadNTDriver(char* szSvrName)
{
	BOOL bRet = FALSE;
	SC_HANDLE hServiceMgr = NULL;//SCM管理器的句柄
	SC_HANDLE hServiceDDK = NULL;//NT驱动程序的服务句柄
	SERVICE_STATUS SvrSta;
	ZeroMemory(&SvrSta, sizeof(SERVICE_STATUS));  // 使用ZeroMemory初始化结构体
	//打开SCM管理器
	hServiceMgr = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
	if (hServiceMgr == NULL)
	{
		//带开SCM管理器失败
		printf("OpenSCManager() Faild %d ! \n", GetLastError());
		bRet = FALSE;
		goto BeforeLeave;
	}
	else
	{
		//带开SCM管理器失败成功
		printf("OpenSCManager() ok ! \n");
	}
	//打开驱动所对应的服务
	hServiceDDK = OpenServiceA(hServiceMgr, szSvrName, SERVICE_ALL_ACCESS);

	if (hServiceDDK == NULL)
	{
		//打开驱动所对应的服务失败
		printf("OpenService() Faild %d ! \n", GetLastError());
		bRet = FALSE;
		goto BeforeLeave;
	}
	else
	{
		printf("OpenService() ok ! \n");
	}
	//停止驱动程序，如果停止失败，只有重新启动才能，再动态加载。  
	if (!ControlService(hServiceDDK, SERVICE_CONTROL_STOP, &SvrSta))
	{
		printf("ControlService() Faild %d !\n", GetLastError());
	}
	else
	{
		//打开驱动所对应的失败
		printf("ControlService() ok !\n");
	}
	//动态卸载驱动程序。  
	if (!DeleteService(hServiceDDK))
	{
		//卸载失败
		printf("DeleteSrevice() Faild %d !\n", GetLastError());
	}
	else
	{
		//卸载成功
		printf("DelServer:DeleteSrevice() ok !\n");
	}
	bRet = TRUE;
BeforeLeave:
	//离开前关闭打开的句柄
	if (hServiceDDK)
	{
		CloseServiceHandle(hServiceDDK);
	}
	if (hServiceMgr)
	{
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

