param([string]$Clip = (Join-Path $PSScriptRoot '..\build\wallpapers\hoshimi-miyabi-1080p.mp4'))
$ErrorActionPreference = 'Stop'
Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class LitelyWindow {
    public delegate bool Visitor(IntPtr w, IntPtr unused);
    [DllImport("user32")] static extern bool EnumThreadWindows(uint thread, Visitor visit, IntPtr unused);
    [DllImport("user32", CharSet = CharSet.Unicode)] static extern int GetClassName(IntPtr w, StringBuilder cls, int n);
    public static IntPtr Find(int pid, string cls) {
        IntPtr found = IntPtr.Zero;
        foreach (System.Diagnostics.ProcessThread t in System.Diagnostics.Process.GetProcessById(pid).Threads)
            EnumThreadWindows((uint)t.Id, (w, x) => { var s = new StringBuilder(128); GetClassName(w, s, 128); if (s.ToString() != cls) return true; found = w; return false; }, IntPtr.Zero);
        return found;
    }
    [DllImport("user32")] public static extern bool PostMessage(IntPtr w, uint m, IntPtr a, IntPtr b);
    [DllImport("user32")] public static extern IntPtr SendMessageTimeout(IntPtr w, uint m, IntPtr a, IntPtr b, uint flags, uint ms, out IntPtr result);
    [DllImport("user32", CharSet = CharSet.Unicode)] static extern int GetWindowText(IntPtr w, StringBuilder s, int n);
    public static string Title(IntPtr w) { var b = new StringBuilder(256); GetWindowText(w, b, 256); return b.ToString(); }
    public static bool Responds(IntPtr w) { IntPtr r; return SendMessageTimeout(w, 0, IntPtr.Zero, IntPtr.Zero, 2, 1000, out r) != IntPtr.Zero; }
}
'@
Get-Process Litely -ErrorAction SilentlyContinue | Stop-Process
Start-Sleep -Milliseconds 500
$data = Join-Path ([IO.Path]::GetTempPath()) ("LitelyStress-" + [Guid]::NewGuid())
New-Item -ItemType Directory $data | Out-Null
$start = [Diagnostics.ProcessStartInfo]::new((Join-Path $PSScriptRoot '..\build\Litely.exe'), ('"{0}"' -f (Get-Item -LiteralPath $Clip).FullName))
$start.UseShellExecute = $false
$start.Environment['LOCALAPPDATA'] = $data
$start.Environment['LITELY_TEST_SKIP_PERSONALIZATION'] = '1'
$start.Environment['LITELY_TEST_VISIBLE'] = '1'
$app = [Diagnostics.Process]::Start($start)
try {
    $window = [IntPtr]::Zero
    for ($i = 0; $i -lt 200 -and $window -eq [IntPtr]::Zero; $i++) { Start-Sleep -Milliseconds 10; $window = [LitelyWindow]::Find($app.Id, 'LitelyController') }
    if ($window -eq [IntPtr]::Zero) { throw 'Controller window not found' }
    function Assert-State($phase, $expected) {
        Start-Sleep -Seconds 3
        if ($app.HasExited) { throw "$phase`: Litely exited ($($app.ExitCode))" }
        if (![LitelyWindow]::Responds($window)) { throw "$phase`: Litely is not responding" }
        $title = [LitelyWindow]::Title($window)
        if ($title -ne $expected) { throw "$phase`: expected '$expected', got '$title'" }
        "$phase`: $title"
    }
    function Send-Storm($message, $argument, $count, $gap) {
        1..$count | ForEach-Object { [LitelyWindow]::PostMessage($window, $message, [IntPtr]$argument, [IntPtr]::Zero) | Out-Null; Start-Sleep -Milliseconds $gap }
    }
    Send-Storm 0x0111 2 21 30
    Assert-State 'odd pause/resume storm from launch' 'Litely - paused'
    Send-Storm 0x0111 2 21 30
    Assert-State 'odd pause/resume storm while paused' 'Litely - playing'
    Send-Storm 0x007E 32 30 50
    Assert-State 'display-change storm' 'Litely - playing'
    Send-Storm 0x0111 2 20 30
    Assert-State 'even pause/resume storm while playing' 'Litely - playing'
    [LitelyWindow]::PostMessage($window, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    if (!$app.WaitForExit(5000)) { throw 'Litely did not exit within 5 s of WM_CLOSE' }
    'clean exit'
} finally {
    if (!$app.HasExited) { $app.Kill() }
    Remove-Item -LiteralPath $data -Recurse -Force -ErrorAction SilentlyContinue
}
