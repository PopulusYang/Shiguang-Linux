#define G_LOG_DOMAIN "ui"

#include <gtk/gtk.h>
#include <gdk/gdk.h>
#include <curl/curl.h>
#include "api.h"

/* ================================================================
 *  预加载队列
 * ================================================================ */
#define WINDOW_HALF  5
#define WINDOW_SIZE  (WINDOW_HALF * 2 + 1)   /* 11 */

/* ================================================================
 *  队列条目
 * ================================================================ */
typedef struct {
    ImageData *meta;
    GBytes    *image;      /* 原图 */
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

/* ================================================================
 *  前向声明
 * ================================================================ */
typedef struct AppState AppState;

/* ================================================================
 *  工作线程任务
 *   side:  +1 = 尾部追加（向前加载）
 *          -1 = 头部插入（向后加载）
 *   slot:  初始化阶段的目标 slot（≥0 时有效）
 * ================================================================ */
typedef enum { SIDE_APPEND = -2, SIDE_PREPEND = -1 } LoadSide;

typedef struct {
    AppState  *state;
    int        side;    /* SIDE_PREPEND 或 SIDE_APPEND */
    int        slot;    /* 初始化目标 slot，增量模式忽略（-1） */
} LoadTask;

/* ================================================================
 *  应用全局状态
 * ================================================================ */
struct AppState {
    GtkWidget    *window;
    GtkWidget    *picture;
    GtkWidget    *placeholder;
    GtkWidget    *label_title;
    GtkWidget    *label_info;
    GtkWidget    *label_counter;
    GtkWidget    *spinner;
    GtkWidget    *btn_prev;
    GtkWidget    *btn_next;
    GtkDropDown  *dropdown;
    char         *provider;

    /* 队列 */
    GPtrArray    *queue;         /* ImageEntry* */
    int           cursor;        /* 当前显示位置 */
    GMutex        mutex;

    /* 双线程控制 */
    gboolean      fwd_busy;      /* 向前线程正在工作 */
    gboolean      bwd_busy;      /* 向后线程正在工作 */
    int           init_loaded;   /* 初始化已加载计数 */
    gboolean      startup_done;
};

/* ---- 前向声明 ---- */
static gboolean on_slot_loaded(gpointer user_data);
static void     trigger_preload(AppState *state);

/* ================================================================
 *  显示当前图片
 * ================================================================ */
static void display_current(AppState *state) {
    g_mutex_lock(&state->mutex);

    if (!state->queue || state->queue->len == 0) {
        g_mutex_unlock(&state->mutex);
        return;
    }

    /* clamp cursor */
    if (state->cursor < 0) state->cursor = 0;
    if (state->cursor >= (int)state->queue->len)
        state->cursor = state->queue->len - 1;

    ImageEntry *e = g_ptr_array_index(state->queue, state->cursor);

    if (e->ready && e->image) {
        GdkTexture *tex = gdk_texture_new_from_bytes(e->image, NULL);
        if (tex) {
            gtk_picture_set_paintable(GTK_PICTURE(state->picture),
                                      GDK_PAINTABLE(tex));
            g_object_unref(tex);
            gtk_widget_set_visible(state->placeholder, FALSE);
        }
    } else {
        gtk_widget_set_visible(state->placeholder, TRUE);
    }

    if (e->meta) {
        gtk_label_set_text(GTK_LABEL(state->label_title),
            e->meta->title && *e->meta->title ? e->meta->title : "（无标题）");
        gchar *info = g_strdup_printf("%s  |  %s",
            e->meta->copyright && *e->meta->copyright ? e->meta->copyright : "",
            e->meta->reldate   && *e->meta->reldate   ? e->meta->reldate   : "");
        gtk_label_set_text(GTK_LABEL(state->label_info), info);
        g_free(info);
    }

    gchar *counter = g_strdup_printf("%d / %u", state->cursor + 1, state->queue->len);
    gtk_label_set_text(GTK_LABEL(state->label_counter), counter);
    g_free(counter);

    gtk_widget_set_sensitive(state->btn_prev, state->cursor > 0);
    gtk_widget_set_sensitive(state->btn_next,
                             state->cursor < (int)state->queue->len - 1);

    g_mutex_unlock(&state->mutex);
}

/* ================================================================
 *  工作线程：加载一张图片
 * ================================================================ */
static gpointer load_one(gpointer user_data) {
    LoadTask *task = user_data;
    AppState *state = task->state;

    g_debug("工作线程: side=%d, slot=%d", task->side, task->slot);

    /* 1. 请求 API */
    ImageData *meta = api_random(state->provider);
    if (!meta) {
        g_debug("工作线程: API 失败");
        task->slot = -1; /* 标记失败 */
        g_idle_add(on_slot_loaded, task);
        return NULL;
    }

    /* 2. 下载原图 */
    GBytes *img = api_download_image(meta->imgurl);

    /* 3. 创建条目 */
    ImageEntry *entry = entry_new();
    entry->meta  = meta;
    entry->image = img;
    entry->ready = TRUE;

    /* 4. 把条目挂在 task 上，让主线程决定放哪里 */
    g_mutex_lock(&state->mutex);

    if (task->slot >= 0) {
        /* 初始化阶段：替换队列指定位置的占位条目 */
        if (task->slot < (int)state->queue->len) {
            entry_free(g_ptr_array_index(state->queue, task->slot));
            state->queue->pdata[task->slot] = entry;
        }
    } else if (task->side == SIDE_APPEND) {
        /* 增量：追加到尾部 */
        g_ptr_array_add(state->queue, entry);
    } else if (task->side == SIDE_PREPEND) {
        /* 增量：插入到头部 */
        g_ptr_array_insert(state->queue, 0, entry);
        state->cursor++; /* 头部插入后 cursor 右移 */
    }

    /* 清理线程状态 */
    if (task->side == SIDE_APPEND)
        state->fwd_busy = FALSE;
    else
        state->bwd_busy = FALSE;

    if (task->slot >= 0)
        state->init_loaded++;

    g_mutex_unlock(&state->mutex);

    g_idle_add(on_slot_loaded, task);
    return NULL;
}

/* ================================================================
 *  主线程回调：一张图片加载完成
 * ================================================================ */
static gboolean on_slot_loaded(gpointer user_data) {
    LoadTask *task = user_data;
    AppState *state = task->state;
    int slot  = task->slot;
    int side  = task->side;

    g_free(task);

    g_debug("主线程: slot=%d side=%d 就绪", slot, side);

    /* 刷新显示 */
    display_current(state);

    /* 初始化阶段：继续加载相邻位置 */
    if (!state->startup_done && slot >= 0) {
        /* slot > WINDOW_HALF（向前）→ 继续加载 slot+1 */
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
                t->side  = SIDE_APPEND;
                g_debug("初始化向前: slot=%d", next);
                g_thread_new("load-fwd", load_one, t);
            }
        }

        /* slot < WINDOW_HALF（向后）→ 继续加载 slot-1 */
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
                t->side  = SIDE_PREPEND;
                g_debug("初始化向后: slot=%d", prev);
                g_thread_new("load-bwd", load_one, t);
            }
        }

        /* 判断初始化是否完成 */
        if (state->init_loaded >= WINDOW_SIZE) {
            state->startup_done = TRUE;
            gtk_spinner_stop(GTK_SPINNER(state->spinner));
            gtk_widget_set_visible(state->spinner, FALSE);
            g_debug("初始化完成: %d/%d", state->init_loaded, WINDOW_SIZE);
        }
    }

    /* 增量预加载 */
    if (state->startup_done)
        trigger_preload(state);

    return G_SOURCE_REMOVE;
}

/* ================================================================
 *  增量预加载：确保前后边界各有 WINDOW_HALF 张
 * ================================================================ */
static void trigger_preload(AppState *state) {
    g_mutex_lock(&state->mutex);

    if (!state->startup_done || !state->queue) {
        g_mutex_unlock(&state->mutex);
        return;
    }

    int before = state->cursor;
    int after  = (int)state->queue->len - 1 - state->cursor;

    /* 尾部不够 → 启动向前加载 */
    if (after <= WINDOW_HALF && !state->fwd_busy) {
        state->fwd_busy = TRUE;
        LoadTask *t = g_new0(LoadTask, 1);
        t->state = state;
        t->side  = SIDE_APPEND;
        t->slot  = -1;

        g_debug("增量预加载: 尾部追加 (after=%d)", after);
        g_mutex_unlock(&state->mutex);
        g_thread_new("load-fwd", load_one, t);
        return;
    }

    /* 头部不够 → 启动向后加载 */
    if (before <= WINDOW_HALF && !state->bwd_busy) {
        state->bwd_busy = TRUE;
        LoadTask *t = g_new0(LoadTask, 1);
        t->state = state;
        t->side  = SIDE_PREPEND;
        t->slot  = -1;

        g_debug("增量预加载: 头部插入 (before=%d)", before);
        g_mutex_unlock(&state->mutex);
        g_thread_new("load-bwd", load_one, t);
        return;
    }

    /* 裁剪：头部超出 WINDOW_HALF+2 丢弃
     * 注意：g_ptr_array_remove_index 会自动调用 GDestroyNotify，
     *       不要在前面手动 entry_free，会双释放！ */
    while (state->cursor > WINDOW_HALF + 2) {
        g_ptr_array_remove_index(state->queue, 0);
        state->cursor--;
    }

    while ((int)state->queue->len - 1 - state->cursor > WINDOW_HALF + 2) {
        g_ptr_array_remove_index(state->queue, state->queue->len - 1);
    }

    g_mutex_unlock(&state->mutex);
}

/* ================================================================
 *  初始化队列：2 个线程，一个向前一个向后
 * ================================================================ */
static void init_queue(AppState *state) {
    g_mutex_lock(&state->mutex);

    /* 释放旧队列 */
    if (state->queue) {
        g_ptr_array_free(state->queue, TRUE);
    }

    /* 创建 11 个占位条目 */
    state->queue = g_ptr_array_new_full(WINDOW_SIZE,
                                         (GDestroyNotify)entry_free);
    for (int i = 0; i < WINDOW_SIZE; i++) {
        g_ptr_array_add(state->queue, entry_new());
    }
    state->cursor     = WINDOW_HALF;
    state->init_loaded = 0;
    state->startup_done = FALSE;
    state->fwd_busy = FALSE;
    state->bwd_busy = FALSE;

    g_mutex_unlock(&state->mutex);

    /* 启动向前加载线程：从 slot=5（当前）开始，向 6,7,8,9,10 */
    state->fwd_busy = TRUE;
    LoadTask *fwd = g_new0(LoadTask, 1);
    fwd->state = state;
    fwd->slot  = WINDOW_HALF;    /* 当前 = slot 5 */
    fwd->side  = SIDE_APPEND;
    g_thread_new("load-fwd", load_one, fwd);

    /* 启动向后加载线程：从 slot=4 开始，向 3,2,1,0 */
    state->bwd_busy = TRUE;
    LoadTask *bwd = g_new0(LoadTask, 1);
    bwd->state = state;
    bwd->slot  = WINDOW_HALF - 1;  /* slot 4 */
    bwd->side  = SIDE_PREPEND;
    g_thread_new("load-bwd", load_one, bwd);

    g_debug("初始化: 2 线程启动 (fwd:5→10, bwd:4→0)");
}

/* ================================================================
 *  导航
 * ================================================================ */
static void on_prev(GtkButton *btn, AppState *state) {
    g_mutex_lock(&state->mutex);
    if (state->cursor > 0) {
        state->cursor--;
        g_debug("导航: ← cursor=%d", state->cursor);
    }
    g_mutex_unlock(&state->mutex);
    display_current(state);
    trigger_preload(state);
}

static void on_next(GtkButton *btn, AppState *state) {
    g_mutex_lock(&state->mutex);
    if (state->cursor < (int)state->queue->len - 1) {
        state->cursor++;
        g_debug("导航: → cursor=%d", state->cursor);
    }
    g_mutex_unlock(&state->mutex);
    display_current(state);
    trigger_preload(state);
}

static void on_provider_changed(GtkDropDown *dd, GParamSpec *pspec, AppState *state) {
    guint idx = gtk_drop_down_get_selected(dd);
    if (idx >= (guint)PROVIDER_COUNT) return;

    g_debug("切换图源: %s -> %s", state->provider, PROVIDERS[idx]);
    g_free(state->provider);
    state->provider = g_strdup(PROVIDERS[idx]);

    gtk_spinner_start(GTK_SPINNER(state->spinner));
    gtk_widget_set_visible(state->spinner, TRUE);
    gtk_picture_set_paintable(GTK_PICTURE(state->picture), NULL);
    gtk_widget_set_visible(state->placeholder, TRUE);

    init_queue(state);
}

/* ================================================================
 *  激活应用
 * ================================================================ */
static void on_activate(GtkApplication *app, gpointer user_data) {
    AppState *state = g_new0(AppState, 1);
    state->provider = g_strdup(PROVIDERS[0]);
    g_mutex_init(&state->mutex);

    /* ---- 窗口 ---- */
    state->window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(state->window), "拾光 — 每日一图");
    gtk_window_set_default_size(GTK_WINDOW(state->window), 1280, 720);

    /* ---- 主布局 ---- */
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(vbox, 12);
    gtk_widget_set_margin_end(vbox, 12);
    gtk_widget_set_margin_top(vbox, 12);
    gtk_widget_set_margin_bottom(vbox, 12);

    /* ---- 顶部控制栏 ---- */
    GtkWidget *ctlbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

    GtkStringList *providers = gtk_string_list_new((const char * const *)PROVIDERS);
    state->dropdown = GTK_DROP_DOWN(gtk_drop_down_new(G_LIST_MODEL(providers), NULL));
    gtk_widget_set_hexpand(GTK_WIDGET(state->dropdown), TRUE);
    g_signal_connect(state->dropdown, "notify::selected",
                     G_CALLBACK(on_provider_changed), state);

    state->btn_prev = gtk_button_new_with_label("◀ 前一张");
    g_signal_connect(state->btn_prev, "clicked", G_CALLBACK(on_prev), state);
    gtk_widget_set_sensitive(state->btn_prev, FALSE);

    state->label_counter = gtk_label_new("0 / 0");
    gtk_widget_set_margin_start(state->label_counter, 12);
    gtk_widget_set_margin_end(state->label_counter, 12);

    state->btn_next = gtk_button_new_with_label("后一张 ▶");
    g_signal_connect(state->btn_next, "clicked", G_CALLBACK(on_next), state);
    gtk_widget_set_sensitive(state->btn_next, FALSE);

    state->spinner = gtk_spinner_new();
    gtk_widget_set_size_request(state->spinner, 24, 24);
    gtk_spinner_start(GTK_SPINNER(state->spinner));

    gtk_box_append(GTK_BOX(ctlbar), GTK_WIDGET(state->dropdown));
    gtk_box_append(GTK_BOX(ctlbar), state->btn_prev);
    gtk_box_append(GTK_BOX(ctlbar), state->label_counter);
    gtk_box_append(GTK_BOX(ctlbar), state->btn_next);
    gtk_box_append(GTK_BOX(ctlbar), state->spinner);

    /* ---- 图片显示区 ---- */
    state->picture = gtk_picture_new();
    gtk_widget_set_halign(state->picture, GTK_ALIGN_FILL);
    gtk_widget_set_valign(state->picture, GTK_ALIGN_FILL);
    gtk_picture_set_content_fit(GTK_PICTURE(state->picture), GTK_CONTENT_FIT_CONTAIN);

    GtkWidget *frame = gtk_frame_new(NULL);
    gtk_frame_set_child(GTK_FRAME(frame), state->picture);
    gtk_widget_set_vexpand(frame, TRUE);

    state->placeholder = gtk_label_new("加载中…");
    gtk_widget_set_halign(state->placeholder, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(state->placeholder, GTK_ALIGN_CENTER);

    GtkWidget *overlay = gtk_overlay_new();
    gtk_overlay_set_child(GTK_OVERLAY(overlay), frame);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), state->placeholder);

    /* ---- 底部信息 ---- */
    state->label_title = gtk_label_new(NULL);
    gtk_label_set_wrap(GTK_LABEL(state->label_title), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(state->label_title), PANGO_WRAP_WORD_CHAR);
    gtk_widget_add_css_class(state->label_title, "title");

    state->label_info = gtk_label_new(NULL);
    gtk_widget_set_halign(state->label_info, GTK_ALIGN_START);
    gtk_widget_add_css_class(state->label_info, "info");

    GtkWidget *infobox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_box_append(GTK_BOX(infobox), state->label_title);
    gtk_box_append(GTK_BOX(infobox), state->label_info);

    /* ---- 组装 ---- */
    gtk_box_append(GTK_BOX(vbox), ctlbar);
    gtk_box_append(GTK_BOX(vbox), overlay);
    gtk_box_append(GTK_BOX(vbox), infobox);

    gtk_window_set_child(GTK_WINDOW(state->window), vbox);

    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css,
        ".title { font-size: 16px; font-weight: bold; margin-top: 8px; }"
        ".info  { font-size: 13px; color: @unfocused_insensitive_color; }");
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(),
        GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    gtk_window_present(GTK_WINDOW(state->window));

    init_queue(state);
}

int main(int argc, char **argv) {
    /* libcurl 全局初始化（多线程下必须有，否则 SSL 线程不安全）*/
    curl_global_init(CURL_GLOBAL_ALL);

    GtkApplication *app = gtk_application_new("com.example.shiguang",
                                               G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);

    curl_global_cleanup();
    return status;
}
