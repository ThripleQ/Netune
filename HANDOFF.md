# Netune 工作交接（HANDOFF）

> 本文档用于把音频缓存功能的上下文完整交接给下一个接手者（AI 助手或人）。
> 推荐阅读顺序：本文档 → `git log --oneline -15` → `README.md` → 各文件头注释。

- 日期：2026-08-24
- 分支：`beta`（本地与远端已同步）
- 交接范围：网易云音频缓存功能（已完成、已构建、已推送）

---

## 1. 项目一句话

Netune 是一个终端音乐播放器：C11 + C++17，FTXUI 渲染 TUI，FFmpeg 解码（含网络流），libnetease 作为网易云 API 后端（`netease-cli` 子进程），Windows + Linux 双平台（当前在 Windows/MSVC 下开发构建）。

源码根：`C:\Users\liuca\Netune`。构建：`cmake --build build --config Release --target netune`。

---

## 2. 本次交接重点：音频缓存

### 2.1 行为（用户可见）

- 播放网易云歌曲时**自动缓存**原始音频流（mp3/flac 字节，不加密）。
- **命中优先**：再次播放同一首歌（同音质）直接读缓存文件，不联网，可离线听。
- **部分缓存**：听到一半切歌，已收到的字节会保留为部分缓存；下次播放从缓存开头无缝续播，缓存耗尽后自动用 HTTP Range 从断点继续拉流，并**回填补全**缓存文件——播完一遍自动变完整缓存。
- **seek 复用**：部分缓存续播时，seek 回已缓存区间直接从磁盘读（不联网）；seek 到未缓存区间走网络并冻结回填（不破坏缓存）。
- **容量管理**：默认 2GB（`cache.audio_limit_mb`），超限自动按 LRU（ts 最旧）删除。
- **透明**：缓存文件是标准音频文件，用户可自行把缓存目录加进本地音乐列表当离线库。

### 2.2 设计决策（为什么这样做）

| 决策 | 理由 |
|---|---|
| 不加密 | 透明缓存的语义下文件可重建、清了没事；加密要多维护一条解密播放链路，收益低。API 合法性取决于取 URL 的方式，与落盘加密无关（网易云 .uc/.ncm 也是客户端拿到流后本地加密）。 |
| 缓存进独立目录，不进本地列表 | 用户明确选择"透明缓存"：`%LOCALAPPDATA%\netune\audio\`，与 config（`data\`）彻底分开。 |
| 命中优先播放 | 不这样的话缓存毫无意义；还能免疫网易云 URL 时效过期问题（缓存文件不依赖 URL）。 |
| 按 song_id 命名而非 URL hash | 网易云 URL 有时效会变，hash 命名会导致缓存失效；song_id 稳定。这也是不用 FFmpeg `shared` 协议的原因（它按 URL hash 命名 + 私有缓存格式不透明）。 |
| 部分缓存续传回填 | 完整播完太少（用户常切歌），只有完整才缓存等于功能废掉。前缀续传让缓存越用越完整。 |
| 顺序前缀模型而非块级(spacemap) | 块级"任意碎片命中"工作量中等、对 3-5 分钟的歌收益有限；前缀模型 + seek 内复用已覆盖实用场景。 |
| 探测期越界读不落盘、探测后重绕 | partial 续播时 FFmpeg probe 若读到前缀末尾之后，网络字节不写盘并保持前缀句柄；探测完成后重开 Range 流并把 tee 重绕回 0，避免小前缀在缓存文件中间留"空洞"（`rec_probing` 字段）。 |
| 探测后同步预取一块（512KB） | partial 续播的前缀耗尽切换点若走冷网络读会有一个 RTT 的卡顿（实际听感可感知）；探测完成后从网络预拉一块到内存，切换时从内存出，播完这块再回落到按需同步读——不是整首下载，无需后台线程。 |

### 2.3 代码地图

| 文件 | 职责 |
|---|---|
| `src/core/audio_cache.c/.h` | 缓存索引（`audio_cache.json`）+ 容量 LRU + 清空。内存镜像 + 写穿 + pthread 锁（模式同 `netease_quality.c`）。 |
| `src/plugins/decoders/ffmpeg/ffmpeg_stream.c/.h` | 三种开流模式：`ffstream_open`（纯网络）、`ffstream_open_rec`（网络 + 录 .part）、`ffstream_open_partial`（缓存前缀 + Range 续传 + 回填）。自定义 AVIO tee。 |
| `src/core/playback_coordinator.c` | `open_stream` 命中决策（完整→decoder / 部分→open_partial / 未命中→open_rec）；EOF 提交完整缓存；停止/切歌转正部分缓存（`cache_keep_partial`）。 |
| `src/main.cpp` | 子命令 `--manage`（主名）+ `--config`（兼容别名）→ `run_config()`。 |
| `src/netune_config.cpp` | 新增"缓存"标签页：显示占用/上限/目录，`d` 清空（带确认）。 |

### 2.4 关键不变量（改代码前必读）

- 缓存目录：`netune_xdg_dir("XDG_CACHE_HOME", NULL)\audio\`（Windows = `%LOCALAPPDATA%\netune\audio\`）。
- 索引格式（`audio_cache.json`）：`{ "<song_id>": { "file", "size", "ts", "quality", "complete" } }`；`complete=1` 完整、`0` 部分前缀；旧条目缺 `complete` 按完整处理。
- 文件命名：`<song_id>.<ext>`；录制中为 `<song_id>.part`；ext 由 `ffstream_media_ext` 按容器探测（.mp3/.flac/.wav/.m4a）。
- config：`cache.audio_limit_mb`（默认 2048）、`cache.audio_enabled`（默认 true；关闭时 find/commit 全部失效，但不清理已有缓存）。
- 播放线程（`playback_coordinator.c`）是缓存的唯一写者：`g_rec_song/g_rec_level/g_rec_active` 线程局部语义，无锁。
- `ffstream_seek` 语义：非 partial 模式 → 删 .part（本次录制作废）；partial 模式 → 交给 `tee_seek` 按位置决定（前缀内读磁盘、前缀外冻结）。
- 录制只在**探测完成之后**开始（probe 期 seek 不污染缓存文件）。
- `ffstream_open_rec`（纯网络录制）在探测完成后**主动 `avio_seek(rec_pb, 0)` 归位再开录**：FFmpeg 只在 mpeg/mpegts 探测后自己归位，WAV 等格式会停在探测读过的位置（WAV 因 `probe_packets=32` 会丢掉开头 ~850KB）；seek 失败则跳过缓存（退化为纯网络播放，不写坏文件）。
- partial 模式探测若越过前缀：前缀句柄保持打开、网络字节不落盘；探测完成后重开 Range 网络源 + tee 重绕到 0，再开追加写句柄——保证缓存文件始终是连续前缀（无空洞）。这是 `rec_probing` 字段的语义。
- partial 续播预取（`rec_prefetch`）：探测完成后同步拉 512KB 网络字节到内存；`tee_read` 在前缀耗尽后先消费预取块（消费时才写盘，保持文件连续），耗尽后回落直接读网络；任何 seek 都清空预取块。
- `ffstream_open_partial` 打开网络源**必须同时设置 `headers`(Range) 和 `offset` option**：只设 Range 时 FFmpeg http 校验 Content-Range 起始偏移失败（"Unexpected offset: expected 0, got N"），直接拒绝打开——已实测确认，两处 `avio_open2` 都要带 `offset`。
- `tee_seek` 越界（前缀外）**先冻结再 seek**：先清 prefetch、关 rec_file/rec_part_read，再尝试 `avio_seek(rec_inner)`；即使底层 seek 失败，缓存文件也不会被错位回填污染。
- `audio_cache_commit` 会 stat 文件大小 + 触发 prune；更新已有条目（partial→complete）保留文件。

### 2.5 提交历史（beta，最新在顶）

```
b843bfc feat(play): serve seeks inside partial cache from disk
d71b155 feat(play): resume partial cache with network backfill
b29ca14 feat(cache): track partial vs complete cache entries
83fcce5 feat(ui): cache management tab in --manage tool
408af1b feat(play): stream recording + cache-hit playback
7069307 feat(cache): transparent audio cache core (index + capacity + clear)
```

更早的关联工作（本功能的前置）：`dab215c`（音质缓存内存镜像）、`0d32948`（切音质原位续播 CMD_RELOAD）、`0841f10`（下载队列）。

### 2.6 待验证清单（建议接手后实测）

> 2026-08-24 已用自动化测试（`netune_test/`，本地 Range HTTP 服务器 + `ffstream_open_partial` 直接解码）验证：
> - 场景 A（完整续播）：部分缓存（262KB 前缀）+ 完整播放 → 回填后缓存文件与源文件**逐字节一致**（7056044B），无缝无空洞；
> - 场景 B（seek 越界）：播 3s 后 seek 到缓存外 → 缓存文件冻结（seek 后不再增长），解码继续；
> - rec 模式（网络录制）：探测后主动 seek 回 0 再开录 → 录制文件与源文件**逐字节一致**（WAV 复现并修复了"缺开头"问题）；
> - net 模式（纯网络）：完整解码 40s。

- [x] 完整播完一首 → `%LOCALAPPDATA%\netune\audio\` 出现 `.mp3`，索引 `complete: true`。
- [x] 播 30 秒切歌 → 出现部分缓存（`complete: false`）；再播同一首 → 日志出现 `Continuing partial cache` 且立即出声。
- [x] 部分缓存继续播完 → 索引变 `complete: true`（回填补全）。
- [ ] 断网重播已完整缓存的歌 → 正常播放（命中本地）。
- [x] 快进到未缓存区间 → 播放不中断（走网络），缓存文件不被破坏。
- [ ] `netune --manage` → 缓存页显示占用/上限，`d` 清空后目录清空。
- [ ] 改 `cache.audio_limit_mb` 为极小值 → 播多首后最旧缓存被自动删除。
- [x] 制造一个很小的部分缓存（播 ~10 秒就切歌）→ 再播同一首 → 日志 `Continuing partial cache`；播完后检查缓存文件字节流连续（无空洞）、索引变 `complete: true`。
- [x] 部分缓存续播时听前缀耗尽处 → 无卡顿/无停顿（512KB 预取块生效），随后继续按需拉流不中断。

### 2.7 已知边界与后续方向

- 顺序前缀模型：seek 到未缓存区后冻结回填，不会补全"空隙"；要支持"任意碎片命中"需升级为 spacemap 块索引（可借鉴 FFmpeg `libavformat/shared.c` 的 `.spacemap` 思路）。
- 部分缓存续传依赖网易云对同一首歌同音质返回内容一致的 URL；若 CDN 实际返回不同编码，前缀拼接可能损坏（罕见，可接受）。
- 崩溃会残留 `.part` 文件；`audio_cache_clear` 会清理，但启动时未做清扫（可加）。
- 网络流 seek 回"已读过的网络区域"（非缓存区）可能失败（FFmpeg http 流缓冲限制），表现为 seek 不生效但播放不中断。
- ~~partial probe 越界读会在缓存文件中间留空洞~~（已修复：探测期越界读不落盘 + 探测后重绕，见 2.2/2.4）。代价：探测若越过前缀会多拉一段网络数据（丢弃），且探测后重连 Range 依赖 URL 未过期（窗口很小，通常无感）。

---

## 3. 给接手者的操作提示

1. 先跑一遍 2.6 验证清单，确认功能符合预期再动代码。
2. 改缓存行为时，保持 2.4 的不变量，特别是"播放线程唯一写者"和"probe 后才开录"。
3. 测试用 `netune --manage` 看缓存状态，日志里 `Continuing partial cache` / `Streaming netease` 可区分命中与拉流。
4. 若有重大行为变更，更新本文件 2.2 的决策表和 2.5 的提交历史。
