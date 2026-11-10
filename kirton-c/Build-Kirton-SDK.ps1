# SPDX-License-Identifier: MPL-2.0
# This Source Code Form is subject to the terms of the Mozilla Public License v. 2.0. See https://mozilla.org/MPL/2.0.
param(
    [string]$Generator = "Ninja",
    [string]$BuildRoot = "build-sdk-release",
    [string]$TestBuildRoot = "build-sdk-test",
    [string]$InstallRoot = "build-sdk-install",
    [string]$DistRoot = "dist",
    [string]$InstallerBundleRoot = "build-sdk-installer-bundle",
    [string]$Version,
    [string]$PackagePrefix = "Kirton-SDK",
    [switch]$SkipTests
)
$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
if ([string]::IsNullOrWhiteSpace($Version)) {
    $Version = (Get-Content -Raw (Join-Path $root "VERSION")).Trim()
}
$version = $Version.Trim()
$arch = [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture
if ($arch -ne "X64") { throw "Windows SDK packaging requires an x64 host; found $arch" }
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) { throw "CMake is required" }

function Resolve-RootedPath {
    param([string]$Path)
    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }
    return Join-Path $root $Path
}

$build = Resolve-RootedPath $BuildRoot
$testBuild = Resolve-RootedPath $TestBuildRoot
if ([System.IO.Path]::IsPathRooted($DistRoot)) {
    $dist = [System.IO.Path]::GetFullPath($DistRoot)
} else {
    $dist = Join-Path $root $DistRoot
}
if ([System.IO.Path]::IsPathRooted($InstallerBundleRoot)) {
    $installerBundle = [System.IO.Path]::GetFullPath($InstallerBundleRoot)
} else {
    $installerBundle = Join-Path $root $InstallerBundleRoot
}
$sdkName = "$PackagePrefix-$version-windows-x64"
$package = Join-Path $dist $sdkName
$install = Resolve-RootedPath $InstallRoot
$zip = Join-Path $dist "$sdkName.zip"
New-Item -ItemType Directory -Force -Path $dist | Out-Null
foreach ($path in @($package, $zip, $install, $installerBundle)) {
    if (Test-Path -LiteralPath $path) {
        throw "Refusing to overwrite existing SDK candidate path: $path"
    }
}

function Invoke-CMake {
    param([string[]]$Arguments)
    & cmake @Arguments
    if ($LASTEXITCODE -ne 0) { throw "cmake $Arguments failed with exit code $LASTEXITCODE" }
}
function Invoke-CTest {
    param([string[]]$Arguments)
    & ctest @Arguments
    if ($LASTEXITCODE -ne 0) { throw "ctest $Arguments failed with exit code $LASTEXITCODE" }
}

$configureArgs = @("-S", $root, "-B", $build, "-DCMAKE_BUILD_TYPE=Release", "-DKIRTON_PRODUCT_VERSION=$version", "-DKIRTON_RUNTIME_LIBRARY_PATH=", "-DKIRTON_CLANG_RUNTIME_PATH=", "-DBUILD_TESTING=OFF")
$testArgs = @("-S", $root, "-B", $testBuild, "-DCMAKE_BUILD_TYPE=Release", "-DKIRTON_PRODUCT_VERSION=$version", "-DKIRTON_RUNTIME_LIBRARY_PATH=", "-DKIRTON_CLANG_RUNTIME_PATH=", "-DBUILD_TESTING=ON")
if ($Generator) { $configureArgs += @("-G", $Generator); $testArgs += @("-G", $Generator) }
if ($Generator -like "Visual Studio*") {
    $configureArgs += @("-A", "x64")
    $testArgs += @("-A", "x64")
}
Invoke-CMake $configureArgs
Invoke-CMake @("--build", $build, "--config", "Release", "--parallel")

if (-not $SkipTests) {
    Invoke-CMake $testArgs
    Invoke-CMake @("--build", $testBuild, "--config", "Release", "--parallel")
    Invoke-CTest @("--test-dir", $testBuild, "-C", "Release", "--output-on-failure")
}

Invoke-CMake @("--install", $build, "--config", "Release", "--prefix", $install)
New-Item -ItemType Directory -Force -Path $package | Out-Null
foreach ($dir in @("bin", "include", "lib", "examples", "docs", "src", "tests", "third_party", "installer")) {
    New-Item -ItemType Directory -Force -Path (Join-Path $package $dir) | Out-Null
}
Copy-Item (Join-Path $install "bin\kirton.exe") (Join-Path $package "bin\")
Copy-Item (Join-Path $install "include\kirton") (Join-Path $package "include\") -Recurse
Copy-Item (Join-Path $install "lib\kirton.lib") (Join-Path $package "lib\")
Copy-Item (Join-Path $root "src\*") (Join-Path $package "src\") -Recurse
Copy-Item (Join-Path $root "tests\*") (Join-Path $package "tests\") -Recurse
Copy-Item (Join-Path $root "third_party\*") (Join-Path $package "third_party\") -Recurse
Copy-Item (Join-Path $root "CMakeLists.txt") (Join-Path $package "CMakeLists.txt")
Copy-Item (Join-Path $root "Build-Kirton-SDK.ps1") (Join-Path $package "Build-Kirton-SDK.ps1")
Copy-Item (Join-Path $root "Build-Kirton-Installer.ps1") (Join-Path $package "Build-Kirton-Installer.ps1")
Copy-Item (Join-Path $root "installer\KirtonSetup.cs") (Join-Path $package "installer\KirtonSetup.cs")
Copy-Item (Join-Path $root "installer\Install-Kirton.ps1") (Join-Path $package "installer\Install-Kirton.ps1")
Copy-Item (Join-Path $root "installer\Uninstall-Kirton.ps1") (Join-Path $package "installer\Uninstall-Kirton.ps1")
Copy-Item (Join-Path $install "share\kirton\examples\*") (Join-Path $package "examples\") -Recurse
Copy-Item (Join-Path $root "examples\embedding\host.c") (Join-Path $package "examples\embedding\host.c")
foreach ($doc in @("GETTING_STARTED.md", "KIRTON_1_3_GUIDE.md", "KIRTON_1_2_GUIDE.md", "KIRTON_OOM_CONTRACT.md", "LANGUAGE_REFERENCE.md", "KIRTON_1_1_GUIDE.md", "TECHNICAL_REFERENCE_V1_1.md", "CLI_REFERENCE.md", "EMBEDDING.md", "LICENSING.md", "KNOWN_LIMITATIONS.md", "GENTOO.md", "SYNTAX_V1.md", "SYNTAX_V1_1.md", "CONFORMANCE_V1.md", "RELEASE_GATE_V1.md", "PLATFORMS_V1.md")) {
    Copy-Item (Join-Path $root "docs\$doc") (Join-Path $package "docs\")
}
Copy-Item (Join-Path $root "README.md") (Join-Path $package "README.md")
Set-Content -LiteralPath (Join-Path $package "VERSION") -Value $version -Encoding ascii
Copy-Item (Join-Path $root "LICENSE_STATUS.md") (Join-Path $package "LICENSE_STATUS.md")
Copy-Item (Join-Path $root "LICENSE") (Join-Path $package "LICENSE")
Copy-Item (Join-Path $root "THIRD_PARTY_NOTICES.md") (Join-Path $package "THIRD_PARTY_NOTICES.md")
Copy-Item (Join-Path $root "CHANGELOG.md") (Join-Path $package "CHANGELOG.md")
$releaseNotesName = "RELEASE_NOTES_$version.md"
Copy-Item (Join-Path $root $releaseNotesName) (Join-Path $package $releaseNotesName)
if ($PackagePrefix -eq "Kirton-Compiler" -and $version -match '(?i)preview|alpha|beta|rc') {
    $productName = "Kirton Compiler 1.2 Preview"
    $uninstallKeyName = "KirtonCompilerPreview120p1"
    @($version, $sdkName, $productName, $uninstallKeyName, $productName) |
        Set-Content -LiteralPath (Join-Path $package "PACKAGE_INFO.txt") -Encoding ascii
    $previewReadme = "# $productName`r`n`r`nThis is an experimental Preview, not a Stable release. See docs/KIRTON_COMPILER_1_2_PREVIEW.md for supported behavior and open defects.`r`n"
    Set-Content -LiteralPath (Join-Path $package "README.md") -Value $previewReadme -Encoding utf8
}
elseif ($PackagePrefix -eq "Kirton-Compiler") {
    $productName = "Kirton Compiler $version Stable"
    $versionDigits = $version -replace '[^0-9]', ''
    $uninstallKeyName = "KirtonCompilerStable$versionDigits"
    @($version, $sdkName, $productName, $uninstallKeyName, $productName) |
        Set-Content -LiteralPath (Join-Path $package "PACKAGE_INFO.txt") -Encoding ascii
    Set-Content -LiteralPath (Join-Path $package "PUBLISHER.txt") -Value "WorldCraft Studio" -Encoding ascii
    Set-Content -LiteralPath (Join-Path $package "RELEASE_CHANNEL.txt") -Value "Stable" -Encoding ascii
}

Compress-Archive -Path $package -DestinationPath $zip -CompressionLevel Optimal

# Exercise the actual archive from a fresh path containing spaces and Cyrillic.
$smoke = Join-Path $env:TEMP ("Киртон SDK smoke with spaces " + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force -Path $smoke | Out-Null
Expand-Archive -LiteralPath $zip -DestinationPath $smoke
$unpacked = Join-Path $smoke "$sdkName"
$exe = Join-Path $unpacked "bin\kirton.exe"
& $exe --version
if ($LASTEXITCODE -ne 0) { throw "Packaged --version failed" }
if (-not (& $exe --version | Select-String -SimpleMatch "Kirton $version")) { throw "Packaged version does not identify $version" }
& $exe (Join-Path $unpacked "examples\hello.kir")
if ($LASTEXITCODE -ne 0) { throw "Packaged hello example failed" }
& $exe (Join-Path $unpacked "examples\getting_started.kir")
if ($LASTEXITCODE -ne 0) { throw "Packaged Getting Started example failed" }
Push-Location $unpacked
try {
    foreach ($guideExample in @("guide_1_2_values.kir", "guide_1_2_library.kir")) {
        foreach ($mode in @(@(), @("--bytecode"))) {
            & $exe @mode "examples\$guideExample"
            if ($LASTEXITCODE -ne 0) { throw "Packaged Kirton 1.2 guide example failed: $guideExample" }
        }
    }
} finally { Pop-Location }
$exampleDir = Join-Path $unpacked "examples\apps"
Push-Location $exampleDir
try {
    & $exe "task_tracker.kir"
    if ($LASTEXITCODE -ne 0) { throw "Packaged task_tracker example failed" }
    & $exe --bytecode "task_tracker.kir"
    if ($LASTEXITCODE -ne 0) { throw "Packaged Bytecode task_tracker example failed" }
} finally { Pop-Location }
Push-Location $unpacked
try {
    & $exe "examples/hello_module.kir"
    if ($LASTEXITCODE -ne 0) { throw "Packaged module example failed" }
} finally { Pop-Location }

# Validate Native compilation from the extracted SDK layout. The SDK is built
# with empty compile-time fallback paths, so this exercises the adjacent
# lib/kirton.lib lookup and runtime Clang discovery used after installation.
function Invoke-InstalledKirton {
    param([string]$Executable, [string[]]$Arguments)
    $output = & $Executable @Arguments 2>&1
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0) {
        throw "Installed CLI command failed ($exitCode): $($Arguments -join ' '); $($output -join ' ')"
    }
    return ($output -join "`n").Trim()
}

$previousKirtonPath = $env:KIRTON_PATH
$env:KIRTON_PATH = Join-Path $unpacked "tests"
try {
    $nativeTests = Join-Path $unpacked "tests"
    $pathProbeSource = Join-Path $smoke "unicode-kirton-path.kir"
    @'
импорт "native_compiler_release_integration_lib" как библиотека
пусть счётчик = библиотека.создать_счетчик(0)
печатать(счётчик(2))
'@ | Set-Content -LiteralPath $pathProbeSource -Encoding utf8NoBOM
    $pathProbeAst = Invoke-InstalledKirton $exe @($pathProbeSource)
    $pathProbeBytecode = Invoke-InstalledKirton $exe @("--bytecode", $pathProbeSource)
    $pathProbeExe = Join-Path $smoke "unicode-kirton-path-native.exe"
    & $exe --native $pathProbeSource -o $pathProbeExe
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $pathProbeExe)) {
        throw "Installed SDK failed Native compilation with Unicode KIRTON_PATH"
    }
    $pathProbeNative = Invoke-InstalledKirton $pathProbeExe @()
    if ($pathProbeAst -ne "12" -or $pathProbeAst -ne $pathProbeBytecode -or
        $pathProbeAst -ne $pathProbeNative) {
        throw "Unicode KIRTON_PATH AST/Bytecode/Native outputs differ"
    }

    $integrationExe = Join-Path $smoke "installed-native-integration.exe"
    Push-Location $nativeTests
    try {
        $integrationSource = "native_compiler_release_integration.kir"
        $integrationAst = Invoke-InstalledKirton $exe @($integrationSource)
        $integrationBytecode = Invoke-InstalledKirton $exe @("--bytecode", $integrationSource)
        & $exe --native $integrationSource -o $integrationExe
        if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $integrationExe)) {
            throw "Installed SDK failed to produce the Native integration EXE"
        }
        $integrationNative = Invoke-InstalledKirton $integrationExe @()
        if ($integrationAst -ne $integrationBytecode -or $integrationAst -ne $integrationNative) {
            throw "Installed Native integration output differs from AST/Bytecode"
        }
    } finally { Pop-Location }

    foreach ($ffiVersion in @("v2", "v3")) {
        $ffiSource = Get-ChildItem -LiteralPath $testBuild -Filter "ffi_${ffiVersion}_cli_Release.kir" -File -Recurse |
            Select-Object -First 1
        $ffiPluginFile = Get-ChildItem -LiteralPath $testBuild -Filter "kirton_ffi_${ffiVersion}_plugin.dll" -File -Recurse |
            Select-Object -First 1
        if (-not $ffiSource -or -not $ffiPluginFile) {
            throw "Installed SDK FFI V$ffiVersion test inputs are missing"
        }
        $ffiPlugin = $ffiPluginFile.FullName
        $ffiExe = Join-Path $smoke "installed-native-ffi-$ffiVersion.exe"
        $ffiAst = Invoke-InstalledKirton $exe @("--allow-ffi", $ffiSource.FullName)
        $ffiBytecode = Invoke-InstalledKirton $exe @("--allow-ffi", "--bytecode", $ffiSource.FullName)
        & $exe --allow-ffi --native $ffiSource.FullName -o $ffiExe
        if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $ffiExe)) {
            throw "Installed SDK failed to compile Native FFI V$ffiVersion"
        }
        $ffiNative = Invoke-InstalledKirton $ffiExe @()
        if ($ffiAst -ne $ffiBytecode -or $ffiAst -ne $ffiNative) {
            throw "Installed FFI V$ffiVersion output differs across AST/Bytecode/Native"
        }
        $expectedFfiMarker = if ($ffiVersion -eq "v2") { "FFI_CLI_OK" } else { "FFI_V3_CLI_OK" }
        if (-not $ffiNative.Contains($expectedFfiMarker)) {
            throw "Installed Native FFI V$ffiVersion did not emit its success marker"
        }
    }
} finally {
    if ($null -eq $previousKirtonPath) {
        Remove-Item Env:\KIRTON_PATH -ErrorAction SilentlyContinue
    } else {
        $env:KIRTON_PATH = $previousKirtonPath
    }
}

# Compile and run the C host against the extracted SDK library and headers.
$hostProject = Join-Path $smoke "host-project"
New-Item -ItemType Directory -Force -Path $hostProject | Out-Null
$hostSource = (Join-Path $unpacked "examples\embedding\host.c").Replace("\", "/")
$sdkRoot = $unpacked.Replace("\", "/")
@"
cmake_minimum_required(VERSION 3.15)
project(kirton_sdk_host C)
add_executable(host "$hostSource")
target_include_directories(host PRIVATE "$sdkRoot/include")
target_link_libraries(host PRIVATE "$sdkRoot/lib/kirton.lib" winhttp user32 gdi32 winmm opengl32 d3d11 dxgi d3dcompiler windowscodecs ole32 dxguid ws2_32)
"@ | Set-Content -Encoding utf8 -LiteralPath (Join-Path $hostProject "CMakeLists.txt")
$hostBuild = Join-Path $hostProject "build"
$hostConfigureArgs = @("-S", $hostProject, "-B", $hostBuild, "-G", $Generator)
if ($Generator -like "Visual Studio*") { $hostConfigureArgs += @("-A", "x64") }
Invoke-CMake $hostConfigureArgs
Invoke-CMake @("--build", $hostBuild, "--config", "Release", "--parallel")
$hostExecutable = Join-Path $hostBuild "host.exe"
if ($Generator -like "Visual Studio*") { $hostExecutable = Join-Path $hostBuild "Release\host.exe" }
& $hostExecutable (Join-Path $unpacked "examples\embedding\hello.kir")
if ($LASTEXITCODE -ne 0) { throw "C host linked to extracted SDK failed" }

$installerSuffix = if ($PackagePrefix -eq "Kirton-Compiler") { "-setup-ru-en" } else { "-setup" }
& (Join-Path $root "Build-Kirton-Installer.ps1") -SdkName $sdkName `
    -InstallerSuffix $installerSuffix -DistRoot $dist `
    -BundleRoot $installerBundle -TestInstall
if ($LASTEXITCODE -ne 0) { throw "Windows setup EXE build or install smoke failed" }

$sha = [System.Security.Cryptography.SHA256]::Create()
$stream = [System.IO.File]::OpenRead($zip)
try { $hash = ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace("-", "") }
finally { $stream.Dispose(); $sha.Dispose() }
Set-Content -Encoding ascii -LiteralPath "$zip.sha256" -Value "$hash  $(Split-Path -Leaf $zip)"
$zipInfo = Get-Item $zip
Write-Output "SDK: $zip"
Write-Output "Size: $($zipInfo.Length) bytes"
Write-Output "SHA-256: $hash"
Write-Output "Tests: $(if ($SkipTests) { 'SKIPPED by request' } else { 'PASS' })"
Write-Output "Extracted archive, Unicode path, AST/Bytecode apps, module and C API smoke: PASS"
Write-Output "Windows setup EXE: built and install-tested"
Write-Output "License: MPL-2.0 included; review provenance note in LICENSE_STATUS.md"
