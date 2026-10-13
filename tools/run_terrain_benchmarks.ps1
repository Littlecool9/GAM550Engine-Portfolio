param(
    [Parameter(Mandatory = $true)][string]$PythonPath,
    [string]$Preset = 'windows-msvc-release',
    [int]$Repeats = 2,
    [string[]]$Variants = @(),
    [string]$OutputDirectory = ''
)

$ErrorActionPreference = 'Stop'
$benchmarkRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$executable = Join-Path $benchmarkRoot "builds/$Preset/MjolnirEngine.exe"
$settingsPath = Join-Path (Split-Path $executable -Parent) 'terrain_streaming.local.json'
$planPath = Join-Path $benchmarkRoot 'games/TerrainCheck/terrain_benchmark.json'
$variantPath = Join-Path $benchmarkRoot 'games/TerrainCheck/terrain_benchmark_variants.json'
if ($Repeats -lt 1) { throw 'Repeats must be positive' }
if (-not (Test-Path -LiteralPath $executable)) { throw "Missing executable: $executable" }
if ($OutputDirectory -eq '') {
    $OutputDirectory = Join-Path $benchmarkRoot ('builds/bench/' + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss'))
}
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$previousSettings = if (Test-Path -LiteralPath $settingsPath) { [IO.File]::ReadAllBytes($settingsPath) } else { $null }
$cases = Get-Content -Raw -LiteralPath $variantPath | ConvertFrom-Json
$tracePaths = @()
try {
    foreach ($case in $cases) {
        if ($Variants.Count -gt 0 -and $case.name -notin $Variants) { continue }
        foreach ($rayTracing in @($false, $true)) {
            $pathName = if ($rayTracing) { 'rt' } else { 'raster' }
            for ($iteration = 0; $iteration -lt $Repeats; $iteration++) {
                [IO.File]::WriteAllText($settingsPath, ($case.overrides | ConvertTo-Json -Depth 10), [Text.UTF8Encoding]::new($false))
                $plan = Get-Content -Raw -LiteralPath $planPath | ConvertFrom-Json
                $stem = "$($case.name)-$pathName-$iteration"
                $tracePath = Join-Path $OutputDirectory "$stem.json"
                $generatedPlan = Join-Path $OutputDirectory "$stem.plan.json"
                $plan.output = $tracePath
                $plan.rayTracing = $rayTracing
                $plan.cacheState = if ($iteration -eq 0) { 'first-process-empty-residency-file-cache-uncontrolled' } else { 'warm-file-replay-empty-residency' }
                $plan | Add-Member -NotePropertyName revision -NotePropertyValue ((git -C $benchmarkRoot rev-parse HEAD).Trim())
                $plan | Add-Member -NotePropertyName runUtc -NotePropertyValue ([DateTime]::UtcNow.ToString('o'))
                [IO.File]::WriteAllText($generatedPlan, ($plan | ConvertTo-Json -Depth 12), [Text.UTF8Encoding]::new($false))
                $process = Start-Process -FilePath $executable -ArgumentList ('--terrain-benchmark "' + $generatedPlan + '"') `
                    -WorkingDirectory (Split-Path $executable -Parent) -WindowStyle Hidden -Wait -PassThru `
                    -RedirectStandardOutput (Join-Path $OutputDirectory "$stem.log") `
                    -RedirectStandardError (Join-Path $OutputDirectory "$stem.stderr.log")
                if ($process.ExitCode -ne 0) { throw "Benchmark failed: $stem ($($process.ExitCode))" }
                $trace = Get-Content -Raw -LiteralPath $tracePath | ConvertFrom-Json
                if (-not $trace.completed) { throw "Incomplete benchmark trace: $tracePath" }
                $tracePaths += $tracePath
                Write-Output "Completed $stem -> $tracePath"
            }
        }
    }
} finally {
    if ($null -ne $previousSettings) {
        [IO.File]::WriteAllBytes($settingsPath, $previousSettings)
    } elseif (Test-Path -LiteralPath $settingsPath) {
        Remove-Item -LiteralPath $settingsPath
    }
}
if ($tracePaths.Count -eq 0) { throw 'No benchmark variants selected' }
$plan = Get-Content -Raw -LiteralPath $planPath | ConvertFrom-Json
$reportThreshold = [double]$plan.fixedDeltaSeconds * 1000
& $PythonPath (Join-Path $benchmarkRoot 'tools/aggregate_terrain_benchmark.py') @tracePaths --out (Join-Path $OutputDirectory 'summary.json') --spike-ms $reportThreshold
if ($LASTEXITCODE -ne 0) { throw 'Benchmark aggregation failed' }
