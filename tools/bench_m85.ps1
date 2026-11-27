# M8.5 ABBA GPU benchmark runner (spec/m8/m8_5_perf_and_nvidia_benchmark.md §2)
# Needs a -checks build (ENGINE_BENCH_FRAMES). Array parameters take comma-separated values:
#   .\tools\bench_m85.ps1 -Frames 2000 -Rounds 1 -Columns targetRis,targetTemporal,rtxpt,rtxgi
#   .\tools\bench_m85.ps1 -Frames 2000 -Rounds 2 -Poses "sponza.gallery","cafe tables" `
#       -ABBA "pipeline=rtxpt","pipeline=rtxpt sharc.queryBounces=1" -ABBALabels "q2,q1"
# A run fails when a validation layer DLL is loaded into the engine (vkconfig overrides survive the loader filter).
# Clocks are not locked here; the header prints the SM clock so a table records it (P0 wants nvidia-smi -lgc).
[CmdletBinding(PositionalBinding = $false)]
param(
    [string]$Exe = ".\build\windows-clang-release-checks\engine.exe",
    [string]$WindowSize = "2560x1440",
    [int]$Frames = 2000,
    [int]$Rounds = 1,
    [string[]]$Poses = @("sponza.gallery", "sponza.courtyard", "cafe tables", "cafe dining", "street windows"),
    [string[]]$Columns = @("shipped", "targetRis", "targetTemporal", "rtxpt", "rtxgi", "rtxdi"),
    [string[]]$ABBA = @(),
    [string]$ABBALabels = ""
)

$ErrorActionPreference = "Stop"

$poseDefs = @{
    "sponza.gallery"   = @{ model = $null; pose = $null }
    "sponza.courtyard" = @{ model = $null; pose = "courtyard" }
    "cafe tables"      = @{ model = "models/bistro/bistro.gltf"; pose = "cafe tables" }
    "cafe dining"      = @{ model = "models/bistro/bistro.gltf"; pose = "cafe dining" }
    "street windows"   = @{ model = "models/bistro/bistro.gltf"; pose = "street windows" }
}

$colDefs = @{
    "shipped"             = ""
    "targetRis"           = "pipeline=targetRis"
    "targetRis_sharc"     = "pipeline=targetRis frame.radianceCache=sharc"
    "targetRis_sharc_q1"  = "pipeline=targetRis frame.radianceCache=sharc sharc.queryBounces=1"
    "targetTemporal"      = "pipeline=targetTemporal"
    "targetTemp_sharc"    = "pipeline=targetTemporal frame.radianceCache=sharc"
    "targetTemp_sharc_q1" = "pipeline=targetTemporal frame.radianceCache=sharc sharc.queryBounces=1"
    "rtxpt"               = "pipeline=rtxpt"
    "rtxpt_q1"            = "pipeline=rtxpt sharc.queryBounces=1"
    "rtxgi"               = "pipeline=rtxgi"
    "rtxdi"               = "pipeline=rtxdi"
    "rawpt_off"           = "frame.indirectBounce=rawPt frame.radianceCache=off"
}

# every GpuTimerScope the bench logs; a renamed scope fails the run instead of reading as 0
$requiredScopes = @(
    "Ray start wait", "ReSTIR DI initial", "ReSTIR DI temporal", "ReSTIR DI shade", "NRC update", "SHaRC update",
    "SHaRC resolve", "Indirect", "NRC train", "NRC resolve", "ReSTIR GI temporal", "ReSTIR GI shade", "Glass",
    "DLSS-RR", "GPU total", "GPU span"
)

$exePath = (Resolve-Path $Exe).Path
$exeDir = Split-Path -Parent $exePath
$logPath = Join-Path $exeDir "logs\engine.log"

# restored in finally: a bench must not leave bench mode or disabled layers in the caller's session
$envKeys = @("VK_LOADER_LAYERS_ENABLE", "VK_LOADER_LAYERS_DISABLE", "VK_KHRONOS_VALIDATION_GPUAV_ENABLE",
    "ENGINE_WINDOW_SIZE", "ENGINE_BENCH_FRAMES", "ENGINE_MODEL", "ENGINE_BENCH_POSE", "ENGINE_CONFIG")
$savedEnv = @{}
foreach ($key in $envKeys) { $savedEnv[$key] = [Environment]::GetEnvironmentVariable($key, "Process") }

function Set-ProcessEnv([string]$key, $value) {
    [Environment]::SetEnvironmentVariable($key, $(if ([string]::IsNullOrEmpty($value)) { $null } else { $value }), "Process")
}

function Invoke-Engine {
    $process = Start-Process -FilePath $exePath -WorkingDirectory $exeDir -NoNewWindow -PassThru
    $validationLoaded = $false
    # layers load inside vkCreateInstance, right after the loader: sample the modules until the loader has been
    # resident for a few seconds, then stop so the measured frames run unobserved
    $deadline = (Get-Date).AddSeconds(60)
    $loaderSeenAt = $null
    while (-not $process.HasExited -and (Get-Date) -lt $deadline) {
        try {
            $process.Refresh()
            $names = $process.Modules | ForEach-Object { $_.ModuleName }
            if ($names -match '^VkLayer_khronos_validation\.dll$') { $validationLoaded = $true; break }
            if (-not $loaderSeenAt -and ($names -match '^vulkan-1\.dll$')) { $loaderSeenAt = Get-Date }
            if ($loaderSeenAt -and ((Get-Date) - $loaderSeenAt).TotalSeconds -gt 3) { break }
        } catch {
            # modules are unreadable while the process starts or exits
        }
        Start-Sleep -Milliseconds 250
    }
    if ($validationLoaded) {
        $process.Kill()
        $process.WaitForExit()
        throw "VkLayer_khronos_validation.dll is loaded: turn vkconfig's layer override off before benchmarking"
    }
    $process.WaitForExit()
    return $process.ExitCode
}

function Invoke-BenchRun([string]$poseName, [string]$configText) {
    if (-not $poseDefs.ContainsKey($poseName)) { throw "Unknown pose '$poseName' (known: $($poseDefs.Keys -join ', '))" }
    $p = $poseDefs[$poseName]
    Set-ProcessEnv "VK_LOADER_LAYERS_ENABLE" $null
    Set-ProcessEnv "VK_LOADER_LAYERS_DISABLE" "*validation*"
    Set-ProcessEnv "VK_KHRONOS_VALIDATION_GPUAV_ENABLE" "false"
    Set-ProcessEnv "ENGINE_WINDOW_SIZE" $WindowSize
    Set-ProcessEnv "ENGINE_BENCH_FRAMES" "$Frames"
    Set-ProcessEnv "ENGINE_MODEL" $p.model
    Set-ProcessEnv "ENGINE_BENCH_POSE" $p.pose
    Set-ProcessEnv "ENGINE_CONFIG" $configText

    $exitCode = Invoke-Engine
    if ($exitCode -ne 0) {
        throw "Bench run failed ($poseName | $configText) with exit code $exitCode"
    }
    $scopes = @{}
    foreach ($line in (Get-Content $logPath | Where-Object { $_ -match '\[Bench\]' })) {
        if ($line -match '\[Bench\]\s+(.+?)\s+mean\s+([0-9.]+)\s+ms') {
            $scopes[$Matches[1].Trim()] = [double]$Matches[2]
        } elseif ($line -match '\[Bench\]\s+SHaRC:\s+([0-9]+)\s+/\s+([0-9]+)\s+occupied\s+\(([0-9.]+)%\),\s+([0-9]+)\s+insert overflows') {
            $scopes["SharcOccupiedPct"] = [double]$Matches[3]
            $scopes["SharcOverflows"] = [int]$Matches[4]
        }
    }
    $missing = $requiredScopes | Where-Object { -not $scopes.ContainsKey($_) }
    if ($missing) {
        throw "Bench log of ($poseName | $configText) lacks scopes: $($missing -join ', ')"
    }
    return [PSCustomObject]@{
        Pose     = $poseName
        Config   = $configText
        DI       = $scopes["ReSTIR DI initial"] + $scopes["ReSTIR DI temporal"] + $scopes["ReSTIR DI shade"]
        Indirect = $scopes["Indirect"]
        SharcUpd = $scopes["SHaRC update"]
        SharcRes = $scopes["SHaRC resolve"]
        Sharc    = $scopes["SHaRC update"] + $scopes["SHaRC resolve"]
        NRC      = $scopes["NRC update"] + $scopes["NRC train"] + $scopes["NRC resolve"]
        RestirGI = $scopes["ReSTIR GI temporal"] + $scopes["ReSTIR GI shade"]
        Glass    = $scopes["Glass"]
        DLSS     = $scopes["DLSS-RR"]
        RayWait  = $scopes["Ray start wait"]
        Total    = $scopes["GPU total"]
        Span     = $scopes["GPU span"]
        SharcOcc = if ($scopes.ContainsKey("SharcOccupiedPct")) { $scopes["SharcOccupiedPct"] } else { 0.0 }
        SharcOvf = if ($scopes.ContainsKey("SharcOverflows")) { $scopes["SharcOverflows"] } else { 0 }
    }
}

function Get-Stats([object[]]$runs, [string]$prop) {
    $m = $runs | Measure-Object -Property $prop -Average -Minimum -Maximum
    return [PSCustomObject]@{ Mean = $m.Average; Spread = $m.Maximum - $m.Minimum }
}

function Format-Row([string]$pose, [string]$label, [object[]]$runs) {
    $avg = { param($prop) (Get-Stats $runs $prop).Mean }
    $total = Get-Stats $runs "Total"
    return "| {0} | {1} | {2:F3} | {3:F3} | {4:F3} | {5:F3} | {6:F3} | {7:F3} | {8:F3} | {9:F3} | {10:F3} | {11:F3} |" -f `
        $pose, $label, (& $avg "DI"), (& $avg "Indirect"), (& $avg "Sharc"), (& $avg "NRC"), `
        (& $avg "RestirGI"), (& $avg "Glass"), (& $avg "DLSS"), $total.Mean, $total.Spread, (& $avg "Span")
}

function Format-Delta([string]$name, [object[]]$runsA, [object[]]$runsB, [string]$prop) {
    $a = Get-Stats $runsA $prop
    $b = Get-Stats $runsB $prop
    $delta = $b.Mean - $a.Mean
    $spread = [Math]::Max($a.Spread, $b.Spread)
    $verdict = if ($runsA.Count -lt 2) { "one run per side: no spread" }
               elseif ([Math]::Abs($delta) -gt $spread) { "beyond spread" } else { "within spread" }
    return "{0} {1:+0.000;-0.000} ms (spread {2:F3}, {3})" -f $name, $delta, $spread, $verdict
}

try {
    $clock = & nvidia-smi --query-gpu=name,clocks.sm,clocks.max.sm,driver_version --format=csv,noheader 2>$null
    Write-Host "GPU: $clock | $WindowSize | $Frames frames | $Rounds round(s)"
    Write-Host "| Pose | Config | DI | Indirect | SHaRC | NRC | ReSTIR GI | Glass | DLSS-RR | GPU total | total spread | GPU span |"
    Write-Host "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|"

    if ($ABBA.Count -gt 0) {
        if ($ABBA.Count -ne 2) { throw "-ABBA takes exactly two configs, comma-separated: -ABBA ""a"",""b""" }
        $labels = if ($ABBALabels) { $ABBALabels.Split(",") } else { @("A", "B") }
        foreach ($pose in $Poses) {
            $runsA = @()
            $runsB = @()
            for ($r = 0; $r -lt $Rounds; $r++) {
                $runsA += Invoke-BenchRun $pose $ABBA[0]
                $runsB += Invoke-BenchRun $pose $ABBA[1]
                $runsB += Invoke-BenchRun $pose $ABBA[1]
                $runsA += Invoke-BenchRun $pose $ABBA[0]
            }
            Write-Host (Format-Row $pose $labels[0] $runsA)
            Write-Host (Format-Row $pose $labels[1] $runsB)
            Write-Host ("  -> {0} - {1}: {2}; {3}; {4}" -f $labels[1], $labels[0],
                (Format-Delta "Indirect" $runsA $runsB "Indirect"), (Format-Delta "GPU total" $runsA $runsB "Total"),
                (Format-Delta "GPU span" $runsA $runsB "Span"))
        }
    } else {
        foreach ($pose in $Poses) {
            foreach ($col in $Columns) {
                $cfg = if ($colDefs.ContainsKey($col)) { $colDefs[$col] } else { $col }
                $runs = @()
                for ($r = 0; $r -lt $Rounds; $r++) {
                    $runs += Invoke-BenchRun $pose $cfg
                }
                Write-Host (Format-Row $pose $col $runs)
            }
        }
    }
} finally {
    foreach ($key in $envKeys) { Set-ProcessEnv $key $savedEnv[$key] }
}
