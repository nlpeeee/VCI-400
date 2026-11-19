#pragma once
#include <guiddef.h>

// Device interface GUIDs this project writes as DeviceInterfaceGUIDs so WinUSB registers
// an interface that our tools can open. Arbitrary, project-owned, one per bound target.

// MI_03 (MIDI function). Used by vci400-winusb-probe and vci400-bridge.
#define VCI400_WINUSB_INTERFACE_GUID_TEXT L"{8E3F0C51-6B2A-4D7E-9A15-1AA102310403}"
inline constexpr wchar_t kVci400WinUsbInterfaceGuidText[] = VCI400_WINUSB_INTERFACE_GUID_TEXT;
inline constexpr GUID kVci400WinUsbInterfaceGuid = {0x8e3f0c51, 0x6b2a, 0x4d7e, {0x9a, 0x15, 0x1a, 0xa1, 0x02, 0x31, 0x04, 0x03}};

// MI_05 (runtime DFU interface, class FE/01/01), for DFU_DETACH.
#define VCI400_WINUSB_DFU_RUNTIME_GUID_TEXT L"{8E3F0C51-6B2A-4D7E-9A15-1AA102310405}"
inline constexpr wchar_t kVci400WinUsbDfuRuntimeGuidText[] = VCI400_WINUSB_DFU_RUNTIME_GUID_TEXT;
inline constexpr GUID kVci400WinUsbDfuRuntimeGuid = {0x8e3f0c51, 0x6b2a, 0x4d7e, {0x9a, 0x15, 0x1a, 0xa1, 0x02, 0x31, 0x04, 0x05}};

// DFU-mode device (non-composite, single interface class FE/01/02) after DFU_DETACH.
#define VCI400_WINUSB_DFU_MODE_GUID_TEXT L"{8E3F0C51-6B2A-4D7E-9A15-1AA10231DF02}"
inline constexpr wchar_t kVci400WinUsbDfuModeGuidText[] = VCI400_WINUSB_DFU_MODE_GUID_TEXT;
inline constexpr GUID kVci400WinUsbDfuModeGuid = {0x8e3f0c51, 0x6b2a, 0x4d7e, {0x9a, 0x15, 0x1a, 0xa1, 0x02, 0x31, 0xdf, 0x02}};
