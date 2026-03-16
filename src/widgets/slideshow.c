/*
 * slideshow.c — Venom Desktop Widget
 * Auto-Slideshow Image Viewer
 *
 * Features:
 *   - Scans ~/Pictures recursively for images (jpg/jpeg/png/gif/bmp/webp/tiff)
 *   - Auto-advance with configurable interval
 *   - Ken Burns effect (pan & zoom animation via Cairo)
 *   - Crossfade transition between images
 *   - Previous / Pause-Play / Next controls
 *   - Filename label + image counter
 *   - Right-click menu: Open folder, Set interval, Shuffle toggle
 *   - Drag & Drop support
 *
 * Config: ~/.config/venom/slideshow.conf
 *   path=/home/USER/Pictures
 *   interval=6          (seconds between slides)
 *   shuffle=1           (1=random, 0=sequential)
 *   show_filename=1
 *   kenburns=1
 *
 * Dependencies: gtk+-3.0, gdk-pixbuf-2.0 (bundled with GTK)
 *
 * Compile:
 *   gcc -shared -fPIC -o slideshow.so slideshow.c \
 *       $(pkg-config --cflags --libs gtk+-3.0) -lm \
 *       -I/path/to/venom-desktop/include
 *
 * Install:
 *   cp slideshow.so ~/.config/venom/widgets/
 */

#include <gtk/gtk.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "../../include/venom-widget-api.h"

/* ─── Dimensions ─── */
#define WIDGET_W      360
#define WIDGET_H      240
#define IMG_W         360
#define IMG_H         240

/* ─── Animation ─── */
#define FADE_STEPS     40       /* crossfade frames */
#define FADE_INTERVAL  30       /* ms per frame → ~750ms total */
#define KB_SCALE_START 1.00     /* Ken Burns start scale */
#define KB_SCALE_END   1.12     /* Ken Burns end scale   */

/* ─── Supported extensions ─── */
static const char *SUPPORTED_EXT[] = {
    ".jpg", ".jpeg", ".png", ".gif", ".bmp",
    ".webp", ".tiff", ".tif", ".svg", NULL
};

/* ══════════════════════════════════════════════
   State
   ══════════════════════════════════════════════ */
typedef struct {
    /* Config */
    char    path[512];
    gint    interval_sec;
    gboolean shuffle;
    gboolean show_filename;
    gboolean kenburns;

    /* File list */
    GPtrArray *files;       /* array of gchar* (full paths) */
    gint       current_idx;
    gint      *order;       /* shuffled index array */
    gint       order_len;

    /* Pixbufs */
    GdkPixbuf *pb_current;  /* displayed image (scaled) */
    GdkPixbuf *pb_next;     /* pre-loaded next image    */

    /* Crossfade */
    gboolean  fading;
    gint      fade_step;
    guint     fade_timer;

    /* Ken Burns */
    gdouble   kb_progress;  /* 0.0 → 1.0 over interval */
    gdouble   kb_start_x, kb_start_y;  /* pan origin (normalized -0.5..0.5) */
    gdouble   kb_end_x,   kb_end_y;

    /* Playback */
    gboolean  playing;
    guint     slide_timer;

    /* UI */
    GtkWidget *root_eb;
    GtkWidget *canvas;

    /* Drag */
    gboolean  dragging;
    gint      drag_rx, drag_ry, drag_wx, drag_wy;

    VenomDesktopAPI *api;
} SlideshowState;

static SlideshowState SS;

/* ══════════════════════════════════════════════
   Config
   ══════════════════════════════════════════════ */
static void load_config(void) {
    const char *home = getenv("HOME");
    if (!home) home = "/root";

    /* defaults */
    snprintf(SS.path, sizeof(SS.path), "%s/Pictures", home);
    SS.interval_sec  = 6;
    SS.shuffle       = TRUE;
    SS.show_filename = TRUE;
    SS.kenburns      = TRUE;

    char conf_path[600];
    snprintf(conf_path, sizeof(conf_path), "%s/.config/venom/slideshow.conf", home);
    FILE *f = fopen(conf_path, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = line, *val = eq + 1;
        if (!strcmp(key, "path"))          snprintf(SS.path, sizeof(SS.path), "%s", val);
        if (!strcmp(key, "interval"))      SS.interval_sec  = atoi(val);
        if (!strcmp(key, "shuffle"))       SS.shuffle       = atoi(val) != 0;
        if (!strcmp(key, "show_filename")) SS.show_filename = atoi(val) != 0;
        if (!strcmp(key, "kenburns"))      SS.kenburns      = atoi(val) != 0;
    }
    fclose(f);
}

/* ══════════════════════════════════════════════
   File scanning
   ══════════════════════════════════════════════ */
static gboolean is_image(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return FALSE;
    char lower[16];
    int i;
    for (i = 0; dot[i] && i < 15; i++)
        lower[i] = g_ascii_tolower(dot[i]);
    lower[i] = '\0';
    for (int j = 0; SUPPORTED_EXT[j]; j++)
        if (!strcmp(lower, SUPPORTED_EXT[j])) return TRUE;
    return FALSE;
}

/* Recursive directory scan */
static void scan_dir(const char *dir_path) {
    GError *err = NULL;
    GDir   *dir = g_dir_open(dir_path, 0, &err);
    if (!dir) { g_clear_error(&err); return; }
    const gchar *name;
    while ((name = g_dir_read_name(dir))) {
        if (name[0] == '.') continue;
        char full[1024];
        snprintf(full, sizeof(full), "%s/%s", dir_path, name);
        if (g_file_test(full, G_FILE_TEST_IS_DIR)) {
            scan_dir(full);
        } else if (is_image(name)) {
            g_ptr_array_add(SS.files, g_strdup(full));
        }
    }
    g_dir_close(dir);
}

static void build_file_list(void) {
    if (SS.files) {
        g_ptr_array_free(SS.files, TRUE);
    }
    SS.files = g_ptr_array_new_with_free_func(g_free);
    scan_dir(SS.path);

    /* Sort alphabetically */
    g_ptr_array_sort(SS.files, (GCompareFunc)g_strcmp0);
}

/* Build shuffle order using Fisher-Yates */
static void build_order(void) {
    g_free(SS.order);
    int n = SS.files ? (int)SS.files->len : 0;
    SS.order_len = n;
    if (n == 0) { SS.order = NULL; return; }
    SS.order = g_malloc(n * sizeof(gint));
    for (int i = 0; i < n; i++) SS.order[i] = i;
    if (SS.shuffle) {
        for (int i = n - 1; i > 0; i--) {
            int j = rand() % (i + 1);
            gint tmp = SS.order[i]; SS.order[i] = SS.order[j]; SS.order[j] = tmp;
        }
    }
}

/* ══════════════════════════════════════════════
   Image loading
   ══════════════════════════════════════════════ */

/* Scale pixbuf to cover IMG_W × IMG_H (crop to fill) */
static GdkPixbuf *scale_to_cover(GdkPixbuf *src, int tw, int th) {
    if (!src) return NULL;
    int sw = gdk_pixbuf_get_width(src);
    int sh = gdk_pixbuf_get_height(src);
    double sx = (double)tw / sw;
    double sy = (double)th / sh;
    double sc = (sx > sy) ? sx : sy;
    int nw = (int)(sw * sc);
    int nh = (int)(sh * sc);
    GdkPixbuf *scaled = gdk_pixbuf_scale_simple(src, nw, nh, GDK_INTERP_BILINEAR);
    /* Crop to center */
    int ox = (nw - tw) / 2;
    int oy = (nh - th) / 2;
    GdkPixbuf *cropped = gdk_pixbuf_new_subpixbuf(scaled, ox, oy, tw, th);
    GdkPixbuf *result  = gdk_pixbuf_copy(cropped);
    g_object_unref(scaled);
    g_object_unref(cropped);
    return result;
}

static GdkPixbuf *load_image(const char *path) {
    GError *err = NULL;
    /* Load at 2× for Ken Burns headroom */
    GdkPixbuf *raw = gdk_pixbuf_new_from_file_at_scale(
        path,
        (int)(IMG_W * KB_SCALE_END * 1.05),
        (int)(IMG_H * KB_SCALE_END * 1.05),
        TRUE, &err);
    if (!raw) { g_clear_error(&err); return NULL; }
    GdkPixbuf *cover = scale_to_cover(raw,
        (int)(IMG_W * KB_SCALE_END * 1.05),
        (int)(IMG_H * KB_SCALE_END * 1.05));
    g_object_unref(raw);
    return cover;
}

/* Randomize Ken Burns parameters for the next slide */
static void randomize_kb(void) {
    /* Random pan direction: corners / edges */
    SS.kb_start_x = ((rand() % 3) - 1) * 0.04;
    SS.kb_start_y = ((rand() % 3) - 1) * 0.04;
    SS.kb_end_x   = ((rand() % 3) - 1) * 0.04;
    SS.kb_end_y   = ((rand() % 3) - 1) * 0.04;
    SS.kb_progress = 0.0;
}

/* ══════════════════════════════════════════════
   Navigation
   ══════════════════════════════════════════════ */
static void update_labels(void) {
    /* labels removed — nothing to update */
    (void)0;
}

/* Forward declaration */
static void start_fade_to_next(void);

/* Preload the next image into pb_next */
static void preload_next(int next_pos) {
    if (SS.pb_next) { g_object_unref(SS.pb_next); SS.pb_next = NULL; }
    if (!SS.files || SS.files->len == 0) return;
    int real_idx = SS.order ? SS.order[next_pos] : next_pos;
    const char *path = (const char *)g_ptr_array_index(SS.files, real_idx);
    SS.pb_next = load_image(path);
}

/* Jump to a specific order-position and display it */
static void go_to(int pos) {
    if (!SS.files || SS.files->len == 0) return;
    int n = (int)SS.files->len;
    pos = ((pos % n) + n) % n;
    SS.current_idx = pos;

    /* Swap pre-loaded or load fresh */
    if (SS.pb_current) { g_object_unref(SS.pb_current); SS.pb_current = NULL; }
    if (SS.pb_next) {
        SS.pb_current = SS.pb_next;
        SS.pb_next    = NULL;
    } else {
        int real_idx = SS.order ? SS.order[pos] : pos;
        const char *path = (const char *)g_ptr_array_index(SS.files, real_idx);
        SS.pb_current = load_image(path);
    }

    randomize_kb();
    update_labels();

    /* Preload next */
    int next_pos = ((pos + 1) % n);
    preload_next(next_pos);

    gtk_widget_queue_draw(SS.canvas);
}

/* ══════════════════════════════════════════════
   Crossfade animation
   ══════════════════════════════════════════════ */
static gboolean fade_tick(gpointer data) {
    SS.fade_step++;
    gtk_widget_queue_draw(SS.canvas);
    if (SS.fade_step >= FADE_STEPS) {
        SS.fading    = FALSE;
        SS.fade_step = 0;
        SS.fade_timer = 0;
        /* pb_next becomes current */
        if (SS.pb_current) g_object_unref(SS.pb_current);
        SS.pb_current = SS.pb_next;
        SS.pb_next    = NULL;
        SS.current_idx = ((SS.current_idx + 1) % (int)SS.files->len);
        randomize_kb();
        update_labels();
        /* Preload the one after */
        int next_pos = ((SS.current_idx + 1) % (int)SS.files->len);
        preload_next(next_pos);
        return FALSE;
    }
    return TRUE;
}

static void start_fade_to_next(void) {
    if (!SS.files || SS.files->len == 0) return;
    if (SS.fading) return;

    int next_pos = ((SS.current_idx + 1) % (int)SS.files->len);

    /* Make sure pb_next is loaded */
    if (!SS.pb_next) preload_next(next_pos);
    if (!SS.pb_next) { go_to(next_pos); return; }

    SS.fading    = TRUE;
    SS.fade_step = 0;
    SS.fade_timer = g_timeout_add(FADE_INTERVAL, fade_tick, NULL);
}

/* ══════════════════════════════════════════════
   Slide timer
   ══════════════════════════════════════════════ */
static gboolean slide_tick(gpointer data) {
    if (!SS.playing) return TRUE;
    start_fade_to_next();
    return TRUE;
}

static void start_timer(void) {
    if (SS.slide_timer) { g_source_remove(SS.slide_timer); SS.slide_timer = 0; }
    SS.slide_timer = g_timeout_add(SS.interval_sec * 1000, slide_tick, NULL);
}

/* Ken Burns tick — runs on a faster timer to animate pan/zoom */
static gboolean kb_tick(gpointer data) {
    if (!SS.playing || SS.fading) return TRUE;
    SS.kb_progress += 1.0 / (SS.interval_sec * (1000.0 / 40.0));
    if (SS.kb_progress > 1.0) SS.kb_progress = 1.0;
    if (SS.kenburns) gtk_widget_queue_draw(SS.canvas);
    return TRUE;
}

/* Progress stripe tick */
static gboolean progress_tick(gpointer data) {
    gtk_widget_queue_draw(SS.canvas);
    return TRUE;
}

/* ══════════════════════════════════════════════
   Canvas drawing
   ══════════════════════════════════════════════ */
static void paint_image_with_kb(cairo_t *cr,
                                  GdkPixbuf *pb,
                                  double alpha,
                                  double kb_t) {
    if (!pb) return;
    int pw = gdk_pixbuf_get_width(pb);
    int ph = gdk_pixbuf_get_height(pb);

    double scale  = KB_SCALE_START + (KB_SCALE_END - KB_SCALE_START) * kb_t;
    double pan_x  = (SS.kb_start_x + (SS.kb_end_x - SS.kb_start_x) * kb_t) * IMG_W;
    double pan_y  = (SS.kb_start_y + (SS.kb_end_y - SS.kb_start_y) * kb_t) * IMG_H;

    /* Center of image on canvas */
    double cx = IMG_W / 2.0 + pan_x;
    double cy = IMG_H / 2.0 + pan_y;

    cairo_save(cr);
    /* Clip to canvas */
    cairo_rectangle(cr, 0, 0, IMG_W, IMG_H);
    cairo_clip(cr);

    cairo_translate(cr, cx, cy);
    cairo_scale(cr, scale, scale);
    gdk_cairo_set_source_pixbuf(cr, pb, -pw/2.0, -ph/2.0);
    cairo_paint_with_alpha(cr, alpha);
    cairo_restore(cr);
}

static gboolean on_draw_canvas(GtkWidget *w, cairo_t *cr, gpointer d) {
    int W = gtk_widget_get_allocated_width(w);
    int H = gtk_widget_get_allocated_height(w);

    /* Black background */
    cairo_set_source_rgb(cr, 0.04, 0.04, 0.06);
    cairo_paint(cr);

    if (!SS.files || SS.files->len == 0) {
        cairo_select_font_face(cr, "monospace",
            CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 13);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.30);
        const char *msg = "No images in ~/Pictures";
        cairo_text_extents_t te;
        cairo_text_extents(cr, msg, &te);
        cairo_move_to(cr, (W - te.width)/2, H/2);
        cairo_show_text(cr, msg);
        return FALSE;
    }

    if (SS.fading) {
        /* Crossfade: current fades out, next fades in */
        double t = (double)SS.fade_step / FADE_STEPS;
        /* Ease in-out */
        double eased = t < 0.5 ? 2*t*t : -1 + (4 - 2*t)*t;

        paint_image_with_kb(cr, SS.pb_current, 1.0 - eased, SS.kb_progress);
        paint_image_with_kb(cr, SS.pb_next,    eased,        0.0);
    } else {
        paint_image_with_kb(cr, SS.pb_current, 1.0, SS.kb_progress);
    }

    return FALSE;
}

/* ══════════════════════════════════════════════
   Button callbacks
   ══════════════════════════════════════════════ */
static void on_prev(GtkButton *b, gpointer d) {
    if (SS.fading) return;
    int n = SS.files ? (int)SS.files->len : 0;
    if (n == 0) return;
    int pos = ((SS.current_idx - 1) + n) % n;
    go_to(pos);
    if (SS.playing) start_timer(); /* reset countdown */
}

static void on_next(GtkButton *b, gpointer d) {
    if (SS.fading) return;
    int n = SS.files ? (int)SS.files->len : 0;
    if (n == 0) return;
    start_fade_to_next();
    if (SS.playing) start_timer();
}

static void on_play_pause(GtkButton *b, gpointer d) {
    SS.playing = !SS.playing;
    if (SS.playing) start_timer();
}

static void on_shuffle_toggle(GtkButton *b, gpointer d) {
    SS.shuffle = !SS.shuffle;
    build_order();
    SS.current_idx = 0;
    go_to(0);
}

/* ── Right-click context menu ── */
static void on_open_folder(GtkMenuItem *item, gpointer d) {
    char cmd[640];
    snprintf(cmd, sizeof(cmd), "xdg-open '%s' &", SS.path);
    system(cmd);
}

static void on_rescan(GtkMenuItem *item, gpointer d) {
    build_file_list();
    build_order();
    SS.current_idx = 0;
    go_to(0);
}

static void show_context_menu(GdkEventButton *ev) {
    GtkWidget *menu = gtk_menu_new();

    GtkWidget *item_folder = gtk_menu_item_new_with_label("📂  Open Folder");
    g_signal_connect(item_folder, "activate", G_CALLBACK(on_open_folder), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item_folder);

    GtkWidget *item_rescan = gtk_menu_item_new_with_label("🔄  Rescan Images");
    g_signal_connect(item_rescan, "activate", G_CALLBACK(on_rescan), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item_rescan);

    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)ev);
}

/* ══════════════════════════════════════════════
   Drag & Drop + right-click
   ══════════════════════════════════════════════ */
static gboolean on_press(GtkWidget *w, GdkEventButton *ev, gpointer d) {
    if (ev->button == 3) { show_context_menu(ev); return TRUE; }
    if (ev->button != 1) return FALSE;
    SS.dragging = TRUE;
    SS.drag_rx = ev->x_root; SS.drag_ry = ev->y_root;
    gint wx, wy;
    gtk_widget_translate_coordinates(w, gtk_widget_get_toplevel(w), 0, 0, &wx, &wy);
    SS.drag_wx = wx; SS.drag_wy = wy;
    return TRUE;
}
static gboolean on_motion(GtkWidget *w, GdkEventMotion *ev, gpointer d) {
    if (!SS.dragging || !SS.api || !SS.api->layout_container) return FALSE;
    gtk_layout_move(GTK_LAYOUT(SS.api->layout_container), w,
        SS.drag_wx + (int)(ev->x_root - SS.drag_rx),
        SS.drag_wy + (int)(ev->y_root - SS.drag_ry));
    return TRUE;
}
static gboolean on_release(GtkWidget *w, GdkEventButton *ev, gpointer d) {
    if (ev->button != 1 || !SS.dragging) return FALSE;
    SS.dragging = FALSE;
    if (SS.api && SS.api->save_position && SS.api->layout_container) {
        gint x, y;
        gtk_widget_translate_coordinates(w, gtk_widget_get_toplevel(w), 0, 0, &x, &y);
        SS.api->save_position("slideshow.so", x, y);
    }
    return TRUE;
}

/* Scroll on canvas = next/prev */
static gboolean on_scroll(GtkWidget *w, GdkEventScroll *ev, gpointer d) {
    if (ev->direction == GDK_SCROLL_DOWN || ev->direction == GDK_SCROLL_RIGHT)
        on_next(NULL, NULL);
    else
        on_prev(NULL, NULL);
    return TRUE;
}

/* ══════════════════════════════════════════════
   CSS
   ══════════════════════════════════════════════ */
static const gchar *SS_CSS =
    "box.ss-card {"
    "  background-color: rgba(10, 10, 16, 0.92);"
    "  border: 1px solid rgba(255,255,255,0.08);"
    "  border-radius: 12px;"
    "  padding: 0px;"
    "}";

/* ══════════════════════════════════════════════
   Widget construction
   ══════════════════════════════════════════════ */
static GtkWidget *create_slideshow_widget(VenomDesktopAPI *api) {
    memset(&SS, 0, sizeof(SS));
    SS.api     = api;
    SS.playing = TRUE;
    srand((unsigned)time(NULL));

    load_config();
    build_file_list();
    build_order();

    /* CSS */
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css, SS_CSS, -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    /* Root event box */
    SS.root_eb = gtk_event_box_new();
    gtk_widget_set_events(SS.root_eb,
        GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
        GDK_POINTER_MOTION_MASK | GDK_SCROLL_MASK);
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(SS.root_eb), FALSE);
    gtk_widget_set_size_request(SS.root_eb, WIDGET_W, WIDGET_H);
    g_signal_connect(SS.root_eb, "button-press-event",   G_CALLBACK(on_press),   NULL);
    g_signal_connect(SS.root_eb, "motion-notify-event",  G_CALLBACK(on_motion),  NULL);
    g_signal_connect(SS.root_eb, "button-release-event", G_CALLBACK(on_release), NULL);
    g_signal_connect(SS.root_eb, "scroll-event",         G_CALLBACK(on_scroll),  NULL);

    /* Card */
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(card), "ss-card");
    gtk_container_add(GTK_CONTAINER(SS.root_eb), card);

    /* ── Canvas (image area) ── */
    SS.canvas = gtk_drawing_area_new();
    gtk_widget_set_size_request(SS.canvas, IMG_W, IMG_H);
    g_signal_connect(SS.canvas, "draw", G_CALLBACK(on_draw_canvas), NULL);
    /* Clip canvas to top-rounded corners */
    gtk_box_pack_start(GTK_BOX(card), SS.canvas, FALSE, FALSE, 0);

    gtk_widget_show_all(SS.root_eb);

    /* Load first image */
    if (SS.files && SS.files->len > 0) {
        go_to(0);
        start_timer();
        /* Ken Burns animation tick: every 40ms */
        g_timeout_add(40, kb_tick, NULL);
    }

    return SS.root_eb;
}

/* ══════════════════════════════════════════════
   Venom entry point
   ══════════════════════════════════════════════ */
VenomWidgetAPI *venom_widget_init(void) {
    static VenomWidgetAPI api;
    api.name          = "Slideshow";
    api.description   = "Auto-advancing image viewer with Ken Burns effect";
    api.author        = "Venom Community";
    api.create_widget = create_slideshow_widget;
    return &api;
}
