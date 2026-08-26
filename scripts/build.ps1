param(
    [ValidateSet('Debug','Release')]
    [string]$Configuration = 'Release',
    [switch]$Package
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
$cmake = Get-Command cmake -ErrorAction SilentlyContinue

if (-not $cmake -and (Test-Path $vswhere)) {
    $install = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($install) {
        $candidate = Join-Path $install 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
        if (Test-Path $candidate) { $cmake = Get-Item $candidate }
    }
}
if (-not $cmake) { throw 'CMake/Visual C++ Build Tools were not found.' }

$buildDir = Join-Path $repoRoot 'build\x64-release'
& $cmake.FullName -S $repoRoot -B $buildDir -G 'Visual Studio 17 2022' -A x64 -DBUILD_TESTING=ON
& $cmake.FullName --build $buildDir --config $Configuration --parallel
& $cmake.FullName --build $buildDir --config $Configuration --target RUN_TESTS

if ($Package) {
    $iscc = 'C:\Program Files (x86)\Inno Setup 6\ISCC.exe'
    if (-not (Test-Path $iscc)) { throw 'Inno Setup 6 was not found.' }
    & $iscc (Join-Path $repoRoot 'installer\CodexQuotaTray.iss') "/DConfiguration=$Configuration"
}

