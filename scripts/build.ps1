# SPDX-FileCopyrightText: 2026 Cyberalien
# SPDX-License-Identifier: GPL-3.0-only

# Windows build helper: finds Visual Studio 2022 (CMake and Ninja included), configures, builds
# and runs the tests. Sibling checkouts of CyMCPSDK, CyUI and SDL3 are used when present, so the
# build needs no network access.
#
#   ./scripts/build.ps1                 # service, client, CLI, MCP relay, application, tests
#   ./scripts/build.ps1 -NoApp          # everything but the desktop application
#   ./scripts/build.ps1 -Config Debug

param(
    [ValidateSet('Release', 'Debug', 'RelWithDebInfo')][string]$Config = 'Release',
    [switch]$NoApp,
    [switch]$NoTests,
    [string]$SDKRoot = '',
    [string]$CyUIRoot = '',
    [string]$SDLRoot = ''
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { throw 'Visual Studio 2022 with the C++ tools is required.' }
$vs = & $vswhere -latest -version '[17.0,18.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio 2022 with the C++ tools is required.' }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
$cmakeBin = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin'
$ninja = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
$cmake = Join-Path $cmakeBin 'cmake.exe'
$ctest = Join-Path $cmakeBin 'ctest.exe'

function Resolve-Checkout([string]$explicit, [string[]]$candidates) {
    if ($explicit) { return (Resolve-Path $explicit).Path }
    foreach ($candidate in $candidates) {
        $path = Join-Path (Split-Path -Parent $root) $candidate
        if (Test-Path (Join-Path $path 'CMakeLists.txt')) { return $path }
    }
    return ''
}

$build = Join-Path $root "build\$($Config.ToLower())"
$arguments = @('-S', $root, '-B', $build, '-G', 'Ninja', "-DCMAKE_BUILD_TYPE=$Config", "-DCMAKE_MAKE_PROGRAM=$ninja")
$sdk = Resolve-Checkout $SDKRoot @('CyMCPSDK')
if ($sdk) { $arguments += "-DFETCHCONTENT_SOURCE_DIR_CYMCPSDK=$($sdk -replace '\\', '/')" }
if ($NoApp) {
    $arguments += '-DCYMODELHUB_BUILD_APP=OFF'
} else {
    $arguments += '-DCYMODELHUB_BUILD_APP=ON'
    $cyui = Resolve-Checkout $CyUIRoot @('CyUI', 'CyImgui_Interface')
    if ($cyui) { $arguments += "-DFETCHCONTENT_SOURCE_DIR_CYUI=$($cyui -replace '\\', '/')" }
    $sdl = Resolve-Checkout $SDLRoot @('SDL', 'CyMCPHub\.deps\SDL3')
    if ($sdl) { $arguments += "-DFETCHCONTENT_SOURCE_DIR_SDL3=$($sdl -replace '\\', '/')" }
}
$arguments += "-DCYMODELHUB_BUILD_TESTS=$(if ($NoTests) { 'OFF' } else { 'ON' })"

$quoted = ($arguments | ForEach-Object { '"' + $_ + '"' }) -join ' '
$commands = "`"$vcvars`" >nul 2>&1 &&`"$cmake`" $quoted && `"$cmake`" --build `"$build`" --parallel"
if (-not $NoTests) { $commands += " && `"$ctest`" --test-dir `"$build`" --output-on-failure --no-tests=error" }
$env:VSLANG = '1033'
cmd /c $commands
if ($LASTEXITCODE -ne 0) { throw "Build failed with exit code $LASTEXITCODE." }
Write-Host "Build output: $build"
