param(
    [ValidateSet('Check', 'Stage', 'Apply')][string]$Mode = 'Check',
    [string]$Repository,
    [string]$CurrentVersion = '0.1.0',
    [string]$ExpectedVersion = '',
    [string]$Architecture = 'x64',
    [string]$StageDirectory = '',
    [string]$InstallDirectory = '',
    [int]$ParentProcessId = 0
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Console]::OutputEncoding = New-Object System.Text.UTF8Encoding($false)
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$updateRoot = Join-Path $env:LOCALAPPDATA 'scrcpy-pad\updates'

function Test-ChildPath([string]$Child, [string]$Parent) {
    $fullParent = [IO.Path]::GetFullPath($Parent).TrimEnd('\') + '\'
    return [IO.Path]::GetFullPath($Child).StartsWith($fullParent, [StringComparison]::OrdinalIgnoreCase)
}

function Get-ArchiveHash([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-', '') }
    finally { $sha.Dispose(); $stream.Dispose() }
}

try {
    if ($Mode -eq 'Apply') {
        # This helper runs from the staging directory, outside the files it replaces.
        if (!(Test-ChildPath $StageDirectory $updateRoot)) { throw 'Invalid update staging directory.' }
        $stage = [IO.Path]::GetFullPath($StageDirectory)
        $target = [IO.Path]::GetFullPath($InstallDirectory)
        if (!(Test-Path -LiteralPath (Join-Path $stage 'scrcpy-ui.exe') -PathType Leaf)) { throw 'Update executable is missing.' }
        if (!(Test-Path -LiteralPath (Join-Path $target 'scrcpy-ui.exe') -PathType Leaf)) { throw 'Installation was not found.' }
        if ($ParentProcessId -le 0) { throw 'Missing parent process.' }
        $parent = Get-Process -Id $ParentProcessId -ErrorAction SilentlyContinue
        if ($parent -and !$parent.WaitForExit(60000)) { throw 'The app did not exit in time.' }
        $backup = Join-Path $stage 'backup'
        New-Item -ItemType Directory -Path $backup -Force | Out-Null
        $copied = @()
        $backedUp = @()
        try {
            # Releases are flat portable packages; only executable/runtime files are installed.
            $files = @(Get-ChildItem -LiteralPath $stage -File | Where-Object { $_.Name -eq 'scrcpy-ui.exe' -or $_.Extension -eq '.dll' })
            foreach ($file in $files) {
                $destination = Join-Path $target $file.Name
                if (Test-Path -LiteralPath $destination) {
                    Copy-Item -LiteralPath $destination -Destination (Join-Path $backup $file.Name) -Force
                    $backedUp += $file.Name
                }
                $copied += $file.Name
                Copy-Item -LiteralPath $file.FullName -Destination $destination -Force
            }
            # The updater is shipped alongside the application in the portable ZIP.
            $newScript = Join-Path $stage 'update.ps1'
            if (Test-Path -LiteralPath $newScript) {
                $destination = Join-Path $target 'update.ps1'
                if (Test-Path -LiteralPath $destination) {
                    Copy-Item -LiteralPath $destination -Destination (Join-Path $backup 'update.ps1') -Force
                    $backedUp += 'update.ps1'
                }
                $copied += 'update.ps1'
                Copy-Item -LiteralPath $newScript -Destination $destination -Force
            }
        } catch {
            foreach ($name in $copied) {
                $destination = Join-Path $target $name
                if ($backedUp -contains $name) {
                    Copy-Item -LiteralPath (Join-Path $backup $name) -Destination $destination -Force
                } elseif (Test-Path -LiteralPath $destination) {
                    Remove-Item -LiteralPath $destination -Force
                }
            }
            throw
        }
        Set-Content -LiteralPath (Join-Path $updateRoot 'result.txt') -Value 'GUI update installed successfully.'
        Start-Process -FilePath (Join-Path $target 'scrcpy-ui.exe') -ArgumentList '--updated' -WorkingDirectory $target -WindowStyle Hidden
        exit 0
    }

    if ($Repository -notmatch '^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$' -or $Repository.Contains('..')) { throw 'Use owner/repository for GitHub updates.' }
    if ($Architecture -notin @('x64', 'arm64', 'x86')) { throw 'Unsupported architecture.' }
    $headers = @{ 'User-Agent' = 'scrcpy-pad-updater'; 'Accept' = 'application/vnd.github+json'; 'X-GitHub-Api-Version' = '2022-11-28' }
    try {
        $release = Invoke-RestMethod -Uri "https://api.github.com/repos/$Repository/releases/latest" -Headers $headers -TimeoutSec 30
    } catch {
        if ($_.Exception.Response -and [int]$_.Exception.Response.StatusCode -eq 404) {
            Write-Output '__NONE__|No published release found (or repository is private).'
            exit 0
        }
        throw
    }
    $tag = $release.tag_name -replace '^v', ''
    if ($tag -notmatch '^\d+\.\d+\.\d+(\.\d+)?$') { throw 'Release tags must use vMAJOR.MINOR.PATCH.' }
    if ([version]$tag -le [version]$CurrentVersion) { Write-Output '__NONE__|You are up to date.'; exit 0 }
    $assetName = "scrcpy-gui-windows-$Architecture.zip"
    $asset = $release.assets | Where-Object { $_.name -eq $assetName } | Select-Object -First 1
    if (!$asset) { throw "Release $tag has no $assetName asset." }
    if ($Mode -eq 'Check') { Write-Output "__AVAILABLE__|$tag"; exit 0 }
    if ($ExpectedVersion -and $ExpectedVersion -ne $tag) { throw 'The latest release changed. Check for updates again.' }
    $assetUri = [Uri]$asset.browser_download_url
    if ($assetUri.Scheme -ne 'https' -or $assetUri.Host -ne 'github.com' -or !$assetUri.AbsolutePath.StartsWith("/$Repository/releases/download/", [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Release download URL does not belong to the configured repository.'
    }
    # Fail closed when GitHub has not supplied a digest for the release asset.
    if ($asset.digest -notmatch '^sha256:([a-fA-F0-9]{64})$') { throw 'This release has no GitHub SHA-256 digest. Re-upload the release asset.' }
    $expectedHash = $Matches[1]
    if ($asset.size -le 0 -or $asset.size -gt 300MB) { throw 'Release archive has an unexpected size.' }
    $stage = Join-Path $updateRoot ([Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $stage -Force | Out-Null
    $archive = Join-Path $stage 'release.zip'
    Write-Output "[update] Downloading GUI $tag..."
    Invoke-WebRequest -UseBasicParsing -Uri $assetUri.AbsoluteUri -OutFile $archive -Headers $headers -TimeoutSec 180
    if ((Get-ArchiveHash $archive) -ne $expectedHash) { throw 'Release SHA-256 verification failed.' }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::OpenRead($archive)
    try {
        $totalSize = 0L
        foreach ($entry in $zip.Entries) {
            $totalSize += $entry.Length
            if ($totalSize -gt 700MB) { throw 'Expanded release is too large.' }
            # A flat archive avoids traversal, unexpected directories and overwriting user data.
            if ($entry.FullName -match '[/\\:]' -or $entry.FullName -eq '.' -or $entry.FullName -eq '..') { throw 'Release must contain a flat portable package.' }
            if ($entry.Name -notmatch '^(scrcpy-ui\.exe|update\.ps1|[^/\\:]+\.dll|LICENSE\.txt|README\.md)$') { throw "Unexpected release file: $($entry.Name)" }
            $destination = Join-Path $stage $entry.Name
            if (!(Test-ChildPath $destination $stage)) { throw 'Invalid archive entry.' }
            [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $destination, $false)
        }
    } finally { $zip.Dispose() }
    foreach ($required in @('scrcpy-ui.exe', 'slint_cpp.dll', 'update.ps1')) {
        if (!(Test-Path -LiteralPath (Join-Path $stage $required) -PathType Leaf)) { throw "Release is missing $required." }
    }
    Write-Output "__READY__|$stage"
} catch {
    $message = $_.Exception.Message
    Write-Output "[update] $message"
    if ($Mode -eq 'Apply') {
        New-Item -ItemType Directory -Path $updateRoot -Force | Out-Null
        Set-Content -LiteralPath (Join-Path $updateRoot 'result.txt') -Value "GUI update failed: $message"
        if ($InstallDirectory -and (Test-Path -LiteralPath (Join-Path $InstallDirectory 'scrcpy-ui.exe'))) {
            Start-Process -FilePath (Join-Path $InstallDirectory 'scrcpy-ui.exe') -ArgumentList '--updated' -WorkingDirectory $InstallDirectory -WindowStyle Hidden
        }
    }
    exit 1
}
