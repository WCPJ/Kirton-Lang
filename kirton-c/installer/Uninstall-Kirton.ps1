# SPDX-License-Identifier: MPL-2.0
# This Source Code Form is subject to the terms of the Mozilla Public License v. 2.0. See https://mozilla.org/MPL/2.0/.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
Add-Type -AssemblyName System.Windows.Forms
$InstallDir = [System.IO.Path]::GetFullPath($PSScriptRoot)
$manifest = Join-Path $InstallDir '.kirton-installed-files.txt'
if (-not (Test-Path $manifest)) { throw 'Kirton install manifest is missing; no files were removed.' }
$infoPath = Join-Path $InstallDir 'PACKAGE_INFO.txt'
if (-not (Test-Path -LiteralPath $infoPath)) { $infoPath = Join-Path $InstallDir 'PREVIEW_INFO.txt' }
if (Test-Path -LiteralPath $infoPath) {
    $info = @(Get-Content -LiteralPath $infoPath)
    if ($info.Count -lt 5) { throw 'Package uninstall metadata is invalid; no files were removed.' }
    $productName = $info[2]
    $uninstallKeyName = $info[3]
    $shortcutDirectoryName = $info[4]
    $language = if ($info.Count -ge 6 -and $info[5] -eq 'en') { 'en' } else { 'ru' }
} else {
    $productName = 'Kirton SDK'
    $uninstallKeyName = 'KirtonSDK'
    $shortcutDirectoryName = 'Kirton SDK'
    $language = 'en'
}
$uninstallKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\' + $uninstallKeyName
$keepPath = $null
$shortcutDir = $null
if (Test-Path $uninstallKey) {
    $key = Get-ItemProperty $uninstallKey -ErrorAction SilentlyContinue
    if ($key -and $key.PSObject.Properties['KirtonPathAdded']) { $keepPath = $key.KirtonPathAdded }
    if ($key -and $key.PSObject.Properties['KirtonShortcutDir']) { $shortcutDir = $key.KirtonShortcutDir }
}
if ($env:KIRTON_UNINSTALL_SILENT -ne '1') {
    $message = if ($language -eq 'ru') {
        "Удалить $productName из каталога:`n$InstallDir`n`nФайлы пользователя, не установленные Kirton, будут сохранены."
    } else {
        "Remove $productName from:`n$InstallDir`n`nFiles not installed by Kirton, including user projects, are kept."
    }
    $title = if ($language -eq 'ru') { "Удаление $productName" } else { "Uninstall $productName" }
    $answer = [System.Windows.Forms.MessageBox]::Show(
        $message,
        $title, [System.Windows.Forms.MessageBoxButtons]::OKCancel,
        [System.Windows.Forms.MessageBoxIcon]::Question)
    if ($answer -ne [System.Windows.Forms.DialogResult]::OK) { exit 0 }
}

$prefix = $InstallDir.TrimEnd('\') + '\'
foreach ($relative in Get-Content -LiteralPath $manifest) {
    if ([string]::IsNullOrWhiteSpace($relative)) { continue }
    $target = [System.IO.Path]::GetFullPath((Join-Path $InstallDir $relative))
    if (-not $target.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Install manifest path escaped the install directory: $relative"
    }
    if (Test-Path -LiteralPath $target -PathType Leaf) { Remove-Item -LiteralPath $target -Force }
}
Remove-Item -LiteralPath $manifest -Force -ErrorAction SilentlyContinue
if ($keepPath) {
    $userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
    $entries = @($userPath -split ';' | Where-Object { $_ -and $_.TrimEnd('\') -ine $keepPath.TrimEnd('\') })
    $remainingPath = $entries -join ';'
    [Environment]::SetEnvironmentVariable('Path', $(if ($remainingPath) { $remainingPath } else { $null }), 'User')
}
if (Test-Path $uninstallKey) { Remove-Item -LiteralPath $uninstallKey -Recurse -Force }
if ($shortcutDir) {
    $shortcutNames = if ($language -eq 'ru') {
        @(('Документация - ' + $productName + '.lnk'), ('Удалить ' + $productName + '.lnk'))
    } else {
        @(($productName + ' Documentation.lnk'), ('Uninstall ' + $productName + '.lnk'))
    }
    foreach ($name in $shortcutNames) {
        Remove-Item -LiteralPath (Join-Path $shortcutDir $name) -Force -ErrorAction SilentlyContinue
    }
    if ((Test-Path $shortcutDir) -and -not (Get-ChildItem -LiteralPath $shortcutDir -Force | Select-Object -First 1)) {
        Remove-Item -LiteralPath $shortcutDir -Force
    }
}
Get-ChildItem -LiteralPath $InstallDir -Directory -Recurse | Sort-Object FullName -Descending | ForEach-Object {
    if (-not (Get-ChildItem -LiteralPath $_.FullName -Force | Select-Object -First 1)) {
        Remove-Item -LiteralPath $_.FullName -Force
    }
}
if (-not (Get-ChildItem -LiteralPath $InstallDir -Force | Where-Object Name -ne 'Uninstall-Kirton.ps1' | Select-Object -First 1)) {
    Remove-Item -LiteralPath (Join-Path $InstallDir 'Uninstall-Kirton.ps1') -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $InstallDir -Force -ErrorAction SilentlyContinue
}
