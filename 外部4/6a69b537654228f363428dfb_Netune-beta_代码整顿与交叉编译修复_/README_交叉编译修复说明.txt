================================================================
  Netune-beta 代码整顿 & 交叉编译修复 — 最终 diff 包
  生成日期: 2026-07-29
================================================================

一、修改文件清单 (26个diff)
────────────────────────────

【compat 兼容层 — 交叉编译修复】
  src_compat_utf8.h.diff                 — 守卫改_WRDE_NOCMD; 修复#else#endif合并; POSIX mkdir_utf8修正; TLS条件编译
  src_compat_dirent.h.diff               — 守卫 _MSC_VER → _WIN32
  src_compat_strings.h.diff              — 守卫 _MSC_VER → _WIN32
  src_compat_wcwidth_compat.h.diff       — 守卫 _MSC_VER → _WIN32
  src_compat_wordexp.h.diff              — 守卫 _MSC_VER → _WIN32
  src_compat_pthread.h_to_msvc_pthread.h.diff — 移至 compat/msvc/ 子目录

【构建系统】
  CMakeLists.txt.diff                    — compat/ 对所有WIN32添加; compat/msvc/ 仅MSVC

【C++ 源文件】
  src_app.cpp.diff                       — 补充<mutex>/<condition_variable>/<thread>
  src_ui_theme.cpp.diff                  — Windows UTF-8路径处理
  src_ui_state_store.cpp.diff            — 代码整顿

【核心 C 文件】
  src_core_cover.c.diff                  — 命令注入防护 + Windows兼容
  src_core_cache_manager.c.diff          — 目录创建Windows兼容
  src_core_playback_coordinator.c.diff   — 头文件清理/死代码删除
  src_core_music_source.c.diff           — 代码整顿
  src_core_audio_output_mgr.c.diff       — 代码整顿
  src_core_spectrum.c.diff               — 代码整顿

【基础设施】
  src_infra_path.c.diff                  — 路径展开简化
  src_infra_config.c.diff                — 代码整顿

【解码器】
  src_plugins_decoders_dr_flac_flac_decoder.c.diff  — Windows UTF-8路径
  src_plugins_decoders_dr_flac_wav_decoder.c.diff   — Windows UTF-8路径
  src_plugins_decoders_dr_mp3_mp3_decoder.c.diff    — Windows兼容
  src_plugins_decoders_ffmpeg_ffmpeg_stream.c.diff  — FFmpeg兼容

【音乐源】
  src_plugins_music_sources_local_local_source.c.diff   — 内存泄漏/目录遍历
  src_plugins_music_sources_netease_netease_api.c.diff  — 命令注入防护
  src_plugins_music_sources_netease_netease-cli_main.go.diff — Go代码整顿

【歌词】
  src_plugins_lyrics_lrc_lrc_parser.c.diff — UTF-8文件打开

二、交叉编译验证结果
────────────────────
  编译器: x86_64-w64-mingw32-gcc (C) / x86_64-w64-mingw32-g++-posix (C++)
  目标:   Windows x86_64
  结果:   37/37 文件全部通过 ✓
  排除:   ffmpeg_stream.c (需vcpkg提供的Windows FFmpeg头文件)

三、关键修复说明
────────────────

1. 兼容层守卫宏统一 (_MSC_VER → _WIN32)
   原代码仅MSVC定义_MSC_VER，MinGW-w64定义_WIN32但不定义_MSC_VER，
   导致MinGW-w64无法使用兼容实现。统一改为_WIN32后两者均可使用。

2. pthread.h 移至 compat/msvc/
   MinGW-w64 POSIX模型自带<pthread.h>，compat/pthread.h会覆盖它，
   导致C++标准库<std::mutex>/<thread>等无法编译。移至msvc/子目录后
   仅MSVC使用，MinGW-w64使用自带的pthread实现。

3. utf8.h 多处修复
   - #else#endif 合并错误 → 分为独立两行
   - POSIX分支 mkdir_utf8 从 _mkdir(p) 改为 mkdir((p), 0755)
   - 线程局部存储: MSVC用__declspec(thread), MinGW用__thread

4. CMakeLists.txt 构建配置
   - src/compat 对所有 WIN32 构建添加 (MSVC + MinGW-w64)
   - src/compat/msvc 仅对 MSVC 添加

5. app.cpp FTXUI头文件依赖
   FTXUI v6.0.0 内部使用 std::mutex/condition_variable/thread
   但未显式包含头文件，在MinGW-w64下暴露，已在FTXUI头文件前补充。

四、工具链要求
──────────────
  - Windows构建推荐使用 MinGW-w64 POSIX 线程模型 (g++-posix)
  - win32 线程模型不支持 C++11 <thread>/<mutex>/<condition_variable>
  - FFmpeg 需通过 vcpkg 安装: vcpkg install ffmpeg:x64-windows
  - 其他依赖: ftxui, yyjson, libyaml, SDL2
