param([string]$UpdateScript, [string]$Fixture, [string]$WorkDirectory)
$ErrorActionPreference = 'Stop'
Import-Module Microsoft.PowerShell.Utility
$testRoot = Join-Path $WorkDirectory ([Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testRoot -Force | Out-Null
$testScript = Join-Path $testRoot 'run-case.ps1'
# Each case runs in a separate interpreter because the actual updater exits.
@'
param([string]$Case, [string]$UpdateScript, [string]$Fixture, [string]$Root)
$ErrorActionPreference = 'Stop'
Import-Module Microsoft.PowerShell.Utility
$env:LOCALAPPDATA = Join-Path $Root $Case
New-Item -ItemType Directory -Path $env:LOCALAPPDATA -Force | Out-Null
$payload = Join-Path $env:LOCALAPPDATA 'payload'
New-Item -ItemType Directory -Path $payload -Force | Out-Null
Copy-Item -LiteralPath $Fixture -Destination (Join-Path $payload 'scrcpy-ui.exe')
Set-Content -LiteralPath (Join-Path $payload 'slint_cpp.dll') -Value 'fixture runtime'
Copy-Item -LiteralPath $UpdateScript -Destination (Join-Path $payload 'update.ps1')
Add-Type -AssemblyName System.IO.Compression.FileSystem
$global:fixtureArchive = Join-Path $env:LOCALAPPDATA 'fixture.zip'
[IO.Compression.ZipFile]::CreateFromDirectory($payload, $global:fixtureArchive)
if ($Case -eq 'traversal') {
    $zip = [IO.Compression.ZipFile]::Open($global:fixtureArchive, [IO.Compression.ZipArchiveMode]::Update)
    $zip.CreateEntry('../escape.txt') | Out-Null
    $zip.Dispose()
}
$sha = [Security.Cryptography.SHA256]::Create()
$global:fixtureDigest = [BitConverter]::ToString($sha.ComputeHash([IO.File]::ReadAllBytes($global:fixtureArchive))).Replace('-', '')
$sha.Dispose()
if ($Case -eq 'bad-hash') { $global:fixtureDigest = '0' * 64 }
function Invoke-RestMethod {
    param($Uri, $Headers, $TimeoutSec)
    if ($Case -eq 'network') { throw 'fixture offline' }
    $tag = if ($Case -eq 'current') { 'v0.1.0' } else { 'v0.2.0' }
    $name = if ($Case -eq 'missing-asset') { 'other.zip' } else { 'scrcpy-gui-windows-x64.zip' }
    $digest = if ($Case -eq 'no-digest') { '' } else { "sha256:$global:fixtureDigest" }
    return [pscustomobject]@{ tag_name = $tag; assets = @([pscustomobject]@{
        name = $name; digest = $digest; size = (Get-Item -LiteralPath $global:fixtureArchive).Length
        browser_download_url = 'https://github.com/DeFerence3/scrcpy-gui-slint/releases/download/v0.2.0/scrcpy-gui-windows-x64.zip'
    }) }
}
function Invoke-WebRequest {
    param($Uri, $OutFile, $Headers, $TimeoutSec, [switch]$UseBasicParsing)
    Copy-Item -LiteralPath $global:fixtureArchive -Destination $OutFile
}
if ($Case -eq 'apply') {
    $stage = Join-Path $env:LOCALAPPDATA 'scrcpy-pad\updates\fixture'
    $target = Join-Path $env:LOCALAPPDATA 'installation'
    New-Item -ItemType Directory -Path $stage, $target -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $payload 'scrcpy-ui.exe') -Destination $stage
    Copy-Item -LiteralPath (Join-Path $payload 'slint_cpp.dll') -Destination $stage
    Copy-Item -LiteralPath (Join-Path $payload 'update.ps1') -Destination $stage
    Set-Content -LiteralPath (Join-Path $target 'scrcpy-ui.exe') -Value 'old executable'
    & $UpdateScript -Mode Apply -StageDirectory $stage -InstallDirectory $target -ParentProcessId 2147483646
    exit $LASTEXITCODE
}
$mode = if ($Case -in @('available', 'current', 'missing-asset', 'network')) { 'Check' } else { 'Stage' }
& $UpdateScript -Mode $mode -Repository 'DeFerence3/scrcpy-gui-slint' -CurrentVersion '0.1.0' -ExpectedVersion '0.2.0'
exit $LASTEXITCODE
'@ | Set-Content -LiteralPath $testScript

$shell = Join-Path $PSHOME 'powershell.exe'
foreach ($case in @('available', 'current', 'missing-asset', 'network', 'stage', 'bad-hash', 'no-digest', 'traversal', 'apply')) {
    $result = & $shell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $testScript -Case $case -UpdateScript $UpdateScript -Fixture $Fixture -Root $testRoot 2>&1
    $code = $LASTEXITCODE
    $output = $result -join "`n"
    $success = $case -in @('available', 'current', 'stage', 'apply')
    if (($success -and $code -ne 0) -or (!$success -and $code -eq 0)) { throw "Case $case returned $code : $output" }
    if ($case -eq 'available' -and !$output.Contains('__AVAILABLE__|0.2.0')) { throw 'Available release was not detected.' }
    if ($case -eq 'current' -and !$output.Contains('__NONE__|')) { throw 'Current release was not detected.' }
    if ($case -eq 'stage' -and !$output.Contains('__READY__|')) { throw 'Verified update was not staged.' }
    if ($case -eq 'apply') {
        $target = Join-Path $testRoot 'apply\installation'
        for ($attempt=0; $attempt -lt 20 -and !(Test-Path -LiteralPath (Join-Path $target 'restarted.txt')); $attempt++) { Start-Sleep -Milliseconds 100 }
        if (!(Test-Path -LiteralPath (Join-Path $target 'restarted.txt'))) { throw 'Updated fixture was not restarted.' }
        $sha = [Security.Cryptography.SHA256]::Create()
        $actual = [BitConverter]::ToString($sha.ComputeHash([IO.File]::ReadAllBytes((Join-Path $target 'scrcpy-ui.exe'))))
        $expected = [BitConverter]::ToString($sha.ComputeHash([IO.File]::ReadAllBytes($Fixture)))
        $sha.Dispose()
        if ($actual -ne $expected) { throw 'Wrong executable installed.' }
    }
    Write-Output "PASS: $case"
}
