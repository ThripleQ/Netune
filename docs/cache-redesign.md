# 缓存系统架构改造设计：异步预下载 + 本地播放

> 状态：待审阅（设计阶段，未实施）
> 关联文档：`HANDOFF.md`（现有架构约定）、`README.md`
> 目标分支：`beta`

## 1. 背景与动机

当前缓存系统采用"同步 tee"模型：播放线程通过 `ffstream_open_rec` / `ffstream_open_partial` 直接消费网络流，读多少就写多少缓存。下载进度与播放进度基本重合（最多领先 512KB 预取块加 FFmpeg 内部缓冲），下载与播放耦合在同一条 read 路径上。

这个模型带来四个实际痛点：

1. 网络抖动直接卡播放——播放线程阻塞在 `avio_read` 上，没有任何缓冲垫。
2. 断网立即卡死——没有"已下载部分继续播"的能力。
3. 下载不领先——无法利用空闲带宽提前把整曲拉下来。
4. seek 受限——只能在前缀内读盘，前缀外必须走网络，且与预取块交互时出现过缓存空洞（已修复）。

正经流媒体平台（Spotify、网易云、YouTube Music）普遍采用"异步预下载 + 本地播放"：下载线程独立领先写盘，播放器从本地文件读取，两者各自为战。本设计文档把 netune 的缓存系统改造为这一模型。

## 2. 现状架构

### 2.1 数据流

```
[网络流 URL] → ffstream_open_rec / ffstream_open_partial
                    │
                    ├─ tee_read：读网络 + 写 .part 缓存（同步）
                    │
                    └─ FFmpeg 解码 → ALSA 输出
```

### 2.2 关键模块

| 模块 | 职责 |
|---|---|
| `src/core/audio_cache.c/.h` | 缓存索引（`audio_cache.json`）+ LRU 容量管理；key = `song_id@quality` |
| `src/plugins/decoders/ffmpeg/ffmpeg_stream.c/.h` | 三种开流：`ffstream_open`（纯网络）、`ffstream_open_rec`（网络+录 `.part`）、`ffstream_open_partial`（前缀续播+Range 回填+512KB 预取） |
| `src/core/playback_coordinator.c` | 命中决策（完整→本地解码 / 部分→续播 / 未命中→录制）；EOF 提交完整缓存；切歌转正部分缓存 |
| `src/plugins/music_sources/netease/netease_quality.c` | 音质解析（override > 全局 > 源表验证 > 降级），决定缓存 tag 的 `quality` |

### 2.3 现有架构约定（HANDOFF.md）

- 播放线程是缓存唯一写者。
- 录制在探测（probe）完成后才开始。
- 探测越界读不落盘，探测后重绕。
- 缓存文件保持字节连续。
- 切歌时部分缓存转正为可续播缓存。

## 3. 目标架构

### 3.1 数据流

```
[网络流 URL]
     │
     ▼
[下载线程] ──写──▶ [.part 缓存文件] ◀──读── [播放线程] ──▶ [FFmpeg 解码] ──▶ [ALSA 输出]
     │                    │
     └── 维护"已下载水位"  └── 水位同步（条件变量）
```

### 3.2 核心原则

1. **下载与播放解耦**：下载线程独立拉流写盘，播放线程只读本地文件。
2. **下载领先**：下载进度远超播放进度，网络抖动由下载线程自行缓冲/重试，播放不受影响。
3. **断网续播**：已下载部分继续播放，下载线程进入重试/暂停状态。
4. **seek 由水位驱动**：跳到已下载区间立即播，跳到未下载区间等待水位推进。

## 4. 详细设计

### 4.1 新模块：`src/core/stream_downloader.c/.h`

独立的下载线程，负责把单个播放 URL 的流持续写入 `.part` 文件，并维护"已下载水位"。

#### 接口

```c
typedef struct stream_downloader stream_downloader_t;

/* 创建下载器并绑定目标文件与 URL。
   不立即开始；由 start() 启动工作线程。 */
stream_downloader_t* stream_downloader_create(const char *url,
                                              const char *part_path);

/* 启动工作线程（幂等）。 */
int stream_downloader_start(stream_downloader_t *d);

/* 已下载水位（字节数，线程安全读取）。 */
int64_t stream_downloader_watermark(stream_downloader_t *d);

/* 播放线程等待水位推进到 target 字节。
   timeout_ms 为 0 表示无限等待；返回 0 成功、-1 超时、-2 下载失败。 */
int stream_downloader_wait_watermark(stream_downloader_t *d,
                                     int64_t target, int timeout_ms);

/* 请求下载器跳到 offset 重新开始（seek 到未下载区间时使用）。 */
int stream_downloader_seek(stream_downloader_t *d, int64_t offset);

/* 停止工作线程并释放。 */
void stream_downloader_destroy(stream_downloader_t *d);
```

#### 内部状态

```c
struct stream_downloader {
    char *url;
    char *part_path;
    FILE *part;              /* 写句柄，仅工作线程访问 */
    int64_t watermark;       /* 已写入字节数，受 mutex 保护 */
    int64_t seek_target;     /* 待执行的 seek 请求 */
    int failed;              /* 下载失败标志 */
    int stop;
    pthread_t thread;
    pthread_mutex_t mtx;
    pthread_cond_t cv;       /* 水位推进 / seek 请求 / 停止 共用 */
};
```

#### 工作线程逻辑

```
loop:
  打开 URL（libcurl 或 FFmpeg http，复用现有网络栈）
  循环：
    读网络块（如 64KB）
    写 .part（追加）
    watermark += n；signal(cv)
    检查 seek_target：若有效，截断 .part 到 seek_target、重置 watermark、重新打开 URL（Range: offset-）
    检查 stop：退出
  网络错误：failed = 1；signal(cv)；进入重试等待
```

#### 复用现有资产

- 网络拉流：参考 `DownloadQueue::worker_loop` 的 libcurl 模式（`src/ui/download_queue.cpp`），或复用 `ffmpeg_stream` 的 http 打开逻辑。
- 进度回调：参考 `DownloadQueue::progress_hook`。
- 线程：可独立 `pthread`，或提交到现有 `thread_pool`（8 线程）。**建议独立线程**，因为下载器生命周期与播放绑定，且需要长连接 + 条件变量等待，不适合池化。

### 4.2 播放侧改造：本地文件播放 + 水位等待

播放线程不再直接读网络，改为：

1. 播放开始前，`stream_downloader_create` + `start`，并等待水位至少覆盖 probe 所需前缀（复用现有 probe 逻辑，但 probe 对象改为本地 `.part` 文件）。
2. 用 `ffstream_open(part_path, ...)` 打开本地 `.part` 文件播放（现有 `ffstream_open` 已支持本地文件路径）。
3. 解码循环中，`ffstream_read` 读到文件尾时：
   - 若下载器 `failed` → 播放失败（`EV_PLAYBACK_ERROR`）。
   - 否则调用 `stream_downloader_wait_watermark(pos + 1)` 等待水位推进，再继续读。
4. 播放位置推进时，可周期性检查水位，用于 UI 展示"已缓存 x%"。

### 4.3 seek 状态机

```
用户 seek 到 target 字节
  │
  ├─ target <= watermark ──▶ 本地文件 fseek(target)，立即播放
  │
  └─ target > watermark ──▶ stream_downloader_seek(target)
                            （下载器截断 .part、Range 重开、重置水位）
                            播放线程 wait_watermark(target) 后从本地读
```

要点：

- seek 到已下载区间是纯本地操作，零延迟。
- seek 到未下载区间时，下载器丢弃已下载内容重新拉取（与现有"seek 到前缀外走网络"语义一致，但现在是下载线程在做，播放线程不阻塞在网络 IO 上）。
- 下载器 seek 与播放线程读之间存在竞态：播放线程必须等待水位 >= target 才能读，由 `wait_watermark` 保证。
- **连续性取舍（已调研）**：seek 到未下载区间有两个选择——"截断重拉"（丢弃 seek 点前缓存、从新起点连续写）与"多段缓存"（保留多段、文件中间出现空洞）。调研结论：**多段缓存是主流播放器的成熟方案**。ExoPlayer（YouTube 的 Android 播放器）的 `CacheSpan` 就是多段缓存：缓存按段（offset+len）管理，seek 到未缓存区间返回一个 hole span（空洞），调用方持写锁从网络拉数据填洞、`commitFile` 提交、`releaseHoleSpan` 释放，支持并行写非重叠空洞。mpv（C 生态）走另一条路：换掉 FFmpeg I/O，用自定义 stream 层 + 内存环形缓冲（`demuxer-max-bytes`/`cache-secs`），seek 出缓冲即丢弃重拉。两者都验证了"自定义 AVIOContext 处理空洞/网络流"是 FFmpeg 官方标准做法（`avio_reading.c` 示例）。**本设计采用多段缓存作为目标方案**（借鉴 CacheSpan 设计），M1-M3 先落地"下载线程 + 本地播放 + 截断重拉"保证正确性，M5 升级为多段缓存；具体映射见 §4.6。

### 4.4 缓存提交与生命周期

- **下载完成**（下载器读到 EOF）：`audio_cache_commit(song_id, quality, complete=1)`，`.part` 重命名为正式缓存文件。现有提交逻辑复用。
- **切歌**：`stream_downloader_destroy` 停止下载，`.part` 保留为部分缓存（现有"切歌转正"逻辑复用）。
- **完整缓存命中**：不启动下载器，直接 `decoder_open` 或 `ffstream_open` 本地文件（含已修复的 m4a 回退路径）。
- **部分缓存命中**：下载器从 `Range: prefix_size-` 续传（复用现有 partial 续播语义，但由下载线程执行）。
- **部分缓存起点**：原架构部分缓存总是从 0 开始的连续前缀；新架构下，若用户 seek 到未下载区间后切歌，部分缓存从 `seek_target` 开始，不再是 0 起点。缓存索引需记录"起点 offset"，续播时播放器先 seek 到起点再读，否则会误以为缓存从 0 开始。`audio_cache` 索引需为此增加一个起点字段（默认 0 兼容现有缓存）。

### 4.5 线程同步模型

| 共享状态 | 保护 | 访问方 |
|---|---|---|
| `watermark` | `mtx` | 写：下载线程；读：播放线程 |
| `seek_target` | `mtx` | 写：播放线程；读：下载线程 |
| `failed` / `stop` | `mtx` | 写：下载线程；读：播放线程 |
| `.part` 文件 | 文件锁或"写追加 + 读已写区间"约定 | 写：下载线程；读：播放线程 |

竞态分析：

- **水位读 vs 文件写**：播放线程只在 `pos < watermark` 时读文件，保证读到的字节已写完（`fwrite` 返回后才推进 watermark）。
- **seek 竞态**：`seek_target` 设置后，下载线程在下一个循环迭代处理；处理期间暂停写，播放线程因 `wait_watermark` 阻塞，不会读到截断后的脏数据。
- **销毁竞态**：`destroy` 先置 `stop` 并 `signal(cv)`，`join` 工作线程，再释放资源。播放线程必须在 `destroy` 前停止读。

### 4.6 多段缓存设计（目标方案，借鉴 ExoPlayer CacheSpan）

多段缓存把"截断重拉"升级为"保留多段 + 空洞写锁"。核心是把 ExoPlayer 的 `CacheSpan` 概念映射到 netune：

| ExoPlayer 概念 | netune 映射 |
|---|---|
| `CacheSpan`（数据段：offset+len+文件） | 段数组 `{offset, len, file}`，每首歌一个段列表 |
| `startReadWrite(key, pos)` 命中 → `isCached=true` | 段索引查询：pos 落在某段内 → 播放线程直接读该段 |
| 未命中 → `isCached=false` 的 hole span（空洞） | 段索引查询：pos 落在空洞 → 返回空洞区间，下载线程持写锁填洞 |
| `commitFile` / `releaseHoleSpan` | 下载线程写完一段后合并进段列表、释放空洞锁 |
| 并行写非重叠空洞 | 下载线程 + 用户主动下载可同时填不同空洞 |
| `SimpleCache`（磁盘 + 索引 + LRU） | 复用现有 `audio_cache`，`AucEntry` 从单文件改为段列表 |

**播放侧（核心难点）**：FFmpeg 读带空洞的文件会误判 EOF/损坏。解决：自定义 `AVIOContext`，`read_packet` 回调里查段索引——pos 在段内读文件、pos 在空洞则阻塞等待下载线程填洞（`wait_watermark` 语义），`seek` 回调查段索引定位。这是 FFmpeg 官方支持的标准做法（`avio_reading.c` 示例），mpv 也验证了替换 FFmpeg I/O 的可行性。

**段管理**：相邻段自动合并（防止段数膨胀）；空洞写锁保证同一空洞只有一个写者；LRU 淘汰按整首歌粒度（段属于同一首歌，一起淘汰）。

**索引兼容**：现有 `audio_cache.json` 的 `complete` 字段保留；段列表作为新字段，缺省时按"单段从 0 到 size"解释，兼容旧缓存。

## 5. 与现有代码的集成点

| 文件 | 改动 |
|---|---|
| `src/core/stream_downloader.c/.h` | **新增**：下载线程实现 |
| `src/plugins/decoders/ffmpeg/ffmpeg_stream.c/.h` | 保留 `ffstream_open`（本地文件播放）；`ffstream_open_rec` / `ffstream_open_partial` 的录制/预取路径逐步退役 |
| `src/core/playback_coordinator.c` | `open_stream` 命中决策改为：完整→本地解码；部分/未命中→启动下载器 + 本地播放；EOF 提交逻辑保留 |
| `src/core/audio_cache.c/.h` | 索引/commit/touch 不变；可能新增"部分缓存续传起点"查询 |
| `src/ui/download_queue.cpp` | 参考其 libcurl worker 模式；不直接复用（职责不同） |
| `CMakeLists.txt` | 新增 `stream_downloader.c` |
| `HANDOFF.md` | 更新"播放线程是缓存唯一写者"等不变量（见 §7） |

## 6. 迁移步骤（里程碑）

| 里程碑 | 内容 | 验收 |
|---|---|---|
| M1 | 下载线程 + 本地播放（未命中场景：下载器写 .part，播放线程读本地） | 新歌可播放，下载进度领先播放，网络抖动不卡播放 |
| M2 | 部分缓存续传（下载器 Range 续传）+ seek 协调 | 部分缓存续播正常，seek 已下载区间零延迟，未下载区间等水位 |
| M3 | 断网恢复（下载器重试/暂停，已下载部分续播） | 断网后已下载部分播完，恢复后继续下载 |
| M4 | 退役 `ffstream_open_rec` / `ffstream_open_partial` 的录制/预取路径 | 旧路径无调用方，代码删除 |
| M5 | 多段缓存（段索引 + 空洞写锁 + 自定义 AVIOContext） | seek 到未下载区间保留已有缓存，空洞由下载线程填洞，FFmpeg 正常解码 |

每个里程碑独立可验证，M1 落地即解决"下载与播放各自为战"的核心诉求；M5 落地多段缓存（借鉴 ExoPlayer CacheSpan），解决"seek 丢弃缓存"的剩余痛点。

## 7. 架构约定更新

- **"播放线程是缓存唯一写者" → "下载线程是缓存唯一写者"**：`g_rec_song` / `g_rec_level` 等线程局部状态迁移到下载线程上下文。
- **"录制在探测完成后开始"**：改为"下载器在探测所需前缀就绪后开始"，语义等价。
- **"缓存文件保持字节连续"**：由下载器单写者追加保证，天然满足。原架构的空洞风险源于播放线程既读又写、seek 时读写位置错位；新架构下载线程独占写句柄、播放线程只读 `pos < watermark` 区间，读写位置错位在结构上不可能发生。
- **新增约定**："播放线程只读 `pos < watermark` 区间"、"seek 未下载区间时下载器截断重拉（保持连续，丢弃 seek 点前缓存）"、"部分缓存记录起点 offset（默认 0）"。

## 8. 风险与缓解

| 风险 | 影响 | 缓解 |
|---|---|---|
| 磁盘 IO 竞争（下载写 + 播放读同一文件） | 播放卡顿 | 写追加 + 读已写区间，操作系统页缓存吸收；必要时 `posix_fadvise` 顺序读 |
| 水位同步死锁 | 播放永久等待 | `wait_watermark` 必须带超时；下载失败置 `failed` 唤醒 |
| 下载器 seek 与播放读竞态 | 读到脏数据 | 播放线程严格 `wait_watermark(target)` 后才读 |
| 缓存文件损坏（下载中断） | 下次续播错位 | 复用现有"EOF 校验 + 部分缓存转正"逻辑；下载器异常退出时 `.part` 标记为不可续播 |
| 内存/句柄泄漏 | 长会话退化 | `destroy` 保证 `join` 后释放；播放结束路径统一清理 |

## 9. 验证方案

### 9.1 单元/集成测试（`netune_test`）

- 下载器：水位单调递增、seek 后水位重置、失败置位。
- 水位等待：target <= 当前水位立即返回；target > 水位阻塞；超时返回 -1。
- 本地播放：`.part` 文件可被 `ffstream_open` 正常解码。

### 9.2 手动验证清单

- 新歌播放：下载进度条领先播放进度。
- 限速网络（如 `tc netem`）：播放不卡顿，下载线程缓冲。
- 断网：已下载部分播完，恢复后继续。
- seek：已下载区间立即响应；未下载区间等待后播放。
- 切歌：部分缓存保留，下次续播。
- 完整缓存命中：不启动下载器，本地直接播。

## 10. 待确认问题

1. **预下载策略**：整曲下载，还是"领先播放 N 秒/固定水位差"？前者缓存完整度最高，后者省流量。建议默认整曲下载（与"缓存"目标一致），可配置领先阈值。
2. **并发下载上限**：当前 `DownloadQueue` 是串行。播放缓存下载器与用户主动下载是否共享带宽/并发限制？建议播放缓存下载器优先级最高。
3. **缓存淘汰**：LRU 容量策略是否调整（下载器可能快速填满配额）？建议保持现有 LRU，但下载器写入时遵守配额。
4. **m4a 缓存**：已修复的"完整缓存 m4a 回退 FFmpeg"路径在下载器模型下是否保留？建议保留（下载器也可能下到 m4a）。
