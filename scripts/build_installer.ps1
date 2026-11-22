# SPDX-FileCopyrightText: 2026 Cyberalien
# SPDX-License-Identifier: GPL-3.0-only

param(
    [string]$Compiler = '',
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$programFiles32 = [Environment]::GetEnvironmentVariable('ProgramFiles(x86)')
if (!$Compiler) {
    $found = Get-Command ISCC.exe -ErrorAction SilentlyContinue
    if ($found) {
        $Compiler = $found.Source
    } else {
        foreach ($candidate in @(
            (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe'),
            (Join-Path $programFiles32 'Inno Setup 6\ISCC.exe')
        )) {
            if (Test-Path -LiteralPath $candidate) { $Compiler = $candidate; break }
        }
    }
}
if (!$Compiler) { throw 'Install Inno Setup 6.7 or pass its ISCC.exe path with -Compiler.' }
$Compiler = (Resolve-Path -LiteralPath $Compiler).Path

$build = Join-Path $root 'build\release'
if (!$SkipBuild) {
    $cli = Join-Path $build 'CyModelHubCli.exe'
    if ((Test-Path -LiteralPath $cli) -and (Get-Process CyModelHubService -ErrorAction SilentlyContinue)) {
        & $cli stop-for-update
        if ($LASTEXITCODE -ne 0) { throw 'Stop the active service before building the installer.' }
    }
    & (Join-Path $PSScriptRoot 'build.ps1')
}

$service = Join-Path $build 'CyModelHubService.exe'
$version = & $service --version
if ($LASTEXITCODE -ne 0 -or $version -notmatch '^\d+\.\d+\.\d+$') { throw 'Cannot read the release build version.' }
$project = Get-Content -LiteralPath (Join-Path $root 'CMakeLists.txt') -Raw
if ($project -notmatch "project\(CyModelHub VERSION $([regex]::Escape($version)) ") {
    throw 'The release build version differs from the source version.'
}

$vswhere = Join-Path $programFiles32 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -version '[17.0,18.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'Visual Studio 2022 with the C++ tools is required.' }
$cmake = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$redist = Get-ChildItem -LiteralPath (Join-Path $vs 'VC\Redist\MSVC') -Directory |
    Where-Object { $_.Name -match '^\d+\.\d+\.\d+$' -and (Test-Path -LiteralPath (Join-Path $_.FullName 'x64\Microsoft.VC143.CRT')) } |
    Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
if (!$redist) { throw 'The x64 Visual C++ redistributable directory is missing.' }

$stage = Join-Path $root ("build\installer-stage-" + [Guid]::NewGuid().ToString('N'))
& $cmake --install $build --component Hub --prefix $stage
if ($LASTEXITCODE -ne 0) { throw 'The release build could not be staged.' }
if (!(Test-Path -LiteralPath (Join-Path $stage 'CyModelHub.exe') -PathType Leaf)) {
    throw 'The Windows desktop application is missing. Build without -NoApp before packaging.'
}
$crt = Join-Path $redist.FullName 'x64\Microsoft.VC143.CRT'
foreach ($file in @('msvcp140.dll', 'msvcp140_atomic_wait.dll', 'vcruntime140.dll', 'vcruntime140_1.dll')) {
    Copy-Item -LiteralPath (Join-Path $crt $file) -Destination (Join-Path $stage $file)
}

$out = Join-Path $root 'dist'
New-Item -Path $out -ItemType Directory -Force | Out-Null
& $Compiler "/DAppVersion=$version" "/DStagedFiles=$stage" "/O$out" (Join-Path $root 'packaging\windows\CyModelHub.iss')
if ($LASTEXITCODE -ne 0) { throw 'Inno Setup compilation failed.' }
Write-Output (Join-Path $out "CyModelHub-$version-windows-x64-setup.exe")
