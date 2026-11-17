# Litely

Open `build\Litely.exe` and choose an MP4. The first file picker opens the prepared wallpapers folder. The app remembers the video and pause state under `%LOCALAPPDATA%\Litely`.

- Left- or right-click the tray icon to open its menu. Pause, resume, choose a video or exit from there.
- Right-click the tray icon and open **Performance** to choose **Smooth resume** (default) or **Maximum savings**. Maximum savings releases the renderer after manual pause or two seconds of complete desktop coverage. Smooth resume keeps it ready. Both modes keep the video frame rate and remember your choice across restarts.
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

## Build and test

Run `build.cmd` with the Visual Studio C++ desktop tools and the Windows SDK installed. The output is a standalone x64 Windows executable when built with the x64 toolchain.

`Test-Playback.ps1 "C:\path\file.mp4"` briefly renders the clip behind desktop icons, captures a frame to a temporary file, and checks paused frame identity, resume latency and loop restart. It reports process and GPU memory and frame submissions. It skips applying the captured image to Windows personalization.

`Test-Savings.ps1` compares the two playback paths and checks frame pacing and pixel accuracy; it briefly shows a video window. `Test-Stress.ps1` storms a test instance with pause and display-change messages. Both stop a running Litely.

`Test-Visibility.ps1 "C:\path\file.mp4"` is an interactive single-monitor test. It temporarily minimizes other windows, tiles two test windows, measures the idle tray, reveals half the desktop, then restores the windows.

