#include "ui/download_queue.h"

extern "C" {
#include "plugins/music_sources/netease/netease_ext.h"
#include "plugins/music_sources/local/local_source.h"
#include "core/event_bus.h"
#include "infra/config_paths.h"
#include "infra/log.h"
}
#include "ui/state_store.h"
#include <algorithm>
#include <cstdio>

#ifdef _WIN32
#define DL_PATH_SEP "\\"
#else
#define DL_PATH_SEP "/"
#endif

DownloadQueue& DownloadQueue::instance() {
    static DownloadQueue q;
    return q;
}

void DownloadQueue::progress_hook(void *ud, long long done, long long total) {
    const std::string *id = static_cast<const std::string *>(ud);
    DownloadQueue::instance().set_progress(
        *id,
        (uint64_t)(done  < 0 ? 0 : done),
        (uint64_t)(total < 0 ? 0 : total));
}

void DownloadQueue::notify() {
    event_bus_publish(EV_DOWNLOAD_UPDATE, NULL, 0);
}

void DownloadQueue::start() {
    std::lock_guard<std::mutex> lk(m_);
    if (started_) return;
    started_ = true;
    worker_ = std::thread(&DownloadQueue::worker_loop, this);
}

void DownloadQueue::stop() {
    {
        std::lock_guard<std::mutex> lk(m_);
        stop_ = true;
        cv_.notify_all();
    }
    if (worker_.joinable()) worker_.join();
}

void DownloadQueue::enqueue(const std::string &id, const std::string &title,
                            const std::string &artist,
                            const std::string &level) {
    {
        std::lock_guard<std::mutex> lk(m_);
        prune();
        items_.push_back(DlItem{id, title, artist, level, DlStatus::Queued,
                                0, 0});
        cv_.notify_one();
    }
    notify();
}

std::vector<DlItem> DownloadQueue::active() const {
    std::lock_guard<std::mutex> lk(m_);
    std::vector<DlItem> out;
    for (const auto &i : items_)
        if (i.status == DlStatus::Queued ||
            i.status == DlStatus::Downloading)
            out.push_back(i);
    return out;
}

void DownloadQueue::set_progress(const std::string &id, uint64_t done,
                                 uint64_t total) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lk(m_);
        for (auto &i : items_) {
            if (i.status == DlStatus::Downloading && i.id == id) {
                if (done > i.done) { i.done = done; changed = true; }
                if (total > 0)     { i.total = total; }
                break;
            }
        }
    }
    /* let the main thread refresh the StateStore mirror */
    if (changed) notify();
}

void DownloadQueue::prune() {
    /* The UI only shows active items; finished/errored ones are reported via
       the app notice, so drop them here to keep the deque bounded. */
    for (auto it = items_.begin(); it != items_.end(); ) {
        if (it->status == DlStatus::Done || it->status == DlStatus::Failed)
            it = items_.erase(it);
        else
            ++it;
    }
}

void DownloadQueue::worker_loop() {
    std::unique_lock<std::mutex> lk(m_);
    for (;;) {
        auto it = std::find_if(items_.begin(), items_.end(),
                               [](const DlItem &i) {
                                   return i.status == DlStatus::Queued;
                               });
        if (it == items_.end()) {
            if (stop_) return;
            cv_.wait(lk);
            continue;
        }
        DlItem cur = *it;
        it->status = DlStatus::Downloading;
        std::string cur_id = cur.id;   /* stable address for the curl callback */
        lk.unlock();
        notify();   /* reflect the queued→downloading flip in the mirror */

        char used_lvl[32] = {0};
        char *path = netease_ext()->download_song(
            cur_id.c_str(), cur.level.c_str(),
            cur.title.c_str(),
            cur.artist.empty() ? nullptr : cur.artist.c_str(),
            used_lvl, sizeof used_lvl, &DownloadQueue::progress_hook, &cur_id);

        bool ok = (path != nullptr);

        lk.lock();
        auto fi = std::find_if(
            items_.begin(), items_.end(),
            [&](const DlItem &i) {
                return i.id == cur_id && i.status == DlStatus::Downloading;
            });
        if (fi != items_.end())
            fi->status = ok ? DlStatus::Done : DlStatus::Failed;
        /* drop the just-finished item while still holding the lock — prune()
           mutates items_ and must never run unlocked against the main
           thread's enqueue()/active() (data race corrupted the deque and
           could surface as a std::system_error("Operation not permitted")
           from the mutex). */
        prune();
        lk.unlock();

        if (ok) {
            LOG_INFO("DOWNLOAD ok: %s (%s)", path,
                     used_lvl[0] ? used_lvl : cur.level.c_str());
            /* register the download dir into the local source so the file
               shows up in 本地, then refresh the local-music groups. */
            char dl_dir[1024];
            snprintf(dl_dir, sizeof dl_dir, "%s" DL_PATH_SEP "downloads",
                     netune_data_root());
            local_register_download_dir(dl_dir);
            event_bus_publish(EV_LOCAL_REFRESH, NULL, 0);
            std::string msg = "\u5DF2\u4E0B\u8F7D: " + cur.title; /* 已下载: */
            if (used_lvl[0]) msg += " (" + std::string(used_lvl) + ")";
            /* result notice rides the same update event; the main-thread
               bridge sets app_notice (StateStore is not thread-safe). */
            event_bus_publish(EV_DOWNLOAD_UPDATE, (void*)msg.c_str(),
                              msg.size() + 1);
            free(path);
        } else {
            LOG_WARN("DOWNLOAD failed: %s", cur_id.c_str());
            std::string msg = "\u4E0B\u8F7D\u5931\u8D25: " + cur.title; /* 下载失败: */
            event_bus_publish(EV_DOWNLOAD_UPDATE, (void*)msg.c_str(),
                              msg.size() + 1);
        }

        /* Re-acquire the lock before the next iteration. The loop body
           unlocks at line ~145 to run the download, so at this point lk
           does NOT own the mutex. If we fall through to find_if()/wait()
           without re-locking, the next cv_.wait(lk) (or a repeated
           lk.unlock()) is called on an unlocked unique_lock and libstdc++
           throws std::system_error("Operation not permitted") — uncaught,
           terminating the worker (observed as SIGABRT after a download
           finished). */
        lk.lock();
    }
}
