<p align="center">
  <img src="assets/vlc-logo.png" alt="VLC" width="140">
</p>

<h1 align="center">VLC</h1>

<p align="center">
  <b>VLC media player, running natively on PlayStation 5.</b><br>
  A port of <a href="https://www.videolan.org/vlc/">VLC</a> 3.0 as a native PS5 app, with a controller-first interface.
</p>

<p align="center">
  <img alt="Version" src="https://img.shields.io/badge/version-0.3-f37a2b">
  <img alt="Platform" src="https://img.shields.io/badge/platform-PS5-1f1f1f">
  <img alt="VLC" src="https://img.shields.io/badge/libvlc-3.0.24-ff8800">
  <img alt="Renderer" src="https://img.shields.io/badge/renderer-Vulkan%20(RADV)-c0561b">
  <img alt="License" src="https://img.shields.io/badge/license-GPL--3.0-blue">
</p>

---

## Features

- Plays nearly anything VLC plays: H.264, HEVC (8 and 10-bit), AV1, VP9, VC-1, MPEG-2 and more, up to 4K, with every common audio format, including TrueHD and DTS-HD.
- **Library** with Home, Videos, Music and Browse: thumbnails, Continue watching, Favourites and search.
- **Playlists**: make your own, add any video or song from anywhere with Options, reorder them; covers made from their videos; `.m3u` files on your drives play too.
- **Subtitles** in every script, styled ASS/SSA, size and colour, delay, plus **OpenSubtitles** downloads.
- **5.1 / 7.1 surround**, equalizer, night mode and audio delay.
- **Network**: SMB shares, DLNA servers, network links (HTTP, HLS, DASH…), free TV channels and internet radio.
- **PC/Phone page**: open `http://<ps5-ip>:8080` to send files to the PS5, use your pc or phone as a remote.
- Music mode with covers and a visualizer, a photo viewer, DVD / Blu-ray folders, and ZIP / RAR / 7z archives.
- Two looks (**Classic** and **Modern**) and **16 languages**.

## Install

You need a PS5 with **kstuff** running, plus **[ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus)**.

Download the [latest release](../../releases/latest), then:

1. Copy the **`PPSA85300`** folder to `/data/homebrew/`.
2. Start **VLC** from the home screen.
3. Add your media: a USB drive, `/data/vlc/`, or send files from your pc/phone (Options › Send files from a phone).

Optional full access. With etaHEN, OnionHEN or the Helper, VLC can lift its sandbox. This gives it all of /data and USB drives plugged in while it runs. download [helper.elf](https://github.com/ZiZc3/VLC-PS5/raw/refs/heads/main/patches/Helper.elf)

## Controls

**Library**

| Button | Action |
|---|---|
| D-Pad | Move |
| ✕ | Play / open |
| ○ | Back |
| △ | Favourite |
| □ | Details / delete |
| L1 / R1 | Switch tabs |
| L2 / R2 | Jump A–Z |
| R3 | Search |
| Options | Settings, search, network link, phone… |

**Player**

| Button | Action |
|---|---|
| ✕ | Play / pause |
| ← / → | Back / forward 10 s |
| L2 / R2 | Back / forward 30 s |
| L1 / R1 | Previous / next chapter (or file) |
| △ | Tracks: audio, subtitles, playback, picture |
| □ | Next frame (while paused) |
| L3 | Screenshot |
| Touchpad | Show the controls · swipe to seek or change the volume |
| ○ | Back |

## Folders on the PS5

| Path | What |
|---|---|
| `/data/homebrew/PPSA85300/media/` | The app's own media folder |
| `/data/vlc/` | Your media on the console (needs full access) |
| `/mnt/usb0` … `/mnt/usb7`, `/mnt/ext0` | USB and extended storage |
| `/data/homebrew/PPSA85300/vlc-ps5.log` | Log of the last session (attach it when reporting a problem) |

## All VLC Features

✅ In the app.

### 🎞️ Video

| Feature | Status |
|---|---|
| H.264, HEVC 8/10-bit, AV1, VP9, VC-1, MPEG-2, MPEG-4 and every format FFmpeg knows | ✅ |
| MKV, MP4, AVI, MOV, TS, WebM, FLV, OGG… | ✅ |
| Up to 4K at 60 Hz output | ✅ |
| Auto fast decoding when a file can't keep up | ✅ |
| Fast keyframe seeking | ✅ |
| Aspect ratios (fit, fill, stretch, 16:9, 4:3, 21:9) and zoom | ✅ |
| Deinterlace (auto / yadif) | ✅ |
| Picture controls: brightness, contrast, saturation, gamma, hue | ✅ |
| Rotate, flip, sharpen | ✅ |
| 360° videos (look around with the right stick) | ✅ |
| Frame-by-frame step | ✅ |
| Screenshots to PNG | ✅ |
| Hardware decoding (H.264 / HEVC) | ✅ |
| HDR → SDR tone mapping | ✅ |
| HDR10 output to the TV | ✅ |
| Hardware decoding (VP9 up to 4K) | Not yet |
| Upscaling up to 4K (FSR 1, Lanczos, Anime4K etc) | Not yet |

### 🔊 Audio

| Feature | Status |
|---|---|
| AAC, AC-3, E-AC-3, DTS, DTS-HD, TrueHD, FLAC, ALAC, Opus, Vorbis, MP3 | ✅ |
| 5.1 / 7.1 surround output | ✅ |
| Speaker test | ✅ |
| Stereo downmix for headphones | ✅ |
| Audio delay (lip sync) | ✅ |
| 10-band equalizer with presets | ✅ |
| Night mode (compressor) | ✅ |
| Volume boost up to 200 % | ✅ |
| Preferred audio language | ✅ |

### 💬 Subtitles

| Feature | Status |
|---|---|
| SRT, ASS/SSA, embedded MKV/MP4, PGS, DVB, DVD, closed captions | ✅ |
| Every script: Arabic, Hebrew, Cyrillic, CJK, Thai, Devanagari… | ✅ |
| Styled ASS/SSA (libass: fonts, positions, karaoke) | ✅ |
| Auto-load `movie.srt` next to the video | ✅ |
| Add a subtitle file while playing | ✅ |
| Subtitle delay | ✅ |
| Size, colour, background box | ✅ |
| Preferred subtitle language | ✅ |
| OpenSubtitles download (your own free API key) | ✅ |

### ▶️ Playback

| Feature | Status |
|---|---|
| Resume and Continue watching | ✅ |
| Seek preview while scrubbing | ✅ |
| Speed 0.25× – 4× | ✅ |
| Playlists | ✅ |
| Chapters (list, skip, marks on the seek bar) | ✅ |
| Bookmarks inside a file | ✅ |
| A–B repeat | ✅ |
| Loop, repeat list, shuffle | ✅ |
| Playlists (.m3u, .m3u8, .pls) | ✅ |
| Up next with countdown, Watch again | ✅ |
| Sleep timer | ✅ |
| Touchpad swipes (seek, volume) | ✅ |

### 📚 Library & interface

| Feature | Status |
|---|---|
| Home, Videos, Music, Browse tabs with thumbnails | ✅ |
| Classic and Modern looks | ✅ |
| Colour themes (Modern) + custom theme file | ✅ |
| 16 languages (follows the console's language) | ✅ |
| First-run setup | ✅ |
| Favourites | ✅ |
| Search with on-screen keyboard | ✅ |
| A–Z jump in long lists | ✅ |
| File details (codec, resolution, bitrate…) | ✅ |
| Delete files and folders | ✅ |
| USB drives detected while running | ✅ |
| Music mode: covers, artist / album, plays behind the menus | ✅ |
| Audio visualizer | ✅ |
| Photo viewer with slideshow | ✅ |
| Text / .nfo viewer | ✅ |

### 🌐 Network

| Feature | Status |
|---|---|
| PC/Phone page: send files over Wi-Fi | ✅ |
| SMB shares (Windows, NAS) with login | ✅ |
| DLNA / UPnP media servers | ✅ |
| Network links: HTTP, HTTPS, HLS, DASH, UDP, RTP, FTP | ✅ |
| Free TV channels (iptv-org) | ✅ |
| Internet radio (radio-browser) | ✅ |

### 💿 Discs & files

| Feature | Status |
|---|---|
| DVD folders and ISOs with menus | ✅ |
| Blu-ray folders and ISOs (unprotected) | ✅ |
| Blu-ray Java (BD-J) menus | ❌ needs Java, which the PS5 hasn't got: those discs play without menus |
| ZIP, RAR and 7z archives open like folders | ✅ |

## Building

On Linux (WSL2 Ubuntu works), with [PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan) built in `~/ps5/PS5_Vulkan`:

```sh
mkdir -p ~/ps5/vlc-ps5/src && cd ~/ps5/vlc-ps5/src
curl -LO https://download.videolan.org/pub/videolan/vlc/3.0.24/vlc-3.0.24.tar.xz
tar xf vlc-3.0.24.tar.xz && cd -               # back to this repository

TARGET=ps5 bash ps5/deps/build-deps.sh     # the libraries (FFmpeg, GnuTLS, libass…)
TARGET=ps5 bash ps5/configure-vlc.sh       # libvlc 3.0.24 with every plugin linked in
bash ps5/build-title.sh                    # dist/PPSA85300/ with a signed eboot.bin
```

The libraries are downloaded by `build-deps.sh`; the changes to VLC and FFmpeg are in [`patches/`](patches). `bash host/build-app.sh` builds a Linux version of the app for testing.

## Credits

- **[VLC](https://www.videolan.org/)** by VideoLAN and its contributors, and **[FFmpeg](https://ffmpeg.org/)**.
- **[Mihawk-99](https://github.com/mihawk-99)**: [PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan), RADV on the PS5.
- **[ps5-payload-dev SDK](https://github.com/ps5-payload-dev/sdk)**, **[kstuff](https://github.com/EchoStretch/kstuff)**, **[etaHEN](https://github.com/LightningMods/etaHEN)**, **[ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus)** and the PS5 scene.
- **[Dear ImGui](https://github.com/ocornut/imgui)**, [volk](https://github.com/zeux/volk), [stb](https://github.com/nothings/stb), GnuTLS, libsmb2, libupnp, libxml2, libass, FreeType, FriBidi, HarfBuzz, libdvdnav, libbluray and libarchive.
- Fonts: **[Inter](https://rsms.me/inter/)**, **Selawik**, **DejaVu Sans Mono** and **Noto**.
- [iptv-org](https://github.com/iptv-org/iptv), [radio-browser.info](https://www.radio-browser.info/) and [OpenSubtitles](https://www.opensubtitles.com/).

## License

VLC for PS5 is licensed under the **GNU General Public License v3.0** (see [`LICENSE`](LICENSE)).
VLC itself is GPL-2.0-or-later / LGPL-2.1-or-later. The bundled libraries and fonts keep their own licenses.
