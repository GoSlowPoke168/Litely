# Litely — handoff for Claude
Updated November 16, 2026. Project root: `C:\Users\jhou2\Documents\Projects and Programs\Litely`.

## Start here

Litely is a native Windows tray app that mirrors one animated MP4 across monitors. The user prioritizes low process RAM and GPU memory, smooth resume, and a desktop that never flashes the old wallpaper. The approved visual and interaction reference is `docs/ui-preview/index.html`. Import defaults are **original resolution and original frame rate**, explicitly chosen by the user.

On November 16, 2026, Claude polished the UI per `docs/superpowers/specs/2026-11-16-ui-polish-design.md`. The work covered anti-aliased GDI+ controls, Segoe UI Variable and Fluent icons, a dark scrollbar, a redesigned import window with estimates, real progress and Cancel, and themed dialogs. The footer was removed. Litely also scrubs its stills from Windows Recent images. Builds are clean at /W4, and the playback probe passes.

## Build, launch, and navigate

From PowerShell in the project root, with the Visual Studio C++ desktop tools and Windows SDK installed:

```powershell
.\build.cmd
.\build\Litely.exe
```

Litely runs as a single instance. Quit it from the tray before rebuilding if the linker reports that the EXE is locked. Closing the Library window leaves the tray app running. Left- or right-click the tray icon to open its menu, which has **Open Litely**, pause/resume and **Settings**.

| File | Responsibility |
| --- | --- |
| `lean_player.h` | Lean playback path: decode thread, frame queue, NV12 presentation, still conversion. Included by main.cpp |
| `main.cpp` | Tray/controller, Media Engine fallback playback, GPU rendering, desktop visibility, frame capture, pause/resume, playback probe |
| `library_ui.h` | Library and Settings, import wizard, preview extraction, rename/remove, usage sampling, DPI-aware painting. Included by main.cpp |
| `litely.rc`, `resource.h`, `app.manifest` | Native dialogs and icon resources, common-controls v6 for themed controls |
| `optimize.py` | Optional offline conversion. Requires Python and either FFmpeg on PATH or imageio-ffmpeg |
| `docs/ui-preview/index.html` | Approved design reference. Its sample resource readings are illustrative |
| `Test-Playback.ps1`, `Test-Visibility.ps1` | Playback probe and interactive single-monitor coverage test |

## Playback and savings

Playback has two paths. The lean path (`lean_player.h`) feeds a hardware decoder directly on a worker thread, queues three frames, and copies each NV12 frame into an NV12 composition swap chain from the UI thread; looping re-seeks only the demuxer, so the decoder is never flushed. If it cannot start, Litely reopens the file with the original Media Engine path. `LITELY_PIPELINE=legacy` forces that fallback, and Settings names the active path. Either way one muted decoder feeds a shared GPU surface through DirectComposition. An exposed area on any monitor keeps the animation running. Full coverage stops the clock. After about two seconds, Litely can apply a still and park or release the renderer. **Smooth resume** retains the renderer for fast restart. **Maximum savings** releases it. Manual pause saves the last submitted frame as the Windows background, reusing a stable per-video BMP path. The optional battery override applies on battery power with Windows Energy Saver active and temporarily uses maximum savings.

The Library shows process CPU, working-set RAM, and local GPU allocation. Its savings comparison uses recent playing samples for the active source while the Library is open. Negative deltas mean lower consumption. A previous Miyabi 720p probe measured 241 frames and 1.08 CPU-seconds over four seconds of playback, then zero measured CPU-seconds during its pause sample. Smooth resume retained similar memory. These are single-machine observations. The wizard’s decoded-frame size and pixel-rate estimates describe the video workload, not total memory or GPU utilization.

## Library and persistence

**Add MP4** opens the file picker and wizard. Original imports reference the source file. Resolution and frame rate are independent choices. Up to 1080p, up to 720p, or a 30 fps cap can create a cached H.264 playback copy. The optimizer pads odd dimensions when encoding at original size. A marquee indicates conversion activity. Imported videos can show a still before activation through Windows thumbnail extraction with a Media Foundation frame fallback. Click the large wallpaper title or double-click a library row to rename. Remove drops the library entry and keeps the video file.

User data lives in `%LOCALAPPDATA%\Litely`. `settings.ini` stores active playback state and preferences, `library.txt` is a UTF-16 path list, and `library-names.ini` stores display names by per-path key. Desktop stills use `wallpaper-*.bmp`, library stills use `*.preview.bmp`, and converted copies live in `optimized\`. Use disposable files for import/remove tests.

## Regression details and evidence

- Keep `WS_EX_COMPOSITED` off the native list box. Owner-drawn rows, buttons and choices paint into a temporary bitmap and blit once.
- Library `WM_SIZE` moves controls in one `DeferWindowPos` batch with `SWP_NOCOPYBITS`. Separate copying moves left Pause pixels on Remove during fast resizes.
- Hovering the hero title shows a translucent outline with a pencil (`titleBox`, set during paint). Clicking inside it opens Rename.
- Shapes use `fillRound`/`strokeRound`/`roundCorners`, which draw in device pixels from 96-dpi units. Dialogs paint under `DpiScope` with their own DPI.
- Explorer writes `BackgroundHistoryPathN` after `SetWallpaper` returns. The main loop therefore waits on a registry-change event and drops paths inside the data folder.
- The import estimate model (GPU ≈ 8 + 19.8 MiB/Mpx, RAM ≈ 62 + 9.5 MiB/Mpx) was fitted to 20 s lean-path `Test-Playback.ps1` runs at 720p–2160p on one PC. GPU fits within 3%; RAM is not linear in pixels and only holds within 15%. Frame rate changed CPU only.
- `Test-Savings.ps1` (medians of five 20 s runs, lean vs Media Engine, November 16): Miyabi 1080p60 CPU 13.4% vs 21.0% of a core, RAM 81.8 vs 158.4 MiB, GPU 48.5 vs 59.5 MiB; Furina 1080p30 CPU 5.9% vs 10.2%, RAM 74.6 vs 126.7 MiB, GPU 48.5 vs 59.5 MiB. Zero late frames; stills and on-screen pixels identical.
- Lean-path lessons: the decode and UI threads run in the MMCSS Playback class, because a background process otherwise stalls 17–45 ms. `IDXGIDevice3::Trim` doubled playback CPU and must stay out. A delay of one refresh is `Present` waiting on the display, so only delays over 1.5 frames count as late. Working set settles about 20 MiB lower after 20 s than at 4 s.
- Probe variables: `LITELY_PROBE_SECONDS` (sample length), `LITELY_PROBE_TOPMOST` (1:1 topmost window plus a screen grab). Test hooks: `LITELY_TEST_VISIBLE` (treat the desktop as uncovered), controller `WM_COMMAND` 2 (pause/resume). `Test-Stress.ps1` uses a temporary `LOCALAPPDATA`; pass it with `ProcessStartInfo`, since `Start-Process` does not forward a changed environment reliably.
- Conversion runs in a kill-on-close job object. `optimize.py --progress` forwards to FFmpeg `-progress`. Cancel removes scratch folders.
- A popup reading "could not complete that action" now includes its HRESULT. The suspected cause, a failed BMP replace, now retries for up to 500 ms.

## Next verification

1. Import, conversion, Cancel, rename and remove were verified on November 16 by filling the picker's filename box. Check them once by hand with a real video, and watch for any reappearance of the error popup and its code.
2. Rename a disposable wallpaper, restart the tray process, verify the name, then remove it and confirm the source MP4 remains. Check mouse hover and keyboard focus on the live dialogs.
3. For playback changes, run `.\Test-Playback.ps1 .\build\wallpapers\hoshimi-miyabi-720p.mp4`. The visibility test temporarily rearranges windows. Mixed-DPI monitors, three-monitor playback, physical Energy Saver transitions, and the exact trigger of a previously reported 0x80004005 playback error remain unverified.
