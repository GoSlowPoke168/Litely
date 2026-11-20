param([int]$Runs = 5, [int]$Seconds = 20)
$ErrorActionPreference = 'Stop'
Add-Type @'
using System;
using System.IO;
public static class Stills {
    // Largest per-channel difference between two 32-bit BMPs of the same size; alpha is ignored.
    public static int MaxDifference(string a, string b) {
        byte[] x = File.ReadAllBytes(a), y = File.ReadAllBytes(b);
        if (x.Length != y.Length) return 255;
        int start = BitConverter.ToInt32(x, 10), worst = 0;
        for (int i = start; i < x.Length; i++) {
            if ((i - start) % 4 == 3) continue;
            int d = Math.Abs(x[i] - y[i]);
            if (d > worst) worst = d;
        }
        return worst;
    }
}
'@
$exe = Join-Path $PSScriptRoot '..\build\Litely.exe'
$scratch = Join-Path ([IO.Path]::GetTempPath()) ("LitelySavings-" + [Guid]::NewGuid())
New-Item -ItemType Directory $scratch | Out-Null
function Invoke-Probe($clip, $pipeline, [switch]$Topmost) {
    $report = Join-Path $scratch ([Guid]::NewGuid().ToString() + '.txt')
    $start = [Diagnostics.ProcessStartInfo]::new($exe, ('--probe "{0}" "{1}"' -f $clip, $report))
    $start.UseShellExecute = $false
    $start.Environment['LITELY_PROBE_SECONDS'] = "$Seconds"
    if ($pipeline -eq 'legacy') { $start.Environment['LITELY_PIPELINE'] = 'legacy' }
    if ($Topmost) { $start.Environment['LITELY_PROBE_TOPMOST'] = '1' }
    $probe = [Diagnostics.Process]::Start($start)
    if (!$probe.WaitForExit(1000 * ($Seconds + 40))) { $probe.Kill(); throw "Probe timed out ($pipeline, $clip)" }
    $text = Get-Content -LiteralPath $report -Raw
    if ($probe.ExitCode -ne 0) { throw "Probe failed ($pipeline, $clip):`n$text" }
    if ($text -notmatch "(?m)^pipeline $pipeline\s*$") { throw "Expected pipeline $pipeline for $clip" }
    $value = { param($pattern) if ($text -match $pattern) { [double]$Matches[1] } else { throw "Report lacks $pattern" } }
    [pscustomobject]@{
        Cpu      = (& $value 'process CPU seconds ([\d.]+)') / (& $value 'sample wall seconds ([\d.]+)')
        Ram      = & $value 'playing working set MiB ([\d.]+)'
        Gpu      = & $value 'playing local GPU allocation MiB ([\d.]+)'
        Frames   = & $value 'pacing frames (\d+)'
        Expected = & $value 'pacing frames \d+ expected (\d+)'
        Late     = & $value 'expected \d+ late (\d+)'
        Report   = $report
    }
}
function Get-Median($values) { $sorted = @($values | Sort-Object); $sorted[[int][math]::Floor(($sorted.Count - 1) / 2)] }
$failures = @()
try {
    foreach ($name in 'hoshimi-miyabi-1080p.mp4', 'furina-in-the-void4k-1080p.mp4') {
        $clip = Join-Path $PSScriptRoot "..\build\wallpapers\$name"
        $legacy = @(); $lean = @()
        for ($i = 0; $i -lt $Runs; $i++) { $legacy += Invoke-Probe $clip legacy; $lean += Invoke-Probe $clip lean }
        foreach ($metric in @{ Name = 'Cpu'; Limit = 0.7; Format = '{0:P1} of a core' }, @{ Name = 'Ram'; Limit = 0.6; Format = '{0:N1} MiB' }, @{ Name = 'Gpu'; Limit = 0.9; Format = '{0:N1} MiB' }) {
            $old = Get-Median $legacy.($metric.Name); $new = Get-Median $lean.($metric.Name); $ratio = $new / $old
            '{0,-32} {1,-4} legacy {2,-18} lean {3,-18} ratio {4:N2} (limit {5})' -f $name, $metric.Name, ($metric.Format -f $old), ($metric.Format -f $new), $ratio, $metric.Limit
            if ($ratio -gt $metric.Limit) { $failures += "$name $($metric.Name) ratio $([math]::Round($ratio, 2)) exceeds $($metric.Limit)" }
        }
        foreach ($run in $lean) {
            if ($run.Late -ne 0) { $failures += "$name lean run had $($run.Late) late frames" }
            if ([math]::Abs($run.Frames - $run.Expected) -gt 2) { $failures += "$name lean run showed $($run.Frames) frames, expected $($run.Expected)" }
        }
        $difference = [Stills]::MaxDifference("$($legacy[0].Report).first.bmp", "$($lean[0].Report).first.bmp")
        '{0,-32} first-frame still, lean vs legacy: max channel difference {1}' -f $name, $difference
        if ($difference -gt 1) { $failures += "$name first-frame stills differ by $difference" }
    }
    Write-Warning 'A video window appears at the top-left of the screen for a few seconds.'
    $clip = Join-Path $PSScriptRoot '..\build\wallpapers\furina-in-the-void4k-1080p.mp4'
    $shown = Invoke-Probe $clip lean -Topmost
    $difference = [Stills]::MaxDifference("$($shown.Report).bmp", "$($shown.Report).screen.bmp")
    "paused still vs screen: max channel difference $difference"
    if ($difference -gt 1) { $failures += "Paused still differs from the screen by $difference" }
} finally {
    Remove-Item -LiteralPath $scratch -Recurse -Force -ErrorAction SilentlyContinue
}
if ($failures) { $failures | ForEach-Object { Write-Host "FAIL: $_" }; exit 1 }
'All savings, pacing and pixel checks passed.'
