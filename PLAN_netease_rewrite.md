## 根本问题

当前 `playback_coordinator` 里硬编码了 netease 专用逻辑（popen、FIFO、curl），**违背架构文档**——播放协调器不应该知道音乐源的具体实现。再加上 `playlist_manager` 和 UI playlist 的双轨数据源，导致永远不同步。

参照 lmusic1 和架构文档，重做分两步：

---

### Step 1: 清除 `playback_coordinator` 的 netease 专有代码

删掉所有 `popen("netease-cli ...")`、`mkfifo`、`fork(curl)` 逻辑。

播放协调器只做一件新增事：遇到非本地路径时，**通过 MusicSource 接口获取 stream URL**，然后用 libcurl 的 write callback 流式写入 decoupler（匿名 pipe），decoder 从 pipe 读。

```
CMD_PLAY → is_local? 
  ├─ 是 → decoder_open(filepath) → 正常解码
  └─ 否 → music_source_get("netease")->get_play_url(id, &url)
          → pipe() 创建匿名管道
          → fork + curl_execl 写 pipe write端
          → decoder_open_pipe(pipe read端) → 流式解码
```

### Step 2: 切断 `playlist_manager` 与网易云的关联

lmusic1 用单一 `playlist[]` 数组处理所有歌——本地和网易云都在同一数组里。绝对不会有不同步问题。

Netune 当前有 `playlist_manager`（存路径）和 `StateStore.playlist`（存完整 SongInfo），两边需要手动 sync。修多少轮都有竞态。

**改法**：网易云播放下，next/prev/auto-advance **完全绕开 `playlist_manager`**，直接操作 `StateStore.playlist`：

```cpp
// ev_playback_finish — 直接用 UI playlist 算下一首
int idx = st.selected_index;
int total = st.playlist.size();
int next = (st.loop_mode == LoopMode::Track) ? idx :
           (idx + 1 < total) ? idx + 1 :
           (st.loop_mode == LoopMode::Playlist) ? 0 : -1;

if (next >= 0) {
    path = st.playlist[next].id;  // 直接从 UI 取，不和 playlist_manager 交互
    publish EV_PLAYBACK_START;
}
```

`playlist_manager` 只用于本地歌（那里它本来就工作正确）。网易云赛道独立。

---

## 实施步骤

| # | 任务 | 文件 |
|---|------|------|
| 1 | 删 playback_coordinator 的 popen/FIFO/curl，改用 MusicSource 接口 + pipe() + fork(curl) | playback_coordinator.c |
| 2 | 重写 ev_playback_finish，不依赖 playlist_manager | app.cpp |
| 3 | 重写 NextTrack/PrevTrack，不依赖 playlist_manager | app.cpp |
| 4 | 保持 mp3_decoder 的自定义 I/O（pipe 兼容） | 不动 |
| 5 | 保持 netease_api.c / netease_source.c | 不动 |
| 6 | 删除 playlist_manager_sync 残余调用 | app.cpp |
| 7 | 编译验证 | — |

要开始吗？
