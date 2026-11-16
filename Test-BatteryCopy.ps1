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
function Start-Litely($data) {
    Get-Process Litely -ErrorAction SilentlyContinue | Stop-Process
    Start-Sleep -Milliseconds 400
    $start = [Diagnostics.ProcessStartInfo]::new((Join-Path $PSScriptRoot 'build\Litely.exe'))
    $start.UseShellExecute = $false
    $start.Environment['LOCALAPPDATA'] = $data
    $start.Environment['LITELY_TEST_SKIP_PERSONALIZATION'] = '1'
    [Diagnostics.Process]::Start($start)
}
function Stop-Litely($app) {
    $window = [BatteryTest]::Find($app.Id, 'LitelyController')
    [BatteryTest]::PostMessage($window, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    if (!$app.WaitForExit(5000)) { $app.Kill(); throw 'Litely did not exit' }
}
function Get-Copies($data) { @(Get-ChildItem (Join-Path $data 'Litely\optimized') -Recurse -Filter '*-original-30fps.mp4' -ErrorAction SilentlyContinue) }
function Get-Descendants($id) {
    $all = Get-CimInstance Win32_Process
    $found = @(); $queue = @($id)
    while ($queue) { $next = @($all | Where-Object { $_.ParentProcessId -in $queue }); $found += $next; $queue = $next.ProcessId }
    $found
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
    Start-Sleep -Seconds 10
    $children = @(Get-Descendants $app.Id | Where-Object Name -match 'python|ffmpeg')
    Stop-Litely $app
    if ($children) { throw 'A conversion started although the copy exists' }
    if ((Get-Copies $data)[0].LastWriteTimeUtc -ne $written) { throw 'The existing copy was rewritten' }
    'existing copy reused'
    foreach ($case in @{ Clip = $furina; Fps = 1; Name = '30 fps source' }, @{ Clip = $miyabi; Fps = 0; Name = 'setting off' }) {
        $data = New-Data $case.Clip $case.Fps; $folders += $data
        $app = Start-Litely $data
        Start-Sleep -Seconds 15
        $children = @(Get-Descendants $app.Id | Where-Object Name -match 'python|ffmpeg')
        Stop-Litely $app
        if ($children -or (Get-Copies $data)) { throw "$($case.Name): a copy was started" }
        "$($case.Name): no copy"
    }
    $data = New-Data $miyabi 1; $folders += $data
    $app = Start-Litely $data
    Start-Sleep -Milliseconds 700
    $children = @(Get-Descendants $app.Id)
    if (!$children) { Stop-Litely $app; throw 'No conversion was running shortly after launch' }
    Stop-Litely $app
    Start-Sleep -Seconds 3
    $alive = @($children | Where-Object { Get-Process -Id $_.ProcessId -ErrorAction SilentlyContinue })
    if ($alive) { throw "Conversion survived exit: $($alive.Name -join ', ')" }
    if (Get-Copies $data) { throw 'A partial copy was left behind' }
    'exit cancels the conversion'
} finally {
    Get-Process Litely -ErrorAction SilentlyContinue | Stop-Process
    Start-Sleep -Milliseconds 500
    $folders | ForEach-Object { Remove-Item -LiteralPath $_ -Recurse -Force -ErrorAction SilentlyContinue }
}
'All battery copy checks passed.'
