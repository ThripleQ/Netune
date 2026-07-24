# Netune v2.0 — 终端音乐播放器

基于 C/FFmpeg 和 FTXUI 构建的终端音乐播放器，支持本地文件播放和网易云音乐集成。

## 构建

```bash
git clone https://github.com/ThripleQ/Netune.git
cd Netune
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
./build/netune
```

### 依赖

| 包 | 用途 |
|------|------|
| CMake | 构建系统 |
| FFmpeg >= 4.0（推荐 >= 5.1；需要 libavformat、libavcodec、libswresample） | 流媒体解码 |
| ALSA | 音频输出（Linux） |
| PulseAudio | 音频输出（Linux） |
| SDL2 | 音频输出（跨平台） |
| yyjson | JSON 解析 |
| libyaml | 配置加载 |
| Go >= 1.22 | 构建 netease-cli（网易云功能需要） |

FTXUI 和 yyjson 由 CMake 自动下载（若系统未安装）。

系统包管理器安装示例（Debian/Ubuntu）：

```bash
apt install cmake pkg-config libavformat-dev libavcodec-dev libswresample-dev \
  libasound2-dev libpulse-dev libsdl2-dev libyaml-dev
```

> 注意：
> - 部分 Debian/Ubuntu 版本没有 `libyyjson-dev` 包。CMake 会自动从源码编译 yyjson，无需手动安装。
> - `netease-cli` 需要 **Go >= 1.22**。旧版 Debian/Ubuntu（如 Ubuntu 22.04 自带 Go 1.18）需要从 [go.dev/dl](https://go.dev/dl/) 安装新版 Go。

### 安装到 PATH

```bash
cp build/netune ~/.local/bin/
cp build/netease-cli ~/.local/bin/
cp -r data ~/.local/bin/data/
netune
```

> `netease-cli` 在检测到 Go >= 1.22 时由 CMake 自动构建。验证：`ls -l build/netease-cli`

## 功能

- 本地 MP3/FLAC/WAV 播放
- 网易云音乐（搜索、歌单、每日推荐）
- 主题系统，内置 5 套预设
- 筛选模式（`/`）和全局搜索
- 快进/快退（`←`/`→`）
- 循环模式：不循环 / 单曲循环 / 列表循环（`r` 键）
- ALSA、PulseAudio、SDL2 自动检测
- 导航栈：`Esc` 返回

## 快捷键

| 按键 | 功能 |
|------|------|
| `j/k` 或 `↑/↓` | 导航 |
| `Tab` | 切换面板 |
| `Enter` | 播放 / 选择 |
| `Space` | 播放 / 暂停 |
| `/` | 筛选播放列表 |
| `←`/`→` | 快进/快退 |
| `+`/`-` | 音量 |
| `r` | 循环模式 |
| `s` | 停止 |
| `m` | 静音 |
| `?` | 帮助 |
| `q` | 退出 |

## 主题

编辑 `data/config.json` → `"theme"`：`文件名字`

## 协议

MIT

[English version](README_EN.md)
