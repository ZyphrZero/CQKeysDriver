#include "stdafx.h"
#include "kmclass.h"
#include "common.h"

// 前向声明
NTSTATUS SearchServiceFromKdbExt(PDRIVER_OBJECT KbdDriverObject, PDEVICE_OBJECT pPortDev);
NTSTATUS SearchServiceFromMouExt(PDRIVER_OBJECT MouDriverObject, PDEVICE_OBJECT pPortDev);

// 系统版本信息结构
typedef struct _SYSTEM_VERSION_INFO {
    ULONG MajorVersion;    // 主版本号
    ULONG MinorVersion;    // 次版本号
    ULONG BuildNumber;     // 构建号
    BOOLEAN Is64Bit;       // 是否64位系统
} SYSTEM_VERSION_INFO, *PSYSTEM_VERSION_INFO;

// 获取系统版本信息
NTSTATUS GetSystemVersionInfo(PSYSTEM_VERSION_INFO VersionInfo) 
{
    RTL_OSVERSIONINFOEXW osVersionInfo = { 0 };
    osVersionInfo.dwOSVersionInfoSize = sizeof(RTL_OSVERSIONINFOEXW);
    
    NTSTATUS status = RtlGetVersion((PRTL_OSVERSIONINFOW)&osVersionInfo);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    
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

typedef struct _DEVICE_EXTENSION {

    PDEVICE_OBJECT       kbdDeviceObject;        //键盘类设备对象
    PDEVICE_OBJECT       mouDeviceObject;        //鼠标类设备对象
    MY_KEYBOARDCALLBACK  My_KbdCallback;         //KeyboardClassServiceCallback函数 
    MY_MOUSECALLBACK     My_MouCallback;         //MouseClassServiceCallback函数

}DEVICE_EXTENSION, * PDEVICE_EXTENSION;

struct
{
    PDEVICE_OBJECT KdbDeviceObject;
    MY_KEYBOARDCALLBACK KeyboardClassServiceCallback;
    PDEVICE_OBJECT MouDeviceObject;
    MY_MOUSECALLBACK MouseClassServiceCallback;
}g_KoMCallBack;

void kmclassUnload(IN PDRIVER_OBJECT DriverObject);
NTSTATUS kmclassCreateClose(IN PDEVICE_OBJECT DeviceObject, IN PIRP Irp);
NTSTATUS kmclassDefaultHandler(IN PDEVICE_OBJECT DeviceObject, IN PIRP Irp);
NTSTATUS kmclassDispatchDeviceControl(IN PDEVICE_OBJECT DeviceObject, IN PIRP Irp);
NTSTATUS GetKmclassInfo(PDEVICE_OBJECT DeviceObject, USHORT Index);

// 设备类型枚举
typedef enum _DEVICE_TYPE_KM {  // 重命名以避免与Windows定义冲突
    DEVICE_TYPE_KM_KEYBOARD,
    DEVICE_TYPE_KM_MOUSE
} DEVICE_TYPE_KM;

// 设备信息结构
typedef struct _DEVICE_SEARCH_INFO {
    UNICODE_STRING DriverName;      // 驱动名称
    PDRIVER_OBJECT DriverObject;    // 驱动对象
    PVOID DriverStart;             // 驱动起始地址
    ULONG DriverSize;              // 驱动大小
} DEVICE_SEARCH_INFO, *PDEVICE_SEARCH_INFO;

// 统一的设备搜索函数
NTSTATUS SearchDeviceCallback(DEVICE_TYPE_KM DeviceType) 
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
    if (DeviceType == DEVICE_TYPE_KM_KEYBOARD) {
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
        if (DeviceType == DEVICE_TYPE_KM_KEYBOARD) {
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

// 修改原有的搜索函数调用新的统一搜索函数
NTSTATUS SearchKdbServiceCallBack(IN PDRIVER_OBJECT DriverObject)
{
    return SearchDeviceCallback(DEVICE_TYPE_KM_KEYBOARD);
}

NTSTATUS SearchMouServiceCallBack(IN PDRIVER_OBJECT DriverObject)
{
    return SearchDeviceCallback(DEVICE_TYPE_KM_MOUSE);
}

// 设备扩展搜索函数的统一实现
NTSTATUS SearchDeviceExtension(
    PDRIVER_OBJECT ClassDriverObject,
    PDEVICE_OBJECT PortDevice,
    DEVICE_TYPE_KM DeviceType)
{
    if (!ClassDriverObject || !PortDevice) {
        return STATUS_INVALID_PARAMETER;
    }

    NTSTATUS status = STATUS_UNSUCCESSFUL;
    PDEVICE_OBJECT targetDevice = NULL;
    PDEVICE_OBJECT tempDevice = PortDevice;
    UNICODE_STRING classDriverName;
    
    // 设置类驱动名称
    if (DeviceType == DEVICE_TYPE_KM_KEYBOARD) {
        RtlInitUnicodeString(&classDriverName, L"\\Driver\\kbdclass");
    } else {
        RtlInitUnicodeString(&classDriverName, L"\\Driver\\mouclass");
    }
    
    // 使用链表遍历算法
    while (tempDevice->AttachedDevice != NULL) {
        // 检查是否是目标类驱动
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
        return STATUS_NOT_FOUND;
    }
    
    // 获取驱动范围
    PVOID driverStart = ClassDriverObject->DriverStart;
    ULONG driverSize = ClassDriverObject->DriverSize;
    
    // 搜索设备扩展
    PUCHAR deviceExt = (PUCHAR)tempDevice->DeviceExtension;
    const ULONG MAX_SEARCH_SIZE = 4096;  // 最大搜索范围
    
    __try {
        // 使用分层线性扫描算法
        for (ULONG i = 0; i < MAX_SEARCH_SIZE; i++, deviceExt++) {
            // 1. 内存安全检查
            if (!MmIsAddressValid(deviceExt)) break;

            // 2. 内存访问保护，获取当前位置的值
            PVOID value = *(PVOID*)deviceExt;
            
            // 3. 设备对象匹配
            if (DeviceType == DEVICE_TYPE_KM_KEYBOARD) {
                // 4. 检查是否已经找到设备对象和回调函数
                if (g_KoMCallBack.KdbDeviceObject && g_KoMCallBack.KeyboardClassServiceCallback) {
                    status = STATUS_SUCCESS;
                    break;
                }
                
                // 5. 设备对象匹配
                if (value == targetDevice) {
                    g_KoMCallBack.KdbDeviceObject = targetDevice;
                    continue;
                }
            } else {
                if (g_KoMCallBack.MouDeviceObject && g_KoMCallBack.MouseClassServiceCallback) {
                    status = STATUS_SUCCESS;
                    break;
                }
                
                if (value == targetDevice) {
                    g_KoMCallBack.MouDeviceObject = targetDevice;
                    continue;
                }
            }
            
            // 6. 验证是否为回调函数
            if (value > driverStart && 
                value < (PVOID)((PUCHAR)driverStart + driverSize) &&
                MmIsAddressValid(value)) {
                // 7. 记录回调函数
                if (DeviceType == DEVICE_TYPE_KM_KEYBOARD) {
                    g_KoMCallBack.KeyboardClassServiceCallback = (MY_KEYBOARDCALLBACK)value;
                } else {
                    g_KoMCallBack.MouseClassServiceCallback = (MY_MOUSECALLBACK)value;
                }
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        KdPrint(("Exception occurred while searching device extension: 0x%X\n", 
            GetExceptionCode()));
        return STATUS_UNSUCCESSFUL;
    }
    
    return status;
}

// 替换原有的搜索函数
NTSTATUS SearchServiceFromKdbExt(PDRIVER_OBJECT KbdDriverObject, PDEVICE_OBJECT pPortDev)
{
    return SearchDeviceExtension(KbdDriverObject, pPortDev, DEVICE_TYPE_KM_KEYBOARD);
}

NTSTATUS SearchServiceFromMouExt(PDRIVER_OBJECT MouDriverObject, PDEVICE_OBJECT pPortDev)
{
    return SearchDeviceExtension(MouDriverObject, pPortDev, DEVICE_TYPE_KM_MOUSE);
}

//////////////////////////////////////////////////////////////////////////
#ifdef __cplusplus
extern "C"
#endif
NTSTATUS DriverEntry(IN PDRIVER_OBJECT DriverObject, IN PUNICODE_STRING  RegistryPath)
{
    UNICODE_STRING     DeviceName, Win32Device;
    PDEVICE_OBJECT     DeviceObject = NULL;
    NTSTATUS           status;
    SYSTEM_VERSION_INFO versionInfo;
    
    // 获取系统版本信息
    status = GetSystemVersionInfo(&versionInfo);
    if (!NT_SUCCESS(status)) {
        KdPrint(("Failed to get system version info: 0x%08X\n", status));
        return status;
    }
    
    // 检查系统版本兼容性
    if (versionInfo.MajorVersion < 6) {  // 低于Windows Vista
        KdPrint(("Unsupported Windows version: %d.%d\n", 
            versionInfo.MajorVersion, versionInfo.MinorVersion));
        return STATUS_NOT_SUPPORTED;
    }
    
    KdPrint(("Windows Version: %d.%d.%d (%s)\n", 
        versionInfo.MajorVersion, 
        versionInfo.MinorVersion,
        versionInfo.BuildNumber,
        versionInfo.Is64Bit ? "64-bit" : "32-bit"));

    RtlInitUnicodeString(&DeviceName, KEYMOUSE_DEVICE_NAME);
    RtlInitUnicodeString(&Win32Device, KEYMOUSE_DOS_DEVICE_NAME);

    for (ULONG i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++)
        DriverObject->MajorFunction[i] = kmclassDefaultHandler;

    DriverObject->MajorFunction[IRP_MJ_CREATE] = kmclassCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = kmclassCreateClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = kmclassDispatchDeviceControl;

    DriverObject->DriverUnload = kmclassUnload;
    status = IoCreateDevice(DriverObject, sizeof(DEVICE_EXTENSION), &DeviceName, FILE_DEVICE_KEYMOUSE, 0, TRUE, &DeviceObject);

    if (!NT_SUCCESS(status))
        return status;
    if (!DeviceObject)
        return STATUS_UNEXPECTED_IO_ERROR;

    DeviceObject->Flags |= DO_DIRECT_IO;
    DeviceObject->AlignmentRequirement = FILE_WORD_ALIGNMENT;
    status = IoCreateSymbolicLink(&Win32Device, &DeviceName);

    //    status = GetKmclassInfo(DeviceObject, KEYBOARD_DEVICE);
    status = SearchKdbServiceCallBack(DriverObject);
    if (!NT_SUCCESS(status))
    {
        KdPrint(("KEYBOARD_DEVICE ERROR, error = 0x%08lx\n", status));
        return status;
    }

    //    status = GetKmclassInfo(DeviceObject, MOUSE_DEVICE);
    status = SearchMouServiceCallBack(DriverObject);
    if (!NT_SUCCESS(status))
    {
        KdPrint(("MOUSE_DEVICE ERROR, error = 0x%08lx\n", status));
        return status;
    }

    DeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

    // 根据系统版本调整驱动行为
    if (versionInfo.MajorVersion >= 10) {  // Windows 10及以上
        DeviceObject->Flags |= DO_POWER_PAGABLE;
    }
    
    if (versionInfo.Is64Bit) {
        // 64位系统特定处理
        DeviceObject->AlignmentRequirement = FILE_64_BYTE_ALIGNMENT;
    }

    return STATUS_SUCCESS;
}

void kmclassUnload(IN PDRIVER_OBJECT DriverObject)
{
    UNICODE_STRING Win32Device;
    RtlInitUnicodeString(&Win32Device, KEYMOUSE_DOS_DEVICE_NAME);
    IoDeleteSymbolicLink(&Win32Device);
    IoDeleteDevice(DriverObject->DeviceObject);
}

NTSTATUS kmclassCreateClose(IN PDEVICE_OBJECT DeviceObject, IN PIRP Irp)
{
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

NTSTATUS kmclassDefaultHandler(IN PDEVICE_OBJECT DeviceObject, IN PIRP Irp)
{
    Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Irp->IoStatus.Status;
}

NTSTATUS kmclassDispatchDeviceControl(IN PDEVICE_OBJECT DeviceObject, IN PIRP Irp)
{
    NTSTATUS			 status = STATUS_SUCCESS;
    PIO_STACK_LOCATION	 irpStack;
    PDEVICE_EXTENSION	 deviceExtension;
    ULONG				 ioControlCode;
    PKEYBOARD_INPUT_DATA KbdInputDataStart, KbdInputDataEnd;
    PMOUSE_INPUT_DATA 	 MouseInputDataStart, MouseInputDataEnd;
    ULONG 				 InputDataConsumed;
    PVOID				 ioBuffer;

    irpStack = IoGetCurrentIrpStackLocation(Irp);
    deviceExtension = (PDEVICE_EXTENSION)DeviceObject->DeviceExtension;

    ioBuffer = Irp->AssociatedIrp.SystemBuffer;
    ioControlCode = irpStack->Parameters.DeviceIoControl.IoControlCode;
    Irp->IoStatus.Information = 0;

    switch (ioControlCode)
    {
    case IOCTL_KEYBOARD:
        if (ioBuffer)
        {
            KEYBOARD_INPUT_DATA kid = *(PKEYBOARD_INPUT_DATA)ioBuffer;
            KbdInputDataStart = &kid;
            KbdInputDataEnd = KbdInputDataStart + 1;
            //deviceExtension->My_KbdCallback(deviceExtension->kbdDeviceObject,
            g_KoMCallBack.KeyboardClassServiceCallback(g_KoMCallBack.KdbDeviceObject,
                KbdInputDataStart,
                KbdInputDataEnd,
                &InputDataConsumed);

            status = STATUS_SUCCESS;
        }
        break;

    case IOCTL_MOUSE:
        if (ioBuffer)
        {
            MOUSE_INPUT_DATA mid = *(PMOUSE_INPUT_DATA)ioBuffer;
            MouseInputDataStart = &mid;
            MouseInputDataEnd = MouseInputDataStart + 1;

            //deviceExtension->My_MouCallback(deviceExtension->mouDeviceObject,
            g_KoMCallBack.MouseClassServiceCallback(g_KoMCallBack.MouDeviceObject,
                MouseInputDataStart,
                MouseInputDataEnd,
                &InputDataConsumed);

            status = STATUS_SUCCESS;
        }
        break;

    default:
        status = STATUS_INVALID_PARAMETER;
        KdPrint(("\nunknown IRP_MJ_DEVICE_CONTROL\n"));
        break;
    }

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}


NTSTATUS GetKmclassInfo(PDEVICE_OBJECT DeviceObject, USHORT Index)
{
    NTSTATUS           status;
    UNICODE_STRING     ObjectName;
    PCWSTR             kmhidName, kmclassName, kmName;
    PVOID              kmDriverStart;
    ULONG              kmDriverSize;
    PVOID* TargetDeviceObject;
    PVOID* TargetclassCallback;
    PDEVICE_EXTENSION  deviceExtension;
    PDRIVER_OBJECT     kmDriverObject = NULL;
    PDRIVER_OBJECT     kmclassDriverObject = NULL;


    deviceExtension = (PDEVICE_EXTENSION)DeviceObject->DeviceExtension;

    switch (Index)
    {
    case KEYBOARD_DEVICE:
        kmName = L"kbd";
        kmhidName = L"\\Driver\\kbdhid";
        kmclassName = L"\\Driver\\kbdclass";
        TargetDeviceObject = (PVOID*)&(deviceExtension->kbdDeviceObject);
        TargetclassCallback = (PVOID*)&(deviceExtension->My_KbdCallback);
        break;
    case MOUSE_DEVICE:
        kmName = L"mou";
        kmhidName = L"\\Driver\\mouhid";
        kmclassName = L"\\Driver\\mouclass";
        TargetDeviceObject = (PVOID*)&(deviceExtension->mouDeviceObject);
        TargetclassCallback = (PVOID*)&(deviceExtension->My_MouCallback);
        break;
    default:
        return STATUS_INVALID_PARAMETER;
    }


    // 通过USB类设备获取驱动对象
    RtlInitUnicodeString(&ObjectName, kmhidName);
    status = ObReferenceObjectByName(&ObjectName,
        OBJ_CASE_INSENSITIVE,
        NULL,
        FILE_READ_ACCESS,
        *IoDriverObjectType,
        KernelMode,
        NULL,
        (PVOID*)&kmDriverObject);


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
            (PVOID*)&kmDriverObject);
        if (!NT_SUCCESS(status))
        {
            KdPrint(("Couldn't Get the i8042prt Driver Object\n"));
            return status;
        }
    }

    // 通过kmclass获取键盘鼠标类驱动对象
    RtlInitUnicodeString(&ObjectName, kmclassName);
    status = ObReferenceObjectByName(&ObjectName,
        OBJ_CASE_INSENSITIVE,
        NULL,
        FILE_READ_ACCESS,
        *IoDriverObjectType,
        KernelMode,
        NULL,
        (PVOID*)&kmclassDriverObject);

    if (!NT_SUCCESS(status))
    {
        KdPrint(("Couldn't Get the kmclass Driver Object\n"));
        return status;
    }
    else
    {
        kmDriverStart = kmclassDriverObject->DriverStart;
        kmDriverSize = kmclassDriverObject->DriverSize;
    }

    ULONG             DeviceExtensionSize;
    PULONG            kmDeviceExtension;
    PDEVICE_OBJECT    kmTempDeviceObject;
    PDEVICE_OBJECT    kmclassDeviceObject;
    PDEVICE_OBJECT    kmDeviceObject = kmDriverObject->DeviceObject;
    while (kmDeviceObject)
    {
        kmTempDeviceObject = kmDeviceObject;
        while (kmTempDeviceObject)
        {
            kmDeviceExtension = (PULONG)kmTempDeviceObject->DeviceExtension;
            kmclassDeviceObject = kmclassDriverObject->DeviceObject;
            DeviceExtensionSize = ((ULONG)kmTempDeviceObject->DeviceObjectExtension - (ULONG)kmTempDeviceObject->DeviceExtension) / 4;
            while (kmclassDeviceObject)
            {
                for (ULONG i = 0; i < DeviceExtensionSize; i++)
                {
                    if (kmDeviceExtension[i] == (ULONG)kmclassDeviceObject &&
                        kmDeviceExtension[i + 1] > (ULONG)kmDriverStart &&
                        kmDeviceExtension[i + 1] < (ULONG)kmDriverStart + kmDriverSize)
                    {
                        // 将获取到的设备对象保存到自定义扩展设备结构
                        *TargetDeviceObject = (PVOID)kmDeviceExtension[i];
                        *TargetclassCallback = (PVOID)kmDeviceExtension[i + 1];
                        KdPrint(("%SDeviceObject == 0x%x\n", kmName, kmDeviceExtension[i]));
                        KdPrint(("%SClassServiceCallback == 0x%x\n", kmName, kmDeviceExtension[i + 1]));
                        return STATUS_SUCCESS;
                    }
                }
                kmclassDeviceObject = kmclassDeviceObject->NextDevice;
            }
            kmTempDeviceObject = kmTempDeviceObject->AttachedDevice;
        }
        kmDeviceObject = kmDeviceObject->NextDevice;
    }
    return STATUS_UNSUCCESSFUL;
}
