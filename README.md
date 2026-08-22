# Netune — 终端音乐播放器

基于 C/FFmpeg 和 FTXUI 构建的终端音乐播放器，支持本地文件播放和网易云音乐集成。

## 构建

```bash
# 稳定版（master）
git clone https://github.com/ThripleQ/Netune.git
cd Netune

# 或测试版（beta，新功能先行）
git clone -b beta https://github.com/ThripleQ/Netune.git netune-beta
cd netune-beta

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
cp build/netease-cli ~/.local/bin/   # 存在时才复制（见下方说明）
netune
```

> - 首次运行会自动生成默认配置和数据树到 `~/.config/netune/`，无需手动复制 `data/`。
> - `netease-cli` 在检测到 Go >= 1.22 时由 CMake 自动构建；无 Go 时会从 GitHub Release 自动下载预编译二进制。两种情况都失败时该文件不存在，可跳过复制——网易云功能不可用，其余功能正常。

## 功能

- 本地 MP3/FLAC/WAV 播放
- 网易云音乐：搜索、歌单、每日推荐、歌单管理（创建/收藏/重命名/删除）、歌曲详情
- 主题系统：15 个颜色槽（背景/文字/线框/频谱/Logo 等），内置 5 套预设，颜色槽支持"无色"（透出终端背景）
- 筛选模式（`/`）和全局搜索
- 快进/快退（`←`/`→`）、循环模式（`r`）
- 鼠标支持：点击选中、滚轮滚动
- 歌词模式（`l`）
- ALSA、PulseAudio、SDL2 自动检测
- 导航栈：`Esc` 返回上一层
- 内置配置管理器 `netune --config`（主题编辑 / 按键绑定 / 本地音乐目录）

## 快捷键

完整按键表按 `?` 查看（含当前配置）。常用：

| 按键 | 功能 |
|------|------|
| `Tab` | 切换面板（歌单 / 歌曲） |
| `j / k` 或 `↓ / ↑` | 上下移动 |
| `Enter` | 播放选中 |
| `Space` | 播放 / 暂停 |
| `n / p` | 下一首 / 上一首 |
| `→ / ←` | 快进 / 快退 |
| `+ / -` | 音量加 / 减 |
| `r` | 循环模式 |
| `l` | 歌词 |
| `m` | 静音 |
| `s` | 停止 |
| `/` | 搜索 |
| `?` | 帮助（再按或 `Esc` 关闭） |
| `Ctrl+X` | 操作小窗（喜欢 / 收藏歌单 / 下载 / 歌曲详情 / 移除） |
| `q` | 退出 |

## 配置

配置文件位于 `~/.config/netune/data/`：

- `config.json` — 主配置（本地音乐目录、主题、按键、播放）
- `themes/*.yaml` — 主题
- `keybindings/default.yaml` — 按键绑定
- `layouts/*.yaml` — 布局

日常配置用内置子命令（无需单独程序）：

```bash
./build/netune --config
```

五个页面：主题（`x` 进颜色槽编辑，`Enter` 选用）、按键（编辑/选用）、本地音乐（`a` 添加目录）、播放（音量/循环/步长）。也可以直接改 `config.json`：

```json
"music_sources": {
  "local": {
    "enabled": true,
    "dirs": ["~/Music/pop", "~/Music/rock"]
  }
}
```

`dirs` 支持 `~` 展开，默认不填则不扫描任何目录。

## 感谢

感谢 [go-musicfox/netease-music](https://github.com/go-musicfox/netease-music) — 完整的网易云 API Go 封装，提供加密/签名/UNM 一站式方案

## 协议

MIT

[English version](README_EN.md)
