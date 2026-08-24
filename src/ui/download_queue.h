/* download_queue.h — serialised download queue with live progress.
 *
 * Downloads run one at a time on a dedicated worker thread; any extra
 * requests queue up behind the current one. Each item exposes a byte
 * count + percentage so the UI can render a progress bar / spinner.
 * Thread-safe: mutations take the internal mutex. The queue is a pure
 * backend — it publishes EV_DOWNLOAD_UPDATE on every state change and the
 * main thread mirrors the active snapshot into StateStore, so the UI only
 * ever reads StateStore, never this queue.
 */
#pragma once

#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ui/state_store.h"  /* DlStatus / DlItem */

class DownloadQueue {
public:
    static DownloadQueue& instance();

    void start();          /* spawn the worker thread (idempotent) */
    void stop();           /* signal + join the worker */
    void enqueue(const std::string &id, const std::string &title,
                 const std::string &artist, const std::string &level);

    /* queued + downloading items in enqueue order (const snapshot).
       Called by the main-thread bridge for the StateStore mirror. */
    std::vector<DlItem> active() const;

private:
    DownloadQueue() = default;
    ~DownloadQueue() = default;
    DownloadQueue(const DownloadQueue&) = delete;
    DownloadQueue& operator=(const DownloadQueue&) = delete;

    /* libcurl progress callback (C-callable) → set_progress() */
    static void progress_hook(void *ud, long long done, long long total);

    void worker_loop();
    void set_progress(const std::string &id, uint64_t done, uint64_t total);
    void prune();          /* drop finished/errored items */
    void notify();         /* publish EV_DOWNLOAD_UPDATE (signal the main thread) */

    mutable std::mutex m_;
    std::condition_variable cv_;
    std::deque<DlItem> items_;
    bool       started_ = false;
    bool       stop_    = false;
    std::thread worker_;
};