param(
    [Parameter(Mandatory=$true)][string]$Interpreter,
    [Parameter(Mandatory=$true)][string]$Guide,
    [Parameter(Mandatory=$true)][string]$WorkingDirectory
)
$ErrorActionPreference = 'Stop'
$text = [System.IO.File]::ReadAllText($Guide)
$pattern = '(?ms)^<!-- expect: (?<expected>.*?) -->\s*^```kirton\r?\n(?<source>.*?)^```\s*$'
foreach ($example in [regex]::Matches($text, $pattern)) {
    $tempFile = Join-Path ([System.IO.Path]::GetTempPath()) ('kirton-guide-' + [guid]::NewGuid().ToString('N') + '.kir')
    try {
        [System.IO.File]::WriteAllText($tempFile, $example.Groups['source'].Value, [System.Text.UTF8Encoding]::new($false))
        Push-Location $WorkingDirectory
        try { $output = (& $Interpreter $tempFile 2>&1 | Out-String); $exitCode = $LASTEXITCODE }
        finally { Pop-Location }
        if ($exitCode -ne 0) { throw "Example $($example.Groups['expected'].Value) exited $exitCode`n$output" }
        $expected = $example.Groups['expected'].Value
        if ($expected -ne 'EXIT_SUCCESS' -and $output -notmatch $expected) {
            throw "Expected /$expected/ in output, got:`n$output"
        }
    }
    finally { Remove-Item -LiteralPath $tempFile -Force -ErrorAction SilentlyContinue }
}
$count = [regex]::Matches($text, $pattern).Count
if ($count -ne 9) { throw "Expected 9 checked Kirton examples; found $count" }
Write-Output "Handbook examples: $count PASS"
