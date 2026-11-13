#include "stdafx.h"
#include "cqkeys.h"
#include "../shared/cqkeys_reports.h"

struct DeviceContext {
    VHFHANDLE vhf;
    WDFWAITLOCK lock;
    BOOLEAN ready;
    BOOLEAN opened;
    ULONG generation;
    NTSTATUS lastStatus;
};
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DeviceContext, GetContext);

struct FileContext { BOOLEAN ownsSession; };
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(FileContext, GetFileContext);

EVT_WDF_DRIVER_DEVICE_ADD CqDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE CqPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE CqReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY CqD0Entry;
EVT_WDF_DEVICE_D0_EXIT CqD0Exit;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL CqDeviceControl;
EVT_WDF_DEVICE_FILE_CREATE CqFileCreate;
EVT_WDF_FILE_CLEANUP CqFileCleanup;
EVT_WDF_DEVICE_QUERY_REMOVE CqQueryRemove;

static NTSTATUS SubmitReport(DeviceContext* context, void* report, ULONG length) {
    HID_XFER_PACKET packet{};
    packet.reportBuffer = static_cast<PUCHAR>(report);
    packet.reportBufferLen = length;
    packet.reportId = packet.reportBuffer[0];
    return VhfReadReportSubmit(context->vhf, &packet);
}

static NTSTATUS ResetReports(DeviceContext* context) {
    CqKeyboardReport keyboard{CQKEYS_KEYBOARD_ID};
    CqMouseReport mouse{CQKEYS_MOUSE_ID};
    CqConsumerReport consumer{CQKEYS_CONSUMER_ID};
    NTSTATUS first = SubmitReport(context, &keyboard, sizeof(keyboard));
    NTSTATUS status = SubmitReport(context, &mouse, sizeof(mouse));
    if (NT_SUCCESS(first)) first = status;
    status = SubmitReport(context, &consumer, sizeof(consumer));
    if (NT_SUCCESS(first)) first = status;
    context->lastStatus = first;
    return first;
}

extern "C" DRIVER_INITIALIZE DriverEntry;
extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT driverObject, PUNICODE_STRING registryPath) {
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, CqDeviceAdd);
    return WdfDriverCreate(driverObject, registryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
}

NTSTATUS CqDeviceAdd(WDFDRIVER driver, PWDFDEVICE_INIT deviceInit) {
    UNREFERENCED_PARAMETER(driver);
    WdfDeviceInitSetDeviceType(deviceInit, CQKEYS_DEVICE_TYPE);
    WdfDeviceInitSetCharacteristics(deviceInit, FILE_DEVICE_SECURE_OPEN, FALSE);
    WdfDeviceInitSetExclusive(deviceInit, TRUE);
    WdfDeviceInitSetIoType(deviceInit, WdfDeviceIoBuffered);

    WDF_PNPPOWER_EVENT_CALLBACKS power;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&power);
    power.EvtDevicePrepareHardware = CqPrepareHardware;
    power.EvtDeviceReleaseHardware = CqReleaseHardware;
    power.EvtDeviceD0Entry = CqD0Entry;
    power.EvtDeviceD0Exit = CqD0Exit;
    power.EvtDeviceQueryRemove = CqQueryRemove;
    WdfDeviceInitSetPnpPowerEventCallbacks(deviceInit, &power);

    WDF_FILEOBJECT_CONFIG files;
    WDF_FILEOBJECT_CONFIG_INIT(&files, CqFileCreate, WDF_NO_EVENT_CALLBACK, CqFileCleanup);
    WDF_OBJECT_ATTRIBUTES fileAttributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&fileAttributes, FileContext);
    WdfDeviceInitSetFileObjectConfig(deviceInit, &files, &fileAttributes);

    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, DeviceContext);
    attributes.ExecutionLevel = WdfExecutionLevelPassive;
    attributes.SynchronizationScope = WdfSynchronizationScopeNone;
    WDFDEVICE device;
    NTSTATUS status = WdfDeviceCreate(&deviceInit, &attributes, &device);
    if (!NT_SUCCESS(status)) return status;
    auto context = GetContext(device);
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = device;
    status = WdfWaitLockCreate(&attributes, &context->lock);
    if (!NT_SUCCESS(status)) return status;
    status = WdfDeviceCreateDeviceInterface(device, &GUID_DEVINTERFACE_CQKEYS, nullptr);
    if (!NT_SUCCESS(status)) return status;

    WDF_IO_QUEUE_CONFIG queue;
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queue, WdfIoQueueDispatchSequential);
    queue.PowerManaged = WdfTrue;
    queue.EvtIoDeviceControl = CqDeviceControl;
    return WdfIoQueueCreate(device, &queue, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
}

NTSTATUS CqPrepareHardware(WDFDEVICE device, WDFCMRESLIST raw, WDFCMRESLIST translated) {
    UNREFERENCED_PARAMETER(raw);
    UNREFERENCED_PARAMETER(translated);
    auto context = GetContext(device);
    VHF_CONFIG config;
    VHF_CONFIG_INIT(&config, WdfDeviceWdmGetDeviceObject(device),
        sizeof(CqReportDescriptor), const_cast<PUCHAR>(CqReportDescriptor));
    NTSTATUS status = VhfCreate(&config, &context->vhf);
    if (!NT_SUCCESS(status)) return status;
    status = VhfStart(context->vhf);
    if (!NT_SUCCESS(status)) {
        VhfDelete(context->vhf, TRUE);
        context->vhf = nullptr;
    }
    return status;
}

NTSTATUS CqReleaseHardware(WDFDEVICE device, WDFCMRESLIST translated) {
    UNREFERENCED_PARAMETER(translated);
    auto context = GetContext(device);
    WdfWaitLockAcquire(context->lock, nullptr);
    context->ready = FALSE;
    if (context->vhf) {
        VhfDelete(context->vhf, TRUE);
        context->vhf = nullptr;
    }
    WdfWaitLockRelease(context->lock);
    return STATUS_SUCCESS;
}

NTSTATUS CqD0Entry(WDFDEVICE device, WDF_POWER_DEVICE_STATE previous) {
    UNREFERENCED_PARAMETER(previous);
    auto context = GetContext(device);
    WdfWaitLockAcquire(context->lock, nullptr);
    ++context->generation;
    NTSTATUS status = ResetReports(context);
    context->ready = NT_SUCCESS(status);
    WdfWaitLockRelease(context->lock);
    return status;
}

NTSTATUS CqD0Exit(WDFDEVICE device, WDF_POWER_DEVICE_STATE target) {
    UNREFERENCED_PARAMETER(target);
    auto context = GetContext(device);
    WdfWaitLockAcquire(context->lock, nullptr);
    NTSTATUS status = ResetReports(context);
    context->ready = FALSE;
    WdfWaitLockRelease(context->lock);
    return status;
}

void CqFileCreate(WDFDEVICE device, WDFREQUEST request, WDFFILEOBJECT file) {
    auto context = GetContext(device);
    WdfWaitLockAcquire(context->lock, nullptr);
    NTSTATUS status = context->opened ? STATUS_SHARING_VIOLATION :
        (context->ready ? ResetReports(context) : STATUS_DEVICE_NOT_READY);
    if (NT_SUCCESS(status)) {
        context->opened = TRUE;
        GetFileContext(file)->ownsSession = TRUE;
    }
    WdfWaitLockRelease(context->lock);
    WdfRequestComplete(request, status);
}

void CqFileCleanup(WDFFILEOBJECT file) {
    auto context = GetContext(WdfFileObjectGetDevice(file));
    WdfWaitLockAcquire(context->lock, nullptr);
    if (GetFileContext(file)->ownsSession) {
        if (context->ready) ResetReports(context);
        context->opened = FALSE;
        GetFileContext(file)->ownsSession = FALSE;
    }
    WdfWaitLockRelease(context->lock);
}

NTSTATUS CqQueryRemove(WDFDEVICE device) {
    auto context = GetContext(device);
    WdfWaitLockAcquire(context->lock, nullptr);
    NTSTATUS status = context->opened ? STATUS_DEVICE_BUSY : STATUS_SUCCESS;
    WdfWaitLockRelease(context->lock);
    return status;
}

void CqDeviceControl(WDFQUEUE queue, WDFREQUEST request, size_t outputLength,
                     size_t inputLength, ULONG code) {
    auto context = GetContext(WdfIoQueueGetDevice(queue));
    WdfWaitLockAcquire(context->lock, nullptr);
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR information = 0;
    WDFFILEOBJECT file = WdfRequestGetFileObject(request);
    if (!file || !GetFileContext(file)->ownsSession) {
        status = STATUS_FILE_CLOSED;
    } else if (code == IOCTL_CQKEYS_STATUS && inputLength == 0 && outputLength >= sizeof(CqDeviceStatus)) {
        CqDeviceStatus* output;
        status = WdfRequestRetrieveOutputBuffer(request, sizeof(*output), reinterpret_cast<void**>(&output), nullptr);
        if (NT_SUCCESS(status)) {
            *output = {sizeof(*output), CQKEYS_PROTOCOL_VERSION, context->generation, context->ready, context->lastStatus};
            information = sizeof(*output);
        }
    } else if (code == IOCTL_CQKEYS_RESET && inputLength == 0 && outputLength == 0) {
        status = context->ready ? ResetReports(context) : STATUS_DEVICE_NOT_READY;
    } else if (code == IOCTL_CQKEYS_REPORT) {
        if (inputLength != sizeof(CqReportRequest) || outputLength != 0) {
            status = STATUS_INVALID_BUFFER_SIZE;
        } else {
            CqReportRequest* input;
            status = WdfRequestRetrieveInputBuffer(request, sizeof(*input), reinterpret_cast<void**>(&input), nullptr);
            if (NT_SUCCESS(status)) {
                if (!CqValidReport(*input)) status = STATUS_INVALID_PARAMETER;
                else if (!context->ready) status = STATUS_DEVICE_NOT_READY;
                else if (input->generation != context->generation) status = STATUS_DEVICE_NOT_CONNECTED;
                else status = SubmitReport(context, input->report, input->reportLength);
            }
        }
        context->lastStatus = status;
    }
    WdfWaitLockRelease(context->lock);
    WdfRequestCompleteWithInformation(request, status, information);
}
