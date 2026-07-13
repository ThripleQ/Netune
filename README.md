# Netune v2.0 — Terminal Music Player

A terminal-based music player with local file support and Netease Cloud Music integration, built with C/FFmpeg and FTXUI.

## Features

- 🎵 **Local playback** — MP3/FLAC/WAV via dr_mp3/dr_flac/dr_wav
- ☁️ **Netease Cloud Music** — Search, playlists, daily recommendations, liked songs
- 🎨 **Theme system** — Multiple built-in themes (Catppuccin, Dracula, Netease Dark/Light)
- ⌨️ **Vim-like keybindings** — `j/k` navigate, `/` search, `Enter` play
- 🔍 **In-list filter** — `/` to filter current playlist in real-time
- 📜 **Scrolling marquee** — Long names auto-scroll on selection
- ⏩ **Seek support** — `←`/`→` forward/backward (local + streaming)
- 🔄 **Audo backends** — ALSA, PulseAudio, SDL2 (auto-detected)
- ⚡ **Async loading** — Non-blocking network operations with loading spinner
- 🧭 **Navigation stack** — `Esc` to go back through history
- 🖼️ **Colored watermark** — Brand logo displayed when list is empty

## Dependencies

| Package | Linux (Arch) | Linux (Debian/Ubuntu) |
|---------|-------------|----------------------|
| Compiler | `base-devel` | `build-essential` |
| CMake | `cmake` | `cmake` |
| FFmpeg | `ffmpeg` | `libavformat-dev libavcodec-dev libswresample-dev libavutil-dev` |
| ALSA | `alsa-lib` | `libasound2-dev` |
| PulseAudio | `libpulse` | `libpulse-dev` |
| SDL2 | `sdl2` (optional) | `libsdl2-dev` (optional) |
| yyjson | `yyjson` | `libyyjson-dev` |
| libyaml | `libyaml` | `libyaml-dev` |

FTXUI is auto-downloaded by CMake (FetchContent).

## Build & Install

### Linux

```bash
# Install dependencies (Arch)
sudo pacman -S base-devel cmake ffmpeg alsa-lib libpulse sdl2 yyjson libyaml

# Install dependencies (Debian/Ubuntu)
sudo apt install build-essential cmake pkg-config \
  libavformat-dev libavcodec-dev libswresample-dev \
  libasound2-dev libpulse-dev libsdl2-dev \
  libyyjson-dev libyaml-dev

# Build
git clone https://github.com/ThripleQ/Netune.git
cd Netune
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# Install
cp build/netune ~/.local/bin/
cp bin/netease-cli ~/.local/bin/
cp -r data ~/.local/bin/data/

# Run
netune
```

You can also run directly from the build directory without installing:

```bash
./build/netune
```

### Windows (manual)

Windows build requires [vcpkg](https://github.com/microsoft/vcpkg) and [Go](https://go.dev/).

```powershell
# Install deps via vcpkg
vcpkg install ffmpeg:x64-windows sdl2:x64-windows yyjson:x64-windows libyaml:x64-windows

# Build
cmake -B build -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_TOOLCHAIN_FILE="[vcpkg-path]\scripts\buildsystems\vcpkg.cmake"
cmake --build build --config Release -j

# Build netease-cli (Go)
cd src\plugins\music_sources\netease\netease-cli
go build -o netease-cli.exe .
move netease-cli.exe ..\..\..\..\build\Release\
```

## Usage

```
┌─ left panel ──┐──┌─ right panel (song list) ─────────┐
│ 本地音乐      │  │  Title — Artist                     │
│ 网易云音乐    │  │  Another Song — Artist              │
│               │  │  ...                                │
├──── status ───┤  │  Netease logo watermark             │
│ ▶ Off 00:00   │  │  (when list empty)                  │
│ ▓▓▓░░░░░░░░░  │  │                                     │
└───────────────┘──┴─────────────────────────────────────┘
```

### Keybindings

| Key | Action |
|-----|--------|
| `j` / `↑` | Move up |
| `k` / `↓` | Move down |
| `Tab` | Switch panel (left/right) |
| `Enter` | Play selected / Open menu item |
| `Space` | Play / Pause |
| `s` | Stop |
| `l` | Cycle loop mode (Off → One → All) |
| `←` / `→` | Seek backward/forward |
| `+` / `-` | Volume up/down |
| `m` | Mute toggle |
| `/` | Filter current playlist |
| `?` | Help |
| `q` | Quit |

### Search

| Mode | Trigger | Behavior |
|------|---------|----------|
| **Filter** | `/` key | Real-time filter of current playlist (local + netease) |
| **Global** | Menu "搜索网易云" | Enter to submit, async Netease API search |

### Themes

Edit `data/config.json` → `"theme": "name"`:

| Name | Description |
|------|-------------|
| `default` | Tokyo Night-inspired dark |
| `catppuccin` | Catppuccin Mocha |
| `dracula` | Dracula |
| `netease_dark` | Netease-style dark |
| `netease_light` | Netease-style light |

## Architecture

```
┌──── UI (FTXUI) ────┐
│  Components + Theme │
└──────┬─ Event Bus ─┐
┌──────▼─────────────┐
│  Core Layer        │
│  Playback/Search   │
└──────┬─────────────┐
┌──────▼─────────────┐
│  Plugins           │
│  MusicSource       │
│  AudioOutput       │
│  Decoder           │
└────────────────────┘
```

## License

MIT
