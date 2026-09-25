/* test_netease_map.c — NSSong → SongInfo mapping (netease search rows).
 *
 * Regression test for the "搜索网易云 结果没有封面" bug: the app's
 * do_netease_search() hand-rolled the field copy and dropped cover_url, so
 * songs that only ever came through that path had no artwork (list rows and
 * the lyric-page cover both stayed blank). The mapping now lives in one
 * place — ne_song_from_ns() (plugins/music_sources/netease/netease_ext.h) —
 * and this test pins every field, plus the NULL-safety the UI relies on
 * (title/artist/cover_url are never NULL) and the shallow→deep copy that
 * carries the metadata into the playlist / current_song. */
#include "test_util.h"
#include "plugins/music_sources/netease/netease_ext.h"
#include "core/music_source.h"

#include <stdlib.h>
#include <string.h>

/* A row exactly like netease-cli's search payload produces. */
static NSSong make_row(void) {
    NSSong ns;
    memset(&ns, 0, sizeof ns);
    ns.id        = "2651425710";
    ns.title     = "稻香(深情版)";
    ns.artist    = "Lucky小爱";
    ns.album     = "稻香(深情版)";
    ns.cover_url = "http://p1.music.126.net/YSc5EpQhsJTiFgoexUnHUA==/109951173982244370.jpg";
    ns.dur_ms    = 231609;
    ns.fee       = 8;
    return ns;
}

int main(void) {
    int ok = 1;

    /* ── full row: every field survives, cover_url included ── */
    NSSong ns = make_row();
    SongInfo si;
    ne_song_from_ns(&si, &ns);

    NT_EXPECT(si.id && strcmp(si.id, "2651425710") == 0, "id: %s", si.id ? si.id : "(null)");
    NT_EXPECT(si.source && strcmp(si.source, "netease") == 0, "source: %s", si.source ? si.source : "(null)");
    NT_EXPECT(si.title && strcmp(si.title, "稻香(深情版)") == 0, "title: %s", si.title ? si.title : "(null)");
    NT_EXPECT(si.artist && strcmp(si.artist, "Lucky小爱") == 0, "artist: %s", si.artist ? si.artist : "(null)");
    NT_EXPECT(si.album && strcmp(si.album, "稻香(深情版)") == 0, "album: %s", si.album ? si.album : "(null)");
    /* the bug: a non-empty cover_url on the wire must reach SongInfo */
    NT_EXPECT(si.cover_url && strcmp(si.cover_url, ns.cover_url) == 0,
              "cover_url: %s", si.cover_url ? si.cover_url : "(null)");
    NT_EXPECT(si.duration_sec == 231, "duration_sec: %d", si.duration_sec);
    NT_EXPECT(si.fee == 8, "fee: %d", si.fee);
    NT_EXPECT(si.aux_label && si.aux_label[0] == '\0', "aux_label should be empty, got %s",
              si.aux_label ? si.aux_label : "(null)");
    NT_EXPECT(si.is_playlist == 0, "is_playlist should be 0");

    /* ── the copy the UI performs (playlist / current_song) keeps it ── */
    SongInfo copied;
    memset(&copied, 0, sizeof copied);
    song_info_copy(&copied, &si);
    NT_CHECK(ok, copied.cover_url && strcmp(copied.cover_url, ns.cover_url) == 0,
             "cover_url lost in song_info_copy: %s",
             copied.cover_url ? copied.cover_url : "(null)");
    song_info_free(&copied);
    song_info_free(&si);

    /* ── missing cover (older API rows): empty, never NULL ── */
    NSSong no_cover = make_row();
    no_cover.cover_url = NULL;
    SongInfo si2;
    ne_song_from_ns(&si2, &no_cover);
    NT_CHECK(ok, si2.cover_url && si2.cover_url[0] == '\0',
             "NULL cover_url should map to \"\", got %s",
             si2.cover_url ? si2.cover_url : "(null)");
    song_info_free(&si2);

    /* ── every string NULL (defensive): no crash, all empty ── */
    NSSong empty;
    memset(&empty, 0, sizeof empty);
    SongInfo si3;
    ne_song_from_ns(&si3, &empty);
    NT_CHECK(ok, si3.id && si3.title && si3.artist && si3.album &&
                 si3.cover_url && si3.aux_label,
             "NULL NSSong fields must still produce non-NULL strings");
    NT_CHECK(ok, si3.source && strcmp(si3.source, "netease") == 0,
             "source should still be netease");
    song_info_free(&si3);

    /* ── NULL arguments are ignored, not dereferenced ── */
    SongInfo si4;
    ne_song_from_ns(&si4, NULL);
    NT_CHECK(ok, si4.id == NULL && si4.cover_url == NULL,
             "src=NULL should leave dst zeroed");
    ne_song_from_ns(NULL, &ns);   /* must not crash */

    printf("%s test_netease_map\n", ok ? "PASS" : "FAIL");
    return ok ? NT_OK : NT_FAIL;
}
