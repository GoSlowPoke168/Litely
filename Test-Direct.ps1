# The direct GPU decoder against the lean path on a copy made by Litely --convert, plus its fallbacks.
param([string]$Clip = (Join-Path $PSScriptRoot 'build\wallpapers\hoshimi-miyabi-1080p.mp4'), [int]$Seconds = 12)
$ErrorActionPreference = 'Stop'
Add-Type @'
using System;
using System.IO;
public static class DirectStills {
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
$exe = Join-Path $PSScriptRoot 'build\Litely.exe'
$scratch = Join-Path ([IO.Path]::GetTempPath()) ("LitelyDirect-" + [Guid]::NewGuid())
New-Item -ItemType Directory $scratch | Out-Null
function Invoke-Probe($file, [hashtable]$Environment = @{}) {
    $report = Join-Path $scratch ([Guid]::NewGuid().ToString() + '.txt')
    $start = [Diagnostics.ProcessStartInfo]::new($exe, ('--probe "{0}" "{1}"' -f $file, $report))
    $start.UseShellExecute = $false
    $start.Environment['LITELY_PROBE_SECONDS'] = "$Seconds"
    foreach ($key in $Environment.Keys) { $start.Environment[$key] = $Environment[$key] }
    $probe = [Diagnostics.Process]::Start($start)
    if (!$probe.WaitForExit(1000 * ($Seconds + 40))) { $probe.Kill(); throw "Probe timed out on $file" }
    $text = "$(Get-Content -LiteralPath $report -Raw -ErrorAction SilentlyContinue)"
    if ($probe.ExitCode -ne 0) { throw "Probe failed on $file`n$text" }
    $value = { param($pattern) if ($text -match $pattern) { [double]$Matches[1] } else { throw "Report lacks $pattern`n$text" } }
    [pscustomobject]@{
        Text = $text; Report = $report
        Pipeline = if ($text -match '(?m)^pipeline (\w+)') { $Matches[1] } else { '' }
        Cpu = (& $value 'process CPU seconds ([\d.]+)') / (& $value 'sample wall seconds ([\d.]+)')
        Ram = & $value 'playing working set MiB ([\d.]+)'; Gpu = & $value 'playing local GPU allocation MiB ([\d.]+)'
        Frames = & $value 'pacing frames (\d+)'; Expected = & $value 'pacing frames \d+ expected (\d+)'; Late = & $value 'expected \d+ late (\d+)'
    }
}
$failures = @()
try {
    $made = Start-Process $exe -ArgumentList ('--convert "{0}" "{1}" 0 0 1' -f $Clip, (Join-Path $scratch 'copy-lowmem.mp4')) -Wait -PassThru
    $converted = Get-ChildItem $scratch -Filter '*-lowmem.mp4' | Select-Object -First 1
    if ($made.ExitCode -or !$converted) { throw "Litely --convert made no lowest-memory copy (exit $($made.ExitCode))" }
    'converted copy: {0:N1} MiB (source {1:N1} MiB)' -f ($converted.Length / 1MB), ((Get-Item $Clip).Length / 1MB)
    $direct = Invoke-Probe $converted.FullName
    $lean = Invoke-Probe $converted.FullName @{ LITELY_PIPELINE = 'lean' }
    foreach ($run in $direct, $lean) { '{0,-7} cpu {1:P1} of a core  ram {2:N1} MiB  gpu {3:N1} MiB  frames {4}/{5} late {6}' -f $run.Pipeline, $run.Cpu, $run.Ram, $run.Gpu, $run.Frames, $run.Expected, $run.Late }
    if ($direct.Pipeline -ne 'direct') { $failures += "Converted file played on '$($direct.Pipeline)', expected direct" }
    if ($lean.Pipeline -ne 'lean') { $failures += "LITELY_PIPELINE=lean played on '$($lean.Pipeline)'" }
    if ($direct.Late -ne 0 -or [math]::Abs($direct.Frames - $direct.Expected) -gt 2) { $failures += "Direct pacing: $($direct.Frames) of $($direct.Expected), late $($direct.Late)" }
    if ($direct.Gpu -gt $lean.Gpu - 10) { $failures += "Direct GPU memory $($direct.Gpu) MiB is not 10 MiB under lean's $($lean.Gpu)" }
    foreach ($still in '.first.bmp', '.bmp') {
        $difference = [DirectStills]::MaxDifference("$($direct.Report)$still", "$($lean.Report)$still")
        if ($still -eq '.first.bmp') {
            "first frame, direct vs lean: max channel difference $difference"
            if ($difference -gt 1) { $failures += "First-frame stills differ by $difference" }
        }
    }
    $original = Invoke-Probe $Clip
    "original file plays on: $($original.Pipeline)"
    if ($original.Pipeline -ne 'lean') { $failures += "The original file played on '$($original.Pipeline)', expected lean" }
    $broken = Invoke-Probe $converted.FullName @{ LITELY_TEST_DIRECT_FAIL = '90' }
    if ($broken.Text -notmatch 'direct decoder failed; falling back') { $failures += 'A decode failure did not report a fallback' }
    else { 'decode failure mid-playback: fell back and finished the probe' }
    $resumed = Invoke-Probe $converted.FullName
    if ($resumed.Text -notmatch 'loop restarted') { $failures += 'Seek and loop restart did not complete on direct' }
} finally {
    Remove-Item -LiteralPath $scratch -Recurse -Force -ErrorAction SilentlyContinue
}
if ($failures) { $failures | ForEach-Object { Write-Host "FAIL: $_" }; exit 1 }
'All direct decoder checks passed.'
