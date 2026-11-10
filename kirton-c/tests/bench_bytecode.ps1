param(
  [Parameter(Mandatory=$true)][string]$Kirton,
  [Parameter(Mandatory=$true)][string]$Profiler,
  [int]$Runs = 7
)
$Kirton = (Resolve-Path $Kirton).Path
$Profiler = (Resolve-Path $Profiler).Path
Push-Location $PSScriptRoot
$scenarios = @('arithmetic', 'operators', 'calls', 'loops', 'foreach', 'variables', 'scopes', 'collections', 'closure_create', 'closure_call', 'struct_fields', 'struct_transfer', 'module_first', 'module_repeat', 'module_call', 'module_cross', 'native_call', 'exception')
foreach ($scenario in $scenarios) {
  $scriptPath = Join-Path $PSScriptRoot "bench_$scenario.kir"
  $expectedOutput = $null
  foreach ($mode in @('AST', 'Bytecode')) {
    $times = @()
    for ($i = 0; $i -lt $Runs; $i++) {
      $argsList = @()
      if ($mode -eq 'Bytecode') { $argsList += '--bytecode' }
      $argsList += @('-e', 'main', $scriptPath)
      $watch = [System.Diagnostics.Stopwatch]::StartNew()
      $output = & $Kirton @argsList 2>$null
      $exitCode = $LASTEXITCODE
      $watch.Stop()
      if ($exitCode -ne 0) { throw "$mode/$scenario run failed with exit code $exitCode" }
      $observedOutput = ($output -join "`n").Trim()
      if ($mode -eq 'AST' -and $i -eq 0) { $expectedOutput = $observedOutput }
      if ($observedOutput -ne $expectedOutput) { throw "$mode/$scenario output differs from AST baseline" }
      $times += $watch.Elapsed.TotalMilliseconds
    }
    $sorted = @($times | Sort-Object)
    $median = $sorted[[int][Math]::Floor($sorted.Count / 2)]
    "{0}/{1}: median {2:N2} ms across {3} runs" -f $scenario, $mode, $median, $Runs
  }
}
$profileDir = [System.IO.Path]::GetTempPath()
foreach ($scenario in $scenarios) {
  $scriptPath = Join-Path $PSScriptRoot "bench_$scenario.kir"
  $astStats = Join-Path $profileDir ("kirton-ast-" + [guid]::NewGuid().ToString('N') + '.txt')
  $bcStats = Join-Path $profileDir ("kirton-bc-" + [guid]::NewGuid().ToString('N') + '.txt')
  $astProfileOutput = & $Profiler ast $scriptPath $Runs $astStats
  $bcProfileOutput = & $Profiler bytecode $scriptPath $Runs $bcStats
  if (($astProfileOutput -join "`n").Trim() -ne ($bcProfileOutput -join "`n").Trim()) {
    throw "Direct AST/Bytecode output mismatch for $scenario"
  }
  Get-Content $astStats
  Get-Content $bcStats
  Remove-Item -LiteralPath $astStats, $bcStats -ErrorAction SilentlyContinue
}
Pop-Location
