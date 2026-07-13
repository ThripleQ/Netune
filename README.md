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
- 🔄 **Loop modes** — Off / One / All via `l` key
- ⚡ **Async loading** — Non-blocking network operations with loading spinner
- 🧭 **Navigation stack** — `Esc` to go back through history

## Screenshot

```
┌─ left panel ──┐──┌─ right panel (song list) ─────────┐
│ 本地音乐      │  │  Title — Artist                     │
│ 网易云音乐    │  │  Another Song — Artist        03:45 │
│               │  │  ...                                │
├──── status ───┤  │  Netease logo watermark             │
│ ▶ Off 00:00   │  │  (when list empty)                  │
│ ▓▓▓░░░░░░░░░  │  │                                     │
└───────────────┘──┴─────────────────────────────────────┘
```

## Dependencies

- C11/C++17 compiler
- CMake ≥ 3.16
- FFmpeg (libavformat, libavcodec, libswresample)
- ALSA (libasound2)
- FTXUI (auto-downloaded via FetchContent)
- yyjson, libyaml (included)

## Build

```bash
git clone https://github.com/ThripleQ/Netune.git
cd Netune
cmake -B build
cmake --build build -j$(nproc)
./build/lmusic
```

### Startup config

Edit `data/config.json` to set audio device, playback behavior, theme, etc.

## Keybindings

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

## Search

Two search modes:

| Mode | Trigger | Behavior |
|------|---------|----------|
| **Filter** | `/` key | Real-time filter of current playlist (local + netease) |
| **Global** | Menu "搜索网易云" | Enter to submit, async Netease API search |

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

## Themes

Edit `data/config.json` → `"theme": "name"`:

| Name | File | Description |
|------|------|-------------|
| `default` | `themes/default.yaml` | Tokyo Night-inspired dark |
| `catppuccin` | `themes/catppuccin.yaml` | Catppuccin Mocha |
| `dracula` | `themes/dracula.yaml` | Dracula |
| `netease_dark` | `themes/netease_dark.yaml` | Netease-style dark |
| `netease_light` | `themes/netease_light.yaml` | Netease-style light |

## License

MIT
