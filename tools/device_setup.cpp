#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setupapi.h>
#include <newdev.h>
#include <initguid.h>
#include <devguid.h>
#include <stdio.h>
#include <wchar.h>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "newdev.lib")

static const wchar_t InstanceId[] = L"ROOT\\CQKEYSVHF\\0000";
static const wchar_t HardwareIds[] = L"ROOT\\CQKEYSVHF\0";

struct DeviceSet {
    HDEVINFO Value;
    DeviceSet() : Value(SetupDiCreateDeviceInfoList(&GUID_DEVCLASS_SYSTEM, nullptr)) {}
    ~DeviceSet() { if (Value != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(Value); }
    DeviceSet(const DeviceSet&) = delete;
    DeviceSet& operator=(const DeviceSet&) = delete;
};

static DWORD Remove(HDEVINFO set, SP_DEVINFO_DATA& device)
{
    SP_REMOVEDEVICE_PARAMS remove = {};
    remove.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
    remove.ClassInstallHeader.InstallFunction = DIF_REMOVE;
    remove.Scope = DI_REMOVEDEVICE_GLOBAL;
    if (!SetupDiSetClassInstallParamsW(set, &device, &remove.ClassInstallHeader, sizeof(remove)) ||
        !SetupDiCallClassInstaller(DIF_REMOVE, set, &device))
        return GetLastError();
    SP_DEVINSTALL_PARAMS_W parameters = {};
    parameters.cbSize = sizeof(parameters);
    if (!SetupDiGetDeviceInstallParamsW(set, &device, &parameters)) return GetLastError();
    return parameters.Flags & (DI_NEEDREBOOT | DI_NEEDRESTART) ? ERROR_SUCCESS_REBOOT_REQUIRED : ERROR_SUCCESS;
}

static DWORD Install(HDEVINFO set, SP_DEVINFO_DATA& device, const wchar_t* inf, BOOL& reboot)
{
    SP_DEVINSTALL_PARAMS_W parameters = {};
    parameters.cbSize = sizeof(parameters);
    if (!SetupDiGetDeviceInstallParamsW(set, &device, &parameters)) return GetLastError();
    parameters.Flags |= DI_ENUMSINGLEINF | DI_QUIETINSTALL;
    parameters.FlagsEx |= DI_FLAGSEX_ALLOWEXCLUDEDDRVS;
    if (wcscpy_s(parameters.DriverPath, inf) != 0) return ERROR_FILENAME_EXCED_RANGE;
    if (!SetupDiSetDeviceInstallParamsW(set, &device, &parameters) ||
        !SetupDiBuildDriverInfoList(set, &device, SPDIT_COMPATDRIVER))
        return GetLastError();

    SP_DRVINFO_DATA_W selected = {};
    selected.cbSize = sizeof(selected);
    // Bind this INF to this exact instance. A hardware-ID-wide update would
    // also mutate any unrelated duplicate instances with the same hardware ID.
    const BOOL installed = SetupDiEnumDriverInfoW(set, &device, SPDIT_COMPATDRIVER, 0, &selected) &&
        DiInstallDevice(nullptr, set, &device, &selected, 0, &reboot);
    const DWORD error = installed ? ERROR_SUCCESS : GetLastError();
    SetupDiDestroyDriverInfoList(set, &device, SPDIT_COMPATDRIVER);
    return error;
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2 || (wcscmp(argv[1], L"install") != 0 && wcscmp(argv[1], L"remove") != 0)) {
        wprintf(L"Usage (Administrator): device_setup install <absolute cqkeys.inf path>\n"
                L"                       device_setup remove\n"
                L"Manages only %s. Does not alter signing policy or other devices.\n", InstanceId);
        return ERROR_INVALID_PARAMETER;
    }
    const bool installing = wcscmp(argv[1], L"install") == 0;
    if (argc != (installing ? 3 : 2)) return ERROR_INVALID_PARAMETER;
    DeviceSet set;
    if (set.Value == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        fwprintf(stderr, L"SetupDiCreateDeviceInfoList failed: %lu\n", error);
        return static_cast<int>(error);
    }
    SP_DEVINFO_DATA device = {};
    device.cbSize = sizeof(device);
    bool created = false;
    bool exists = SetupDiOpenDeviceInfoW(set.Value, InstanceId, nullptr, 0, &device) != FALSE;
    if (!exists && GetLastError() != ERROR_NO_SUCH_DEVINST) {
        const DWORD error = GetLastError();
        fwprintf(stderr, L"Opening %s failed: %lu\n", InstanceId, error);
        return static_cast<int>(error);
    }
    if (!installing) {
        if (!exists) { wprintf(L"Device is not installed.\n"); return 0; }
        const DWORD error = Remove(set.Value, device);
        wprintf(L"Remove result: %lu (3010 means restart required).\n", error);
        return static_cast<int>(error);
    }

    wchar_t inf[MAX_PATH];
    const DWORD length = GetFullPathNameW(argv[2], MAX_PATH, inf, nullptr);
    if (!length || length >= MAX_PATH) return ERROR_INVALID_NAME;
    const DWORD attributes = GetFileAttributesW(inf);
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY))
        return ERROR_FILE_NOT_FOUND;

    // Validate/stage the signed package before creating a persistent devnode.
    if (!SetupCopyOEMInfW(inf, nullptr, SPOST_PATH, 0, nullptr, 0, nullptr, nullptr)) {
        const DWORD error = GetLastError();
        fwprintf(stderr, L"Staging INF failed: %lu. Check package signature/trust.\n", error);
        return static_cast<int>(error);
    }
    if (!exists) {
        if (!SetupDiCreateDeviceInfoW(set.Value, InstanceId, &GUID_DEVCLASS_SYSTEM,
                L"CQKeys Virtual HID Input Device", nullptr, 0, &device) ||
            !SetupDiSetDeviceRegistryPropertyW(set.Value, &device, SPDRP_HARDWAREID,
                reinterpret_cast<const BYTE*>(HardwareIds), sizeof(HardwareIds)) ||
            !SetupDiCallClassInstaller(DIF_REGISTERDEVICE, set.Value, &device)) {
            const DWORD error = GetLastError();
            fwprintf(stderr, L"Creating %s failed: %lu\n", InstanceId, error);
            return static_cast<int>(error);
        }
        created = true;
    }
    BOOL reboot = FALSE;
    const DWORD error = Install(set.Value, device, inf, reboot);
    if (error != ERROR_SUCCESS) {
        fwprintf(stderr, L"Installing driver failed: %lu\n", error);
        if (created) {
            const DWORD rollback = Remove(set.Value, device);
            fwprintf(stderr, L"New device rollback result: %lu. Staged INF remains in Driver Store.\n", rollback);
        }
        return static_cast<int>(error);
    }
    wprintf(L"PnP installation completed. Restart required: %s\n", reboot ? L"yes" : L"no");
    return reboot ? ERROR_SUCCESS_REBOOT_REQUIRED : ERROR_SUCCESS;
}
