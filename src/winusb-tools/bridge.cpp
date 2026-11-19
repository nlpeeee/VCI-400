// vci400-bridge: user-mode MIDI driver replacement for the Vestax VCI-400.
//
// Reads USB-MIDI 1.0 from interface 4 bulk IN 0x83 through Microsoft's WinUSB
// (MI_03 bound with vci400-winusb-bind) and publishes it on a named virtual MIDI
// port created with the installed teVirtualMIDI driver (loopMIDI's driver,
// Tobias Erichsen). MIDI that applications send to the port (LEDs) is encoded
// as USB-MIDI and written to bulk OUT 0x02, unless --input-only is given.
//
// No kernel code of our own, no signing, no policy change. The virtual port
// exists while this process runs; the USB side reconnects after unplugging.
//
// teVirtualMIDI API as exported by teVirtualMIDI64.dll 1.3.0.43; its SDK is free
// for non-commercial use (commercial use needs a licence from Tobias Erichsen).

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <setupapi.h>
#include <winusb.h>
#include <usbspec.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <mutex>
#include <share.h>
#include <string>
#include <vector>

#include "vci400_winusb.h"

namespace {

constexpr wchar_t kToolVersion[] = L"0.1.0";
constexpr UCHAR kMidiIn = 0x83;
constexpr UCHAR kMidiOut = 0x02;
constexpr DWORD kMaxSysex = 65535;

// teVirtualMIDI
typedef struct _VM_MIDI_PORT VM_MIDI_PORT, *LPVM_MIDI_PORT;
typedef void(CALLBACK* LPVM_MIDI_DATA_CB)(LPVM_MIDI_PORT, LPBYTE, DWORD, DWORD_PTR);
typedef LPVM_MIDI_PORT(CALLBACK* CreatePortEx2Fn)(LPCWSTR, LPVM_MIDI_DATA_CB, DWORD_PTR, DWORD, DWORD);
typedef void(CALLBACK* ClosePortFn)(LPVM_MIDI_PORT);
typedef BOOL(CALLBACK* SendDataFn)(LPVM_MIDI_PORT, LPBYTE, DWORD);
typedef LPCWSTR(CALLBACK* VersionFn)(PWORD, PWORD, PWORD, PWORD);
constexpr DWORD TE_VM_FLAGS_PARSE_RX = 1;
constexpr DWORD TE_VM_FLAGS_PARSE_TX = 2;
constexpr DWORD TE_VM_FLAGS_INSTANTIATE_BOTH = 12;

struct VirtualMidi {
    HMODULE dll = nullptr;
    CreatePortEx2Fn create = nullptr;
    ClosePortFn close = nullptr;
    SendDataFn send = nullptr;
    VersionFn version = nullptr;
    VersionFn driverVersion = nullptr;
    bool Load() {
        dll = LoadLibraryExW(L"teVirtualMIDI64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!dll) return false;
        create = reinterpret_cast<CreatePortEx2Fn>(GetProcAddress(dll, "virtualMIDICreatePortEx2"));
        close = reinterpret_cast<ClosePortFn>(GetProcAddress(dll, "virtualMIDIClosePort"));
        send = reinterpret_cast<SendDataFn>(GetProcAddress(dll, "virtualMIDISendData"));
        version = reinterpret_cast<VersionFn>(GetProcAddress(dll, "virtualMIDIGetVersion"));
        driverVersion = reinterpret_cast<VersionFn>(GetProcAddress(dll, "virtualMIDIGetDriverVersion"));
        return create && close && send;
    }
};

std::atomic<bool> g_stop{false};
bool g_verbose = false;
bool g_inputOnly = false;
std::atomic<uint64_t> g_toApp{0}, g_toDevice{0}, g_dropped{0};

BOOL WINAPI OnConsoleCtrl(DWORD) {
    g_stop = true;
    return TRUE;
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

void PrintBytes(const wchar_t* direction, const BYTE* data, DWORD length) {
    wprintf(L"%ls", direction);
    for (DWORD i = 0; i < std::min<DWORD>(length, 24); ++i) wprintf(L" %02X", data[i]);
    if (length > 24) wprintf(L" ... (%lu bytes)", length);
    wprintf(L"\n");
}

// Optional traffic log (--log): every message in both directions, full length,
// with milliseconds since start. Written from the USB thread and the
// teVirtualMIDI callback thread, so it is serialised.
FILE* g_log = nullptr;
std::mutex g_logLock;
ULONGLONG g_start = 0;

void LogLine(const char* direction, const BYTE* data, DWORD length, const char* note = nullptr) {
    if (!g_log) return;
    std::lock_guard<std::mutex> guard(g_logLock);
    fprintf(g_log, "%10llu %s", GetTickCount64() - g_start, direction);
    for (DWORD i = 0; data && i < length; ++i) fprintf(g_log, " %02X", data[i]);
    if (note) fprintf(g_log, "  %s", note);
    fprintf(g_log, "\n");
    fflush(g_log);
}

void LogNote(const char* text, DWORD error = 0) {
    if (!g_log) return;
    char buffer[96];
    if (error) snprintf(buffer, sizeof buffer, "%s error %lu", text, error);
    else snprintf(buffer, sizeof buffer, "%s", text);
    LogLine("note", nullptr, 0, buffer);
}

// ---- USB side -------------------------------------------------------------

std::wstring FindInterfacePath() {
    std::wstring path;
    HDEVINFO set = SetupDiGetClassDevsW(&kVci400WinUsbInterfaceGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return path;
    SP_DEVICE_INTERFACE_DATA item{};
    item.cbSize = sizeof item;
    if (SetupDiEnumDeviceInterfaces(set, nullptr, &kVci400WinUsbInterfaceGuid, 0, &item)) {
        DWORD needed = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &item, nullptr, 0, &needed, nullptr);
        if (needed >= sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W) && needed <= 65536) {
            std::vector<BYTE> buffer(needed);
            auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buffer.data());
            detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
            if (SetupDiGetDeviceInterfaceDetailW(set, &item, detail, needed, nullptr, nullptr)) path = detail->DevicePath;
        }
    }
    SetupDiDestroyDeviceInfoList(set);
    return path;
}

struct UsbMidi {
    std::mutex lock;  // guards handles against the app->device callback
    HANDLE file = INVALID_HANDLE_VALUE;
    WINUSB_INTERFACE_HANDLE first = nullptr;
    std::vector<WINUSB_INTERFACE_HANDLE> associated;
    WINUSB_INTERFACE_HANDLE midi = nullptr;  // interface that owns 0x83/0x02
    bool haveOut = false;

    void CloseLocked() {
        for (auto h : associated) WinUsb_Free(h);
        associated.clear();
        if (first) WinUsb_Free(first);
        first = midi = nullptr;
        haveOut = false;
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        file = INVALID_HANDLE_VALUE;
    }

    DWORD Open() {
        std::lock_guard<std::mutex> guard(lock);
        CloseLocked();
        std::wstring path = FindInterfacePath();
        if (path.empty()) return ERROR_DEVICE_NOT_CONNECTED;
        file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
        if (file == INVALID_HANDLE_VALUE) return GetLastError();
        if (!WinUsb_Initialize(file, &first)) {
            DWORD e = GetLastError();
            CloseLocked();
            return e;
        }
        std::vector<WINUSB_INTERFACE_HANDLE> all{first};
        for (UCHAR index = 0; index < 16; ++index) {
            WINUSB_INTERFACE_HANDLE next = nullptr;
            if (!WinUsb_GetAssociatedInterface(first, index, &next)) break;
            associated.push_back(next);
            all.push_back(next);
        }
        for (auto handle : all) {
            USB_INTERFACE_DESCRIPTOR d{};
            if (!WinUsb_QueryInterfaceSettings(handle, 0, &d)) continue;
            bool hasIn = false, hasOut = false;
            for (UCHAR i = 0; i < d.bNumEndpoints; ++i) {
                WINUSB_PIPE_INFORMATION pipe{};
                if (!WinUsb_QueryPipe(handle, 0, i, &pipe) || pipe.PipeType != UsbdPipeTypeBulk) continue;
                hasIn |= pipe.PipeId == kMidiIn;
                hasOut |= pipe.PipeId == kMidiOut;
            }
            if (hasIn) {
                midi = handle;
                haveOut = hasOut;
                break;
            }
        }
        if (!midi) {
            CloseLocked();
            return ERROR_NOT_FOUND;
        }
        ULONG readTimeout = 200, writeTimeout = 500;
        WinUsb_SetPipePolicy(midi, kMidiIn, PIPE_TRANSFER_TIMEOUT, sizeof readTimeout, &readTimeout);
        UCHAR yes = TRUE;
        WinUsb_SetPipePolicy(midi, kMidiIn, ALLOW_PARTIAL_READS, sizeof yes, &yes);
        if (haveOut) WinUsb_SetPipePolicy(midi, kMidiOut, PIPE_TRANSFER_TIMEOUT, sizeof writeTimeout, &writeTimeout);
        return ERROR_SUCCESS;
    }

    void Close() {
        std::lock_guard<std::mutex> guard(lock);
        CloseLocked();
    }

    // Reads are issued without the lock held: WinUSB allows concurrent pipes,
    // and only this thread ever closes the handles.
    WINUSB_INTERFACE_HANDLE ReadHandle() {
        std::lock_guard<std::mutex> guard(lock);
        return midi;
    }

    bool Write(const std::vector<BYTE>& packets) {
        std::lock_guard<std::mutex> guard(lock);
        if (!midi || !haveOut) {
            LogNote("write skipped: controller not connected");
            return false;
        }
        ULONG written = 0;
        bool ok = WinUsb_WritePipe(midi, kMidiOut, const_cast<BYTE*>(packets.data()), static_cast<ULONG>(packets.size()), &written, nullptr) &&
                  written == packets.size();
        if (!ok) LogNote("bulk OUT write failed", GetLastError());
        return ok;
    }
};

UsbMidi g_usb;

// ---- USB-MIDI <-> MIDI byte stream ----------------------------------------

// Decodes USB-MIDI 1.0 event packets into complete MIDI messages (cable 0 only).
struct UsbMidiDecoder {
    std::vector<BYTE> sysex;
    template <typename Emit>
    void Packet(const BYTE* p, Emit emit) {
        unsigned cable = p[0] >> 4, cin = p[0] & 0xF;
        if (cable != 0) return;
        auto sysexAppend = [&](int count, bool end) {
            for (int i = 1; i <= count; ++i) {
                if (sysex.size() < kMaxSysex) sysex.push_back(p[i]);
                else { ++g_dropped; sysex.clear(); return; }
            }
            if (end) {
                if (!sysex.empty() && sysex.front() == 0xF0) emit(sysex.data(), static_cast<DWORD>(sysex.size()));
                sysex.clear();
            }
        };
        switch (cin) {
        case 0x2: emit(p + 1, 2); break;
        case 0x3: emit(p + 1, 3); break;
        case 0x4: sysexAppend(3, false); break;
        case 0x5:
            if (!sysex.empty() || p[1] == 0xF7) sysexAppend(1, true);
            else emit(p + 1, 1);
            break;
        case 0x6: sysexAppend(2, true); break;
        case 0x7: sysexAppend(3, true); break;
        case 0x8: case 0x9: case 0xA: case 0xB: case 0xE: emit(p + 1, 3); break;
        case 0xC: case 0xD: emit(p + 1, 2); break;
        case 0xF: emit(p + 1, 1); break;
        default: break;  // 0x0/0x1 reserved; zero padding
        }
    }
};

// Encodes one complete MIDI message into USB-MIDI 1.0 packets on cable 0.
std::vector<BYTE> EncodeUsbMidi(const BYTE* m, DWORD length) {
    std::vector<BYTE> out;
    auto packet = [&](BYTE cin, BYTE a, BYTE b, BYTE c) { out.insert(out.end(), {cin, a, b, c}); };
    if (!length) return out;
    BYTE s = m[0];
    if (s == 0xF0) {
        DWORD i = 0;
        while (length - i > 3) {
            packet(0x4, m[i], m[i + 1], m[i + 2]);
            i += 3;
        }
        DWORD rest = length - i;
        packet(static_cast<BYTE>(0x4 + rest), m[i], rest > 1 ? m[i + 1] : 0, rest > 2 ? m[i + 2] : 0);
    } else if (s >= 0xF8) {
        packet(0xF, s, 0, 0);
    } else if (s == 0xF1 || s == 0xF3) {
        if (length >= 2) packet(0x2, s, m[1], 0);
    } else if (s == 0xF2) {
        if (length >= 3) packet(0x3, s, m[1], m[2]);
    } else if (s == 0xF6) {
        packet(0x5, s, 0, 0);
    } else if (s >= 0x80 && s < 0xF0) {
        BYTE kind = s >> 4;
        DWORD need = (kind == 0xC || kind == 0xD) ? 2 : 3;
        if (length >= need) packet(kind, s, m[1], need == 3 ? m[2] : 0);
    }
    return out;
}

// ---- virtual port ---------------------------------------------------------

VirtualMidi g_vm;
LPVM_MIDI_PORT g_port = nullptr;

void CALLBACK FromApplications(LPVM_MIDI_PORT, LPBYTE data, DWORD length, DWORD_PTR) {
    if (!data || !length) return;  // port shutting down
    if (g_verbose) PrintBytes(L"app -> device:", data, length);
    LogLine("app>dev", data, length, g_inputOnly ? "(input-only: not sent)" : nullptr);
    if (g_inputOnly) return;
    std::vector<BYTE> packets = EncodeUsbMidi(data, length);
    if (packets.empty()) {
        LogNote("message not encodable as USB-MIDI; dropped");
        ++g_dropped;
        return;
    }
    if (g_usb.Write(packets)) ++g_toDevice;
    else ++g_dropped;
}

void Usage() {
    wprintf(L"vci400-bridge %ls\n"
            L"  vci400-bridge [--name \"VCI-400\"] [--input-only] [--verbose] [--log traffic.log]\n"
            L"Publishes the VCI-400 (MI_03 on WinUSB) as a virtual MIDI port until Ctrl+C.\n", kToolVersion);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    std::wstring name = L"VCI-400";
    for (int i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--name") && i + 1 < argc) name = argv[++i];
        else if (!wcscmp(argv[i], L"--input-only")) g_inputOnly = true;
        else if (!wcscmp(argv[i], L"--verbose")) g_verbose = true;
        else if (!wcscmp(argv[i], L"--log") && i + 1 < argc) {
            // Shared open so the log can be read while the bridge is running.
            g_log = _wfsopen(argv[++i], L"w", _SH_DENYNO);
            if (!g_log) {
                wprintf(L"Cannot open log file %ls\n", argv[i]);
                return 1;
            }
        }
        else { Usage(); return 1; }
    }
    g_start = GetTickCount64();
    SetConsoleCtrlHandler(OnConsoleCtrl, TRUE);
    setvbuf(stdout, nullptr, _IONBF, 0);  // keep logs current when redirected

    if (!g_vm.Load()) {
        wprintf(L"teVirtualMIDI64.dll is not available (install loopMIDI or the teVirtualMIDI runtime).\n");
        return 3;
    }
    if (g_vm.version && g_vm.driverVersion) {
        WORD a, b, c, d, e, f, g, h;
        LPCWSTR dllText = g_vm.version(&a, &b, &c, &d);
        LPCWSTR drvText = g_vm.driverVersion(&e, &f, &g, &h);
        wprintf(L"teVirtualMIDI dll %ls, driver %ls\n", dllText ? dllText : L"?", drvText ? drvText : L"?");
    }
    g_port = g_vm.create(name.c_str(), FromApplications, 0, kMaxSysex,
                         TE_VM_FLAGS_PARSE_RX | TE_VM_FLAGS_PARSE_TX | TE_VM_FLAGS_INSTANTIATE_BOTH);
    if (!g_port) {
        DWORD e = GetLastError();
        wprintf(L"Creating virtual port \"%ls\" failed: %lu (%ls)%ls\n", name.c_str(), e, ErrorText(e).c_str(),
                e == ERROR_ALREADY_EXISTS ? L" - a port with that name exists (loopMIDI?)" : L"");
        return 3;
    }
    wprintf(L"Virtual MIDI port \"%ls\" is up (%ls). Ctrl+C to stop.\n", name.c_str(),
            g_inputOnly ? L"controller -> apps only" : L"both directions");

    UsbMidiDecoder decoder;
    auto emit = [&](const BYTE* bytes, DWORD length) {
        if (g_verbose) PrintBytes(L"device -> app:", bytes, length);
        LogLine("dev>app", bytes, length);
        if (g_vm.send(g_port, const_cast<BYTE*>(bytes), length)) ++g_toApp;
        else {
            LogNote("virtualMIDISendData failed", GetLastError());
            ++g_dropped;
        }
    };

    std::vector<BYTE> buffer(512);
    bool connected = false;
    DWORD lastOpenError = 0;
    ULONGLONG lastReport = GetTickCount64();
    uint64_t reportedApp = 0, reportedDevice = 0;
    while (!g_stop) {
        if (!connected) {
            DWORD e = g_usb.Open();
            if (e == ERROR_SUCCESS) {
                connected = true;
                decoder.sysex.clear();
                wprintf(L"Controller connected%ls.\n", g_usb.haveOut ? L"" : L" (no bulk OUT 0x02: input only)");
                LogNote(g_usb.haveOut ? "controller connected" : "controller connected without bulk OUT");
            } else {
                if (e != lastOpenError)
                    wprintf(L"Waiting for the controller on WinUSB: %lu (%ls)\n", e, ErrorText(e).c_str());
                lastOpenError = e;
                Sleep(1000);
                continue;
            }
            lastOpenError = 0;
        }
        WINUSB_INTERFACE_HANDLE handle = g_usb.ReadHandle();
        ULONG got = 0;
        if (!WinUsb_ReadPipe(handle, kMidiIn, buffer.data(), static_cast<ULONG>(buffer.size()), &got, nullptr)) {
            DWORD e = GetLastError();
            if (e != ERROR_SEM_TIMEOUT) {
                wprintf(L"Controller read failed: %lu (%ls). Reconnecting.\n", e, ErrorText(e).c_str());
                LogNote("bulk IN read failed; reconnecting", e);
                g_usb.Close();
                connected = false;
                Sleep(500);
            }
        } else {
            for (ULONG offset = 0; offset + 4 <= got; offset += 4) decoder.Packet(buffer.data() + offset, emit);
        }
        ULONGLONG now = GetTickCount64();
        if (now - lastReport >= 10000) {
            uint64_t app = g_toApp, device = g_toDevice;
            if (app != reportedApp || device != reportedDevice)
                wprintf(L"messages: controller->apps %llu, apps->controller %llu, dropped %llu\n", app, device, g_dropped.load());
            reportedApp = app;
            reportedDevice = device;
            lastReport = now;
        }
    }

    wprintf(L"Stopping. controller->apps %llu, apps->controller %llu, dropped %llu\n", g_toApp.load(), g_toDevice.load(), g_dropped.load());
    g_vm.close(g_port);
    g_usb.Close();
    LogNote("stopped");
    if (g_log) fclose(g_log);
    return 0;
}
