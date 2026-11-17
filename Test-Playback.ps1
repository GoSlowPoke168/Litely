param([Parameter(Mandatory,Position=0,ValueFromRemainingArguments)][string[]]$Clips, [ValidateSet('direct','lean','legacy','any')][string]$Pipeline = 'any')
$ErrorActionPreference = 'Stop'
$exe = Join-Path $PSScriptRoot 'build\Litely.exe'
$env:LITELY_PIPELINE = if ($Pipeline -in 'legacy', 'lean') { $Pipeline } else { $null }
foreach ($clip in $Clips) {
    $file = Get-Item -LiteralPath $clip
    $report = Join-Path ([IO.Path]::GetTempPath()) ("Litely-" + [Guid]::NewGuid() + ".txt")
    try {
        $arguments = '--probe "{0}" "{1}"' -f $file.FullName, $report
        $player = Start-Process $exe -ArgumentList $arguments -WindowStyle Hidden -PassThru
        if (!$player.WaitForExit(40000 + 1000 * [int]$env:LITELY_PROBE_SECONDS)) { $player.Kill(); throw 'Renderer timed out' }
        $file.Name
        $lines = @(Get-Content -LiteralPath $report)
        $lines
        if ($player.ExitCode -ne 0) { throw "Renderer failed ($($player.ExitCode))" }
        if (!($lines -match '^pipeline (legacy|lean|direct)$')) { throw 'Report has no pipeline line' }
        if (!($lines -match '^pacing frames \d+ expected \d+ late \d+ worst [\d.]+ ms$')) { throw 'Report has no pacing line' }
        if ($Pipeline -ne 'any' -and !($lines -contains "pipeline $Pipeline")) { throw "Expected pipeline $Pipeline" }
        if (!(Test-Path -LiteralPath "$report.first.bmp")) { throw 'No first-frame still' }
        Add-Type -AssemblyName System.Drawing
        $bitmap=[Drawing.Bitmap]::FromFile("$report.bmp")
        try {
            if ($bitmap.Width -le 0 -or $bitmap.Height -le 0) { throw "Invalid paused frame" }
            "Paused image {0} x {1}" -f $bitmap.Width,$bitmap.Height
        } finally { $bitmap.Dispose() }
    } finally {
        foreach ($path in $report, "$report.bmp", "$report.bmp.tmp", "$report.first.bmp") {
            if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path }
        }
    }
}

