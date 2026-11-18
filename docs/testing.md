# Testing

`Test-Playback.ps1 "C:\path\file.mp4"` briefly renders the clip behind desktop icons, captures a frame to a temporary file, and checks paused frame identity, resume latency and loop restart. It reports process and GPU memory and frame submissions. It skips applying the captured image to Windows personalization.

`Test-Direct.ps1` converts a clip with `Litely.exe --convert` and checks the direct decoder against the lean path on it (pixels, pacing, memory, fallback). `Test-Savings.ps1` compares the two playback paths and checks frame pacing and pixel accuracy; it briefly shows a video window. `Test-Stress.ps1` storms a test instance with pause and display-change messages. Both stop a running Litely.

`Test-Visibility.ps1 "C:\path\file.mp4"` is an interactive single-monitor test. It temporarily minimizes other windows, tiles two test windows, measures the idle tray, reveals half the desktop, then restores the windows.
