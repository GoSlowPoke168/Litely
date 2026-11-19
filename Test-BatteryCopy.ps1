param([int]$Timeout = 180)
$ErrorActionPreference = 'Stop'
Add-Type -ReferencedAssemblies System.Diagnostics.Process, System.ComponentModel.Primitives, System.Collections.NonGeneric @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class BatteryTest {
    public delegate bool Visitor(IntPtr w, IntPtr unused);
    [DllImport("user32")] static extern bool EnumThreadWindows(uint thread, Visitor visit, IntPtr unused);
    [DllImport("user32", CharSet = CharSet.Unicode)] static extern int GetClassName(IntPtr w, StringBuilder cls, int n);
    [DllImport("user32")] public static extern bool PostMessage(IntPtr w, uint m, IntPtr a, IntPtr b);
    public static IntPtr Find(int pid, string cls) {
        IntPtr found = IntPtr.Zero;
        foreach (System.Diagnostics.ProcessThread t in System.Diagnostics.Process.GetProcessById(pid).Threads)
            EnumThreadWindows((uint)t.Id, (w, x) => { var s = new StringBuilder(128); GetClassName(w, s, 128); if (s.ToString() != cls) return true; found = w; return false; }, IntPtr.Zero);
        return found;
    }
}
'@
$wallpapers = Join-Path $PSScriptRoot 'build\wallpapers'
$miyabi = Join-Path $wallpapers 'hoshimi-miyabi-1080p.mp4'
$furina = Join-Path $wallpapers 'furina-in-the-void4k-1080p.mp4'
$ffmpeg = python -c "import imageio_ffmpeg;print(imageio_ffmpeg.get_ffmpeg_exe())"
function New-Data($clip, $batteryFps) {
    $data = Join-Path ([IO.Path]::GetTempPath()) ("LitelyBattery-" + [Guid]::NewGuid())
    New-Item -ItemType Directory (Join-Path $data 'Litely') | Out-Null
    [IO.File]::WriteAllText((Join-Path $data 'Litely\settings.ini'), "[wallpaper]`r`nfile=$clip`r`npaused=0`r`ntime=0`r`nbatteryFps=$batteryFps`r`n", [Text.Encoding]::Unicode)
    $data
}
function Start-Litely($data, [switch]$Visible) {
    Get-Process Litely -ErrorAction SilentlyContinue | Stop-Process
    Start-Sleep -Milliseconds 400
    $start = [Diagnostics.ProcessStartInfo]::new((Join-Path $PSScriptRoot 'build\Litely.exe'))
    $start.UseShellExecute = $false
    $start.Environment['LOCALAPPDATA'] = $data
    $start.Environment['LITELY_TEST_SKIP_PERSONALIZATION'] = '1'
    if ($Visible) { $start.Environment['LITELY_TEST_VISIBLE'] = '1' }
    [Diagnostics.Process]::Start($start)
}
function Stop-Litely($app) {
    $window = [BatteryTest]::Find($app.Id, 'LitelyController')
    [BatteryTest]::PostMessage($window, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    if (!$app.WaitForExit(5000)) { $app.Kill(); throw 'Litely did not exit' }
}
function Get-Copies($data) { @(Get-ChildItem (Join-Path $data 'Litely\optimized') -Recurse -Filter '*-original-30fps.mp4' -ErrorAction SilentlyContinue) }
# A conversion in progress writes a .part file next to where the copy will go.
function Get-Partial($data) { @(Get-ChildItem (Join-Path $data 'Litely\optimized') -Recurse -Filter '*.part' -ErrorAction SilentlyContinue) }
function Watch-Partial($data, $seconds) {
    for ($i = 0; $i -lt $seconds * 10; $i++) { if (Get-Partial $data) { return $true }; Start-Sleep -Milliseconds 100 }
    $false
}
$folders = @()
try {
    $data = New-Data $miyabi 1; $folders += $data
    $app = Start-Litely $data
    for ($i = 0; $i -lt $Timeout -and !(Get-Copies $data); $i++) { Start-Sleep -Seconds 1 }
    Start-Sleep -Seconds 2
    $copies = Get-Copies $data
    if ($copies.Count -ne 1) { Stop-Litely $app; throw "Expected one 30 fps copy, found $($copies.Count)" }
    $about = (& $ffmpeg -hide_banner -i $copies[0].FullName 2>&1 | Out-String)
    if ($about -notmatch '1920x1080' -or $about -notmatch ', 30 fps') { Stop-Litely $app; throw "Copy is not 1920x1080 at 30 fps:`n$about" }
    Stop-Litely $app
    'copy made once: ' + $copies[0].Name
    $written = $copies[0].LastWriteTimeUtc
    $app = Start-Litely $data
    $started = Watch-Partial $data 10
    Stop-Litely $app
    if ($started) { throw 'A conversion started although the copy exists' }
    if ((Get-Copies $data)[0].LastWriteTimeUtc -ne $written) { throw 'The existing copy was rewritten' }
    'existing copy reused'
    foreach ($case in @{ Clip = $furina; Fps = 1; Name = '30 fps source' }, @{ Clip = $miyabi; Fps = 0; Name = 'setting off' }) {
        $data = New-Data $case.Clip $case.Fps; $folders += $data
        $app = Start-Litely $data
        $started = Watch-Partial $data 15
        Stop-Litely $app
        if ($started -or (Get-Copies $data)) { throw "$($case.Name): a copy was started" }
        "$($case.Name): no copy"
    }
    $data = New-Data $miyabi 1; $folders += $data
    $app = Start-Litely $data
    if (!(Watch-Partial $data 5)) { Stop-Litely $app; throw 'No conversion was running shortly after launch' }
    Stop-Litely $app
    Start-Sleep -Seconds 1
    if ((Get-Partial $data) -or (Get-Copies $data)) { throw 'A partial copy was left behind' }
    'exit cancels the conversion'
    # A killed Litely must not leave its conversion running.
    $data = New-Data $miyabi 1; $folders += $data
    $app = Start-Litely $data
    if (!(Watch-Partial $data 5)) { Stop-Litely $app; throw 'No conversion was running shortly after launch' }
    $app.Kill(); $app.WaitForExit(); Start-Sleep -Milliseconds 300
    $left = @(Get-CimInstance Win32_Process -Filter "Name='Litely.exe'" | Where-Object CommandLine -like '*--convert*')
    if ($left) { $left | ForEach-Object { Stop-Process -Id $_.ProcessId -Force }; throw 'The conversion kept running after Litely was killed' }
    'killing Litely stops the conversion'
    # Once the copy is made, the encoder's memory must not stay in Litely.
    $data = New-Data $miyabi 0; $folders += $data
    $app = Start-Litely $data -Visible; Start-Sleep -Seconds 20; $app.Refresh(); $base = $app.PrivateMemorySize64; Stop-Litely $app
    $data = New-Data $miyabi 1; $folders += $data
    $app = Start-Litely $data -Visible
    for ($i = 0; $i -lt $Timeout -and !(Get-Copies $data); $i++) { Start-Sleep -Seconds 1 }
    Start-Sleep -Seconds 15; $app.Refresh(); $after = $app.PrivateMemorySize64; Stop-Litely $app
    $extra = ($after - $base) / 1MB
    if (!(Get-Copies $data) -or $extra -gt 10) { throw ('Litely kept {0:N0} MiB more after making the copy' -f $extra) }
    'memory returned after the copy ({0:N0} MiB difference)' -f $extra
} finally {
    Get-Process Litely -ErrorAction SilentlyContinue | Stop-Process
    Start-Sleep -Milliseconds 500
    $folders | ForEach-Object { Remove-Item -LiteralPath $_ -Recurse -Force -ErrorAction SilentlyContinue }
}
'All battery copy checks passed.'
