# Litely — handoff for Claude
Updated November 16, 2026. Project root: `C:\Users\jhou2\Documents\Projects and Programs\Litely`.

## Start here

Litely is a native Windows tray app that mirrors one animated MP4 across monitors. The user prioritizes low process RAM and GPU memory, smooth resume, and a desktop that never flashes the old wallpaper. The approved visual and interaction reference is `docs/ui-preview/index.html`. Import defaults are **original resolution and original frame rate**, explicitly chosen by the user.

State on November 16, 2026: `main` holds the UI polish, the lean playback path with Media Engine fallback, and the optional 30 fps battery mode (all merged, nothing pushed; there is no remote). Designs are in `docs/superpowers/specs/`, plans in `docs/superpowers/plans/`. Builds are clean at /W4.

**Stage 2 is built on branch `direct-decoder` (not merged):** `direct_player.h` drives the GPU's H.264 decoder directly for MP4s in a simple form (progressive 4:2:0, one reference frame, no reordering, one slice per picture, constant frame rate). It parses the MP4 index and the SPS/PPS itself, reads one sample at a time, decodes on the UI thread one frame ahead into two surfaces, and loads no Media Foundation. It sits behind the lean path's presentation code: `leanStart` tries `directOpen` first, and `leanAcquire`, `leanSchedule` and `leanStill` branch on `directActive`. A file outside the simple form plays on the lean path as before; a decode failure posts `DirectFailed`, which reopens the file on the lean path. `LITELY_PIPELINE=lean` skips it; `LITELY_TEST_DIRECT_FAIL=N` fails the Nth decode.

The Add MP4 window's **Playback: Lowest memory** choice runs `optimize.py --lowest-memory` (libx264 `-preset slow -crf 14 -refs 1 -bf 0 -weightp 0`, `slices=1:keyint=60`) and names the copy `…-lowmem.mp4`. Measured by `Test-Direct.ps1` on Miyabi 1080p60 (12 s, two monitors): direct 6.4% of a core, 63 MiB RAM, 21.6 MiB GPU; lean on the same file 10.9%, 93 MiB, 39.6 MiB; first-frame pixels identical. The copy was 21.3 MiB against a 7.0 MiB source, VMAF about 96.5: colour and edges are unchanged, fine grain is slightly smoothed. Estimate model for the wizard: GPU ≈ 8.6 + 6.8 MiB/Mpx (within 5% from 720p to 2160p), RAM ≈ 56 + 7.7 MiB/Mpx (within 11%).

An independent review found no memory-safety holes in the parsers; its Critical and Important findings are fixed (freeze right after a direct-to-lean restart, short-term reference flag, non-reference pictures, bounded key-frame gap of 120 frames, `DecoderBeginFrame` retry, profiles limited to 66/77/88/100, bounded box scan, the battery copy of a lowest-memory wallpaper is now lowest-memory too, stale `leanShown`). Deferred from that review: PPS/SPS ids and in-band parameter sets are not compared with avcC, `dec_ref_pic_marking` is not parsed (long-term references are not rejected), a slice larger than the driver's bitstream buffer falls back for good, `-fps_mode cfr` on a variable-frame-rate source, and the import window is 550 dip tall (too tall for a 1080p display at 200%). The README's "Measured savings" table has the three-player comparison.

Known gaps: only tested on the RTX 4060; existing library entries get the option only by re-adding them; a saved position restores by decoding forward from the previous key frame (up to 59 frames) on the UI thread. The prototype and its measurements are in `spike\` and `docs/superpowers/plans/2026-11-16-stage2-direct-decoder.md`.

The user tests with 1080p and higher clips and does not use the 720p copies. They want progress reported in plain terms, and they pay per usage: keep test runs short and do not re-run the 9-minute `Test-Savings.ps1` more than needed.

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

## Deferred review notes

From the independent review of the lean player (none is a crash or a visible defect in normal use):
- `leanHeld` is not enforced in `draw()`; after an Energy Saver freeze a stale timer tick can present one more frame.
- `leanDropStill` flushes the D3D context after every frame's still check; guard it to run only when a still was made.
- Cancelling a pending pause while the desktop is covered leaves the player open with no unload timer.
- `batteryCopyFailed` is session-wide, so one failed clip disables copies for all until the box is re-ticked.
- A deleted 30 fps copy is remade only on the next power, activation or setting event.
- The battery job and the import wizard share a scratch directory; cancelling one removes the other's scratch.
- `updateBatteryCopy` reads the shell property store on the UI thread.
- A saved position past the end may force the fallback instead of restarting from zero.
- The UI thread joins MMCSS for both pipelines, including Library painting.
- `playingFile` is empty before the first open, so the Library can briefly read "30 fps on battery".
- The decode thread catches only `HRESULT`.
- `leanJoin` waits on the UI thread for a slow file open.
- A swap while paused keeps the old decoder's surface pool until the next frame.
- Video size comes from `MF_MT_FRAME_SIZE`, not the display aperture.
- The swap test log reports which branch ran, not that the windows survived.
- Four review fixes have no test: colour space on a kept swap, a decoder that never outputs, guarded copy completion, fallback pinning only on start failure.

## Next project

Stage 2 of `docs/superpowers/specs/2026-11-16-lean-player-design.md`: prototype a decoder that drives the GPU's H.264 decode API directly and measure it against the lean path (estimate to confirm or refute: about 6% of a core, 40 MiB RAM, 23 MiB GPU at 1080p60). The Media Foundation decoder keeps reference frames + 7 surfaces and ignores pool hints, which is where most remaining GPU memory goes. If it holds up, add a first-run calibration that times the available paths.

## Regression details and evidence

- Keep `WS_EX_COMPOSITED` off the native list box. Owner-drawn rows, buttons and choices paint into a temporary bitmap and blit once.
- Library `WM_SIZE` moves controls in one `DeferWindowPos` batch with `SWP_NOCOPYBITS`. Separate copying moves left Pause pixels on Remove during fast resizes.
- Hovering the hero title shows a translucent outline with a pencil (`titleBox`, set during paint). Clicking inside it opens Rename.
- Shapes use `fillRound`/`strokeRound`/`roundCorners`, which draw in device pixels from 96-dpi units. Dialogs paint under `DpiScope` with their own DPI.
- Explorer writes `BackgroundHistoryPathN` after `SetWallpaper` returns. The main loop therefore waits on a registry-change event and drops paths inside the data folder.
- The import estimate model (GPU ≈ 8 + 19.8 MiB/Mpx, RAM ≈ 62 + 9.5 MiB/Mpx) was fitted to 20 s lean-path `Test-Playback.ps1` runs at 720p–2160p on one PC. GPU fits within 3%; RAM is not linear in pixels and only holds within 15%. Frame rate changed CPU only.
- `Test-Savings.ps1` (medians of five 20 s runs, lean vs Media Engine, November 16): Miyabi 1080p60 CPU 13.4% vs 21.0% of a core, RAM 81.8 vs 158.4 MiB, GPU 48.5 vs 59.5 MiB; Furina 1080p30 CPU 5.9% vs 10.2%, RAM 74.6 vs 126.7 MiB, GPU 48.5 vs 59.5 MiB. Zero late frames; stills and on-screen pixels identical. Those figures were taken with only the laptop panel attached. With an external monitor also attached the same evening, the Media Engine path measured much cheaper (Miyabi CPU 13.0%, RAM 121 MiB; Furina CPU 7.3%, RAM 90 MiB) while the lean path was unchanged (11.1% and 84 MiB; 7.1% and 77 MiB), so `Test-Savings.ps1` failed its CPU and RAM limits in that configuration. GPU memory (0.82×) and pixel checks held in both. The cause of the Media Engine's change is not understood.
- Lean-path lessons: the decode and UI threads run in the MMCSS Playback class, because a background process otherwise stalls 17–45 ms. `IDXGIDevice3::Trim` doubled playback CPU and must stay out. A delay of one refresh is `Present` waiting on the display, so only delays over 1.5 frames count as late. Working set settles about 20 MiB lower after 20 s than at 4 s.
- Probe variables: `LITELY_PROBE_SECONDS` (sample length), `LITELY_PROBE_TOPMOST` (1:1 topmost window plus a screen grab). Test hooks: `LITELY_TEST_VISIBLE` (treat the desktop as uncovered), controller `WM_COMMAND` 2 (pause/resume). `Test-Stress.ps1` uses a temporary `LOCALAPPDATA`; pass it with `ProcessStartInfo`, since `Start-Process` does not forward a changed environment reliably.
- Conversion runs in a kill-on-close job object. `optimize.py --progress` forwards to FFmpeg `-progress`. Cancel removes scratch folders.
- A popup reading "could not complete that action" now includes its HRESULT. The suspected cause, a failed BMP replace, now retries for up to 500 ms.

## Battery mode

**30 fps on battery** (`batteryFps` in `settings.ini`, off by default) makes `optimized\<key>\<mtime>\<name>-original-30fps.mp4` with `optimize.py --height 0 --cap-fps` in a below-normal, kill-on-close job. The main loop waits on the job's process handle. `playFile()` returns the copy when the setting is on, the PC is on battery and the copy exists; `updatePlayFile()` restarts only the decode thread at the current position, so the swap chain and last frame stay. On the fallback path it reopens the player. A failed job shows "Couldn't make the 30 fps copy. Needs Python with FFmpeg." for the session.

Test hooks: `LITELY_TEST_POWER=ac|battery` enables simulated power and controller message `WM_APP+5` (wParam 0 AC, 1 battery); `LITELY_TEST_LOG` appends one `swap <file> at <position> first <pts> kept|reopened` line per swap. `Test-BatteryCopy.ps1` and `Test-BatterySwitch.ps1` cover creation, reuse, cancellation, switching and a deleted copy.

## Next verification

1. Import, conversion, Cancel, rename and remove were verified on November 16 by filling the picker's filename box. Check them once by hand with a real video, and watch for any reappearance of the error popup and its code.
2. Rename a disposable wallpaper, restart the tray process, verify the name, then remove it and confirm the source MP4 remains. Check mouse hover and keyboard focus on the live dialogs.
3. For playback changes, run `.\Test-Playback.ps1 .\build\wallpapers\hoshimi-miyabi-1080p.mp4` with `-Pipeline lean` and `-Pipeline legacy`, then `Test-Savings.ps1` and `Test-Stress.ps1`. `Test-Visibility.ps1` now covers every monitor and pauses with controller `WM_COMMAND` 2; it passed on November 16 with two monitors on the lean path. The visibility test temporarily rearranges windows. Not verified: a real unplug (power changes were simulated), the Energy Saver rule taking precedence over 30 fps mode, 100% display scaling for the Settings readout, a hybrid-GPU configuration where the Intel adapter drives the panel, mixed-DPI monitors, three-monitor playback, physical Energy Saver transitions, and the exact trigger of a previously reported 0x80004005 playback error remain unverified.
