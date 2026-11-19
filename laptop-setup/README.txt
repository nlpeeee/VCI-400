VCI-400 + Serato DJ laptop setup
================================

1. Download the official ASIO4ALL installer (.exe) from https://asio4all.org and put it in the
   "vendor" folder. It isn't included.
2. Connect the VCI-400 (power adaptor on) and close Serato.
3. If the VCI-400 hasn't had the MIDI fix yet, run VCI400-FirmwareFix.exe first (option 1).
4. Double-click Setup.cmd and allow administrator rights. It:
   - removes the old, blocked Vestax driver packages (asks first);
   - clears leftover WinUSB bindings;
   - installs ASIO4ALL (checks that it's signed by its author);
   - registers it under the ASIO name "VCI-400";
   - optionally turns off USB selective suspend;
   - restarts the Windows MIDI service;
   - checks everything.
5. In Serato: Setup > Audio > ASIO panel. In ASIO4ALL, enable only the Vestax VCI-400, then set
   the buffer.

Run Setup.cmd again any time. It only changes what isn't already set up, and each run writes a
log next to the script.

WARNING: never run Vestax's FirmUp.exe (panel/MIDI firmware) while v21 audio firmware is
installed. Switch to d006 first with VCI400-FirmwareFix.exe (option 2).
