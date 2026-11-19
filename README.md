# VCI-400 on Windows 11 without the Vestax driver

The **Vestax VCI-400** stopped working on current Windows 11. Its 2012 driver (Thesycon TUSBAudio
1.61) is cross-signed, and Windows has blocked such drivers since April 2026.

The controller doesn't need that driver for audio: Windows' built-in USB Audio 2.0 driver works.
MIDI is different. The firmware has a **one-byte mistake** in its USB MIDI descriptor:
`baInterfaceNr` points at interface 1 instead of 4. The old driver ignored it, but Windows' own
MIDI drivers don't. As a result:
- Windows creates no usable MIDI port.
- Serato can't pair the controller.

This project fixes that byte in Vestax's firmware and flashes it with standard USB DFU through
Microsoft's inbox WinUSB driver. There are **no drivers to install, no test-signing, and no
Secure Boot, HVCI or driver-policy changes.**

Tested on one standard VCI-400 with:
- Windows 11 25H2 x64;
- Serato DJ Pro 5.1;
- ASIO4ALL 2.22;
- patched audio firmware v21 and panel firmware 2.03.

Result: Serato pairs natively (`***CONNECTED*** … BcdDevice = 0x2100`) and plays with zero
dropouts.

## ⚠ Read before you start

> **Do NOT update the panel/MIDI firmware (Vestax `FirmUp.exe`) while the patched v21 (or v20)
> audio firmware is installed.**
>
> On our test unit, a panel update attempted with v21 loaded **erased the panel firmware**. The
> controls went dead, and the audio firmware could no longer enter update mode until the panel was
> repaired. Vestax's own procedure only updates the panel while audio firmware **d006** is loaded.
>
> Safe order:
> 1. `VCI400-FirmwareFix.exe` → option **2** (d006);
> 2. run FirmUp;
> 3. `VCI400-FirmwareFix.exe` → option **1** (v21).
>
> The panel chip's bootloader can't be overwritten over USB, so a failed panel update is
> recoverable. See [docs/troubleshooting.md](docs/troubleshooting.md).

- Firmware flashing always carries some risk. **Use at your own risk.** This project isn't
  affiliated with Vestax, Serato, XMOS, Thesycon or Microsoft.
- The VCI-400's **factory firmware is never written**. DFU only writes the separate "upgrade"
  slot, so option 3 ("Restore factory firmware") always takes you back. If writing stops partway,
  the unit starts its factory firmware.
- Keep the **power adaptor connected**, and don't unplug the controller while it's writing.
- The firmware images are Vestax's, with one descriptor byte and its checksum changed. See
  [FIRMWARE-NOTICE.md](FIRMWARE-NOTICE.md).

## Quick start (VCI-400 owners)

1. Download `VCI400-FirmwareFix.exe` from [Releases](../../releases). Windows SmartScreen may
   warn because it's unsigned; choose "More info → Run anyway".
2. Connect the VCI-400 by USB with its power adaptor on, and close all DJ software.
3. Run it and choose **1** to install **v21**, Vestax's firmware for Serato DJ. Follow the
   prompts; it asks you to switch the unit off and on once or twice.
4. When it says **DONE**, Windows shows a MIDI device called **"Vestax VCI-400"** and Serato
   pairs the controller.

The menu:

| Option | Firmware | Revision | Use |
|---|---|---|---|
| 1 | v21 (21.00) + MIDI fix | 0x2100 | Serato DJ (Vestax's final Serato firmware: deck C/D mixer mode, 48 kHz) |
| 2 | d006 (6.00) + MIDI fix | 0x0600 | Standard firmware. **Required before panel/MIDI firmware updates** |
| 3 | Factory | as shipped | Undo |

Backups of any previously stored firmware, and a log, go to `Documents\VCI-400 Firmware Fix`.

## Serato DJ laptop setup

`laptop-setup/` (also in the release zip) prepares a DJ laptop. It:
- removes the old, blocked Vestax driver packages;
- clears leftover WinUSB bindings;
- installs **ASIO4ALL**;
- registers it under the ASIO name **"VCI-400"**, the name Serato looks for;
- optionally turns off USB selective suspend;
- restarts the Windows MIDI service;
- checks everything.

ASIO4ALL is not included. Download the official installer from <https://asio4all.org>, put it in
`laptop-setup\vendor\`, then run `Setup.cmd`. Afterwards, in Serato, open the ASIO panel and
enable only the Vestax VCI-400 outputs.

## Tools

| Tool | Purpose |
|---|---|
| `VCI400-FirmwareFix.exe` | Guided firmware installer and undo, with both images embedded |
| `fwpatch.exe` | `verify` / `selftest` / `patch` XMOS tools≤12 images (checks every CRC) |
| `vci400-dfu.exe` | Low-level DFU: `status`, `detach`, `upload`, `download`, `revert-factory` |
| `vci400-winusb-bind.exe` | Bind or restore inbox WinUSB on MI_03, MI_05 or the DFU-mode device |
| `vci400-winusb-probe.exe` | Read raw USB-MIDI from interface 4 (diagnostics) |
| `vci400-bridge.exe` | WinUSB to virtual MIDI port bridge (needs loopMIDI's teVirtualMIDI). Gives MIDI even with the unpatched firmware, but not Serato pairing |

Build: run `.\build.ps1`. It needs Visual Studio 2022 or later C++ tools and a Windows SDK, and
outputs to `bin\`.

## How it works

See [docs/technical.md](docs/technical.md). In short:
- The images are XMOS tools≤12 flash images, stored bit-reversed.
- Each has three CRC layers (tile, image, page), all using the XS1 `crc32` with polynomial
  0xEDB88320.
- `fwpatch` changes the byte and re-solves the affected CRC. The published images differ from
  Vestax's in exactly 5 bytes.
- Flashing uses standard DFU 1.1: DETACH, a hub port reset, DNLOAD in 64-byte blocks, and the
  XMOS vendor request 0xF1 for revert.

## Licence

The tools and scripts are under the MIT licence (see [LICENSE](LICENSE)). The firmware images
aren't covered by it (see [FIRMWARE-NOTICE.md](FIRMWARE-NOTICE.md)).
