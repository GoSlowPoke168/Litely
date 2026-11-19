<p align="center">
  <img src="assets/readme/banner.png" alt="Litely" width="720">
</p>

<p align="center"><b>Animated video wallpapers for Windows that stay light on your PC.</b></p>

<p align="center">
  <a href="../../releases/latest"><img src="https://img.shields.io/badge/Download-latest-778DFC?style=for-the-badge&logo=github&logoColor=white" alt="Download"></a>
  <img src="https://img.shields.io/badge/Windows-10%20%7C%2011-6CCCFC?style=for-the-badge" alt="Windows 10 | 11">
  <img src="https://img.shields.io/badge/Size-0.8%20MB-F9A47E?style=for-the-badge" alt="0.8 MB">
  <a href="LICENSE"><img src="https://img.shields.io/badge/License-GPL--3.0-A3E4A0?style=for-the-badge" alt="License: GPL-3.0"></a>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/RAM-88%25%20less-2EA043?style=for-the-badge" alt="88% less RAM">
  <img src="https://img.shields.io/badge/Video%20memory-87%25%20less-2EA043?style=for-the-badge" alt="87% less video memory">
  <img src="https://img.shields.io/badge/CPU-75%25%20less-2EA043?style=for-the-badge" alt="75% less CPU">
  <br>
  <sub>Than Lively Wallpaper, playing the same 1080p 60 fps video on two monitors. <a href="#litely-vs-lively-wallpaper">See the comparison</a></sub>
</p>

<!-- Demo video: edit this file on GitHub, drag an MP4 (under 10 MB) onto the next line, and delete the placeholder image. -->
<p align="center">
  <img src="assets/readme/placeholder-demo.png" alt="Demo video" width="820">
</p>

## Why Litely

<table>
  <tr>
    <td width="50%" valign="top">
      <h3>Light on resources</h3>
      Under 80 MiB of RAM while playing 1080p at 60 fps, about an eighth of what Lively Wallpaper uses. Video is decoded on the GPU and drawn straight to the desktop.
    </td>
    <td width="50%" valign="top">
      <h3>Gets out of your way</h3>
      Stops when a game or window covers the desktop, the PC locks or the displays turn off, and picks up again the moment the desktop shows. Can drop to 30 fps on battery.
    </td>
  </tr>
  <tr>
    <td valign="top">
      <h3>One small app</h3>
      A single 0.8 MB program with nothing else to install. It converts videos itself.
    </td>
    <td valign="top">
      <h3>Lowest memory mode</h3>
      Converts a video once so Litely can decode it itself, using under 25 MiB of video memory at 1080p.
    </td>
  </tr>
  <tr>
    <td valign="top">
      <h3>Almost any video, or a GIF</h3>
      MP4, MKV, MOV, WebM, AVI, WMV, MPEG and GIF files, in any format Windows can decode, including HEVC, VP9 and AV1.
    </td>
    <td valign="top">
      <h3>Every monitor</h3>
      One video fills each monitor, cropped to fit, and is decoded only once for all of them.
    </td>
  </tr>
  <tr>
    <td valign="top">
      <h3>A Library for your wallpapers</h3>
      Keep them in a list or a gallery and switch with one click. Live CPU, RAM and video memory readings show what the wallpaper costs. The accent colour follows your wallpaper.
    </td>
    <td valign="top">
      <h3>Pause on any frame</h3>
      Pausing makes the current frame your Windows wallpaper, so the desktop looks the same while Litely is paused or closed.
    </td>
  </tr>
</table>

## Screenshots

<p align="center">
  <img src="assets/readme/library.png" alt="Library" width="820">
  <br><sub>Library</sub>
</p>

<p align="center">
  <img src="assets/readme/gallery.png" alt="Gallery view" width="820">
  <br><sub>Gallery view</sub>
</p>

## Install

1. Download **Litely-Setup.exe** from [Releases](../../releases/latest) and run it. It installs for your account only, so no administrator prompt.
2. Keep **Start Litely with Windows** ticked so your wallpaper plays after every restart. The Litely icon appears in the system tray.

Prefer no installer? **Litely-portable.zip** on the same page runs from any folder.

> [!NOTE]
> Litely isn't code-signed yet, so Windows may say "Windows protected your PC". Select **More info**, then **Run anyway**.

**Requirements:** Windows 10 or 11 (64-bit) and a GPU that decodes H.264 video.

## Usage

1. Click the tray icon and choose **Open Litely**.
2. Select **Add video** and pick a video or GIF. Litely makes a Lowest memory copy by default; you can keep the original instead.
3. Click the video in the Library to put it on your desktop.
4. Right-click the tray icon to see what the wallpaper is using, pause on the current frame, change the performance mode or exit.

## Litely vs Lively Wallpaper

The same 1080p 60 fps video on the same laptop, as the wallpaper on both monitors. Lower is better.

| App | CPU (one core) | RAM | Video memory | Install size |
| --- | --- | --- | --- | --- |
| Lively Wallpaper | 42.2% | 640 MiB | 377 MiB | 396 MB |
| Litely | 10.4% (−75%) | 79 MiB (−88%) | 51 MiB (−87%) | 0.8 MB |
| Litely, Lowest memory | **10.5% (−75%)** | **65 MiB (−90%)** | **24 MiB (−94%)** | **0.8 MB** |

<details>
<summary><b>Method and notes</b></summary>

- Both apps measured from outside the same way: CPU time, working-set RAM and the GPU's dedicated memory, summed over all of each app's processes. Medians of three 20-second runs, alternated, November 17, 2026, on an RTX 4060 laptop.
- Lively used its default settings with hardware decoding on, except that its pause rules were set to keep playing while another window had focus.
- Lively runs one mpv player per monitor. On one monitor it measured 25.5% CPU, 469 MiB RAM and 220 MiB video memory.
- Litely decodes once for all monitors. No late frames in any Litely run.

</details>

## Performance

Litely against Windows' built-in video player on 1080p videos, RTX 4060 laptop. Lower is better.

| Video | Player | CPU (one core) | RAM | Video memory |
| --- | --- | --- | --- | --- |
| 60 fps | Windows' built-in player | 15.1% | 110 MiB | 59.5 MiB |
| 60 fps | Litely | 10.5% (−30%) | 79 MiB (−29%) | 48.5 MiB (−18%) |
| 60 fps | Litely, Lowest memory | **8.8% (−42%)** | **64 MiB (−42%)** | **21.6 MiB (−64%)** |
| 30 fps | Windows' built-in player | 8.2% | 85 MiB | 59.5 MiB |
| 30 fps | Litely | 5.5% (−33%) | 72 MiB (−16%) | 48.5 MiB (−18%) |
| 30 fps | Litely, Lowest memory | **4.9% (−40%)** | **62 MiB (−27%)** | **21.6 MiB (−64%)** |

On the same laptop's **Intel UHD Graphics**, Lowest memory saves even more:

| Video | Player | RAM | Video memory |
| --- | --- | --- | --- |
| 60 fps | Litely | 158 MiB | 89.8 MiB |
| 60 fps | Litely, Lowest memory | **76 MiB (−52%)** | **29.8 MiB (−67%)** |

At 2160p, Litely used 173 MiB of video memory and Lowest memory 65 MiB.

<details>
<summary><b>Method and notes</b></summary>

- Process CPU, working-set RAM and local GPU memory while playing. Medians of three 20-second runs per player, alternated in one session, November 17, 2026. Laptop panel plus one external monitor.
- No late frames in any run, on either GPU. All players show the same picture for the same file.
- Litely plays the original file untouched. Lowest memory plays a converted copy: about three times larger (24.3 MiB and 11.6 MiB for these 10-second videos), colours and edges unchanged, fine grain slightly smoothed (VMAF 96.9 against the source at 60 fps).
- The built-in player's cost varies with the display setup: with only the laptop panel it measured 21.0% and 158 MiB at 60 fps, so the savings there are larger.
- The Intel runs leave out CPU because both displays are wired to the NVIDIA GPU, so Windows copies each frame across outside Litely.
- AMD graphics are untested. If Lowest memory cannot run, Litely plays the file the normal way.

</details>

## Options

<details>
<summary><b>Performance modes</b></summary>

Right-click the tray icon and open **Performance**. **Smooth resume** (default) keeps the renderer ready so the video restarts instantly. **Maximum savings** releases it after a manual pause or two seconds of full coverage. Both keep the video's frame rate.

</details>

<details>
<summary><b>Playback copies and Lowest memory</b></summary>

**Add video** can make a 1080p, 720p or 30 fps playback copy, with live progress and an estimated memory saving. **Playback: Lowest memory** (the default) converts the video once into a simpler H.264 form that Litely decodes itself, using about half the video memory. The copy is roughly three times larger and very slightly softer in fine grain; the original is kept. Conversion uses your GPU's video encoder when it has one.

GIFs and MPEG streams (`.mpg`, `.ts`) always get a copy. If Windows has no decoder for a video, Litely says so and names the Microsoft Store extension to install (HEVC, AV1, VP9 or MPEG-2).

</details>

<details>
<summary><b>30 fps on battery</b></summary>

**Settings > 30 fps on battery** (off by default) makes a 30 fps copy of the active wallpaper once, in the background, and plays it while on battery. Litely uses about 30 MiB more RAM while it makes the copy.

</details>

<details>
<summary><b>Start with Windows</b></summary>

**Settings > Startup > Start with Windows** opens Litely quietly in the tray when you sign in. The installer sets it for you; the portable copy can turn it on here.

</details>

<details>
<summary><b>Accent colour</b></summary>

**Settings > Appearance** sets the accent colour: **Match wallpaper** (default) takes it from the wallpaper on your desktop, or choose one of eight colours.

</details>

<details>
<summary><b>Good to know</b></summary>

- Choosing a video also sets a still from it as the Windows wallpaper, which Windows shows while the player starts.
- Litely keeps its stills out of **Settings > Personalization > Background > Recent images**.
- The still is set on every virtual desktop, so switching desktops doesn't stall while Windows swaps wallpapers.
- The Library shows Litely's usage with its window open and an estimate without it. With the Library closed, the tray menu shows the wallpaper's own usage.
- Translucent and irregular windows can leave the animation running, because Litely only counts opaque windows as covering the desktop.

</details>

## Build from source

Litely is written in C++17 against Win32, Direct3D 11 and Media Foundation. Run `build.cmd` with the Visual Studio C++ desktop tools and the Windows SDK installed. It produces `build\Litely.exe`. `installer\build.cmd` also builds `Litely-Setup.exe` and `Litely-portable.zip` in `build\installer`, and needs [Inno Setup 6](https://jrsoftware.org/isinfo.php).

See [How Litely works](docs/how-it-works.md) and [Testing](docs/testing.md).

## License

Litely is free software under the [GNU General Public License v3](LICENSE).
