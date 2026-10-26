param([string]$RunScript, [string]$WorkDirectory)
$ErrorActionPreference = 'Stop'
$testRoot = Join-Path $WorkDirectory ([Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testRoot -Force | Out-Null
$runner = Join-Path $testRoot 'run-case.ps1'
@'
param([string]$Case, [string]$Root)
$ErrorActionPreference = 'Stop'
$global:caseRoot = $Root
$global:caseName = $Case
function Get-Process { param($Name, $ErrorAction) }
function cmake {
    $build = Join-Path $global:caseRoot 'build'
    if ($args[0] -eq '--build') {
        if ($args[1] -ne $build -or $args[3] -ne 'Debug') { throw 'Wrong build path or configuration' }
        if ($args[4] -ne '--target' -or $args[5] -ne 'scrcpy-ui') { throw 'Run must build the app without unrelated test targets' }
        Add-Content -LiteralPath (Join-Path $global:caseRoot 'trace.txt') -Value 'build'
        $global:LASTEXITCODE = if ($global:caseName -eq 'build-failure') { 17 } else { 0 }
        return
    }
    if ($args[0] -ne '-S' -or $args[1] -ne $global:caseRoot -or $args[2] -ne '-B' -or $args[3] -ne $build) {
        throw 'Configuration used the caller directory instead of the script directory'
    }
    Add-Content -LiteralPath (Join-Path $global:caseRoot 'trace.txt') -Value 'configure'
    $global:LASTEXITCODE = if ($global:caseName -eq 'configure-failure') { 23 } else { 0 }
    $cache = if ($global:caseName -eq 'single-config-change' -and $args -notcontains '-DCMAKE_BUILD_TYPE=Debug') {
        'CMAKE_BUILD_TYPE:STRING=Release'
    } elseif ($global:caseName -like 'single-config*') { 'CMAKE_BUILD_TYPE:STRING=Debug' } else { 'CMAKE_CONFIGURATION_TYPES:STRING=Debug;Release' }
    Set-Content -LiteralPath (Join-Path $build 'CMakeCache.txt') -Value $cache
}
function Start-Process {
    param($FilePath, $WorkingDirectory)
    $relative = if ($global:caseName -like 'single-config*') { 'build/app/scrcpy-ui.exe' } else { 'build/app/Debug/scrcpy-ui.exe' }
    $expected = Join-Path $global:caseRoot $relative
    if ($FilePath -ne $expected -or $WorkingDirectory -ne (Split-Path -Parent $expected)) { throw 'Wrong launch path' }
    Add-Content -LiteralPath (Join-Path $global:caseRoot 'trace.txt') -Value 'launch'
}
Set-Location -LiteralPath (Split-Path -Parent $Root)
& (Join-Path $Root 'run.ps1')
exit $LASTEXITCODE
'@ | Set-Content -LiteralPath $runner

foreach ($case in @('configure-failure', 'build-failure', 'multi-config', 'single-config', 'single-config-change', 'missing-executable')) {
    $caseRoot = Join-Path $testRoot "$case with spaces"
    $app = Join-Path $caseRoot 'build/app/Debug'
    New-Item -ItemType Directory -Path $app -Force | Out-Null
    Copy-Item -LiteralPath $RunScript -Destination (Join-Path $caseRoot 'run.ps1')
    # A stale executable must never be launched after a failed configure/build.
    if ($case -ne 'missing-executable') {
        $relative = if ($case -like 'single-config*') { 'build/app/scrcpy-ui.exe' } else { 'build/app/Debug/scrcpy-ui.exe' }
        Set-Content -LiteralPath (Join-Path $caseRoot $relative) -Value 'fixture executable'
    }
    try {
        # Windows PowerShell wraps stderr as errors even for expected failures.
        $ErrorActionPreference = 'Continue'
        $output = & powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $runner -Case $case -Root $caseRoot 2>&1
        $code = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = 'Stop'
    }
    $trace = @(Get-Content -LiteralPath (Join-Path $caseRoot 'trace.txt')) -join ','
    $expectedTrace = if ($case -eq 'configure-failure') { 'configure' } elseif ($case -in @('build-failure', 'missing-executable')) { 'configure,build' } elseif ($case -eq 'single-config-change') { 'configure,configure,build,launch' } else { 'configure,build,launch' }
    $expectedCode = if ($case -in @('multi-config', 'single-config', 'single-config-change')) { 0 } else { 1 }
    if ($code -ne $expectedCode -or $trace -ne $expectedTrace) {
        throw "${case}: expected exit $expectedCode and '$expectedTrace'; got exit $code and '$trace'. $output"
    }
    Write-Output "PASS: $case"
}
