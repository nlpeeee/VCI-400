#Requires -Version 5.1
<#
.SYNOPSIS
    Prepares a Windows 11 x64 DJ laptop so Serato DJ can use a Vestax VCI-400 for audio and MIDI
    without the old (blocked) Vestax driver.

.DESCRIPTION
    Idempotent. Run through Setup.cmd (it elevates). Steps:
      1. Refuse to run while Serato is open.
      2. Remove old Vestax/Thesycon driver packages (vci400v161*.inf), after confirmation.
      3. Clear leftover WinUSB bindings on the VCI-400 (from the firmware tools) so Windows'
         own drivers apply.
      4. Install ASIO4ALL silently from .\vendor (official installer, signature checked).
      5. Point the ASIO registration "VCI-400" (the name Serato uses) at ASIO4ALL.
      6. Optionally disable USB selective suspend on the active power plan.
      7. Restart the Windows MIDI service.
      8. Verify firmware, MIDI port and ASIO, then print the ASIO4ALL steps for Serato.
#>
[CmdletBinding()]
param([switch]$Yes)  # answer "yes" to every confirmation

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$log = Join-Path $here ("setup-log-{0:yyyyMMdd-HHmmss}.txt" -f (Get-Date))
Start-Transcript -LiteralPath $log | Out-Null

function Step([string]$text) { Write-Host "`n== $text" -ForegroundColor Cyan }
function Ok([string]$text) { Write-Host "   OK   $text" -ForegroundColor Green }
function Warn([string]$text) { Write-Host "   WARN $text" -ForegroundColor Yellow }
function Confirm([string]$question) {
    if ($Yes) { return $true }
    (Read-Host "   $question [Y/N]") -match '^[Yy]'
}

try {
    $principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Run Setup.cmd (it asks for administrator rights).' }
    if ([Environment]::OSVersion.Version.Build -lt 22000 -or -not [Environment]::Is64BitOperatingSystem) { Warn 'Designed for Windows 11 x64.' }

    Step 'Checking for running DJ software'
    while (Get-Process -Name 'Serato DJ Pro', 'Serato DJ Lite', 'vci400-bridge' -ErrorAction SilentlyContinue) {
        Read-Host '   Close Serato (and any VCI-400 tools), then press Enter'
    }
    Ok 'none running'

    Step 'Old Vestax driver packages'
    $packages = @()
    $current = @{}
    foreach ($line in (pnputil /enum-drivers)) {
        if ($line -match '^\s*Published Name:\s*(\S+)') { $current = @{ Published = $Matches[1] } }
        elseif ($line -match '^\s*Original Name:\s*(\S+)') { $current.Original = $Matches[1]; if ($current.Original -like 'vci400v*') { $packages += [pscustomobject]$current } }
    }
    if ($packages) {
        $packages | ForEach-Object { Write-Host "   found $($_.Published) ($($_.Original))" }
        if (Confirm 'Remove these blocked Vestax driver packages so Windows uses its own drivers?') {
            foreach ($p in $packages) { pnputil /delete-driver $p.Published /uninstall /force | Out-Null; Ok "removed $($p.Published)" }
        } else { Warn 'kept; the VCI-400 may not work while they are installed' }
    } else { Ok 'none installed' }

    Step 'VCI-400 driver bindings'
    $vci = @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object InstanceId -match 'VID_1AA1&PID_0231')
    if (-not $vci) { Warn 'VCI-400 not connected; connect it (with its power adaptor) and run setup again to verify' }
    else {
        $winusb = $vci | Where-Object { (Get-PnpDeviceProperty -InstanceId $_.InstanceId -KeyName DEVPKEY_Device_Service -ErrorAction SilentlyContinue).Data -eq 'WinUSB' }
        if ($winusb) {
            Write-Host "   WinUSB is bound to: $($winusb.InstanceId -join ', ')"
            $root = $vci | Where-Object InstanceId -notmatch '&MI_' | Select-Object -First 1
            pnputil /remove-device $root.InstanceId /subtree | Out-Null
            pnputil /scan-devices | Out-Null
            Start-Sleep 5
            Ok 'device reset; Windows reapplied its own drivers'
        } else { Ok 'Windows drivers in use' }
    }

    Step 'ASIO4ALL'
    $hklm = [Microsoft.Win32.RegistryKey]::OpenBaseKey('LocalMachine', 'Registry64')
    $asioRoot = $hklm.OpenSubKey('SOFTWARE\ASIO', $true)
    if (-not $asioRoot) { $asioRoot = $hklm.CreateSubKey('SOFTWARE\ASIO') }
    $asio4all = $asioRoot.GetSubKeyNames() | Where-Object { $_ -like 'ASIO4ALL*' } | Select-Object -First 1
    if (-not $asio4all) {
        $installer = Get-ChildItem (Join-Path $here 'vendor'), (Join-Path $here '..\vendor') -Filter '*.exe' -ErrorAction SilentlyContinue |
            Where-Object Name -match 'asio4all' | Select-Object -First 1
        if (-not $installer) { throw 'Put the official ASIO4ALL installer (.exe) in the vendor folder, then run setup again.' }
        $sig = Get-AuthenticodeSignature -LiteralPath $installer.FullName
        Write-Host "   installer $($installer.Name), signature: $($sig.Status) $($sig.SignerCertificate.Subject)"
        # Official ASIO4ALL installers are signed by their author, Michael Tippach (verified for 2.22).
        $trusted = $sig.Status -eq 'Valid' -and $sig.SignerCertificate.Subject -match 'CN=Michael Tippach'
        if (-not $trusted -and -not (Confirm 'The installer is not signed by Michael Tippach (ASIO4ALL author). Run it anyway?')) { throw 'ASIO4ALL not installed.' }
        $proc = Start-Process -FilePath $installer.FullName -ArgumentList '/S' -PassThru -Wait
        Start-Sleep 2
        $asio4all = $asioRoot.GetSubKeyNames() | Where-Object { $_ -like 'ASIO4ALL*' } | Select-Object -First 1
        if (-not $asio4all) {
            Warn "silent install did not register ASIO4ALL (exit $($proc.ExitCode)); starting the normal installer"
            Start-Process -FilePath $installer.FullName -Wait
            $asio4all = $asioRoot.GetSubKeyNames() | Where-Object { $_ -like 'ASIO4ALL*' } | Select-Object -First 1
        }
        if (-not $asio4all) { throw 'ASIO4ALL is not registered after installation.' }
    }
    $clsid = [string]$asioRoot.OpenSubKey($asio4all).GetValue('CLSID')
    $server = $hklm.OpenSubKey("SOFTWARE\Classes\CLSID\$clsid\InprocServer32")
    if (-not $server) { throw "$asio4all has no 64-bit COM registration; reinstall ASIO4ALL." }
    Ok "$asio4all $clsid $([string]$server.GetValue(''))"

    Step 'Serato ASIO name "VCI-400"'
    $alias = $asioRoot.OpenSubKey('VCI-400')
    $before = if ($alias) { [string]$alias.GetValue('CLSID') } else { '' }
    if ($before -ne $clsid) {
        $alias = $asioRoot.CreateSubKey('VCI-400')
        $alias.SetValue('CLSID', $clsid, 'String')
        $alias.SetValue('Description', 'VCI-400', 'String')
        Ok "VCI-400 -> $asio4all (was: $(if ($before) { $before } else { 'not set' }))"
    } else { Ok "VCI-400 already -> $asio4all" }

    Step 'USB power saving'
    $usbSuspend = '2a737441-1930-4402-8d77-b2bebba308a3', '48e6b7a6-50f5-4782-a5d4-53bb8f07e226'
    if (Confirm 'Disable USB selective suspend on the active power plan (recommended for DJ use)?') {
        powercfg /setacvalueindex SCHEME_CURRENT $usbSuspend[0] $usbSuspend[1] 0 | Out-Null
        powercfg /setdcvalueindex SCHEME_CURRENT $usbSuspend[0] $usbSuspend[1] 0 | Out-Null
        powercfg /setactive SCHEME_CURRENT | Out-Null
        Ok 'USB selective suspend disabled'
    } else { Warn 'left unchanged' }

    Step 'Windows MIDI service'
    Restart-Service midisrv -ErrorAction SilentlyContinue
    Start-Sleep 4
    Ok 'restarted'

    Step 'Verification'
    $root = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object InstanceId -match '^USB\\VID_1AA1&PID_0231\\' | Select-Object -First 1
    if ($root) {
        $rev = ((Get-PnpDeviceProperty -InstanceId $root.InstanceId -KeyName DEVPKEY_Device_HardwareIds).Data[0] -split '&')[-1]
        if ($rev -in 'REV_2100', 'REV_2000') { Ok "firmware $rev (MIDI-fixed builds use 2100/2000)" }
        else { Warn "firmware ${rev}: run VCI400-FirmwareFix.exe from this package to install the MIDI-fixed firmware" }
    }
    Add-Type -Namespace Vci -Name Mm -MemberDefinition @'
[StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)] public struct InCaps { public ushort wMid, wPid; public uint ver; [MarshalAs(UnmanagedType.ByValTStr, SizeConst=32)] public string name; public uint support; }
[DllImport("winmm.dll")] public static extern uint midiInGetNumDevs();
[DllImport("winmm.dll", CharSet=CharSet.Unicode)] public static extern uint midiInGetDevCapsW(UIntPtr id, out InCaps c, uint size);
[DllImport("winmm.dll")] public static extern uint midiInMessage(IntPtr h, uint msg, IntPtr p1, IntPtr p2);
'@
    $found = $false
    for ($i = 0; $i -lt [Vci.Mm]::midiInGetNumDevs(); $i++) {
        $caps = New-Object Vci.Mm+InCaps
        [void][Vci.Mm]::midiInGetDevCapsW([UIntPtr][uint32]$i, [ref]$caps, 76)
        if ($caps.name -notlike '*VCI-400*') { continue }
        $size = [Runtime.InteropServices.Marshal]::AllocHGlobal(4); $buf = [Runtime.InteropServices.Marshal]::AllocHGlobal(2048)
        $path = ''
        if (-not [Vci.Mm]::midiInMessage([IntPtr]$i, 0x80D, $size, [IntPtr]0)) {
            $n = [Runtime.InteropServices.Marshal]::ReadInt32($size)
            if ($n -gt 0 -and $n -le 2048 -and -not [Vci.Mm]::midiInMessage([IntPtr]$i, 0x80C, $buf, [IntPtr]$n)) { $path = [Runtime.InteropServices.Marshal]::PtrToStringUni($buf) }
        }
        [Runtime.InteropServices.Marshal]::FreeHGlobal($size); [Runtime.InteropServices.Marshal]::FreeHGlobal($buf)
        if ($path -match 'vid_1aa1&pid_0231') { Ok "MIDI port '$($caps.name)' on the USB device (Serato can pair it)"; $found = $true }
        else { Warn "MIDI port '$($caps.name)' is not on the USB device ($path)" }
    }
    if (-not $found) { Warn 'no USB VCI-400 MIDI port; check the firmware, then unplug/replug the VCI-400' }

    Write-Host @'

Next, in Serato DJ (one time):
  1. Connect the VCI-400 (power adaptor on) and open Serato. It should connect to the VCI-400.
  2. Setup > Audio: the device is "VCI-400" (ASIO4ALL). Click the ASIO panel button.
  3. In ASIO4ALL, switch ON only the Vestax VCI-400 outputs (and inputs if used); switch every
     other device OFF. Start with a 256-448 sample buffer and lower it while playback stays clean.
'@
    Write-Host "Log: $log" -ForegroundColor Cyan
} catch {
    Write-Host "`nSETUP STOPPED: $($_.Exception.Message)" -ForegroundColor Red
    Write-Host "Log: $log"
    exit 1
} finally {
    Stop-Transcript | Out-Null
}
