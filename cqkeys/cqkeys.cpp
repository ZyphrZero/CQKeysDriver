#include "stdafx.h"
#include "cqkeys.h"
#include "input_state.h"
#include "../shared/cqkeys_reports.h"

struct DeviceContext {
    VHFHANDLE vhf;
    WDFWAITLOCK stateLock;
    WDFWAITLOCK ioLock;
    WDFWAITLOCK cleanupLock;
    WDFWAITLOCK routeLock;
    WDFQUEUE inputQueue;
    WDFWORKITEM stopWork;
    WDFREQUEST stopRequest;
    WDFFILEOBJECT stopFile;
    WDFFILEOBJECT owner;
    CqInputState input;
    uint64_t epoch;
    uint64_t nextSession;
    uint32_t stopReason;
    volatile LONG admittedReports;
    BOOLEAN d0;
    BOOLEAN removing;
    BOOLEAN stopQueued;
};
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DeviceContext, GetContext);

struct FileContext {
    CqSessionState session;
    BOOLEAN began;
    BOOLEAN closing;
    BOOLEAN fullCleanup;
};
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(FileContext, GetFileContext);

EVT_WDF_DRIVER_DEVICE_ADD CqDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE CqPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE CqReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY CqD0Entry;
EVT_WDF_DEVICE_D0_EXIT CqD0Exit;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL CqDeviceControl;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL CqInputControl;
EVT_WDF_IO_QUEUE_IO_CANCELED_ON_QUEUE CqInputCanceled;
EVT_WDF_DEVICE_FILE_CREATE CqFileCreate;
EVT_WDF_FILE_CLEANUP CqFileCleanup;
EVT_WDF_DEVICE_QUERY_REMOVE CqQueryRemove;
EVT_WDF_WORKITEM CqStopWork;

static NTSTATUS SubmitReport(DeviceContext* context, void* report, uint32_t length) {
    HID_XFER_PACKET packet{};
    packet.reportBuffer = static_cast<PUCHAR>(report);
    packet.reportBufferLen = length;
    packet.reportId = packet.reportBuffer[0];
    return VhfReadReportSubmit(context->vhf, &packet);
}

static NTSTATUS Neutralize(DeviceContext* context, bool duringD0Exit = false) {
    WdfWaitLockAcquire(context->stateLock, nullptr);
    CqInputState state = context->input;
    const bool available = context->vhf && (context->d0 || duringD0Exit) && !context->removing;
    const NTSTATUS unavailable = !context->vhf || context->removing ?
        STATUS_DEVICE_NOT_CONNECTED : STATUS_DEVICE_NOT_READY;
    WdfWaitLockRelease(context->stateLock);
    NTSTATUS status = state.Neutralize([&](void* report, uint32_t length) -> int32_t {
        return available ? SubmitReport(context, report, length) : unavailable;
    });
    WdfWaitLockAcquire(context->stateLock, nullptr);
    context->input = state;
    WdfWaitLockRelease(context->stateLock);
    return status;
}

static CqDeviceState Snapshot(DeviceContext* context, FileContext* file) {
    CqDeviceState state{};
    state.header = CqHeader(sizeof(state), context->epoch, file->session.sessionId);
    state.phase = file->session.phase;
    state.ready = context->d0 && !context->removing;
    state.pendingCleanup = context->input.pendingCleanup;
    state.stopReason = context->stopReason;
    state.lastAcceptedSequence = context->input.lastAcceptedSequence;
    state.lastSubmittedSequence = context->input.lastSubmittedSequence;
    state.inputStatus = context->input.inputStatus;
    state.cleanupStatus = context->input.cleanupStatus;
    state.firstCleanupStatus = context->input.firstCleanupStatus;
    state.keyboard = context->input.keyboard;
    state.buttons = context->input.buttons;
    state.consumer = context->input.consumer;
    return state;
}

static void GateSession(DeviceContext* context, FileContext* file, uint32_t reason) {
    if (file->session.phase == CQKEYS_SESSION_ACTIVE ||
        file->session.phase == CQKEYS_SESSION_CONNECTED ||
        file->session.phase == CQKEYS_SESSION_STARTING) {
        file->fullCleanup = TRUE;
        context->stopReason = reason;
    }
    if (file->session.phase != CQKEYS_SESSION_STOPPED) file->session.Stop();
}

static NTSTATUS FinishStop(DeviceContext* context, FileContext* file) {
    WdfWaitLockAcquire(context->cleanupLock, nullptr);
    WdfWaitLockAcquire(context->routeLock, nullptr);
    WdfIoQueuePurgeSynchronously(context->inputQueue);
    WdfWaitLockRelease(context->routeLock);
    WdfWaitLockAcquire(context->ioLock, nullptr);
    WdfWaitLockAcquire(context->stateLock, nullptr);
    if (file->fullCleanup) {
        context->input.RequireNeutral();
        file->fullCleanup = FALSE;
    }
    WdfWaitLockRelease(context->stateLock);
    NTSTATUS status = Neutralize(context);
    WdfWaitLockAcquire(context->stateLock, nullptr);
    file->session.FinishStop(NT_SUCCESS(status));
    WdfWaitLockRelease(context->stateLock);
    WdfWaitLockRelease(context->ioLock);
    WdfWaitLockRelease(context->cleanupLock);
    return status;
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
    context->input = CqInputState{};
    context->input.keyboard.id = CQKEYS_KEYBOARD_ID;
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = device;
    status = WdfWaitLockCreate(&attributes, &context->stateLock);
    if (!NT_SUCCESS(status)) return status;
    status = WdfWaitLockCreate(&attributes, &context->ioLock);
    if (!NT_SUCCESS(status)) return status;
    status = WdfWaitLockCreate(&attributes, &context->cleanupLock);
    if (!NT_SUCCESS(status)) return status;
    status = WdfWaitLockCreate(&attributes, &context->routeLock);
    if (!NT_SUCCESS(status)) return status;
    status = WdfDeviceCreateDeviceInterface(device, &GUID_DEVINTERFACE_CQKEYS, nullptr);
    if (!NT_SUCCESS(status)) return status;

    WDF_WORKITEM_CONFIG work;
    WDF_WORKITEM_CONFIG_INIT(&work, CqStopWork);
    work.AutomaticSerialization = FALSE;
    status = WdfWorkItemCreate(&work, &attributes, &context->stopWork);
    if (!NT_SUCCESS(status)) return status;

    WDF_IO_QUEUE_CONFIG queue;
    WDF_IO_QUEUE_CONFIG_INIT(&queue, WdfIoQueueDispatchSequential);
    queue.PowerManaged = WdfTrue;
    queue.EvtIoDeviceControl = CqInputControl;
    queue.EvtIoCanceledOnQueue = CqInputCanceled;
    status = WdfIoQueueCreate(device, &queue, WDF_NO_OBJECT_ATTRIBUTES, &context->inputQueue);
    if (!NT_SUCCESS(status)) return status;
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queue, WdfIoQueueDispatchParallel);
    queue.PowerManaged = WdfFalse;
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
    WdfWaitLockAcquire(context->ioLock, nullptr);
    VHFHANDLE vhf = nullptr;
    NTSTATUS status = VhfCreate(&config, &vhf);
    if (NT_SUCCESS(status)) {
        status = VhfStart(vhf);
        if (!NT_SUCCESS(status)) {
            WdfWaitLockRelease(context->ioLock);
            VhfDelete(vhf, TRUE);
            return status;
        }
        context->vhf = vhf;
    }
    WdfWaitLockAcquire(context->stateLock, nullptr);
    context->removing = FALSE;
    WdfWaitLockRelease(context->stateLock);
    WdfWaitLockRelease(context->ioLock);
    return status;
}

NTSTATUS CqReleaseHardware(WDFDEVICE device, WDFCMRESLIST translated) {
    UNREFERENCED_PARAMETER(translated);
    auto context = GetContext(device);
    WdfWaitLockAcquire(context->stateLock, nullptr);
    context->d0 = FALSE;
    context->removing = TRUE;
    context->stopReason = CQKEYS_STOP_REMOVAL;
    if (context->owner) GetFileContext(context->owner)->session.Invalidate();
    WdfWaitLockRelease(context->stateLock);
    WdfWorkItemFlush(context->stopWork);
    WdfWaitLockAcquire(context->cleanupLock, nullptr);
    WdfWaitLockAcquire(context->routeLock, nullptr);
    WdfIoQueuePurgeSynchronously(context->inputQueue);
    WdfWaitLockRelease(context->routeLock);
    WdfWaitLockAcquire(context->ioLock, nullptr);
    Neutralize(context);
    WdfWaitLockAcquire(context->stateLock, nullptr);
    VHFHANDLE vhf = context->vhf;
    context->vhf = nullptr;
    WdfWaitLockRelease(context->stateLock);
    WdfWaitLockRelease(context->ioLock);
    WdfWaitLockRelease(context->cleanupLock);
    if (vhf) VhfDelete(vhf, TRUE);
    return STATUS_SUCCESS;
}

NTSTATUS CqD0Entry(WDFDEVICE device, WDF_POWER_DEVICE_STATE previous) {
    UNREFERENCED_PARAMETER(previous);
    auto context = GetContext(device);
    WdfWaitLockAcquire(context->cleanupLock, nullptr);
    WdfWaitLockAcquire(context->ioLock, nullptr);
    WdfWaitLockAcquire(context->stateLock, nullptr);
    if (context->epoch == ~uint64_t{0} || !context->vhf) {
        WdfWaitLockRelease(context->stateLock);
        WdfWaitLockRelease(context->ioLock);
        WdfWaitLockRelease(context->cleanupLock);
        return STATUS_DEVICE_NOT_READY;
    }
    ++context->epoch;
    context->d0 = TRUE;
    context->input.RequireNeutral();
    if (context->owner) GetFileContext(context->owner)->session.Invalidate();
    WdfWaitLockRelease(context->stateLock);
    Neutralize(context);
    WdfWaitLockRelease(context->ioLock);
    WdfWaitLockRelease(context->cleanupLock);
    return STATUS_SUCCESS;
}

NTSTATUS CqD0Exit(WDFDEVICE device, WDF_POWER_DEVICE_STATE target) {
    UNREFERENCED_PARAMETER(target);
    auto context = GetContext(device);
    WdfWaitLockAcquire(context->stateLock, nullptr);
    context->d0 = FALSE;
    context->stopReason = CQKEYS_STOP_POWER;
    context->input.RequireNeutral();
    if (context->owner) GetFileContext(context->owner)->session.Invalidate();
    WdfWaitLockRelease(context->stateLock);
    WdfWaitLockAcquire(context->cleanupLock, nullptr);
    WdfWaitLockAcquire(context->ioLock, nullptr);
    WdfWaitLockAcquire(context->stateLock, nullptr);
    context->input.RequireNeutral();
    WdfWaitLockRelease(context->stateLock);
    NTSTATUS status = Neutralize(context, true);
    WdfWaitLockRelease(context->ioLock);
    WdfWaitLockRelease(context->cleanupLock);
    return status;
}

void CqFileCreate(WDFDEVICE device, WDFREQUEST request, WDFFILEOBJECT file) {
    auto context = GetContext(device);
    WdfWaitLockAcquire(context->stateLock, nullptr);
    NTSTATUS status = context->owner ? STATUS_SHARING_VIOLATION : STATUS_SUCCESS;
    if (NT_SUCCESS(status)) {
        context->owner = file;
        GetFileContext(file)->session.connectedEpoch = context->epoch;
    }
    WdfWaitLockRelease(context->stateLock);
    WdfRequestComplete(request, status);
}

void CqFileCleanup(WDFFILEOBJECT file) {
    auto context = GetContext(WdfFileObjectGetDevice(file));
    auto owner = GetFileContext(file);
    WdfWaitLockAcquire(context->stateLock, nullptr);
    if (context->owner != file) {
        WdfWaitLockRelease(context->stateLock);
        return;
    }
    owner->closing = TRUE;
    const bool began = owner->began;
    if (began) GateSession(context, owner, CQKEYS_STOP_FILE);
    WdfWaitLockRelease(context->stateLock);
    if (began) FinishStop(context, owner);
    WdfWaitLockAcquire(context->stateLock, nullptr);
    owner->session.phase = CQKEYS_SESSION_CLOSED;
    context->owner = nullptr;
    WdfWaitLockRelease(context->stateLock);
}

NTSTATUS CqQueryRemove(WDFDEVICE device) {
    auto context = GetContext(device);
    WdfWaitLockAcquire(context->stateLock, nullptr);
    NTSTATUS status = context->owner ? STATUS_DEVICE_BUSY : STATUS_SUCCESS;
    WdfWaitLockRelease(context->stateLock);
    return status;
}

void CqInputCanceled(WDFQUEUE queue, WDFREQUEST request) {
    auto context = GetContext(WdfIoQueueGetDevice(queue));
    InterlockedDecrement(&context->admittedReports);
    WdfRequestComplete(request, STATUS_CANCELLED);
}

void CqInputControl(WDFQUEUE queue, WDFREQUEST request, size_t outputLength,
                    size_t inputLength, ULONG code) {
    UNREFERENCED_PARAMETER(outputLength);
    UNREFERENCED_PARAMETER(inputLength);
    UNREFERENCED_PARAMETER(code);
    auto context = GetContext(WdfIoQueueGetDevice(queue));
    CqReportRequest* input;
    NTSTATUS status = WdfRequestRetrieveInputBuffer(request, sizeof(*input), reinterpret_cast<void**>(&input), nullptr);
    CqReportRequest report{};
    if (NT_SUCCESS(status)) report = *input;
    WdfWaitLockAcquire(context->ioLock, nullptr);
    WdfWaitLockAcquire(context->stateLock, nullptr);
    WDFFILEOBJECT file = WdfRequestGetFileObject(request);
    if (NT_SUCCESS(status)) {
        if (!file || context->owner != file || GetFileContext(file)->closing) status = STATUS_FILE_CLOSED;
        else if (!GetFileContext(file)->session.Matches(report.header) ||
                 !GetFileContext(file)->session.CanSubmit(context->epoch)) status = STATUS_DEVICE_NOT_CONNECTED;
        else if (!context->d0 || context->removing || !context->vhf) status = STATUS_DEVICE_NOT_READY;
        else if (WdfRequestIsCanceled(request)) status = STATUS_CANCELLED;
    }
    WdfWaitLockRelease(context->stateLock);
    if (NT_SUCCESS(status)) {
        status = SubmitReport(context, report.report, report.reportLength);
        WdfWaitLockAcquire(context->stateLock, nullptr);
        context->input.inputStatus = status;
        if (NT_SUCCESS(status)) context->input.Commit(report);
        else {
            context->input.RequireNeutral();
            context->stopReason = CQKEYS_STOP_INPUT_ERROR;
            if (file) GetFileContext(file)->session.phase = CQKEYS_SESSION_CLEANUP_FAILED;
        }
        WdfWaitLockRelease(context->stateLock);
    }
    ULONG_PTR information = 0;
    if (NT_SUCCESS(status)) {
        CqSubmitResult* output;
        status = WdfRequestRetrieveOutputBuffer(request, sizeof(*output), reinterpret_cast<void**>(&output), nullptr);
        if (NT_SUCCESS(status)) {
            output->header = CqHeader(sizeof(*output), report.header.deviceEpoch, report.header.sessionId);
            output->sequence = report.sequence;
            information = sizeof(*output);
        }
    }
    InterlockedDecrement(&context->admittedReports);
    WdfWaitLockRelease(context->ioLock);
    WdfRequestCompleteWithInformation(request, status, information);
}

static NTSTATUS BeginSession(DeviceContext* context, FileContext* file,
                              const CqRequestHeader& header, CqRequestHeader* output) {
    WdfWaitLockAcquire(context->cleanupLock, nullptr);
    WdfWaitLockAcquire(context->ioLock, nullptr);
    WdfWaitLockAcquire(context->stateLock, nullptr);
    NTSTATUS status = STATUS_SUCCESS;
    if (!context->d0 || context->removing || !context->vhf) status = STATUS_DEVICE_NOT_READY;
    else if (file->closing || context->stopQueued || !file->session.CanBegin(context->epoch) ||
             header.deviceEpoch != context->epoch || header.sessionId != 0) status = STATUS_DEVICE_NOT_CONNECTED;
    else if (context->nextSession == ~uint64_t{0}) status = STATUS_INTEGER_OVERFLOW;
    if (NT_SUCCESS(status)) {
        if (file->session.phase == CQKEYS_SESSION_CONNECTED) context->input.RequireNeutral();
        file->began = TRUE;
        file->session.phase = CQKEYS_SESSION_STARTING;
    }
    WdfWaitLockRelease(context->stateLock);
    if (NT_SUCCESS(status)) status = Neutralize(context);
    WdfWaitLockRelease(context->ioLock);
    if (NT_SUCCESS(status)) WdfIoQueueStart(context->inputQueue);
    WdfWaitLockAcquire(context->stateLock, nullptr);
    if (NT_SUCCESS(status) && (!context->d0 || context->removing || file->closing ||
        file->session.phase != CQKEYS_SESSION_STARTING)) status = STATUS_DEVICE_NOT_CONNECTED;
    if (NT_SUCCESS(status)) {
        file->session.Activate(context->epoch, ++context->nextSession);
        context->input.lastAcceptedSequence = 0;
        context->input.lastSubmittedSequence = 0;
        context->input.inputStatus = STATUS_SUCCESS;
        context->stopReason = 0;
        *output = CqHeader(sizeof(*output), context->epoch, file->session.sessionId);
    } else if (file->session.phase == CQKEYS_SESSION_STARTING) {
        file->session.phase = CQKEYS_SESSION_CLEANUP_FAILED;
    }
    WdfWaitLockRelease(context->stateLock);
    WdfWaitLockRelease(context->cleanupLock);
    return status;
}

void CqStopWork(WDFWORKITEM work) {
    auto context = GetContext(reinterpret_cast<WDFDEVICE>(WdfWorkItemGetParentObject(work)));
    WDFREQUEST request = context->stopRequest;
    WDFFILEOBJECT file = context->stopFile;
    NTSTATUS status = FinishStop(context, GetFileContext(file));
    ULONG_PTR information = 0;
    if (NT_SUCCESS(status)) {
        CqDeviceState* output;
        status = WdfRequestRetrieveOutputBuffer(request, sizeof(*output), reinterpret_cast<void**>(&output), nullptr);
        if (NT_SUCCESS(status)) {
            WdfWaitLockAcquire(context->stateLock, nullptr);
            *output = Snapshot(context, GetFileContext(file));
            WdfWaitLockRelease(context->stateLock);
            information = sizeof(*output);
        }
    }
    if (WdfRequestIsCanceled(request)) { status = STATUS_CANCELLED; information = 0; }
    WdfWaitLockAcquire(context->stateLock, nullptr);
    context->stopRequest = nullptr;
    context->stopFile = nullptr;
    context->stopQueued = FALSE;
    WdfWaitLockRelease(context->stateLock);
    WdfRequestCompleteWithInformation(request, status, information);
    WdfObjectDereference(file);
}

void CqDeviceControl(WDFQUEUE queue, WDFREQUEST request, size_t outputLength,
                     size_t inputLength, ULONG code) {
    auto context = GetContext(WdfIoQueueGetDevice(queue));
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR information = 0;
    CqRequestHeader* buffer = nullptr;
    size_t expectedInput = code == IOCTL_CQKEYS_SUBMIT_REPORT ? sizeof(CqReportRequest) : sizeof(CqRequestHeader);
    size_t expectedOutput = 0;
    switch (code) {
    case IOCTL_CQKEYS_QUERY_INFO: expectedOutput = sizeof(CqDeviceInfo); break;
    case IOCTL_CQKEYS_BEGIN_SESSION: expectedOutput = sizeof(CqRequestHeader); break;
    case IOCTL_CQKEYS_SUBMIT_REPORT: expectedOutput = sizeof(CqSubmitResult); break;
    case IOCTL_CQKEYS_STOP_SESSION: case IOCTL_CQKEYS_QUERY_STATE: expectedOutput = sizeof(CqDeviceState); break;
    default: WdfRequestComplete(request, status); return;
    }
    if (inputLength != expectedInput || outputLength != expectedOutput) {
        WdfRequestComplete(request, STATUS_INVALID_BUFFER_SIZE);
        return;
    }
    status = WdfRequestRetrieveInputBuffer(request, expectedInput, reinterpret_cast<void**>(&buffer), nullptr);
    if (!NT_SUCCESS(status)) { WdfRequestComplete(request, status); return; }
    CqRequestHeader header = *buffer;
    if (!CqValidHeader(header, static_cast<uint32_t>(expectedInput))) {
        WdfRequestComplete(request, STATUS_REVISION_MISMATCH);
        return;
    }
    void* output = nullptr;
    status = WdfRequestRetrieveOutputBuffer(request, expectedOutput, &output, nullptr);
    if (!NT_SUCCESS(status)) { WdfRequestComplete(request, status); return; }
    WDFFILEOBJECT fileObject = WdfRequestGetFileObject(request);
    if (code == IOCTL_CQKEYS_SUBMIT_REPORT) WdfWaitLockAcquire(context->routeLock, nullptr);
    WdfWaitLockAcquire(context->stateLock, nullptr);
    auto file = fileObject ? GetFileContext(fileObject) : nullptr;
    if (!file || context->owner != fileObject || file->closing) status = STATUS_FILE_CLOSED;
    else if (context->removing && code != IOCTL_CQKEYS_QUERY_INFO && code != IOCTL_CQKEYS_QUERY_STATE)
        status = STATUS_DEVICE_NOT_CONNECTED;
    else if (WdfRequestIsCanceled(request)) status = STATUS_CANCELLED;
    else if (code == IOCTL_CQKEYS_QUERY_INFO || code == IOCTL_CQKEYS_QUERY_STATE) {
        if (header.deviceEpoch != 0 || header.sessionId != 0) status = STATUS_INVALID_PARAMETER;
        else if (code == IOCTL_CQKEYS_QUERY_INFO) {
            *static_cast<CqDeviceInfo*>(output) = {CqHeader(sizeof(CqDeviceInfo), context->epoch),
                CQKEYS_DRIVER_MAJOR, CQKEYS_DRIVER_MINOR, CQKEYS_DRIVER_PATCH, CQKEYS_DRIVER_BUILD,
                CQKEYS_CAPABILITIES, CQKEYS_MAX_REPORT_LENGTH, context->d0 && !context->removing, 0};
            information = sizeof(CqDeviceInfo);
        } else {
            *static_cast<CqDeviceState*>(output) = Snapshot(context, file);
            information = sizeof(CqDeviceState);
        }
    } else if (code == IOCTL_CQKEYS_BEGIN_SESSION) {
        WdfWaitLockRelease(context->stateLock);
        status = BeginSession(context, file, header, static_cast<CqRequestHeader*>(output));
        WdfRequestCompleteWithInformation(request, status, NT_SUCCESS(status) ? sizeof(CqRequestHeader) : 0);
        return;
    } else if (code == IOCTL_CQKEYS_SUBMIT_REPORT) {
        auto input = reinterpret_cast<CqReportRequest*>(buffer);
        if (!CqValidReport(*input)) status = STATUS_INVALID_PARAMETER;
        else if (!file->session.Matches(header) || !file->session.CanSubmit(context->epoch)) status = STATUS_DEVICE_NOT_CONNECTED;
        else if (!context->d0 || context->removing) status = STATUS_DEVICE_NOT_READY;
        else if (input->sequence <= context->input.lastAcceptedSequence) status = STATUS_INVALID_PARAMETER;
        else if (InterlockedCompareExchange(&context->admittedReports, 1, 0) != 0) status = STATUS_DEVICE_BUSY;
        else {
            context->input.lastAcceptedSequence = input->sequence;
            WdfWaitLockRelease(context->stateLock);
            status = WdfRequestForwardToIoQueue(request, context->inputQueue);
            if (!NT_SUCCESS(status)) {
                InterlockedDecrement(&context->admittedReports);
                WdfRequestComplete(request, status);
            }
            WdfWaitLockRelease(context->routeLock);
            return;
        }
    } else if (code == IOCTL_CQKEYS_STOP_SESSION) {
        bool initial = file->session.sessionId == 0 && header.sessionId == 0 &&
            (header.deviceEpoch == file->session.connectedEpoch || header.deviceEpoch == 0);
        if (!file->session.Matches(header) && !initial) status = STATUS_DEVICE_NOT_CONNECTED;
        else if (context->stopQueued) status = STATUS_DEVICE_BUSY;
        else {
            GateSession(context, file, CQKEYS_STOP_EXPLICIT);
            file->began = TRUE;
            context->stopQueued = TRUE;
            context->stopRequest = request;
            context->stopFile = fileObject;
            WdfObjectReference(fileObject);
            WdfWorkItemEnqueue(context->stopWork);
            WdfWaitLockRelease(context->stateLock);
            return;
        }
    }
    WdfWaitLockRelease(context->stateLock);
    if (code == IOCTL_CQKEYS_SUBMIT_REPORT) WdfWaitLockRelease(context->routeLock);
    WdfRequestCompleteWithInformation(request, status, information);
}
