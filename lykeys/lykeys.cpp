#include "stdafx.h"
#include "lykeys.h"
#include "common.h"

// 反hook保护相关结构体和函数
typedef struct _PROTECTED_REGION {
    PVOID StartAddress;
    SIZE_T Size;
    ULONG OriginalChecksum;
} PROTECTED_REGION, *PPROTECTED_REGION;

typedef struct _HOOK_PROTECTION {
    PROTECTED_REGION CodeRegion;
    PROTECTED_REGION CallbackRegion;
    PROTECTED_REGION DriverRegion;
    volatile LONG IsProtectionActive;
    KSPIN_LOCK ProtectionLock;
    KDPC VerificationDpc;
    KTIMER VerificationTimer;
} HOOK_PROTECTION, *PHOOK_PROTECTION;

// 全局保护对象
static HOOK_PROTECTION g_HookProtection = {0};

// 函数声明
NTSTATUS InitializeHookProtection();
void CleanupHookProtection();
BOOLEAN VerifyMemoryIntegrity(PPROTECTED_REGION Region);
VOID VerificationRoutine(PKDPC Dpc, PVOID Context, PVOID SystemArg1, PVOID SystemArg2);
NTSTATUS ProtectCallbackFunction(PVOID CallbackAddress, SIZE_T Size);
NTSTATUS UnprotectCallbackFunction(PVOID CallbackAddress);
BOOLEAN IsAddressInProtectedRegion(PVOID Address);
ULONG CalculateChecksum(PVOID Buffer, SIZE_T Size);

// 前向声明
NTSTATUS SearchServiceFromKdbExt(PDRIVER_OBJECT KbdDriverObject, PDEVICE_OBJECT pPortDev);
NTSTATUS SearchServiceFromMouExt(PDRIVER_OBJECT MouDriverObject, PDEVICE_OBJECT pPortDev);
void lykeysUnload(IN PDRIVER_OBJECT DriverObject);

// 系统版本信息结构
typedef struct _SYSTEM_VERSION_INFO {
    ULONG MajorVersion;    // 主版本号
    ULONG MinorVersion;    // 次版本号
    ULONG BuildNumber;     // 构建号
    BOOLEAN Is64Bit;       // 是否64位系统
} SYSTEM_VERSION_INFO, *PSYSTEM_VERSION_INFO;

// 内联获取系统版本信息函数
__forceinline NTSTATUS GetSystemVersionInfo(PSYSTEM_VERSION_INFO VersionInfo) 
{
    if (!VersionInfo) return STATUS_INVALID_PARAMETER;
    
    RTL_OSVERSIONINFOEXW osVersionInfo = { sizeof(RTL_OSVERSIONINFOEXW) };
    NTSTATUS status = RtlGetVersion(reinterpret_cast<PRTL_OSVERSIONINFOW>(&osVersionInfo));
    if (!NT_SUCCESS(status)) return status;
    
    VersionInfo->MajorVersion = osVersionInfo.dwMajorVersion;
    VersionInfo->MinorVersion = osVersionInfo.dwMinorVersion;
    VersionInfo->BuildNumber = osVersionInfo.dwBuildNumber;
    
#ifdef _WIN64
    VersionInfo->Is64Bit = TRUE;
#else
    VersionInfo->Is64Bit = FALSE;
#endif
    
    return STATUS_SUCCESS;
}

// 设备扩展结构
typedef struct _DEVICE_EXTENSION {
    PDEVICE_OBJECT kbdDeviceObject;      
    PDEVICE_OBJECT mouDeviceObject;      
    MY_KEYBOARDCALLBACK My_KbdCallback;   
    MY_MOUSECALLBACK My_MouCallback;     
} DEVICE_EXTENSION, *PDEVICE_EXTENSION;

// 回调结构
struct LY_CALLBACK {
    PDEVICE_OBJECT KdbDeviceObject;
    MY_KEYBOARDCALLBACK KeyboardClassServiceCallback;
    PDEVICE_OBJECT MouDeviceObject;
    MY_MOUSECALLBACK MouseClassServiceCallback;
    
    // 内联检查函数
    __forceinline bool HasKeyboardCallback() const {
        return KdbDeviceObject && KeyboardClassServiceCallback;
    }
    
    __forceinline bool HasMouseCallback() const {
        return MouDeviceObject && MouseClassServiceCallback;
    }
    
    // 内联清理函数
    __forceinline void Clear() {
        KdbDeviceObject = nullptr;
        KeyboardClassServiceCallback = nullptr;
        MouDeviceObject = nullptr;
        MouseClassServiceCallback = nullptr;
    }
} g_KoMCallBack;

// 内联处理函数
__forceinline NTSTATUS lykeysCreateClose(IN PIRP Irp)
{
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

__forceinline NTSTATUS lykeysDefaultHandler(IN PDEVICE_OBJECT DeviceObject, IN PIRP Irp)
{
    Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Irp->IoStatus.Status;
}

// 优化设备控制函数
NTSTATUS lykeysDispatchDeviceControl(IN PDEVICE_OBJECT DeviceObject, IN PIRP Irp)
{
    const PIO_STACK_LOCATION irpStack = IoGetCurrentIrpStackLocation(Irp);
    const PDEVICE_EXTENSION deviceExtension = static_cast<PDEVICE_EXTENSION>(DeviceObject->DeviceExtension);
    const PVOID ioBuffer = Irp->AssociatedIrp.SystemBuffer;
    const ULONG ioControlCode = irpStack->Parameters.DeviceIoControl.IoControlCode;
    NTSTATUS status = STATUS_SUCCESS;
    
    Irp->IoStatus.Information = 0;

    __try {
        switch (ioControlCode)
        {
        case IOCTL_KEYBOARD:
            if (ioBuffer && g_KoMCallBack.HasKeyboardCallback())
            {
                // 创建非const副本以匹配回调函数签名
                KEYBOARD_INPUT_DATA kid = *static_cast<PKEYBOARD_INPUT_DATA>(ioBuffer);
                ULONG inputDataConsumed;
                g_KoMCallBack.KeyboardClassServiceCallback(
                    g_KoMCallBack.KdbDeviceObject,
                    &kid,
                    &kid + 1,
                    &inputDataConsumed);
            }
            break;

        case IOCTL_MOUSE:
            if (ioBuffer && g_KoMCallBack.HasMouseCallback())
            {
                // 创建非const副本以匹配回调函数签名
                MOUSE_INPUT_DATA mid = *static_cast<PMOUSE_INPUT_DATA>(ioBuffer);
                ULONG inputDataConsumed;
                g_KoMCallBack.MouseClassServiceCallback(
                    g_KoMCallBack.MouDeviceObject,
                    &mid,
                    &mid + 1,
                    &inputDataConsumed);
            }
            break;

        default:
            status = STATUS_INVALID_PARAMETER;
            KdPrint(("Unknown IRP_MJ_DEVICE_CONTROL\n"));
            break;
        }
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        status = GetExceptionCode();
        KdPrint(("Exception in lykeysDispatchDeviceControl: 0x%X\n", status));
    }

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}

// 设备类型枚举
typedef enum _DEVICE_TYPE_LY {  // 重命名以避免与Windows定义冲突
    DEVICE_TYPE_LY_KEYBOARD,
    DEVICE_TYPE_LY_MOUSE
} DEVICE_TYPE_LY;

// 设备信息结构
typedef struct _DEVICE_SEARCH_INFO {
    UNICODE_STRING DriverName;      // 驱动名称
    PDRIVER_OBJECT DriverObject;    // 驱动对象
    PVOID DriverStart;             // 驱动起始地址
    ULONG DriverSize;              // 驱动大小
} DEVICE_SEARCH_INFO, *PDEVICE_SEARCH_INFO;

// 统一的设备搜索函数
NTSTATUS SearchDeviceCallback(DEVICE_TYPE_LY DeviceType) 
{
    NTSTATUS status = STATUS_SUCCESS;
    UNICODE_STRING usbDevName, ps2DevName, classDevName;
    PDRIVER_OBJECT usbDriverObj = NULL;
    PDRIVER_OBJECT ps2DriverObj = NULL;
    PDRIVER_OBJECT classDriverObj = NULL;
    PDEVICE_OBJECT targetDevice = NULL;
    
    // 初始化设备信息
    DEVICE_SEARCH_INFO devInfo = {0};
    
    // 根据设备类型设置相应的驱动名称
    if (DeviceType == DEVICE_TYPE_LY_KEYBOARD) {
        RtlInitUnicodeString(&usbDevName, L"\\Driver\\kbdhid");
        RtlInitUnicodeString(&ps2DevName, L"\\Driver\\i8042prt");
        RtlInitUnicodeString(&classDevName, L"\\Driver\\kbdclass");
    } else {
        RtlInitUnicodeString(&usbDevName, L"\\Driver\\mouhid");
        RtlInitUnicodeString(&ps2DevName, L"\\Driver\\i8042prt");
        RtlInitUnicodeString(&classDevName, L"\\Driver\\mouclass");
    }
    
    // 尝试获取USB设备驱动
    status = ObReferenceObjectByName(
        &usbDevName,
        OBJ_CASE_INSENSITIVE,
        NULL,
        0,
        *IoDriverObjectType,
        KernelMode,
        NULL,
        (PVOID*)&usbDriverObj
    );
    
    if (!NT_SUCCESS(status)) {
        KdPrint(("Failed to get USB driver, trying PS/2: 0x%08X\n", status));
        // 如果USB设备不存在，尝试PS/2设备
        status = ObReferenceObjectByName(
            &ps2DevName,
            OBJ_CASE_INSENSITIVE,
            NULL,
            0,
            *IoDriverObjectType,
            KernelMode,
            NULL,
            (PVOID*)&ps2DriverObj
        );
        
        if (!NT_SUCCESS(status)) {
            KdPrint(("Failed to get PS/2 driver: 0x%08X\n", status));
        }
    }
    
    // 检查是否至少有一个设备驱动可用
    if (!usbDriverObj && !ps2DriverObj) {
        KdPrint(("No available device drivers found\n"));
        status = STATUS_DEVICE_NOT_CONNECTED;
        goto cleanup;
    }
    
    // 获取类驱动
    status = ObReferenceObjectByName(
        &classDevName,
        OBJ_CASE_INSENSITIVE,
        NULL,
        0,
        *IoDriverObjectType,
        KernelMode,
        NULL,
        (PVOID*)&classDriverObj
    );
    
    if (!NT_SUCCESS(status)) {
        KdPrint(("Failed to get class driver object: 0x%08X\n", status));
        goto cleanup;
    }
    
    // 设置驱动信息
    devInfo.DriverObject = classDriverObj;
    devInfo.DriverStart = classDriverObj->DriverStart;
    devInfo.DriverSize = classDriverObj->DriverSize;
    
    // 获取目标设备对象
    PDRIVER_OBJECT activeDriver = usbDriverObj ? usbDriverObj : ps2DriverObj;
    if (activeDriver && activeDriver->DeviceObject) {
        targetDevice = activeDriver->DeviceObject;
        KdPrint(("Using %s driver\n", usbDriverObj ? "USB" : "PS/2"));
    } else {
        KdPrint(("No valid device object found\n"));
        status = STATUS_DEVICE_NOT_CONNECTED;
        goto cleanup;
    }
    
    // 搜索设备回调
    while (targetDevice) {
        if (DeviceType == DEVICE_TYPE_LY_KEYBOARD) {
            status = SearchServiceFromKdbExt(classDriverObj, targetDevice);
        } else {
            status = SearchServiceFromMouExt(classDriverObj, targetDevice);
        }
        
        if (NT_SUCCESS(status)) {
            KdPrint(("Successfully found device callback\n"));
            break;
        }
        targetDevice = targetDevice->NextDevice;
    }
    
    if (!NT_SUCCESS(status)) {
        KdPrint(("Failed to find device callback\n"));
    }
    
cleanup:
    // 清理资源
    if (usbDriverObj) {
        ObDereferenceObject(usbDriverObj);
    }
    if (ps2DriverObj) {
        ObDereferenceObject(ps2DriverObj);
    }
    if (classDriverObj) {
        ObDereferenceObject(classDriverObj);
    }
    
    return status;
}

NTSTATUS SearchKdbServiceCallBack()
{
    return SearchDeviceCallback(DEVICE_TYPE_LY_KEYBOARD);
}

NTSTATUS SearchMouServiceCallBack()
{
    return SearchDeviceCallback(DEVICE_TYPE_LY_MOUSE);
}

// 设备扩展搜索函数的统一实现
NTSTATUS SearchDeviceExtension(
    PDRIVER_OBJECT ClassDriverObject,
    PDEVICE_OBJECT PortDevice,
    DEVICE_TYPE_LY DeviceType)
{
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    
    // 1. 参数验证
    if (!ClassDriverObject || !PortDevice || !MmIsAddressValid(PortDevice)) {
        KdPrint(("Invalid parameters in SearchDeviceExtension\n"));
        return STATUS_INVALID_PARAMETER;
    }

    __try {
        PDEVICE_OBJECT targetDevice = NULL;
        PDEVICE_OBJECT tempDevice = PortDevice;
        UNICODE_STRING classDriverName;
        
        // 2. 设置类驱动名称
        if (DeviceType == DEVICE_TYPE_LY_KEYBOARD) {
            RtlInitUnicodeString(&classDriverName, L"\\Driver\\kbdclass");
        } else {
            RtlInitUnicodeString(&classDriverName, L"\\Driver\\mouclass");
        }
        
        // 3. 安全遍历设备栈
        while (tempDevice && MmIsAddressValid(tempDevice)) {
            // 检查AttachedDevice
            if (!tempDevice->AttachedDevice || !MmIsAddressValid(tempDevice->AttachedDevice)) {
                break;
            }
            
            // 检查DriverObject
            if (!tempDevice->AttachedDevice->DriverObject || 
                !MmIsAddressValid(tempDevice->AttachedDevice->DriverObject)) {
                break;
            }
            
            // 检查驱动名称
            if (RtlCompareUnicodeString(
                &tempDevice->AttachedDevice->DriverObject->DriverName,
                &classDriverName,
                TRUE) == 0) {
                targetDevice = tempDevice->AttachedDevice;
                break;
            }
            tempDevice = tempDevice->AttachedDevice;
        }
        
        if (!targetDevice) {
            KdPrint(("Target device not found\n"));
            return STATUS_NOT_FOUND;
        }
        
        // 4. 验证驱动范围
        if (!ClassDriverObject->DriverStart || !ClassDriverObject->DriverSize) {
            KdPrint(("Invalid driver range\n"));
            return STATUS_INVALID_PARAMETER;
        }
        
        PVOID driverStart = ClassDriverObject->DriverStart;
        ULONG driverSize = ClassDriverObject->DriverSize;
        
        // 5. 验证设备扩展
        if (!tempDevice->DeviceExtension || !MmIsAddressValid(tempDevice->DeviceExtension)) {
            KdPrint(("Invalid device extension\n"));
            return STATUS_INVALID_PARAMETER;
        }
        
        // 6. 搜索设备扩展
        PUCHAR deviceExt = (PUCHAR)tempDevice->DeviceExtension;
        const ULONG MAX_SEARCH_SIZE = 4096;
        BOOLEAN foundCallback = FALSE;
        
        for (ULONG i = 0; i < MAX_SEARCH_SIZE; i++, deviceExt++) {
            // 内存访问保护
            if (!MmIsAddressValid(deviceExt) || !MmIsAddressValid(deviceExt + sizeof(PVOID) - 1)) {
                break;
            }
            
            // 获取当前值
            PVOID value = *(PVOID*)deviceExt;
            if (!value || !MmIsAddressValid(value)) {
                continue;
            }
            
            // 设备对象匹配
            if (DeviceType == DEVICE_TYPE_LY_KEYBOARD) {
                if (value == targetDevice) {
                    g_KoMCallBack.KdbDeviceObject = targetDevice;
                    continue;
                }
                
                // 验证回调函数
                if (value > driverStart && 
                    value < (PVOID)((PUCHAR)driverStart + driverSize) &&
                    MmIsAddressValid(value)) {
                    g_KoMCallBack.KeyboardClassServiceCallback = (MY_KEYBOARDCALLBACK)value;
                    foundCallback = TRUE;
                }
                
                // 检查是否找到所需信息
                if (g_KoMCallBack.KdbDeviceObject && foundCallback) {
                    status = STATUS_SUCCESS;
                    break;
                }
            } else {
                if (value == targetDevice) {
                    g_KoMCallBack.MouDeviceObject = targetDevice;
                    continue;
                }
                
                // 验证回调函数
                if (value > driverStart && 
                    value < (PVOID)((PUCHAR)driverStart + driverSize) &&
                    MmIsAddressValid(value)) {
                    g_KoMCallBack.MouseClassServiceCallback = (MY_MOUSECALLBACK)value;
                    foundCallback = TRUE;
                }
                
                // 检查是否找到所需信息
                if (g_KoMCallBack.MouDeviceObject && foundCallback) {
                    status = STATUS_SUCCESS;
                    break;
                }
            }
        }
        
        // 7. 验证结果
        if (NT_SUCCESS(status)) {
            if (DeviceType == DEVICE_TYPE_LY_KEYBOARD) {
                if (!g_KoMCallBack.KdbDeviceObject || !g_KoMCallBack.KeyboardClassServiceCallback) {
                    status = STATUS_UNSUCCESSFUL;
                }
            } else {
                if (!g_KoMCallBack.MouDeviceObject || !g_KoMCallBack.MouseClassServiceCallback) {
                    status = STATUS_UNSUCCESSFUL;
                }
            }
        }
        
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        KdPrint(("Exception in SearchDeviceExtension: 0x%X\n", GetExceptionCode()));
        status = STATUS_UNSUCCESSFUL;
    }
    
    return status;
}

NTSTATUS SearchServiceFromKdbExt(PDRIVER_OBJECT KbdDriverObject, PDEVICE_OBJECT pPortDev)
{
    return SearchDeviceExtension(KbdDriverObject, pPortDev, DEVICE_TYPE_LY_KEYBOARD);
}

NTSTATUS SearchServiceFromMouExt(PDRIVER_OBJECT MouDriverObject, PDEVICE_OBJECT pPortDev)
{
    return SearchDeviceExtension(MouDriverObject, pPortDev, DEVICE_TYPE_LY_MOUSE);
}

// 计算内存区域校验和
ULONG CalculateChecksum(PVOID Buffer, SIZE_T Size) {
    if (!Buffer || !Size || !MmIsAddressValid(Buffer)) {
        return 0;
    }

    ULONG checksum = 0;
    PUCHAR data = (PUCHAR)Buffer;

    __try {
        for (SIZE_T i = 0; i < Size; i++) {
            if (!MmIsAddressValid(&data[i])) {
                return 0;
            }
            checksum += data[i];
            checksum = _rotl(checksum, 1);
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }

    return checksum;
}

// 验证内存区域完整性
BOOLEAN VerifyMemoryIntegrity(PPROTECTED_REGION Region) {
    if (!Region || !Region->StartAddress || !Region->Size) {
        return FALSE;
    }

    ULONG currentChecksum = CalculateChecksum(Region->StartAddress, Region->Size);
    return (currentChecksum == Region->OriginalChecksum);
}

// DPC例程用于定期验证内存完整性
VOID VerificationRoutine(
    PKDPC Dpc,
    PVOID Context,
    PVOID SystemArg1,
    PVOID SystemArg2
) {
    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(SystemArg1);
    UNREFERENCED_PARAMETER(SystemArg2);

    PHOOK_PROTECTION protection = (PHOOK_PROTECTION)Context;
    if (!protection) {
        KdPrint(("Invalid protection context\n"));
        return;
    }

    // 检查保护是否活动
    if (InterlockedCompareExchange(&protection->IsProtectionActive, 1, 1) != 1) {
        return;
    }

    // 获取自旋锁
    KIRQL oldIrql;
    KeAcquireSpinLock(&protection->ProtectionLock, &oldIrql);

    __try {
        // 验证所有受保护区域
        BOOLEAN codeIntegrity = TRUE;
        BOOLEAN callbackIntegrity = TRUE;
        BOOLEAN driverIntegrity = TRUE;

        if (protection->CodeRegion.StartAddress && protection->CodeRegion.Size) {
            codeIntegrity = VerifyMemoryIntegrity(&protection->CodeRegion);
        }

        if (protection->CallbackRegion.StartAddress && protection->CallbackRegion.Size) {
            callbackIntegrity = VerifyMemoryIntegrity(&protection->CallbackRegion);
        }

        if (protection->DriverRegion.StartAddress && protection->DriverRegion.Size) {
            driverIntegrity = VerifyMemoryIntegrity(&protection->DriverRegion);
        }

        // 如果检测到修改，记录日志并触发保护
        if (!codeIntegrity || !callbackIntegrity || !driverIntegrity) {
            KdPrint(("Memory integrity check failed: Code=%d, Callback=%d, Driver=%d\n",
                    codeIntegrity, callbackIntegrity, driverIntegrity));

            // 在实际生产环境中，您可以选择以下任一方式：
            // 1. 触发蓝屏
            // 2. 尝试恢复原始状态
            // 3. 记录事件并通知用户层
            // 这里我们选择记录事件而不是直接蓝屏
            
            KdPrint(("Hook detected! Memory integrity violation.\n"));
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        KdPrint(("Exception in VerificationRoutine: 0x%X\n", GetExceptionCode()));
    }

    KeReleaseSpinLock(&protection->ProtectionLock, oldIrql);
}

// 初始化hook保护
NTSTATUS InitializeHookProtection() {
    NTSTATUS status = STATUS_SUCCESS;

    __try {
        // 初始化自旋锁
        KeInitializeSpinLock(&g_HookProtection.ProtectionLock);

        // 初始化DPC
        KeInitializeDpc(&g_HookProtection.VerificationDpc,
                        VerificationRoutine,
                        &g_HookProtection);

        // 初始化定时器
        KeInitializeTimer(&g_HookProtection.VerificationTimer);

        // 设置定时器参数 (改为每5秒触发一次，减少系统负担)
        LARGE_INTEGER dueTime;
        dueTime.QuadPart = -50000000LL; // 5秒 = -50,000,000 * 100ns
        
        // 先将保护状态设置为活动
        InterlockedExchange(&g_HookProtection.IsProtectionActive, 1);

        // 启动定时器
        if (!KeSetTimerEx(&g_HookProtection.VerificationTimer,
                       dueTime,
                       5000, // 周期为5秒
                       &g_HookProtection.VerificationDpc)) {
            KdPrint(("Failed to set timer\n"));
            return STATUS_UNSUCCESSFUL;
        }

        KdPrint(("Hook protection initialized successfully\n"));
        return STATUS_SUCCESS;

    } __except(EXCEPTION_EXECUTE_HANDLER) {
        KdPrint(("Exception in InitializeHookProtection: 0x%X\n", GetExceptionCode()));
        return STATUS_UNSUCCESSFUL;
    }
}

// 清理hook保护
void CleanupHookProtection() {
    // 停止定时器
    KeCancelTimer(&g_HookProtection.VerificationTimer);
    
    // 等待所有DPC完成
    KeFlushQueuedDpcs();
    
    // 禁用保护
    InterlockedExchange(&g_HookProtection.IsProtectionActive, 0);
}

// 保护回调函数
NTSTATUS ProtectCallbackFunction(PVOID CallbackAddress, SIZE_T Size) {
    if (!CallbackAddress || !Size || !MmIsAddressValid(CallbackAddress)) {
        return STATUS_INVALID_PARAMETER;
    }

    KIRQL oldIrql;
    KeAcquireSpinLock(&g_HookProtection.ProtectionLock, &oldIrql);

    g_HookProtection.CallbackRegion.StartAddress = CallbackAddress;
    g_HookProtection.CallbackRegion.Size = Size;
    g_HookProtection.CallbackRegion.OriginalChecksum = 
        CalculateChecksum(CallbackAddress, Size);

    KeReleaseSpinLock(&g_HookProtection.ProtectionLock, oldIrql);
    return STATUS_SUCCESS;
}

// 取消回调函数保护
NTSTATUS UnprotectCallbackFunction(PVOID CallbackAddress) {
    if (!CallbackAddress) {
        return STATUS_INVALID_PARAMETER;
    }

    KIRQL oldIrql;
    KeAcquireSpinLock(&g_HookProtection.ProtectionLock, &oldIrql);

    if (g_HookProtection.CallbackRegion.StartAddress == CallbackAddress) {
        RtlZeroMemory(&g_HookProtection.CallbackRegion, 
                      sizeof(PROTECTED_REGION));
    }

    KeReleaseSpinLock(&g_HookProtection.ProtectionLock, oldIrql);
    return STATUS_SUCCESS;
}

// 检查地址是否在受保护区域内
BOOLEAN IsAddressInProtectedRegion(PVOID Address) {
    if (!Address) return FALSE;

    KIRQL oldIrql;
    KeAcquireSpinLock(&g_HookProtection.ProtectionLock, &oldIrql);

    BOOLEAN result = FALSE;
    
    // 检查代码区域
    if (g_HookProtection.CodeRegion.StartAddress &&
        Address >= g_HookProtection.CodeRegion.StartAddress &&
        Address < (PVOID)((PUCHAR)g_HookProtection.CodeRegion.StartAddress + 
                         g_HookProtection.CodeRegion.Size)) {
        result = TRUE;
    }
    
    // 检查回调区域
    else if (g_HookProtection.CallbackRegion.StartAddress &&
             Address >= g_HookProtection.CallbackRegion.StartAddress &&
             Address < (PVOID)((PUCHAR)g_HookProtection.CallbackRegion.StartAddress + 
                              g_HookProtection.CallbackRegion.Size)) {
        result = TRUE;
    }
    
    // 检查驱动区域
    else if (g_HookProtection.DriverRegion.StartAddress &&
             Address >= g_HookProtection.DriverRegion.StartAddress &&
             Address < (PVOID)((PUCHAR)g_HookProtection.DriverRegion.StartAddress + 
                              g_HookProtection.DriverRegion.Size)) {
        result = TRUE;
    }

    KeReleaseSpinLock(&g_HookProtection.ProtectionLock, oldIrql);
    return result;
}

//////////////////////////////////////////////////////////////////////////
#ifdef __cplusplus
extern "C"
#endif
NTSTATUS DriverEntry(IN PDRIVER_OBJECT DriverObject)
{
    UNICODE_STRING deviceName, win32Device;
    PDEVICE_OBJECT deviceObject = NULL;
    NTSTATUS status;
    BOOLEAN symbolicLinkCreated = FALSE;
    
    // 1. 获取系统版本信息
    SYSTEM_VERSION_INFO versionInfo;
    status = GetSystemVersionInfo(&versionInfo);
    if (!NT_SUCCESS(status)) {
        KdPrint(("Failed to get system version info: 0x%08X\n", status));
        return status;
    }
    
    // 2. 检查系统版本兼容性
    if (versionInfo.MajorVersion < 6) {
        KdPrint(("Unsupported Windows version: %d.%d\n", 
            versionInfo.MajorVersion, versionInfo.MinorVersion));
        return STATUS_NOT_SUPPORTED;
    }
    
    KdPrint(("Windows Version: %d.%d.%d (%s)\n", 
        versionInfo.MajorVersion, 
        versionInfo.MinorVersion,
        versionInfo.BuildNumber,
        versionInfo.Is64Bit ? "64-bit" : "32-bit"));

    __try {
        // 3. 初始化设备名称
        RtlInitUnicodeString(&deviceName, KEYMOUSE_DEVICE_NAME);
        RtlInitUnicodeString(&win32Device, KEYMOUSE_DOS_DEVICE_NAME);

        // 4. 设置分发函数
        for (ULONG i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++) {
            DriverObject->MajorFunction[i] = lykeysDefaultHandler;
        }

        DriverObject->MajorFunction[IRP_MJ_CREATE] = [](PDEVICE_OBJECT, PIRP Irp) { return lykeysCreateClose(Irp); };
        DriverObject->MajorFunction[IRP_MJ_CLOSE] = [](PDEVICE_OBJECT, PIRP Irp) { return lykeysCreateClose(Irp); };
        DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = lykeysDispatchDeviceControl;
        DriverObject->DriverUnload = lykeysUnload;

        // 5. 创建设备对象
        status = IoCreateDevice(
            DriverObject,
            sizeof(DEVICE_EXTENSION),
            &deviceName,
            FILE_DEVICE_KEYMOUSE,
            0,
            TRUE,
            &deviceObject);

        if (!NT_SUCCESS(status)) {
            KdPrint(("Failed to create device: 0x%08X\n", status));
            return status;
        }

        if (!deviceObject) {
            KdPrint(("Device object is NULL\n"));
            return STATUS_UNEXPECTED_IO_ERROR;
        }

        // 6. 设置设备标志
        deviceObject->Flags |= DO_DIRECT_IO;
        deviceObject->AlignmentRequirement = FILE_WORD_ALIGNMENT;

        // 7. 创建符号链接
        status = IoCreateSymbolicLink(&win32Device, &deviceName);
        if (!NT_SUCCESS(status)) {
            KdPrint(("Failed to create symbolic link: 0x%08X\n", status));
            goto cleanup;
        }
        symbolicLinkCreated = TRUE;

        // 8. 搜索键盘设备
        status = SearchKdbServiceCallBack();
        if (!NT_SUCCESS(status)) {
            KdPrint(("Failed to find keyboard device: 0x%08X\n", status));
            goto cleanup;
        }

        // 9. 搜索鼠标设备
        status = SearchMouServiceCallBack();
        if (!NT_SUCCESS(status)) {
            KdPrint(("Failed to find mouse device: 0x%08X\n", status));
            goto cleanup;
        }

        // 10. 根据系统版本调整设备标志
        deviceObject->Flags &= ~DO_DEVICE_INITIALIZING;
        
        if (versionInfo.MajorVersion >= 10) {
            deviceObject->Flags |= DO_POWER_PAGABLE;
        }
    
        if (versionInfo.Is64Bit) {
            deviceObject->AlignmentRequirement = FILE_64_BYTE_ALIGNMENT;
        }

        // 11. 初始化反hook保护
        if (NT_SUCCESS(status)) {
            // 保护驱动对象
            g_HookProtection.DriverRegion.StartAddress = DriverObject;
            g_HookProtection.DriverRegion.Size = sizeof(DRIVER_OBJECT);
            g_HookProtection.DriverRegion.OriginalChecksum = 
                CalculateChecksum(DriverObject, sizeof(DRIVER_OBJECT));

            // 保护代码段
            PVOID codeBase = DriverObject->DriverStart;
            SIZE_T codeSize = DriverObject->DriverSize;
            g_HookProtection.CodeRegion.StartAddress = codeBase;
            g_HookProtection.CodeRegion.Size = codeSize;
            g_HookProtection.CodeRegion.OriginalChecksum = 
                CalculateChecksum(codeBase, codeSize);

            // 保护回调函数
            if (g_KoMCallBack.KeyboardClassServiceCallback) {
                ProtectCallbackFunction(g_KoMCallBack.KeyboardClassServiceCallback, 
                                      PAGE_SIZE);
            }
            if (g_KoMCallBack.MouseClassServiceCallback) {
                ProtectCallbackFunction(g_KoMCallBack.MouseClassServiceCallback, 
                                      PAGE_SIZE);
            }

            // 最后启动保护机制
            status = InitializeHookProtection();
            if (!NT_SUCCESS(status)) {
                KdPrint(("Warning: Failed to initialize hook protection: 0x%08X\n", status));
                // 继续运行，不要因为保护初始化失败就退出
                status = STATUS_SUCCESS;
            }
        }

        KdPrint(("Driver initialized successfully\n"));
        return STATUS_SUCCESS;

    } __except(EXCEPTION_EXECUTE_HANDLER) {
        KdPrint(("Exception in DriverEntry: 0x%X\n", GetExceptionCode()));
        status = STATUS_UNSUCCESSFUL;
    }

cleanup:
    if (!NT_SUCCESS(status)) {
        if (symbolicLinkCreated) {
            IoDeleteSymbolicLink(&win32Device);
        }
        if (deviceObject) {
            IoDeleteDevice(deviceObject);
        }
        CleanupHookProtection();
    }
    return status;
}

void lykeysUnload(IN PDRIVER_OBJECT DriverObject)
{
    UNICODE_STRING win32Device;
    PDEVICE_OBJECT deviceObject = DriverObject->DeviceObject;
    LARGE_INTEGER interval;

    KdPrint(("开始卸载驱动...\n"));

    // 1. 清理反hook保护
    CleanupHookProtection();

    // 2. 删除符号链接
    RtlInitUnicodeString(&win32Device, KEYMOUSE_DOS_DEVICE_NAME);
    IoDeleteSymbolicLink(&win32Device);

    // 3. 等待100ms确保没有新的IRP
    interval.QuadPart = -1000000; // 100ms in 100ns units
    KeDelayExecutionThread(KernelMode, FALSE, &interval);

    if (deviceObject != NULL) {
        // 4. 清理设备扩展
        PDEVICE_EXTENSION deviceExtension = (PDEVICE_EXTENSION)deviceObject->DeviceExtension;
        if (deviceExtension != NULL) {
            // 取消回调函数保护
            if (deviceExtension->My_KbdCallback) {
                UnprotectCallbackFunction(deviceExtension->My_KbdCallback);
            }
            if (deviceExtension->My_MouCallback) {
                UnprotectCallbackFunction(deviceExtension->My_MouCallback);
            }

            deviceExtension->kbdDeviceObject = NULL;
            deviceExtension->mouDeviceObject = NULL;
            deviceExtension->My_KbdCallback = NULL;
            deviceExtension->My_MouCallback = NULL;
        }

        // 5. 清理全局回调对象
        g_KoMCallBack.Clear();

        // 6. 删除设备对象
        IoDeleteDevice(deviceObject);
    }

    KdPrint(("驱动卸载完成\n"));
}

NTSTATUS GetLyKeysInfo(PDEVICE_OBJECT DeviceObject, USHORT Index)
{
    NTSTATUS           status;
    UNICODE_STRING     ObjectName;
    PCWSTR            baseDeviceName, hidDriverName, classDriverName;
    PVOID             classDriverStart;
    ULONG             classDriverSize;
    PVOID*            TargetDeviceObject;
    PVOID*            TargetclassCallback;
    PDEVICE_EXTENSION deviceExtension;
    PDRIVER_OBJECT    hidDriverObject = NULL;
    PDRIVER_OBJECT    classDriverObject = NULL;

    deviceExtension = (PDEVICE_EXTENSION)DeviceObject->DeviceExtension;

    switch (Index)
    {
    case KEYBOARD_DEVICE:
        baseDeviceName = L"kbd";
        hidDriverName = L"\\Driver\\kbdhid";
        classDriverName = L"\\Driver\\kbdclass";
        TargetDeviceObject = (PVOID*)&(deviceExtension->kbdDeviceObject);
        TargetclassCallback = (PVOID*)&(deviceExtension->My_KbdCallback);
        break;
    case MOUSE_DEVICE:
        baseDeviceName = L"mou";
        hidDriverName = L"\\Driver\\mouhid";
        classDriverName = L"\\Driver\\mouclass";
        TargetDeviceObject = (PVOID*)&(deviceExtension->mouDeviceObject);
        TargetclassCallback = (PVOID*)&(deviceExtension->My_MouCallback);
        break;
    default:
        return STATUS_INVALID_PARAMETER;
    }

    // 通过USB类设备获取驱动对象
    RtlInitUnicodeString(&ObjectName, hidDriverName);
    status = ObReferenceObjectByName(&ObjectName,
        OBJ_CASE_INSENSITIVE,
        NULL,
        FILE_READ_ACCESS,
        *IoDriverObjectType,
        KernelMode,
        NULL,
        (PVOID*)&hidDriverObject);

    if (!NT_SUCCESS(status))
    {
        // 通过i8042prt获取驱动对象
        RtlInitUnicodeString(&ObjectName, L"\\Driver\\i8042prt");
        status = ObReferenceObjectByName(&ObjectName,
            OBJ_CASE_INSENSITIVE,
            NULL,
            FILE_READ_ACCESS,
            *IoDriverObjectType,
            KernelMode,
            NULL,
            (PVOID*)&hidDriverObject);
        if (!NT_SUCCESS(status))
        {
            KdPrint(("Couldn't Get the i8042prt Driver Object\n"));
            return status;
        }
    }

    // 通过class driver获取键盘鼠标类驱动对象
    RtlInitUnicodeString(&ObjectName, classDriverName);
    status = ObReferenceObjectByName(&ObjectName,
        OBJ_CASE_INSENSITIVE,
        NULL,
        FILE_READ_ACCESS,
        *IoDriverObjectType,
        KernelMode,
        NULL,
        (PVOID*)&classDriverObject);

    if (!NT_SUCCESS(status))
    {
        KdPrint(("Couldn't Get the Class Driver Object\n"));
        return status;
    }
    else
    {
        classDriverStart = classDriverObject->DriverStart;
        classDriverSize = classDriverObject->DriverSize;
    }

    ULONG             DeviceExtensionSize;
    PULONG            deviceExt;
    PDEVICE_OBJECT    tempDeviceObject;
    PDEVICE_OBJECT    currentDeviceObject = classDriverObject->DeviceObject;
    
    while (currentDeviceObject)
    {
        tempDeviceObject = currentDeviceObject;
        while (tempDeviceObject)
        {
            deviceExt = (PULONG)tempDeviceObject->DeviceExtension;
            currentDeviceObject = classDriverObject->DeviceObject;
            DeviceExtensionSize = ((ULONG)tempDeviceObject->DeviceObjectExtension - (ULONG)tempDeviceObject->DeviceExtension) / 4;
            while (currentDeviceObject)
            {
                for (ULONG i = 0; i < DeviceExtensionSize; i++)
                {
                    if (deviceExt[i] == (ULONG)currentDeviceObject &&
                        deviceExt[i + 1] > (ULONG)classDriverStart &&
                        deviceExt[i + 1] < (ULONG)classDriverStart + classDriverSize)
                    {
                        // 将获取到的设备对象保存到自定义扩展设备结构
                        *TargetDeviceObject = (PVOID)deviceExt[i];
                        *TargetclassCallback = (PVOID)deviceExt[i + 1];
                        KdPrint(("%SDeviceObject == 0x%x\n", baseDeviceName, deviceExt[i]));
                        KdPrint(("%SClassServiceCallback == 0x%x\n", baseDeviceName, deviceExt[i + 1]));
                        return STATUS_SUCCESS;
                    }
                }
                currentDeviceObject = currentDeviceObject->NextDevice;
            }
            tempDeviceObject = tempDeviceObject->AttachedDevice;
        }
        currentDeviceObject = currentDeviceObject->NextDevice;
    }
    return STATUS_UNSUCCESSFUL;
}
