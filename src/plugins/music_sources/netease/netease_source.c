#include "netease_source.h"
#include "netease_api.h"
#include "core/music_source_manager.h"
#include "infra/log.h"
#include <string.h>
#include <stdlib.h>

static int ns_init(void) {
    if (netease_init() != 0) return -1;
    LOG_INFO("netease source initialized"); return 0;
}
static void ns_shutdown(void) { netease_shutdown(); }
static int ns_search(const char *kw, int p, int ps, SearchResult *out) {
    memset(out,0,sizeof(*out));if(!kw)return-1;int l=ps>0?ps:20,o=p>0?p*l:0;
    NSSearchResult nr;if(netease_search(kw,l,o,&nr)!=0||nr.count<=0)return-1;
    out->songs=calloc((size_t)nr.count,sizeof(SongInfo));out->count=nr.count;out->total=nr.count;
    for(int i=0;i<nr.count;i++){SongInfo*s=&out->songs[i];s->id=strdup(nr.songs[i].id);s->source=strdup("netease");s->title=strdup(nr.songs[i].title?nr.songs[i].title:"");s->artist=strdup(nr.songs[i].artist?nr.songs[i].artist:"");s->album=strdup(nr.songs[i].album?nr.songs[i].album:"");s->duration_sec=nr.songs[i].dur_ms/1000;s->cover_url=strdup("");s->aux_label=strdup("");}
    netease_search_free(&nr);return 0;
}
static int ns_detail(const char *id, SongInfo *out) {memset(out,0,sizeof(*out));out->id=strdup(id);out->source=strdup("netease");out->title=strdup("");out->artist=strdup("");out->album=strdup("");out->cover_url=strdup("");out->aux_label=strdup("");return 0;}
static int ns_url(const char *id, int q, char *url, size_t sz) {return netease_play_url(id,url,sz);}
static int ns_lyric(const char *id, char *b, size_t sz) {(void)id;if(sz)b[0]=0;return-1;}
static int ns_cover(const char *id, char *b, size_t sz) {(void)id;if(sz)b[0]=0;return-1;}
static bool ns_avail(void) {return true;}

static MusicSource g_ns = {.name="netease",.priority=20,.init=ns_init,.shutdown=ns_shutdown,.search=ns_search,.get_song_detail=ns_detail,.get_play_url=ns_url,.get_lyric=ns_lyric,.get_cover_url=ns_cover,.is_available=ns_avail};

void netease_source_register(void) {music_source_register(&g_ns);}
MusicSource* netease_source_create(void) {return &g_ns;}
