#include "pch.h"
#include "pnp_device.h"
#include "cqkeys.h"
#include "../shared/cqkeys_protocol.h"
#include <setupapi.h>
#include <cfgmgr32.h>
#include <initguid.h>
#include <devpkey.h>
#include <winternl.h>
#include <newdev.h>
#include <sddl.h>
#include <string>
#include <vector>
#include <new>

namespace {
    const GUID kSystemClass = {0x4d36e97d, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};
    HANDLE g_sessionLease = nullptr;

    class DeviceInfoSet {
    public:
        explicit DeviceInfoSet(HDEVINFO handle) : value(handle) {}
        ~DeviceInfoSet() {
            DWORD error = GetLastError();
            if (value != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(value);
            SetLastError(error);
        }
        DeviceInfoSet(const DeviceInfoSet&) = delete;
        DeviceInfoSet& operator=(const DeviceInfoSet&) = delete;
        HDEVINFO value;
    };

    BOOL Error(DWORD error) { SetLastError(error); return FALSE; }

    BOOL ValidateDevice(HDEVINFO devices, SP_DEVINFO_DATA& device) {
        wchar_t hardware[256]{};
        DWORD type = 0, size = 0;
        if (!SetupDiGetDeviceRegistryPropertyW(devices, &device, SPDRP_HARDWAREID, &type,
                reinterpret_cast<BYTE*>(hardware), sizeof(hardware), &size)) return FALSE;
        const wchar_t expected[] = CQKEYS_HARDWARE_ID L"\0";
        if (type != REG_MULTI_SZ || size != sizeof(expected) ||
            _wcsnicmp(hardware, expected, _countof(expected)) != 0 ||
            hardware[_countof(expected) - 1] != L'\0') return Error(ERROR_INVALID_DATA);
        return TRUE;
    }

    BOOL CheckNoOpenSession() {
        HANDLE handle = CqOpenInterface();
        if (handle != INVALID_HANDLE_VALUE) return CloseHandle(handle);
        DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_NO_SUCH_DEVINST ||
            error == ERROR_NO_MORE_ITEMS || error == ERROR_NOT_READY) return TRUE;
        return FALSE;
    }

    BOOL CheckRestart(HDEVINFO devices, SP_DEVINFO_DATA& device) {
        SP_DEVINSTALL_PARAMS_W parameters{};
        parameters.cbSize = sizeof(parameters);
        if (!SetupDiGetDeviceInstallParamsW(devices, &device, &parameters)) return FALSE;
        if (parameters.Flags & (DI_NEEDREBOOT | DI_NEEDRESTART)) return Error(ERROR_SUCCESS_REBOOT_REQUIRED);
        return TRUE;
    }

    BOOL ChangeState(HDEVINFO devices, SP_DEVINFO_DATA& device, DWORD state) {
        SP_PROPCHANGE_PARAMS parameters{};
        parameters.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
        parameters.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
        parameters.StateChange = state;
        parameters.Scope = DICS_FLAG_GLOBAL;
        if (!SetupDiSetClassInstallParamsW(devices, &device, &parameters.ClassInstallHeader, sizeof(parameters)) ||
            !SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, devices, &device)) return FALSE;
        return CheckRestart(devices, device);
    }

    BOOL CheckStartProblem(DEVINST instance, CqDriverStartResult& diagnostics) {
        ULONG flags = 0, problem = 0;
        CONFIGRET result = CM_Get_DevNode_Status(&flags, &problem, instance, 0);
        if (result != CR_SUCCESS) {
            diagnostics.diagnosticError = CM_MapCrToWin32Err(result, ERROR_GEN_FAILURE);
            return Error(diagnostics.diagnosticError);
        }
        if (!(flags & DN_HAS_PROBLEM)) return TRUE;
        diagnostics.problemCode = problem;
        LONG status = 0;
        DEVPROPTYPE type = 0;
        ULONG bytes = sizeof(status);
        result = CM_Get_DevNode_PropertyW(instance, &DEVPKEY_Device_ProblemStatus, &type,
            reinterpret_cast<PBYTE>(&status), &bytes, 0);
        if (result == CR_SUCCESS) {
            if (type != DEVPROP_TYPE_NTSTATUS || bytes != sizeof(status)) {
                diagnostics.diagnosticError = ERROR_INVALID_DATA;
                return Error(ERROR_INVALID_DATA);
            }
            diagnostics.problemStatus = status;
        } else {
            diagnostics.diagnosticError = CM_MapCrToWin32Err(result, ERROR_GEN_FAILURE);
            if (result != CR_NO_SUCH_VALUE) return Error(diagnostics.diagnosticError);
        }
        if (problem == CM_PROB_NEED_RESTART) return Error(ERROR_SUCCESS_REBOOT_REQUIRED);
        if (status < 0) return Error(RtlNtStatusToDosError(status));
        if (problem == CM_PROB_UNSIGNED_DRIVER) return Error(ERROR_INVALID_IMAGE_HASH);
        if (problem == CM_PROB_DISABLED) return Error(ERROR_SERVICE_DISABLED);
        return Error(ERROR_GEN_FAILURE);
    }

    BOOL PackagePath(const char* path, std::wstring& result) {
        if (!path || !*path) return Error(ERROR_INVALID_PARAMETER);
        int length = MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, path, -1, nullptr, 0);
        if (!length) return FALSE;
        if (length > MAX_PATH) return Error(ERROR_FILENAME_EXCED_RANGE);
        std::vector<wchar_t> wide(length);
        if (!MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, path, -1, wide.data(), length)) return FALSE;
        wchar_t full[MAX_PATH];
        DWORD written = GetFullPathNameW(wide.data(), MAX_PATH, full, nullptr);
        if (!written) return FALSE;
        if (written >= MAX_PATH) return Error(ERROR_FILENAME_EXCED_RANGE);
        result = full;
        auto extension = result.find_last_of(L'.');
        if (extension == std::wstring::npos) return Error(ERROR_INVALID_PARAMETER);
        if (_wcsicmp(result.c_str() + extension, L".sys") == 0) result.replace(extension, 4, L".inf");
        else if (_wcsicmp(result.c_str() + extension, L".inf") != 0) return Error(ERROR_INVALID_PARAMETER);
        DWORD attributes = GetFileAttributesW(result.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) return FALSE;
        if (attributes & FILE_ATTRIBUTE_DIRECTORY) return Error(ERROR_INVALID_PARAMETER);
        GUID deviceClass;
        wchar_t className[MAX_CLASS_NAME_LEN];
        if (!SetupDiGetINFClassW(result.c_str(), &deviceClass, className, MAX_CLASS_NAME_LEN, nullptr)) return FALSE;
        if (!IsEqualGUID(deviceClass, kSystemClass)) return Error(ERROR_CLASS_MISMATCH);
        return TRUE;
    }
}

BOOL CqAcquireSession() {
    if (g_sessionLease) return TRUE;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)",
            SDDL_REVISION_1, &descriptor, nullptr)) return FALSE;
    SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
    HANDLE lease = CreateEventW(&security, TRUE, FALSE, L"Global\\CQKeysVhfSessionV2");
    DWORD error = GetLastError();
    LocalFree(descriptor);
    if (!lease) return Error(error);
    if (error == ERROR_ALREADY_EXISTS) { CloseHandle(lease); return Error(ERROR_BUSY); }
    g_sessionLease = lease;
    return TRUE;
}

BOOL CqReleaseSession() {
    if (g_sessionLease && !CloseHandle(g_sessionLease)) return FALSE;
    g_sessionLease = nullptr;
    return TRUE;
}

HANDLE CqOpenInterface() try {
    DeviceInfoSet devices(SetupDiGetClassDevsW(&GUID_DEVINTERFACE_CQKEYS, nullptr, nullptr,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE));
    if (devices.value == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
    SP_DEVINFO_DATA device{};
    device.cbSize = sizeof(device);
    if (!SetupDiOpenDeviceInfoW(devices.value, CQKEYS_INSTANCE_ID, nullptr, 0, &device)) return INVALID_HANDLE_VALUE;
    SP_DEVICE_INTERFACE_DATA interfaceData{};
    interfaceData.cbSize = sizeof(interfaceData);
    if (!SetupDiEnumDeviceInterfaces(devices.value, &device, &GUID_DEVINTERFACE_CQKEYS, 0, &interfaceData))
        return INVALID_HANDLE_VALUE;
    DWORD bytes = 0;
    SetupDiGetDeviceInterfaceDetailW(devices.value, &interfaceData, nullptr, 0, &bytes, nullptr);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) return INVALID_HANDLE_VALUE;
    std::vector<BYTE> buffer(bytes);
    auto detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buffer.data());
    detail->cbSize = sizeof(*detail);
    if (!SetupDiGetDeviceInterfaceDetailW(devices.value, &interfaceData, detail, bytes, nullptr, nullptr))
        return INVALID_HANDLE_VALUE;
    return CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
} catch (const std::bad_alloc&) {
    SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return INVALID_HANDLE_VALUE;
}

BOOL CqInstallDevice(const char* path, CqDriverStartResult* diagnostics) try {
    CqDriverStartResult local{sizeof(CqDriverStartResult), CQ_DRIVER_START_RESULT_ABI};
    if (!diagnostics) diagnostics = &local;
    *diagnostics = local;
    std::wstring infPath;
    if (!PackagePath(path, infPath)) return FALSE;
    if (!CheckNoOpenSession()) return FALSE;
    DeviceInfoSet devices(SetupDiCreateDeviceInfoList(&kSystemClass, nullptr));
    if (devices.value == INVALID_HANDLE_VALUE) return FALSE;
    SP_DEVINFO_DATA device{};
    device.cbSize = sizeof(device);
    if (!SetupDiOpenDeviceInfoW(devices.value, CQKEYS_INSTANCE_ID, nullptr, 0, &device)) {
        if (GetLastError() != ERROR_NO_SUCH_DEVINST) return FALSE;
        if (!SetupDiCreateDeviceInfoW(devices.value, CQKEYS_INSTANCE_ID, &kSystemClass,
                L"CQKeys Virtual HID Source", nullptr, 0, &device)) return FALSE;
        const wchar_t hardwareIds[] = CQKEYS_HARDWARE_ID L"\0";
        if (!SetupDiSetDeviceRegistryPropertyW(devices.value, &device, SPDRP_HARDWAREID,
                reinterpret_cast<const BYTE*>(hardwareIds), sizeof(hardwareIds)) ||
            !SetupDiCallClassInstaller(DIF_REGISTERDEVICE, devices.value, &device)) return FALSE;
    }
    if (!ValidateDevice(devices.value, device)) return FALSE;
    wchar_t stagedInf[MAX_PATH];
    if (!SetupCopyOEMInfW(infPath.c_str(), nullptr, SPOST_PATH, 0, stagedInf, MAX_PATH, nullptr, nullptr)) return FALSE;
    SP_DEVINSTALL_PARAMS_W parameters{};
    parameters.cbSize = sizeof(parameters);
    if (!SetupDiGetDeviceInstallParamsW(devices.value, &device, &parameters)) return FALSE;
    parameters.Flags |= DI_ENUMSINGLEINF | DI_QUIETINSTALL;
    wcscpy_s(parameters.DriverPath, stagedInf);
    if (!SetupDiSetDeviceInstallParamsW(devices.value, &device, &parameters) ||
        !SetupDiBuildDriverInfoList(devices.value, &device, SPDIT_COMPATDRIVER)) return FALSE;
    SP_DRVINFO_DATA_W selected{};
    selected.cbSize = sizeof(selected);
    if (!SetupDiEnumDriverInfoW(devices.value, &device, SPDIT_COMPATDRIVER, 0, &selected)) return FALSE;
    BOOL reboot = FALSE;
    if (!DiInstallDevice(nullptr, devices.value, &device, &selected, DIIDFLAG_NOFINISHINSTALLUI, &reboot)) return FALSE;
    if (reboot) return Error(ERROR_SUCCESS_REBOOT_REQUIRED);
    if (!ChangeState(devices.value, device, DICS_ENABLE)) return FALSE;
    const ULONGLONG start = GetTickCount64();
    for (;;) {
        HANDLE handle = CqOpenInterface();
        if (handle != INVALID_HANDLE_VALUE) {
            if (!CloseHandle(handle)) return FALSE;
            SetLastError(ERROR_SUCCESS);
            return TRUE;
        }
        DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_NO_SUCH_DEVINST &&
            error != ERROR_NO_MORE_ITEMS && error != ERROR_NOT_READY) return FALSE;
        if (!CheckStartProblem(device.DevInst, *diagnostics)) return FALSE;
        if (GetTickCount64() - start >= 5000) return Error(ERROR_TIMEOUT);
        Sleep(50);
    }
} catch (const std::bad_alloc&) {
    return Error(ERROR_NOT_ENOUGH_MEMORY);
}

BOOL CqChangeDevice(bool remove) {
    if (!CheckNoOpenSession()) return FALSE;
    DeviceInfoSet devices(SetupDiCreateDeviceInfoList(&kSystemClass, nullptr));
    if (devices.value == INVALID_HANDLE_VALUE) return FALSE;
    SP_DEVINFO_DATA device{};
    device.cbSize = sizeof(device);
    if (!SetupDiOpenDeviceInfoW(devices.value, CQKEYS_INSTANCE_ID, nullptr, 0, &device)) {
        if (GetLastError() != ERROR_NO_SUCH_DEVINST) return FALSE;
        SetLastError(ERROR_SUCCESS);
        return TRUE;
    }
    if (!ValidateDevice(devices.value, device)) return FALSE;
    if (!remove) {
        if (!ChangeState(devices.value, device, DICS_DISABLE)) return FALSE;
        ULONG status = 0, problem = 0;
        CONFIGRET result = CM_Get_DevNode_Status(&status, &problem, device.DevInst, 0);
        if (result != CR_SUCCESS) return Error(CM_MapCrToWin32Err(result, ERROR_GEN_FAILURE));
        if (!(status & DN_HAS_PROBLEM) || problem != CM_PROB_DISABLED) return Error(ERROR_BUSY);
    } else {
        BOOL reboot = FALSE;
        if (!DiUninstallDevice(nullptr, devices.value, &device, 0, &reboot)) return FALSE;
        if (reboot) return Error(ERROR_SUCCESS_REBOOT_REQUIRED);
        DEVINST instance;
        CONFIGRET result = CM_Locate_DevNodeW(&instance, const_cast<PWSTR>(CQKEYS_INSTANCE_ID), CM_LOCATE_DEVNODE_NORMAL);
        if (result == CR_SUCCESS) return Error(ERROR_BUSY);
        if (result != CR_NO_SUCH_DEVNODE) return Error(CM_MapCrToWin32Err(result, ERROR_GEN_FAILURE));
    }
    SetLastError(ERROR_SUCCESS);
    return TRUE;
}
