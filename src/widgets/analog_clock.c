/*
 * analog_clock.c — Venom Desktop Widget
 *
 * A high-quality analog clock drawn entirely with Cairo.
 *
 * Visual features:
 *   • Circular face with deep glass/dark aesthetic
 *   • Metallic bezel ring (double-stroke gradient illusion)
 *   • 12 major hour markers (beveled rectangles) + 48 minute ticks
 *   • Roman numeral labels at the 12 / 3 / 6 / 9 positions
 *   • Hour hand   — wide, tapered, rounded cap, subtle glow
 *   • Minute hand — slender, tapered, rounded cap
 *   • Second hand — slim red needle + rear counterweight + lollipop tip
 *   • Central boss — layered circles simulating a polished screw
 *   • Smooth 33 ms redraws (~30 fps) for fluid second-hand sweep
 *   • Date + weekday label rendered below the face
 *   • Full Drag-and-Drop via VenomDesktopAPI
 *     — mouse events attached to the Cairo DrawingArea
 *     — gdk_seat_grab keeps pointer captured during fast drags
 *     — position clamped to (0,0) minimum, saved on mouse release
 *
 * Compile (outside source tree):
 *   gcc -shared -fPIC -o analog_clock.so analog_clock.c \
 *       $(pkg-config --cflags --libs gtk+-3.0) -lm \
 *       -I/path/to/venom-desktop/include
 *
 * Inside Venom source tree (src/widgets/):
 *   make widgets
 *
 * Install:
 *   cp analog_clock.so ~/.config/venom/widgets/
 */

#include <gtk/gtk.h>
#include <cairo.h>
#include <math.h>
#include <time.h>
#include <string.h>
#include "../../include/venom-widget-api.h"

/* ================================================================== */
/*  Tunables                                                            */
/* ================================================================== */
#define CLOCK_SIZE      200     /* diameter of the Cairo drawing area  */
#define REDRAW_MS        33     /* ~30 fps for smooth sweep             */
#define FACE_R          0.88    /* face radius as fraction of half-size */
#define BEZEL_R         0.96    /* bezel outer radius                   */

/* ================================================================== */
/*  Widget state                                                        */
/* ================================================================== */
typedef struct {
    GtkWidget        *root_eb;       /* root EventBox — the widget "body" passed to the manager */
    GtkWidget        *drawing_area;  /* Cairo canvas  — receives all mouse events               */
    GtkWidget        *lbl_date;      /* "Monday, 16 March 2026"                                 */

    /* drag — all coordinates in screen (root) space */
    gboolean          dragging;
    int               drag_sx;       /* pointer x at drag start (screen coords) */
    int               drag_sy;       /* pointer y at drag start (screen coords) */
    int               widget_sx;     /* widget x at drag start (layout coords)  */
    int               widget_sy;     /* widget y at drag start (layout coords)  */

    VenomDesktopAPI  *api;
} AnalogClock;

static AnalogClock *g_ac = NULL;   /* singleton */

/* ================================================================== */
/*  Time helpers                                                        */
/* ================================================================== */
static void get_time(int *hour, int *min, int *sec, long *nsec) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm *t = localtime(&ts.tv_sec);
    *hour = t->tm_hour % 12;
    *min  = t->tm_min;
    *sec  = t->tm_sec;
    *nsec = ts.tv_nsec;
}

static void get_date_string(char *buf, size_t n) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    strftime(buf, n, "%A,  %d %B %Y", t);
}

/* ================================================================== */
/*  Cairo helpers                                                       */
/* ================================================================== */

/* Set an RGBA source using simple floats */
static void set_color(cairo_t *cr, double r, double g, double b, double a) {
    cairo_set_source_rgba(cr, r, g, b, a);
}

/* Draw a filled + stroked circle */
static void filled_circle(cairo_t *cr,
                           double cx, double cy, double radius,
                           double fr, double fg, double fb, double fa,
                           double sr, double sg, double sb, double sa,
                           double lw) {
    cairo_arc(cr, cx, cy, radius, 0, 2*M_PI);
    set_color(cr, fr, fg, fb, fa);
    cairo_fill_preserve(cr);
    cairo_set_line_width(cr, lw);
    set_color(cr, sr, sg, sb, sa);
    cairo_stroke(cr);
}

/* ================================================================== */
/*  Main draw function                                                  */
/* ================================================================== */
static gboolean on_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data) {
    (void)user_data;

    int w = gtk_widget_get_allocated_width(widget);
    int h = gtk_widget_get_allocated_height(widget);

    double cx = w / 2.0;
    double cy = h / 2.0;
    double R  = (w < h ? w : h) / 2.0;   /* half the shorter dimension */

    /* radii in pixels */
    double face_r  = R * FACE_R;
    double bezel_r = R * BEZEL_R;

    /* ── Get current time ── */
    int   hour, min, sec;
    long  nsec;
    get_time(&hour, &min, &sec, &nsec);

    /* fractional seconds for smooth sweep */
    double sec_f  = sec  + nsec / 1e9;
    double min_f  = min  + sec_f  / 60.0;
    double hour_f = hour + min_f  / 60.0;

    /* ── Anti-alias ── */
    cairo_set_antialias(cr, CAIRO_ANTIALIAS_BEST);

    /* ================================================================
     *  1. OUTER SHADOW / GLOW  (soft drop shadow under bezel)
     * ================================================================ */
    for (int i = 6; i >= 1; i--) {
        double blur = i * 2.0;
        cairo_arc(cr, cx + 1, cy + 2, bezel_r + blur, 0, 2*M_PI);
        cairo_set_source_rgba(cr, 0, 0, 0, 0.04);
        cairo_fill(cr);
    }

    /* ================================================================
     *  2. BEZEL  (metallic dark ring)
     * ================================================================ */
    /* outer edge — bright highlight at top-left */
    cairo_pattern_t *bezel_pat = cairo_pattern_create_linear(
        cx - bezel_r, cy - bezel_r, cx + bezel_r, cy + bezel_r);
    cairo_pattern_add_color_stop_rgba(bezel_pat, 0.0,  0.55, 0.55, 0.60, 1.0);
    cairo_pattern_add_color_stop_rgba(bezel_pat, 0.4,  0.22, 0.22, 0.25, 1.0);
    cairo_pattern_add_color_stop_rgba(bezel_pat, 0.75, 0.10, 0.10, 0.12, 1.0);
    cairo_pattern_add_color_stop_rgba(bezel_pat, 1.0,  0.38, 0.38, 0.42, 1.0);

    cairo_arc(cr, cx, cy, bezel_r, 0, 2*M_PI);
    cairo_set_source(cr, bezel_pat);
    cairo_fill(cr);
    cairo_pattern_destroy(bezel_pat);

    /* inner bezel ring (inset bevel) */
    cairo_arc(cr, cx, cy, bezel_r - 2, 0, 2*M_PI);
    cairo_set_line_width(cr, 1.5);
    set_color(cr, 0.06, 0.06, 0.08, 0.90);
    cairo_stroke(cr);

    /* ================================================================
     *  3. CLOCK FACE  (dark glass)
     * ================================================================ */
    cairo_pattern_t *face_pat = cairo_pattern_create_radial(
        cx - face_r*0.25, cy - face_r*0.30, face_r * 0.05,
        cx,               cy,               face_r);
    cairo_pattern_add_color_stop_rgba(face_pat, 0.0,  0.18, 0.20, 0.28, 1.0);
    cairo_pattern_add_color_stop_rgba(face_pat, 0.55, 0.08, 0.09, 0.14, 1.0);
    cairo_pattern_add_color_stop_rgba(face_pat, 1.0,  0.04, 0.04, 0.07, 1.0);

    cairo_arc(cr, cx, cy, face_r, 0, 2*M_PI);
    cairo_set_source(cr, face_pat);
    cairo_fill(cr);
    cairo_pattern_destroy(face_pat);

    /* subtle inner edge highlight */
    cairo_arc(cr, cx, cy, face_r, 0, 2*M_PI);
    cairo_set_line_width(cr, 1.2);
    set_color(cr, 0.40, 0.44, 0.55, 0.40);
    cairo_stroke(cr);

    /* ================================================================
     *  4. GLASS REFLECTION  (semi-transparent white arc on upper half)
     * ================================================================ */
    cairo_save(cr);
    cairo_arc(cr, cx, cy, face_r * 0.97, 0, 2*M_PI);
    cairo_clip(cr);

    cairo_pattern_t *gloss = cairo_pattern_create_radial(
        cx, cy - face_r * 0.55, face_r * 0.10,
        cx, cy - face_r * 0.20, face_r * 0.90);
    cairo_pattern_add_color_stop_rgba(gloss, 0.0, 1, 1, 1, 0.14);
    cairo_pattern_add_color_stop_rgba(gloss, 0.5, 1, 1, 1, 0.04);
    cairo_pattern_add_color_stop_rgba(gloss, 1.0, 1, 1, 1, 0.00);
    cairo_arc(cr, cx, cy - face_r*0.18, face_r*0.82, M_PI, 2*M_PI);
    cairo_set_source(cr, gloss);
    cairo_fill(cr);
    cairo_pattern_destroy(gloss);
    cairo_restore(cr);

    /* ================================================================
     *  5. MINUTE TICK MARKS  (60 ticks, 48 short + 12 long)
     * ================================================================ */
    for (int i = 0; i < 60; i++) {
        double angle = i * (2*M_PI / 60.0) - M_PI_2;
        gboolean is_hour_mark = (i % 5 == 0);

        double outer = face_r * 0.93;
        double inner = is_hour_mark ? face_r * 0.76 : face_r * 0.87;
        double lw    = is_hour_mark ? 2.8 : 1.0;

        double x1 = cx + cos(angle) * inner;
        double y1 = cy + sin(angle) * inner;
        double x2 = cx + cos(angle) * outer;
        double y2 = cy + sin(angle) * outer;

        cairo_move_to(cr, x1, y1);
        cairo_line_to(cr, x2, y2);
        cairo_set_line_width(cr, lw);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);

        if (is_hour_mark)
            set_color(cr, 0.88, 0.90, 0.95, 0.95);
        else
            set_color(cr, 0.55, 0.58, 0.65, 0.60);

        cairo_stroke(cr);
    }

    /* ================================================================
     *  6. ROMAN NUMERAL LABELS  at 12, 3, 6, 9
     * ================================================================ */
    static const char *labels[] = { "XII", "III", "VI", "IX" };
    double label_r = face_r * 0.62;

    cairo_select_font_face(cr, "serif",
        CAIRO_FONT_SLANT_ITALIC, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, face_r * 0.115);

    for (int i = 0; i < 4; i++) {
        double angle = i * (M_PI_2) - M_PI_2;
        double lx = cx + cos(angle) * label_r;
        double ly = cy + sin(angle) * label_r;

        cairo_text_extents_t te;
        cairo_text_extents(cr, labels[i], &te);
        lx -= te.width  / 2 + te.x_bearing;
        ly -= te.height / 2 + te.y_bearing;

        /* subtle glow */
        set_color(cr, 0.50, 0.65, 1.0, 0.12);
        cairo_move_to(cr, lx + 1, ly + 1);
        cairo_show_text(cr, labels[i]);

        set_color(cr, 0.82, 0.86, 0.96, 0.90);
        cairo_move_to(cr, lx, ly);
        cairo_show_text(cr, labels[i]);
    }

    /* ================================================================
     *  7. HOUR HAND
     *     Tapered wide shape: wider at base, pointed tip
     * ================================================================ */
    {
        double angle = hour_f * (2*M_PI / 12.0) - M_PI_2;
        double length = face_r * 0.54;
        double base_w = face_r * 0.055;
        double rear   = face_r * 0.14;  /* how far behind center */

        cairo_save(cr);
        cairo_translate(cr, cx, cy);
        cairo_rotate(cr, angle);

        /* glow layer */
        cairo_set_line_width(cr, base_w * 2.8);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        set_color(cr, 0.50, 0.65, 1.0, 0.08);
        cairo_move_to(cr, -rear, 0);
        cairo_line_to(cr, length, 0);
        cairo_stroke(cr);

        /* hand body — tapered polygon */
        cairo_move_to(cr,  length,   0);
        cairo_line_to(cr, -rear,     base_w * 0.55);
        cairo_line_to(cr, -rear * 1.1, 0);
        cairo_line_to(cr, -rear,    -base_w * 0.55);
        cairo_close_path(cr);

        cairo_pattern_t *hp = cairo_pattern_create_linear(-rear, 0, length, 0);
        cairo_pattern_add_color_stop_rgba(hp, 0.0, 0.28, 0.32, 0.45, 1.0);
        cairo_pattern_add_color_stop_rgba(hp, 0.4, 0.68, 0.72, 0.82, 1.0);
        cairo_pattern_add_color_stop_rgba(hp, 1.0, 0.40, 0.44, 0.56, 1.0);
        cairo_set_source(cr, hp);
        cairo_fill_preserve(cr);
        cairo_pattern_destroy(hp);
        cairo_set_line_width(cr, 0.6);
        set_color(cr, 0.20, 0.22, 0.30, 0.80);
        cairo_stroke(cr);

        cairo_restore(cr);
    }

    /* ================================================================
     *  8. MINUTE HAND
     *     Slender tapered shape, longer than hour hand
     * ================================================================ */
    {
        double angle = min_f * (2*M_PI / 60.0) - M_PI_2;
        double length = face_r * 0.78;
        double base_w = face_r * 0.035;
        double rear   = face_r * 0.16;

        cairo_save(cr);
        cairo_translate(cr, cx, cy);
        cairo_rotate(cr, angle);

        /* glow */
        cairo_set_line_width(cr, base_w * 2.5);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        set_color(cr, 0.55, 0.70, 1.0, 0.07);
        cairo_move_to(cr, -rear, 0);
        cairo_line_to(cr, length, 0);
        cairo_stroke(cr);

        /* body */
        cairo_move_to(cr,  length,  0);
        cairo_line_to(cr, -rear,    base_w * 0.50);
        cairo_line_to(cr, -rear * 1.05, 0);
        cairo_line_to(cr, -rear,   -base_w * 0.50);
        cairo_close_path(cr);

        cairo_pattern_t *mp = cairo_pattern_create_linear(-rear, 0, length, 0);
        cairo_pattern_add_color_stop_rgba(mp, 0.0, 0.28, 0.32, 0.45, 1.0);
        cairo_pattern_add_color_stop_rgba(mp, 0.3, 0.72, 0.76, 0.88, 1.0);
        cairo_pattern_add_color_stop_rgba(mp, 1.0, 0.50, 0.55, 0.68, 1.0);
        cairo_set_source(cr, mp);
        cairo_fill_preserve(cr);
        cairo_pattern_destroy(mp);
        cairo_set_line_width(cr, 0.5);
        set_color(cr, 0.18, 0.20, 0.28, 0.80);
        cairo_stroke(cr);

        cairo_restore(cr);
    }

    /* ================================================================
     *  9. SECOND HAND
     *     Thin red needle + lollipop circle near tip + rear counterweight
     * ================================================================ */
    {
        double angle  = sec_f * (2*M_PI / 60.0) - M_PI_2;
        double length = face_r * 0.84;
        double rear   = face_r * 0.22;
        double lw     = 1.4;

        cairo_save(cr);
        cairo_translate(cr, cx, cy);
        cairo_rotate(cr, angle);

        /* glow trail */
        cairo_set_line_width(cr, lw * 4);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        set_color(cr, 0.90, 0.10, 0.10, 0.07);
        cairo_move_to(cr, 0, 0);
        cairo_line_to(cr, length * 0.9, 0);
        cairo_stroke(cr);

        /* main needle (front) */
        cairo_set_line_width(cr, lw);
        set_color(cr, 0.95, 0.15, 0.10, 1.00);
        cairo_move_to(cr, 0, 0);
        cairo_line_to(cr, length, 0);
        cairo_stroke(cr);

        /* counterweight (rear, slightly wider) */
        cairo_set_line_width(cr, lw * 2.8);
        set_color(cr, 0.85, 0.12, 0.08, 0.92);
        cairo_move_to(cr, 0, 0);
        cairo_line_to(cr, -rear, 0);
        cairo_stroke(cr);

        /* lollipop circle near tip */
        double lp_x = length * 0.80;
        double lp_r = face_r * 0.038;
        cairo_arc(cr, lp_x, 0, lp_r, 0, 2*M_PI);
        set_color(cr, 0.95, 0.15, 0.10, 1.00);
        cairo_fill(cr);

        cairo_restore(cr);
    }

    /* ================================================================
     *  10. CENTRAL BOSS  (layered polished screw look)
     * ================================================================ */
    double boss_r = face_r * 0.055;

    /* shadow ring */
    filled_circle(cr, cx, cy, boss_r + 2.5,
                  0, 0, 0, 0.35,
                  0, 0, 0, 0.0,  0);

    /* outer ring */
    filled_circle(cr, cx, cy, boss_r,
                  0.18, 0.20, 0.28, 1.0,
                  0.50, 0.55, 0.65, 0.80, 1.0);

    /* middle highlight ring */
    filled_circle(cr, cx, cy, boss_r * 0.65,
                  0.55, 0.60, 0.70, 1.0,
                  0.30, 0.34, 0.42, 0.60, 0.5);

    /* center glint */
    filled_circle(cr, cx - boss_r*0.18, cy - boss_r*0.20, boss_r * 0.28,
                  0.85, 0.88, 0.95, 0.70,
                  0, 0, 0, 0.0, 0);

    return FALSE;
}

/* ================================================================== */
/*  Update callback — redraws canvas + refreshes date label            */
/* ================================================================== */
static gboolean on_tick(gpointer data) {
    AnalogClock *ac = (AnalogClock *)data;
    if (!ac->drawing_area) return G_SOURCE_REMOVE;

    /* redraw clock */
    gtk_widget_queue_draw(ac->drawing_area);

    /* refresh date label once a second is enough; do it every tick cheaply */
    char buf[80];
    get_date_string(buf, sizeof(buf));
    gtk_label_set_text(GTK_LABEL(ac->lbl_date), buf);

    return G_SOURCE_CONTINUE;
}

/* ================================================================== */
/*  Drag & Drop                                                         */
/*                                                                      */
/*  Mouse events fire on the GtkDrawingArea (Cairo canvas).            */
/*  We move root_eb inside the parent GtkLayout — that is the widget   */
/*  the desktop manager originally placed on screen.                   */
/*  gdk_seat_grab keeps the pointer captured even when the cursor      */
/*  leaves the widget boundary during a fast drag.                     */
/* ================================================================== */
static gboolean on_press(GtkWidget *w, GdkEventButton *ev, gpointer ud) {
    AnalogClock *ac = (AnalogClock *)ud;

    if (ev->button == 1) {
        ac->dragging = TRUE;

        /* remember where the pointer was (screen / root coords) */
        ac->drag_sx = (int)ev->x_root;
        ac->drag_sy = (int)ev->y_root;

        /* remember where root_eb currently sits in the layout */
        gint wx = 0, wy = 0;
        gtk_widget_translate_coordinates(
            ac->root_eb,
            gtk_widget_get_toplevel(ac->root_eb),
            0, 0, &wx, &wy);
        ac->widget_sx = wx;
        ac->widget_sy = wy;

        /* grab the pointer so motion events keep arriving during fast drags */
        gdk_seat_grab(
            gdk_display_get_default_seat(gdk_display_get_default()),
            gtk_widget_get_window(w),
            GDK_SEAT_CAPABILITY_POINTER,
            FALSE, NULL, (GdkEvent *)ev, NULL, NULL);

        return TRUE;
    }
    return FALSE;
}

static gboolean on_motion(GtkWidget *w, GdkEventMotion *ev, gpointer ud) {
    (void)w;
    AnalogClock *ac = (AnalogClock *)ud;

    if (ac->dragging && ac->api && ac->api->layout_container) {
        int nx = ac->widget_sx + (int)(ev->x_root - ac->drag_sx);
        int ny = ac->widget_sy + (int)(ev->y_root - ac->drag_sy);

        /* prevent the widget from going off the top-left edge */
        if (nx < 0) nx = 0;
        if (ny < 0) ny = 0;

        /* move the whole root_eb (not just the drawing area) */
        gtk_layout_move(
            GTK_LAYOUT(ac->api->layout_container),
            ac->root_eb,
            nx, ny);

        return TRUE;
    }
    return FALSE;
}

static gboolean on_release(GtkWidget *w, GdkEventButton *ev, gpointer ud) {
    (void)w;
    AnalogClock *ac = (AnalogClock *)ud;

    if (ev->button == 1 && ac->dragging) {
        ac->dragging = FALSE;

        /* release pointer grab */
        gdk_seat_ungrab(
            gdk_display_get_default_seat(gdk_display_get_default()));

        /* persist the final position so the manager can restore it */
        if (ac->api && ac->api->save_position && ac->api->layout_container) {
            gint x = 0, y = 0;
            gtk_widget_translate_coordinates(
                ac->root_eb,
                gtk_widget_get_toplevel(ac->root_eb),
                0, 0, &x, &y);
            ac->api->save_position("analog_clock.so", x, y);
        }
        return TRUE;
    }
    return FALSE;
}

/* ================================================================== */
/*  CSS for the date label + transparent frame                         */
/* ================================================================== */
static const char *CLOCK_CSS =
    "frame#clock_frame {"
    "  background: transparent;"
    "  border: none;"
    "  padding: 4px;"
    "}"
    "#lbl_clock_date {"
    "  color: rgba(180, 200, 240, 0.70);"
    "  font-family: 'Rajdhani', 'Share Tech Mono', 'Liberation Mono', monospace;"
    "  font-size: 12px;"
    "  font-weight: 600;"
    "  letter-spacing: 1.5px;"
    "}";

/* ================================================================== */
/*  Build the widget UI                                                 */
/* ================================================================== */
static GtkWidget *create_clock_widget(VenomDesktopAPI *desktop_api) {
    g_ac = g_new0(AnalogClock, 1);
    AnalogClock *ac = g_ac;
    ac->api = desktop_api;

    /* CSS */
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css, CLOCK_CSS, -1, NULL);
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    /* root event box — layout container only, no event mask needed here */
    ac->root_eb = gtk_event_box_new();
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(ac->root_eb), FALSE);

    /* transparent frame */
    GtkWidget *frame = gtk_frame_new(NULL);
    gtk_widget_set_name(frame, "clock_frame");
    gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_NONE);
    gtk_container_set_border_width(GTK_CONTAINER(frame), 0);
    gtk_container_add(GTK_CONTAINER(ac->root_eb), frame);

    /* vertical layout: canvas + date */
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_add(GTK_CONTAINER(frame), vbox);

    /* Cairo drawing area — owns the mouse events for both drawing and DnD */
    ac->drawing_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(ac->drawing_area, CLOCK_SIZE, CLOCK_SIZE);
    gtk_widget_set_events(ac->drawing_area,
        GDK_BUTTON_PRESS_MASK   |
        GDK_BUTTON_RELEASE_MASK |
        GDK_POINTER_MOTION_MASK |
        GDK_POINTER_MOTION_HINT_MASK);
    g_signal_connect(ac->drawing_area, "draw",                G_CALLBACK(on_draw),    ac);
    g_signal_connect(ac->drawing_area, "button-press-event",  G_CALLBACK(on_press),   ac);
    g_signal_connect(ac->drawing_area, "motion-notify-event", G_CALLBACK(on_motion),  ac);
    g_signal_connect(ac->drawing_area, "button-release-event",G_CALLBACK(on_release), ac);
    gtk_box_pack_start(GTK_BOX(vbox), ac->drawing_area, FALSE, FALSE, 0);

    /* date label */
    ac->lbl_date = gtk_label_new("");
    gtk_widget_set_name(ac->lbl_date, "lbl_clock_date");
    gtk_widget_set_halign(ac->lbl_date, GTK_ALIGN_CENTER);
    {
        char buf[80];
        get_date_string(buf, sizeof(buf));
        gtk_label_set_text(GTK_LABEL(ac->lbl_date), buf);
    }
    gtk_box_pack_start(GTK_BOX(vbox), ac->lbl_date, FALSE, FALSE, 0);

    gtk_widget_show_all(ac->root_eb);

    /* ~30 fps redraw timer */
    g_timeout_add(REDRAW_MS, on_tick, ac);

    return ac->root_eb;
}

/* ================================================================== */
/*  Plugin entry point                                                  */
/* ================================================================== */
VenomWidgetAPI *venom_widget_init(void) {
    static VenomWidgetAPI api;
    api.name          = "Analog Clock";
    api.description   = "High-quality Cairo analog clock with smooth sweep hands.";
    api.author        = "Venom Community";
    api.create_widget = create_clock_widget;
    return &api;
}
