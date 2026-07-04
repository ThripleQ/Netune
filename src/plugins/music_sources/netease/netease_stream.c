#include "netease_stream.h"
#include "netease_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>

static pid_t g_child = 0;
static char  g_fifo[1024] = "";

FILE *netease_stream_open(const char *song_id,
                          char *fifo_path_out, size_t path_size) {
    fprintf(stderr, "STREAM_OPEN: id=%s\n", song_id ? song_id : "null");
    if (!song_id || !song_id[0]) return NULL;

    /* 1. Resolve play URL */
    char url[2048] = {0};
    if (netease_get_play_url(song_id, 0, url, sizeof(url)) != 0 || !url[0]) {
        fprintf(stderr, "STREAM_OPEN: no play URL\n");
        return NULL;
    }

    /* 2. Create FIFO */
    snprintf(g_fifo, sizeof(g_fifo), "/tmp/netune_%s.mp3", song_id);
    unlink(g_fifo);
    if (mkfifo(g_fifo, 0600) != 0) {
        fprintf(stderr, "STREAM_OPEN: mkfifo failed for %s\n", g_fifo);
        return NULL;
    }

    /* 3. Fork curl */
    pid_t child = fork();
    if (child < 0) { unlink(g_fifo); return NULL; }
    if (child == 0) {
        execl("/usr/bin/curl", "curl", "-sL", url,
              "--max-time", "30", "-o", g_fifo, NULL);
        _exit(1);
    }
    g_child = child;

    /* 4. Return read end + fill path */
    if (fifo_path_out && path_size > 0)
        snprintf(fifo_path_out, path_size, "%s", g_fifo);

    FILE *f = fopen(g_fifo, "rb");
    fprintf(stderr, "STREAM_OPEN: fifo=%s fp=%p child=%d\n", g_fifo, (void*)f, child);
    return f;
}

void netease_stream_close(FILE *f) {
    fprintf(stderr, "STREAM_CLOSE: fp=%p child=%d fifo=%s\n", (void*)f, g_child, g_fifo);
    if (g_child > 0) {
        kill(g_child, SIGKILL);
        waitpid(g_child, NULL, 0);
        g_child = 0;
    }
    if (f) fclose(f);
    if (g_fifo[0]) {
        unlink(g_fifo);
        g_fifo[0] = '\0';
    }
}
