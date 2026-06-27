#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "core/music_source.h"

/* Register the Netease Cloud Music source plugin */
void netease_source_register(void);
MusicSource* netease_source_create(void);

#ifdef __cplusplus
}
#endif
