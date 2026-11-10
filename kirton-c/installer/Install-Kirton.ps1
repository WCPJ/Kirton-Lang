# SPDX-License-Identifier: MPL-2.0
# This Source Code Form is subject to the terms of the Mozilla Public License v. 2.0. See https://mozilla.org/MPL/2.0/.
param(
    [Parameter(Mandatory=$true)][string]$InstallDir,
    [switch]$Silent,
    [switch]$AddPath,
    [switch]$CreateShortcuts,
    [ValidateSet('ru','en')][string]$Language = 'en'
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
Add-Type -AssemblyName System.IO.Compression.FileSystem

$InstallDir = [System.IO.Path]::GetFullPath($InstallDir)
$payloads = @(Get-ChildItem -LiteralPath $PSScriptRoot -Filter 'Kirton-*-windows-x64.zip' -File)
if ($payloads.Count -ne 1) { throw "Expected one SDK payload archive, found $($payloads.Count)." }
$payload = $payloads[0].FullName
$uninstallerSource = Join-Path $PSScriptRoot 'Uninstall-Kirton.ps1'
if (-not (Test-Path -LiteralPath $payload -PathType Leaf)) { throw 'SDK payload ZIP is missing.' }
if (-not (Test-Path -LiteralPath $uninstallerSource -PathType Leaf)) { throw 'Uninstaller payload is missing.' }

New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
$temp = Join-Path $env:TEMP ('kirton-sdk-install-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temp | Out-Null
try {
    [System.IO.Compression.ZipFile]::ExtractToDirectory($payload, $temp)
    $sdkName = [System.IO.Path]::GetFileNameWithoutExtension($payload)
    $source = Join-Path $temp $sdkName
    if (-not (Test-Path (Join-Path $source 'bin\kirton.exe'))) { throw 'SDK payload is incomplete.' }
    $infoPath = Join-Path $source 'PACKAGE_INFO.txt'
    if (-not (Test-Path -LiteralPath $infoPath)) { $infoPath = Join-Path $source 'PREVIEW_INFO.txt' }
    if (Test-Path -LiteralPath $infoPath) {
        $info = @(Get-Content -LiteralPath $infoPath)
        if ($info.Count -lt 5 -or $info[1] -ne $sdkName) { throw 'Package install metadata is invalid.' }
        $productVersion = $info[0]
        $productName = $info[2]
        $uninstallKeyName = $info[3]
        $shortcutDirectoryName = $info[4]
    } else {
        $productVersion = (Get-Content -Raw (Join-Path $source 'VERSION')).Trim()
        $productName = 'Kirton SDK'
        $uninstallKeyName = 'KirtonSDK'
        $shortcutDirectoryName = 'Kirton SDK'
    }

    $manifest = Join-Path $InstallDir '.kirton-installed-files.txt'
    $oldFiles = @()
    if (Test-Path -LiteralPath $manifest) { $oldFiles = @(Get-Content -LiteralPath $manifest) }
    $sourcePrefix = $source.TrimEnd('\') + '\'
    $newFiles = @(Get-ChildItem -LiteralPath $source -File -Recurse | ForEach-Object {
        $_.FullName.Substring($sourcePrefix.Length)
    })
    $managedBefore = @{}
    foreach ($relative in $oldFiles) { if ($relative) { $managedBefore[$relative] = $true } }
    $conflicts = @($newFiles | Where-Object {
        (Test-Path -LiteralPath (Join-Path $InstallDir $_) -PathType Leaf) -and -not $managedBefore.ContainsKey($_)
    })
    if ($conflicts.Count -gt 0) {
        $shown = ($conflicts | Select-Object -First 8) -join "`r`n"
        throw "These files already exist and are not managed by Kirton. Choose another folder or move them first:`r`n$shown"
    }
    $managedFiles = @($oldFiles + $newFiles + 'Uninstall-Kirton.ps1' | Where-Object {
        $_ -and $_ -ne '.kirton-installed-files.txt'
    } | Sort-Object -Unique)
    Set-Content -LiteralPath $manifest -Value $managedFiles -Encoding utf8
    foreach ($relative in $newFiles) {
        $target = Join-Path $InstallDir $relative
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
        Copy-Item -LiteralPath (Join-Path $source $relative) -Destination $target -Force
    }
    Copy-Item -LiteralPath $uninstallerSource -Destination (Join-Path $InstallDir 'Uninstall-Kirton.ps1') -Force
    $installedInfo = Join-Path $InstallDir 'PACKAGE_INFO.txt'
    if (-not (Test-Path -LiteralPath $installedInfo)) { $installedInfo = Join-Path $InstallDir 'PREVIEW_INFO.txt' }
    if (Test-Path -LiteralPath $installedInfo) {
        $infoLines = @(Get-Content -LiteralPath $installedInfo)
        if ($infoLines.Count -lt 5) { throw 'Package install metadata is incomplete.' }
        Set-Content -LiteralPath $installedInfo -Value (@($infoLines[0..4]) + $Language) -Encoding ascii
    }

    $pathAdded = $null
    $binPath = Join-Path $InstallDir 'bin'
    if ($AddPath) {
        $currentPath = [Environment]::GetEnvironmentVariable('Path', 'User')
        $entries = @($currentPath -split ';' | Where-Object { $_ })
        if (-not ($entries | Where-Object { $_.TrimEnd('\') -ieq $binPath.TrimEnd('\') })) {
            [Environment]::SetEnvironmentVariable('Path', (@($entries) + $binPath) -join ';', 'User')
            $pathAdded = $binPath
        }
    }

    $shortcutDir = $null
    if ($CreateShortcuts) {
        $shortcutDir = Join-Path $env:APPDATA ('Microsoft\Windows\Start Menu\Programs\' + $shortcutDirectoryName)
        New-Item -ItemType Directory -Force -Path $shortcutDir | Out-Null
        $shell = New-Object -ComObject WScript.Shell
        $documentationShortcut = if ($Language -eq 'ru') { 'Документация - ' + $productName + '.lnk' } else { $productName + ' Documentation.lnk' }
        $uninstallShortcut = if ($Language -eq 'ru') { 'Удалить ' + $productName + '.lnk' } else { 'Uninstall ' + $productName + '.lnk' }
        $readme = $shell.CreateShortcut((Join-Path $shortcutDir $documentationShortcut))
        $readme.TargetPath = Join-Path $InstallDir 'README.md'
        $readme.WorkingDirectory = $InstallDir
        $readme.Save()
        $remove = $shell.CreateShortcut((Join-Path $shortcutDir $uninstallShortcut))
        $remove.TargetPath = Join-Path $PSHOME 'powershell.exe'
        $remove.Arguments = '-NoProfile -ExecutionPolicy Bypass -File "' + (Join-Path $InstallDir 'Uninstall-Kirton.ps1') + '"'
        $remove.WorkingDirectory = $InstallDir
        $remove.Save()
    }

    if ($env:KIRTON_SETUP_TEST -ne '1') {
        $uninstallKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\' + $uninstallKeyName
        New-Item -Path $uninstallKey -Force | Out-Null
        $uninstallCommand = 'powershell.exe -NoProfile -ExecutionPolicy Bypass -File "' + (Join-Path $InstallDir 'Uninstall-Kirton.ps1') + '"'
        Set-ItemProperty -Path $uninstallKey -Name DisplayName -Value $productName
        Set-ItemProperty -Path $uninstallKey -Name DisplayVersion -Value $productVersion
        Set-ItemProperty -Path $uninstallKey -Name Publisher -Value 'WorldCraft Studio'
        Set-ItemProperty -Path $uninstallKey -Name InstallLocation -Value $InstallDir
        Set-ItemProperty -Path $uninstallKey -Name UninstallString -Value $uninstallCommand
        New-ItemProperty -Path $uninstallKey -Name NoModify -Value 1 -PropertyType DWord -Force | Out-Null
        New-ItemProperty -Path $uninstallKey -Name NoRepair -Value 1 -PropertyType DWord -Force | Out-Null
        if ($pathAdded) { Set-ItemProperty -Path $uninstallKey -Name KirtonPathAdded -Value $pathAdded }
        if ($shortcutDir) { Set-ItemProperty -Path $uninstallKey -Name KirtonShortcutDir -Value $shortcutDir }
    }
} finally {
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}
