#define G_LOG_DOMAIN "ui"

#include <gtk/gtk.h>
#include <gdk/gdk.h>
#include <curl/curl.h>
#include "api.h"

/* ================================================================
 *  预加载队列
 * ================================================================ */
#define WINDOW_HALF  5
#define WINDOW_SIZE  (WINDOW_HALF * 2 + 1)

/* ================================================================
 *  队列条目
 * ================================================================ */
typedef struct {
    ImageData *meta;
    GBytes    *image;
    gboolean   ready;
} ImageEntry;

static ImageEntry *entry_new(void) { return g_new0(ImageEntry, 1); }
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
typedef enum   { ORDER_SEQUENTIAL, ORDER_RANDOM } ImageOrder;

/* ================================================================ */
typedef struct {
    AppState *state;
    int       side;   /* -2=append, -1=prepend */
    int       slot;   /* >=0 for init */
} LoadTask;

struct AppState {
    GtkWidget   *window;
    GtkWidget   *picture;
    GtkWidget   *overlay_title;    /* 标题（overlay） */
    GtkWidget   *overlay_info;     /* 版权信息（overlay） */
    GtkWidget   *spinner;
    char        *provider;
    ImageOrder   order;            /* 加载顺序 */

    /* 队列 */
    GPtrArray   *queue;
    int          cursor;
    GMutex       mutex;
    gboolean     fwd_busy;
    gboolean     bwd_busy;
    int          init_loaded;
    gboolean     startup_done;

    /* 设置对话框不再缓存，每次打开重新创建 */
};

/* ---- 前向声明 ---- */
static gboolean on_slot_loaded(gpointer user_data);
static void     trigger_preload(AppState *state);
static void     display_current(AppState *state);

/* ================================================================
 *  显示当前图片
 * ================================================================ */
static void display_current(AppState *state) {
    g_mutex_lock(&state->mutex);
    if (!state->queue || state->queue->len == 0) {
        g_mutex_unlock(&state->mutex);
        return;
    }

    if (state->cursor < 0) state->cursor = 0;
    if (state->cursor >= (int)state->queue->len)
        state->cursor = state->queue->len - 1;

    ImageEntry *e = g_ptr_array_index(state->queue, state->cursor);

    if (e->ready && e->image) {
        /* 已就绪：更新图片、文字、隐藏 spinner */
        GdkTexture *tex = gdk_texture_new_from_bytes(e->image, NULL);
        if (tex) {
            gtk_picture_set_paintable(GTK_PICTURE(state->picture),
                                      GDK_PAINTABLE(tex));
            g_object_unref(tex);
        }
        gtk_widget_set_visible(state->spinner, FALSE);
        gtk_spinner_stop(GTK_SPINNER(state->spinner));
    } else {
        /* 未就绪：保留上一张图，但显示 spinner 表示加载中 */
        gtk_widget_set_visible(state->spinner, TRUE);
        gtk_spinner_start(GTK_SPINNER(state->spinner));
    }

    /* 更新文字信息 */
    if (e->meta && e->meta->title) {
        const char *title = *e->meta->title ? e->meta->title : "（无标题）";
        gtk_label_set_text(GTK_LABEL(state->overlay_title), title);
    } else if (!e->ready) {
        gtk_label_set_text(GTK_LABEL(state->overlay_title), "加载中…");
    }

    gchar *info = g_strdup_printf("%s  |  %s  |  %d/%u",
        (e->meta && e->meta->copyright) ? e->meta->copyright : "",
        (e->meta && e->meta->reldate)   ? e->meta->reldate   : "",
        state->cursor + 1, state->queue->len);
    gtk_label_set_text(GTK_LABEL(state->overlay_info), info);
    g_free(info);

    g_mutex_unlock(&state->mutex);
}

/* ================================================================
 *  工作线程：加载一张图片
 * ================================================================ */
static gpointer load_one(gpointer user_data) {
    LoadTask *task = user_data;
    AppState *state = task->state;

    /* 全部用 api_random，保证每张图不同 */
    ImageData *meta = api_random(state->provider);

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

/* ================================================================
 *  主线程回调
 * ================================================================ */
static gboolean on_slot_loaded(gpointer user_data) {
    LoadTask *task = user_data;
    AppState *state = task->state;
    int slot = task->slot;
    int side = task->side;
    g_free(task);

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
                t->state = state; t->slot = next; t->side = -2;
                g_thread_new("fwd", load_one, t);
            }
        }
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
                t->state = state; t->slot = prev; t->side = -1;
                g_thread_new("bwd", load_one, t);
            }
        }
        if (state->init_loaded >= WINDOW_SIZE) {
            state->startup_done = TRUE;
            g_debug("初始化完成: %d/%d", state->init_loaded, WINDOW_SIZE);
        }
    }

    display_current(state);
    trigger_preload(state);
    return G_SOURCE_REMOVE;
}

/* ================================================================
 *  增量预加载 + 裁剪
 * ================================================================ */
static void trigger_preload(AppState *state) {
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
        t->state = state; t->side = -2; t->slot = -1;
        g_mutex_unlock(&state->mutex);
        g_thread_new("fwd-incr", load_one, t);
        return;
    }
    if (before <= WINDOW_HALF && !state->bwd_busy) {
        state->bwd_busy = TRUE;
        LoadTask *t = g_new0(LoadTask, 1);
        t->state = state; t->side = -1; t->slot = -1;
        g_mutex_unlock(&state->mutex);
        g_thread_new("bwd-incr", load_one, t);
        return;
    }

    /* 裁剪 */
    while (state->cursor > WINDOW_HALF + 2)
        g_ptr_array_remove_index(state->queue, 0), state->cursor--;
    while ((int)state->queue->len - 1 - state->cursor > WINDOW_HALF + 2)
        g_ptr_array_remove_index(state->queue, state->queue->len - 1);

    g_mutex_unlock(&state->mutex);
}

/* ================================================================
 *  键盘 ← → 导航
 * ================================================================ */
static gboolean on_key_pressed(GtkEventControllerKey *controller,
                               guint keyval, guint keycode,
                               GdkModifierType state, AppState *app) {
    if (keyval == GDK_KEY_Left || keyval == GDK_KEY_h) {
        g_mutex_lock(&app->mutex);
        if (app->cursor > 0) app->cursor--;
        g_mutex_unlock(&app->mutex);
        display_current(app);
        trigger_preload(app);
        return TRUE;
    }
    if (keyval == GDK_KEY_Right || keyval == GDK_KEY_l) {
        g_mutex_lock(&app->mutex);
        if (app->cursor < (int)app->queue->len - 1) app->cursor++;
        g_mutex_unlock(&app->mutex);
        display_current(app);
        trigger_preload(app);
        return TRUE;
    }
    return FALSE;
}

/* ================================================================
 *  鼠标滚轮切换图片
 * ================================================================ */
static gboolean on_scroll(GtkEventControllerScroll *controller,
                          double dx, double dy, AppState *state) {
    g_mutex_lock(&state->mutex);
    if (dy > 0 && state->cursor < (int)state->queue->len - 1)
        state->cursor++;
    else if (dy < 0 && state->cursor > 0)
        state->cursor--;
    g_mutex_unlock(&state->mutex);
    display_current(state);
    trigger_preload(state);
    return TRUE;
}

/* ---- 左键点击：获取焦点使键盘生效 ---- */
static void on_left_click(GtkGestureClick *gesture, int n_press,
                          double x, double y, AppState *state) {
    gtk_widget_grab_focus(state->picture);
}

/* ================================================================
 *  右键菜单
 * ================================================================ */
static void on_about_activate(GSimpleAction *action, GVariant *param, AppState *state) {
    GtkWidget *about = gtk_about_dialog_new();
    gtk_about_dialog_set_program_name(GTK_ABOUT_DIALOG(about), "拾光");
    gtk_about_dialog_set_version(GTK_ABOUT_DIALOG(about), "0.1.0");
    gtk_about_dialog_set_comments(GTK_ABOUT_DIALOG(about), "每日一图桌面客户端");
    gtk_window_set_transient_for(GTK_WINDOW(about), GTK_WINDOW(state->window));
    gtk_window_present(GTK_WINDOW(about));
}

static void on_quit_activate(GSimpleAction *action, GVariant *param, AppState *state) {
    gtk_window_destroy(GTK_WINDOW(state->window));
}

/* ---- 右键手势 ---- */
static void on_right_click(GtkGestureClick *gesture, int n_press,
                           double x, double y, AppState *state) {
    GMenu *menu = g_menu_new();
    g_menu_append(menu, "设置…",   "app.settings");
    g_menu_append(menu, "关于",    "app.about");
    g_menu_append(menu, "退出",    "app.quit");

    GtkWidget *pop = gtk_popover_menu_new_from_model(G_MENU_MODEL(menu));
    gtk_popover_set_pointing_to(GTK_POPOVER(pop), &(GdkRectangle){x, y, 1, 1});
    gtk_popover_set_has_arrow(GTK_POPOVER(pop), FALSE);
    gtk_widget_set_parent(pop, state->picture);
    gtk_popover_popup(GTK_POPOVER(pop));
    g_object_unref(menu);
}

/* ================================================================
 *  设置对话框
 * ================================================================ */
/* ---- 设置对话框（每次打开重新创建，关闭即销毁）---- */

typedef struct {
    AppState   *state;
    GtkWidget  *window;
    GtkWidget  *dropdown;
    GtkWidget  *radio_rand;
} SettingsCtx;

static void on_settings_destroy(GtkWindow *win, SettingsCtx *ctx) {
    g_free(ctx);
}

static void on_settings_apply(GtkButton *btn, SettingsCtx *ctx) {
    AppState *state = ctx->state;
    guint idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(ctx->dropdown));
    if (idx < (guint)PROVIDER_COUNT) {
        g_free(state->provider);
        state->provider = g_strdup(PROVIDERS[idx]);
    }
    state->order = gtk_check_button_get_active(
        GTK_CHECK_BUTTON(ctx->radio_rand)) ? ORDER_RANDOM : ORDER_SEQUENTIAL;

    g_debug("设置: provider=%s, order=%s", state->provider,
            state->order == ORDER_RANDOM ? "random" : "sequential");

    /* 清空队列重新加载 */
    g_mutex_lock(&state->mutex);
    if (state->queue) g_ptr_array_free(state->queue, TRUE);
    state->queue = g_ptr_array_new_full(WINDOW_SIZE, (GDestroyNotify)entry_free);
    for (int i = 0; i < WINDOW_SIZE; i++)
        g_ptr_array_add(state->queue, entry_new());
    state->cursor = WINDOW_HALF;
    state->init_loaded = 0;
    state->startup_done = FALSE;
    state->fwd_busy = FALSE;
    state->bwd_busy = FALSE;
    g_mutex_unlock(&state->mutex);

    gtk_widget_set_visible(state->spinner, TRUE);
    gtk_spinner_start(GTK_SPINNER(state->spinner));

    state->fwd_busy = TRUE;
    LoadTask *fwd = g_new0(LoadTask, 1);
    fwd->state = state; fwd->slot = WINDOW_HALF; fwd->side = -2;
    g_thread_new("fwd", load_one, fwd);

    state->bwd_busy = TRUE;
    LoadTask *bwd = g_new0(LoadTask, 1);
    bwd->state = state; bwd->slot = WINDOW_HALF - 1; bwd->side = -1;
    g_thread_new("bwd", load_one, bwd);

    gtk_window_destroy(GTK_WINDOW(ctx->window));
}

static void on_settings_activate(GSimpleAction *action, GVariant *param, AppState *state) {
    SettingsCtx *ctx = g_new0(SettingsCtx, 1);
    ctx->state = state;

    GtkWidget *win = gtk_window_new();
    ctx->window = win;
    gtk_window_set_title(GTK_WINDOW(win), "设置");
    gtk_window_set_default_size(GTK_WINDOW(win), 320, 200);
    gtk_window_set_transient_for(GTK_WINDOW(win), GTK_WINDOW(state->window));
    gtk_window_set_modal(GTK_WINDOW(win), TRUE);
    g_signal_connect(win, "destroy", G_CALLBACK(on_settings_destroy), ctx);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(vbox, 16);
    gtk_widget_set_margin_end(vbox, 16);
    gtk_widget_set_margin_top(vbox, 16);
    gtk_widget_set_margin_bottom(vbox, 16);

    GtkWidget *lbl1 = gtk_label_new("图源：");
    gtk_widget_set_halign(lbl1, GTK_ALIGN_START);
    GtkStringList *providers = gtk_string_list_new((const char * const *)PROVIDERS);
    GtkWidget *dropdown = gtk_drop_down_new(G_LIST_MODEL(providers), NULL);
    ctx->dropdown = dropdown;

    /* 默认选中当前 provider */
    for (int i = 0; i < PROVIDER_COUNT; i++) {
        if (g_strcmp0(state->provider, PROVIDERS[i]) == 0) {
            gtk_drop_down_set_selected(GTK_DROP_DOWN(dropdown), i);
            break;
        }
    }

    GtkWidget *lbl2 = gtk_label_new("排序方式：");
    gtk_widget_set_halign(lbl2, GTK_ALIGN_START);
    GtkWidget *radio_seq  = gtk_check_button_new_with_label("顺序排列 (今日)");
    GtkWidget *radio_rand = gtk_check_button_new_with_label("随机排列");
    ctx->radio_rand = radio_rand;
    gtk_check_button_set_active(GTK_CHECK_BUTTON(
        state->order == ORDER_RANDOM ? radio_rand : radio_seq), TRUE);
    gtk_check_button_set_group(GTK_CHECK_BUTTON(radio_rand),
                                GTK_CHECK_BUTTON(radio_seq));

    GtkWidget *btn_apply = gtk_button_new_with_label("应用");
    gtk_widget_add_css_class(btn_apply, "suggested-action");
    g_signal_connect(btn_apply, "clicked", G_CALLBACK(on_settings_apply), ctx);

    GtkWidget *btn_cancel = gtk_button_new_with_label("取消");
    g_signal_connect_swapped(btn_cancel, "clicked",
                             G_CALLBACK(gtk_window_destroy), win);

    GtkWidget *btnbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(btnbox, GTK_ALIGN_END);
    gtk_widget_set_hexpand(btnbox, TRUE);
    gtk_box_append(GTK_BOX(btnbox), btn_cancel);
    gtk_box_append(GTK_BOX(btnbox), btn_apply);

    gtk_box_append(GTK_BOX(vbox), lbl1);
    gtk_box_append(GTK_BOX(vbox), dropdown);
    gtk_box_append(GTK_BOX(vbox), lbl2);
    gtk_box_append(GTK_BOX(vbox), radio_seq);
    gtk_box_append(GTK_BOX(vbox), radio_rand);
    gtk_box_append(GTK_BOX(vbox), btnbox);

    gtk_window_set_child(GTK_WINDOW(win), vbox);
    gtk_window_present(GTK_WINDOW(win));
}

/* ================================================================
 *  初始化队列
 * ================================================================ */
static void init_queue(AppState *state) {
    g_mutex_lock(&state->mutex);
    if (state->queue) g_ptr_array_free(state->queue, TRUE);

    state->queue = g_ptr_array_new_full(WINDOW_SIZE, (GDestroyNotify)entry_free);
    for (int i = 0; i < WINDOW_SIZE; i++)
        g_ptr_array_add(state->queue, entry_new());
    state->cursor       = WINDOW_HALF;
    state->init_loaded   = 0;
    state->startup_done  = FALSE;
    state->fwd_busy      = FALSE;
    state->bwd_busy      = FALSE;
    g_mutex_unlock(&state->mutex);

    state->fwd_busy = TRUE;
    LoadTask *fwd = g_new0(LoadTask, 1);
    fwd->state = state; fwd->slot = WINDOW_HALF; fwd->side = -2;
    g_thread_new("fwd", load_one, fwd);

    state->bwd_busy = TRUE;
    LoadTask *bwd = g_new0(LoadTask, 1);
    bwd->state = state; bwd->slot = WINDOW_HALF - 1; bwd->side = -1;
    g_thread_new("bwd", load_one, bwd);
}

/* ================================================================
 *  激活
 * ================================================================ */
static void on_activate(GtkApplication *app, gpointer user_data) {
    AppState *state = g_new0(AppState, 1);
    state->provider = g_strdup(PROVIDERS[0]);
    state->order    = ORDER_SEQUENTIAL;
    g_mutex_init(&state->mutex);

    /* ---- 窗口 ---- */
    state->window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(state->window), "拾光");
    gtk_window_set_default_size(GTK_WINDOW(state->window), 1024, 768);

    /* ---- 图片(铺满) ---- */
    state->picture = gtk_picture_new();
    gtk_widget_set_halign(state->picture, GTK_ALIGN_FILL);
    gtk_widget_set_valign(state->picture, GTK_ALIGN_FILL);
    gtk_widget_set_hexpand(state->picture, TRUE);
    gtk_widget_set_vexpand(state->picture, TRUE);
    gtk_picture_set_content_fit(GTK_PICTURE(state->picture), GTK_CONTENT_FIT_CONTAIN);

    /* ---- 文字 overlay ---- */
    state->overlay_title = gtk_label_new(NULL);
    gtk_widget_set_halign(state->overlay_title, GTK_ALIGN_START);
    gtk_widget_set_valign(state->overlay_title, GTK_ALIGN_END);
    gtk_widget_set_margin_start(state->overlay_title, 16);
    gtk_widget_set_margin_bottom(state->overlay_title, 4);
    gtk_widget_add_css_class(state->overlay_title, "overlay-title");

    state->overlay_info = gtk_label_new(NULL);
    gtk_widget_set_halign(state->overlay_info, GTK_ALIGN_START);
    gtk_widget_set_valign(state->overlay_info, GTK_ALIGN_END);
    gtk_widget_set_margin_start(state->overlay_info, 16);
    gtk_widget_set_margin_bottom(state->overlay_info, 24);
    gtk_widget_add_css_class(state->overlay_info, "overlay-info");

    GtkWidget *info_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(info_box), state->overlay_title);
    gtk_box_append(GTK_BOX(info_box), state->overlay_info);

    /* Spinner */
    state->spinner = gtk_spinner_new();
    gtk_widget_set_size_request(state->spinner, 48, 48);
    gtk_widget_set_halign(state->spinner, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(state->spinner, GTK_ALIGN_CENTER);
    gtk_spinner_start(GTK_SPINNER(state->spinner));

    /* ---- Overlay 叠层: 底=图片, 上=文字信息+spinner ---- */
    GtkWidget *overlay = gtk_overlay_new();
    gtk_overlay_set_child(GTK_OVERLAY(overlay), state->picture);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), info_box);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), state->spinner);

    gtk_window_set_child(GTK_WINDOW(state->window), overlay);

    /* ---- CSS ---- */
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css,
        ".overlay-title {"
        "  font-size: 18px; font-weight: bold;"
        "  color: white;"
        "  text-shadow: 0 1px 3px rgba(0,0,0,0.8);"
        "}"
        ".overlay-info {"
        "  font-size: 13px;"
        "  color: rgba(255,255,255,0.85);"
        "  text-shadow: 0 1px 3px rgba(0,0,0,0.7);"
        "}");
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(),
        GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    /* ---- 键盘控制器（← → / h l 导航）写在窗口级别 ---- */
    GtkEventController *key_ctrl = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(key_ctrl, GTK_PHASE_CAPTURE);
    g_signal_connect(key_ctrl, "key-pressed", G_CALLBACK(on_key_pressed), state);
    gtk_widget_add_controller(state->window, key_ctrl);

    /* ---- 滚轮控制器 ---- */
    GtkEventController *scroll_ctrl = gtk_event_controller_scroll_new(
        GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    g_signal_connect(scroll_ctrl, "scroll", G_CALLBACK(on_scroll), state);
    gtk_widget_add_controller(state->window, scroll_ctrl);

    /* ---- 左键点击：获取焦点，使键盘生效 ---- */
    GtkEventController *left_click = GTK_EVENT_CONTROLLER(gtk_gesture_click_new());
    g_signal_connect(left_click, "pressed", G_CALLBACK(on_left_click), state);
    gtk_widget_add_controller(state->picture, left_click);

    /* ---- 右键手势 ---- */
    GtkEventController *right_click = GTK_EVENT_CONTROLLER(gtk_gesture_click_new());
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(right_click), GDK_BUTTON_SECONDARY);
    g_signal_connect(right_click, "pressed", G_CALLBACK(on_right_click), state);
    gtk_widget_add_controller(state->window, right_click);

    /* ---- App Actions ---- */
    GSimpleActionGroup *actions = g_simple_action_group_new();
    GSimpleAction *act;
    act = g_simple_action_new("settings", NULL);
    g_signal_connect(act, "activate", G_CALLBACK(on_settings_activate), state);
    g_action_map_add_action(G_ACTION_MAP(actions), G_ACTION(act));
    act = g_simple_action_new("about", NULL);
    g_signal_connect(act, "activate", G_CALLBACK(on_about_activate), state);
    g_action_map_add_action(G_ACTION_MAP(actions), G_ACTION(act));
    act = g_simple_action_new("quit", NULL);
    g_signal_connect(act, "activate", G_CALLBACK(on_quit_activate), state);
    g_action_map_add_action(G_ACTION_MAP(actions), G_ACTION(act));
    gtk_widget_insert_action_group(state->window, "app", G_ACTION_GROUP(actions));

    /* ---- 显示 ---- */
    gtk_window_present(GTK_WINDOW(state->window));

    /* ---- 初始化加载 ---- */
    init_queue(state);
}

int main(int argc, char **argv) {
    curl_global_init(CURL_GLOBAL_ALL);

    GtkApplication *app = gtk_application_new("com.example.shiguang",
                                               G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);

    curl_global_cleanup();
    return status;
}
