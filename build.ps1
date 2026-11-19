<#
.SYNOPSIS
    Builds every tool into .\bin (x64, static CRT). Needs Visual Studio 2022 or later (or Build
    Tools) with "Desktop development with C++" and a Windows 10/11 SDK. Installs nothing.
#>
[CmdletBinding()]
param([string]$Configuration = 'Release')
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$bin = Join-Path $root 'bin'
$obj = Join-Path $root 'obj'
New-Item -ItemType Directory -Force $bin, $obj | Out-Null

# Verify the embedded firmware against firmware\SHA256SUMS.txt before anything is built.
foreach ($line in Get-Content (Join-Path $root 'firmware\SHA256SUMS.txt')) {
    if ($line -notmatch '^([0-9A-Fa-f]{64})\s+\*?(\S+)$') { continue }
    $file = Join-Path $root "firmware\$($Matches[2])"
    if ((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -ne $Matches[1].ToUpper()) { throw "Checksum mismatch: $file" }
}
function Embed([string]$name, [string]$inc) {
    $bytes = [IO.File]::ReadAllBytes((Join-Path $root "firmware\$name"))
    $sb = [Text.StringBuilder]::new()
    for ($i = 0; $i -lt $bytes.Length; $i++) {
        [void]$sb.Append('0x').Append($bytes[$i].ToString('X2')).Append(',')
        if ($i % 24 -eq 23) { [void]$sb.AppendLine() }
    }
    Set-Content -LiteralPath (Join-Path $obj $inc) -Value $sb.ToString() -Encoding ASCII
}
Embed 'vci400_v21_AUDIO_midifix.bin' 'firmware_v21_midifix.inc'
Embed 'vci400_d006_AUDIO_midifix.bin' 'firmware_d006_midifix.inc'

# Import the x64 developer environment (compiler + Windows SDK).
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio C++ build tools not found.' }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
foreach ($line in (cmd /c "`"$vcvars`" >nul && set")) {
    if ($line -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] }
}

$common = @('/nologo', '/std:c++20', '/EHsc', '/W4', '/WX', '/O2', '/MT', '/permissive-', '/DUNICODE', '/D_UNICODE',
    '/D_WIN32_WINNT=0x0A00', '/DWINVER=0x0A00', '/DNTDDI_VERSION=0x0A000011', "/I$root\src\common", "/I$obj")
$targets = @(
    @{ Name = 'VCI400-FirmwareFix'; Src = 'src\firmware-fix\fwfix.cpp'; Libs = 'setupapi.lib newdev.lib cfgmgr32.lib winusb.lib ole32.lib shell32.lib winmm.lib advapi32.lib'; Admin = $true },
    @{ Name = 'fwpatch'; Src = 'src\fwpatch\fwpatch.cpp'; Libs = 'bcrypt.lib' },
    @{ Name = 'vci400-dfu'; Src = 'src\dfu\dfu.cpp'; Libs = 'setupapi.lib winusb.lib bcrypt.lib cfgmgr32.lib' },
    @{ Name = 'vci400-winusb-bind'; Src = 'src\winusb-bind\bind.cpp'; Libs = 'setupapi.lib newdev.lib cfgmgr32.lib advapi32.lib ole32.lib' },
    @{ Name = 'vci400-winusb-probe'; Src = 'src\winusb-tools\probe.cpp'; Libs = 'setupapi.lib winusb.lib' },
    @{ Name = 'vci400-bridge'; Src = 'src\winusb-tools\bridge.cpp'; Libs = 'setupapi.lib winusb.lib' }
)
foreach ($t in $targets) {
    $log = Join-Path $obj "$($t.Name).log"
    $link = @('/link') + ($t.Libs -split ' ')
    if ($t.Admin) { $link += @('/MANIFEST:EMBED', "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'") }
    & cl.exe @common "/Fo:$obj\$($t.Name).obj" "/Fe:$bin\$($t.Name).exe" (Join-Path $root $t.Src) @link *> $log
    if ($LASTEXITCODE -ne 0) { Get-Content $log -Tail 30; throw "FAIL $($t.Name) (see $log)" }
    Write-Output ("PASS {0,-20} {1}" -f $t.Name, (Get-FileHash "$bin\$($t.Name).exe").Hash)
}
