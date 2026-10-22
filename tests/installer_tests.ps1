param([string]$Template, [string]$MakeNsis, [string]$Fixture, [string]$WorkDirectory)
$ErrorActionPreference = 'Stop'
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class TrayFixture {
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr FindWindow(string name, string title);
}
'@
if ([TrayFixture]::FindWindow('ScrcpyTrayMsgWnd', $null) -ne [IntPtr]::Zero) {
    Write-Output 'Quit the running tray app to test the installer.'
    exit 77
}
$name = 'scrcpy-installer-test-' + [Guid]::NewGuid().ToString('N')
$testRoot = Join-Path ([IO.Path]::GetFullPath($WorkDirectory)) $name
$payload = Join-Path $testRoot 'payload'
$installDir = Join-Path $testRoot 'installed app with spaces'
New-Item -ItemType Directory -Path $payload -Force | Out-Null
Copy-Item -LiteralPath $Fixture -Destination (Join-Path $payload 'scrcpy-ui.exe')
Set-Content -LiteralPath (Join-Path $testRoot 'license.txt') -Value 'Installer test fixture'
$values = @{
    CPACK_NSIS_PACKAGE_NAME = $name
    CPACK_NSIS_DISPLAY_NAME = $name
    CPACK_PACKAGE_INSTALL_DIRECTORY = $name
    CPACK_PACKAGE_INSTALL_REGISTRY_KEY = $name
    CPACK_PACKAGE_VENDOR = 'scrcpy-gui test'
    CPACK_PACKAGE_VERSION = '0.1.0'
    CPACK_TEMPORARY_DIRECTORY = $payload
    CPACK_TOPLEVEL_DIRECTORY = $testRoot
    CPACK_OUTPUT_FILE_NAME = 'setup.exe'
    CPACK_RESOURCE_FILE_LICENSE = (Join-Path $testRoot 'license.txt')
    CPACK_NSIS_FULL_INSTALL = 'File /r "${INST_DIR}\*.*"'
    CPACK_NSIS_DELETE_FILES = 'Delete "$INSTDIR\scrcpy-ui.exe"'
    CPACK_NSIS_DELETE_DIRECTORIES = ''
}
$script = Get-Content -LiteralPath $Template -Raw
foreach ($key in $values.Keys) { $script = $script.Replace("@$key@", $values[$key]) }
if ($script -match '@CPACK_') { throw 'Unexpanded installer template variable.' }
$scriptPath = Join-Path $testRoot 'installer.nsi'
Set-Content -LiteralPath $scriptPath -Value $script -Encoding UTF8
& $MakeNsis /V2 $scriptPath
if ($LASTEXITCODE -ne 0) { throw 'Could not compile installer fixture.' }
$setup = Join-Path $testRoot 'setup.exe'
$runKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
$uninstallKey = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\$name"
$shortcuts = Join-Path ([Environment]::GetFolderPath('Programs')) $name
$expected = '"' + (Join-Path $installDir 'scrcpy-ui.exe') + '" --startup'

function Run-Setup {
    # NSIS requires /D last, with no quotes around the path even with spaces.
    $process = Start-Process -FilePath $setup -ArgumentList "/S /D=$installDir" -WindowStyle Hidden -PassThru
    if (!$process.WaitForExit(15000)) { $process.Kill(); throw 'Setup timed out.' }
    return $process.ExitCode
}
function Run-Uninstall {
    $process = Start-Process -FilePath (Join-Path $installDir 'Uninstall.exe') -ArgumentList "/S _?=$installDir" -WindowStyle Hidden -PassThru
    if (!$process.WaitForExit(15000)) { $process.Kill(); throw 'Uninstall timed out.' }
    return $process.ExitCode
}
function Get-Startup { return (Get-ItemProperty -LiteralPath $runKey -ErrorAction SilentlyContinue).$name }
function Require-Installed {
    if ((Get-Startup) -ne $expected) { throw 'Startup command is missing, incorrect, or unquoted.' }
    if ((Get-ItemProperty -LiteralPath $uninstallKey).InstallLocation -ne $installDir) { throw 'Per-user uninstall registration is missing.' }
    if (!(Test-Path -LiteralPath (Join-Path $shortcuts 'scrcpy-gui.lnk'))) { throw 'Start menu shortcut is missing.' }
    $marker = Join-Path $installDir 'started.txt'
    for ($i = 0; $i -lt 50 -and !(Test-Path -LiteralPath $marker); $i++) { Start-Sleep -Milliseconds 100 }
    if (!(Test-Path -LiteralPath $marker) -or (Get-Content -LiteralPath $marker -Raw) -ne '--startup') { throw 'Installer did not launch the app quietly.' }
    foreach ($process in @(Get-Process scrcpy-ui -ErrorAction SilentlyContinue)) {
        if ($process.Path -eq (Join-Path $installDir 'scrcpy-ui.exe') -and !$process.WaitForExit(5000)) {
            throw 'Startup fixture did not exit.'
        }
    }
}
function Start-TrayFixture {
    $process = Start-Process -FilePath $Fixture -ArgumentList '--tray' -WindowStyle Hidden -PassThru
    for ($i = 0; $i -lt 50; $i++) {
        if ([TrayFixture]::FindWindow('ScrcpyTrayMsgWnd', $null) -ne [IntPtr]::Zero) { return $process }
        Start-Sleep -Milliseconds 100
    }
    $process.Kill()
    throw 'Tray fixture did not start.'
}
$tray = $null
try {
    $tray = Start-TrayFixture
    if ((Run-Setup) -eq 0 -or (Get-Startup)) { throw 'Setup should refuse a running tray app before registering startup.' }
    $tray.Kill(); $tray.WaitForExit(); $tray = $null
    Write-Output 'PASS: running-app installation failure leaves startup unchanged'

    if ((Run-Setup) -ne 0) { throw 'Installation failed.' }
    Require-Installed
    Write-Output 'PASS: per-user installation registers quoted startup and launches immediately'

    Remove-Item -LiteralPath (Join-Path $installDir 'started.txt')
    if ((Run-Setup) -ne 0) { throw 'Reinstallation failed.' }
    Require-Installed
    Write-Output 'PASS: reinstallation preserves one startup entry and relaunches'

    $tray = Start-TrayFixture
    if ((Run-Uninstall) -eq 0) { throw 'Uninstall should refuse a running tray app.' }
    Require-Installed
    $tray.Kill(); $tray.WaitForExit(); $tray = $null
    Write-Output 'PASS: running-app uninstall failure preserves installation'

    $newer = '"C:\newer installation\scrcpy-ui.exe" --startup'
    Set-ItemProperty -LiteralPath $runKey -Name $name -Value $newer
    if ((Run-Uninstall) -ne 0) { throw 'Uninstallation failed.' }
    if ((Get-Startup) -ne $newer) { throw 'Old uninstall removed another installation startup entry.' }
    Write-Output 'PASS: older uninstall preserves newer startup registration'

    Remove-Item -LiteralPath (Join-Path $installDir 'started.txt')
    if ((Run-Setup) -ne 0) { throw 'Installation for removal test failed.' }
    Require-Installed
    if ((Run-Uninstall) -ne 0) { throw 'Final uninstallation failed.' }
    if ((Get-Startup) -or (Test-Path -LiteralPath $uninstallKey)) { throw 'Uninstall left registration behind.' }
    if (Test-Path -LiteralPath (Join-Path $installDir 'scrcpy-ui.exe')) { throw 'Uninstall left executable behind.' }
    if (Test-Path -LiteralPath $shortcuts) { throw 'Uninstall left shortcuts behind.' }
    if (!(Test-Path -LiteralPath (Join-Path $installDir 'started.txt'))) { throw 'Uninstall removed an unrelated file.' }
    Write-Output 'PASS: uninstall removes startup and app files while preserving unrelated files'
} finally {
    if ($tray -and !$tray.HasExited) { $tray.Kill(); $tray.WaitForExit() }
    # Only the uniquely named fixture registration and shortcuts are touched.
    Remove-ItemProperty -LiteralPath $runKey -Name $name -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $uninstallKey -ErrorAction SilentlyContinue
    foreach ($file in @('scrcpy-gui.lnk', 'Uninstall.lnk')) {
        Remove-Item -LiteralPath (Join-Path $shortcuts $file) -ErrorAction SilentlyContinue
    }
    if (Test-Path -LiteralPath $shortcuts) { [IO.Directory]::Delete($shortcuts) }
}
