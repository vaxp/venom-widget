/*
 * calendar.c — Venom Desktop Widget
 * A full-featured glassmorphism calendar widget.
 *
 * Features:
 *   - Full month grid display
 *   - Today highlight
 *   - Prev / Next month navigation buttons
 *   - Drag & Drop support via VenomDesktopAPI
 *
 * Compile:
 *   gcc -shared -fPIC -o calendar.so calendar.c \
 *       $(pkg-config --cflags --libs gtk+-3.0) \
 *       -I/path/to/venom-desktop/include
 *
 * Or, inside the Venom source tree (src/widgets/):
 *   make widgets
 *
 * Install:
 *   cp calendar.so ~/.config/venom/widgets/
 */

#include <gtk/gtk.h>
#include <time.h>
#include <stdio.h>
#include <string.h>
#include "../../include/venom-widget-api.h"

/* ------------------------------------------------------------------ */
/*  State                                                               */
/* ------------------------------------------------------------------ */

static VenomDesktopAPI *api_handle  = NULL;

/* Drag state */
static gboolean is_dragging   = FALSE;
static int drag_start_x       = 0;
static int drag_start_y       = 0;
static int widget_start_x     = 0;
static int widget_start_y     = 0;

/* Calendar state */
static int view_year  = 0;   /* currently displayed year  */
static int view_month = 0;   /* currently displayed month (0–11) */
static int today_day  = 0;
static int today_mon  = 0;
static int today_year = 0;

/* UI handles we need to update on navigation */
static GtkWidget *lbl_month_year = NULL;  /* "March 2026" header  */
static GtkWidget *grid_days      = NULL;  /* GtkGrid holding day cells */
static GtkWidget *root_event_box = NULL;  /* root widget for DnD      */

/* ------------------------------------------------------------------ */
/*  Helper: days in a month                                            */
/* ------------------------------------------------------------------ */
static int days_in_month(int year, int month) {
    /* month: 0-based (0=Jan … 11=Dec) */
    static const int dim[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (month == 1) {
        /* leap year check */
        if ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0))
            return 29;
    }
    return dim[month];
}

/* Day-of-week for the 1st of a month: 0=Sun … 6=Sat */
static int first_weekday(int year, int month) {
    struct tm t = {0};
    t.tm_year = year - 1900;
    t.tm_mon  = month;
    t.tm_mday = 1;
    mktime(&t);
    return t.tm_wday;   /* 0=Sun */
}

/* ------------------------------------------------------------------ */
/*  CSS                                                                 */
/* ------------------------------------------------------------------ */
static const char *CALENDAR_CSS =
    /* ── root frame (glassmorphism base) ── */
    "frame#cal_frame {"
    "  background: rgba(0, 0, 0, 0.31);"
    "  border-radius: 18px;"
    "  border: none;"
    "  padding: 14px 16px 16px 16px;"
    "}"

    /* ── header: month / year label ── */
    "#lbl_month_year {"
    "  color: #e8eaf6;"
    "  font-family: 'Orbitron', 'Rajdhani', 'Share Tech Mono', monospace;"
    "  font-size: 17px;"
    "  font-weight: 700;"
    "  letter-spacing: 1.5px;"
    "}"

    /* ── nav buttons ── */
    "button#btn_prev, button#btn_next {"
    "  background: rgba(0, 0, 0, 0);"
    "  border: 0px solid rgba(0, 0, 0, 0);"
    "  border-radius: 8px;"
    "  color: #90caf9;"
    "  font-size: 16px;"
    "  padding: 2px 10px;"
    "  min-width: 32px;"
    "}"
    "button#btn_prev:hover, button#btn_next:hover {"
    "  background: rgba(144,202,249,0.18);"
    "  color: #ffffff;"
    "}"

    /* ── weekday header row ── */
    "#lbl_wday {"
    "  color: rgba(144,202,249,0.75);"
    "  font-family: 'Share Tech Mono', monospace;"
    "  font-size: 11px;"
    "  font-weight: 700;"
    "  letter-spacing: 1px;"
    "}"

    /* ── normal day cells ── */
    "label#lbl_day {"
    "  color: rgba(220,230,255,0.85);"
    "  font-family: 'Rajdhani', 'Liberation Mono', monospace;"
    "  font-size: 14px;"
    "  font-weight: 500;"
    "  border-radius: 8px;"
    "  padding: 4px 6px;"
    "  min-width: 28px;"
    "  min-height: 26px;"
    "}"

    /* ── today highlight ── */
    "label#lbl_day_today {"
    "  color: #0d0d1a;"
    "  background: linear-gradient(135deg, #64b5f6 0%, #a78bfa 100%);"
    "  font-family: 'Rajdhani', 'Liberation Mono', monospace;"
    "  font-size: 14px;"
    "  font-weight: 700;"
    "  border-radius: 8px;"
    "  padding: 4px 6px;"
    "  min-width: 28px;"
    "  min-height: 26px;"
    "}"

    /* ── empty/filler cells ── */
    "label#lbl_empty {"
    "  min-width: 28px;"
    "  min-height: 26px;"
    "}";

/* ------------------------------------------------------------------ */
/*  Rebuild the day grid for the current view_year / view_month        */
/* ------------------------------------------------------------------ */
static void rebuild_grid(void) {
    /* Remove all children from the grid */
    GList *children = gtk_container_get_children(GTK_CONTAINER(grid_days));
    for (GList *l = children; l; l = l->next)
        gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(children);

    /* Week-day header row */
    static const char *wdays[] = {"SU","MO","TU","WE","TH","FR","SA"};
    for (int col = 0; col < 7; col++) {
        GtkWidget *lbl = gtk_label_new(wdays[col]);
        gtk_widget_set_name(lbl, "lbl_wday");
        gtk_widget_set_halign(lbl, GTK_ALIGN_CENTER);
        gtk_grid_attach(GTK_GRID(grid_days), lbl, col, 0, 1, 1);
    }

    int first_wd = first_weekday(view_year, view_month);
    int num_days = days_in_month(view_year, view_month);

    int row = 1;
    int col = first_wd; /* start offset */

    /* Leading empty cells */
    for (int c = 0; c < first_wd; c++) {
        GtkWidget *e = gtk_label_new("");
        gtk_widget_set_name(e, "lbl_empty");
        gtk_grid_attach(GTK_GRID(grid_days), e, c, row, 1, 1);
    }

    for (int day = 1; day <= num_days; day++) {
        char buf[4];
        snprintf(buf, sizeof(buf), "%d", day);

        GtkWidget *lbl = gtk_label_new(buf);
        gtk_widget_set_halign(lbl, GTK_ALIGN_CENTER);
        gtk_widget_set_valign(lbl, GTK_ALIGN_CENTER);

        /* Is this today? */
        if (day == today_day && view_month == today_mon && view_year == today_year)
            gtk_widget_set_name(lbl, "lbl_day_today");
        else
            gtk_widget_set_name(lbl, "lbl_day");

        gtk_grid_attach(GTK_GRID(grid_days), lbl, col, row, 1, 1);

        col++;
        if (col == 7) { col = 0; row++; }
    }

    gtk_widget_show_all(grid_days);
}

/* ------------------------------------------------------------------ */
/*  Update the "March 2026" header label                               */
/* ------------------------------------------------------------------ */
static void update_header(void) {
    static const char *months[] = {
        "January","February","March","April","May","June",
        "July","August","September","October","November","December"
    };
    char buf[64];
    snprintf(buf, sizeof(buf), "%s  %d", months[view_month], view_year);
    gtk_label_set_text(GTK_LABEL(lbl_month_year), buf);
}

/* ------------------------------------------------------------------ */
/*  Navigation callbacks                                                */
/* ------------------------------------------------------------------ */
static void on_prev_month(GtkButton *btn, gpointer data) {
    view_month--;
    if (view_month < 0) { view_month = 11; view_year--; }
    update_header();
    rebuild_grid();
}

static void on_next_month(GtkButton *btn, gpointer data) {
    view_month++;
    if (view_month > 11) { view_month = 0; view_year++; }
    update_header();
    rebuild_grid();
}

/* ------------------------------------------------------------------ */
/*  Drag & Drop handlers                                                */
/* ------------------------------------------------------------------ */
static gboolean on_button_press(GtkWidget *w, GdkEventButton *ev, gpointer d) {
    if (ev->button == 1) {
        is_dragging   = TRUE;
        drag_start_x  = (int)ev->x_root;
        drag_start_y  = (int)ev->y_root;
        gint wx, wy;
        gtk_widget_translate_coordinates(w, gtk_widget_get_toplevel(w), 0, 0, &wx, &wy);
        widget_start_x = wx;
        widget_start_y = wy;
        return TRUE;
    }
    return FALSE;
}

static gboolean on_motion(GtkWidget *w, GdkEventMotion *ev, gpointer d) {
    if (is_dragging && api_handle && api_handle->layout_container) {
        int nx = widget_start_x + (int)(ev->x_root - drag_start_x);
        int ny = widget_start_y + (int)(ev->y_root - drag_start_y);
        gtk_layout_move(GTK_LAYOUT(api_handle->layout_container), w, nx, ny);
        return TRUE;
    }
    return FALSE;
}

static gboolean on_button_release(GtkWidget *w, GdkEventButton *ev, gpointer d) {
    if (ev->button == 1 && is_dragging) {
        is_dragging = FALSE;
        if (api_handle && api_handle->save_position && api_handle->layout_container) {
            gint x, y;
            gtk_widget_translate_coordinates(w, gtk_widget_get_toplevel(w), 0, 0, &x, &y);
            api_handle->save_position("calendar.so", x, y);
        }
        return TRUE;
    }
    return FALSE;
}

/* ------------------------------------------------------------------ */
/*  create_widget — called once by the desktop manager                 */
/* ------------------------------------------------------------------ */
static GtkWidget *create_calendar_ui(VenomDesktopAPI *desktop_api) {
    api_handle = desktop_api;

    /* ── Seed today's date ── */
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    today_day  = t->tm_mday;
    today_mon  = t->tm_mon;
    today_year = t->tm_year + 1900;
    view_month = today_mon;
    view_year  = today_year;

    /* ── Load CSS ── */
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css, CALENDAR_CSS, -1, NULL);
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    /* ── Root EventBox (captures DnD mouse events) ── */
    root_event_box = gtk_event_box_new();
    gtk_widget_set_events(root_event_box,
        GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK);
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(root_event_box), FALSE);
    g_signal_connect(root_event_box, "button-press-event",   G_CALLBACK(on_button_press),   NULL);
    g_signal_connect(root_event_box, "motion-notify-event",  G_CALLBACK(on_motion),         NULL);
    g_signal_connect(root_event_box, "button-release-event", G_CALLBACK(on_button_release), NULL);

    /* ── Outer Frame (glass look) ── */
    GtkWidget *frame = gtk_frame_new(NULL);
    gtk_widget_set_name(frame, "cal_frame");
    gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_NONE);
    gtk_container_set_border_width(GTK_CONTAINER(frame), 0);
    gtk_container_add(GTK_CONTAINER(root_event_box), frame);

    /* ── Main vertical box ── */
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_add(GTK_CONTAINER(frame), vbox);

    /* ── Header row: [<]  "March 2026"  [>] ── */
    GtkWidget *hbox_nav = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

    GtkWidget *btn_prev = gtk_button_new_with_label("‹");
    gtk_widget_set_name(btn_prev, "btn_prev");
    g_signal_connect(btn_prev, "clicked", G_CALLBACK(on_prev_month), NULL);

    lbl_month_year = gtk_label_new("");
    gtk_widget_set_name(lbl_month_year, "lbl_month_year");
    gtk_widget_set_hexpand(lbl_month_year, TRUE);
    gtk_label_set_justify(GTK_LABEL(lbl_month_year), GTK_JUSTIFY_CENTER);
    update_header();

    GtkWidget *btn_next = gtk_button_new_with_label("›");
    gtk_widget_set_name(btn_next, "btn_next");
    g_signal_connect(btn_next, "clicked", G_CALLBACK(on_next_month), NULL);

    gtk_box_pack_start(GTK_BOX(hbox_nav), btn_prev,       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox_nav), lbl_month_year, TRUE,  TRUE,  0);
    gtk_box_pack_start(GTK_BOX(hbox_nav), btn_next,       FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(vbox), hbox_nav, FALSE, FALSE, 0);

    /* ── Day grid ── */
    grid_days = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid_days), 4);
    gtk_grid_set_column_spacing(GTK_GRID(grid_days), 6);
    gtk_grid_set_row_homogeneous(GTK_GRID(grid_days), TRUE);
    gtk_grid_set_column_homogeneous(GTK_GRID(grid_days), TRUE);
    rebuild_grid();

    gtk_box_pack_start(GTK_BOX(vbox), grid_days, TRUE, TRUE, 0);

    gtk_widget_show_all(root_event_box);
    return root_event_box;
}

/* ------------------------------------------------------------------ */
/*  Plugin entry point — loaded by dlsym("venom_widget_init")          */
/* ------------------------------------------------------------------ */
VenomWidgetAPI *venom_widget_init(void) {
    static VenomWidgetAPI api;
    api.name          = "Calendar";
    api.description   = "Glassmorphism monthly calendar with navigation.";
    api.author        = "Venom Community";
    api.create_widget = create_calendar_ui;
    return &api;
}
