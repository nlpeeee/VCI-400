// vci400-winusb-bind: switch one VCI-400 USB function between Microsoft's inbox WinUSB
// driver and its normal best-match driver.
//
//   status   Read-only: instance, IDs, service, INF, problem code, interface GUID value.
//   bind     Run as administrator. Writes DeviceInterfaceGUIDs on the selected devnode only,
//            then installs the generic "WinUsb Device" model from %SystemRoot%\INF\winusb.inf
//            (Microsoft-signed, inbox). No certificate, catalog, policy or signing change.
//   restore  Run as administrator. Removes that value and reinstalls the best-ranked
//            compatible inbox driver (normally usbmidi2.inf for MI_03). A devnode with no
//            compatible driver (MI_05, DFU mode) is uninstalled and rescanned instead, which
//            returns it to its original "no driver" state.
//
// Targets (one per run; default --interface MI_03):
//   --interface MI_03  USB\VID_1AA1&PID_0231&MI_03  MIDI function
//   --interface MI_05  USB\VID_1AA1&PID_0231&MI_05  runtime DFU interface (FE/01/01)
//   --dfu-mode         USB\VID_1AA1&PID_0231\...    the non-composite device in DFU mode
//                      after DFU_DETACH; must expose Class_FE&SubClass_01&Prot_02
// Every other function of the device is left alone.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <setupapi.h>
#include <newdev.h>
#include <cfgmgr32.h>
#include <tlhelp32.h>
#include <combaseapi.h>

#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <initializer_list>
#include <string>
#include <vector>

#include "vci400_winusb.h"

namespace {

constexpr wchar_t kWinUsbHardwareId[] = L"USB\\MS_COMP_WINUSB";
constexpr wchar_t kVendorPrefix[] = L"USB\\VID_1AA1&";
constexpr wchar_t kDfuModeClassId[] = L"CLASS_FE&SUBCLASS_01&PROT_02";  // compared upper-case

struct Target {
    const wchar_t* name;        // as shown to the user
    const wchar_t* prefix;      // instance ID prefix
    bool dfuMode;               // also require the DFU-mode class ID
    const wchar_t* guidText;    // DeviceInterfaceGUIDs value
};

constexpr Target kTargets[] = {
    {L"MI_03", L"USB\\VID_1AA1&PID_0231&MI_03\\", false, kVci400WinUsbInterfaceGuidText},
    {L"MI_05", L"USB\\VID_1AA1&PID_0231&MI_05\\", false, kVci400WinUsbDfuRuntimeGuidText},
    // A composite child always has "&MI_" before the backslash, so this prefix only matches
    // devnodes of the whole device: the composite parent in runtime mode, or the DFU-mode device.
    {L"DFU mode", L"USB\\VID_1AA1&PID_0231\\", true, kVci400WinUsbDfuModeGuidText},
};

std::wstring ErrorText(DWORD error) {
    if (error == ERROR_NO_SUCH_DEVINST) return L"no matching present devnode";
    if (error == ERROR_TOO_MANY_NAMES) return L"more than one matching devnode";
    wchar_t* buffer = nullptr;
    DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                  nullptr, error, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring text = length ? std::wstring(buffer, length) : L"unknown error";
    if (buffer) LocalFree(buffer);
    while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r' || text.back() == L' ')) text.pop_back();
    return text;
}

int Fail(const wchar_t* step, DWORD error) {
    wprintf(L"FAIL %ls: %lu (0x%08lX) %ls\n", step, error, error, ErrorText(error).c_str());
    return 1;
}

std::wstring Upper(std::wstring text) {
    for (wchar_t& c : text) c = static_cast<wchar_t>(towupper(c));
    return text;
}

bool IsElevated() {
    HANDLE token = nullptr;
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    bool elevated = OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) &&
                    GetTokenInformation(token, TokenElevation, &elevation, sizeof elevation, &size) && elevation.TokenIsElevated;
    if (token) CloseHandle(token);
    return elevated;
}

std::wstring RunningMidiApp() {
    const wchar_t* names[] = {L"Serato DJ Pro.exe", L"Serato DJ Lite.exe", L"Mixxx.exe", L"reaper.exe", L"VirtualDJ.exe", L"Traktor.exe"};
    std::wstring found;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return found;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof entry;
    for (BOOL ok = Process32FirstW(snapshot, &entry); ok && found.empty(); ok = Process32NextW(snapshot, &entry))
        for (const wchar_t* name : names)
            if (_wcsicmp(entry.szExeFile, name) == 0) found = name;
    CloseHandle(snapshot);
    return found;
}

struct Device {
    HDEVINFO set = INVALID_HANDLE_VALUE;
    SP_DEVINFO_DATA data{};
    std::wstring instance;
    ~Device() {
        if (set != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(set);
    }
};

std::vector<std::wstring> RegistryMultiString(HDEVINFO set, SP_DEVINFO_DATA* data, DWORD property) {
    std::vector<std::wstring> values;
    std::vector<wchar_t> buffer(4096, L'\0');
    DWORD type = 0;
    if (!SetupDiGetDeviceRegistryPropertyW(set, data, property, &type, reinterpret_cast<BYTE*>(buffer.data()),
                                           static_cast<DWORD>((buffer.size() - 2) * sizeof(wchar_t)), nullptr) ||
        type != REG_MULTI_SZ)
        return values;
    for (const wchar_t* p = buffer.data(); *p; p += wcslen(p) + 1) values.emplace_back(p);
    return values;
}

// Returns the first hardware or compatible ID that names the DFU-mode interface class, or "".
std::wstring DfuModeClassMatch(HDEVINFO set, SP_DEVINFO_DATA* data) {
    for (DWORD property : {SPDRP_HARDWAREID, SPDRP_COMPATIBLEIDS})
        for (const std::wstring& id : RegistryMultiString(set, data, property))
            if (Upper(id).find(kDfuModeClassId) != std::wstring::npos) return id;
    return L"";
}

// Finds exactly one present devnode for the target.
DWORD FindDevice(const Target& target, Device& device, bool verbose) {
    device.set = SetupDiGetClassDevsW(nullptr, L"USB", nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (device.set == INVALID_HANDLE_VALUE) return GetLastError();
    const std::wstring prefix = Upper(target.prefix);
    int matches = 0;
    for (DWORD index = 0;; ++index) {
        SP_DEVINFO_DATA data{};
        data.cbSize = sizeof data;
        if (!SetupDiEnumDeviceInfo(device.set, index, &data)) break;
        wchar_t id[MAX_DEVICE_ID_LEN];
        if (!SetupDiGetDeviceInstanceIdW(device.set, &data, id, MAX_DEVICE_ID_LEN, nullptr)) continue;
        const std::wstring upper = Upper(id);
        if (!target.dfuMode) {
            if (upper.compare(0, prefix.size(), prefix) != 0) continue;
        } else {
            // DFU mode: look at every non-composite-child devnode of the vendor and say why it
            // was or was not taken, so a re-enumeration with an unexpected PID is visible.
            if (upper.compare(0, wcslen(kVendorPrefix), kVendorPrefix) != 0 || upper.find(L"&MI_") != std::wstring::npos) continue;
            const std::wstring classId = DfuModeClassMatch(device.set, &data);
            const bool samePid = upper.compare(0, prefix.size(), prefix) == 0;
            if (verbose) {
                if (classId.empty())
                    wprintf(L"  skipped:  %ls (no Class_FE&SubClass_01&Prot_02 ID; %ls)\n", id,
                            samePid ? L"runtime composite device, not in DFU mode" : L"other PID");
                else if (!samePid)
                    wprintf(L"  skipped:  %ls (DFU-mode class %ls, but not PID_0231)\n", id, classId.c_str());
                else
                    wprintf(L"  matched:  %ls by instance prefix %ls and ID %ls\n", id, target.prefix, classId.c_str());
            }
            if (classId.empty() || !samePid) continue;
        }
        ++matches;
        device.data = data;
        device.instance = id;
    }
    if (matches == 0) return ERROR_NO_SUCH_DEVINST;
    if (matches > 1) return ERROR_TOO_MANY_NAMES;
    return ERROR_SUCCESS;
}

std::wstring RegistryString(const Device& device, DWORD property) {
    wchar_t buffer[1024]{};
    DWORD type = 0;
    if (!SetupDiGetDeviceRegistryPropertyW(device.set, const_cast<SP_DEVINFO_DATA*>(&device.data), property, &type,
                                           reinterpret_cast<BYTE*>(buffer), sizeof buffer - sizeof(wchar_t), nullptr))
        return L"";
    return buffer;
}

std::wstring Joined(const Device& device, DWORD property) {
    std::wstring text;
    for (const std::wstring& id : RegistryMultiString(device.set, const_cast<SP_DEVINFO_DATA*>(&device.data), property))
        text += (text.empty() ? L"" : L" | ") + id;
    return text.empty() ? L"(none)" : text;
}

std::wstring DriverInf(const Device& device) {
    HKEY key = SetupDiOpenDevRegKey(device.set, const_cast<SP_DEVINFO_DATA*>(&device.data), DICS_FLAG_GLOBAL, 0, DIREG_DRV, KEY_READ);
    if (key == INVALID_HANDLE_VALUE) return L"";
    wchar_t buffer[512]{};
    DWORD size = sizeof buffer - sizeof(wchar_t);
    DWORD type = 0;
    std::wstring inf;
    if (RegQueryValueExW(key, L"InfPath", nullptr, &type, reinterpret_cast<BYTE*>(buffer), &size) == ERROR_SUCCESS && type == REG_SZ) inf = buffer;
    RegCloseKey(key);
    return inf;
}

std::wstring InterfaceGuidValue(const Device& device) {
    HKEY key = SetupDiOpenDevRegKey(device.set, const_cast<SP_DEVINFO_DATA*>(&device.data), DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
    if (key == INVALID_HANDLE_VALUE) return L"";
    wchar_t buffer[512]{};
    DWORD size = sizeof buffer - 2 * sizeof(wchar_t);
    DWORD type = 0;
    std::wstring value;
    if (RegQueryValueExW(key, L"DeviceInterfaceGUIDs", nullptr, &type, reinterpret_cast<BYTE*>(buffer), &size) == ERROR_SUCCESS) value = buffer;
    RegCloseKey(key);
    return value;
}

void PrintStatus(const Target& target, const Device& device) {
    ULONG status = 0, problem = 0;
    CONFIGRET cr = CM_Get_DevNode_Status(&status, &problem, device.data.DevInst, 0);
    wprintf(L"Target:     %ls\n", target.name);
    wprintf(L"Instance:   %ls\n", device.instance.c_str());
    wprintf(L"HardwareID: %ls\n", Joined(device, SPDRP_HARDWAREID).c_str());
    wprintf(L"Compatible: %ls\n", Joined(device, SPDRP_COMPATIBLEIDS).c_str());
    wprintf(L"Service:    %ls\n", RegistryString(device, SPDRP_SERVICE).c_str());
    wprintf(L"Class:      %ls\n", RegistryString(device, SPDRP_CLASS).c_str());
    wprintf(L"INF:        %ls\n", DriverInf(device).c_str());
    if (cr == CR_SUCCESS)
        wprintf(L"Status:     %ls (problem %lu)\n", (status & DN_STARTED) ? L"started" : L"not started", problem);
    else
        wprintf(L"Status:     not available (CONFIGRET %lu)\n", cr);
    std::wstring guid = InterfaceGuidValue(device);
    wprintf(L"DeviceInterfaceGUIDs: %ls (bind writes %ls)\n", guid.empty() ? L"(not set)" : guid.c_str(), target.guidText);
}

int Bind(const Target& target, Device& device) {
    // 1. Interface GUID so our tools can open the WinUSB device. Multi-string: value + 2 NULs.
    HKEY key = SetupDiCreateDevRegKeyW(device.set, &device.data, DICS_FLAG_GLOBAL, 0, DIREG_DEV, nullptr, nullptr);
    if (key == INVALID_HANDLE_VALUE) return Fail(L"open Device Parameters", GetLastError());
    std::wstring multi = std::wstring(target.guidText) + L'\0' + L'\0';
    LSTATUS rs = RegSetValueExW(key, L"DeviceInterfaceGUIDs", 0, REG_MULTI_SZ, reinterpret_cast<const BYTE*>(multi.data()),
                                static_cast<DWORD>(multi.size() * sizeof(wchar_t)));
    RegCloseKey(key);
    if (rs != ERROR_SUCCESS) return Fail(L"write DeviceInterfaceGUIDs", static_cast<DWORD>(rs));

    // 2. Restrict the driver search to the inbox winusb.inf and its class, like
    //    Device Manager's "Let me pick > Universal Serial Bus devices".
    wchar_t inf[MAX_PATH];
    GetWindowsDirectoryW(inf, MAX_PATH);
    wcscat_s(inf, L"\\INF\\winusb.inf");
    GUID classGuid{};
    wchar_t className[MAX_CLASS_NAME_LEN];
    if (!SetupDiGetINFClassW(inf, &classGuid, className, MAX_CLASS_NAME_LEN, nullptr)) return Fail(L"read winusb.inf class", GetLastError());
    // SPDRP_CLASSGUID takes the GUID as a REG_SZ string ("{...}"), not a binary GUID.
    wchar_t classText[40];
    if (!StringFromGUID2(classGuid, classText, 40)) return Fail(L"format USBDevice class GUID", ERROR_INVALID_DATA);
    if (!SetupDiSetDeviceRegistryPropertyW(device.set, &device.data, SPDRP_CLASSGUID, reinterpret_cast<const BYTE*>(classText),
                                           static_cast<DWORD>((wcslen(classText) + 1) * sizeof(wchar_t))))
        return Fail(L"select USBDevice class", GetLastError());

    SP_DEVINSTALL_PARAMS_W params{};
    params.cbSize = sizeof params;
    if (!SetupDiGetDeviceInstallParamsW(device.set, &device.data, &params)) return Fail(L"get install params", GetLastError());
    params.Flags |= DI_ENUMSINGLEINF;
    params.FlagsEx |= DI_FLAGSEX_ALLOWEXCLUDEDDRVS;
    wcscpy_s(params.DriverPath, inf);
    if (!SetupDiSetDeviceInstallParamsW(device.set, &device.data, &params)) return Fail(L"set install params", GetLastError());
    if (!SetupDiBuildDriverInfoList(device.set, &device.data, SPDIT_CLASSDRIVER)) return Fail(L"build winusb.inf driver list", GetLastError());

    // 3. Pick the generic model by hardware ID (USB\MS_COMP_WINUSB), not by localized name.
    SP_DRVINFO_DATA_W chosen{};
    bool found = false;
    for (DWORD index = 0; !found; ++index) {
        SP_DRVINFO_DATA_W driver{};
        driver.cbSize = sizeof driver;
        if (!SetupDiEnumDriverInfoW(device.set, &device.data, SPDIT_CLASSDRIVER, index, &driver)) break;
        std::vector<BYTE> buffer(8192);
        auto* detail = reinterpret_cast<SP_DRVINFO_DETAIL_DATA_W*>(buffer.data());
        detail->cbSize = sizeof(SP_DRVINFO_DETAIL_DATA_W);
        if (!SetupDiGetDriverInfoDetailW(device.set, &device.data, &driver, detail, static_cast<DWORD>(buffer.size()), nullptr)) continue;
        wprintf(L"  candidate: %ls [%ls]\n", driver.Description, detail->HardwareID);
        if (_wcsicmp(detail->HardwareID, kWinUsbHardwareId) == 0) {
            chosen = driver;
            found = true;
        }
    }
    if (!found) return Fail(L"find generic WinUsb Device model", ERROR_NOT_FOUND);
    if (!SetupDiSetSelectedDriverW(device.set, &device.data, &chosen)) return Fail(L"select driver", GetLastError());

    // 4. Install. DiInstallDevice applies only to this devnode.
    BOOL reboot = FALSE;
    if (!DiInstallDevice(nullptr, device.set, &device.data, &chosen, 0, &reboot)) return Fail(L"install WinUSB", GetLastError());
    wprintf(L"PASS bind %ls: %ls -> %ls%ls\n", target.name, device.instance.c_str(), chosen.Description, reboot ? L" (reboot required)" : L"");
    return 0;
}

int Restore(const Target& target, Device& device) {
    HKEY key = SetupDiOpenDevRegKey(device.set, &device.data, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_SET_VALUE);
    if (key != INVALID_HANDLE_VALUE) {
        LSTATUS rs = RegDeleteValueW(key, L"DeviceInterfaceGUIDs");
        RegCloseKey(key);
        if (rs != ERROR_SUCCESS && rs != ERROR_FILE_NOT_FOUND) return Fail(L"remove DeviceInterfaceGUIDs", static_cast<DWORD>(rs));
    }
    if (!SetupDiBuildDriverInfoList(device.set, &device.data, SPDIT_COMPATDRIVER)) return Fail(L"build compatible driver list", GetLastError());
    SP_DRVINFO_DATA_W first{};
    first.cbSize = sizeof first;
    BOOL reboot = FALSE;
    if (SetupDiEnumDriverInfoW(device.set, &device.data, SPDIT_COMPATDRIVER, 0, &first)) {
        // Null driver: install the best-ranked compatible driver (usbmidi2.inf for MI_03 on this system).
        if (!DiInstallDevice(nullptr, device.set, &device.data, nullptr, 0, &reboot)) return Fail(L"reinstall best driver", GetLastError());
        wprintf(L"PASS restore %ls%ls\n", target.name, reboot ? L" (reboot required)" : L"");
        return 0;
    }
    // No compatible driver exists (the runtime DFU interface and the DFU-mode device have none
    // in-box). Uninstall the devnode and rescan its parent so it returns without a driver.
    wprintf(L"No compatible driver for %ls; uninstalling the devnode and rescanning.\n", device.instance.c_str());
    DEVINST parent = 0;
    bool haveParent = CM_Get_Parent(&parent, device.data.DevInst, 0) == CR_SUCCESS;
    if (!DiUninstallDevice(nullptr, device.set, &device.data, 0, &reboot)) return Fail(L"uninstall devnode", GetLastError());
    if (haveParent) {
        CONFIGRET cr = CM_Reenumerate_DevNode(parent, CM_REENUMERATE_SYNCHRONOUS);
        if (cr != CR_SUCCESS) wprintf(L"  rescan of parent returned CONFIGRET %lu; unplug and replug the VCI-400.\n", cr);
    }
    wprintf(L"PASS restore %ls (driverless)%ls\n", target.name, reboot ? L" (reboot required)" : L"");
    return 0;
}

int Usage() {
    wprintf(L"usage: vci400-winusb-bind status|bind|restore [--interface MI_03|MI_05 | --dfu-mode]\n"
            L"  default target: --interface MI_03\n");
    return 1;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    const wchar_t* mode = argc >= 2 ? argv[1] : L"";
    bool status = !wcscmp(mode, L"status"), bind = !wcscmp(mode, L"bind"), restore = !wcscmp(mode, L"restore");
    if (!status && !bind && !restore) return Usage();

    const Target* target = nullptr;
    for (int i = 2; i < argc; ++i) {
        const Target* picked = nullptr;
        if (!wcscmp(argv[i], L"--interface") && i + 1 < argc) {
            ++i;
            if (!_wcsicmp(argv[i], L"MI_03")) picked = &kTargets[0];
            else if (!_wcsicmp(argv[i], L"MI_05")) picked = &kTargets[1];
            else {
                wprintf(L"Unknown interface %ls (expected MI_03 or MI_05).\n", argv[i]);
                return Usage();
            }
        } else if (!wcscmp(argv[i], L"--dfu-mode")) {
            picked = &kTargets[2];
        } else {
            wprintf(L"Unknown argument %ls.\n", argv[i]);
            return Usage();
        }
        if (target && target != picked) {
            wprintf(L"Choose one target.\n");
            return Usage();
        }
        target = picked;
    }
    if (!target) target = &kTargets[0];

    Device device;
    DWORD error = FindDevice(*target, device, true);
    if (error) {
        std::wstring step = std::wstring(L"find a single present ") + target->name + L" devnode (" + target->prefix + L"...)";
        return Fail(step.c_str(), error);
    }
    if (status) {
        PrintStatus(*target, device);
        return 0;
    }
    if (!IsElevated()) return Fail(L"bind/restore need an administrator terminal", ERROR_ACCESS_DENIED);
    std::wstring app = RunningMidiApp();
    if (!app.empty()) {
        wprintf(L"Close %ls before switching a VCI-400 driver.\n", app.c_str());
        return 1;
    }
    wprintf(L"Before:\n");
    PrintStatus(*target, device);
    int result = bind ? Bind(*target, device) : Restore(*target, device);
    Device after;
    if (FindDevice(*target, after, false) == ERROR_SUCCESS) {
        wprintf(L"After:\n");
        PrintStatus(*target, after);
    } else {
        wprintf(L"After: %ls not present yet; run status again in a few seconds.\n", target->name);
    }
    return result;
}
