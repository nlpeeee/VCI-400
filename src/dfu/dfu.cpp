// vci400-dfu: XMOS USB-audio DFU for the Vestax VCI-400 over Microsoft's inbox WinUSB.
//
// Protocol: XMOS sc_usb_audio module_dfu (dfu.xc, dfu_types.h, flash_interface.c) and the XMOS
// host tool xmosdfu.cpp; see docs/technical.md. Requires the DFU interface bound
// to WinUSB with vci400-winusb-bind (runtime MI_05, or the DFU-mode device).
//
//   vci400-dfu list
//   vci400-dfu status
//   vci400-dfu enter-dfu                       runtime only: XMOS 0xF2, reboot into DFU mode
//   vci400-dfu upload <out.bin>                DFU mode: read the upgrade image (0 bytes = none)
//   vci400-dfu download <image.bin> --confirm  DFU mode: write a CRC-verified image, then reboot
//   vci400-dfu revert-factory --confirm        DFU mode: XMOS 0xF1 (erase upgrade images), reboot
//   vci400-dfu leave-dfu                       DFU mode: XMOS 0xF3, reboot into the application
//
// Never touches any interface other than the DFU interface, and never writes the factory image
// (the device firmware only writes upgrade images).

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <initguid.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <devpkey.h>
#include <winusb.h>
#include <usbspec.h>
#include <winioctl.h>
#include <usbiodef.h>

// From usbioctl.h, which cannot be included next to winusb.h (its LowSpeed macro clashes).
#define VCI_IOCTL_USB_HUB_CYCLE_PORT CTL_CODE(FILE_DEVICE_USB, USB_HUB_CYCLE_PORT, METHOD_BUFFERED, FILE_ANY_ACCESS)
#pragma pack(push, 1)
struct VciCyclePortParams {
    ULONG ConnectionIndex;
    ULONG StatusReturned;
};
#pragma pack(pop)

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "vci400_winusb.h"
#include "xmos_image.h"

namespace {

constexpr UCHAR kToDevice = 0x21, kFromDevice = 0xA1;
constexpr UCHAR DFU_DETACH = 0;
constexpr UCHAR DFU_DNLOAD = 1, DFU_UPLOAD = 2, DFU_GETSTATUS = 3, DFU_CLRSTATUS = 4, DFU_GETSTATE = 5;
constexpr UCHAR XMOS_REVERTFACTORY = 0xF1, XMOS_RESETINTODFU = 0xF2, XMOS_RESETFROMDFU = 0xF3;
constexpr UCHAR STATE_APP_IDLE = 0x00, STATE_DFU_IDLE = 0x02, STATE_DFU_DOWNLOAD_IDLE = 0x05, STATE_DFU_ERROR = 0x0A;
constexpr USHORT kBlock = 64;  // wTransferSize from the DFU functional descriptor

const char* StateName(UCHAR s) {
    static const char* names[] = {"appIDLE", "appDETACH", "dfuIDLE", "dfuDNLOAD-SYNC", "dfuDNBUSY", "dfuDNLOAD-IDLE",
                                  "dfuMANIFEST-SYNC", "dfuMANIFEST", "dfuMANIFEST-WAIT-RESET", "dfuUPLOAD-IDLE", "dfuERROR"};
    return s < 11 ? names[s] : "unknown";
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

std::vector<std::wstring> InterfacePaths(const GUID& guid) {
    std::vector<std::wstring> paths;
    HDEVINFO set = SetupDiGetClassDevsW(&guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return paths;
    for (DWORD index = 0;; ++index) {
        SP_DEVICE_INTERFACE_DATA item{};
        item.cbSize = sizeof item;
        if (!SetupDiEnumDeviceInterfaces(set, nullptr, &guid, index, &item)) break;
        DWORD needed = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &item, nullptr, 0, &needed, nullptr);
        if (needed < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W) || needed > 65536) continue;
        std::vector<BYTE> buffer(needed);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buffer.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (SetupDiGetDeviceInterfaceDetailW(set, &item, detail, needed, nullptr, nullptr)) paths.push_back(detail->DevicePath);
    }
    SetupDiDestroyDeviceInfoList(set);
    return paths;
}

struct Dfu {
    HANDLE file = INVALID_HANDLE_VALUE;
    WINUSB_INTERFACE_HANDLE first = nullptr, handle = nullptr;
    std::vector<WINUSB_INTERFACE_HANDLE> associated;
    std::wstring path;
    UCHAR interfaceNumber = 0, protocol = 0;  // protocol 1 = runtime, 2 = DFU mode
    USB_DEVICE_DESCRIPTOR device{};

    ~Dfu() { Close(); }

    void Close() {
        for (auto h : associated) WinUsb_Free(h);
        associated.clear();
        if (first) WinUsb_Free(first);
        first = handle = nullptr;
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        file = INVALID_HANDLE_VALUE;
    }

    // Opens the first WinUSB-bound VCI-400 DFU interface (DFU mode preferred over runtime).
    DWORD Open() {
        std::vector<std::wstring> candidates = InterfacePaths(kVci400WinUsbDfuModeGuid);
        for (auto& p : InterfacePaths(kVci400WinUsbDfuRuntimeGuid)) candidates.push_back(p);
        if (candidates.empty()) return ERROR_DEVICE_NOT_CONNECTED;
        DWORD last = ERROR_NOT_FOUND;
        for (const auto& candidate : candidates) {
            file = CreateFileW(candidate.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
            if (file == INVALID_HANDLE_VALUE) { last = GetLastError(); continue; }
            if (!WinUsb_Initialize(file, &first)) { last = GetLastError(); CloseHandle(file); file = INVALID_HANDLE_VALUE; continue; }
            ULONG got = 0;
            WinUsb_GetDescriptor(first, USB_DEVICE_DESCRIPTOR_TYPE, 0, 0, reinterpret_cast<PUCHAR>(&device), sizeof device, &got);
            std::vector<WINUSB_INTERFACE_HANDLE> all{first};
            for (UCHAR i = 0; i < 16; ++i) {
                WINUSB_INTERFACE_HANDLE next = nullptr;
                if (!WinUsb_GetAssociatedInterface(first, i, &next)) break;
                associated.push_back(next);
                all.push_back(next);
            }
            for (auto h : all) {
                USB_INTERFACE_DESCRIPTOR d{};
                if (WinUsb_QueryInterfaceSettings(h, 0, &d) && d.bInterfaceClass == 0xFE && d.bInterfaceSubClass == 0x01) {
                    handle = h;
                    interfaceNumber = d.bInterfaceNumber;
                    protocol = d.bInterfaceProtocol;
                    break;
                }
            }
            if (handle && device.idVendor == 0x1AA1) {
                path = candidate;
                SetTimeout(5000);
                return ERROR_SUCCESS;
            }
            for (auto h : associated) WinUsb_Free(h);
            associated.clear();
            WinUsb_Free(first);
            first = handle = nullptr;
            CloseHandle(file);
            file = INVALID_HANDLE_VALUE;
            last = ERROR_NOT_FOUND;
        }
        return last;
    }

    void SetTimeout(ULONG milliseconds) {
        WinUsb_SetPipePolicy(handle, 0, PIPE_TRANSFER_TIMEOUT, sizeof milliseconds, &milliseconds);
    }

    DWORD Control(UCHAR type, UCHAR request, USHORT value, PUCHAR data, USHORT length, ULONG* transferred = nullptr) {
        WINUSB_SETUP_PACKET setup{};
        setup.RequestType = type;
        setup.Request = request;
        setup.Value = value;
        setup.Index = interfaceNumber;
        setup.Length = length;
        ULONG got = 0;
        BOOL ok = WinUsb_ControlTransfer(handle, setup, data, length, &got, nullptr);
        if (transferred) *transferred = got;
        return ok ? ERROR_SUCCESS : GetLastError();
    }

    DWORD GetState(UCHAR& state) { return Control(kFromDevice, DFU_GETSTATE, 0, &state, 1); }

    DWORD GetStatus(UCHAR& status, UCHAR& state) {
        UCHAR b[6]{};
        DWORD e = Control(kFromDevice, DFU_GETSTATUS, 0, b, 6);
        status = b[0];
        state = b[4];
        return e;
    }
};

int Fail(const char* step, DWORD e) {
    wprintf(L"FAIL %hs: %lu (%ls)\n", step, e, ErrorText(e).c_str());
    return 3;
}

int OpenOrFail(Dfu& dfu) {
    DWORD e = dfu.Open();
    if (e) {
        wprintf(L"No WinUSB-bound VCI-400 DFU interface found (%lu: %ls).\n"
                L"Bind it first (admin): vci400-winusb-bind bind --interface MI_05, or --dfu-mode after enter-dfu.\n",
                e, ErrorText(e).c_str());
        return 3;
    }
    wprintf(L"Device %04X:%04X bcdDevice %04X, DFU interface %u, %ls mode\n  %ls\n", dfu.device.idVendor, dfu.device.idProduct,
            dfu.device.bcdDevice, dfu.interfaceNumber, dfu.protocol == 2 ? L"DFU" : L"runtime", dfu.path.c_str());
    return 0;
}

bool ReadFile(const wchar_t* path, std::vector<uint8_t>& out) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"rb") || !f) return false;
    uint8_t buffer[4096];
    size_t got;
    while ((got = fread(buffer, 1, sizeof buffer, f)) > 0) out.insert(out.end(), buffer, buffer + got);
    fclose(f);
    return true;
}

int CmdStatus(Dfu& dfu) {
    UCHAR state = 0xFF;
    DWORD e1 = dfu.GetState(state);
    if (e1) wprintf(L"GETSTATE failed: %lu (%ls)\n", e1, ErrorText(e1).c_str());
    else wprintf(L"GETSTATE: %u %hs\n", state, StateName(state));
    if (dfu.protocol != 2) {
        // Vestax's 2012 runtime firmware does not answer GETSTATUS (observed: timeout).
        wprintf(L"(GETSTATUS is only queried in DFU mode)\n");
        return e1 ? 3 : 0;
    }
    UCHAR status = 0xFF, state2 = 0xFF;
    DWORD e2 = dfu.GetStatus(status, state2);
    if (e2) wprintf(L"GETSTATUS failed: %lu (%ls)\n", e2, ErrorText(e2).c_str());
    else wprintf(L"GETSTATUS: status %u, state %u %hs\n", status, state2, StateName(state2));
    return (e1 || e2) ? 3 : 0;
}

bool RequireDfuMode(const Dfu& dfu) {
    if (dfu.protocol == 2) return true;
    wprintf(L"The device is in runtime mode. Run 'vci400-dfu enter-dfu' first, then bind the DFU-mode device.\n");
    return false;
}

// Returns the device to dfuIDLE if it is in dfuERROR.
DWORD EnsureIdle(Dfu& dfu) {
    UCHAR state = 0;
    DWORD e = dfu.GetState(state);
    if (e) return e;
    if (state == STATE_DFU_ERROR) {
        dfu.Control(kToDevice, DFU_CLRSTATUS, 0, nullptr, 0);
        if ((e = dfu.GetState(state))) return e;
    }
    wprintf(L"state before transfer: %hs\n", StateName(state));
    return state == STATE_DFU_IDLE ? ERROR_SUCCESS : ERROR_INVALID_STATE;
}

int CmdUpload(Dfu& dfu, const wchar_t* out) {
    if (!RequireDfuMode(dfu)) return 3;
    DWORD e = EnsureIdle(dfu);
    if (e) return Fail("device not in dfuIDLE", e);
    std::vector<uint8_t> image;
    for (USHORT block = 0; block < 0xFFFF; ++block) {
        UCHAR buffer[kBlock];
        ULONG got = 0;
        if ((e = dfu.Control(kFromDevice, DFU_UPLOAD, block, buffer, kBlock, &got))) return Fail("UPLOAD", e);
        if (got == 0) break;
        image.insert(image.end(), buffer, buffer + got);
    }
    FILE* f = nullptr;
    if (_wfopen_s(&f, out, L"wb") || !f) return Fail("open output", ERROR_CANNOT_MAKE);
    if (!image.empty()) fwrite(image.data(), 1, image.size(), f);
    fclose(f);
    if (image.empty()) {
        wprintf(L"PASS upload: 0 bytes. No upgrade image is present; the unit runs its factory image.\n");
        return 0;
    }
    xmos_image::Image img;
    bool parsed = xmos_image::FromBytes(image, img);
    bool valid = parsed && xmos_image::Verify(img).All();
    wprintf(L"PASS upload: %zu bytes written to %ls; upgrade image %hs\n", image.size(), out,
            valid ? "parses and all CRCs verify" : (parsed ? "parses but CRCs FAIL" : img.error.c_str()));
    return 0;
}

int CmdDownload(Dfu& dfu, const wchar_t* path) {
    if (!RequireDfuMode(dfu)) return 3;
    std::vector<uint8_t> bytes;
    if (!ReadFile(path, bytes)) return Fail("read image", ERROR_FILE_NOT_FOUND);
    xmos_image::Image img;
    if (!xmos_image::FromBytes(bytes, img)) { wprintf(L"FAIL image: %hs\n", img.error.c_str()); return 3; }
    if (!xmos_image::Verify(img).All()) { wprintf(L"FAIL image: CRC verification failed; refusing to flash\n"); return 3; }
    // Keep the zero padding up to a whole 256-byte flash page, as in the shipped files.
    size_t length = (bytes.size() + kBlock - 1) / kBlock * kBlock;
    bytes.resize(length, 0);
    wprintf(L"Image verified: size 0x%X, %zu tiles, %zu blocks of %u bytes\n", img.size, img.tiles.size(), length / kBlock, kBlock);

    DWORD e = EnsureIdle(dfu);
    if (e) return Fail("device not in dfuIDLE", e);
    for (size_t block = 0; block < length / kBlock; ++block) {
        dfu.SetTimeout(block == 0 ? 30000 : 10000);  // block 0 erases the old upgrade image
        if ((e = dfu.Control(kToDevice, DFU_DNLOAD, static_cast<USHORT>(block), &bytes[block * kBlock], kBlock))) {
            wprintf(L"\nFAIL DNLOAD block %zu: %lu (%ls). Do not power off; run 'vci400-dfu status'.\n", block, e, ErrorText(e).c_str());
            return 3;
        }
        UCHAR status = 0, state = 0;
        if ((e = dfu.GetStatus(status, state))) return Fail("GETSTATUS", e);
        if (status != 0 || state != STATE_DFU_DOWNLOAD_IDLE) {
            wprintf(L"\nFAIL block %zu: status %u state %hs\n", block, status, StateName(state));
            return 3;
        }
        if (block % 64 == 0 || block + 1 == length / kBlock) wprintf(L"\r  %zu/%zu blocks", block + 1, length / kBlock);
    }
    wprintf(L"\n");
    if ((e = dfu.Control(kToDevice, DFU_DNLOAD, 0, nullptr, 0))) return Fail("final zero-length DNLOAD", e);
    UCHAR status = 0, state = 0;
    if ((e = dfu.GetStatus(status, state))) return Fail("GETSTATUS after manifest", e);
    wprintf(L"manifest: status %u, state %hs\n", status, StateName(state));
    if (status != 0) return Fail("manifest status", ERROR_GEN_FAILURE);
    dfu.Control(kToDevice, XMOS_RESETFROMDFU, 0, nullptr, 0);  // device reboots; errors are expected
    wprintf(L"PASS download: image written and device told to reboot. Power-cycle the VCI-400, then check bcdDevice and the descriptor.\n");
    return 0;
}

// In the 2013-era XMOS code (sc_usb_audio v6.1 dfu.xc) REVERTFACTORY is dispatched in runtime
// mode as well and only erases upgrade images (no reboot). --runtime uses that path, for
// firmware (v21) whose DETACH-to-DFU reboot hangs.
int CmdRevert(Dfu& dfu, bool allowRuntime) {
    if (dfu.protocol != 2 && !allowRuntime) {
        RequireDfuMode(dfu);
        wprintf(L"(or add --runtime to send the revert in normal mode)\n");
        return 3;
    }
    dfu.SetTimeout(30000);  // the device erases flash before acknowledging
    DWORD e = dfu.Control(kToDevice, XMOS_REVERTFACTORY, 0, nullptr, 0);
    if (e) return Fail("REVERTFACTORY", e);
    Sleep(2000);
    if (dfu.protocol == 2) dfu.Control(kToDevice, XMOS_RESETFROMDFU, 0, nullptr, 0);
    wprintf(L"PASS revert-factory: request accepted; upgrade images erased. Power-cycle the VCI-400:\n"
            L"it should come back with the factory firmware (REV_0600).\n");
    return 0;
}

int CmdEnter(Dfu& dfu) {
    if (dfu.protocol == 2) { wprintf(L"Already in DFU mode.\n"); return 0; }
    UCHAR state = 0xFF;
    if (dfu.GetState(state) == ERROR_SUCCESS) wprintf(L"runtime state: %hs\n", StateName(state));
    dfu.Control(kToDevice, XMOS_RESETINTODFU, 0, nullptr, 0);  // device reboots; errors are expected
    wprintf(L"Sent RESETINTODFU. The VCI-400 reboots into DFU mode in a few seconds.\n"
            L"Next (admin): vci400-winusb-bind status --dfu-mode, then bind --dfu-mode.\n");
    return 0;
}

// Port-cycles the VCI-400's upstream hub port (disconnect + reconnect = USB bus reset).
// Needs an administrator token to open the hub. Returns a Win32 error.
DWORD CyclePort() {
    DEVINST device = 0, hub = 0;
    wchar_t composite[] = L"USB\\VID_1AA1&PID_0231\\0000";
    if (CM_Locate_DevNodeW(&device, composite, CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) return ERROR_NO_SUCH_DEVINST;
    if (CM_Get_Parent(&hub, device, 0) != CR_SUCCESS) return ERROR_NO_SUCH_DEVINST;
    ULONG port = 0, size = sizeof port;
    DEVPROPTYPE type = 0;
    if (CM_Get_DevNode_PropertyW(device, &DEVPKEY_Device_Address, &type, reinterpret_cast<PBYTE>(&port), &size, 0) != CR_SUCCESS || !port)
        return ERROR_INVALID_DATA;
    wchar_t hubId[MAX_DEVICE_ID_LEN];
    if (CM_Get_Device_IDW(hub, hubId, MAX_DEVICE_ID_LEN, 0) != CR_SUCCESS) return ERROR_NO_SUCH_DEVINST;
    ULONG length = 0;
    if (CM_Get_Device_Interface_List_SizeW(&length, const_cast<LPGUID>(&GUID_DEVINTERFACE_USB_HUB), hubId, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS || length < 2)
        return ERROR_NOT_FOUND;
    std::vector<wchar_t> list(length);
    if (CM_Get_Device_Interface_ListW(const_cast<LPGUID>(&GUID_DEVINTERFACE_USB_HUB), hubId, list.data(), length, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS)
        return ERROR_NOT_FOUND;
    HANDLE hubHandle = CreateFileW(list.data(), GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hubHandle == INVALID_HANDLE_VALUE) return GetLastError();
    VciCyclePortParams params{};
    params.ConnectionIndex = port;
    DWORD returned = 0;
    BOOL ok = DeviceIoControl(hubHandle, VCI_IOCTL_USB_HUB_CYCLE_PORT, &params, sizeof params, &params, sizeof params, &returned, nullptr);
    DWORD error = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(hubHandle);
    wprintf(L"cycle port %lu on %ls: %ls\n", port, hubId, ok ? L"done" : ErrorText(error).c_str());
    return error;
}

// Standard DFU 1.1 entry, as used by pre-2014 XMOS firmware: DFU_DETACH arms a timer, and a
// USB reset before it expires reboots into DFU mode (sc_usb_audio dfu.xc DFU_Detach /
// DFUReportResetState). The functional descriptor has bitWillDetach = 0, so the host resets.
int CmdDetach(Dfu& dfu, bool cycle) {
    if (dfu.protocol == 2) { wprintf(L"Already in DFU mode.\n"); return 0; }
    // The firmware stores timeout*100000 ticks (10 ns) in 32 bits, so stay below 42949 ms.
    // The WinUSB driver install for the DFU-mode devnode resets the device; that reset must
    // land inside this window or the firmware returns to the application.
    constexpr USHORT kTimeoutMs = 40000;
    DWORD e = dfu.Control(kToDevice, DFU_DETACH, kTimeoutMs, nullptr, 0);
    if (e) return Fail("DFU_DETACH", e);
    wprintf(L"DFU_DETACH accepted (timeout %u ms).\n", kTimeoutMs);
    if (cycle) {
        dfu.Close();  // release our handles before the device disappears
        e = CyclePort();
        if (e == ERROR_SUCCESS) {
            wprintf(L"Port reset sent. Within a few seconds the VCI-400 should re-enumerate in DFU mode.\n"
                    L"Next (admin): vci400-winusb-bind status --dfu-mode, then bind --dfu-mode.\n");
            return 0;
        }
        wprintf(L"Automatic port reset failed (run as administrator?).\n");
    }
    wprintf(L"Now, within 40 seconds: unplug the USB cable and plug it back in, keeping the power adaptor connected.\n"
            L"Then (admin): vci400-winusb-bind status --dfu-mode, then bind --dfu-mode.\n");
    return 0;
}

int CmdLeave(Dfu& dfu) {
    dfu.Control(kToDevice, XMOS_RESETFROMDFU, 0, nullptr, 0);
    wprintf(L"Sent RESETFROMDFU; the device reboots into its application.\n");
    return 0;
}

void Usage() {
    wprintf(L"vci400-dfu list | status | detach [--manual] | enter-dfu | upload <out.bin> | download <image.bin> --confirm |\n"
            L"           revert-factory --confirm | leave-dfu\n");
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) { Usage(); return 2; }
    std::wstring cmd = argv[1];
    bool confirm = argc >= 3 && !wcscmp(argv[argc - 1], L"--confirm");
    if (cmd == L"list") {
        for (const GUID* g : {&kVci400WinUsbDfuRuntimeGuid, &kVci400WinUsbDfuModeGuid})
            for (const auto& p : InterfacePaths(*g)) wprintf(L"%ls  %ls\n", g == &kVci400WinUsbDfuModeGuid ? L"dfu-mode" : L"runtime ", p.c_str());
        return 0;
    }
    Dfu dfu;
    if (int r = OpenOrFail(dfu)) return r;
    if (cmd == L"status") return CmdStatus(dfu);
    if (cmd == L"enter-dfu") return CmdEnter(dfu);
    if (cmd == L"detach") return CmdDetach(dfu, !(argc >= 3 && !wcscmp(argv[2], L"--manual")));
    if (cmd == L"leave-dfu") return CmdLeave(dfu);
    if (cmd == L"upload" && argc >= 3) return CmdUpload(dfu, argv[2]);
    if (cmd == L"download" && argc >= 3) {
        if (!confirm) { wprintf(L"Refusing to flash without --confirm.\n"); return 2; }
        return CmdDownload(dfu, argv[2]);
    }
    if (cmd == L"revert-factory") {
        bool runtime = false;
        for (int i = 2; i < argc; ++i) runtime |= !wcscmp(argv[i], L"--runtime");
        bool confirmed = false;
        for (int i = 2; i < argc; ++i) confirmed |= !wcscmp(argv[i], L"--confirm");
        if (!confirmed) { wprintf(L"Refusing to revert without --confirm.\n"); return 2; }
        return CmdRevert(dfu, runtime);
    }
    Usage();
    return 2;
}
