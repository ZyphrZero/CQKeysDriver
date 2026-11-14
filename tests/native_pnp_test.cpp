#include "../cqkeysdll/pch.h"
#include "../cqkeysdll/pnp_device.h"
#include "../shared/cqkeys_protocol.h"
#include <setupapi.h>
#include <cfgmgr32.h>
#include <devpkey.h>
#include <newdev.h>
#include <sddl.h>
#include <string>
#include <vector>
#include <iostream>
#include <cstdlib>
#include <new>

namespace allocation_fault {
    bool armed = false;
    std::size_t targetSize = 0;
}

void resetPackageState();

__declspec(noinline) void* operator new(std::size_t size) {
    if (allocation_fault::armed && size == allocation_fault::targetSize) {
        allocation_fault::armed = false;
        throw std::bad_alloc();
    }
    if (void* memory = std::malloc(size ? size : 1)) return memory;
    throw std::bad_alloc();
}

__declspec(noinline) void operator delete(void* memory) noexcept { std::free(memory); }
__declspec(noinline) void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }

namespace fake {
    HDEVINFO set = reinterpret_cast<HDEVINFO>(1);
    HANDLE deviceHandle = reinterpret_cast<HANDLE>(2);
    HANDLE leaseHandle = reinterpret_cast<HANDLE>(3);
    bool exists = false, enabled = false, registered = false, restart = false, removalPending = false;
    bool wrongHardware = false, wrongClass = false, leaseExists = false;
    bool failLeaseClose = false;
    DWORD openError = 0, copyError = 0, selectError = 0, installError = 0, removeError = 0;
    DWORD state = 0, installFlags = 0;
    int creations = 0, installs = 0, copies = 0;
    ULONGLONG ticks = 0;
    ULONGLONG interfaceReadyAt = 0, problemAt = 0;
    ULONG startProblem = 0;
    LONG problemStatus = 0;
    CONFIGRET statusError = CR_SUCCESS, propertyError = CR_SUCCESS;
    DEVPROPTYPE problemType = DEVPROP_TYPE_NTSTATUS;
    ULONG problemSize = sizeof(LONG);
    int statusQueries = 0, propertyQueries = 0;
    std::wstring inspectedPath;
    std::wstring boundInf, failingPackage;
    DWORD bindingError = 0, deleteError = 0;
    DEVPROPTYPE bindingType = DEVPROP_TYPE_STRING;
    std::vector<std::wstring> deleteAttempts, deletedPackages;
    const GUID systemClass = {0x4d36e97d, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};
    BOOL error(DWORD value) { SetLastError(value); return FALSE; }
    void reset() {
        exists = enabled = registered = restart = removalPending = wrongHardware = wrongClass = leaseExists = false;
        failLeaseClose = false;
        openError = copyError = selectError = installError = removeError = state = installFlags = 0;
        creations = installs = copies = 0; ticks = 0; inspectedPath.clear();
        interfaceReadyAt = problemAt = 0;
        startProblem = 0; problemStatus = 0;
        statusError = propertyError = CR_SUCCESS;
        problemType = DEVPROP_TYPE_NTSTATUS; problemSize = sizeof(LONG);
        statusQueries = propertyQueries = 0;
        boundInf = L"oem42.inf"; failingPackage.clear();
        bindingError = deleteError = 0; bindingType = DEVPROP_TYPE_STRING;
        deleteAttempts.clear(); deletedPackages.clear();
        resetPackageState();
    }
    DWORD WINAPI attributes(LPCWSTR path) { inspectedPath = path; return FILE_ATTRIBUTE_NORMAL; }
    BOOL WINAPI infClass(LPCWSTR, LPGUID result, PWSTR name, DWORD, PDWORD) {
        *result = systemClass;
        if (wrongClass) ++result->Data1;
        wcscpy_s(name, MAX_CLASS_NAME_LEN, L"System");
        return TRUE;
    }
    HDEVINFO WINAPI createList(const GUID*, HWND) { return set; }
    HDEVINFO WINAPI classDevs(const GUID*, PCWSTR, HWND, DWORD) { return set; }
    BOOL WINAPI destroyList(HDEVINFO) { SetLastError(ERROR_INVALID_HANDLE); return TRUE; }
    BOOL WINAPI openInfo(HDEVINFO, PCWSTR id, HWND, DWORD, PSP_DEVINFO_DATA info) {
        if (wcscmp(id, CQKEYS_INSTANCE_ID)) return error(ERROR_INVALID_NAME);
        if (!exists) return error(ERROR_NO_SUCH_DEVINST);
        info->DevInst = 42;
        return TRUE;
    }
    BOOL WINAPI createInfo(HDEVINFO, PCWSTR id, const GUID*, PCWSTR, HWND, DWORD flags, PSP_DEVINFO_DATA info) {
        if (wcscmp(id, CQKEYS_INSTANCE_ID) || flags) return error(ERROR_INVALID_PARAMETER);
        ++creations; exists = true; boundInf.clear(); info->DevInst = 42; return TRUE;
    }
    BOOL WINAPI setProperty(HDEVINFO, PSP_DEVINFO_DATA, DWORD property, const BYTE* data, DWORD size) {
        const wchar_t ids[] = CQKEYS_HARDWARE_ID L"\0";
        return property == SPDRP_HARDWAREID && size == sizeof(ids) && !memcmp(data, ids, size);
    }
    BOOL WINAPI getProperty(HDEVINFO, PSP_DEVINFO_DATA, DWORD property, PDWORD type, PBYTE buffer, DWORD, PDWORD size) {
        const wchar_t ids[] = CQKEYS_HARDWARE_ID L"\0";
        if (property != SPDRP_HARDWAREID) return error(ERROR_INVALID_PARAMETER);
        *type = REG_MULTI_SZ; *size = sizeof(ids); memcpy(buffer, ids, sizeof(ids));
        if (wrongHardware) buffer[0] = 'X';
        return TRUE;
    }
    BOOL WINAPI classInstaller(DI_FUNCTION function, HDEVINFO, PSP_DEVINFO_DATA) {
        if (function == DIF_REGISTERDEVICE) { registered = true; return TRUE; }
        if (function != DIF_PROPERTYCHANGE) return error(ERROR_INVALID_PARAMETER);
        enabled = state == DICS_ENABLE;
        return TRUE;
    }
    BOOL WINAPI getParams(HDEVINFO, PSP_DEVINFO_DATA, PSP_DEVINSTALL_PARAMS_W parameters) {
        parameters->Flags = restart ? DI_NEEDREBOOT : 0;
        return TRUE;
    }
    BOOL WINAPI setParams(HDEVINFO, PSP_DEVINFO_DATA, PSP_DEVINSTALL_PARAMS_W parameters) {
        installFlags = parameters->Flags;
        if (wcscmp(parameters->DriverPath, L"C:\\Windows\\INF\\oem42.inf")) return error(ERROR_INVALID_DATA);
        return TRUE;
    }
    BOOL WINAPI setClassParams(HDEVINFO, PSP_DEVINFO_DATA, PSP_CLASSINSTALL_HEADER header, DWORD size) {
        auto parameters = reinterpret_cast<SP_PROPCHANGE_PARAMS*>(header);
        if (size != sizeof(*parameters) || parameters->Scope != DICS_FLAG_GLOBAL) return error(ERROR_INVALID_PARAMETER);
        state = parameters->StateChange;
        return TRUE;
    }
    BOOL WINAPI copyInf(PCWSTR, PCWSTR, DWORD, DWORD, PWSTR destination, DWORD capacity, PDWORD, PWSTR*) {
        ++copies;
        if (copyError) return error(copyError);
        wcscpy_s(destination, capacity, L"C:\\Windows\\INF\\oem42.inf"); return TRUE;
    }
    BOOL WINAPI buildDrivers(HDEVINFO, PSP_DEVINFO_DATA, DWORD kind) {
        return kind == SPDIT_COMPATDRIVER ? TRUE : error(ERROR_INVALID_PARAMETER);
    }
    BOOL WINAPI enumDriver(HDEVINFO, PSP_DEVINFO_DATA, DWORD kind, DWORD index, PSP_DRVINFO_DATA_W selected) {
        if (selectError) return error(selectError);
        if (kind != SPDIT_COMPATDRIVER || index) return error(ERROR_INVALID_PARAMETER);
        selected->Reserved = 42; return TRUE;
    }
    BOOL WINAPI install(HWND, HDEVINFO, PSP_DEVINFO_DATA device, PSP_DRVINFO_DATA_W selected, DWORD flags, PBOOL reboot) {
        ++installs;
        if (installError) return error(installError);
        if (device->DevInst != 42 || selected->Reserved != 42 || flags != DIIDFLAG_NOFINISHINSTALLUI)
            return error(ERROR_INVALID_PARAMETER);
        *reboot = restart; enabled = true; boundInf = L"oem42.inf"; return TRUE;
    }
    BOOL WINAPI driverProperty(HDEVINFO, PSP_DEVINFO_DATA, const DEVPROPKEY* key, DEVPROPTYPE* type,
                               PBYTE buffer, DWORD capacity, PDWORD bytes, DWORD flags) {
        if (!IsEqualDevPropKey(*key, DEVPKEY_Device_DriverInfPath) || flags)
            return error(ERROR_INVALID_PARAMETER);
        if (bindingError) return error(bindingError);
        if (boundInf.empty()) return error(ERROR_NOT_FOUND);
        *type = bindingType;
        *bytes = static_cast<DWORD>((boundInf.size() + 1) * sizeof(wchar_t));
        if (*bytes > capacity) return error(ERROR_INSUFFICIENT_BUFFER);
        memcpy(buffer, boundInf.c_str(), *bytes);
        return TRUE;
    }
    BOOL WINAPI deletePackage(PCWSTR name, DWORD flags, PVOID reserved) {
        if (flags || reserved || exists) return error(ERROR_INVALID_PARAMETER);
        deleteAttempts.emplace_back(name);
        if (deleteError && (failingPackage.empty() || failingPackage == name)) return error(deleteError);
        deletedPackages.emplace_back(name);
        return TRUE;
    }
    BOOL WINAPI enumInterface(HDEVINFO, PSP_DEVINFO_DATA, const GUID*, DWORD, PSP_DEVICE_INTERFACE_DATA) {
        return enabled && ticks >= interfaceReadyAt && (!startProblem || ticks < problemAt)
            ? TRUE : error(ERROR_NO_MORE_ITEMS);
    }
    BOOL WINAPI interfaceDetail(HDEVINFO, PSP_DEVICE_INTERFACE_DATA, PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail,
                               DWORD, PDWORD required, PSP_DEVINFO_DATA) {
        if (!detail) { *required = sizeof(*detail) + 128; return error(ERROR_INSUFFICIENT_BUFFER); }
        wcscpy_s(detail->DevicePath, 64, L"test-interface"); return TRUE;
    }
    HANDLE WINAPI openFile(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES,
                          DWORD disposition, DWORD flags, HANDLE) {
        if (openError) { SetLastError(openError); return INVALID_HANDLE_VALUE; }
        if (wcscmp(path, L"test-interface") || access != (GENERIC_READ | GENERIC_WRITE) || share ||
            disposition != OPEN_EXISTING || flags != FILE_FLAG_OVERLAPPED) {
            SetLastError(ERROR_INVALID_PARAMETER); return INVALID_HANDLE_VALUE;
        }
        return deviceHandle;
    }
    BOOL WINAPI close(HANDLE handle) {
        if (handle == leaseHandle && failLeaseClose) return error(ERROR_ACCESS_DENIED);
        return TRUE;
    }
    CONFIGRET WINAPI status(PULONG flags, PULONG problem, DEVINST, ULONG) {
        ++statusQueries;
        if (statusError != CR_SUCCESS) return statusError;
        if (enabled && startProblem && ticks >= problemAt) {
            *flags = DN_HAS_PROBLEM; *problem = startProblem;
            return CR_SUCCESS;
        }
        *flags = enabled ? DN_STARTED : DN_HAS_PROBLEM;
        *problem = enabled ? 0 : CM_PROB_DISABLED;
        return CR_SUCCESS;
    }
    CONFIGRET WINAPI devNodeProperty(DEVINST instance, const DEVPROPKEY* key, DEVPROPTYPE* type,
                                     PBYTE buffer, PULONG size, ULONG flags) {
        ++propertyQueries;
        if (instance != 42 || !IsEqualDevPropKey(*key, DEVPKEY_Device_ProblemStatus) || flags ||
            *size != sizeof(problemStatus)) return CR_INVALID_DATA;
        if (propertyError != CR_SUCCESS) return propertyError;
        *type = problemType; *size = problemSize;
        memcpy(buffer, &problemStatus, sizeof(problemStatus));
        return CR_SUCCESS;
    }
    BOOL WINAPI uninstall(HWND, HDEVINFO, PSP_DEVINFO_DATA, DWORD, PBOOL reboot) {
        if (removeError) return error(removeError);
        *reboot = restart;
        if (!removalPending && !restart) { exists = false; enabled = false; }
        return TRUE;
    }
    CONFIGRET WINAPI locate(PDEVINST, DEVINSTID_W, ULONG) { return exists ? CR_SUCCESS : CR_NO_SUCH_DEVNODE; }
    ULONGLONG WINAPI clock() { return ticks; }
    void WINAPI sleep(DWORD interval) { ticks += interval; }
    HANDLE WINAPI event(LPSECURITY_ATTRIBUTES security, BOOL, BOOL, LPCWSTR) {
        if (!security || !security->lpSecurityDescriptor || security->bInheritHandle) return nullptr;
        SetLastError(leaseExists ? ERROR_ALREADY_EXISTS : ERROR_SUCCESS); return leaseHandle;
    }
}

#define GetFileAttributesW fake::attributes
#define SetupDiGetINFClassW fake::infClass
#define SetupDiCreateDeviceInfoList fake::createList
#define SetupDiGetClassDevsW fake::classDevs
#define SetupDiDestroyDeviceInfoList fake::destroyList
#define SetupDiOpenDeviceInfoW fake::openInfo
#define SetupDiCreateDeviceInfoW fake::createInfo
#define SetupDiSetDeviceRegistryPropertyW fake::setProperty
#define SetupDiGetDeviceRegistryPropertyW fake::getProperty
#define SetupDiCallClassInstaller fake::classInstaller
#define SetupDiGetDeviceInstallParamsW fake::getParams
#define SetupDiSetDeviceInstallParamsW fake::setParams
#define SetupDiSetClassInstallParamsW fake::setClassParams
#define SetupCopyOEMInfW fake::copyInf
#define SetupDiGetDevicePropertyW fake::driverProperty
#define SetupUninstallOEMInfW fake::deletePackage
#define SetupDiBuildDriverInfoList fake::buildDrivers
#define SetupDiEnumDriverInfoW fake::enumDriver
#define DiInstallDevice fake::install
#define SetupDiEnumDeviceInterfaces fake::enumInterface
#define SetupDiGetDeviceInterfaceDetailW fake::interfaceDetail
#define CreateFileW fake::openFile
#define CloseHandle fake::close
#define CM_Get_DevNode_Status fake::status
#define CM_Get_DevNode_PropertyW fake::devNodeProperty
#define DiUninstallDevice fake::uninstall
#define CM_Locate_DevNodeW fake::locate
#define GetTickCount64 fake::clock
#define Sleep fake::sleep
#define CreateEventW fake::event
#include "../cqkeysdll/pnp_device.cpp"

void resetPackageState() {
    g_stagedInf[0] = L'\0';
    g_packagesToDelete.clear();
    g_packageRemovalPending = false;
}

int failures = 0;
void check(bool condition, const char* message) {
    if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}

int main() {
    fake::reset();
    bool escaped = false;
    BOOL installed = TRUE;
    allocation_fault::targetSize = sizeof(L"C:\\test\\cqkeys.inf");
    allocation_fault::armed = true;
    try { installed = CqInstallDevice("C:\\test\\cqkeys.inf"); }
    catch (const std::bad_alloc&) { escaped = true; }
    allocation_fault::armed = false;
    check(!escaped && !installed && GetLastError() == ERROR_NOT_ENOUGH_MEMORY && fake::installs == 0,
        "path allocation failure stays inside the BOOL API");
    fake::reset(); fake::exists = fake::enabled = true;
    escaped = false;
    HANDLE interfaceHandle = nullptr;
    allocation_fault::targetSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W) + 128;
    allocation_fault::armed = true;
    try { interfaceHandle = CqOpenInterface(); }
    catch (const std::bad_alloc&) { escaped = true; }
    allocation_fault::armed = false;
    check(!escaped && interfaceHandle == INVALID_HANDLE_VALUE && GetLastError() == ERROR_NOT_ENOUGH_MEMORY,
        "interface allocation failure returns a Win32 error");
    fake::reset();
    std::string oversizedPath(MAX_PATH + 1, 'a');
    oversizedPath += ".inf";
    escaped = false;
    installed = TRUE;
    allocation_fault::targetSize = (oversizedPath.size() + 1) * sizeof(wchar_t);
    allocation_fault::armed = true;
    try { installed = CqInstallDevice(oversizedPath.c_str()); }
    catch (const std::bad_alloc&) { escaped = true; }
    bool rejectedBeforeAllocation = allocation_fault::armed;
    allocation_fault::armed = false;
    check(!escaped && !installed && rejectedBeforeAllocation && GetLastError() == ERROR_FILENAME_EXCED_RANGE,
        "oversized package path is rejected before allocating its conversion buffer");
    fake::reset();
    check(!CqInstallDevice(nullptr) && GetLastError() == ERROR_INVALID_PARAMETER, "null path fails before PnP");
    check(!CqInstallDevice("C:\\test\\cqkeys.dll") && fake::creations == 0, "unsupported extension does not create node");
    fake::wrongClass = true;
    check(!CqInstallDevice("C:\\test\\cqkeys.inf") && GetLastError() == ERROR_CLASS_MISMATCH, "INF class mismatch");
    fake::wrongClass = false;
    check(CqInstallDevice("C:\\test\\cqkeys.sys"), "SYS selector resolves complete INF package");
    check(fake::inspectedPath == L"C:\\test\\cqkeys.inf", "never installs a bare SYS");
    check(fake::registered && fake::creations == 1 && fake::installs == 1, "register and install fixed root device");
    check((fake::installFlags & DI_ENUMSINGLEINF) != 0, "driver selection is restricted to staged INF");
    check(CqChangeDevice(false) && !fake::enabled && fake::exists, "disable preserves node");
    check(CqInstallDevice("C:\\test\\cqkeys.inf") && fake::creations == 1, "reuse fixed instance without duplicate nodes");
    fake::openError = ERROR_SHARING_VIOLATION;
    check(!CqChangeDevice(true) && GetLastError() == ERROR_SHARING_VIOLATION, "held raw handle prevents removal");
    int before = fake::installs;
    check(!CqInstallDevice("C:\\test\\cqkeys.inf") && fake::installs == before, "held handle prevents update");
    fake::openError = 0;
    fake::wrongHardware = true;
    check(!CqChangeDevice(true) && GetLastError() == ERROR_INVALID_DATA, "never remove mismatched hardware");
    fake::wrongHardware = false;
    fake::removalPending = true;
    check(!CqChangeDevice(true) && GetLastError() == ERROR_BUSY, "removal success requires absent node");
    fake::removalPending = false;
    fake::removeError = ERROR_ACCESS_DENIED;
    check(!CqChangeDevice(true) && GetLastError() == ERROR_ACCESS_DENIED, "preserve removal error across RAII cleanup");
    fake::removeError = 0;
    check(CqChangeDevice(true) && !fake::exists, "retry removal");
    check(CqChangeDevice(true), "absent device is already removed");

    fake::reset(); fake::copyError = ERROR_ACCESS_DENIED;
    check(!CqInstallDevice("C:\\test\\cqkeys.inf") && GetLastError() == ERROR_ACCESS_DENIED && fake::installs == 0,
        "package staging failure is not installation success");
    check(CqChangeDevice(true), "clean partially registered root node");
    fake::reset(); fake::selectError = ERROR_NO_MORE_ITEMS;
    check(!CqInstallDevice("C:\\test\\cqkeys.inf") && fake::installs == 0, "incompatible INF does not bind arbitrary driver");
    fake::reset(); fake::restart = true;
    check(!CqInstallDevice("C:\\test\\cqkeys.inf") && GetLastError() == ERROR_SUCCESS_REBOOT_REQUIRED, "installation restart is explicit");
    check(!CqChangeDevice(false) && GetLastError() == ERROR_SUCCESS_REBOOT_REQUIRED, "disable restart is explicit");
    check(!CqChangeDevice(true) && GetLastError() == ERROR_SUCCESS_REBOOT_REQUIRED, "removal restart is explicit");

    fake::reset(); fake::startProblem = CM_PROB_DRIVER_FAILED_LOAD;
    fake::problemStatus = static_cast<LONG>(0xC0000603);
    CqDriverStartResult diagnostics{};
    check(!CqInstallDevice("C:\\test\\cqkeys.inf", &diagnostics) && GetLastError() == static_cast<DWORD>(CERT_E_REVOKED),
        "revoked driver returns the real NTSTATUS mapping instead of timeout");
    check(diagnostics.problemCode == CM_PROB_DRIVER_FAILED_LOAD &&
        diagnostics.problemStatus == static_cast<LONG>(0xC0000603) && !diagnostics.diagnosticError,
        "raw problem code and NTSTATUS are preserved for callers");
    check(fake::ticks == 0 && fake::statusQueries == 1 && fake::propertyQueries == 1,
        "terminal load failure is detected without waiting five seconds");
    fake::reset(); fake::interfaceReadyAt = 150;
    check(CqInstallDevice("C:\\test\\cqkeys.inf", &diagnostics) && fake::ticks == 150 && GetLastError() == ERROR_SUCCESS,
        "interface startup without a device problem can still complete asynchronously");
    check(!diagnostics.problemCode && !diagnostics.problemStatus && !diagnostics.diagnosticError,
        "a successful retry does not reuse stale startup diagnostics");
    fake::reset(); fake::interfaceReadyAt = 6000;
    check(!CqInstallDevice("C:\\test\\cqkeys.inf") && GetLastError() == ERROR_TIMEOUT && fake::ticks == 5000,
        "timeout remains reserved for an interface that never appears without a reported problem");
    fake::reset(); fake::interfaceReadyAt = 6000; fake::problemAt = 5000;
    fake::startProblem = CM_PROB_DRIVER_FAILED_LOAD; fake::problemStatus = static_cast<LONG>(0xC0000603);
    check(!CqInstallDevice("C:\\test\\cqkeys.inf") && GetLastError() == static_cast<DWORD>(CERT_E_REVOKED),
        "a failure at the timeout boundary is not overwritten by ERROR_TIMEOUT");
    fake::reset(); fake::interfaceReadyAt = 6000; fake::statusError = CR_ACCESS_DENIED;
    check(!CqInstallDevice("C:\\test\\cqkeys.inf", &diagnostics) && GetLastError() == ERROR_ACCESS_DENIED &&
        diagnostics.diagnosticError == ERROR_ACCESS_DENIED,
        "device status query failure is not hidden behind timeout");
    fake::reset(); fake::startProblem = CM_PROB_FAILED_START; fake::propertyError = CR_ACCESS_DENIED;
    check(!CqInstallDevice("C:\\test\\cqkeys.inf") && GetLastError() == ERROR_ACCESS_DENIED,
        "problem status query failure is explicit");
    fake::reset(); fake::startProblem = CM_PROB_FAILED_START; fake::propertyError = CR_NO_SUCH_VALUE;
    check(!CqInstallDevice("C:\\test\\cqkeys.inf", &diagnostics) && GetLastError() == ERROR_GEN_FAILURE &&
        fake::ticks == 0 && diagnostics.problemCode == CM_PROB_FAILED_START && diagnostics.diagnosticError != 0,
        "a device problem without optional NTSTATUS is still a failure, not a timeout");
    fake::reset(); fake::startProblem = CM_PROB_DRIVER_FAILED_LOAD; fake::problemType = DEVPROP_TYPE_UINT32;
    check(!CqInstallDevice("C:\\test\\cqkeys.inf") && GetLastError() == ERROR_INVALID_DATA,
        "problem status property type must be NTSTATUS");
    fake::reset(); fake::startProblem = CM_PROB_DRIVER_FAILED_LOAD; fake::problemSize = sizeof(USHORT);
    check(!CqInstallDevice("C:\\test\\cqkeys.inf") && GetLastError() == ERROR_INVALID_DATA,
        "problem status property size must match NTSTATUS");
    fake::reset(); fake::startProblem = CM_PROB_NEED_RESTART;
    check(!CqInstallDevice("C:\\test\\cqkeys.inf") && GetLastError() == ERROR_SUCCESS_REBOOT_REQUIRED,
        "PnP restart problem preserves the manual restart contract");
    fake::reset(); fake::startProblem = CM_PROB_FAILED_START; fake::problemStatus = static_cast<LONG>(0xC0000022);
    check(!CqInstallDevice("C:\\test\\cqkeys.inf", &diagnostics) && GetLastError() == ERROR_ACCESS_DENIED &&
        diagnostics.problemStatus == fake::problemStatus,
        "NTSTATUS mapping handles failures other than revoked certificates");
    fake::reset(); fake::startProblem = CM_PROB_UNSIGNED_DRIVER; fake::propertyError = CR_NO_SUCH_VALUE;
    check(!CqInstallDevice("C:\\test\\cqkeys.inf") && GetLastError() == ERROR_INVALID_IMAGE_HASH,
        "unsigned-driver problem remains actionable without optional NTSTATUS");
    fake::reset(); fake::startProblem = CM_PROB_FAILED_START; fake::problemStatus = static_cast<LONG>(0xC0FF1234);
    check(!CqInstallDevice("C:\\test\\cqkeys.inf", &diagnostics) && GetLastError() == ERROR_MR_MID_NOT_FOUND &&
        diagnostics.problemStatus == fake::problemStatus,
        "unmapped NTSTATUS is retained rather than replaced with a fabricated cause");
    fake::reset(); fake::startProblem = CM_PROB_FAILED_START; fake::problemStatus = static_cast<LONG>(0xE1234567);
    check(!CqInstallDevice("C:\\test\\cqkeys.inf", &diagnostics) && GetLastError() == 0xE1234567 &&
        diagnostics.problemStatus == fake::problemStatus,
        "customer-defined status follows the system's pass-through mapping");
    fake::reset(); fake::openError = ERROR_ACCESS_DENIED;
    check(!CqInstallDevice("C:\\test\\cqkeys.inf") && GetLastError() == ERROR_ACCESS_DENIED && fake::statusQueries == 0,
        "non-retryable interface errors are not overwritten by PnP diagnostics");

    fake::reset();
    check(CqInstallDevice("C:\\test\\cqkeys.inf") && CqChangeDevice(true) && fake::deleteAttempts.empty(),
        "ordinary removal preserves Driver Store packages");
    fake::reset();
    check(CqInstallDevice("C:\\test\\cqkeys.inf") && CqChangeDevice(true, true),
        "explicit package removal follows successful device removal");
    check(fake::deletedPackages == std::vector<std::wstring>{L"oem42.inf"},
        "staged and bound package are deduplicated and deleted by exact published name without force");
    check(CqChangeDevice(true, true) && fake::deleteAttempts.size() == 1,
        "completed package removal is idempotent");

    fake::reset();
    check(CqInstallDevice("C:\\test\\cqkeys.inf"), "prepare package removal retry");
    fake::deleteError = ERROR_INF_IN_USE_BY_DEVICES;
    check(!CqChangeDevice(true, true) && GetLastError() == ERROR_INF_IN_USE_BY_DEVICES && !fake::exists,
        "a shared package fails honestly after device removal");
    check(!CqChangeDevice(true) && GetLastError() == ERROR_BUSY,
        "ordinary unload cannot silently discard pending package deletion");
    check(!CqReleaseSession() && GetLastError() == ERROR_BUSY,
        "runtime cleanup cannot abandon pending package deletion");
    check(!CqInstallDevice("C:\\test\\cqkeys.inf") && GetLastError() == ERROR_BUSY,
        "pending package deletion blocks a new installation");
    fake::deleteError = 0;
    check(CqChangeDevice(true, true) && fake::deletedPackages == std::vector<std::wstring>{L"oem42.inf"},
        "retry retains exact package identity after the device is gone");

    fake::reset(); fake::exists = true; fake::boundInf = L"oem41.inf";
    fake::installError = ERROR_ACCESS_DENIED;
    check(!CqInstallDevice("C:\\test\\cqkeys.inf"), "simulate update failure after staging");
    fake::deleteError = ERROR_ACCESS_DENIED; fake::failingPackage = L"oem42.inf";
    check(!CqChangeDevice(true, true) && GetLastError() == ERROR_ACCESS_DENIED &&
        fake::deletedPackages == std::vector<std::wstring>{L"oem41.inf"},
        "failed update targets both the previous binding and newly staged package");
    fake::deleteError = 0;
    check(CqChangeDevice(true, true) && fake::deletedPackages ==
        std::vector<std::wstring>{L"oem41.inf", L"oem42.inf"} && fake::deleteAttempts.size() == 3,
        "partial deletion retry does not delete an already completed package again");

    fake::reset(); fake::exists = fake::enabled = true; fake::openError = ERROR_SHARING_VIOLATION;
    check(!CqChangeDevice(true, true) && fake::deleteAttempts.empty() && fake::exists,
        "open handles block package removal before changing the node");
    fake::reset(); fake::exists = true; fake::wrongHardware = true;
    check(!CqChangeDevice(true, true) && fake::deleteAttempts.empty() && fake::exists,
        "mismatched hardware never authorizes package deletion");
    fake::reset(); fake::exists = true; fake::bindingError = ERROR_ACCESS_DENIED;
    check(!CqChangeDevice(true, true) && GetLastError() == ERROR_ACCESS_DENIED && fake::exists,
        "unreadable package identity fails before node removal");
    for (const wchar_t* invalidName : {L"cqkeys.inf", L"..\\oem42.inf", L"C:\\Windows\\INF\\oem42.inf", L"oem.inf"}) {
        fake::reset(); fake::exists = true; fake::boundInf = invalidName;
        check(!CqChangeDevice(true, true) && GetLastError() == ERROR_INVALID_DATA && fake::exists,
            "only published OEM INF basenames are accepted for deletion");
    }
    fake::reset(); fake::exists = true; fake::bindingType = DEVPROP_TYPE_UINT32;
    check(!CqChangeDevice(true, true) && GetLastError() == ERROR_INVALID_DATA && fake::exists,
        "package identity property must be a string");
    fake::reset(); fake::exists = true; fake::restart = true;
    check(!CqChangeDevice(true, true) && GetLastError() == ERROR_SUCCESS_REBOOT_REQUIRED && fake::deleteAttempts.empty(),
        "restart-required removal does not prematurely delete the package");
    fake::reset(); fake::exists = true; fake::removalPending = true;
    check(!CqChangeDevice(true, true) && GetLastError() == ERROR_BUSY && fake::deleteAttempts.empty(),
        "package deletion waits for actual node disappearance");
    fake::reset();
    check(!CqChangeDevice(false, true) && GetLastError() == ERROR_INVALID_PARAMETER,
        "package deletion cannot be combined with disable-only");
    check(CqInstallDevice("C:\\test\\cqkeys.inf"), "stage package before external node removal");
    fake::exists = fake::enabled = false;
    check(CqChangeDevice(true, true) && fake::deletedPackages == std::vector<std::wstring>{L"oem42.inf"},
        "session-staged package can be deleted after external node disappearance");

    fake::reset(); fake::leaseExists = true;
    check(!CqAcquireSession() && GetLastError() == ERROR_BUSY, "cross-process lease rejects existing event");
    fake::leaseExists = false;
    check(CqAcquireSession() && CqAcquireSession(), "owned lease can be reused");
    fake::failLeaseClose = true;
    check(!CqReleaseSession() && g_sessionLease == fake::leaseHandle && GetLastError() == ERROR_ACCESS_DENIED,
        "failed lease close retains ownership for retry");
    fake::failLeaseClose = false;
    check(CqReleaseSession(), "failed lease close can be retried");
    check(CqAcquireSession(), "released lease can be acquired again");
    check(CqReleaseSession() && CqReleaseSession(), "completed lease release is idempotent");
    if (failures) return 1;
    std::cout << "Native SetupAPI/PnP boundary tests passed (all device operations simulated)\n";
    return 0;
}
