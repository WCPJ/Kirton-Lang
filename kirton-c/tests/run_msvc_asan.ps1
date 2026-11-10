param(
    [string]$BuildDirectory = "build-stage5-asan",
    [int]$TestTimeoutSeconds = 30
)

$ErrorActionPreference = "Stop"
$buildPath = (Resolve-Path -LiteralPath $BuildDirectory).Path
$cachePath = Join-Path $buildPath "CMakeCache.txt"
if (-not (Test-Path -LiteralPath $cachePath)) {
    throw "CMake cache not found: $cachePath"
}

$compilerEntry = Get-Content -LiteralPath $cachePath |
    Where-Object { $_ -match '^CMAKE_C_COMPILER(?::[^=]+)?=(.+)$' } |
    Select-Object -First 1
if (-not $compilerEntry -or $compilerEntry -notmatch '^CMAKE_C_COMPILER(?::[^=]+)?=(.+)$') {
    throw "CMAKE_C_COMPILER is not set in $cachePath"
}
$compilerPath = (Resolve-Path -LiteralPath $Matches[1]).Path
$compilerDirectory = Split-Path -Parent $compilerPath
if ($compilerPath -match '\\x64\\cl\.exe$') {
    $asanRuntime = Join-Path $compilerDirectory "clang_rt.asan_dynamic-x86_64.dll"
} elseif ($compilerPath -match '\\x86\\cl\.exe$') {
    $asanRuntime = Join-Path $compilerDirectory "clang_rt.asan_dynamic-i386.dll"
} else {
    throw "Unsupported MSVC target architecture in compiler path: $compilerPath"
}
if (-not (Test-Path -LiteralPath $asanRuntime)) {
    throw "MSVC ASan runtime for this compiler is missing: $asanRuntime"
}

$env:PATH = "$compilerDirectory;$env:PATH"
$kirton = Join-Path $buildPath "kirton.exe"
if (-not (Test-Path -LiteralPath $kirton)) {
    throw "ASan executable not found: $kirton"
}
$versionOutput = & $kirton --version 2>&1
$versionExit = $LASTEXITCODE
if ($versionExit -ne 0) {
    throw "ASan executable failed its startup check (exit $versionExit): $($versionOutput -join ' ')"
}

$ctest = (Get-Command ctest.exe -ErrorAction Stop).Source
$logDirectory = Join-Path $buildPath "Testing\Temporary"
New-Item -ItemType Directory -Force -Path $logDirectory | Out-Null
$logPath = Join-Path $logDirectory "MSVC_ASAN_CTest.log"
@(
    "Compiler: $compilerPath"
    "ASan runtime: $asanRuntime"
    "Startup check: $($versionOutput -join ' ') (exit $versionExit)"
    "CTest timeout per test: $TestTimeoutSeconds seconds"
    ""
) | Set-Content -LiteralPath $logPath -Encoding UTF8

& $ctest --test-dir $buildPath --output-on-failure --timeout $TestTimeoutSeconds -j 1 2>&1 |
    Tee-Object -FilePath $logPath -Append
$ctestExit = $LASTEXITCODE
Write-Output "ASan CTest log: $logPath"
exit $ctestExit
