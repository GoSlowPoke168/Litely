$ErrorActionPreference = 'Stop'
Add-Type -ReferencedAssemblies System.Diagnostics.Process, System.ComponentModel.Primitives, System.Collections.NonGeneric @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class SwitchTest {
    public delegate bool Visitor(IntPtr w, IntPtr unused);
    [DllImport("user32")] static extern bool EnumThreadWindows(uint thread, Visitor visit, IntPtr unused);
    [DllImport("user32", CharSet = CharSet.Unicode)] static extern int GetClassName(IntPtr w, StringBuilder cls, int n);
    [DllImport("user32", CharSet = CharSet.Unicode)] static extern int GetWindowText(IntPtr w, StringBuilder s, int n);
    [DllImport("user32")] public static extern bool PostMessage(IntPtr w, uint m, IntPtr a, IntPtr b);
    [DllImport("user32")] static extern IntPtr SendMessageTimeout(IntPtr w, uint m, IntPtr a, IntPtr b, uint flags, uint ms, out IntPtr result);
    public static bool Responds(IntPtr w) { IntPtr r; return SendMessageTimeout(w, 0, IntPtr.Zero, IntPtr.Zero, 2, 1000, out r) != IntPtr.Zero; }
    public static string Title(IntPtr w) { var b = new StringBuilder(256); GetWindowText(w, b, 256); return b.ToString(); }
    public static IntPtr Find(int pid, string cls) {
        IntPtr found = IntPtr.Zero;
        foreach (System.Diagnostics.ProcessThread t in System.Diagnostics.Process.GetProcessById(pid).Threads)
            EnumThreadWindows((uint)t.Id, (w, x) => { var s = new StringBuilder(128); GetClassName(w, s, 128); if (s.ToString() != cls) return true; found = w; return false; }, IntPtr.Zero);
        return found;
    }
}
'@
$clip = Join-Path $PSScriptRoot 'build\wallpapers\hoshimi-miyabi-1080p.mp4'
$data = Join-Path ([IO.Path]::GetTempPath()) ("LitelySwitch-" + [Guid]::NewGuid())
New-Item -ItemType Directory (Join-Path $data 'Litely') | Out-Null
[IO.File]::WriteAllText((Join-Path $data 'Litely\settings.ini'), "[wallpaper]`r`nfile=$clip`r`npaused=0`r`ntime=0`r`nbatteryFps=1`r`n", [Text.Encoding]::Unicode)
$log = Join-Path $data 'swaps.log'
Get-Process Litely -ErrorAction SilentlyContinue | Stop-Process
Start-Sleep -Milliseconds 400
$start = [Diagnostics.ProcessStartInfo]::new((Join-Path $PSScriptRoot 'build\Litely.exe'))
$start.UseShellExecute = $false
$start.Environment['LOCALAPPDATA'] = $data
$start.Environment['LITELY_TEST_SKIP_PERSONALIZATION'] = '1'
$start.Environment['LITELY_TEST_VISIBLE'] = '1'
$start.Environment['LITELY_TEST_POWER'] = 'ac'
$start.Environment['LITELY_TEST_LOG'] = $log
$start.Environment['LITELY_TEST_POWER'] = 'ac'
$app = [Diagnostics.Process]::Start($start)
function Get-Copy { Get-ChildItem (Join-Path $data 'Litely\optimized') -Recurse -Filter '*-original-30fps.mp4' -ErrorAction SilentlyContinue | Select-Object -First 1 }
function Wait-Copy { for ($i = 0; $i -lt 180 -and !(Get-Copy); $i++) { Start-Sleep -Seconds 1 }; if (!(Get-Copy)) { throw 'The 30 fps copy was not made' } }
function Set-Power($battery) { [SwitchTest]::PostMessage($window, 0x8005, [IntPtr][int]$battery, [IntPtr]::Zero) | Out-Null }
function Wait-Title($expected, $seconds = 2) {
    for ($i = 0; $i -lt $seconds * 20; $i++) { if ([SwitchTest]::Title($window) -eq $expected) { return }; Start-Sleep -Milliseconds 50 }
    throw "Expected '$expected', got '$([SwitchTest]::Title($window))'"
}
function Assert-Swap($name, $limit) {
    $line = ''
    for ($i = 0; $i -lt 60 -and !$line.Contains($name); $i++) {
        Start-Sleep -Milliseconds 50
        $line = "$(Get-Content -LiteralPath $log -ErrorAction SilentlyContinue | Select-Object -Last 1)"
    }
    if (!($line -match '^swap (.+) at ([0-9.]+) first ([0-9.]+) (kept|reopened)$')) { throw "Unexpected log line '$line'" }
    if ($Matches[1] -ne $name) { throw "Swapped to '$($Matches[1])', expected '$name'" }
    if ($Matches[4] -ne 'kept') { throw 'The output was reopened, so the still could flash' }
    $jump = [double]$Matches[3] - [double]$Matches[2]
    if ($jump -lt 0 -or $jump -gt $limit) { throw "Position jumped by $jump s" }
    "swap to $name, position moved $([math]::Round($jump, 4)) s, output kept"
}
try {
    Start-Sleep -Seconds 3
    $window = [SwitchTest]::Find($app.Id, 'LitelyController')
    Wait-Title 'Litely - playing'
    Wait-Copy
    Start-Sleep -Seconds 2
    $copy = Get-Copy
    Set-Power $true
    Wait-Title 'Litely - playing (30 fps on battery)'
    Assert-Swap $copy.Name 0.07
    Start-Sleep -Seconds 2
    Set-Power $false
    Wait-Title 'Litely - playing'
    Assert-Swap (Split-Path $clip -Leaf) 0.04
    Remove-Item -LiteralPath $copy.Directory.FullName -Recurse -Force
    Set-Power $true
    Start-Sleep -Seconds 1
    if ($app.HasExited) { throw 'Litely exited after the copy was deleted' }
    Wait-Copy
    Wait-Title 'Litely - playing (30 fps on battery)' 5
    'deleted copy was remade and resumed'
    [SwitchTest]::PostMessage($window, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    if (!$app.WaitForExit(5000)) { throw 'Litely did not exit' }
    $start.Environment['LITELY_PIPELINE'] = 'legacy'
    $app = [Diagnostics.Process]::Start($start)
    Start-Sleep -Seconds 3
    $window = [SwitchTest]::Find($app.Id, 'LitelyController')
    Wait-Title 'Litely - playing'
    [SwitchTest]::PostMessage($window, 0x0111, [IntPtr]2, [IntPtr]::Zero) | Out-Null
    Wait-Title 'Litely - paused'
    Start-Sleep -Seconds 1
    Set-Power $true
    Start-Sleep -Seconds 2
    $app.Refresh(); $before = $app.TotalProcessorTime.TotalSeconds
    Start-Sleep -Seconds 2
    $app.Refresh(); $used = $app.TotalProcessorTime.TotalSeconds - $before
    if ([SwitchTest]::Title($window) -ne 'Litely - paused') { throw "A power change un-paused the fallback player: '$([SwitchTest]::Title($window))'" }
    if ($used -gt 0.05) { throw "The paused fallback player used $used CPU-seconds in 2 s after a power change" }
    'paused fallback player stays idle across a power change'
    [SwitchTest]::PostMessage($window, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    if (!$app.WaitForExit(5000)) { throw 'Litely did not exit' }
} finally {
    if (!$app.HasExited) { $app.Kill() }
    Start-Sleep -Milliseconds 500
    Remove-Item -LiteralPath $data -Recurse -Force -ErrorAction SilentlyContinue
}
'All battery switch checks passed.'
