/*
 * mpris-player.c — Venom Desktop Widget  (v2 — redesigned)
 * MPRIS2 Music Player  +  PulseAudio master-volume control
 *
 * Layout (matches reference screenshot):
 *   ┌──────────────────────────────┐
 *   │  ┌────────────────────────┐  │
 *   │  │     Album  Art  (HD)   │  │  ← full-width art, rounded corners
 *   │  └────────────────────────┘  │
 *   │  00:53 ══════●══════ 01:51   │  ← seek-style progress
 *   │       Title (bold)           │
 *   │       Artist (muted)         │
 *   │  🔇─────●──────────────── 🔊 │  ← PulseAudio master volume
 *   │   shuffle  ◀◀  ⏸  ▶▶  repeat │
 *   └──────────────────────────────┘
 *
 * Volume slider → PulseAudio default-sink via pactl (no libpulse needed).
 * Falls back to MPRIS Volume property if pactl is unavailable.
 *
 * Dependencies: gtk+-3.0, gio-2.0
 * Optional:     pactl (pulseaudio-utils) for system volume
 *
 * Compile:
 *   gcc -shared -fPIC -o mpris-player.so mpris-player.c \
 *       $(pkg-config --cflags --libs gtk+-3.0 gio-2.0) \
 *       -lm -I/path/to/venom-desktop/include
 *
 * Install:
 *   cp mpris-player.so ~/.config/venom/widgets/
 */

#include <gtk/gtk.h>
#include <gio/gio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../../include/venom-widget-api.h"

/* ─── MPRIS D-Bus ─── */
#define MPRIS_PREFIX        "org.mpris.MediaPlayer2."
#define MPRIS_OBJ           "/org/mpris/MediaPlayer2"
#define MPRIS_PLAYER_IFACE  "org.mpris.MediaPlayer2.Player"
#define DBUS_PROPS_IFACE    "org.freedesktop.DBus.Properties"
#define DBUS_NAME           "org.freedesktop.DBus"
#define DBUS_OBJ            "/org/freedesktop/DBus"
#define DBUS_IFACE          "org.freedesktop.DBus"

/* ─── Dimensions ─── */
#define WIDGET_W   300
#define WIDGET_H   390
#define ART_W      268
#define ART_H      200

/* ─── Colors ─── */
#define COL_BG         "rgba(0, 0, 0, 0.23)"
#define COL_BORDER     "rgba(140, 150, 220, 0.18)"
#define COL_TITLE      "#e8eaf6"
#define COL_ARTIST     "rgba(180,185,230,0.65)"
#define COL_TIME       "rgba(160,168,220,0.55)"
#define COL_ACCENT     "#7c83d4"
#define COL_CTRL       "rgba(210,215,255,0.80)"
#define COL_CTRL_HOVER "#ffffff"
#define COL_TRACK_BG   "rgba(255,255,255,0.12)"

/* ══════════════════════════════════════════════
   State
   ══════════════════════════════════════════════ */
typedef struct {
    GtkWidget *root_eb;
    GtkWidget *art_area;
    GtkWidget *lbl_title;
    GtkWidget *lbl_artist;
    GtkWidget *lbl_elapsed;
    GtkWidget *lbl_total;
    GtkWidget *progress_area;
    GtkWidget *btn_shuffle;
    GtkWidget *btn_prev;
    GtkWidget *btn_play;
    GtkWidget *btn_next;
    GtkWidget *btn_repeat;
    GtkWidget *vol_slider;

    gchar    *service;
    gboolean  is_playing;
    gboolean  shuffle;
    gint      loop_status;
    gint64    position_us;
    gint64    length_us;
    gdouble   mpris_volume;
    gchar    *art_url;

    GdkPixbuf *art_pixbuf;

    gint      pa_volume;
    gboolean  pa_available;

    gboolean  seeking;
    gdouble   seek_frac;

    gboolean  dragging;
    gint      drag_rx, drag_ry;
    gint      drag_wx, drag_wy;

    VenomDesktopAPI *api;
} PS;

static PS S;

/* ══════════════════════════════════════════════
   PulseAudio helpers (via pactl)
   ══════════════════════════════════════════════ */
static gboolean pa_check(void) {
    static gint checked = -1;
    if (checked >= 0) return (gboolean)checked;
    checked = (system("which pactl > /dev/null 2>&1") == 0) ? 1 : 0;
    return (gboolean)checked;
}

static gint pa_get_volume(void) {
    FILE *fp = popen("pactl get-sink-volume @DEFAULT_SINK@ 2>/dev/null", "r");
    if (!fp) return 80;
    char line[256] = {0};
    fgets(line, sizeof(line), fp);
    pclose(fp);
    char *pct = strstr(line, "/ ");
    if (pct) { pct += 2; return atoi(pct); }
    return 80;
}

static void pa_set_volume(gint pct) {
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    char cmd[128];
    snprintf(cmd, sizeof(cmd),
             "pactl set-sink-volume @DEFAULT_SINK@ %d%% > /dev/null 2>&1", pct);
    system(cmd);
}

/* ══════════════════════════════════════════════
   MPRIS D-Bus helpers
   ══════════════════════════════════════════════ */
static gchar *find_mpris_service(void) {
    GError *err = NULL;
    GDBusConnection *conn = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
    if (!conn) { g_clear_error(&err); return NULL; }
    GVariant *reply = g_dbus_connection_call_sync(
        conn, DBUS_NAME, DBUS_OBJ, DBUS_IFACE,
        "ListNames", NULL, G_VARIANT_TYPE("(as)"),
        G_DBUS_CALL_FLAGS_NONE, 500, NULL, &err);
    g_object_unref(conn);
    if (!reply) { g_clear_error(&err); return NULL; }
    gchar *found = NULL;
    GVariantIter *it; gchar *nm;
    g_variant_get(reply, "(as)", &it);
    while (g_variant_iter_next(it, "s", &nm)) {
        if (g_str_has_prefix(nm, MPRIS_PREFIX) && !found)
            found = g_strdup(nm);
        g_free(nm);
    }
    g_variant_iter_free(it);
    g_variant_unref(reply);
    return found;
}

static void mpris_call(const gchar *method) {
    if (!S.service) return;
    GError *err = NULL;
    GDBusConnection *conn = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
    if (!conn) { g_clear_error(&err); return; }
    g_dbus_connection_call_sync(conn, S.service, MPRIS_OBJ,
        MPRIS_PLAYER_IFACE, method, NULL, NULL,
        G_DBUS_CALL_FLAGS_NONE, 500, NULL, NULL);
    g_object_unref(conn);
}

static void mpris_set_bool_prop(const gchar *prop, gboolean val) {
    if (!S.service) return;
    GError *err = NULL;
    GDBusConnection *conn = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
    if (!conn) { g_clear_error(&err); return; }
    g_dbus_connection_call_sync(conn, S.service, MPRIS_OBJ, DBUS_PROPS_IFACE,
        "Set",
        g_variant_new("(ssv)", MPRIS_PLAYER_IFACE, prop, g_variant_new_boolean(val)),
        NULL, G_DBUS_CALL_FLAGS_NONE, 500, NULL, NULL);
    g_object_unref(conn);
}

static void mpris_set_loop(const gchar *status) {
    if (!S.service) return;
    GError *err = NULL;
    GDBusConnection *conn = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
    if (!conn) { g_clear_error(&err); return; }
    g_dbus_connection_call_sync(conn, S.service, MPRIS_OBJ, DBUS_PROPS_IFACE,
        "Set",
        g_variant_new("(ssv)", MPRIS_PLAYER_IFACE, "LoopStatus",
                      g_variant_new_string(status)),
        NULL, G_DBUS_CALL_FLAGS_NONE, 500, NULL, NULL);
    g_object_unref(conn);
}

static void mpris_seek_to(gdouble frac) {
    if (!S.service || S.length_us <= 0) return;
    gint64 target = (gint64)(frac * (gdouble)S.length_us);
    gint64 delta  = target - S.position_us;
    GError *err = NULL;
    GDBusConnection *conn = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
    if (!conn) { g_clear_error(&err); return; }
    g_dbus_connection_call_sync(conn, S.service, MPRIS_OBJ,
        MPRIS_PLAYER_IFACE, "Seek",
        g_variant_new("(x)", delta), NULL,
        G_DBUS_CALL_FLAGS_NONE, 500, NULL, NULL);
    g_object_unref(conn);
    S.position_us = target;
}

/* ── Album art loader ── */
static void load_art(const gchar *url) {
    if (S.art_pixbuf) { g_object_unref(S.art_pixbuf); S.art_pixbuf = NULL; }
    if (!url) goto redraw;
    GError *err = NULL;
    if (g_str_has_prefix(url, "file://")) {
        S.art_pixbuf = gdk_pixbuf_new_from_file_at_scale(
            url + 7, ART_W, ART_H, FALSE, &err);
        if (err) g_clear_error(&err);
    } else if (g_str_has_prefix(url, "http")) {
        char tmp[256];
        snprintf(tmp, sizeof(tmp), "/tmp/venom_art_%u.jpg", g_str_hash(url));
        char cmd[512];
        snprintf(cmd, sizeof(cmd),
                 "curl -sL --max-time 5 -o '%s' '%s' 2>/dev/null", tmp, url);
        if (system(cmd) == 0)
            S.art_pixbuf = gdk_pixbuf_new_from_file_at_scale(
                tmp, ART_W, ART_H, FALSE, &err);
        if (err) g_clear_error(&err);
    }
redraw:
    if (S.art_area) gtk_widget_queue_draw(S.art_area);
}

/* ══════════════════════════════════════════════
   Custom drawing (Cairo)
   ══════════════════════════════════════════════ */
static void rounded_rect(cairo_t *cr,
                          double x, double y, double w, double h, double r) {
    cairo_new_sub_path(cr);
    cairo_arc(cr, x+w-r, y+r,   r, -G_PI/2,  0);
    cairo_arc(cr, x+w-r, y+h-r, r,  0,        G_PI/2);
    cairo_arc(cr, x+r,   y+h-r, r,  G_PI/2,   G_PI);
    cairo_arc(cr, x+r,   y+r,   r,  G_PI,    3*G_PI/2);
    cairo_close_path(cr);
}

/* Album art */
static gboolean on_draw_art(GtkWidget *w, cairo_t *cr, gpointer d) {
    int width  = gtk_widget_get_allocated_width(w);
    int height = gtk_widget_get_allocated_height(w);

    rounded_rect(cr, 0, 0, width, height, 10);
    cairo_clip(cr);

    if (S.art_pixbuf) {
        int pw = gdk_pixbuf_get_width(S.art_pixbuf);
        int ph = gdk_pixbuf_get_height(S.art_pixbuf);
        double sx = (double)width  / pw;
        double sy = (double)height / ph;
        double sc = sx > sy ? sx : sy;
        cairo_save(cr);
        cairo_scale(cr, sc, sc);
        gdk_cairo_set_source_pixbuf(cr, S.art_pixbuf,
            (width/sc  - pw) / 2.0,
            (height/sc - ph) / 2.0);
        cairo_paint(cr);
        cairo_restore(cr);
    } else {
        /* Placeholder gradient */
        cairo_pattern_t *g = cairo_pattern_create_linear(0, 0, 0, height);
        cairo_pattern_add_color_stop_rgb(g, 0.0, 0.15, 0.17, 0.32);
        cairo_pattern_add_color_stop_rgb(g, 1.0, 0.08, 0.09, 0.18);
        cairo_set_source(cr, g);
        cairo_paint(cr);
        cairo_pattern_destroy(g);
        /* Music note */
        cairo_set_source_rgba(cr, 1, 1, 1, 0.15);
        cairo_select_font_face(cr, "sans",
            CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 52);
        cairo_text_extents_t te;
        cairo_text_extents(cr, "♫", &te);
        cairo_move_to(cr,
            (width  - te.width)  / 2.0 - te.x_bearing,
            (height - te.height) / 2.0 - te.y_bearing);
        cairo_show_text(cr, "♫");
    }

    /* Bottom gradient overlay */
    cairo_pattern_t *ov = cairo_pattern_create_linear(0, height*0.55, 0, height);
    cairo_pattern_add_color_stop_rgba(ov, 0.0, 0.11, 0.12, 0.19, 0.0);
    cairo_pattern_add_color_stop_rgba(ov, 1.0, 0.11, 0.12, 0.19, 0.6);
    cairo_reset_clip(cr);
    rounded_rect(cr, 0, 0, width, height, 10);
    cairo_clip(cr);
    cairo_set_source(cr, ov);
    cairo_paint(cr);
    cairo_pattern_destroy(ov);

    return FALSE;
}

/* Custom seek / progress bar */
static gboolean on_draw_progress(GtkWidget *w, cairo_t *cr, gpointer d) {
    int width  = gtk_widget_get_allocated_width(w);
    int height = gtk_widget_get_allocated_height(w);
    double cy     = height / 2.0;
    double track_h = 4.0;
    double y0     = cy - track_h / 2.0;

    gdouble frac = 0.0;
    if (S.seeking)
        frac = S.seek_frac;
    else if (S.length_us > 0)
        frac = CLAMP((gdouble)S.position_us / (gdouble)S.length_us, 0.0, 1.0);

    double filled = frac * width;

    /* Track background */
    cairo_set_source_rgba(cr, 1, 1, 1, 0.14);
    rounded_rect(cr, 0, y0, width, track_h, track_h/2);
    cairo_fill(cr);

    /* Filled */
    if (filled > 1.0) {
        cairo_set_source_rgba(cr, 0.49, 0.51, 0.84, 1.0);
        rounded_rect(cr, 0, y0, filled, track_h, track_h/2);
        cairo_fill(cr);
    }

    /* Thumb */
    double thumb_r = 6.0;
    double tx = CLAMP(filled, thumb_r, width - thumb_r);
    cairo_set_source_rgba(cr, 0.93, 0.94, 1.0, 1.0);
    cairo_arc(cr, tx, cy, thumb_r, 0, 2*G_PI);
    cairo_fill(cr);

    return FALSE;
}

/* Seek interaction */
static gboolean on_progress_press(GtkWidget *w, GdkEventButton *ev, gpointer d) {
    if (ev->button != 1) return FALSE;
    S.seeking   = TRUE;
    S.seek_frac = CLAMP(ev->x / gtk_widget_get_allocated_width(w), 0.0, 1.0);
    gtk_widget_queue_draw(w);
    return TRUE;
}
static gboolean on_progress_motion(GtkWidget *w, GdkEventMotion *ev, gpointer d) {
    if (!S.seeking) return FALSE;
    S.seek_frac = CLAMP(ev->x / gtk_widget_get_allocated_width(w), 0.0, 1.0);
    gtk_widget_queue_draw(w);
    return TRUE;
}
static gboolean on_progress_release(GtkWidget *w, GdkEventButton *ev, gpointer d) {
    if (ev->button != 1 || !S.seeking) return FALSE;
    S.seek_frac = CLAMP(ev->x / gtk_widget_get_allocated_width(w), 0.0, 1.0);
    S.seeking   = FALSE;
    mpris_seek_to(S.seek_frac);
    gtk_widget_queue_draw(w);
    return TRUE;
}

/* ══════════════════════════════════════════════
   Button callbacks
   ══════════════════════════════════════════════ */
static void on_prev   (GtkButton *b, gpointer d) { mpris_call("Previous"); }
static void on_play   (GtkButton *b, gpointer d) { mpris_call("PlayPause"); }
static void on_next   (GtkButton *b, gpointer d) { mpris_call("Next"); }

static void on_shuffle(GtkButton *b, gpointer d) {
    S.shuffle = !S.shuffle;
    mpris_set_bool_prop("Shuffle", S.shuffle);
    gtk_widget_set_opacity(GTK_WIDGET(b), S.shuffle ? 1.0 : 0.40);
}

static void on_repeat(GtkButton *b, gpointer d) {
    S.loop_status = (S.loop_status + 1) % 3;
    const gchar *modes[] = {"None", "Track", "Playlist"};
    const gchar *icons[] = {"🔁",   "🔂",    "🔁"};
    mpris_set_loop(modes[S.loop_status]);
    gtk_button_set_label(b, icons[S.loop_status]);
    gtk_widget_set_opacity(GTK_WIDGET(b), S.loop_status > 0 ? 1.0 : 0.40);
}

/* Volume: PulseAudio preferred, MPRIS fallback */
static void on_volume_changed(GtkRange *range, gpointer d) {
    gdouble val = gtk_range_get_value(range);
    if (S.pa_available) {
        S.pa_volume = (gint)(val * 100.0);
        pa_set_volume(S.pa_volume);
    } else {
        if (!S.service) return;
        GError *err = NULL;
        GDBusConnection *conn = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
        if (!conn) { g_clear_error(&err); return; }
        g_dbus_connection_call_sync(conn, S.service, MPRIS_OBJ, DBUS_PROPS_IFACE,
            "Set",
            g_variant_new("(ssv)", MPRIS_PLAYER_IFACE, "Volume",
                          g_variant_new_double(val)),
            NULL, G_DBUS_CALL_FLAGS_NONE, 500, NULL, NULL);
        g_object_unref(conn);
    }
}

/* ══════════════════════════════════════════════
   MPRIS polling  (every 2 s)
   ══════════════════════════════════════════════ */
static gboolean poll_mpris(gpointer data) {
    gchar *svc = find_mpris_service();
    if (!svc) {
        if (S.service) { g_free(S.service); S.service = NULL; }
        gtk_label_set_text(GTK_LABEL(S.lbl_title),   "No player");
        gtk_label_set_text(GTK_LABEL(S.lbl_artist),  "—");
        gtk_label_set_text(GTK_LABEL(S.lbl_elapsed), "00:00");
        gtk_label_set_text(GTK_LABEL(S.lbl_total),   "00:00");
        if (S.art_pixbuf) { g_object_unref(S.art_pixbuf); S.art_pixbuf = NULL; }
        gtk_widget_queue_draw(S.art_area);
        gtk_widget_queue_draw(S.progress_area);
        return TRUE;
    }
    if (!S.service || strcmp(S.service, svc) != 0) {
        g_free(S.service); S.service = svc;
    } else { g_free(svc); }

    GError *err = NULL;
    GDBusConnection *conn = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
    if (!conn) { g_clear_error(&err); return TRUE; }

    GVariant *reply = g_dbus_connection_call_sync(
        conn, S.service, MPRIS_OBJ, DBUS_PROPS_IFACE,
        "GetAll", g_variant_new("(s)", MPRIS_PLAYER_IFACE),
        G_VARIANT_TYPE("(a{sv})"),
        G_DBUS_CALL_FLAGS_NONE, 1000, NULL, &err);
    g_object_unref(conn);
    if (!reply) { g_clear_error(&err); return TRUE; }

    GVariantIter *top; g_variant_get(reply, "(a{sv})", &top);
    gchar *key; GVariant *val;

    while (g_variant_iter_next(top, "{sv}", &key, &val)) {

        if (!strcmp(key, "PlaybackStatus")) {
            S.is_playing = !strcmp(g_variant_get_string(val, NULL), "Playing");
            gtk_button_set_label(GTK_BUTTON(S.btn_play), S.is_playing ? "⏸" : "▶");

        } else if (!strcmp(key, "Shuffle")) {
            S.shuffle = g_variant_get_boolean(val);
            gtk_widget_set_opacity(S.btn_shuffle, S.shuffle ? 1.0 : 0.40);

        } else if (!strcmp(key, "LoopStatus")) {
            const gchar *ls = g_variant_get_string(val, NULL);
            S.loop_status = !strcmp(ls,"Track") ? 1 : !strcmp(ls,"Playlist") ? 2 : 0;
            gtk_widget_set_opacity(S.btn_repeat, S.loop_status > 0 ? 1.0 : 0.40);

        } else if (!strcmp(key, "Position")) {
            if (!S.seeking) S.position_us = g_variant_get_int64(val);

        } else if (!strcmp(key, "Volume") && !S.pa_available) {
            S.mpris_volume = g_variant_get_double(val);
            g_signal_handlers_block_by_func(S.vol_slider,
                G_CALLBACK(on_volume_changed), NULL);
            gtk_range_set_value(GTK_RANGE(S.vol_slider), S.mpris_volume);
            g_signal_handlers_unblock_by_func(S.vol_slider,
                G_CALLBACK(on_volume_changed), NULL);

        } else if (!strcmp(key, "Metadata")) {
            GVariantIter *meta; g_variant_get(val, "a{sv}", &meta);
            gchar *mk; GVariant *mv;
            while (g_variant_iter_next(meta, "{sv}", &mk, &mv)) {
                if (!strcmp(mk, "xesam:title"))
                    gtk_label_set_text(GTK_LABEL(S.lbl_title),
                                       g_variant_get_string(mv, NULL));
                else if (!strcmp(mk, "xesam:artist")) {
                    GVariantIter *ai; g_variant_get(mv, "as", &ai);
                    gchar *artist;
                    if (g_variant_iter_next(ai, "s", &artist)) {
                        gtk_label_set_text(GTK_LABEL(S.lbl_artist), artist);
                        g_free(artist);
                    }
                    g_variant_iter_free(ai);
                } else if (!strcmp(mk, "mpris:length")) {
                    S.length_us = g_variant_get_int64(mv);
                } else if (!strcmp(mk, "mpris:artUrl")) {
                    const gchar *new_url = g_variant_get_string(mv, NULL);
                    if (!S.art_url || strcmp(S.art_url, new_url)) {
                        g_free(S.art_url);
                        S.art_url = g_strdup(new_url);
                        load_art(S.art_url);
                    }
                }
                g_free(mk); g_variant_unref(mv);
            }
            g_variant_iter_free(meta);
        }
        g_free(key); g_variant_unref(val);
    }
    g_variant_iter_free(top);
    g_variant_unref(reply);

    /* Time labels */
    if (!S.seeking) {
        gint64 ps = S.position_us / 1000000;
        gint64 ls = S.length_us   / 1000000;
        char el[16], to[16];
        snprintf(el, sizeof(el), "%02ld:%02ld", ps/60, ps%60);
        snprintf(to, sizeof(to), "%02ld:%02ld", ls/60, ls%60);
        gtk_label_set_text(GTK_LABEL(S.lbl_elapsed), el);
        gtk_label_set_text(GTK_LABEL(S.lbl_total),   to);
        gtk_widget_queue_draw(S.progress_area);
    }

    /* PulseAudio volume sync */
    if (S.pa_available) {
        gint pav = pa_get_volume();
        if (abs(pav - S.pa_volume) > 1) {
            S.pa_volume = pav;
            g_signal_handlers_block_by_func(S.vol_slider,
                G_CALLBACK(on_volume_changed), NULL);
            gtk_range_set_value(GTK_RANGE(S.vol_slider), pav / 100.0);
            g_signal_handlers_unblock_by_func(S.vol_slider,
                G_CALLBACK(on_volume_changed), NULL);
        }
    }

    return TRUE;
}

/* ══════════════════════════════════════════════
   Drag & Drop
   ══════════════════════════════════════════════ */
static gboolean on_card_press(GtkWidget *w, GdkEventButton *ev, gpointer d) {
    if (ev->button != 1) return FALSE;
    S.dragging = TRUE;
    S.drag_rx  = ev->x_root; S.drag_ry = ev->y_root;
    gint wx, wy;
    gtk_widget_translate_coordinates(w, gtk_widget_get_toplevel(w), 0, 0, &wx, &wy);
    S.drag_wx = wx; S.drag_wy = wy;
    return TRUE;
}
static gboolean on_card_motion(GtkWidget *w, GdkEventMotion *ev, gpointer d) {
    if (!S.dragging || !S.api || !S.api->layout_container) return FALSE;
    gtk_layout_move(GTK_LAYOUT(S.api->layout_container), w,
        S.drag_wx + (int)(ev->x_root - S.drag_rx),
        S.drag_wy + (int)(ev->y_root - S.drag_ry));
    return TRUE;
}
static gboolean on_card_release(GtkWidget *w, GdkEventButton *ev, gpointer d) {
    if (ev->button != 1 || !S.dragging) return FALSE;
    S.dragging = FALSE;
    if (S.api && S.api->save_position && S.api->layout_container) {
        gint x, y;
        gtk_widget_translate_coordinates(w, gtk_widget_get_toplevel(w), 0, 0, &x, &y);
        S.api->save_position("mpris-player.so", x, y);
    }
    return TRUE;
}

/* ══════════════════════════════════════════════
   CSS
   ══════════════════════════════════════════════ */
static const gchar *WIDGET_CSS =
    "box.mp-card {"
    "  background-color: " COL_BG ";"
    "  border: 1px solid " COL_BORDER ";"
    "  border-radius: 16px;"
    "  padding: 16px 16px 12px 16px;"
    "}"
    "label.mp-title {"
    "  color: " COL_TITLE ";"
    "  font-family: 'Cantarell', 'Noto Sans', sans-serif;"
    "  font-weight: 700;"
    "  font-size: 14px;"
    "}"
    "label.mp-artist {"
    "  color: " COL_ARTIST ";"
    "  font-family: 'Cantarell', sans-serif;"
    "  font-size: 11.5px;"
    "}"
    "label.mp-time {"
    "  color: " COL_TIME ";"
    "  font-family: monospace;"
    "  font-size: 10.5px;"
    "  min-width: 36px;"
    "}"
    "button.mp-ctrl {"
    "  background: none;"
    "  border: none;"
    "  box-shadow: none;"
    "  color: " COL_CTRL ";"
    "  font-size: 17px;"
    "  padding: 4px 6px;"
    "  border-radius: 8px;"
    "}"
    "button.mp-ctrl:hover {"
    "  background-color: rgba(255,255,255,0.09);"
    "  color: " COL_CTRL_HOVER ";"
    "}"
    "button.mp-play {"
    "  font-size: 22px;"
    "  padding: 4px 14px;"
    "}"
    "scale.mp-vol trough {"
    "  background-color: " COL_TRACK_BG ";"
    "  border-radius: 3px;"
    "  min-height: 3px;"
    "}"
    "scale.mp-vol highlight {"
    "  background-color: " COL_ACCENT ";"
    "  border-radius: 3px;"
    "}"
    "scale.mp-vol slider {"
    "  background-color: #d0d4f8;"
    "  border-radius: 50%;"
    "  min-width: 12px;"
    "  min-height: 12px;"
    "  border: none;"
    "  box-shadow: 0 1px 4px rgba(0,0,0,0.5);"
    "}"
    "label.mp-vol-icon {"
    "  color: rgba(180,185,240,0.50);"
    "  font-size: 12px;"
    "}";

/* ══════════════════════════════════════════════
   Widget construction
   ══════════════════════════════════════════════ */
static GtkWidget *create_mpris_widget(VenomDesktopAPI *api) {
    memset(&S, 0, sizeof(S));
    S.api          = api;
    S.pa_available = pa_check();
    S.pa_volume    = S.pa_available ? pa_get_volume() : 80;

    /* CSS */
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css, WIDGET_CSS, -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    /* Root event box */
    S.root_eb = gtk_event_box_new();
    gtk_widget_set_events(S.root_eb,
        GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK);
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(S.root_eb), FALSE);
    gtk_widget_set_size_request(S.root_eb, WIDGET_W, WIDGET_H);
    g_signal_connect(S.root_eb, "button-press-event",   G_CALLBACK(on_card_press),   NULL);
    g_signal_connect(S.root_eb, "motion-notify-event",  G_CALLBACK(on_card_motion),  NULL);
    g_signal_connect(S.root_eb, "button-release-event", G_CALLBACK(on_card_release), NULL);

    /* Card */
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_style_context_add_class(gtk_widget_get_style_context(card), "mp-card");
    gtk_container_add(GTK_CONTAINER(S.root_eb), card);

    /* ── 1. Album Art ── */
    S.art_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(S.art_area, ART_W, ART_H);
    g_signal_connect(S.art_area, "draw", G_CALLBACK(on_draw_art), NULL);
    gtk_box_pack_start(GTK_BOX(card), S.art_area, FALSE, FALSE, 0);

    /* ── 2. Progress row ── */
    GtkWidget *prog_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(card), prog_row, FALSE, FALSE, 0);

    S.lbl_elapsed = gtk_label_new("00:00");
    gtk_style_context_add_class(gtk_widget_get_style_context(S.lbl_elapsed), "mp-time");
    gtk_widget_set_halign(S.lbl_elapsed, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(prog_row), S.lbl_elapsed, FALSE, FALSE, 0);

    S.progress_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(S.progress_area, -1, 20);
    gtk_widget_set_events(S.progress_area,
        GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK);
    g_signal_connect(S.progress_area, "draw",
                     G_CALLBACK(on_draw_progress),    NULL);
    g_signal_connect(S.progress_area, "button-press-event",
                     G_CALLBACK(on_progress_press),   NULL);
    g_signal_connect(S.progress_area, "motion-notify-event",
                     G_CALLBACK(on_progress_motion),  NULL);
    g_signal_connect(S.progress_area, "button-release-event",
                     G_CALLBACK(on_progress_release), NULL);
    gtk_box_pack_start(GTK_BOX(prog_row), S.progress_area, TRUE, TRUE, 0);

    S.lbl_total = gtk_label_new("00:00");
    gtk_style_context_add_class(gtk_widget_get_style_context(S.lbl_total), "mp-time");
    gtk_widget_set_halign(S.lbl_total, GTK_ALIGN_END);
    gtk_box_pack_start(GTK_BOX(prog_row), S.lbl_total, FALSE, FALSE, 0);

    /* ── 3. Title + Artist ── */
    GtkWidget *info_col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_box_pack_start(GTK_BOX(card), info_col, FALSE, FALSE, 0);

    S.lbl_title = gtk_label_new("Not playing");
    gtk_label_set_ellipsize(GTK_LABEL(S.lbl_title), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(S.lbl_title), 28);
    gtk_widget_set_halign(S.lbl_title, GTK_ALIGN_CENTER);
    gtk_style_context_add_class(gtk_widget_get_style_context(S.lbl_title), "mp-title");
    gtk_box_pack_start(GTK_BOX(info_col), S.lbl_title, FALSE, FALSE, 0);

    S.lbl_artist = gtk_label_new("—");
    gtk_label_set_ellipsize(GTK_LABEL(S.lbl_artist), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(S.lbl_artist), 30);
    gtk_widget_set_halign(S.lbl_artist, GTK_ALIGN_CENTER);
    gtk_style_context_add_class(gtk_widget_get_style_context(S.lbl_artist), "mp-artist");
    gtk_box_pack_start(GTK_BOX(info_col), S.lbl_artist, FALSE, FALSE, 0);

    /* ── 4. Volume row  🔇 ──●── 🔊 ── */
    GtkWidget *vol_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(card), vol_row, FALSE, FALSE, 0);

    GtkWidget *lbl_lo = gtk_label_new("🔇");
    gtk_style_context_add_class(gtk_widget_get_style_context(lbl_lo), "mp-vol-icon");
    gtk_box_pack_start(GTK_BOX(vol_row), lbl_lo, FALSE, FALSE, 0);

    S.vol_slider = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0.0, 1.0, 0.01);
    gtk_scale_set_draw_value(GTK_SCALE(S.vol_slider), FALSE);
    gtk_range_set_value(GTK_RANGE(S.vol_slider),
                        S.pa_available ? S.pa_volume / 100.0 : 0.8);
    gtk_style_context_add_class(gtk_widget_get_style_context(S.vol_slider), "mp-vol");
    g_signal_connect(S.vol_slider, "value-changed", G_CALLBACK(on_volume_changed), NULL);
    gtk_box_pack_start(GTK_BOX(vol_row), S.vol_slider, TRUE, TRUE, 0);

    GtkWidget *lbl_hi = gtk_label_new("🔊");
    gtk_style_context_add_class(gtk_widget_get_style_context(lbl_hi), "mp-vol-icon");
    gtk_box_pack_start(GTK_BOX(vol_row), lbl_hi, FALSE, FALSE, 0);

    /* ── 5. Controls row  ⇄  ◀◀  ▶  ▶▶  🔁 ── */
    GtkWidget *ctrl_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_widget_set_halign(ctrl_row, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(card), ctrl_row, FALSE, FALSE, 0);

    S.btn_shuffle = gtk_button_new_with_label("⇄");
    gtk_style_context_add_class(gtk_widget_get_style_context(S.btn_shuffle), "mp-ctrl");
    gtk_widget_set_opacity(S.btn_shuffle, 0.40);
    g_signal_connect(S.btn_shuffle, "clicked", G_CALLBACK(on_shuffle), NULL);
    gtk_box_pack_start(GTK_BOX(ctrl_row), S.btn_shuffle, FALSE, FALSE, 0);

    S.btn_prev = gtk_button_new_with_label("◀◀");
    gtk_style_context_add_class(gtk_widget_get_style_context(S.btn_prev), "mp-ctrl");
    g_signal_connect(S.btn_prev, "clicked", G_CALLBACK(on_prev), NULL);
    gtk_box_pack_start(GTK_BOX(ctrl_row), S.btn_prev, FALSE, FALSE, 4);

    S.btn_play = gtk_button_new_with_label("▶");
    gtk_style_context_add_class(gtk_widget_get_style_context(S.btn_play), "mp-ctrl");
    gtk_style_context_add_class(gtk_widget_get_style_context(S.btn_play), "mp-play");
    g_signal_connect(S.btn_play, "clicked", G_CALLBACK(on_play), NULL);
    gtk_box_pack_start(GTK_BOX(ctrl_row), S.btn_play, FALSE, FALSE, 4);

    S.btn_next = gtk_button_new_with_label("▶▶");
    gtk_style_context_add_class(gtk_widget_get_style_context(S.btn_next), "mp-ctrl");
    g_signal_connect(S.btn_next, "clicked", G_CALLBACK(on_next), NULL);
    gtk_box_pack_start(GTK_BOX(ctrl_row), S.btn_next, FALSE, FALSE, 4);

    S.btn_repeat = gtk_button_new_with_label("🔁");
    gtk_style_context_add_class(gtk_widget_get_style_context(S.btn_repeat), "mp-ctrl");
    gtk_widget_set_opacity(S.btn_repeat, 0.40);
    g_signal_connect(S.btn_repeat, "clicked", G_CALLBACK(on_repeat), NULL);
    gtk_box_pack_start(GTK_BOX(ctrl_row), S.btn_repeat, FALSE, FALSE, 0);

    gtk_widget_show_all(S.root_eb);

    poll_mpris(NULL);
    g_timeout_add(10, poll_mpris, NULL);

    return S.root_eb;
}

/* ══════════════════════════════════════════════
   Venom entry point
   ══════════════════════════════════════════════ */
VenomWidgetAPI *venom_widget_init(void) {
    static VenomWidgetAPI api;
    api.name          = "MPRIS Music Player";
    api.description   = "MPRIS2 player with PulseAudio master volume control";
    api.author        = "Venom Community";
    api.create_widget = create_mpris_widget;
    return &api;
}
