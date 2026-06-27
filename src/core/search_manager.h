#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "music_source.h"
#include "music_source_manager.h"
#include <stdbool.h>

/* ── Search manager ────────────────────────────────────
 * Wraps music_source_manager with state management:
 * - Current keyword and page tracking
 * - Cross-source aggregation + deduplication
 * - Event bus integration (EV_SEARCH_START/RESULT/ERROR)
 * - Caching of search results
 * ──────────────────────────────────────────────────── */

/* Maximum number of results per page */
#define SEARCH_PAGE_SIZE 20

/* ── API ────────────────────────────────────────────── */

/* Initialize the search manager */
int search_manager_init(void);

/* Execute a search. source_name can be NULL (all sources) or a specific
 * source like "local", "netease".
 * Returns 0 if search started (results delivered via event bus).
 * Keyword NULL or "" means "clear and return to previous mode". */
int search_manager_search(const char *keyword, int page);
int search_manager_search_source(const char *source_name,
                                  const char *keyword, int page);

/* Get the most recent results (borrowed pointer, do not free). */
const SearchResult* search_manager_results(void);

/* Get current search query state */
const char* search_manager_keyword(void);
int         search_manager_page(void);
int         search_manager_total_results(void);

/* Clear current search and free results. */
void search_manager_clear(void);

/* Shutdown */
void search_manager_shutdown(void);

#ifdef __cplusplus
}
#endif
