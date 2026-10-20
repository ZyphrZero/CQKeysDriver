#include "stdafx.h"
#include "cqkeys.h"
#include "common.h"

// 前向声明
NTSTATUS SearchServiceFromKdbExt(PDRIVER_OBJECT KbdDriverObject, PDEVICE_OBJECT pPortDev);
NTSTATUS SearchServiceFromMouExt(PDRIVER_OBJECT MouDriverObject, PDEVICE_OBJECT pPortDev);
void cqkeysUnload(IN PDRIVER_OBJECT DriverObject);

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
__forceinline NTSTATUS cqkeysCreateClose(IN PIRP Irp)
{
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

__forceinline NTSTATUS cqkeysDefaultHandler(IN PDEVICE_OBJECT DeviceObject, IN PIRP Irp)
{
    Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Irp->IoStatus.Status;
}

// 优化设备控制函数
NTSTATUS cqkeysDispatchDeviceControl(IN PDEVICE_OBJECT DeviceObject, IN PIRP Irp)
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
        KdPrint(("Exception in cqkeysDispatchDeviceControl: 0x%X\n", status));
    }

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}

// 设备类型枚举
typedef enum _DEVICE_TYPE_CQ {  // 重命名以避免与Windows定义冲突
    DEVICE_TYPE_CQ_KEYBOARD,
    DEVICE_TYPE_CQ_MOUSE
} DEVICE_TYPE_CQ;

// 设备信息结构
typedef struct _DEVICE_SEARCH_INFO {
    UNICODE_STRING DriverName;      // 驱动名称
    PDRIVER_OBJECT DriverObject;    // 驱动对象
    PVOID DriverStart;             // 驱动起始地址
    ULONG DriverSize;              // 驱动大小
} DEVICE_SEARCH_INFO, *PDEVICE_SEARCH_INFO;

// 统一的设备搜索函数
NTSTATUS SearchDeviceCallback(DEVICE_TYPE_CQ DeviceType)
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
    if (DeviceType == DEVICE_TYPE_CQ_KEYBOARD) {
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
        if (DeviceType == DEVICE_TYPE_CQ_KEYBOARD) {
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
    return SearchDeviceCallback(DEVICE_TYPE_CQ_KEYBOARD);
}

NTSTATUS SearchMouServiceCallBack()
{
    return SearchDeviceCallback(DEVICE_TYPE_CQ_MOUSE);
}

// 设备扩展搜索函数的统一实现
NTSTATUS SearchDeviceExtension(
    PDRIVER_OBJECT ClassDriverObject,
    PDEVICE_OBJECT PortDevice,
    DEVICE_TYPE_CQ DeviceType)
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
        if (DeviceType == DEVICE_TYPE_CQ_KEYBOARD) {
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
            if (DeviceType == DEVICE_TYPE_CQ_KEYBOARD) {
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
            if (DeviceType == DEVICE_TYPE_CQ_KEYBOARD) {
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
    return SearchDeviceExtension(KbdDriverObject, pPortDev, DEVICE_TYPE_CQ_KEYBOARD);
}

NTSTATUS SearchServiceFromMouExt(PDRIVER_OBJECT MouDriverObject, PDEVICE_OBJECT pPortDev)
{
    return SearchDeviceExtension(MouDriverObject, pPortDev, DEVICE_TYPE_CQ_MOUSE);
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
            DriverObject->MajorFunction[i] = cqkeysDefaultHandler;
        }

        DriverObject->MajorFunction[IRP_MJ_CREATE] = [](PDEVICE_OBJECT, PIRP Irp) { return cqkeysCreateClose(Irp); };
        DriverObject->MajorFunction[IRP_MJ_CLOSE] = [](PDEVICE_OBJECT, PIRP Irp) { return cqkeysCreateClose(Irp); };
        DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = cqkeysDispatchDeviceControl;
        DriverObject->DriverUnload = cqkeysUnload;

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
            status = STATUS_UNEXPECTED_IO_ERROR;
            goto cleanup;
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
        // 清理全局回调对象
        g_KoMCallBack.Clear();
    }
    return status;
}

void cqkeysUnload(IN PDRIVER_OBJECT DriverObject)
{
    UNICODE_STRING win32Device;
    PDEVICE_OBJECT deviceObject = DriverObject->DeviceObject;
    LARGE_INTEGER interval;

    KdPrint(("开始卸载驱动...\n"));

    // 1. 删除符号链接
    RtlInitUnicodeString(&win32Device, KEYMOUSE_DOS_DEVICE_NAME);
    IoDeleteSymbolicLink(&win32Device);

    // 2. 等待100ms确保没有新的IRP
    interval.QuadPart = -1000000; // 100ms in 100ns units
    KeDelayExecutionThread(KernelMode, FALSE, &interval);

    if (deviceObject != NULL) {
        // 3. 清理设备扩展
        PDEVICE_EXTENSION deviceExtension = (PDEVICE_EXTENSION)deviceObject->DeviceExtension;
        if (deviceExtension != NULL) {
            deviceExtension->kbdDeviceObject = NULL;
            deviceExtension->mouDeviceObject = NULL;
            deviceExtension->My_KbdCallback = NULL;
            deviceExtension->My_MouCallback = NULL;
        }

        // 4. 清理全局回调对象
        g_KoMCallBack.Clear();

        // 5. 删除设备对象
        IoDeleteDevice(deviceObject);
    }

    KdPrint(("驱动卸载完成\n"));
}

NTSTATUS GetCqKeysInfo(PDEVICE_OBJECT DeviceObject, USHORT Index)
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
    BOOLEAN           hidDriverReferenced = FALSE;
    BOOLEAN           classDriverReferenced = FALSE;

    __try {
        deviceExtension = (PDEVICE_EXTENSION)DeviceObject->DeviceExtension;
        if (!deviceExtension) {
            status = STATUS_INVALID_PARAMETER;
            goto cleanup;
        }

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
            status = STATUS_INVALID_PARAMETER;
            goto cleanup;
        }

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
                goto cleanup;
            }
        }
        hidDriverReferenced = TRUE;

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
            goto cleanup;
        }
        classDriverReferenced = TRUE;

        classDriverStart = classDriverObject->DriverStart;
        classDriverSize = classDriverObject->DriverSize;

        ULONG             DeviceExtensionSize;
        PULONG            deviceExt;
        PDEVICE_OBJECT    tempDeviceObject;
        PDEVICE_OBJECT    currentDeviceObject = classDriverObject->DeviceObject;
        
        while (currentDeviceObject && MmIsAddressValid(currentDeviceObject))
        {
            tempDeviceObject = currentDeviceObject;
            while (tempDeviceObject && MmIsAddressValid(tempDeviceObject))
            {
                deviceExt = (PULONG)tempDeviceObject->DeviceExtension;
                if (!MmIsAddressValid(deviceExt)) {
                    break;
                }
                
                currentDeviceObject = classDriverObject->DeviceObject;
                DeviceExtensionSize = ((ULONG)tempDeviceObject->DeviceObjectExtension - (ULONG)tempDeviceObject->DeviceExtension) / 4;
                
                while (currentDeviceObject && MmIsAddressValid(currentDeviceObject))
                {
                    for (ULONG i = 0; i < DeviceExtensionSize - 1; i++)
                    {
                        if (!MmIsAddressValid(&deviceExt[i]) || !MmIsAddressValid(&deviceExt[i + 1])) {
                            break;
                        }

                        if (deviceExt[i] == (ULONG)currentDeviceObject &&
                            deviceExt[i + 1] > (ULONG)classDriverStart &&
                            deviceExt[i + 1] < (ULONG)classDriverStart + classDriverSize)
                        {
                            *TargetDeviceObject = (PVOID)deviceExt[i];
                            *TargetclassCallback = (PVOID)deviceExt[i + 1];
                            KdPrint(("%SDeviceObject == 0x%x\n", baseDeviceName, deviceExt[i]));
                            KdPrint(("%SClassServiceCallback == 0x%x\n", baseDeviceName, deviceExt[i + 1]));
                            status = STATUS_SUCCESS;
                            goto cleanup;
                        }
                    }
                    currentDeviceObject = currentDeviceObject->NextDevice;
                }
                tempDeviceObject = tempDeviceObject->AttachedDevice;
            }
            currentDeviceObject = currentDeviceObject->NextDevice;
        }
        status = STATUS_UNSUCCESSFUL;

    } __except(EXCEPTION_EXECUTE_HANDLER) {
        KdPrint(("Exception in GetCqKeysInfo: 0x%X\n", GetExceptionCode()));
        status = STATUS_UNSUCCESSFUL;
    }

cleanup:
    if (hidDriverReferenced && hidDriverObject) {
        ObDereferenceObject(hidDriverObject);
    }
    if (classDriverReferenced && classDriverObject) {
        ObDereferenceObject(classDriverObject);
    }
    return status;
}
