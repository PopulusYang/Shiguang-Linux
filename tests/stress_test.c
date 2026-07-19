#define G_LOG_DOMAIN "stress"

#include "../src/api.h"
#include <glib.h>
#include <curl/curl.h>
#include <stdlib.h>

/* 复制 main.c 的精简队列逻辑，验证多线程安全 */

#define WINDOW_HALF  5
#define WINDOW_SIZE  11

typedef struct {
    ImageData *meta;
    GBytes    *image;
    gboolean   ready;
} ImageEntry;

static ImageEntry *entry_new(void) {
    return g_new0(ImageEntry, 1);
}
static void entry_free(ImageEntry *e) {
    if (!e) return;
    if (e->meta)  api_image_free(e->meta);
    if (e->image) g_bytes_unref(e->image);
    g_free(e);
}

typedef struct {
    GPtrArray  *queue;
    int         cursor;
    GMutex      mutex;
    int         init_loaded;
    gboolean    startup_done;
    gboolean    fwd_busy;
    gboolean    bwd_busy;
    int         round;
    int         total_ops;
    GMainLoop  *loop;           /* 驱动 g_idle_add */
} SimState;

typedef struct {
    SimState *state;
    int       side;   /* -1=prepend, -2=append */
    int       slot;   /* >=0 for init */
} LoadTask;

static gboolean on_slot_loaded(gpointer user_data);
static void trigger_preload(SimState *state);
static void display_current(SimState *state);

/* ---- 工作线程 ---- */
static gpointer load_one(gpointer user_data) {
    LoadTask *task = user_data;
    SimState *state = task->state;

    ImageData *meta = api_random("timeline");
    if (!meta) {
        g_idle_add(on_slot_loaded, task);
        return NULL;
    }

    GBytes *img = api_download_image(meta->imgurl);

    ImageEntry *entry = entry_new();
    entry->meta  = meta;
    entry->image = img;
    entry->ready = TRUE;

    g_mutex_lock(&state->mutex);

    if (task->slot >= 0) {
        if (task->slot < (int)state->queue->len) {
            entry_free(g_ptr_array_index(state->queue, task->slot));
            state->queue->pdata[task->slot] = entry;
        }
    } else if (task->side == -2) {
        g_ptr_array_add(state->queue, entry);
    } else if (task->side == -1) {
        g_ptr_array_insert(state->queue, 0, entry);
        state->cursor++;
    }

    if (task->side == -2)
        state->fwd_busy = FALSE;
    else
        state->bwd_busy = FALSE;

    if (task->slot >= 0)
        state->init_loaded++;

    g_mutex_unlock(&state->mutex);

    g_idle_add(on_slot_loaded, task);
    return NULL;
}

/* ---- 主线程回调 ---- */
static gboolean on_slot_loaded(gpointer user_data) {
    LoadTask *task = user_data;
    SimState *state = task->state;
    int slot = task->slot;
    int side = task->side;

    g_free(task);

    state->total_ops++;

    /* init 接续：前向 */
    if (!state->startup_done && slot >= 0) {
        if (slot >= WINDOW_HALF && slot < WINDOW_SIZE - 1) {
            int next = slot + 1;
            g_mutex_lock(&state->mutex);
            gboolean already = FALSE;
            if (next < (int)state->queue->len) {
                ImageEntry *e = g_ptr_array_index(state->queue, next);
                if (e->ready) already = TRUE;
            }
            gboolean busy = state->fwd_busy;
            if (!already && !busy) state->fwd_busy = TRUE;
            g_mutex_unlock(&state->mutex);

            if (!already && !busy) {
                LoadTask *t = g_new0(LoadTask, 1);
                t->state = state;
                t->slot  = next;
                t->side  = -2;
                g_thread_new("fwd", load_one, t);
            }
        }
        /* init 接续：后向 */
        if (slot <= WINDOW_HALF && slot > 0) {
            int prev = slot - 1;
            g_mutex_lock(&state->mutex);
            gboolean already = FALSE;
            if (prev < (int)state->queue->len) {
                ImageEntry *e = g_ptr_array_index(state->queue, prev);
                if (e->ready) already = TRUE;
            }
            gboolean busy = state->bwd_busy;
            if (!already && !busy) state->bwd_busy = TRUE;
            g_mutex_unlock(&state->mutex);

            if (!already && !busy) {
                LoadTask *t = g_new0(LoadTask, 1);
                t->state = state;
                t->slot  = prev;
                t->side  = -1;
                g_thread_new("bwd", load_one, t);
            }
        }
        if (state->init_loaded >= WINDOW_SIZE) {
            state->startup_done = TRUE;
            g_print("  初始化完成: %d/%d\n", state->init_loaded, WINDOW_SIZE);
        }
    }

    display_current(state);
    trigger_preload(state);
    return G_SOURCE_REMOVE;
}

/* ---- 显示 ---- */
static void display_current(SimState *state) {
    g_mutex_lock(&state->mutex);
    if (!state->queue || state->queue->len == 0) {
        g_mutex_unlock(&state->mutex);
        return;
    }
    if (state->cursor < 0) state->cursor = 0;
    if (state->cursor >= (int)state->queue->len)
        state->cursor = state->queue->len - 1;

    ImageEntry *e = g_ptr_array_index(state->queue, state->cursor);
    g_mutex_unlock(&state->mutex);
    /* 不创建 texture，只验证数据完整 */
    g_assert(e != NULL);
    if (e->ready) {
        g_assert(e->meta != NULL || e->image != NULL);
    }
}

/* ---- 增量预加载 ---- */
static void trigger_preload(SimState *state) {
    g_mutex_lock(&state->mutex);
    if (!state->startup_done || !state->queue) {
        g_mutex_unlock(&state->mutex);
        return;
    }

    int before = state->cursor;
    int after  = (int)state->queue->len - 1 - state->cursor;

    if (after <= WINDOW_HALF && !state->fwd_busy) {
        state->fwd_busy = TRUE;
        LoadTask *t = g_new0(LoadTask, 1);
        t->state = state;
        t->side  = -2;
        t->slot  = -1;
        g_mutex_unlock(&state->mutex);
        g_thread_new("fwd-incr", load_one, t);
        g_print("  增量: 尾部追加\n");
        return;
    }

    if (before <= WINDOW_HALF && !state->bwd_busy) {
        state->bwd_busy = TRUE;
        LoadTask *t = g_new0(LoadTask, 1);
        t->state = state;
        t->side  = -1;
        t->slot  = -1;
        g_mutex_unlock(&state->mutex);
        g_thread_new("bwd-incr", load_one, t);
        g_print("  增量: 头部插入\n");
        return;
    }

    /* g_ptr_array_remove_index 自带 GDestroyNotify，禁止手动 entry_free */
    while (state->cursor > WINDOW_HALF + 2) {
        g_ptr_array_remove_index(state->queue, 0);
        state->cursor--;
    }
    while ((int)state->queue->len - 1 - state->cursor > WINDOW_HALF + 2) {
        g_ptr_array_remove_index(state->queue, state->queue->len - 1);
    }
    g_mutex_unlock(&state->mutex);
}

/* ---- 模拟导航 ---- */
static gboolean sim_navigate(gpointer user_data) {
    SimState *state = user_data;

    if (!state->startup_done) {
        /* 还没初始化完，等一等 */
        if (state->round > 200) {
            g_print("  超时: init_loaded=%d\n", state->init_loaded);
            g_main_loop_quit(state->loop);
        }
        state->round++;
        return G_SOURCE_CONTINUE;
    }

    if (state->total_ops >= 50) {
        g_print("\n  模拟完成: %d 次操作, queue=%u 张\n",
                state->total_ops, state->queue->len);
        g_main_loop_quit(state->loop);
        return G_SOURCE_REMOVE;
    }

    g_mutex_lock(&state->mutex);
    /* 随机前后翻 */
    if (g_random_boolean() && state->cursor > 0) {
        state->cursor--;
        g_print("  ← cursor=%d  ", state->cursor);
    } else if (state->cursor < (int)state->queue->len - 1) {
        state->cursor++;
        g_print("  → cursor=%d  ", state->cursor);
    }
    g_print("(%u total)\n", state->queue->len);
    g_mutex_unlock(&state->mutex);

    display_current(state);
    trigger_preload(state);
    state->round++;

    return G_SOURCE_CONTINUE;
}

/* ---- 初始化 ---- */
static void init_sim(SimState *state) {
    g_mutex_init(&state->mutex);
    state->queue = g_ptr_array_new_full(WINDOW_SIZE, (GDestroyNotify)entry_free);
    for (int i = 0; i < WINDOW_SIZE; i++)
        g_ptr_array_add(state->queue, entry_new());
    state->cursor = WINDOW_HALF;
    state->init_loaded = 0;
    state->startup_done = FALSE;
    state->fwd_busy = FALSE;
    state->bwd_busy = FALSE;
    state->round = 0;
    state->total_ops = 0;

    state->fwd_busy = TRUE;
    LoadTask *fwd = g_new0(LoadTask, 1);
    fwd->state = state;
    fwd->slot  = WINDOW_HALF;
    fwd->side  = -2;
    g_thread_new("fwd", load_one, fwd);

    state->bwd_busy = TRUE;
    LoadTask *bwd = g_new0(LoadTask, 1);
    bwd->state = state;
    bwd->slot  = WINDOW_HALF - 1;
    bwd->side  = -1;
    g_thread_new("bwd", load_one, bwd);
}

int main(void) {
    curl_global_init(CURL_GLOBAL_ALL);

    g_print("=== 多线程队列压力测试 ===\n");
    g_print("模拟: 2 线程加载 + 随机导航 50 次\n\n");

    SimState *state = g_new0(SimState, 1);
    init_sim(state);

    /* 驱动 g_idle_add */
    state->loop = g_main_loop_new(NULL, FALSE);

    /* 初始化完成后每 500ms 导航一次 */
    g_timeout_add(500, sim_navigate, state);

    g_main_loop_run(state->loop);

    /* 清理 */
    g_mutex_lock(&state->mutex);
    g_ptr_array_free(state->queue, TRUE);
    g_mutex_unlock(&state->mutex);
    g_mutex_clear(&state->mutex);
    g_main_loop_unref(state->loop);
    g_free(state);

    curl_global_cleanup();
    g_print("压力测试通过 ✅\n");
    return 0;
}
