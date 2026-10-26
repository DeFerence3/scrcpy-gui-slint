[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo', 'MinSizeRel')]
    [string]$Configuration = 'Debug'
)

$ErrorActionPreference = 'Stop'
$buildDirectory = Join-Path $PSScriptRoot 'build'

try {
    if (-not (Get-Command cmake -CommandType Application -ErrorAction SilentlyContinue)) {
        throw 'CMake was not found on PATH. Install CMake and reopen the terminal.'
    }

    # Reconfigure even when build exists: an interrupted setup can leave only
    # a directory or an incomplete cache, without usable build files.
    Write-Host "Configuring $Configuration in $buildDirectory"
    & cmake -S $PSScriptRoot -B $buildDirectory `
        -DSLINT_FEATURE_BACKEND_QT=OFF `
        -DSLINT_FEATURE_RENDERER_SKIA=OFF `
        -DSLINT_FEATURE_RENDERER_SOFTWARE=ON
    if ($LASTEXITCODE -ne 0) {
        throw "CMake configuration failed (exit code $LASTEXITCODE). See the error above."
    }

    $appDirectory = Join-Path $buildDirectory 'app'
    $cachePath = Join-Path $buildDirectory 'CMakeCache.txt'
    $multiConfig = Select-String -LiteralPath $cachePath `
        -Pattern '^CMAKE_CONFIGURATION_TYPES:[^=]+=.+$' -Quiet
    if (-not $multiConfig -and -not (Select-String -LiteralPath $cachePath `
            -Pattern "^CMAKE_BUILD_TYPE:[^=]+=$Configuration$" -Quiet)) {
        & cmake -S $PSScriptRoot -B $buildDirectory "-DCMAKE_BUILD_TYPE=$Configuration"
        if ($LASTEXITCODE -ne 0) {
            throw "CMake configuration failed (exit code $LASTEXITCODE). See the error above."
        }
    }
    $executable = if ($multiConfig) {
        Join-Path $appDirectory "$Configuration/scrcpy-ui.exe"
    } else {
        Join-Path $appDirectory 'scrcpy-ui.exe'
    }

    # Stop this checkout's app and wait for Windows to release its binaries.
    $appPrefix = $appDirectory + [IO.Path]::DirectorySeparatorChar
    Get-Process -Name scrcpy-ui -ErrorAction SilentlyContinue | Where-Object {
        $_.Path -and $_.Path.StartsWith($appPrefix, [StringComparison]::OrdinalIgnoreCase)
    } | ForEach-Object {
        Write-Host "Stopping scrcpy-ui (PID $($_.Id))"
        $_ | Stop-Process -Force
        if (-not $_.WaitForExit(10000)) {
            throw "Timed out waiting for scrcpy-ui (PID $($_.Id)) to stop."
        }
    }

    Write-Host "Building scrcpy-ui ($Configuration)"
    & cmake --build $buildDirectory --config $Configuration --target scrcpy-ui --parallel
    if ($LASTEXITCODE -ne 0) {
        throw "Build failed (exit code $LASTEXITCODE). The app was not launched."
    }
    if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
        throw "The build succeeded but the executable is missing: $executable"
    }

    Write-Host "Launching $executable"
    Start-Process -FilePath $executable -WorkingDirectory (Split-Path -Parent $executable)
} catch {
    Write-Error $_ -ErrorAction Continue
    exit 1
}
