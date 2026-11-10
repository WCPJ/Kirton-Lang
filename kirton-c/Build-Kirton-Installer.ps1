# SPDX-License-Identifier: MPL-2.0
# This Source Code Form is subject to the terms of the Mozilla Public License v. 2.0. See https://mozilla.org/MPL/2.0/.
param(
    [switch]$TestInstall,
    [string]$SdkName,
    [string]$InstallerSuffix = '-setup',
    [string]$DistRoot = 'dist',
    [string]$BundleRoot = 'build-sdk-installer-bundle'
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
if ([System.IO.Path]::IsPathRooted($DistRoot)) {
    $dist = [System.IO.Path]::GetFullPath($DistRoot)
} else {
    $dist = Join-Path $root $DistRoot
}
if ([string]::IsNullOrWhiteSpace($SdkName)) {
    $version = (Get-Content -Raw (Join-Path $root 'VERSION')).Trim()
    $SdkName = "Kirton-SDK-$version-windows-x64"
} else {
    $version = $SdkName -replace '^.*?-([0-9].*)-windows-x64$', '$1'
}
$sdkZip = Join-Path $dist "$sdkName.zip"
if ([System.IO.Path]::IsPathRooted($BundleRoot)) {
    $bundle = [System.IO.Path]::GetFullPath($BundleRoot)
} else {
    $bundle = Join-Path $root $BundleRoot
}
$installer = Join-Path $dist ($sdkName + $InstallerSuffix + '.exe')
$installerHash = "$installer.sha256"
$csharp = Join-Path $root 'installer\KirtonSetup.cs'
$cscCandidates = @(
    'C:\Program Files\Microsoft Visual Studio\18\Insiders\MSBuild\Current\Bin\Roslyn\csc.exe',
    'C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe'
)
$csc = $cscCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
$framework = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319'
if (-not (Test-Path $sdkZip)) { throw "Build the portable SDK first: $sdkZip" }
foreach ($path in @($bundle, $installer, $installerHash)) {
    if (Test-Path -LiteralPath $path) {
        throw "Refusing to overwrite existing installer candidate path: $path"
    }
}
if (-not $csc) { throw 'A C# compiler for the .NET Framework is required to build the Windows setup EXE.' }
if (-not (Test-Path -LiteralPath $csharp)) { throw 'The Windows installer source is missing.' }
foreach ($required in @('LICENSE','installer\Install-Kirton.ps1','installer\Uninstall-Kirton.ps1')) {
    if (-not (Test-Path -LiteralPath (Join-Path $root $required))) { throw "Required installer file is missing: $required" }
}
New-Item -ItemType Directory -Path $bundle | Out-Null
Copy-Item -LiteralPath $sdkZip -Destination (Join-Path $bundle "$sdkName.zip")
Copy-Item -LiteralPath (Join-Path $root 'LICENSE') -Destination (Join-Path $bundle 'LICENSE')
Copy-Item -LiteralPath (Join-Path $root 'installer\Install-Kirton.ps1') -Destination $bundle
Copy-Item -LiteralPath (Join-Path $root 'installer\Uninstall-Kirton.ps1') -Destination $bundle
$packageFolder = Join-Path $dist $sdkName
$packageInfo = Join-Path $packageFolder 'PACKAGE_INFO.txt'
$previewInfo = Join-Path $packageFolder 'PREVIEW_INFO.txt'
if (Test-Path -LiteralPath $packageInfo) {
    $settings = @(Get-Content -LiteralPath $packageInfo)
} elseif (Test-Path -LiteralPath $previewInfo) {
    $settings = @(Get-Content -LiteralPath $previewInfo)
} else {
    $versionDigits = $version -replace '[^0-9]', ''
    $settings = @($version, $sdkName, 'Kirton SDK', "KirtonSDK$versionDigits", "Kirton SDK $version")
}
if ($settings.Count -lt 5 -or $settings[1] -ne $sdkName) { throw 'The package setup metadata is missing or inconsistent.' }
Set-Content -LiteralPath (Join-Path $bundle 'SetupSettings.txt') -Value $settings -Encoding ascii

$compilerArgs = @(
    '/nologo', '/target:winexe', '/platform:anycpu', '/optimize+', '/codepage:65001',
    "/out:$installer",
    '/reference:System.Windows.Forms.dll', '/reference:System.Drawing.dll', '/reference:System.Core.dll',
    "/resource:$(Join-Path $bundle "$sdkName.zip"),KirtonSdkZip",
    "/resource:$(Join-Path $bundle 'LICENSE'),MplLicense",
    "/resource:$(Join-Path $bundle 'Install-Kirton.ps1'),InstallScript",
    "/resource:$(Join-Path $bundle 'Uninstall-Kirton.ps1'),UninstallScript",
    "/resource:$(Join-Path $bundle 'SetupSettings.txt'),SetupSettings",
    $csharp
)
& $csc @compilerArgs
if ($LASTEXITCODE -ne 0 -or -not (Test-Path $installer)) { throw "C# compiler failed to create the setup EXE (exit $LASTEXITCODE)." }

$sha = [System.Security.Cryptography.SHA256]::Create()
$stream = [System.IO.File]::OpenRead($installer)
try { $hash = ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-', '') }
finally { $stream.Dispose(); $sha.Dispose() }
Set-Content -LiteralPath "$installer.sha256" -Encoding ascii -Value "$hash  $(Split-Path -Leaf $installer)"
Write-Output "Installer: $installer"
Write-Output "Size: $((Get-Item $installer).Length) bytes"
Write-Output "SHA-256: $hash"
Write-Output 'Installer UI: native Windows Forms EXE; no IExpress wrapper'
Write-Output 'Signature: unsigned (no signing certificate was provided)'

if ($TestInstall) {
    $testDir = Join-Path $env:TEMP ('Kirton SDK installer test ' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $testDir | Out-Null
    try {
        $testArgs = '--smoke-test "' + $testDir + '"'
        $run = Start-Process -FilePath $installer -ArgumentList $testArgs -Wait -PassThru
        if ($run.ExitCode -ne 0) { throw "Setup EXE smoke test failed with exit code $($run.ExitCode)." }
        Write-Output 'EXE install/uninstall smoke: PASS (license files installed; user project preserved)'
    } finally {
        if (Test-Path -LiteralPath $testDir) {
            $resolvedTestDir = [System.IO.Path]::GetFullPath($testDir)
            $tempPrefix = [System.IO.Path]::GetFullPath($env:TEMP).TrimEnd('\') + '\'
            if (-not $resolvedTestDir.StartsWith($tempPrefix, [StringComparison]::OrdinalIgnoreCase) -or
                (Split-Path -Leaf $resolvedTestDir) -notlike 'Kirton SDK installer test *') {
                throw "Refusing to clean unexpected installer test directory: $resolvedTestDir"
            }
            Remove-Item -LiteralPath $resolvedTestDir -Recurse -Force
        }
    }
}

if (Test-Path $bundle) { Remove-Item -LiteralPath $bundle -Recurse -Force }
