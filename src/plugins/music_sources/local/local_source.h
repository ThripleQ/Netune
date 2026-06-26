#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "core/music_source.h"

/* Register the local file music source plugin */
void local_source_register(void);
MusicSource* local_source_create(void);

#ifdef __cplusplus
}
#endif
