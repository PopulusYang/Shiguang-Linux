#include <gtk/gtk.h>
#include <gdk/gdk.h>
#include "api.h"

/* ---- 应用全局状态 ---- */
typedef struct {
    GtkWidget *window;
    GtkWidget *picture;
    GtkWidget *label_title;
    GtkWidget *label_info;
    GtkWidget *spinner;
    GtkWidget *btn_today;
    GtkWidget *btn_random;
    GtkDropDown *dropdown;
    char      *provider;
} AppState;

/* ---- 图片加载结果（线程间传递）---- */
typedef struct {
    AppState  *state;
    GBytes    *image_bytes;
    char      *title;
    char      *copyright;
    char      *reldate;
    char      *error;
} LoadResult;

static void load_result_free(LoadResult *r) {
    if (!r) return;
    if (r->image_bytes) g_bytes_unref(r->image_bytes);
    g_free(r->title);
    g_free(r->copyright);
    g_free(r->reldate);
    g_free(r->error);
    g_free(r);
}

/* ---- 在当前 provider 上执行请求 ---- */
static ImageData *load_image_data(AppState *state, gboolean random) {
    if (random)
        return api_random(state->provider);
    else
        return api_today(state->provider);
}

/* ---- 工作线程：网络请求 ---- */
static gpointer worker_thread(gpointer user_data) {
    LoadResult *result = g_new0(LoadResult, 1);
    result->state = user_data;

    gboolean is_random = GPOINTER_TO_INT(
        g_object_get_data(G_OBJECT(result->state->btn_random), "is-random"));

    /* 1. 请求 API */
    ImageData *img = load_image_data(result->state, is_random);
    if (!img) {
        result->error = g_strdup("API 请求失败，请检查网络");
        return result;
    }

    /* 2. 下载缩略图 */
    result->image_bytes = api_download_image(img->thumburl);

    /* 3. 拷贝文本数据 */
    result->title     = g_strdup(img->title);
    result->copyright = g_strdup(img->copyright);
    result->reldate   = g_strdup(img->reldate);

    api_image_free(img);
    return result;
}

/* ---- 主线程：更新 UI（由 g_idle_add 回调）---- */
static gboolean on_loaded(gpointer user_data) {
    LoadResult *r = user_data;
    AppState *state = r->state;

    /* 停止 spinner */
    gtk_spinner_stop(GTK_SPINNER(state->spinner));
    gtk_widget_set_visible(state->spinner, FALSE);

    /* 恢复按钮 */
    gtk_widget_set_sensitive(state->btn_today, TRUE);
    gtk_widget_set_sensitive(state->btn_random, TRUE);
    gtk_widget_set_sensitive(GTK_WIDGET(state->dropdown), TRUE);

    if (r->error) {
        gtk_label_set_text(GTK_LABEL(state->label_title), r->error);
        gtk_label_set_text(GTK_LABEL(state->label_info), "");
        gtk_picture_set_paintable(GTK_PICTURE(state->picture), NULL);
        load_result_free(r);
        return G_SOURCE_REMOVE;
    }

    /* 更新图片 */
    if (r->image_bytes) {
        GdkTexture *tex = gdk_texture_new_from_bytes(r->image_bytes, NULL);
        if (tex) {
            gtk_picture_set_paintable(GTK_PICTURE(state->picture),
                                      GDK_PAINTABLE(tex));
            g_object_unref(tex);
        }
    }

    /* 更新文字 */
    gtk_label_set_text(GTK_LABEL(state->label_title),
                       r->title && *r->title ? r->title : "（无标题）");

    char *info = g_strdup_printf("%s  |  %s",
        r->copyright && *r->copyright ? r->copyright : "版权未知",
        r->reldate   && *r->reldate   ? r->reldate   : "");
    gtk_label_set_text(GTK_LABEL(state->label_info), info);
    g_free(info);

    load_result_free(r);
    return G_SOURCE_REMOVE;
}

/* ---- 触发加载 ---- */
static void start_load(AppState *state, gboolean is_random) {
    /* 禁用按钮防重复点击 */
    gtk_widget_set_sensitive(state->btn_today, FALSE);
    gtk_widget_set_sensitive(state->btn_random, FALSE);
    gtk_widget_set_sensitive(GTK_WIDGET(state->dropdown), FALSE);

    /* 显示 spinner */
    gtk_widget_set_visible(state->spinner, TRUE);
    gtk_spinner_start(GTK_SPINNER(state->spinner));

    /* 标记是否随机 */
    g_object_set_data(G_OBJECT(state->btn_random), "is-random",
                      GINT_TO_POINTER(is_random));

    g_thread_new("image-loader", worker_thread, state);
}

/* ---- 回调：点击今日 ---- */
static void on_today(GtkButton *btn, AppState *state) {
    start_load(state, FALSE);
}

/* ---- 回调：点击随机 ---- */
static void on_random(GtkButton *btn, AppState *state) {
    start_load(state, TRUE);
}

/* ---- 回调：切换图源 ---- */
static void on_provider_changed(GtkDropDown *dd, GParamSpec *pspec, AppState *state) {
    guint idx = gtk_drop_down_get_selected(dd);
    if (idx < PROVIDER_COUNT) {
        g_free(state->provider);
        state->provider = g_strdup(PROVIDERS[idx]);
        start_load(state, FALSE); /* 自动加载今日 */
    }
}

/* ---- 激活应用 ---- */
static void on_activate(GtkApplication *app, gpointer user_data) {
    AppState *state = g_new0(AppState, 1);
    state->provider = g_strdup(PROVIDERS[0]);

    /* ---- 窗口 ---- */
    state->window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(state->window), "拾光 — 每日一图");
    gtk_window_set_default_size(GTK_WINDOW(state->window), 600, 700);

    /* ---- 主布局 ---- */
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(vbox, 12);
    gtk_widget_set_margin_end(vbox, 12);
    gtk_widget_set_margin_top(vbox, 12);
    gtk_widget_set_margin_bottom(vbox, 12);

    /* ---- 顶部控制栏 ---- */
    GtkWidget *ctlbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

    /* Provider 下拉 */
    GtkStringList *providers = gtk_string_list_new((const char * const *)PROVIDERS);
    state->dropdown = GTK_DROP_DOWN(gtk_drop_down_new(G_LIST_MODEL(providers), NULL));
    gtk_widget_set_hexpand(GTK_WIDGET(state->dropdown), TRUE);
    g_signal_connect(state->dropdown, "notify::selected",
                     G_CALLBACK(on_provider_changed), state);

    /* Today 按钮 */
    state->btn_today = gtk_button_new_with_label("今日");
    g_signal_connect(state->btn_today, "clicked", G_CALLBACK(on_today), state);

    /* Random 按钮 */
    state->btn_random = gtk_button_new_with_label("随机");
    g_signal_connect(state->btn_random, "clicked", G_CALLBACK(on_random), state);

    /* Spinner */
    state->spinner = gtk_spinner_new();
    gtk_widget_set_size_request(state->spinner, 24, 24);
    gtk_widget_set_visible(state->spinner, FALSE);

    gtk_box_append(GTK_BOX(ctlbar), GTK_WIDGET(state->dropdown));
    gtk_box_append(GTK_BOX(ctlbar), state->btn_today);
    gtk_box_append(GTK_BOX(ctlbar), state->btn_random);
    gtk_box_append(GTK_BOX(ctlbar), state->spinner);

    /* ---- 图片显示区 ---- */
    state->picture = gtk_picture_new();
    gtk_widget_set_vexpand(state->picture, TRUE);
    gtk_widget_set_halign(state->picture, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(state->picture, GTK_ALIGN_CENTER);
    gtk_picture_set_can_shrink(GTK_PICTURE(state->picture), FALSE);
    gtk_picture_set_content_fit(GTK_PICTURE(state->picture), GTK_CONTENT_FIT_CONTAIN);

    /* 占位文字 */
    GtkWidget *placeholder = gtk_label_new("点击「今日」或「随机」加载图片");
    gtk_widget_set_halign(placeholder, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(placeholder, GTK_ALIGN_CENTER);

    /* 用 Overlay 把占位和 picture 叠起来 */
    GtkWidget *overlay = gtk_overlay_new();
    gtk_overlay_set_child(GTK_OVERLAY(overlay), placeholder);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), state->picture);

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

    /* ---- 加载 CSS ---- */
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css,
        ".title { font-size: 16px; font-weight: bold; margin-top: 8px; }"
        ".info  { font-size: 13px; color: @unfocused_insensitive_color; }");
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(),
        GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    /* ---- 显示 ---- */
    gtk_window_present(GTK_WINDOW(state->window));

    /* 启动时自动加载今日一图 */
    start_load(state, FALSE);
}

int main(int argc, char **argv) {
    GtkApplication *app = gtk_application_new("com.example.shiguang",
                                               G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
