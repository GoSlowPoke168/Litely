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
  <img src="https://img.shields.io/badge/RAM-86%25%20less-2EA043?style=for-the-badge" alt="86% less RAM">
  <img src="https://img.shields.io/badge/Video%20memory-86%25%20less-2EA043?style=for-the-badge" alt="86% less video memory">
  <img src="https://img.shields.io/badge/CPU-75%25%20less-2EA043?style=for-the-badge" alt="75% less CPU">
  <br>
  <sub>Than Lively Wallpaper, playing the same 1080p 60 fps video on two 1080p monitors. <a href="#litely-vs-lively-wallpaper">See the comparison</a></sub>
</p>

<!-- Demo video: edit this file on GitHub, drag an MP4 (under 10 MB) onto the next line, and delete the placeholder image. -->
<p align="center">
  <img src="assets/readme/placeholder-demo.png" alt="Demo video" width="820">
</p>

Pick a video or GIF and Litely plays it behind your desktop icons, on every monitor. Your GPU does the decoding, playback stops whenever a window or game covers the desktop, and the whole app is one 0.8 MB program.

## Litely vs Lively Wallpaper

Blossom Island, a 1080p 60 fps sample that comes with Litely, as the wallpaper on two 1080p monitors. Shorter bars are better.

<p align="center">
  <img src="assets/readme/comparison.svg" alt="Litely uses 75% less CPU, 86% less RAM and 86% less video memory than Lively Wallpaper" width="820">
</p>

Litely decodes the video once for all monitors and draws it straight to the desktop. Lively runs a separate player for each monitor.

<details>
<summary><b>Exact figures for four videos</b></summary>

| Video | App | CPU (one core) | RAM | Video memory |
| --- | --- | --- | --- | --- |
| Blossom Island (sample) · 1080p 60 fps | Lively Wallpaper | 21.8% | 572 MiB | 376 MiB |
|  | Litely | 5.4% ![−75%](https://img.shields.io/badge/%E2%88%9275%25-2EA043) | 78 MiB ![−86%](https://img.shields.io/badge/%E2%88%9286%25-2EA043) | 51 MiB ![−86%](https://img.shields.io/badge/%E2%88%9286%25-2EA043) |
|  | Litely, Lowest memory | 6.7% ![−69%](https://img.shields.io/badge/%E2%88%9269%25-2EA043) | 64 MiB ![−89%](https://img.shields.io/badge/%E2%88%9289%25-2EA043) | 24 MiB ![−94%](https://img.shields.io/badge/%E2%88%9294%25-2EA043) |
| Anime wallpaper · 1080p 60 fps | Lively Wallpaper | 21.1% | 608 MiB | 376 MiB |
|  | Litely | 7.3% ![−65%](https://img.shields.io/badge/%E2%88%9265%25-2EA043) | 78 MiB ![−87%](https://img.shields.io/badge/%E2%88%9287%25-2EA043) | 51 MiB ![−86%](https://img.shields.io/badge/%E2%88%9286%25-2EA043) |
|  | Litely, Lowest memory | 4.4% ![−79%](https://img.shields.io/badge/%E2%88%9279%25-2EA043) | 63 MiB ![−90%](https://img.shields.io/badge/%E2%88%9290%25-2EA043) | 24 MiB ![−94%](https://img.shields.io/badge/%E2%88%9294%25-2EA043) |
| Anime wallpaper · 1440p 60 fps | Lively Wallpaper | 23.1% | 659 MiB | 588 MiB |
|  | Litely | 6.0% ![−74%](https://img.shields.io/badge/%E2%88%9274%25-2EA043) | 92 MiB ![−86%](https://img.shields.io/badge/%E2%88%9286%25-2EA043) | 133 MiB ![−77%](https://img.shields.io/badge/%E2%88%9277%25-2EA043) |
|  | Litely, Lowest memory | 5.0% ![−78%](https://img.shields.io/badge/%E2%88%9278%25-2EA043) | 80 MiB ![−88%](https://img.shields.io/badge/%E2%88%9288%25-2EA043) | 35 MiB ![−94%](https://img.shields.io/badge/%E2%88%9294%25-2EA043) |
| Rainy Studio (sample) · 1080p 30 fps | Lively Wallpaper | 15.8% | 617 MiB | 376 MiB |
|  | Litely | 4.1% ![−74%](https://img.shields.io/badge/%E2%88%9274%25-2EA043) | 76 MiB ![−88%](https://img.shields.io/badge/%E2%88%9288%25-2EA043) | 51 MiB ![−86%](https://img.shields.io/badge/%E2%88%9286%25-2EA043) |
|  | Litely, Lowest memory | 3.0% ![−81%](https://img.shields.io/badge/%E2%88%9281%25-2EA043) | 63 MiB ![−90%](https://img.shields.io/badge/%E2%88%9290%25-2EA043) | 24 MiB ![−94%](https://img.shields.io/badge/%E2%88%9294%25-2EA043) |

Install size: Lively 396 MB, Litely 0.8 MB. [How this was measured](#how-this-was-measured)

</details>

## Features

<table>
  <tr>
    <td width="50%" valign="middle">
      <h3>Out of your way</h3>
      <p>Stops when a window or game covers the desktop or the PC locks, then resumes right away.</p>
    </td>
    <td width="50%" valign="middle">
      <h3>Every monitor, one decoder</h3>
      <p>The video fills each monitor, cropped to fit, and is decoded only once for all of them.</p>
    </td>
  </tr>
  <tr>
    <td valign="middle">
      <h3>Lowest memory mode</h3>
      <p>Converts a video once into a form Litely decodes itself, for under 25 MiB of video memory at 1080p.</p>
    </td>
    <td valign="middle">
      <h3>Almost any video, or a GIF</h3>
      <p>MP4, MKV, MOV, WebM, AVI, WMV, MPEG and GIF, including HEVC, VP9 and AV1 video.</p>
    </td>
  </tr>
  <tr>
    <td valign="middle">
      <h3>Pause on any frame</h3>
      <p>The paused frame becomes your Windows wallpaper, so the desktop looks the same while Litely is off.</p>
    </td>
    <td valign="middle">
      <h3>Kind to your battery</h3>
      <p>Optionally plays a 30 fps copy on battery, or pauses while Windows Energy Saver is on.</p>
    </td>
  </tr>
</table>

## Screenshots

<p align="center">
  <img src="assets/readme/library.png" alt="Library" width="820">
  <br><sub>The Library, with live CPU, RAM and video memory for the wallpaper</sub>
</p>

<p align="center">
  <img src="assets/readme/gallery.png" alt="Gallery view" width="820">
  <br><sub>Gallery view</sub>
</p>

## Install

1. Download **Litely-Setup.exe** from [Releases](../../releases/latest) and run it. It installs for your account only, so there's no administrator prompt.
2. Litely opens its Library. Pick a sample and select **Set wallpaper**, or select **Add video** to use your own.

From then on Litely starts with Windows and sits in the system tray. Prefer no installer? **Litely-portable.zip** runs from any folder, and **Litely-samples.zip** has the sample videos with their sources.

> [!NOTE]
> Litely isn't code-signed yet, so Windows may say "Windows protected your PC". Select **More info**, then **Run anyway**.

**Requirements:** Windows 10 or 11 (64-bit) and a GPU that decodes H.264 video.

## Good to know

- **Lowest memory** is chosen when you add a video. The converted copy is up to about three times larger than the original and very slightly softer in fine grain. Your original file is kept.
- **Smooth resume** (default) keeps the player ready so the video restarts instantly. **Maximum savings** frees it whenever the wallpaper is paused or covered. Switch in **Settings** or the tray menu.
- **"Without this window"** under the Library's usage readings estimates what the wallpaper uses once the Library is closed. Litely's RAM figures match Task Manager's Memory column.
- **Only opaque windows count as covering the desktop.** Translucent windows leave the animation running.
- **Your Windows wallpaper** is set to a still from the video on every virtual desktop, so switching desktops is instant. These stills stay out of Windows' Recent images.
- **Your Library, settings and converted copies** are kept in `%LOCALAPPDATA%\Litely`. Uninstalling asks whether to delete them.

## When the wallpaper is paused

Lively keeps its players loaded while paused. Litely's **Maximum savings** releases its player and video memory entirely; **Smooth resume** keeps a little ready so the video restarts instantly. Litely uses the same when a window covers the desktop.

| Paused | CPU (one core) | RAM | Video memory |
| --- | --- | --- | --- |
| Lively Wallpaper | 1.9% | 553 MiB | 412 MiB |
| Litely, Smooth resume | 0.0% | 81 MiB ![−85%](https://img.shields.io/badge/%E2%88%9285%25-2EA043) | 59 MiB ![−86%](https://img.shields.io/badge/%E2%88%9286%25-2EA043) |
| Litely, Maximum savings | 0.0% | 33 MiB ![−94%](https://img.shields.io/badge/%E2%88%9294%25-2EA043) | 0 MiB ![−100%](https://img.shields.io/badge/%E2%88%92100%25-2EA043) |
| Litely, Lowest memory, Maximum savings | 0.0% | 25 MiB ![−95%](https://img.shields.io/badge/%E2%88%9295%25-2EA043) | 0 MiB ![−100%](https://img.shields.io/badge/%E2%88%92100%25-2EA043) |

## How this was measured

- November 20, 2026, on a laptop with Intel UHD Graphics driving its panel and an RTX 4060 driving an external monitor, both set to 1920 × 1080.
- Every app was measured from outside the same way: CPU time, working-set RAM and video memory, summed over all of each app's processes. Video memory is the GPU's dedicated memory, or its shared memory on a GPU where the app has none. Playing figures average two 20-second runs; paused figures are one run.
- Lively Wallpaper 2.2.1 with default settings and hardware decoding, in its Duplicate layout, which runs one mpv player per monitor. While playing it was set to keep playing when another window had focus; it was paused with its own pause command.
- Every app played the same original files. Lowest memory played the copy Litely made when the video was added, 1.7 to 3.3 times larger than the original.
- Litely played every run without a late frame; Lowest memory had one late frame in two of eight 30-second runs.
- RAM here is the working set, which also counts shared Windows code, so it reads higher than Litely's own figures and Task Manager. Install size leaves out the optional sample videos.
- AMD graphics are untested. If Lowest memory can't run, Litely plays the file the normal way.

## Build from source

Litely is written in C++17 against Win32, Direct3D 11 and Media Foundation.

- **App:** run `build.cmd` with the Visual Studio C++ desktop tools and the Windows SDK. It produces `build\Litely.exe`.
- **Installer and portable zip:** run `installer\build.cmd`, which also needs [Inno Setup 6](https://jrsoftware.org/isinfo.php). Sample wallpapers are bundled from `build\samples` when that folder has MP4s.

See [How Litely works](docs/how-it-works.md) and [Testing](docs/testing.md).

## License

Litely is free software under the [GNU General Public License v3](LICENSE).

Copyright © 2026 GoSlowPoke168. The bundled sample videos are from Pixabay and remain under the [Pixabay Content License](https://pixabay.com/service/license-summary/).
