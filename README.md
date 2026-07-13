# Netune v2.0 — Terminal Music Player

A terminal-based music player with local file support and Netease Cloud Music integration, built with C/FFmpeg and FTXUI.

## Build

```bash
git clone https://github.com/ThripleQ/Netune.git
cd Netune
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
./build/netune
```

### Dependencies

| Package | Required for |
|---------|-------------|
| CMake | Build system |
| FFmpeg (libavformat, libavcodec, libswresample) | Streaming decoder |
| ALSA | Audio output (Linux) |
| PulseAudio | Audio output (Linux) |
| SDL2 | Audio output (cross-platform) |
| yyjson | JSON parsing |
| libyaml | Config loading |
| Go | Building netease-cli (needed for Netease features) |

FTXUI is auto-downloaded by CMake.

Install with your system's package manager. For example on Debian/Ubuntu:
`apt install cmake libavformat-dev libavcodec-dev libswresample-dev \
  libasound2-dev libpulse-dev libsdl2-dev libyyjson-dev libyaml-dev`

### Install to PATH

```bash
cp build/netune ~/.local/bin/
cp bin/netease-cli ~/.local/bin/
cp -r data ~/.local/bin/data/
netune
```

## Features

- Local MP3/FLAC/WAV playback
- Netease Cloud Music (search, playlists, daily recommend)
- Theme system with 5 presets
- Fiter mode (`/`) and global search
- Seek forward/backward (`←`/`→`)
- Loop modes: Off / One / All (`l` key)
- ALSA, PulseAudio, SDL2 auto-detection
- Navigation stack: `Esc` to go back

## Keybindings

| Key | Action |
|-----|--------|
| `j/k` or `↑/↓` | Navigate |
| `Tab` | Switch panel |
| `Enter` | Play / Select |
| `Space` | Play / Pause |
| `/` | Filter playlist |
| `←`/`→` | Seek |
| `+`/`-` | Volume |
| `l` | Loop mode |
| `s` | Stop |
| `m` | Mute |
| `?` | Help |
| `q` | Quit |

## Themes

Edit `data/config.json` → `"theme"`: `default`, `catppuccin`, `dracula`, `netease_dark`, `netease_light`.

## License

MIT
