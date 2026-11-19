// vci400-winusb-probe: read the VCI-400 MIDI stream directly through WinUSB.
// Requires MI_03 bound to WinUSB by vci400-winusb-bind.
//
//   list     Opens the device and reports every interface and pipe in the MI_03
//            function (answers whether interface 4 belongs to it). No transfers.
//   capture  Reads bulk IN endpoint 0x83 and decodes USB-MIDI 1.0 event packets
//            while you move the controls. Never writes to the device.

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
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "vci400_winusb.h"

namespace {

constexpr wchar_t kToolVersion[] = L"0.1.0";
constexpr UCHAR kMidiInEndpoint = 0x83;
constexpr size_t kMaxLogged = 20000;
std::atomic<bool> g_stop{false};

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

std::string Utf8(const std::wstring& text) {
    if (text.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
    return out;
}

std::string JsonString(const std::string& text) {
    std::string out = "\"";
    for (unsigned char ch : text) {
        if (ch == '"' || ch == '\\') { out += '\\'; out += static_cast<char>(ch); }
        else if (ch < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", ch); out += b; }
        else out += static_cast<char>(ch);
    }
    return out + "\"";
}

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

struct PipeInfo {
    UCHAR endpoint;
    int type;
    USHORT maxPacket;
};

struct InterfaceInfo {
    int associatedIndex;  // -1 = first interface (the WinUSB handle itself)
    USB_INTERFACE_DESCRIPTOR descriptor;
    std::vector<PipeInfo> pipes;
};

const char* PipeTypeText(int type) {
    switch (type) {
    case UsbdPipeTypeControl: return "control";
    case UsbdPipeTypeIsochronous: return "isochronous";
    case UsbdPipeTypeBulk: return "bulk";
    case UsbdPipeTypeInterrupt: return "interrupt";
    default: return "unknown";
    }
}

InterfaceInfo Describe(WINUSB_INTERFACE_HANDLE handle, int associatedIndex) {
    InterfaceInfo info{associatedIndex, {}, {}};
    if (!WinUsb_QueryInterfaceSettings(handle, 0, &info.descriptor)) return info;
    for (UCHAR i = 0; i < info.descriptor.bNumEndpoints; ++i) {
        WINUSB_PIPE_INFORMATION pipe{};
        if (WinUsb_QueryPipe(handle, 0, i, &pipe)) info.pipes.push_back({pipe.PipeId, pipe.PipeType, pipe.MaximumPacketSize});
    }
    return info;
}

// USB-MIDI 1.0 event packet: byte 0 = cable (high nibble) | CIN (low nibble).
int MidiBytesForCin(unsigned cin) {
    static const int lengths[16] = {0, 0, 2, 3, 3, 1, 2, 3, 3, 3, 3, 3, 2, 2, 3, 1};
    return lengths[cin & 0xF];
}

std::string DescribeEvent(const UCHAR* p) {
    char buf[128];
    unsigned cable = p[0] >> 4, cin = p[0] & 0xF, status = p[1], d1 = p[2] & 0x7F, d2 = p[3] & 0x7F, ch = (status & 0xF) + 1;
    switch (cin) {
    case 0x8: snprintf(buf, sizeof buf, "cable %u ch%u note-off %u vel %u", cable, ch, d1, d2); break;
    case 0x9: snprintf(buf, sizeof buf, "cable %u ch%u note-on %u vel %u", cable, ch, d1, d2); break;
    case 0xA: snprintf(buf, sizeof buf, "cable %u ch%u poly-pressure %u = %u", cable, ch, d1, d2); break;
    case 0xB: snprintf(buf, sizeof buf, "cable %u ch%u CC %u = %u", cable, ch, d1, d2); break;
    case 0xC: snprintf(buf, sizeof buf, "cable %u ch%u program %u", cable, ch, d1); break;
    case 0xD: snprintf(buf, sizeof buf, "cable %u ch%u channel-pressure %u", cable, ch, d1); break;
    case 0xE: snprintf(buf, sizeof buf, "cable %u ch%u pitch-bend %u", cable, ch, d1 | (d2 << 7)); break;
    case 0x4: case 0x5: case 0x6: case 0x7: snprintf(buf, sizeof buf, "cable %u sysex part (CIN %X)", cable, cin); break;
    default: snprintf(buf, sizeof buf, "cable %u CIN %X bytes %02X %02X %02X", cable, cin, p[1], p[2], p[3]); break;
    }
    return buf;
}

void Usage() {
    wprintf(L"vci400-winusb-probe %ls\n"
            L"  vci400-winusb-probe list    [--output report.json]\n"
            L"  vci400-winusb-probe capture [--seconds N] [--output report.json]\n"
            L"Needs MI_03 bound with 'vci400-winusb-bind bind'. Receive-only.\n", kToolVersion);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2 || (wcscmp(argv[1], L"list") && wcscmp(argv[1], L"capture"))) { Usage(); return 1; }
    const bool capture = !wcscmp(argv[1], L"capture");
    std::wstring output;
    DWORD seconds = 20;
    for (int i = 2; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--output") && i + 1 < argc) output = argv[++i];
        else if (!wcscmp(argv[i], L"--seconds") && i + 1 < argc) seconds = static_cast<DWORD>(std::wcstoul(argv[++i], nullptr, 10));
        else { Usage(); return 1; }
    }
    if (seconds == 0 || seconds > 600) seconds = 20;
    SetConsoleCtrlHandler(OnConsoleCtrl, TRUE);

    std::wstring path = FindInterfacePath();
    if (path.empty()) {
        wprintf(L"No WinUSB interface %ls is present. Run 'vci400-winusb-bind bind' as administrator first.\n", kVci400WinUsbInterfaceGuidText);
        return 3;
    }
    wprintf(L"Device: %ls\n", path.c_str());
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        wprintf(L"Open failed: %lu (%ls)\n", e, ErrorText(e).c_str());
        return 3;
    }
    WINUSB_INTERFACE_HANDLE first = nullptr;
    if (!WinUsb_Initialize(file, &first)) {
        DWORD e = GetLastError();
        wprintf(L"WinUsb_Initialize failed: %lu (%ls)\n", e, ErrorText(e).c_str());
        CloseHandle(file);
        return 3;
    }

    // Enumerate the whole MI_03 function: first interface plus associated ones.
    std::vector<InterfaceInfo> interfaces{Describe(first, -1)};
    std::vector<WINUSB_INTERFACE_HANDLE> associated;
    for (UCHAR index = 0; index < 16; ++index) {
        WINUSB_INTERFACE_HANDLE next = nullptr;
        if (!WinUsb_GetAssociatedInterface(first, index, &next)) break;
        associated.push_back(next);
        interfaces.push_back(Describe(next, index));
    }
    WINUSB_INTERFACE_HANDLE midiIn = nullptr;
    USHORT midiInPacket = 0;
    for (size_t i = 0; i < interfaces.size(); ++i) {
        const auto& item = interfaces[i];
        const auto& d = item.descriptor;
        wprintf(L"  interface %u alt %u class %02X subclass %02X protocol %02X endpoints %u%ls\n", d.bInterfaceNumber, d.bAlternateSetting,
                d.bInterfaceClass, d.bInterfaceSubClass, d.bInterfaceProtocol, d.bNumEndpoints,
                d.bInterfaceClass == 1 && d.bInterfaceSubClass == 3 ? L"  (MIDIStreaming)" : d.bInterfaceClass == 1 && d.bInterfaceSubClass == 1 ? L"  (AudioControl)" : L"");
        for (const auto& pipe : item.pipes) {
            wprintf(L"      pipe %02X %hs max %u\n", pipe.endpoint, PipeTypeText(pipe.type), pipe.maxPacket);
            if (pipe.endpoint == kMidiInEndpoint && pipe.type == UsbdPipeTypeBulk) {
                midiIn = i == 0 ? first : associated[i - 1];
                midiInPacket = pipe.maxPacket;
            }
        }
    }
    wprintf(L"Bulk IN 0x%02X %ls\n", kMidiInEndpoint, midiIn ? L"is reachable in this function." : L"was NOT found in this function.");

    int exitCode = midiIn ? 0 : 3;
    std::vector<std::pair<double, std::vector<UCHAR>>> events;
    size_t totalEvents = 0, transfers = 0;
    std::set<std::pair<unsigned, unsigned>> distinct;
    DWORD readError = 0;
    if (capture && midiIn) {
        ULONG timeout = 200;
        WinUsb_SetPipePolicy(midiIn, kMidiInEndpoint, PIPE_TRANSFER_TIMEOUT, sizeof timeout, &timeout);
        UCHAR allowShort = TRUE;
        WinUsb_SetPipePolicy(midiIn, kMidiInEndpoint, ALLOW_PARTIAL_READS, sizeof allowShort, &allowShort);
        std::vector<UCHAR> buffer(std::max<USHORT>(midiInPacket, 64));
        LARGE_INTEGER frequency, start, now;
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&start);
        const LONGLONG deadline = start.QuadPart + static_cast<LONGLONG>(seconds) * frequency.QuadPart;
        wprintf(L"Capturing for %lu s. Move faders, knobs, jog wheels and buttons now. Ctrl+C stops early.\n", seconds);
        while (!g_stop) {
            QueryPerformanceCounter(&now);
            if (now.QuadPart >= deadline) break;
            ULONG got = 0;
            if (!WinUsb_ReadPipe(midiIn, kMidiInEndpoint, buffer.data(), static_cast<ULONG>(buffer.size()), &got, nullptr)) {
                DWORD e = GetLastError();
                if (e == ERROR_SEM_TIMEOUT) continue;
                readError = e;
                wprintf(L"Read failed: %lu (%ls)\n", e, ErrorText(e).c_str());
                break;
            }
            ++transfers;
            QueryPerformanceCounter(&now);
            double ms = 1000.0 * static_cast<double>(now.QuadPart - start.QuadPart) / static_cast<double>(frequency.QuadPart);
            for (ULONG offset = 0; offset + 4 <= got; offset += 4) {
                const UCHAR* p = buffer.data() + offset;
                if (!p[0] && !p[1] && !p[2] && !p[3]) continue;  // padding
                std::string text = DescribeEvent(p);
                wprintf(L"%9.1f ms  %02X %02X %02X %02X  %hs\n", ms, p[0], p[1], p[2], p[3], text.c_str());
                if (MidiBytesForCin(p[0]) >= 2) distinct.insert({p[1], p[2] & 0x7Fu});
                ++totalEvents;
                if (events.size() < kMaxLogged) events.push_back({ms, std::vector<UCHAR>(p, p + 4)});
            }
        }
        wprintf(L"\nRESULT: %zu USB-MIDI events in %zu transfers, %zu distinct status/data1 pairs.\n", totalEvents, transfers, distinct.size());
        if (readError) exitCode = 3;
        else if (totalEvents) { wprintf(L"PASS: the controller sends standard USB-MIDI on 0x83. A WinUSB user-mode bridge is viable.\n"); exitCode = 0; }
        else { wprintf(L"NO DATA: endpoint readable but silent. The controller probably needs a host message first.\n"); exitCode = 2; }
    }

    for (auto handle : associated) WinUsb_Free(handle);
    WinUsb_Free(first);
    CloseHandle(file);

    if (!output.empty()) {
        std::string json = "{\"tool\":\"vci400-winusb-probe\",\"version\":\"0.1.0\",\"mode\":" + JsonString(Utf8(argv[1])) +
                           ",\"device\":" + JsonString(Utf8(path)) + ",\"interfaces\":[";
        for (size_t i = 0; i < interfaces.size(); ++i) {
            const auto& d = interfaces[i].descriptor;
            json += std::string(i ? "," : "") + "{\"number\":" + std::to_string(d.bInterfaceNumber) + ",\"class\":" + std::to_string(d.bInterfaceClass) +
                    ",\"subclass\":" + std::to_string(d.bInterfaceSubClass) + ",\"pipes\":[";
            for (size_t p = 0; p < interfaces[i].pipes.size(); ++p) {
                const auto& pipe = interfaces[i].pipes[p];
                json += std::string(p ? "," : "") + "{\"endpoint\":" + std::to_string(pipe.endpoint) + ",\"type\":" + JsonString(PipeTypeText(pipe.type)) +
                        ",\"max_packet\":" + std::to_string(pipe.maxPacket) + "}";
            }
            json += "]}";
        }
        json += "],\"midi_in_reachable\":" + std::string(midiIn ? "true" : "false") + ",\"transfers\":" + std::to_string(transfers) +
                ",\"total_events\":" + std::to_string(totalEvents) + ",\"distinct_status_data1\":" + std::to_string(distinct.size()) +
                ",\"read_error\":" + std::to_string(readError) + ",\"events\":[";
        for (size_t i = 0; i < events.size(); ++i) {
            char line[96];
            const auto& b = events[i].second;
            snprintf(line, sizeof line, "%s{\"ms\":%.1f,\"bytes\":\"%02X%02X%02X%02X\"}", i ? "," : "", events[i].first, b[0], b[1], b[2], b[3]);
            json += line;
        }
        json += "],\"exit_code\":" + std::to_string(exitCode) + "}\n";
        FILE* out = nullptr;
        if (_wfopen_s(&out, output.c_str(), L"wb") == 0 && out) {
            fwrite(json.data(), 1, json.size(), out);
            fclose(out);
            wprintf(L"Report: %ls\n", output.c_str());
        }
    }
    return exitCode;
}
