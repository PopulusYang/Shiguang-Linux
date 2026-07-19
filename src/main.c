#define G_LOG_DOMAIN "ui"

#include <gtk/gtk.h>
#include <gdk/gdk.h>
#include <curl/curl.h>
#include <json-glib/json-glib.h>
#include <string.h>
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
/* ================================================================ */
typedef struct {
    AppState *state;
    int       side;   /* -2=append, -1=prepend */
    int       slot;   /* >=0 for init */
} LoadTask;

struct AppState {
    GtkWidget   *window;
    GtkWidget   *stack;            /* 双图 crossfade 切换 */
    GtkWidget   *picture_a;
    GtkWidget   *picture_b;
    gboolean     active_is_b;      /* 当前显示的是 picture_b */
    GtkWidget   *overlay_slogan;
    GtkWidget   *overlay_title;
    GtkWidget   *overlay_info;
    GtkWidget   *spinner;
    char        *provider;

    /* 队列 */
    GPtrArray   *queue;
    int          cursor;
    GMutex       mutex;
    gboolean     fwd_busy;
    gboolean     bwd_busy;
    int          init_loaded;
    gboolean     startup_done;

    /* 侧边栏控件 */
    GtkWidget   *sidebar_revealer;
    GtkWidget   *label_hitokoto;
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
        /* 新图片设到非活动的 picture 上，然后 crossfade 切换 */
        GtkWidget *inactive = state->active_is_b
            ? state->picture_a : state->picture_b;
        const char *target = state->active_is_b ? "a" : "b";

        GdkTexture *tex = gdk_texture_new_from_bytes(e->image, NULL);
        if (tex) {
            gtk_picture_set_paintable(GTK_PICTURE(inactive),
                                      GDK_PAINTABLE(tex));
            g_object_unref(tex);
            gtk_stack_set_visible_child_name(
                GTK_STACK(state->stack), target);
            state->active_is_b = !state->active_is_b;
        }
        gtk_widget_set_visible(state->spinner, FALSE);
        gtk_spinner_stop(GTK_SPINNER(state->spinner));
    } else {
        /* 未就绪：保留上一张图，但显示 spinner 表示加载中 */
        gtk_widget_set_visible(state->spinner, TRUE);
        gtk_spinner_start(GTK_SPINNER(state->spinner));
    }

    /* 更新文字信息 */
    if (e->meta && e->meta->title && *e->meta->title) {
        gtk_label_set_text(GTK_LABEL(state->overlay_title), e->meta->title);
    } else if (!e->ready) {
        gtk_label_set_text(GTK_LABEL(state->overlay_title), "加载中…");
    } else {
        gtk_label_set_text(GTK_LABEL(state->overlay_title), "（无标题）");
    }

    /* 收藏为空时显示提示 */
    if (e->meta && e->meta->reldate && *e->meta->reldate &&
        g_strcmp0(e->meta->reldate, "右键收藏图片后会出现在这里") == 0) {
        gtk_label_set_text(GTK_LABEL(state->overlay_title), "收藏夹为空");
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

    ImageData *meta = NULL;
    GBytes    *img  = NULL;
    gboolean   is_local = (g_strcmp0(state->provider, "favorites") == 0);

    if (is_local) {
        /* 收藏：从本地目录随机加载 */
        int count = api_favorites_count();
        if (count == 0) {
            g_debug("收藏为空，无图片可加载");
            meta = g_new0(ImageData, 1);
            meta->title   = g_strdup("收藏夹为空");
            meta->imgurl  = g_strdup("");
            meta->reldate = g_strdup("右键收藏图片后会出现在这里");
        } else {
            int idx = g_random_int_range(0, count);
            char *path = api_favorite_path_by_index(idx);
            if (path) {
                /* 从本地文件加载 */
                GMappedFile *mf = g_mapped_file_new(path, FALSE, NULL);
                if (mf) {
                    img = g_bytes_new_with_free_func(
                        g_mapped_file_get_contents(mf),
                        g_mapped_file_get_length(mf),
                        (GDestroyNotify)g_mapped_file_unref, mf);
                }

                /* 从文件名提取标题 */
                char *basename = g_path_get_basename(path);
                char *dot = strrchr(basename, '.');
                if (dot) *dot = '\0';

                meta = g_new0(ImageData, 1);
                meta->title   = g_strdup(basename);
                meta->imgurl  = path;  /* path 所有权转移 */
                meta->reldate = g_strdup("本地收藏");
                g_free(basename);
            } else {
                meta = g_new0(ImageData, 1);
                meta->title  = g_strdup("加载失败");
                meta->imgurl = g_strdup("");
            }
        }
    } else {
        /* 在线图源 */
        meta = api_random(state->provider);
        if (meta)
            img = api_download_image(meta->imgurl);
    }

    if (!meta) {
        g_idle_add(on_slot_loaded, task);
        return NULL;
    }

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
/* ---- 图源显示名 ---- */
static const char *PROVIDER_NAMES[] = {
    /* 官方 */
    "拾光", "周度精选", "故纸堆", "Windows 聚焦",
    /* 收藏 */
    "我的收藏",
    /* 三方 */
    "必应", "NASA", "ONE · 一个", "wallhaven",
    "Unsplash", "一梦幽黎", "彼岸图网", "故宫博物院",
    "NASA Images", "轻壁纸", "极简壁纸", "壁纸社",
    "壁纸汇", "WallHere", "花猫壁纸", "3G 壁纸",
    "WallpaperUP", "colorhub", "Pexels", "极简壁纸2",
    "Simple Desktops", "蔚蓝主页", "Wallpaper Abyss",
    "pixiv", "Pixabay", "backiee", "ESO",
    "Skitterphoto", "乌云壁纸", "公元桌面",
};
#define OFFICIAL_COUNT 4   /* timeline, glutton, snake, spotlight */

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
    gtk_widget_grab_focus(state->active_is_b
        ? state->picture_b : state->picture_a);
}

/* ================================================================
 *  右键菜单
 * ================================================================ */
static void on_about_activate(GSimpleAction *action, GVariant *param, AppState *state) {
    const char *authors[] = {
        "南瓜多糖 — 拾光原作者，提供图片与 API",
        "Populus  — Linux 桌面客户端移植",
        NULL
    };

    GtkWidget *about = gtk_about_dialog_new();
    gtk_about_dialog_set_program_name(GTK_ABOUT_DIALOG(about), "拾光");
    gtk_about_dialog_set_version(GTK_ABOUT_DIALOG(about), "0.1.0");
    gtk_about_dialog_set_comments(GTK_ABOUT_DIALOG(about),
        "Linux 第三方桌面客户端\n"
        "非官方版本，仅是对原 Windows UWP 版的移植");
    gtk_about_dialog_set_authors(GTK_ABOUT_DIALOG(about), authors);
    gtk_about_dialog_set_website(GTK_ABOUT_DIALOG(about),
        "https://gallery.timeline.ink/");
    gtk_about_dialog_set_website_label(GTK_ABOUT_DIALOG(about),
        "拾光官方网站");
    gtk_about_dialog_set_license_type(GTK_ABOUT_DIALOG(about), GTK_LICENSE_MIT_X11);
    gtk_window_set_transient_for(GTK_WINDOW(about), GTK_WINDOW(state->window));
    gtk_window_present(GTK_WINDOW(about));
}

/* ---- 下载任务（线程间传递）---- */
typedef struct {
    char   *url;
    char   *filepath;
    GtkWindow *parent;
} SaveTask;

static void save_task_free(SaveTask *t) {
    if (!t) return;
    g_free(t->url);
    g_free(t->filepath);
    g_free(t);
}

static gboolean on_save_done(gpointer user_data) {
    SaveTask *task = user_data;
    int ok = GPOINTER_TO_INT(
        g_object_get_data(G_OBJECT(task->parent), "save-ok"));

    if (ok) {
        GtkAlertDialog *dlg = gtk_alert_dialog_new("保存成功");
        gtk_alert_dialog_set_detail(dlg, task->filepath);
        gtk_alert_dialog_show(dlg, task->parent);
        g_object_unref(dlg);
    } else {
        GtkAlertDialog *dlg = gtk_alert_dialog_new("保存失败");
        gtk_alert_dialog_set_detail(dlg, "下载失败，请检查网络连接");
        gtk_alert_dialog_show(dlg, task->parent);
        g_object_unref(dlg);
    }
    save_task_free(task);
    return G_SOURCE_REMOVE;
}

static gpointer save_worker(gpointer user_data) {
    SaveTask *task = user_data;
    int ok = api_download_to_file(task->url, task->filepath);

    g_object_set_data(G_OBJECT(task->parent), "save-ok",
                      GINT_TO_POINTER(ok));
    g_idle_add(on_save_done, task);
    return NULL;
}

static void on_save_response(GObject *source, GAsyncResult *result, gpointer user_data) {
    GtkFileDialog *dlg = GTK_FILE_DIALOG(source);
    GError *error = NULL;
    GFile *file = gtk_file_dialog_save_finish(dlg, result, &error);

    if (!file) {
        if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            g_warning("保存对话框错误: %s", error->message);
        g_clear_error(&error);
        return;
    }

    SaveTask *task = user_data;
    task->filepath = g_file_get_path(file);
    g_object_unref(file);

    g_debug("保存到: %s", task->filepath);
    g_thread_new("save-worker", save_worker, task);
}

/* ================================================================
 *  收藏 action
 * ================================================================ */
static gboolean on_fav_done(gpointer user_data) {
    SaveTask *task = user_data;
    int ok = GPOINTER_TO_INT(
        g_object_get_data(G_OBJECT(task->parent), "fav-ok"));
    gboolean already = GPOINTER_TO_INT(
        g_object_get_data(G_OBJECT(task->parent), "fav-already"));

    if (already) {
        GtkAlertDialog *dlg = gtk_alert_dialog_new("已收藏");
        gtk_alert_dialog_set_detail(dlg, "该图片已在收藏夹中");
        gtk_alert_dialog_show(dlg, task->parent);
        g_object_unref(dlg);
    } else if (ok) {
        GtkAlertDialog *dlg = gtk_alert_dialog_new("收藏成功");
        gtk_alert_dialog_show(dlg, task->parent);
        g_object_unref(dlg);
    } else {
        GtkAlertDialog *dlg = gtk_alert_dialog_new("收藏失败");
        gtk_alert_dialog_set_detail(dlg, "下载失败，请检查网络连接");
        gtk_alert_dialog_show(dlg, task->parent);
        g_object_unref(dlg);
    }
    save_task_free(task);
    return G_SOURCE_REMOVE;
}

static gpointer fav_worker(gpointer user_data) {
    SaveTask *task = user_data;
    int ok = api_save_favorite(task->url, task->filepath ? task->filepath : "image");
    g_object_set_data(G_OBJECT(task->parent), "fav-ok",
                      GINT_TO_POINTER(ok));
    g_idle_add(on_fav_done, task);
    return NULL;
}

/* ================================================================
 *  设为壁纸
 * ================================================================ */
#define WALLPAPER_DIR  ".cache/shiguang"
#define WALLPAPER_FILE "wallpaper"

static char *save_current_to_cache(AppState *state) {
    g_mutex_lock(&state->mutex);
    ImageEntry *e = g_ptr_array_index(state->queue, state->cursor);
    if (!e->image) {
        g_mutex_unlock(&state->mutex);
        return NULL;
    }
    GBytes *bytes = g_bytes_ref(e->image);  /* 持有引用，跨锁安全 */
    g_mutex_unlock(&state->mutex);

    const char *home = g_get_home_dir();
    char *dir = g_build_filename(home, WALLPAPER_DIR, NULL);
    g_mkdir_with_parents(dir, 0755);

    char *path = g_build_filename(dir, WALLPAPER_FILE, NULL);
    g_free(dir);

    gsize size;
    const guint8 *data = g_bytes_get_data(bytes, &size);

    GError *error = NULL;
    gboolean ok = g_file_set_contents(path, (const char *)data, size, &error);
    g_bytes_unref(bytes);

    if (!ok) {
        g_warning("写入壁纸缓存失败: %s", error->message);
        g_clear_error(&error);
        g_free(path);
        return NULL;
    }

    g_debug("壁纸缓存: %s (%lu 字节)", path, (unsigned long)size);
    return path;
}

static gboolean set_wallpaper(const char *filepath, GtkWindow *parent) {
    const char *desktop = g_getenv("XDG_CURRENT_DESKTOP");
    g_debug("当前桌面: %s", desktop ? desktop : "(未知)");

    /* GNOME */
    if (desktop && strstr(desktop, "GNOME")) {
        char *uri = g_strdup_printf("file://%s", filepath);

        /* 明亮模式 */
        GSettings *s = g_settings_new("org.gnome.desktop.background");
        g_settings_set_string(s, "picture-uri", uri);

        /* 暗色模式（可能不存在，忽略错误）*/
        g_settings_set_string(s, "picture-uri-dark", uri);

        g_settings_sync();
        g_object_unref(s);
        g_free(uri);

        GtkAlertDialog *dlg = gtk_alert_dialog_new("壁纸已更新");
        gtk_alert_dialog_show(dlg, parent);
        g_object_unref(dlg);
        return TRUE;
    }

    /* KDE Plasma */
    if (desktop && strstr(desktop, "KDE")) {
        char *uri = g_strdup_printf("file://%s", filepath);
        char *cmd  = g_strdup_printf("plasma-apply-wallpaperimage \"%s\"", uri);

        int ret = system(cmd);
        g_free(cmd);
        g_free(uri);

        GtkAlertDialog *dlg = gtk_alert_dialog_new(ret == 0 ? "壁纸已更新" : "设置失败");
        if (ret != 0)
            gtk_alert_dialog_set_detail(dlg, "请确认 plasma-apply-wallpaperimage 可用");
        gtk_alert_dialog_show(dlg, parent);
        g_object_unref(dlg);
        return ret == 0;
    }

    /* XFCE */
    if (desktop && strstr(desktop, "XFCE")) {
        char *cmd = g_strdup_printf(
            "xfconf-query -c xfce4-desktop "
            "-p /backdrop/screen0/monitor0/workspace0/last-image "
            "-s \"%s\"", filepath);
        int ret = system(cmd);
        g_free(cmd);

        GtkAlertDialog *dlg = gtk_alert_dialog_new(ret == 0 ? "壁纸已更新" : "设置失败");
        if (ret != 0)
            gtk_alert_dialog_set_detail(dlg, "请确认 xfconf-query 可用");
        gtk_alert_dialog_show(dlg, parent);
        g_object_unref(dlg);
        return ret == 0;
    }

    /* 未知桌面 */
    char *msg = g_strdup_printf(
        "当前桌面 (%s) 尚不支持自动设置\n"
        "壁纸已保存到:\n%s\n请手动设置",
        desktop ? desktop : "未知", filepath);
    GtkAlertDialog *dlg = gtk_alert_dialog_new("提示");
    gtk_alert_dialog_set_detail(dlg, msg);
    gtk_alert_dialog_show(dlg, parent);
    g_object_unref(dlg);
    g_free(msg);
    return FALSE;
}

static void on_wallpaper_activate(GSimpleAction *action, GVariant *param, AppState *state) {
    char *path = save_current_to_cache(state);
    if (!path) {
        GtkAlertDialog *dlg = gtk_alert_dialog_new("无法设置壁纸");
        gtk_alert_dialog_set_detail(dlg, "当前图片尚未加载完成");
        gtk_alert_dialog_show(dlg, GTK_WINDOW(state->window));
        g_object_unref(dlg);
        return;
    }
    set_wallpaper(path, GTK_WINDOW(state->window));
    g_free(path);
}

static void on_favorite_activate(GSimpleAction *action, GVariant *param, AppState *state) {
    g_mutex_lock(&state->mutex);
    if (!state->queue || state->queue->len == 0) {
        g_mutex_unlock(&state->mutex);
        return;
    }
    ImageEntry *e = g_ptr_array_index(state->queue, state->cursor);
    if (!e->meta || !e->meta->title || !e->meta->imgurl || !*e->meta->imgurl) {
        g_mutex_unlock(&state->mutex);
        return;
    }

    /* 本地收藏无需重复下载 */
    if (g_strcmp0(state->provider, "favorites") == 0) {
        g_mutex_unlock(&state->mutex);
        GtkAlertDialog *dlg = gtk_alert_dialog_new("已在收藏夹");
        gtk_alert_dialog_set_detail(dlg, "当前浏览的已是本地收藏图片");
        gtk_alert_dialog_show(dlg, GTK_WINDOW(state->window));
        g_object_unref(dlg);
        return;
    }

    /* 检查重复 */
    if (api_is_favorited(e->meta->title)) {
        g_mutex_unlock(&state->mutex);
        GtkAlertDialog *dlg = gtk_alert_dialog_new("已收藏");
        gtk_alert_dialog_set_detail(dlg, "该图片已在收藏夹中");
        gtk_alert_dialog_show(dlg, GTK_WINDOW(state->window));
        g_object_unref(dlg);
        return;
    }

    SaveTask *task = g_new0(SaveTask, 1);
    task->url      = g_strdup(e->meta->imgurl);
    task->filepath = g_strdup(e->meta->title);
    task->parent   = GTK_WINDOW(state->window);
    g_mutex_unlock(&state->mutex);

    g_object_set_data(G_OBJECT(task->parent), "fav-already",
                      GINT_TO_POINTER(FALSE));
    g_thread_new("fav-worker", fav_worker, task);
}

static void on_save_activate(GSimpleAction *action, GVariant *param, AppState *state) {
    /* 获取当前图片信息 */
    g_mutex_lock(&state->mutex);
    if (!state->queue || state->queue->len == 0) {
        g_mutex_unlock(&state->mutex);
        return;
    }
    ImageEntry *e = g_ptr_array_index(state->queue, state->cursor);
    if (!e->meta || !e->meta->imgurl || !*e->meta->imgurl) {
        g_mutex_unlock(&state->mutex);
        return;
    }

    SaveTask *task = g_new0(SaveTask, 1);
    task->url    = g_strdup(e->meta->imgurl);
    task->parent = GTK_WINDOW(state->window);

    /* 用图片标题作为默认文件名，清理非法字符 */
    const char *title = (e->meta->title && *e->meta->title)
                      ? e->meta->title : "image";

    /* 确定扩展名 */
    const char *ext = strrchr(e->meta->imgurl, '.');
    if (ext) {
        const char *q = strchr(ext, '?');
        size_t len = q ? (size_t)(q - ext) : strlen(ext);
        char *name = g_strdup_printf("%s%.*s", title, (int)len, ext);
        /* 清理文件名中的非法字符 */
        for (char *p = name; *p; p++)
            if (*p == '/' || *p == '\\' || *p == ':') *p = '_';
        task->filepath = name;
    } else {
        task->filepath = g_strdup_printf("%s.jpg", title);
        for (char *p = task->filepath; *p; p++)
            if (*p == '/' || *p == '\\' || *p == ':') *p = '_';
    }

    g_mutex_unlock(&state->mutex);

    GtkFileDialog *dlg = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dlg, "保存原图");
    gtk_file_dialog_set_initial_name(dlg, task->filepath);
    g_free(task->filepath);
    task->filepath = NULL;

    gtk_file_dialog_save(dlg, GTK_WINDOW(state->window), NULL,
                         on_save_response, task);
    g_object_unref(dlg);
}

static gboolean on_window_close(GtkWindow *win, AppState *state) {
    g_debug("窗口关闭，退出应用");
    g_application_quit(g_application_get_default());
    return TRUE; /* 阻止默认关闭行为，由 quit 统一处理 */
}

static void on_quit_activate(GSimpleAction *action, GVariant *param, AppState *state) {
    g_application_quit(g_application_get_default());
}

/* ---- 右键手势 ---- */
static void on_right_click(GtkGestureClick *gesture, int n_press,
                           double x, double y, AppState *state) {
    GMenu *menu = g_menu_new();
    g_menu_append(menu, "设为壁纸",    "app.wallpaper");
    g_menu_append(menu, "收藏",       "app.favorite");
    g_menu_append(menu, "下载原图…",  "app.save");
    g_menu_append(menu, "设置…",      "app.settings");
    g_menu_append(menu, "关于",       "app.about");
    g_menu_append(menu, "退出",       "app.quit");

    GtkWidget *pop = gtk_popover_menu_new_from_model(G_MENU_MODEL(menu));
    gtk_popover_set_pointing_to(GTK_POPOVER(pop), &(GdkRectangle){x, y, 1, 1});
    gtk_popover_set_has_arrow(GTK_POPOVER(pop), FALSE);
    gtk_widget_set_parent(pop, state->stack);
    gtk_popover_popup(GTK_POPOVER(pop));
    g_object_unref(menu);
}

/* ================================================================
 *  设置对话框
 * ================================================================ */
/* ---- 侧边栏：应用设置并重新加载 ---- */
/* ---- 公共：切换到指定图源 ---- */
static void switch_provider(AppState *state, const char *provider) {
    g_free(state->provider);
    state->provider = g_strdup(provider);
    g_debug("切换图源: %s", provider);

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
    gtk_picture_set_paintable(GTK_PICTURE(state->picture_a), NULL);
    gtk_picture_set_paintable(GTK_PICTURE(state->picture_b), NULL);

    state->fwd_busy = TRUE;
    LoadTask *fwd = g_new0(LoadTask, 1);
    fwd->state = state; fwd->slot = WINDOW_HALF; fwd->side = -2;
    g_thread_new("fwd", load_one, fwd);

    state->bwd_busy = TRUE;
    LoadTask *bwd = g_new0(LoadTask, 1);
    bwd->state = state; bwd->slot = WINDOW_HALF - 1; bwd->side = -1;
    g_thread_new("bwd", load_one, bwd);
}

/* ---- 点击图源按钮 ---- */
static void on_provider_btn(GtkButton *btn, AppState *state) {
    const char *provider = g_object_get_data(G_OBJECT(btn), "provider");
    switch_provider(state, provider);
    gtk_revealer_set_reveal_child(
        GTK_REVEALER(state->sidebar_revealer), FALSE);
}

/* ---- 一言 ---- */
static size_t hitokoto_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    g_string_append_len((GString *)userdata, ptr, size * nmemb);
    return size * nmemb;
}

static char *api_fetch_hitokoto(void) {
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;

    GString *buf = g_string_new(NULL);

    curl_easy_setopt(curl, CURLOPT_URL, "https://glitter.timeline.ink/api/v1?json=1");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, hitokoto_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        g_string_free(buf, TRUE);
        return NULL;
    }

    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, buf->str, buf->len, NULL)) {
        g_string_free(buf, TRUE);
        g_object_unref(parser);
        return NULL;
    }
    g_string_free(buf, TRUE);

    JsonReader *r = json_reader_new(json_parser_get_root(parser));
    char *content = NULL;
    if (json_reader_read_member(r, "data") &&
        json_reader_read_element(r, 0) &&
        json_reader_read_member(r, "sentence")) {
        content = g_strdup(json_reader_get_string_value(r) ?: "");
    }
    g_object_unref(r);
    g_object_unref(parser);
    return content;
}

typedef struct { AppState *state; char *text; } HitokotoTask;

static gboolean on_hitokoto_loaded(gpointer data) {
    HitokotoTask *t = data;
    if (t->text)
        gtk_label_set_text(GTK_LABEL(t->state->label_hitokoto), t->text);
    g_free(t->text);
    g_free(t);
    return G_SOURCE_REMOVE;
}

static gpointer hitokoto_thread(gpointer data) {
    HitokotoTask *t = data;
    t->text = api_fetch_hitokoto();
    g_idle_add(on_hitokoto_loaded, t);
    return NULL;
}

/* ---- 侧边栏：关闭 ---- */
static void on_sidebar_close(GtkButton *btn, AppState *state) {
    gtk_revealer_set_reveal_child(
        GTK_REVEALER(state->sidebar_revealer), FALSE);
}

/* ---- 创建图源按钮 ---- */
static GtkWidget *provider_button(const char *id, const char *label, AppState *state) {
    GtkWidget *btn = gtk_button_new_with_label(label);
    g_object_set_data_full(G_OBJECT(btn), "provider", g_strdup(id), g_free);
    g_signal_connect(btn, "clicked", G_CALLBACK(on_provider_btn), state);
    return btn;
}

/* ---- 右键菜单：弹出侧边栏 ---- */
static void on_settings_activate(GSimpleAction *action, GVariant *param,
                                  AppState *state) {
    /* 触发一言加载 */
    HitokotoTask *t = g_new0(HitokotoTask, 1);
    t->state = state;
    g_thread_new("hitokoto", hitokoto_thread, t);

    gtk_revealer_set_reveal_child(
        GTK_REVEALER(state->sidebar_revealer), TRUE);
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
    g_mutex_init(&state->mutex);

    /* ---- 窗口 ---- */
    state->window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(state->window), "拾光（第三方）");
    gtk_window_set_default_size(GTK_WINDOW(state->window), 1024, 768);
    g_signal_connect(state->window, "close-request",
                     G_CALLBACK(on_window_close), state);


    /* ---- 双图 Stack（crossfade 过渡）---- */
    state->picture_a = gtk_picture_new();
    gtk_picture_set_content_fit(GTK_PICTURE(state->picture_a), GTK_CONTENT_FIT_CONTAIN);

    state->picture_b = gtk_picture_new();
    gtk_picture_set_content_fit(GTK_PICTURE(state->picture_b), GTK_CONTENT_FIT_CONTAIN);

    state->stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(state->stack),
                                   GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_stack_set_transition_duration(GTK_STACK(state->stack), 400);
    gtk_stack_add_named(GTK_STACK(state->stack), state->picture_a, "a");
    gtk_stack_add_named(GTK_STACK(state->stack), state->picture_b, "b");
    gtk_stack_set_visible_child_name(GTK_STACK(state->stack), "a");
    state->active_is_b = FALSE;
    gtk_widget_set_hexpand(state->stack, TRUE);
    gtk_widget_set_vexpand(state->stack, TRUE);

    /* ---- Slogan（顶部居中）---- */
    state->overlay_slogan = gtk_label_new("时光如歌，岁月如诗");
    gtk_widget_set_halign(state->overlay_slogan, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(state->overlay_slogan, GTK_ALIGN_START);
    gtk_widget_set_margin_top(state->overlay_slogan, 48);
    gtk_widget_add_css_class(state->overlay_slogan, "overlay-slogan");

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

    /* 图片区 Overlay: 底=图片, 上=文字+spinner */
    GtkWidget *pic_overlay = gtk_overlay_new();
    gtk_overlay_set_child(GTK_OVERLAY(pic_overlay), state->stack);
    gtk_overlay_add_overlay(GTK_OVERLAY(pic_overlay), state->overlay_slogan);
    gtk_overlay_add_overlay(GTK_OVERLAY(pic_overlay), info_box);
    gtk_overlay_add_overlay(GTK_OVERLAY(pic_overlay), state->spinner);

    /* ---- 左侧侧边栏 ---- */
    GtkWidget *sidebar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_size_request(sidebar, 240, -1);
    gtk_widget_set_valign(sidebar, GTK_ALIGN_FILL);
    gtk_widget_set_margin_top(sidebar, 12);
    gtk_widget_set_margin_bottom(sidebar, 12);
    gtk_widget_add_css_class(sidebar, "sidebar");

    /* 标题 */
    GtkWidget *s_title = gtk_label_new("图源");
    gtk_widget_set_halign(s_title, GTK_ALIGN_START);
    gtk_widget_set_margin_start(s_title, 12);
    gtk_widget_set_margin_end(s_title, 12);
    gtk_widget_add_css_class(s_title, "sidebar-title");

    /* 可滚动区域 */
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

    GtkWidget *btnlist = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_start(btnlist, 8);
    gtk_widget_set_margin_end(btnlist, 8);

    /* 官方标签 */
    GtkWidget *lbl_official = gtk_label_new("拾光官方");
    gtk_widget_set_halign(lbl_official, GTK_ALIGN_START);
    gtk_widget_set_margin_start(lbl_official, 4);
    gtk_widget_set_margin_top(lbl_official, 4);
    gtk_widget_add_css_class(lbl_official, "sidebar-label");
    gtk_box_append(GTK_BOX(btnlist), lbl_official);

    /* 官方按钮 */
    for (int i = 0; i < OFFICIAL_COUNT; i++)
        gtk_box_append(GTK_BOX(btnlist),
            provider_button(PROVIDERS[i], PROVIDER_NAMES[i], state));

    /* 收藏按钮 */
    gtk_box_append(GTK_BOX(btnlist),
        provider_button(PROVIDERS[OFFICIAL_COUNT], PROVIDER_NAMES[OFFICIAL_COUNT], state));

    /* 分隔线 */
    GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_margin_top(sep, 8);
    gtk_widget_set_margin_bottom(sep, 4);
    gtk_box_append(GTK_BOX(btnlist), sep);

    /* 三方标签 */
    GtkWidget *lbl_3rd = gtk_label_new("三方图源");
    gtk_widget_set_halign(lbl_3rd, GTK_ALIGN_START);
    gtk_widget_set_margin_start(lbl_3rd, 4);
    gtk_widget_add_css_class(lbl_3rd, "sidebar-label");
    gtk_box_append(GTK_BOX(btnlist), lbl_3rd);

    /* 三方按钮 */
    for (int i = OFFICIAL_COUNT + 1; i < PROVIDER_COUNT; i++)
        gtk_box_append(GTK_BOX(btnlist),
            provider_button(PROVIDERS[i], PROVIDER_NAMES[i], state));

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), btnlist);

    /* 一言 */
    state->label_hitokoto = gtk_label_new("加载中…");
    gtk_widget_set_margin_start(state->label_hitokoto, 12);
    gtk_widget_set_margin_end(state->label_hitokoto, 12);
    gtk_label_set_wrap(GTK_LABEL(state->label_hitokoto), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(state->label_hitokoto), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars(GTK_LABEL(state->label_hitokoto), 22);
    gtk_widget_add_css_class(state->label_hitokoto, "sidebar-hitokoto");

    /* 关闭按钮 */
    GtkWidget *btn_close = gtk_button_new_with_label("关闭");
    gtk_widget_set_margin_start(btn_close, 8);
    gtk_widget_set_margin_end(btn_close, 8);
    gtk_widget_set_margin_bottom(btn_close, 4);
    g_signal_connect(btn_close, "clicked", G_CALLBACK(on_sidebar_close), state);

    gtk_box_append(GTK_BOX(sidebar), s_title);
    gtk_box_append(GTK_BOX(sidebar), scroll);
    gtk_box_append(GTK_BOX(sidebar), state->label_hitokoto);
    gtk_box_append(GTK_BOX(sidebar), btn_close);

    /* Revealer：从左滑出 */
    state->sidebar_revealer = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(state->sidebar_revealer),
                                     GTK_REVEALER_TRANSITION_TYPE_SLIDE_RIGHT);
    gtk_revealer_set_transition_duration(GTK_REVEALER(state->sidebar_revealer), 250);
    gtk_revealer_set_child(GTK_REVEALER(state->sidebar_revealer), sidebar);
    gtk_revealer_set_reveal_child(GTK_REVEALER(state->sidebar_revealer), FALSE);

    /* ---- 水平布局: [侧边栏] [图片区] ---- */
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_append(GTK_BOX(hbox), state->sidebar_revealer);
    gtk_box_append(GTK_BOX(hbox), pic_overlay);
    gtk_window_set_child(GTK_WINDOW(state->window), hbox);

    /* ---- CSS ---- */
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css,
        ".overlay-slogan {"
        "  font-size: 15px;"
        "  color: rgba(255,255,255,0.5);"
        "  text-shadow: 0 1px 3px rgba(0,0,0,0.6);"
        "  font-style: italic;"
        "}"
        ".overlay-title {"
        "  font-size: 18px; font-weight: bold;"
        "  color: white;"
        "  text-shadow: 0 1px 3px rgba(0,0,0,0.8);"
        "}"
        ".overlay-info {"
        "  font-size: 13px;"
        "  color: rgba(255,255,255,0.85);"
        "  text-shadow: 0 1px 3px rgba(0,0,0,0.7);"
        "}"
        ".sidebar {"
        "  background-color: rgba(255,255,255,0.85);"
        "  border-right: 1px solid rgba(0,0,0,0.1);"
        "}"
        ".sidebar-title {"
        "  font-size: 20px; font-weight: bold;"
        "  color: black;"
        "}"
        ".sidebar-hitokoto {"
        "  font-size: 13px;"
        "  color: rgba(0,0,0,0.7);"
        "  font-style: italic;"
        "  padding: 8px 0;"
        "  border-top: 1px solid rgba(0,0,0,0.1);"
        "  border-bottom: 1px solid rgba(0,0,0,0.1);"
        "}"
        ".sidebar-label {"
        "  font-size: 12px; font-weight: bold;"
        "  color: rgba(0,0,0,0.55);"
        "  text-transform: uppercase; letter-spacing: 1px;"
        "}"
        ".sidebar checkbutton {"
        "  color: black;"
        "}"
        ".sidebar checkbutton check {"
        "  background-color: rgba(0,0,0,0.08);"
        "  border-color: rgba(0,0,0,0.3);"
        "}"
        ".sidebar dropdown > button {"
        "  color: black;"
        "}"
        ".sidebar button {"
        "  color: black;"
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
    gtk_widget_add_controller(state->stack, scroll_ctrl);

    /* ---- 左键点击：获取焦点，使键盘生效 ---- */
    GtkEventController *left_click = GTK_EVENT_CONTROLLER(gtk_gesture_click_new());
    g_signal_connect(left_click, "pressed", G_CALLBACK(on_left_click), state);
    gtk_widget_add_controller(state->stack, left_click);

    /* ---- 右键手势 ---- */
    GtkEventController *right_click = GTK_EVENT_CONTROLLER(gtk_gesture_click_new());
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(right_click), GDK_BUTTON_SECONDARY);
    g_signal_connect(right_click, "pressed", G_CALLBACK(on_right_click), state);
    gtk_widget_add_controller(state->window, right_click);

    /* ---- App Actions ---- */
    GSimpleActionGroup *actions = g_simple_action_group_new();
    GSimpleAction *act;
    act = g_simple_action_new("wallpaper", NULL);
    g_signal_connect(act, "activate", G_CALLBACK(on_wallpaper_activate), state);
    g_action_map_add_action(G_ACTION_MAP(actions), G_ACTION(act));
    act = g_simple_action_new("favorite", NULL);
    g_signal_connect(act, "activate", G_CALLBACK(on_favorite_activate), state);
    g_action_map_add_action(G_ACTION_MAP(actions), G_ACTION(act));
    act = g_simple_action_new("save", NULL);
    g_signal_connect(act, "activate", G_CALLBACK(on_save_activate), state);
    g_action_map_add_action(G_ACTION_MAP(actions), G_ACTION(act));
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

/* GResource 注册（由 glib-compile-resources 生成）*/
extern GResource *icon_get_resource(void);

int main(int argc, char **argv) {
    curl_global_init(CURL_GLOBAL_ALL);

    g_resources_register(icon_get_resource());

    GtkApplication *app = gtk_application_new("io.github.populusyang.shiguang",
                                               G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);

    curl_global_cleanup();
    return status;
}
