# Firmware notice

The files in `firmware/`, and the images embedded in `VCI400-FirmwareFix.exe`, are **Vestax
Corporation's VCI-400 audio firmware**. Vestax ceased trading in 2014. Serato and DJ TechTools
still host the original files for VCI-400 owners. They are **not** covered by this repository's
MIT licence, and all rights remain with their owners.

Each image was changed in exactly five bytes:
- the MIDI class-specific AudioControl header's `baInterfaceNr[0]`, from 1 to 4 (one byte);
- the containing tile's 32-bit CRC word, recomputed (four bytes).

| Published image | Original file | Original SHA-256 |
|---|---|---|
| `vci400_v21_AUDIO_midifix.bin` | `vci400_v21_AUDIO.bin` (Serato DJ firmware package, 2013) | `CF78438E11C7491B465763C25542C7EE0652C8FDD7F1752BAB628468C1E909C3` |
| `vci400_d006_AUDIO_midifix.bin` | `vci400_d006_AUDIO.bin` / `vci400_d006.bin` | `1EE2DCC240AACEAB813BAAE001CC90B5E621880F7B886C778D89F2B57533DFC8` |
| `vci400_v20_AUDIO_midifix.bin` | `vci400_v20.bin` (Firmware V2.0 package) | `E1E27A97A00D3531D06062BB6BD8F17A7B8545B2CF2B628EDF4102EA91A1ACF4` |

You can reproduce each image yourself from the original:

```
fwpatch patch vci400_v21_AUDIO.bin out.bin
```

The output must match `firmware/SHA256SUMS.txt`.

They're published only so owners of a discontinued controller can keep using it. If you hold
rights to this firmware and object, please open an issue.
