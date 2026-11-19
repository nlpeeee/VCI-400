# Technical notes

## The bug

The VCI-400 is a USB 2.0 composite device (VID `1AA1`, PID `0231`). Its interfaces are:

| Interface | Function |
|---|---|
| 0 | UAC2 AudioControl |
| 1 | UAC2 playback |
| 2 | UAC2 capture |
| 3 | MIDI AudioControl |
| 4 | MIDIStreaming (bulk OUT `0x02`, bulk IN `0x83`) |
| 5 | DFU runtime |

The MIDI AudioControl header reads:

```
09 24 01 00 01 09 00 01 01      <- baInterfaceNr[0] = 1, should be 4
```

The same bug exists in the XMOS USB Audio reference design of that era. XMOS fixed it in
`lib_xua` in 2024 ("baInterfaceNr field in MIDI Class-specific AC Interface Descriptor").
Windows' `usbmidi2` and `usbaudio` drivers follow the reference, find no MIDIStreaming interface,
and expose nothing (or fail with Code 10).

## Firmware image format (XMOS tools ≤ 12)

**Bit order.** Every byte in flash is bit-reversed (`sortBits` in XMOS `flashlib`). Words are
little-endian once reversed.

**Header words:**

| Word | Contents |
|---|---|
| 0 | tag `0x01A551E5` |
| 1 | page CRC |
| 2 | image CRC |
| 3 | features |
| 4 | version |
| 5 | size in bytes |
| 6 | tile count *N* |
| 7… | *N* × {offset, flags, words} |

**CRC step.** Every check uses the XS1 `crc32` instruction: 32 times,
`carry=d&1; d=(d>>1)|((s&1)<<31); s>>=1; if carry d^=0xEDB88320`.

**The three checks:**

| CRC | Computation | Valid when |
|---|---|---|
| Tile | `crc=~0`, feed `words-1`, then every tile word including the trailing CRC word | the result is `0xFFFFFFFF` |
| Image | `crc=~0`, feed words 4 … size/4−1, then word 2 | `~crc == 0` |
| Page | `crc=~0`, feed words 4 … 63, then word 1 | `~crc == 0` |

`fwpatch selftest` zeros every CRC word, re-solves them as a 32×32 GF(2) system, and reproduces
the shipped files byte for byte.

## DFU

**Interface 5** is standard DFU 1.1: functional descriptor `09 21 07 fa 00 40 00 10 01`, with
wTransferSize 64 and no WillDetach.

**Entering DFU mode:**
- The firmware enters DFU mode on `DFU_DETACH` followed by a USB reset within `wValue` ms. The
  firmware stores the timeout as `ms*100000` ticks in 32 bits, so keep it under 42 s.
- The tools reset the port with `IOCTL_USB_HUB_CYCLE_PORT`.
- In DFU mode the device keeps VID/PID and instance ID, but becomes non-composite (class
  FE/01/02). WinUSB is bound to it, and that driver install itself resets the device, so it must
  happen inside the DETACH window.
- XMOS's vendor `0xF2` (RESETINTODFU) is **not** usable on this firmware: the reboot hangs.

**Writing (DNLOAD):** 64-byte blocks with a GETSTATUS after each, then a zero-length DNLOAD. The
first block erases any existing upgrade image. UPLOAD reads only the upgrade image.

**Revert:** vendor `0xF1` (REVERTFACTORY) erases every upgrade image and doesn't reboot.

**v21 specifics:**
- On v21 the DETACH→DFU reboot hangs, so you can't enter DFU mode from v21. 0xF1 sent in
  **runtime** mode still erases the slot, even though it answers with a stall. After a
  power-cycle the factory image runs.
- The wizard does this automatically before flashing or restoring. Each step was proven by
  hand on the test unit; the automated sequence hasn't yet been run end to end.
