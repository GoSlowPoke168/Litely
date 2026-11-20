# Testing

`tests\Test-Playback.ps1 "C:\path\file.mp4"` briefly renders the clip behind desktop icons, captures a frame to a temporary file, and checks paused frame identity, resume latency and loop restart. It reports process and GPU memory and frame submissions. It skips applying the captured image to Windows personalization.

`tests\Test-Direct.ps1` converts a clip with `Litely.exe --convert` and checks the direct decoder against the lean path on it (pixels, pacing, memory, fallback). `tests\Test-Savings.ps1` compares the two playback paths and checks frame pacing and pixel accuracy; it briefly shows a video window. `tests\Test-Stress.ps1` storms a test instance with pause and display-change messages. Both stop a running Litely.

`tests\Test-Visibility.ps1 "C:\path\file.mp4"` is an interactive single-monitor test. It temporarily minimizes other windows, tiles two test windows, measures the idle tray, reveals half the desktop, then restores the windows.

The tests expect the clips `hoshimi-miyabi-1080p.mp4` and `furina-in-the-void4k-1080p.mp4` in `build\wallpapers`. They are not in the repository, so pass your own clip where a script takes one.
