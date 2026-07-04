#include "netease_api.h"
#include "infra/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <ctype.h>

#define CLI "netease-cli"
static char g_name[128] = "";

/* ── popen helper ─────────────────────────────────── */
static char *run(const char *fmt, ...) {
    char cmd[2048]; va_list ap;
    va_start(ap, fmt); vsnprintf(cmd, sizeof(cmd), fmt, ap); va_end(ap);
    FILE *fp = popen(cmd, "r"); if (!fp) return NULL;
    size_t cap = 8192, len = 0; char *b = malloc(cap); if (!b) { pclose(fp); return NULL; }
    while (!feof(fp)) { if (len+1024>=cap) { cap*=2; char*t=realloc(b,cap); if(!t){free(b);pclose(fp);return NULL;} b=t; } size_t r=fread(b+len,1,cap-len-1,fp); if(r>0)len+=r; else break; }
    b[len]=0; pclose(fp); return b;
}
/* ── lightweight JSON helpers ─────────────────────── */
static char *jstr(const char *j, const char *k) {
    char s[128]; snprintf(s,sizeof(s),"\"%s\"",k);
    const char *p = strstr(j,s); if(!p)return NULL; p+=strlen(s);
    while(*p==':'||*p==' '||*p=='\t'||*p=='\n')p++;
    if(*p=='"'){p++;size_t cap=512,w=0;char*o=malloc(cap);if(!o)return NULL;
        while(*p&&*p!='"'&&w<cap-1){
            if(*p=='\\'&&*(p+1)=='u'&&isxdigit((unsigned char)*(p+2))&&isxdigit((unsigned char)*(p+3))&&isxdigit((unsigned char)*(p+4))&&isxdigit((unsigned char)*(p+5))){
                char h[5]={*(p+2),*(p+3),*(p+4),*(p+5),0};unsigned long cp=strtoul(h,NULL,16);
                if(cp<0x80)o[w++]=(char)cp;else if(cp<0x800){o[w++]=(char)(0xC0|(cp>>6));o[w++]=(char)(0x80|(cp&0x3F));}else{o[w++]=(char)(0xE0|(cp>>12));o[w++]=(char)(0x80|((cp>>6)&0x3F));o[w++]=(char)(0x80|(cp&0x3F));}p+=6;
            }else o[w++]=*p++;
        }o[w]=0;return o;
    }if(isdigit((unsigned char)*p)||*p=='-'){const char*e=p;while(isdigit((unsigned char)*e)||*e=='.')e++;char*o=malloc((size_t)(e-p)+1);if(o){memcpy(o,p,(size_t)(e-p));o[e-p]=0;}return o;}
    return NULL;
}
static long long jint(const char *j, const char *k) {
    char s[128]; snprintf(s,sizeof(s),"\"%s\"",k);
    const char *p = strstr(j,s); if(!p)return 0; p+=strlen(s);
    while(*p==':'||*p==' '||*p=='\t'||*p=='\n')p++; return atoll(p);
}
static const char *jobj(const char *j, const char *k) {
    char s[128]; snprintf(s,sizeof(s),"\"%s\"",k);
    const char *p = strstr(j,s); if(!p)return NULL; p+=strlen(s);
    while(*p&&*p!='{'&&*p!='[')p++; return (*p=='{'||*p=='[')?p:NULL;
}
static const char *jmatch(const char *o) {
    char oc=*o,cc=(oc=='{')?'}':']';int d=1;const char*p=o+1;
    while(*p&&d>0){if(*p=='"'){p++;while(*p&&*p!='"'){if(*p=='\\')p++;p++;}}else if(*p==oc)d++;else if(*p==cc)d--;p++;}return p;
}
/* ── parse one song ────────────────────────────────── */
static void fill(SongInfo *s, const char *jsn) {
    memset(s,0,sizeof(*s)); s->source=strdup("netease"); s->cover_url=strdup(""); s->aux_label=strdup("");
    char *id=jstr(jsn,"id"); s->id=id?id:strdup("");
    char *t=jstr(jsn,"name"); s->title=t?t:strdup("");
    const char *ar=jobj(jsn,"ar");if(ar){const char*ap=ar+1;while(*ap&&*ap!='{')ap++;if(*ap=='{'){char*an=jstr(ap,"name");s->artist=an?an:strdup("");}else s->artist=strdup("");}else s->artist=strdup("");
    s->duration_sec=(int)(jint(jsn,"dt")/1000);
}
/* ── parse songs array into SongInfo* ──────────────── */
static int parselist(const char *json, const char *loc __attribute__((unused)), SongInfo **out, int *cnt) {
    *out=NULL; *cnt=0; if(!json)return -1;
    const char *s=jobj(json,"songs");if(!s||*s!='['){const char*r=jobj(json,"result");if(r)s=jobj(r,"songs");}
    if(!s||*s!='[')return -1;
    int n=0;const char*p=s+1;while(*p){while(*p&&*p!='{'&&*p!=']')p++;if(*p==']')break;p=jmatch(p);n++;}
    if(!n)return -1;
    *out=(SongInfo*)calloc((size_t)n,sizeof(SongInfo));*cnt=0;
    p=s+1;int oi=0;while(*p){while(*p&&*p!='{'&&*p!=']')p++;if(*p==']')break;
        const char*e=jmatch(p);fill(&(*out)[oi],p);oi++;p=e;}
    *cnt=oi;return oi>0?0:-1;
}

/* ── Init ──────────────────────────────────────────── */
int netease_init(void) {
    char *n=run("%s account-name 2>/dev/null",CLI);
    if(!n){LOG_WARN("netease-cli not found");return -1;}
    if(n[0]&&strcmp(n,"未登录\n")!=0&&strcmp(n,"error\n")!=0){size_t l=strlen(n);if(l>0&&n[l-1]=='\n')n[l-1]=0;snprintf(g_name,sizeof(g_name),"%s",n);}
    free(n);LOG_INFO("netease ready");return 0;
}
void netease_shutdown(void) {}
const char* netease_account_name(void) { return g_name[0]?g_name:NULL; }

/* ── Search ────────────────────────────────────────── */
int netease_search(const char *kw, int l, int o __attribute__((unused)), NSSearchResult *out) {
    memset(out,0,sizeof(*out)); if(!kw)return -1;
    char *j=run("%s search \"%s\" 2>/dev/null",CLI,kw); if(!j)return -1;
    const char *s=jobj(jobj(j,"result")?jobj(j,"result"):j,"songs");
    if(!s||*s!='['){free(j);return 0;}
    int n=0,max=l>0?l:30;const char*p=s+1;while(*p){while(*p&&*p!='{'&&*p!=']')p++;if(*p==']')break;p=jmatch(p);n++;if(n>=max)break;}
    if(n==0){free(j);return 0;} out->songs=calloc((size_t)n,sizeof(NSSong)); out->count=n;
    int oi=0;p=s+1;while(*p&&oi<n){while(*p&&*p!='{'&&*p!=']')p++;if(*p==']')break;const char*e=jmatch(p);
        NSSong *r=&out->songs[oi]; r->id=jstr(p,"id");r->title=jstr(p,"name");r->artist=jstr(p,"artist");
        if(!r->artist){const char*a=jobj(p,"ar");if(a){const char*ap=a+1;while(*ap&&*ap!='{')ap++;if(*ap=='{')r->artist=jstr(ap,"name");}}if(!r->artist)r->artist=strdup("");
        r->album=jstr(p,"album");if(!r->album){const char*a=jobj(p,"al");if(a)r->album=jstr(a,"name");}if(!r->album)r->album=strdup("");
        r->dur_ms=(int)jint(p,"dt");oi++;p=e;}
    out->count=oi; free(j); return 0;
}
void netease_search_free(NSSearchResult *r) { if(!r)return;for(int i=0;i<r->count;i++){free(r->songs[i].id);free(r->songs[i].title);free(r->songs[i].artist);free(r->songs[i].album);}free(r->songs);r->songs=NULL;r->count=0;}

/* ── Login QR ─────────────────────────────────────── */
int netease_qr_key(char *u, size_t usz, char *url, size_t usz2) {
    char *j=run("%s qr-key 2>/dev/null",CLI);if(!j)return -1;
    char *uk=jstr(j,"unikey"),*ul=jstr(j,"url"); int r=-1;
    if(uk&&ul&&uk[0]&&ul[0]){snprintf(u,usz,"%s",uk);snprintf(url,usz2,"%s",ul);r=0;}
    free(uk);free(ul);free(j);return r;
}
char* netease_qr_render(const char *url) { return run("%s qr-render \"%s\" 2>/dev/null",CLI,url); }
int netease_qr_poll(const char *uk) {
    char *j=run("%s qr-check \"%s\" 2>/dev/null",CLI,uk);if(!j)return -1;
    long long c=jint(j,"code");free(j);
    if(c==803){char*n=run("%s account-name 2>/dev/null",CLI);if(n){size_t l=strlen(n);if(l>0&&n[l-1]=='\n')n[l-1]=0;if(strcmp(n,"error")!=0&&strcmp(n,"未登录")!=0)snprintf(g_name,sizeof(g_name),"%s",n);free(n);}return 0;}
    if(c==800)return 2;
    if(c==802)return 3; return 1;
}
bool netease_is_logged_in(void) { return g_name[0]!=0; }

/* ── Playlists ────────────────────────────────────── */
int netease_playlists(SongInfo **out, int *count) {
    char *j=run("%s playlists 2>/dev/null",CLI);if(!j)return -1;
    const char *pl=jobj(j,"playlists");if(!pl||*pl!='['){free(j);return -1;}
    int n=0;const char*p=pl+1;while(*p){while(*p&&*p!='{'&&*p!=']')p++;if(*p==']')break;p=jmatch(p);n++;}
    if(!n){free(j);return -1;}
    *out=calloc((size_t)n,sizeof(SongInfo));*count=0;
    p=pl+1;int oi=0;while(*p){while(*p&&*p!='{'&&*p!=']')p++;if(*p==']')break;const char*e=jmatch(p);
        SongInfo *s=&(*out)[oi];memset(s,0,sizeof(*s));s->source=strdup("netease");s->cover_url=strdup("");s->aux_label=strdup("歌单");
        char *id=jstr(p,"id");s->id=id?id:strdup("");
        s->title=jstr(p,"name");if(!s->title)s->title=strdup("");
        oi++;p=e;}
    *count=oi;free(j);return 0;
}
int netease_playlist_songs(const char *id, SongInfo **out, int *count) {
    char *j=run("%s playlist-tracks \"%s\" 2>/dev/null",CLI,id);if(!j)return -1;
    int r=parselist(j,"songs",out,count);free(j);return r;
}
int netease_liked_songs(SongInfo **out, int *count) {
    char *j=run("%s liked 2>/dev/null",CLI);if(!j)return -1;
    int r=parselist(j,"songs",out,count);free(j);return r;
}
int netease_menu_songs(int type, int limit __attribute__((unused)), SongInfo **out, int *count) {
    if(type==0){char*j=run("%s recommend-songs 2>/dev/null",CLI);if(!j)return -1;int r=parselist(j,"songs",out,count);free(j);return r;}
    return -1;
}

/* ── Play URL ──────────────────────────────────────── */
int netease_play_url(const char *id, char *url, size_t sz) {
    const char *lvl="standard";
    char *j=run("%s song-url \"%s\" %s 2>/dev/null",CLI,id,lvl);if(!j)return -1;
    const char *d=jobj(j,"data");int r=-1;
    if(d&&*d=='['){const char*p=d+1;while(*p&&*p!='{')p++;if(*p=='{'){char*u=jstr(p,"url");if(u&&u[0]){snprintf(url,sz,"%s",u);r=0;}free(u);}}
    free(j);if(r!=0&&sz>0)url[0]=0;return r;
}
char* netease_download(const char *id, const char *url) {
    char path[256];snprintf(path,sizeof(path),"/tmp/netune_%s.mp3",id);
    unlink(path);
    char cmd[3072];snprintf(cmd,sizeof(cmd),"curl -sL --max-time 60 \"%s\" -o \"%s\"",url,path);
    int rc=system(cmd);
    if(rc!=0){unlink(path);return NULL;}
    char *out=strdup(path);return out;
}
