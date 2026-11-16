param([Parameter(Mandatory,Position=0,ValueFromRemainingArguments)][string[]]$Clips)
$ErrorActionPreference = 'Stop'
$exe = Join-Path $PSScriptRoot 'build\Litely.exe'
foreach ($clip in $Clips) {
    $file = Get-Item -LiteralPath $clip
    $report = Join-Path ([IO.Path]::GetTempPath()) ("Litely-" + [Guid]::NewGuid() + ".txt")
    try {
        $arguments = '--probe "{0}" "{1}"' -f $file.FullName, $report
        $player = Start-Process $exe -ArgumentList $arguments -WindowStyle Hidden -PassThru
        if (!$player.WaitForExit(40000)) { $player.Kill(); throw 'Renderer timed out' }
        $file.Name
        Get-Content -LiteralPath $report
        if ($player.ExitCode -ne 0) { throw "Renderer failed ($($player.ExitCode))" }
        Add-Type -AssemblyName System.Drawing
        $bitmap=[Drawing.Bitmap]::FromFile("$report.bmp")
        try {
            if ($bitmap.Width -le 0 -or $bitmap.Height -le 0) { throw "Invalid paused frame" }
            "Paused image {0} x {1}" -f $bitmap.Width,$bitmap.Height
        } finally { $bitmap.Dispose() }
    } finally {
        foreach ($path in $report, "$report.bmp", "$report.bmp.tmp") {
            if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path }
        }
    }
}

