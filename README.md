# Litely

Open `build\Litely.exe` and choose an MP4. The first file picker opens the prepared wallpapers folder. The app remembers the video and pause state under `%LOCALAPPDATA%\Litely`.

- Left- or right-click the tray icon to open its menu. Pause, resume, choose a video or exit from there.
- Right-click the tray icon and open **Performance** to choose **Smooth resume** (default) or **Maximum savings**. Maximum savings releases the renderer after manual pause or two seconds of complete desktop coverage. Smooth resume keeps it ready. Both modes keep the video frame rate and remember your choice across restarts.
- In **Add MP4**, **Playback: Lowest memory** converts the video once into a simpler H.264 form that Litely decodes itself, using about half the video memory. The copy is roughly three times larger and very slightly softer in fine grain; the original is kept.
- **Settings > 30 fps on battery** (off by default) makes a 30 fps copy of the active wallpaper once, in the background, and plays it while on battery. It needs Python with FFmpeg.
- Choosing a video also sets a still from it as the Windows wallpaper, even when the desktop is covered. Re-selecting the same file reuses its saved image. Windows displays that still while the player starts.
- Manual pause saves the video frame as a BMP, makes it the Windows Fill wallpaper on all monitors, and stops the playback clock. That still remains after exit.
- Litely keeps its stills out of Settings > Personalization > Background > Recent images.
- Playback decodes on the GPU and presents the decoder's own frames, falling back to the Media Engine when a PC or file can't do that. Settings shows which path is active.
- One muted player feeds a shared DirectComposition surface, mirrored and cropped to fill each monitor. An exposed part of any monitor keeps playback active. After two seconds of full coverage, the app overwrites the existing still with the last submitted frame and applies the selected performance mode. In Smooth resume mode, revealing the desktop restarts playback without rebuilding the graphics device or seeking. Locking the session or turning off the displays releases the renderer and its video buffers.
- The app recognizes ordinary opaque windows and their combined coverage. Translucent and irregular windows can leave the animation active. Desktop embedding uses Explorer's desktop host and can vary with Windows builds.

## Choosing a copy

The supplied 720p and 1080p copies retain Furina's 30 fps and Miyabi's 60 fps. Choose 720p for the smallest tested memory footprint or 1080p for sharper detail. Original MP4s also work when their codec is available in Windows.

The Library's **Add MP4** wizard can make a 1080p, 720p or 30 fps playback copy with live progress and an estimated memory saving. To prepare one by hand, run `python optimize.py "C:\path\file.mp4" --height 720`. The offline tool requires FFmpeg on PATH or the Python imageio-ffmpeg package. It writes a new file to `build\wallpapers`. Playback requires neither Python nor FFmpeg.

## Measured savings

Process CPU, working-set RAM and local GPU memory while playing at 1080p. Medians of three 20-second runs per player in one session on November 17, 2026: RTX 4060 laptop, laptop panel plus one external monitor. Percentages are against the original player.

| Clip | Player | CPU (one core) | RAM | GPU memory | File |
| --- | --- | --- | --- | --- | --- |
| Miyabi 60 fps | Original (Media Engine) | 15.1% | 110 MiB | 59.5 MiB | 7.0 MiB |
| Miyabi 60 fps | Lean player | 10.5% (−30%) | 79 MiB (−29%) | 48.5 MiB (−18%) | 7.0 MiB |
| Miyabi 60 fps | Lowest memory | 8.8% (−42%) | 64 MiB (−42%) | 21.6 MiB (−64%) | 21.3 MiB |
| Furina 30 fps | Original (Media Engine) | 8.2% | 85 MiB | 59.5 MiB | 4.4 MiB |
| Furina 30 fps | Lean player | 5.5% (−33%) | 72 MiB (−16%) | 48.5 MiB (−18%) | 4.4 MiB |
| Furina 30 fps | Lowest memory | 4.9% (−40%) | 62 MiB (−27%) | 21.6 MiB (−64%) | 13.4 MiB |

- The lean player plays the original file untouched. Lowest memory plays a converted copy: about three times larger, colours and edges unchanged, fine grain slightly smoothed (VMAF about 96.5 against the source for Miyabi).
- No late frames in any run. All three show the same picture for the same file.
- The original player's cost varies with the display setup: with only the laptop panel attached it measured 21.0% and 158 MiB on Miyabi, so the savings there are larger. The other two barely change.
- At 2160p the lean player used 173 MiB of GPU memory and Lowest memory 65 MiB.
- One PC, one GPU. Lowest memory is untested on Intel and AMD graphics; it falls back to the lean player if it cannot run.

## Build and test

Run `build.cmd` with the Visual Studio C++ desktop tools and the Windows SDK installed. The output is a standalone x64 Windows executable when built with the x64 toolchain.

`Test-Playback.ps1 "C:\path\file.mp4"` briefly renders the clip behind desktop icons, captures a frame to a temporary file, and checks paused frame identity, resume latency and loop restart. It reports process and GPU memory and frame submissions. It skips applying the captured image to Windows personalization.

`Test-Direct.ps1` converts a clip with `optimize.py --lowest-memory` and checks the direct decoder against the lean path on it (pixels, pacing, memory, fallback). `Test-Savings.ps1` compares the two playback paths and checks frame pacing and pixel accuracy; it briefly shows a video window. `Test-Stress.ps1` storms a test instance with pause and display-change messages. Both stop a running Litely.

`Test-Visibility.ps1 "C:\path\file.mp4"` is an interactive single-monitor test. It temporarily minimizes other windows, tiles two test windows, measures the idle tray, reveals half the desktop, then restores the windows.

