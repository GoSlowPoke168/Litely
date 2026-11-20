param([Parameter(Mandatory)][string]$Clip)
$ErrorActionPreference = "Stop"
if (Get-Process Litely -ErrorAction SilentlyContinue) { throw "Exit the running tray app before this test." }
Add-Type -AssemblyName System.Windows.Forms
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
using System.Diagnostics;
using System.Threading.Tasks;
public class DesktopTest {
    [DllImport("dwmapi.dll")] static extern int DwmFlush();
    [DllImport("user32.dll")] static extern IntPtr SendMessageTimeout(IntPtr h,uint msg,IntPtr w,IntPtr l,uint flags,uint timeout,out UIntPtr result);
    public static Task<double[]> ObserveResume(IntPtr hwnd) {
        return Task.Run(() => {
            var clock=Stopwatch.StartNew();
            double maxDwm=0, maxTray=0;
            if(DwmFlush()!=0) throw new Exception("DwmFlush failed");
            PostMessage(hwnd,0x111,(IntPtr)2,IntPtr.Zero);
            while(clock.ElapsedMilliseconds<650){
                var start=clock.Elapsed.TotalMilliseconds;
                UIntPtr result;
                if(SendMessageTimeout(hwnd,0,IntPtr.Zero,IntPtr.Zero,2,2000,out result)==IntPtr.Zero)
                    throw new Exception("Tray unresponsive");
                maxTray=Math.Max(maxTray,clock.Elapsed.TotalMilliseconds-start);
                start=clock.Elapsed.TotalMilliseconds;
                if(DwmFlush()!=0) throw new Exception("DwmFlush failed");
                maxDwm=Math.Max(maxDwm,clock.Elapsed.TotalMilliseconds-start);
            }
            return new double[]{maxTray,maxDwm};
        });
    }
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string cls,IntPtr title);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h,System.Text.StringBuilder text,int size);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h,uint msg,IntPtr w,IntPtr l);
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr value);
}
"@
[void][DesktopTest]::SetProcessDpiAwarenessContext([IntPtr](-4))
$exe = (Get-Item (Join-Path $PSScriptRoot "..\build\Litely.exe")).FullName
$video = (Get-Item -LiteralPath $Clip).FullName
$testRoot = Join-Path ([IO.Path]::GetTempPath()) ("wallpaper-idle-" + [Guid]::NewGuid())
$config = Join-Path $testRoot "Litely"
[void](New-Item -ItemType Directory -Path $config)
$ini = Join-Path $config "settings.ini"
[IO.File]::WriteAllText($ini, "[wallpaper]`r`nfile=$video`r`npaused=1`r`ntime=0`r`n", [Text.Encoding]::Unicode)
$shell = New-Object -ComObject Shell.Application
$forms = @()
$oldLocal = $env:LOCALAPPDATA
$oldSkip = $env:LITELY_TEST_SKIP_PERSONALIZATION
function Wait-Player($predicate) {
    $limit = [DateTime]::UtcNow.AddSeconds(12)
    while ([DateTime]::UtcNow -lt $limit) {
        [Windows.Forms.Application]::DoEvents()
        $p = Get-Process Litely -ErrorAction SilentlyContinue | Where-Object Path -eq $exe | Select-Object -Last 1
        if ($p -and (& $predicate $p)) { return $p }
        Start-Sleep -Milliseconds 100
    }
    $h=[DesktopTest]::FindWindow("LitelyController",[IntPtr]::Zero)
    $title=New-Object Text.StringBuilder 256
    [void][DesktopTest]::GetWindowText($h,$title,256)
    Get-Content -LiteralPath $ini | Out-Host
    throw "Timed out in $stage. Controller $h, status $title, PID $($p.Id), RAM $($p.WorkingSet64)."
}
function Tray-Status {
    $text = New-Object Text.StringBuilder 256
    [void][DesktopTest]::GetWindowText($hwnd,$text,256)
    $text.ToString()
}
# Start-Process does not reliably forward a changed environment.
function Start-Test {
    $start=[Diagnostics.ProcessStartInfo]::new($exe)
    $start.UseShellExecute=$false
    $start.Environment['LOCALAPPDATA']=$testRoot
    $start.Environment['LITELY_TEST_SKIP_PERSONALIZATION']='1'
    [Diagnostics.Process]::Start($start)
}
try {
    $env:LITELY_TEST_SKIP_PERSONALIZATION="1"
    $env:LOCALAPPDATA=$testRoot
    $shell.MinimizeAll()
    $initial=Start-Test
    $env:LOCALAPPDATA=$oldLocal
    $stage="initial"
    [void](Wait-Player {param($p) [DesktopTest]::FindWindow("LitelyController",[IntPtr]::Zero) -ne [IntPtr]::Zero})
    $hwnd=[DesktopTest]::FindWindow("LitelyController",[IntPtr]::Zero)
    [void][DesktopTest]::PostMessage($hwnd,0x111,[IntPtr]2,[IntPtr]::Zero)
    $stage="visible"
    $active=Wait-Player {param($p) $p.WorkingSet64 -gt 55MB}
    "Visible player PID $($active.Id)"
    Start-Sleep -Milliseconds 700
    $poster=Get-ChildItem -LiteralPath $config -Filter "wallpaper-*.bmp" -File
    if(@($poster).Count -ne 1){throw "Expected one poster before coverage"}
    $startHash=(Get-FileHash -LiteralPath $poster.FullName).Hash
    $area=[Windows.Forms.Screen]::PrimaryScreen.WorkingArea
    $mid=[int]($area.Width / 2)
    foreach ($side in 0,1) {
        $f=New-Object Windows.Forms.Form
        $forms += $f
        $f.FormBorderStyle='None'; $f.StartPosition='Manual'; $f.ShowInTaskbar=$false
        $f.Text='Wallpaper visibility test'; $f.TopMost=$true
        $f.SetBounds(($area.Left+$mid*$side),$area.Top,($mid+($area.Width%2)),$area.Height)
        $f.Show()
    }
    foreach ($screen in [Windows.Forms.Screen]::AllScreens | Where-Object { !$_.Primary }) {
        $f=New-Object Windows.Forms.Form
        $forms += $f
        $f.FormBorderStyle='None'; $f.StartPosition='Manual'; $f.ShowInTaskbar=$false
        $f.Text='Wallpaper visibility test'; $f.TopMost=$true
        $f.Bounds=$screen.WorkingArea
        $f.Show()
    }
    $stage="covered"
    $idle=Wait-Player {param($p) $p.Id -eq $active.Id -and (Tray-Status) -like "*desktop covered"}
    Start-Sleep -Milliseconds 2800
    $idle.Refresh()
    $before=$idle.TotalProcessorTime
    Start-Sleep -Seconds 2
    $idle.Refresh()
    "Covered tray working set {0:N1} MiB, private commit {1:N1} MiB, CPU {2} ms over 2 s" -f ($idle.WorkingSet64/1MB),($idle.PrivateMemorySize64/1MB),($idle.TotalProcessorTime-$before).TotalMilliseconds
    $after=Get-ChildItem -LiteralPath $config -Filter "wallpaper-*.bmp" -File
    if(@($after).Count -ne 1 -or $after.FullName -ne $poster.FullName -or (Get-FileHash -LiteralPath $after.FullName).Hash -eq $startHash){throw "Cover did not refresh the same still image"}
    "Covered playback updated the existing poster in place"
    $forms[0].Close()
    $stage="reveal"
    $resumed=Wait-Player {param($p) $p.Id -eq $idle.Id -and (Tray-Status) -like "*playing"}
    "Revealing half of the desktop resumed playback in PID $($resumed.Id)"
    Start-Sleep -Milliseconds 700
    [void][DesktopTest]::PostMessage($hwnd,0x111,[IntPtr]2,[IntPtr]::Zero)
    $stage="pause"
    [void](Wait-Player {param($p) $p.Id -eq $active.Id -and (Tray-Status) -like "*paused"})
    if ([DesktopTest]::FindWindow("LitelyController",[IntPtr]::Zero) -ne $hwnd) { throw "Tray window changed" }
    [void](Wait-Player {param($p) (Get-Content -LiteralPath $ini) -contains "paused=1"})
    Start-Sleep -Milliseconds ([Windows.Forms.SystemInformation]::DoubleClickTime + 30)
    $observer=[DesktopTest]::ObserveResume($hwnd)
    $stage="manual resume"
    [void](Wait-Player {param($p) $p.Id -eq $active.Id -and (Tray-Status) -like "*playing"})
    $latency=$observer.GetAwaiter().GetResult()
    "Resume observation: max tray response {0:N1} ms, max compositor wait {1:N1} ms" -f $latency[0],$latency[1]
    "Manual pause and resume kept the same tray window and process"
    [void][DesktopTest]::PostMessage($hwnd,0x2B1,[IntPtr]7,[IntPtr]::Zero)
    $stage="session lock notification"
    [void](Wait-Player {param($p) $p.WorkingSet64 -lt 55MB})
    [void][DesktopTest]::PostMessage($hwnd,0x2B1,[IntPtr]8,[IntPtr]::Zero)
    $stage="session unlock notification"
    [void](Wait-Player {param($p) $p.WorkingSet64 -gt 55MB})
    "Lock notification released renderer memory; unlock restored playback"
    Start-Sleep -Milliseconds 600
    [void][DesktopTest]::PostMessage($hwnd,0x111,[IntPtr]2,[IntPtr]::Zero)
    $stage="warm pause before switching modes"
    [void](Wait-Player {param($p) (Get-Content $ini) -contains "paused=1" -and $p.WorkingSet64 -gt 55MB})
    [void][DesktopTest]::PostMessage($hwnd,0x111,[IntPtr]5,[IntPtr]::Zero)
    $stage="maximum savings while paused"
    [void](Wait-Player {param($p) (Get-Content $ini) -contains "maximumSavings=1" -and $p.WorkingSet64 -lt 55MB})
    [void][DesktopTest]::PostMessage($hwnd,0x10,[IntPtr]::Zero,[IntPtr]::Zero)
    if (!$active.WaitForExit(5000)) {throw "Test instance did not exit"}
    $env:LOCALAPPDATA=$testRoot
    $restarted=Start-Test
    $env:LOCALAPPDATA=$oldLocal
    $stage="reload paused profile"
    [void](Wait-Player {param($p) [DesktopTest]::FindWindow("LitelyController",[IntPtr]::Zero) -ne [IntPtr]::Zero})
    $hwnd=[DesktopTest]::FindWindow("LitelyController",[IntPtr]::Zero)
    [void][DesktopTest]::PostMessage($hwnd,0x111,[IntPtr]2,[IntPtr]::Zero)
    $stage="cold resume"
    [void](Wait-Player {param($p) $p.WorkingSet64 -gt 55MB})
    Start-Sleep -Milliseconds 600
    [void][DesktopTest]::PostMessage($hwnd,0x111,[IntPtr]2,[IntPtr]::Zero)
    $stage="saved maximum savings mode"
    [void](Wait-Player {param($p) (Get-Content $ini) -contains "paused=1" -and $p.WorkingSet64 -lt 55MB})
    [void][DesktopTest]::PostMessage($hwnd,0x111,[IntPtr]4,[IntPtr]::Zero)
    $stage="switch back to smooth resume"
    [void](Wait-Player {param($p) (Get-Content $ini) -contains "maximumSavings=0"})
    "Switching to Maximum savings released memory while paused; restart retained that behavior; Smooth resume was selectable again"
} finally {
    $env:LOCALAPPDATA=$oldLocal
    $env:LITELY_TEST_SKIP_PERSONALIZATION=$oldSkip
    foreach ($f in $forms) { $f.Dispose() }
    $hwnd=[DesktopTest]::FindWindow("LitelyController",[IntPtr]::Zero)
    if($hwnd -ne [IntPtr]::Zero) { [void][DesktopTest]::PostMessage($hwnd,0x10,[IntPtr]::Zero,[IntPtr]::Zero) }
    Start-Sleep -Milliseconds 500
    $shell.UndoMinimizeAll()
    Get-ChildItem -LiteralPath $config -Filter "wallpaper-*.bmp" -File | ForEach-Object { Remove-Item -LiteralPath $_.FullName }
    if(Test-Path -LiteralPath $ini){Remove-Item -LiteralPath $ini}
    if((Get-ChildItem -LiteralPath $config).Count -eq 0){[IO.Directory]::Delete($config)}
    if((Get-ChildItem -LiteralPath $testRoot).Count -eq 0){[IO.Directory]::Delete($testRoot)}
}




