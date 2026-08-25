# Netune 工作交接（HANDOFF）

> 本文档用于把音频缓存功能的上下文完整交接给下一个接手者（AI 助手或人）。
> 推荐阅读顺序：本文档 → `git log --oneline -15` → `README.md` → 各文件头注释。

- 日期：2026-08-25
- 分支：`beta`（本地与远端已同步）
- 交接范围：网易云音频缓存（已完成）+ UI 状态收敛 / 搜索缓存线程修复 / Netease 特化接口（已完成，CI 双平台通过）+ 缓存架构升级 M1（后台下载线程 + 本地播放，进行中）

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
- **边下边播（M1）**：缓存未命中时，后台下载线程把流写入 `.part`，播放线程同时从本地文件解码播放——下载与播放各自独立推进，下载进度可远超播放进度（对齐主流流媒体平台体验），不再依赖播放线程同步拉流。
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
| 未命中走后台下载线程（M1） | 缓存未命中时，`stream_downloader` 用 libcurl 独立线程把流写入 `.part`，播放线程用 `ffstream_open_growing` 从本地文件解码。下载与播放解耦：下载线程跑在前面（或落后）都不影响对方；播放到水位线且下载未完成时才阻塞等待。对齐 ExoPlayer 的"下载线程 + 播放线程各自为战"。 |
| 增长文件用无缓冲 I/O + EOF 排空 | 下载线程写、播放线程读同一 `.part`：写句柄 `setvbuf(_IONBF)` 保证字节立即可见；读句柄同样无缓冲。读端在 EOF 处通过 wait 回调阻塞等待下载推进；wait 返回"完成"后再补一次排空读取，避免"EOF 检测与完成信号之间写入的最后字节"丢失。 |
| AVIO 读回调 EOF 必须返回 `AVERROR_EOF` | FFmpeg 6.0 的 `avio_read` 绕过缓冲路径（`size > buffer_size`）时，读回调返回 0 会被当成"无数据、重试"而**死循环**（实测卡死）；必须返回负值 `AVERROR_EOF` 表示文件结束。`growing_read` 已按此修复。 |

### 2.3 代码地图

| 文件 | 职责 |
|---|---|
| `src/core/audio_cache.c/.h` | 缓存索引（`audio_cache.json`）+ 容量 LRU + 清空。内存镜像 + 写穿 + pthread 锁（模式同 `netease_quality.c`）。 |
| `src/core/stream_downloader.c/.h` | **M1 新增**：后台下载线程。libcurl 把 URL 流写入 `.part`（唯一写者），维护水位线（watermark）+ done/failed/stop 状态，`wait_watermark` 供播放线程阻塞同步。销毁时 `.part` 留在磁盘，由调用方决定提交/保留/删除。 |
| `src/plugins/decoders/ffmpeg/ffmpeg_stream.c/.h` | 四种开流模式：`ffstream_open`（纯网络）、`ffstream_open_rec`（网络 + 录 .part）、`ffstream_open_partial`（缓存前缀 + Range 续传 + 回填）、`ffstream_open_growing`（**M1 新增**：本地增长文件，读端阻塞等待下载推进）。自定义 AVIO tee。 |
| `src/core/playback_coordinator.c` | `open_stream` 命中决策（完整→decoder / 部分→open_partial / 未命中→**启动下载器 + open_growing**）；EOF 提交完整缓存；停止/切歌转正部分缓存（`cache_keep_partial`）。 |
| `src/main.cpp` | 子命令 `--manage`（主名）+ `--config`（兼容别名）→ `run_config()`。 |
| `src/netune_config.cpp` | 新增"缓存"标签页：显示占用/上限/目录，`d` 清空（带确认）。 |

### 2.4 关键不变量（改代码前必读）

- 缓存目录：`netune_xdg_dir("XDG_CACHE_HOME", NULL)\audio\`（Windows = `%LOCALAPPDATA%\netune\audio\`）。
- 索引格式（`audio_cache.json`）：`{ "<song_id>": { "file", "size", "ts", "quality", "complete" } }`；`complete=1` 完整、`0` 部分前缀；旧条目缺 `complete` 按完整处理。
- 文件命名：`<song_id>.<ext>`；录制中为 `<song_id>.part`；ext 由 `ffstream_media_ext` 按容器探测（.mp3/.flac/.wav/.m4a）。
- config：`cache.audio_limit_mb`（默认 2048）、`cache.audio_enabled`（默认 true；关闭时 find/commit 全部失效，但不清理已有缓存）。
- 播放线程（`playback_coordinator.c`）是**旧录制模式**（`open_rec`/`open_partial` 回填）缓存的唯一写者：`g_rec_song/g_rec_level/g_rec_active` 线程局部语义，无锁。
- **M1 增长文件模式**（未命中 → `stream_downloader` + `ffstream_open_growing`）：`.part` 的**唯一写者是下载线程**（`dl_write_cb`），播放线程只读水位线以下的字节。`g_downloader/g_part_path` 由播放线程持有（无锁，仅播放线程访问）。下载器销毁时 `.part` 留在磁盘，由 `cache_keep_partial`（切歌/停止）或 EOF 提交逻辑决定 rename 成完整/部分缓存。
- 增长文件读端（`growing_read`）：EOF 时经 wait 回调（**带当前读位置 pos**，精确等待 `watermark > pos`）阻塞等待下载推进；wait 返回"完成"后补一次排空读取再返回 `AVERROR_EOF`。**读回调 EOF 必须返回 `AVERROR_EOF`（负值）而非 0**，否则 FFmpeg 6.0 `avio_read` 绕过缓冲路径死循环（见 2.2）。
- 增长文件 seek（`growing_seek`）：目标 **clamp 到当前磁盘大小**（超界落到水位，不产生文件空洞）；`ffstream_seek` 对 growing 模式不触碰 recorder 字段（`!rec_partial && !growing` 才 discard）。
- 增长文件时长：probe 时文件未下完，`fmt->duration` 偏低；播放线程按 `(文件大小 × 8 / bitrate)` **动态刷新 total_frames**，进度条/seek 边界随下载增长。
- 下载失败（`failed`）时解码侧返回 EIO → 播放线程发 **`EV_PLAYBACK_ERROR`**（而非静默 `EV_PLAYBACK_FINISH`），已下载字节转正部分缓存。
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
5936998 fix(cache): harden M1 growing-file playback
        —— M1 加固：失败发 EV_PLAYBACK_ERROR / total_frames 随下载刷新 /
           growing_seek 超界 clamp / wait 回调带 pos 精确等待 / 超时保护
26460ec feat(cache): M1 background downloader + growing-file playback
        —— M1：stream_downloader（libcurl 下载线程）+ ffstream_open_growing
           （增长文件解码），下载/播放解耦；docs/cache-redesign.md 入库
b843bfc feat(play): serve seeks inside partial cache from disk
d71b155 feat(play): resume partial cache with network backfill
b29ca14 feat(cache): track partial vs complete cache entries
83fcce5 feat(ui): cache management tab in --manage tool
408af1b feat(play): stream recording + cache-hit playback
7069307 feat(cache): transparent audio cache core (index + capacity + clear)
```

更早的关联工作（本功能的前置）：`dab215c`（音质缓存内存镜像）、`0d32948`（切音质原位续播 CMD_RELOAD）、`0841f10`（下载队列）。

### 2.6 待验证清单（建议接手后实测）

> ⚠️ **`netune_test/` 未入库**（2026-08-25 核实）：以下历史验证记录提到的
> `netune_test/`（`test_partial` / `test_stream` / `test_cache` / `test_grow`）
> 在 beta 分支的 git 历史中从未提交过，源码包里也没有该目录。它曾是本地
> 独立测试工程（详见 5.5 的本地构建故障记录）。接手者如需跑测试，需先
> 从原作者处取得该目录（或重建等价测试）；M1 相关行为（增长文件解码、
> wait 回调）目前无自动化回归，靠 2.6 的手动清单兜底。
>
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
- [x] **M1 增长文件解码**（`netune_test` 等价：`test_grow` 直接调 `ffstream_open_growing`）：静态文件完整解码 220500 帧；增长文件（先给前半段，EOF 时 wait 回调追加后半段）同样完整解码 220500 帧，wait 回调正确触发。修复了读回调返回 0 导致 FFmpeg `avio_read` 死循环的问题。
- [ ] **M1 端到端**：缓存未命中播网易云歌 → 日志 `Streaming netease (download+play)`，播放立即出声，下载线程独立推进；播完 → `.part` rename 成完整缓存；中途切歌 → `.part` 保留为部分缓存。

### 2.7 已知边界与后续方向

- 顺序前缀模型：seek 到未缓存区后冻结回填，不会补全"空隙"；要支持"任意碎片命中"需升级为 spacemap 块索引（可借鉴 FFmpeg `libavformat/shared.c` 的 `.spacemap` 思路）。
- **M1 增长文件模式**：播放线程读到水位线时阻塞等待下载推进（`wait_more_data` 500ms 上限轮询），下载落后时播放会停顿——这是"边下边播"的固有语义，与主流流媒体一致；后续 M2+ 可加"下载进度 > 播放进度 N 秒才允许播放"的缓冲策略，或断网恢复。
- **M1 后续方向**（cache-redesign.md 里程碑）：M2 断网恢复（下载失败时播放已缓存前缀并暂停等待重连）、M3 退役旧录制路径（`open_rec`/`open_partial` 回填）、M4 多段缓存（spacemap 块索引，任意碎片命中）、M5 缓存唯一写者收编（下载器统一写，播放线程只读）。
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
5. 改网易云相关功能时走 `netease_ext()` 接口（5.2/5.3），不要直接 include `netease_api.h`。
6. 本地构建被 5.5 的 MSBuild 故障阻塞时，用 CI（push beta 触发）验证，别改代码绕。

---

## 4. UI 层状态与并发（2026-08-24 补充）

### 4.1 状态收敛：所有 UI 状态进 StateStore

`StateStore`（`src/ui/state_store.h`）是 **UI 唯一真相源**。曾经散落在 `app.cpp` 文件级全局的 UI 状态已全部收编进 `AppState`：

| 原全局变量 | 现 AppState 字段 |
|---|---|
| `g_lyric_pending_id` | `lyric_pending_id`（异步歌词拉取去重标志） |
| `g_seek_accum` / `g_seek_target` / `g_last_seek_tp` | `seek_accum` / `seek_target` / `seek_last_ms`（150ms 防抖用 ms 整数） |
| `g_resize_w` / `g_resize_h` | `last_resize_w` / `last_resize_h`（SIGWINCH vs Ctrl+/ 判别） |
| `g_top_search_pushed` | `top_search_pushed`（搜索框 nav 入栈标志） |

改 UI 时**不要新建文件级全局可变状态**——放进 `AppState` + 提供 setter，事件回调只做"搬运"。

### 4.2 仍留全局的（有意为之）

- `g_login_*`（unikey / qr 位图 / ready / stamp）：**跨线程**（QR 生成 worker 写 + `volatile` 同步，主线程读）。要动必须做独立 LoginFlow 带自己的同步，不能直接塞 StateStore。
- `g_ns_cache`：网易云搜索结果缓存。**线程模型已修复**（见 4.3），保留为独立数据缓存合理。
- `g_running` / `g_thread_pool` / `g_keybindings`：基础设施，非 UI 状态。

### 4.3 搜索缓存线程模型（重要）

`g_ns_cache`（`app.cpp` 内 `static std::map<std::string, std::vector<SongInfo>>`，LRU cap 32）：
- **只有主线程访问**：搜索 worker（`std::thread(...).detach()`）**不碰缓存**——它只把结果打包成 `SearchLoadResult` 发 `EV_SEARCH_LOADED` 事件；主线程回调 `ev_search_loaded` 写缓存 + `set_playlist`。
- 这是修复过的设计（原来是 worker 无锁写缓存 + 主线程读，数据竞争）。**别改回**在 worker 里写缓存的写法；若要动缓存，保持"单线程访问"或加锁。
- 缓存命中路径：`do_netease_search` 开头查缓存 / `netease_search_apply_cache`。

### 4.4 事件总线线程约定

- `event_bus_publish` **只入队**（浅拷贝 payload，指针指向的数据跨线程传递、主线程释放），回调在 `event_bus_poll`（主线程 Renderer 内）执行。
- 所以"worker 线程"只负责：拉数据 → 打包 payload → `publish`；**所有状态写入都在主线程回调里做**。跨线程共享数据（如歌词 `Lyrics*`、搜索结果数组）的所有权协议：worker 分配 → 主线程回调释放。

### 4.5 遗留问题（已知，未处理）

- `app.cpp` ~3500 行上帝文件：承载全部业务/渲染/输入/事件回调。后续按领域拆控制器（Login/Search/Playlist/Playback/Download）。
- `mpris.cpp` 独立线程 + 播放状态镜像，与 StateStore 双份，靠手工同步。
- `Renderer` 里做了登录轮询/后台线程等业务逻辑（副作用），应只读 state。
- `do_netease_search` 的 worker 是 `.detach()`，shutdown 时不可等待（事件会入队但无人 poll，进程退出由 OS 回收）。

---

## 5. Netease 特化接口（2026-08-24 补充）

### 5.1 动机

通用插件接口 `MusicSource`（`core/music_source.h`）只覆盖"所有源共有"的能力（search/detail/play_url/lyric/cover），覆盖不了网易云的登录/歌单增删改/下载/已购/音质/VIP。过去 `app.cpp` 直接 include `netease_api.h`（实现）硬编码 20+ 处 `netease_*()` 调用——UI 层触达音乐源实现，这是"UI 难改"的耦合根源之一。

### 5.2 方案：通用接口 + 特化扩展（独立模块）

特化接口**不挂进** `MusicSource`（避免接口膨胀/泄漏），作为独立模块：

```
src/plugins/music_sources/netease/netease_ext.h  ← 自包含：NeteaseExt 函数指针表 + 类型(NSSong/NSSearchResult/SongDetail/NQ_*) + netease_download_progress
src/plugins/music_sources/netease/netease_ext.c  ← 填充表 + netease_ext() 单例（编译期 static const）
```

- `netease_api.h` 删掉类型/宏定义，改为 include `netease_ext.h`（**实现依赖接口**）。
- 应用层（`app.cpp` / `download_queue.cpp` / `state_store.cpp`）只 include `netease_ext.h`，不再 include `netease_api.h`。
- `app.cpp` 用文件级 `static const NeteaseExt *g_ne = netease_ext();`（**声明即初始化**，无 NULL 窗口）。

**调用方式**：`g_ne->search(...)` / `netease_ext()->download_song(...)`。

### 5.3 关键约定（改代码前必读）

- `g_ne` 是主线程初始化的只读指针；worker 线程（`std::thread`）里读它安全（thread 构造有 happens-before），**但不要在 worker 里写**。
- `netease_quality.h`（`nq_*` 独立模块）**不在** ext 表里，保持现状。
- `download_url` **不在** ext 表里——它是 `netease_download_song` 的内部后端（lossless/hires/jymaster 档走官方下载端点，`netease_api.c:1383`），UI 层从不直接调。别再把它加回 ext。
- 扩展新能力时优先加进 `NeteaseExt` 表；只有纯内部 helper 才留在 `netease_api.h` 不外露。
- `g_ne` 的使用点全部在主线程事件回调 / worker（只读），已审查无数据竞争。

### 5.4 验证

- **CI 双平台通过**（Build #514，commit `72c2906`）：linux（ubuntu + 系统 FFmpeg）1m51s、windows（vcpkg + MSBuild）3m36s，Configure/Build/Upload 全成功。
- 特化接口主体构建在本地通过过（`netease_ext.c` 链接成功）；最后的 `g_ne` 声明初始化 + 移除 `download_url` 两处微调未能本地构建（见 5.5）。

### 5.5 本地构建环境故障（重要，接手者须知）

**本地 Windows/MSVC 构建当前不可用**：`MSBuild.exe` 连 VCTargetsPath 探测都 `Access violation` 崩溃（CMake 配置阶段，尚未编译源码；全新 build 目录、干净 PATH、`MSBUILDDISABLENODEREUSE=1`、重启电脑均复现）。CI 的 windows-latest 用相同 MSBuild 路径却能构建成功 → 结论：**本机 VS 安装状态损坏，与代码无关**。

- 修复计划：VS Installer 修复（`vs_installer.exe modify --installPath "C:\Program Files\Microsoft Visual Studio\18\Community" --passive --norestart`），无效再彻底重装（`uninstall` + `install --add Microsoft.VisualStudio.Workload.NativeDesktop`）。
- 修复后验证：`netune` 构建通过 + `netune_test/` 回归（test_partial / test_stream / test_cache）。
- 注意：排查中删除了 `build/_deps/ftxui-build` 与 `ftxui-subbuild`，重建时 FTXUI 会重新构建（正常，耗时略增）。
