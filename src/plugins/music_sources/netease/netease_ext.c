/* netease_ext.c — 网易云特化接口实现：把 netease_api 的函数包成
 * NeteaseExt 函数表，应用层通过 netease_ext() 取用，不直接依赖
 * netease_api.h。 */
#include "netease_ext.h"
#include "netease_api.h"

static const NeteaseExt g_ext = {
    .account_name       = netease_account_name,
    .is_logged_in       = netease_is_logged_in,
    .login_refresh      = netease_login_refresh,
    .logout             = netease_logout,

    .qr_key             = netease_qr_key,
    .qr_render          = netease_qr_render,
    .qr_image           = netease_qr_image,
    .qr_poll            = netease_qr_poll,

    .search             = netease_search,
    .search_free        = netease_search_free,
    .search_playlists   = netease_search_playlists,
    .check_music        = netease_check_music,

    .playlists          = netease_playlists,
    .playlist_songs     = netease_playlist_songs,
    .liked_songs        = netease_liked_songs,
    .menu_songs         = netease_menu_songs,
    .daily_playlists    = netease_daily_playlists,
    .recent_songs       = netease_recent_songs,
    .toplist            = netease_toplist,
    .like_song          = netease_like_song,
    .liked_check        = netease_liked_check,
    .subscribe_playlist = netease_subscribe_playlist,
    .track_add          = netease_track_add,
    .track_remove       = netease_track_remove,
    .playlist_create    = netease_playlist_create,
    .playlist_rename    = netease_playlist_rename,
    .playlist_delete    = netease_playlist_delete,

    .song_detail        = netease_song_detail,
    .song_detail_free   = song_detail_free,

    .song_music_quality = netease_song_music_quality,
    .vip_level          = netease_vip_level,

    .song_owned         = netease_song_owned,
    .purchased_refresh  = netease_purchased_refresh,
    .is_purchased       = netease_is_purchased,
    .purchased_songs    = netease_purchased_songs,
    .purchased_albums   = netease_purchased_albums,
    .album_songs        = netease_album_songs,

    .download_song      = netease_download_song,
    .check_quality      = netease_check_quality,
    .download_url       = netease_download_url,

    .lyric              = netease_lyric,
};

const NeteaseExt *netease_ext(void) {
    return &g_ext;
}
