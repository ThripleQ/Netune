#include "event_bus.h"
#include "infra/log.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

/* ── Subscriber ───────────────────────────────────── */
#define MAX_SUBSCRIBERS_PER_TYPE 16

typedef struct {
    EventCallback callback;
    void        *user_data;
} Subscriber;

typedef struct {
    Subscriber subs[MAX_SUBSCRIBERS_PER_TYPE];
    int        count;
} SubscriberList;

/* ── Event queue node ─────────────────────────────── */
typedef struct EventNode {
    BusEvent         event;
    struct EventNode *next;
} EventNode;

/* ── Global state ──────────────────────────────────── */
static SubscriberList g_subscribers[EV_COUNT];
static pthread_mutex_t g_sub_mutex = PTHREAD_MUTEX_INITIALIZER;

static EventNode      *g_queue_head = NULL;
static EventNode      *g_queue_tail = NULL;
static pthread_mutex_t  g_queue_mutex = PTHREAD_MUTEX_INITIALIZER;

static volatile bool g_running = false;

/* ── Init / Shutdown ──────────────────────────────── */
int event_bus_init(void) {
    memset(g_subscribers, 0, sizeof(g_subscribers));
    g_queue_head = NULL;
    g_queue_tail = NULL;
    g_running = true;
    LOG_INFO("Event bus initialized");
    return 0;
}

void event_bus_shutdown(void) {
    g_running = false;

    /* drain queue */
    pthread_mutex_lock(&g_queue_mutex);
    EventNode *node = g_queue_head;
    while (node) {
        EventNode *next = node->next;
        free(node->event.data);
        free(node);
        node = next;
    }
    g_queue_head = g_queue_tail = NULL;
    pthread_mutex_unlock(&g_queue_mutex);

    LOG_INFO("Event bus shutdown");
}

/* ── Subscribe ────────────────────────────────────── */
int event_bus_subscribe(EventType type, EventCallback cb, void *user_data) {
    if (type >= EV_COUNT || !cb) return -1;

    pthread_mutex_lock(&g_sub_mutex);
    SubscriberList *list = &g_subscribers[type];
    if (list->count >= MAX_SUBSCRIBERS_PER_TYPE) {
        pthread_mutex_unlock(&g_sub_mutex);
        LOG_ERROR("Too many subscribers for event type %d", type);
        return -1;
    }
    list->subs[list->count].callback  = cb;
    list->subs[list->count].user_data = user_data;
    list->count++;
    pthread_mutex_unlock(&g_sub_mutex);

    LOG_DEBUG("Subscribed to event type %d (total: %d)", type, list->count);
    return 0;
}

/* ── Publish (thread-safe, non-blocking) ──────────── */
int event_bus_publish(EventType type, void *data, size_t data_size) {
    if (type >= EV_COUNT) return -1;
    if (!g_running) return -1;

    EventNode *node = (EventNode*)calloc(1, sizeof(EventNode));
    if (!node) return -1;

    node->event.type = type;
    node->event.data_size = data_size;
    node->event.ref_count = 0;
    if (data && data_size > 0) {
        node->event.data = malloc(data_size);
        if (!node->event.data) {
            free(node);
            return -1;
        }
        memcpy(node->event.data, data, data_size);
    } else {
        node->event.data = NULL;
    }

    pthread_mutex_lock(&g_queue_mutex);
    if (!g_queue_tail) {
        g_queue_head = g_queue_tail = node;
    } else {
        g_queue_tail->next = node;
        g_queue_tail = node;
    }
    pthread_mutex_unlock(&g_queue_mutex);

    return 0;
}

/* ── Poll (call from main thread) ──────────────────── */
void event_bus_poll(void) {
    /* dequeue all pending events under lock */
    EventNode *pending = NULL;

    pthread_mutex_lock(&g_queue_mutex);
    pending = g_queue_head;
    g_queue_head = g_queue_tail = NULL;
    pthread_mutex_unlock(&g_queue_mutex);

    /* dispatch outside lock */
    while (pending) {
        EventNode *node = pending;
        pending = node->next;

        EventType type = node->event.type;

        pthread_mutex_lock(&g_sub_mutex);
        SubscriberList *list = &g_subscribers[type];
        for (int i = 0; i < list->count; i++) {
            list->subs[i].callback(&node->event, list->subs[i].user_data);
        }
        pthread_mutex_unlock(&g_sub_mutex);

        free(node->event.data);
        free(node);
    }
}
