# SPDX-License-Identifier: MPL-2.0
param(
    [Parameter(Mandatory = $true)][string]$InstallDir
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$sdkName = 'Kirton-Compiler-1.2.0-preview.1-windows-x64'
$version = '1.2.0-preview.1'
$zip = Join-Path $root "dist\$sdkName.zip"
$installPath = [System.IO.Path]::GetFullPath($InstallDir)
$programsPrefix = [System.IO.Path]::GetFullPath((Join-Path $env:LOCALAPPDATA 'Programs')).TrimEnd('\') + '\'
if (-not $installPath.StartsWith($programsPrefix, [StringComparison]::OrdinalIgnoreCase) -or
    (Split-Path -Leaf $installPath) -notlike 'Kirton-Compiler-1.2.0-preview.1-windows-x64-test-*') {
    throw "InstallDir must be an isolated Preview test directory below $programsPrefix"
}
if (Test-Path -LiteralPath $installPath) { throw "InstallDir already exists: $installPath" }
if (-not (Test-Path -LiteralPath $zip -PathType Leaf)) { throw "Preview ZIP is missing: $zip" }

$previewKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\KirtonCompilerPreview120p1'
$stableKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\KirtonSDK'
$stableZip = Join-Path $root 'dist\Kirton-SDK-1.1.0-windows-x64.zip'
$userPathBefore = [Environment]::GetEnvironmentVariable('Path', 'User')
$processPathBefore = $env:Path
$stableRegistryBefore = Get-ItemProperty -LiteralPath $stableKey -ErrorAction SilentlyContinue
$stableZipHashBefore = if (Test-Path -LiteralPath $stableZip) {
    (Get-FileHash -LiteralPath $stableZip -Algorithm SHA256).Hash
} else { $null }
$payloadRoot = Join-Path $env:TEMP ('Kirton Preview Install Payload ' + [guid]::NewGuid().ToString('N'))
$hostRoot = Join-Path $env:TEMP ('Kirton Preview Installed C Host ' + [guid]::NewGuid().ToString('N'))
$installAttempted = $false
$shortcutDir = $null

try {
    New-Item -ItemType Directory -Path $payloadRoot | Out-Null
    Copy-Item -LiteralPath $zip -Destination $payloadRoot
    Copy-Item -LiteralPath (Join-Path $root 'installer\Install-Kirton.ps1') -Destination $payloadRoot
    Copy-Item -LiteralPath (Join-Path $root 'installer\Uninstall-Kirton.ps1') -Destination $payloadRoot

    $installAttempted = $true
    & (Join-Path $payloadRoot 'Install-Kirton.ps1') -InstallDir $installPath -AddPath -CreateShortcuts -Language ru

    $bin = Join-Path $installPath 'bin'
    $exe = Join-Path $bin 'kirton.exe'
    $requiredFiles = @(
        'bin\kirton.exe', 'include\kirton\eval.h', 'lib\kirton.lib',
        'src\bytecode.c', 'third_party\stb_image.h', 'LICENSE',
        'LICENSE_STATUS.md', 'THIRD_PARTY_NOTICES.md', 'PREVIEW_INFO.txt',
        'docs\KIRTON_COMPILER_1_2_PREVIEW.md', 'examples\embedding\host.c',
        'examples\apps\task_tracker.kir'
    )
    foreach ($relative in $requiredFiles) {
        if (-not (Test-Path -LiteralPath (Join-Path $installPath $relative) -PathType Leaf)) {
            throw "Installed SDK file is missing: $relative"
        }
    }

    $info = @(Get-Content -LiteralPath (Join-Path $installPath 'PREVIEW_INFO.txt'))
    if ($info.Count -lt 6 -or $info[0] -ne $version -or $info[1] -ne $sdkName -or $info[2] -ne 'Kirton Compiler 1.2 Preview' -or $info[5] -ne 'ru') {
        throw 'Installed Preview metadata is incorrect.'
    }
    $shortcutDir = Join-Path $env:APPDATA ('Microsoft\Windows\Start Menu\Programs\' + $info[4])
    if (-not (Test-Path -LiteralPath (Join-Path $shortcutDir ('Документация - ' + $info[2] + '.lnk'))) -or
        -not (Test-Path -LiteralPath (Join-Path $shortcutDir ('Удалить ' + $info[2] + '.lnk')))) {
        throw 'Preview shortcuts are missing.'
    }

    $pathAfterInstall = [Environment]::GetEnvironmentVariable('Path', 'User')
    $pathEntries = @($pathAfterInstall -split ';')
    if (@($pathEntries | Where-Object { $_.TrimEnd('\') -ieq $bin.TrimEnd('\') }).Count -ne 1) {
        throw 'Preview bin directory is not present exactly once in user PATH.'
    }
    $previewRegistration = Get-ItemProperty -LiteralPath $previewKey
    if ($previewRegistration.DisplayVersion -ne $version -or $previewRegistration.InstallLocation -ne $installPath -or
        $previewRegistration.DisplayName -ne 'Kirton Compiler 1.2 Preview' -or $previewRegistration.Publisher -ne 'WorldCraft Studio') {
        throw 'Preview uninstall registration has an incorrect version or location.'
    }

    $env:Path = "$bin;$processPathBefore"
    $resolvedExe = (Get-Command kirton.exe -ErrorAction Stop).Source
    if ($resolvedExe -ne $exe) { throw "PATH resolved the wrong Kirton executable: $resolvedExe" }
    $versionOutput = & kirton.exe --version
    if ($LASTEXITCODE -ne 0 -or $versionOutput -ne "Kirton $version") {
        throw "Installed version check failed: $versionOutput"
    }

    $apps = Join-Path $installPath 'examples\apps'
    Push-Location $apps
    try {
        $astOutput = @(& kirton.exe 'task_tracker.kir' 2>&1)
        $astExit = $LASTEXITCODE
        $bytecodeOutput = @(& kirton.exe --bytecode 'task_tracker.kir' 2>&1)
        $bytecodeExit = $LASTEXITCODE
    } finally { Pop-Location }
    if ($astExit -ne 0 -or $bytecodeExit -ne 0 -or
        -not ($astOutput -match 'TASK_TRACKER_OK') -or
        -not ($bytecodeOutput -match 'TASK_TRACKER_OK') -or
        [string]::Join("`n", $astOutput) -cne [string]::Join("`n", $bytecodeOutput)) {
        throw 'Installed AST/Bytecode application run failed or produced different output.'
    }

    New-Item -ItemType Directory -Path $hostRoot | Out-Null
    $hostSource = (Join-Path $installPath 'examples\embedding\host.c').Replace('\', '/')
    $sdkRoot = $installPath.Replace('\', '/')
    @"
cmake_minimum_required(VERSION 3.15)
project(kirton_installed_host C)
add_executable(host "$hostSource")
target_include_directories(host PRIVATE "$sdkRoot/include")
target_link_libraries(host PRIVATE "$sdkRoot/lib/kirton.lib" winhttp user32 gdi32 winmm opengl32 d3d11 dxgi d3dcompiler windowscodecs ole32 dxguid ws2_32)
"@ | Set-Content -LiteralPath (Join-Path $hostRoot 'CMakeLists.txt') -Encoding utf8
    & cmake -S $hostRoot -B (Join-Path $hostRoot 'build') -G Ninja | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Installed C host CMake configure failed.' }
    & cmake --build (Join-Path $hostRoot 'build') --parallel | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Installed C host build failed.' }
    $hostOutput = & (Join-Path $hostRoot 'build\host.exe') (Join-Path $installPath 'examples\embedding\hello.kir')
    if ($LASTEXITCODE -ne 0 -or -not ($hostOutput -match 'Hello from an embedded Kirton VM')) {
        throw 'Installed C host example failed.'
    }

    $env:KIRTON_UNINSTALL_SILENT = '1'
    & (Join-Path $installPath 'Uninstall-Kirton.ps1')
    Remove-Item Env:\KIRTON_UNINSTALL_SILENT -ErrorAction SilentlyContinue
    $installAttempted = $false

    if ([Environment]::GetEnvironmentVariable('Path', 'User') -cne $userPathBefore) {
        throw 'User PATH did not return exactly to its original value after uninstall.'
    }
    if (Test-Path -LiteralPath $previewKey) { throw 'Preview uninstall registration remains.' }
    if (Test-Path -LiteralPath $shortcutDir) { throw 'Preview Start menu shortcuts remain.' }
    if (Test-Path -LiteralPath $installPath) { throw 'Preview install directory remains.' }

    $stableRegistryAfter = Get-ItemProperty -LiteralPath $stableKey -ErrorAction SilentlyContinue
    if ($null -eq $stableRegistryBefore) {
        if ($null -ne $stableRegistryAfter) { throw 'Preview changed the stable SDK registration.' }
    } elseif ($stableRegistryAfter.InstallLocation -ne $stableRegistryBefore.InstallLocation -or
        $stableRegistryAfter.DisplayVersion -ne $stableRegistryBefore.DisplayVersion) {
        throw 'Preview changed the stable SDK registration.'
    }
    if ($null -ne $stableZipHashBefore -and
        (Get-FileHash -LiteralPath $stableZip -Algorithm SHA256).Hash -ne $stableZipHashBefore) {
        throw 'Stable SDK 1.1 ZIP changed.'
    }

    [pscustomobject]@{
        Install = 'PASS'
        UserPath = 'PASS; restored exactly after uninstall'
        Version = $version
        AST = 'PASS'
        Bytecode = 'PASS; output matched AST'
        CHost = 'PASS'
        Uninstall = 'PASS; install files/registration/shortcuts removed'
        StableSdk1_1 = 'Unchanged'
    } | Format-List
} finally {
    $env:Path = $processPathBefore
    Remove-Item Env:\KIRTON_UNINSTALL_SILENT -ErrorAction SilentlyContinue
    if ($installAttempted -and (Test-Path -LiteralPath (Join-Path $installPath 'Uninstall-Kirton.ps1'))) {
        $env:KIRTON_UNINSTALL_SILENT = '1'
        try { & (Join-Path $installPath 'Uninstall-Kirton.ps1') } catch { }
        Remove-Item Env:\KIRTON_UNINSTALL_SILENT -ErrorAction SilentlyContinue
    }
    if (Test-Path -LiteralPath $hostRoot) { Remove-Item -LiteralPath $hostRoot -Recurse -Force }
    if (Test-Path -LiteralPath $payloadRoot) { Remove-Item -LiteralPath $payloadRoot -Recurse -Force }
}
