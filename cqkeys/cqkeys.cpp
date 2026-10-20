#include <ntddk.h>
#include <wdf.h>
#include <hidport.h>
#include <vhf.h>
#include "input_reports.h"
#include "report_queue.h"
#include "hid_descriptor.h"

struct DEVICE_CONTEXT {
    VHFHANDLE Vhf;
    PIO_WORKITEM WorkItem;
    KSPIN_LOCK Lock;
    EX_RUNDOWN_REF Rundown;
    KEVENT WorkerIdle;
    BOOLEAN Online;
    BOOLEAN Initialized;
    BOOLEAN Stopping;
    BOOLEAN Ready;
    BOOLEAN WorkerQueued;
    NTSTATUS LastSubmitStatus;
    cq::ReportQueue Pending;
    cq::Report CurrentReport;
    HID_XFER_PACKET CurrentPacket;
};
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, DeviceContext)

struct FILE_CONTEXT { BOOLEAN Closed; };
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(FILE_CONTEXT, FileContext)

extern "C" DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD CqDeviceAdd;
EVT_WDF_OBJECT_CONTEXT_CLEANUP CqDeviceCleanup;
EVT_WDF_DEVICE_FILE_CREATE CqFileCreate;
EVT_WDF_FILE_CLEANUP CqFileCleanup;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL CqDeviceControl;
EVT_WDF_DEVICE_D0_ENTRY CqD0Entry;
EVT_WDF_DEVICE_D0_EXIT CqD0Exit;
EVT_VHF_READY_FOR_NEXT_READ_REPORT CqReady;
_Function_class_(IO_WORKITEM_ROUTINE)
_IRQL_requires_(PASSIVE_LEVEL)
void CqDrain(_In_ PDEVICE_OBJECT device, _In_ PVOID parameter);

// Caller holds Lock. A queued worker owns rundown until it stops accessing VHF.
// Native state remains valid even after WDF's child objects have been cleaned up.
static void ScheduleLocked(DEVICE_CONTEXT* context)
{
    if (!context->Stopping && context->Online && context->Ready &&
        context->Pending.Count && !context->WorkerQueued &&
        NT_SUCCESS(context->LastSubmitStatus) &&
        ExAcquireRundownProtection(&context->Rundown)) {
        context->WorkerQueued = TRUE;
        KeClearEvent(&context->WorkerIdle);
        IoQueueWorkItem(context->WorkItem, CqDrain, DelayedWorkQueue, context);
    }
}

_Use_decl_annotations_
void CqDrain(PDEVICE_OBJECT, PVOID parameter)
{
    auto context = static_cast<DEVICE_CONTEXT*>(parameter);
    unsigned processed = 0;
    for (;;) {
        KIRQL irql;
        KeAcquireSpinLock(&context->Lock, &irql);
        if (context->Stopping || !context->Online || !context->Ready ||
            !context->Pending.Count || !NT_SUCCESS(context->LastSubmitStatus)) {
            context->WorkerQueued = FALSE;
            KeSetEvent(&context->WorkerIdle, IO_NO_INCREMENT, FALSE);
            KeReleaseSpinLock(&context->Lock, irql);
            break;
        }
        if (processed == CQ_REPORT_QUEUE_CAPACITY) {
            // A running I/O work item may be requeued. Hand its rundown
            // reference to the next invocation without advertising idle.
            IoQueueWorkItem(context->WorkItem, CqDrain, DelayedWorkQueue, context);
            KeReleaseSpinLock(&context->Lock, irql);
            return;
        }
        // Only a ready notification permits reuse of CurrentReport/Packet.
        context->CurrentReport = context->Pending.Pop();
        context->Ready = FALSE;
        context->CurrentPacket.reportBuffer = context->CurrentReport.Data;
        context->CurrentPacket.reportBufferLen = context->CurrentReport.Length;
        context->CurrentPacket.reportId = context->CurrentReport.Data[0];
        KeReleaseSpinLock(&context->Lock, irql);

        // VHF may invoke CqReady inline. Do not hold Lock across this call.
        NTSTATUS status = VhfReadReportSubmit(context->Vhf, &context->CurrentPacket);
        ++processed;
        if (!NT_SUCCESS(status)) {
            KeAcquireSpinLock(&context->Lock, &irql);
            context->LastSubmitStatus = status;
            context->Pending.Clear();
            KeReleaseSpinLock(&context->Lock, irql);
            KdPrint(("cqkeys: VhfReadReportSubmit failed: 0x%08X\n", status));
        }
    }
    ExReleaseRundownProtection(&context->Rundown);
}

void CqReady(PVOID parameter)
{
    auto context = static_cast<DEVICE_CONTEXT*>(parameter);
    KIRQL irql;
    KeAcquireSpinLock(&context->Lock, &irql);
    context->Ready = TRUE;
    ScheduleLocked(context);
    KeReleaseSpinLock(&context->Lock, irql);
}

static void ReleaseInputLocked(DEVICE_CONTEXT* context)
{
    context->Pending.Release();
    // Never touch the report already owned by VHF here.
    ScheduleLocked(context);
}

NTSTATUS CqD0Entry(WDFDEVICE device, WDF_POWER_DEVICE_STATE)
{
    auto context = DeviceContext(device);
    KIRQL irql;
    KeAcquireSpinLock(&context->Lock, &irql);
    context->Online = TRUE;
    ReleaseInputLocked(context);
    KeReleaseSpinLock(&context->Lock, irql);
    return STATUS_SUCCESS;
}

NTSTATUS CqD0Exit(WDFDEVICE device, WDF_POWER_DEVICE_STATE)
{
    auto context = DeviceContext(device);
    KIRQL irql;
    KeAcquireSpinLock(&context->Lock, &irql);
    context->Online = FALSE;
    context->Pending.Clear();
    KeReleaseSpinLock(&context->Lock, irql);
    KeWaitForSingleObject(&context->WorkerIdle, Executive, KernelMode, FALSE, nullptr);
    return STATUS_SUCCESS;
}

void CqFileCreate(WDFDEVICE, WDFREQUEST request, WDFFILEOBJECT file)
{
    const auto name = WdfFileObjectGetFileName(file);
    const NTSTATUS status = name && name->Length ? STATUS_OBJECT_NAME_INVALID : STATUS_SUCCESS;
    FileContext(file)->Closed = !NT_SUCCESS(status);
    WdfRequestComplete(request, status);
}

void CqFileCleanup(WDFFILEOBJECT file)
{
    auto context = DeviceContext(WdfFileObjectGetDevice(file));
    KIRQL irql;
    KeAcquireSpinLock(&context->Lock, &irql);
    // An old IOCTL cannot enqueue a key-down after this file's neutral reports.
    if (!FileContext(file)->Closed) {
        FileContext(file)->Closed = TRUE;
        if (!context->Stopping) ReleaseInputLocked(context);
    }
    KeReleaseSpinLock(&context->Lock, irql);
}

static NTSTATUS TranslationStatus(cq::Result result)
{
    switch (result) {
    case cq::Result::Ok: return STATUS_SUCCESS;
    case cq::Result::InvalidLength: return STATUS_INFO_LENGTH_MISMATCH;
    case cq::Result::Unsupported: return STATUS_NOT_SUPPORTED;
    case cq::Result::Full: return STATUS_BUFFER_OVERFLOW;
    default: return STATUS_INVALID_PARAMETER;
    }
}

void CqDeviceControl(WDFQUEUE queue, WDFREQUEST request, size_t outputLength,
                     size_t inputLength, ULONG code)
{
    auto context = DeviceContext(WdfIoQueueGetDevice(queue));
    WDFFILEOBJECT file = WdfRequestGetFileObject(request);
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR information = 0;
    PVOID buffer = nullptr;
    size_t required = 0;
    if (code == IOCTL_KEYBOARD) required = sizeof(CQ_KEYBOARD_INPUT);
    else if (code == IOCTL_MOUSE) required = sizeof(CQ_MOUSE_INPUT);
    else if (code != IOCTL_CQ_STATUS && code != IOCTL_CQ_RESET) {
        WdfRequestComplete(request, status);
        return;
    }
    if (inputLength != required ||
        (code != IOCTL_CQ_STATUS && outputLength != 0)) {
        WdfRequestComplete(request, STATUS_INFO_LENGTH_MISMATCH);
        return;
    }
    if (required) {
        status = WdfRequestRetrieveInputBuffer(request, required, &buffer, nullptr);
    } else if (code == IOCTL_CQ_STATUS) {
        status = WdfRequestRetrieveOutputBuffer(request, sizeof(CQ_DRIVER_STATUS), &buffer, nullptr);
    } else status = STATUS_SUCCESS;
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(request, status);
        return;
    }

    KIRQL irql;
    KeAcquireSpinLock(&context->Lock, &irql);
    if (!file || FileContext(file)->Closed || context->Stopping) {
        status = STATUS_FILE_CLOSED;
    } else if (code == IOCTL_CQ_STATUS) {
        CQ_DRIVER_STATUS snapshot = {};
        snapshot.Size = sizeof(snapshot);
        snapshot.Version = CQ_PROTOCOL_VERSION;
        snapshot.Flags = context->Online ? CQ_STATUS_ONLINE : 0;
        if (context->Online && NT_SUCCESS(context->LastSubmitStatus))
            snapshot.Flags |= CQ_STATUS_READY;
        snapshot.QueuedReports = context->Pending.Count;
        snapshot.QueueCapacity = CQ_REPORT_QUEUE_CAPACITY;
        snapshot.LastSubmitStatus = context->LastSubmitStatus;
        RtlCopyMemory(buffer, &snapshot, sizeof(snapshot));
        information = sizeof(snapshot);
        status = STATUS_SUCCESS;
    } else if (!context->Online) {
        status = STATUS_DEVICE_NOT_READY;
    } else if (!NT_SUCCESS(context->LastSubmitStatus)) {
        status = context->LastSubmitStatus;
    } else if (code == IOCTL_CQ_RESET) {
        ReleaseInputLocked(context);
        status = STATUS_SUCCESS;
    } else {
        cq::Batch batch;
        status = TranslationStatus(cq::PrepareInput(code, buffer, inputLength, context->Pending.State, batch));
        if (NT_SUCCESS(status)) {
            if (!context->Pending.Push(batch)) {
                status = STATUS_DEVICE_BUSY;
            } else {
                ScheduleLocked(context);
            }
        }
    }
    KeReleaseSpinLock(&context->Lock, irql);
    WdfRequestCompleteWithInformation(request, status, information);
}

void CqDeviceCleanup(WDFOBJECT object)
{
    auto context = DeviceContext(object);
    // WdfDeviceCreate can fail before the post-create initialization below.
    if (!context->Initialized) return;
    KIRQL irql;
    KeAcquireSpinLock(&context->Lock, &irql);
    context->Stopping = TRUE;
    context->Online = FALSE;
    KeReleaseSpinLock(&context->Lock, irql);
    ExWaitForRundownProtectionRelease(&context->Rundown);
    if (context->Vhf) {
        VhfDelete(context->Vhf, TRUE);
        context->Vhf = nullptr;
    }
    if (context->WorkItem) {
        IoFreeWorkItem(context->WorkItem);
        context->WorkItem = nullptr;
    }
}

NTSTATUS CqDeviceAdd(WDFDRIVER, PWDFDEVICE_INIT init)
{
    DECLARE_CONST_UNICODE_STRING(name, KEYMOUSE_DEVICE_NAME);
    DECLARE_CONST_UNICODE_STRING(link, KEYMOUSE_DOS_DEVICE_NAME);
    DECLARE_CONST_UNICODE_STRING(sddl, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)");
    NTSTATUS status = WdfDeviceInitAssignName(init, &name);
    if (!NT_SUCCESS(status)) return status;
    status = WdfDeviceInitAssignSDDLString(init, &sddl);
    if (!NT_SUCCESS(status)) return status;
    WdfDeviceInitSetDeviceType(init, CQ_DEVICE_TYPE);
    WdfDeviceInitSetCharacteristics(init, FILE_DEVICE_SECURE_OPEN, FALSE);
    WdfDeviceInitSetExclusive(init, TRUE);
    WdfDeviceInitSetIoType(init, WdfDeviceIoBuffered);

    WDF_FILEOBJECT_CONFIG files;
    WDF_FILEOBJECT_CONFIG_INIT(&files, CqFileCreate, WDF_NO_EVENT_CALLBACK, CqFileCleanup);
    WDF_OBJECT_ATTRIBUTES fileAttributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&fileAttributes, FILE_CONTEXT);
    WdfDeviceInitSetFileObjectConfig(init, &files, &fileAttributes);

    WDF_PNPPOWER_EVENT_CALLBACKS power;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&power);
    power.EvtDeviceD0Entry = CqD0Entry;
    power.EvtDeviceD0Exit = CqD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(init, &power);

    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, DEVICE_CONTEXT);
    attributes.ExecutionLevel = WdfExecutionLevelPassive;
    attributes.EvtCleanupCallback = CqDeviceCleanup;
    WDFDEVICE device;
    status = WdfDeviceCreate(&init, &attributes, &device);
    if (!NT_SUCCESS(status)) return status;
    auto context = DeviceContext(device);
    KeInitializeSpinLock(&context->Lock);
    ExInitializeRundownProtection(&context->Rundown);
    KeInitializeEvent(&context->WorkerIdle, NotificationEvent, TRUE);
    context->Initialized = TRUE;
    context->WorkItem = IoAllocateWorkItem(WdfDeviceWdmGetDeviceObject(device));
    if (!context->WorkItem) return STATUS_INSUFFICIENT_RESOURCES;

    status = WdfDeviceCreateSymbolicLink(device, &link);
    if (!NT_SUCCESS(status)) return status;
    WDF_IO_QUEUE_CONFIG queue;
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queue, WdfIoQueueDispatchSequential);
    queue.PowerManaged = WdfTrue;
    queue.EvtIoDeviceControl = CqDeviceControl;
    status = WdfIoQueueCreate(device, &queue, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) return status;

    VHF_CONFIG config;
    VHF_CONFIG_INIT(&config, WdfDeviceWdmGetDeviceObject(device),
                    sizeof(CqReportDescriptor), CqReportDescriptor);
    config.VhfClientContext = context;
    config.EvtVhfReadyForNextReadReport = CqReady;
    config.VersionNumber = CQ_PROTOCOL_VERSION;
    status = VhfCreate(&config, &context->Vhf);
    if (!NT_SUCCESS(status)) return status;
    return VhfStart(context->Vhf);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING registryPath)
{
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, CqDeviceAdd);
    return WdfDriverCreate(driver, registryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
}
