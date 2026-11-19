// VCI400-FirmwareFix: guided wizard that installs MIDI-fixed audio firmware on a Vestax VCI-400
// (or restores its factory firmware) using only Microsoft's inbox WinUSB driver and standard
// USB DFU. Spec: README.md. Protocol: docs/technical.md.
//
// The embedded image is Vestax audio firmware v21 with one descriptor byte changed
// (MIDI AudioControl baInterfaceNr 1 -> 4) and its CRCs recomputed (fwpatch).

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <initguid.h>
#include <setupapi.h>
#include <newdev.h>
#include <cfgmgr32.h>
#include <devpkey.h>
#include <tlhelp32.h>
#include <combaseapi.h>
#include <shlobj.h>
#include <knownfolders.h>
#include <winusb.h>
#include <usbspec.h>
#include <winioctl.h>
#include <usbiodef.h>
#include <mmsystem.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <functional>
#include <string>
#include <vector>

#include "vci400_winusb.h"
#include "xmos_image.h"

namespace {

// ---- embedded firmware ------------------------------------------------------------------------
const uint8_t kFirmwareV21[] = {
#include "firmware_v21_midifix.inc"
};
const uint8_t kFirmwareD006[] = {
#include "firmware_d006_midifix.inc"
};
struct FirmwareImage {
    const wchar_t* name;
    const uint8_t* data;
    size_t size;
    const wchar_t* revision;  // bcdDevice after flashing
};
const FirmwareImage kV21{L"Vestax audio firmware v21 (21.00, for Serato DJ) with MIDI descriptor fix", kFirmwareV21, sizeof kFirmwareV21, L"REV_2100"};
const FirmwareImage kD006{L"Vestax audio firmware d006 (6.00, standard) with MIDI descriptor fix", kFirmwareD006, sizeof kFirmwareD006, L"REV_0600"};

// ---- constants -----------------------------------------------------------------------------------
constexpr wchar_t kComposite[] = L"USB\\VID_1AA1&PID_0231\\";
constexpr wchar_t kMi05[] = L"USB\\VID_1AA1&PID_0231&MI_05\\";
constexpr wchar_t kWinUsbHardwareId[] = L"USB\\MS_COMP_WINUSB";
constexpr UCHAR kToDevice = 0x21, kFromDevice = 0xA1;
constexpr UCHAR DFU_DETACH = 0, DFU_DNLOAD = 1, DFU_UPLOAD = 2, DFU_GETSTATUS = 3, DFU_CLRSTATUS = 4, DFU_GETSTATE = 5;
constexpr UCHAR XMOS_REVERTFACTORY = 0xF1, XMOS_RESETFROMDFU = 0xF3;
constexpr UCHAR STATE_DFU_IDLE = 0x02, STATE_DFU_DOWNLOAD_IDLE = 0x05, STATE_DFU_ERROR = 0x0A;
constexpr USHORT kBlock = 64;
constexpr USHORT kDetachMs = 40000;  // firmware stores ms*100000 ticks in 32 bits
#define VCI_IOCTL_USB_HUB_CYCLE_PORT CTL_CODE(FILE_DEVICE_USB, USB_HUB_CYCLE_PORT, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define VCI_IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION CTL_CODE(FILE_DEVICE_USB, USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION, METHOD_BUFFERED, FILE_ANY_ACCESS)

// ---- logging -------------------------------------------------------------------------------------
FILE* g_log = nullptr;
std::wstring g_folder;

void Say(const wchar_t* format, ...) {
    va_list args;
    va_start(args, format);
    wchar_t buffer[2048];
    _vsnwprintf_s(buffer, _TRUNCATE, format, args);
    va_end(args);
    fputws(buffer, stdout);
    if (g_log) {
        fputws(buffer, g_log);
        fflush(g_log);
    }
}

std::wstring ErrorText(DWORD error) {
    wchar_t* buffer = nullptr;
    DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                  nullptr, error, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring text = length ? std::wstring(buffer, length) : L"unknown error";
    if (buffer) LocalFree(buffer);
    while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r' || text.back() == L' ')) text.pop_back();
    return text;
}

std::wstring Upper(std::wstring text) {
    for (wchar_t& c : text) c = static_cast<wchar_t>(towupper(c));
    return text;
}

std::wstring Timestamp() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t b[32];
    swprintf_s(b, L"%04u%02u%02u-%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return b;
}

void Pause(const wchar_t* prompt) {
    Say(L"\n%ls\n> Press Enter to continue (Ctrl+C to quit) ", prompt);
    wchar_t line[64];
    fgetws(line, 64, stdin);
    if (g_log) fputws(L"[Enter]\n", g_log);
}

wchar_t Ask(const wchar_t* prompt, const wchar_t* choices) {
    for (;;) {
        Say(L"%ls ", prompt);
        wchar_t line[64]{};
        if (!fgetws(line, 64, stdin)) return 0;
        wchar_t c = static_cast<wchar_t>(towupper(line[0]));
        if (wcschr(choices, c) && c) {
            if (g_log) fwprintf(g_log, L"[%lc]\n", c);
            return c;
        }
    }
}

// ---- device discovery ------------------------------------------------------------------------------
struct Node {
    DEVINST devinst = 0;
    std::wstring id, revision;
    bool dfuMode = false;
    std::wstring service;
};

std::vector<std::wstring> MultiProperty(DEVINST dn, const DEVPROPKEY& key) {
    std::vector<std::wstring> out;
    ULONG size = 0;
    DEVPROPTYPE type = 0;
    if (CM_Get_DevNode_PropertyW(dn, &key, &type, nullptr, &size, 0) != CR_BUFFER_SMALL || size == 0) return out;
    std::vector<BYTE> buffer(size);
    if (CM_Get_DevNode_PropertyW(dn, &key, &type, buffer.data(), &size, 0) != CR_SUCCESS || type != DEVPROP_TYPE_STRING_LIST) return out;
    for (const wchar_t* p = reinterpret_cast<const wchar_t*>(buffer.data()); *p; p += wcslen(p) + 1) out.emplace_back(p);
    return out;
}

std::wstring StringProperty(DEVINST dn, const DEVPROPKEY& key) {
    wchar_t buffer[512]{};
    ULONG size = sizeof buffer - sizeof(wchar_t);
    DEVPROPTYPE type = 0;
    if (CM_Get_DevNode_PropertyW(dn, &key, &type, reinterpret_cast<PBYTE>(buffer), &size, 0) != CR_SUCCESS) return L"";
    return buffer;
}

// The whole device: runtime composite parent or DFU-mode device (same instance ID).
std::vector<Node> FindWholeDevices() {
    std::vector<Node> nodes;
    ULONG length = 0;
    if (CM_Get_Device_ID_List_SizeW(&length, L"USB", CM_GETIDLIST_FILTER_ENUMERATOR | CM_GETIDLIST_FILTER_PRESENT) != CR_SUCCESS) return nodes;
    std::vector<wchar_t> list(length);
    if (CM_Get_Device_ID_ListW(L"USB", list.data(), length, CM_GETIDLIST_FILTER_ENUMERATOR | CM_GETIDLIST_FILTER_PRESENT) != CR_SUCCESS) return nodes;
    for (const wchar_t* p = list.data(); *p; p += wcslen(p) + 1) {
        std::wstring id = Upper(p);
        if (id.rfind(kComposite, 0) != 0) continue;
        DEVINST dn = 0;
        if (CM_Locate_DevNodeW(&dn, const_cast<wchar_t*>(p), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) continue;
        Node node{dn, p, L"", false, StringProperty(dn, DEVPKEY_Device_Service)};
        for (const auto& hw : MultiProperty(dn, DEVPKEY_Device_HardwareIds)) {
            size_t at = Upper(hw).find(L"&REV_");
            if (at != std::wstring::npos) node.revision = Upper(hw).substr(at + 1, 8);
        }
        for (const auto& compat : MultiProperty(dn, DEVPKEY_Device_CompatibleIds))
            if (Upper(compat).find(L"CLASS_FE&SUBCLASS_01&PROT_02") != std::wstring::npos) node.dfuMode = true;
        nodes.push_back(node);
    }
    return nodes;
}

bool FindOne(Node& node) {
    auto nodes = FindWholeDevices();
    if (nodes.size() != 1) return false;
    node = nodes[0];
    return true;
}

bool WaitFor(const std::function<bool()>& ready, DWORD timeoutMs, const wchar_t* what) {
    Say(L"  waiting for %ls", what);
    for (DWORD waited = 0; waited <= timeoutMs; waited += 500) {
        if (ready()) {
            Say(L" ok\n");
            return true;
        }
        Sleep(500);
        if (waited % 2000 == 0) Say(L".");
    }
    Say(L" timed out\n");
    return false;
}

// ---- live descriptor (hub IOCTL, standard GET_DESCRIPTOR only) ----------------------------------
// Returns baInterfaceNr[0] of the MIDI AudioControl header (interface 3), or -1.
int ReadMidiHeaderInterface(const Node& node) {
    DEVINST hub = 0;
    if (CM_Get_Parent(&hub, node.devinst, 0) != CR_SUCCESS) return -1;
    ULONG port = 0, size = sizeof port;
    DEVPROPTYPE type = 0;
    if (CM_Get_DevNode_PropertyW(node.devinst, &DEVPKEY_Device_Address, &type, reinterpret_cast<PBYTE>(&port), &size, 0) != CR_SUCCESS) return -1;
    wchar_t hubId[MAX_DEVICE_ID_LEN];
    if (CM_Get_Device_IDW(hub, hubId, MAX_DEVICE_ID_LEN, 0) != CR_SUCCESS) return -1;
    ULONG length = 0;
    if (CM_Get_Device_Interface_List_SizeW(&length, const_cast<LPGUID>(&GUID_DEVINTERFACE_USB_HUB), hubId, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS || length < 2) return -1;
    std::vector<wchar_t> list(length);
    if (CM_Get_Device_Interface_ListW(const_cast<LPGUID>(&GUID_DEVINTERFACE_USB_HUB), hubId, list.data(), length, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS) return -1;
    HANDLE h = CreateFileW(list.data(), GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return -1;
    auto get = [&](USHORT total, std::vector<uint8_t>& out) {
        std::vector<uint8_t> buffer(12 + total, 0);
        memcpy(buffer.data(), &port, 4);
        uint8_t setup[8] = {0x80, 6, 0, 2, 0, 0, static_cast<uint8_t>(total & 255), static_cast<uint8_t>(total >> 8)};
        memcpy(buffer.data() + 4, setup, 8);
        DWORD returned = 0;
        if (!DeviceIoControl(h, VCI_IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION, buffer.data(), static_cast<DWORD>(buffer.size()),
                             buffer.data(), static_cast<DWORD>(buffer.size()), &returned, nullptr) || returned < 12 + 4)
            return false;
        out.assign(buffer.begin() + 12, buffer.begin() + returned);
        return true;
    };
    std::vector<uint8_t> head, config;
    int result = -1;
    if (get(9, head) && head.size() >= 4) {
        USHORT total = static_cast<USHORT>(head[2] | (head[3] << 8));
        if (total >= 9 && get(total, config)) {
            int iface = -1, cls = -1, sub = -1;
            for (size_t o = 0; o + 2 <= config.size() && config[o] >= 2 && o + config[o] <= config.size(); o += config[o]) {
                const uint8_t* d = &config[o];
                if (d[1] == 4 && d[0] >= 9) { iface = d[2]; cls = d[5]; sub = d[6]; }
                else if (d[1] == 0x24 && d[0] >= 9 && d[2] == 1 && cls == 1 && sub == 1 && iface == 3 && d[3] == 0x00 && d[4] == 0x01)
                    result = d[8];
            }
        }
    }
    CloseHandle(h);
    return result;
}

// ---- WinUSB binding (inbox winusb.inf, generic "WinUsb Device" model) -----------------------------
DWORD BindWinUsb(DEVINST target, const wchar_t* guidText) {
    wchar_t id[MAX_DEVICE_ID_LEN];
    if (CM_Get_Device_IDW(target, id, MAX_DEVICE_ID_LEN, 0) != CR_SUCCESS) return ERROR_NO_SUCH_DEVINST;
    HDEVINFO set = SetupDiCreateDeviceInfoList(nullptr, nullptr);
    if (set == INVALID_HANDLE_VALUE) return GetLastError();
    SP_DEVINFO_DATA data{};
    data.cbSize = sizeof data;
    DWORD error = ERROR_SUCCESS;
    do {
        if (!SetupDiOpenDeviceInfoW(set, id, nullptr, 0, &data)) { error = GetLastError(); break; }
        HKEY key = SetupDiCreateDevRegKeyW(set, &data, DICS_FLAG_GLOBAL, 0, DIREG_DEV, nullptr, nullptr);
        if (key == INVALID_HANDLE_VALUE) { error = GetLastError(); break; }
        std::wstring multi = std::wstring(guidText) + L'\0' + L'\0';
        LSTATUS rs = RegSetValueExW(key, L"DeviceInterfaceGUIDs", 0, REG_MULTI_SZ, reinterpret_cast<const BYTE*>(multi.data()),
                                    static_cast<DWORD>(multi.size() * sizeof(wchar_t)));
        RegCloseKey(key);
        if (rs != ERROR_SUCCESS) { error = static_cast<DWORD>(rs); break; }
        wchar_t inf[MAX_PATH];
        GetWindowsDirectoryW(inf, MAX_PATH);
        wcscat_s(inf, L"\\INF\\winusb.inf");
        GUID classGuid{};
        wchar_t className[MAX_CLASS_NAME_LEN], classText[40];
        if (!SetupDiGetINFClassW(inf, &classGuid, className, MAX_CLASS_NAME_LEN, nullptr) || !StringFromGUID2(classGuid, classText, 40)) { error = GetLastError(); break; }
        if (!SetupDiSetDeviceRegistryPropertyW(set, &data, SPDRP_CLASSGUID, reinterpret_cast<const BYTE*>(classText),
                                               static_cast<DWORD>((wcslen(classText) + 1) * sizeof(wchar_t)))) { error = GetLastError(); break; }
        SP_DEVINSTALL_PARAMS_W params{};
        params.cbSize = sizeof params;
        if (!SetupDiGetDeviceInstallParamsW(set, &data, &params)) { error = GetLastError(); break; }
        params.Flags |= DI_ENUMSINGLEINF;
        params.FlagsEx |= DI_FLAGSEX_ALLOWEXCLUDEDDRVS;
        wcscpy_s(params.DriverPath, inf);
        if (!SetupDiSetDeviceInstallParamsW(set, &data, &params) || !SetupDiBuildDriverInfoList(set, &data, SPDIT_CLASSDRIVER)) { error = GetLastError(); break; }
        SP_DRVINFO_DATA_W chosen{};
        bool found = false;
        for (DWORD i = 0; !found; ++i) {
            SP_DRVINFO_DATA_W driver{};
            driver.cbSize = sizeof driver;
            if (!SetupDiEnumDriverInfoW(set, &data, SPDIT_CLASSDRIVER, i, &driver)) break;
            std::vector<BYTE> buffer(8192);
            auto* detail = reinterpret_cast<SP_DRVINFO_DETAIL_DATA_W*>(buffer.data());
            detail->cbSize = sizeof(SP_DRVINFO_DETAIL_DATA_W);
            if (SetupDiGetDriverInfoDetailW(set, &data, &driver, detail, static_cast<DWORD>(buffer.size()), nullptr) &&
                _wcsicmp(detail->HardwareID, kWinUsbHardwareId) == 0) { chosen = driver; found = true; }
        }
        if (!found) { error = ERROR_NOT_FOUND; break; }
        BOOL reboot = FALSE;
        if (!SetupDiSetSelectedDriverW(set, &data, &chosen) || !DiInstallDevice(nullptr, set, &data, &chosen, 0, &reboot)) { error = GetLastError(); break; }
    } while (false);
    SetupDiDestroyDeviceInfoList(set);
    return error;
}

// Uninstalls the VCI-400 device tree and rescans, so Windows reapplies its inbox drivers.
DWORD ResetDeviceDrivers() {
    Node node;
    if (!FindOne(node)) return ERROR_NO_SUCH_DEVINST;
    DEVINST hub = 0;
    CM_Get_Parent(&hub, node.devinst, 0);
    HDEVINFO set = SetupDiCreateDeviceInfoList(nullptr, nullptr);
    SP_DEVINFO_DATA data{};
    data.cbSize = sizeof data;
    DWORD error = ERROR_SUCCESS;
    BOOL reboot = FALSE;
    if (!SetupDiOpenDeviceInfoW(set, node.id.c_str(), nullptr, 0, &data) || !DiUninstallDevice(nullptr, set, &data, 0, &reboot)) error = GetLastError();
    SetupDiDestroyDeviceInfoList(set);
    if (hub) CM_Reenumerate_DevNode(hub, CM_REENUMERATE_SYNCHRONOUS);
    return error;
}

void RestartMidiService() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return;
    SC_HANDLE svc = OpenServiceW(scm, L"midisrv", SERVICE_STOP | SERVICE_START | SERVICE_QUERY_STATUS);
    if (svc) {
        SERVICE_STATUS status{};
        ControlService(svc, SERVICE_CONTROL_STOP, &status);
        for (int i = 0; i < 40; ++i) {
            if (QueryServiceStatus(svc, &status) && status.dwCurrentState == SERVICE_STOPPED) break;
            Sleep(250);
        }
        StartServiceW(svc, 0, nullptr);
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
}

// ---- DFU over WinUSB ---------------------------------------------------------------------------------
std::vector<std::wstring> InterfacePaths(const GUID& guid) {
    std::vector<std::wstring> paths;
    ULONG length = 0;
    if (CM_Get_Device_Interface_List_SizeW(&length, const_cast<LPGUID>(&guid), nullptr, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS || length < 2) return paths;
    std::vector<wchar_t> list(length);
    if (CM_Get_Device_Interface_ListW(const_cast<LPGUID>(&guid), nullptr, list.data(), length, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS) return paths;
    for (const wchar_t* p = list.data(); *p; p += wcslen(p) + 1) paths.emplace_back(p);
    return paths;
}

struct Dfu {
    HANDLE file = INVALID_HANDLE_VALUE;
    WINUSB_INTERFACE_HANDLE handle = nullptr;
    ~Dfu() { Close(); }
    void Close() {
        if (handle) WinUsb_Free(handle);
        handle = nullptr;
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        file = INVALID_HANDLE_VALUE;
    }
    // runtime: MI_05 GUID; DFU mode: DFU-mode GUID. The DFU interface is the first interface.
    DWORD Open(const GUID& guid) {
        Close();
        auto paths = InterfacePaths(guid);
        if (paths.empty()) return ERROR_DEVICE_NOT_CONNECTED;
        file = CreateFileW(paths[0].c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
        if (file == INVALID_HANDLE_VALUE) return GetLastError();
        if (!WinUsb_Initialize(file, &handle)) { DWORD e = GetLastError(); Close(); return e; }
        SetTimeout(5000);
        return ERROR_SUCCESS;
    }
    void SetTimeout(ULONG ms) { WinUsb_SetPipePolicy(handle, 0, PIPE_TRANSFER_TIMEOUT, sizeof ms, &ms); }
    UCHAR InterfaceNumber() {
        USB_INTERFACE_DESCRIPTOR d{};
        return WinUsb_QueryInterfaceSettings(handle, 0, &d) ? d.bInterfaceNumber : 0;
    }
    DWORD Control(UCHAR type, UCHAR request, USHORT value, PUCHAR data, USHORT length, ULONG* got = nullptr) {
        WINUSB_SETUP_PACKET setup{type, request, value, InterfaceNumber(), length};
        ULONG transferred = 0;
        BOOL ok = WinUsb_ControlTransfer(handle, setup, data, length, &transferred, nullptr);
        if (got) *got = transferred;
        return ok ? ERROR_SUCCESS : GetLastError();
    }
    DWORD State(UCHAR& s) { return Control(kFromDevice, DFU_GETSTATE, 0, &s, 1); }
    DWORD Status(UCHAR& status, UCHAR& state) {
        UCHAR b[6]{};
        DWORD e = Control(kFromDevice, DFU_GETSTATUS, 0, b, 6);
        status = b[0];
        state = b[4];
        return e;
    }
};

DWORD CyclePort(DEVINST device) {
    DEVINST hub = 0;
    if (CM_Get_Parent(&hub, device, 0) != CR_SUCCESS) return ERROR_NO_SUCH_DEVINST;
    ULONG port = 0, size = sizeof port;
    DEVPROPTYPE type = 0;
    if (CM_Get_DevNode_PropertyW(device, &DEVPKEY_Device_Address, &type, reinterpret_cast<PBYTE>(&port), &size, 0) != CR_SUCCESS || !port) return ERROR_INVALID_DATA;
    wchar_t hubId[MAX_DEVICE_ID_LEN];
    if (CM_Get_Device_IDW(hub, hubId, MAX_DEVICE_ID_LEN, 0) != CR_SUCCESS) return ERROR_NO_SUCH_DEVINST;
    ULONG length = 0;
    if (CM_Get_Device_Interface_List_SizeW(&length, const_cast<LPGUID>(&GUID_DEVINTERFACE_USB_HUB), hubId, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS || length < 2) return ERROR_NOT_FOUND;
    std::vector<wchar_t> list(length);
    if (CM_Get_Device_Interface_ListW(const_cast<LPGUID>(&GUID_DEVINTERFACE_USB_HUB), hubId, list.data(), length, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS) return ERROR_NOT_FOUND;
    HANDLE h = CreateFileW(list.data(), GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return GetLastError();
#pragma pack(push, 1)
    struct { ULONG ConnectionIndex; ULONG StatusReturned; } params{port, 0};
#pragma pack(pop)
    DWORD returned = 0;
    BOOL ok = DeviceIoControl(h, VCI_IOCTL_USB_HUB_CYCLE_PORT, &params, sizeof params, &params, sizeof params, &returned, nullptr);
    DWORD error = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(h);
    return error;
}

// ---- wizard steps ----------------------------------------------------------------------------------
bool Abort(const wchar_t* why, DWORD error = 0) {
    if (error) Say(L"\nSTOPPED: %ls (%lu: %ls)\n", why, error, ErrorText(error).c_str());
    else Say(L"\nSTOPPED: %ls\n", why);
    Say(L"Nothing further was written. Power-cycle the VCI-400; it will start normally. Log: %ls\n", g_folder.c_str());
    return false;
}

bool NoDjAppsRunning() {
    const wchar_t* names[] = {L"Serato DJ Pro.exe", L"Serato DJ Lite.exe", L"Mixxx.exe", L"VirtualDJ.exe", L"Traktor.exe", L"rekordbox.exe", L"vci400-bridge.exe"};
    for (;;) {
        std::wstring found;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        PROCESSENTRY32W e{};
        e.dwSize = sizeof e;
        for (BOOL ok = Process32FirstW(snap, &e); ok && found.empty(); ok = Process32NextW(snap, &e))
            for (auto n : names)
                if (!_wcsicmp(e.szExeFile, n)) found = n;
        CloseHandle(snap);
        if (found.empty()) return true;
        Pause((L"Please close " + found + L" first.").c_str());
    }
}

// Finds MI_05 under the composite device and makes sure it is on WinUSB.
bool PrepareRuntimeDfu(const Node& node) {
    DEVINST mi05 = 0;
    for (DEVINST c = 0; CM_Get_Child(&c, node.devinst, 0) == CR_SUCCESS;) {
        for (;;) {
            wchar_t id[MAX_DEVICE_ID_LEN];
            if (CM_Get_Device_IDW(c, id, MAX_DEVICE_ID_LEN, 0) == CR_SUCCESS && Upper(id).rfind(kMi05, 0) == 0) mi05 = c;
            DEVINST next = 0;
            if (CM_Get_Sibling(&next, c, 0) != CR_SUCCESS) break;
            c = next;
        }
        break;
    }
    if (!mi05) return Abort(L"the VCI-400's update interface (MI_05) was not found");
    if (_wcsicmp(StringProperty(mi05, DEVPKEY_Device_Service).c_str(), L"WinUSB") != 0 || InterfacePaths(kVci400WinUsbDfuRuntimeGuid).empty()) {
        DWORD e = BindWinUsb(mi05, kVci400WinUsbDfuRuntimeGuidText);
        if (e) return Abort(L"could not bind WinUSB to the update interface", e);
        if (!WaitFor([] { return !InterfacePaths(kVci400WinUsbDfuRuntimeGuid).empty(); }, 15000, L"update interface")) return Abort(L"update interface did not appear");
    }
    return true;
}

bool WaitForRuntime(const wchar_t* prompt);

// Firmware v21 hangs when asked to reboot into update mode. Its stored copy can still be erased
// from normal mode (XMOS 0xF1; the request reports a stall but the erase happens), after which a
// power-cycle starts the factory firmware, which enters update mode normally.
bool LeaveV21IfNeeded() {
    Node node;
    if (!FindOne(node) || node.dfuMode || node.revision != L"REV_2100") return true;
    Say(L"Firmware v21 can't switch into update mode directly, so its stored copy is erased first\n"
        L"(the factory firmware stays). The VCI-400 then restarts on its factory firmware.\n");
    if (!PrepareRuntimeDfu(node)) return false;
    Dfu dfu;
    DWORD e = ERROR_GEN_FAILURE;
    for (int attempt = 1; attempt <= 3 && e; ++attempt) {
        Sleep(1500);
        e = dfu.Open(kVci400WinUsbDfuRuntimeGuid);
        if (!e) {
            dfu.SetTimeout(30000);
            e = dfu.Control(kToDevice, XMOS_REVERTFACTORY, 0, nullptr, 0);
            if (e == ERROR_GEN_FAILURE) e = ERROR_SUCCESS;  // v21 stalls the status stage after erasing
        }
        dfu.Close();
    }
    if (e) return Abort(L"could not send the erase request", e);
    Sleep(2000);
    if (!WaitForRuntime(L"Switch the VCI-400 OFF and ON again (keep USB connected).")) return false;
    if (!FindOne(node) || node.revision == L"REV_2100") return Abort(L"the VCI-400 still runs v21; its stored copy was not erased");
    Say(L"Now running firmware %ls.\n", node.revision.c_str());
    return true;
}

// Runtime -> DFU mode with WinUSB bound to the DFU-mode device, state dfuIDLE.
bool EnterDfuMode(Dfu& dfu) {
    Node node;
    if (!FindOne(node)) return Abort(L"VCI-400 not found (connect exactly one, with its power adaptor on)");
    if (!node.dfuMode) {
        Say(L"[1/3] Preparing the update interface (Microsoft WinUSB)...\n");
        DEVINST mi05 = 0;
        std::wstring mi05Id;
        for (DEVINST child = 0; CM_Get_Child(&child, node.devinst, 0) == CR_SUCCESS;) {
            for (DEVINST c = child;;) {
                wchar_t id[MAX_DEVICE_ID_LEN];
                if (CM_Get_Device_IDW(c, id, MAX_DEVICE_ID_LEN, 0) == CR_SUCCESS && Upper(id).rfind(kMi05, 0) == 0) { mi05 = c; mi05Id = id; }
                DEVINST next = 0;
                if (CM_Get_Sibling(&next, c, 0) != CR_SUCCESS) break;
                c = next;
            }
            break;
        }
        if (!mi05) return Abort(L"the VCI-400's update interface (MI_05) was not found");
        if (_wcsicmp(StringProperty(mi05, DEVPKEY_Device_Service).c_str(), L"WinUSB") != 0 || InterfacePaths(kVci400WinUsbDfuRuntimeGuid).empty()) {
            DWORD e = BindWinUsb(mi05, kVci400WinUsbDfuRuntimeGuidText);
            if (e) return Abort(L"could not bind WinUSB to the update interface", e);
            if (!WaitFor([] { return !InterfacePaths(kVci400WinUsbDfuRuntimeGuid).empty(); }, 15000, L"update interface")) return Abort(L"update interface did not appear");
        }
        Say(L"[2/3] Switching the VCI-400 into update mode...\n");
        // A freshly (re)bound interface can refuse the first request; retry before giving up.
        DWORD e = ERROR_GEN_FAILURE;
        for (int attempt = 1; attempt <= 4 && e; ++attempt) {
            Sleep(attempt == 1 ? 1500 : 3000);
            if ((e = dfu.Open(kVci400WinUsbDfuRuntimeGuid)) == ERROR_SUCCESS) e = dfu.Control(kToDevice, DFU_DETACH, kDetachMs, nullptr, 0);
            if (e) {
                Say(L"  attempt %d: %ls\n", attempt, ErrorText(e).c_str());
                dfu.Close();
            }
        }
        if (e) return Abort(L"DFU_DETACH was refused; switch the VCI-400 off and on, then run this again", e);
        dfu.Close();
        if ((e = CyclePort(node.devinst))) return Abort(L"could not reset the USB port", e);
        if (!WaitFor([] { Node n; return FindOne(n) && n.dfuMode; }, 20000, L"update mode")) return Abort(L"the VCI-400 did not enter update mode");
        if (!FindOne(node)) return Abort(L"lost the device");
    }
    Say(L"[3/3] Connecting to update mode...\n");
    if (InterfacePaths(kVci400WinUsbDfuModeGuid).empty()) {
        DWORD e = BindWinUsb(node.devinst, kVci400WinUsbDfuModeGuidText);
        if (e) return Abort(L"could not bind WinUSB in update mode", e);
        if (!WaitFor([] { return !InterfacePaths(kVci400WinUsbDfuModeGuid).empty(); }, 20000, L"update-mode interface")) return Abort(L"update-mode interface did not appear");
    }
    DWORD e = dfu.Open(kVci400WinUsbDfuModeGuid);
    if (e) return Abort(L"could not open update mode", e);
    UCHAR state = 0;
    if ((e = dfu.State(state))) return Abort(L"update mode did not answer", e);
    if (state == STATE_DFU_ERROR) { dfu.Control(kToDevice, DFU_CLRSTATUS, 0, nullptr, 0); dfu.State(state); }
    if (state != STATE_DFU_IDLE) return Abort(L"update mode is not idle");
    Say(L"  in update mode (dfuIDLE)\n");
    return true;
}

bool UploadSlot(Dfu& dfu, std::vector<uint8_t>& image) {
    image.clear();
    for (USHORT block = 0; block < 0xFFFF; ++block) {
        UCHAR b[kBlock];
        ULONG got = 0;
        DWORD e = dfu.Control(kFromDevice, DFU_UPLOAD, block, b, kBlock, &got);
        if (e) return Abort(L"reading the stored firmware failed", e);
        if (!got) break;
        image.insert(image.end(), b, b + got);
    }
    return true;
}

bool WaitForRuntime(const wchar_t* prompt) {
    Pause(prompt);
    if (!WaitFor([] { Node n; return FindOne(n) && !n.dfuMode; }, 60000, L"the VCI-400")) return Abort(L"the VCI-400 did not come back");
    Sleep(2000);
    return true;
}

bool Revert(Dfu& dfu) {
    Say(L"Erasing the stored update (factory firmware stays)...\n");
    dfu.SetTimeout(30000);
    DWORD e = dfu.Control(kToDevice, XMOS_REVERTFACTORY, 0, nullptr, 0);
    if (e) return Abort(L"the firmware refused the revert request", e);
    Sleep(2000);
    dfu.Control(kToDevice, XMOS_RESETFROMDFU, 0, nullptr, 0);
    dfu.Close();
    return WaitForRuntime(L"Switch the VCI-400 OFF and ON again (keep USB connected).");
}

bool Cleanup() {
    Say(L"\nRestoring Windows' own drivers for the VCI-400...\n");
    DWORD e = ResetDeviceDrivers();
    if (e) Say(L"  driver reset returned %lu (%ls); unplug and replug the VCI-400 if audio/MIDI are missing\n", e, ErrorText(e).c_str());
    WaitFor([] { Node n; return FindOne(n) && !_wcsicmp(n.service.c_str(), L"usbccgp"); }, 30000, L"Windows drivers");
    Sleep(3000);
    Say(L"Restarting the Windows MIDI service...\n");
    RestartMidiService();
    Sleep(3000);
    UINT ins = midiInGetNumDevs();
    bool port = false;
    for (UINT i = 0; i < ins; ++i) {
        MIDIINCAPSW caps{};
        if (midiInGetDevCapsW(i, &caps, sizeof caps) == MMSYSERR_NOERROR && wcsstr(caps.szPname, L"VCI-400")) {
            Say(L"  MIDI input found: %ls\n", caps.szPname);
            port = true;
        }
    }
    if (!port) Say(L"  No VCI-400 MIDI port yet; unplug and replug the VCI-400 once.\n");
    return true;
}

bool Install(const FirmwareImage& image) {
    Node node;
    if (!FindOne(node)) return Abort(L"VCI-400 not found (connect exactly one, with its power adaptor on)");
    if (!node.dfuMode) {
        int midi = ReadMidiHeaderInterface(node);
        Say(L"Connected: %ls, firmware %ls, MIDI header points at interface %d %ls\n", node.id.c_str(), node.revision.c_str(), midi,
            midi == 4 ? L"(already fixed)" : midi == 1 ? L"(needs the fix)" : L"");
        if (midi == 4 && node.revision == image.revision &&
            Ask(L"This firmware is already installed. Install it again? [Y/N]", L"YN") != L'Y')
            return true;
    }
    xmos_image::Image img;
    std::vector<uint8_t> fw(image.data, image.data + image.size);
    if (!xmos_image::FromBytes(fw, img) || !xmos_image::Verify(img).All()) return Abort(L"the embedded firmware failed its integrity check");
    Pause(L"Keep the VCI-400's power adaptor connected and do not unplug it until told to.");
    if (!LeaveV21IfNeeded()) return false;

    Dfu dfu;
    if (!EnterDfuMode(dfu)) return false;
    std::vector<uint8_t> slot;
    if (!UploadSlot(dfu, slot)) return false;
    if (!slot.empty()) {
        std::wstring path = g_folder + L"\\backup-upgrade-slot-" + Timestamp() + L".bin";
        FILE* f = nullptr;
        if (!_wfopen_s(&f, path.c_str(), L"wb") && f) { fwrite(slot.data(), 1, slot.size(), f); fclose(f); }
        Say(L"A previous update is stored (%zu bytes); saved to %ls\n", slot.size(), path.c_str());
        if (!Revert(dfu) || !EnterDfuMode(dfu) || !UploadSlot(dfu, slot)) return false;
        if (!slot.empty()) return Abort(L"this VCI-400 could not erase its stored update, so flashing was not attempted");
    } else {
        Say(L"No stored update: the VCI-400 runs its factory firmware.\n");
    }

    Say(L"\nInstalling %ls...\n", image.name);
    size_t length = (fw.size() + kBlock - 1) / kBlock * kBlock;
    fw.resize(length, 0);
    UCHAR state = 0;
    for (size_t block = 0; block < length / kBlock; ++block) {
        dfu.SetTimeout(block == 0 ? 30000 : 10000);
        DWORD e = dfu.Control(kToDevice, DFU_DNLOAD, static_cast<USHORT>(block), &fw[block * kBlock], kBlock);
        UCHAR status = 0;
        if (!e) e = dfu.Status(status, state);
        if (e || status || state != STATE_DFU_DOWNLOAD_IDLE) {
            Say(L"\n");
            return Abort(L"writing failed part-way; the VCI-400 will start its factory firmware", e ? e : ERROR_GEN_FAILURE);
        }
        size_t done = (block + 1) * 100 / (length / kBlock);
        // Progress goes to the console only; the log gets a line per 25%.
        wprintf(L"\r  [%-50.*ls] %3zu%%", static_cast<int>(done / 2), L"##################################################", done);
        if (g_log && (block + 1) % (length / kBlock / 4) == 0) fwprintf(g_log, L"  %zu%% written\n", done);
    }
    Say(L"\n");
    UCHAR status = 0;
    DWORD e = dfu.Control(kToDevice, DFU_DNLOAD, 0, nullptr, 0);
    if (!e) e = dfu.Status(status, state);
    if (e || status) return Abort(L"finishing the write failed", e ? e : ERROR_GEN_FAILURE);
    dfu.Control(kToDevice, XMOS_RESETFROMDFU, 0, nullptr, 0);
    dfu.Close();
    Say(L"Firmware written.\n");
    if (!WaitForRuntime(L"Switch the VCI-400 OFF and ON again (keep USB connected).")) return false;

    if (!FindOne(node)) return Abort(L"lost the device");
    int midi = ReadMidiHeaderInterface(node);
    Say(L"Now running firmware %ls, MIDI header points at interface %d\n", node.revision.c_str(), midi);
    bool ok = node.revision == image.revision && midi == 4;
    Cleanup();
    Say(ok ? L"\nDONE: the MIDI-fixed firmware is installed.\n" : L"\nWARNING: the new firmware did not report as expected; see the log.\n");
    if (ok && &image == &kV21)
        Say(L"\nIMPORTANT: do NOT run Vestax's panel/MIDI firmware updater (FirmUp.exe) while this firmware is\n"
            L"installed. A panel update attempted on v21 erased the panel firmware on our test unit and stopped\n"
            L"this tool from reaching update mode until it was repaired. To update the panel firmware, first run\n"
            L"this tool with option 2 (d006), update the panel, then come back to option 1.\n");
    return ok;
}

bool RestoreFactory() {
    Pause(L"This erases the stored update so the VCI-400 runs its original factory firmware.");
    Node current;
    if (FindOne(current) && !current.dfuMode && current.revision == L"REV_2100") {
        if (!LeaveV21IfNeeded()) return false;
        Cleanup();
        Say(L"\nDONE: factory firmware restored.\n");
        return true;
    }
    Dfu dfu;
    if (!EnterDfuMode(dfu)) return false;
    std::vector<uint8_t> slot;
    if (!UploadSlot(dfu, slot)) return false;
    if (slot.empty()) {
        Say(L"No stored update: the VCI-400 already runs its factory firmware.\n");
        dfu.Control(kToDevice, XMOS_RESETFROMDFU, 0, nullptr, 0);
        dfu.Close();
        if (!WaitForRuntime(L"Switch the VCI-400 OFF and ON again (keep USB connected).")) return false;
    } else {
        std::wstring path = g_folder + L"\\backup-upgrade-slot-" + Timestamp() + L".bin";
        FILE* f = nullptr;
        if (!_wfopen_s(&f, path.c_str(), L"wb") && f) { fwrite(slot.data(), 1, slot.size(), f); fclose(f); }
        Say(L"Stored update saved to %ls\n", path.c_str());
        if (!Revert(dfu)) return false;
    }
    Node node;
    if (FindOne(node)) Say(L"Now running firmware %ls\n", node.revision.c_str());
    Cleanup();
    Say(L"\nDONE: factory firmware restored.\n");
    return true;
}

}  // namespace

int wmain() {
    SetConsoleOutputCP(CP_UTF8);
    PWSTR documents = nullptr;
    if (SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &documents) == S_OK) {
        g_folder = std::wstring(documents) + L"\\VCI-400 Firmware Fix";
        CoTaskMemFree(documents);
        CreateDirectoryW(g_folder.c_str(), nullptr);
        _wfopen_s(&g_log, (g_folder + L"\\log-" + Timestamp() + L".txt").c_str(), L"w, ccs=UTF-8");
    }
    Say(L"VCI-400 Firmware Fix\n====================\n"
        L"Fixes the VCI-400's USB MIDI so Windows 11 and Serato DJ see it without the old Vestax driver.\n"
        L"Uses only Windows' built-in drivers. Your VCI-400's factory firmware is never overwritten.\n\n"
        L"  1. Install %ls\n"
        L"  2. Install %ls\n"
        L"     (load this before using Vestax's panel/MIDI updater FirmUp.exe - never run FirmUp on option 1)\n"
        L"  3. Restore factory firmware (undo)\n  4. Quit\n\n", kV21.name, kD006.name);
    wchar_t choice = Ask(L"Choose 1, 2, 3 or 4:", L"1234");
    if (choice == L'4' || !choice) return 0;
    if (!NoDjAppsRunning()) return 1;
    bool ok = choice == L'1' ? Install(kV21) : choice == L'2' ? Install(kD006) : RestoreFactory();
    Say(L"\nLog and backups: %ls\n", g_folder.c_str());
    Pause(L"Finished.");
    if (g_log) fclose(g_log);
    return ok ? 0 : 1;
}
