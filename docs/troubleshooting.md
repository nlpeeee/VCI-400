# Troubleshooting

## "The VCI-400 did not enter update mode" / "DFU_DETACH was refused"

1. Switch the VCI-400 off. Then unplug **both** the USB cable and the power adaptor for 30 s,
   reconnect, and run the tool again. One attempt is enough. Don't run other tools in between,
   because a refused request can leave the update handler stuck until the next power-off.
2. Make sure the controls send MIDI (or the panel firmware is healthy). On our unit, update mode
   stayed unreachable while the panel firmware was erased, and worked again once it was
   repaired. See below.
3. On **v21**, update mode can't be entered at all. To leave v21:
   1. Run `vci400-winusb-bind bind --interface MI_05` (admin).
   2. Run `vci400-dfu revert-factory --confirm --runtime`. It may report error 31, but the
      erase still happens.
   3. Power-cycle. The unit comes up as `REV_0600` (factory).

## Panel/MIDI firmware update failed (controls dead, FirmUp says "no reply")

The panel chip (Renesas H8) keeps its bootloader, so it can be re-flashed. FirmUp needs a
working MIDI port to reach it.

- **With patched d006 loaded** (option 2): use the native "Vestax VCI-400" port.
- **With factory firmware and no MIDI port:** use the bridge.
  1. Run `vci400-winusb-bind bind --interface MI_03` (admin).
  2. Start `vci400-bridge --log traffic.log`. This needs teVirtualMIDI, which comes with
     loopMIDI.
  3. Run `Restart-Service midisrv` (admin). A port named "VCI-400" appears.
- **Then flash:**
  1. Power on while holding **F1+F3**. These are the buttons under the 1st and 3rd effect knobs
     of the left deck.
  2. In FirmUp, select the port for both MIDI In and Out, choose `VCI400up000.bin` from the
     extracted `VCI400up_0203_MIDI` folder, and click **Start**.
- **What the log should show:** 226 blocks out, and an ACK `F0 00 40 02 00 30 02 43 F7` back for
  each one.
- **"Get Ver" in bootloader mode on an erased panel** returns version `FF FF FF FF`. The XMOS
  splits those bytes out of the SysEx, so FirmUp shows "no reply". That's expected.

Afterwards, power-cycle normally and install the audio firmware you want with
`VCI400-FirmwareFix.exe`.

## No "Vestax VCI-400" MIDI port after flashing

Restart the Windows MIDI service (`Restart-Service midisrv`, admin), or unplug and replug the
controller. The wizard does both automatically at the end.
